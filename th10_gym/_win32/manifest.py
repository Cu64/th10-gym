"""Verify embedded asInvoker manifests without executing native tools."""
import ctypes
import xml.etree.ElementTree as ET
from pathlib import Path


def verify(path: Path):
    tree = ET.fromstring(_read_embedded_manifest(path))
    levels = [element.attrib for element in tree.iter()
              if element.tag.rsplit('}', 1)[-1] == 'requestedExecutionLevel']
    if levels != [{'level': 'asInvoker', 'uiAccess': 'false'}]:
        raise ValueError(f'{path.name}: expected explicit asInvoker, got {levels}')
    return {'tool': path.name, 'requested_execution_level': 'asInvoker'}


def _read_embedded_manifest(path: Path) -> bytes:
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LoadLibraryExW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p, ctypes.c_uint32]
    kernel.LoadLibraryExW.restype = ctypes.c_void_p
    kernel.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    kernel.FindResourceW.restype = ctypes.c_void_p
    kernel.SizeofResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.SizeofResource.restype = ctypes.c_uint32
    kernel.LoadResource.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    kernel.LoadResource.restype = ctypes.c_void_p
    kernel.LockResource.argtypes = [ctypes.c_void_p]
    kernel.LockResource.restype = ctypes.c_void_p
    kernel.FreeLibrary.argtypes = [ctypes.c_void_p]
    module = kernel.LoadLibraryExW(str(path), None, 2)  # LOAD_LIBRARY_AS_DATAFILE
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        resource = kernel.FindResourceW(module, ctypes.c_void_p(1), ctypes.c_void_p(24))
        if not resource:
            raise ValueError(f'{path.name}: no embedded executable manifest')
        length = kernel.SizeofResource(module, resource)
        loaded = kernel.LoadResource(module, resource)
        if not length or not loaded:
            raise ValueError(f'{path.name}: manifest resource unavailable')
        pointer = kernel.LockResource(loaded)
        if not pointer:
            raise ValueError(f'{path.name}: manifest resource unavailable')
        return ctypes.string_at(pointer, length)
    finally:
        kernel.FreeLibrary(module)
