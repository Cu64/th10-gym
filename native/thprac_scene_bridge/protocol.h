#pragma once

#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace thprac_scene_bridge {

constexpr std::uint32_t kMagic = 0x42534854;  // "THSB" in little-endian memory.
constexpr std::uint32_t kVersion = 13;
constexpr std::uint32_t kRequestTtlMinMs = 1'000;
constexpr std::uint32_t kRequestTtlMaxMs = 300'000;

enum class Status : std::int32_t {
  Idle = 0,
  Queued = 1,
  AwaitingFrontend = 2,
  StartPending = 4,
  StartedConfigured = 5,
  Rejected = 6,
  Expired = 7,
  Failed = 8,
};

enum class RestartStatus : std::int32_t {
  Idle = 0,
  Queued = 1,
  Rejected = 4,
  Failed = 5,
  ActionQueued = 6,
  TitleReady = 7,
  RetryStarted = 8,
};

enum class RestartPhase : std::int32_t {
  None = 0,
  Complete = 4,
  AwaitTitle = 5,
  TitleReady = 6,
  AwaitStartup = 7,
};

enum class RestartRoute : std::int32_t {
  GameOverReturnTitle = 1,
  PracticeGameOverReturnTitle = 2,
  PracticeStageCompleteReturnTitle = 3,
  RetailRetry = 4,
  GameOverRetry = 5,
  PracticeCompleteRetry = 6,
  ProfileChange = 7,
};

constexpr bool Th10ReturnsToTitle(RestartRoute route) {
  return route == RestartRoute::GameOverReturnTitle ||
         route == RestartRoute::PracticeGameOverReturnTitle ||
         route == RestartRoute::PracticeStageCompleteReturnTitle ||
         route == RestartRoute::ProfileChange;
}

constexpr bool Th10IsRetailRetry(RestartRoute route) {
  return route == RestartRoute::RetailRetry || route == RestartRoute::GameOverRetry ||
         route == RestartRoute::PracticeCompleteRetry;
}

constexpr std::uint8_t kTh10RestartInputHookSiteBytes[] = {
    0x66, 0x8b, 0x0e, 0xbb, 0x01, 0x00, 0x00, 0x00};
constexpr bool Th10RestartInputTargetMatches(std::int32_t controllerIndex,
                                             std::uint32_t inputAddress) {
  return controllerIndex == 0 && inputAddress == 0x00474e30;
}

inline bool Th10RestartInputHookSiteMatches(const std::uint8_t* bytes,
                                            std::size_t size) {
  return bytes != nullptr && size == sizeof(kTh10RestartInputHookSiteBytes) &&
         std::memcmp(bytes, kTh10RestartInputHookSiteBytes,
                     sizeof(kTh10RestartInputHookSiteBytes)) == 0;
}

struct SceneRestartGuard {
  std::int32_t ownerState = -1;
  std::int32_t ownerPathMarker = -1;
  std::int32_t lives = 0;
  std::int32_t stage = 0;
  std::int32_t modeFlags = -1;
  std::int32_t managerFlags = -1;
  std::int32_t input = -1;
  std::int32_t frontendInput = -1;
};

constexpr bool Th10GameOverReturnTitleGuard(bool readable,
                                            const SceneRestartGuard& guard) {
  return readable && guard.ownerState == 6 && guard.ownerPathMarker == 0 &&
         guard.lives == -1 && guard.stage == 1 && guard.modeFlags == 0 &&
         (guard.managerFlags & 0x1000) == 0 && guard.input == 0 &&
         guard.frontendInput == 0;
}

constexpr bool Th10PracticeGameOverReturnTitleGuard(
    bool readable, const SceneRestartGuard& guard) {
  return readable && guard.ownerState == 6 && guard.ownerPathMarker == 0 &&
         guard.lives == -1 && guard.stage >= 1 && guard.stage <= 6 &&
         guard.modeFlags == 0x10 && (guard.managerFlags & 0x1000) == 0 &&
         guard.input == 0 && guard.frontendInput == 0;
}

