# Using TH10 Gym for RL

## Choose a scene

### Normal play

On a new environment, call `env.reset()` to start a normal run at Stage 1. To switch back from practice, call `env.reset(options={"mode": "normal"})`.

### Practice play

This reset starts practice at the first Stage 3 boss encounter.

```python
observation, info = env.reset(options={
    "mode": "practice",
    "stage": 3,
    "section": "boss",
})
```

| Practice option | Values |
| --- | --- |
| `stage` | 1–6. Defaults to 1. |
| `section`, `section_index` | Starting point within the stage. Defaults to `"stage-start"` and index 0. See [practice section numbers](#practice-section-numbers). |
| `life` | Spare lives, 0–9. Defaults to 9. Use 0 to start on the last life. |
| `power` | Starting power, 0–100. Defaults to 0. |
| `faith` | Starting faith, 0–999,990 in multiples of 10. Defaults to 50,000. |

#### Practice section numbers

Pick a `section` from the column headings and a `section_index` from your stage's row. Fight numbering starts at 0. For `"stage-portion"`, specify an index starting at 1.

| Stage | `stage-portion` | `midboss` | `boss` | `nonspell` | `spell` |
| --- | --- | --- | --- | --- | --- |
| 1 | 1–5 | 0–2 | 0–3 | 0–3 | 0–2 |
| 2 | 1–5 | 0 | 0–4 | 0–1 | 0–3 |
| 3 | 1–7 | 0 | 0–5 | 0–2 | 0–3 |
| 4 | 1–8 | 0 | 0–6 | 0–3 | 0–3 |
| 5 | 1–6 | 0–1 | 0–6 | 0–3 | 0–4 |
| 6 | 1–4 | — | 0–8 | 0–3 | 0–4 |

### Choose a character and difficulty

In either mode, put these settings in `options` when you call `env.reset()`.

| Option | Values |
| --- | --- |
| `character` | `"reimu"` (default) or `"marisa"`. |
| `shot_type` | `"A"`, `"B"` (default), or `"C"`. |
| `difficulty` | 0 = Easy, 1 = Normal (default), 2 = Hard, 3 = Lunatic. |

> [!TIP]
> We recommend starting with Reimu B. She shoots straight ahead, so we think the agent will have an easier time learning to move directly below a target to hit it.

## Episode boundaries

Call `env.reset()` after `step()` returns `terminated=True` or `truncated=True`. Pass no options to reuse the previous settings. If you pass `options`, include every setting you want to carry over.

| Condition | Result |
| --- | --- |
| Game Over or loss of the last life | `terminated=True`. |
| Loss of any life | `terminated=True` if you pass `terminate_on_miss=True` to `gym.make()`. |
| Practice section cleared | `info["section_cleared"]` is true. Gym also sets `truncated=True` if the step did not terminate. |
| Step limit reached | `truncated=True` if you set a limit with `gym.make(..., max_episode_steps=N)`. |

If you estimate future rewards during training, use zero when `terminated=True` or when a practice clear completes your task. If only a step limit ended the episode, estimate future rewards from the final observation returned by `step()`.

Touhou 10 uses its own random number generator. `reset(seed=...)` seeds the Gymnasium environment's generator.

To add a stopping condition, pass `terminated_fn` to `gym.make()`. The function takes the current game state and returns `True` when the task should end.

## Rewards

Gym returns zero reward by default. To reward score changes once per step, use `info["delta_score"]` in a wrapper around `step()`.

To reward each game update, pass a function such as `reward_fn=score_reward` to `gym.make()`. It receives the game state before the update, the requested controls as an integer, and the game state after the update. Treat the inputs as read-only. If one step spans several updates, Gym adds their rewards together.

```python
def score_reward(previous, _input_mask, current):
    return float(current["score"] - previous["score"])
```

| Field in `info` | What it tells you |
| --- | --- |
| `miss_occurred` | The player lost a life during this step. |
| `last_life_lost` | The player lost their last life. |
| `bomb_used` | The player started a bomb during this step. |
| `delta_score`, `delta_faith` | How much the counters changed during the step. Multiply `delta_score` by 10 for the change in displayed score. |
| `crossed_collection_line` | The player moved above the item collection line at `y=128` during the step. |
| `item_auto_collect_active` | Automatic item collection is active at the end of the step. |
| `total_boss_hp` | Total remaining health across the bosses in the observation. |

## Game ticks and agent steps

During play, `step()` usually advances the game by one update, or tick. A step can include extra ticks, for example to advance dialogue. Read `info["ticks_elapsed"]` to see how many ticks passed.

If `gamma` is your discount factor per game tick, multiply the estimated future reward after the step by `gamma ** info["ticks_elapsed"]`. A three-tick step uses `gamma ** 3`.

To wait until the player is back before choosing another action after losing a spare life, set `auto_advance_respawn=True` in `gym.make()`.

## Options for collecting training data

### Reuse game processes

Gym tries to reuse the open game to save startup time. Set `advance_game_over=True` in `gym.make()` to let `reset()` use the game's Retry option after the last life.

To relaunch the game every episode, set `reuse_normal_process=False` or `reuse_practice_process=False` for the mode you use.

### Skip drawing during collection

For faster training data collection, set `skip_render=True` in `gym.make()`. Gym skips drawing during play. The game still opens a window. [DXVK and frame-rate settings](dxvk.md)

### Mute game audio

Pass `mute_audio=True` to `gym.make()` to start with sound muted. To change it during play, call `env.unwrapped.set_muted(True)` or `env.unwrapped.set_muted(False)`.
