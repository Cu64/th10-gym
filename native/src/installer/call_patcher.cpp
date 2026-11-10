#include "call_patcher.h"

#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <string>

#include "remote_process.h"

namespace th10installer {
namespace {

constexpr DWORD kDebugTimeoutMs = 15000;
constexpr SIZE_T kCallSize = 5;
constexpr BYTE kCallOpcode = 0xe8;

struct PatchState {
  PatchSpec spec;
  BYTE original[kCallSize]{};
  BYTE replacement[kCallSize]{};
  DWORD old_protection = 0;
  bool written = false;
  bool protection_changed = false;
};

bool attach_breakpoint_address(const std::vector<Module>& mods, DWORD& address,
                               std::string& why) {
  HMODULE n = GetModuleHandleW(L"ntdll.dll");
  FARPROC f = n ? GetProcAddress(n, "DbgBreakPoint") : nullptr;
  if (!f) {
    why = "local ntdll!DbgBreakPoint unavailable";
    return false;
  }
  DWORD local = (DWORD)(uintptr_t)n, rva = (DWORD)((uintptr_t)f - local),
        size = 0;
  DWORD remote = remote_module_by_name(mods, L"ntdll.dll", &size);
  if (!remote || rva >= size) {
    why = "remote ntdll or matching DbgBreakPoint RVA unavailable";
    return false;
  }
  address = remote + rva;
  return true;
}

bool thread_ip_clear(DWORD tid, DWORD begin, DWORD end, std::string& why) {
  auto thread = own_handle(
      OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid));
  if (!thread) {
    const DWORD error = GetLastError();
    why = "OpenThread(" + std::to_string(tid) + "):" + hexerr(error);
    return false;
  }
  CONTEXT context{};
  context.ContextFlags = CONTEXT_CONTROL;
  if (!GetThreadContext(thread.get(), &context)) {
    const DWORD error = GetLastError();
    why = "GetThreadContext(" + std::to_string(tid) + "):" + hexerr(error);
    return false;
  }
  if (context.Eip >= begin && context.Eip < end) {
    why =
        "thread EIP inside 5-byte patch span at " + std::to_string(context.Eip);
    return false;
  }
  return true;
}

bool thread_ips_clear(DWORD pid, DWORD begin, DWORD end, std::string& why) {
  auto snap = own_handle(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0));
  if (!snap) {
    why = hexerr(GetLastError());
    return false;
  }
  THREADENTRY32 e{};
  e.dwSize = sizeof(e);
  bool ok = Thread32First(snap.get(), &e) != FALSE;
  bool found = false;
  while (ok) {
    if (e.th32OwnerProcessID == pid) {
      found = true;
      if (!thread_ip_clear(e.th32ThreadID, begin, end, why)) return false;
    }
    e.dwSize = sizeof(e);
    ok = Thread32Next(snap.get(), &e) != FALSE;
  }
  DWORD last = GetLastError();
  if (!found) {
    why = "no process threads enumerated";
    return false;
  }
  if (last != ERROR_NO_MORE_FILES) {
    why = hexerr(last);
    return false;
  }
  return true;
}

// One debugger attachment. Holds the attach breakpoint's debug event pending,
// which keeps every thread stopped, until the patches are finished.
class PatchSession {
 public:
  PatchSession(InstallTarget target, const std::vector<PatchSpec>& specs,
               th10hook::HookStatusMapping* shared, Report& report,
               decltype(&WriteProcessMemory) write_patch)
      : target_(target),
        shared_(shared),
        r_(report),
        write_patch_(write_patch) {
    patches_.reserve(specs.size());
    for (const auto& spec : specs) patches_.push_back(PatchState{spec});
  }

  bool run();

 private:
  bool attach();
  bool wait_for_attach_breakpoint(DWORD breakpoint, ULONGLONG attach_deadline);
  bool check_thread_positions();
  bool prepare_replacements();
  void write_patches();
  bool write_one(PatchState& patch);
  bool publish_installed();
  bool rollback();
  void clear_installed_state();

  bool continue_event(DWORD status);
  bool detach();
  bool abort_attach();
  void append_error(const std::string& error) {
    r_.error += (r_.error.empty() ? "" : "; ") + error;
  }

