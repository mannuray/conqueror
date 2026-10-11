"""Breaking-change detection between two versions of protocol/defs."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from .loader import DEFS_FILES, load
from .model import Field, ProtocolModel


def _shape(f: Field) -> tuple[object, ...]:
    items = tuple((i.name, i.type) for i in f.items) if f.items else None
    return (f.name, f.type, f.count, items, f.count_field, f.max_count)


def breaking_changes(old: ProtocolModel, new: ProtocolModel) -> list[str]:
    changes: list[str] = []
    new_frames = {f.name: f for f in new.frames}
    for of in old.frames:
        nf = new_frames.get(of.name)
        if nf is None:
            changes.append(f"frame {of.name} removed")
            continue
        if (nf.type, nf.dir) != (of.type, of.dir):
            changes.append(f"frame {of.name}: type/direction changed")
        if [_shape(f) for f in nf.fields] != [_shape(f) for f in of.fields]:
            changes.append(f"frame {of.name}: fields changed")
    old_status = old.ack_status
    if new.ack_status[: len(old_status)] != old_status:
        changes.append("ack_status reordered or entries removed (append only)")
    return changes


def _load_ref(root: Path, ref: str, tmp: Path) -> ProtocolModel:
    for stem in DEFS_FILES:
        rel = f"protocol/defs/{stem}.yaml"
        text = subprocess.run(
            ["git", "-C", str(root), "show", f"{ref}:{rel}"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        (tmp / f"{stem}.yaml").write_text(text, encoding="utf-8")
    return load(tmp)


def cmd_diff(root: Path, ref: str) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        old = _load_ref(root, ref, Path(tmp))
    new = load(root / "protocol" / "defs")
    changes = breaking_changes(old, new)
    for change in changes:
        print(f"protogen: breaking: {change}", file=sys.stderr)
    if changes and new.meta.protocol_version == old.meta.protocol_version:
        print("protogen: bump protocol_version in protocol/defs/meta.yaml", file=sys.stderr)
        return 1
    return 0
