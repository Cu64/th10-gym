#include "hook_loader.h"

#include <memory>

#include "remote_process.h"
#include "report.h"

namespace th10installer {
namespace {

constexpr DWORD kThreadTimeoutMs = 10000;

struct FreeRemotePath {
  HANDLE process;
  void operator()(void* address) const noexcept {
    VirtualFreeEx(process, address, 0, MEM_RELEASE);
  }
};

}  // namespace

bool remote_load(HANDLE p, DWORD loadFn, const std::wstring& dll, DWORD& module,
                 Report& r, ULONGLONG deadline) {
  if (!remaining_ms(deadline, kThreadTimeoutMs)) {
    r.error = "hook installation deadline expired before remote LoadLibraryW";
    step(r, "remote_LoadLibraryW", "timeout", r.error);
    return false;
  }
  const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
  std::unique_ptr<void, FreeRemotePath> remote(
      VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                     PAGE_READWRITE),
      FreeRemotePath{p});
  if (!remote) {
    r.error = hexerr(GetLastError());
    step(r, "allocate_remote_path", "failed", r.error);
    return false;
  }
  const auto free_path = [&] {
    if (!VirtualFreeEx(p, remote.get(), 0, MEM_RELEASE)) {
      const std::string error =
          "remote path cleanup failed: " + hexerr(GetLastError());
      step(r, "release_remote_path", "failed", error);
      r.error += (r.error.empty() ? "" : "; ") + error;
      return false;
    }
    remote.release();
    step(r, "release_remote_path", "ok");
    return true;
  };
  SIZE_T put = 0;
  const BOOL wrote =
      WriteProcessMemory(p, remote.get(), dll.c_str(), bytes, &put);
  if (!wrote || put != bytes) {
    r.error = hexerr(wrote ? ERROR_PARTIAL_COPY : GetLastError());
    step(r, "write_remote_path", "failed", r.error);
    free_path();
    return false;
  }
  if (!remaining_ms(deadline, kThreadTimeoutMs)) {
    r.error = "hook installation deadline expired before remote LoadLibraryW";
    step(r, "remote_LoadLibraryW", "timeout", r.error);
    free_path();
    return false;
  }
  auto thread = own_handle(CreateRemoteThread(
      p, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loadFn),
      remote.get(), 0, nullptr));
  if (!thread) {
    r.error = hexerr(GetLastError());
    step(r, "remote_LoadLibraryW", "failed", r.error);
    free_path();
    return false;
  }
  r.hook_load_state = "pending";
  DWORD code = 0, error = 0;
  const auto wait = wait_remote(
      thread.get(), remaining_ms(deadline, kThreadTimeoutMs), code, error);
  if (wait == RemoteWait::TimedOut || wait == RemoteWait::WaitFailed) {
    // A live thread may still read its argument after this handle is closed.
    remote.release();
    r.hook_load_state = "unknown";
    r.error =
        wait == RemoteWait::TimedOut
            ? "remote LoadLibraryW timed out"
            : "remote LoadLibraryW completion unconfirmed: " + hexerr(error);
    step(r, "remote_LoadLibraryW", "completion_unconfirmed_path_retained",
         r.error);
    return false;
  }
  if (wait == RemoteWait::ExitCodeFailed || code == 0) {
    r.hook_load_state =
        wait == RemoteWait::ExitCodeFailed ? "unknown" : "failed";
    r.error =
        wait == RemoteWait::ExitCodeFailed
            ? "remote LoadLibraryW exit code unavailable: " + hexerr(error)
            : "LoadLibraryW returned NULL";
    step(r, "remote_LoadLibraryW", "failed", r.error);
    free_path();
    return false;
  }
  module = code;
  r.hook_loaded = true;
  r.hook_load_state = "loaded";
  if (!free_path()) return false;
  step(r, "remote_LoadLibraryW", "ok",
       "module loaded; address returned by remote thread");
  return true;
}

}  // namespace th10installer
