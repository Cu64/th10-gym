"""Run a privately owned copy of the supported game, from launch through resets to shutdown."""

from __future__ import annotations

import ctypes
import os
import re
import struct
import tempfile
import time
from contextlib import ExitStack
from dataclasses import replace
from pathlib import Path

from .._cleanup import cleanup_after_error
from .._game import ADDRESS_GAMEPLAY_INPUT, ADDRESS_STAGE_TICK, PLAYER_ACTIVE
from .._scene import client as scene_client
from .._scene import restart
from .._scene.game_files import populate_game, verify_game_assets
from .._scene.profile import DIALOGUE_LEASE_MS, request_from_options
from .._scene.protocol import RESTART_ROUTE_LABELS
from .._snapshot.format import GAME_OVER, ObservationError
from .._stepping.gate import InvalidGateStatus
from .._stepping.snapshot_channel import require_matches_halted
from .._stepping.stepper import TickStepper
from .._win32.atomic import AtomicOps
from .._win32.loader import install_hook
from .._win32.manifest import verify
from .._win32.process import normalized_path, process_paths, stop_owned
from .._win32.render import DrawSkip
from ..readers.memory import MemoryReader, pe_identity

DXVK_D3D9_SHA256 = "265888c31ca78dffa290c39cb7e50bfb02762590e41927906e46fb32f01497fa"
HALT_TIMEOUT_SECONDS = 2
RESET_SECONDS = 4.5
START_TIMEOUT_SECONDS = 5
# Resets return control at this stage tick so every episode starts alike.
RESET_HANDOFF_TICK = 8
NEUTRAL_INPUT_BYTES = b"\0\0"


