#include "report.h"

#include <cstdio>

namespace th10installer {
namespace {

std::string quote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += (char)c;
    } else if (c < 0x20) {
      char b[7];
      std::snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else
      o += (char)c;
  }
  return o + '"';
}

}  // namespace

void step(Report& r, const char* name, const char* status,
          const std::string& detail) {
  r.steps.push_back({name, status, detail});
}
std::string utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
  if (n <= 0) return "<encoding-error>";
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr,
                      nullptr);
  return s;
}
std::string hexerr(DWORD e) {
  char b[32];
  std::snprintf(b, sizeof(b), "win32:%lu", (unsigned long)e);
  return b;
}

void emit(const Report& r) {
  std::string s =
      "{\"pid\":" + std::to_string(r.pid) +
      ",\"mode\":" + quote(r.install_requested ? "install" : "inspect") +
      ",\"target_verified\":" + (r.target_verified ? "true" : "false") +
      ",\"hook_loaded\":" + (r.hook_loaded ? "true" : "false") +
      ",\"initialized\":" + (r.initialized ? "true" : "false") +
      ",\"patch_applied\":" + (r.patch_applied ? "true" : "false") +
      ",\"hook_load_state\":" + quote(r.hook_load_state) +
      ",\"initialization_state\":" + quote(r.initialization_state) +
      ",\"installed\":" + (r.installed ? "true" : "false") +
      ",\"debugger_attached\":" + (r.debugger_attached ? "true" : "false") +
      ",\"error\":" + (r.error.empty() ? "null" : quote(r.error)) +
      ",\"module_inventory\":[";
  for (size_t i = 0; i < r.modules.size(); i++) {
    const auto& m = r.modules[i];
    if (i) s += ",";
    const std::wstring n = m.name;
    bool thprac = n.find(L"thprac") != std::wstring::npos;
    bool vpatch = n.find(L"vpatch") != std::wstring::npos;
    s += "{\"name\":" + quote(utf8(m.name)) +
         ",\"path\":" + quote(utf8(m.path)) +
         ",\"base\":" + std::to_string(m.base) +
         ",\"size\":" + std::to_string(m.size) +
         ",\"selected_stack\":" + ((thprac || vpatch) ? "true" : "false") +
         ",\"thprac\":" + (thprac ? "true" : "false") +
         ",\"vpatch\":" + (vpatch ? "true" : "false") + "}";
  }
  s += "],\"steps\":[";
  for (size_t i = 0; i < r.steps.size(); i++) {
    const auto& x = r.steps[i];
    if (i) s += ",";
    s += "{\"name\":" + quote(x.name) + ",\"status\":" + quote(x.status) +
         ",\"detail\":" + quote(x.detail) + "}";
  }
  s += "]}\n";
  DWORD written = 0;
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out && out != INVALID_HANDLE_VALUE)
    WriteFile(out, s.data(), (DWORD)s.size(), &written, nullptr);
}

}  // namespace th10installer
