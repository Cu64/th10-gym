#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <climits>
#include <cstdio>
#include <cstring>

#include "game_hooks.h"
#include "hook_status.h"
#include "input_controller.h"

namespace th10hook {
namespace {
using retail::kPlayerPointer;
using retail::kReplayManagerPointer;
using retail::kSegmentTick;
using retail::kStageTick;

__declspec(thread) int g_controller_thread_token;
constexpr std::uintptr_t kInputCurrent = retail::kInputWords;
constexpr std::uint32_t kPlayerStateOffset = retail::kPlayerState;
constexpr std::uint32_t kReplayModeOffset = retail::kReplayMode;
constexpr std::uint32_t kMaxCommandAgeMs = 2000;
constexpr std::uint16_t kControlledInputMask = 0x00f7;

bool bounded_address(std::uint32_t address, std::uint32_t size) {
  return address >= 0x00400000u && size <= 0x100000000ull - address;
}

bool bounded_replay_manager_address(std::uint32_t address, std::uint32_t size) {
  return address >= 0x00010000u && size <= 0x7fff0000u &&
         address <= 0x7fff0000u - size;
}

bool read_u16(void*, std::uintptr_t address, std::uint16_t* out) {
  if (out == nullptr) return false;
  __try {
    *out = *reinterpret_cast<const volatile std::uint16_t*>(address);
    return true;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                      GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}

bool write_u16(void*, std::uintptr_t address, std::uint16_t value) {
  __try {
    *reinterpret_cast<volatile std::uint16_t*>(address) = value;
    return true;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                      GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}

bool read_u32(void*, std::uintptr_t address, std::uint32_t* out) {
  if (out == nullptr) return false;
  __try {
    *out = *reinterpret_cast<const volatile std::uint32_t*>(address);
    return true;
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                      GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    return false;
  }
}

bool deadline_passed(std::uint32_t now, std::uint32_t then, std::uint32_t ttl) {
  return static_cast<std::uint32_t>(now - then) > ttl;
}

}  // namespace

InputController::InputController(InputChannelMapping* mapping,
                                 const GameMemoryAccess& memory)
    : mapping_(mapping),
      memory_(memory),
      session_id_(0),
      last_command_sequence_(0),
      last_publication_(0),
      calc_id_(0),
      calc_stage_tick_(0),
      calc_segment_tick_(0),
      player_id_(0),
      held_player_context_(0),
      held_replay_context_(0),
      player_calls_(0),
      original_input_(0),
      injected_input_(0),
      has_session_(false),
      has_saved_input_(false),
      replay_seen_(false),
      player_entered_(false),
      player_exited_(false),
      chain_ready_(false),
      has_held_contexts_(false),
      faulted_(false),
      transaction_active_(false),
      reset_mode_(false),
      owner_thread_token_(nullptr),
      owner_violation_(0) {}

bool InputController::claim_owner_thread() {
  void* token = &g_controller_thread_token;
  const auto observed =
      InterlockedCompareExchangePointer(&owner_thread_token_, token, nullptr);
  if (observed == nullptr || observed == token) return true;
  InterlockedExchange(&owner_violation_, 1);
  return false;
}

bool InputController::memory_ok() const {
  return memory_.read_u16 != nullptr && memory_.write_u16 != nullptr &&
         memory_.read_u32 != nullptr && mapping_ != nullptr &&
         mapping_->magic == InputChannelMagic &&
         mapping_->version == InputChannelVersion &&
         mapping_->size == sizeof(InputChannelMapping);
}

void InputController::publish_ack(const InputAck& ack) {
  if (mapping_ == nullptr) return;
  volatile LONG* stamp =
      reinterpret_cast<volatile LONG*>(&mapping_->ack_publication);
  const LONG previous = InterlockedCompareExchange(stamp, 0, 0);
  if (previous < 0 || previous > LONG_MAX - 2) return;
  const LONG odd = (previous & 1) ? previous + 2 : previous + 1;
  InterlockedExchange(stamp, odd);
  mapping_->ack = ack;
  MemoryBarrier();
  InterlockedExchange(stamp, odd + 1);
}

bool InputController::read_new_command(InputChannelCommand& command) {
  volatile LONG* publication =
      reinterpret_cast<volatile LONG*>(&mapping_->command_publication);
  const LONG before = InterlockedCompareExchange(publication, 0, 0);
  if (before <= 0 || (before & 1) ||
      static_cast<std::uint32_t>(before) == last_publication_)
    return false;
  MemoryBarrier();
  command = mapping_->command;
  MemoryBarrier();
  const LONG after = InterlockedCompareExchange(publication, 0, 0);
  if (before != after || (after & 1)) return false;
  last_publication_ = static_cast<std::uint32_t>(after);
  return true;
}

bool InputController::accept_command_session(std::uint32_t session_id) {
  if (has_session_ && session_id <= session_id_)
    return session_id == session_id_;
  if (!control_.begin_session(session_id)) return false;
  session_id_ = session_id;
  has_session_ = true;
  last_command_sequence_ = 0;
  return true;
}

void InputController::finish_rejected_poll(InputAck& ack) {
  if (!restore_input()) {
    faulted_ = true;
    ack.reason = InputCancelReason::InvalidContext;
  }
  clear_transaction();
  clear_scope();
  if (ack.kind != InputAckKind::None) publish_ack(ack);
}

void InputController::poll_command(std::uint32_t now_ms) {
  if (mapping_ == nullptr || !memory_ok()) return;
  const std::uint32_t heartbeat =
      static_cast<std::uint32_t>(InterlockedCompareExchange(
          reinterpret_cast<volatile LONG*>(&mapping_->producer_heartbeat_ms), 0,
          0));
  if (deadline_passed(now_ms, heartbeat, kMaxCommandAgeMs)) {
    InputAck ack{};
    control_.observe_gate(now_ms,
                          TickContext{false, false, false, false, false}, ack);
    if (ack.kind != InputAckKind::None) finish_rejected_poll(ack);
    return;
  }
  if (has_saved_input_ || transaction_active_) return;

  InputChannelCommand live{};
  if (!read_new_command(live)) return;

  const InputCommand command{live.session_id, live.sequence, live.actions};
  const auto reject = [&](InputCancelReason reason) {
    publish_ack(InputAck{1, sizeof(InputAck), live.session_id, live.sequence, 0,
                         0, live.actions, 0, InputAckKind::Cancelled, reason,
                         0});
  };
  if (!valid_command(command) || live.flags != 0 || live.ticks != 1 ||
      live.ttl_ms == 0 || live.ttl_ms > InputMaxTtlMs) {
    reject(InputCancelReason::InvalidCommand);
    return;
  }
  if (!accept_command_session(live.session_id)) {
    reject(InputCancelReason::StaleOrDuplicate);
    return;
  }
  if (last_command_sequence_ != 0 &&
      static_cast<std::int32_t>(live.sequence - last_command_sequence_) <= 0) {
    reject(InputCancelReason::StaleOrDuplicate);
    return;
  }
  last_command_sequence_ = live.sequence;
  const std::uint32_t age = now_ms - live.published_at_ms;
  if (age >= live.ttl_ms) {
    reject(InputCancelReason::Expired);
    return;
  }

  InputAck ack{};
  const auto result = control_.submit(command, now_ms, live.ttl_ms - age, ack);
  if (result == InputSubmitResult::Accepted) transaction_active_ = true;
  if (ack.kind != InputAckKind::None) publish_ack(ack);
}

bool InputController::read_tick_context(TickContext& gate,
                                        std::uint32_t& stage_tick,
                                        std::uint32_t& segment_tick) {
  std::uint32_t player = 0, replay = 0, state = 0, mode = 1;
  gate = TickContext{false, false, false, false, true};
  if (!memory_.read_u32(memory_.context, kPlayerPointer, &player) ||
      !memory_.read_u32(memory_.context, kReplayManagerPointer, &replay) ||
      !memory_.read_u32(memory_.context, kStageTick, &stage_tick) ||
      !memory_.read_u32(memory_.context, kSegmentTick, &segment_tick))
    return false;
  if (player == 0) {
    gate.state_readable = true;
    gate.gameplay_active = false;
    return true;
  }
  if (!bounded_address(player, kPlayerStateOffset + sizeof(state)) ||
      !bounded_replay_manager_address(replay,
                                      kReplayModeOffset + sizeof(mode)) ||
      !memory_.read_u32(memory_.context, player + kPlayerStateOffset, &state) ||
      !memory_.read_u32(memory_.context, replay + kReplayModeOffset, &mode))
    return false;
  gate.state_readable = true;
  gate.gameplay_active = state <= 4 && mode == 0;
  gate.paused = false;
  gate.resetting = false;
  return true;
}

bool InputController::validate_callback_context(void* callback_context,
                                                std::uint32_t kind) {
  if (kind != th10hook::kCallbackPlayerPre &&
      kind != th10hook::kCallbackPlayerPost &&
      kind != th10hook::kCallbackReplayPre &&
      kind != th10hook::kCallbackReplayPost)
    return true;
  const std::uintptr_t address = (kind == th10hook::kCallbackPlayerPre ||
                                  kind == th10hook::kCallbackPlayerPost)
                                     ? kPlayerPointer
                                     : kReplayManagerPointer;
  std::uint32_t current = 0;
  return callback_context != nullptr &&
         memory_.read_u32(memory_.context, address, &current) &&
         current == static_cast<std::uint32_t>(
                        reinterpret_cast<std::uintptr_t>(callback_context));
}

bool InputController::restore_input() {
  if (!has_saved_input_) return true;
  std::uint16_t current = 0;
  if (!memory_.read_u16(memory_.context, kInputCurrent, &current)) return false;
  const std::uint16_t already_restored = static_cast<std::uint16_t>(
      ~(current ^ original_input_) & kControlledInputMask);
  const std::uint16_t still_injected = static_cast<std::uint16_t>(
      ~(current ^ injected_input_) & kControlledInputMask);
  const std::uint16_t conflict = static_cast<std::uint16_t>(
      kControlledInputMask & ~(already_restored | still_injected));
  const std::uint16_t restore_mask = still_injected;
  const std::uint16_t restored = static_cast<std::uint16_t>(
      (current & ~restore_mask) | (original_input_ & restore_mask));
  if (restored != current &&
      !memory_.write_u16(memory_.context, kInputCurrent, restored))
    return false;
  if (conflict == 0) {
    has_saved_input_ = false;
    return true;
  }
  return false;  // Preserve competing action bits; retry restoration on a later
                 // callback.
}

void InputController::clear_transaction() {
  transaction_active_ = false;
  has_held_contexts_ = false;
  held_player_context_ = held_replay_context_ = 0;
}

void InputController::clear_scope() {
  calc_id_ = player_id_ = 0;
  calc_stage_tick_ = calc_segment_tick_ = 0;
  player_calls_ = 0;
  replay_seen_ = player_entered_ = player_exited_ = chain_ready_ = false;
}

void InputController::cancel(InputCancelReason reason) {
  InputAck ack{};
  const bool cancelled = control_.cancel(reason, ack);
  if (!cancelled && (transaction_active_ || has_saved_input_)) {
    ack = InputAck{1,
                   sizeof(InputAck),
                   session_id_,
                   last_command_sequence_,
                   calc_id_,
                   0,
                   0,
                   0,
                   InputAckKind::Cancelled,
                   reason,
                   0};
  }
  if (!restore_input()) {
    faulted_ = true;
    if (ack.kind != InputAckKind::None)
      ack.reason = InputCancelReason::InvalidContext;
  }
  clear_transaction();
  clear_scope();
  if (ack.kind != InputAckKind::None) publish_ack(ack);
}

void InputController::BeforeCallback(void* callback_context, std::uint32_t kind,
                                     std::uint32_t callback_id,
                                     std::uint32_t parent_callback_id) {
  if (!claim_owner_thread()) return;
  if (InterlockedExchange(&owner_violation_, 0) != 0) {
    cancel(InputCancelReason::InvalidContext);
    faulted_ = true;
  }
  if (reset_mode_) {
    if (transaction_active_ || has_saved_input_ || control_.active())
      cancel(InputCancelReason::Disconnected);
    return;
  }
  const std::uint32_t now = GetTickCount();
  if ((has_saved_input_ || transaction_active_) && !memory_ok()) {
    cancel(InputCancelReason::InvalidContext);
    faulted_ = true;
    return;
  }
  if ((has_saved_input_ || transaction_active_) && mapping_ != nullptr) {
    const std::uint32_t heartbeat =
        static_cast<std::uint32_t>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&mapping_->producer_heartbeat_ms),
            0, 0));
    if (deadline_passed(now, heartbeat, kMaxCommandAgeMs)) {
      cancel(InputCancelReason::Disconnected);
      return;
    }
  }
  switch (kind) {
    case th10hook::kCallbackCalcPre:
      before_calc_pre(now, callback_id);
      break;
    case th10hook::kCallbackReplayPost:
      before_replay_post(callback_context, parent_callback_id);
      break;
    case th10hook::kCallbackPlayerPre:
      before_player_pre(now, callback_context, callback_id, parent_callback_id);
      break;
    case th10hook::kCallbackPlayerPost:
      before_player_post(now, callback_context, callback_id,
                         parent_callback_id);
      break;
    case th10hook::kCallbackCalcPost:
      before_calc_post(now, callback_id);
      break;
    default:
      // ReplayPre and unknown kinds need only the common guards.
      break;
  }
}

