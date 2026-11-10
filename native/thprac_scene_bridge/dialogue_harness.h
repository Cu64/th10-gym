#pragma once

#include <cstddef>
#include <cstdint>

namespace thprac_scene_bridge {

// A bounded override for the verified opcode-10 dialogue wait branch. This
// helper does not identify dialogue phases beyond the interpreter opcode.
class DialogueHarness {
 public:
  enum class Error : std::uint32_t {
    None = 0,
    UnsupportedTarget = 1,
    InvalidDuration = 2,
  };

  static constexpr std::uint32_t kMinDurationMs = 1;
  static constexpr std::uint32_t kMaxDurationMs = 10'000;
  static constexpr std::uintptr_t kBranch = 0x00416A4C;
  static constexpr std::uintptr_t kTakenTarget = 0x00416F2D;
  static constexpr std::uintptr_t kFallthroughTarget = 0x00416A52;
  static constexpr std::int32_t kDialogueWaitOpcode = 0x0A;

  struct Counters {
    std::uint64_t visits = 0;
    std::uint64_t overrides = 0;
    std::uint64_t expirations = 0;
  };

  bool ValidateSite(std::uintptr_t site, const std::uint8_t* bytes,
                    std::size_t size) {
    static const std::uint8_t expected[] = {0x0F, 0x84, 0xDB, 0x04, 0x00, 0x00};
    if (site != kBranch || !bytes || size != sizeof(expected) ||
        !Equal(bytes, expected, sizeof(expected))) {
      site_valid_ = false;
      active_ = false;
      deadline_ms_ = 0;
      error_ = Error::UnsupportedTarget;
      return false;
    }
    site_valid_ = true;
    error_ = Error::None;
    return true;
  }

  bool Arm(std::uint64_t now_ms, std::uint32_t duration_ms) {
    active_ = false;
    expired_ = false;
    deadline_ms_ = 0;
    counters_ = {};
    error_ = Error::None;
    if (!site_valid_) {
      error_ = Error::UnsupportedTarget;
      return false;
    }
    if (duration_ms < kMinDurationMs || duration_ms > kMaxDurationMs ||
        now_ms > UINT64_MAX - duration_ms) {
      error_ = Error::InvalidDuration;
      return false;
    }
    deadline_ms_ = now_ms + duration_ms;
    active_ = true;
    return true;
  }

  // Renew only for a short interval. A stopped or disconnected client lets
  // the lease expire without further game-thread or OS input.
  bool Renew(std::uint64_t now_ms, std::uint32_t duration_ms) {
    if (!site_valid_) {
      error_ = Error::UnsupportedTarget;
      return false;
    }
    if (duration_ms < kMinDurationMs || duration_ms > kMaxDurationMs ||
        now_ms > UINT64_MAX - duration_ms) {
      error_ = Error::InvalidDuration;
      return false;
    }
    deadline_ms_ = now_ms + duration_ms;
    active_ = true;
    expired_ = false;
    error_ = Error::None;
    return true;
  }

  bool RenewUntil(std::uint64_t now_ms, std::uint64_t deadline_ms) {
    if (deadline_ms <= now_ms || deadline_ms - now_ms > kMaxDurationMs) {
      active_ = false;
      deadline_ms_ = 0;
      error_ = Error::InvalidDuration;
      return false;
    }
    return Renew(now_ms, static_cast<std::uint32_t>(deadline_ms - now_ms));
  }

  // Call at the guarded branch. The opcode check prevents an override if
  // the caller's interpreter context does not identify opcode 10.
  bool ShouldAdvance(std::uint64_t now_ms, std::int32_t script_opcode,
                     bool original_zf) {
    ++counters_.visits;
    Poll(now_ms);
    if (!active_ || script_opcode != kDialogueWaitOpcode || !original_zf)
      return false;
    ++counters_.overrides;
    return true;
  }

  void Poll(std::uint64_t now_ms) {
    if (active_ && now_ms >= deadline_ms_) {
      active_ = false;
      expired_ = true;
      deadline_ms_ = 0;
      ++counters_.expirations;
    }
  }

  // This is the JZ routing rule at kBranch. The original target remains in
  // use unless the bounded helper requests the fallthrough path.
  static bool ShouldTakeZeroBranch(bool original_zf, bool force_advance) {
    return original_zf && !force_advance;
  }

  static std::uintptr_t BranchTarget(bool original_zf, bool force_advance) {
    return ShouldTakeZeroBranch(original_zf, force_advance)
               ? kTakenTarget
               : kFallthroughTarget;
  }

  void Disable() {
    active_ = false;
    deadline_ms_ = 0;
  }

  void Reset() {
    site_valid_ = false;
    active_ = false;
    expired_ = false;
    deadline_ms_ = 0;
    error_ = Error::None;
    counters_ = {};
  }

  bool capable() const { return site_valid_; }
  bool active() const { return active_; }
  bool expired() const { return expired_; }
  Error error() const { return error_; }
  std::uint64_t deadline_ms() const { return deadline_ms_; }
  Counters counters() const { return counters_; }

 private:
  static bool Equal(const std::uint8_t* lhs, const std::uint8_t* rhs,
                    std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
      if (lhs[i] != rhs[i]) return false;
    return true;
  }

  bool site_valid_ = false;
  bool active_ = false;
  bool expired_ = false;
  std::uint64_t deadline_ms_ = 0;
  Error error_ = Error::None;
  Counters counters_{};
};

}  // namespace thprac_scene_bridge