class GameSession:
    """Own a private game and advance it only through the native tick gate."""

    def __init__(self, game_exe, native_dir, launcher, *, atomic_dll=None, mute_audio=False,
                 dxvk_dll=None, dxvk_sha256=None, skip_render=False):
        if os.name != "nt" or ctypes.sizeof(ctypes.c_void_p) != 8:
            raise OSError("Live TH10 requires 64-bit Python on Windows")
        if dxvk_sha256 is not None and dxvk_dll is None:
            raise ValueError("dxvk_sha256 requires dxvk_dll")
        if dxvk_sha256 is not None and (
            not isinstance(dxvk_sha256, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", dxvk_sha256)
        ):
            raise ValueError("dxvk_sha256 must contain 64 hexadecimal digits")
        self.game_exe = Path(game_exe).resolve(strict=True)
        self.native_dir = Path(native_dir).resolve(strict=True)
        self.launcher = Path(launcher).resolve(strict=True)
        self.dxvk_dll = Path(dxvk_dll).resolve(strict=True) if dxvk_dll is not None else None
        self._dxvk_sha256 = dxvk_sha256.lower() if dxvk_sha256 is not None else DXVK_D3D9_SHA256
        self.atomic_dll = Path(atomic_dll).resolve(strict=True) if atomic_dll else (
            self.native_dir / "atomic_mapping_x64.dll"
        )
        self.pid = None
        self.scene = None
        self._resources = ExitStack()
        self._game = None
        self._temp = None
        self._stepper = None
        self._mute_audio = bool(mute_audio)
        self._audio_muter = None
        self._dialogue_renew_at = 0.0
        self._skip_render = bool(skip_render)
        self._draw_skip = None

    def start(self, options: dict) -> dict:
        """Launch the game, start the requested scene, and return its first snapshot."""
        if self._temp is not None:
            raise RuntimeError("Create a fresh GameSession for each launch")
        request = request_from_options(options)
        self._launch(request)
        result = scene_client.submit_scene(self.pid, request, self._atomic)
        if result["status"] != "started_configured" or "client_wait" in result:
            raise RuntimeError(f"Scene start failed: {result}")
        self._request = request
        self.scene = {"observed": result["observed"], "applied": result["applied"]}
        self._stepper.wait_for_gameplay_progress(time.monotonic() + START_TIMEOUT_SECONDS)
        self._stepper.arm(time.monotonic() + START_TIMEOUT_SECONDS)
        restart.validate_scene(result, request)
        self._align_to_handoff_tick(request)
        if self._skip_render:
            self._draw_skip = DrawSkip(self.pid, self._game, self.memory)
        return self._stepper.record

    def _launch(self, request):
        installer = self.native_dir / "th10_hook_installer.exe"
        hook = (self.native_dir / "th10_hook.dll").resolve(strict=True)
        atomic_dll = self.atomic_dll.resolve(strict=True)
        for helper in (installer, self.launcher):
            verify(helper)
        if not pe_identity(str(self.game_exe))["verified"]:
            raise ValueError("Expected the supported retail TH10 1.00a executable")
        self._atomic = AtomicOps(atomic_dll)
        self._copy_game()
        launched = scene_client.launch_game(self._game, self.launcher, self._atomic, without_oilp=True)
        self.pid = launched.game_pid
        self.memory = self._resources.enter_context(MemoryReader(self.pid))
        if not self.memory.image_path.samefile(self._game):
            raise RuntimeError("The scene bridge returned a different executable")
        # The installer briefly stops the game under a debugger. Wait for the
        # title screen first so graphics startup runs uninterrupted.
        scene_client.wait_for_title(self.pid, self._atomic, timeout_ms=request.timeout_ms)
        if self._mute_audio:
            self._start_audio_muter()
        install_hook(installer, self.pid, hook)
        self._stepper = TickStepper(self.pid, self._atomic, self._resources)

    def _copy_game(self):
        vpatch, dxvk = verify_game_assets(self.game_exe, self.dxvk_dll, self._dxvk_sha256)
        self._temp = tempfile.TemporaryDirectory(prefix="th10-gym-")
        destination = Path(self._temp.name)
        self._game = destination / self.game_exe.name
        populate_game(destination, self.game_exe, vpatch, dxvk)

    def restart(self, options: dict) -> tuple[dict, str]:
        """Reset within the same process. Raises SceneResetError if it cannot be verified."""
        request = request_from_options(options)
        if replace(request, request_id=self._request.request_id) == self._request:
            request = self._request
        try:
            return self._restart(request)
        except restart.SceneResetError:
            raise
        except (OSError, RuntimeError, InvalidGateStatus, ObservationError) as error:
            raise restart.SceneResetError(str(error)) from error

    def _restart(self, request):
        prior = self._stepper.record
        if prior["lives"] < 0 and prior["terminal_outcome"] != GAME_OVER:
            raise restart.SceneResetError("Last life exhausted without a verified Game Over menu")
        changing_profile = request != self._request
        route = restart.PROFILE_CHANGE if changing_profile else restart.restart_route(prior)
        current, before, deadline = self._request_restart(prior, route)
        if changing_profile:
            current = self._start_changed_profile(request, deadline)
        else:
            restart.validate_scene(current, request, before)
        self._stepper.wait_for_gameplay_progress(deadline, after_sequence=prior["calc_post_sequence"])
        ticks = struct.unpack("<II", self.memory.read(ADDRESS_STAGE_TICK, 8))
        if not 0 < ticks[0] == ticks[1] < RESET_HANDOFF_TICK:
            raise restart.SceneResetError(f"Scene reset advanced to ticks {ticks}")
        self._stepper.rearm_after_reset(ticks, deadline)
        record = self._stepper.record
        if (record["generation"] <= prior["generation"]
                or record["calc_post_sequence"] <= prior["calc_post_sequence"]):
            raise restart.SceneResetError("Reset returned a stale snapshot")
        self._align_to_handoff_tick(request)
        self._request = request
        self.scene = {"observed": current["observed"], "applied": current["applied"]}
        return self._stepper.record, RESTART_ROUTE_LABELS[route]

    def _request_restart(self, prior, route):
        status = self._stepper.wait_halted(time.monotonic() + HALT_TIMEOUT_SECONDS)
        require_matches_halted(prior, status)
        if self._draw_skip is not None:
            self._draw_skip.set_skipped(False)
        if self.memory.read(ADDRESS_GAMEPLAY_INPUT, 2) != NEUTRAL_INPUT_BYTES:
            raise restart.SceneResetError("Reset requires neutral gameplay input")
        before, restart_sequence = restart.queue_restart(self.pid, self._atomic, self._request, route)
        held = self._stepper.wait_halted(time.monotonic() + HALT_TIMEOUT_SECONDS)
        if held.pass_count != status.pass_count:
            raise restart.SceneResetError("The tick gate advanced while queuing the reset")
        require_matches_halted(prior, held)
        deadline = time.monotonic() + RESET_SECONDS
        self._stepper.release_for_reset(deadline)
        current = restart.wait_for_restart(self.pid, self._atomic, before, route, restart_sequence, deadline)
        return current, before, deadline

    def _start_changed_profile(self, request, deadline):
        remaining_ms = int((deadline - time.monotonic()) * 1000)
        if remaining_ms <= 0:
            raise restart.SceneResetError("No reset time remaining to start the new profile")
        current = scene_client.submit_scene(self.pid, replace(request, timeout_ms=remaining_ms), self._atomic)
        restart.validate_scene(current, request)
        return current

    def _align_to_handoff_tick(self, request):
        """Run neutral ticks up to the handoff tick and check the starting resources."""
        record = self._stepper.record
        tick = record["stage_tick"]
        if not 0 < tick == record["segment_tick"] <= RESET_HANDOFF_TICK:
            raise restart.SceneResetError(f"Scene start reached ticks {tick}, {record['segment_tick']} before halt")
        for expected in range(tick + 1, RESET_HANDOFF_TICK + 1):
            record = self.step(0, draw=True)
            if (record["stage_tick"], record["segment_tick"]) != (expected, expected) or record["terminal_outcome"]:
                raise restart.SceneResetError("Could not align the scene start to the handoff tick")
        record = self._stepper.record
        lives, power, faith = restart.initial_resources(request)
        if ((record["player_state"], record["lives"], record["power"], record["faith"], record["score"])
                != (PLAYER_ACTIVE, lives, power, faith, 0)
                or self.memory.read(ADDRESS_GAMEPLAY_INPUT, 2) != NEUTRAL_INPUT_BYTES):
            raise restart.SceneResetError("Halted start did not restore the initial resources")

    def step(self, input_mask: int, *, draw=False) -> dict:
        """Run one tick with `input_mask` and return its verified snapshot."""
        before = self._stepper.check_halted()
        if self._draw_skip is not None:
            record = self._stepper.record
            self._draw_skip.set_skipped(
                not draw and record["player_state"] == PLAYER_ACTIVE
                and not record["dialogue_active"] and not record["terminal_outcome"]
            )
        return self._stepper.advance(input_mask, before)

    def step_dialogue(self) -> dict:
        """Run one neutral tick while keeping the bridge's dialogue skip enabled."""
        record = self._stepper.record
        if not record.get("dialogue_active") or record["terminal_outcome"]:
            raise RuntimeError("A dialogue tick requires active TH10 dialogue")
        if time.monotonic() >= self._dialogue_renew_at:
            scene_client.set_dialogue_skip(self.pid, self._atomic, self._request.request_id, DIALOGUE_LEASE_MS)
            self._dialogue_renew_at = time.monotonic() + DIALOGUE_LEASE_MS / 2000
        return self.step(0)

    def end_dialogue(self) -> None:
        scene_client.set_dialogue_skip(self.pid, self._atomic, self._request.request_id, 0)
        self._dialogue_renew_at = 0.0

    def close(self):
        try:
            self._resources.close()
        except BaseException as error:
            cleanup_after_error(self._close_game, error, "TH10 process cleanup failed")
            raise
        self._close_game()

    def _close_game(self):
        if self._temp is None:
            return
        target = normalized_path(self._game)
        for pid, path in process_paths().items():
            if path == target:
                stop_owned(pid, target)
        self._temp.cleanup()
        self._temp = None
        self._game = None
        self.pid = None

    def _start_audio_muter(self):
        from .._win32.audio import ProcessAudioMute
        self._audio_muter = ProcessAudioMute(self.pid)
        self._resources.callback(self._close_audio_muter)

    def _close_audio_muter(self):
        audio_muter, self._audio_muter = self._audio_muter, None
        audio_muter.close()

    def set_muted(self, muted=True):
        self._mute_audio = bool(muted)
        if self._audio_muter is not None:
            self._audio_muter.set_muted(self._mute_audio)
        elif self._mute_audio and self.pid is not None:
            self._start_audio_muter()
        return self.audio_mute_status

    @property
    def audio_mute_status(self):
        if self._audio_muter is not None:
            return self._audio_muter.status
        return {"enabled": self._mute_audio, "pid": self.pid, "muted_sessions": 0, "error": None}