void InputController::before_calc_pre(std::uint32_t now,
                                      std::uint32_t callback_id) {
  if (calc_id_ != 0) cancel(InputCancelReason::WrongCallback);
  if (has_saved_input_ && !restore_input()) {
    cancel(InputCancelReason::InvalidContext);
    return;
  }
  clear_scope();
  calc_id_ = callback_id;
  TickContext gate{};
  std::uint32_t a = 0, b = 0;
  if (!read_tick_context(gate, a, b)) {
    InputAck ack{};
    control_.observe_gate(now, TickContext{false, false, false, false, true},
                          ack);
    if (ack.kind != InputAckKind::None) publish_gate_rejection(ack);
    clear_scope();
    return;
  }
  InputAck gate_ack{};
  if (!control_.observe_gate(now, gate, gate_ack)) {
    if (gate_ack.kind != InputAckKind::None) publish_gate_rejection(gate_ack);
    clear_scope();
    return;
  }
  calc_stage_tick_ = a;
  calc_segment_tick_ = b;
}

void InputController::before_replay_post(void* callback_context,
                                         std::uint32_t parent_callback_id) {
  replay_seen_ = calc_id_ != 0 && parent_callback_id == calc_id_ &&
                 validate_callback_context(callback_context,
                                           th10hook::kCallbackReplayPost);
  if (replay_seen_ && control_.active()) {
    const auto replay = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(callback_context));
    if (!has_held_contexts_) {
      held_replay_context_ = replay;
      has_held_contexts_ = true;
    } else if (held_replay_context_ != replay) {
      cancel(InputCancelReason::InvalidContext);
      return;
    }
  }
}

