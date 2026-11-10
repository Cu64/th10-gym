#include <cstdio>
#include <vector>

#include "remote_process.h"
#include "report.h"

using namespace th10installer;

namespace {
struct Script {
  DWORD errors[8]{};
  unsigned count = 0, calls = 0, delays = 0;
};
Script* g_script = nullptr;
bool scripted_enumerator(DWORD, std::vector<Module>& out, DWORD& error) {
  ++g_script->calls;
  out.clear();
  const DWORD next = g_script->errors[g_script->calls - 1];
  if (next == ERROR_SUCCESS)
    out.push_back({1, 2, L"fixture.dll", L"fixture.dll"});
  error = next;
  return next == ERROR_SUCCESS;
}
void count_delay(DWORD ms) {
  if (ms == 2) ++g_script->delays;
}
bool check(bool ok, const char* what) {
  if (!ok)
    std::fprintf(stderr, "FAIL: %s\n", what);
  else
    std::printf("PASS: %s\n", what);
  return ok;
}
bool case_transient_then_success() {
  Script script{};
  script.errors[0] = ERROR_BAD_LENGTH;
  script.errors[1] = ERROR_SUCCESS;
  g_script = &script;
  std::vector<Module> modules{{9, 9, L"stale", L"stale"}};
  DWORD error = 0;
  bool ok =
      enumerate_modules(1, modules, error, scripted_enumerator, count_delay);
  return check(
      ok && error == ERROR_SUCCESS && script.calls == 2 && script.delays == 1 &&
          modules.size() == 1 && modules[0].name == L"fixture.dll",
      "transient ERROR_BAD_LENGTH retries and returns fresh module list");
}
bool case_nonretry_error_once() {
  Script script{};
  script.errors[0] = ERROR_ACCESS_DENIED;
  g_script = &script;
  std::vector<Module> modules;
  DWORD error = 0;
  bool ok =
      enumerate_modules(1, modules, error, scripted_enumerator, count_delay);
  return check(!ok && error == ERROR_ACCESS_DENIED && script.calls == 1 &&
                   script.delays == 0 && modules.empty(),
               "non-retry error fails immediately");
}
bool case_exhausted_bad_length() {
  Script script{};
  for (auto& error : script.errors) error = ERROR_BAD_LENGTH;
  g_script = &script;
  std::vector<Module> modules{{9, 9, L"stale", L"stale"}};
  DWORD error = 0;
  bool ok =
      enumerate_modules(1, modules, error, scripted_enumerator, count_delay);
  return check(
      !ok && error == ERROR_BAD_LENGTH && script.calls == 8 &&
          script.delays == 7 && modules.empty(),
      "persistent ERROR_BAD_LENGTH stops after eight bounded attempts");
}
bool case_real_self_enumeration() {
  std::vector<Module> modules;
  DWORD error = 0;
  bool ok = enumerate_modules(GetCurrentProcessId(), modules, error);
  bool found = false;
  for (const auto& module : modules)
    if (module.base && module.size && !module.name.empty()) found = true;
  return check(
      ok && found,
      "real read-only Toolhelp enumeration succeeds for current process");
}
}  // namespace

int wmain() {
  bool ok = case_transient_then_success();
  ok = case_nonretry_error_once() && ok;
  ok = case_exhausted_bad_length() && ok;
  ok = case_real_self_enumeration() && ok;
  return ok ? 0 : 1;
}
