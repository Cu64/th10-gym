"""Shared-memory layout and publication rules for the THPrac scene bridge."""

from __future__ import annotations

import ctypes
import sys
import uuid

from .._cleanup import cleanup_after_error
from .._win32.atomic import AtomicOps
from .._win32.mapping import MappedView, mapping_api
from .profile import DIALOGUE_LABELS, DIALOGUE_LEASE_MS, SECTION_LABELS, SceneRequest

MAGIC = 0x42534854
VERSION = 13
RETAIL_RETRY, GAME_OVER_RETRY, PRACTICE_COMPLETE_RETRY, PROFILE_CHANGE = range(4, 8)
# Keep retired labels for decoding archived bridge snapshots.
RESTART_STATUS_LABELS = {0: "idle", 1: "queued", 2: "injecting",
                         3: "keys_sent", 4: "rejected", 5: "failed",
                         6: "action_queued", 7: "title_ready", 8: "retry_started"}
RESTART_PHASE_LABELS = {0: "none", 1: "escape_hold", 2: "escape_delay",
                        3: "reset_hold", 4: "complete", 5: "await_title",
                        6: "title_ready", 7: "await_startup"}
RESTART_ROUTE_LABELS = {
    0: "esc_r", 1: "gameover_return_title",
    2: "practice_gameover_return_title",
    3: "practice_stage_complete_return_title",
    RETAIL_RETRY: "retail_retry",
    GAME_OVER_RETRY: "gameover_retry",
    PRACTICE_COMPLETE_RETRY: "practice_complete_retry",
    PROFILE_CHANGE: "profile_change",
}

STATUS_LABELS = {
    0: "idle",
    1: "queued",
    2: "awaiting_frontend",
    3: "prepared_at_practice_menu",
    4: "start_pending",
    5: "started_configured",
    6: "rejected",
    7: "expired",
    8: "failed",
}
ERROR_LABELS = {
    0: "none",
    1: "protocol_mismatch",
    2: "invalid_request",
    3: "unsupported_section",
    4: "frontend_mismatch",
    5: "startup_readback_mismatch",
    6: "deadline_expired",
    7: "safety_readback_mismatch",
    8: "hook_mismatch",
    9: "practice_mode_not_confirmed",
    10: "section_patch_mismatch",
}
SECTION_PATCH_LABELS = {0: "unverified", 1: "matched", 2: "mismatch"}


