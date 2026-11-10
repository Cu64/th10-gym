#pragma once

#include "input_controller.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace th10hook {

static constexpr std::uint32_t GateChannelMagic = 0x31505453;  // "STP1"
static constexpr std::uint16_t GateChannelVersion = 4;
static constexpr std::uint32_t GateMaxHeartbeatAgeMs = 2000;
// Every command sets this flag. Each input command authorizes one whole tick.
static constexpr std::uint32_t GateCommandSingleTick = 0x00000001;
// These commands mark the start and end of one bounded Retry transaction.
static constexpr std::uint32_t GateCommandResetBegin = 0x00000004;
static constexpr std::uint32_t GateCommandResetRearm = 0x00000008;
static constexpr std::uint32_t GateResetMaxDurationMs = 5000;

// Shared-memory channel for the gate's commands and status. Actions travel
// separately through the input channel.
struct GateChannelMapping {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t size;
  volatile std::int32_t producer_heartbeat_ms;
  volatile std::int32_t command_publication;
  std::uint32_t command_sequence;
  std::uint32_t enabled;
  std::uint32_t published_at_ms;
  std::uint32_t flags;
  volatile std::int32_t status_publication;
  std::uint32_t state;
  std::uint32_t echoed_command_sequence;
  std::uint32_t pass_count;
  std::uint32_t skip_count;
  std::uint32_t snapshot_sequence;
  std::uint32_t stage_tick;
  std::uint32_t segment_tick;
  std::uint32_t player_pointer;
  float player_x;
  float player_y;
  std::uint32_t error;
  // A rearm command carries the stage and segment ticks the client just read.
  // The gate also accepts the immediately following pair.
  std::uint32_t expected_stage_tick;
  std::uint32_t expected_segment_tick;
};

static_assert(std::is_standard_layout<GateChannelMapping>::value &&
                  std::is_trivially_copyable<GateChannelMapping>::value,
              "step mapping must remain a fixed-layout record");
static_assert(sizeof(GateChannelMapping) == 88 &&
                  offsetof(GateChannelMapping, expected_stage_tick) == 80 &&
                  offsetof(GateChannelMapping, expected_segment_tick) == 84,
              "strict reset step mapping ABI changed");

enum class GateState : std::uint32_t {
  Disabled = 0,
  Arming = 1,
  Halted = 2,
  Running = 3,
  Fault = 4,
  Resetting = 5
};
enum class GateError : std::uint32_t {
  None = 0,
  BadMapping = 1,
  BadCommand = 2,
  HeartbeatLost = 3,
  Escape = 4,
  OwnerViolation = 5,
  InvalidChain = 6,
  InputRelease = 7,
  SnapshotPublication = 8,
  SnapshotNotification = 9,
};

inline bool escape_pressed_for_foreground_process(std::uint32_t foreground_pid,
                                                  std::uint32_t target_pid,
                                                  bool key_down) {
  return foreground_pid != 0 && target_pid != 0 &&
         foreground_pid == target_pid && key_down;
}

struct GateTickState {
  std::uint32_t snapshot_sequence;
  std::uint32_t stage_tick;
  std::uint32_t segment_tick;
  std::uint32_t player_pointer;
  std::uint32_t player_state;
  float player_x;
  float player_y;
};

GateTickState gate_state_from(const TickState& state,
                              std::uint32_t snapshot_sequence);

class TickGate {
 public:
  using EscapeProbe = bool (*)(void*);

  explicit TickGate(GateChannelMapping* mapping, InputController* input,
                    EscapeProbe escape_probe = nullptr,
                    void* escape_context = nullptr);

  // Called at the outer-loop whole-tick callsite. False means skip the entire
  // FUN_00439390 call and return its ordinary-loop continuation value (zero).
  // The caller must preserve the wrapper's verified ABI on either branch.
  bool BeforeTick();

  // Start each original whole tick and retain its CalcPost state.
  void BeginTick();
  void NoteCalcPost(const GateTickState& snapshot, bool active_chain_valid,
                    bool verification_failed);

  // Called after the original whole tick returns. The caller supplies a fresh
  // post-tick read and validation result.
  // `verification_failed` means the callback chain changed ticks or context, or
  // the final post-tick sample disagreed with its CalcPost sample.
  void AfterTick(const GateTickState& snapshot, bool final_snapshot_valid);
  // Used by snapshot capture when the game state changes during capture.
  void FailVerification() { fault(GateError::InvalidChain); }
  void FailSnapshotPublication(GateError error) { fault(error); }

  GateState state() const { return state_; }
  bool OwnsCurrentThread() const;
  GateError error() const { return error_; }
  const GateTickState& halted_snapshot() const { return snapshot_; }
  std::uint32_t pass_count() const { return pass_count_; }
  std::uint32_t skip_count() const { return skip_count_; }

 private:
  struct GateCommand {
    std::uint32_t sequence, enabled, published_at, flags;
    std::uint32_t expected_stage_tick, expected_segment_tick;
  };

  bool claim_owner();
  bool mapping_ok() const;
  std::uint32_t read_heartbeat() const;
  bool read_command(GateCommand& command) const;
  bool accept_command(std::uint32_t now);
  void start_arming();
  void begin_reset(std::uint32_t now);
  void rearm_after_reset(const GateCommand& command);
  void fault(GateError reason);
  void publish_status();
  bool escape_down() const;
  bool check_escape();
  bool pass_reset_tick(std::uint32_t now);
  bool tick_with_invalid_mapping(std::uint32_t now);
  bool validate_halted_snapshot();
  bool poll_and_admit_tick();

  GateChannelMapping* mapping_;
  InputController* input_;
  EscapeProbe escape_probe_;
  void* escape_context_;
  volatile PVOID owner_thread_token_;
  volatile LONG owner_violation_;
  std::uint32_t last_command_sequence_;
  std::uint32_t echoed_command_sequence_;
  std::uint32_t pass_count_;
  std::uint32_t skip_count_;
  GateState state_;
  GateError error_;
  GateTickState snapshot_;
  GateTickState calc_post_snapshot_;
  bool has_calc_post_;
  bool calc_post_active_;
  bool verification_failed_;
  bool escape_ready_;
  bool arming_tick_passed_;
  std::uint32_t reset_started_ms_;
};

// Create the PID-scoped Local\\TH10Gym.Gate.<pid> mapping.
bool CreateGateChannel(std::uint32_t pid, HANDLE* mapping_handle,
                       GateChannelMapping** mapping_view,
                       std::uint32_t* win32_error);

}  // namespace th10hook
