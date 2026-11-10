#define WIN32_LEAN_AND_MEAN
#include "snapshot_format.h"
#include <windows.h>
#include <climits>
#include <cstdio>
#include <cstring>

namespace th10hook {

bool CreateSnapshotChannel(std::uint32_t pid, void** mapping_handle,
                           SnapshotChannelMapping** mapping_view,
                           std::uint32_t* win32_error) {
  if (!pid || !mapping_handle || !mapping_view || !win32_error) return false;
  *mapping_handle = nullptr;
  *mapping_view = nullptr;
  *win32_error = ERROR_INVALID_PARAMETER;
  wchar_t name[112]{};
  const int chars = _snwprintf_s(name, sizeof(name) / sizeof(name[0]),
                                 _TRUNCATE, L"Local\\TH10Gym.Snapshot.%lu",
                                 static_cast<unsigned long>(pid));
  if (chars < 0) {
    *win32_error = ERROR_INVALID_NAME;
    return false;
  }
  HANDLE handle =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         sizeof(SnapshotChannelMapping), name);
  if (!handle) {
    *win32_error = GetLastError();
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(handle);
    *win32_error = ERROR_ALREADY_EXISTS;
    return false;
  }
  auto* view = static_cast<SnapshotChannelMapping*>(
      MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                    sizeof(SnapshotChannelMapping)));
  if (!view) {
    *win32_error = GetLastError();
    CloseHandle(handle);
    return false;
  }
  std::memset(view, 0, sizeof(*view));
  view->header.magic = SnapshotMagic;
  view->header.version = SnapshotVersion;
  view->header.header_size = sizeof(SnapshotHeader);
  view->header.record_size = sizeof(SnapshotRecord);
  view->header.target_pid = pid;
  view->header.capacity = 1;
  MemoryBarrier();
  *mapping_handle = handle;
  *mapping_view = view;
  *win32_error = ERROR_SUCCESS;
  return true;
}

bool PublishSnapshot(SnapshotChannelMapping* mapping,
                     const SnapshotRecord& source,
                     const std::uint8_t* supplement,
                     std::size_t supplement_size) {
  if (!mapping) return false;
  volatile LONG* publication =
      reinterpret_cast<volatile LONG*>(&mapping->header.publication);
  const LONG previous = InterlockedCompareExchange(publication, 0, 0);
  if (previous < 0 || previous > LONG_MAX - 2) {
    InterlockedOr(reinterpret_cast<volatile LONG*>(&mapping->header.flags),
                  SnapshotSequenceExhausted);
    return false;
  }
  const LONG odd = previous + 1;
  if ((previous & 1) ||
      InterlockedCompareExchange(publication, odd, previous) != previous) {
    InterlockedIncrement(
        reinterpret_cast<volatile LONG*>(&mapping->header.dropped_count));
    InterlockedOr(reinterpret_cast<volatile LONG*>(&mapping->header.flags),
                  RawWriterBusyDrop);
    return false;
  }
  const std::uint32_t generation = static_cast<std::uint32_t>(odd + 1);
  std::memcpy(&mapping->record, &source, sizeof(source));
  mapping->header.reserved[0] = static_cast<std::uint32_t>(supplement_size);
  if (supplement_size)
    std::memcpy(mapping->supplement, supplement, supplement_size);
  mapping->record.generation = generation;
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&mapping->record.stamp_begin),
      static_cast<LONG>(generation | 0x80000000u));
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&mapping->record.stamp_end), 0);
  MemoryBarrier();
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&mapping->record.stamp_end),
      static_cast<LONG>(generation));
  MemoryBarrier();
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&mapping->record.stamp_begin),
      static_cast<LONG>(generation));
  MemoryBarrier();
  InterlockedExchange(publication, odd + 1);
  return true;
}

}  // namespace th10hook
