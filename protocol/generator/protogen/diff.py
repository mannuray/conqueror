"""Breaking-change detection between two versions of protocol/defs."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from .loader import DEFS_FILES, load
from .model import Field, Frame, ProtocolModel


def _shape(f: Field) -> tuple[object, ...]:
    # Attributes that change wire meaning. `range`, `unit` and `doc` are excluded: they are
    # validation/documentation only and never alter the encoded bytes.
    items = tuple((i.name, i.type) for i in f.items) if f.items else None
    sentinel = tuple(sorted(f.sentinel.items())) if f.sentinel else None
    return (f.name, f.type, f.count, items, f.count_field, f.max_count, f.scale, f.enum, sentinel)


def _describe(f: Field) -> str:
    return ",".join(str(x) for x in _shape(f) if x is not None)


def _frame_changes(of: Frame, nf: Frame) -> list[str]:
    out: list[str] = []
    if nf.type != of.type:
        out.append(f"frame {of.name}: type id changed {of.type} -> {nf.type}")
    if nf.dir != of.dir:
        out.append(f"frame {of.name}: direction changed {of.dir} -> {nf.dir}")
    if nf.ack != of.ack:
        out.append(f"frame {of.name}: ack flag changed {of.ack} -> {nf.ack}")
    old_fields = [_shape(f) for f in of.fields]
    new_fields = [_shape(f) for f in nf.fields]
    if old_fields != new_fields:
        old_d = "[" + "; ".join(_describe(f) for f in of.fields) + "]"
        new_d = "[" + "; ".join(_describe(f) for f in nf.fields) + "]"
        out.append(f"frame {of.name}: fields changed {old_d} -> {new_d}")
    return out


def _error_changes(old: ProtocolModel, new: ProtocolModel) -> list[str]:
    out: list[str] = []
    new_domains = {d.prefix: d for d in new.errors}
    new_codes = {c.code: c for d in new.errors for c in d.codes}
    new_retired = {r for d in new.errors for r in d.retired}
    for od in old.errors:
        nd = new_domains.get(od.prefix)
        nd_codes = {c.code: c for c in nd.codes} if nd else {}
        for oc in od.codes:
            nc = nd_codes.get(oc.code)
            if nc is None:
                out.append(f"error {od.prefix} code {oc.code} ({oc.reason}) removed")
            elif nc.reason != oc.reason:
                out.append(
                    f"error {od.prefix} code {oc.code}: reason changed {oc.reason} -> {nc.reason}"
                )
        for r in od.retired:
            if r not in new_retired:
                out.append(f"error {od.prefix}: retired code {r} removed from retired list")
    old_retired = {r for d in old.errors for r in d.retired}
    for code in sorted(old_retired & new_codes.keys()):
        out.append(f"error code {code} ({new_codes[code].reason}) reuses a retired code")
    return out


def _enum_changes(old: ProtocolModel, new: ProtocolModel) -> list[str]:
    out: list[str] = []
    new_enums = {e.name: e for e in new.enums}
    for oe in old.enums:
        ne = new_enums.get(oe.name)
        if ne is None:
            out.append(f"enum {oe.name} removed")
            continue
        for name, value in oe.values.items():
            if name not in ne.values:
                out.append(f"enum {oe.name}: value {name}={value} removed")
            elif ne.values[name] != value:
                out.append(f"enum {oe.name}: {name} changed {value} -> {ne.values[name]}")
    return out


def _setting_changes(old: ProtocolModel, new: ProtocolModel) -> list[str]:
    out: list[str] = []
    new_settings = {s.name: s for s in new.settings}
    for os_ in old.settings:
        ns = new_settings.get(os_.name)
        if ns is None:
            out.append(f"setting {os_.name} removed")
            continue
        for attr in ("id", "type", "scale"):
            ov, nv = getattr(os_, attr), getattr(ns, attr)
            if ov != nv:
                out.append(f"setting {os_.name}: {attr} changed {ov} -> {nv}")
    return out


def breaking_changes(old: ProtocolModel, new: ProtocolModel) -> list[str]:
    changes: list[str] = []
    new_frames = {f.name: f for f in new.frames}
    for of in old.frames:
        nf = new_frames.get(of.name)
        if nf is None:
            changes.append(f"frame {of.name} removed")
            continue
        changes.extend(_frame_changes(of, nf))
    old_status = old.ack_status
    if new.ack_status[: len(old_status)] != old_status:
        changes.append(
            f"ack_status reordered or entries removed (append only): "
            f"{old_status} -> {new.ack_status}"
        )
    changes.extend(_error_changes(old, new))
    changes.extend(_enum_changes(old, new))
    changes.extend(_setting_changes(old, new))
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
    try:
        with tempfile.TemporaryDirectory() as tmp:
            old = _load_ref(root, ref, Path(tmp))
    except subprocess.CalledProcessError:
        print(f"protogen: cannot read defs at {ref}", file=sys.stderr)
        return 2
    new = load(root / "protocol" / "defs")
    changes = breaking_changes(old, new)
    for change in changes:
        print(f"protogen: breaking: {change}", file=sys.stderr)
    if changes and new.meta.protocol_version == old.meta.protocol_version:
        print("protogen: bump protocol_version in protocol/defs/meta.yaml", file=sys.stderr)
        return 1
    return 0
