"""Byte layout of the snapshot the hook DLL publishes at each halted tick.

A snapshot is a 64-byte header, a fixed record, and a variable block of entity
tables. See native/include/th10/snapshot_format.h for the writer's side.
"""

import math
import struct
from dataclasses import dataclass
from enum import IntEnum

import numpy as np

MAGIC, VERSION, HEADER_SIZE = 0x31524F54, 3, 64
RECORD_SIZE, CAPACITY = 98420, 1
MAPPING_SIZE = HEADER_SIZE + RECORD_SIZE
ENTITIES_CAPACITY = 1 << 20
NAME = "Local\\TH10Gym.Snapshot.{}"

HEADER = struct.Struct("<IHH6I8I")
RECORD_START = struct.Struct("<13I9H2x")
BULLET = struct.Struct("<I2HI8f")
_PLAYER = struct.Struct("<Ii2f")
_RESOURCES = struct.Struct("<2iI3i")
_BOSS = struct.Struct("<2I2fi")
_COUNTS = struct.Struct("<4I")
_GROUPS = struct.Struct("<5I")
_TICKS = struct.Struct("<2I")
assert HEADER.size == HEADER_SIZE and RECORD_START.size == 72

PLAYER_OFFSET, RESOURCES_OFFSET, BOSS_OFFSET, COUNTS_OFFSET = 72, 88, 112, 132
GROUPS_OFFSET, TICKS_OFFSET, BULLET_OFFSET = 148, 168, 176
BULLET_COUNT = 2000
BULLET_READY_OFFSET = BULLET_OFFSET + BULLET_COUNT * BULLET.size
OUTCOME_OFFSET = BULLET_READY_OFFSET + BULLET_COUNT * 4
STAMP_END_OFFSET = RECORD_SIZE - 4
assert BULLET.size == 44 and BULLET_READY_OFFSET == 88176

# Bits in the groups status masks.
GROUP_PLAYER, GROUP_RESOURCES, GROUP_BOMB, GROUP_BOSS = 1, 2, 4, 8
GROUP_BULLETS, GROUP_LASERS, GROUP_INPUT = 16, 32, 64
KNOWN_GROUPS = 0x7F
# Bits in the record flags.
TICK_VALID, ACK_MATCHED, POLICY_READY, CAPTURE_FAULT = 1, 2, 4, 8
KNOWN_RECORD_FLAGS = TICK_VALID | ACK_MATCHED | POLICY_READY | CAPTURE_FAULT
# Values of terminal_outcome.
GAME_OVER, PRACTICE_COMPLETE = 1, 2


class AckKind(IntEnum):
    NONE = 0
    SNAPSHOT_COMPLETE = 5
    CANCELLED = 6
    TERMINAL = 9


_BULLET_FLOATS = ("x", "y", "vx", "vy", "speed", "angle", "width", "height")
_BULLET_FIELDS = ("slot", "state", "raw_flags", *_BULLET_FLOATS)
_BULLET_GYM_FIELDS = (*_BULLET_FIELDS, "spawn_ready")
_BULLET_WIRE = np.dtype([
    ("slot", "<u4"), ("state", "<u2"), ("reserved", "<u2"), ("raw_flags", "<u4"),
    *((name, "<f4") for name in _BULLET_FLOATS),
])
assert _BULLET_WIRE.itemsize == BULLET.size


class ObservationError(ValueError):
    """A snapshot is malformed, unstable, stale, or incomplete for its use."""


class UnstableObservation(ObservationError):
    """The writer is mid-publication, so no coherent copy was available."""


@dataclass(frozen=True, slots=True)
class PackedBullets:
    rows: np.ndarray
    ready: np.ndarray

    def column(self, name):
        return self.ready if name == "spawn_ready" else self.rows[name]


def materialize_bullets(record: dict) -> None:
    """Give raw callbacks their usual mutable list on the original record."""
    packed = record.get("bullets")
    if isinstance(packed, PackedBullets):
        rows = packed.rows[list(_BULLET_FIELDS)].tolist()
        record["bullets"] = [
            dict(zip(_BULLET_GYM_FIELDS, (*row, ready), strict=True))
            for row, ready in zip(rows, packed.ready.tolist(), strict=True)
        ]