void InputController::before_player_pre(std::uint32_t now,
                                        void* callback_context,
                                        std::uint32_t callback_id,
                                        std::uint32_t parent_callback_id) {
  if (calc_id_ == 0 || parent_callback_id != calc_id_ || !replay_seen_ ||
      player_entered_ || player_calls_ != 0 || faulted_ ||
      !validate_callback_context(callback_context,
                                 th10hook::kCallbackPlayerPre)) {
    if (transaction_active_ || has_saved_input_)
      cancel(InputCancelReason::WrongCallback);
    return;
  }
  const auto player_context = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(callback_context));
  if (control_.active()) {
    if (has_held_contexts_ && held_player_context_ != 0 &&
        held_player_context_ != player_context) {
      cancel(InputCancelReason::InvalidContext);
      return;
    }
    if (held_player_context_ == 0) held_player_context_ = player_context;
  }
  TickContext gate{};
  std::uint32_t a = 0, b = 0;
  if (!read_tick_context(gate, a, b)) {
    if (transaction_active_ || has_saved_input_)
      cancel(InputCancelReason::InvalidContext);
    return;
  }
  if (!gate.gameplay_active ||
      static_cast<std::uint32_t>(a - calc_stage_tick_) != 1 ||
      static_cast<std::uint32_t>(b - calc_segment_tick_) != 1) {
    if (transaction_active_ || has_saved_input_)
      cancel(InputCancelReason::FailedChecks);
    return;
  }
  InputAck gate_ack{};
  if (!control_.observe_gate(now, gate, gate_ack)) {
    if (gate_ack.kind != InputAckKind::None) {
      publish_gate_rejection(gate_ack);
      clear_scope();
    }
    return;
  }
  apply_player_input(now, gate, callback_id);
}

