"""Scene choices, defaults, and validation shared by Gym and the local CLI."""

import uuid
from dataclasses import dataclass

DIALOGUE_LEASE_MS = 10_000
MAX_TTL_MS = 300_000
MIN_TTL_MS = 1_000

CHARACTERS = {"reimu": 0, "marisa": 1}
SHOT_TYPES = {"A": 0, "B": 1, "C": 2}
SECTION_KINDS = {
    "stage-start": 0,
    "stage-portion": 1,
    "midboss": 2,
    "boss": 3,
    "nonspell": 4,
    "spell": 5,
    "normal-game": 6,
}
SECTION_LABELS = {value: key for key, value in SECTION_KINDS.items()}
DIALOGUE_POLICIES = {"keep": 0, "skip": 1}
DIALOGUE_LABELS = {value: key for key, value in DIALOGUE_POLICIES.items()}
PORTION_COUNTS = (5, 5, 7, 8, 6, 4)
SECTION_COUNTS = {
    2: (3, 1, 1, 1, 2, 0),
    3: (4, 5, 6, 7, 7, 9),
    4: (4, 2, 3, 4, 4, 4),
    5: (3, 4, 4, 4, 5, 5),
}


@dataclass(frozen=True)
class SceneOptions:
    character: str
    shot_type: str
    difficulty: int
    stage: int
    section: str
    section_index: int
    section_dialogue: str
    life: int | None
    power: int | None
    faith: int | None
    timeout_ms: int
    invincible: bool = False
    infinite_lives: bool = False
    infinite_power: bool = False
    time_lock: bool = False
    auto_bomb: bool = False
    no_faith_loss: bool = False
    dialogue_advance_ms: int = 0
    request_id: str | None = None


@dataclass(frozen=True)
class SceneRequest:
    request_id: str
    character: int
    shot_type: int
    difficulty: int
    stage: int
    section_kind: int
    section_index: int
    life: int
    power: int
    faith: int
    invincible: bool
    infinite_lives: bool
    infinite_power: bool
    time_lock: bool
    auto_bomb: bool
    no_faith_loss: bool
    section_dialogue_policy: int
    dialogue_advance_ms: int
    timeout_ms: int


def _validate_normal_game(options: SceneOptions) -> None:
    if options.stage != 1 or options.section_index != 0:
        raise ValueError("normal-game requires stage 1 and section-index 0.")
    if any(value is not None for value in (options.life, options.power, options.faith)):
        raise ValueError("normal-game uses natural resources; omit life, power, and faith presets.")
    if options.section_dialogue != "keep":
        raise ValueError("normal-game requires section-dialogue keep.")


def _validate_practice_resources(options: SceneOptions) -> None:
    if options.life is None or not 0 <= options.life <= 9:
        raise ValueError("life is required and must be in the range 0..9.")
    if options.power is None or not 0 <= options.power <= 100:
        raise ValueError("power is required and must be in the range 0..100.")
    if options.faith is None or not 0 <= options.faith <= 999_990 or options.faith % 10:
        raise ValueError("faith is required, must be in 0..999990, and must use a step of 10.")


def _validate_section_index(options: SceneOptions, kind: int, index: int) -> None:
    if kind == 0 and index != 0:
        raise ValueError("stage-start requires section-index 0.")
    if kind == 1 and not 1 <= index <= PORTION_COUNTS[options.stage - 1]:
        raise ValueError("stage-portion index is outside this stage's available portions.")
    if kind in SECTION_COUNTS:
        count = SECTION_COUNTS[kind][options.stage - 1]
        if not 0 <= index < count:
            raise ValueError(f"{options.section} section-index must be in 0..{count - 1} for stage {options.stage}.")


def _request_id(supplied: str | None) -> str:
    try:
        return str(uuid.UUID(supplied)) if supplied else str(uuid.uuid4())
    except (ValueError, AttributeError) as exc:
        raise ValueError("request-id must be a UUID.") from exc