  const InstallTarget target_;
  th10hook::HookStatusMapping* shared_;
  Report& r_;
  decltype(&WriteProcessMemory) write_patch_;
  std::vector<PatchState> patches_;
  DEBUG_EVENT event_{};
  bool event_pending_ = false;
  DWORD continue_status_ = DBG_CONTINUE;
  bool kill_policy_set_ = false;
  DWORD kill_policy_error_ = ERROR_SUCCESS;
};

bool PatchSession::continue_event(DWORD status) {
  if (!event_pending_) return true;
  DWORD last = ERROR_SUCCESS;
  for (int i = 0; i < 3; i++) {
    if (ContinueDebugEvent(target_.pid, event_.dwThreadId, status)) {
      event_pending_ = false;
      return true;
    }
    last = GetLastError();
    Sleep(10);
  }
  SetLastError(last);
  return false;
}

bool PatchSession::detach() {
  DWORD last = ERROR_SUCCESS;
  for (int i = 0; i < 3; i++) {
    if (DebugActiveProcessStop(target_.pid)) {
      r_.debugger_attached = false;
      step(r_, "DebugActiveProcessStop", "ok", "debugger detached");
      return true;
    }
    last = GetLastError();
    Sleep(50);
  }
  SetLastError(last);
  const std::string error =
      "DebugActiveProcessStop failed after bounded retries: " + hexerr(last);
  step(r_, "DebugActiveProcessStop", "failed", error);
  append_error(error);
  return false;
}

bool PatchSession::abort_attach() {
  if (!continue_event(continue_status_))
    r_.error += "; ContinueDebugEvent failed: " + hexerr(GetLastError());
  detach();
  return false;
}

bool PatchSession::rollback() {
  bool ok = true;
  for (auto it = patches_.rbegin(); it != patches_.rend(); ++it) {
    if (!it->written && !it->protection_changed) continue;
    void* site = reinterpret_cast<void*>(it->spec.site);
    DWORD temp = 0;
    BOOL prot = VirtualProtectEx(target_.process, site, kCallSize,
                                 PAGE_EXECUTE_READWRITE, &temp);
    SIZE_T put = 0;
    BOOL wrote =
        !it->written || (prot &&
                         WriteProcessMemory(target_.process, site, it->original,
                                            kCallSize, &put) &&
                         put == kCallSize);
    BOOL flushed =
        !it->written ||
        (wrote && FlushInstructionCache(target_.process, site, kCallSize));
    DWORD ignored = 0;
    BOOL restored = prot && VirtualProtectEx(target_.process, site, kCallSize,
                                             it->old_protection, &ignored);
    BYTE check[kCallSize]{};
    bool verified =
        read_exact(target_.process, it->spec.site, check, kCallSize) &&
        memcmp(check, it->original, kCallSize) == 0;
    const bool restored_current =
        prot && wrote && flushed && restored && verified;
    ok = ok && restored_current;
    if (restored_current) {
      it->written = false;
      it->protection_changed = false;
    }
  }
  r_.patch_applied =
      std::any_of(patches_.begin(), patches_.end(),
                  [](const PatchState& patch) { return patch.written; });
  return ok;
}

void PatchSession::clear_installed_state() {
  r_.patch_applied = false;
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&shared_->hook_install_state),
      th10hook::kHookNotInstalled);
}

