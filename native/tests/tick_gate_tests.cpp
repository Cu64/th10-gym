#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "tick_verifier.h"
#include "tick_gate.h"

using th10hook::CallbackSample;
using th10hook::Down;
using th10hook::escape_pressed_for_foreground_process;
using th10hook::GameMemoryAccess;
using th10hook::GateChannelMagic;
using th10hook::GateChannelMapping;
using th10hook::GateChannelVersion;
using th10hook::GateCommandResetBegin;
using th10hook::GateCommandResetRearm;
using th10hook::GateCommandSingleTick;
using th10hook::GateError;
using th10hook::GateMaxHeartbeatAgeMs;
using th10hook::GateResetMaxDurationMs;
using th10hook::GateState;
using th10hook::GateTickState;
using th10hook::InputAckKind;
using th10hook::InputChannelCommand;
using th10hook::InputChannelMagic;
using th10hook::InputChannelMapping;
using th10hook::InputChannelVersion;
using th10hook::InputController;
using th10hook::InputMaxTtlMs;
using th10hook::kCallbackCalcPost;
using th10hook::kCallbackCalcPre;
using th10hook::kCallbackPlayerPost;
using th10hook::kCallbackPlayerPre;
using th10hook::kCallbackReplayPost;
using th10hook::kCallbackReplayPre;
using th10hook::kSampleFlagReadFault;
using th10hook::Left;
using th10hook::Right;
using th10hook::Shoot;
using th10hook::TickGate;
using th10hook::TickVerifier;
using th10hook::Up;
namespace {
constexpr std::uintptr_t kInput = 0x00474e5c;
constexpr std::uintptr_t kPlayerPointer = 0x00477834;
constexpr std::uintptr_t kReplayPointer = 0x00477838;
constexpr std::uintptr_t kStageTick = 0x00474c88;
constexpr std::uintptr_t kSegmentTick = 0x00474c8c;
constexpr std::uint32_t kPlayer = 0x00600000;
constexpr std::uint32_t kReplay = 0x00610000;
constexpr std::uint32_t kUnknownCommandFlag = 1;

struct Fixture {
  InputChannelMapping input_mapping{};
  GateChannelMapping step_mapping{};
  std::uint16_t input = 0x0105;
  std::uint32_t player = kPlayer;
  std::uint32_t replay = kReplay;
  std::uint32_t state = 1;
  std::uint32_t mode = 0;
  std::uint32_t stage_tick = 100;
  std::uint32_t segment_tick = 200;
  std::uint32_t x_bits = 0x3f800000;
  std::uint32_t y_bits = 0x40000000;
  bool escape = false;
};

bool read16(void* p, std::uintptr_t address, std::uint16_t* out) {
  auto& f = *static_cast<Fixture*>(p);
  if (address != kInput || out == nullptr) return false;
  *out = f.input;
  return true;
}
bool write16(void* p, std::uintptr_t address, std::uint16_t value) {
  auto& f = *static_cast<Fixture*>(p);
  if (address != kInput) return false;
  f.input = value;
  return true;
}
bool read32(void* p, std::uintptr_t address, std::uint32_t* out) {
  auto& f = *static_cast<Fixture*>(p);
  if (out == nullptr) return false;
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
    case kPlayer + 0x3c0:
      *out = f.x_bits;
      return true;
    case kPlayer + 0x3c4:
      *out = f.y_bits;
      return true;
    case kReplay + 0x10:
      *out = f.mode;
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
bool escape_probe(void* p) { return static_cast<Fixture*>(p)->escape; }

void init(Fixture& f) {
  f.input_mapping.magic = InputChannelMagic;
  f.input_mapping.version = InputChannelVersion;
  f.input_mapping.size = sizeof(InputChannelMapping);
  f.step_mapping.magic = GateChannelMagic;
  f.step_mapping.version = GateChannelVersion;
  f.step_mapping.size = sizeof(GateChannelMapping);
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.input_mapping.producer_heartbeat_ms),
      static_cast<LONG>(GetTickCount()));
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.step_mapping.producer_heartbeat_ms),
      static_cast<LONG>(GetTickCount()));
}

