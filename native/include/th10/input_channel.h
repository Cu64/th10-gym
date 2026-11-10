#pragma once

#include "tick_input_control.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace th10hook {

static constexpr std::uint32_t InputChannelMagic = 0x31555448;  // "HTU1"
static constexpr std::uint16_t InputChannelVersion = 1;

// Mapping name: Local\\TH10Gym.Input.<pid>
// Each publication counter is a 32-bit seqlock written with Win32
// InterlockedExchange: odd while writing, even when complete. Readers copy
// between two equal even reads. Writers must never reuse an active command.
struct InputChannelCommand {
  std::uint32_t session_id;
  std::uint32_t sequence;
  std::uint32_t published_at_ms;
  std::uint32_t ttl_ms;
  std::uint16_t actions;
  std::uint16_t ticks;  // Always 1.
  std::uint32_t flags;  // Always 0.
};

struct InputChannelMapping {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t size;
  volatile std::int32_t producer_heartbeat_ms;
  volatile std::int32_t command_publication;
  InputChannelCommand command;
  volatile std::int32_t ack_publication;
  InputAck ack;
};

static_assert(std::is_standard_layout<InputChannelCommand>::value &&
                  std::is_trivially_copyable<InputChannelCommand>::value,
              "command must remain a plain fixed-layout record");
static_assert(sizeof(InputChannelCommand) == 24 &&
                  offsetof(InputChannelCommand, actions) == 16,
              "live command ABI changed");
static_assert(std::is_standard_layout<InputChannelMapping>::value &&
                  std::is_trivially_copyable<InputChannelMapping>::value,
              "mapping must remain a plain fixed-layout record");
static_assert(sizeof(InputChannelMapping) == 72 &&
                  offsetof(InputChannelMapping, producer_heartbeat_ms) == 8 &&
                  offsetof(InputChannelMapping, command_publication) == 12 &&
                  offsetof(InputChannelMapping, command) == 16 &&
                  offsetof(InputChannelMapping, ack_publication) == 40 &&
                  offsetof(InputChannelMapping, ack) == 44,
              "live mapping ABI changed");

}  // namespace th10hook
