"""config/drive.yaml (AI engine LLD §17.1): the driving limits and the cm/s -> % speed table."""

from __future__ import annotations

import itertools
from dataclasses import fields, replace
from pathlib import Path
from typing import Any, cast

import yaml

from conqueror_ai.adapters.drive_link import SPEED_TABLE
from conqueror_ai.domain.drive.limits import Limits

# The measured limits (spec 8 §2) and slice 1's cap: the file may make driving more careful, never less
AT_MOST = ("v_cap_cms", "v_cruise_cms", "dv_cms_per_s", "dw_dps_per_s")
AT_LEAST = ("v_min_cms", "inner_min_cms", "r_floor_m", "spin_dps", "clear_m", "stop_range_m")


def load_drive(path: Path) -> tuple[Limits, tuple[tuple[float, float], ...]]:
    """The limits and the speed table. A missing file or key keeps the defaults; an unknown key, or a limit
    less careful than the measured one, is an error."""
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
    careless = sorted(
        [k for k in AT_MOST if getattr(lim, k) > getattr(Limits(), k)]
        + [k for k in AT_LEAST if getattr(lim, k) < getattr(Limits(), k)]
    )
    if careless:
        raise ValueError(f"{path}: the limits may only be made more careful - {', '.join(careless)}")
    rows: list[Any] = list(d.get("speed_table") or [])
    table = tuple((float(r[0]), float(r[1])) for r in rows) if rows else SPEED_TABLE
    if any(b[0] <= a[0] or b[1] < a[1] for a, b in itertools.pairwise(table)):
        raise ValueError(f"{path}: the speed table must rise")
    return lim, table
