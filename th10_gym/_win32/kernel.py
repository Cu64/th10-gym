"""Win32 handle and wait primitives."""

import ctypes
from contextlib import contextmanager

SYNCHRONIZE = 0x00100000
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 0x102


def kernel_api():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel.CloseHandle.restype = ctypes.c_int
    return kernel


@contextmanager
def owned_handle(kernel, handle):
    try:
        yield handle
    finally:
        kernel.CloseHandle(handle)
