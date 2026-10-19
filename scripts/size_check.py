#!/usr/bin/env python3
"""Size guard (coding standard §3.1, bridge LLD R15.10): files <= 400 lines, functions <= 40 NLOC, CCN <= 10.

Checks firmware/esp32/src and firmware/uno/src (generated code excluded) with lizard. Exceptions live in
scripts/size_allow.txt as `path:function  # reason` or `path  # reason`. --baseline prints today's offenders
in that format (used once to seed the list).
"""

from __future__ import annotations

import csv
import io
import pathlib
import subprocess
import sys
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parents[1]
DIRS = ["firmware/esp32/src", "firmware/uno/src"]
MAX_FILE, MAX_NLOC, MAX_CCN = 400, 40, 10


@dataclass(frozen=True)
class Fn:
    file: str
    name: str
    nloc: int
    ccn: int


def parse_allow(text: str) -> set[str]:
    out: set[str] = set()
    for line in text.splitlines():
        entry = line.split("#", 1)[0].strip()
        if entry:
            out.add(entry)
    return out


def violations(fns: list[Fn], file_lines: dict[str, int], allow: set[str]) -> list[str]:
    out: list[str] = []
    for path, n in sorted(file_lines.items()):
        if n > MAX_FILE and path not in allow:
            out.append(f"{path}  {n} lines > {MAX_FILE}")
    for f in fns:
        key = f"{f.file}:{f.name}"
        if key in allow:
            continue
        if f.nloc > MAX_NLOC:
            out.append(f"{key}  nloc {f.nloc} > {MAX_NLOC}")
        if f.ccn > MAX_CCN:
            out.append(f"{key}  ccn {f.ccn} > {MAX_CCN}")
    return out


def sources() -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for d in DIRS:
        for p in sorted((ROOT / d).rglob("*")):
            if p.suffix in {".cpp", ".h"} and "generated" not in p.parts:
                out.append(p)
    return out


def measure(files: list[pathlib.Path]) -> tuple[list[Fn], dict[str, int]]:
    cmd = ["uvx", "lizard@1.24.1", "--csv", "-l", "cpp", *map(str, files)]
    text = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    fns: list[Fn] = []
    for row in csv.reader(io.StringIO(text)):  # NLOC,CCN,token,PARAM,length,location,file,function,long_name,start,end
        rel = str(pathlib.Path(row[6]).resolve().relative_to(ROOT))
        fns.append(Fn(file=rel, name=row[7], nloc=int(row[0]), ccn=int(row[1])))
    lines = {str(p.relative_to(ROOT)): len(p.read_text(encoding="utf-8").splitlines()) for p in files}
    return fns, lines


def main() -> int:
    fns, lines = measure(sources())
    if "--baseline" in sys.argv:
        for v in violations(fns, lines, set()):
            print(v.split("  ")[0] + "  # baseline 2026-10-19 (Plan P empties the bridge entries)")
        return 0
    allow = parse_allow((ROOT / "scripts" / "size_allow.txt").read_text(encoding="utf-8"))
    bad = violations(fns, lines, allow)
    for v in bad:
        print(f"size_check: {v}", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