void InputController::apply_player_input(std::uint32_t now,
                                         const TickContext& gate,
                                         std::uint32_t callback_id) {
  std::uint16_t actions = 0;
  InputAck ack{};
  if (!control_.player_callback_entry(now, gate, calc_id_, actions, ack)) {
    if (ack.kind == InputAckKind::None) return;
    if (ack.kind == InputAckKind::Cancelled) {
      transaction_active_ = false;
      has_held_contexts_ = false;
      held_player_context_ = held_replay_context_ = 0;
    }
    publish_ack(ack);
    return;
  }
  std::uint16_t current = 0;
  if (!memory_.read_u16(memory_.context, kInputCurrent, &current)) {
    cancel(InputCancelReason::InvalidContext);
    return;
  }
  // Hold the action through CalcPost for later consumers such as ItemManager.
  const std::uint16_t injected =
      static_cast<std::uint16_t>((current & ~kControlledInputMask) | actions);
  if (!memory_.write_u16(memory_.context, kInputCurrent, injected)) {
    cancel(InputCancelReason::InvalidContext);
    return;
  }
  original_input_ = current;
  injected_input_ = injected;
  has_saved_input_ = true;
  player_id_ = callback_id;
  player_entered_ = true;
  ++player_calls_;
  publish_ack(ack);
}

