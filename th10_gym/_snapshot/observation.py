"""Typed Gymnasium spaces and conversion from decoded snapshots."""

from copy import deepcopy
from operator import itemgetter

import gymnasium as gym
import numpy as np

from ..readers import boss, lasers
from . import format as raw_observation
from . import tables as game_state

STATUS_FIELDS = ("available", "valid", "truncated", "unsupported", "read_fault")
BOSS_COLUMNS = {"registered": "?", **dict.fromkeys(("x", "y", "vx", "vy"), "f4"), "hp": "i4"}
BULLET_COLUMNS = {
    "slot": "i4", "state": "i4", "raw_flags": "u4", "spawn_ready": "u4",
    **dict.fromkeys(("x", "y", "vx", "vy", "speed", "angle", "width", "height"), "f4"),
}
_BULLET_DTYPE = np.dtype(list(BULLET_COLUMNS.items()))
_BULLET_VALUES = itemgetter(*BULLET_COLUMNS)
LASER_COLUMNS = {"state": "u4", **dict.fromkeys(lasers.OBSERVATION_FIELDS, "f4")}
STATE_COLUMNS = {
    name: {field: game_state.TABLE_DTYPES[name][field] for field in fields}
    for name, fields in game_state.OBSERVATION_FIELDS.items()
}
STATE_COLUMNS["enemy_callbacks"]["enemy_observation_index"] = np.dtype("i4")
PHASE_TABLES = ("spell", "visual_countdown", "enemy_callbacks")
PLAYER_COLUMNS = dict.fromkeys(("state", "lives", "power", "bomb_timer", "protection_timer",
                                "timer_current", "deathbomb_remaining"), "i4")
PLAYER_COLUMNS.update(bomb_active="?", timer_subframe="f4")
COUNTER_COLUMNS = {
    "score": "i8", "faith": "i4", "faith_decay_rate": "i4",
    "faith_timer_current": "i4", "faith_timer_fraction": "f4",
}
_OBSERVED_RECORD_FIELDS = frozenset({
    "player_state", "player_x", "player_y", "lives", "power", "bomb_active", "bomb_timer",
    "protection_timer", "state_timer_current", "player_timer_subframe", "deathbomb_remaining",
    "player_options", "player_options_active_count", "boss_slots", "boss_slot0", "boss_x", "boss_y", "boss_hp",
    "bullets", "lasers", "bullet_scanned", "bullet_captured", "laser_scanned", "laser_captured", "groups",
    "phase_timers", *COUNTER_COLUMNS, *STATE_COLUMNS,
})


def _columns(values, schema, capacity=None):
    columns = {}
    for name, dtype in schema.items():
        data = np.asarray(values(name), dtype=dtype)
        if data.dtype.kind == "f":
            finite = np.isfinite(data)
            data = np.where(finite, data, 0)
            columns[name + "_finite"] = _pad(finite, capacity)
        columns[name] = _pad(data, capacity)
    return columns


def _pad(data, capacity):
    if capacity is None:
        return data
    padded = np.zeros(capacity, dtype=data.dtype)
    padded[:len(data)] = data
    return padded


def _row_table(rows, schema, capacity=None):
    return _columns(lambda name: [row.get(name, 0) for row in rows], schema, capacity)


def _bullet_table(rows):
    if isinstance(rows, raw_observation.PackedBullets):
        return _columns(rows.column, BULLET_COLUMNS, raw_observation.BULLET_COUNT)
    data = np.fromiter(map(_BULLET_VALUES, rows), dtype=_BULLET_DTYPE, count=len(rows))
    return _columns(data.__getitem__, BULLET_COLUMNS, raw_observation.BULLET_COUNT)


class _ScalarBox(gym.spaces.Box):
    def contains(self, value):
        # A stacked Sequence iterates its 1D column as NumPy scalars.
        if isinstance(value, np.generic):
            value = np.asarray(value)
        return super().contains(value)


def _box(dtype, shape=()):
    dtype = np.dtype(dtype)
    if dtype.kind == "f":
        low, high = -np.inf, np.inf
    elif dtype.kind == "b":
        low, high = 0, 1
    else:
        bounds = np.iinfo(dtype)
        low, high = bounds.min, bounds.max
    box_type = gym.spaces.Box if shape else _ScalarBox
    return box_type(low, high, shape=shape, dtype=dtype)


def _table_space(schema, capacity=None):
    def column_space(dtype):
        if capacity is None:
            return gym.spaces.Sequence(_box(dtype), stack=True)
        return _box(dtype, (capacity,))

    columns = {name: column_space(dtype) for name, dtype in schema.items()}
    columns.update({name + "_finite": column_space("?") for name, dtype in schema.items()
                    if np.dtype(dtype).kind == "f"})
    return gym.spaces.Dict(columns)


