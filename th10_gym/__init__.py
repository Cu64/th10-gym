"""Touhou 10 memory access and Gymnasium integration."""

ENV_ID = "TH10-v0"


def register_env() -> None:
    """Register the optional Gymnasium interface."""
    from gymnasium.envs.registration import register, registry

    if ENV_ID not in registry:
        register(id=ENV_ID, entry_point="th10_gym.env:TH10Env", nondeterministic=True)


# Keep memory-only use independent of Gymnasium.
try:
    register_env()
except ModuleNotFoundError as error:
    if error.name != "gymnasium":
        raise