class Mapping(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("request_sequence", ctypes.c_int32),
        ("response_sequence", ctypes.c_int32),
        ("response_lock", ctypes.c_int32),
        ("status", ctypes.c_int32),
        ("error", ctypes.c_int32),
        ("request_id", ctypes.c_uint8 * 16),
        ("deadline_tick_ms", ctypes.c_uint64),
        ("character", ctypes.c_int32),
        ("shot_type", ctypes.c_int32),
        ("difficulty", ctypes.c_int32),
        ("stage", ctypes.c_int32),
        ("section_kind", ctypes.c_int32),
        ("section_index", ctypes.c_int32),
        ("life", ctypes.c_int32),
        ("power", ctypes.c_int32),
        ("faith", ctypes.c_int32),
        ("invincible", ctypes.c_int32),
        ("infinite_lives", ctypes.c_int32),
        ("infinite_power", ctypes.c_int32),
        ("time_lock", ctypes.c_int32),
        ("auto_bomb", ctypes.c_int32),
        ("no_faith_loss", ctypes.c_int32),
        ("observed_character", ctypes.c_int32),
        ("observed_shot_type", ctypes.c_int32),
        ("observed_difficulty", ctypes.c_int32),
        ("observed_menu_state", ctypes.c_int32),
        ("observed_menu_substate", ctypes.c_int32),
        ("applied_stage", ctypes.c_int32),
        ("applied_section_kind", ctypes.c_int32),
        ("applied_section_index", ctypes.c_int32),
        ("applied_section_id", ctypes.c_int32),
        ("applied_life", ctypes.c_int32),
        ("applied_power", ctypes.c_int32),
        ("applied_faith", ctypes.c_int32),
        ("applied_invincible", ctypes.c_int32),
        ("applied_infinite_lives", ctypes.c_int32),
        ("applied_infinite_power", ctypes.c_int32),
        ("applied_time_lock", ctypes.c_int32),
        ("applied_auto_bomb", ctypes.c_int32),
        ("applied_no_faith_loss", ctypes.c_int32),
        ("last_update_tick_ms", ctypes.c_uint64),
        ("observed_game_mode_flags", ctypes.c_uint32),
        ("applied_game_mode_flags", ctypes.c_uint32),
        ("section_dialogue_policy", ctypes.c_int32),
        ("section_patch_verification", ctypes.c_int32),
        ("dialogue_advance_ms", ctypes.c_int32),
        ("dialogue_advance_forced", ctypes.c_int32),
        ("dialogue_advance_deadline_tick_ms", ctypes.c_uint64),
        ("dialogue_branch_visits", ctypes.c_int32),
        ("dialogue_lease_ms", ctypes.c_int32),
        ("dialogue_lease_generation", ctypes.c_int32),
        ("dialogue_lease_deadline_tick_ms", ctypes.c_uint64),
        ("dialogue_lease_request_sequence", ctypes.c_int32),
        ("dialogue_lease_request_id", ctypes.c_uint8 * 16),
        ("restart_request_sequence", ctypes.c_int32),
        ("restart_response_sequence", ctypes.c_int32),
        ("restart_status", ctypes.c_int32),
        ("restart_error", ctypes.c_int32),
        ("restart_phase", ctypes.c_int32),
        ("restart_input_polls", ctypes.c_int32),
        ("restart_scene_request_id", ctypes.c_uint8 * 16),
        ("restart_deadline_tick_ms", ctypes.c_uint64),
        ("restart_route", ctypes.c_int32),
        ("restart_guard_owner_state", ctypes.c_int32),
        ("restart_guard_owner_path_marker", ctypes.c_int32),
        ("restart_guard_lives", ctypes.c_int32),
        ("restart_guard_stage", ctypes.c_int32),
        ("restart_guard_mode_flags", ctypes.c_int32),
        ("restart_guard_manager_flags", ctypes.c_int32),
        ("restart_guard_input", ctypes.c_int32),
        ("restart_guard_frontend_input", ctypes.c_int32),
        ("reserved_v8_tail", ctypes.c_uint32),
        ("restart_last_input_active", ctypes.c_int32),
        ("restart_input_hook_entries", ctypes.c_int32),
        ("restart_inactive_input_polls", ctypes.c_int32),
        ("restart_synthetic_input_polls", ctypes.c_int32),
    ]


assert ctypes.sizeof(Mapping) == 384
assert Mapping.request_sequence.offset == 12
assert Mapping.response_lock.offset == 20
assert Mapping.request_id.offset == 32
assert Mapping.deadline_tick_ms.offset == 48
assert Mapping.character.offset == 56
assert Mapping.observed_character.offset == 116
assert Mapping.applied_section_id.offset == 148
assert Mapping.last_update_tick_ms.offset == 192
assert Mapping.observed_game_mode_flags.offset == 200
assert Mapping.applied_game_mode_flags.offset == 204
assert Mapping.section_dialogue_policy.offset == 208
assert Mapping.section_patch_verification.offset == 212
assert Mapping.dialogue_advance_ms.offset == 216
assert Mapping.dialogue_advance_forced.offset == 220
assert Mapping.dialogue_advance_deadline_tick_ms.offset == 224
assert Mapping.dialogue_branch_visits.offset == 232
assert Mapping.dialogue_lease_ms.offset == 236
assert Mapping.dialogue_lease_generation.offset == 240
assert Mapping.dialogue_lease_deadline_tick_ms.offset == 248
assert Mapping.dialogue_lease_request_sequence.offset == 256
assert Mapping.dialogue_lease_request_id.offset == 260
assert Mapping.restart_request_sequence.offset == 276
assert Mapping.restart_response_sequence.offset == 280
assert Mapping.restart_status.offset == 284
assert Mapping.restart_error.offset == 288
assert Mapping.restart_phase.offset == 292
assert Mapping.restart_input_polls.offset == 296
assert Mapping.restart_scene_request_id.offset == 300
assert Mapping.restart_deadline_tick_ms.offset == 320
assert Mapping.restart_route.offset == 328
assert Mapping.restart_guard_owner_state.offset == 332
assert Mapping.restart_guard_owner_path_marker.offset == 336
assert Mapping.restart_guard_lives.offset == 340
assert Mapping.restart_guard_stage.offset == 344
assert Mapping.restart_guard_mode_flags.offset == 348
assert Mapping.restart_guard_manager_flags.offset == 352
assert Mapping.restart_guard_input.offset == 356
assert Mapping.restart_guard_frontend_input.offset == 360
assert Mapping.reserved_v8_tail.offset == 364
assert Mapping.restart_last_input_active.offset == 368
assert Mapping.restart_input_hook_entries.offset == 372
assert Mapping.restart_inactive_input_polls.offset == 376
assert Mapping.restart_synthetic_input_polls.offset == 380


