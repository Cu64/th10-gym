// Run the production patcher against a fixture process.
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "call_patcher.h"
#include "game_hooks.h"
#include "hook_loader.h"
#include "pe_file.h"
#include "remote_process.h"
#include "report.h"

using namespace th10installer;
using th10hook::retail::kHooks;

constexpr ULONGLONG kInstallTimeoutMs = 35000;

#include <string>
#include <thread>

namespace {
struct FixtureMetadata {
  DWORD version, size, call_site_rva[8], call_span, original_rva;
  DWORD replacement_rva, decoy_rva, shared_state_size, scenario;
  BYTE expected_call[8][5], reserved[4];
};
struct FixtureState {
  LONG magic;
  DWORD version, size;
  volatile LONG ready, active_calls, parked_workers, completed_calls;
  volatile LONG bad_returns, original_calls, replacement_calls, decoy_calls;
  volatile LONG worker_count, scenario;
  DWORD park_thread_id;
  volatile LONG remote_path_checked;
};
static_assert(sizeof(FixtureMetadata) == 108, "fixture metadata ABI mismatch");
static_assert(sizeof(FixtureState) == 60, "fixture state ABI mismatch");

unsigned patch_writes = 0;
BOOL WINAPI fail_third_patch_write(HANDLE process, void* address,
                                   const void* bytes, SIZE_T size,
                                   SIZE_T* written) {
  if (++patch_writes != 3)
    return WriteProcessMemory(process, address, bytes, size, written);
  // Change the remote bytes but simulate a failure with no usable byte count.
  SIZE_T changed = 0;
  if (!WriteProcessMemory(process, address, bytes, size, &changed) ||
      changed != size)
    return FALSE;
  *written = 0;
  SetLastError(ERROR_PARTIAL_COPY);
  return FALSE;
}

bool check(bool ok, const char* message) {
  if (!ok) std::fprintf(stderr, "FAIL: %s\n", message);
  return ok;
}

bool wait_ready(HANDLE mapping, FixtureState*& state) {
  state = static_cast<FixtureState*>(
      MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FixtureState)));
  if (!state) return false;
  for (int i = 0; i < 500 && !state->ready; ++i) Sleep(10);
  return state->ready && state->magic == 0x54464c31 && state->version == 1 &&
         state->size == sizeof(FixtureState);
}