void InputController::publish_gate_rejection(InputAck& ack) {
  if (!restore_input()) ack.reason = InputCancelReason::InvalidContext;
  clear_transaction();
  publish_ack(ack);
}

void InputController::before_player_post(std::uint32_t now,
                                         void* callback_context,
                                         std::uint32_t callback_id,
                                         std::uint32_t parent_callback_id) {
  if (!player_entered_ || callback_id != player_id_ ||
      parent_callback_id != calc_id_ || player_exited_) {
    if (transaction_active_ || has_saved_input_)
      cancel(InputCancelReason::WrongCallback);
    return;
  }
  if (!validate_callback_context(callback_context,
                                 th10hook::kCallbackPlayerPost)) {
    cancel(InputCancelReason::InvalidContext);
    return;
  }
  TickContext gate{};
  std::uint32_t a = 0, b = 0;
  InputAck ack{};
  if (!read_tick_context(gate, a, b)) {
    cancel(InputCancelReason::InvalidContext);
    return;
  }
  if (!control_.player_callback_exit(now, gate, calc_id_, ack)) {
    cancel(ack.kind == InputAckKind::None ? InputCancelReason::InvalidContext
                                          : ack.reason);
    return;
  }
  player_exited_ = true;
  publish_ack(ack);
}

void InputController::before_calc_post(std::uint32_t now,
                                       std::uint32_t callback_id) {
  // A source-attested end-of-scene tick can contain only the Calc pair.
  // Leave its command armed for the outer gate to verify after the tick.
  if (callback_id == calc_id_ && control_.active() && !replay_seen_ &&
      !player_entered_ && !has_saved_input_)
    return;
  if (callback_id != calc_id_ || !player_entered_ || !player_exited_ ||
      player_calls_ != 1 || !has_saved_input_) {
    if (transaction_active_ || has_saved_input_)
      cancel(InputCancelReason::WrongCallback);
    return;
  }
  TickContext gate{};
  std::uint32_t a = 0, b = 0;
  if (!read_tick_context(gate, a, b)) {
    cancel(InputCancelReason::InvalidContext);
    return;
  }
  const bool counter_b_advanced =
      static_cast<std::uint32_t>(b - calc_segment_tick_) == 1;
  const bool counter_b_valid = counter_b_advanced || b == 0;
  if (!gate.gameplay_active ||
      static_cast<std::uint32_t>(a - calc_stage_tick_) != 1 ||
      !counter_b_valid) {
    cancel(InputCancelReason::FailedChecks);
    return;
  }
  InputAck ack{};
  if (!control_.calc_chain_end(
          now, gate, {calc_id_, calc_stage_tick_, a, player_calls_}, ack)) {
    publish_ack(ack);
    cancel(InputCancelReason::FailedChecks);
    return;
  }
  chain_ready_ = true;
  publish_ack(ack);
}