bool PatchSession::attach() {
  DWORD breakpoint = 0;
  std::string why;
  if (!attach_breakpoint_address(r_.modules, breakpoint, why)) {
    r_.error = why;
    step(r_, "resolve_attach_breakpoint", "failed", why);
    return false;
  }
  const DWORD attach_budget = remaining_ms(target_.deadline, kDebugTimeoutMs);
  if (!attach_budget) {
    r_.error = "hook installation deadline expired before debugger attach";
    step(r_, "wait_attach_breakpoint", "timeout", r_.error);
    return false;
  }
  const ULONGLONG attach_deadline =
      std::min(target_.deadline, GetTickCount64() + attach_budget);
  if (!DebugActiveProcess(target_.pid)) {
    r_.error = hexerr(GetLastError());
    step(r_, "DebugActiveProcess", "failed", r_.error);
    return false;
  }
  kill_policy_set_ = DebugSetProcessKillOnExit(FALSE) != FALSE;
  kill_policy_error_ = kill_policy_set_ ? ERROR_SUCCESS : GetLastError();
  r_.debugger_attached = true;
  step(r_, "DebugActiveProcess", "ok",
       "debug attach started; process threads are suspended");
  step(r_, "DebugSetProcessKillOnExit", kill_policy_set_ ? "ok" : "failed",
       kill_policy_set_ ? "disabled immediately on debugger thread"
                        : hexerr(kill_policy_error_));
  if (!wait_for_attach_breakpoint(breakpoint, attach_deadline)) {
    if (r_.error.empty())
      r_.error = "timed out waiting for verified ntdll attach breakpoint";
    step(r_, "wait_attach_breakpoint", "failed", r_.error);
    return abort_attach();
  }
  step(r_, "wait_attach_breakpoint", "ok",
       "first-chance breakpoint matched remote ntdll!DbgBreakPoint; event "
       "remains pending, process stopped");
  if (!kill_policy_set_) {
    r_.error = "DebugSetProcessKillOnExit(FALSE) failed: " +
               hexerr(kill_policy_error_);
    return abort_attach();
  }
  return true;
}

bool PatchSession::wait_for_attach_breakpoint(DWORD breakpoint,
                                              ULONGLONG attach_deadline) {
  while (const DWORD ms = remaining_ms(attach_deadline, 100)) {
    if (!WaitForDebugEvent(&event_, ms)) {
      const DWORD error = GetLastError();
      if (error == ERROR_SEM_TIMEOUT) continue;
      r_.error = hexerr(error);
      return false;
    }
    event_pending_ = true;
    continue_status_ = DBG_CONTINUE;
    if (event_.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
      const auto image_file = own_handle(event_.u.CreateProcessInfo.hFile);
    } else if (event_.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
      const auto image_file = own_handle(event_.u.LoadDll.hFile);
    } else if (event_.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
      const auto& exception = event_.u.Exception;
      if (exception.dwFirstChance &&
          exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT &&
          (DWORD)(uintptr_t)exception.ExceptionRecord.ExceptionAddress ==
              breakpoint)
        return true;
      continue_status_ = DBG_EXCEPTION_NOT_HANDLED;
    } else if (event_.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
      r_.error = "target exited during debugger attach";
      return false;
    }
    if (!continue_event(continue_status_)) {
      r_.error = hexerr(GetLastError());
      return false;
    }
  }
  return false;
}

bool PatchSession::check_thread_positions() {
  std::string why;
  for (const auto& patch : patches_) {
    if (!thread_ips_clear(target_.pid, patch.spec.site,
                          patch.spec.site + kCallSize, why)) {
      r_.error = why;
      step(r_, "thread_instruction_pointers", "failed",
           std::string(patch.spec.name) + ": " + why);
      return false;
    }
  }
  step(r_, "thread_instruction_pointers", "ok",
       "all enumerated process threads are outside every 5-byte CALL span");
  return true;
}

bool PatchSession::prepare_replacements() {
  for (auto& patch : patches_) {
    if (!read_exact(target_.process, patch.spec.site, patch.original,
                    kCallSize) ||
        memcmp(patch.original, patch.spec.expected, kCallSize) != 0) {
      r_.error = std::string("mapped CALL bytes changed before patch at ") +
                 patch.spec.name;
      step(r_, "revalidate_call_bytes", "failed", r_.error);
      return false;
    }
    patch.replacement[0] = kCallOpcode;
    const DWORD relative = patch.spec.wrapper - (patch.spec.site + kCallSize);
    memcpy(patch.replacement + 1, &relative, sizeof(relative));
  }
  return true;
}

