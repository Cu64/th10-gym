#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" __declspec(dllexport) LONG WINAPI
Th10AtomicExchange32(volatile LONG* target, LONG value) noexcept {
  if (target == nullptr) return 0;
  return InterlockedExchange(target, value);
}

extern "C" __declspec(dllexport) LONG WINAPI
Th10AtomicLoad32(volatile LONG* target) noexcept {
  if (target == nullptr) return 0;
  return InterlockedCompareExchange(target, 0, 0);
}
