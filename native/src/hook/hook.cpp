// The hook DLL's entry points: the initializer the installer calls, and the
// functions the x86 wrappers in tick_hooks.cpp call around each game callback.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "game_hooks.h"
#include "game_memory.h"
#include "hook_internal.h"
#include "hook_status.h"
#include "image_check.h"
#include "input_controller.h"
#include "snapshot_capture.h"
#include "tick_gate.h"
#include "tick_verifier.h"

#if !defined(_M_IX86)
#error Build this DLL with the Visual Studio x86 environment.
#endif

namespace {

using namespace th10hook;

constexpr std::uint32_t kScopeLimit = 16;

HookStatusMapping* volatile g_hook_status = nullptr;
alignas(InputController) unsigned char g_controller_storage[sizeof(
    InputController)];
InputController* volatile g_controller = nullptr;
alignas(TickGate) unsigned char g_gate_storage[sizeof(TickGate)];
TickGate* volatile g_gate = nullptr;

__declspec(thread) std::uint32_t g_callback_sequence = 0;
volatile LONG g_callback_id_sequence = 0;

// Nested callbacks of one kind on this thread. Depth past the limit is counted
// so that the matching exits stay balanced, but those calls have no ID.
struct CallbackScope {
  std::uint32_t ids[kScopeLimit];
  std::uint32_t parents[kScopeLimit];
  std::uint32_t depth;
  std::uint32_t overflow;
};

struct ThreadScopes {
  std::uint32_t current_calc;
  CallbackScope calc, replay, player;
};
__declspec(thread) ThreadScopes g_scopes{};
__declspec(thread) TickVerifier g_tick_verifier{};

template <typename T>
T* load_pointer(T* volatile& pointer) {
  return static_cast<T*>(InterlockedCompareExchangePointer(
      reinterpret_cast<PVOID volatile*>(&pointer), nullptr, nullptr));
}

std::uint32_t process_id() { return GetCurrentProcessId(); }

void publish_readiness_sample(const CallbackSample& record) {
  auto* state = g_hook_status;
  auto* stamp = reinterpret_cast<volatile LONG*>(&state->readiness_publication);
  const LONG previous = InterlockedCompareExchange(stamp, 0, 0);
  if (previous >= LONG_MAX - 1) return;
  InterlockedExchange(stamp, previous + 1);
  state->calc_sequence = record.sequence;
  const bool active = record.flags == 0 && record.player_state == 1 &&
                      std::isfinite(record.player_x) &&
                      std::isfinite(record.player_y);
  state->player_pointer = active ? record.player_pointer : 0;
  state->stage_tick = record.stage_tick;
  state->segment_tick = record.segment_tick;
  InterlockedExchange(stamp, previous + 2);
}

void sample_callback(void* context, std::uint32_t callback_kind,
                     std::uint32_t callback_id,
                     std::uint32_t parent_callback_id,
                     std::uint32_t extra_flags) {
  auto* controller = load_pointer(g_controller);
  if (g_callback_sequence == LONG_MAX) {
    if (controller)
      controller->AfterCallback(callback_kind, callback_id, parent_callback_id,
                                {0, false});
    return;
  }

  CallbackSample record{};
  record.sequence = ++g_callback_sequence;
  record.callback_kind = callback_kind;
  record.thread_id = GetCurrentThreadId();
  record.flags = extra_flags;
  record.callback_id = callback_id;
  record.parent_callback_id = parent_callback_id;
  if (controller)
    controller->BeforeCallback(context, callback_kind, callback_id,
                               parent_callback_id);

  // Fixed, guarded snapshot reads. No VirtualQuery, allocation, logging, or
  // wait occurs in the callback. Each field group remains
  // sequential/non-coherent.
  __try {
    const volatile auto* input =
        reinterpret_cast<const volatile std::uint16_t*>(retail::kInputWords);
    record.input_current = input[0];
    record.input_previous = input[1];
    record.input_repeated = input[2];
    record.input_pressed_or_aux = input[3];
    record.input_released_or_flags = input[4];

    const auto player = *reinterpret_cast<const volatile std::uint32_t*>(
        retail::kPlayerPointer);
    record.player_pointer = player;
    record.player_state = INT32_MIN;
    record.player_x = 0.0f;
    record.player_y = 0.0f;
    if (bounded_game_address(player,
                             retail::kPlayerState + sizeof(std::int32_t))) {
      const auto* base = reinterpret_cast<const volatile std::uint8_t*>(player);
      record.player_state = *reinterpret_cast<const volatile std::int32_t*>(
          base + retail::kPlayerState);
      record.player_x =
          *reinterpret_cast<const volatile float*>(base + retail::kPlayerX);
      record.player_y =
          *reinterpret_cast<const volatile float*>(base + retail::kPlayerY);
    }
  } __except (read_fault_filter(GetExceptionInformation())) {
    record.player_pointer = 0;
    record.player_state = INT32_MIN;
    record.player_x = 0.0f;
    record.player_y = 0.0f;
    record.input_current = record.input_previous = record.input_repeated = 0;
    record.input_pressed_or_aux = record.input_released_or_flags = 0;
    record.flags |= kSampleFlagReadFault;
  }

  record.stage_tick = record.segment_tick = 0;
  __try {
    record.stage_tick =
        *reinterpret_cast<const volatile std::uint32_t*>(retail::kStageTick);
    record.segment_tick =
        *reinterpret_cast<const volatile std::uint32_t*>(retail::kSegmentTick);
  } __except (read_fault_filter(GetExceptionInformation())) {
    record.stage_tick = record.segment_tick = 0;
    record.flags |= kSampleFlagTickReadFault;
  }
  if (controller)
    controller->AfterCallback(callback_kind, callback_id, parent_callback_id,
                              {record.sequence, record.flags == 0});
  auto* step_gate = load_pointer(g_gate);
  const auto state = step_gate ? step_gate->state() : GateState::Disabled;
  if (state == GateState::Arming || state == GateState::Running)
    g_tick_verifier.observe(record);
  else if (callback_kind == kCallbackCalcPost &&
           (state == GateState::Disabled || state == GateState::Resetting) &&
           step_gate && step_gate->OwnsCurrentThread())
    publish_readiness_sample(record);
}

bool is_callback_entry(std::uint32_t kind) {
  return kind == kCallbackCalcPre || kind == kCallbackReplayPre ||
         kind == kCallbackPlayerPre;
}

CallbackScope* scope_for(std::uint32_t kind) {
  switch (kind) {
    case kCallbackCalcPre:
    case kCallbackCalcPost:
      return &g_scopes.calc;
    case kCallbackReplayPre:
    case kCallbackReplayPost:
      return &g_scopes.replay;
    case kCallbackPlayerPre:
    case kCallbackPlayerPost:
      return &g_scopes.player;
    default:
      return nullptr;
  }
}

// Open a scope for a callback entry. Returns false past the depth limit.
bool enter_scope(CallbackScope& scope, std::uint32_t parent, std::uint32_t& id,
                 std::uint32_t& flags) {
  if (scope.depth >= kScopeLimit) {
    ++scope.overflow;
    flags |= kSampleFlagScopeOverflow;
    return false;
  }
  const LONG next = InterlockedIncrement(&g_callback_id_sequence);
  id = next > 0 ? static_cast<std::uint32_t>(next) : 0;
  scope.ids[scope.depth] = id;
  scope.parents[scope.depth] = parent;
  ++scope.depth;
  if (!id) flags |= kSampleFlagNoCallbackId;
  return true;
}

// Find the scope a callback exit closes. Returns true if one should be popped
// after the exit has been sampled.
bool match_scope(CallbackScope& scope, std::uint32_t& id,
                 std::uint32_t& flags) {
  if (scope.overflow) {
    --scope.overflow;
    flags |= kSampleFlagNoCallbackId | kSampleFlagScopeOverflow;
    return false;
  }
  if (!scope.depth) {
    flags |= kSampleFlagNoCallbackId;
    return false;
  }
  id = scope.ids[scope.depth - 1];
  if (!id) flags |= kSampleFlagNoCallbackId;
  return true;
}

// Hand the finished tick to the gate and, if it halts, publish the snapshot.
void finish_controlled_tick(TickGate& gate, InputController* controller) {
  const bool complete_chain = g_tick_verifier.complete_chain();
  const auto state_before_tick = gate.state();
  const auto outcome = state_before_tick == GateState::Running
                           ? read_terminal_outcome()
                           : TerminalOutcome::None;
  const auto calc = g_tick_verifier.snapshot();
  const bool terminal_tick =
      outcome != TerminalOutcome::None &&
      g_tick_verifier.terminal_calc_pair() && controller != nullptr &&
      controller->CompleteTerminal(g_tick_verifier.callback_id(),
                                   calc.snapshot_sequence);
  const bool valid_tick = complete_chain || terminal_tick;
  if (g_tick_verifier.has_calc_post || g_tick_verifier.verification_failed)
    gate.NoteCalcPost(calc, valid_tick, g_tick_verifier.verification_failed);

  TickState state{};
  const bool state_read =
      controller != nullptr && controller->ReadTickState(state);
  GateTickState final_state{};
  if (state_read)
    final_state = gate_state_from(state, g_tick_verifier.has_calc_post
                                             ? g_tick_verifier.post_sequence()
                                             : 0);
  gate.AfterTick(final_state, state_read);

  if (!valid_tick || gate.state() != GateState::Halted) return;
  CallbackSample calc_post{};
  std::uint16_t consumed[5]{};
  if (terminal_tick ? g_tick_verifier.copy_terminal_calc(calc_post)
                    : g_tick_verifier.copy_tick_end(calc_post, consumed))
    publish_halted_snapshot(gate, state_before_tick == GateState::Arming,
                            calc_post, outcome,
                            terminal_tick ? nullptr : &consumed);
}

}  // namespace

