// Redirect the game's CALL sites to the hook wrappers while it is stopped under
// a debugger.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>

#include "hook_status.h"
#include "report.h"

namespace th10installer {

// The owning handle stays in the installer for the duration of the operation.
struct InstallTarget {
  HANDLE process;
  DWORD pid;
  ULONGLONG deadline;
};

struct PatchSpec {
  DWORD site, wrapper;
  const BYTE* expected;
  const char* name;
};

// Attach, verify every site, write all patches, publish the installed state,
// and detach. Any failure restores the original bytes and protections.
bool debug_patch_sites(
    InstallTarget target, const std::vector<PatchSpec>& specs,
    th10hook::HookStatusMapping* shared, Report& r,
    decltype(&WriteProcessMemory) write_patch = WriteProcessMemory);

}  // namespace th10installer
