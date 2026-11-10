# Observation space

## Build a fixed-size input from nearby bullets

Call `offsets, has_bullet = nearby_bullets(observation)` after `env.reset()` or `env.step()`. The function returns 32 pairs of X and Y offsets from the player, closest first. Use `has_bullet` to tell which rows contain a bullet.

```python
import numpy as np


def nearby_bullets(observation):
    limit = 32
    bullets = observation["bullets"]
    count = int(observation["counts"][1])
    valid = bullets["x_finite"][:count] & bullets["y_finite"][:count]
    positions = np.column_stack((bullets["x"][:count], bullets["y"][:count]))
    relative = positions[valid] - observation["player"]["position"]
    distances = np.linalg.norm(relative, axis=1)
    nearest = relative[np.argsort(distances)[:limit]]

    offsets = np.zeros((limit, 2), dtype=np.float32)
    has_bullet = np.zeros(limit, dtype=bool)
    offsets[:len(nearest)] = nearest
    has_bullet[:len(nearest)] = True
    return offsets, has_bullet
```

## Field reference

The observation is a nested dictionary of NumPy arrays. Its Gymnasium space is `env.observation_space`.

| Field | How to read it |
| --- | --- |
| `player.position` | `[x, y]`. X increases to the right and Y increases downward. |
| `player.options.position` | X and Y for the helpers that move with the player. `player.options.state` follows the same row order. |
| `player.lives` | Spare lives. Zero means the current life is the last one. |
| `bullets` | The first `counts[1]` rows contain bullets. The rest are zeros. `width` and `height` give the full hitbox dimensions. |
| `boss` | Use `registered` to select rows that contain a boss. |
| `enemies`, `lasers` | The number of rows can change between steps. `enemies` includes bosses. Laser shape fields depend on the laser type. |
| `items` | Use `state != 0` to select active items. The table has 2,198 rows when item data is available and zero otherwise. |
| `player_shots`, `player_attacks` | 128 shot rows and 33 attack rows, including inactive entries. |
| `phase_timers.spell`, `phase_timers.visual_countdown` | One row when present and zero rows otherwise. |
| `phase_timers.enemy_callbacks` | `enemy_observation_index` gives the corresponding row in `enemies`. |
| `score`, `faith` | Game counters. To get the score shown in-game, multiply `score` by 10. |

Check the matching `*_finite` field before using a value. Gym sets the flag to `False` for padding and for any NaN or infinity it replaces with zero.

If you track bullets across steps, their row numbers can change and the game can reuse a bullet's `slot` value for a new bullet.

<details>
<summary>All fields and array shapes</summary>

`()` is the shape of a scalar. `(N,)` is the shape of a column with one value per row. Columns in the same table have the same length. When a table has no rows, its columns are empty.

