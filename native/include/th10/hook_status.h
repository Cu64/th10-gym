#pragma once

#include <cstddef>
#include <cstdint>

namespace th10hook {

// The installer confirms installation here. Python reads the CalcPost sample
// only before arming and during Retry.
constexpr wchar_t kHookStatusName[] = L"Local\\TH10Gym.HookStatus.%lu";
constexpr std::uint32_t kHookStatusMagic = 0x49303154;  // "T10I"
constexpr std::uint16_t kHookStatusVersion = 1;
constexpr std::uint32_t kSampleFlagReadFault = 1;
constexpr std::uint32_t kSampleFlagTickReadFault = 2;
constexpr std::uint32_t kSampleFlagNoCallbackId = 4;
constexpr std::uint32_t kSampleFlagScopeOverflow = 8;
constexpr std::uint32_t kHookNotInstalled = 0;
constexpr std::uint32_t kHookInstalled =
    1;  // Set by the installer only after a successful patch

enum CallbackKind : std::uint32_t {
  kCallbackCalcPost = 2,
  kCallbackReplayPre = 3,
  kCallbackReplayPost = 4,
  kCallbackPlayerPre = 5,
  kCallbackPlayerPost = 6,
  kCallbackCalcPre = 7,
};

struct HookStatusMapping {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t size;
  std::uint32_t target_pid;
  volatile std::int32_t hook_install_state;
  volatile std::int32_t readiness_publication;
  std::uint32_t calc_sequence;
  std::uint32_t player_pointer;
  std::uint32_t stage_tick;
  std::uint32_t segment_tick;
};
static_assert(sizeof(HookStatusMapping) == 36 &&
                  offsetof(HookStatusMapping, hook_install_state) == 12 &&
                  offsetof(HookStatusMapping, readiness_publication) == 16 &&
                  offsetof(HookStatusMapping, calc_sequence) == 20,
              "hook status layout changed");

// What the hook reads at each game callback, for the tick verifier.
struct CallbackSample {
  std::uint32_t sequence;
  std::uint32_t callback_kind;
  std::uint32_t thread_id;
  std::uint32_t callback_id;
  std::uint32_t parent_callback_id;
  std::uint32_t flags;
  std::uint32_t player_pointer;
  std::int32_t player_state;
  float player_x;
  float player_y;
  std::uint32_t stage_tick;
  std::uint32_t segment_tick;
  std::uint16_t input_current;
  std::uint16_t input_previous;
  std::uint16_t input_repeated;
  std::uint16_t input_pressed_or_aux;
  std::uint16_t input_released_or_flags;
};
}  // namespace th10hook
