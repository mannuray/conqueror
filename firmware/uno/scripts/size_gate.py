#!/usr/bin/env python3
"""Fail if the Uno firmware exceeds the RAM/flash budget (LLD §9.20)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

# (flash, static RAM) budgets per PlatformIO env (LLD §9.20, §11.2).
LIMITS = {"mega": (240 * 1024, 6 * 1024), "uno": (28 * 1024, 1536)}
AVR_SIZE = Path.home() / ".platformio" / "packages" / "toolchain-atmelavr" / "bin" / "avr-size"


def main() -> int:
    env = sys.argv[1] if len(sys.argv) > 1 else "mega"
    flash_limit, ram_limit = LIMITS[env]
    elf = Path(__file__).resolve().parents[1] / ".pio" / "build" / env / "firmware.elf"
    out = subprocess.run(
        [str(AVR_SIZE), "-A", str(elf)], check=True, capture_output=True, text=True
    ).stdout
    sections: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith(".") and parts[1].isdigit():
            sections[parts[0]] = int(parts[1])
    flash = sections.get(".text", 0) + sections.get(".data", 0)
    ram = sections.get(".data", 0) + sections.get(".bss", 0) + sections.get(".noinit", 0)
    print(f"size_gate[{env}]: flash {flash} / {flash_limit} B, static RAM {ram} / {ram_limit} B")
    if flash > flash_limit or ram > ram_limit:
        print("size_gate: OVER BUDGET", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