void InputController::AfterCallback(std::uint32_t kind,
                                    std::uint32_t callback_id,
                                    std::uint32_t parent_callback_id,
                                    SnapshotCommit snapshot) {
  if (!claim_owner_thread()) return;
  if (InterlockedExchange(&owner_violation_, 0) != 0) {
    cancel(InputCancelReason::InvalidContext);
    faulted_ = true;
    return;
  }
  if (reset_mode_) {
    if (transaction_active_ || has_saved_input_ || control_.active())
      cancel(InputCancelReason::Disconnected);
    return;
  }
  if ((!snapshot.valid || snapshot.sequence == 0) &&
      (transaction_active_ || has_saved_input_)) {
    cancel(InputCancelReason::InvalidSnapshot);
    return;
  }
  if (kind == th10hook::kCallbackCalcPost && callback_id == calc_id_ &&
      chain_ready_) {
    const bool restored = restore_input();
    if (!restored) {
      cancel(InputCancelReason::InvalidContext);
      return;
    }
    TickContext gate{};
    std::uint32_t a = 0, b = 0;
    if (!read_tick_context(gate, a, b)) {
      cancel(InputCancelReason::InvalidContext);
      return;
    }
    InputAck ack{};
    control_.complete_snapshot(GetTickCount(), gate, calc_id_, snapshot, ack);
    publish_ack(ack);
    transaction_active_ = control_.active();
    if (!transaction_active_) {
      has_held_contexts_ = false;
      held_player_context_ = held_replay_context_ = 0;
    }
    clear_scope();
  } else if (kind == th10hook::kCallbackCalcPost && callback_id == calc_id_) {
    clear_scope();
  } else if (kind == th10hook::kCallbackPlayerPre && player_entered_ &&
             callback_id != player_id_) {
    cancel(InputCancelReason::WrongCallback);
  } else if (kind == th10hook::kCallbackPlayerPost && player_exited_ &&
             (callback_id != player_id_ || parent_callback_id != calc_id_)) {
    cancel(InputCancelReason::WrongCallback);
  }
}

bool InputController::PollCommand() {
  if (!claim_owner_thread()) return false;
  if (InterlockedExchange(&owner_violation_, 0) != 0) {
    cancel(InputCancelReason::InvalidContext);
    faulted_ = true;
    return false;
  }
  if (!memory_ok()) {
    if (transaction_active_ || has_saved_input_)
      cancel(InputCancelReason::InvalidContext);
    faulted_ = true;
    return false;
  }
  const std::uint32_t now = GetTickCount();
  poll_command(now);
  if (!control_.active()) return false;

  TickContext gate{};
  std::uint32_t stage_tick = 0, segment_tick = 0;
  if (!read_tick_context(gate, stage_tick, segment_tick))
    gate = TickContext{false, false, false, false, true};
  InputAck ack{};
  if (!control_.observe_gate(now, gate, ack)) finish_rejected_poll(ack);
  return control_.active() && !faulted_;
}

bool InputController::CompleteTerminal(std::uint32_t calc_id,
                                       std::uint32_t snapshot_sequence) {
  InputAck ack{};
  if (!claim_owner_thread() || has_saved_input_ || faulted_ ||
      !control_.complete_terminal(GetTickCount(), calc_id, snapshot_sequence,
                                  ack))
    return false;
  clear_transaction();
  clear_scope();
  publish_ack(ack);
  return true;
}

void InputController::CancelCommand(InputCancelReason reason) {
  if (!claim_owner_thread()) return;
  if (InterlockedExchange(&owner_violation_, 0) != 0) {
    cancel(InputCancelReason::InvalidContext);
    faulted_ = true;
    return;
  }
  if (transaction_active_ || has_saved_input_ || control_.active())
    cancel(reason);
}

bool InputController::SetResetMode(bool enabled) {
  if (!claim_owner_thread()) return false;
  if (InterlockedExchange(&owner_violation_, 0) != 0) {
    cancel(InputCancelReason::InvalidContext);
    faulted_ = true;
    return false;
  }
  if (enabled) {
    if (transaction_active_ || has_saved_input_ || control_.active())
      cancel(InputCancelReason::Disconnected);
    if (faulted_ || transaction_active_ || has_saved_input_ ||
        control_.active())
      return false;
    reset_mode_ = true;
    return true;
  }
  if (!reset_mode_) return true;
  if (!discard_pending_reset_command()) return false;
  reset_mode_ = false;
  return true;
}

