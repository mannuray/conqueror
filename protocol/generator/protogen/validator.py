"""Validation rules for the protocol model. Returns human-readable issues."""

from __future__ import annotations

from .model import WIRE_RANGE, Frame, ProtocolModel
from .naming import pascal

_DIR_RANGE = {"esp32_to_uno": (0x01, 0x7F), "uno_to_esp32": (0x80, 0xFF)}


def _frame_issues(frame: Frame, max_payload: int, enum_names: set[str]) -> list[str]:
    issues: list[str] = []
    lo, hi = _DIR_RANGE[frame.dir]
    if not lo <= frame.type <= hi:
        issues.append(f"frame {frame.name}: type 0x{frame.type:02X} outside {frame.dir} range")
    if frame.max_payload() > max_payload:
        issues.append(f"frame {frame.name}: payload {frame.max_payload()} > {max_payload}")
    seen: set[str] = set()
    for f in frame.fields:
        if f.name in seen:
            issues.append(f"frame {frame.name}: duplicate field {f.name}")
        if f.enum is not None and f.enum not in enum_names:
            issues.append(f"frame {frame.name}.{f.name}: unknown enum {f.enum}")
        if f.items is not None:
            earlier = [g for g in frame.fields if g.name in seen]
            match = [g for g in earlier if g.name == f.count_field]
            if not match or match[0].type != "u8":
                issues.append(
                    f"frame {frame.name}.{f.name}: count_field {f.count_field} must be an "
                    "earlier u8 field"
                )
        seen.add(f.name)
    return issues


def validate(model: ProtocolModel) -> list[str]:
    issues: list[str] = []
    enum_names = {e.name for e in model.enums}

    names: set[str] = set()
    types: set[tuple[str, int]] = set()
    for frame in model.frames:
        if frame.name in names:
            issues.append(f"frame {frame.name}: duplicate name")
        if (frame.dir, frame.type) in types:
            issues.append(f"frame {frame.name}: duplicate type 0x{frame.type:02X}")
        names.add(frame.name)
        types.add((frame.dir, frame.type))
        issues += _frame_issues(frame, model.meta.max_payload, enum_names)

    codes: set[int] = set()
    reasons: set[str] = set()
    for domain in model.errors:
        lo, hi = domain.range
        for err in domain.codes:
            if not lo <= err.code <= hi:
                issues.append(f"error {err.reason}: code {err.code} outside {domain.prefix} range")
            if err.code in domain.retired:
                issues.append(f"error {err.reason}: code {err.code} is retired")
            if err.code in codes:
                issues.append(f"error {err.reason}: duplicate code {err.code}")
            if err.reason in reasons:
                issues.append(f"error {err.reason}: duplicate reason")
            codes.add(err.code)
            reasons.add(err.reason)

    if not model.ack_status or model.ack_status[0] != "OK":
        issues.append("ack_status: first entry must be OK")
    if len(model.ack_status) > 256:
        issues.append("ack_status: more than 256 entries")
    for reason in model.ack_status[1:]:
        if reason not in reasons:
            issues.append(f"ack_status: unknown reason {reason}")

    frame_classes = {pascal(f.name) for f in model.frames}
    for e in model.enums:
        if e.name in frame_classes:
            issues.append(f"enum {e.name}: name clash with a generated frame class")

    kinds: set[int] = set()
    for ev in model.events:
        if not 0 <= ev.kind <= 0xFF or ev.kind in kinds:
            issues.append(f"event {ev.name}: kind {ev.kind} invalid or duplicate")
        kinds.add(ev.kind)

    ids: set[int] = set()
    setting_names: set[str] = set()
    for s in model.settings:
        if not 0 <= s.id <= 0xFF or s.id in ids:
            issues.append(f"setting {s.name}: id {s.id} invalid or duplicate")
        if s.name in setting_names:
            issues.append(f"setting {s.name}: duplicate name")
        wlo, whi = WIRE_RANGE[s.type]
        if s.min < wlo or s.max > whi:
            issues.append(f"setting {s.name}: min/max outside {s.type} wire range")
        if not s.min <= s.default <= s.max:
            issues.append(f"setting {s.name}: default {s.default} outside [{s.min}, {s.max}]")
        ids.add(s.id)
        setting_names.add(s.name)

    for g in model.golden:
        if g.frame not in names:
            issues.append(f"golden: unknown frame {g.frame}")
    return issues
