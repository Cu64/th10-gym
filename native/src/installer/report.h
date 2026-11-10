// The JSON report the installer writes to stdout for its Python caller.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

namespace th10installer {

struct Step {
  std::string name, status, detail;
};

struct Module {
  DWORD base, size;
  std::wstring path, name;
};

struct Report {
  DWORD pid = 0;
  bool install_requested = false;
  bool target_verified = false;
  bool hook_loaded = false;
  bool initialized = false;
  bool patch_applied = false;
  bool installed = false;
  bool debugger_attached = false;
  const char* hook_load_state = "not_started";
  const char* initialization_state = "not_started";
  std::string error;
  std::vector<Step> steps;
  std::vector<Module> modules;
};

void step(Report& r, const char* name, const char* status,
          const std::string& detail = {});
std::string utf8(const std::wstring& w);
std::string hexerr(DWORD e);
void emit(const Report& r);

}  // namespace th10installer