def decode_header(data: bytes, pid: int) -> dict:
    if len(data) != HEADER_SIZE:
        raise ObservationError("Short snapshot header")
    (magic, version, size, record_size, target_pid, capacity, publication, drops, flags,
     entities_size, *reserved) = HEADER.unpack(data)
    if (magic, version, size, target_pid, capacity, record_size) != (
            MAGIC, VERSION, HEADER_SIZE, pid, CAPACITY, RECORD_SIZE):
        raise ObservationError("Snapshot layout mismatch or wrong target PID")
    if publication & 1:
        raise UnstableObservation("Snapshot publication is in progress")
    if flags:
        raise ObservationError(f"Snapshot writer flags are set: {flags:#x}")
    if drops:
        raise ObservationError(f"Snapshot writer dropped {drops} snapshot(s)")
    if entities_size > ENTITIES_CAPACITY:
        raise ObservationError("Snapshot entity tables exceed the channel capacity")
    if any(reserved):
        raise ObservationError("Snapshot header reserved fields are nonzero")
    return dict(pid=target_pid, version=version, record_size=record_size, capacity=capacity,
                publication=publication, dropped_count=drops, flags=flags, entities_size=entities_size)


def _require_finite(values, label):
    if not all(math.isfinite(x) for x in values):
        raise ObservationError(f"Non-finite value in {label}")


def _decode_bullet_rows(data: bytes, count: int) -> np.ndarray:
    rows = np.frombuffer(data, dtype=_BULLET_WIRE, count=count, offset=BULLET_OFFSET)
    slots = rows["slot"]
    if (slots >= BULLET_COUNT).any() or rows["reserved"].any():
        raise ObservationError("Invalid bullet slot or reserved field in captured pool")
    if (np.bincount(slots) > 1).any():
        raise ObservationError("Duplicate bullet slot in captured pool")
    geometry = np.ndarray(
        (count, len(_BULLET_FLOATS)), "<f4", buffer=data,
        offset=BULLET_OFFSET + _BULLET_WIRE.fields["x"][1], strides=(BULLET.size, 4),
    )
    if not np.isfinite(geometry).all():
        raise ObservationError("Non-finite value in captured bullet geometry")
    return rows


def _decode_packed_bullets(data: bytes, count: int) -> PackedBullets:
    return PackedBullets(
        _decode_bullet_rows(data, count),
        np.frombuffer(data, dtype="<u4", count=count, offset=BULLET_READY_OFFSET),
    )


def record_stamp(data: bytes) -> int | None:
    """Return the matching stamp at both record ends, or None if the record is torn."""
    begin, = struct.unpack_from("<I", data, 0)
    end, = struct.unpack_from("<I", data, STAMP_END_OFFSET)
    return begin if begin and not begin & 1 and begin == end else None


def decode_record(data: bytes) -> dict:
    # Imported here because the entity decoder needs this module's error types.
    from .entities import decode_entities

    if not RECORD_SIZE <= len(data) <= RECORD_SIZE + ENTITIES_CAPACITY:
        raise ObservationError("Snapshot record size is out of bounds")
    record = _decode_fixed_fields(data)
    if len(data) == RECORD_SIZE and not record["capture_fault"]:
        raise ObservationError("Snapshot has no entity tables and reports no capture fault")
    if record["laser_scanned"] or record["laser_captured"]:
        raise ObservationError("Laser counts belong in the entity tables")
    record["terminal_outcome"] = _decode_terminal_outcome(data, record)
    record["bullets"] = _decode_packed_bullets(data, record["bullet_captured"])
    record["lasers"] = []
    if len(data) > RECORD_SIZE:
        decode_entities(memoryview(data)[RECORD_SIZE:], record)
    return record


def _validate_groups(groups, input_valid, player, boss):
    available, valid, truncated, unsupported, read_fault = groups
    if any(mask & ~KNOWN_GROUPS for mask in groups):
        raise ObservationError("Unknown group status bit")
    if (valid & ~available or valid & (unsupported | read_fault) or
            truncated & ~available or unsupported & ~available):
        raise ObservationError("Inconsistent group status")
    if bool(valid & GROUP_INPUT) != bool(input_valid):
        raise ObservationError("Consumed-input validity disagrees with the group status")
    if valid & GROUP_PLAYER:
        _require_finite(player, "player")
    if valid & GROUP_BOSS:
        _require_finite(boss, "boss")


