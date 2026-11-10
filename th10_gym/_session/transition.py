"""Group the ticks that make up one Gym step and total their effects."""

from collections.abc import Callable
from dataclasses import dataclass

from .._game import ITEM_COLLECTION_LINE_Y, PLAYER_RESPAWN_STATES
from .._snapshot.format import GAME_OVER, GROUP_RESOURCES

_RESPAWN_TICK_LIMIT = 240
_GAME_OVER_TICK_LIMIT = 240
_DIALOGUE_TICK_LIMIT = 3600
NEUTRAL_INPUT = 0


def dialogue_active(record):
    return bool(record.get("dialogue_active")) and not record.get("terminal_outcome")


def _advance_dialogue(session, count):
    if count > _DIALOGUE_TICK_LIMIT:
        raise RuntimeError("TH10 did not finish dialogue within the tick limit")
    return session.step_dialogue()


def finish_reset_dialogue(session, record):
    """Run any dialogue active at a reset so the caller starts in playable state."""
    ticks = 0
    while dialogue_active(record):
        ticks += 1
        record = _advance_dialogue(session, ticks)
    if not ticks:
        return record, 0
    session.end_dialogue()
    if record.get("terminal_outcome"):
        raise RuntimeError("TH10 reached a terminal outcome during reset dialogue")
    return record, ticks


@dataclass
class Transition:
    """The combined result of the ticks run for one Gym step."""

    record: dict
    reward: float = 0.0
    ticks: int = 0
    miss: bool = False
    last_life_lost: bool = False
    bomb: bool = False
    crossed_collection_line: bool = False
    terminated: bool = False

    def _append_tick(self, record, mask, reward):
        self.reward += reward(self.record, mask, record)
        self.ticks += 1
        self.record = record

    def _note_life_change(self, previous, record):
        lost_life = record["lives"] < previous["lives"]
        self.last_life_lost = _last_life_lost(previous, record)
        self.miss |= lost_life
        return lost_life

    def _note_action_events(self, previous, record):
        self.bomb |= _bomb_started(previous, record)
        self.crossed_collection_line |= previous["player_y"] >= ITEM_COLLECTION_LINE_Y > record["player_y"]


@dataclass(frozen=True, slots=True)
class TransitionPolicy:
    reward: Callable[[dict, int, dict], float]
    terminated: Callable[[dict, bool], bool]
    auto_respawn: bool
    advance_game_over: bool

    def run(self, session, previous, first, action):
        """Extend the first tick through dialogue, respawn, and Game Over as configured."""
        result = Transition(previous)
        lost_life = result._note_life_change(previous, first)
        result._note_action_events(previous, first)
        result._append_tick(first, action, self.reward)
        result.terminated = result.last_life_lost or self.terminated(first, lost_life)
        recovering = self.auto_respawn and lost_life
        dialogue_ticks = 0
        respawn_ticks = 1
        while not result.terminated and not result.record.get("terminal_outcome"):
            before = result.record
            if dialogue_active(before):
                dialogue_ticks += 1
                record = _advance_dialogue(session, dialogue_ticks)
            elif recovering and before["player_state"] in PLAYER_RESPAWN_STATES:
                if respawn_ticks == _RESPAWN_TICK_LIMIT:
                    raise RuntimeError("TH10 did not finish respawning within the tick limit")
                respawn_ticks += 1
                record = session.step(NEUTRAL_INPUT)
            else:
                break
            result._append_tick(record, NEUTRAL_INPUT, self.reward)
            lost_life = result._note_life_change(before, record)
            recovering = self.auto_respawn and (
                lost_life or (recovering and record["player_state"] in PLAYER_RESPAWN_STATES)
            )
            result.terminated = result.last_life_lost or self.terminated(record, lost_life)
            result._note_action_events(before, record)
        if dialogue_ticks:
            session.end_dialogue()
        if self.advance_game_over and result.last_life_lost:
            _reach_game_over(session, result, self.reward)
        return result


def _reach_game_over(session, result, reward):
    for _ in range(_GAME_OVER_TICK_LIMIT):
        if result.record.get("terminal_outcome"):
            break
        result._append_tick(session.step(NEUTRAL_INPUT), NEUTRAL_INPUT, reward)
    if result.record.get("terminal_outcome") != GAME_OVER:
        raise RuntimeError("TH10 did not reach the Game Over menu after the last life")


def _last_life_lost(previous, record):
    return (previous["lives"] == 0 and record["lives"] == -1
            and bool(previous.get("groups", {}).get("valid", 0)
                     & record.get("groups", {}).get("valid", 0) & GROUP_RESOURCES))


def _bomb_started(previous, record):
    owner = record.get("bomb_pointer")
    return bool(owner and owner == previous.get("bomb_pointer")
                and previous["bomb_active"] == 0 and record["bomb_active"] == 1)
