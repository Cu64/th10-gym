#pragma once

#include <cstdint>

namespace th10hook {

enum Action : std::uint16_t {
  Shoot = 0x0001,
  Bomb = 0x0002,
  Focus = 0x0004,
  Up = 0x0010,
  Down = 0x0020,
  Left = 0x0040,
  Right = 0x0080,
  KnownActions = 0x00f7
};

// In-process command used by the controller after reading the live mapping.
struct InputCommand {
  std::uint32_t session_id;
  std::uint32_t sequence;
  std::uint16_t actions;
};

inline bool valid_command(const InputCommand& command) {
  if (command.session_id == 0 || command.sequence == 0 ||
      (command.actions & ~KnownActions) != 0)
    return false;
  return !((command.actions & Up) && (command.actions & Down)) &&
         !((command.actions & Left) && (command.actions & Right));
}

}  // namespace th10hook
