# Action space

Set `action_mode` in `gym.make()` to choose the controls your agent can use. For example, use `action_mode="compact_19"` to keep Shoot held and limit bombing to a stationary action. All three modes use a Gymnasium `Discrete` action space.

| Mode | Size | Controls |
| --- | ---: | --- |
| `full_72` (default) | 72 | Nine movement choices, each with independent Shoot, Focus, and Bomb controls. Action 0 sends no input. |
| `always_shoot_36` | 36 | Nine movement choices with Shoot held, each with optional Focus and Bomb. |
| `compact_19` | 19 | Nine movement choices with Shoot held, each with optional Focus, plus one stationary Shoot + Bomb action. |

> [!NOTE]
> In the default mode, Shoot is off unless the agent selects an action that includes it.

## Shooting modes

In `always_shoot_36` and `compact_19`, start with the ID for the movement you want. Add 9 to hold Focus as well.

| ID | Movement |
| ---: | --- |
| 0 | Stay |
| 1 | Up |
| 2 | Down |
| 3 | Left |
| 4 | Right |
| 5 | Up-left |
| 6 | Up-right |
| 7 | Down-left |
| 8 | Down-right |

In `always_shoot_36`, add 18 to any of the first 18 IDs to add Bomb. In `compact_19`, action 18 is stationary Shoot + Bomb.

## Full mode

In `full_72`, start with the base ID for the movement you want. Add together the offsets for each button you want to hold. Shoot adds 1, Bomb adds 2, and Focus adds 4. For example, Right + Shoot + Bomb is action `48 + 1 + 2 = 51`.

| Movement | Base ID |
| --- | ---: |
| Stay | 0 |
| Up | 8 |
| Down | 16 |
| Left | 24 |
| Up-left | 32 |
| Down-left | 40 |
| Right | 48 |
| Up-right | 56 |
| Down-right | 64 |

`th10_gym.env.ACTION_MASKS[action]` maps a full-mode ID to its game input bits. In every mode, `step()` returns the requested input bits in `info["input_mask"]`.

## Action availability

To skip unavailable Bomb actions, use `info["action_mask"]` from `reset()` or `step()` when choosing the next action. The mask has one entry per action ID. An entry of `False` marks an unavailable action.

```python
action = env.action_space.sample(mask=info["action_mask"].astype("int8"))
observation, reward, terminated, truncated, info = env.step(action)
```

Check `info["control_locked"]` for periods when ordinary controls may have no effect, such as respawn.
