"""Exercise the public Gymnasium API against a scripted session."""

import errno
import hashlib
import os
import stat
import struct
from pathlib import Path
from types import SimpleNamespace

import gymnasium as gym
import numpy as np
import pytest
from wire_fixtures import decoded

from th10_gym import actions as gym_actions
from th10_gym._scene import game_files
from th10_gym._scene import restart as reset_protocol
from th10_gym._scene.profile import request_from_options
from th10_gym._session import session as session_module
from th10_gym._session.session import GameSession
from th10_gym._snapshot import format as raw
from th10_gym._snapshot import observation as _observation
from th10_gym._stepping.gate import GateState, GateStatus
from th10_gym._win32 import loader
from th10_gym._win32.mapping import FILE_MAP_READ, MappedView
from th10_gym._win32.render import DRAW_ENTRY, DRAW_ORIGINAL, DRAW_SKIP, DrawSkip
from th10_gym.env import TH10Env
from th10_gym.readers import boss, bullets, lasers, memory


def make_env(**options):
    return TH10Env(game_exe="fixture", native_dir="fixture", launcher="fixture", **options)


def test_draw_skip_restores_the_checked_code_and_rejects_changes(monkeypatch):
    code = bytearray(DRAW_ORIGINAL)
    writes = []

    def read(address, size):
        assert address == DRAW_ENTRY
        return bytes(code[:size])

    def write(patch):
        writes.append(patch)
        code[:len(patch)] = patch

    toggle = DrawSkip(7, Path("game"), SimpleNamespace(read=read))
    monkeypatch.setattr(toggle, "_write", write)
    toggle.set_skipped(True)
    toggle.set_skipped(True)
    toggle.set_skipped(False)
    assert writes == [DRAW_SKIP, DRAW_ORIGINAL]
    assert bytes(code) == DRAW_ORIGINAL

    code[0] = 0xCC
    with pytest.raises(RuntimeError, match="changed before render toggle"):
        toggle.set_skipped(True)
    assert writes == [DRAW_SKIP, DRAW_ORIGINAL]


@pytest.mark.skipif(os.name != "nt", reason="Windows game workspace")
@pytest.mark.parametrize("route", [
    "hardlink", "cross_volume", "read_only", "dxvk_release", "dxvk_custom", "dxvk_wrong_hash", "dxvk_x64",
])
def test_game_workspace_shares_only_immutable_archives(monkeypatch, tmp_path, route):
    source = tmp_path / "source"
    source.mkdir()
    archives = ("th10.dat", "thbgm.dat")
    private = ("th10.exe", "vpatch_th10.dll", "th10.cfg", "vpatch.ini")
    for filename in (*archives, *private):
        (source / filename).write_text(filename, encoding="ascii")
    monkeypatch.setattr(game_files, "VPATCH_SHA256", hashlib.sha256(b"vpatch_th10.dll").hexdigest())
    monkeypatch.setattr(session_module.tempfile, "tempdir", str(tmp_path))
    if route == "cross_volume":
        def cross_volume(*_):
            raise OSError(errno.EXDEV, "different volume")

        monkeypatch.setattr(game_files.os, "link", cross_volume)
    readonly = source / "thbgm.dat"
    if route == "read_only":
        readonly.chmod(stat.S_IREAD)
    dxvk_dll = None
    dxvk_sha256 = None
    if route.startswith("dxvk_"):
        dxvk_dll = tmp_path / "renderer.dll"
        image = bytearray(0x40 + 24 + 96)
        image[:2] = b"MZ"
        struct.pack_into("<I", image, 0x3C, 0x40)
        image[0x40:0x44] = b"PE\0\0"
        struct.pack_into("<H", image, 0x44, 0x8664 if route == "dxvk_x64" else 0x14C)
        struct.pack_into("<HHH", image, 0x54, 96, 0x2000, 0x10B)
        dxvk_dll.write_bytes(image)
        digest = hashlib.sha256(image).hexdigest()
        if route == "dxvk_release":
            monkeypatch.setattr(session_module, "DXVK_D3D9_SHA256", digest)
        elif route in ("dxvk_custom", "dxvk_x64"):
            dxvk_sha256 = digest.upper()
    session = GameSession(source / "th10.exe", source, source / "th10.exe",
                             dxvk_dll=dxvk_dll, dxvk_sha256=dxvk_sha256)
    try:
        try:
            if route in ("dxvk_wrong_hash", "dxvk_x64"):
                message = "SHA-256 mismatch" if route == "dxvk_wrong_hash" else "32-bit Windows DLL"
                with pytest.raises(ValueError, match=message):
                    session._copy_game()
                assert session._temp is None
                return
            session._copy_game()
            workspace = session._game.parent
            renderer = workspace / "d3d9.dll"
            if dxvk_dll is not None:
                assert renderer.read_bytes() == dxvk_dll.read_bytes()
                assert not renderer.samefile(dxvk_dll)
            else:
                assert not renderer.exists()
            for filename in (*archives, *private):
                original, isolated = source / filename, workspace / filename
                linked = filename in archives and route != "cross_volume" and (
                    route != "read_only" or original != readonly
                )
                assert original.samefile(isolated) == linked
                assert original.read_bytes() == isolated.read_bytes()
            (workspace / "th10.cfg").write_text("private configuration", encoding="ascii")
        finally:
            session.close()
        assert not workspace.exists()
        assert (source / "th10.cfg").read_text(encoding="ascii") == "th10.cfg"
        assert readonly.read_text(encoding="ascii") == "thbgm.dat"
        if route == "read_only":
            assert readonly.stat().st_file_attributes & stat.FILE_ATTRIBUTE_READONLY
    finally:
        if route == "read_only":
            readonly.chmod(stat.S_IWRITE)


