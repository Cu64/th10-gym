"""Correlate process-local retries with the THPrac scene bridge."""

import time

from .._game import GAME_MODE_PRACTICE
from .._snapshot.format import GAME_OVER, PRACTICE_COMPLETE
from .profile import DIALOGUE_LABELS, SECTION_KINDS, SECTION_LABELS
from .protocol import (
    GAME_OVER_RETRY,
    PRACTICE_COMPLETE_RETRY,
    PROFILE_CHANGE,
    RESTART_ROUTE_LABELS,
    RETAIL_RETRY,
    SharedMapping,
    read_snapshot,
    submit_mutex,
    write_restart_request,
)


class SceneResetError(RuntimeError):
    """The same-process reset could not be verified."""


def is_practice(request):
    return request.section_kind != SECTION_KINDS["normal-game"]


def initial_resources(request):
    if is_practice(request):
        return request.life, request.power, request.faith // 10
    return 2, 0, 5000


def restart_route(record):
    outcome = record["terminal_outcome"]
    if outcome == GAME_OVER:
        return GAME_OVER_RETRY
    if outcome == PRACTICE_COMPLETE:
        return PRACTICE_COMPLETE_RETRY
    return RETAIL_RETRY


def queue_restart(pid, atomic, request, route):
    with submit_mutex(pid), SharedMapping(pid, create=False, atomic=atomic) as shared:
        before = read_snapshot(shared.view)
        if (before["status"] != "started_configured" or before["request_id"] != request.request_id
                or before["applied"]["practice_mode"] != is_practice(request)
                or any(before["applied"]["safety"].values())):
            raise SceneResetError("Bridge does not own the requested scene")
        sequence = write_restart_request(shared, request.request_id, route, timeout_ms=4_000)
        published = read_snapshot(shared.view)["restart"]
        _check_identity(published, request.request_id, route, sequence)
        if published["status"] != "queued":
            raise SceneResetError("Restart was consumed before gate release")
        return before, sequence


def wait_for_restart(pid, atomic, before, route, sequence, deadline):
    scene_id = before["request_id"]
    with SharedMapping(pid, create=False, atomic=atomic) as shared:
        while time.monotonic() < deadline:
            current = read_snapshot(shared.view)
            restart = current["restart"]
            _check_identity(restart, scene_id, route, sequence)
            if restart["response_sequence"] == sequence and _restart_finished(current, before, route):
                return current
            time.sleep(0.002)
    raise SceneResetError(f"Timed out verifying {RESTART_ROUTE_LABELS[route]}")


def _restart_finished(current, before, route):
    restart = current["restart"]
    status = restart["status"]
    if status in ("rejected", "failed") or restart["error"]:
        raise SceneResetError(f"Restart {status}, bridge error {restart['error']}")
    expected = "title_ready" if route == PROFILE_CHANGE else "retry_started"
    if status != expected:
        return False
    if restart["synthetic_input_polls"]:
        raise SceneResetError("Process-local reset injected input")
    return (
        current["status"] == "started_configured"
        and current["request_id"] == before["request_id"]
        and (route == PROFILE_CHANGE or current["last_update_tick_ms"] > before["last_update_tick_ms"])
    )


def validate_scene(current, request, previous=None):
    observed, applied = current["observed"], current["applied"]
    if (current["status"] != "started_configured" or current["request_id"] != request.request_id
            or "client_wait" in current
            or (observed["character"], observed["shot_type"], observed["difficulty"]) !=
            (request.character, request.shot_type, request.difficulty)
            or applied["game_mode_flags"] != (GAME_MODE_PRACTICE if is_practice(request) else 0)
            or any(applied["safety"].values())
            or applied["section_kind"] != SECTION_LABELS[request.section_kind]
            or applied["section_dialogue"] != DIALOGUE_LABELS[request.section_dialogue_policy]
            or (applied["stage"], applied["section_index"]) != (request.stage, request.section_index)
            or (applied["life"], applied["power"], applied["faith_raw_value"]) != initial_resources(request)):
        raise SceneResetError("Bridge returned different scene settings")
    if previous is not None and (applied["section_kind"], applied["section_id"]) != (
            previous["applied"]["section_kind"], previous["applied"]["section_id"]):
        raise SceneResetError("Bridge restarted a different section")


def _check_identity(restart, scene_id, route, sequence):
    if (restart["request_sequence"] != sequence or restart["scene_request_id"] != scene_id
            or restart["route"] != RESTART_ROUTE_LABELS[route]):
        raise SceneResetError("Restart request identity changed")
