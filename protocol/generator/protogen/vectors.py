"""Deterministic test vectors: typical + extremes per frame, plus hand-written goldens."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .encoding import build_frame
from .model import WIRE_RANGE, Frame, ProtocolModel


@dataclass(frozen=True)
class Vector:
    frame: str
    variant: str
    seq: int
    values: dict[str, Any]
    data: bytes


def _scalar(wire: str, idx: int, variant: str) -> int:
    if variant == "typical":
        base = ((idx + 1) * 3) % 100  # < 100: fits every wire type, incl. u8/i8 array elements
        return -base if wire.startswith("i") and idx % 2 else base
    lo, hi = WIRE_RANGE[wire]
    return hi if idx % 2 == 0 else lo


def make_values(frame: Frame, variant: str) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for idx, f in enumerate(frame.fields):
        if f.items is not None:
            assert f.max_count is not None and f.count_field is not None
            n = min(2, f.max_count) if variant == "typical" else f.max_count
            values[f.name] = [
                [_scalar(it.type, idx * 10 + k + j, variant) for j, it in enumerate(f.items)]
                for k in range(n)
            ]
            values[f.count_field] = n
        elif f.count is not None:
            assert f.type is not None
            values[f.name] = [_scalar(f.type, idx * 10 + k, variant) for k in range(f.count)]
        else:
            assert f.type is not None
            values[f.name] = _scalar(f.type, idx, variant)
    return values


def make_vectors(model: ProtocolModel) -> list[Vector]:
    vectors: list[Vector] = []
    for i, frame in enumerate(model.frames):
        for j, variant in enumerate(("typical", "extremes")):
            seq = (2 * i + j) & 0xFF
            values = make_values(frame, variant)
            data = build_frame(model.meta, frame, seq, values)
            vectors.append(Vector(frame.name, variant, seq, values, data))
    for g in model.golden:
        data = build_frame(model.meta, model.frame(g.frame), g.seq, g.values)
        vectors.append(Vector(g.frame, "golden", g.seq, dict(g.values), data))
    return vectors