void publish_step(Fixture& f, std::uint32_t sequence, bool enabled,
                  std::uint32_t flags = 0, std::uint32_t anchor_a = 0,
                  std::uint32_t anchor_b = 0) {
  const auto now = GetTickCount();
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.step_mapping.producer_heartbeat_ms),
      static_cast<LONG>(now));
  const LONG old = InterlockedCompareExchange(
      reinterpret_cast<volatile LONG*>(&f.step_mapping.command_publication), 0,
      0);
  const LONG odd = (old & 1) ? old + 2 : old + 1;
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.step_mapping.command_publication),
      odd);
  f.step_mapping.command_sequence = sequence;
  f.step_mapping.enabled = enabled ? 1u : 0u;
  f.step_mapping.published_at_ms = now;
  f.step_mapping.flags = flags;
  f.step_mapping.expected_stage_tick = anchor_a;
  f.step_mapping.expected_segment_tick = anchor_b;
  MemoryBarrier();
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.step_mapping.command_publication),
      odd + 1);
}

void publish_input(Fixture& f, std::uint32_t sequence, std::uint16_t actions,
                   std::uint16_t updates, std::uint32_t flags = 0) {
  const auto now = GetTickCount();
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.input_mapping.producer_heartbeat_ms),
      static_cast<LONG>(now));
  const LONG old = InterlockedCompareExchange(
      reinterpret_cast<volatile LONG*>(&f.input_mapping.command_publication), 0,
      0);
  const LONG odd = (old & 1) ? old + 2 : old + 1;
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.input_mapping.command_publication),
      odd);
  f.input_mapping.command = InputChannelCommand{
      1, sequence, now, InputMaxTtlMs, actions, updates, flags};
  MemoryBarrier();
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.input_mapping.command_publication),
      odd + 1);
}

InputController controller(Fixture& f) {
  return InputController(&f.input_mapping,
                         GameMemoryAccess{&f, read16, write16, read32});
}
GateTickState evidence(Fixture& f, std::uint32_t seq) {
  float x = 0.0f, y = 0.0f;
  std::memcpy(&x, &f.x_bits, sizeof(x));
  std::memcpy(&y, &f.y_bits, sizeof(y));
  return GateTickState{seq, f.stage_tick, f.segment_tick, f.player, f.state, x,
                       y};
}

void run_input_tick(Fixture& f, InputController& c, TickGate& gate,
                    std::uint32_t sequence, std::uint16_t actions,
                    std::uint32_t calc) {
  publish_input(f, sequence, actions, 1);
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.step_mapping.producer_heartbeat_ms),
      static_cast<LONG>(GetTickCount()));
  gate.BeginTick();
  assert(gate.BeforeTick());
  assert(gate.state() == GateState::Running);
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPre, calc, 0);
  c.AfterCallback(th10hook::kCallbackCalcPre, calc, 0, {calc + 1, true});
  ++f.stage_tick;
  ++f.segment_tick;
  c.BeforeCallback(reinterpret_cast<void*>(kReplay),
                   th10hook::kCallbackReplayPost, calc + 1, calc);
  c.AfterCallback(th10hook::kCallbackReplayPost, calc + 1, calc,
                  {calc + 2, true});
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPre, calc + 2, calc);
  c.AfterCallback(th10hook::kCallbackPlayerPre, calc + 2, calc,
                  {calc + 3, true});
  constexpr std::uint16_t injected_bits = Shoot | Up | Down | Left | Right;
  assert((f.input & injected_bits) == actions);
  c.BeforeCallback(reinterpret_cast<void*>(kPlayer),
                   th10hook::kCallbackPlayerPost, calc + 2, calc);
  c.AfterCallback(th10hook::kCallbackPlayerPost, calc + 2, calc,
                  {calc + 4, true});
  c.BeforeCallback(nullptr, th10hook::kCallbackCalcPost, calc, 0);
  c.AfterCallback(th10hook::kCallbackCalcPost, calc, 0, {calc + 5, true});
  assert(f.input == 0x0104);  // restore the underlying held-input word
  const auto snap = evidence(f, calc + 5);
  gate.NoteCalcPost(snap, true, false);
  gate.AfterTick(snap, true);
  assert(gate.state() == GateState::Halted);
  assert(f.input_mapping.ack.kind == InputAckKind::SnapshotComplete);
  assert(f.input_mapping.ack.command_sequence == sequence &&
         f.input_mapping.ack.actions == actions &&
         f.input_mapping.ack.player_callback_exits == 1);
}