bool run_case(const std::wstring& fixturePath, const wchar_t* mode) {
  std::fwprintf(stderr, L"CASE start: %s\n", mode && *mode ? mode : L"success");
  std::fflush(stderr);
  const std::wstring id = L"t" + std::to_wstring(GetTickCount()) + L"_" +
                          std::to_wstring(GetCurrentProcessId());
  std::wstring command =
      L"\"" + fixturePath + L"\" --id " + id + L" --workers 4";
  const bool inSpan = mode && wcscmp(mode, L"--park-in-span") == 0;
  const bool mismatch = mode && wcscmp(mode, L"--bytes-mismatch") == 0;
  if (inSpan || mismatch) command += L" " + std::wstring(mode);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(fixturePath.c_str(), &command[0], nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    std::fprintf(stderr, "CreateProcess fixture failed: %lu\n", GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  const std::wstring prefix = L"Local\\Th10LoaderFixture_" + id;
  HANDLE mapping = nullptr, runEvent = nullptr, stopEvent = nullptr;
  FixtureState* state = nullptr;
  bool ok = false;
  for (int i = 0; i < 500 && !mapping; ++i) {
    mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                               (prefix + L"_state").c_str());
    if (mapping) break;
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
      DWORD exit_code = 0;
      GetExitCodeProcess(pi.hProcess, &exit_code);
      std::fprintf(stderr, "fixture exited early with code %lu\n", exit_code);
      break;
    }
    Sleep(10);
  }
  if (!mapping || !wait_ready(mapping, state)) {
    std::fprintf(stderr, "fixture did not publish valid shared state\n");
    goto cleanup;
  }
  runEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, (prefix + L"_run").c_str());
  stopEvent =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, (prefix + L"_stop").c_str());
  if (!runEvent || !stopEvent) {
    std::fprintf(stderr, "fixture events unavailable\n");
    goto cleanup;
  }

  {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                     PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                                     PROCESS_CREATE_THREAD | SYNCHRONIZE,
                                 FALSE, pi.dwProcessId);
    if (!process) {
      std::fprintf(stderr, "OpenProcess fixture failed: %lu\n", GetLastError());
      goto cleanup;
    }
    std::vector<Module> modules;
    DWORD error = 0;
    if (!enumerate_modules(pi.dwProcessId, modules, error)) {
      std::fprintf(stderr, "fixture module inventory failed: %lu\n", error);
      CloseHandle(process);
      goto cleanup;
    }
    DWORD base = 0;
    for (const auto& module : modules) {
      if (eqi(module.path, fixturePath)) {
        base = module.base;
        break;
      }
    }
    std::vector<BYTE> image;
    std::string why;
    ExportInfo metadataExport{};
    if (!base || !load_bytes(fixturePath, image, error) ||
        !pe_export(image, "FixtureGetMetadata", metadataExport, why)) {
      std::fprintf(stderr, "fixture metadata export resolution failed: %s\n",
                   why.c_str());
      CloseHandle(process);
      goto cleanup;
    }
    HANDLE metadataThread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            static_cast<uintptr_t>(base + metadataExport.rva)),
        nullptr, 0, nullptr);
    DWORD remoteMetadata = 0, waitError = 0;
    if (!metadataThread || wait_remote(metadataThread, 5000, remoteMetadata,
                                       waitError) != RemoteWait::Completed) {
      std::fprintf(stderr, "fixture metadata call failed: %lu\n",
                   metadataThread ? waitError : GetLastError());
      if (metadataThread) CloseHandle(metadataThread);
      CloseHandle(process);
      goto cleanup;
    }
    CloseHandle(metadataThread);
    FixtureMetadata meta{};
    const auto& original_bytes = kHooks[0].original_bytes;
    if (!remoteMetadata ||
        !read_exact(process, remoteMetadata, &meta, sizeof(meta)) ||
        meta.version != 4 || meta.size != sizeof(meta) ||
        meta.call_span != sizeof(original_bytes) ||
        !std::all_of(std::begin(meta.expected_call),
                     std::end(meta.expected_call), [&](const auto& call) {
                       return std::memcmp(call, original_bytes,
                                          sizeof(original_bytes)) == 0;
                     })) {
      std::fprintf(stderr, "fixture metadata validation failed\n");
      CloseHandle(process);
      goto cleanup;
    }
    const DWORD site = base + meta.call_site_rva[0];
    const DWORD lastSite = base + meta.call_site_rva[7];
    if (mode && wcscmp(mode, L"--park-in-span") == 0) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_CONTROL;
      HANDLE parked = OpenThread(
          THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
          FALSE, state->park_thread_id);
      bool positioned =
          parked && GetThreadContext(parked, &c) && c.Eip == lastSite + 1;
      if (parked) CloseHandle(parked);
      if (!check(positioned,
                 "suspended worker EIP is explicitly set inside CALL span")) {
        CloseHandle(process);
        goto cleanup;
      }
    }
    Report report;
    report.pid = pi.dwProcessId;
    report.modules = std::move(modules);
    if (wcscmp(mode, L"--remote-timeout") == 0) {
      ExportInfo reader{};
      DWORD unused_module = 0;
      ok = check(pe_export(image, "FixtureReadPathAfterRelease", reader, why),
                 "remote timeout fixture export resolves");
      if (ok) {
        const bool loaded =
            remote_load(process, base + reader.rva, L"fixture remote path",
                        unused_module, report, GetTickCount64() + 500);
        ok = check(
            !loaded && std::strcmp(report.hook_load_state, "unknown") == 0 &&
                report.error.find("timed out") != std::string::npos,
            "loader returns its timeout report before the remote thread exits");
        SetEvent(runEvent);
        for (int i = 0; i < 500 && !state->remote_path_checked; ++i) Sleep(10);
        ok = check(state->remote_path_checked == 1,
                   "remote thread can still read its path after the timeout") &&
             ok;
      }
      CloseHandle(process);
      goto cleanup;
    }
    th10hook::HookStatusMapping localShared{};
    std::vector<PatchSpec> patchSpecs;
    patchSpecs.push_back({site, base + meta.replacement_rva,
                          meta.expected_call[0], "fixture CALL 0"});
    patchSpecs.push_back({base + meta.call_site_rva[1],
                          base + meta.replacement_rva, meta.expected_call[1],
                          "fixture CALL 1"});
    patchSpecs.push_back({base + meta.call_site_rva[2],
                          base + meta.replacement_rva, meta.expected_call[2],
                          "fixture CALL 2"});
    patchSpecs.push_back({base + meta.call_site_rva[3],
                          base + meta.replacement_rva, meta.expected_call[3],
                          "fixture CALL 3"});
    for (DWORD i = 4; i < 8; ++i)
      patchSpecs.push_back({base + meta.call_site_rva[i],
                            base + meta.replacement_rva, meta.expected_call[i],
                            "fixture CALL"});
    const bool writeFailure = wcscmp(mode, L"--write-failure") == 0;
    const bool expired = wcscmp(mode, L"--deadline-expired") == 0;
    DWORD original_protections[8]{};
    for (DWORD i = 0; i < 8; ++i) {
      MEMORY_BASIC_INFORMATION region{};
      VirtualQueryEx(process,
                     reinterpret_cast<void*>(base + meta.call_site_rva[i]),
                     &region, sizeof(region));
      original_protections[i] = region.Protect;
    }
    patch_writes = 0;
    const bool patched = debug_patch_sites(
        {process, pi.dwProcessId,
         GetTickCount64() + (expired ? 0 : kInstallTimeoutMs)},
        patchSpecs, &localShared, report,
        writeFailure ? fail_third_patch_write : WriteProcessMemory);
    std::fprintf(stderr,
                 "CASE engine returned: patched=%d attached=%d error=%s\n",
                 patched ? 1 : 0, report.debugger_attached ? 1 : 0,
                 report.error.c_str());
    std::fflush(stderr);
    if (writeFailure || expired) {
      ok = check(
          !patched && !report.debugger_attached && !report.patch_applied &&
              localShared.hook_install_state == th10hook::kHookNotInstalled,
          "loader failure leaves hooks uninstalled and no debugger attached");
      ok = check(writeFailure ? patch_writes == 3 &&
                                    report.error.find("WriteProcessMemory") !=
                                        std::string::npos
                              : report.error.find("deadline expired") !=
                                    std::string::npos,
                 "loader preserves the original failure reason") &&
           ok;
      for (DWORD i = 0; i < 8; ++i) {
        const DWORD address = base + meta.call_site_rva[i];
        BYTE actual[5]{};
        MEMORY_BASIC_INFORMATION region{};
        const bool restored =
            read_exact(process, address, actual, sizeof(actual)) &&
            std::memcmp(actual, meta.expected_call[i], sizeof(actual)) == 0 &&
            VirtualQueryEx(process, reinterpret_cast<void*>(address), &region,
                           sizeof(region)) &&
            region.Protect == original_protections[i];
        ok = check(restored,
                   "original CALL bytes and protection remain intact") &&
             ok;
      }
      SetEvent(runEvent);
      for (int i = 0; i < 500 && !state->completed_calls; ++i) Sleep(10);
      ok = check(state->completed_calls > 0 && state->replacement_calls == 0 &&
                     state->bad_returns == 0,
                 "fixture resumes and executes only the original CALLs") &&
           ok;
    } else if (inSpan) {
      ok = check(!patched && !report.debugger_attached &&
                     !report.patch_applied &&
                     report.error.find("inside 5-byte patch span") !=
                         std::string::npos,
                 "engine refuses EIP-in-span and detaches");
    } else if (mismatch) {
      ok = check(
          !patched && !report.debugger_attached && !report.patch_applied &&
              report.error.find("mapped CALL bytes") != std::string::npos,
          "engine refuses any changed last CALL before patching all "
          "eight sites");
      for (DWORD i = 0; i < 8; ++i) {
        BYTE actual[5]{};
        BYTE before[5];
        std::memcpy(before, meta.expected_call[i], sizeof(before));
        if (i == 7) {
          const DWORD rel =
              base + meta.decoy_rva - (base + meta.call_site_rva[i] + 5);
          before[0] = 0xe8;
          std::memcpy(before + 1, &rel, sizeof(rel));
        }
        const bool unchanged = read_exact(process, base + meta.call_site_rva[i],
                                          actual, sizeof(actual)) &&
                               std::memcmp(actual, before, sizeof(actual)) == 0;
        ok = check(unchanged,
                   "Changed-bytes refusal leaves every CALL site unchanged") &&
             ok;
      }
      SetEvent(runEvent);
      for (int i = 0; i < 500 && !state->decoy_calls; ++i) Sleep(10);
      ok =
          ok && check(state->decoy_calls > 0 && state->replacement_calls == 0 &&
                          state->bad_returns == 0,
                      "fixture continues on original decoy path after refusal");
    } else {
      ok = check(
          patched && report.installed && !report.debugger_attached &&
              report.patch_applied &&
              localShared.hook_install_state == th10hook::kHookInstalled,
          "same engine patches, publishes state, and detaches successfully");
      if (ok) {
        for (DWORD i = 0; i < 8; ++i) {
          BYTE actual[5]{};
          BYTE expected[5] = {0xe8, 0, 0, 0, 0};
          const DWORD patchSite = base + meta.call_site_rva[i];
          const DWORD rel = base + meta.replacement_rva - (patchSite + 5);
          std::memcpy(expected + 1, &rel, sizeof(rel));
          const bool replaced =
              read_exact(process, patchSite, actual, sizeof(actual)) &&
              std::memcmp(actual, expected, sizeof(actual)) == 0;
          ok = check(replaced,
                     "all eight CALL sites match their replacement bytes") &&
               ok;
        }
      }
      SetEvent(runEvent);
      for (int i = 0; i < 500 && !state->replacement_calls; ++i) Sleep(10);
      ok = ok &&
           check(state->replacement_calls > 0 && state->original_calls > 0 &&
                     state->completed_calls > 0 && state->bad_returns == 0,
                 "fixture continues through replacement and original after "
                 "detach");
    }
    if (!ok)
      for (const auto& s : report.steps)
        std::fprintf(stderr, "%s %s: %s\n", s.name.c_str(), s.status.c_str(),
                     s.detail.c_str());
    CloseHandle(process);
  }