def _decode_fixed_fields(data: bytes) -> dict:
    if record_stamp(data) is None:
        raise ObservationError("Snapshot record stamps do not match")
    (stamp, generation, schema_version, flags, tick_count, calc_post_sequence,
     calc_callback_id, thread_id, qpc_low, qpc_high, session_id, command_sequence,
     completed_ticks, requested_actions, ack_kind, input_valid, reserved,
     *input_words) = RECORD_START.unpack_from(data)
    player_pointer, player_state, player_x, player_y = _PLAYER.unpack_from(data, PLAYER_OFFSET)
    (lives, power, bomb_pointer, bomb_active, bomb_timer,
     protection_timer) = _RESOURCES.unpack_from(data, RESOURCES_OFFSET)
    boss_manager, boss_slot0, boss_x, boss_y, boss_hp = _BOSS.unpack_from(data, BOSS_OFFSET)
    bullet_scanned, bullet_captured, laser_scanned, laser_captured = _COUNTS.unpack_from(data, COUNTS_OFFSET)
    groups = _GROUPS.unpack_from(data, GROUPS_OFFSET)
    stage_tick, segment_tick = _TICKS.unpack_from(data, TICKS_OFFSET)

    if stamp != generation or generation == 0 or schema_version != VERSION:
        raise ObservationError("Record generation or schema version is invalid")
    if not tick_count or flags & ~KNOWN_RECORD_FLAGS or reserved:
        raise ObservationError("Invalid record tick count, flags, or reserved field")
    if input_valid not in (0, 1):
        raise ObservationError("Consumed-input validity is not boolean")
    if bullet_scanned > BULLET_COUNT or bullet_captured > bullet_scanned:
        raise ObservationError("Bullet counts exceed the scanned pool")
    _validate_groups(groups, input_valid, (player_x, player_y), (boss_x, boss_y, boss_hp))

    available, valid, truncated, unsupported, read_fault = groups
    return {
        "generation": generation, "schema_version": schema_version, "flags": flags,
        "tick_valid": bool(flags & TICK_VALID),
        "ack_matched": bool(flags & ACK_MATCHED),
        "policy_ready": bool(flags & POLICY_READY),
        "capture_fault": bool(flags & CAPTURE_FAULT),
        "tick_count": tick_count, "calc_post_sequence": calc_post_sequence,
        "calc_callback_id": calc_callback_id, "thread_id": thread_id, "qpc": qpc_high << 32 | qpc_low,
        "session_id": session_id, "command_sequence": command_sequence,
        "completed_ticks": completed_ticks, "requested_actions": requested_actions,
        "ack_kind": ack_kind, "consumed_input_valid": bool(input_valid),
        "consumed_input": tuple(input_words) if input_valid else None,
        "player_pointer": player_pointer, "player_state": player_state,
        "player_x": player_x, "player_y": player_y, "lives": lives, "power": power,
        "bomb_pointer": bomb_pointer, "bomb_active": bomb_active,
        "bomb_timer": bomb_timer, "protection_timer": protection_timer,
        "boss_manager": boss_manager, "boss_slot0": boss_slot0,
        "boss_x": boss_x, "boss_y": boss_y, "boss_hp": boss_hp,
        "bullet_scanned": bullet_scanned, "bullet_captured": bullet_captured,
        "laser_scanned": laser_scanned, "laser_captured": laser_captured,
        "stage_tick": stage_tick, "segment_tick": segment_tick,
        "groups": {"available": available, "valid": valid, "truncated": truncated,
                   "unsupported": unsupported, "read_fault": read_fault},
    }


def _decode_terminal_outcome(data: bytes, record: dict) -> int:
    outcome, = struct.unpack_from("<I", data, OUTCOME_OFFSET)
    if (outcome not in (0, GAME_OVER, PRACTICE_COMPLETE)
            or record["ack_kind"] == AckKind.TERMINAL
            and (not outcome or record["consumed_input_valid"])):
        raise ObservationError("Invalid terminal outcome")
    return outcome
