# TH10 Gym

TH10 Gym is a Gymnasium environment for *Touhou 10: Mountain of Faith*. It supports normal and practice play with configurable difficulty, character, and shot type.

> [!IMPORTANT]
> The environment returns zero reward by default. [Define a reward for your task](docs/training.md#rewards).

## Installation

### Prerequisites

- 64-bit Windows and 64-bit Python 3.11 or newer
- Git and PowerShell 7 (`pwsh`)
- Visual Studio 2022 C++ x86/x64 Build Tools (v143) and the Windows SDK

### Get Touhou 10

Download a copy of *Touhou 10: Mountain of Faith*, version 1.00a.

### Set up VPatch

If `vpatch_th10.dll` is already next to `th10.exe` and its hash matches the table below, use that copy.

To install VPatch, extract the download and copy `vpatch_th10.dll` from `vpatch_rev7` next to `th10.exe`. [VPatch download](https://maribelhearn.com/tools#vpatch) · [English VPatch guide](https://daichungus.codeberg.page/touhoulinuxguide/#install-vpatch)

| File | SHA-256 |
| --- | --- |
| `th10.exe` | `2f14760b6fbbf57549541583283badb9a19a4222b90f0a146d5aa17f01dc9040` |
| `vpatch_th10.dll` | `0aa5b79fdf5e8f57bcb505b4bf34b0875eb94e2f51269da6a370621e4c57431d` |

### Build the native components

```powershell
git clone https://github.com/Cu64/th10-gym.git
cd th10-gym
pwsh -NoProfile -File build_native.ps1 -WithSceneBridge
```

### Install the Python package

```powershell
python -m pip install .
```

### Verify the installation

Replace `path/to/th10.exe` with your game path and run the example from the repository root. On success, it prints the player's position.

```python
import gymnasium as gym
import th10_gym

with gym.make(
    th10_gym.ENV_ID,
    game_exe="path/to/th10.exe",
    native_dir="build/native",
    launcher="tools/thprac_scene_bridge_source/Release/thprac.exe",
) as env:
    env.reset()
    observation, _, _, _, _ = env.step(0)
    print(observation["player"]["position"])
```

## Documentation

- [Observation space](docs/observations.md)
- [Action space](docs/actions.md)
- [Using TH10 Gym for RL](docs/training.md)
- [Optional DXVK setup](docs/dxvk.md)

## Acknowledgements

- TH10 Gym uses a patched version of THPrac to start normal runs and practice sessions. [THPrac](https://github.com/touhouworldcup/thprac)
- Twinject was a reference for reading Touhou 10 game state. [Twinject](https://github.com/Netdex/twinject)

## License

[TH10 Gym license](LICENSE.txt) · [THPrac license](THPRAC-LICENSE.txt)
