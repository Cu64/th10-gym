#include "dialogue_harness.h"

#include <cassert>

using thprac_scene_bridge::DialogueHarness;

int main() {
  static const std::uint8_t expected[] = {0x0F, 0x84, 0xDB, 0x04, 0x00, 0x00};
  static const std::uint8_t wrong[] = {0x0F, 0x85, 0xDB, 0x04, 0x00, 0x00};
  DialogueHarness harness;

  assert(!harness.capable());
  assert(!harness.Arm(10, 100));
  assert(harness.error() == DialogueHarness::Error::UnsupportedTarget);
  assert(DialogueHarness::ShouldTakeZeroBranch(true, false));
  assert(!DialogueHarness::ShouldTakeZeroBranch(false, false));
  assert(DialogueHarness::BranchTarget(true, false) ==
         DialogueHarness::kTakenTarget);
  assert(DialogueHarness::BranchTarget(false, false) ==
         DialogueHarness::kFallthroughTarget);

  assert(!harness.ValidateSite(DialogueHarness::kBranch + 1, expected,
                               sizeof(expected)));
  assert(!harness.ValidateSite(DialogueHarness::kBranch, wrong, sizeof(wrong)));
  assert(!harness.capable());
  assert(harness.ValidateSite(DialogueHarness::kBranch, expected,
                              sizeof(expected)));
  assert(harness.capable());
  assert(harness.Arm(10, 100));
  assert(harness.active());

  // The branch callback records a visit but only forces opcode 10.
  assert(!harness.ShouldAdvance(11, 9, true));
  assert(
      !harness.ShouldAdvance(11, DialogueHarness::kDialogueWaitOpcode, false));
  assert(harness.counters().overrides == 0);
  assert(harness.ShouldAdvance(11, DialogueHarness::kDialogueWaitOpcode, true));
  assert(!DialogueHarness::ShouldTakeZeroBranch(true, true));
  assert(!DialogueHarness::ShouldTakeZeroBranch(false, true));
  assert(DialogueHarness::BranchTarget(true, true) ==
         DialogueHarness::kFallthroughTarget);
  assert(DialogueHarness::BranchTarget(false, true) ==
         DialogueHarness::kFallthroughTarget);
  assert(harness.counters().visits == 3);
  assert(harness.counters().overrides == 1);

  // With no override, preserve the original JZ choice in both directions.
  assert(DialogueHarness::ShouldTakeZeroBranch(true, false));
  assert(!DialogueHarness::ShouldTakeZeroBranch(false, false));

  // Expiry disables the override at the deadline and preserves original ZF.
  assert(
      !harness.ShouldAdvance(110, DialogueHarness::kDialogueWaitOpcode, true));
  assert(!harness.active());
  assert(harness.expired());
  assert(harness.counters().expirations == 1);
  assert(DialogueHarness::ShouldTakeZeroBranch(true, false));

  // Runtime renewals preserve counters, and the independent frame poll
  // expires the lease even when the dialogue hook is not visited.
  assert(harness.Renew(200, 100));
  assert(
      harness.ShouldAdvance(201, DialogueHarness::kDialogueWaitOpcode, true));
  harness.Poll(300);
  assert(!harness.active());
  assert(harness.counters().overrides == 2);
  assert(harness.counters().expirations == 2);
  assert(!harness.RenewUntil(301, 301));
  assert(!harness.RenewUntil(301, 10'302));
  assert(harness.RenewUntil(301, 401));
  assert(!harness.RenewUntil(402, 401));
  assert(!harness.active());
  assert(!harness.Arm(0, DialogueHarness::kMaxDurationMs + 1));
  assert(harness.error() == DialogueHarness::Error::InvalidDuration);

  assert(harness.Arm(200, 100));
  harness.Disable();
  assert(
      !harness.ShouldAdvance(201, DialogueHarness::kDialogueWaitOpcode, true));
  assert(!harness.active());
  harness.Reset();
  assert(!harness.capable());
  assert(!harness.expired());
  assert(harness.counters().visits == 0);
  assert(harness.counters().overrides == 0);
  return 0;
}