bool InputController::discard_pending_reset_command() {
  if (mapping_ == nullptr || mapping_->magic != InputChannelMagic ||
      mapping_->version != InputChannelVersion ||
      mapping_->size != sizeof(InputChannelMapping))
    return false;
  volatile LONG* publication =
      reinterpret_cast<volatile LONG*>(&mapping_->command_publication);
  const LONG before = InterlockedCompareExchange(publication, 0, 0);
  if (before == 0) return true;
  if (before < 0 || (before & 1)) return false;
  if (static_cast<std::uint32_t>(before) == last_publication_) return true;

  MemoryBarrier();
  const InputChannelCommand command = mapping_->command;
  MemoryBarrier();
  const LONG after = InterlockedCompareExchange(publication, 0, 0);
  if (before != after || (after & 1)) return false;

  last_publication_ = static_cast<std::uint32_t>(after);
  if (command.session_id == 0 || command.sequence == 0) return true;
  publish_ack(InputAck{1, sizeof(InputAck), command.session_id,
                       command.sequence, 0, 0, command.actions, 0,
                       InputAckKind::Cancelled, InputCancelReason::Disconnected,
                       0});
  return true;
}

bool InputController::ReadTickState(TickState& evidence) const {
  evidence = {};
  if (!memory_ok()) return false;
  std::uint32_t player = 0;
  if (!memory_.read_u32(memory_.context, kPlayerPointer, &player) ||
      !memory_.read_u32(memory_.context, kStageTick, &evidence.stage_tick) ||
      !memory_.read_u32(memory_.context, kSegmentTick,
                        &evidence.segment_tick) ||
      !bounded_address(player, kPlayerStateOffset + sizeof(std::uint32_t)) ||
      !memory_.read_u32(memory_.context, player + retail::kPlayerState,
                        &evidence.player_state) ||
      !memory_.read_u32(memory_.context, player + retail::kPlayerX,
                        &evidence.player_x_bits) ||
      !memory_.read_u32(memory_.context, player + retail::kPlayerY,
                        &evidence.player_y_bits))
    return false;
  evidence.player_pointer = player;
  return true;
}

bool CreateInputChannel(std::uint32_t pid, HANDLE* mapping_handle,
                        InputChannelMapping** mapping_view,
                        std::uint32_t* win32_error) {
  if (mapping_handle == nullptr || mapping_view == nullptr ||
      win32_error == nullptr || pid == 0)
    return false;
  *mapping_handle = nullptr;
  *mapping_view = nullptr;
  *win32_error = ERROR_INVALID_PARAMETER;
  wchar_t name[96]{};
  const int count = _snwprintf_s(name, sizeof(name) / sizeof(name[0]),
                                 _TRUNCATE, L"Local\\TH10Gym.Input.%lu",
                                 static_cast<unsigned long>(pid));
  if (count < 0) {
    *win32_error = ERROR_INVALID_NAME;
    return false;
  }
  HANDLE handle =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         sizeof(InputChannelMapping), name);
  if (handle == nullptr) {
    *win32_error = GetLastError();
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(handle);
    *win32_error = ERROR_ALREADY_EXISTS;
    return false;
  }
  auto* view = static_cast<InputChannelMapping*>(
      MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                    sizeof(InputChannelMapping)));
  if (view == nullptr) {
    *win32_error = GetLastError();
    CloseHandle(handle);
    return false;
  }
  std::memset(view, 0, sizeof(*view));
  view->magic = InputChannelMagic;
  view->version = InputChannelVersion;
  view->size = sizeof(InputChannelMapping);
  MemoryBarrier();
  *mapping_handle = handle;
  *mapping_view = view;
  *win32_error = ERROR_SUCCESS;
  return true;
}

GameMemoryAccess DefaultGameMemoryAccess() {
  return GameMemoryAccess{nullptr, read_u16, write_u16, read_u32};
}

}  // namespace th10hook
