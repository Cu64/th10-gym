// Standalone x86 debug-patching fixture. This translation unit is deliberately
// unavailable unless built as its own fixture executable; it is not a
// production-loader switch and must never be linked into the hook DLL.
#if !defined(_M_IX86)
#error Build the fixture with the x86 toolchain.
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

static_assert(sizeof(void*) == 4, "fixture requires a 32-bit process");

namespace {
constexpr LONG kStateMagic = 0x54464c31;  // "TFL1"
constexpr DWORD kMetadataVersion = 4;
constexpr DWORD kStateVersion = 1;
constexpr DWORD kPatchSiteCount = 8;
constexpr int kExpectedReturn = 0x4a31;
constexpr DWORD kMaxWorkers = 32;

struct SharedState {
  LONG magic;
  DWORD version;
  DWORD size;
  volatile LONG ready;
  volatile LONG active_calls;
  volatile LONG parked_workers;
  volatile LONG completed_calls;
  volatile LONG bad_returns;
  volatile LONG original_calls;
  volatile LONG replacement_calls;
  volatile LONG decoy_calls;
  volatile LONG worker_count;
  volatile LONG scenario;  // 0=normal, 1=last original_bytes mismatch,
                           // 2=last-site EIP-in-span
  DWORD park_thread_id;
  volatile LONG remote_path_checked;
};

// Fixed-width values and RVAs make this record inspectable from another x86
// process without sharing a C++ object layout or assuming an absolute image
// base. The expected five bytes always describe CALL FixtureOriginal.
struct FixtureMetadata {
  DWORD version;
  DWORD size;
  DWORD call_site_rva[kPatchSiteCount];
  DWORD call_span;
  DWORD original_rva;
  DWORD replacement_rva;
  DWORD decoy_rva;
  DWORD shared_state_size;
  DWORD scenario;
  BYTE expected_call[kPatchSiteCount][5];
  BYTE reserved[4];
};
static_assert(sizeof(FixtureMetadata) == 108, "fixture metadata ABI changed");

SharedState* g_state = nullptr;
HANDLE g_run_event = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_worker_handles[kMaxWorkers]{};
DWORD g_worker_count = 0;
HANDLE g_parked_thread = nullptr;
void* g_compat_trampoline = nullptr;
BYTE* g_call_site = nullptr;
BYTE* g_call_sites[kPatchSiteCount]{};

bool safe_id(const wchar_t* value) {
  if (!value || !*value || wcslen(value) > 48) return false;
  for (const wchar_t* p = value; *p; ++p) {
    const wchar_t c = *p;
    if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
          (c >= L'0' && c <= L'9') || c == L'-' || c == L'_'))
      return false;
  }
  return true;
}

bool make_object_names(const wchar_t* id, wchar_t* state, size_t state_n,
                       wchar_t* run, size_t run_n, wchar_t* stop,
                       size_t stop_n) {
  const int a = _snwprintf_s(state, state_n, _TRUNCATE,
                             L"Local\\Th10LoaderFixture_%s_state", id);
  const int b = _snwprintf_s(run, run_n, _TRUNCATE,
                             L"Local\\Th10LoaderFixture_%s_run", id);
  const int c = _snwprintf_s(stop, stop_n, _TRUNCATE,
                             L"Local\\Th10LoaderFixture_%s_stop", id);
  return a >= 0 && b >= 0 && c >= 0;
}

extern "C" __declspec(noinline) int __stdcall FixtureOriginal();
extern "C" __declspec(noinline) int __stdcall FixtureReplacement();
extern "C" __declspec(noinline) int __stdcall FixtureDecoy();

extern "C" __declspec(noinline) int __stdcall FixtureOriginal() {
  if (g_state) InterlockedIncrement(&g_state->original_calls);
  return kExpectedReturn;
}

extern "C" __declspec(noinline) int __stdcall FixtureReplacement() {
  if (g_state) InterlockedIncrement(&g_state->replacement_calls);
  return FixtureOriginal();
}

extern "C" __declspec(noinline) int __stdcall FixtureDecoy() {
  if (g_state) InterlockedIncrement(&g_state->decoy_calls);
  return kExpectedReturn;
}

DWORD rva_of(const void* address) {
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  const auto value = reinterpret_cast<std::uintptr_t>(address);
  if (value < base || value - base > MAXDWORD) return 0;
  return static_cast<DWORD>(value - base);
}

