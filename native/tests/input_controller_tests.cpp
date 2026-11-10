#include <cassert>
#include <cstdint>
#include <thread>

#include "input_controller.h"
#include "hook_status.h"

using th10hook::Bomb;
using th10hook::Down;
using th10hook::Focus;
using th10hook::GameMemoryAccess;
using th10hook::InputAckKind;
using th10hook::InputCancelReason;
using th10hook::InputChannelCommand;
using th10hook::InputChannelMagic;
using th10hook::InputChannelMapping;
using th10hook::InputChannelVersion;
using th10hook::InputController;
using th10hook::Left;
using th10hook::Right;
using th10hook::Shoot;
using th10hook::Up;
namespace {
constexpr std::uintptr_t kInput = 0x00474e5c;
constexpr std::uintptr_t kPlayerPointer = 0x00477834;
constexpr std::uintptr_t kReplayPointer = 0x00477838;
constexpr std::uintptr_t kStageTick = 0x00474c88;
constexpr std::uintptr_t kSegmentTick = 0x00474c8c;
constexpr std::uint32_t kPlayer = 0x00600000;
constexpr std::uint32_t kReplay = 0x00610000;

struct Fixture {
  InputChannelMapping mapping{};
  std::uint16_t input = 0x0105;
  std::uint32_t player = kPlayer;
  std::uint32_t replay = kReplay;
  std::uint32_t state = 1;
  std::uint32_t mode = 0;
  std::uint32_t stage_tick = 100;
  std::uint32_t segment_tick = 200;
};

bool fixture_read16(void* context, std::uintptr_t address, std::uint16_t* out) {
  auto& f = *static_cast<Fixture*>(context);
  if (address != kInput || out == nullptr) return false;
  *out = f.input;
  return true;
}
bool fixture_write16(void* context, std::uintptr_t address,
                     std::uint16_t value) {
  auto& f = *static_cast<Fixture*>(context);
  if (address != kInput) return false;
  f.input = value;
  return true;
}
bool fixture_read32(void* context, std::uintptr_t address, std::uint32_t* out) {
  auto& f = *static_cast<Fixture*>(context);
  if (out == nullptr) return false;
  if (address == static_cast<std::uintptr_t>(f.replay) + 0x10) {
    *out = f.mode;
    return true;
  }
  switch (address) {
    case kPlayerPointer:
      *out = f.player;
      return true;
    case kReplayPointer:
      *out = f.replay;
      return true;
    case kPlayer + 0x458:
      *out = f.state;
      return true;
    case kStageTick:
      *out = f.stage_tick;
      return true;
    case kSegmentTick:
      *out = f.segment_tick;
      return true;
    default:
      return false;
  }
}

void publish(Fixture& f, std::uint32_t sequence, std::uint16_t actions,
             std::uint16_t updates = 1, std::uint32_t flags = 0,
             std::uint32_t ttl_ms = 1000) {
  const auto now = GetTickCount();
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.mapping.producer_heartbeat_ms),
      static_cast<LONG>(now));
  const LONG old = InterlockedCompareExchange(
      reinterpret_cast<volatile LONG*>(&f.mapping.command_publication), 0, 0);
  LONG odd = (old & 1) ? old + 2 : old + 1;
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.mapping.command_publication), odd);
  f.mapping.command =
      InputChannelCommand{9, sequence, now, ttl_ms, actions, updates, flags};
  MemoryBarrier();
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.mapping.command_publication),
      odd + 1);
}

InputController make_controller(Fixture& f) {
  f.mapping.magic = InputChannelMagic;
  f.mapping.version = InputChannelVersion;
  f.mapping.size = sizeof(InputChannelMapping);
  return InputController(
      &f.mapping,
      GameMemoryAccess{&f, fixture_read16, fixture_write16, fixture_read32});
}

void finish_observation(InputController& c, std::uint32_t kind,
                        std::uint32_t id, std::uint32_t parent) {
  c.AfterCallback(kind, id, parent, {id + 1000, true});
}

