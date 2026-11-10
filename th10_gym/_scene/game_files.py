"""Verify and populate the inputs for a private TH10 game directory."""

import errno
import hashlib
import os
import shutil
import stat
import struct
from pathlib import Path

VPATCH_SHA256 = "0aa5b79fdf5e8f57bcb505b4bf34b0875eb94e2f51269da6a370621e4c57431d"


def verify_game_assets(game_exe, dxvk_dll, dxvk_sha256):
    vpatch = game_exe.with_name("vpatch_th10.dll")
    if not vpatch.is_file() or hashlib.sha256(vpatch.read_bytes()).hexdigest() != VPATCH_SHA256:
        raise ValueError("Place the supported vpatch_th10.dll next to the TH10 executable")
    dxvk = dxvk_dll.read_bytes() if dxvk_dll is not None else None
    if dxvk is not None:
        if hashlib.sha256(dxvk).hexdigest() != dxvk_sha256:
            raise ValueError(
                "DXVK DLL SHA-256 mismatch. Use the official DXVK 3.1.1 x32 d3d9.dll "
                "or supply dxvk_sha256 for a trusted build."
            )
        _verify_x86_dll(dxvk)
    return vpatch, dxvk


def populate_game(destination, game_exe, vpatch, dxvk):
    for source in (game_exe, vpatch):
        shutil.copy2(source, destination / source.name)
    if dxvk is not None:
        (destination / "d3d9.dll").write_bytes(dxvk)
    for filename in ("th10.dat", "thbgm.dat"):
        _share_archive(game_exe.with_name(filename), destination / filename)
    for filename in ("th10.cfg", "vpatch.ini"):
        config = game_exe.with_name(filename)
        if config.is_file():
            shutil.copy2(config, destination / filename)


def _verify_x86_dll(data: bytes) -> None:
    message = "dxvk_dll must be a 32-bit Windows DLL (x86 PE32)"
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError(message)
    pe_offset, = struct.unpack_from("<I", data, 0x3C)
    optional_offset = pe_offset + 24
    if pe_offset < 0x40 or optional_offset > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError(message)
    machine, = struct.unpack_from("<H", data, pe_offset + 4)
    optional_size, flags = struct.unpack_from("<HH", data, pe_offset + 20)
    if (machine != 0x14C or not flags & 0x2000 or optional_size < 96
            or optional_offset + optional_size > len(data)
            or struct.unpack_from("<H", data, optional_offset)[0] != 0x10B):
        raise ValueError(message)


def _share_archive(source: Path, target: Path) -> None:
    source = source.resolve(strict=True)
    # TemporaryDirectory may clear a read-only attribute during Windows
    # cleanup. Give it a private file when that would change the source.
    attributes = getattr(source.stat(), "st_file_attributes", 0)
    if not attributes & stat.FILE_ATTRIBUTE_READONLY:
        try:
            os.link(source, target)
            return
        except OSError as error:
            if (error.errno not in (errno.EXDEV, errno.ENOTSUP)
                    and getattr(error, "winerror", None) not in (1, 17, 50)):
                raise
    shutil.copy2(source, target)
