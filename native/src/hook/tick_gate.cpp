#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <climits>
#include <cstdio>
#include <cstring>

#include "tick_gate.h"

namespace th10hook {
namespace {
__declspec(thread) int g_gate_thread_token;

bool deadline_passed(std::uint32_t now, std::uint32_t then, std::uint32_t ttl) {
  return static_cast<std::uint32_t>(now - then) > ttl;
}

bool same_snapshot(const GateTickState& a, const GateTickState& b) {
  return a.stage_tick == b.stage_tick && a.segment_tick == b.segment_tick &&
         a.player_pointer == b.player_pointer &&
         a.player_state == b.player_state &&
         std::memcmp(&a.player_x, &b.player_x, sizeof(float)) == 0 &&
         std::memcmp(&a.player_y, &b.player_y, sizeof(float)) == 0;
}

bool default_escape_probe(void*) {
  const HWND foreground = GetForegroundWindow();
  if (foreground == nullptr) return false;
  DWORD foreground_pid = 0;
  if (GetWindowThreadProcessId(foreground, &foreground_pid) == 0) return false;
  const bool key_down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
  return escape_pressed_for_foreground_process(foreground_pid,
                                               GetCurrentProcessId(), key_down);
}
}  // namespace

GateTickState gate_state_from(const TickState& state,
                              std::uint32_t snapshot_sequence) {
  GateTickState result{};
  result.snapshot_sequence = snapshot_sequence;
  result.stage_tick = state.stage_tick;
  result.segment_tick = state.segment_tick;
  result.player_pointer = state.player_pointer;
  result.player_state = state.player_state;
  std::memcpy(&result.player_x, &state.player_x_bits, sizeof(float));
  std::memcpy(&result.player_y, &state.player_y_bits, sizeof(float));
  return result;
}

TickGate::TickGate(GateChannelMapping* mapping, InputController* input,
                   EscapeProbe escape_probe, void* escape_context)
    : mapping_(mapping),
      input_(input),
      escape_probe_(escape_probe ? escape_probe : default_escape_probe),
      escape_context_(escape_context),
      owner_thread_token_(nullptr),
      owner_violation_(0),
      last_command_sequence_(0),
      echoed_command_sequence_(0),
      pass_count_(0),
      skip_count_(0),
      state_(GateState::Disabled),
      error_(GateError::None),
      snapshot_{},
      calc_post_snapshot_{},
      has_calc_post_(false),
      calc_post_active_(false),
      verification_failed_(false),
      escape_ready_(false),
      arming_tick_passed_(false),
      reset_started_ms_(0) {}

bool TickGate::claim_owner() {
  void* token = &g_gate_thread_token;
  const auto observed =
      InterlockedCompareExchangePointer(&owner_thread_token_, token, nullptr);
  if (observed == nullptr || observed == token) return true;
  InterlockedExchange(&owner_violation_, 1);
  return false;
}

bool TickGate::OwnsCurrentThread() const {
  return owner_thread_token_ == &g_gate_thread_token;
}

bool TickGate::mapping_ok() const {
  return mapping_ != nullptr && mapping_->magic == GateChannelMagic &&
         mapping_->version == GateChannelVersion &&
         mapping_->size == sizeof(GateChannelMapping);
}

bool TickGate::escape_down() const {
  return escape_probe_ != nullptr && escape_probe_(escape_context_);
}

void TickGate::publish_status() {
  if (!mapping_) return;
  volatile LONG* stamp =
      reinterpret_cast<volatile LONG*>(&mapping_->status_publication);
  const LONG previous = InterlockedCompareExchange(stamp, 0, 0);
  if (previous < 0 || previous > LONG_MAX - 2) return;
  const LONG odd = (previous & 1) ? previous + 2 : previous + 1;
  InterlockedExchange(stamp, odd);
  mapping_->state = static_cast<std::uint32_t>(state_);
  mapping_->echoed_command_sequence = echoed_command_sequence_;
  mapping_->pass_count = pass_count_;
  mapping_->skip_count = skip_count_;
  mapping_->snapshot_sequence = snapshot_.snapshot_sequence;
  mapping_->stage_tick = snapshot_.stage_tick;
  mapping_->segment_tick = snapshot_.segment_tick;
  mapping_->player_pointer = snapshot_.player_pointer;
  mapping_->player_x = snapshot_.player_x;
  mapping_->player_y = snapshot_.player_y;
  mapping_->error = static_cast<std::uint32_t>(error_);
  MemoryBarrier();
  InterlockedExchange(stamp, odd + 1);
}

void TickGate::fault(GateError reason) {
  state_ = GateState::Fault;
  error_ = reason;
  if (input_) input_->CancelCommand(InputCancelReason::FailedChecks);
  publish_status();
}

std::uint32_t TickGate::read_heartbeat() const {
  return static_cast<std::uint32_t>(InterlockedCompareExchange(
      reinterpret_cast<volatile LONG*>(&mapping_->producer_heartbeat_ms), 0,
      0));
}

bool TickGate::read_command(GateCommand& command) const {
  volatile LONG* publication =
      reinterpret_cast<volatile LONG*>(&mapping_->command_publication);
  const LONG before = InterlockedCompareExchange(publication, 0, 0);
  if (before <= 0 || (before & 1)) return false;
  MemoryBarrier();
  command.sequence = mapping_->command_sequence;
  command.enabled = mapping_->enabled;
  command.published_at = mapping_->published_at_ms;
  command.flags = mapping_->flags;
  command.expected_stage_tick = mapping_->expected_stage_tick;
  command.expected_segment_tick = mapping_->expected_segment_tick;
  MemoryBarrier();
  const LONG after = InterlockedCompareExchange(publication, 0, 0);
  return before == after && !(after & 1);
}

void TickGate::start_arming() {
  arming_tick_passed_ = false;
  state_ = GateState::Arming;
  escape_ready_ = false;
  error_ = GateError::None;
  pass_count_ = 0;
  skip_count_ = 0;
  snapshot_ = {};
  if (input_) input_->CancelCommand(InputCancelReason::Disconnected);
  publish_status();
}

void TickGate::begin_reset(std::uint32_t now) {
  TickState state{};
  if (!input_->ReadTickState(state) || state.player_pointer == 0 ||
      !same_snapshot(snapshot_,
                     gate_state_from(state, snapshot_.snapshot_sequence))) {
    fault(GateError::InvalidChain);
    return;
  }
  if (!input_->SetResetMode(true) || input_->active() || input_->faulted()) {
    fault(GateError::InputRelease);
    return;
  }
  reset_started_ms_ = now;
  state_ = GateState::Resetting;
  error_ = GateError::None;
  publish_status();
}

void TickGate::rearm_after_reset(const GateCommand& command) {
  TickState state{};
  if (!input_->ReadTickState(state) || state.player_pointer == 0 ||
      state.player_state != 1) {
    fault(GateError::InvalidChain);
    return;
  }
  // The game may have run one tick between the client's read and this command.
  const std::uint32_t stage_delta =
      state.stage_tick - command.expected_stage_tick;
  const std::uint32_t segment_delta =
      state.segment_tick - command.expected_segment_tick;
  if (!((stage_delta == 0 && segment_delta == 0) ||
        (stage_delta == 1 && segment_delta == 1))) {
    fault(GateError::InvalidChain);
    return;
  }
  input_->CancelCommand(InputCancelReason::Disconnected);
  if (!input_->SetResetMode(false) || input_->active() || input_->faulted()) {
    input_->SetResetMode(true);
    fault(GateError::InputRelease);
    return;
  }
  snapshot_ = gate_state_from(state, 0);
  arming_tick_passed_ = false;
  reset_started_ms_ = 0;
  state_ = GateState::Arming;
  error_ = GateError::None;
  publish_status();
}

bool TickGate::accept_command(std::uint32_t now) {
  if (!mapping_ok()) return false;
  GateCommand command{};
  if (!read_command(command) || command.sequence == 0 ||
      command.sequence == last_command_sequence_)
    return false;
  if (last_command_sequence_ != 0 &&
      static_cast<std::int32_t>(command.sequence - last_command_sequence_) <= 0)
    return false;

  last_command_sequence_ = command.sequence;
  echoed_command_sequence_ = command.sequence;
  if (command.enabled != 1 ||
      deadline_passed(now, read_heartbeat(), GateMaxHeartbeatAgeMs) ||
      deadline_passed(now, command.published_at, GateMaxHeartbeatAgeMs)) {
    fault(command.enabled != 1 ? GateError::BadCommand
                               : GateError::HeartbeatLost);
    return true;
  }
  const bool no_expected_ticks =
      command.expected_stage_tick == 0 && command.expected_segment_tick == 0;
  if (state_ == GateState::Disabled && command.flags == GateCommandSingleTick &&
      no_expected_ticks) {
    start_arming();
  } else if (state_ == GateState::Halted &&
             command.flags == (GateCommandSingleTick | GateCommandResetBegin) &&
             no_expected_ticks && input_ && !input_->active()) {
    begin_reset(now);
  } else if (state_ == GateState::Resetting &&
             command.flags == (GateCommandSingleTick | GateCommandResetRearm) &&
             !deadline_passed(now, reset_started_ms_, GateResetMaxDurationMs) &&
             input_ && !input_->active() && !input_->faulted()) {
    rearm_after_reset(command);
  } else {
    fault(GateError::BadCommand);
  }
  return true;
}

bool TickGate::pass_reset_tick(std::uint32_t now) {
  if (!mapping_ok()) {
    fault(GateError::BadMapping);
    return false;
  }
  if (deadline_passed(now, read_heartbeat(), GateMaxHeartbeatAgeMs)) {
    fault(GateError::HeartbeatLost);
    return false;
  }
  if (deadline_passed(now, reset_started_ms_, GateResetMaxDurationMs)) {
    fault(GateError::BadCommand);
    return false;
  }
  ++pass_count_;
  publish_status();
  return true;
}

bool TickGate::check_escape() {
  // Require a released sample before accepting a new press so leaving the
  // pause screen cannot fault startup.
  if (state_ != GateState::Halted && state_ != GateState::Running &&
      state_ != GateState::Fault)
    return true;
  if (!escape_down()) {
    escape_ready_ = true;
    return true;
  }
  if (!escape_ready_) return true;
  fault(GateError::Escape);
  return false;
}

bool TickGate::tick_with_invalid_mapping(std::uint32_t now) {
  if (state_ != GateState::Disabled) fault(GateError::BadMapping);
  if (mapping_ != nullptr &&
      deadline_passed(now, read_heartbeat(), GateMaxHeartbeatAgeMs)) {
    fault(GateError::HeartbeatLost);
    return false;
  }
  return state_ != GateState::Fault;
}

bool TickGate::validate_halted_snapshot() {
  TickState evidence{};
  if (input_ != nullptr && input_->ReadTickState(evidence) &&
      evidence.player_pointer != 0 &&
      same_snapshot(snapshot_,
                    gate_state_from(evidence, snapshot_.snapshot_sequence)))
    return true;
  fault(GateError::InvalidChain);
  return false;
}

bool TickGate::BeforeTick() {
  if (!claim_owner()) return state_ == GateState::Disabled;
  if (InterlockedExchange(&owner_violation_, 0) != 0) {
    fault(GateError::OwnerViolation);
    return false;
  }
  const std::uint32_t now = GetTickCount();
  if (mapping_ok()) accept_command(now);
  if (state_ == GateState::Resetting) return pass_reset_tick(now);
  if (!check_escape()) return false;
  if (!mapping_ok()) return tick_with_invalid_mapping(now);

  const std::uint32_t heartbeat = read_heartbeat();
  if (state_ != GateState::Disabled &&
      deadline_passed(now, heartbeat, GateMaxHeartbeatAgeMs)) {
    fault(GateError::HeartbeatLost);
    return false;
  }
  return poll_and_admit_tick();
}

bool TickGate::poll_and_admit_tick() {
  if (state_ == GateState::Halted && !validate_halted_snapshot()) return false;
  if (state_ == GateState::Halted && input_->PollCommand()) {
    state_ = GateState::Running;
    error_ = GateError::None;
    publish_status();
  }
  // Recheck even a newly accepted command before admitting a running tick.
  if (state_ == GateState::Running && input_ != nullptr &&
      !input_->PollCommand()) {
    state_ = GateState::Halted;
    publish_status();
  }
  if (state_ == GateState::Halted && !validate_halted_snapshot()) return false;
  if (state_ == GateState::Halted || state_ == GateState::Fault) {
    ++skip_count_;
    publish_status();
    return false;
  }
  if (state_ == GateState::Arming) {
    if (arming_tick_passed_) {
      fault(GateError::InvalidChain);
      ++skip_count_;
      return false;
    }
    arming_tick_passed_ = true;
  }
  if (state_ == GateState::Arming || state_ == GateState::Running) {
    ++pass_count_;
    publish_status();
  }
  return true;
}

void TickGate::BeginTick() {
  has_calc_post_ = false;
  calc_post_active_ = false;
  verification_failed_ = false;
  calc_post_snapshot_ = {};
}

void TickGate::NoteCalcPost(const GateTickState& snapshot,
                            bool active_chain_valid, bool verification_failed) {
  if (has_calc_post_) {
    verification_failed_ = true;
    return;
  }
  has_calc_post_ = true;
  calc_post_snapshot_ = snapshot;
  calc_post_active_ = active_chain_valid;
  verification_failed_ = verification_failed;
}

void TickGate::AfterTick(const GateTickState& snapshot,
                         bool final_snapshot_valid) {
  if (!claim_owner()) return;
  if (state_ != GateState::Arming && state_ != GateState::Running) return;
  if (verification_failed_) {
    fault(GateError::InvalidChain);
    return;
  }
  if (!has_calc_post_ || !calc_post_active_) {
    fault(GateError::InvalidChain);
    return;
  }
  if (!final_snapshot_valid || !same_snapshot(calc_post_snapshot_, snapshot)) {
    fault(GateError::InvalidChain);
    return;
  }
  if (snapshot.snapshot_sequence != 0) snapshot_ = snapshot;
  if (state_ == GateState::Arming) {
    state_ = GateState::Halted;
    error_ = GateError::None;
    publish_status();
    return;
  }
  if (input_ != nullptr && input_->active()) {
    fault(GateError::BadCommand);
    return;
  }
  state_ = GateState::Halted;
  error_ = GateError::None;
  publish_status();
}

bool CreateGateChannel(std::uint32_t pid, HANDLE* mapping_handle,
                       GateChannelMapping** mapping_view,
                       std::uint32_t* win32_error) {
  if (mapping_handle == nullptr || mapping_view == nullptr ||
      win32_error == nullptr || pid == 0)
    return false;
  *mapping_handle = nullptr;
  *mapping_view = nullptr;
  *win32_error = ERROR_INVALID_PARAMETER;
  wchar_t name[96]{};
  const int count =
      _snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE,
                   L"Local\\TH10Gym.Gate.%lu", static_cast<unsigned long>(pid));
  if (count < 0) {
    *win32_error = ERROR_INVALID_NAME;
    return false;
  }
  HANDLE handle =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         sizeof(GateChannelMapping), name);
  if (handle == nullptr) {
    *win32_error = GetLastError();
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(handle);
    *win32_error = ERROR_ALREADY_EXISTS;
    return false;
  }
  auto* view = static_cast<GateChannelMapping*>(
      MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                    sizeof(GateChannelMapping)));
  if (view == nullptr) {
    *win32_error = GetLastError();
    CloseHandle(handle);
    return false;
  }
  std::memset(view, 0, sizeof(*view));
  view->magic = GateChannelMagic;
  view->version = GateChannelVersion;
  view->size = sizeof(GateChannelMapping);
  MemoryBarrier();
  *mapping_handle = handle;
  *mapping_view = view;
  *win32_error = ERROR_SUCCESS;
  return true;
}

}  // namespace th10hook