cleanup:
  if (stopEvent) SetEvent(stopEvent);
  if (pi.hProcess && WaitForSingleObject(pi.hProcess, 5000) != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 99);
    WaitForSingleObject(pi.hProcess, 2000);
    ok = false;
  }
  if (state) UnmapViewOfFile(state);
  if (mapping) CloseHandle(mapping);
  if (runEvent) CloseHandle(runEvent);
  if (stopEvent) CloseHandle(stopEvent);
  CloseHandle(pi.hProcess);
  return ok;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::fwprintf(
        stderr,
        L"usage: loader_patch_fixture_tests.exe <loader_fixture.exe>\n");
    return 2;
  }
  const std::wstring path = argv[1];
  std::fwprintf(stderr, L"fixture path: %s\n", path.c_str());
  std::fflush(stderr);
  bool ok = run_case(path, L"");
  ok = run_case(path, L"--bytes-mismatch") && ok;
  ok = run_case(path, L"--park-in-span") && ok;
  ok = run_case(path, L"--write-failure") && ok;
  ok = run_case(path, L"--deadline-expired") && ok;
  ok = run_case(path, L"--remote-timeout") && ok;
  std::wprintf(ok ? L"All real debug_patch_sites fixture cases passed.\n"
                  : L"Fixture patch tests failed.\n");
  return ok ? 0 : 1;
}
