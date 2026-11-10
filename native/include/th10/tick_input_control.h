#pragma once

#include "input_bits.h"

#include <cstddef>
#include <cstdint>

namespace th10hook {

static constexpr std::uint32_t InputMaxTtlMs = 5000;

enum class InputAckKind : std::uint8_t {
  None,
  Accepted,
  PlayerEntry,
  PlayerExit,
  CalcChainEnd,
  SnapshotComplete,
  Cancelled,
  TerminalTick = 9
};

enum class InputCancelReason : std::uint8_t {
  None,
  InvalidCommand,
  StaleOrDuplicate,
  Busy,
  Expired,
  Disconnected,
  InvalidContext,
  WrongCallback,
  InvalidSnapshot,
  FailedChecks,
  AckUnavailable
};

static constexpr std::uint16_t InputCheckStageTickAdvanced = 0x0001;
static constexpr std::uint16_t InputCheckSinglePlayerCall = 0x0002;
static constexpr std::uint16_t InputRequiredChecks =
    InputCheckStageTickAdvanced | InputCheckSinglePlayerCall;

struct TickContext {
  bool state_readable;
  bool gameplay_active;
  bool paused;
  bool resetting;
  bool connected;
};

struct CalcChainEnd {
  std::uint32_t callback_id;
  std::uint32_t stage_tick_before;
  std::uint32_t stage_tick_after;
  std::uint32_t player_calls;
};

struct SnapshotCommit {
  std::uint32_t sequence;
  bool valid;
};

struct InputAck {
  std::uint16_t version;
  std::uint16_t size;
  std::uint32_t session_id;
  std::uint32_t command_sequence;
  std::uint32_t calc_callback_id;
  std::uint32_t snapshot_sequence;
  std::uint16_t actions;
  std::uint16_t player_callback_exits;
  InputAckKind kind;
  InputCancelReason reason;
  std::uint16_t checks;
};

static_assert(sizeof(InputAck) == 28, "acknowledgement ABI changed");
static_assert(offsetof(InputAck, session_id) == 4 &&
                  offsetof(InputAck, calc_callback_id) == 12 &&
                  offsetof(InputAck, kind) == 24,
              "acknowledgement field offsets changed");

enum class InputSubmitResult : std::uint8_t {
  Accepted,
  Invalid,
  StaleOrDuplicate,
  Busy
};

// Game-thread state machine for one command: armed, inside the Player callback,
// after it, and waiting for the snapshot. InputController moves commands and
// acknowledgements through shared memory. No function here waits, allocates,
// patches code, or writes game input.
class TickInputControl {
 public:
  InputSubmitResult submit(const InputCommand& command, std::uint32_t now_ms,
                           std::uint32_t ttl_ms, InputAck& ack) {
    ack = {};
    if (!valid_command(command) || ttl_ms == 0 || ttl_ms > InputMaxTtlMs) {
      fill_ack(ack, InputAckKind::Cancelled, InputCancelReason::InvalidCommand,
               command);
      return InputSubmitResult::Invalid;
    }
    if (state_ != State::Idle) {
      fill_ack(ack, InputAckKind::Cancelled, InputCancelReason::Busy, command);
      return InputSubmitResult::Busy;
    }
    if (!has_session_ || command.session_id != session_id_ ||
        (has_sequence_ &&
         static_cast<std::int32_t>(command.sequence - last_sequence_) <= 0)) {
      fill_ack(ack, InputAckKind::Cancelled,
               InputCancelReason::StaleOrDuplicate, command);
      return InputSubmitResult::StaleOrDuplicate;
    }

    last_sequence_ = command.sequence;
    has_sequence_ = true;
    active_ = command;
    deadline_ms_ = now_ms + ttl_ms;
    state_ = State::Armed;
    fill_ack(ack, InputAckKind::Accepted, InputCancelReason::None, active_);
    return InputSubmitResult::Accepted;
  }

  bool begin_session(std::uint32_t session_id) {
    if (session_id == 0 || session_id == UINT32_MAX || state_ != State::Idle ||
        (has_session_ && session_id <= session_id_))
      return false;
    session_id_ = session_id;
    last_sequence_ = 0;
    has_sequence_ = false;
    has_session_ = true;
    return true;
  }

  // Cancel the command if the client disconnected or the game left normal
  // play. Returns whether the command can continue.
  bool observe_gate(std::uint32_t now_ms, const TickContext& gate,
                    InputAck& ack) {
    ack = {};
    if (state_ == State::Idle) return false;
    if (deadline_reached(now_ms, deadline_ms_)) {
      cancel(InputCancelReason::Expired, ack);
      return false;
    }
    if (!gate.connected) {
      cancel(InputCancelReason::Disconnected, ack);
      return false;
    }
    if (!gate.state_readable || !gate.gameplay_active || gate.paused ||
        gate.resetting) {
      cancel(InputCancelReason::InvalidContext, ack);
      return false;
    }
    return true;
  }

  // Call immediately before the Player callback. A true return supplies the
  // actions to apply through the rest of the calculation chain.
  bool player_callback_entry(std::uint32_t now_ms, const TickContext& gate,
                             std::uint32_t calc_callback_id,
                             std::uint16_t& actions, InputAck& ack) {
    actions = 0;
    ack = {};
    if (!observe_gate(now_ms, gate, ack)) return false;
    if (state_ != State::Armed || calc_callback_id == 0) {
      cancel(InputCancelReason::WrongCallback, ack);
      return false;
    }
    calc_callback_id_ = calc_callback_id;
    state_ = State::InPlayerCallback;
    actions = active_.actions;
    fill_ack(ack, InputAckKind::PlayerEntry, InputCancelReason::None, active_);
    ack.calc_callback_id = calc_callback_id_;
    return true;
  }

