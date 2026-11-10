// Load the hook DLL into the target with a remote LoadLibraryW thread.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

#include "report.h"

namespace th10installer {

bool remote_load(HANDLE p, DWORD loadFn, const std::wstring& dll, DWORD& module,
                 Report& r, ULONGLONG deadline);

}  // namespace th10installer
