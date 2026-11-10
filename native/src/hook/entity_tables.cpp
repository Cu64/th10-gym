#define WIN32_LEAN_AND_MEAN
#include "snapshot_format.h"
#include <windows.h>
#include <cmath>
#include <cstring>

namespace th10hook {
namespace {

// Wire order matches the decoder in th10_gym/_snapshot/entities.py.
struct EntityTablesHeader {
  std::int32_t stage, score, faith, faith_decay_rate, faith_timer_current;
  float faith_timer_fraction;
  std::uint32_t item_manager, spell_owner, countdown_owner;
  std::int32_t state_timer_current;
  float player_timer_subframe;
  std::int32_t options_active;
  std::uint32_t dialogue_word, enemy_count, laser_manager, enemy_rows,
      laser_rows;
  std::uint32_t capture_qpc_low, capture_qpc_high;
};
static_assert(sizeof(EntityTablesHeader) == 76);

int read_fault(unsigned code) {
  return code == EXCEPTION_ACCESS_VIOLATION ||
                 code == EXCEPTION_IN_PAGE_ERROR || code == EXCEPTION_GUARD_PAGE
             ? EXCEPTION_EXECUTE_HANDLER
             : EXCEPTION_CONTINUE_SEARCH;
}

bool read(std::uintptr_t address, void* output, std::size_t size) {
  if (address < 0x10000 ||
      static_cast<std::uint64_t>(address) + size > 0x100000000ull)
    return false;
  __try {
    std::memcpy(output, reinterpret_cast<const void*>(address), size);
    return true;
  } __except (read_fault(GetExceptionCode())) {
    return false;
  }
}

template <typename T>
bool read(std::uintptr_t address, T& value) {
  return read(address, &value, sizeof(value));
}

// Add offsets in 64 bits so corrupt game pointers cannot wrap into another
// readable 32-bit region before the span check.
std::uint64_t at(std::uint32_t base, std::uint32_t offset) {
  return static_cast<std::uint64_t>(base) + offset;
}

class Writer {
 public:
  explicit Writer(std::uint8_t* output) : output_(output) {}

  bool copy(std::uint64_t address, std::size_t bytes) {
    if (address > UINT32_MAX || bytes > EntityTablesCapacity - size_ ||
        !read(static_cast<std::uintptr_t>(address), output_ + size_, bytes))
      return false;
    size_ += bytes;
    return true;
  }

  template <typename T>
  bool append(const T& row) {
    if (sizeof(row) > EntityTablesCapacity - size_) return false;
    std::memcpy(output_ + size_, &row, sizeof(row));
    size_ += sizeof(row);
    return true;
  }

  std::size_t size() const { return size_; }

