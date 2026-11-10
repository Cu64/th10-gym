#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace th10 {

using Sha256Hex = char[65];

// Writes a null-terminated lowercase digest on success. Returns ERROR_SUCCESS,
// a Win32 error, or a BCrypt NTSTATUS preserved as a DWORD.
DWORD sha256_file_hex(const wchar_t* path, Sha256Hex& hex);

}  // namespace th10
