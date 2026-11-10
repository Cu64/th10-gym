# Set up DXVK for TH10 Gym

To run Touhou 10 with Vulkan, set `dxvk_dll` in `gym.make()`. DXVK requires a Windows driver with 32-bit Vulkan support. [DXVK driver requirements](https://github.com/doitsujin/dxvk/wiki/Driver-support#dxvk-3x)

## Set up the renderer

Download `dxvk-3.1.1.tar.gz` and extract it under `tools/`. [DXVK 3.1.1 downloads](https://github.com/doitsujin/dxvk/releases/tag/v3.1.1)

Add `dxvk_dll="tools/dxvk-3.1.1/x32/d3d9.dll"` to `gym.make()` in the [installation check](../README.md#verify-the-installation). To verify that DXVK is active, set `DXVK_HUD=version,devinfo,fps` before running the check. The game window shows the DXVK version, GPU, and frame rate.

The expected SHA-256 for `x32/d3d9.dll` is `265888c31ca78dffa290c39cb7e50bfb02762590e41927906e46fb32f01497fa`.

## Raise the VPatch frame-rate limit

To let Gym collect more steps per second, raise the frame-rate limit in `vpatch.ini` next to `th10.exe`. Add these settings under `[Option]`.

```ini
[Option]
GameFPS = 960
Vsync = 0
```

Close and recreate the environment after editing `vpatch.ini` so Gym starts a game with the new settings.

## Troubleshoot startup

If the game fails to start with DXVK, set `DXVK_LOG_PATH` to an existing directory and try again. Check `th10_d3d9.log` for errors. [DXVK on Windows](https://github.com/doitsujin/dxvk/wiki/Windows)

If reset reports `Scene start reached ticks ... before halt`, try the reset again. If the error repeats, lower `GameFPS` and recreate the environment.