void complete_left_command() {
  Fixture f;
  f.replay =
      0x001de4c8;  // Captured Stage 1 ReplayManager below the image base.
  auto c = make_controller(f);
  publish(f, 1, Left);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 11, 0);
  finish_observation(c, th10hook::kCallbackCalcPre, 11, 0);
  f.stage_tick++;
  f.segment_tick++;
  c.BeforeCallback(reinterpret_cast<void*>(f.replay),
                   th10hook::kCallbackReplayPost, 12, 11);
  finish_observation(c, th10hook::kCallbackReplayPost, 12, 11);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 13, 11);
  assert(f.input == static_cast<std::uint16_t>((0x0105 & ~0x00f7) | Left));
  finish_observation(c, th10hook::kCallbackPlayerPre, 13, 11);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPost, 13, 11);
  finish_observation(c, th10hook::kCallbackPlayerPost, 13, 11);
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, 11, 0);
  assert(f.input == static_cast<std::uint16_t>((0x0105 & ~0x00f7) | Left));
  c.AfterCallback(th10hook::kCallbackCalcPost, 11, 0, {77, true});
  assert(f.input == 0x0105);
  assert(f.mapping.ack.kind == InputAckKind::SnapshotComplete);
  assert(f.mapping.ack.actions == Left &&
         f.mapping.ack.snapshot_sequence == 77);
  assert(f.mapping.ack.calc_callback_id == 11);
}

void bad_counter_delta_never_writes() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Right);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 21, 0);
  f.stage_tick++;  // Counter B did not advance.
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 22, 21);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 23, 21);
  assert(f.input == 0x0105);
  assert(f.mapping.ack.kind != InputAckKind::SnapshotComplete);
}

void counter_reset_never_writes() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Left);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 24, 0);
  f.stage_tick = 99;
  f.segment_tick = 201;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 25, 24);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 26, 24);
  assert(f.input == 0x0105);
  assert(f.mapping.ack.kind == InputAckKind::Cancelled);
}

void stale_callback_object_never_writes() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Right);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 27, 0);
  f.stage_tick++;
  f.segment_tick++;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 28, 27);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer + 4),
                   th10hook::kCallbackPlayerPre, 29, 27);
  assert(f.input == 0x0105);
  assert(f.mapping.ack.kind == InputAckKind::Cancelled);
}

void foreign_thread_cannot_apply_input() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Left);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 30, 0);
  f.stage_tick++;
  f.segment_tick++;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 31, 30);
  std::thread foreign([&] {
    c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                     th10hook::kCallbackPlayerPre, 32, 30);
  });
  foreign.join();
  assert(f.input == 0x0105);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 32, 30);
  assert(f.input == 0x0105);
  assert(f.mapping.ack.kind == InputAckKind::Cancelled);
}

void duplicate_player_restores_immediately() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Left);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 31, 0);
  f.stage_tick++;
  f.segment_tick++;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 32, 31);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 33, 31);
  assert(f.input != 0x0105);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 34, 31);
  assert(f.input == 0x0105);
  assert(f.mapping.ack.kind == InputAckKind::Cancelled);
}

void failed_snapshot_cannot_ack_success() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Right);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 41, 0);
  f.stage_tick++;
  f.segment_tick++;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 42, 41);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 43, 41);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPost, 43, 41);
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, 41, 0);
  c.AfterCallback(th10hook::kCallbackCalcPost, 41, 0, {0, false});
  assert(f.input == 0x0105);
  assert(f.mapping.ack.kind == InputAckKind::Cancelled);
  assert(f.mapping.ack.kind != InputAckKind::SnapshotComplete);
}

void override_conflict_is_never_overwritten() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Left);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 51, 0);
  f.stage_tick++;
  f.segment_tick++;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 52, 51);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 53, 51);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPost, 53, 51);
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, 51, 0);
  f.input = 0x0222;  // Simulate another producer taking ownership.
  c.AfterCallback(th10hook::kCallbackCalcPost, 51, 0, {88, true});
  assert(f.input ==
         0x0227);  // Restore unchanged owned bits; preserve conflicts.
  assert(f.mapping.ack.kind == InputAckKind::Cancelled);
  assert(f.mapping.ack.reason == InputCancelReason::InvalidContext);
}

void invalid_player_state_cancels_armed_command() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Right);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 61, 0);
  f.state = 5;
  f.stage_tick++;
  f.segment_tick++;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 62, 61);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 63, 61);
  assert(f.input == 0x0105);
  assert(f.mapping.ack.kind == InputAckKind::Cancelled);
}

void calc_only_tick_leaves_command_armed_for_the_terminal_check() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Right);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 64, 0);
  // A verified end-of-scene tick has no Replay or Player call. The outer gate
  // decides afterward whether it was terminal.
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, 64, 0);
  assert(f.input == 0x0105);
  assert(f.mapping.ack.kind == InputAckKind::Accepted);
  assert(c.CompleteTerminal(64, 70));
  assert(f.mapping.ack.kind == InputAckKind::TerminalTick &&
         f.mapping.ack.snapshot_sequence == 70);
}