def validate_request(options: SceneOptions) -> SceneRequest:
    if options.character not in CHARACTERS or options.shot_type not in SHOT_TYPES:
        raise ValueError("Choose character reimu or marisa, and shot type A, B, or C.")
    if not 0 <= options.difficulty <= 3:
        raise ValueError("difficulty must be in the range 0..3.")
    if not 1 <= options.stage <= 6:
        raise ValueError("stage must be in the range 1..6.")
    if options.section not in SECTION_KINDS:
        raise ValueError("section must be one of: " + ", ".join(SECTION_KINDS))
    normal_game = options.section == "normal-game"
    if normal_game:
        _validate_normal_game(options)
    else:
        _validate_practice_resources(options)
    if not MIN_TTL_MS <= options.timeout_ms <= MAX_TTL_MS:
        raise ValueError(f"timeout-ms must be in {MIN_TTL_MS}..{MAX_TTL_MS}.")
    if options.section_index < 0:
        raise ValueError("section-index must be zero or greater.")
    if options.section_dialogue not in DIALOGUE_POLICIES:
        raise ValueError("section-dialogue must be keep or skip.")
    if not 0 <= options.dialogue_advance_ms <= DIALOGUE_LEASE_MS:
        raise ValueError("dialogue-advance-ms must be 0 (disabled) or in 1..10000.")

    kind = SECTION_KINDS[options.section]
    index = options.section_index
    _validate_section_index(options, kind, index)

    return SceneRequest(
        request_id=_request_id(options.request_id),
        character=CHARACTERS[options.character],
        shot_type=SHOT_TYPES[options.shot_type],
        difficulty=options.difficulty,
        stage=options.stage,
        section_kind=kind,
        section_index=index,
        life=0 if normal_game else options.life,
        power=0 if normal_game else options.power,
        faith=0 if normal_game else options.faith,
        invincible=options.invincible,
        infinite_lives=options.infinite_lives,
        infinite_power=options.infinite_power,
        time_lock=options.time_lock,
        auto_bomb=options.auto_bomb,
        no_faith_loss=options.no_faith_loss,
        section_dialogue_policy=DIALOGUE_POLICIES[options.section_dialogue],
        dialogue_advance_ms=options.dialogue_advance_ms,
        timeout_ms=options.timeout_ms,
    )


def _normalize_reset_fields(fields: dict) -> None:
    for key in ("character", "shot_type", "section", "section_dialogue"):
        if not isinstance(fields[key], str):
            raise ValueError(f"{key} must be a string")
    fields["character"] = fields["character"].lower()
    fields["shot_type"] = fields["shot_type"].upper()
    for key in ("difficulty", "stage", "section_index", "life", "power", "faith"):
        value = fields[key]
        if type(value) is not int and not (key in ("life", "power", "faith") and value is None):
            raise ValueError(f"{key} must be an integer")


def request_from_options(options: dict) -> SceneRequest:
    settings = dict(options)
    mode = settings.pop("mode", "normal")
    if mode not in ("normal", "practice"):
        raise ValueError("mode must be normal or practice")
    practice = mode == "practice"
    fields = {
        "character": "reimu", "shot_type": "B", "difficulty": 1, "stage": 1,
        "section": "normal-game",
        "section_index": 0, "section_dialogue": "keep",
        "life": None, "power": None, "faith": None,
    }
    if practice:
        fields.update(section="stage-start", life=9, power=0, faith=50000)
    unknown = settings.keys() - fields.keys()
    if unknown:
        raise ValueError(f"Unknown reset option(s): {', '.join(sorted(unknown))}")
    fields.update(settings)
    if (fields["section"] == "normal-game") == practice:
        raise ValueError("Practice sections require mode='practice'")
    _normalize_reset_fields(fields)
    return validate_request(SceneOptions(**fields, timeout_ms=30_000))
