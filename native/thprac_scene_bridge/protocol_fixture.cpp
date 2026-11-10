#include "protocol.h"

static_assert(thprac_scene_bridge::Th10RetailRetryAllowed({1, 1, 0, 7, 7, 0, 0,
                                                           0, 0},
                                                          1, true));
static_assert(thprac_scene_bridge::Th10RetailRetryAllowed({3, 3, 0x10, 7, 7, 0,
                                                           0, 0, 0},
                                                          3, false));
static_assert(!thprac_scene_bridge::Th10RetailRetryAllowed({1, 1, 0, 7, 7, 0x10,
                                                            0, 0, 0},
                                                           1, true));
static_assert(!thprac_scene_bridge::Th10RetailRetryAllowed({1, 1, 0, 10, 7, 0,
                                                            0, 0, 0},
                                                           1, true));
static_assert(!thprac_scene_bridge::Th10RetailRetryAllowed({2, 1, 0, 7, 7, 0, 0,
                                                            0, 0},
                                                           1, true));
static_assert(thprac_scene_bridge::Th10RetailRetryAllowed(
    {1, 1, 0, 7, 7, 0x10, 0, 0, 0}, 1, true,
    thprac_scene_bridge::RestartRoute::GameOverRetry));
static_assert(!thprac_scene_bridge::Th10RetailRetryAllowed(
    {1, 1, 0, 7, 7, 0, 0, 0, 0}, 1, true,
    thprac_scene_bridge::RestartRoute::GameOverRetry));
static_assert(thprac_scene_bridge::Th10RetailRetryTerminalMatches(
    thprac_scene_bridge::RestartRoute::GameOverRetry, true, 6, 0, -1));
static_assert(!thprac_scene_bridge::Th10RetailRetryTerminalMatches(
    thprac_scene_bridge::RestartRoute::GameOverRetry, false, 6, 1, -1));
static_assert(thprac_scene_bridge::Th10RetailRetryTerminalMatches(
    thprac_scene_bridge::RestartRoute::PracticeCompleteRetry, false, 6, 1, 0));
static_assert(!thprac_scene_bridge::Th10RetailRetryTerminalMatches(
    thprac_scene_bridge::RestartRoute::PracticeCompleteRetry, true, 6, 1, 0));

#include <cstddef>