void passive_calc_chains_do_not_retain_scope() {
  Fixture f;
  auto c = make_controller(f);
  for (std::uint32_t id = 71; id != 76; ++id) {
    c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, id, 0);
    finish_observation(c, th10hook::kCallbackCalcPre, id, 0);
    c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, id, 0);
    finish_observation(c, th10hook::kCallbackCalcPost, id, 0);
    assert(f.input == 0x0105);
  }
  assert(f.mapping.ack.kind == InputAckKind::None);
}

void run_one_chain(InputController& c, Fixture& f, std::uint32_t id,
                   std::uint16_t actions = Left) {
  const auto original_input = f.input;
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, id, 0);
  ++f.stage_tick;
  ++f.segment_tick;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, id + 1, id);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, id + 2, id);
  constexpr std::uint16_t kControlledBits = 0x00f7;
  assert(f.input == static_cast<std::uint16_t>(
                        (original_input & ~kControlledBits) | actions));
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPost, id + 2, id);
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, id, 0);
  c.AfterCallback(th10hook::kCallbackCalcPost, id, 0, {id + 1000, true});
  assert(f.input == original_input);
}

void serialized_shoot_commands_complete_and_restore_input() {
  Fixture f;
  f.input = 0x0104;  // Shoot is released; unrelated input bits remain set.
  auto c = make_controller(f);
  for (std::uint32_t i = 0; i < 40; ++i) {
    publish(f, i + 1, Shoot);
    c.PollCommand();
    run_one_chain(c, f, 500 + i * 10, Shoot);
    assert(f.mapping.ack.kind == InputAckKind::SnapshotComplete);
    assert(f.mapping.ack.actions == Shoot &&
           f.mapping.ack.player_callback_exits == 1);
    assert(f.input == 0x0104);
  }
}

void serialized_shoot_and_cardinal_commands_complete_and_restore_input() {
  for (const auto movement : {Up, Down, Left, Right}) {
    Fixture f;
    f.input = 0x0104;  // Original shoot bit is released.
    auto c = make_controller(f);
    const auto actions = static_cast<std::uint16_t>(Shoot | movement);
    publish(f, 1, actions);
    c.PollCommand();
    run_one_chain(c, f, 580, actions);
    assert(f.mapping.ack.kind == InputAckKind::SnapshotComplete);
    assert(f.mapping.ack.actions == actions &&
           f.mapping.ack.player_callback_exits == 1);
    assert(f.input == 0x0104);
  }
}

void shoot_restore_preserves_unowned_input_changes() {
  Fixture f;
  f.input = 0x0104;
  auto c = make_controller(f);
  publish(f, 1, Shoot);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 540, 0);
  ++f.stage_tick;
  ++f.segment_tick;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 541, 540);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 542, 540);
  assert((f.input & Shoot) != 0);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPost, 542, 540);
  f.input = static_cast<std::uint16_t>(f.input |
                                       0x0008);  // An unrelated producer bit.
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, 540, 0);
  c.AfterCallback(th10hook::kCallbackCalcPost, 540, 0, {1540, true});
  assert(f.input == 0x010c);
  assert((f.input & Shoot) == 0);
  assert(f.mapping.ack.kind == InputAckKind::SnapshotComplete);
}

void shoot_restore_clears_shoot_when_another_action_bit_conflicts() {
  Fixture f;
  f.input = 0x0104;
  auto c = make_controller(f);
  publish(f, 1, Shoot);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 550, 0);
  ++f.stage_tick;
  ++f.segment_tick;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 551, 550);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 552, 550);
  assert((f.input & Shoot) != 0);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPost, 552, 550);
  f.input =
      static_cast<std::uint16_t>(f.input | Left);  // Competing movement writer.
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, 550, 0);
  c.AfterCallback(th10hook::kCallbackCalcPost, 550, 0, {1550, true});
  assert((f.input & Shoot) == 0);
  assert((f.input & Left) != 0);
  assert(f.mapping.ack.kind == InputAckKind::Cancelled);
  assert(f.mapping.ack.reason == InputCancelReason::InvalidContext);
}

