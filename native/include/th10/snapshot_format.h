#pragma once

#include <cstddef>
#include <cstdint>
#include "snapshot_layout_checks.h"

namespace th10hook {

static constexpr std::uint32_t SnapshotMagic = 0x31524f54;  // "TOR1"
static constexpr std::uint32_t SnapshotBulletCapacity = 2000;
static constexpr std::uint32_t RawLaserCapacity = 256;
static constexpr std::uint16_t SnapshotVersion = 3;
static constexpr std::size_t EntityTablesCapacity = 1u << 20;
static constexpr std::uint32_t RawObservationRecordSize = 98420;
static constexpr std::uint32_t RawObservationMappingSize = 98484;

enum class TerminalOutcome : std::uint32_t { None, GameOver, PracticeComplete };

enum RawGroup : std::uint32_t {
  GroupPlayer = 1,
  GroupResources = 2,
  GroupBomb = 4,
  GroupBoss = 8,
  GroupBullets = 16,
  GroupLasers = 32,
  GroupConsumedInput = 64
};
enum RawRecordFlag : std::uint32_t {
  SnapshotTickValid = 1,
  SnapshotAckMatched = 2,
  SnapshotPolicyReady = 4,
  SnapshotCaptureFault = 8
};
enum RawHeaderFlag : std::uint32_t {
  SnapshotSequenceExhausted = 1,
  RawWriterBusyDrop = 2
};

struct RawBulletRecord {
  std::uint32_t slot;
  std::uint16_t state;
  std::uint16_t reserved;
  std::uint32_t raw_flags;
  float x, y, vx, vy, speed, angle, width, height;
};
struct RawLaserRecord {
  std::uint32_t node;
  std::uint32_t next;
  std::uint32_t state;
  float x, y, raw_dx, raw_dy, angle, raw_geometry40, raw_geometry44;
};

struct SnapshotRecord {
  volatile std::uint32_t stamp_begin;
  std::uint32_t generation;
  std::uint32_t schema_version;
  std::uint32_t flags;
  std::uint32_t tick_count;
  std::uint32_t calc_post_sequence;
  std::uint32_t callback_id;
  std::uint32_t thread_id;
  std::uint32_t qpc_low;
  std::uint32_t qpc_high;
  std::uint32_t session_id;
  std::uint32_t command_sequence;
  std::uint32_t completed_ticks;
  std::uint16_t requested_actions;
  std::uint16_t ack_kind;
  std::uint16_t consumed_input_valid;
  std::uint16_t reserved16;
  std::uint16_t consumed_input_words[5];
  std::uint32_t player_pointer;
  std::int32_t player_state;
  float player_x;
  float player_y;
  std::int32_t lives;
  std::int32_t power;
  std::uint32_t bomb_pointer;
  std::int32_t bomb_active;
  std::int32_t bomb_timer;
  std::int32_t protection_timer;
  std::uint32_t boss_manager;
  std::uint32_t boss_slot0;
  float boss_x;
  float boss_y;
  std::int32_t boss_current_hp;
  std::uint32_t bullet_scanned;
  std::uint32_t bullet_captured;
  std::uint32_t laser_scanned;
  std::uint32_t laser_captured;
  std::uint32_t groups_available;
  std::uint32_t groups_valid;
  std::uint32_t groups_truncated;
  std::uint32_t groups_unsupported;
  std::uint32_t groups_read_fault;
  std::uint32_t stage_tick;
  std::uint32_t segment_tick;
  RawBulletRecord bullets[SnapshotBulletCapacity];
  std::uint32_t bullet_spawn_ready[SnapshotBulletCapacity];
  TerminalOutcome terminal_outcome;
  std::uint8_t unused_legacy_laser_bytes[2236];
  volatile std::uint32_t stamp_end;
};

struct SnapshotHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t header_size;
  std::uint32_t record_size;
  std::uint32_t target_pid;
  std::uint32_t capacity;
  volatile std::int32_t publication;
  volatile std::int32_t dropped_count;
  volatile std::int32_t flags;
  std::uint32_t reserved[8];
};
struct SnapshotChannelMapping {
  SnapshotHeader header;
  SnapshotRecord record;
  std::uint8_t supplement[EntityTablesCapacity];
};

OBS_ASSERT_LAYOUT_RAWBULLETSOURCE(RawBulletRecord);
OBS_ASSERT_LAYOUT_RAWLASERSOURCE(RawLaserRecord);
OBS_ASSERT_LAYOUT_RAWOBSERVATIONHEADERSOURCE(SnapshotHeader);

static_assert(sizeof(RawBulletRecord) == 44 && sizeof(RawLaserRecord) == 40,
              "raw pool record ABI changed");
static_assert(sizeof(SnapshotHeader) == 64 &&
                  sizeof(SnapshotRecord) == RawObservationRecordSize &&
                  sizeof(SnapshotChannelMapping) ==
                      RawObservationMappingSize + EntityTablesCapacity,
              "raw observation mapping ABI changed");
static_assert(sizeof(RawBulletRecord) == SNAPSHOT_LAYOUT_RAWBULLETRECORD_SIZE &&
                  sizeof(RawLaserRecord) ==
                      SNAPSHOT_LAYOUT_RAWLASERRECORD_SIZE &&
                  sizeof(SnapshotHeader) ==
                      SNAPSHOT_LAYOUT_RAWOBSERVATIONHEADER_SIZE,
              "raw source descriptor sizes");
static_assert(offsetof(SnapshotRecord, bullet_spawn_ready) == 88176 &&
                  offsetof(SnapshotRecord, terminal_outcome) == 96176 &&
                  offsetof(SnapshotRecord, stamp_end) == 98416,
              "strict Gym raw observation ABI");
static_assert(offsetof(SnapshotRecord, consumed_input_words) == 60 &&
                  offsetof(SnapshotRecord, player_pointer) == 72 &&
                  offsetof(SnapshotRecord, session_id) == 40 &&
                  offsetof(SnapshotRecord, requested_actions) == 52 &&
                  offsetof(SnapshotRecord, groups_available) == 148 &&
                  offsetof(SnapshotRecord, stage_tick) == 168 &&
                  offsetof(SnapshotRecord, bullets) == 176 &&
                  offsetof(SnapshotHeader, publication) == 20 &&
                  offsetof(SnapshotHeader, dropped_count) == 24,
              "raw observation record offsets changed");

bool CreateSnapshotChannel(std::uint32_t pid, void** mapping_handle,
                           SnapshotChannelMapping** mapping_view,
                           std::uint32_t* win32_error);
bool PublishSnapshot(SnapshotChannelMapping* mapping,
                     const SnapshotRecord& record,
                     const std::uint8_t* supplement,
                     std::size_t supplement_size);

// Writes the packed tail for the same tick as record and returns its byte size.
// On a read fault, returns zero. The caller publishes a faulted base record.
std::size_t CaptureEntityTables(SnapshotRecord& record, std::uint8_t* output);

}  // namespace th10hook