def observation(record: dict) -> dict:
    options = record["player_options"]
    player_values = dict(
        state=record["player_state"], lives=record["lives"], power=record["power"],
        bomb_active=record["bomb_active"], bomb_timer=record["bomb_timer"],
        protection_timer=record["protection_timer"], timer_current=record["state_timer_current"],
        timer_subframe=record["player_timer_subframe"], deathbomb_remaining=record["deathbomb_remaining"],
    )
    player = {name: np.asarray(player_values[name], dtype=dtype) for name, dtype in PLAYER_COLUMNS.items()}
    player["position"] = np.array([record["player_x"], record["player_y"]], dtype=np.float32)
    player["options"] = {
        "position": np.column_stack((options["x"], options["y"])),
        "state": options["state"].copy(),
        "active_count": np.asarray(record["player_options_active_count"], dtype=np.int32),
    }

    phase = record["phase_timers"]
    addresses = {row["address"]: index for index, row in enumerate(record["enemies"])}
    callbacks = phase["enemy_callbacks"]
    callback_values = {name: callbacks[name] for name in callbacks.dtype.names}
    callback_values["enemy_observation_index"] = [addresses[address] for address in callbacks["enemy_address"]]

    return {
        "player": player,
        "boss": _row_table(record["boss_slots"], BOSS_COLUMNS, boss.SLOT_COUNT),
        "bullets": _bullet_table(record["bullets"]),
        "lasers": _row_table(record["lasers"], LASER_COLUMNS),
        "counts": np.array([record["bullet_scanned"], record["bullet_captured"],
                            record["laser_scanned"], record["laser_captured"]], dtype=np.int32),
        "groups": np.array([record["groups"][name] for name in STATUS_FIELDS], dtype=np.uint8),
        **{key: np.asarray(record[key], dtype=dtype) for key, dtype in COUNTER_COLUMNS.items()},
        **{key: _columns(record[key].__getitem__, columns) for key, columns in STATE_COLUMNS.items()
           if key not in PHASE_TABLES},
        "phase_timers": {
            "spell": _columns(phase["spell"].__getitem__, STATE_COLUMNS["spell"]),
            "visual_countdown": _columns(phase["visual_countdown"].__getitem__, STATE_COLUMNS["visual_countdown"]),
            "enemy_callbacks": _columns(callback_values.__getitem__, STATE_COLUMNS["enemy_callbacks"]),
        },
    }


def _diagnostic_columns(rows, observed, columns):
    names = [name for name in rows.dtype.names if name not in columns]
    # A NumPy multi-field view retains the original row width and hidden bytes.
    raw = np.empty(rows.shape, dtype=[(name, rows.dtype[name]) for name in names])
    for name in names:
        raw[name] = rows[name]

    nonfinite = {}
    for name in columns:
        finite = observed.get(name + "_finite")
        if finite is not None and not finite.all():
            indices = np.flatnonzero(~finite)
            nonfinite[name] = {"indices": indices, "bits": rows[name].view("<u4")[indices]}
    return raw, nonfinite


def diagnostics(record: dict, observed: dict) -> dict:
    """Return independent diagnostics for fields the typed observation cannot represent."""
    raw = deepcopy({key: value for key, value in record.items() if key not in _OBSERVED_RECORD_FIELDS})
    raw["boss_slots"] = [{"pointer": slot["pointer"]} for slot in record["boss_slots"]]
    raw["lasers"] = deepcopy([
        {key: value for key, value in row.items() if key not in LASER_COLUMNS} for row in record["lasers"]
    ])
    options = record["player_options"]
    if "x_fixed" in options.dtype.names:
        raw["player_options"] = options[["x_fixed", "y_fixed"]].astype([("x_fixed", "<i4"), ("y_fixed", "<i4")])

    phase = record["phase_timers"]
    raw_phase = raw["phase_timers"] = deepcopy({key: value for key, value in phase.items() if key not in PHASE_TABLES})
    nonfinite = {}
    for name, columns in STATE_COLUMNS.items():
        source, target, typed = (phase, raw_phase, observed["phase_timers"]) if name in PHASE_TABLES else (
            record, raw, observed
        )
        extra, bits = _diagnostic_columns(source[name], typed[name], columns)
        if extra.dtype.names:
            target[name] = extra
        if bits:
            nonfinite[name] = bits
    if nonfinite:
        raw["nonfinite_bits"] = nonfinite
    return raw


def observation_space() -> gym.spaces.Dict:
    return gym.spaces.Dict({
        "player": gym.spaces.Dict({
            **{name: _box(dtype) for name, dtype in PLAYER_COLUMNS.items()},
            "position": _box("f4", (2,)),
            "options": gym.spaces.Dict({
                "position": _box("f4", (4, 2)), "state": _box("i4", (4,)), "active_count": _box("i4"),
            }),
        }),
        "boss": _table_space(BOSS_COLUMNS, boss.SLOT_COUNT),
        "bullets": _table_space(BULLET_COLUMNS, raw_observation.BULLET_COUNT),
        "lasers": _table_space(LASER_COLUMNS),
        "counts": _box("i4", (4,)),
        "groups": _box("u1", (len(STATUS_FIELDS),)),
        **{key: _box(dtype) for key, dtype in COUNTER_COLUMNS.items()},
        **{key: _table_space(columns) for key, columns in STATE_COLUMNS.items() if key not in PHASE_TABLES},
        "phase_timers": gym.spaces.Dict({key: _table_space(STATE_COLUMNS[key]) for key in PHASE_TABLES}),
    })