// Called at the Calc, Replay and Player callbacks by the x86 wrappers.
void RecordCallback(void* context, std::uint32_t callback_kind,
                    std::int32_t /*dispatcher_result*/) {
  const DWORD previous_last_error = GetLastError();
  const bool is_calc =
      callback_kind == kCallbackCalcPre || callback_kind == kCallbackCalcPost;
  std::uint32_t callback_id = 0, parent_callback_id = g_scopes.current_calc;
  std::uint32_t flags = 0;
  CallbackScope* scope = scope_for(callback_kind);
  bool pop_scope = false;
  if (scope && is_callback_entry(callback_kind)) {
    if (enter_scope(*scope, parent_callback_id, callback_id, flags) && is_calc)
      g_scopes.current_calc = callback_id;
  } else if (scope) {
    pop_scope = match_scope(*scope, callback_id, flags);
    if (pop_scope && is_calc)
      parent_callback_id = scope->parents[scope->depth - 1];
  }
  sample_callback(context, callback_kind, callback_id, parent_callback_id,
                  flags);
  if (pop_scope) {
    --scope->depth;
    if (is_calc) g_scopes.current_calc = scope->parents[scope->depth];
  }
  SetLastError(previous_last_error);
}

extern "C" int WINAPI Th10BeforeTick() {
  const DWORD previous_last_error = GetLastError();
  auto* gate = load_pointer(g_gate);
  bool run_tick = true;
  g_tick_verifier.begin();
  if (gate != nullptr) {
    gate->BeginTick();
    run_tick = gate->BeforeTick();
    // The original tick contains Sleep(0) on its timing gate. A skipped
    // whole tick bypasses that yield, so pause the outer loop briefly.
    if (!run_tick) Sleep(1);
  }
  SetLastError(previous_last_error);
  return run_tick ? 1 : 0;
}

