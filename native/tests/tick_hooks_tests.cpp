#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "hook_status.h"

static_assert(sizeof(void*) == 4, "wrapper fixture requires x86");

extern "C" volatile DWORD g_original_calc;
extern "C" volatile DWORD g_original_replay;
extern "C" volatile DWORD g_original_player;
extern "C" volatile DWORD g_original_tick;
extern "C" int WINAPI Th10CalcChainHookWrapper(void*);
extern "C" int WINAPI Th10WholeTickStepWrapper(void*);
extern "C" void __stdcall Th10ReplayAdapterHookWrapper();
extern "C" void __stdcall Th10PlayerAdapterHookWrapper();

namespace {
struct Event {
  DWORD kind;
  void* context;
  int result;
};
struct Seen {
  DWORD eax, ecx, edx, edi, arg;
  DWORD flags;
  float st0;
  BYTE xmm[16];
};
Event events[16]{};
DWORD event_count = 0;
Seen seen{};
volatile DWORD step_allow = 1, tick_calls = 0;
volatile DWORD out_eax = 0, out_esp_before = 0, out_esp_after = 0,
               out_flags = 0;
volatile WORD out_fpu_status = 0;
BYTE out_xmm[16]{};
const DWORD kReturn = 0x4a31;
const BYTE kXmm[16] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
                       0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f};

bool check(bool ok, const char* what) {
  if (!ok) std::fprintf(stderr, "FAIL: %s\n", what);
  return ok;
}
}  // namespace

// Test recorder keeps the real helper's LastError contract and deliberately
// destroys FP/SSE state. The wrappers must restore that state before the body.
void RecordCallback(void* context, std::uint32_t kind, std::int32_t result) {
  const DWORD error = GetLastError();
  if (event_count < 16) events[event_count++] = {kind, context, result};
  __asm {
        fninit
        pxor xmm0, xmm0
  }
  SetLastError(error);
}

extern "C" int WINAPI Th10BeforeTick() {
  const DWORD error = GetLastError();
  __asm { fninit }
  __asm {pxor xmm0, xmm0} SetLastError(error);
  return static_cast<int>(step_allow);
}
extern "C" void WINAPI Th10AfterTick() {
  const DWORD error = GetLastError();
  __asm { fninit }
  __asm {pxor xmm0, xmm0} SetLastError(error);
}

extern "C" __declspec(naked) int __stdcall MockCalc() {
  __asm {
        mov dword ptr [seen.eax], eax
        mov dword ptr [seen.ecx], ecx
        mov dword ptr [seen.edx], edx
        mov dword ptr [seen.edi], edi
        mov eax, dword ptr [esp + 4]
        mov dword ptr [seen.arg], eax
        pushfd
        pop dword ptr [seen.flags]
        fstp dword ptr [seen.st0]
        movdqu xmmword ptr [seen.xmm], xmm0
        mov eax, 04a31h
        ret 4
  }
}
extern "C" __declspec(naked) int __stdcall MockTick() {
  __asm {
        pushfd
        pop dword ptr [seen.flags]
        inc dword ptr [tick_calls]
        mov dword ptr [seen.eax], eax
        mov dword ptr [seen.ecx], ecx
        mov dword ptr [seen.edx], edx
        mov dword ptr [seen.edi], edi
        mov eax, dword ptr [esp + 4]
        mov dword ptr [seen.arg], eax
        fstp dword ptr [seen.st0]
        movdqu xmmword ptr [seen.xmm], xmm0
        push dword ptr [seen.flags]
        popfd
        mov eax, 04a31h
        ret 4
  }
}
extern "C" __declspec(naked) int __stdcall MockReplay() {
  __asm {
        mov dword ptr [seen.eax], eax
        mov dword ptr [seen.ecx], ecx
        mov dword ptr [seen.edx], edx
        mov dword ptr [seen.edi], edi
        pushfd
        pop dword ptr [seen.flags]
        fstp dword ptr [seen.st0]
        movdqu xmmword ptr [seen.xmm], xmm0
        mov eax, 04a31h
        ret
  }
}
extern "C" __declspec(naked) int __stdcall MockPlayer() {
  __asm {
        mov dword ptr [seen.eax], eax
        mov dword ptr [seen.ecx], ecx
        mov dword ptr [seen.edx], edx
        mov dword ptr [seen.edi], edi
        mov eax, dword ptr [esp + 4]
        mov dword ptr [seen.arg], eax
        pushfd
        pop dword ptr [seen.flags]
        fstp dword ptr [seen.st0]
        movdqu xmmword ptr [seen.xmm], xmm0
        mov eax, 04a31h
        ret 4
  }
}

