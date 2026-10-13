"""PlatformIO pre-script: git archive -> build/source.bin for the "source" partition (LLD R2.3/6).

source.bin = 64-byte header (magic CQSRC1, 40-hex commit, dirty flag, payload length, CRC-32)
             + gzip -9 of `git archive --format=tar HEAD` (honours .gitattributes export-ignore).
The build fails when the payload does not fit the partition, when any secrets.h is in the tar,
or when a password value from the local include/secrets.h appears anywhere in the tar.
Standalone: python3 scripts/pack_source.py [project_dir]
"""

from __future__ import annotations

import gzip
import io
import pathlib
import re
import struct
import subprocess
import sys
import tarfile
import zlib

PARTITION_SIZE = 0x1E0000
HEADER_SIZE = 64
SECRET_KEYS = ("CQ_WIFI_PASSWORD", "CQ_AP_PASSWORD", "CQ_OTA_PASSWORD")


def git(root: pathlib.Path, *args: str) -> bytes:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, check=True).stdout


def defines(path: pathlib.Path) -> dict[str, str]:
    if not path.exists():
        return {}
    text = path.read_text(encoding="utf-8")
    found: dict[str, str] = {}
    for key in SECRET_KEYS:
        m = re.search(rf'#define\s+{key}\s+"([^"]*)"', text)
        if m:
            found[key] = m.group(1)
    return found


def secret_values(project: pathlib.Path) -> dict[str, str]:
    """Real secret values: template placeholders (secrets.example.h, committed) are skipped."""
    real = defines(project / "include" / "secrets.h")
    template = defines(project / "include" / "secrets.example.h")
    return {k: v for k, v in real.items() if len(v) >= 4 and v != template.get(k)}


def check_tar(tar_bytes: bytes, secrets: dict[str, str]) -> list[str]:
    problems: list[str] = []
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as tar:
        for member in tar.getmembers():
            if member.name.endswith("secrets.h"):
                problems.append(f"{member.name} is in the archive")
    for key, value in secrets.items():
        if value.encode() in tar_bytes:
            problems.append(f"the value of {key} appears in the archive")
    return problems


def header(commit: str, dirty: bool, payload: bytes) -> bytes:
    head = struct.pack(
        "<8s40sB3xII4x",
        b"CQSRC1\0\0",
        commit.encode().ljust(40, b"0")[:40],
        1 if dirty else 0,
        len(payload),
        zlib.crc32(payload) & 0xFFFFFFFF,
    )
    assert len(head) == HEADER_SIZE
    return head


def build(project: pathlib.Path) -> int:
    root = pathlib.Path(git(project, "rev-parse", "--show-toplevel").decode().strip())
    commit = git(root, "rev-parse", "HEAD").decode().strip()
    dirty = bool(git(root, "status", "--porcelain", "--untracked-files=no").strip())
    tar_bytes = git(root, "archive", "--format=tar", "HEAD")
    problems = check_tar(tar_bytes, secret_values(project))
    if problems:
        for p in problems:
            print(f"pack_source: REFUSED - {p}")
        return 1
    payload = gzip.compress(tar_bytes, compresslevel=9, mtime=0)
    if len(payload) > PARTITION_SIZE - HEADER_SIZE:
        print(f"pack_source: {len(payload)} B does not fit the partition (check export-ignore)")
        return 1
    out = project / "build" / "source.bin"
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(header(commit, dirty, payload) + payload)
    note = " (DIRTY: packs HEAD, not your working tree)" if dirty else ""
    print(f"pack_source: {len(payload)} B, commit {commit[:8]}{note} -> build/source.bin")
    return 0


if __name__ == "__main__":
    raise SystemExit(build(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()))
else:  # PlatformIO extra script
    Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env
    if build(pathlib.Path(env.subst("$PROJECT_DIR"))) != 0:  # type: ignore[name-defined]  # noqa: F821
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
