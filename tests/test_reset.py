"""Exercise reset routing and the bridge mailbox wire format."""

import ctypes
import struct
import time
from contextlib import ExitStack, nullcontext
from pathlib import Path
from types import SimpleNamespace

import pytest

from th10_gym._scene import protocol as _scene_protocol
from th10_gym._scene import restart as _reset
from th10_gym._scene.profile import request_from_options
from th10_gym._session import session as session_module
from th10_gym._snapshot import observation as _observation
from th10_gym._stepping import gate as _step
from th10_gym._win32 import mapping as mapped_memory
from th10_gym.env import TH10Env


def test_readiness_check_ignores_stale_and_uncommitted_samples(monkeypatch):
    pid = 1234
    player = 0x400000
    region = ctypes.create_string_buffer(36)
    base = ctypes.addressof(region)
    samples = iter([
        (2, 10, player, 1, 1),  # Last sequence before Retry.
        (3, 11, player, 2, 2),  # Publication still in progress.
        (4, 12, player, 3, 3),
        (6, 13, player + 4, 4, 4),  # A different player cannot confirm progress.
        (8, 14, player + 4, 5, 5),
    ])
    observed = []

    def publish_next(_seconds):
        stamp, sequence, pointer, counter_a, counter_b = next(samples)
        payload = struct.pack("<IHHIIi4I", 0x49303154, 1, 36, pid, 1,
                              stamp, sequence, pointer, counter_a, counter_b)
        ctypes.memmove(base, payload, len(payload))
        observed.append(sequence)

    kernel = SimpleNamespace(
        OpenFileMappingW=lambda access, inherit, name: 1,
        MapViewOfFile=lambda handle, access, high, low, size: base if size == 36 else 0,
        UnmapViewOfFile=lambda address: True,
        CloseHandle=lambda handle: True,
    )
    monkeypatch.setattr(mapped_memory, "mapping_api", lambda: kernel)
    monkeypatch.setattr(_step, "time", SimpleNamespace(monotonic=lambda: 0, sleep=publish_next))
    atomic = SimpleNamespace(load32=lambda address: ctypes.c_int32.from_address(address).value)
    publish_next(0)

    _step.wait_for_gameplay_progress(pid, atomic, 1, after_sequence=10)

    assert observed == [10, 11, 12, 13, 14]


def test_audio_shutdown_failure_still_closes_the_game(monkeypatch):
    from th10_gym._win32 import audio as _audio

    calls = []
    audio_error = RuntimeError("audio shutdown failed")

    def close_audio():
        raise audio_error

    monkeypatch.setattr(_audio, "ProcessAudioMute",
                        lambda pid: SimpleNamespace(close=close_audio, status={}))
    session = session_module.GameSession.__new__(session_module.GameSession)
    session.pid = 1234
    session._resources = ExitStack()
    session._resources.callback(calls.append, "mapping")
    session._audio_muter = None
    session._close_game = lambda: calls.append("game")
    session.set_muted(True)

    with pytest.raises(RuntimeError) as caught:
        session.close()

    assert caught.value is audio_error
    assert calls == ["mapping", "game"]


