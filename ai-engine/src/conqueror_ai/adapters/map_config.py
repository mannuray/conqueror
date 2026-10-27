"""config/map.yaml (AI engine LLD §18.9): heights, ranges and the depth settings."""

from __future__ import annotations

from dataclasses import fields
from pathlib import Path
from typing import Any, cast

import yaml

from conqueror_ai.domain.depth import DepthSettings, Heights, MapConfig, Ranges

# the owner's 20 cm (design-notes §159), and what would make the tank see less: the file may make them more
# careful, never less (final review I2)
AT_LEAST = ("clear_m",)
AT_MOST = ("floor_m", "trust_deg")


def _section(path: Path, d: dict[str, Any], name: str, cls: type[Any]) -> Any:
    given: Any = d.get(name) or {}
    if not isinstance(given, dict):
        raise ValueError(f"{path}: {name} must be a mapping")
    given = cast("dict[str, Any]", given)
    known = {f.name for f in fields(cls)}
    unknown = sorted(set(given) - known)
    if unknown:
        raise ValueError(f"{path}: unknown {name} {', '.join(unknown)}")
    types = {f.name: f.type for f in fields(cls)}
    out: dict[str, Any] = {}
    for k, v in given.items():
        if types[k] == "int":
            if not float(v).is_integer() or int(v) < 1:
                raise ValueError(f"{path}: {name}.{k} must be a whole number, 1 or more")
            out[k] = int(v)
        else:
            out[k] = float(v)
    return cls(**out)


def load_map_config(path: Path) -> MapConfig:
    """The settings; a missing file, section or key keeps the defaults; an unknown key, or a height less
    careful than the owner's, is an error."""
    if not path.exists():
        return MapConfig()
    try:
        d: Any = yaml.safe_load(path.read_text()) or {}
    except yaml.YAMLError as e:
        raise ValueError(f"{path}: not valid YAML - {e}") from None
    if not isinstance(d, dict):
        raise ValueError(f"{path}: expected a mapping at the top")
    d = cast("dict[str, Any]", d)
    unknown = sorted(set(d) - {"heights", "ranges", "depth"})
    if unknown:
        raise ValueError(f"{path}: unknown sections {', '.join(unknown)}")
    cfg = MapConfig(
        _section(path, d, "heights", Heights),
        _section(path, d, "ranges", Ranges),
        _section(path, d, "depth", DepthSettings),
    )
    h, dp = cfg.heights, cfg.depth
    careless = [k for k in AT_LEAST if getattr(h, k) < getattr(Heights(), k)]
    careless += [k for k in AT_MOST if k in ("floor_m",) and getattr(h, k) > getattr(Heights(), k)]
    careless += [k for k in AT_MOST if k in ("trust_deg",) and getattr(dp, k) > getattr(DepthSettings(), k)]
    if h.low_top_m > h.clear_m:
        careless.append("low_top_m (above clear_m)")
    if careless:
        raise ValueError(f"{path}: the heights may only be made more careful - {', '.join(careless)}")
    return cfg