def mapping_name(pid: int) -> str:
    if pid <= 0 or pid > 0x7FFFFFFF:
        raise ValueError("pid must be a positive Windows process ID.")
    return rf"Local\THPracSceneBridge_TH10_{pid}"


def _kernel32():
    if sys.platform != "win32":
        raise RuntimeError("Shared-memory submit and status commands require Windows.")
    kernel32 = mapping_api()
    kernel32.GetTickCount64.restype = ctypes.c_uint64
    return kernel32


class SharedMapping(MappedView):
    def __init__(self, pid: int, *, create: bool, atomic: AtomicOps):
        self.atomic = atomic
        self.name = mapping_name(pid)
        super().__init__(self.name, 0x000F001F, ctypes.sizeof(Mapping),
                         kernel=_kernel32(), create=create)
        try:
            self.view = Mapping.from_address(self.address)
            self._ensure_header(create)
        except BaseException as error:
            cleanup_after_error(self.close, error, "Mapping cleanup failed")
            raise

    def _ensure_header(self, create: bool) -> None:
        view = self.view
        if view.magic == MAGIC and view.version == VERSION and view.struct_size == ctypes.sizeof(Mapping):
            return
        if not create:
            raise RuntimeError("The mapping has an unsupported or incomplete protocol header.")
        if view.request_sequence != 0 or view.response_sequence != 0:
            raise RuntimeError("The mapping contains a request with an invalid protocol header.")
        view.version = VERSION
        view.struct_size = ctypes.sizeof(Mapping)
        view.status = 0
        view.error = 0
        self.atomic.exchange32(self.address, MAGIC)


def read_snapshot(view: Mapping, attempts: int = 100, now_tick_ms: int | None = None) -> dict[str, object]:
    for _ in range(attempts):
        request_before = view.request_sequence
        if request_before < 0:
            continue
        before = view.response_lock
        if before & 1:
            continue
        snapshot = bytes(ctypes.string_at(ctypes.addressof(view), ctypes.sizeof(Mapping)))
        after = view.response_lock
        request_after = view.request_sequence
        if before == after and not (after & 1) and request_before == request_after:
            copy = Mapping.from_buffer_copy(snapshot)
            return _decode_snapshot(copy, now_tick_ms)
    raise RuntimeError("Could not read a stable status snapshot.")