@pytest.mark.parametrize("reuse, failure, route", [
    (True, None, "retail_retry"),
    (True, "bridge timeout", "fresh_process"),
    (True, "unknown gate state 99", "fresh_process"),
    (False, None, "fresh_process"),
])
@pytest.mark.parametrize("mode", ["normal", "practice"])
def test_gym_reuses_options_and_reports_reset_route(monkeypatch, reuse, failure, route, mode):
    calls = []
    start = dict(player_state=1, lives=2, power=0, bomb_pointer=0, bomb_active=0,
                 player_y=400, score=0, faith=5000, boss_slots=[])

    class Session:
        restart = session_module.GameSession.restart

        def __init__(self, *args, **kwargs):
            self.pid = len(calls) + 1

        def start(self, options):
            self.options = options
            self._request = request_from_options(options)
            calls.append(("start", options))
            return start

        def _restart(self, request):
            if request != self._request:
                self._request = request
                return start, "profile_change"
            calls.append(("restart", self.options))
            if failure == "bridge timeout":
                raise TimeoutError(failure)
            if failure:
                _step.parse_status(_step.STATUS_FORMAT.pack(99, *([0] * 10)))
            return start, "retail_retry"

        def close(self):
            calls.append(("close", self.pid))

    monkeypatch.setattr(session_module, "GameSession", Session)
    monkeypatch.setattr(_observation, "observation", lambda record: record["player_state"])
    monkeypatch.setattr(_observation, "diagnostics", lambda record, _: record)
    options = {"mode": mode, "character": "marisa"}
    with TH10Env(game_exe="fixture", native_dir="fixture", launcher="fixture",
                 reuse_practice_process=reuse, reuse_normal_process=reuse) as env:
        _, first = env.reset(options=options)
        _, second = env.reset()
        assert first["reset_route"] == "fresh_process"
        assert second["reset_route"] == route
        assert (first["pid"] == second["pid"]) == (route == "retail_retry")
        assert second.get("reset_fallback") == failure
        assert [passed for operation, passed in calls if operation != "close"] == (
            [options] * (3 if failure else 2)
        )
        _, normal = env.reset(options={})
        assert normal["reset_route"] == ("profile_change" if reuse else "fresh_process")


@pytest.mark.parametrize("synthetic_polls", [0, 1])
@pytest.mark.parametrize("outcome, route", [
    (0, _reset.RETAIL_RETRY),
    (_reset.GAME_OVER, _reset.GAME_OVER_RETRY),
    (_reset.PRACTICE_COMPLETE, _reset.PRACTICE_COMPLETE_RETRY),
    (None, _reset.PROFILE_CHANGE),
])
def test_restart_wait_requires_route_specific_ack_in_production_mapping(
        monkeypatch, synthetic_polls, outcome, route):
    fixture = Path(__file__).with_name("fixtures") / "thprac_bridge_restart.bin"
    view = _scene_protocol.Mapping.from_buffer_copy(fixture.read_bytes())
    view.applied_game_mode_flags = 0x10
    shared = SimpleNamespace(
        view=view, address=ctypes.addressof(view),
        kernel32=SimpleNamespace(GetTickCount64=lambda: 1_000),
        atomic=SimpleNamespace(exchange32=lambda addr, value: setattr(
            ctypes.c_int32.from_address(addr), "value", value)),
    )
    monkeypatch.setattr(_reset, "submit_mutex", lambda _: nullcontext())
    monkeypatch.setattr(_reset, "SharedMapping", lambda *a, **kw: nullcontext(shared))
    request = SimpleNamespace(request_id=_scene_protocol.read_snapshot(view)["request_id"],
                              section_kind=0)
    if outcome is not None:
        assert _reset.restart_route({"terminal_outcome": outcome}) == route
    before, sequence = _reset.queue_restart(7, shared.atomic, request, route)
    assert sequence == 2 and view.restart_deadline_tick_ms == 5_000
    view.restart_response_sequence = sequence
    view.restart_status = 6  # Action queued, awaiting fresh startup.

    def finish_startup(_):
        if route == _reset.PROFILE_CHANGE:
            view.restart_status = 7
            view.restart_phase = 6
            view.restart_synthetic_input_polls = synthetic_polls
            return
        if view.restart_status == 6:
            view.restart_status = 8
            view.restart_phase = 4
        else:
            view.last_update_tick_ms += 1
            view.restart_synthetic_input_polls = synthetic_polls

    monkeypatch.setattr(_reset.time, "sleep", finish_startup)
    if synthetic_polls:
        with pytest.raises(_reset.SceneResetError, match="injected input"):
            _reset.wait_for_restart(7, shared.atomic, before, route, sequence, time.monotonic() + 1)
    else:
        fresh = _reset.wait_for_restart(7, shared.atomic, before, route, sequence, time.monotonic() + 1)
        assert fresh["last_update_tick_ms"] == before["last_update_tick_ms"] + (outcome is not None)


