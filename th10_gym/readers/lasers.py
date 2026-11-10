"""Raw TH10 lasers memory decoder."""
from __future__ import annotations

import math
import struct

MANAGER_POINTER_ADDRESS = 0x47781C
LIST_HEAD_OFFSET = 0x18
NEXT_OFFSET = 0x08
NODE_STATE_OFFSET = 0x0C
NODE_READ_SIZE = 0x48
# These are raw common-view values. +0x40/+0x44 are type-dependent; in
# particular, +0x44 is not universally a width.
FLOAT_FIELDS = (("x", 0x24), ("y", 0x28), ("motion_x", 0x30),
                ("motion_y", 0x34), ("angle", 0x3C),
                ("raw_40", 0x40), ("raw_44", 0x44))
COMPLETE_STATUSES = {"completed", "empty"}
OBSERVATION_FIELDS = ("x", "y", "raw_dx", "raw_dy", "angle", "raw_geometry40", "raw_geometry44")


def _u32(api, handle, address: int) -> int:
    if not 0 <= address <= 0xFFFFFFFC:
        raise ValueError("32-bit read address out of range")
    data = api.read(handle, address, 4)
    if len(data) != 4:
        raise ValueError("short 32-bit read")
    return struct.unpack("<I", data)[0]


def _read_node(api, handle, address: int):
    if address < 0x10000 or address + NODE_READ_SIZE > 2**32:
        return None, f"node span outside bounded 32-bit address space: 0x{address:08X}"
    try:
        blob = api.read(handle, address, NODE_READ_SIZE)
        if len(blob) != NODE_READ_SIZE:
            raise ValueError(f"short node read at 0x{address:08X}")
    except (OSError, ValueError) as exc:
        return None, f"{type(exc).__name__}: {exc}"
    return blob, None


def traverse(api, handle, head: int, max_nodes: int | None = None) -> dict:
    if max_nodes is not None and max_nodes < 1:
        raise ValueError("max_nodes must be positive")
    if head == 0:
        return {"status": "empty", "nodes": [], "cycle_detected": False,
                "limit_reached": False}
    nodes, seen, address = [], set(), head
    while address:
        if address in seen:
            return {"status": "cycle_detected", "nodes": nodes,
                    "cycle_detected": True, "limit_reached": False}
        if max_nodes is not None and len(nodes) >= max_nodes:
            return {"status": "node_limit_reached", "nodes": nodes,
                    "cycle_detected": False, "limit_reached": True}
        blob, error = _read_node(api, handle, address)
        if error is not None:
            return {"status": "partial_read_error" if nodes else "read_error",
                    "error": error, "nodes": nodes,
                    "cycle_detected": False, "limit_reached": False}
        seen.add(address)
        address, node = _decode_node(blob, address)
        nodes.append(node)
    status = "partial_candidate_read" if any("nonfinite_fields" in node for node in nodes) else "completed"
    return {"status": status, "nodes": nodes,
            "cycle_detected": False, "limit_reached": False}


def _decode_node(blob, address):
    next_pointer, = struct.unpack_from("<I", blob, NEXT_OFFSET)
    state, = struct.unpack_from("<I", blob, NODE_STATE_OFFSET)
    fields = {}
    item = {"address": f"0x{address:08X}", "next": f"0x{next_pointer:08X}",
            "vtable": struct.unpack_from("<I", blob)[0], "node_state_raw": state, "raw_fields": fields}
    for name, offset in FLOAT_FIELDS:
        value, = struct.unpack_from("<f", blob, offset)
        if math.isfinite(value):
            fields[name] = value
            continue
        fields[name] = None
        item.setdefault("nonfinite_fields", []).append(name)
        item.setdefault("nonfinite_bits", {})[name] = struct.unpack_from("<I", blob, offset)[0]
    return next_pointer, item


def read_candidate(api, handle, max_nodes: int | None = None) -> dict:
    manager = _u32(api, handle, MANAGER_POINTER_ADDRESS)
    if manager == 0:
        return {"status": "unknown_null_manager", "manager_pointer": "0x00000000",
                "nodes": [], "cycle_detected": False, "limit_reached": False}
    if manager < 0x10000 or manager + LIST_HEAD_OFFSET + 4 > 2**32:
        raise ValueError("manager span outside bounded 32-bit address space")
    head = _u32(api, handle, manager + LIST_HEAD_OFFSET)
    result = traverse(api, handle, head, max_nodes)
    result.update(manager_pointer=f"0x{manager:08X}",
                  list_head=f"0x{head:08X}",
                  manager_pointer_address=f"0x{MANAGER_POINTER_ADDRESS:08X}")
    return result
