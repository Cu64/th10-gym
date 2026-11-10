"""Advance a game process one verified tick at a time."""

import secrets
import time

from .._snapshot.format import AckKind
from .._win32.process import process_alive
from . import gate as gate_module
from . import input_channel, snapshot_channel
from .gate import GateChannel, GateError, GateState, Heartbeat
from .input_channel import InputChannel, StepCommand
from .snapshot_channel import SnapshotChannel, SnapshotReady

_ACTION_TIMEOUT_MS = 2_000
_SNAPSHOT_TIMEOUT_SECONDS = 2
_STATUS_POLL_SECONDS = 0.002
_RESETTING_POLL_SECONDS = 0.005
_SNAPSHOT_FAILURES = {
    GateError.SNAPSHOT_PUBLICATION: "TH10 could not publish its snapshot",
    GateError.SNAPSHOT_NOTIFICATION: "TH10 published its snapshot but could not signal the ready event",
}


class TickStepper:
    """Own the shared-memory channels and the latest verified snapshot."""

    def __init__(self, pid, atomic, resources):
        self.pid = pid
        self.record = None
        self._atomic = atomic
        self._session_id = secrets.randbelow(0xFFFFFFFE) + 1
        self._input_sequence = 0
        self._gate_sequence = 0
        self._gate = resources.enter_context(GateChannel(pid, atomic=atomic))
        self._input = resources.enter_context(InputChannel(pid, atomic=atomic))
        self._snapshots = resources.enter_context(SnapshotChannel(pid))
        self._snapshot_ready = resources.enter_context(SnapshotReady(pid))
        self._heartbeat = Heartbeat(self._gate, self._input)
        resources.callback(self._heartbeat.close)

    def arm(self, deadline):
        """Take control of a running game and capture its first halted snapshot."""
        self._gate_sequence += 1
        self._gate.publish_enable(self._gate_sequence)
        self._capture_initial(deadline)

    def wait_for_gameplay_progress(self, deadline, *, after_sequence=0):
        gate_module.wait_for_gameplay_progress(self.pid, self._atomic, deadline, after_sequence=after_sequence)

    def check_halted(self):
        """Confirm the gate still holds the latest snapshot's state."""
        self._heartbeat.check()
        status = self._gate.read_status()
        if not status or status.state != GateState.HALTED or status.error:
            raise RuntimeError("The TH10 tick gate is no longer halted")
        snapshot_channel.require_matches_halted(self.record, status)
        return status

    def advance(self, input_mask, before):
        """Run one tick with `input_mask` and return its verified snapshot."""
        self._input_sequence += 1
        command = StepCommand(self._session_id, self._input_sequence, input_mask)
        input_channel.publish_command(self._input, command, _ACTION_TIMEOUT_MS)
        ack = input_channel.wait_for_ack(self._input, command, _ACTION_TIMEOUT_MS)
        if ack is None or ack.kind not in (AckKind.SNAPSHOT_COMPLETE, AckKind.TERMINAL):
            raise RuntimeError(f"TH10 action did not produce a verified tick: {ack!r}")
        status = self._wait_snapshot(time.monotonic() + _SNAPSHOT_TIMEOUT_SECONDS)
        if status.pass_count != (before.pass_count + 1) & 0xFFFFFFFF:
            raise RuntimeError("TH10 did not complete exactly one gate pass")
        self.record = self._snapshots.snapshot_after_step(
            command, ack, status, previous_generation=self.record["generation"], read_status=self._gate.read_status,
        )
        return self.record

    def wait_halted(self, deadline):
        """Return the status of a halted gate, failing on a fault or a dead game."""
        while time.monotonic() < deadline:
            self._heartbeat.check()
            if not process_alive(self.pid):
                raise RuntimeError("The TH10 process exited")
            status = self._gate.read_status()
            if status and status.command_sequence == self._gate_sequence:
                if status.error or status.state == GateState.FAULT:
                    raise RuntimeError(f"TH10 tick gate fault: {status}")
                if status.state == GateState.HALTED:
                    return status
            time.sleep(_STATUS_POLL_SECONDS)
        raise TimeoutError("Timed out waiting for a halted TH10 tick")

    def release_for_reset(self, deadline):
        """Let the game run freely, within the gate's reset limit, so the bridge can restart it."""
        self._gate_sequence += 1
        self._gate.publish_reset_begin(self._gate_sequence)
        while time.monotonic() < deadline:
            if not process_alive(self.pid):
                raise RuntimeError("The TH10 process exited during the reset")
            self._heartbeat.beat()
            status = self._gate.read_status()
            if status is not None and status.command_sequence == self._gate_sequence:
                if status.state == GateState.FAULT:
                    raise RuntimeError(f"TH10 tick gate fault {status.error}")
                if status.state == GateState.RESETTING:
                    if status.error:
                        raise RuntimeError(f"TH10 tick gate reports error {status.error}")
                    return
            time.sleep(_RESETTING_POLL_SECONDS)
        raise TimeoutError("TH10 tick gate did not enter its reset state")

    def rearm_after_reset(self, expected_ticks, deadline):
        """Take control again from the ticks just read and capture the new start."""
        self._gate_sequence += 1
        self._gate.publish_reset_rearm(self._gate_sequence, expected_ticks)
        self._capture_initial(deadline)

    def _capture_initial(self, deadline):
        status = self._wait_snapshot(deadline)
        self.record = snapshot_channel.initial_snapshot(
            self._snapshots.read_matching, status, read_status=self._gate.read_status,
        )

    def _wait_snapshot(self, deadline):
        try:
            self._snapshot_ready.wait(deadline)
        except TimeoutError as timeout:
            if message := self._snapshot_failure(self._gate.read_status()):
                raise RuntimeError(message) from timeout
            raise
        self._heartbeat.check()
        status = self._gate.read_status()
        if message := self._snapshot_failure(status):
            raise RuntimeError(message)
        if (not status or status.command_sequence != self._gate_sequence or status.error
                or status.state != GateState.HALTED):
            raise RuntimeError(f"Snapshot-ready event lacks a valid halted gate status: {status!r}")
        return status

    def _snapshot_failure(self, status):
        if status and status.command_sequence == self._gate_sequence:
            return _SNAPSHOT_FAILURES.get(status.error)
        return None