th10hook::CallbackSample record(std::uint32_t seq, std::uint32_t kind,
                                std::uint32_t context, std::uint32_t parent,
                                std::uint32_t a, std::uint32_t b,
                                std::uint32_t player = kPlayer) {
  th10hook::CallbackSample r{};
  r.sequence = seq;
  r.callback_kind = kind;
  r.thread_id = 99;
  r.callback_id = context;
  r.parent_callback_id = parent;
  r.player_pointer = player;
  r.player_state = 1;
  r.stage_tick = a;
  r.segment_tick = b;
  return r;
}

void halts_after_one_tick_and_rejects_bad_commands() {
  Fixture f;
  init(f);
  auto c = controller(f);
  TickGate gate(&f.step_mapping, &c, escape_probe, &f);
  publish_step(f, 1, true, GateCommandSingleTick);
  gate.BeginTick();
  assert(gate.BeforeTick());  // bounded passive start_state tick
  const auto initial = evidence(f, 6);
  gate.NoteCalcPost(initial, true, false);
  gate.AfterTick(initial, true);
  assert(gate.state() == GateState::Halted);

  publish_input(f, 1, Left, 2);
  gate.BeginTick();
  assert(!gate.BeforeTick());
  assert(gate.state() == GateState::Halted);
  assert(f.input_mapping.ack.kind == InputAckKind::Cancelled);
  assert((f.input & 0x00f0) == 0);  // rejected before any action update

  Fixture lease;
  lease.input = 0x0104;
  init(lease);
  auto lease_controller = controller(lease);
  TickGate lease_gate(&lease.step_mapping, &lease_controller, escape_probe,
                      &lease);
  publish_step(lease, 1, true, GateCommandSingleTick);
  lease_gate.BeginTick();
  assert(lease_gate.BeforeTick());
  auto lease_start = evidence(lease, 6);
  lease_gate.NoteCalcPost(lease_start, true, false);
  lease_gate.AfterTick(lease_start, true);
  publish_input(lease, 1, Shoot, 1, kUnknownCommandFlag);
  lease_gate.BeginTick();
  assert(!lease_gate.BeforeTick());
  assert(lease_gate.state() == GateState::Halted);
  assert(lease.input_mapping.ack.kind == InputAckKind::Cancelled);
  assert(lease.input == 0x0104 && lease.stage_tick == 100 &&
         lease.segment_tick == 200);

  Fixture single;
  single.input = 0x0104;
  init(single);
  auto single_controller = controller(single);
  TickGate single_gate(&single.step_mapping, &single_controller, escape_probe,
                       &single);
  publish_step(single, 1, true, GateCommandSingleTick);
  single_gate.BeginTick();
  assert(single_gate.BeforeTick());
  auto snap = evidence(single, 6);
  single_gate.NoteCalcPost(snap, true, false);
  single_gate.AfterTick(snap, true);
  const std::uint16_t actions[] = {static_cast<std::uint16_t>(Shoot | Right),
                                   static_cast<std::uint16_t>(Shoot | Left),
                                   Shoot};
  for (std::uint32_t i = 0; i < 3; ++i) {
    run_input_tick(single, single_controller, single_gate, i + 1, actions[i],
                   100 + i * 10);
    assert(single.stage_tick == 101 + i && single.segment_tick == 201 + i);
    single_gate.BeginTick();
    assert(!single_gate.BeforeTick());
    assert(single.stage_tick == 101 + i && single.segment_tick == 201 + i);
    assert(single.input == 0x0104);
  }
}

