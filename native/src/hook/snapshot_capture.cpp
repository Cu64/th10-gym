#include "snapshot_capture.h"

#include <climits>
#include <cmath>
#include <cstring>

#include "game_hooks.h"
#include "game_memory.h"

namespace th10hook {
namespace {

constexpr std::uint32_t kBulletPoolOffset = 0x60, kBulletStride = 0x7f0;

SnapshotChannelMapping* volatile g_snapshot_channel = nullptr;
InputChannelMapping* g_input_channel = nullptr;
HANDLE g_snapshot_ready = nullptr;
SnapshotRecord g_snapshot_scratch{};
alignas(4) std::uint8_t g_entity_tables[EntityTablesCapacity];
std::uint32_t g_snapshot_tick_count = 0;

bool is_finite(float value) { return std::isfinite(value) != 0; }

struct GuardState {
  std::uint32_t player;
  std::uint32_t state;
  std::uint32_t x_bits;
  std::uint32_t y_bits;
  std::uint32_t stage_tick;
  std::uint32_t segment_tick;
  std::uint32_t bomb_manager;
  std::uint32_t boss_manager;
  std::uint32_t boss_slot0;
  std::uint32_t bullet_manager;
  std::uint32_t laser_manager;
  std::uint32_t laser_head;
};

bool read_guard_state(GuardState& a) {
  a = {};
  float x = 0.0f, y = 0.0f;
  if (!read_game(retail::kPlayerPointer, a.player) || a.player == 0 ||
      !bounded_game_address(a.player, retail::kPlayerObjectSize) ||
      !read_game(a.player + retail::kPlayerState, a.state) ||
      !read_game(a.player + retail::kPlayerX, x) ||
      !read_game(a.player + retail::kPlayerY, y) ||
      !read_game(retail::kStageTick, a.stage_tick) ||
      !read_game(retail::kSegmentTick, a.segment_tick) ||
      !read_game(retail::kBombManagerPointer, a.bomb_manager) ||
      !read_game(retail::kBossManagerPointer, a.boss_manager) ||
      !read_game(retail::kBulletManagerPointer, a.bullet_manager) ||
      !read_game(retail::kLaserManagerPointer, a.laser_manager) ||
      !is_finite(x) || !is_finite(y))
    return false;
  if (a.boss_manager != 0) {
    if (!bounded_game_address(a.boss_manager, 0x14) ||
        !read_game(a.boss_manager + 0x10, a.boss_slot0))
      return false;
  }
  if (a.laser_manager != 0) {
    if (!bounded_game_address(a.laser_manager, 0x1c) ||
        !read_game(a.laser_manager + 0x18, a.laser_head))
      return false;
  }
  std::memcpy(&a.x_bits, &x, sizeof(x));
  std::memcpy(&a.y_bits, &y, sizeof(y));
  return true;
}

bool same_guard_state(const GuardState& a, const GuardState& b) {
  return std::memcmp(&a, &b, sizeof(a)) == 0;
}

bool read_latest_ack(InputAck& ack) {
  ack = {};
  auto* mapping = g_input_channel;
  if (mapping == nullptr || mapping->magic != InputChannelMagic ||
      mapping->version != InputChannelVersion ||
      mapping->size != sizeof(*mapping))
    return false;
  volatile LONG* stamp =
      reinterpret_cast<volatile LONG*>(&mapping->ack_publication);
  const LONG before = InterlockedCompareExchange(stamp, 0, 0);
  if (before <= 0 || (before & 1)) return false;
  MemoryBarrier();
  std::memcpy(&ack, &mapping->ack, sizeof(ack));
  MemoryBarrier();
  return before == InterlockedCompareExchange(stamp, 0, 0) && !(before & 1);
}

bool capture_player(SnapshotRecord& out) {
  const auto pointer = out.player_pointer;
  if (!read_game(pointer + retail::kPlayerState, out.player_state) ||
      !read_game(pointer + retail::kPlayerX, out.player_x) ||
      !read_game(pointer + retail::kPlayerY, out.player_y) ||
      !is_finite(out.player_x) || !is_finite(out.player_y))
    return false;
  out.groups_available |= GroupPlayer;
  out.groups_valid |= GroupPlayer;
  return true;
}

bool capture_resources(SnapshotRecord& out) {
  out.groups_available |= GroupResources;
  if (!read_game(retail::kLives, out.lives) ||
      !read_game(retail::kPower, out.power))
    return false;
  out.groups_valid |= GroupResources;
  return true;
}

bool capture_bomb(SnapshotRecord& out) {
  if (!read_game(retail::kBombManagerPointer, out.bomb_pointer)) return false;
  out.groups_available |= GroupBomb;
  if (!read_game(out.player_pointer + retail::kPlayerProtectionTimer,
                 out.protection_timer))
    return false;
  // A null controller leaves bomb state unknown.
  if (out.bomb_pointer == 0) return true;
  if (!bounded_game_address(out.bomb_pointer, 0x2c) ||
      !read_game(out.bomb_pointer + 0x28, out.bomb_active) ||
      !read_game(out.bomb_pointer + 0x18, out.bomb_timer))
    return false;
  out.groups_valid |= GroupBomb;
  return true;
}

bool read_bullet_fields(std::uintptr_t item, RawBulletRecord& row) {
  return read_game(item, row.raw_flags) && read_game(item + 0x3b4, row.x) &&
         read_game(item + 0x3b8, row.y) &&
         read_game(item + retail::kPlayerX, row.vx) &&
         read_game(item + retail::kPlayerY, row.vy) &&
         read_game(item + 0x3d8, row.speed) &&
         read_game(item + 0x3e4, row.angle) &&
         read_game(item + 0x3f0, row.width) &&
         read_game(item + 0x3f4, row.height) && is_finite(row.x) &&
         is_finite(row.y) && is_finite(row.vx) && is_finite(row.vy) &&
         is_finite(row.speed) && is_finite(row.angle) && is_finite(row.width) &&
         is_finite(row.height);
}

bool scan_bullet_pool(std::uintptr_t base, SnapshotRecord& out) {
  // Retail initializes the entry after these 2,000 slots as a wrap sentinel.
  for (std::uint32_t slot = 0; slot < SnapshotBulletCapacity; ++slot) {
    const std::uintptr_t item =
        base + static_cast<std::uintptr_t>(slot) * kBulletStride;
    std::uint16_t state = 0;
    ++out.bullet_scanned;
    if (!read_game(item + 0x446, state)) return false;
    if (state == 0) continue;
    if (!read_game(item + 0x314, out.bullet_spawn_ready[out.bullet_captured]))
      return false;
    auto& row = out.bullets[out.bullet_captured];
    row.slot = slot;
    row.state = state;
    if (!read_bullet_fields(item, row)) return false;
    ++out.bullet_captured;
  }
  return true;
}

bool capture_bullets(SnapshotRecord& out) {
  std::uint32_t manager = 0;
  if (!read_game(retail::kBulletManagerPointer, manager)) return false;
  if (manager == 0) return true;
  out.groups_available |= GroupBullets;
  if (!bounded_game_address(
          manager,
          kBulletPoolOffset + SnapshotBulletCapacity * kBulletStride) ||
      !scan_bullet_pool(manager + kBulletPoolOffset, out))
    return false;
  out.groups_valid |= GroupBullets;
  return true;
}

void capture_core_groups(SnapshotRecord& out) {
  std::uint32_t pointer = 0;
  if (!read_game(retail::kPlayerPointer, pointer) || pointer == 0 ||
      !bounded_game_address(pointer, retail::kPlayerObjectSize)) {
    out.groups_read_fault |= GroupPlayer | GroupConsumedInput;
    out.flags |= SnapshotCaptureFault;
    return;
  }
  out.player_pointer = pointer;
  std::uint32_t faults = 0;
  if (!capture_player(out)) faults |= GroupPlayer;
  if (!capture_resources(out)) faults |= GroupResources;
  if (!capture_bomb(out)) faults |= GroupBomb;
  if (!capture_bullets(out)) faults |= GroupBullets;
  out.groups_read_fault |= faults;
  if (faults != 0) out.flags |= SnapshotCaptureFault;
}

bool ack_matches_tick(const InputAck& ack, const CallbackSample& calc) {
  const bool normal = ack.kind == InputAckKind::SnapshotComplete &&
                      ack.checks == InputRequiredChecks &&
                      ack.player_callback_exits != 0;
  const bool terminal = ack.kind == InputAckKind::TerminalTick &&
                        ack.checks == 0 && ack.player_callback_exits == 0;
  return ack.version == 1 && ack.size == sizeof(ack) && (normal || terminal) &&
         ack.reason == InputCancelReason::None && ack.session_id != 0 &&
         ack.command_sequence != 0 &&
         ack.calc_callback_id == calc.callback_id &&
         ack.snapshot_sequence == calc.sequence;
}

}  // namespace

void init_snapshot_capture(SnapshotChannelMapping* channel,
                           InputChannelMapping* input_channel,
                           HANDLE ready_event) {
  g_input_channel = input_channel;
  g_snapshot_ready = ready_event;
  InterlockedExchangePointer(
      reinterpret_cast<PVOID volatile*>(&g_snapshot_channel), channel);
}

TerminalOutcome read_terminal_outcome() {
  using Outcome = TerminalOutcome;
  std::uint32_t manager = 0, owner = 0, replay = 0;
  std::uint32_t flags = 0, mode = 0, replay_mode = 1;
  std::int32_t manager_state = -1, owner_state = -1, path = -1, lives = 0;
  if (!read_game(retail::kGameManagerPointer, manager) ||
      !bounded_game_address(manager, 0x60) ||
      !read_game(manager + 0x58, flags) || (flags & 0x10) == 0 ||
      !read_game(manager + 0x5c, manager_state) || manager_state != 0 ||
      !read_game(retail::kMenuOwnerPointer, owner) ||
      !bounded_game_address(owner, 0x1e8) ||
      !read_game(owner + 4, owner_state) || owner_state != 6 ||
      !read_game(owner + 0x1e4, path) ||
      !read_game(retail::kReplayManagerPointer, replay) ||
      !bounded_replay_manager_address(replay, 0x14) ||
      !read_game(replay + 0x10, replay_mode) || replay_mode != 0 ||
      !read_game(retail::kLives, lives) ||
      !read_game(retail::kGameModeFlags, mode))
    return Outcome::None;
  if (path == 0 && lives == -1) return Outcome::GameOver;
  if (path == 1 && lives >= 0 && mode == 0x10) return Outcome::PracticeComplete;
  return Outcome::None;
}

void publish_halted_snapshot(TickGate& gate, bool first_snapshot,
                             const CallbackSample& calc_post,
                             TerminalOutcome outcome,
                             const std::uint16_t (*consumed_input)[5]) {
  auto* mapping =
      static_cast<SnapshotChannelMapping*>(InterlockedCompareExchangePointer(
          reinterpret_cast<PVOID volatile*>(&g_snapshot_channel), nullptr,
          nullptr));
  if (!mapping) {
    gate.FailSnapshotPublication(GateError::SnapshotPublication);
    return;
  }

  auto& record = g_snapshot_scratch;
  std::memset(&record, 0, sizeof(record));
  record.schema_version = SnapshotVersion;
  record.terminal_outcome = outcome;
  if (g_snapshot_tick_count == UINT32_MAX) {
    record.flags |= SnapshotCaptureFault;
    InterlockedOr(reinterpret_cast<volatile LONG*>(&mapping->header.flags),
                  SnapshotSequenceExhausted);
  } else {
    record.tick_count = ++g_snapshot_tick_count;
  }
  record.calc_post_sequence = calc_post.sequence;
  record.callback_id = calc_post.callback_id;
  record.thread_id = calc_post.thread_id;
  record.stage_tick = calc_post.stage_tick;
  record.segment_tick = calc_post.segment_tick;
  LARGE_INTEGER qpc{};
  QueryPerformanceCounter(&qpc);
  record.qpc_low = qpc.LowPart;
  record.qpc_high = static_cast<std::uint32_t>(qpc.HighPart);

  const bool no_player_consumer = consumed_input == nullptr;
  if (consumed_input) {
    std::memcpy(record.consumed_input_words, *consumed_input,
                sizeof(*consumed_input));
    record.consumed_input_valid = 1;
    record.groups_available |= GroupConsumedInput;
    record.groups_valid |= GroupConsumedInput;
  }

  if (!first_snapshot) {
    InputAck ack{};
    if (read_latest_ack(ack) && ack_matches_tick(ack, calc_post) &&
        (ack.kind == InputAckKind::TerminalTick) == no_player_consumer) {
      record.session_id = ack.session_id;
      record.command_sequence = ack.command_sequence;
      record.completed_ticks =
          no_player_consumer ? 1 : ack.player_callback_exits;
      record.requested_actions = ack.actions;
      record.ack_kind = static_cast<std::uint16_t>(ack.kind);
      record.flags |= SnapshotAckMatched;
    } else {
      record.flags |= SnapshotCaptureFault;
    }
  }

  GuardState before{}, after{};
  std::size_t supplement_size = 0;
  const bool before_ok = read_guard_state(before);
  const bool matches_calc_post =
      before_ok && before.player == calc_post.player_pointer &&
      before.state == static_cast<std::uint32_t>(calc_post.player_state) &&
      before.stage_tick == calc_post.stage_tick &&
      before.segment_tick == calc_post.segment_tick &&
      before.x_bits ==
          [&]() {
            std::uint32_t v{};
            std::memcpy(&v, &calc_post.player_x, 4);
            return v;
          }() &&
      before.y_bits == [&]() {
        std::uint32_t v{};
        std::memcpy(&v, &calc_post.player_y, 4);
        return v;
      }();
  if (before_ok && matches_calc_post) {
    record.flags |= SnapshotTickValid;
    capture_core_groups(record);
    supplement_size = CaptureEntityTables(record, g_entity_tables);
    if (!supplement_size) record.flags |= SnapshotCaptureFault;
    const bool after_ok = read_guard_state(after);
    if (!after_ok || !same_guard_state(before, after)) {
      record.flags |= SnapshotCaptureFault;
      record.flags &= ~SnapshotTickValid;
      gate.FailVerification();
    }
  } else {
    record.flags |= SnapshotCaptureFault;
    record.groups_read_fault |= GroupPlayer;
    gate.FailVerification();
  }
  if (!PublishSnapshot(mapping, record, g_entity_tables, supplement_size)) {
    gate.FailSnapshotPublication(GateError::SnapshotPublication);
    return;
  }
  if (!SetEvent(g_snapshot_ready))
    gate.FailSnapshotPublication(GateError::SnapshotNotification);
}

}  // namespace th10hook