  // Call after the same Player callback returns. The actions stay applied so
  // later callbacks in the chain can read them.
  bool player_callback_exit(std::uint32_t now_ms, const TickContext& gate,
                            std::uint32_t calc_callback_id, InputAck& ack) {
    ack = {};
    if (!observe_gate(now_ms, gate, ack)) return false;
    if (state_ != State::InPlayerCallback ||
        calc_callback_id != calc_callback_id_) {
      cancel(InputCancelReason::WrongCallback, ack);
      return false;
    }
    state_ = State::AfterPlayerCallback;
    fill_ack(ack, InputAckKind::PlayerExit, InputCancelReason::None, active_);
    ack.calc_callback_id = calc_callback_id_;
    ack.player_callback_exits = 1;
    return true;
  }

  // Call at the end of the calculation chain. Requires that the stage tick
  // advanced by one and that the Player callback ran exactly once.
  bool calc_chain_end(std::uint32_t now_ms, const TickContext& gate,
                      const CalcChainEnd& chain, InputAck& ack) {
    ack = {};
    if (!observe_gate(now_ms, gate, ack)) return false;
    if (state_ != State::AfterPlayerCallback ||
        chain.callback_id != calc_callback_id_) {
      cancel(InputCancelReason::WrongCallback, ack);
      return false;
    }
    if (static_cast<std::uint32_t>(chain.stage_tick_after -
                                   chain.stage_tick_before) != 1 ||
        chain.player_calls != 1) {
      cancel(InputCancelReason::FailedChecks, ack);
      return false;
    }
    state_ = State::AwaitingSnapshot;
    fill_ack(ack, InputAckKind::CalcChainEnd, InputCancelReason::None, active_);
    ack.calc_callback_id = calc_callback_id_;
    ack.player_callback_exits = 1;
    ack.checks = InputRequiredChecks;
    return true;
  }

  // Snapshot completion closes the command. Snapshot IDs are observation
  // sequence IDs, not simulation-frame IDs. Invalid capture cancels the pair.
  bool complete_snapshot(std::uint32_t now_ms, const TickContext& gate,
                         std::uint32_t calc_callback_id,
                         SnapshotCommit snapshot, InputAck& ack) {
    ack = {};
    if (!observe_gate(now_ms, gate, ack)) return false;
    if (state_ != State::AwaitingSnapshot ||
        calc_callback_id != calc_callback_id_ || snapshot.sequence == 0 ||
        !snapshot.valid) {
      cancel(InputCancelReason::InvalidSnapshot, ack);
      return false;
    }
    fill_ack(ack, InputAckKind::SnapshotComplete, InputCancelReason::None,
             active_);
    ack.calc_callback_id = calc_callback_id_;
    ack.snapshot_sequence = snapshot.sequence;
    ack.player_callback_exits = 1;
    ack.checks = InputRequiredChecks;
    clear_active();
    return true;
  }

  bool cancel(InputCancelReason reason, InputAck& ack) {
    ack = {};
    if (state_ == State::Idle) return false;
    fill_ack(ack, InputAckKind::Cancelled, reason, active_);
    ack.calc_callback_id = calc_callback_id_;
    clear_active();
    return true;
  }

  // A verified terminal tick has no Player callback. Attribute its snapshot
  // to the pending command without counting a callback exit.
  bool complete_terminal(std::uint32_t now_ms, std::uint32_t calc_callback_id,
                         std::uint32_t snapshot_sequence, InputAck& ack) {
    if (state_ != State::Armed || deadline_reached(now_ms, deadline_ms_) ||
        !calc_callback_id || !snapshot_sequence)
      return false;
    fill_ack(ack, InputAckKind::TerminalTick, InputCancelReason::None, active_);
    ack.calc_callback_id = calc_callback_id;
    ack.snapshot_sequence = snapshot_sequence;
    clear_active();
    return true;
  }

  bool active() const { return state_ != State::Idle; }

 private:
  enum class State : std::uint8_t {
    Idle,
    Armed,
    InPlayerCallback,
    AfterPlayerCallback,
    AwaitingSnapshot
  };

  static bool deadline_reached(std::uint32_t now, std::uint32_t deadline) {
    return static_cast<std::int32_t>(now - deadline) >= 0;
  }

  static void fill_ack(InputAck& ack, InputAckKind kind,
                       InputCancelReason reason, const InputCommand& command) {
    ack.version = 1;
    ack.size = sizeof(InputAck);
    ack.session_id = command.session_id;
    ack.command_sequence = command.sequence;
    ack.actions = command.actions;
    ack.kind = kind;
    ack.reason = reason;
    ack.checks = 0;
  }

  void clear_active() {
    active_ = {};
    calc_callback_id_ = 0;
    deadline_ms_ = 0;
    state_ = State::Idle;
  }

  InputCommand active_{};
  std::uint32_t session_id_ = 0;
  std::uint32_t last_sequence_ = 0;
  std::uint32_t deadline_ms_ = 0;
  std::uint32_t calc_callback_id_ = 0;
  bool has_session_ = false;
  bool has_sequence_ = false;
  State state_ = State::Idle;
};

}  // namespace th10hook
