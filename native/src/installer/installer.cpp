// Verify a running TH10 process, load the hook DLL into it, and redirect its
// CALL sites. Reports the outcome of each stage as one line of JSON on stdout.
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "call_patcher.h"
#include "file_sha256.h"
#include "game_hooks.h"
#include "hook_loader.h"
#include "hook_status.h"
#include "pe_file.h"
#include "remote_process.h"
#include "report.h"

#if !defined(_M_IX86)
#error Build the installer with the x86 Visual C++ environment.
#endif

namespace th10installer {
namespace {

using th10hook::retail::Hook;
using th10hook::retail::kHooks;
using th10hook::retail::kImageBase;

constexpr DWORD kInstallTimeoutMs = 35000;
constexpr DWORD kThreadTimeoutMs = 10000;
constexpr DWORD kHookImageBase = 0x50000000;
constexpr char kInitExport[] = "Th10InitializeHook";

enum ExitCode : int {
  kExitOk = 0,
  kExitBadArguments = 2,
  kExitNoAccess = 3,
  kExitVerificationFailed = 4,
  kExitLoadFailed = 5,
  kExitPatchFailed = 6,
};

struct Options {
  DWORD pid = 0;
  bool have_pid = false;
  std::wstring dll;
  bool have_dll = false;
};

// Record a failed stage and return the process exit code for it.
int fail(Report& r, const char* name, const char* status, std::string error,
         int code) {
  r.error = std::move(error);
  step(r, name, status, r.error);
  return code;
}

bool parse_pid(const wchar_t* argument, Options& options) {
  wchar_t* end = nullptr;
  const unsigned long value = wcstoul(argument, &end, 10);
  if (!end || *end || value == 0 || value > 0xfffffffful) return false;
  options.pid = static_cast<DWORD>(value);
  options.have_pid = true;
  return true;
}

int parse_arguments(int argc, wchar_t** argv, Options& options, Report& r) {
  std::string error;
  for (int i = 1; i < argc; i++) {
    if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
      if (!parse_pid(argv[++i], options)) error = "invalid --pid";
      continue;
    }
    if (wcscmp(argv[i], L"--dll") == 0 && i + 1 < argc) {
      options.dll = argv[++i];
      options.have_dll = true;
      continue;
    }
    if (wcscmp(argv[i], L"--install") == 0) {
      r.install_requested = true;
      continue;
    }
    error = "unknown or incomplete argument";
  }
  r.pid = options.pid;
  if (error.empty() &&
      (!options.have_pid || (r.install_requested && !options.have_dll)))
    error = "usage: --pid N [--dll path] [--install]";
  return error.empty()
             ? kExitOk
             : fail(r, "arguments", "failed", error, kExitBadArguments);
}

// Check the target's executable, base address, and unpatched CALL bytes.
int verify_target(HANDLE p, DWORD pid, DWORD& remote_base, Report& r) {
  std::wstring image_path(32768, L'\0');
  DWORD chars = (DWORD)image_path.size();
  if (!QueryFullProcessImageNameW(p, 0, &image_path[0], &chars))
    return fail(r, "process_image_path", "failed", hexerr(GetLastError()),
                kExitNoAccess);
  image_path.resize(chars);
  th10::Sha256Hex actual_hash;
  DWORD error = th10::sha256_file_hex(image_path.c_str(), actual_hash);
  if (error != ERROR_SUCCESS)
    return fail(r, "target_sha256", "failed", hexerr(error), kExitNoAccess);
  if (std::strcmp(actual_hash, th10hook::retail::kExecutableSha256) != 0)
    return fail(r, "target_sha256", "failed",
                std::string("process image disk SHA-256 mismatch; actual=") +
                    actual_hash,
                kExitVerificationFailed);
  step(r, "target_sha256", "ok", actual_hash);

  std::vector<Module> modules;
  if (!enumerate_modules(pid, modules, error))
    return fail(r, "module_inventory", "failed", hexerr(error), kExitNoAccess);
  r.modules = modules;
  step(r, "module_inventory", "ok",
       "module paths/bases/sizes recorded; THPrac and VPatch tagged by module "
       "name");

  remote_base = 0;
  for (const auto& module : modules)
    if (eqi(module.path, image_path)) {
      remote_base = module.base;
      break;
    }
  DWORD image_size = 0, preferred = 0;
  if (!remote_base || remote_base != kImageBase ||
      !read_remote_pe(p, remote_base, image_size, preferred) ||
      preferred != kImageBase)
    return fail(r, "target_pe", "failed",
                "remote main module is not the expected mapped PE32/i386 image "
                "at 0x400000",
                kExitVerificationFailed);
  for (const Hook& hook : kHooks) {
    BYTE current[sizeof(hook.original_bytes)]{};
    if (hook.rva > image_size || sizeof(current) > image_size - hook.rva ||
        !remote_readable_exec(p, remote_base + hook.rva, sizeof(current)) ||
        !read_exact(p, remote_base + hook.rva, current, sizeof(current)) ||
        memcmp(current, hook.original_bytes, sizeof(current)) != 0)
      return fail(
          r, "mapped_call_bytes", "failed",
          std::string("mapped CALL bytes mismatch or non-executable span at ") +
              hook.name,
          kExitVerificationFailed);
  }
  r.target_verified = true;
  step(
      r, "target_identity", "ok",
      "disk SHA-256, remote PE32/i386 image base, and mapped CALL bytes match");
  return kExitOk;
}

struct HookImage {
  std::wstring path;
  ExportInfo initializer;
  std::vector<ExportInfo> wrappers;
};

// Check the hook DLL's file and that its fixed address range is free in the
// target.
int verify_hook_image(HANDLE p, const std::wstring& dll, HookImage& image,
                      Report& r) {
  std::wstring full_path(32768, L'\0');
  const DWORD length = GetFullPathNameW(dll.c_str(), (DWORD)full_path.size(),
                                        &full_path[0], nullptr);
  if (!length || length >= full_path.size())
    return fail(r, "hook_path", "failed", hexerr(GetLastError()),
                kExitVerificationFailed);
  full_path.resize(length);
  image.path = full_path;

  std::vector<BYTE> bytes;
  DWORD error = 0;
  if (!load_bytes(full_path, bytes, error))
    return fail(r, "hook_pe", "failed", hexerr(error), kExitVerificationFailed);
  std::string why;
  image.wrappers.resize(std::size(kHooks));
  bool parsed = pe_export(bytes, kInitExport, image.initializer, why);
  for (size_t i = 0; parsed && i < image.wrappers.size(); ++i)
    parsed = pe_export(bytes, kHooks[i].wrapper, image.wrappers[i], why);
  if (!parsed)
    return fail(r, "hook_exports", "failed", why, kExitVerificationFailed);
  step(r, "hook_exports", "ok",
       "PE parsed from disk without loading or executing it locally");

  const auto fixed_image = [&](const ExportInfo& info) {
    constexpr WORD required = IMAGE_FILE_DLL | IMAGE_FILE_RELOCS_STRIPPED;
    return info.preferredBase == kHookImageBase &&
           info.imageSize == image.initializer.imageSize &&
           (info.fileCharacteristics & required) == required &&
           (info.dllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) ==
               0;
  };
  if (!fixed_image(image.initializer) ||
      !std::all_of(image.wrappers.begin(), image.wrappers.end(), fixed_image))
    return fail(r, "hook_fixed_image", "failed",
                "hook DLL is not a consistent fixed-base, relocation-stripped, "
                "non-DYNAMICBASE x86 image at 0x50000000",
                kExitVerificationFailed);
  step(r, "hook_fixed_image", "ok",
       "PE headers confirm preferred base 0x50000000, "
       "IMAGE_FILE_RELOCS_STRIPPED, "
       "and no DYNAMIC_BASE flag");

  for (const auto& module : r.modules)
    if (eqi(module.name, base_name(full_path)))
      return fail(r, "already_loaded_check", "failed",
                  "hook module with same basename already loaded; refusing "
                  "duplicate or stale installation",
                  kExitVerificationFailed);
  if (!remote_image_range_free(p, image.initializer.preferredBase,
                               image.initializer.imageSize, why))
    return fail(r, "hook_preferred_range", "occupied_or_unverifiable", why,
                kExitVerificationFailed);
  step(r, "hook_preferred_range", "free",
       "entire preferred SizeOfImage range is MEM_FREE before LoadLibraryW; "
       "fixed-base collision is refused");
  return kExitOk;
}

// Load the DLL and confirm the loaded image matches the file on disk.
int load_hook(InstallTarget target, const HookImage& image, DWORD& hook_base,
              Report& r) {
  std::string why;
  DWORD load_library = 0;
  if (!local_export_owner("LoadLibraryW", load_library, r.modules, why))
    return fail(r, "resolve_remote_LoadLibraryW", "failed", why,
                kExitVerificationFailed);
  step(r, "resolve_remote_LoadLibraryW", "ok",
       "resolved forwarded API owner locally and mapped its owner-relative RVA "
       "remotely");
  if (!remote_load(target.process, load_library, image.path, hook_base, r,
                   target.deadline))
    return kExitLoadFailed;

  std::vector<Module> after;
  DWORD error = 0;
  if (!enumerate_modules(target.pid, after, error))
    return fail(r, "confirm_hook_module", "failed",
                "cannot confirm loaded hook module: " + hexerr(error),
                kExitLoadFailed);
  const auto loaded =
      std::find_if(after.begin(), after.end(), [&](const Module& module) {
        return module.base == hook_base && eqi(module.path, image.path);
      });
  if (loaded == after.end())
    return fail(r, "confirm_hook_module", "failed",
                "LoadLibraryW result not matched to module inventory path/base",
                kExitLoadFailed);
  const DWORD remote_module_size = loaded->size;
  r.modules = after;

  const ExportInfo& initializer = image.initializer;
  DWORD actual_size = 0, actual_preferred = 0;
  const auto fits_image = [&](const ExportInfo& info) {
    return info.rva < initializer.imageSize &&
           info.bytes.size() <= initializer.imageSize - info.rva;
  };
  if (!read_remote_pe(target.process, hook_base, actual_size,
                      actual_preferred) ||
      !fits_image(initializer) ||
      !std::all_of(image.wrappers.begin(), image.wrappers.end(), fits_image) ||
      actual_preferred != initializer.preferredBase ||
      hook_base != actual_preferred || actual_size != initializer.imageSize ||
      actual_size > remote_module_size)
    return fail(r, "remote_exports", "failed",
                "loaded hook PE base/size/export RVA differs from parsed file",
                kExitLoadFailed);
  const auto matches_file = [&](const ExportInfo& info) {
    const DWORD address = hook_base + info.rva;
    std::vector<BYTE> actual(info.bytes.size());
    return remote_readable_exec(target.process, address, actual.size()) &&
           read_exact(target.process, address, actual.data(), actual.size()) &&
           memcmp(actual.data(), info.bytes.data(), actual.size()) == 0;
  };
  if (!matches_file(initializer) ||
      !std::all_of(image.wrappers.begin(), image.wrappers.end(), matches_file))
    return fail(r, "remote_exports", "failed",
                "remote initializer or hook wrapper bytes do not match DLL "
                "file exports",
                kExitLoadFailed);
  step(r, "remote_exports", "ok",
       "initializer and hook wrappers match file-backed bytes at the preferred "
       "base");
  return kExitOk;
}

int initialize_hook(HANDLE p, DWORD initializer_address, ULONGLONG deadline,
                    Report& r) {
  if (!remaining_ms(deadline, kThreadTimeoutMs))
    return fail(r, "remote_initialize", "timeout",
                "hook installation deadline expired before initialization",
                kExitLoadFailed);
  auto thread = own_handle(CreateRemoteThread(
      p, nullptr, 0,
      reinterpret_cast<LPTHREAD_START_ROUTINE>(initializer_address), nullptr, 0,
      nullptr));
  if (!thread) {
    r.initialization_state = "failed";
    return fail(r, "remote_initialize", "failed", hexerr(GetLastError()),
                kExitLoadFailed);
  }
  r.initialization_state = "pending";
  DWORD result = 0, error = 0;
  const auto wait = wait_remote(
      thread.get(), remaining_ms(deadline, kThreadTimeoutMs), result, error);
  thread.reset();
  if (wait == RemoteWait::TimedOut) {
    r.initialization_state = "unknown";
    return fail(r, "remote_initialize", "timeout",
                "initializer timed out; module retained", kExitLoadFailed);
  }
  if (wait != RemoteWait::Completed) {
    r.initialization_state = "unknown";
    return fail(
        r, "remote_initialize", "failed",
        "initializer result unavailable; module retained: " + hexerr(error),
        kExitLoadFailed);
  }
  if (result != ERROR_SUCCESS) {
    r.initialization_state = "failed";
    return fail(r, "remote_initialize", "failed",
                "initializer returned " + hexerr(result), kExitLoadFailed);
  }
  r.initialized = true;
  r.initialization_state = "initialized";
  step(r, "remote_initialize", "ok",
       "Th10InitializeHook returned ERROR_SUCCESS; implementation pins its "
       "module "
       "for process lifetime");
  return kExitOk;
}

int patch_call_sites(InstallTarget target, DWORD remote_base, DWORD hook_base,
                     const HookImage& image, Report& r) {
  wchar_t name[96];
  _snwprintf_s(name, _countof(name), _TRUNCATE, th10hook::kHookStatusName,
               target.pid);
  auto mapping =
      own_handle(OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name));
  if (!mapping)
    return fail(r, "open_shared_mapping", "failed",
                "hook shared mapping unavailable after initialization: " +
                    hexerr(GetLastError()),
                kExitLoadFailed);
  std::unique_ptr<th10hook::HookStatusMapping, UnmapView> shared(
      static_cast<th10hook::HookStatusMapping*>(
          MapViewOfFile(mapping.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                        sizeof(th10hook::HookStatusMapping))));
  if (!shared)
    return fail(r, "map_shared_state", "failed",
                "MapViewOfFile failed: " + hexerr(GetLastError()),
                kExitLoadFailed);
  if (shared->magic != th10hook::kHookStatusMagic ||
      shared->version != th10hook::kHookStatusVersion ||
      shared->size != sizeof(th10hook::HookStatusMapping) ||
      shared->target_pid != target.pid ||
      shared->hook_install_state != th10hook::kHookNotInstalled)
    return fail(r, "validate_shared_state", "failed",
                "hook status layout/PID mismatch or hook already installed",
                kExitLoadFailed);
  step(r, "validate_shared_state", "ok",
       "status layout, PID and not-installed state verified");

