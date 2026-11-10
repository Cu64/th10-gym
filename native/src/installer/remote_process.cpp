#include "remote_process.h"

#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "report.h"

namespace th10installer {

UniqueHandle own_handle(HANDLE handle) {
  return UniqueHandle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

DWORD remaining_ms(ULONGLONG deadline, DWORD limit) {
  const ULONGLONG now = GetTickCount64();
  return now < deadline
             ? static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, limit))
             : 0;
}

bool eqi(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::wstring base_name(const std::wstring& p) {
  const size_t at = p.find_last_of(L"\\/");
  return at == std::wstring::npos ? p : p.substr(at + 1);
}
bool read_exact(HANDLE p, DWORD address, void* out, SIZE_T n) {
  SIZE_T got = 0;
  return ReadProcessMemory(p, reinterpret_cast<const void*>(address), out, n,
                           &got) &&
         got == n;
}

bool enumerate_modules_once(DWORD pid, std::vector<Module>& out, DWORD& err) {
  out.clear();
  err = ERROR_SUCCESS;
  auto snap = own_handle(
      CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
  if (!snap) {
    err = GetLastError();
    return false;
  }
  MODULEENTRY32W e{};
  e.dwSize = sizeof(e);
  bool ok = Module32FirstW(snap.get(), &e) != FALSE;
  DWORD iterationError = ok ? ERROR_SUCCESS : GetLastError();
  while (ok) {
    out.push_back({(DWORD)(uintptr_t)e.modBaseAddr, e.modBaseSize, e.szExePath,
                   e.szModule});
    e.dwSize = sizeof(e);
    ok = Module32NextW(snap.get(), &e) != FALSE;
    if (!ok) iterationError = GetLastError();
  }
  if (iterationError != ERROR_NO_MORE_FILES) {
    err = iterationError;
    out.clear();
    return false;
  }
  if (out.empty()) {
    err = ERROR_NO_MORE_FILES;
    return false;
  }
  return true;
}

void module_snapshot_retry_delay(DWORD milliseconds) { Sleep(milliseconds); }

bool enumerate_modules(DWORD pid, std::vector<Module>& out, DWORD& err,
                       ModuleEnumerator enumerateOnce, RetryDelay delay) {
  constexpr unsigned kAttempts = 8;
  constexpr DWORD kRetryDelayMs = 2;
  for (unsigned attempt = 0; attempt < kAttempts; ++attempt) {
    out.clear();
    err = ERROR_SUCCESS;
    if (enumerateOnce(pid, out, err)) return true;
    out.clear();
    if (err != ERROR_BAD_LENGTH) return false;
    if (attempt + 1 < kAttempts) delay(kRetryDelayMs);
  }
  err = ERROR_BAD_LENGTH;
  return false;
}

bool read_remote_pe(HANDLE p, DWORD base, DWORD& imageSize,
                    DWORD& preferredBase) {
  IMAGE_DOS_HEADER dos{};
  if (!read_exact(p, base, &dos, sizeof(dos)) ||
      dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
      dos.e_lfanew > 0x100000)
    return false;
  IMAGE_NT_HEADERS32 nt{};
  if (!read_exact(p, base + (DWORD)dos.e_lfanew, &nt, sizeof(nt)) ||
      nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
      nt.OptionalHeader.SizeOfImage < 0x40000 ||
      nt.OptionalHeader.SizeOfImage > 0x10000000)
    return false;
  imageSize = nt.OptionalHeader.SizeOfImage;
  preferredBase = nt.OptionalHeader.ImageBase;
  return true;
}
bool remote_readable_exec(HANDLE p, DWORD address, SIZE_T len) {
  MEMORY_BASIC_INFORMATION m{};
  if (!VirtualQueryEx(p, reinterpret_cast<const void*>(address), &m, sizeof(m)))
    return false;
  DWORD prot = m.Protect & 0xff;
  const DWORD start = (DWORD)(uintptr_t)m.BaseAddress;
  if (address < start) return false;
  const DWORD offset = address - start;
  return m.State == MEM_COMMIT && !(m.Protect & PAGE_GUARD) &&
         (prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
          prot == PAGE_EXECUTE_WRITECOPY) &&
         offset <= m.RegionSize && len <= m.RegionSize - offset;
}

bool remote_image_range_free(HANDLE p, DWORD base, DWORD imageSize,
                             std::string& why) {
  if (!base || !imageSize || imageSize > 0xffffffffu - base) {
    why = "preferred image range overflows x86 address space";
    return false;
  }
  const DWORD end = base + imageSize;
  DWORD cursor = base;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQueryEx(p, reinterpret_cast<const void*>(cursor), &m,
                        sizeof(m))) {
      why = "VirtualQueryEx failed in preferred image range: " +
            hexerr(GetLastError());
      return false;
    }
    const DWORD regionBase = (DWORD)(uintptr_t)m.BaseAddress;
    if (regionBase > cursor || m.RegionSize == 0 ||
        m.RegionSize > 0xffffffffu - regionBase) {
      why =
          "invalid or non-covering VirtualQueryEx region in preferred image "
          "range";
      return false;
    }
    const DWORD regionEnd = regionBase + (DWORD)m.RegionSize;
    if (regionEnd <= cursor) {
      why = "VirtualQueryEx region did not advance preferred image range scan";
      return false;
    }
    if (m.State != MEM_FREE) {
      char detail[96];
      std::snprintf(detail, sizeof(detail),
                    "preferred image range occupied at 0x%08lx (state 0x%lx)",
                    (unsigned long)cursor, (unsigned long)m.State);
      why = detail;
      return false;
    }
    cursor = regionEnd < end ? regionEnd : end;
  }
  return true;
}

DWORD remote_module_by_name(const std::vector<Module>& m,
                            const std::wstring& name, DWORD* size) {
  for (const auto& x : m)
    if (eqi(x.name, name)) {
      if (size) *size = x.size;
      return x.base;
    }
  return 0;
}
bool local_export_owner(const char* symbol, DWORD& remoteFn,
                        const std::vector<Module>& mods, std::string& why) {
  HMODULE k = GetModuleHandleW(L"kernel32.dll");
  FARPROC f = k ? GetProcAddress(k, symbol) : nullptr;
  if (!f) {
    why = hexerr(GetLastError());
    return false;
  }
  HMODULE owner = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCWSTR)f, &owner)) {
    why = hexerr(GetLastError());
    return false;
  }
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(owner, path, MAX_PATH);
  if (!n || n >= MAX_PATH) {
    why = hexerr(GetLastError());
    return false;
  }
  DWORD sz = 0, base = remote_module_by_name(mods, base_name(path), &sz);
  DWORD rva = (DWORD)((uintptr_t)f - (uintptr_t)owner);
  if (!base || rva >= sz) {
    why =
        "actual forwarded API owner absent or RVA out of range in remote "
        "process";
    return false;
  }
  remoteFn = base + rva;
  return true;
}

RemoteWait wait_remote(HANDLE thread, DWORD timeout, DWORD& code, DWORD& err) {
  const DWORD wait = WaitForSingleObject(thread, timeout);
  if (wait == WAIT_TIMEOUT) {
    err = WAIT_TIMEOUT;
    return RemoteWait::TimedOut;
  }
  if (wait != WAIT_OBJECT_0) {
    err = GetLastError();
    return RemoteWait::WaitFailed;
  }
  if (!GetExitCodeThread(thread, &code)) {
    err = GetLastError();
    return RemoteWait::ExitCodeFailed;
  }
  return RemoteWait::Completed;
}

}  // namespace th10installer