extern "C" void WINAPI Th10AfterTick() {
  const DWORD previous_last_error = GetLastError();
  auto* gate = load_pointer(g_gate);
  auto* controller = load_pointer(g_controller);
  if (gate != nullptr && (gate->state() == GateState::Arming ||
                          gate->state() == GateState::Running))
    finish_controlled_tick(*gate, controller);
  SetLastError(previous_last_error);
}

namespace {

// Own a new mapping until initialization succeeds, then keep it for the process
// lifetime.
template <typename T>
struct PendingMapping {
  HANDLE handle = nullptr;
  T* view = nullptr;

  PendingMapping() = default;
  PendingMapping(const PendingMapping&) = delete;
  PendingMapping& operator=(const PendingMapping&) = delete;

  ~PendingMapping() {
    if (view) UnmapViewOfFile(view);
    if (handle) CloseHandle(handle);
  }

  T* retain_for_process() {
    handle = nullptr;
    T* retained = view;
    view = nullptr;
    return retained;
  }
};

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI Th10InitializeHook(void*) {
  if (load_pointer(g_hook_status)) return ERROR_SUCCESS;
  if (!target_is_unpatched_game()) return ERROR_BAD_EXE_FORMAT;

  wchar_t name[96];
  const int chars = _snwprintf_s(name, _countof(name), _TRUNCATE,
                                 kHookStatusName, process_id());
  if (chars < 0) return ERROR_INVALID_NAME;

  PendingMapping<HookStatusMapping> status;
  constexpr DWORD status_size = sizeof(HookStatusMapping);
  status.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                     PAGE_READWRITE, 0, status_size, name);
  if (!status.handle || GetLastError() == ERROR_ALREADY_EXISTS)
    return GetLastError();
  status.view = static_cast<HookStatusMapping*>(MapViewOfFile(
      status.handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, status_size));
  if (!status.view) return GetLastError();