bool write_call_target(void* site, const void* target) {
  auto* bytes = static_cast<BYTE*>(site);
  if (bytes[0] != 0xe8) return false;
  const auto next = reinterpret_cast<std::uintptr_t>(site) + 5;
  const auto dest = reinterpret_cast<std::uintptr_t>(target);
  const auto displacement = static_cast<std::uint32_t>(dest - next);
  DWORD old_protect = 0;
  if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old_protect))
    return false;
  std::memcpy(bytes + 1, &displacement, sizeof(displacement));
  const BOOL flushed = FlushInstructionCache(GetCurrentProcess(), site, 5);
  DWORD ignored = 0;
  const BOOL restored = VirtualProtect(site, 5, old_protect, &ignored);
  return flushed && restored;
}

bool prepare_compatible_call(void* site) {
  // Put the seam and its fixed-displacement trampoline in one allocation
  // beyond the image, so the test never depends on unused gaps in the EXE.
  const auto image =
      reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
  const auto* nt =
      reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    return false;
  const std::uintptr_t imageEnd = image + nt->OptionalHeader.SizeOfImage;
  // With ASLR, the first address past the image is sometimes taken, so scan
  // upward. An arbitrary allocation could land below the image.
  constexpr std::uintptr_t kAllocationGranularity = 0x10000;
  constexpr DWORD kMaxAttempts = 4096;
  const std::uintptr_t first =
      (imageEnd + 0xffff) & ~static_cast<std::uintptr_t>(0xffff);
  for (DWORD i = 0; i < kMaxAttempts && !g_compat_trampoline; ++i)
    g_compat_trampoline = VirtualAlloc(
        reinterpret_cast<void*>(first + i * kAllocationGranularity), 0x20000,
        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (!g_compat_trampoline) return false;
  g_call_site = static_cast<BYTE*>(g_compat_trampoline);
  for (DWORD i = 0; i < kPatchSiteCount; ++i)
    g_call_sites[i] = g_call_site + 0x100 + i * 0x100;
  site = g_call_sites[0];
  std::memset(g_call_site, 0x90, 0x20000);
  auto* bytes = static_cast<BYTE*>(site);
  const DWORD original =
      static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(FixtureOriginal));
  for (DWORD i = 0; i < kPatchSiteCount; ++i) {
    const std::uintptr_t target =
        reinterpret_cast<std::uintptr_t>(g_call_sites[i]) + 5 + 0x107cc;
    auto* thunk = reinterpret_cast<BYTE*>(target);
    thunk[0] = 0xb8;  // mov eax, FixtureOriginal
    std::memcpy(thunk + 1, &original, sizeof(original));
    thunk[5] = 0xff;
    thunk[6] = 0xe0;  // jmp eax
    if (!FlushInstructionCache(GetCurrentProcess(), thunk, 7)) return false;
  }
  DWORD old_protect = 0;
  if (!VirtualProtect(g_compat_trampoline, 0x20000, PAGE_EXECUTE_READ,
                      &old_protect))
    return false;
  const BYTE expected[5] = {0xe8, 0xcc, 0x07, 0x01, 0x00};
  DWORD prior = 0;
  if (!VirtualProtect(g_compat_trampoline, 0x20000, PAGE_EXECUTE_READWRITE,
                      &prior))
    return false;
  std::memcpy(site, expected, sizeof(expected));
  bytes[5] =
      0xc3;  // return to the fixture worker after FixtureOriginal returns
  for (DWORD i = 1; i < kPatchSiteCount; ++i) {
    std::memcpy(g_call_sites[i], expected, sizeof(expected));
    g_call_sites[i][5] = 0xc3;
  }
  const BOOL flushed =
      FlushInstructionCache(GetCurrentProcess(), g_compat_trampoline, 0x500);
  DWORD ignored = 0;
  const BOOL restored =
      VirtualProtect(g_compat_trampoline, 0x20000, PAGE_EXECUTE_READ, &ignored);
  return flushed && restored;
}