bool PatchSession::write_one(PatchState& patch) {
  void* site = reinterpret_cast<void*>(patch.spec.site);
  if (!VirtualProtectEx(target_.process, site, kCallSize,
                        PAGE_EXECUTE_READWRITE, &patch.old_protection)) {
    r_.error = std::string("VirtualProtectEx failed at ") + patch.spec.name +
               ": " + hexerr(GetLastError());
    return false;
  }
  patch.protection_changed = true;
  // A failed write may leave changed bytes even without a byte count.
  patch.written = true;
  r_.patch_applied = true;
  SIZE_T put = 0;
  const BOOL wrote =
      write_patch_(target_.process, site, patch.replacement, kCallSize, &put);
  const DWORD write_error = wrote ? ERROR_PARTIAL_COPY : GetLastError();
  const bool complete = wrote && put == kCallSize;
  const BOOL flushed =
      complete ? FlushInstructionCache(target_.process, site, kCallSize)
               : FALSE;
  const DWORD flush_error = flushed ? ERROR_SUCCESS : GetLastError();
  DWORD ignored = 0;
  const BOOL restored = VirtualProtectEx(target_.process, site, kCallSize,
                                         patch.old_protection, &ignored);
  const DWORD restore_error = restored ? ERROR_SUCCESS : GetLastError();
  patch.protection_changed = !restored;
  const std::string name = patch.spec.name;
  if (!complete) {
    r_.error =
        "WriteProcessMemory failed at " + name + ": " + hexerr(write_error);
    return false;
  }
  if (!flushed) {
    r_.error =
        "FlushInstructionCache failed at " + name + ": " + hexerr(flush_error);
    return false;
  }
  if (!restored) {
    r_.error =
        "protection restore failed at " + name + ": " + hexerr(restore_error);
    return false;
  }
  BYTE check[kCallSize]{};
  if (!read_exact(target_.process, patch.spec.site, check, kCallSize) ||
      memcmp(check, patch.replacement, kCallSize) != 0) {
    r_.error = "patch readback mismatch at " + name;
    return false;
  }
  return true;
}

void PatchSession::write_patches() {
  for (auto& patch : patches_) {
    if (!remaining_ms(target_.deadline, 1)) {
      r_.error = "hook installation deadline expired while patching hooks";
      return;
    }
    if (!write_one(patch)) return;
  }
  if (!remaining_ms(target_.deadline, 1))
    r_.error = "hook installation deadline expired before committing hooks";
}

bool PatchSession::publish_installed() {
  InterlockedExchange(
      reinterpret_cast<volatile LONG*>(&shared_->hook_install_state),
      th10hook::kHookInstalled);
  if ((DWORD)shared_->hook_install_state == th10hook::kHookInstalled) {
    step(r_, "publish_hook_state", "ok",
         "shared state changed only after every patched CALL and protection "
         "verified");
    return true;
  }
  r_.error = "shared hook state failed post-patch verification";
  const bool rolled_back = rollback();
  if (rolled_back)
    clear_installed_state();
  else
    r_.error += "; rollback failed; hook state may be uncertain";
  step(r_, "publish_hook_state", "failed",
       r_.error + (rolled_back ? "; callsite rolled back" : ""));
  return false;
}

bool PatchSession::run() {
  if (patches_.empty()) {
    r_.error = "no patch sites provided";
    return false;
  }
  if (!attach()) return false;
  if (!check_thread_positions() || !prepare_replacements())
    return abort_attach();
  write_patches();
  if (!r_.error.empty()) {
    const bool rolled_back = rollback();
    if (!rolled_back)
      r_.error +=
          "; rollback failed; remote bytes or protection may be uncertain";
    step(r_, "write_patch", "failed",
         r_.error +
             (rolled_back
                  ? "; every changed CALL and protection restored and verified"
                  : ""));
    return abort_attach();
  }
  if (!publish_installed()) return abort_attach();
  r_.patch_applied = true;
  step(r_, "write_patch", "ok",
       "all 5-byte rel32 CALLs written, instruction caches flushed, "
       "protections restored, and full readbacks matched");
  if (!continue_event(DBG_CONTINUE)) {
    r_.error = "ContinueDebugEvent failed: " + hexerr(GetLastError());
    const bool rolled_back = rollback();
    if (rolled_back)
      clear_installed_state();
    else
      r_.error += "; rollback failed; hook state may be uncertain";
    step(r_, "continue_attach_event", "failed",
         r_.error +
             (rolled_back ? "; original bytes and protection restored" : ""));
    return abort_attach();
  }
  if (!detach()) return false;
  r_.installed = true;
  return true;
}

}  // namespace

bool debug_patch_sites(InstallTarget target,
                       const std::vector<PatchSpec>& specs,
                       th10hook::HookStatusMapping* shared, Report& r,
                       decltype(&WriteProcessMemory) write_patch) {
  return PatchSession(target, specs, shared, r, write_patch).run();
}

}  // namespace th10installer