constexpr bool Th10PracticeStageCompleteReturnTitleGuard(
    bool readable, const SceneRestartGuard& guard) {
  return readable && guard.ownerState == 6 && guard.ownerPathMarker == 1 &&
         guard.lives >= 0 && guard.stage >= 1 && guard.stage <= 6 &&
         guard.modeFlags == 0x10 && (guard.managerFlags & 0x1000) == 0 &&
         guard.input == 0 && guard.frontendInput == 0;
}

enum class SectionPatchVerification : std::int32_t {
  Unverified = 0,
  Matched = 1,
  Mismatch = 2,
};

enum class SectionDialoguePolicy : std::int32_t {
  Keep = 0,
  SkipSectionDialogue = 1,
};

enum class Error : std::int32_t {
  None = 0,
  ProtocolMismatch = 1,
  InvalidRequest = 2,
  UnsupportedSection = 3,
  FrontendMismatch = 4,
  StartupReadbackMismatch = 5,
  DeadlineExpired = 6,
  SafetyReadbackMismatch = 7,
  HookMismatch = 8,
  PracticeModeNotConfirmed = 9,
  SectionPatchMismatch = 10,
};

constexpr std::int32_t kTh10PracticeModeFlag = 0x10;

constexpr bool Th10DialogueBranchTaken(bool zeroFlag, bool forceAdvance) {
  return zeroFlag && !forceAdvance;
}

constexpr std::uintptr_t Th10DialogueNextIp(std::int32_t messageCase,
                                            bool zeroFlag, bool forceAdvance) {
  const bool take = Th10DialogueBranchTaken(zeroFlag, forceAdvance);
  if (messageCase == 10) return take ? 0x416f2d : 0x416a52;
  return 0;
}

enum class SectionKind : std::int32_t {
  StageStart = 0,
  StagePortion = 1,
  Midboss = 2,
  Boss = 3,
  Nonspell = 4,
  Spell = 5,
  NormalGameStart = 6,
};

struct RestartSessionState {
  bool startedConfigured;
  std::int32_t sectionKind;
  std::int32_t stage;
  bool normalRunActive;
  bool practiceMode;
  bool cheatsDisabled;
  bool requestIdMatches;
};

constexpr bool Th10RestartSessionAllowed(RestartRoute route,
                                         const RestartSessionState& session) {
  if (!session.startedConfigured || !session.cheatsDisabled ||
      !session.requestIdMatches)
    return false;
  const bool normalSession =
      session.sectionKind == (std::int32_t)SectionKind::NormalGameStart &&
      session.stage == 1 && session.normalRunActive;
  const bool practiceSession =
      session.sectionKind != (std::int32_t)SectionKind::NormalGameStart &&
      session.stage >= 1 && session.stage <= 6 && !session.normalRunActive &&
      session.practiceMode;
  if (route == RestartRoute::GameOverReturnTitle) return normalSession;
  if (route == RestartRoute::PracticeGameOverReturnTitle ||
      route == RestartRoute::PracticeStageCompleteReturnTitle ||
      route == RestartRoute::PracticeCompleteRetry)
    return practiceSession;
  if (route != RestartRoute::ProfileChange && !Th10IsRetailRetry(route))
    return false;
  return normalSession || practiceSession;
}

struct RetailRetryState {
  std::int32_t stage;
  std::int32_t startStage;
  std::uint32_t modeFlags;
  std::int32_t currentState;
  std::int32_t previousState;
  std::uint32_t managerFlags;
  std::int32_t replayMode;
  std::uint16_t input;
  std::uint32_t frontendInput;
};