DWORD WINAPI worker_main(void*) {
  HANDLE waits[2] = {g_stop_event, g_run_event};
  for (;;) {
    InterlockedIncrement(&g_state->parked_workers);
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    InterlockedDecrement(&g_state->parked_workers);
    if (wait == WAIT_OBJECT_0) break;
    if (wait != WAIT_OBJECT_0 + 1) {
      InterlockedIncrement(&g_state->bad_returns);
      break;
    }

    InterlockedIncrement(&g_state->active_calls);
    int result = kExpectedReturn;
    for (DWORD i = 0; i < kPatchSiteCount; ++i) {
      const auto call = reinterpret_cast<int(__cdecl*)()>(g_call_sites[i]);
      result = call();
    }
    InterlockedDecrement(&g_state->active_calls);
    InterlockedIncrement(&g_state->completed_calls);
    if (result != kExpectedReturn) InterlockedIncrement(&g_state->bad_returns);
    YieldProcessor();
  }
  return 0;
}

BOOL WINAPI console_handler(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
      event == CTRL_CLOSE_EVENT) {
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_run_event) SetEvent(g_run_event);
    return TRUE;
  }
  return FALSE;
}

void close_fixture_handles() {
  if (g_stop_event) SetEvent(g_stop_event);
  if (g_run_event) SetEvent(g_run_event);
  if (g_worker_count)
    WaitForMultipleObjects(g_worker_count, g_worker_handles, TRUE, 5000);
  for (DWORD i = 0; i < g_worker_count; ++i) {
    if (g_worker_handles[i]) CloseHandle(g_worker_handles[i]);
  }
  if (g_parked_thread) CloseHandle(g_parked_thread);
  if (g_run_event) CloseHandle(g_run_event);
  if (g_stop_event) CloseHandle(g_stop_event);
}

bool parse_workers(const wchar_t* arg, DWORD* count) {
  if (!arg || !*arg) return false;
  wchar_t* end = nullptr;
  const unsigned long value = wcstoul(arg, &end, 10);
  if (!end || *end || value == 0 || value > kMaxWorkers) return false;
  *count = static_cast<DWORD>(value);
  return true;
}

}  // namespace

extern "C" DWORD WINAPI FixtureReadPathAfterRelease(void* argument) {
  if (WaitForSingleObject(g_run_event, 10000) != WAIT_OBJECT_0)
    return ERROR_TIMEOUT;
  const bool correct = wcscmp(static_cast<const wchar_t*>(argument),
                              L"fixture remote path") == 0;
  InterlockedExchange(&g_state->remote_path_checked, correct ? 1 : -1);
  return correct ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

extern "C" __declspec(dllexport) DWORD WINAPI FixtureGetMetadata(void*) {
  static FixtureMetadata metadata{};
  static bool initialized = false;
  if (!initialized) {
    auto* site = g_call_site ? g_call_site + 0x100 : nullptr;
    if (!site) return 0;
    metadata.version = kMetadataVersion;
    metadata.size = sizeof(metadata);
    for (DWORD i = 0; i < kPatchSiteCount; ++i)
      metadata.call_site_rva[i] = rva_of(g_call_sites[i]);
    metadata.call_span = 5;
    metadata.original_rva =
        rva_of(reinterpret_cast<const void*>(FixtureOriginal));
    metadata.replacement_rva =
        rva_of(reinterpret_cast<const void*>(FixtureReplacement));
    metadata.decoy_rva = rva_of(reinterpret_cast<const void*>(FixtureDecoy));
    metadata.shared_state_size = sizeof(SharedState);
    metadata.scenario = g_state ? static_cast<DWORD>(g_state->scenario) : 0;
    const BYTE expected[5] = {0xe8, 0xcc, 0x07, 0x01, 0x00};
    for (DWORD i = 0; i < kPatchSiteCount; ++i)
      std::memcpy(metadata.expected_call[i], expected, sizeof(expected));
    initialized = true;
  }
  return static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(&metadata));
}

