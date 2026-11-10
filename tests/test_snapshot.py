"""Decode native snapshot bytes and tie them to the gate and input acknowledgement."""

import ctypes
import os
import struct
import time
from dataclasses import replace
from types import SimpleNamespace
from unittest.mock import patch

import pytest
from wire_fixtures import PID, FakeReader, ack, decoded, halted_status, header_bytes, record_bytes, snapshot_bytes

from th10_gym._snapshot import format as wire
from th10_gym._stepping import snapshot_channel, stepper
from th10_gym._stepping.gate import GateError, GateState
from th10_gym._stepping.input_channel import StepCommand
from th10_gym._stepping.stepper import TickStepper


class SnapshotReader(FakeReader):
    read_matching = snapshot_channel.SnapshotChannel.read_matching
    snapshot_after_step = snapshot_channel.SnapshotChannel.snapshot_after_step


def read_matching_for(body):
    return SnapshotReader(body).read_matching


def still_halted(status=None):
    return lambda: status or halted_status()


def test_header_rejects_wrong_process_and_unfinished_publication():
    with pytest.raises(wire.ObservationError, match="PID"):
        wire.decode_header(header_bytes(pid=PID + 1), PID)
    with pytest.raises(wire.UnstableObservation, match="progress"):
        wire.decode_header(header_bytes(publication=3), PID)


@pytest.mark.parametrize("version", [1, 2])
def test_older_formats_are_rejected(version):
    data = record_bytes()
    struct.pack_into("<I", data, 8, version)
    with pytest.raises(wire.ObservationError, match="version"):
        wire.decode_record(bytes(data))
    with pytest.raises(wire.ObservationError, match="layout mismatch"):
        snapshot_channel.read_stable(FakeReader(data, head=header_bytes(version=version)), PID)


def test_torn_record_stamps_are_rejected():
    data = record_bytes()
    struct.pack_into("<I", data, wire.STAMP_END_OFFSET, 4)
    with pytest.raises(wire.ObservationError, match="stamps"):
        wire.decode_record(bytes(data))


def test_record_preserves_group_status():
    item = decoded(read_fault=wire.GROUP_PLAYER)
    assert item["groups"]["read_fault"] == wire.GROUP_PLAYER
    assert item["groups"]["available"] == 0
    assert item["boss_slot0"] == 0 and item["policy_ready"] is False
    assert (item["stage_tick"], item["segment_tick"]) == (100, 200)


def test_invalid_group_cannot_be_marked_valid():
    with pytest.raises(wire.ObservationError, match="Inconsistent"):
        decoded(available=wire.GROUP_PLAYER, valid=wire.GROUP_PLAYER, unsupported=wire.GROUP_PLAYER)


def test_truncated_bullets_keep_their_status():
    item = decoded(available=wire.GROUP_BULLETS, truncated=wire.GROUP_BULLETS, valid=wire.GROUP_BULLETS,
                   bullet_scanned=2000, bullet_captured=64)
    assert len(item["bullets"].rows) == 64
    assert item["groups"]["truncated"] == wire.GROUP_BULLETS


@pytest.mark.parametrize("offset, fmt, value, error", [
    (0, "<I", wire.BULLET_COUNT, "Invalid bullet slot"),
    (6, "<H", 1, "reserved field"),
    (wire.BULLET.size, "<I", 0, "Duplicate bullet slot"),
    (12, "<I", 0x7FC00000, "Non-finite"),
])
def test_bullet_rows_are_validated(offset, fmt, value, error):
    data = bytearray(snapshot_bytes(bullet_scanned=2, bullet_captured=2))
    struct.pack_into(fmt, data, wire.BULLET_OFFSET + offset, value)
    with pytest.raises(wire.ObservationError, match=error):
        wire.decode_record(bytes(data))


def test_bullets_stay_packed_until_a_callback_needs_rows():
    item = decoded(bullet_scanned=2, bullet_captured=2)
    assert isinstance(item["bullets"], wire.PackedBullets)
    wire.materialize_bullets(item)
    assert [row["slot"] for row in item["bullets"]] == [0, 1]
    assert item["bullets"][0]["spawn_ready"] == 0


