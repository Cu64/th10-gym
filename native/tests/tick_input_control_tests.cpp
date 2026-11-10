// Offline tests for the single-tick input state machine.
#include <cassert>
#include "input_channel.h"

using th10hook::Bomb;
using th10hook::InputAck;
using th10hook::InputAckKind;
using th10hook::InputCancelReason;
using th10hook::InputCommand;
using th10hook::InputMaxTtlMs;
using th10hook::InputRequiredChecks;
using th10hook::InputSubmitResult;
using th10hook::Left;
using th10hook::Right;
using th10hook::Shoot;
using th10hook::TickContext;
using th10hook::TickInputControl;
using th10hook::Up;

static InputCommand command(std::uint32_t epoch, std::uint32_t sequence,
                            std::uint16_t actions = Shoot) {
  return InputCommand{epoch, sequence, actions};
}

static TickContext active_gate() {
  return TickContext{true, true, false, false, true};
}

static void test_complete_command_carries_matching_ids() {
  TickInputControl control;
  InputAck ack{};
  std::uint16_t actions = 0xffff;
  assert(control.begin_session(41));
  assert(control.submit(command(41, 1, Shoot | Right), 100, 100, ack) ==
         InputSubmitResult::Accepted);
  assert(ack.kind == InputAckKind::Accepted && ack.command_sequence == 1);
  assert(ack.version == 1 && ack.size == sizeof(InputAck));
  assert(control.player_callback_entry(101, active_gate(), 900, actions, ack));
  assert(actions == (Shoot | Right));
  assert(ack.kind == InputAckKind::PlayerEntry && ack.calc_callback_id == 900 &&
         ack.player_callback_exits == 0);
  assert(control.player_callback_exit(102, active_gate(), 900, ack));
  assert(ack.kind == InputAckKind::PlayerExit &&
         ack.player_callback_exits == 1);
  assert(control.calc_chain_end(103, active_gate(), {900, 44, 45, 1}, ack));
  assert(ack.kind == InputAckKind::CalcChainEnd &&
         ack.checks == InputRequiredChecks);
  assert(control.complete_snapshot(104, active_gate(), 900, {77, true}, ack));
  assert(ack.kind == InputAckKind::SnapshotComplete && ack.session_id == 41 &&
         ack.command_sequence == 1 && ack.calc_callback_id == 900 &&
         ack.snapshot_sequence == 77 && ack.player_callback_exits == 1 &&
         ack.checks == InputRequiredChecks);
  assert(!control.active());
}

static void test_replays_stale_and_invalid_commands_fail() {
  TickInputControl control;
  InputAck ack{};
  assert(control.begin_session(41));
  const auto cmd = command(41, 2, Bomb);
  assert(control.submit(cmd, 200, 100, ack) == InputSubmitResult::Accepted);
  assert(control.submit(cmd, 200, 100, ack) == InputSubmitResult::Busy);
  assert(control.cancel(InputCancelReason::Disconnected, ack));
  assert(ack.kind == InputAckKind::Cancelled &&
         ack.reason == InputCancelReason::Disconnected && ack.actions == Bomb);
  assert(control.submit(cmd, 201, 100, ack) ==
         InputSubmitResult::StaleOrDuplicate);
  assert(control.submit(command(41, 1), 201, 100, ack) ==
         InputSubmitResult::StaleOrDuplicate);
  assert(control.submit(command(41, 3), 300, 0, ack) ==
         InputSubmitResult::Invalid);
  assert(control.submit(command(41, 3), 300, InputMaxTtlMs + 1, ack) ==
         InputSubmitResult::Invalid);
  assert(control.submit(command(41, 3, Left | Right), 300, 100, ack) ==
         InputSubmitResult::Invalid);
}

static void test_context_loss_clears_the_action() {
  TickInputControl control;
  InputAck ack{};
  std::uint16_t actions = 0xffff;
  assert(control.begin_session(41));
  std::uint32_t sequence = 3;

  auto gate = active_gate();
  gate.paused = true;
  assert(control.submit(command(41, sequence++, Left), 400, 100, ack) ==
         InputSubmitResult::Accepted);
  assert(!control.player_callback_entry(401, gate, 901, actions, ack));
  assert(actions == 0 && ack.reason == InputCancelReason::InvalidContext);

  gate = active_gate();
  gate.resetting = true;
  assert(control.submit(command(41, sequence++, Left), 500, 100, ack) ==
         InputSubmitResult::Accepted);
  assert(!control.observe_gate(501, gate, ack));
  assert(ack.reason == InputCancelReason::InvalidContext);

  gate = active_gate();
  gate.state_readable = false;
  assert(control.submit(command(41, sequence++, Left), 600, 100, ack) ==
         InputSubmitResult::Accepted);
  assert(!control.observe_gate(601, gate, ack));
  assert(ack.reason == InputCancelReason::InvalidContext);

  gate = active_gate();
  gate.connected = false;
  assert(control.submit(command(41, sequence++, Left), 700, 100, ack) ==
         InputSubmitResult::Accepted);
  assert(!control.observe_gate(701, gate, ack));
  assert(ack.reason == InputCancelReason::Disconnected);
}