  const auto pid = process_id();
  status.view->magic = kHookStatusMagic;
  status.view->version = kHookStatusVersion;
  status.view->size = sizeof(HookStatusMapping);
  status.view->target_pid = pid;
  status.view->hook_install_state = kHookNotInstalled;

  std::uint32_t error = ERROR_SUCCESS;
  PendingMapping<InputChannelMapping> input;
  PendingMapping<GateChannelMapping> gate_channel;
  PendingMapping<SnapshotChannelMapping> snapshots;
  if (!CreateInputChannel(pid, &input.handle, &input.view, &error) ||
      !CreateGateChannel(pid, &gate_channel.handle, &gate_channel.view,
                         &error) ||
      !CreateSnapshotChannel(pid, &snapshots.handle, &snapshots.view, &error))
    return error;

  const int ready_chars =
      _snwprintf_s(name, _countof(name), _TRUNCATE,
                   L"Local\\TH10Gym.SnapshotReady.%lu", pid);
  if (ready_chars < 0) return ERROR_INVALID_NAME;
  std::unique_ptr<void, decltype(&CloseHandle)> ready(
      CreateEventW(nullptr, FALSE, FALSE, name), &CloseHandle);
  const DWORD ready_error = GetLastError();
  if (!ready || ready_error == ERROR_ALREADY_EXISTS) return ready_error;

  HMODULE pinned = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_PIN,
                          reinterpret_cast<LPCWSTR>(&Th10InitializeHook),
                          &pinned))
    return GetLastError();

  auto* input_view = input.retain_for_process();
  auto* controller = new (g_controller_storage)
      InputController(input_view, DefaultGameMemoryAccess());
  auto* tick_gate = new (g_gate_storage)
      TickGate(gate_channel.retain_for_process(), controller);
  InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_controller),
                             controller);
  InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_gate),
                             tick_gate);
  init_snapshot_capture(snapshots.retain_for_process(), input_view,
                        ready.release());

  // The installer waits for this export to finish before patching the call
  // sites.
  InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_hook_status),
                             status.retain_for_process());
  return ERROR_SUCCESS;
}
