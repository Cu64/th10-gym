"""Read-only Win32 process access for retail TH10 1.00a."""
from __future__ import annotations

import ctypes
import hashlib
import math
import os
import struct
from pathlib import Path

from .._cleanup import cleanup_after_error
from .._win32.process import QUERY_LIMITED_INFORMATION, image_path, process_api

EXPECTED_SHA256 = "2f14760b6fbbf57549541583283badb9a19a4222b90f0a146d5aa17f01dc9040"
TYPE_INFO = {"i32": (4, "<i"), "u32": (4, "<I"), "f32": (4, "<f"),
             "u16": (2, "<H"), "i16": (2, "<h"), "u8": (1, "<B")}


def checked(address: int, size: int = 4) -> int:
    """Validate a byte span in the game's 32-bit address space."""
    if not isinstance(address, int) or address < 0 or size < 0 or address + size > 2**32:
        raise ValueError("address span is outside 32-bit address space")
    return address


def read_exact(read, address: int, size: int = 4) -> bytes:
    """Read one complete bounded byte span through a read callback."""
    data = read(checked(address, size), size)
    if len(data) != size:
        raise ValueError(f"short read: expected {size} bytes, got {len(data)}")
    return data


def pe_identity(path: str) -> dict:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
        stream.seek(0)
        if stream.read(2) != b"MZ":
            raise ValueError("target image is not a PE executable (missing MZ)")
        stream.seek(0x3C)
        offset_data = stream.read(4)
        if len(offset_data) != 4:
            raise ValueError("truncated DOS header")
        pe_offset = struct.unpack("<I", offset_data)[0]
        if pe_offset > 16 * 1024 * 1024:
            raise ValueError("PE header offset is unreasonable")
        stream.seek(pe_offset)
        header = stream.read(24)
        if len(header) != 24 or header[:4] != b"PE\0\0":
            raise ValueError("invalid PE header")
        machine, = struct.unpack_from("<H", header, 4)
        optional_size, = struct.unpack_from("<H", header, 20)
        optional = stream.read(optional_size)
        if len(optional) < 32 or struct.unpack_from("<H", optional)[0] != 0x10B:
            raise ValueError("target is not PE32")
        image_base, = struct.unpack_from("<I", optional, 28)
    return {"sha256": digest.hexdigest(), "machine": f"0x{machine:04X}", "image_base": f"0x{image_base:08X}",
            "verified": digest.hexdigest() == EXPECTED_SHA256 and machine == 0x14C and image_base == 0x400000}


def runtime_pe_identity(api, handle) -> dict:
    base = 0x400000
    def bounded_read(address: int, size: int) -> bytes:
        if not 0 <= address <= 2**32 - size or not 1 <= size <= 4096:
            raise ValueError("runtime PE read is outside bounds")
        return api.read(handle, address, size)

    if bounded_read(base, 2) != b"MZ":
        raise ValueError("runtime image at 0x400000 has no MZ signature")
    e_lfanew = struct.unpack("<I", bounded_read(base + 0x3C, 4))[0]
    if e_lfanew > 16 * 1024 * 1024:
        raise ValueError("runtime PE header offset is unreasonable")
    pe = base + e_lfanew
    if pe + 4 + 20 + 32 > 2**32:
        raise ValueError("runtime PE header address overflows 32-bit space")
    header = bounded_read(pe, 24)
    if header[:4] != b"PE\0\0":
        raise ValueError("runtime image has no PE signature")
    machine, = struct.unpack_from("<H", header, 4)
    optional_size, = struct.unpack_from("<H", header, 20)
    if optional_size > 4096:
        raise ValueError("runtime optional header exceeds 4096-byte read cap")
    optional = bounded_read(pe + 24, optional_size) if optional_size else b""
    if len(optional) < 32:
        raise ValueError("runtime optional header is truncated")
    magic, = struct.unpack_from("<H", optional)
    image_base, = struct.unpack_from("<I", optional, 28)
    return {"base_address": f"0x{base:08X}", "machine": f"0x{machine:04X}",
            "optional_magic": f"0x{magic:04X}", "image_base": f"0x{image_base:08X}",
            "verified": machine == 0x14C and magic == 0x10B and image_base == base}


class WinApi:
    """Explicitly typed Win32 calls; only read/query rights are requested."""
    PROCESS_VM_READ = 0x0010
    PROCESS_QUERY_LIMITED_INFORMATION = QUERY_LIMITED_INFORMATION

    def __init__(self):
        if os.name != "nt":
            raise OSError("live diagnostics require Windows")
        self.kernel32 = process_api()
        self.kernel32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                                    ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.kernel32.ReadProcessMemory.restype = ctypes.c_int

    def open(self, pid: int):
        handle = self.kernel32.OpenProcess(self.PROCESS_VM_READ | self.PROCESS_QUERY_LIMITED_INFORMATION, 0, pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        return handle

    def image_path(self, handle) -> str:
        return image_path(self.kernel32, handle)

    def read(self, handle, address: int, size: int) -> bytes:
        buf = ctypes.create_string_buffer(size)
        count = ctypes.c_size_t()
        ok = self.kernel32.ReadProcessMemory(handle, ctypes.c_void_p(address), buf, size, ctypes.byref(count))
        if not ok or count.value != size:
            raise ctypes.WinError(ctypes.get_last_error() or 299)
        return buf.raw[:size]


def read_field(api: WinApi, handle, field: dict):
    address = field.get("address")
    if address is None:
        pointer = struct.unpack("<I", api.read(handle, field["pointer_address"], 4))[0]
        if pointer == 0:
            raise ValueError("resolved pointer is null")
        address = pointer + field["offset"]
    size, fmt = TYPE_INFO[field["type"]]
    if not 0 <= address or address + size > 2**32:
        raise ValueError("resolved address is outside 32-bit address space")
    value = struct.unpack(fmt, api.read(handle, address, size))[0]
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError("non-finite float")
    return value, address


class MemoryReader:
    """Read a verified retail TH10 process using a read-only handle."""

    def __init__(self, pid: int):
        if type(pid) is not int or not 0 < pid < 2**32:
            raise ValueError("pid must be a positive Win32 process ID")
        self.api = WinApi()
        self.handle = self.api.open(pid)
        try:
            self.image_path = Path(self.api.image_path(self.handle))
            self.image = pe_identity(str(self.image_path))
            if not self.image["verified"] or not runtime_pe_identity(self.api, self.handle)["verified"]:
                raise ValueError("Expected the supported x86 TH10 1.00a executable at 0x400000")
        except BaseException as error:
            cleanup_after_error(self.close, error, "Memory reader cleanup failed")
            raise

    def read(self, address: int, size: int) -> bytes:
        """Read bytes exactly, with bounds for the game's 32-bit address space."""
        if self.handle is None:
            raise RuntimeError("MemoryReader is closed")
        if not 0 < size <= 16 * 1024 * 1024 or not 0 <= address <= 2**32 - size:
            raise ValueError("Read must be within the 32-bit address space and at most 16 MiB")
        return self.api.read(self.handle, address, size)

    def field(self, spec: dict):
        """Read a scalar by direct address or by a 32-bit pointer plus offset."""
        if self.handle is None:
            raise RuntimeError("MemoryReader is closed")
        return read_field(self.api, self.handle, spec)

    def close(self) -> None:
        if self.handle is not None:
            self.api.kernel32.CloseHandle(self.handle)
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, _type, error, _traceback):
        if error is None:
            self.close()
        else:
            cleanup_after_error(self.close, error, "Memory reader cleanup failed")
