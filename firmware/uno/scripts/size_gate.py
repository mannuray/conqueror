#!/usr/bin/env python3
"""Fail if the Uno firmware exceeds the RAM/flash budget (LLD §9.20)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

FLASH_LIMIT = 30 * 1024
RAM_LIMIT = 1750
ELF = Path(__file__).resolve().parents[1] / ".pio" / "build" / "uno" / "firmware.elf"
AVR_SIZE = Path.home() / ".platformio" / "packages" / "toolchain-atmelavr" / "bin" / "avr-size"


def main() -> int:
    out = subprocess.run(
        [str(AVR_SIZE), "-A", str(ELF)], check=True, capture_output=True, text=True
    ).stdout
    sections: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith(".") and parts[1].isdigit():
            sections[parts[0]] = int(parts[1])
    flash = sections.get(".text", 0) + sections.get(".data", 0)
    ram = sections.get(".data", 0) + sections.get(".bss", 0) + sections.get(".noinit", 0)
    print(f"size_gate: flash {flash} / {FLASH_LIMIT} B, static RAM {ram} / {RAM_LIMIT} B")
    if flash > FLASH_LIMIT or ram > RAM_LIMIT:
        print("size_gate: OVER BUDGET", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
