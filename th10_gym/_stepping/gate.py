"""Client for the tick gate, which lets the game run one authorized tick at a time."""

import ctypes
import math
import os
import struct
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass
from enum import IntEnum, IntFlag
from typing import NamedTuple

from .._win32.mapping import FILE_MAP_READ, FILE_MAP_WRITE, AtomicMapping, MappedView
from . import seqlock
from .input_channel import HEARTBEAT_OFFSET as INPUT_HEARTBEAT_OFFSET

MAGIC = 0x31505453  # "STP1"
VERSION = 4
CHANNEL_SIZE = 88
HEARTBEAT_OFFSET = 8
COMMAND_COUNTER_OFFSET = 12
COMMAND_OFFSET = 16
STATUS_COUNTER_OFFSET = 32
STATUS_OFFSET = 36
EXPECTED_TICKS_OFFSET = 80
COMMAND_FORMAT = struct.Struct("<IIII")
STATUS_FORMAT = struct.Struct("<8IffI")

INSTALL_MAGIC = 0x49303154  # "T10I"
INSTALL_VERSION = 1
HOOKS_INSTALLED = 1
INSTALL_HEADER = struct.Struct("<IHHII")
READINESS_FORMAT = struct.Struct("<4I")
READINESS_COUNTER_OFFSET = INSTALL_HEADER.size
READINESS_OFFSET = READINESS_COUNTER_OFFSET + 4
INSTALL_STATUS_SIZE = READINESS_OFFSET + READINESS_FORMAT.size


class GateState(IntEnum):
    DISABLED = 0
    ARMING = 1
    HALTED = 2
    RUNNING = 3
    FAULT = 4
    RESETTING = 5


class GateError(IntEnum):
    NONE = 0
    BAD_MAPPING = 1
    BAD_COMMAND = 2
    HEARTBEAT_LOST = 3
    ESCAPE = 4
    OWNER_VIOLATION = 5
    INVALID_CHAIN = 6
    INPUT_RELEASE = 7
    SNAPSHOT_PUBLICATION = 8
    SNAPSHOT_NOTIFICATION = 9


class GateCommand(IntFlag):
    SINGLE_TICK = 1
    RESET_BEGIN = 4
    RESET_REARM = 8


class InvalidGateStatus(ValueError):
    """A malformed response from the native gate."""


@dataclass(frozen=True, slots=True)
class GateStatus:
    state: int
    command_sequence: int
    pass_count: int
    skip_count: int
    snapshot_sequence: int
    stage_tick: int
    segment_tick: int
    player_pointer: int
    player_x: float
    player_y: float
    error: int


class ReadinessSample(NamedTuple):
    """The latest end-of-Calc sample, published until the gate takes control."""

    sequence: int
    player_pointer: int
    stage_tick: int
    segment_tick: int


def parse_status(data: bytes) -> GateStatus:
    if len(data) != STATUS_FORMAT.size:
        raise InvalidGateStatus("short gate status")
    fields = STATUS_FORMAT.unpack(data)
    status = GateStatus(*fields)
    if status.state not in tuple(GateState):
        raise InvalidGateStatus(f"unknown gate state {status.state}")
    if not math.isfinite(status.player_x) or not math.isfinite(status.player_y):
        raise InvalidGateStatus("gate player coordinates are not finite")
    return status