int main() {
  using thprac_scene_bridge::Mapping;
  using thprac_scene_bridge::Th10DialogueBranchTaken;
  using thprac_scene_bridge::Th10DialogueNextIp;
  using thprac_scene_bridge::Th10RestartInputHookSiteMatches;
  if (!Th10DialogueBranchTaken(true, false) ||
      Th10DialogueBranchTaken(false, false) ||
      Th10DialogueBranchTaken(true, true) ||
      Th10DialogueBranchTaken(false, true))
    return 4;
  if (Th10DialogueNextIp(10, true, false) != 0x416f2d ||
      Th10DialogueNextIp(10, false, false) != 0x416a52 ||
      Th10DialogueNextIp(10, true, true) != 0x416a52 ||
      Th10DialogueNextIp(10, false, true) != 0x416a52 ||
      Th10DialogueNextIp(5, true, true) != 0)
    return 5;
  using thprac_scene_bridge::RestartRoute;
  using thprac_scene_bridge::SectionKind;
  using thprac_scene_bridge::Th10GameOverReturnTitleGuard;
  using thprac_scene_bridge::Th10PracticeGameOverReturnTitleGuard;
  using thprac_scene_bridge::Th10PracticeStageCompleteReturnTitleGuard;
  using thprac_scene_bridge::Th10RestartSessionAllowed;
  if (Th10RestartSessionAllowed(static_cast<RestartRoute>(0),
                                {true, (int)SectionKind::NormalGameStart, 1,
                                 true, false, true, true}) ||
      Th10RestartSessionAllowed(
          static_cast<RestartRoute>(0),
          {true, (int)SectionKind::Boss, 3, false, true, true, true}) ||
      !Th10RestartSessionAllowed(RestartRoute::RetailRetry,
                                 {true, (int)SectionKind::NormalGameStart, 1,
                                  true, false, true, true}) ||
      !Th10RestartSessionAllowed(
          RestartRoute::RetailRetry,
          {true, (int)SectionKind::Boss, 3, false, true, true, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::GameOverReturnTitle,
          {true, (int)SectionKind::Boss, 3, false, true, true, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::RetailRetry,
          {true, (int)SectionKind::Boss, 3, true, true, true, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::RetailRetry,
          {true, (int)SectionKind::Boss, 3, false, false, true, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::RetailRetry,
          {true, (int)SectionKind::Boss, 3, false, true, false, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::RetailRetry,
          {true, (int)SectionKind::Boss, 3, false, true, true, false}) ||
      !Th10RestartSessionAllowed(RestartRoute::GameOverReturnTitle,
                                 {true, (int)SectionKind::NormalGameStart, 1,
                                  true, false, true, true}) ||
      !Th10RestartSessionAllowed(
          RestartRoute::PracticeGameOverReturnTitle,
          {true, (int)SectionKind::StagePortion, 1, false, true, true, true}) ||
      Th10RestartSessionAllowed(RestartRoute::PracticeGameOverReturnTitle,
                                {true, (int)SectionKind::NormalGameStart, 1,
                                 true, false, true, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::PracticeGameOverReturnTitle,
          {true, (int)SectionKind::StagePortion, 7, false, true, true, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::PracticeGameOverReturnTitle,
          {true, (int)SectionKind::StagePortion, 1, true, true, true, true}) ||
      Th10RestartSessionAllowed(RestartRoute::PracticeGameOverReturnTitle,
                                {true, (int)SectionKind::StagePortion, 1, false,
                                 false, true, true}) ||
      Th10RestartSessionAllowed(RestartRoute::PracticeGameOverReturnTitle,
                                {true, (int)SectionKind::StagePortion, 1, false,
                                 true, false, true}) ||
      Th10RestartSessionAllowed(RestartRoute::PracticeGameOverReturnTitle,
                                {true, (int)SectionKind::StagePortion, 1, false,
                                 true, true, false}) ||
      !Th10RestartSessionAllowed(
          RestartRoute::PracticeStageCompleteReturnTitle,
          {true, (int)SectionKind::StagePortion, 1, false, true, true, true}) ||
      Th10RestartSessionAllowed(RestartRoute::PracticeStageCompleteReturnTitle,
                                {true, (int)SectionKind::NormalGameStart, 1,
                                 true, false, true, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::PracticeStageCompleteReturnTitle,
          {true, (int)SectionKind::StagePortion, 7, false, true, true, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::PracticeStageCompleteReturnTitle,
          {true, (int)SectionKind::StagePortion, 1, true, true, true, true}) ||
      Th10RestartSessionAllowed(RestartRoute::PracticeStageCompleteReturnTitle,
                                {true, (int)SectionKind::StagePortion, 1, false,
                                 false, true, true}) ||
      Th10RestartSessionAllowed(RestartRoute::PracticeStageCompleteReturnTitle,
                                {true, (int)SectionKind::StagePortion, 1, false,
                                 true, false, true}) ||
      Th10RestartSessionAllowed(
          RestartRoute::PracticeStageCompleteReturnTitle,
          {true, (int)SectionKind::StagePortion, 1, false, true, true, false}))
    return 16;
  const auto* restartSite = thprac_scene_bridge::kTh10RestartInputHookSiteBytes;
  std::uint8_t mutatedRestartSite[sizeof(
      thprac_scene_bridge::kTh10RestartInputHookSiteBytes)];
  std::memcpy(mutatedRestartSite, restartSite, sizeof(mutatedRestartSite));
  mutatedRestartSite[0] ^= 0x01;
  if (!Th10RestartInputHookSiteMatches(
          restartSite,
          sizeof(thprac_scene_bridge::kTh10RestartInputHookSiteBytes)) ||
      Th10RestartInputHookSiteMatches(mutatedRestartSite,
                                      sizeof(mutatedRestartSite)) ||
      Th10RestartInputHookSiteMatches(restartSite, 6) ||
      Th10RestartInputHookSiteMatches(nullptr, 7))
    return 9;
  if (!thprac_scene_bridge::Th10RestartInputTargetMatches(0, 0x00474e30) ||
      thprac_scene_bridge::Th10RestartInputTargetMatches(1, 0x00474e30) ||
      thprac_scene_bridge::Th10RestartInputTargetMatches(0, 0x00474e30 + 0x6a))
    return 15;
  if (!Th10GameOverReturnTitleGuard(true, {6, 0, -1, 1, 0, 0x200, 0, 0}) ||
      Th10GameOverReturnTitleGuard(false, {6, 0, -1, 1, 0, 0, 0, 0}) ||
      Th10GameOverReturnTitleGuard(true, {5, 0, -1, 1, 0, 0, 0, 0}) ||
      Th10GameOverReturnTitleGuard(true, {6, 1, -1, 1, 0, 0, 0, 0}) ||
      Th10GameOverReturnTitleGuard(true, {6, 0, 0, 1, 0, 0, 0, 0}) ||
      Th10GameOverReturnTitleGuard(true, {6, 0, -1, 2, 0, 0, 0, 0}) ||
      Th10GameOverReturnTitleGuard(true, {6, 0, -1, 1, 1, 0, 0, 0}) ||
      Th10GameOverReturnTitleGuard(true, {6, 0, -1, 1, 0, 0x1000, 0, 0}) ||
      Th10GameOverReturnTitleGuard(true, {6, 0, -1, 1, 0, 0, 1, 0}) ||
      Th10GameOverReturnTitleGuard(true, {6, 0, -1, 1, 0, 0, 0, 1}))
    return 8;
  if (!Th10PracticeGameOverReturnTitleGuard(true,
                                            {6, 0, -1, 1, 0x10, 0x200, 0, 0}) ||
      !Th10PracticeGameOverReturnTitleGuard(true,
                                            {6, 0, -1, 6, 0x10, 0, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(false,
                                           {6, 0, -1, 1, 0x10, 0, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true,
                                           {5, 0, -1, 1, 0x10, 0, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true,
                                           {6, 1, -1, 1, 0x10, 0, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true, {6, 0, 0, 1, 0x10, 0, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true,
                                           {6, 0, -1, 0, 0x10, 0, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true,
                                           {6, 0, -1, 7, 0x10, 0, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true, {6, 0, -1, 1, 0, 0, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true,
                                           {6, 0, -1, 1, 0x11, 0, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true,
                                           {6, 0, -1, 1, 0x10, 0x1000, 0, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true,
                                           {6, 0, -1, 1, 0x10, 0, 1, 0}) ||
      Th10PracticeGameOverReturnTitleGuard(true, {6, 0, -1, 1, 0x10, 0, 0, 1}))
    return 32;
  if (!Th10PracticeStageCompleteReturnTitleGuard(
          true, {6, 1, 0, 1, 0x10, 0x200, 0, 0}) ||
      !Th10PracticeStageCompleteReturnTitleGuard(true,
                                                 {6, 1, 4, 6, 0x10, 0, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(false,
                                                {6, 1, 0, 1, 0x10, 0, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(true,
                                                {5, 1, 0, 1, 0x10, 0, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(true,
                                                {6, 0, 0, 1, 0x10, 0, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(true,
                                                {6, 1, -1, 1, 0x10, 0, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(true,
                                                {6, 1, 0, 0, 0x10, 0, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(true,
                                                {6, 1, 0, 7, 0x10, 0, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(true,
                                                {6, 1, 0, 1, 0, 0, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(true,
                                                {6, 1, 0, 1, 0x11, 0, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(
          true, {6, 1, 0, 1, 0x10, 0x1000, 0, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(true,
                                                {6, 1, 0, 1, 0x10, 0, 1, 0}) ||
      Th10PracticeStageCompleteReturnTitleGuard(true,
                                                {6, 1, 0, 1, 0x10, 0, 0, 1}))
    return 33;
  return thprac_scene_bridge::kVersion == 13 && sizeof(Mapping) == 384 &&
                 offsetof(Mapping, request_sequence) == 12 &&
                 offsetof(Mapping, response_lock) == 20 &&
                 offsetof(Mapping, request_id) == 32 &&
                 offsetof(Mapping, deadline_tick_ms) == 48 &&
                 offsetof(Mapping, character) == 56 &&
                 offsetof(Mapping, observed_character) == 116 &&
                 offsetof(Mapping, applied_section_id) == 148 &&
                 offsetof(Mapping, last_update_tick_ms) == 192 &&
                 offsetof(Mapping, observed_game_mode_flags) == 200 &&
                 offsetof(Mapping, section_dialogue_policy) == 208 &&
                 offsetof(Mapping, section_patch_verification) == 212 &&
                 offsetof(Mapping, dialogue_advance_ms) == 216 &&
                 offsetof(Mapping, dialogue_advance_forced) == 220 &&
                 offsetof(Mapping, dialogue_advance_deadline_tick_ms) == 224 &&
                 offsetof(Mapping, dialogue_branch_visits) == 232 &&
                 offsetof(Mapping, dialogue_lease_ms) == 236 &&
                 offsetof(Mapping, dialogue_lease_generation) == 240 &&
                 offsetof(Mapping, dialogue_lease_deadline_tick_ms) == 248 &&
                 offsetof(Mapping, dialogue_lease_request_sequence) == 256 &&
                 offsetof(Mapping, dialogue_lease_request_id) == 260 &&
                 offsetof(Mapping, restart_request_sequence) == 276 &&
                 offsetof(Mapping, restart_response_sequence) == 280 &&
                 offsetof(Mapping, restart_status) == 284 &&
                 offsetof(Mapping, restart_error) == 288 &&
                 offsetof(Mapping, restart_phase) == 292 &&
                 offsetof(Mapping, restart_input_polls) == 296 &&
                 offsetof(Mapping, restart_scene_request_id) == 300 &&
                 offsetof(Mapping, restart_deadline_tick_ms) == 320 &&
                 offsetof(Mapping, restart_route) == 328 &&
                 offsetof(Mapping, restart_guard_owner_state) == 332 &&
                 offsetof(Mapping, restart_guard_owner_path_marker) == 336 &&
                 offsetof(Mapping, restart_guard_lives) == 340 &&
                 offsetof(Mapping, restart_guard_stage) == 344 &&
                 offsetof(Mapping, restart_guard_mode_flags) == 348 &&
                 offsetof(Mapping, restart_guard_manager_flags) == 352 &&
                 offsetof(Mapping, restart_guard_input) == 356 &&
                  offsetof(Mapping, restart_guard_frontend_input) == 360 &&
                 offsetof(Mapping, reserved_v8_tail) == 364 &&
                  offsetof(Mapping, restart_last_input_active) == 368 &&
                  offsetof(Mapping, restart_input_hook_entries) == 372 &&
                  offsetof(Mapping, restart_inactive_input_polls) == 376 &&
                  offsetof(Mapping, restart_synthetic_input_polls) == 380
             ? 0
             : 1;
}