static void test_expiry_is_wrap_safe() {
  TickInputControl control;
  InputAck ack{};
  assert(control.begin_session(41));
  assert(control.submit(command(41, 7, Up), 0xfffffff0u, 32, ack) ==
         InputSubmitResult::Accepted);
  assert(control.observe_gate(0xffffffffu, active_gate(), ack));
  assert(!control.observe_gate(0x10u, active_gate(), ack));
  assert(ack.reason == InputCancelReason::Expired);
}

static void test_callbacks_must_match_in_order() {
  TickInputControl control;
  InputAck ack{};
  std::uint16_t actions = 0xffff;
  assert(control.begin_session(41));

  // Entry and exit must refer to the same nonzero callback.
  assert(control.submit(command(41, 8), 1000, 100, ack) ==
         InputSubmitResult::Accepted);
  assert(!control.player_callback_entry(1001, active_gate(), 0, actions, ack));
  assert(actions == 0 && ack.reason == InputCancelReason::WrongCallback);
  assert(control.submit(command(41, 9), 1100, 100, ack) ==
         InputSubmitResult::Accepted);
  assert(control.player_callback_entry(1101, active_gate(), 902, actions, ack));
  assert(!control.player_callback_exit(1102, active_gate(), 903, ack));
  assert(ack.reason == InputCancelReason::WrongCallback);

  // A second Player call before the snapshot invalidates the command.
  assert(control.submit(command(41, 10), 1200, 100, ack) ==
         InputSubmitResult::Accepted);
  assert(control.player_callback_entry(1201, active_gate(), 904, actions, ack));
  assert(control.player_callback_exit(1202, active_gate(), 904, ack));
  assert(
      !control.player_callback_entry(1203, active_gate(), 905, actions, ack));
  assert(actions == 0 && ack.reason == InputCancelReason::WrongCallback);
}

static void test_snapshot_must_follow_a_valid_chain() {
  TickInputControl control;
  InputAck ack{};
  std::uint16_t actions = 0xffff;
  assert(control.begin_session(41));
  std::uint32_t sequence = 1;
  const auto run_to_exit = [&](std::uint32_t now, std::uint32_t callback) {
    assert(control.submit(command(41, sequence++), now, 100, ack) ==
           InputSubmitResult::Accepted);
    assert(control.player_callback_entry(now + 1, active_gate(), callback,
                                         actions, ack));
    assert(control.player_callback_exit(now + 2, active_gate(), callback, ack));
  };

  run_to_exit(1000, 904);
  assert(!control.complete_snapshot(1003, active_gate(), 904, {1, true}, ack));
  assert(ack.reason == InputCancelReason::InvalidSnapshot);

  run_to_exit(1100, 905);
  assert(control.calc_chain_end(1103, active_gate(), {905, 1, 2, 1}, ack));
  assert(!control.complete_snapshot(1104, active_gate(), 905, {0, true}, ack));
  assert(ack.reason == InputCancelReason::InvalidSnapshot);

  run_to_exit(1200, 906);
  assert(control.calc_chain_end(1203, active_gate(), {906, 1, 2, 1}, ack));
  assert(
      !control.complete_snapshot(1204, active_gate(), 906, {88, false}, ack));
  assert(ack.reason == InputCancelReason::InvalidSnapshot);

  // The chain requires one counter advance and one Player call.
  run_to_exit(1300, 907);
  assert(!control.calc_chain_end(1303, active_gate(), {907, 5, 7, 1}, ack));
  assert(ack.reason == InputCancelReason::FailedChecks);
  run_to_exit(1400, 908);
  assert(!control.calc_chain_end(1403, active_gate(), {908, 7, 8, 2}, ack));
  assert(ack.reason == InputCancelReason::FailedChecks);
}

static void test_terminal_tick_has_no_player_callback() {
  TickInputControl control;
  InputAck ack{};
  assert(control.begin_session(41));
  assert(!control.complete_terminal(100, 900, 77, ack));
  assert(control.submit(command(41, 1, Left), 100, 100, ack) ==
         InputSubmitResult::Accepted);
  assert(!control.complete_terminal(101, 0, 77, ack));
  assert(!control.complete_terminal(101, 900, 0, ack));
  assert(control.complete_terminal(101, 900, 77, ack));
  assert(ack.kind == InputAckKind::TerminalTick &&
         ack.reason == InputCancelReason::None && ack.calc_callback_id == 900 &&
         ack.snapshot_sequence == 77 && ack.player_callback_exits == 0 &&
         ack.checks == 0);
  assert(!control.active());
}

static void test_session_epochs_only_advance() {
  TickInputControl control;
  InputAck ack{};
  assert(!control.begin_session(0));
  assert(!control.begin_session(UINT32_MAX));
  assert(control.begin_session(41));
  assert(!control.begin_session(41));
  assert(control.begin_session(42));
  assert(control.submit(command(41, 12), 1400, 100, ack) ==
         InputSubmitResult::StaleOrDuplicate);
  assert(control.submit(command(42, 1), 1400, 100, ack) ==
         InputSubmitResult::Accepted);
}

int main() {
  test_complete_command_carries_matching_ids();
  test_replays_stale_and_invalid_commands_fail();
  test_context_loss_clears_the_action();
  test_expiry_is_wrap_safe();
  test_callbacks_must_match_in_order();
  test_snapshot_must_follow_a_valid_chain();
  test_terminal_tick_has_no_player_callback();
  test_session_epochs_only_advance();
  return 0;
}
