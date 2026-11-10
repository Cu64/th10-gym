"""Read snapshots published by the hook DLL and tie them to the command that produced them."""

import ctypes
import math
import time
from contextlib import ExitStack

from .._snapshot import format as wire
from .._snapshot.format import AckKind, ObservationError, UnstableObservation
from .._win32.kernel import SYNCHRONIZE, WAIT_TIMEOUT, owned_handle
from .._win32.mapping import FILE_MAP_READ, MappedView
from .._win32.process import process_api
from .gate import GateState, GateStatus
from .input_channel import ACK_FORMAT, REQUIRED_CHECKS, InputAck, StepCommand
from .input_channel import VERSION as ACK_VERSION

READY_EVENT_NAME = "Local\\TH10Gym.SnapshotReady.{}"
_COORDINATE_TOLERANCE = 1e-4
_READ_ATTEMPTS = 100


def _read_candidate(reader, pid):
    """Read a payload bracketed by matching headers and check its stamps."""
    try:
        before = wire.decode_header(reader.read(0, wire.HEADER_SIZE), pid)
        size = before["record_size"] + before["entities_size"]
        payload = reader.read(wire.HEADER_SIZE, size)
        after = wire.decode_header(reader.read(0, wire.HEADER_SIZE), pid)
    except UnstableObservation:
        return None
    if before != after:
        return None
    if len(payload) != size:
        raise ObservationError("Short snapshot record")
    stamp = wire.record_stamp(payload)
    if stamp is None or stamp != after["publication"]:
        return None
    return after, payload


def read_stable(reader, pid: int) -> dict:
    """Copy a snapshot protected by its publication counter."""
    for _ in range(_READ_ATTEMPTS):
        candidate = _read_candidate(reader, pid)
        if candidate is not None:
            header, payload = candidate
            record = wire.decode_record(payload)
            record["header"] = header
            return record
    raise UnstableObservation("No stable snapshot")


class SnapshotChannel(MappedView):
    """Read-only view of the hook DLL's latest snapshot."""

    def __init__(self, pid: int):
        self.pid = pid
        self.size = wire.MAPPING_SIZE + wire.ENTITIES_CAPACITY
        super().__init__(wire.NAME.format(pid), FILE_MAP_READ, self.size)

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > self.size:
            raise ObservationError("Read outside the snapshot channel")
        return ctypes.string_at(self.address + offset, size)

    def read_matching(self, matches, failure_message):
        try:
            record = read_stable(self, self.pid)
        except UnstableObservation:
            record = None
        if record is not None and matches(record):
            return record
        raise ObservationError(failure_message)

    def snapshot_after_step(self, command: StepCommand, ack: InputAck, halted: GateStatus, *,
                            previous_generation: int, read_status) -> dict:
        """Read one snapshot and verify it belongs to the acknowledged command."""
        _require_valid_ack(ack, command)
        if halted.state != GateState.HALTED:
            raise ObservationError("A halted gate status is required after the step")
        if ack.snapshot_sequence != halted.snapshot_sequence:
            raise ObservationError("Gate status and acknowledgement reference different snapshots")
        expected = (command.session_id, command.sequence, 1)
        record = self.read_matching(
            lambda row: (row["session_id"], row["command_sequence"], row["completed_ticks"]) == expected,
            "The published snapshot does not match this acknowledgement",
        )
        if (not record["tick_valid"] or not record["ack_matched"] or
                record["capture_fault"] or record["ack_kind"] != ack.kind):
            raise ObservationError("Snapshot lacks a valid acknowledgement for a complete tick")
        if (record["requested_actions"] != command.actions or
                record["calc_post_sequence"] != ack.snapshot_sequence or
                ack.calc_callback_id != record["calc_callback_id"]):
            raise ObservationError("Snapshot does not match the acknowledged command")
        if record["generation"] != previous_generation + 2:
            raise ObservationError("Snapshot generation skipped or regressed")
        _require_still_halted(record, read_status, "step")
        return record


