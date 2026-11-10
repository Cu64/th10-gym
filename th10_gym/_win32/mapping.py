"""Shared Win32 API bindings for the package's memory-mapped protocols."""

import ctypes

from .._cleanup import cleanup_after_error
from .kernel import kernel_api


def mapping_api():
    kernel = kernel_api()
    kernel.CreateFileMappingW.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
        ctypes.c_uint32, ctypes.c_wchar_p,
    ]
    kernel.CreateFileMappingW.restype = ctypes.c_void_p
    kernel.OpenFileMappingW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
    kernel.OpenFileMappingW.restype = ctypes.c_void_p
    kernel.MapViewOfFile.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_size_t,
    ]
    kernel.MapViewOfFile.restype = ctypes.c_void_p
    kernel.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    kernel.UnmapViewOfFile.restype = ctypes.c_int
    return kernel



FILE_MAP_WRITE = 0x0002
FILE_MAP_READ = 0x0004


class MappedView:
    """Own one Windows mapping handle and its view."""

    def __init__(self, name, access, size, *, kernel=None, create=False):
        self.kernel32 = mapping_api() if kernel is None else kernel
        self.base = None
        if create:
            self.handle = self.kernel32.CreateFileMappingW(
                ctypes.c_void_p(-1), None, 0x04, 0, size, name,
            )
        else:
            self.handle = self.kernel32.OpenFileMappingW(access, False, name)
        if not self.handle:
            raise OSError(ctypes.get_last_error(), f"Could not open Windows mapping {name!r}")
        try:
            self.base = self.kernel32.MapViewOfFile(self.handle, access, 0, 0, size)
            if not self.base:
                raise OSError(ctypes.get_last_error(), f"Could not map Windows view {name!r}")
        except BaseException as error:
            cleanup_after_error(self._close_handle, error, "Mapping handle cleanup failed")
            raise

    @property
    def address(self):
        return self.base

    def close(self):
        try:
            if self.base:
                self.kernel32.UnmapViewOfFile(self.base)
                self.base = None
        except BaseException as error:
            cleanup_after_error(self._close_handle, error, "Mapping handle cleanup failed")
            raise
        self._close_handle()

    def _close_handle(self):
        if self.handle:
            self.kernel32.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, _type, error, _traceback):
        if error is None:
            self.close()
        else:
            cleanup_after_error(self.close, error, "Mapping cleanup failed")


class AtomicMapping(MappedView):
    """Access stamped mailbox fields through the Windows interlocked shim."""

    def __init__(self, name, size, *, atomic):
        self.atomic = atomic
        super().__init__(name, FILE_MAP_READ | FILE_MAP_WRITE, size)

    def read_stamp(self, offset):
        return self.atomic.load32(self.base + offset) & 0xFFFFFFFF

    def read_bytes(self, offset, size):
        return ctypes.string_at(self.base + offset, size)

    def atomic_store(self, offset, value):
        self.atomic.exchange32(self.base + offset, ctypes.c_int32(value).value)