@pytest.mark.parametrize("mode, count, bomb_id", [
    ("full_72", 72, 2), ("always_shoot_36", 36, 18),
    ("compact_19", 19, 18),
])
def test_action_modes_and_deathbomb_mask(mode, count, bomb_id):
    actions = gym_actions.actions_for_mode(mode)
    assert len(set(actions)) == count
    assert actions[bomb_id] == (2 if mode == "full_72" else 3)
    assert all(gym_actions.valid_actions(action) for action in actions)
    assert all(action & 1 for action in actions) if mode != "full_72" else actions[0] == 0
    record = dict(player_state=1, power=20, bomb_pointer=1, bomb_active=0, dialogue_active=False)
    assert gym_actions.action_mask(actions, record)[bomb_id]
    record.update(dialogue_active=True)
    assert not gym_actions.action_mask(actions, record)[bomb_id]
    record.update(player_state=4, state_timer_current=7)
    assert gym_actions.action_mask(actions, record)[bomb_id]
    for change in ({"state_timer_current": 8}, {"power": 19}, {"bomb_active": 1}, {"player_state": 2}):
        assert not gym_actions.action_mask(actions, record | change)[bomb_id]


@pytest.mark.parametrize("options, outcome, expected, masks", [
    ({"terminate_on_miss": True}, 0, (True, False, True, 1), [1]),
    ({"auto_advance_respawn": True}, 0, (False, False, True, 91), [1] + [0] * 90),
    ({"max_episode_steps": 1}, 1, (True, False, True, 1), [1]),
    ({}, 2, (False, True, False, 1), [1]),
    ({"max_episode_steps": 1}, 0, (False, True, True, 1), [1]),
])
def test_gym_lifecycle_flags_and_diagnostics(monkeypatch, options, outcome, expected, masks):
    start = dict(player_state=4, lives=1, power=20, bomb_pointer=3, bomb_active=0,
                 player_y=129, score=10, faith=5000, boss_slots=[], state_timer_current=7)
    end = start | dict(player_state=2 if outcome != 2 else 1, lives=1 if outcome == 2 else 0,
                       terminal_outcome=outcome, player_y=127, score=30, faith=4990)
    calls = []
    records = iter([
        end,
        *[end | dict(player_state=2, faith=3333)] * 44,
        *[end | dict(player_state=0, score=130, faith=3315)] * 45,
        end | dict(player_state=1, score=131, faith=3315),
    ])

    def step(mask):
        calls.append(mask)
        return next(records)

    session = SimpleNamespace(start=lambda _: start, step=step, close=lambda: None)
    monkeypatch.setattr(session_module, "GameSession", lambda *args, **kwargs: session)
    monkeypatch.setattr(_observation, "observation", lambda record: record["player_state"])
    monkeypatch.setattr(_observation, "diagnostics", lambda record, _: {"bomb_pointer": record["bomb_pointer"]})
    env = make_env(action_mode="compact_19", **options)
    try:
        _, reset_info = env.reset()
        assert reset_info["action_mask"][18] and not reset_info["control_locked"]
        _, _, terminated, truncated, info = env.step(0)
        assert (terminated, truncated, info["miss_occurred"], info["ticks_elapsed"]) == expected
        assert info["section_cleared"] == (outcome == 2)
        assert not info["bomb_used"] and info["crossed_collection_line"]
        delta_score, delta_faith = (121, -1685) if options.get("auto_advance_respawn") else (20, -10)
        assert isinstance(info["delta_score"], np.int64) and info["delta_score"] == delta_score
        assert isinstance(info["delta_faith"], np.int64) and info["delta_faith"] == delta_faith
        assert calls == masks
        if terminated or truncated:
            with pytest.raises(gym.error.ResetNeeded):
                env.step(0)
    finally:
        env.close()


