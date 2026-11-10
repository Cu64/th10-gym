// Read-only inspection of the target process, plus small shared helpers.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

#include "report.h"

namespace th10installer {

struct CloseWinHandle {
  void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<void, CloseWinHandle>;
UniqueHandle own_handle(HANDLE handle);

struct UnmapView {
  void operator()(const void* view) const noexcept { UnmapViewOfFile(view); }
};

// Milliseconds left before `deadline`, capped at `limit`.
DWORD remaining_ms(ULONGLONG deadline, DWORD limit);

bool eqi(const std::wstring& a, const std::wstring& b);
std::wstring base_name(const std::wstring& p);
bool read_exact(HANDLE p, DWORD address, void* out, SIZE_T n);

using ModuleEnumerator = bool (*)(DWORD, std::vector<Module>&, DWORD&);
using RetryDelay = void (*)(DWORD);
bool enumerate_modules_once(DWORD pid, std::vector<Module>& out, DWORD& err);
void module_snapshot_retry_delay(DWORD milliseconds);
// Toolhelp can fail transiently while modules load, so retry a bounded number
// of times.
bool enumerate_modules(DWORD pid, std::vector<Module>& out, DWORD& err,
                       ModuleEnumerator enumerateOnce = enumerate_modules_once,
                       RetryDelay delay = module_snapshot_retry_delay);

bool read_remote_pe(HANDLE p, DWORD base, DWORD& imageSize,
                    DWORD& preferredBase);
bool remote_readable_exec(HANDLE p, DWORD address, SIZE_T len);
bool remote_image_range_free(HANDLE p, DWORD base, DWORD imageSize,
                             std::string& why);
DWORD remote_module_by_name(const std::vector<Module>& m,
                            const std::wstring& name, DWORD* size = nullptr);
// Find where a kernel32 export actually lives, following forwarding, and map
// its owner-relative offset into the target's copy of that module.
bool local_export_owner(const char* symbol, DWORD& remoteFn,
                        const std::vector<Module>& mods, std::string& why);

enum class RemoteWait { Completed, TimedOut, WaitFailed, ExitCodeFailed };
RemoteWait wait_remote(HANDLE thread, DWORD timeout, DWORD& code, DWORD& err);

}  // namespace th10installer
