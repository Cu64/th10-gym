"""Row layouts of the entity tables carried by each snapshot."""

import numpy as np

ITEM_COUNT = 2198
SHOT_COUNT = 128
ATTACK_COUNT = 33  # 32 live slots and the allocator overflow row.
CALLBACK_COUNT = 8


def _timer(prefix=""):
    fields = (("previous", "<i4"), ("current", "<i4"), ("subframe", "<f4"),
              ("scale_pointer", "<u4"), ("flags", "<u4"))
    return [(prefix + name, dtype) for name, dtype in fields]


def _vector(names, dtype="<f4"):
    return [(name, dtype) for name in names.split()]


ITEM_DTYPE = np.dtype([
    *_vector("x y z vx vy vz"), *_timer("timer_"),
    ("state", "<i4"), ("kind", "<i4"), ("sprite_kind", "<i4"),
    ("attraction_speed", "<f4"), ("delay", "<i4"),
])
SHOT_DTYPE = np.dtype([
    *_timer("timer_"), *_vector("x y z vx vy vz speed_or_angle_step angle polar_magnitude polar_delta"),
    ("motion_flags", "<u4"), ("state", "<i4"),
    ("primary_vm_id", "<u4"), ("secondary_vm_id", "<u4"),
    ("tracked_enemy", "<u4"), ("collided_this_update", "<i4"),
    ("collision_vm_pending", "<i4"), ("descriptor_pointer", "<u4"),
])
ATTACK_DTYPE = np.dtype([
    *_vector("radius radius_delta angle angle_delta extent_x extent_y x y z vx vy vz"),
    *_vector("motion_angle motion_angular_step timer_step timer_step_delta"),
    ("motion_flags", "<u4"), *_timer("timer_"),
    ("damage", "<i4"), ("hit_accumulator", "<i4"), ("hit_cap", "<i4"),
    ("collision_interval", "<i4"), ("active_flags", "<u4"),
])
ENEMY_DTYPE = np.dtype([
    *_vector("node address next previous", "<u4"),
    *_vector("x y z vx vy vz"),
    *_vector("damage_extent_x damage_extent_y contact_extent_x contact_extent_y"),
    *_vector("ecl_int_0 ecl_int_1 ecl_int_2 ecl_int_3", "<i4"),
    *_vector("ecl_float_0 ecl_float_1 ecl_float_2 ecl_float_3"),
    *_timer("update_timer_"),
    ("life", "<i4"), ("raw_2400", "<i4"),
    ("damage_reduction_timer", "<i4"), ("player_collision_timer", "<i4"),
    ("flags", "<u4"), ("registration_slot", "<i4"),
])
CALLBACK_LAYOUT = np.dtype([
    ("life_threshold", "<i4"), ("timer_threshold", "<i4"),
    ("life_callback_pointer", "<u4"), ("timer_callback_pointer", "<u4"),
])
CALLBACK_DTYPE = np.dtype([("enemy_address", "<u4"), ("slot", "<u4"), *CALLBACK_LAYOUT.descr])
SPELL_LAYOUT = np.dtype(_timer())
COUNTDOWN_LAYOUT = np.dtype([("value", "<i4")])

TABLE_DTYPES = {
    "enemies": ENEMY_DTYPE,
    "items": ITEM_DTYPE,
    "player_shots": SHOT_DTYPE,
    "player_attacks": ATTACK_DTYPE,
    "enemy_callbacks": CALLBACK_DTYPE,
    "spell": SPELL_LAYOUT,
    "visual_countdown": COUNTDOWN_LAYOUT,
}

# Typed observation columns exclude process pointers and game-script internals.
OBSERVATION_FIELDS = {
    "enemies": tuple(name for name in ENEMY_DTYPE.names if name not in {
        "node", "address", "next", "previous",
        "ecl_int_0", "ecl_int_1", "ecl_int_2", "ecl_int_3",
        "ecl_float_0", "ecl_float_1", "ecl_float_2", "ecl_float_3",
        "raw_2400", "update_timer_scale_pointer",
    }),
    "items": tuple(name for name in ITEM_DTYPE.names if name != "timer_scale_pointer"),
    "player_shots": tuple(name for name in SHOT_DTYPE.names if name not in {
        "primary_vm_id", "secondary_vm_id", "tracked_enemy", "descriptor_pointer",
        "collision_vm_pending", "timer_scale_pointer",
    }),
    "player_attacks": tuple(name for name in ATTACK_DTYPE.names if name != "timer_scale_pointer"),
    "enemy_callbacks": ("slot", "life_threshold", "timer_threshold"),
    "spell": tuple(name for name in SPELL_LAYOUT.names if name != "scale_pointer"),
    "visual_countdown": ("value",),
}


def options_rows(options):
    dtype = np.dtype([("x", "<f4"), ("y", "<f4"), ("state", "<i4"),
                      ("x_fixed", "<i4"), ("y_fixed", "<i4")])
    rows = np.empty(4, dtype=dtype)
    for index, (state, x, y) in enumerate(options):
        rows[index] = (int(x) * 0.01, int(y) * 0.01, state, x, y)
    return rows
