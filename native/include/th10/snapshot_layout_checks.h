// Compile-time checks of the snapshot records' byte layout.
// Keep in step with the decoder in th10_gym/_snapshot/format.py.
#pragma once
#include <cstddef>
#include <cstdint>
namespace th10hook {
constexpr std::uint32_t SNAPSHOT_LAYOUT_RAWBULLETRECORD_SIZE = 44;
constexpr std::uint32_t SNAPSHOT_LAYOUT_RAWBULLETRECORD_ALIGNMENT = 4;
constexpr std::uint32_t SNAPSHOT_LAYOUT_RAWLASERRECORD_SIZE = 40;
constexpr std::uint32_t SNAPSHOT_LAYOUT_RAWLASERRECORD_ALIGNMENT = 4;
constexpr std::uint32_t SNAPSHOT_LAYOUT_RAWOBSERVATIONHEADER_SIZE = 64;
constexpr std::uint32_t SNAPSHOT_LAYOUT_RAWOBSERVATIONHEADER_ALIGNMENT = 4;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_SLOT_OFFSET = 0;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_STATE_OFFSET = 4;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_RESERVED_OFFSET = 6;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_RAW_FLAGS_OFFSET = 8;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_X_OFFSET = 12;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_Y_OFFSET = 16;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_VX_OFFSET = 20;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_VY_OFFSET = 24;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_SPEED_OFFSET = 28;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_ANGLE_OFFSET = 32;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_WIDTH_OFFSET = 36;
constexpr std::uint32_t OBS_RAWBULLETSOURCE_HEIGHT_OFFSET = 40;
#define OBS_ASSERT_LAYOUT_RAWBULLETSOURCE(Type)                                \
  static_assert(                                                               \
      sizeof(Type) == SNAPSHOT_LAYOUT_RAWBULLETRECORD_SIZE &&                  \
          alignof(Type) == SNAPSHOT_LAYOUT_RAWBULLETRECORD_ALIGNMENT &&        \
          offsetof(Type, slot) == OBS_RAWBULLETSOURCE_SLOT_OFFSET &&           \
          offsetof(Type, state) == OBS_RAWBULLETSOURCE_STATE_OFFSET &&         \
          offsetof(Type, reserved) == OBS_RAWBULLETSOURCE_RESERVED_OFFSET &&   \
          offsetof(Type, raw_flags) == OBS_RAWBULLETSOURCE_RAW_FLAGS_OFFSET && \
          offsetof(Type, x) == OBS_RAWBULLETSOURCE_X_OFFSET &&                 \
          offsetof(Type, y) == OBS_RAWBULLETSOURCE_Y_OFFSET &&                 \
          offsetof(Type, vx) == OBS_RAWBULLETSOURCE_VX_OFFSET &&               \
          offsetof(Type, vy) == OBS_RAWBULLETSOURCE_VY_OFFSET &&               \
          offsetof(Type, speed) == OBS_RAWBULLETSOURCE_SPEED_OFFSET &&         \
          offsetof(Type, angle) == OBS_RAWBULLETSOURCE_ANGLE_OFFSET &&         \
          offsetof(Type, width) == OBS_RAWBULLETSOURCE_WIDTH_OFFSET &&         \
          offsetof(Type, height) == OBS_RAWBULLETSOURCE_HEIGHT_OFFSET,         \
      "RawBulletSource descriptor layout mismatch")
constexpr std::uint32_t OBS_RAWLASERSOURCE_NODE_OFFSET = 0;
constexpr std::uint32_t OBS_RAWLASERSOURCE_NEXT_OFFSET = 4;
constexpr std::uint32_t OBS_RAWLASERSOURCE_STATE_OFFSET = 8;
constexpr std::uint32_t OBS_RAWLASERSOURCE_X_OFFSET = 12;
constexpr std::uint32_t OBS_RAWLASERSOURCE_Y_OFFSET = 16;
constexpr std::uint32_t OBS_RAWLASERSOURCE_RAW_DX_OFFSET = 20;
constexpr std::uint32_t OBS_RAWLASERSOURCE_RAW_DY_OFFSET = 24;
constexpr std::uint32_t OBS_RAWLASERSOURCE_ANGLE_OFFSET = 28;
constexpr std::uint32_t OBS_RAWLASERSOURCE_RAW_GEOMETRY40_OFFSET = 32;
constexpr std::uint32_t OBS_RAWLASERSOURCE_RAW_GEOMETRY44_OFFSET = 36;
#define OBS_ASSERT_LAYOUT_RAWLASERSOURCE(Type)                          \
  static_assert(                                                        \
      sizeof(Type) == SNAPSHOT_LAYOUT_RAWLASERRECORD_SIZE &&            \
          alignof(Type) == SNAPSHOT_LAYOUT_RAWLASERRECORD_ALIGNMENT &&  \
          offsetof(Type, node) == OBS_RAWLASERSOURCE_NODE_OFFSET &&     \
          offsetof(Type, next) == OBS_RAWLASERSOURCE_NEXT_OFFSET &&     \
          offsetof(Type, state) == OBS_RAWLASERSOURCE_STATE_OFFSET &&   \
          offsetof(Type, x) == OBS_RAWLASERSOURCE_X_OFFSET &&           \
          offsetof(Type, y) == OBS_RAWLASERSOURCE_Y_OFFSET &&           \
          offsetof(Type, raw_dx) == OBS_RAWLASERSOURCE_RAW_DX_OFFSET && \
          offsetof(Type, raw_dy) == OBS_RAWLASERSOURCE_RAW_DY_OFFSET && \
          offsetof(Type, angle) == OBS_RAWLASERSOURCE_ANGLE_OFFSET &&   \
          offsetof(Type, raw_geometry40) ==                             \
              OBS_RAWLASERSOURCE_RAW_GEOMETRY40_OFFSET &&               \
          offsetof(Type, raw_geometry44) ==                             \
              OBS_RAWLASERSOURCE_RAW_GEOMETRY44_OFFSET,                 \
      "RawLaserSource descriptor layout mismatch")
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_MAGIC_OFFSET = 0;
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_VERSION_OFFSET = 4;
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_HEADER_SIZE_OFFSET = 6;
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_RECORD_SIZE_OFFSET = 8;
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_TARGET_PID_OFFSET = 12;
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_CAPACITY_OFFSET = 16;
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_PUBLICATION_OFFSET = 20;
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_DROPPED_COUNT_OFFSET =
    24;
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_FLAGS_OFFSET = 28;
constexpr std::uint32_t OBS_RAWOBSERVATIONHEADERSOURCE_RESERVED_OFFSET = 32;
#define OBS_ASSERT_LAYOUT_RAWOBSERVATIONHEADERSOURCE(Type)                   \
  static_assert(                                                             \
      sizeof(Type) == SNAPSHOT_LAYOUT_RAWOBSERVATIONHEADER_SIZE &&           \
          alignof(Type) == SNAPSHOT_LAYOUT_RAWOBSERVATIONHEADER_ALIGNMENT && \
          offsetof(Type, magic) ==                                           \
              OBS_RAWOBSERVATIONHEADERSOURCE_MAGIC_OFFSET &&                 \
          offsetof(Type, version) ==                                         \
              OBS_RAWOBSERVATIONHEADERSOURCE_VERSION_OFFSET &&               \
          offsetof(Type, header_size) ==                                     \
              OBS_RAWOBSERVATIONHEADERSOURCE_HEADER_SIZE_OFFSET &&           \
          offsetof(Type, record_size) ==                                     \
              OBS_RAWOBSERVATIONHEADERSOURCE_RECORD_SIZE_OFFSET &&           \
          offsetof(Type, target_pid) ==                                      \
              OBS_RAWOBSERVATIONHEADERSOURCE_TARGET_PID_OFFSET &&            \
          offsetof(Type, capacity) ==                                        \
              OBS_RAWOBSERVATIONHEADERSOURCE_CAPACITY_OFFSET &&              \
          offsetof(Type, publication) ==                                     \
              OBS_RAWOBSERVATIONHEADERSOURCE_PUBLICATION_OFFSET &&           \
          offsetof(Type, dropped_count) ==                                   \
              OBS_RAWOBSERVATIONHEADERSOURCE_DROPPED_COUNT_OFFSET &&         \
          offsetof(Type, flags) ==                                           \
              OBS_RAWOBSERVATIONHEADERSOURCE_FLAGS_OFFSET &&                 \
          offsetof(Type, reserved) ==                                        \
              OBS_RAWOBSERVATIONHEADERSOURCE_RESERVED_OFFSET,                \
      "RawObservationHeaderSource descriptor layout mismatch")
}  // namespace th10hook
