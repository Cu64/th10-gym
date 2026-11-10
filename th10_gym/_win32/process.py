"""Process discovery, identity, and cleanup for the Windows runtime."""

import ctypes
import os
import sys

from .kernel import SYNCHRONIZE, WAIT_OBJECT_0, WAIT_TIMEOUT, kernel_api, owned_handle

QUERY_LIMITED_INFORMATION = 0x1000
TERMINATE = 0x0001


def process_api():
    kernel = kernel_api()
    kernel.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.QueryFullProcessImageNameW.argtypes = (
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32),
    )
    kernel.QueryFullProcessImageNameW.restype = ctypes.c_int
    kernel.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel.WaitForSingleObject.restype = ctypes.c_uint32
    return kernel


def image_path(kernel, handle):
    path = ctypes.create_unicode_buffer(32768)
    capacity = ctypes.c_uint32(len(path))
    if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(capacity)):
        raise ctypes.WinError(ctypes.get_last_error())
    return path.value


def normalized_path(path):
    return os.path.normcase(os.path.realpath(path))


def process_paths() -> dict[int, str]:
    if sys.platform != "win32":
        raise RuntimeError("Private launcher process discovery requires Windows.")
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    psapi.EnumProcesses.argtypes = (ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32,
                                  ctypes.POINTER(ctypes.c_uint32))
    psapi.EnumProcesses.restype = ctypes.c_int
    pids = (ctypes.c_uint32 * 32768)()
    needed = ctypes.c_uint32()
    if not psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(needed)):
        raise ctypes.WinError(ctypes.get_last_error())
    count = min(needed.value // ctypes.sizeof(ctypes.c_uint32), len(pids))
    kernel = process_api()
    result = {}
    for pid in pids[:count]:
        if not pid:
            continue
        handle = kernel.OpenProcess(QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            continue
        with owned_handle(kernel, handle):
            try:
                path = image_path(kernel, handle)
            except OSError:
                # The process can exit between enumeration and path lookup.
                continue
            result[pid] = normalized_path(path)
    return result


def process_alive(pid: int) -> bool:
    if os.name != "nt":
        return False
    kernel = process_api()
    handle = kernel.OpenProcess(SYNCHRONIZE, False, pid)
    if not handle:
        return False
    with owned_handle(kernel, handle):
        return kernel.WaitForSingleObject(handle, 0) == WAIT_TIMEOUT


def stop_owned(pid, path):
    kernel = process_api()
    kernel.TerminateProcess.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel.TerminateProcess.restype = ctypes.c_int
    handle = kernel.OpenProcess(SYNCHRONIZE | QUERY_LIMITED_INFORMATION | TERMINATE, False, pid)
    if not handle:
        if process_alive(pid):
            raise OSError(f"Could not open owned TH10 process {pid} for cleanup")
        return
    with owned_handle(kernel, handle):
        if normalized_path(image_path(kernel, handle)) != path:
            return
        if kernel.WaitForSingleObject(handle, 0) == WAIT_OBJECT_0:
            return
        if not kernel.TerminateProcess(handle, 0):
            error = ctypes.get_last_error()
            if kernel.WaitForSingleObject(handle, 0) != WAIT_OBJECT_0:
                raise ctypes.WinError(error)
        if kernel.WaitForSingleObject(handle, 5000) != WAIT_OBJECT_0:
            raise TimeoutError("Owned TH10 process did not exit")