def _decode_snapshot(copy: Mapping, now_tick_ms: int | None) -> dict[str, object]:
    return {
        "request_sequence": copy.request_sequence,
        "response_sequence": copy.response_sequence,
        "status": STATUS_LABELS.get(copy.status, f"unknown_{copy.status}"),
        "error": ERROR_LABELS.get(copy.error, f"unknown_{copy.error}"),
        "request_id": str(uuid.UUID(bytes=bytes(copy.request_id))) if any(copy.request_id) else None,
        "deadline_tick_ms": copy.deadline_tick_ms,
        "observed": {
            "character": copy.observed_character,
            "shot_type": copy.observed_shot_type,
            "difficulty": copy.observed_difficulty,
            "menu_state": copy.observed_menu_state,
            "menu_substate": copy.observed_menu_substate,
            "game_mode_flags": copy.observed_game_mode_flags,
            "practice_mode": bool(copy.observed_game_mode_flags & 0x10),
        },
        "applied": {
            "stage": copy.applied_stage,
            "section_kind": SECTION_LABELS.get(copy.applied_section_kind, f"unknown_{copy.applied_section_kind}"),
            "section_index": copy.applied_section_index,
            "section_id": copy.applied_section_id,
            "life": copy.applied_life,
            "power": copy.applied_power,
            "faith": copy.applied_faith * 10,
            "faith_raw_value": copy.applied_faith,
            "safety": {
                "invincible": bool(copy.applied_invincible),
                "infinite_lives": bool(copy.applied_infinite_lives),
                "infinite_power": bool(copy.applied_infinite_power),
                "time_lock": bool(copy.applied_time_lock),
                "auto_bomb": bool(copy.applied_auto_bomb),
                "no_faith_loss": bool(copy.applied_no_faith_loss),
            },
            "game_mode_flags": copy.applied_game_mode_flags,
            "practice_mode": bool(copy.applied_game_mode_flags & 0x10),
            "section_dialogue": DIALOGUE_LABELS.get(copy.section_dialogue_policy,
                                                    f"unknown_{copy.section_dialogue_policy}"),
            "section_patch_verification": SECTION_PATCH_LABELS.get(copy.section_patch_verification,
                                                                   f"unknown_{copy.section_patch_verification}"),
            "dialogue_advance_ms": copy.dialogue_advance_ms,
            "dialogue_advance_forced": copy.dialogue_advance_forced,
            "dialogue_branch_visits": copy.dialogue_branch_visits,
            "dialogue_advance_deadline_tick_ms": copy.dialogue_advance_deadline_tick_ms,
            "dialogue_advance_state": _dialogue_state(copy, now_tick_ms),
        },
        "last_update_tick_ms": copy.last_update_tick_ms,
        "restart": {
            "request_sequence": copy.restart_request_sequence,
            "response_sequence": copy.restart_response_sequence,
            "status": RESTART_STATUS_LABELS.get(
                copy.restart_status, f"unknown_{copy.restart_status}"),
            "error": copy.restart_error,
            "phase": copy.restart_phase,
            "phase_label": RESTART_PHASE_LABELS.get(
                copy.restart_phase, f"unknown_{copy.restart_phase}"),
            "input_polls": copy.restart_input_polls,
            "last_input_active": bool(copy.restart_last_input_active),
            "input_hook_entries": copy.restart_input_hook_entries,
            "inactive_input_polls": copy.restart_inactive_input_polls,
            "synthetic_input_polls": copy.restart_synthetic_input_polls,
            "scene_request_id": str(uuid.UUID(
                bytes=bytes(copy.restart_scene_request_id)))
                if any(copy.restart_scene_request_id) else None,
            "deadline_tick_ms": copy.restart_deadline_tick_ms,
            "route": RESTART_ROUTE_LABELS.get(
                copy.restart_route, f"unknown_{copy.restart_route}"),
            "guard": {
                "owner_state": copy.restart_guard_owner_state,
                "owner_path_marker": copy.restart_guard_owner_path_marker,
                "lives": copy.restart_guard_lives,
                "stage": copy.restart_guard_stage,
                "mode_flags": copy.restart_guard_mode_flags,
                "manager_flags": copy.restart_guard_manager_flags,
                "input": copy.restart_guard_input,
                "frontend_input": copy.restart_guard_frontend_input,
            },
        },
    }


def _dialogue_state(copy: Mapping, now_tick_ms: int | None) -> str:
    if copy.dialogue_advance_ms == 0:
        return "disabled"
    if copy.dialogue_advance_deadline_tick_ms == 0:
        return "unarmed"
    if now_tick_ms is None:
        return "unknown"
    return "expired" if now_tick_ms >= copy.dialogue_advance_deadline_tick_ms else "active"


def publish_sequence(view: Mapping, atomic: AtomicOps, sequence: int) -> None:
    address = ctypes.addressof(view) + Mapping.request_sequence.offset
    atomic.exchange32(address, sequence)


def write_restart_request(shared: SharedMapping, scene_id: str, route: int,
                          timeout_ms: int) -> int:
    scene_bytes = uuid.UUID(scene_id).bytes
    view = shared.view
    if view.restart_status in (1, 2, 6):
        raise RuntimeError("A restart request is already pending in this game process.")
    last_sequence = view.restart_request_sequence
    sequence = last_sequence + 1 if 0 <= last_sequence < 0x7FFFFFFF else 1
    view.restart_status = 1
    view.restart_error = 0
    view.restart_phase = 0
    view.restart_input_polls = 0
    view.restart_last_input_active = 0
    view.restart_input_hook_entries = 0
    view.restart_inactive_input_polls = 0
    view.restart_synthetic_input_polls = 0
    view.restart_deadline_tick_ms = shared.kernel32.GetTickCount64() + timeout_ms
    view.restart_route = route
    view.restart_scene_request_id[:] = scene_bytes
    shared.atomic.exchange32(shared.address + Mapping.restart_request_sequence.offset, sequence)
    return sequence


