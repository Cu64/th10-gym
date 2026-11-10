#include <cassert>
#include <cstring>

#include "game_hooks.h"
#include "game_memory.h"
#include "snapshot_capture.h"

namespace th10hook {
namespace {

constexpr std::uint32_t kPlayer = 0x01000000;
constexpr std::uint32_t kBulletManager = 0x02000000;
constexpr std::uintptr_t kBulletPool = kBulletManager + 0x60;
constexpr std::uintptr_t kBulletStride = 0x7f0;
constexpr std::uint32_t kMiddleSlot = 2;
constexpr std::uint32_t kLastSlot = SnapshotBulletCapacity - 1;
constexpr std::uint32_t kStableGroups =
    GroupPlayer | GroupResources | GroupConsumedInput;
constexpr std::uint32_t kAvailableGroups = kStableGroups | GroupBomb;

SnapshotChannelMapping channel{};
std::uint32_t bullet_manager = kBulletManager;
std::uintptr_t fault_address = 0;
std::uint32_t state_reads = 0;
std::uint32_t player_x_reads = 0;
bool move_during_capture = false;

template <typename T>
bool supply(void* output, std::size_t length, T value) {
  assert(length == sizeof(value));
  std::memcpy(output, &value, length);
  return true;
}

const SnapshotRecord& capture() {
  TickGate gate(nullptr, nullptr);
  CallbackSample calc{};
  calc.sequence = 1;
  calc.player_pointer = kPlayer;
  calc.player_state = 1;
  const std::uint16_t input[5]{};
  state_reads = player_x_reads = 0;
  publish_halted_snapshot(gate, true, calc, TerminalOutcome::None, &input);
  assert(gate.error() ==
         (move_during_capture ? GateError::InvalidChain : GateError::None));
  return channel.record;
}

void assert_bullet(const SnapshotRecord& record, std::uint32_t row,
                   std::uint32_t slot) {
  assert(record.bullets[row].slot == slot);
  assert(record.bullets[row].state == 2);
  assert(record.bullets[row].raw_flags == 42);
  assert(record.bullets[row].x == 12.5f);
  assert(record.bullet_spawn_ready[row] == 3);
}

}  // namespace

bool bounded_game_address(std::uint32_t address, std::size_t length) {
  return address >= 0x00400000u && length <= 0x100000000ull - address;
}

bool bounded_replay_manager_address(std::uint32_t, std::size_t) {
  assert(false);  // This fixture does not probe terminal outcomes.
  return false;
}

// Supply the retail addresses used by the production publisher.
bool guarded_read(std::uintptr_t address, void* output, std::size_t length) {
  if (address == fault_address) return false;
  if (address == retail::kPlayerPointer) return supply(output, length, kPlayer);
  if (address == kPlayer + retail::kPlayerState)
    return supply(output, length, std::uint32_t{1});
  if (address == kPlayer + retail::kPlayerX && move_during_capture)
    return supply(output, length, ++player_x_reads == 3 ? 1.0f : 0.0f);
  if (address == retail::kBulletManagerPointer)
    return supply(output, length, bullet_manager);
  if (address < kBulletPool) {
    std::memset(output, 0, length);
    return true;
  }
  const auto slot = (address - kBulletPool) / kBulletStride;
  const auto offset = (address - kBulletPool) % kBulletStride;
  assert(slot < SnapshotBulletCapacity);
  if (offset == 0x446) {
    ++state_reads;
    const std::uint16_t state =
        slot == 0 || slot == kMiddleSlot || slot == kLastSlot ? 2 : 0;
    return supply(output, length, state);
  }
  if (offset == 0x314) return supply(output, length, std::uint32_t{3});
  if (offset == 0) return supply(output, length, std::uint32_t{42});
  if (offset == 0x3b4) return supply(output, length, 12.5f);
  std::memset(output, 0, length);
  return true;
}

// The supplement has its own layout. This fixture checks the core record.
std::size_t CaptureEntityTables(SnapshotRecord&, std::uint8_t* output) {
  *output = 0;
  return 1;
}

}  // namespace th10hook

int main() {
  using namespace th10hook;
  HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  assert(ready);
  init_snapshot_capture(&channel, nullptr, ready);

  bullet_manager = 0;
  const auto& absent = capture();
  assert(absent.flags == SnapshotTickValid);
  assert(absent.groups_available == kAvailableGroups);
  assert(absent.groups_valid == kStableGroups);
  assert(absent.groups_read_fault == 0);
  assert(absent.bullet_scanned == 0 && absent.bullet_captured == 0);

  bullet_manager = kBulletManager;
  const auto& complete = capture();
  assert(complete.flags == SnapshotTickValid);
  assert(complete.groups_available == (kAvailableGroups | GroupBullets));
  assert(complete.groups_valid == (kStableGroups | GroupBullets));
  assert(complete.groups_read_fault == 0);
  assert(complete.bullet_scanned == SnapshotBulletCapacity);
  assert(state_reads == SnapshotBulletCapacity);
  assert(complete.bullet_captured == 3);
  assert_bullet(complete, 0, 0);
  assert_bullet(complete, 1, kMiddleSlot);
  assert_bullet(complete, 2, kLastSlot);

  fault_address = kBulletPool + kMiddleSlot * kBulletStride + 0x3b8;
  const auto& partial = capture();
  assert(partial.flags == (SnapshotTickValid | SnapshotCaptureFault));
  assert(partial.groups_available == (kAvailableGroups | GroupBullets));
  assert(partial.groups_valid == kStableGroups);
  assert(partial.groups_read_fault == GroupBullets);
  assert(partial.bullet_scanned == kMiddleSlot + 1);
  assert(partial.bullet_captured == 1);
  assert_bullet(partial, 0, 0);

  fault_address = 0;
  move_during_capture = true;
  const auto& changed = capture();
  assert(changed.flags == SnapshotCaptureFault);
  CloseHandle(ready);
}
