"""Launch TH10 and submit scene requests to its native bridge."""

from __future__ import annotations

import hashlib
import json
import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from .._win32.atomic import AtomicOps
from .._win32.process import normalized_path, process_alive, process_paths
from ..readers.memory import EXPECTED_SHA256
from .profile import MAX_TTL_MS, MIN_TTL_MS, SceneRequest
from .protocol import (
    VERSION,
    SharedMapping,
    publish_dialogue_lease,
    read_snapshot,
    request_is_busy,
    submit_mutex,
    write_request,
)

THPRAC_COMMIT = "ffb077707e7779b8437608e2fbdb7d6e5775b863"
THPRAC_UPDATES_DISABLED = 2


@dataclass(frozen=True)
class LaunchedGame:
    game_pid: int
    launcher_pid: int
    game_exe: Path


def launch_game(game_exe: Path, launcher: Path, atomic: AtomicOps, *,
                discovery_ms: int = 30_000, without_oilp: bool = False) -> LaunchedGame:
    if not MIN_TTL_MS <= discovery_ms <= MAX_TTL_MS:
        raise ValueError(f"discovery-ms must be in {MIN_TTL_MS}..{MAX_TTL_MS}.")
    game_exe = game_exe.resolve(strict=True)
    launcher = launcher.resolve(strict=True)
    game_hash = hashlib.sha256(game_exe.read_bytes()).hexdigest()
    if game_hash != EXPECTED_SHA256:
        raise RuntimeError(f"Unsupported TH10 executable SHA256: {game_hash}.")
    _verify_launcher(launcher, atomic.path)
    target = normalized_path(game_exe)
    before = process_paths()
    existing = sorted(pid for pid, path in before.items() if path == target)
    if existing:
        raise RuntimeError(f"Refusing launch: this exact game path is already running (PID {existing[0]}).")
    command = [str(launcher)]
    if without_oilp:
        command.append("--without-oilp")
    command.append(str(game_exe))
    launcher_process = subprocess.Popen(command, cwd=str(launcher.parent), close_fds=True)
    deadline = time.monotonic() + discovery_ms / 1000
    while time.monotonic() < deadline:
        current = process_paths()
        new_matches = sorted(pid for pid, path in current.items() if path == target and pid not in before)
        if len(new_matches) == 1:
            return LaunchedGame(new_matches[0], launcher_process.pid, game_exe)
        if len(new_matches) > 1:
            raise RuntimeError("More than one new process matches the requested game path; refusing to choose a PID.")
        if launcher_process.poll() is not None and launcher_process.returncode != 0:
            raise RuntimeError(f"The launcher exited with code {launcher_process.returncode} before starting the game.")
        time.sleep(0.2)
    raise TimeoutError(f"No new process with the exact game path appeared within {discovery_ms} ms.")