@pytest.mark.parametrize("outcome", [0, raw.PRACTICE_COMPLETE])
def test_dialogue_returns_one_transition_with_all_updates(monkeypatch, outcome):
    start = dict(player_state=1, lives=0, power=100, bomb_pointer=1, bomb_active=0,
                 player_y=300, score=0, faith=5000, boss_slots=[], dialogue_active=False, terminal_outcome=0)
    message = start | dict(dialogue_active=True, score=10)
    end = start | dict(dialogue_active=message["dialogue_active"] if outcome else False,
                       score=40, faith=4980, terminal_outcome=outcome)
    records = iter([message, message | dict(score=20), end])
    calls = []

    def step(mask):
        calls.append(mask)
        return next(records)

    session = SimpleNamespace(start=lambda _: start, step=step, step_dialogue=lambda: step(0),
                              end_dialogue=lambda: calls.append("end"), close=lambda: None)
    monkeypatch.setattr(session_module, "GameSession", lambda *args, **kwargs: session)
    monkeypatch.setattr(_observation, "observation", lambda record: record["score"])
    monkeypatch.setattr(_observation, "diagnostics", lambda record, _: {})
    with make_env(action_mode="compact_19", reward_fn=lambda old, _, new: new["score"] - old["score"]) as env:
        env.reset()
        obs, reward, terminated, truncated, info = env.step(0)
        assert (obs, reward, terminated, truncated) == (40, 40, False, bool(outcome))
        assert (info["ticks_elapsed"], info["delta_score"], info["delta_faith"]) == (3, 40, -20)
        assert calls == [1, 0, 0, "end"]
        assert env._episode_steps == 1


