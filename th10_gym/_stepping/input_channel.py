"""Client for the input channel, which carries one action per tick and its acknowledgement."""

import ctypes
import os
import struct
import time
from dataclasses import dataclass

from .._snapshot.format import AckKind
from .._win32.mapping import AtomicMapping
from ..actions import valid_actions
from .seqlock import publishing, stable_read, tick_count_ms

MAGIC = 0x31555448  # "HTU1"
VERSION = 1
CHANNEL_SIZE = 72
HEARTBEAT_OFFSET = 8
COMMAND_COUNTER_OFFSET = 12
COMMAND_OFFSET = 16
ACK_COUNTER_OFFSET = 40
ACK_OFFSET = 44
COMMAND_FORMAT = struct.Struct("<IIIIHHI")
ACK_FORMAT = struct.Struct("<HHIIIIHHBBH")
MAX_TTL_MS = 5000
TICKS_PER_COMMAND = 1
ACK_POLL_INTERVAL_SECONDS = 0.0002
# The native code sets both checks on a normal tick: the stage tick advanced by
# one and the Player callback ran exactly once.
REQUIRED_CHECKS = 0x0003


@dataclass(frozen=True, slots=True)
class StepCommand:
    session_id: int
    sequence: int
    actions: int


@dataclass(frozen=True, slots=True)
class InputAck:
    version: int
    size: int
    session_id: int
    sequence: int
    calc_callback_id: int
    snapshot_sequence: int
    actions: int
    player_callback_exits: int
    kind: int
    reason: int
    checks: int


def parse_ack(data: bytes) -> InputAck:
    return InputAck(*ACK_FORMAT.unpack(data))


class InputChannel(AtomicMapping):
    def __init__(self, pid, *, atomic):
        if os.name != "nt":
            raise RuntimeError("The input channel requires Windows")
        if ctypes.sizeof(ctypes.c_void_p) != 8:
            raise RuntimeError("The x64 atomic helper requires 64-bit Python")
        self.pid = pid
        super().__init__(f"Local\\TH10Gym.Input.{pid}", CHANNEL_SIZE, atomic=atomic)
        magic, version, size = struct.unpack("<IHH", self.read_bytes(0, 8))
        if (magic, version, size) != (MAGIC, VERSION, CHANNEL_SIZE):
            self.close()
            raise ValueError(f"Input channel layout mismatch: magic={magic:#x}, version={version}, size={size}")


def publish_command(channel, command: StepCommand, ttl_ms):
    if not valid_actions(command.actions):
        raise ValueError("actions contain unknown or opposing input bits")
    if not 1 <= ttl_ms <= MAX_TTL_MS:
        raise ValueError(f"Commands require ttl_ms in 1..{MAX_TTL_MS}")
    now_ms = tick_count_ms(channel)
    payload = COMMAND_FORMAT.pack(
        command.session_id, command.sequence, now_ms, ttl_ms, command.actions, TICKS_PER_COMMAND, 0,
    )
    with publishing(channel, COMMAND_COUNTER_OFFSET) as counter:
        ctypes.memmove(channel.base + COMMAND_OFFSET, payload, len(payload))
        channel.atomic_store(HEARTBEAT_OFFSET, now_ms)
    return counter


def _read_ack(channel):
    channel.atomic_store(HEARTBEAT_OFFSET, tick_count_ms(channel))
    return stable_read(channel.read_stamp, channel.read_bytes, ACK_COUNTER_OFFSET, ACK_OFFSET, ACK_FORMAT.size)


def _is_final_ack(ack, session_id, sequence):
    if (ack.version, ack.size, ack.session_id, ack.sequence) != (VERSION, ACK_FORMAT.size, session_id, sequence):
        return False
    if ack.kind == AckKind.CANCELLED:
        return True
    if ack.reason or not ack.snapshot_sequence:
        return False
    if ack.kind == AckKind.TERMINAL:
        return (ack.player_callback_exits, ack.checks) == (0, 0)
    return ack.kind == AckKind.SNAPSHOT_COMPLETE and (ack.player_callback_exits, ack.checks) == (1, REQUIRED_CHECKS)


def wait_for_ack(channel, command: StepCommand, timeout_ms):
    """Return the command's final acknowledgement, or None after the timeout."""
    deadline = time.monotonic() + timeout_ms / 1000.0
    while time.monotonic() < deadline:
        stable = _read_ack(channel)
        ack = parse_ack(stable[1]) if stable is not None else None
        if ack is not None and _is_final_ack(ack, command.session_id, command.sequence):
            return ack
        time.sleep(ACK_POLL_INTERVAL_SECONDS)
    return None