def test_dialogue_is_active_only_when_the_game_reports_it():
    assert decoded()["dialogue_active"] is None  # No message owner in this snapshot.


def test_stable_read_retries_across_a_new_publication_and_keeps_long_laser_lists():
    laser_rows = struct.pack("<4I7I", 1, 0x46DA60, 2, 1, 0x7FC01234, *([0] * 6))
    for node in range(2, 301):
        laser_rows += struct.pack("<4I7f", node, 0x46DA60, node + 1 if node < 300 else 0, 1, *([0.0] * 7))
    fields = dict(bullet_scanned=2, bullet_captured=2, stage=6, laser_manager=0x410000,
                  laser_rows=laser_rows, laser_count=300)
    torn = snapshot_bytes(generation=2, **fields)
    coherent = snapshot_bytes(generation=4, **fields)
    entities_size = len(torn) - wire.RECORD_SIZE
    first, second = header_bytes(2, entities_size=entities_size), header_bytes(4, entities_size=entities_size)
    reads = iter([first, torn, second, second, coherent, second])
    reader = SimpleNamespace(read=lambda _offset, _size: next(reads))

    item = snapshot_channel.read_stable(reader, PID)

    assert (item["generation"], item["stage"], item["deathbomb_remaining"]) == (4, 6, 5)
    assert item["laser_captured"] == 300
    assert item["groups"]["available"] & wire.GROUP_LASERS
    assert not item["groups"]["valid"] & wire.GROUP_LASERS
    assert item["lasers"][0]["nonfinite_bits"] == {"x": 0x7FC01234}
    assert item["lasers"][-1]["node"] == 300
    with pytest.raises(wire.ObservationError, match="counts or size"):
        wire.decode_record(coherent[:-1])


def _after_step(reader, input_ack, status, **fields):
    options = dict(previous_generation=0, read_status=still_halted())
    return reader.snapshot_after_step(StepCommand(3, 9, 0x80), input_ack, status, **(options | fields))


def test_step_snapshot_requires_ack_and_halted_identity():
    reader = SnapshotReader(snapshot_bytes())
    assert _after_step(reader, ack(), halted_status())["command_sequence"] == 9
    with pytest.raises(wire.ObservationError, match="identity"):
        _after_step(reader, ack(sequence=8), halted_status())
    with pytest.raises(wire.ObservationError, match="action mismatch"):
        _after_step(reader, ack(actions=0), halted_status())
    with pytest.raises(wire.ObservationError, match="generation"):
        _after_step(reader, ack(), halted_status(), previous_generation=4)
    with pytest.raises(wire.ObservationError, match="halted gate status"):
        _after_step(reader, ack(), halted_status(state=GateState.RUNNING))
    with pytest.raises(wire.ObservationError, match="does not match the halted"):
        _after_step(reader, ack(), halted_status(), read_status=still_halted(halted_status(stage_tick=101)))
    with pytest.raises(wire.ObservationError, match="left Halted"):
        _after_step(reader, ack(), halted_status(),
                    read_status=still_halted(halted_status(state=GateState.RUNNING)))


def test_initial_snapshot_requires_no_command_attribution():
    fields = dict(session_id=0, command=0, completed=0, ack_kind=wire.AckKind.NONE, requested_actions=0,
                  record_flags=wire.TICK_VALID)
    read_matching = read_matching_for(snapshot_bytes(**fields))
    record = snapshot_channel.initial_snapshot(read_matching, halted_status(), read_status=still_halted())
    assert record["generation"] == 2
    with pytest.raises(wire.ObservationError, match="initial"):
        snapshot_channel.initial_snapshot(read_matching_for(snapshot_bytes(session_id=3)), halted_status(),
                                          read_status=still_halted())