void fault_stays_halted_when_the_client_is_lost() {
  Fixture f;
  init(f);
  auto c = controller(f);
  TickGate gate(&f.step_mapping, &c, escape_probe, &f);
  publish_step(f, 1, true, GateCommandSingleTick);
  gate.BeginTick();
  assert(gate.BeforeTick());
  const auto snap = evidence(f, 6);
  gate.NoteCalcPost(snap, true, false);
  gate.AfterTick(snap, true);
  gate.BeginTick();
  assert(!gate.BeforeTick());
  ++f.stage_tick;  // start_state changes while halted
  gate.BeginTick();
  assert(!gate.BeforeTick());
  assert(gate.state() == GateState::Fault);
  gate.BeginTick();
  assert(!gate.BeforeTick());
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&f.step_mapping.producer_heartbeat_ms),
      static_cast<LONG>(GetTickCount() - 5000));
  gate.BeginTick();
  assert(!gate.BeforeTick());  // A lost client keeps the game halted.
  assert(gate.state() == GateState::Fault &&
         gate.error() == GateError::HeartbeatLost);

  Fixture malformed;
  init(malformed);
  auto malformed_controller = controller(malformed);
  TickGate malformed_gate(&malformed.step_mapping, &malformed_controller,
                          escape_probe, &malformed);
  publish_step(malformed, 1, true, GateCommandSingleTick);
  malformed_gate.BeginTick();
  assert(malformed_gate.BeforeTick());
  const auto start_state = evidence(malformed, 6);
  malformed_gate.NoteCalcPost(start_state, true, false);
  malformed_gate.AfterTick(start_state, true);
  malformed.step_mapping.magic = 0;
  malformed_gate.BeginTick();
  assert(!malformed_gate.BeforeTick());
  assert(malformed_gate.state() == GateState::Fault);
  InterlockedExchange(reinterpret_cast<volatile LONG*>(
                          &malformed.step_mapping.producer_heartbeat_ms),
                      static_cast<LONG>(GetTickCount() - 5000));
  malformed_gate.BeginTick();
  assert(!malformed_gate.BeforeTick());
  assert(malformed_gate.state() == GateState::Fault &&
         malformed_gate.error() == GateError::HeartbeatLost);
}

void reset_is_bounded_and_rearms_at_the_sampled_ticks() {
  Fixture f;
  f.input = 0x0100;
  init(f);
  auto c = controller(f);
  TickGate gate(&f.step_mapping, &c, escape_probe, &f);

  publish_step(f, 1, true, GateCommandSingleTick);
  gate.BeginTick();
  assert(gate.BeforeTick());
  auto first = evidence(f, 6);
  gate.NoteCalcPost(first, true, false);
  gate.AfterTick(first, true);
  assert(gate.state() == GateState::Halted);
  const auto original_passes = gate.pass_count();

  publish_step(f, 2, true, GateCommandSingleTick | GateCommandResetBegin);
  gate.BeginTick();
  assert(gate.BeforeTick());
  assert(gate.state() == GateState::Resetting && !c.active());
  assert(f.input == 0x0100);
  const auto after_begin_passes = gate.pass_count();
  publish_input(f, 1, Left, 1);
  c.BeforeCallback(nullptr, kCallbackPlayerPre, 25, 24);
  assert(f.input == 0x0100);

  gate.BeginTick();
  assert(gate.BeforeTick());
  ++f.stage_tick;
  ++f.segment_tick;
  gate.AfterTick(evidence(f, 0), true);
  assert(gate.state() == GateState::Resetting);
  assert(gate.pass_count() == after_begin_passes + 1);

  // Re-arm when the published pair exactly matches the live game counters.
  publish_step(f, 3, true, GateCommandSingleTick | GateCommandResetRearm,
               f.stage_tick, f.segment_tick);
  gate.BeginTick();
  assert(gate.BeforeTick());
  assert(gate.state() == GateState::Arming);
  ++f.stage_tick;
  ++f.segment_tick;
  auto rearmed = evidence(f, 8);
  gate.NoteCalcPost(rearmed, true, false);
  gate.AfterTick(rearmed, true);
  assert(gate.state() == GateState::Halted);
  assert(gate.pass_count() == original_passes + 3);
  assert(f.input == 0x0100);
  assert(f.input_mapping.ack.kind == InputAckKind::Cancelled &&
         f.input_mapping.ack.reason ==
             th10hook::InputCancelReason::Disconnected &&
         f.input_mapping.ack.command_sequence == 1 &&
         f.input_mapping.ack.actions == Left);
  gate.BeginTick();
  assert(!gate.BeforeTick());

  // A one-tick client/native read race is also valid for the pair.
  Fixture raced;
  raced.input = 0x0100;
  init(raced);
  auto raced_controller = controller(raced);
  TickGate raced_gate(&raced.step_mapping, &raced_controller, escape_probe,
                      &raced);
  publish_step(raced, 1, true, GateCommandSingleTick);
  raced_gate.BeginTick();
  assert(raced_gate.BeforeTick());
  auto raced_first = evidence(raced, 6);
  raced_gate.NoteCalcPost(raced_first, true, false);
  raced_gate.AfterTick(raced_first, true);
  publish_step(raced, 2, true, GateCommandSingleTick | GateCommandResetBegin);
  raced_gate.BeginTick();
  assert(raced_gate.BeforeTick());
  publish_step(raced, 3, true, GateCommandSingleTick | GateCommandResetRearm,
               raced.stage_tick, raced.segment_tick);
  ++raced.stage_tick;
  ++raced.segment_tick;
  raced_gate.BeginTick();
  assert(raced_gate.BeforeTick());
  assert(raced_gate.state() == GateState::Arming);
}