```text
boss:
  hp: int32 (8,)
  registered: bool (8,)
  vx: float32 (8,)
  vx_finite: bool (8,)
  vy: float32 (8,)
  vy_finite: bool (8,)
  x: float32 (8,)
  x_finite: bool (8,)
  y: float32 (8,)
  y_finite: bool (8,)
bullets:
  angle: float32 (2000,)
  angle_finite: bool (2000,)
  height: float32 (2000,)
  height_finite: bool (2000,)
  raw_flags: uint32 (2000,)
  slot: int32 (2000,)
  spawn_ready: uint32 (2000,)
  speed: float32 (2000,)
  speed_finite: bool (2000,)
  state: int32 (2000,)
  vx: float32 (2000,)
  vx_finite: bool (2000,)
  vy: float32 (2000,)
  vy_finite: bool (2000,)
  width: float32 (2000,)
  width_finite: bool (2000,)
  x: float32 (2000,)
  x_finite: bool (2000,)
  y: float32 (2000,)
  y_finite: bool (2000,)
counts: int32 (4,)
enemies:
  contact_extent_x: float32 (N,)
  contact_extent_x_finite: bool (N,)
  contact_extent_y: float32 (N,)
  contact_extent_y_finite: bool (N,)
  damage_extent_x: float32 (N,)
  damage_extent_x_finite: bool (N,)
  damage_extent_y: float32 (N,)
  damage_extent_y_finite: bool (N,)
  damage_reduction_timer: int32 (N,)
  flags: uint32 (N,)
  life: int32 (N,)
  player_collision_timer: int32 (N,)
  registration_slot: int32 (N,)
  update_timer_current: int32 (N,)
  update_timer_flags: uint32 (N,)
  update_timer_previous: int32 (N,)
  update_timer_subframe: float32 (N,)
  update_timer_subframe_finite: bool (N,)
  vx: float32 (N,)
  vx_finite: bool (N,)
  vy: float32 (N,)
  vy_finite: bool (N,)
  vz: float32 (N,)
  vz_finite: bool (N,)
  x: float32 (N,)
  x_finite: bool (N,)
  y: float32 (N,)
  y_finite: bool (N,)
  z: float32 (N,)
  z_finite: bool (N,)
faith: int32 ()
faith_decay_rate: int32 ()
faith_timer_current: int32 ()
faith_timer_fraction: float32 ()
groups: uint8 (5,)
items:
  attraction_speed: float32 (N,)
  attraction_speed_finite: bool (N,)
  delay: int32 (N,)
  kind: int32 (N,)
  sprite_kind: int32 (N,)
  state: int32 (N,)
  timer_current: int32 (N,)
  timer_flags: uint32 (N,)
  timer_previous: int32 (N,)
  timer_subframe: float32 (N,)
  timer_subframe_finite: bool (N,)
  vx: float32 (N,)
  vx_finite: bool (N,)
  vy: float32 (N,)
  vy_finite: bool (N,)
  vz: float32 (N,)
  vz_finite: bool (N,)
  x: float32 (N,)
  x_finite: bool (N,)
  y: float32 (N,)
  y_finite: bool (N,)
  z: float32 (N,)
  z_finite: bool (N,)
lasers:
  angle: float32 (N,)
  angle_finite: bool (N,)
  raw_dx: float32 (N,)
  raw_dx_finite: bool (N,)
  raw_dy: float32 (N,)
  raw_dy_finite: bool (N,)
  raw_geometry40: float32 (N,)
  raw_geometry40_finite: bool (N,)
  raw_geometry44: float32 (N,)
  raw_geometry44_finite: bool (N,)
  state: uint32 (N,)
  x: float32 (N,)
  x_finite: bool (N,)
  y: float32 (N,)
  y_finite: bool (N,)
phase_timers:
  enemy_callbacks:
    enemy_observation_index: int32 (N,)
    life_threshold: int32 (N,)
    slot: uint32 (N,)
    timer_threshold: int32 (N,)
  spell:
    current: int32 (N,)
    flags: uint32 (N,)
    previous: int32 (N,)
    subframe: float32 (N,)
    subframe_finite: bool (N,)
  visual_countdown:
    value: int32 (N,)
player:
  bomb_active: bool ()
  bomb_timer: int32 ()
  deathbomb_remaining: int32 ()
  lives: int32 ()
  options:
    active_count: int32 ()
    position: float32 (4, 2)
    state: int32 (4,)
  position: float32 (2,)
  power: int32 ()
  protection_timer: int32 ()
  state: int32 ()
  timer_current: int32 ()
  timer_subframe: float32 ()
player_attacks:
  active_flags: uint32 (N,)
  angle: float32 (N,)
  angle_delta: float32 (N,)
  angle_delta_finite: bool (N,)
  angle_finite: bool (N,)
  collision_interval: int32 (N,)
  damage: int32 (N,)
  extent_x: float32 (N,)
  extent_x_finite: bool (N,)
  extent_y: float32 (N,)
  extent_y_finite: bool (N,)
  hit_accumulator: int32 (N,)
  hit_cap: int32 (N,)
  motion_angle: float32 (N,)
  motion_angle_finite: bool (N,)
  motion_angular_step: float32 (N,)
  motion_angular_step_finite: bool (N,)
  motion_flags: uint32 (N,)
  radius: float32 (N,)
  radius_delta: float32 (N,)
  radius_delta_finite: bool (N,)
  radius_finite: bool (N,)
  timer_current: int32 (N,)
  timer_flags: uint32 (N,)
  timer_previous: int32 (N,)
  timer_step: float32 (N,)
  timer_step_delta: float32 (N,)
  timer_step_delta_finite: bool (N,)
  timer_step_finite: bool (N,)
  timer_subframe: float32 (N,)
  timer_subframe_finite: bool (N,)
  vx: float32 (N,)
  vx_finite: bool (N,)
  vy: float32 (N,)
  vy_finite: bool (N,)
  vz: float32 (N,)
  vz_finite: bool (N,)
  x: float32 (N,)
  x_finite: bool (N,)
  y: float32 (N,)
  y_finite: bool (N,)
  z: float32 (N,)
  z_finite: bool (N,)
player_shots:
  angle: float32 (N,)
  angle_finite: bool (N,)
  collided_this_update: int32 (N,)
  motion_flags: uint32 (N,)
  polar_delta: float32 (N,)
  polar_delta_finite: bool (N,)
  polar_magnitude: float32 (N,)
  polar_magnitude_finite: bool (N,)
  speed_or_angle_step: float32 (N,)
  speed_or_angle_step_finite: bool (N,)
  state: int32 (N,)
  timer_current: int32 (N,)
  timer_flags: uint32 (N,)
  timer_previous: int32 (N,)
  timer_subframe: float32 (N,)
  timer_subframe_finite: bool (N,)
  vx: float32 (N,)
  vx_finite: bool (N,)
  vy: float32 (N,)
  vy_finite: bool (N,)
  vz: float32 (N,)
  vz_finite: bool (N,)
  x: float32 (N,)
  x_finite: bool (N,)
  y: float32 (N,)
  y_finite: bool (N,)
  z: float32 (N,)
  z_finite: bool (N,)
score: int64 ()
```

</details>

## Snapshot diagnostics

Read the current stage from `info["raw"]["stage"]`. See [episode boundaries](training.md#episode-boundaries) for the fields that tell you why an episode ended.