def test_terminal_snapshot_does_not_claim_a_player_callback():
    fields = dict(ack_kind=wire.AckKind.TERMINAL, input_current=0, outcome=wire.PRACTICE_COMPLETE)
    terminal_ack = ack(kind=wire.AckKind.TERMINAL, checks=0, exits=0)
    reader = SnapshotReader(snapshot_bytes(**fields))
    record = _after_step(reader, terminal_ack, halted_status())
    assert record["terminal_outcome"] == wire.PRACTICE_COMPLETE
    assert record["completed_ticks"] == 1 and record["consumed_input"] is None
    with pytest.raises(wire.ObservationError, match="one completed tick"):
        _after_step(reader, replace(terminal_ack, player_callback_exits=1), halted_status())
    with pytest.raises(wire.ObservationError, match="terminal outcome"):
        decoded(ack_kind=wire.AckKind.TERMINAL, input_current=0, outcome=0)


@pytest.mark.skipif(os.name != "nt", reason="Windows snapshot event")
def test_snapshot_ready_wait_consumes_a_real_auto_reset_event():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateEventW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_wchar_p]
    kernel.CreateEventW.restype = ctypes.c_void_p
    kernel.SetEvent.argtypes = [ctypes.c_void_p]
    kernel.SetEvent.restype = ctypes.c_int
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel.CloseHandle.restype = ctypes.c_int
    pid = os.getpid()
    event = kernel.CreateEventW(None, False, False, snapshot_channel.READY_EVENT_NAME.format(pid))
    assert event
    try:
        with snapshot_channel.SnapshotReady(pid) as ready:
            with pytest.raises(TimeoutError):
                ready.wait(time.monotonic())
            assert kernel.SetEvent(event)
            ready.wait(time.monotonic() + 1)
            with pytest.raises(TimeoutError):
                ready.wait(time.monotonic())
    finally:
        kernel.CloseHandle(event)


def bare_stepper(**fields):
    instance = object.__new__(TickStepper)
    instance.pid, instance._session_id, instance._input_sequence, instance._gate_sequence = PID, 3, 8, 1
    instance._heartbeat = SimpleNamespace(check=lambda: None)
    instance._input = object()
    for name, value in fields.items():
        setattr(instance, name, value)
    return instance


def test_step_waits_for_publication_before_validating_the_snapshot():
    before = halted_status()
    after = halted_status(pass_count=16, snapshot_sequence=94, stage_tick=101, segment_tick=201)
    instance = bare_stepper(record=decoded(), _snapshots=SnapshotReader(snapshot_bytes()),
                            _gate=SimpleNamespace(read_status=lambda: before))

    def signal(_deadline):
        instance._gate.read_status = lambda: after
        instance._snapshots = SnapshotReader(
            snapshot_bytes(generation=4, calc_post_sequence=94, stage_tick=101, segment_tick=201), publication=4,
        )

    instance._snapshot_ready = SimpleNamespace(wait=signal)
    with (patch.object(stepper.input_channel, "publish_command") as publish,
          patch.object(stepper.input_channel, "wait_for_ack", return_value=ack(snapshot_sequence=94)) as wait):
        assert instance.advance(0x80, before)["generation"] == 4
    publish.assert_called_once_with(instance._input, StepCommand(3, 9, 0x80), stepper._ACTION_TIMEOUT_MS)
    assert wait.call_args.args[1] is publish.call_args.args[1]


@pytest.mark.parametrize(("sequence", "error", "expected"), [
    (12, GateError.SNAPSHOT_PUBLICATION, "could not publish"),
    (12, GateError.SNAPSHOT_NOTIFICATION, "could not signal"),
    (11, GateError.SNAPSHOT_NOTIFICATION, None),
])
def test_snapshot_timeout_reports_current_native_fault(sequence, error, expected):
    timeout = TimeoutError("no snapshot-ready event")

    def wait(_deadline):
        raise timeout

    status = halted_status(command_sequence=sequence, error=error)
    instance = bare_stepper(_gate_sequence=12, _snapshot_ready=SimpleNamespace(wait=wait),
                            _gate=SimpleNamespace(read_status=lambda: status))
    if expected is None:
        with pytest.raises(TimeoutError) as caught:
            instance._wait_snapshot(1)
        assert caught.value is timeout
    else:
        with pytest.raises(RuntimeError, match=expected) as caught:
            instance._wait_snapshot(1)
        assert caught.value.__cause__ is timeout