bool body_state_ok(DWORD eax, DWORD ecx, DWORD edx, DWORD edi, DWORD arg) {
  return seen.eax == eax && seen.ecx == ecx && seen.edx == edx &&
         seen.edi == edi && seen.arg == arg && (seen.flags & 0x40) != 0 &&
         seen.st0 == 1.0f && std::memcmp(seen.xmm, kXmm, sizeof(kXmm)) == 0;
}

bool output_state_ok() {
  return out_eax == kReturn && out_esp_before == out_esp_after &&
         (out_flags & 0x40) != 0 && ((out_fpu_status >> 11) & 7) == 0 &&
         std::memcmp(out_xmm, kXmm, sizeof(kXmm)) == 0;
}

bool run_calc() {
  event_count = 0;
  std::memset(&seen, 0, sizeof(seen));
  g_original_calc = (DWORD)(uintptr_t)&MockCalc;
  const DWORD owner = 0x12345678;
  SetLastError(0x1357);
  __asm {
        mov eax, 011223344h
        mov ecx, 055667788h
        mov edx, 099aabbcch
        mov edi, 0ddeeff00h
        movdqu xmm0, xmmword ptr [kXmm]
        fld1
        cmp eax,eax
        mov out_esp_before, esp
        push owner
        call Th10CalcChainHookWrapper
        mov out_esp_after, esp
        mov out_eax, eax
        pushfd
        pop out_flags
        fnstsw ax
        mov out_fpu_status, ax
        movdqu xmmword ptr [out_xmm], xmm0
  }
  bool ok =
      check(GetLastError() == 0x1357, "calc LastError preserved") &&
      check(
          body_state_ok(0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00, owner),
          "calc body sees original GPR/flags/x87/SSE and stack argument") &&
      check(output_state_ok(),
            "calc wrapper preserves return, stack, post-body flags/x87/SSE") &&
      check(event_count == 2 && events[0].kind == th10hook::kCallbackCalcPre &&
                events[1].kind == th10hook::kCallbackCalcPost &&
                events[1].result == (int)kReturn &&
                events[0].context == (void*)owner &&
                events[1].context == (void*)owner,
            "calc wrapper publishes matching pre/post events and preserves "
            "return");
  return ok;
}

bool run_whole_tick(bool allow) {
  std::memset(&seen, 0, sizeof(seen));
  tick_calls = 0;
  step_allow = allow ? 1u : 0u;
  g_original_tick = (DWORD)(uintptr_t)&MockTick;
  const DWORD owner = 0x76543210;
  SetLastError(0x1a2b);
  __asm {
        mov eax, 011223344h
        mov ecx, 055667788h
        mov edx, 099aabbcch
        mov edi, 0ddeeff00h
        movdqu xmm0, xmmword ptr [kXmm]
        fld1
        cmp eax,eax
        mov out_esp_before, esp
        push owner
        call Th10WholeTickStepWrapper
        mov out_esp_after, esp
        mov out_eax, eax
        pushfd
        pop out_flags
        fnstsw ax
        mov out_fpu_status, ax
        movdqu xmmword ptr [out_xmm], xmm0
  }
  bool common =
      check(GetLastError() == 0x1a2b, "whole tick LastError preserved") &&
      check(out_esp_before == out_esp_after,
            "whole tick wrapper consumes exactly one stack argument");
  if (allow) {
    return common && check(tick_calls == 1, "whole tick runs once") &&
           check(body_state_ok(0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00,
                               owner),
                 "whole tick body sees original GPR/flags/x87/SSE and "
                 "argument") &&
           check(output_state_ok(),
                 "whole tick preserves original result and post-body machine "
                 "state");
  }
  return common &&
         check(tick_calls == 0, "skipped whole tick never calls original") &&
         check(
             out_eax == 0,
             "skipped whole tick returns outer-loop continuation value zero") &&
         check((out_flags & 0x40) != 0, "skipped whole tick restores flags") &&
         check(((out_fpu_status >> 11) & 7) == 7,
               "skipped whole tick restores x87 stack depth") &&
         check(std::memcmp(out_xmm, kXmm, sizeof(kXmm)) == 0,
               "skipped whole tick restores SSE state");
}

