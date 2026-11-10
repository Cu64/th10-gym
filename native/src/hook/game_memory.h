// Fault-tolerant reads of game memory from inside the game process.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace th10hook {

bool bounded_game_address(std::uint32_t address, std::size_t length);
// The replay manager can live below the image base, so it has its own bounds.
bool bounded_replay_manager_address(std::uint32_t address, std::size_t length);
// Handle read faults only. Write and execute faults still crash the game.
int read_fault_filter(EXCEPTION_POINTERS* exception);
bool guarded_read(std::uintptr_t address, void* output, std::size_t length);

template <typename T>
bool read_game(std::uintptr_t address, T& value) {
  return guarded_read(address, &value, sizeof(value));
}

}  // namespace th10hook