int wmain(int argc, wchar_t** argv) {
  const wchar_t* id = nullptr;
  DWORD workers = 4;
  bool mismatch = false;
  bool park_in_span = false;
  for (int i = 1; i < argc; ++i) {
    if (wcscmp(argv[i], L"--id") == 0 && i + 1 < argc)
      id = argv[++i];
    else if (wcscmp(argv[i], L"--workers") == 0 && i + 1 < argc) {
      if (!parse_workers(argv[++i], &workers)) return 2;
    } else if (wcscmp(argv[i], L"--bytes-mismatch") == 0)
      mismatch = true;
    else if (wcscmp(argv[i], L"--park-in-span") == 0)
      park_in_span = true;
    else
      return 2;
  }
  if (!safe_id(id)) {
    std::fwprintf(stderr,
                  L"Usage: loader_fixture.exe --id TOKEN [--workers 1..32] "
                  L"[--bytes-mismatch] [--park-in-span]\n");
    return 2;
  }

  wchar_t state_name[128], run_name[128], stop_name[128];
  if (!make_object_names(id, state_name, _countof(state_name), run_name,
                         _countof(run_name), stop_name, _countof(stop_name)))
    return 2;
  HANDLE mapping =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         sizeof(SharedState), state_name);
  if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) {
    if (mapping) CloseHandle(mapping);
    std::fwprintf(stderr, L"Could not create unique fixture mapping.\n");
    return 3;
  }
  g_state = static_cast<SharedState*>(
      MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
  g_run_event = CreateEventW(nullptr, TRUE, FALSE, run_name);
  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, stop_name);
  if (!g_state || !g_run_event || !g_stop_event ||
      GetLastError() == ERROR_ALREADY_EXISTS) {
    std::fwprintf(stderr, L"Could not create fixture state/events.\n");
    if (g_state) UnmapViewOfFile(g_state);
    if (mapping) CloseHandle(mapping);
    close_fixture_handles();
    return 3;
  }
  std::memset(g_state, 0, sizeof(*g_state));
  g_state->magic = kStateMagic;
  g_state->version = kStateVersion;
  g_state->size = sizeof(*g_state);
  g_state->scenario = mismatch ? 1 : (park_in_span ? 2 : 0);

  if (!prepare_compatible_call(nullptr)) {
    std::fwprintf(stderr, L"Compiler did not emit the expected CALL seam.\n");
    UnmapViewOfFile(g_state);
    CloseHandle(mapping);
    close_fixture_handles();
    return 4;
  }
  auto* site = g_call_sites[0];
  if (mismatch &&
      !write_call_target(g_call_sites[kPatchSiteCount - 1],
                         reinterpret_cast<const void*>(FixtureDecoy))) {
    std::fwprintf(stderr,
                  L"Could not prepare deliberate original_bytes mismatch.\n");
    UnmapViewOfFile(g_state);
    CloseHandle(mapping);
    close_fixture_handles();
    return 4;
  }

  SetConsoleCtrlHandler(console_handler, TRUE);
  g_worker_count = workers;
  g_state->worker_count = workers;
  for (DWORD i = 0; i < workers; ++i) {
    g_worker_handles[i] =
        CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
    if (!g_worker_handles[i]) {
      SetEvent(g_stop_event);
      g_worker_count = i;
      close_fixture_handles();
      UnmapViewOfFile(g_state);
      CloseHandle(mapping);
      return 5;
    }
  }
  if (park_in_span) {
    auto* parked_site = g_call_sites[kPatchSiteCount - 1];
    auto start = reinterpret_cast<LPTHREAD_START_ROUTINE>(parked_site + 1);
    g_parked_thread = CreateThread(nullptr, 0, start, nullptr, CREATE_SUSPENDED,
                                   &g_state->park_thread_id);
    if (!g_parked_thread) {
      close_fixture_handles();
      UnmapViewOfFile(g_state);
      CloseHandle(mapping);
      return 5;
    }
    CONTEXT parked{};
    parked.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(g_parked_thread, &parked)) {
      close_fixture_handles();
      UnmapViewOfFile(g_state);
      CloseHandle(mapping);
      return 5;
    }
    parked.Eip =
        static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(parked_site + 1));
    if (!SetThreadContext(g_parked_thread, &parked)) {
      close_fixture_handles();
      UnmapViewOfFile(g_state);
      CloseHandle(mapping);
      return 5;
    }
  }

  InterlockedExchange(&g_state->ready, 1);
  const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  std::wprintf(
      L"LOADER_FIXTURE_READY pid=%lu base=%08lX site_rva=%08lX span=5 "
      L"scenario=%ld mapping=%s run=%s stop=%s\n",
      GetCurrentProcessId(), static_cast<unsigned long>(base),
      static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(site) - base),
      g_state->scenario, state_name, run_name, stop_name);
  std::fflush(stdout);

  WaitForSingleObject(g_stop_event, INFINITE);
  close_fixture_handles();
  UnmapViewOfFile(g_state);
  CloseHandle(mapping);
  return 0;
}
