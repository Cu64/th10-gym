"""Win32 atomic operations for native shared memory."""

import ctypes
from pathlib import Path


class AtomicOps:
    def __init__(self, dll_path):
        self.path = Path(dll_path)
        self.library = ctypes.WinDLL(str(self.path), use_last_error=True)
        self.library.Th10AtomicExchange32.argtypes = [ctypes.c_void_p, ctypes.c_int32]
        self.library.Th10AtomicExchange32.restype = ctypes.c_int32
        self.library.Th10AtomicLoad32.argtypes = [ctypes.c_void_p]
        self.library.Th10AtomicLoad32.restype = ctypes.c_int32

    def exchange32(self, address, value):
        return self.library.Th10AtomicExchange32(ctypes.c_void_p(address), ctypes.c_int32(value).value)

    def load32(self, address):
        return self.library.Th10AtomicLoad32(ctypes.c_void_p(address))
