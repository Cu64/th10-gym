"""Decode the variable block of entity tables that follows a snapshot's fixed record."""

import math
import struct

import numpy as np

from .._game import DEATHBOMB_WINDOW_TICKS, PLAYER_DEATHBOMB_WINDOW
from ..readers import boss, lasers
from . import format as wire
from . import tables

HEADER = struct.Struct("<5if3Iifi7I")
BOSS = struct.Struct("<I4fi")
LASER = struct.Struct("<4I7f")
OPTION = np.dtype([("state", "<i4"), ("x_fixed", "<i4"), ("y_fixed", "<i4")])
ENEMY = np.dtype([("row", tables.ENEMY_DTYPE),
                  ("callbacks", tables.CALLBACK_LAYOUT, (tables.CALLBACK_COUNT,))])
assert HEADER.size == 76 and ENEMY.itemsize == 260

MAX_ENEMY_ROWS, MAX_LASER_ROWS = 1024, 8192
OPTION_COUNT = 4


def decode_entities(data: memoryview, record: dict) -> None:
    if len(data) < HEADER.size:
        raise wire.ObservationError("Short snapshot entity tables")
    (stage, score, faith, decay, faith_current, faith_fraction, item_manager, spell_owner,
     countdown_owner, state_timer, player_subframe, options_active, dialogue_word, enemy_count,
     laser_manager, enemy_rows, laser_rows, qpc_low, qpc_high) = HEADER.unpack_from(data)
    boss_count = boss.SLOT_COUNT if record["boss_manager"] else 0
    item_count = tables.ITEM_COUNT if item_manager else 0
    table_counts = (("items", item_count), ("player_shots", tables.SHOT_COUNT),
                    ("player_attacks", tables.ATTACK_COUNT))
    fixed_bytes = sum(tables.TABLE_DTYPES[key].itemsize * count for key, count in table_counts)
    expected = (HEADER.size + boss_count * BOSS.size + fixed_bytes + OPTION_COUNT * OPTION.itemsize
                + tables.SPELL_LAYOUT.itemsize * bool(spell_owner)
                + tables.COUNTDOWN_LAYOUT.itemsize * bool(countdown_owner)
                + enemy_rows * ENEMY.itemsize + laser_rows * LASER.size)
    if (expected != len(data) or enemy_rows > MAX_ENEMY_ROWS or laser_rows > MAX_LASER_ROWS
            or (enemy_rows or enemy_count) and not boss_count or laser_rows and not laser_manager):
        raise wire.ObservationError("Invalid snapshot entity table counts or size")

    laser_rows_decoded, all_finite = _decode_tables(
        data, record, boss_count=boss_count, table_counts=table_counts,
        spell_owner=spell_owner, countdown_owner=countdown_owner, enemy_rows=enemy_rows,
    )
    if laser_manager:
        record["groups"]["available"] |= wire.GROUP_LASERS
        if all_finite:
            record["groups"]["valid"] |= wire.GROUP_LASERS

    capture_end_qpc = qpc_high << 32 | qpc_low
    if capture_end_qpc < record["qpc"]:
        raise wire.ObservationError("Snapshot capture end precedes its start")
    in_deathbomb_window = record["player_state"] == PLAYER_DEATHBOMB_WINDOW
    record.update(stage=stage, score=score, faith=faith, faith_decay_rate=decay,
                  faith_timer_current=faith_current, faith_timer_fraction=faith_fraction,
                  item_manager=item_manager, enemy_manager_count=enemy_count if boss_count else None,
                  player_options_active_count=options_active, state_timer_current=state_timer,
                  player_timer_subframe=player_subframe,
                  deathbomb_remaining=max(DEATHBOMB_WINDOW_TICKS - state_timer, 0) if in_deathbomb_window else 0,
                  dialogue_active=bool(dialogue_word) if countdown_owner else None, lasers=laser_rows_decoded,
                  laser_scanned=laser_rows, laser_captured=laser_rows, capture_end_qpc=capture_end_qpc)


def _decode_tables(data, record, *, boss_count, table_counts, spell_owner, countdown_owner, enemy_rows):
    offset = HEADER.size

    def rows(dtype, count):
        nonlocal offset
        result = np.frombuffer(data, dtype=dtype, count=count, offset=offset)
        offset += count * dtype.itemsize
        return result

    boss_end = offset + boss_count * BOSS.size
    record["boss_slots"] = _decode_boss_slots(data[offset:boss_end])
    offset = boss_end
    for key, count in table_counts:
        record[key] = rows(tables.TABLE_DTYPES[key], count)
    options = rows(OPTION, OPTION_COUNT)
    record["player_options"] = tables.options_rows(options)
    spell = rows(tables.SPELL_LAYOUT, int(bool(spell_owner)))
    countdown = rows(tables.COUNTDOWN_LAYOUT, int(bool(countdown_owner)))
    enemies = rows(ENEMY, enemy_rows)
    record["enemies"] = enemies["row"].copy()
    record["phase_timers"] = dict(spell_owner=spell_owner, countdown_owner=countdown_owner,
                                  spell=spell, visual_countdown=countdown,
                                  enemy_callbacks=_decode_enemy_callbacks(enemies))

    return _decode_laser_rows(data[offset:])


def _decode_boss_slots(data: memoryview) -> list[dict]:
    slots = []
    for slot, (pointer, x, y, vx, vy, hp) in enumerate(BOSS.iter_unpack(data)):
        entry = dict(slot=slot, pointer=f"0x{pointer:08X}", registered=bool(pointer))
        if pointer:
            if not all(map(math.isfinite, (x, y, vx, vy))):
                raise wire.ObservationError("Non-finite boss state")
            entry.update(x=x, y=y, vx=vx, vy=vy, hp=hp)
        slots.append(entry)
    return slots


def _decode_enemy_callbacks(enemies: np.ndarray) -> np.ndarray:
    enemy_rows = len(enemies)
    callback_count = tables.CALLBACK_COUNT
    callbacks = np.empty((enemy_rows, callback_count), dtype=tables.CALLBACK_DTYPE)
    callbacks["enemy_address"] = enemies["row"]["address"][:, None]
    callbacks["slot"] = np.arange(callback_count, dtype="<u4")
    for field in tables.CALLBACK_LAYOUT.names:
        callbacks[field] = enemies["callbacks"][field]
    return callbacks.reshape(-1)


def _decode_laser_rows(data: memoryview) -> tuple[list[dict], bool]:
    decoded = []
    all_finite = True
    offset = 0
    for node, vtable, next_node, state, *fields in LASER.iter_unpack(data):
        nonfinite = {}
        entry = dict(node=node, next=next_node, state=state, vtable=vtable, nonfinite_bits=nonfinite)
        for index, (name, value) in enumerate(zip(lasers.OBSERVATION_FIELDS, fields, strict=True)):
            if not math.isfinite(value):
                nonfinite[name], = struct.unpack_from("<I", data, offset + 16 + index * 4)
                value = None
                all_finite = False
            entry[name] = value
        decoded.append(entry)
        offset += LASER.size
    return decoded, all_finite
