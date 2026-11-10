#include <windows.h>
#include <cstdint>
#include "hook_internal.h"
#include "hook_status.h"

// Mutable only in the standalone native wrapper fixture. Production builds use
// the verified TH10 addresses and expose no setter for these targets.
#if defined(TH10_HOOK_WRAPPER_FIXTURE)
extern "C" volatile DWORD g_original_calc = 0;
extern "C" volatile DWORD g_original_replay = 0;
extern "C" volatile DWORD g_original_player = 0;
extern "C" volatile DWORD g_original_tick = 0;
#else
extern "C" volatile DWORD g_original_calc = 0x00449c00;
extern "C" volatile DWORD g_original_replay = 0x004297d0;
extern "C" volatile DWORD g_original_player = 0x00425730;
extern "C" volatile DWORD g_original_tick = 0x00439390;
#endif

extern "C" int WINAPI Th10BeforeTick();
extern "C" void WINAPI Th10AfterTick();

// Wrapper for the outer loop's complete FUN_00439390 tick call. A skipped tick
// returns zero so the normal outer loop continues. This preserves the observed
// x86 stdcall one-argument/RET 4 contract and never synthesizes dispatcher
// state.
extern "C" __declspec(dllexport, naked) int WINAPI
Th10WholeTickStepWrapper(void*) {
  __asm {
        pushfd
        pushad
        mov eax, esp
        sub esp, 544
        and esp, 0fffffff0h
        mov dword ptr [esp + 512], eax
        fxsave [esp]
        call Th10BeforeTick
        mov dword ptr [esp + 516], eax
        cmp dword ptr [esp + 516], 0
        je step_skip
        fxrstor [esp]
        mov esp, dword ptr [esp + 512]
        popad
        popfd

        push dword ptr [esp + 4]
        call dword ptr [g_original_tick]

        pushfd
        pushad
        mov eax, esp
        sub esp, 544
        and esp, 0fffffff0h
        mov dword ptr [esp + 512], eax
        fxsave [esp]
        call Th10AfterTick
        fxrstor [esp]
        mov esp, dword ptr [esp + 512]
        popad
        popfd
        ret 4

    step_skip:
        fxrstor [esp]
        mov esp, dword ptr [esp + 512]
        popad
        popfd
        mov eax, 0
        ret 4
  }
}

// CALL-site replacement for Render's stack-argument, callee-cleanup dispatcher
// call. Each telemetry call preserves GPRs, flags, x87, SSE, and LastError.
extern "C" __declspec(dllexport, naked) int WINAPI
Th10CalcChainHookWrapper(void*) {
  __asm {
        pushfd
        pushad
        mov eax, esp
        sub esp, 544
        and esp, 0fffffff0h
        mov dword ptr [esp + 512], eax
        fxsave [esp]
        push 080000000h
        push 7
        mov edx, dword ptr [esp + 520]
        push dword ptr [edx + 40]
        call RecordCallback
        add esp, 12
        fxrstor [esp]
        mov esp, dword ptr [esp + 512]
        popad
        popfd

        push dword ptr [esp + 4]
        call dword ptr [g_original_calc]

        pushfd
        pushad
        mov eax, esp
        sub esp, 544
        and esp, 0fffffff0h
        mov dword ptr [esp + 512], eax
        fxsave [esp]
        mov edx, dword ptr [esp + 512]
        push dword ptr [edx + 28]
        push 2
        mov edx, dword ptr [esp + 520]
        push dword ptr [edx + 40]
        call RecordCallback
        add esp, 12
        fxrstor [esp]
        mov esp, dword ptr [esp + 512]
        popad
        popfd
        ret 4
  }
}

// The Replay adapter establishes EDI=ECX before this call. Its target consumes
// EDI as context, takes no stack argument, and returns with RET.
extern "C"
    __declspec(dllexport, naked) void __stdcall Th10ReplayAdapterHookWrapper() {
  __asm {
        pushfd
        pushad
        mov eax, esp
        sub esp, 544
        and esp, 0fffffff0h
        mov dword ptr [esp + 512], eax
        fxsave [esp]
        push 080000000h
        push 3
        mov edx, dword ptr [esp + 520]
        push dword ptr [edx]
        call RecordCallback
        add esp, 12
        fxrstor [esp]
        mov esp, dword ptr [esp + 512]
        popad
        popfd

        call dword ptr [g_original_replay]

        pushfd
        pushad
        mov eax, esp
        sub esp, 544
        and esp, 0fffffff0h
        mov dword ptr [esp + 512], eax
        fxsave [esp]
        mov edx, dword ptr [esp + 512]
        push dword ptr [edx + 28]
        push 4
        mov edx, dword ptr [esp + 520]
        push dword ptr [edx]
        call RecordCallback
        add esp, 12
        fxrstor [esp]
        mov esp, dword ptr [esp + 512]
        popad
        popfd
        ret
  }
}

// The Player adapter pushes ECX before the call. Its target consumes a stack
// argument and executes RET 4. Copy the argument for the body, then consume the
// adapter's original pushed argument when this wrapper returns.
extern "C"
    __declspec(dllexport, naked) void __stdcall Th10PlayerAdapterHookWrapper() {
  __asm {
        pushfd
        pushad
        mov eax, esp
        sub esp, 544
        and esp, 0fffffff0h
        mov dword ptr [esp + 512], eax
        fxsave [esp]
        push 080000000h
        push 5
        mov edx, dword ptr [esp + 520]
        push dword ptr [edx + 40]
        call RecordCallback
        add esp, 12
        fxrstor [esp]
        mov esp, dword ptr [esp + 512]
        popad
        popfd

        push dword ptr [esp + 4]
        call dword ptr [g_original_player]

        pushfd
        pushad
        mov eax, esp
        sub esp, 544
        and esp, 0fffffff0h
        mov dword ptr [esp + 512], eax
        fxsave [esp]
        mov edx, dword ptr [esp + 512]
        push dword ptr [edx + 28]
        push 6
        mov edx, dword ptr [esp + 520]
        push dword ptr [edx + 40]
        call RecordCallback
        add esp, 12
        fxrstor [esp]
        mov esp, dword ptr [esp + 512]
        popad
        popfd
        ret 4
  }
}