bool run_replay() {
  event_count = 0;
  std::memset(&seen, 0, sizeof(seen));
  g_original_replay = (DWORD)(uintptr_t)&MockReplay;
  SetLastError(0x2468);
  __asm {
        mov eax, 011223344h
        mov ecx, 055667788h
        mov edx, 099aabbcch
        mov edi, 0ddeeff00h
        movdqu xmm0, xmmword ptr [kXmm]
        fld1
        cmp eax,eax
        mov out_esp_before, esp
        call Th10ReplayAdapterHookWrapper
        mov out_esp_after, esp
        mov out_eax, eax
        pushfd
        pop out_flags
        fnstsw ax
        mov out_fpu_status, ax
        movdqu xmmword ptr [out_xmm], xmm0
  }
  return check(GetLastError() == 0x2468, "Replay LastError preserved") &&
         check(body_state_ok(0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00, 0),
               "Replay body sees original GPR/flags/x87/SSE and EDI context") &&
         check(output_state_ok(),
               "Replay wrapper preserves return, stack, post-body "
               "flags/x87/SSE") &&
         check(event_count == 2 &&
                   events[0].kind == th10hook::kCallbackReplayPre &&
                   events[1].kind == th10hook::kCallbackReplayPost &&
                   events[1].result == (int)kReturn &&
                   events[0].context == (void*)0xddeeff00 &&
                   events[1].context == (void*)0xddeeff00,
               "Replay wrapper publishes matching pre/post events and "
               "preserves return");
}

bool run_player() {
  event_count = 0;
  std::memset(&seen, 0, sizeof(seen));
  g_original_player = (DWORD)(uintptr_t)&MockPlayer;
  const DWORD arg = 0x87654321;
  SetLastError(0x369c);
  __asm {
        mov eax, 011223344h
        mov ecx, 055667788h
        mov edx, 099aabbcch
        mov edi, 0ddeeff00h
        movdqu xmm0, xmmword ptr [kXmm]
        fld1
        cmp eax,eax
        mov out_esp_before, esp
        push arg
        call Th10PlayerAdapterHookWrapper
        mov out_esp_after, esp
        mov out_eax, eax
        pushfd
        pop out_flags
        fnstsw ax
        mov out_fpu_status, ax
        movdqu xmmword ptr [out_xmm], xmm0
  }
  return check(GetLastError() == 0x369c, "Player LastError preserved") &&
         check(
             body_state_ok(0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00, arg),
             "Player body sees original GPR/flags/x87/SSE and stack "
             "argument") &&
         check(output_state_ok(),
               "Player wrapper preserves return, stack, post-body "
               "flags/x87/SSE") &&
         check(event_count == 2 &&
                   events[0].kind == th10hook::kCallbackPlayerPre &&
                   events[1].kind == th10hook::kCallbackPlayerPost &&
                   events[1].result == (int)kReturn &&
                   events[0].context == (void*)arg &&
                   events[1].context == (void*)arg,
               "Player wrapper publishes matching pre/post events and "
               "preserves return");
}

int main() {
  bool ok = run_calc();
  ok = run_replay() && ok;
  ok = run_player() && ok;
  ok = run_whole_tick(true) && ok;
  ok = run_whole_tick(false) && ok;
  if (ok) std::puts("All hook wrapper ABI fixtures passed.");
  return ok ? 0 : 1;
}