  std::vector<PatchSpec> patches;
  patches.reserve(std::size(kHooks));
  for (size_t i = 0; i < std::size(kHooks); ++i) {
    const Hook& hook = kHooks[i];
    patches.push_back({remote_base + hook.rva,
                       hook_base + image.wrappers[i].rva, hook.original_bytes,
                       hook.name});
  }
  return debug_patch_sites(target, patches, shared.get(), r) ? kExitOk
                                                             : kExitPatchFailed;
}

int run(int argc, wchar_t** argv, Report& r) {
  const ULONGLONG deadline = GetTickCount64() + kInstallTimeoutMs;
  Options options;
  if (const int code = parse_arguments(argc, argv, options, r)) return code;

  DWORD access = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE;
  if (r.install_requested)
    access |= PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD;
  const auto process = own_handle(OpenProcess(access, FALSE, options.pid));
  const HANDLE p = process.get();
  if (!p)
    return fail(r, "OpenProcess", "failed", hexerr(GetLastError()),
                kExitNoAccess);

  DWORD remote_base = 0;
  if (const int code = verify_target(p, options.pid, remote_base, r))
    return code;
  if (!r.install_requested && !options.have_dll) {
    step(r, "mutation", "not_requested",
         "inspection mode is read-only; no hook DLL preflight requested");
    return kExitOk;
  }
  HookImage image;
  if (const int code = verify_hook_image(p, options.dll, image, r)) return code;
  if (!r.install_requested) {
    step(r, "mutation", "not_requested",
         "DLL fixed-base and full preferred-range preflight passed; inspection "
         "mode did not load it");
    return kExitOk;
  }
  const InstallTarget target{p, options.pid, deadline};
  DWORD hook_base = 0;
  if (const int code = load_hook(target, image, hook_base, r)) return code;
  if (const int code =
          initialize_hook(p, hook_base + image.initializer.rva, deadline, r))
    return code;
  return patch_call_sites(target, remote_base, hook_base, image, r);
}

}  // namespace
}  // namespace th10installer

int wmain(int argc, wchar_t** argv) {
  th10installer::Report report;
  const int code = th10installer::run(argc, argv, report);
  th10installer::emit(report);
  return code;
}
