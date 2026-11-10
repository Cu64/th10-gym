"""Raw TH10 boss memory decoder."""
from __future__ import annotations

import math
import struct

from .memory import TYPE_INFO, checked, read_exact

MANAGER_PTR = 0x477704
SLOT_OFFSET, SLOT_COUNT = 0x10, 8
FIELDS = (("x", 0x1068, "f32"), ("y", 0x106C, "f32"),
          ("vx", 0x1074, "f32"), ("vy", 0x1078, "f32"),
          ("hp", 0x23FC, "i32"))


def decode_slots(manager: int, read) -> dict:
    checked(manager, SLOT_OFFSET + SLOT_COUNT * 4)
    slots = [_decode_slot(read, index, manager + SLOT_OFFSET + index * 4) for index in range(SLOT_COUNT)]
    pointer_errors = sum("pointer_error" in entry for entry in slots)
    field_errors = sum(f"{name}_error" in entry for entry in slots for name, _, _ in FIELDS)
    return {"slot_count": SLOT_COUNT, "field_errors": field_errors,
            "slot_pointer_errors": pointer_errors, "read_errors": field_errors + pointer_errors,
            "slots": slots}


def _decode_slot(read, index, address):
    try:
        pointer, = struct.unpack("<I", read_exact(read, address))
    except (OSError, ValueError) as error:
        return {"slot": index, "registered": None, "pointer_error": f"{type(error).__name__}: {error}"}
    entry = {"slot": index, "pointer": f"0x{pointer:08X}", "registered": bool(pointer)}
    if not pointer:
        return entry
    for name, offset, kind in FIELDS:
        size, fmt = TYPE_INFO[kind]
        try:
            value, = struct.unpack(fmt, read_exact(read, pointer + offset, size))
            if kind == "f32" and not math.isfinite(value):
                raise ValueError("non-finite float")
        except (OSError, ValueError) as error:
            entry[name + "_error"] = f"{type(error).__name__}: {error}"
        else:
            entry[name] = value
    return entry
