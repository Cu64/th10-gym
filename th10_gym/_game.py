"""Facts about retail TH10 1.00a shared by the Python runtime."""

# Values of the player object's state field.
PLAYER_ACTIVE = 1
PLAYER_DEATHBOMB_WINDOW = 4
PLAYER_RESPAWN_STATES = (0, 2)
PLAYER_LOCKED_STATES = (0, 2, 3)
PLAYER_NO_AUTO_COLLECT_STATES = (2, 4)

# Moving above this Y value starts automatic item collection.
ITEM_COLLECTION_LINE_Y = 128
# A bomb needs this much raw power.
BOMB_POWER_COST = 20
# Ticks a hit player can still bomb.
DEATHBOMB_WINDOW_TICKS = 8

# Addresses read directly during resets.
ADDRESS_GAMEPLAY_INPUT = 0x474E5C
ADDRESS_STAGE_TICK = 0x474C88  # Followed by the segment tick.

# Game mode flag set while a practice game runs.
GAME_MODE_PRACTICE = 0x10