class GateChannel(AtomicMapping):
    """The gate's command and status records."""

    def __init__(self, pid: int, *, atomic):
        if os.name != "nt":
            raise RuntimeError("The gate channel requires Windows")
        if ctypes.sizeof(ctypes.c_void_p) != 8:
            raise RuntimeError("The x64 atomic helper requires 64-bit Python")
        self.pid = pid
        super().__init__(f"Local\\TH10Gym.Gate.{pid}", CHANNEL_SIZE, atomic=atomic)
        magic, version, size = struct.unpack("<IHH", ctypes.string_at(self.base, 8))
        if (magic, version, size) != (MAGIC, VERSION, CHANNEL_SIZE):
            self.close()
            raise ValueError(f"Gate channel layout mismatch: magic={magic:#x}, version={version}, size={size}")

    def heartbeat(self) -> None:
        self.atomic_store(HEARTBEAT_OFFSET, seqlock.tick_count_ms(self))

    def publish_enable(self, sequence: int) -> int:
        return self._publish(sequence, GateCommand.SINGLE_TICK)

    def publish_reset_begin(self, sequence: int) -> int:
        """Release the gate so the scene bridge can restart the game."""
        return self._publish(sequence, GateCommand.SINGLE_TICK | GateCommand.RESET_BEGIN)

    def publish_reset_rearm(self, sequence: int, expected_ticks: tuple[int, int]) -> int:
        """Take control again from the stage and segment ticks just read."""
        return self._publish(sequence, GateCommand.SINGLE_TICK | GateCommand.RESET_REARM, expected_ticks)

    def _publish(self, sequence, flags, expected_ticks=(0, 0)):
        published = seqlock.tick_count_ms(self)
        payload = COMMAND_FORMAT.pack(sequence, 1, published, flags)
        expected = struct.pack("<II", *expected_ticks)
        with seqlock.publishing(self, COMMAND_COUNTER_OFFSET) as counter:
            ctypes.memmove(self.base + COMMAND_OFFSET, payload, len(payload))
            ctypes.memmove(self.base + EXPECTED_TICKS_OFFSET, expected, len(expected))
            self.atomic_store(HEARTBEAT_OFFSET, published)
        return counter

    def read_status(self) -> GateStatus | None:
        stable = seqlock.stable_read(self.read_stamp, self.read_bytes, STATUS_COUNTER_OFFSET,
                                     STATUS_OFFSET, STATUS_FORMAT.size)
        return parse_status(stable[1]) if stable is not None else None


class Heartbeat:
    """Keep both channels' heartbeats fresh while Python waits on other work."""

    def __init__(self, gate, input_channel, interval: float = 0.25):
        self._gate = gate
        self._input = input_channel
        self._interval = interval
        self._stop = threading.Event()
        self._error = None
        self._thread = threading.Thread(target=self._run, name="th10-heartbeat", daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.wait(self._interval):
            try:
                self.beat()
            except Exception as error:  # Surface failures on the stepping thread.
                self._error = error
                return

    def beat(self):
        self._gate.heartbeat()
        self._input.atomic_store(INPUT_HEARTBEAT_OFFSET, seqlock.tick_count_ms(self._input))

    def check(self):
        if self._error is not None:
            raise RuntimeError(f"Heartbeat failed: {self._error}") from self._error

    def close(self):
        self._stop.set()
        self._thread.join(timeout=max(1.0, self._interval * 2))


@contextmanager
def _readiness_reader(pid, atomic):
    name = f"Local\\TH10Gym.HookStatus.{pid}"
    access = FILE_MAP_READ | FILE_MAP_WRITE  # Interlocked reads require a writable view.
    with MappedView(name, access, INSTALL_STATUS_SIZE) as mapped:
        base = mapped.address
        header = INSTALL_HEADER.unpack(ctypes.string_at(base, INSTALL_HEADER.size))
        if header != (INSTALL_MAGIC, INSTALL_VERSION, INSTALL_STATUS_SIZE, pid, HOOKS_INSTALLED):
            raise ValueError("Hook install status has the wrong layout, PID, or installation state")

        def read_sample():
            stable = seqlock.stable_read(
                lambda offset: atomic.load32(base + offset),
                lambda offset, size: ctypes.string_at(base + offset, size),
                READINESS_COUNTER_OFFSET, READINESS_OFFSET, READINESS_FORMAT.size,
                attempts=4,
            )
            return ReadinessSample(*READINESS_FORMAT.unpack(stable[1])) if stable is not None else None

        yield read_sample


def wait_for_gameplay_progress(pid, atomic, deadline, *, after_sequence=0):
    """Wait until two fresh samples show the same player and advancing ticks."""
    with _readiness_reader(pid, atomic) as read_sample:
        previous = None
        while time.monotonic() < deadline:
            sample = read_sample()
            fresh = sample is not None and sample.sequence > after_sequence
            if fresh and previous and (
                sample.sequence > previous.sequence and sample.player_pointer == previous.player_pointer
                and sample.stage_tick > previous.stage_tick and sample.segment_tick > previous.segment_tick
            ):
                return
            if fresh:
                previous = sample if sample.player_pointer else None
            time.sleep(0.002)
        raise TimeoutError("TH10 gameplay ticks did not advance after scene start")
