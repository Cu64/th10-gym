"""Run the native installer and validate its report."""

import json
import subprocess
from pathlib import Path

# Leave time for the installer's 35-second deadline and rollback.
INSTALLER_WATCHDOG_SECONDS = 45


def install_hook(installer: Path, pid: int, hook: Path) -> None:
    command = [str(installer), "--pid", str(pid), "--dll", str(hook), "--install"]
    try:
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                                errors="replace", timeout=INSTALLER_WATCHDOG_SECONDS)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"{installer.name} exceeded its {INSTALLER_WATCHDOG_SECONDS}-second watchdog") from error
    _verify_installation(installer.name, result)


def _last_report(output: str) -> dict | None:
    if not output:
        return None
    try:
        report = json.loads(output.splitlines()[-1])
    except json.JSONDecodeError:
        return None
    return report if isinstance(report, dict) else None


def _verify_installation(installer_name: str, result: subprocess.CompletedProcess) -> None:
    output = result.stdout.strip()
    report = _last_report(output)
    if result.returncode:
        detail = (report or {}).get("error") or output[-1600:]
        raise RuntimeError(f"{installer_name} exited {result.returncode}: "
                           f"{result.stderr.strip()} {detail}".rstrip())
    if report is None:
        raise RuntimeError(f"{installer_name} did not return an installation report")
    required = ("target_verified", "hook_loaded", "initialized", "patch_applied", "installed")
    if not all(report.get(key) is True for key in required):
        raise RuntimeError(f"Hook installation failed verification: {report}")