 private:
  std::uint8_t* output_;
  std::size_t size_ = 0;
};

bool boss_slots(SnapshotRecord& record, Writer& writer) {
  if (!read(0x477704, record.boss_manager)) return false;
  if (!record.boss_manager) return true;
  if (at(record.boss_manager, 0x30) > UINT32_MAX) return false;
  std::uint32_t slots[8]{};
  if (!read(record.boss_manager + 0x10, slots)) return false;
  for (std::size_t i = 0; i < 8; ++i) {
    struct Slot {
      std::uint32_t pointer;
      float x, y, vx, vy;
      std::int32_t hp;
    };
    static_assert(sizeof(Slot) == 24);
    Slot slot{};
    slot.pointer = slots[i];
    if (slot.pointer) {
      if (at(slot.pointer, 0x2400) > UINT32_MAX ||
          !read(slot.pointer + 0x1068, &slot.x, 8) ||
          !read(slot.pointer + 0x1074, &slot.vx, 8) ||
          !read(slot.pointer + 0x23fc, slot.hp) || !std::isfinite(slot.x) ||
          !std::isfinite(slot.y) || !std::isfinite(slot.vx) ||
          !std::isfinite(slot.vy))
        return false;
    }
    if (i == 0) {
      record.boss_slot0 = slot.pointer;
      record.boss_x = slot.x;
      record.boss_y = slot.y;
      record.boss_current_hp = slot.hp;
    }
    if (!writer.append(slot)) return false;
  }
  record.groups_available |= GroupBoss;
  record.groups_valid |= GroupBoss;
  return true;
}

bool enemies(std::uint32_t manager, EntityTablesHeader& header,
             Writer& writer) {
  if (!manager) return true;
  if (at(manager, 0x64) > UINT32_MAX) return false;
  std::uint32_t list[3]{};
  if (!read(manager + 0x58, list)) return false;
  header.enemy_count = list[2];
  std::uint32_t node = list[0];
  while (node) {
    // Fail closed on corrupt/cyclic lists and capacity overflow.
    if (header.enemy_rows == 1024) return false;
    std::uint32_t link[3]{};
    if (!read(node, link) || link[0] < 0x10000) return false;
    const auto enemy = link[0];
    if (!writer.append(node) || !writer.append(link) ||
        !writer.copy(at(enemy, 0x1068), 24) ||  // position and velocity
        !writer.copy(at(enemy, 0x10ec), 16) ||  // damage and contact extents
        !writer.copy(at(enemy, 0x1138),
                     52) ||                    // ECL variables and update timer
        !writer.copy(at(enemy, 0x23fc), 8) ||  // life and adjacent raw field
        !writer.copy(at(enemy, 0x245c), 4) ||
        !writer.copy(at(enemy, 0x2470), 4) ||
        !writer.copy(at(enemy, 0x2480), 4) ||
        !writer.copy(at(enemy, 0x248c), 4) ||
        !writer.copy(at(enemy, 0x2494), 8 * 16))  // callback slots
      return false;
    node = link[1];
    ++header.enemy_rows;
  }
  return true;
}

bool lasers(EntityTablesHeader& header, Writer& writer) {
  if (!header.laser_manager) return true;
  if (at(header.laser_manager, 0x1c) > UINT32_MAX) return false;
  std::uint32_t node = 0;
  if (!read(header.laser_manager + 0x18, node)) return false;
  while (node) {
    if (header.laser_rows == 8192) return false;
    std::uint32_t fields[0x48 / 4]{};
    if (!read(node, fields)) return false;
    const std::uint32_t row[] = {node,       fields[0],  fields[2],  fields[3],
                                 fields[9],  fields[10], fields[12], fields[13],
                                 fields[15], fields[16], fields[17]};
    if (!writer.append(row)) return false;
    node = fields[2];
    ++header.laser_rows;
  }
  return true;
}

}  // namespace

std::size_t CaptureEntityTables(SnapshotRecord& record, std::uint8_t* output) {
  const auto player = record.player_pointer;
  EntityTablesHeader header{};
  Writer writer(output);
  if (player < 0x10000 || at(player, 0x4314) > UINT32_MAX ||
      !read(0x474c7c, header.stage) || !read(0x474c44, header.score) ||
      !read(0x474c4c, header.faith) ||
      !read(0x474c50, header.faith_decay_rate) ||
      !read(0x474c58, header.faith_timer_current) ||
      !read(0x474c5c, header.faith_timer_fraction) ||
      !read(0x477818, header.item_manager) ||
      !read(0x4776f4, header.spell_owner) ||
      !read(0x47770c, header.countdown_owner) ||
      !read(0x47781c, header.laser_manager) ||
      !read(player + 0x478, header.state_timer_current) ||
      !read(player + 0x47c, header.player_timer_subframe) ||
      !read(player + 0x3500, header.options_active))
    return 0;
  if (header.countdown_owner &&
      (at(header.countdown_owner, 0x9ec4) > UINT32_MAX ||
       !read(header.countdown_owner + 0x9eb8, header.dialogue_word)))
    return 0;
  if (!writer.append(header) || !boss_slots(record, writer)) return 0;

  // Preserve fixed-pool rows, including inactive slots, but omit the 944
  // bytes of unobserved state in each item slot.
  if (header.item_manager) {
    for (std::uint32_t i = 0; i < 2198; ++i) {
      const auto item = at(header.item_manager, 0x14 + i * 0x3f0);
      if (!writer.copy(item + 0x3ac, 24) || !writer.copy(item + 0x3c8, 40))
        return 0;
    }
  }
  if (!writer.copy(at(player, 0x49c), 128 * 0x5c) ||
      !writer.copy(at(player, 0x350c), 33 * 0x6c))
    return 0;
  for (std::uint32_t i = 0; i < 4; ++i) {
    const auto option = at(player, 0x32a0 + i * 0x98);
    if (!writer.copy(option, 4) || !writer.copy(option + 0x3c, 8)) return 0;
  }
  if ((header.spell_owner &&
       !writer.copy(at(header.spell_owner, 0x3734), 20)) ||
      (header.countdown_owner &&
       !writer.copy(at(header.countdown_owner, 0x9ec0), 4)) ||
      !enemies(record.boss_manager, header, writer) || !lasers(header, writer))
    return 0;
  LARGE_INTEGER clock{};
  QueryPerformanceCounter(&clock);
  header.capture_qpc_low = clock.LowPart;
  header.capture_qpc_high = static_cast<std::uint32_t>(clock.HighPart);
  std::memcpy(output, &header, sizeof(header));
  return writer.size();
}

}  // namespace th10hook
