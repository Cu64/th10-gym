// Parse the hook DLL from disk without loading it locally.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

namespace th10installer {

struct ExportInfo {
  DWORD rva = 0, imageSize = 0, preferredBase = 0;
  WORD fileCharacteristics = 0, dllCharacteristics = 0;
  std::vector<BYTE> bytes;  // The export's first file-backed code bytes.
};

bool load_bytes(const std::wstring& path, std::vector<BYTE>& b, DWORD& err);
bool pe_export(const std::vector<BYTE>& b, const char* name, ExportInfo& result,
               std::string& why);

}  // namespace th10installer