def test_restart_preserves_identity_only_for_same_resolved_profile(monkeypatch):
    session = object.__new__(session_module.GameSession)
    session._request = request_from_options({"mode": "practice", "life": 2})
    monkeypatch.setattr(session, "_restart", lambda request: request)
    assert session.restart({"mode": "practice", "life": 2, "power": 0}) is session._request
    changed = session.restart({"mode": "practice", "life": 1})
    assert changed.life == 1 and changed.request_id != session._request.request_id
    normal = session.restart({})
    assert not _reset.is_practice(normal) and normal.request_id != session._request.request_id


def test_dialogue_steps_publish_and_release_the_owned_scene_lease(monkeypatch):
    fixture = Path(__file__).with_name("fixtures") / "thprac_bridge_restart.bin"
    view = _scene_protocol.Mapping.from_buffer_copy(fixture.read_bytes())
    view.request_sequence = view.response_sequence = 3
    view.status = 5
    atomic = SimpleNamespace(exchange32=lambda address, value: setattr(
        ctypes.c_int32.from_address(address), "value", value))
    shared = SimpleNamespace(view=view, atomic=atomic,
                             kernel32=SimpleNamespace(GetTickCount64=lambda: 1_000))
    monkeypatch.setattr(session_module.scene_client, "submit_mutex", lambda _: nullcontext())
    monkeypatch.setattr(session_module.scene_client, "SharedMapping", lambda *a, **kw: nullcontext(shared))
    session = object.__new__(session_module.GameSession)
    session.pid = 7
    session._atomic = atomic
    session._request = SimpleNamespace(request_id=_scene_protocol.read_snapshot(view)["request_id"])
    session._dialogue_renew_at = 0
    session._stepper = SimpleNamespace(record={"dialogue_active": False, "terminal_outcome": 0})
    with pytest.raises(RuntimeError, match="requires active TH10 dialogue"):
        session.step_dialogue()
    session._stepper.record["dialogue_active"] = True
    monkeypatch.setattr(session, "step", lambda mask: mask)
    assert session.step_dialogue() == 0
    assert (view.dialogue_lease_ms, view.dialogue_lease_deadline_tick_ms,
            view.dialogue_lease_request_sequence) == (10_000, 11_000, 3)
    assert bytes(view.dialogue_lease_request_id) == bytes(view.request_id)
    generation = view.dialogue_lease_generation
    assert session.step_dialogue() == 0 and view.dialogue_lease_generation == generation
    session.end_dialogue()
    assert (view.dialogue_lease_ms, view.dialogue_lease_deadline_tick_ms,
            view.dialogue_lease_generation) == (0, 0, generation + 2)


def test_reset_commands_publish_expected_ticks_with_the_command(monkeypatch):
    mailbox = ctypes.create_string_buffer(_step.CHANNEL_SIZE)
    mapping = object.__new__(_step.GateChannel)
    mapping.base = ctypes.addressof(mailbox)
    publications = []

    def exchange(address, value):
        if address == mapping.base + _step.COMMAND_COUNTER_OFFSET and value % 2 == 0:
            publications.append((
                value,
                _step.COMMAND_FORMAT.unpack_from(mailbox, _step.COMMAND_OFFSET),
                struct.unpack_from("<II", mailbox, _step.EXPECTED_TICKS_OFFSET),
            ))
        ctypes.c_uint32.from_address(address).value = value

    mapping.atomic = SimpleNamespace(
        load32=lambda address: ctypes.c_uint32.from_address(address).value,
        exchange32=exchange,
    )
    monkeypatch.setattr(_step.seqlock, "tick_count_ms", lambda _: 100)
    assert mapping.publish_reset_begin(2) == 2
    assert mapping.publish_reset_rearm(3, (3, 3)) == 4
    assert publications == [
        (2, (2, 1, 100, 5), (0, 0)),
        (4, (3, 1, 100, 9), (3, 3)),
    ]