void reset_rejects_late_and_unrequested_rearm() {
  Fixture outside;
  outside.input = 0x0100;
  init(outside);
  auto outside_controller = controller(outside);
  TickGate outside_gate(&outside.step_mapping, &outside_controller,
                        escape_probe, &outside);
  publish_step(outside, 1, true, GateCommandSingleTick | GateCommandResetRearm,
               outside.stage_tick, outside.segment_tick);
  outside_gate.BeginTick();
  assert(!outside_gate.BeforeTick());
  assert(outside_gate.state() == GateState::Fault);

  Fixture late;
  late.input = 0x0100;
  init(late);
  auto late_controller = controller(late);
  TickGate late_gate(&late.step_mapping, &late_controller, escape_probe, &late);
  publish_step(late, 1, true, GateCommandSingleTick);
  late_gate.BeginTick();
  assert(late_gate.BeforeTick());
  auto first = evidence(late, 6);
  late_gate.NoteCalcPost(first, true, false);
  late_gate.AfterTick(first, true);
  publish_step(late, 2, true, GateCommandSingleTick | GateCommandResetBegin);
  late_gate.BeginTick();
  assert(late_gate.BeforeTick());
  Sleep(GateResetMaxDurationMs + 20);
  publish_step(late, 3, true, GateCommandSingleTick | GateCommandResetRearm,
               late.stage_tick, late.segment_tick);
  late_gate.BeginTick();
  assert(!late_gate.BeforeTick());
  assert(late_gate.state() == GateState::Fault);

  Fixture watchdog;
  watchdog.input = 0x0100;
  init(watchdog);
  auto watchdog_controller = controller(watchdog);
  TickGate watchdog_gate(&watchdog.step_mapping, &watchdog_controller,
                         escape_probe, &watchdog);
  publish_step(watchdog, 1, true, GateCommandSingleTick);
  watchdog_gate.BeginTick();
  assert(watchdog_gate.BeforeTick());
  auto watchdog_first = evidence(watchdog, 6);
  watchdog_gate.NoteCalcPost(watchdog_first, true, false);
  watchdog_gate.AfterTick(watchdog_first, true);
  publish_step(watchdog, 2, true,
               GateCommandSingleTick | GateCommandResetBegin);
  watchdog_gate.BeginTick();
  assert(watchdog_gate.BeforeTick());
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(
          &watchdog.step_mapping.producer_heartbeat_ms),
      static_cast<LONG>(GetTickCount() - GateMaxHeartbeatAgeMs - 1));
  watchdog_gate.BeginTick();
  assert(!watchdog_gate.BeforeTick());
  assert(watchdog_gate.state() == GateState::Fault &&
         watchdog_gate.error() == GateError::HeartbeatLost);
  publish_step(watchdog, 3, true, GateCommandSingleTick | GateCommandResetRearm,
               watchdog.stage_tick, watchdog.segment_tick);
  watchdog_gate.BeginTick();
  assert(!watchdog_gate.BeforeTick());
  assert(watchdog_gate.state() == GateState::Fault);

  Fixture bad_ticks;
  bad_ticks.input = 0x0100;
  init(bad_ticks);
  auto bad_controller = controller(bad_ticks);
  TickGate bad_gate(&bad_ticks.step_mapping, &bad_controller, escape_probe,
                    &bad_ticks);
  publish_step(bad_ticks, 1, true, GateCommandSingleTick);
  bad_gate.BeginTick();
  assert(bad_gate.BeforeTick());
  auto bad_first = evidence(bad_ticks, 6);
  bad_gate.NoteCalcPost(bad_first, true, false);
  bad_gate.AfterTick(bad_first, true);
  publish_step(bad_ticks, 2, true,
               GateCommandSingleTick | GateCommandResetBegin);
  bad_gate.BeginTick();
  assert(bad_gate.BeforeTick());
  publish_step(bad_ticks, 3, true,
               GateCommandSingleTick | GateCommandResetRearm,
               bad_ticks.stage_tick, bad_ticks.segment_tick);
  bad_ticks.stage_tick += 2;
  bad_ticks.segment_tick += 2;
  bad_gate.BeginTick();
  assert(!bad_gate.BeforeTick());
  assert(bad_gate.state() == GateState::Fault &&
         bad_gate.error() == GateError::InvalidChain);

  Fixture unstable;
  unstable.input = 0x0100;
  init(unstable);
  auto unstable_controller = controller(unstable);
  TickGate unstable_gate(&unstable.step_mapping, &unstable_controller,
                         escape_probe, &unstable);
  publish_step(unstable, 1, true, GateCommandSingleTick);
  unstable_gate.BeginTick();
  assert(unstable_gate.BeforeTick());
  auto unstable_first = evidence(unstable, 6);
  unstable_gate.NoteCalcPost(unstable_first, true, false);
  unstable_gate.AfterTick(unstable_first, true);
  publish_step(unstable, 2, true,
               GateCommandSingleTick | GateCommandResetBegin);
  unstable_gate.BeginTick();
  assert(unstable_gate.BeforeTick());
  publish_input(unstable, 1, Left, 1);
  const LONG publication = InterlockedCompareExchange(
      reinterpret_cast<volatile LONG*>(
          &unstable.input_mapping.command_publication),
      0, 0);
  InterlockedExchange(reinterpret_cast<volatile LONG*>(
                          &unstable.input_mapping.command_publication),
                      publication + 1);
  publish_step(unstable, 3, true, GateCommandSingleTick | GateCommandResetRearm,
               unstable.stage_tick, unstable.segment_tick);
  unstable_gate.BeginTick();
  assert(!unstable_gate.BeforeTick());
  assert(unstable_gate.state() == GateState::Fault &&
         unstable_gate.error() == GateError::InputRelease);
}