@pytest.mark.parametrize("menu_ready, valid_resources, terminated, advance_game_over", [
    (False, True, True, False), (True, True, True, False), (False, False, False, False),
    (False, True, True, True),
])
def test_last_life_ends_episode_and_retry_still_requires_menu(
        monkeypatch, menu_ready, valid_resources, terminated, advance_game_over):
    def snapshot(lives, outcome=0, valid=True):
        fields = dict(available=raw.GROUP_RESOURCES, valid=raw.GROUP_RESOURCES if valid else 0)
        if outcome:
            fields.update(ack_kind=0)
        record = decoded(lives=lives, player_state=2 if lives == -1 else 1, outcome=outcome, **fields)
        return record | dict(dialogue_active=False, score=0, faith=5000, boss_slots=[])

    calls = []
    options = {"mode": "practice", "life": 0}

    class FixtureSession(GameSession):
        def __init__(self, *_args, **_kwargs):
            self._request = request_from_options(options)
            self._draw_skip = None
            self.memory = SimpleNamespace(read=lambda *_: b"\0\0")
            self.pid, self._atomic = 7, None
            self._stepper = SimpleNamespace(record=None, wait_halted=self._wait_halted)

        def start(self, _options):
            calls.append("start")
            self._stepper.record = snapshot(0)
            return self._stepper.record

        def step(self, mask):
            calls.append(mask)
            outcome = raw.GAME_OVER if menu_ready or (advance_game_over and mask == 0) else 0
            self._stepper.record = snapshot(-1, outcome, valid_resources)
            return self._stepper.record

        def _wait_halted(self, _deadline):
            assert menu_ready or advance_game_over
            record = self._stepper.record
            return GateStatus(GateState.HALTED, 1, 1, 0, record["calc_post_sequence"], record["stage_tick"],
                              record["segment_tick"], record["player_pointer"], record["player_x"],
                              record["player_y"], 0)

        def close(self):
            calls.append("close")

    def queue_restart(_pid, _atomic, _request, route):
        calls.append(route)
        raise reset_protocol.SceneResetError("offline Retry stops after checking its route")

    monkeypatch.setattr(session_module, "GameSession", FixtureSession)
    monkeypatch.setattr(reset_protocol, "queue_restart", queue_restart)
    monkeypatch.setattr(_observation, "observation", lambda record: record["lives"])
    monkeypatch.setattr(_observation, "diagnostics", lambda record, _: {"terminal_outcome": record["terminal_outcome"]})
    with make_env(action_mode="always_shoot_36", auto_advance_respawn=valid_resources,
                  advance_game_over=advance_game_over) as env:
        env.reset(options=options)
        obs, _, done, truncated, info = env.step(0)
        ready = menu_ready or advance_game_over
        steps = [1, 0] if advance_game_over else [1]
        assert (obs, done, truncated, info["last_life_lost"], info["ticks_elapsed"]) == (
            -1, terminated, False, valid_resources, len(steps))
        assert info["raw"]["terminal_outcome"] == (raw.GAME_OVER if ready else 0)
        assert calls == ["start", *steps]
        if done:
            with pytest.raises(gym.error.ResetNeeded):
                env.step(0)
            _, reset_info = env.reset()
            assert reset_info["reset_route"] == "fresh_process"
            retry = [reset_protocol.GAME_OVER_RETRY] if ready else []
            assert calls == ["start", *steps, *retry, "close", "start"]


def test_observation_matches_the_declared_space_and_callbacks_get_mutable_rows(monkeypatch):
    fields = dict(available=raw.GROUP_PLAYER | raw.GROUP_BULLETS, valid=raw.GROUP_PLAYER | raw.GROUP_BULLETS,
                  bullet_scanned=2000, bullet_captured=3, player_state=1)

    class Session:
        def start(self, _options):
            return decoded(**fields)

        def step(self, _mask):
            return decoded(generation=4, **fields)

        def close(self):
            pass

    seen = []

    def reward(previous, _mask, current):
        seen.append((type(previous["bullets"]), type(current["bullets"])))
        current["bullets"][0]["angle"] = -0.5
        return 0.0

    monkeypatch.setattr(session_module, "GameSession", lambda *args, **kwargs: Session())
    with gym.make("TH10-v0", game_exe="fixture", native_dir="fixture", launcher="fixture",
                  max_episode_steps=1, reward_fn=reward) as env:
        obs, info = env.reset()
        assert isinstance(env.unwrapped._record["bullets"], raw.PackedBullets)
        assert env.observation_space.contains(obs)
        assert obs["bullets"]["x"].shape == (raw.BULLET_COUNT,)
        assert obs["counts"][1] == 3
        assert {"bullets", "groups", "lives"}.isdisjoint(info["raw"])
        obs, _, _, truncated, _ = env.step(0)
        assert env.observation_space.contains(obs) and truncated
    assert seen == [(list, list)]


@pytest.mark.parametrize("retry", [False, True])
def test_failed_reset_preserves_original_error_when_cleanup_is_interrupted(monkeypatch, retry):
    cause = OSError("bridge timed out")
    original_error = reset_protocol.SceneResetError("Retry failed") if retry else OSError("memory read failed")

    def start(_options):
        if not retry:
            raise original_error
        return decoded(player_state=1)

    def restart(_options):
        raise original_error from cause

    def close():
        raise KeyboardInterrupt("cleanup interrupted")

    session = SimpleNamespace(start=start, restart=restart, close=close)
    monkeypatch.setattr(session_module, "GameSession", lambda *args, **kwargs: session)
    env = gym.make("TH10-v0", game_exe="fixture", native_dir="fixture", launcher="fixture")
    if retry:
        env.reset()
    with pytest.raises(type(original_error)) as failure:
        env.reset()
    assert failure.value is original_error
    if retry:
        assert failure.value.__cause__ is cause
    assert "cleanup interrupted" in " ".join(failure.value.__notes__)
    with pytest.raises(gym.error.ResetNeeded):
        env.unwrapped.step(0)


