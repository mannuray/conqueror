"""Refuse to flash anything that is not the ESP32-S3 N8R8 board (bridge LLD R2.3 step 8).

Standalone:   python3 scripts/check_chip.py --port /dev/ttyACM0
Self-test:    python3 scripts/check_chip.py --self-test
PlatformIO:   loaded as post: script; runs before every USB upload (not before OTA).
Works with esptool v4 ("Chip is ...", flash_id) and v5 ("Chip type: ...", flash-id).
"""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import sys

EXPECTED_MAC = "48:ca:43:9e:74:b4"

CHIP_RE = re.compile(r"(?:Chip is|Chip type:)\s*(ESP32-S3)\b")
FLASH_RE = re.compile(r"Detected flash size:\s*8MB")
PSRAM_RE = re.compile(r"Embedded PSRAM 8MB")
MAC_RE = re.compile(r"MAC:\s*([0-9a-fA-F:]{17})")


def evaluate(text: str) -> tuple[list[str], str | None]:
    """Return (problems, mac). No problems = safe to flash."""
    problems: list[str] = []
    if not CHIP_RE.search(text):
        problems.append("chip is not an ESP32-S3")
    if not FLASH_RE.search(text):
        problems.append("flash size is not 8MB")
    if not PSRAM_RE.search(text):
        problems.append("no embedded 8MB PSRAM reported")
    m = MAC_RE.search(text)
    return problems, (m.group(1).lower() if m else None)


def esptool_cmd(python: str = sys.executable, package_dir: str | None = None) -> list[str]:
    if package_dir and pathlib.Path(package_dir, "esptool.py").exists():
        return [python, str(pathlib.Path(package_dir, "esptool.py"))]
    probe = subprocess.run([python, "-c", "import esptool"], capture_output=True, check=False)
    if probe.returncode == 0:
        return [python, "-m", "esptool"]
    if shutil.which("uvx"):
        return ["uvx", "--from", "esptool", "esptool"]
    raise SystemExit("check_chip: esptool not found (uv tool install esptool)")


def run_check(cmd: list[str], port: str) -> int:
    text = ""
    for verb in ("flash-id", "flash_id"):  # v5 name first, v4 fallback
        proc = subprocess.run(
            [*cmd, "--port", port, verb], capture_output=True, text=True, check=False
        )
        text = proc.stdout + proc.stderr
        if proc.returncode == 0:
            break
    problems, mac = evaluate(text)
    if problems:
        print(text)
        for p in problems:
            print(f"check_chip: REFUSED - {p}")
        return 1
    print(f"check_chip: OK - ESP32-S3, 8MB flash, 8MB PSRAM, MAC {mac}")
    if mac != EXPECTED_MAC:
        print(f"check_chip: WARNING - MAC {mac} is not the owner's board ({EXPECTED_MAC})")
    return 0


V4_SAMPLE = """Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
MAC: 48:ca:43:9e:74:b4
Detected flash size: 8MB"""
V5_SAMPLE = """Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
MAC:                48:ca:43:9e:74:b4
Detected flash size: 8MB"""
WROVER_SAMPLE = """Chip is ESP32-D0WD-V3 (revision v3.0)
Features: WiFi, BT, Dual Core, 240MHz
MAC: 08:3a:f2:00:00:01
Detected flash size: 4MB"""


def self_test() -> int:
    assert evaluate(V4_SAMPLE) == ([], EXPECTED_MAC)
    assert evaluate(V5_SAMPLE) == ([], EXPECTED_MAC)
    problems, _ = evaluate(WROVER_SAMPLE)
    assert len(problems) == 3, problems
    print("check_chip self-test: OK")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    if "--port" not in argv:
        print("usage: check_chip.py --port /dev/ttyACM0 | --self-test")
        return 2
    return run_check(esptool_cmd(), argv[argv.index("--port") + 1])


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
elif "Import" in globals():  # loaded by PlatformIO as an extra script (not when imported)
    Import("env")  # type: ignore[name-defined]  # noqa: F821

    def _before_upload(source, target, env):  # type: ignore[no-untyped-def]
        if env.subst("$UPLOAD_PROTOCOL") == "espota":
            return
        pkg = env.PioPlatform().get_package_dir("tool-esptoolpy")
        port = env.subst("$UPLOAD_PORT")
        if not port:
            env.AutodetectUploadPort()
            port = env.subst("$UPLOAD_PORT")
        if run_check(esptool_cmd(env.subst("$PYTHONEXE"), pkg), port) != 0:
            env.Exit(1)

    env.AddPreAction("upload", _before_upload)  # type: ignore[name-defined]  # noqa: F821
