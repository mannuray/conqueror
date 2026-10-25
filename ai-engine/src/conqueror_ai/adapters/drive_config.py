"""config/drive.yaml (AI engine LLD §17.1): the driving limits and the cm/s -> % speed table."""

from __future__ import annotations

import itertools
from dataclasses import fields, replace
from pathlib import Path
from typing import Any, cast

import yaml

from conqueror_ai.adapters.drive_link import SPEED_TABLE
from conqueror_ai.domain.drive.limits import Limits


def load_drive(path: Path) -> tuple[Limits, tuple[tuple[float, float], ...]]:
    """The limits and the speed table. A missing file or key keeps the defaults; an unknown key is an
    error."""
    if not path.exists():
        return Limits(), SPEED_TABLE
    try:
        d: Any = yaml.safe_load(path.read_text()) or {}
    except yaml.YAMLError as e:
        raise ValueError(f"{path}: not valid YAML - {e}") from None
    if not isinstance(d, dict):
        raise ValueError(f"{path}: expected a mapping at the top")
    d = cast("dict[str, Any]", d)
    known = {f.name for f in fields(Limits)}
    given: dict[str, Any] = dict(d.get("limits") or {})
    unknown = sorted(set(given) - known)
    if unknown:
        raise ValueError(f"{path}: unknown limits {', '.join(unknown)}")
    lim = replace(Limits(), **{k: float(v) for k, v in given.items()})
    rows: list[Any] = list(d.get("speed_table") or [])
    table = tuple((float(r[0]), float(r[1])) for r in rows) if rows else SPEED_TABLE
    if any(b[0] <= a[0] or b[1] < a[1] for a, b in itertools.pairwise(table)):
        raise ValueError(f"{path}: the speed table must rise")
    return lim, table