constexpr bool Th10RetailRetryAllowed(
    const RetailRetryState& state, std::int32_t requestedStage, bool normal,
    RestartRoute route = RestartRoute::RetailRetry,
    bool changingProfile = false) {
  const std::uint32_t expectedFlags =
      route == RestartRoute::RetailRetry ? 0 : 0x10;
  const bool stageMatches =
      state.stage == requestedStage ||
      (changingProfile && normal && state.stage >= 1 && state.stage <= 6);
  return stageMatches && state.startStage == requestedStage &&
         state.modeFlags == (normal ? 0u : 0x10u) && state.currentState == 7 &&
         state.previousState == 7 &&
         (state.managerFlags & 0x74) == expectedFlags &&
         state.replayMode == 0 && state.input == 0 && state.frontendInput == 0;
}

constexpr bool Th10RetailRetryTerminalMatches(RestartRoute route, bool normal,
    std::int32_t ownerState, std::int32_t ownerPathMarker, std::int32_t lives) {
  if (ownerState != 6) return false;
  if (route == RestartRoute::GameOverRetry) return ownerPathMarker == 0 && lives == -1;
  return route == RestartRoute::PracticeCompleteRetry && !normal &&
         ownerPathMarker == 1 && lives >= 0;
}

// Shared by the Python CLI and the private in-process THPrac fork. Keep all
// fields fixed-width. Writers publish request_sequence last with Interlocked
// operations. The game thread is the only consumer of requests.
struct alignas(8) Mapping {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t struct_size;
  volatile LONG request_sequence;
  volatile LONG response_sequence;
  volatile LONG response_lock;
  volatile LONG status;
  volatile LONG error;
  std::uint8_t request_id[16];
  std::uint64_t deadline_tick_ms;

  std::int32_t character;
  std::int32_t shot_type;
  std::int32_t difficulty;
  std::int32_t stage;
  std::int32_t section_kind;
  std::int32_t section_index;
  std::int32_t life;
  std::int32_t power;
  std::int32_t faith;
  std::int32_t invincible;
  std::int32_t infinite_lives;
  std::int32_t infinite_power;
  std::int32_t time_lock;
  std::int32_t auto_bomb;
  std::int32_t no_faith_loss;

  std::int32_t observed_character;
  std::int32_t observed_shot_type;
  std::int32_t observed_difficulty;
  std::int32_t observed_menu_state;
  std::int32_t observed_menu_substate;
  std::int32_t applied_stage;
  std::int32_t applied_section_kind;
  std::int32_t applied_section_index;
  std::int32_t applied_section_id;
  std::int32_t applied_life;
  std::int32_t applied_power;
  std::int32_t applied_faith;
  std::int32_t applied_invincible;
  std::int32_t applied_infinite_lives;
  std::int32_t applied_infinite_power;
  std::int32_t applied_time_lock;
  std::int32_t applied_auto_bomb;
  std::int32_t applied_no_faith_loss;
  std::uint64_t last_update_tick_ms;
  std::uint32_t observed_game_mode_flags;
  std::uint32_t applied_game_mode_flags;
  std::int32_t section_dialogue_policy;
  std::int32_t section_patch_verification;
  std::int32_t dialogue_advance_ms;
  std::int32_t dialogue_advance_forced;
  std::uint64_t dialogue_advance_deadline_tick_ms;
  std::int32_t dialogue_branch_visits;
  std::int32_t dialogue_lease_ms;
  volatile LONG dialogue_lease_generation;
  std::uint64_t dialogue_lease_deadline_tick_ms;
  std::int32_t dialogue_lease_request_sequence;
  std::uint8_t dialogue_lease_request_id[16];

