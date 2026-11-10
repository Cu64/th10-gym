#include "game_memory.h"

namespace th10hook {

bool bounded_game_address(std::uint32_t address, std::size_t length) {
  return address >= 0x00400000 && length <= 0x100000000ull - address;
}

bool bounded_replay_manager_address(std::uint32_t address, std::size_t length) {
  return address >= 0x00010000 && length <= 0x7FFF0000ull &&
         address <= 0x7FFF0000ull - length;
}

int read_fault_filter(EXCEPTION_POINTERS* exception) {
  if (exception == nullptr || exception->ExceptionRecord == nullptr)
    return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = exception->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR)
    return EXCEPTION_CONTINUE_SEARCH;
  const auto& record = *exception->ExceptionRecord;
  // ExceptionInformation[0] is the access kind: zero means read. Do not mask
  // write/execute faults or unrelated exceptions as a recoverable snapshot
  // miss.
  return record.NumberParameters >= 2 && record.ExceptionInformation[0] == 0
             ? EXCEPTION_EXECUTE_HANDLER
             : EXCEPTION_CONTINUE_SEARCH;
}

bool guarded_read(std::uintptr_t address, void* output, std::size_t length) {
  if (!bounded_game_address(static_cast<std::uint32_t>(address), length) ||
      output == nullptr)
    return false;
  __try {
    auto* dst = static_cast<std::uint8_t*>(output);
    const volatile auto* src =
        reinterpret_cast<const volatile std::uint8_t*>(address);
    for (std::size_t i = 0; i < length; ++i) dst[i] = src[i];
    return true;
  } __except (read_fault_filter(GetExceptionInformation())) {
    return false;
  }
}

}  // namespace th10hook
