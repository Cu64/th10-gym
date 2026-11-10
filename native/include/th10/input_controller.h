#pragma once

#include "input_channel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace th10hook {

// The production callbacks use guarded raw game-memory access. Offline fixtures
// provide the same three functions against ordinary test memory.
struct GameMemoryAccess {
  void* context;
  bool (*read_u16)(void*, std::uintptr_t, std::uint16_t*);
  bool (*write_u16)(void*, std::uintptr_t, std::uint16_t);
  bool (*read_u32)(void*, std::uintptr_t, std::uint32_t*);
};

struct TickState {
  std::uint32_t stage_tick;
  std::uint32_t segment_tick;
  std::uint32_t player_pointer;
  std::uint32_t player_state;
  std::uint32_t player_x_bits;
  std::uint32_t player_y_bits;
};

class InputController {
 public:
  explicit InputController(InputChannelMapping* mapping,
                           const GameMemoryAccess& memory);

  // Called only on the game thread, around each callback sample.
  // These methods perform bounded reads, writes, Interlocked operations, and
  // GetTickCount only. They do not allocate, wait, or access files.
  void BeforeCallback(void* callback_context, std::uint32_t callback_kind,
                      std::uint32_t callback_id,
                      std::uint32_t parent_callback_id);
  void AfterCallback(std::uint32_t callback_kind, std::uint32_t callback_id,
                     std::uint32_t parent_callback_id, SnapshotCommit snapshot);

  // Used by the gate while the game is halted.
  // These calls stay on the game thread and perform no wait or allocation.
  bool PollCommand();
  void CancelCommand(InputCancelReason reason);
  bool SetResetMode(bool enabled);
  bool CompleteTerminal(std::uint32_t calc_id, std::uint32_t snapshot_sequence);
  bool ReadTickState(TickState& evidence) const;
  bool active() const { return control_.active() && !faulted_; }
  bool faulted() const { return faulted_; }

  bool initialized() const { return mapping_ != nullptr && memory_ok(); }

 private:
  void before_calc_pre(std::uint32_t now, std::uint32_t callback_id);
  void before_replay_post(void* callback_context,
                          std::uint32_t parent_callback_id);
  void before_player_pre(std::uint32_t now, void* callback_context,
                         std::uint32_t callback_id,
                         std::uint32_t parent_callback_id);
  void apply_player_input(std::uint32_t now, const TickContext& gate,
                          std::uint32_t callback_id);
  void before_player_post(std::uint32_t now, void* callback_context,
                          std::uint32_t callback_id,
                          std::uint32_t parent_callback_id);
  void before_calc_post(std::uint32_t now, std::uint32_t callback_id);
  bool memory_ok() const;
  void poll_command(std::uint32_t now_ms);
  bool read_new_command(InputChannelCommand& command);
  bool accept_command_session(std::uint32_t session_id);
  void finish_rejected_poll(InputAck& ack);
  void publish_ack(const InputAck& ack);
  void publish_gate_rejection(InputAck& ack);
  void cancel(InputCancelReason reason);
  bool read_tick_context(TickContext& gate, std::uint32_t& stage_tick,
                         std::uint32_t& segment_tick);
  bool validate_callback_context(void* callback_context, std::uint32_t kind);
  bool restore_input();
  void clear_transaction();
  void clear_scope();
  bool discard_pending_reset_command();
  bool claim_owner_thread();

  InputChannelMapping* mapping_;
  GameMemoryAccess memory_;
  TickInputControl control_;
  std::uint32_t session_id_;
  std::uint32_t last_command_sequence_;
  std::uint32_t last_publication_;
  std::uint32_t calc_id_;
  std::uint32_t calc_stage_tick_;
  std::uint32_t calc_segment_tick_;
  std::uint32_t player_id_;
  std::uint32_t held_player_context_;
  std::uint32_t held_replay_context_;
  std::uint32_t player_calls_;
  std::uint16_t original_input_;
  std::uint16_t injected_input_;
  bool has_session_;
  bool has_saved_input_;
  bool replay_seen_;
  bool player_entered_;
  bool player_exited_;
  bool chain_ready_;
  bool has_held_contexts_;
  bool faulted_;
  bool transaction_active_;
  bool reset_mode_;
  volatile PVOID owner_thread_token_;
  volatile LONG owner_violation_;
};

// Create and initialize the PID-scoped 72-byte mapping. Call from the hook
// initializer, before any code hook can run. Existing mappings are rejected.
bool CreateInputChannel(std::uint32_t pid, HANDLE* mapping_handle,
                        InputChannelMapping** mapping_view,
                        std::uint32_t* win32_error);
GameMemoryAccess DefaultGameMemoryAccess();

}  // namespace th10hook
