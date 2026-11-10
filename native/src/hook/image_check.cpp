#include "image_check.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "file_sha256.h"
#include "game_hooks.h"

namespace th10hook {
namespace {

using namespace retail;

bool readable_span(std::uintptr_t address, std::size_t length) {
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(reinterpret_cast<const void*>(address), &info,
                   sizeof(info)) != sizeof(info))
    return false;
  const DWORD protection = info.Protect & 0xff;
  const bool readable =
      protection == PAGE_READONLY || protection == PAGE_READWRITE ||
      protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
      protection == PAGE_EXECUTE_READWRITE ||
      protection == PAGE_EXECUTE_WRITECOPY;
  const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
  const auto end = start + info.RegionSize;
  return info.State == MEM_COMMIT && readable && !(info.Protect & PAGE_GUARD) &&
         address >= start && address <= end && length <= end - address;
}

bool matches_mapped_bytes(std::uintptr_t address, const std::uint8_t* expected,
                          std::size_t length) {
  return readable_span(address, length) &&
         std::memcmp(reinterpret_cast<const void*>(address), expected,
                     length) == 0;
}

}  // namespace

bool target_is_unpatched_game() {
  auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
  if (reinterpret_cast<std::uintptr_t>(base) != kImageBase) return false;
  if (!readable_span(kImageBase, sizeof(IMAGE_DOS_HEADER))) return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
      dos->e_lfanew > 0x100000 ||
      !readable_span(kImageBase + static_cast<std::uintptr_t>(dos->e_lfanew),
                     sizeof(IMAGE_NT_HEADERS32)))
    return false;
  const auto* nt =
      reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
      nt->OptionalHeader.ImageBase != kImageBase ||
      static_cast<std::uint32_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) >
          nt->OptionalHeader.SizeOfImage ||
      kPlayerAdapterRva + sizeof(kPlayerAdapterBytes) >
          nt->OptionalHeader.SizeOfImage)
    return false;
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return false;
  th10::Sha256Hex actual;
  if (th10::sha256_file_hex(path, actual) != ERROR_SUCCESS ||
      std::strcmp(actual, kExecutableSha256) != 0 ||
      !matches_mapped_bytes(kImageBase + kPlayerAdapterRva, kPlayerAdapterBytes,
                            sizeof(kPlayerAdapterBytes)))
    return false;
  for (const auto& hook : kHooks) {
    if (hook.rva + sizeof(hook.original_bytes) >
            nt->OptionalHeader.SizeOfImage ||
        !matches_mapped_bytes(kImageBase + hook.rva, hook.original_bytes,
                              sizeof(hook.original_bytes)))
      return false;
  }
  return true;
}

}  // namespace th10hook
