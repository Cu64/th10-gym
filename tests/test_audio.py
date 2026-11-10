import os
from queue import Queue
from types import SimpleNamespace

import pytest

from th10_gym import env as env_module
from th10_gym._session import session as session_module
from th10_gym._win32 import audio as _audio
from th10_gym.env import TH10Env


def test_audio_mute_tracks_current_pid_and_setting_across_resets(monkeypatch):
    sessions = []

    class Session:
        def __init__(self, *_args, mute_audio=False, **_kwargs):
            self.pid = 100 + len(sessions)
            self.closed = False
            self.muted = mute_audio
            sessions.append(self)

        def start(self, _options):
            return {}

        def close(self):
            self.closed = True

        def set_muted(self, muted):
            self.muted = muted

        @property
        def audio_mute_status(self):
            return {
                "enabled": self.muted, "pid": self.pid, "muted_sessions": int(self.muted), "error": None,
            }

    monkeypatch.setattr(session_module, "GameSession", Session)
    monkeypatch.setattr(env_module, "snapshot_observation", SimpleNamespace(
        observation=lambda _r: 0, observation_space=lambda: object(),
    ))
    monkeypatch.setattr(TH10Env, "_info", lambda self, _record, _observation, **_kwargs: {})
    env = TH10Env(game_exe="x", native_dir="x", launcher="x", mute_audio=True,
                 reuse_normal_process=False)
    env.reset()
    assert env.audio_mute_status["pid"] == sessions[0].pid
    env.set_muted(False)
    assert not sessions[0].muted
    env.set_muted(True)
    assert sessions[0].muted
    env.reset()
    assert sessions[0].closed
    assert env.audio_mute_status["pid"] == sessions[1].pid
    assert sessions[1].muted
    env.close()
    assert sessions[1].closed


def test_audio_mute_disabled_by_default():
    env = TH10Env(game_exe="x", native_dir="x", launcher="x")
    assert env.audio_mute_status == {
        "enabled": False, "pid": None, "muted_sessions": 0, "error": None,
    }


@pytest.mark.skipif(os.name != "nt", reason="Windows audio controller")
def test_audio_worker_applies_toggle_and_stops_while_disabled(monkeypatch):
    requests = Queue()

    def apply(pid, muted):
        requests.put((pid, muted))
        return 1

    monkeypatch.setattr(_audio, "_enumerate_and_set", apply)
    controller = _audio.ProcessAudioMute(123, interval=60)
    try:
        assert requests.get(timeout=2) == (123, True)
        controller.set_muted(False)
        assert requests.get(timeout=2) == (123, False)
    finally:
        controller.close()
    assert not controller._thread.is_alive()
    assert not controller.status["enabled"]