def test_failed_mapping_acquisition_closes_handle_and_preserves_mapping_error():
    mapping_error = OSError("could not map view")
    closed = []

    def map_view(*_):
        raise mapping_error

    def close_handle(handle):
        closed.append(handle)
        raise OSError("could not close mapping handle")

    kernel = SimpleNamespace(OpenFileMappingW=lambda *_: 7, MapViewOfFile=map_view, CloseHandle=close_handle)
    with pytest.raises(OSError) as failure:
        MappedView("fixture", FILE_MAP_READ, 64, kernel=kernel)
    assert failure.value is mapping_error
    assert closed == [7]
    assert "could not close mapping handle" in " ".join(failure.value.__notes__)


def test_memory_reader_keeps_identity_error_if_handle_cleanup_fails(monkeypatch):
    identity_error = OSError("cannot query process identity")

    def image_path(_handle):
        raise identity_error

    def close_handle(_handle):
        raise OSError("cannot close process handle")

    api = SimpleNamespace(open=lambda _pid: 1, image_path=image_path,
                          kernel32=SimpleNamespace(CloseHandle=close_handle))
    monkeypatch.setattr(memory, "WinApi", lambda: api)
    with pytest.raises(OSError) as failure:
        memory.MemoryReader(7)
    assert failure.value is identity_error
    assert "cannot close process handle" in " ".join(failure.value.__notes__)


def test_loader_failure_keeps_exit_diagnostic_when_report_is_not_an_object(monkeypatch):
    result = SimpleNamespace(returncode=9, stdout="[]", stderr="hook initialization failed")
    monkeypatch.setattr(loader.subprocess, "run", lambda *args, **kwargs: result)
    with pytest.raises(RuntimeError, match=r"loader.exe exited 9: hook initialization failed \[\]"):
        loader.install_hook(Path("loader.exe"), 7, Path("hook.dll"))


def test_session_attempts_process_cleanup_after_resource_cleanup_fails():
    resource_error = OSError("mapping cleanup failed")

    def close_resources():
        raise resource_error

    def close_game():
        raise OSError("process cleanup failed")

    session = SimpleNamespace(_resources=SimpleNamespace(close=close_resources), _close_game=close_game)
    with pytest.raises(OSError) as failure:
        GameSession.close(session)
    assert failure.value is resource_error
    assert "process cleanup failed" in " ".join(resource_error.__notes__)


def test_bullet_reader_keeps_every_record_unless_a_limit_is_requested():
    payload = bytearray(bullets.BLOB_SIZE)
    for slot in range(bullets.SLOTS):
        struct.pack_into("<H", payload, bullets.POOL_OFFSET + slot * bullets.STRIDE + bullets.STATE_OFFSET, 1)
    records = bullets.decode_blob(payload, 0)["records_stored"]
    assert len(records) == bullets.SLOTS and records[-1]["slot"] == 1999
    assert len(bullets.decode_blob(payload, 0, limit=64)["records_stored"]) == 64


def test_decoders_surface_unexpected_reader_errors():
    def broken_reader(*_):
        raise TypeError("reader bug")

    with pytest.raises(TypeError, match="reader bug"):
        boss.decode_slots(0x10000, broken_reader)
    with pytest.raises(TypeError, match="reader bug"):
        lasers.traverse(SimpleNamespace(read=broken_reader), None, 0x10000)


def test_scene_options_separate_normal_and_practice():
    normal = request_from_options({"character": "Marisa", "shot_type": "c"})
    assert (normal.character, normal.shot_type, normal.section_kind) == (1, 2, 6)
    assert (normal.life, normal.power, normal.faith) == (0, 0, 0)

    practice = request_from_options({"mode": "practice", "stage": 3, "section": "boss"})
    assert (practice.stage, practice.section_kind) == (3, 3)
    assert (practice.life, practice.power, practice.faith) == (9, 0, 50_000)
    assert not practice.invincible and practice.dialogue_advance_ms == 0

    with pytest.raises(ValueError, match="requires stage 1"):
        request_from_options({"mode": "normal", "stage": 3})
