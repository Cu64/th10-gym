"""Raw TH10 bullets memory decoder."""
from __future__ import annotations

import math
import struct
from collections import Counter

MANAGER_PTR = 0x4776F0
# The 2,001st physical record is the allocator's wrap sentinel, not a bullet.
POOL_OFFSET, STRIDE, SLOTS = 0x60, 0x7F0, 2000
STATE_OFFSET = 0x446
BLOB_SIZE = POOL_OFFSET + STRIDE * SLOTS


_GEOMETRY_FIELDS = (("x", 0x3B4), ("y", 0x3B8), ("vx", 0x3C0), ("vy", 0x3C4),
                    ("collisionWidth", 0x3F0), ("collisionHeight", 0x3F4))


def decode_blob(blob: bytes, base: int, *, limit: int | None = None) -> dict:
    if len(blob) != BLOB_SIZE:
        raise ValueError(f"pool blob must be exactly {BLOB_SIZE} bytes")
    counts, records, errors = Counter(), [], 0
    collision_flag_count = 0
    for i in range(SLOTS):
        start = POOL_OFFSET + i * STRIDE
        state, = struct.unpack_from("<H", blob, start + STATE_OFFSET)
        counts[str(state)] += 1
        if not state:
            continue
        flags, = struct.unpack_from("<I", blob, start)
        flag_set = bool(flags & 0x2)
        collision_flag_count += int(flag_set)
        item = _decode_record(blob, start, base, i, state, flags)
        errors += "error" in item
        if limit is None or len(records) < limit:
            records.append(item)
    nonzero = SLOTS - counts["0"]
    return {"slot_count": SLOTS, "nonzero_state_count": nonzero,
            "state_histogram": dict(sorted(counts.items(), key=lambda p: int(p[0]))),
            "nonzero_state_with_collision_flag_count": collision_flag_count,
            "record_errors": errors, "records_stored": records, "records_truncated": nonzero - len(records)}


def _decode_record(blob, start, base, slot, state, flags):
    item = {"slot": slot, "state": state, "address": f"0x{base + start:08X}",
            "flags_raw": flags, "collision_flag_0x2": bool(flags & 0x2), "collidable_verified": None}
    for name, offset in _GEOMETRY_FIELDS:
        value, = struct.unpack_from("<f", blob, start + offset)
        if not math.isfinite(value):
            return {"slot": slot, "state": state, "error": f"non-finite {name}"}
        item[name] = value
    return item
