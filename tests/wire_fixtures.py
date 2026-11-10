"""Build snapshot bytes in the native wire format. Offsets are explicit to guard the layout."""

import struct

from th10_gym._snapshot import format as wire
from th10_gym._stepping.gate import GateState, GateStatus
from th10_gym._stepping.input_channel import REQUIRED_CHECKS, InputAck

PID = 1234
_TAIL_HEADER = "<5if3Iifi7I"
_FIXED_TABLES = bytes(128 * 0x5C + 33 * 0x6C + 4 * 12)  # shots, attacks, options


def record_bytes(*, generation=2, session_id=3, command=9, completed=1, record_flags=3,
                 ack_kind=5, requested_actions=0x80, read_fault=0, truncated=0, unsupported=0,
                 available=0, valid=0, bullet_scanned=0, bullet_captured=0,
                 tick_count=7, calc_post_sequence=88, calc_callback_id=22, boss_manager=0,
                 player_pointer=0x401000, player_state=4, player_x=12.5, player_y=32.0,
                 lives=9, stage_tick=100, segment_tick=200, input_current=0x80, outcome=0) -> bytearray:
    data = bytearray(wire.RECORD_SIZE)
    struct.pack_into("<13I9H2x", data, 0, generation, generation, wire.VERSION, record_flags,
                     tick_count, calc_post_sequence, calc_callback_id, 33, 123, 0, session_id, command, completed,
                     requested_actions, ack_kind, 1 if valid & wire.GROUP_INPUT else 0, 0,
                     input_current, 0, 0, 0, 0)
    struct.pack_into("<Ii2f", data, 72, player_pointer, player_state, player_x, player_y)
    struct.pack_into("<2i4I", data, 88, lives, 80, 0, 0, 0, 0)
    struct.pack_into("<2I2fi", data, 112, boss_manager, 0, 0.0, 0.0, 0)
    struct.pack_into("<4I", data, 132, bullet_scanned, bullet_captured, 0, 0)
    struct.pack_into("<5I", data, 148, available, valid, truncated, unsupported, read_fault)
    struct.pack_into("<2I", data, 168, stage_tick, segment_tick)
    for slot in range(bullet_captured):
        wire.BULLET.pack_into(data, wire.BULLET_OFFSET + slot * wire.BULLET.size,
                              slot, 1, 0, 0, *([0.0] * 8))
    struct.pack_into("<I", data, wire.OUTCOME_OFFSET, outcome)
    struct.pack_into("<I", data, wire.STAMP_END_OFFSET, generation)
    return data


def entity_bytes(*, stage=1, laser_manager=0, laser_rows=b"", laser_count=0, dialogue_word=0) -> bytes:
    header = struct.pack(_TAIL_HEADER, stage, 20, 1000, 0, 0, 0.0, 0, 0, 0, 3, 0.5, 0,
                         dialogue_word, 0, laser_manager, 0, laser_count, 124, 0)
    return header + _FIXED_TABLES + laser_rows


def snapshot_bytes(**fields) -> bytes:
    """Return a complete record and its entity tables."""
    entity_fields = {key: fields.pop(key) for key in ("stage", "laser_manager", "laser_rows", "laser_count",
                                                      "dialogue_word") if key in fields}
    return bytes(record_bytes(**fields)) + entity_bytes(**entity_fields)


def decoded(**fields) -> dict:
    return wire.decode_record(snapshot_bytes(**fields))


def header_bytes(publication=2, flags=0, pid=PID, version=wire.VERSION, entities_size=0) -> bytes:
    return wire.HEADER.pack(wire.MAGIC, version, wire.HEADER_SIZE, wire.RECORD_SIZE, pid, wire.CAPACITY,
                            publication, 0, flags, entities_size, *([0] * 7))


class FakeReader:
    """Serve a header and body the way the mapped snapshot view does."""

    pid = PID

    def __init__(self, body: bytes, publication=2, head: bytes | None = None):
        self.body = bytes(body)
        self.head = head or header_bytes(publication, entities_size=len(self.body) - wire.RECORD_SIZE)

    def read(self, offset, size):
        return (self.head + self.body)[offset:offset + size]


def ack(*, session_id=3, sequence=9, kind=wire.AckKind.SNAPSHOT_COMPLETE, reason=0, checks=REQUIRED_CHECKS,
        actions=0x80, exits=1, snapshot_sequence=88, calc_callback_id=22) -> InputAck:
    return InputAck(version=1, size=28, session_id=session_id, sequence=sequence, calc_callback_id=calc_callback_id,
                    snapshot_sequence=snapshot_sequence, actions=actions, player_callback_exits=exits,
                    kind=kind, reason=reason, checks=checks)


def halted_status(state=GateState.HALTED, command_sequence=1, pass_count=15, snapshot_sequence=88,
                  stage_tick=100, segment_tick=200, error=0) -> GateStatus:
    return GateStatus(state=state, command_sequence=command_sequence, pass_count=pass_count, skip_count=0,
                      snapshot_sequence=snapshot_sequence, stage_tick=stage_tick, segment_tick=segment_tick,
                      player_pointer=0x401000, player_x=12.5, player_y=32.0, error=error)