void serialized_vertical_commands_complete() {
  for (const auto action : {Up, Down}) {
    Fixture f;
    auto c = make_controller(f);
    publish(f, 1, action);
    c.PollCommand();
    run_one_chain(c, f, 300, action);
    assert(f.mapping.ack.kind == InputAckKind::SnapshotComplete);
    assert(f.mapping.ack.actions == action &&
           f.mapping.ack.player_callback_exits == 1);
    assert(f.input == 0x0105);
  }
}

void serialized_diagonal_focus_and_bomb_commands_complete() {
  constexpr std::uint16_t directions[] = {
      0,
      Up,
      Down,
      Left,
      Right,
      static_cast<std::uint16_t>(Up | Left),
      static_cast<std::uint16_t>(Up | Right),
      static_cast<std::uint16_t>(Down | Left),
      static_cast<std::uint16_t>(Down | Right)};
  std::uint32_t sequence = 1;
  for (const auto direction : directions) {
    for (const auto focus : {std::uint16_t{0}, std::uint16_t{Focus}}) {
      const auto base = static_cast<std::uint16_t>(Shoot | direction | focus);
      for (const auto bomb : {std::uint16_t{0}, std::uint16_t{Bomb}}) {
        const auto action = static_cast<std::uint16_t>(base | bomb);
        Fixture f;
        f.input = 0x0105;
        auto c = make_controller(f);
        publish(f, sequence++, action, 1);
        c.PollCommand();
        c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 390, 0);
        ++f.stage_tick;
        ++f.segment_tick;
        c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                         th10hook::kCallbackReplayPost, 391, 390);
        c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                         th10hook::kCallbackPlayerPre, 392, 390);
        assert((f.input & 0x00f7) == action);
        c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                         th10hook::kCallbackPlayerPost, 392, 390);
        // The focus bit remains set until CalcPost so later ItemManager work
        // in this calculation chain can consume it.
        assert((f.input & 0x00f7) == action);
        c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, 390, 0);
        c.AfterCallback(th10hook::kCallbackCalcPost, 390, 0,
                        {1390 + sequence, true});
        assert(f.input == 0x0105);
        assert(f.mapping.ack.kind == InputAckKind::SnapshotComplete);
        assert(f.mapping.ack.actions == action &&
               f.mapping.ack.player_callback_exits == 1);
      }
    }
  }
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, Left | Right, 1);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 490, 0);
  assert(f.mapping.ack.kind == InputAckKind::Cancelled);
  assert(f.mapping.ack.reason == InputCancelReason::InvalidCommand);
}

void bomb_pulse_is_visible_through_calc_post_then_restored() {
  Fixture f;
  f.input = 0;
  auto c = make_controller(f);
  const auto actions = static_cast<std::uint16_t>(Shoot | Up | Bomb);
  publish(f, 1, actions, 1);
  c.PollCommand();
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, 395, 0);
  ++f.stage_tick;
  ++f.segment_tick;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, 396, 395);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, 397, 395);
  assert(f.input == actions);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPost, 397, 395);
  assert(f.input == actions);
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, 395, 0);
  c.AfterCallback(th10hook::kCallbackCalcPost, 395, 0, {1395, true});
  assert(f.input == 0);
  assert(f.mapping.ack.kind == InputAckKind::SnapshotComplete);
}

void neutral_command_preserves_nonmovement_bits_and_holds() {
  Fixture f;
  auto c = make_controller(f);
  publish(f, 1, 0, 1);
  c.PollCommand();
  run_one_chain(c, f, 70, 0);
  assert(f.mapping.ack.kind == InputAckKind::SnapshotComplete &&
         f.mapping.ack.player_callback_exits == 1);
}

}  // namespace

int main() {
  complete_left_command();
  serialized_shoot_commands_complete_and_restore_input();
  serialized_shoot_and_cardinal_commands_complete_and_restore_input();
  shoot_restore_preserves_unowned_input_changes();
  shoot_restore_clears_shoot_when_another_action_bit_conflicts();
  serialized_vertical_commands_complete();
  serialized_diagonal_focus_and_bomb_commands_complete();
  bomb_pulse_is_visible_through_calc_post_then_restored();
  bad_counter_delta_never_writes();
  counter_reset_never_writes();
  stale_callback_object_never_writes();
  foreign_thread_cannot_apply_input();
  duplicate_player_restores_immediately();
  failed_snapshot_cannot_ack_success();
  override_conflict_is_never_overwritten();
  invalid_player_state_cancels_armed_command();
  calc_only_tick_leaves_command_armed_for_the_terminal_check();
  passive_calc_chains_do_not_retain_scope();
  neutral_command_preserves_nonmovement_bits_and_holds();
}
