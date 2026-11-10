"""Toggle the retail draw block at a verified halted gameplay endpoint."""

import ctypes
from contextlib import contextmanager
from pathlib import Path

from .._cleanup import cleanup_after_error
from .kernel import owned_handle
from .process import QUERY_LIMITED_INFORMATION, image_path, process_api

# Retail and VPatch's alternate draw branch both enter here, after calculation
# and sound. Preserve the frame counter reset at 0x439508 so it cannot wrap
# while drawing is suppressed. Then jump to the shared tail at 0x439511.
DRAW_ENTRY = 0x43948E
DRAW_ORIGINAL = bytes.fromhex("a1 30 1c 49 00 8b 08 50 ff 91 a4 00 00 00")
DRAW_SKIP = bytes.fromhex("c6 45 14 00 e9 7a 00 00 00 90 90 90 90 90")


class DrawSkip:
    def __init__(self, pid, image, memory):
        self.pid = pid
        self.image = image
        self.memory = memory
        self.skipped = False
        if memory.read(DRAW_ENTRY, len(DRAW_ORIGINAL)) != DRAW_ORIGINAL:
            raise RuntimeError("Unsupported TH10 draw block for skip_render")

    def set_skipped(self, skip):
        if skip == self.skipped:
            return
        before, after = (DRAW_ORIGINAL, DRAW_SKIP) if skip else (DRAW_SKIP, DRAW_ORIGINAL)
        if self.memory.read(DRAW_ENTRY, len(before)) != before:
            raise RuntimeError("TH10 draw block changed before render toggle")
        self._write(after)
        if self.memory.read(DRAW_ENTRY, len(after)) != after:
            raise RuntimeError("Could not verify the TH10 render toggle")
        self.skipped = skip

    def _write(self, data):
        kernel = _code_write_api()
        handle = kernel.OpenProcess(QUERY_LIMITED_INFORMATION | 0x20 | 0x08, False, self.pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        with owned_handle(kernel, handle):
            if not Path(image_path(kernel, handle)).samefile(self.image):
                raise RuntimeError("TH10 render toggle requires the owned game process")
            with _writable_code(kernel, handle, len(data)):
                written = ctypes.c_size_t()
                if (not kernel.WriteProcessMemory(handle, DRAW_ENTRY, ctypes.create_string_buffer(data),
                                                  len(data), ctypes.byref(written))
                        or written.value != len(data)):
                    raise ctypes.WinError(ctypes.get_last_error() or 299)
                if not kernel.FlushInstructionCache(handle, DRAW_ENTRY, len(data)):
                    raise ctypes.WinError(ctypes.get_last_error())


def _code_write_api():
    kernel = process_api()
    pointer, size = ctypes.c_void_p, ctypes.c_size_t
    dword, boolean = ctypes.c_uint32, ctypes.c_int
    kernel.VirtualProtectEx.argtypes = (pointer, pointer, size, dword, ctypes.POINTER(dword))
    kernel.VirtualProtectEx.restype = boolean
    kernel.WriteProcessMemory.argtypes = (pointer, pointer, pointer, size, ctypes.POINTER(size))
    kernel.WriteProcessMemory.restype = boolean
    kernel.FlushInstructionCache.argtypes = (pointer, pointer, size)
    kernel.FlushInstructionCache.restype = boolean
    return kernel


@contextmanager
def _writable_code(kernel, handle, size):
    old = ctypes.c_uint32()
    if not kernel.VirtualProtectEx(handle, DRAW_ENTRY, size, 0x40, ctypes.byref(old)):
        raise ctypes.WinError(ctypes.get_last_error())

    def restore():
        if not kernel.VirtualProtectEx(handle, DRAW_ENTRY, size, old.value, ctypes.byref(ctypes.c_uint32())):
            raise ctypes.WinError(ctypes.get_last_error())

    try:
        yield
    except BaseException as error:
        cleanup_after_error(restore, error, "Could not restore TH10 code protection")
        raise
    else:
        restore()