  volatile LONG restart_request_sequence;
  volatile LONG restart_response_sequence;
  volatile LONG restart_status;
  volatile LONG restart_error;
  volatile LONG restart_phase;
  volatile LONG restart_input_polls;
  std::uint8_t restart_scene_request_id[16];
  std::uint64_t restart_deadline_tick_ms;
  volatile LONG restart_route;
  volatile LONG restart_guard_owner_state;
  volatile LONG restart_guard_owner_path_marker;
  volatile LONG restart_guard_lives;
  volatile LONG restart_guard_stage;
  volatile LONG restart_guard_mode_flags;
  volatile LONG restart_guard_manager_flags;
  volatile LONG restart_guard_input;
  volatile LONG restart_guard_frontend_input;
  volatile LONG reserved_v8_tail;
  volatile LONG restart_last_input_active;
  volatile LONG restart_input_hook_entries;
  volatile LONG restart_inactive_input_polls;
  volatile LONG restart_synthetic_input_polls;  // Reserved. Always zero on v13.
};

static_assert(alignof(Mapping) == 8);
static_assert(offsetof(Mapping, request_sequence) == 12);
static_assert(offsetof(Mapping, response_sequence) == 16);
static_assert(offsetof(Mapping, response_lock) == 20);
static_assert(offsetof(Mapping, request_id) == 32);
static_assert(offsetof(Mapping, deadline_tick_ms) == 48);
static_assert(offsetof(Mapping, character) == 56);
static_assert(offsetof(Mapping, observed_character) == 116);
static_assert(offsetof(Mapping, applied_section_id) == 148);
static_assert(offsetof(Mapping, last_update_tick_ms) == 192);
static_assert(offsetof(Mapping, observed_game_mode_flags) == 200);
static_assert(offsetof(Mapping, section_dialogue_policy) == 208);
static_assert(offsetof(Mapping, section_patch_verification) == 212);
static_assert(offsetof(Mapping, dialogue_advance_ms) == 216);
static_assert(offsetof(Mapping, dialogue_advance_forced) == 220);
static_assert(offsetof(Mapping, dialogue_advance_deadline_tick_ms) == 224);
static_assert(offsetof(Mapping, dialogue_branch_visits) == 232);
static_assert(offsetof(Mapping, dialogue_lease_ms) == 236);
static_assert(offsetof(Mapping, dialogue_lease_generation) == 240);
static_assert(offsetof(Mapping, dialogue_lease_deadline_tick_ms) == 248);
static_assert(offsetof(Mapping, dialogue_lease_request_sequence) == 256);
static_assert(offsetof(Mapping, dialogue_lease_request_id) == 260);
static_assert(offsetof(Mapping, restart_request_sequence) == 276);
static_assert(offsetof(Mapping, restart_response_sequence) == 280);
static_assert(offsetof(Mapping, restart_status) == 284);
static_assert(offsetof(Mapping, restart_error) == 288);
static_assert(offsetof(Mapping, restart_phase) == 292);
static_assert(offsetof(Mapping, restart_input_polls) == 296);
static_assert(offsetof(Mapping, restart_scene_request_id) == 300);
static_assert(offsetof(Mapping, restart_deadline_tick_ms) == 320);
static_assert(offsetof(Mapping, restart_route) == 328);
static_assert(offsetof(Mapping, restart_guard_owner_state) == 332);
static_assert(offsetof(Mapping, restart_guard_owner_path_marker) == 336);
static_assert(offsetof(Mapping, restart_guard_lives) == 340);
static_assert(offsetof(Mapping, restart_guard_stage) == 344);
static_assert(offsetof(Mapping, restart_guard_mode_flags) == 348);
static_assert(offsetof(Mapping, restart_guard_manager_flags) == 352);
static_assert(offsetof(Mapping, restart_guard_input) == 356);
static_assert(offsetof(Mapping, restart_guard_frontend_input) == 360);
static_assert(offsetof(Mapping, reserved_v8_tail) == 364);
static_assert(offsetof(Mapping, restart_last_input_active) == 368);
static_assert(offsetof(Mapping, restart_input_hook_entries) == 372);
static_assert(offsetof(Mapping, restart_inactive_input_polls) == 376);
static_assert(offsetof(Mapping, restart_synthetic_input_polls) == 380);
static_assert(sizeof(Mapping) == 384);

}  // namespace thprac_scene_bridge
