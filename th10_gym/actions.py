"""TH10 action IDs, input bits, and advisory action masks."""

import numpy as np

from ._game import BOMB_POWER_COST, DEATHBOMB_WINDOW_TICKS, PLAYER_ACTIVE, PLAYER_DEATHBOMB_WINDOW

SHOOT, BOMB, FOCUS = 0x01, 0x02, 0x04
UP, DOWN, LEFT, RIGHT = 0x10, 0x20, 0x40, 0x80
KNOWN_ACTIONS = 0x00F7


def valid_actions(actions):
    if actions & ~KNOWN_ACTIONS:
        return False
    return not ((actions & UP and actions & DOWN) or (actions & LEFT and actions & RIGHT))


ACTION_MASKS = tuple(mask for mask in range(KNOWN_ACTIONS + 1) if valid_actions(mask))
_MOVEMENT = (0, UP, DOWN, LEFT, RIGHT, UP | LEFT, UP | RIGHT, DOWN | LEFT, DOWN | RIGHT)
_SHOOT_MOVEMENT = tuple(SHOOT | direction | focus for focus in (0, FOCUS) for direction in _MOVEMENT)


def actions_for_mode(mode):
    if mode == "full_72":
        return ACTION_MASKS
    if mode == "always_shoot_36":
        return _SHOOT_MOVEMENT + tuple(mask | BOMB for mask in _SHOOT_MOVEMENT)
    if mode == "compact_19":
        return _SHOOT_MOVEMENT + (SHOOT | BOMB,)
    raise ValueError(f"Unknown TH10 action mode: {mode!r}")


def action_mask(actions, record):
    """Mark actions the game would honor now. Only Bomb can be unavailable."""
    state = record["player_state"]
    can_bomb = (
        record["power"] >= BOMB_POWER_COST and record.get("bomb_pointer", 0) != 0
        and record["bomb_active"] == 0
        and ((state == PLAYER_ACTIVE and record.get("dialogue_active") is False)
             or (state == PLAYER_DEATHBOMB_WINDOW
                 and record.get("state_timer_current", DEATHBOMB_WINDOW_TICKS) < DEATHBOMB_WINDOW_TICKS))
    )
    return np.array([can_bomb or not (action & BOMB) for action in actions], dtype=np.bool_)