class SnapshotReady:
    """Wait for the hook DLL to finish publishing a snapshot."""

    def __init__(self, pid: int):
        kernel = self.kernel = process_api()
        kernel.OpenEventW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
        kernel.OpenEventW.restype = ctypes.c_void_p
        kernel.WaitForMultipleObjects.argtypes = [
            ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p), ctypes.c_int, ctypes.c_uint32,
        ]
        kernel.WaitForMultipleObjects.restype = ctypes.c_uint32
        event = kernel.OpenEventW(SYNCHRONIZE, False, READY_EVENT_NAME.format(pid))
        if not event:
            raise OSError(ctypes.get_last_error(), "Could not open the snapshot-ready event. Rebuild the hook DLL.")
        with ExitStack() as resources:
            resources.enter_context(owned_handle(kernel, event))
            process = kernel.OpenProcess(SYNCHRONIZE, False, pid)
            if not process:
                raise OSError(ctypes.get_last_error(), "Could not open the game process for the snapshot wait")
            resources.enter_context(owned_handle(kernel, process))
            self.handles = (ctypes.c_void_p * 2)(process, event)
            self._resources = resources.pop_all()

    def wait(self, deadline: float) -> None:
        remaining_ms = max(0, math.ceil((deadline - time.monotonic()) * 1000))
        result = self.kernel.WaitForMultipleObjects(2, self.handles, False, remaining_ms)
        if result == 1:  # WAIT_OBJECT_0 + event index
            return
        if result == 0:
            raise RuntimeError("The TH10 process exited before publishing its snapshot")
        if result == WAIT_TIMEOUT:
            raise TimeoutError("Timed out waiting for the complete TH10 snapshot")
        raise ctypes.WinError(ctypes.get_last_error())

    def close(self) -> None:
        self._resources.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def require_matches_halted(record: dict, status: GateStatus) -> None:
    """Require a snapshot to describe the state the gate halted on."""
    if (record["calc_post_sequence"] != status.snapshot_sequence or
            record["stage_tick"] != status.stage_tick or
            record["segment_tick"] != status.segment_tick or
            record["player_pointer"] != status.player_pointer or
            abs(record["player_x"] - status.player_x) > _COORDINATE_TOLERANCE or
            abs(record["player_y"] - status.player_y) > _COORDINATE_TOLERANCE):
        raise ObservationError("Snapshot does not match the halted gate state")


def _require_still_halted(record, read_status, label):
    status = read_status()
    if status is None or status.state != GateState.HALTED:
        raise ObservationError(f"The gate left Halted while the {label} snapshot was captured")
    require_matches_halted(record, status)


def _require_valid_ack(ack: InputAck, command: StepCommand):
    terminal = ack.kind == AckKind.TERMINAL
    if (ack.version != ACK_VERSION or ack.size != ACK_FORMAT.size or
            ack.kind not in (AckKind.SNAPSHOT_COMPLETE, AckKind.TERMINAL) or ack.reason != 0 or
            ack.checks != (0 if terminal else REQUIRED_CHECKS)):
        raise ObservationError("The acknowledgement does not report a complete tick")
    if ack.session_id != command.session_id or ack.sequence != command.sequence:
        raise ObservationError("Acknowledgement identity mismatch")
    if ack.actions != command.actions:
        raise ObservationError("Acknowledgement action mismatch")
    if ack.player_callback_exits != (0 if terminal else 1):
        raise ObservationError("Acknowledgement does not confirm one completed tick")


def initial_snapshot(read_matching, halted: GateStatus, *, read_status) -> dict:
    """Validate the snapshot taken when the gate first halts, before any command."""
    record = read_matching(
        lambda row: not (row["session_id"] or row["command_sequence"])
        and row["calc_post_sequence"] == halted.snapshot_sequence,
        "The published snapshot is not the initial snapshot",
    )
    if (not record["tick_valid"] or record["ack_matched"] or
            record["capture_fault"] or record["completed_ticks"] or
            record["requested_actions"] or record["ack_kind"] != AckKind.NONE):
        raise ObservationError("Initial snapshot has command attribution or a capture fault")
    _require_still_halted(record, read_status, "initial")
    return record