def _sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _verify_launcher(launcher, atomic_helper):
    """Check that the launcher and its helper match the marker written by the bridge build."""
    marker_path = launcher.parent / "scene_bridge_private_build.json"
    if not marker_path.is_file():
        raise RuntimeError("The launcher has no build marker. Run native/scripts/build_scene_bridge.ps1 first.")
    marker = json.loads(marker_path.read_text(encoding="ascii"))
    if marker.get("upstream_commit") != THPRAC_COMMIT:
        raise RuntimeError("The launcher build marker does not identify the pinned THPrac source.")
    if marker.get("protocol_version") != VERSION:
        actual = marker.get("protocol_version")
        raise RuntimeError(f"Bridge protocol mismatch: expected {VERSION}, marker has {actual!r}.")
    artifacts = (("thprac.exe", "launcher_sha256"), ("thprac32.dll", "thprac32_sha256"),
                 ("thprac64.dll", "thprac64_sha256"))
    for filename, key in artifacts:
        artifact = launcher.parent / filename
        if not artifact.is_file() or _sha256(artifact) != marker.get(key):
            raise RuntimeError(f"Launcher file check failed for {filename}.")
    if not atomic_helper.is_file() or _sha256(atomic_helper) != marker.get("atomic_helper_sha256"):
        raise RuntimeError("The atomic helper is missing or differs from the build marker.")
    private_settings = launcher.parent / ".thprac_data" / "settings.json"
    try:
        settings = json.loads(private_settings.read_text(encoding="ascii"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError("The launcher settings are missing or invalid.") from exc
    if (settings.get("check_update") != THPRAC_UPDATES_DISABLED or settings.get("dont_search_ongoing_game") is not True
            or settings.get("thprac_admin_rights") is not False):
        raise RuntimeError("The launcher settings must disable updates, automatic attachment, and elevation.")


def wait_for_title(pid: int, atomic: AtomicOps, *, timeout_ms: int) -> None:
    """Wait for the bridge to observe the title state that accepts Start."""
    deadline = time.monotonic() + timeout_ms / 1000
    with SharedMapping(pid, create=True, atomic=atomic) as shared:
        snapshot = None
        while time.monotonic() < deadline:
            if not process_alive(pid):
                raise RuntimeError("TH10 exited before reaching the title screen")
            snapshot = read_snapshot(shared.view)
            if snapshot["status"] == "failed":
                raise RuntimeError(f"TH10 scene bridge failed before title: {snapshot}")
            observed = snapshot["observed"]
            if (observed["menu_state"], observed["menu_substate"]) == (2, 2):
                return
            time.sleep(0.01)
    raise TimeoutError(f"TH10 did not reach a ready title screen: {snapshot}")


def submit_scene(pid: int, request: SceneRequest, atomic: AtomicOps, *,
                 report: Callable[[dict], None] | None = None) -> dict:
    with submit_mutex(pid), SharedMapping(pid, create=True, atomic=atomic) as shared:
        view = shared.view
        if view.status == 8 and view.error == 8:
            raise RuntimeError("Private THPrac bridge hook bytes do not match the verified TH10 build.")
        if request_is_busy(view.status, view.request_sequence, view.response_sequence):
            raise RuntimeError("A previous request is still pending in this game process.")
        sequence = max(1, view.request_sequence + 1)
        clock = shared.kernel32.GetTickCount64
        deadline = clock() + request.timeout_ms
        write_request(view, request, shared.atomic, sequence, deadline)
        if report is not None:
            report({"request_id": request.request_id, "request_sequence": sequence,
                    "status": "queued", "mapping_name": shared.name})

        return _wait_for_scene(view, sequence, clock, deadline, report)


def _wait_for_scene(view, sequence, clock, deadline, report):
    last_status = None
    while clock() < deadline:
        snapshot = read_snapshot(view, now_tick_ms=clock())
        if snapshot["request_sequence"] == snapshot["response_sequence"] == sequence:
            status = snapshot["status"]
            if report is not None and status != last_status:
                report(snapshot)
            last_status = status
            if status in {"started_configured", "rejected", "expired", "failed"}:
                return snapshot
        time.sleep(0.005)
    snapshot = read_snapshot(view, now_tick_ms=clock())
    snapshot["client_wait"] = "timed_out_before_terminal_status"
    if report is not None:
        report(snapshot)
    return snapshot


def set_dialogue_skip(pid, atomic, request_id, duration_ms):
    """Keep the bridge skipping dialogue waits for `duration_ms`, or stop it with 0."""
    with submit_mutex(pid), SharedMapping(pid, create=False, atomic=atomic) as shared:
        snapshot = read_snapshot(shared.view)
        sequence = snapshot["request_sequence"]
        if (snapshot["request_id"] != request_id or sequence != snapshot["response_sequence"]
                or snapshot["status"] != "started_configured"):
            raise RuntimeError("Skipping dialogue requires the current verified TH10 scene")
        deadline = shared.kernel32.GetTickCount64() + duration_ms if duration_ms else 0
        publish_dialogue_lease(shared.view, shared.atomic, duration_ms, deadline, sequence, request_id)