def publish_dialogue_lease(
    view: Mapping, atomic: AtomicOps, duration_ms: int, deadline_tick_ms: int,
    request_sequence: int, request_id: str,
) -> int:
    if not 0 <= duration_ms <= DIALOGUE_LEASE_MS:
        raise ValueError(f"dialogue lease must be in 0..{DIALOGUE_LEASE_MS} ms.")
    if duration_ms == 0:
        deadline_tick_ms = 0
    elif deadline_tick_ms <= 0:
        raise ValueError("an active dialogue lease requires an absolute deadline.")
    stable_generation = int(view.dialogue_lease_generation) & 0x7FFFFFFE
    odd_generation = (stable_generation + 1) & 0x7FFFFFFF
    generation = (stable_generation + 2) & 0x7FFFFFFE
    if generation == 0:
        generation = 2
    atomic.exchange32(ctypes.addressof(view) + Mapping.dialogue_lease_generation.offset, odd_generation)
    view.dialogue_lease_ms = duration_ms
    view.dialogue_lease_deadline_tick_ms = deadline_tick_ms
    view.dialogue_lease_request_sequence = request_sequence
    view.dialogue_lease_request_id[:] = uuid.UUID(request_id).bytes
    address = ctypes.addressof(view) + Mapping.dialogue_lease_generation.offset
    atomic.exchange32(address, generation)
    return generation


def write_request(view: Mapping, request: SceneRequest, atomic: AtomicOps, sequence: int, deadline: int) -> None:
    publish_sequence(view, atomic, -sequence)
    request_uuid = uuid.UUID(request.request_id)
    ctypes.memmove(
        ctypes.addressof(view) + Mapping.request_id.offset,
        request_uuid.bytes,
        16,
    )
    view.deadline_tick_ms = deadline
    view.character = request.character
    view.shot_type = request.shot_type
    view.difficulty = request.difficulty
    view.stage = request.stage
    view.section_kind = request.section_kind
    view.section_index = request.section_index
    view.life = request.life
    view.power = request.power
    view.faith = request.faith
    view.invincible = int(request.invincible)
    view.infinite_lives = int(request.infinite_lives)
    view.infinite_power = int(request.infinite_power)
    view.time_lock = int(request.time_lock)
    view.auto_bomb = int(request.auto_bomb)
    view.no_faith_loss = int(request.no_faith_loss)
    view.section_dialogue_policy = request.section_dialogue_policy
    view.dialogue_advance_ms = request.dialogue_advance_ms
    view.dialogue_advance_deadline_tick_ms = 0
    publish_dialogue_lease(view, atomic, 0, 0, sequence, request.request_id)
    publish_sequence(view, atomic, sequence)


def request_is_busy(status: int, request_sequence: int, response_sequence: int) -> bool:
    return status in {2, 3, 4} or (request_sequence != response_sequence and request_sequence != 0)


class NamedMutex:
    def __init__(self, name: str, timeout_ms: int, busy_message: str):
        kernel32 = _kernel32()
        kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
        kernel32.CreateMutexW.restype = ctypes.c_void_p
        kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel32.WaitForSingleObject.restype = ctypes.c_uint32
        kernel32.ReleaseMutex.argtypes = [ctypes.c_void_p]
        kernel32.ReleaseMutex.restype = ctypes.c_int
        self.kernel32 = kernel32
        self.handle = kernel32.CreateMutexW(None, False, name)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        wait = kernel32.WaitForSingleObject(self.handle, timeout_ms)
        if wait not in (0, 0x80):
            kernel32.CloseHandle(self.handle)
            self.handle = None
            raise RuntimeError(busy_message)

    def close(self) -> None:
        if self.handle:
            self.kernel32.ReleaseMutex(self.handle)
            self.kernel32.CloseHandle(self.handle)
            self.handle = None


    def __enter__(self) -> NamedMutex:
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


def submit_mutex(pid: int) -> NamedMutex:
    return NamedMutex(rf"Local\THPracSceneBridgeSubmit_{pid}", 0,
                      "Another client is already submitting to this game process.")