void verifier_filters_samples_and_rejects_duplicate_children() {
  CallbackSample calc{};
  calc.callback_kind = kCallbackCalcPre;
  calc.callback_id = 10;
  calc.parent_callback_id = 9;
  calc.thread_id = 8;
  calc.player_pointer = kPlayer;
  calc.player_state = 1;
  calc.sequence = 1;

  CallbackSample player = calc;
  player.callback_kind = kCallbackPlayerPre;
  player.callback_id = 11;
  player.parent_callback_id = calc.callback_id;
  player.sequence = 2;
  player.input_current = Left;

  CallbackSample post = calc;
  post.callback_kind = kCallbackCalcPost;
  post.sequence = 3;

  TickVerifier verifier{};
  verifier.begin();
  verifier.observe(calc);
  verifier.observe(player);
  player.input_current = Right;
  ++player.thread_id;
  verifier.observe(player);
  player.thread_id = calc.thread_id;
  ++player.parent_callback_id;
  verifier.observe(player);
  verifier.observe(post);

  CallbackSample retained{};
  std::uint16_t words[5]{};
  assert(verifier.copy_tick_end(retained, words));
  assert(retained.sequence == post.sequence && words[0] == Left);
  player.parent_callback_id = calc.callback_id;
  verifier.observe(player);
  assert(!verifier.copy_tick_end(retained, words));

  verifier.begin();
  verifier.observe(calc);
  ++post.parent_callback_id;
  verifier.observe(post);
  assert(!verifier.has_calc_post && verifier.verification_failed);
}

void escape_guard_only_accepts_target_foreground_keypress() {
  assert(escape_pressed_for_foreground_process(42, 42, true));
  assert(!escape_pressed_for_foreground_process(17, 42, true));
  assert(!escape_pressed_for_foreground_process(0, 42,
                                                true));  // no foreground window
  assert(!escape_pressed_for_foreground_process(42, 0, true));
  assert(
      !escape_pressed_for_foreground_process(42, 42, false));  // key released
}
}  // namespace

int main() {
  halts_after_one_tick_and_rejects_bad_commands();
  fault_stays_halted_when_the_client_is_lost();
  reset_is_bounded_and_rearms_at_the_sampled_ticks();
  reset_rejects_late_and_unrequested_rearm();
  verifier_filters_samples_and_rejects_duplicate_children();
  escape_guard_only_accepts_target_foreground_keypress();
  return 0;
}
