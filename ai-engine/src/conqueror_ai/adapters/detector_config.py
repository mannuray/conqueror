"""The detector's config files (AI engine LLD §16.1): config/vocabulary.yaml (the words) and config/balls.yaml
(the colour balls). Plain YAML; a bad file or value is a ValueError naming the file and the row."""

from __future__ import annotations

from dataclasses import asdict
from pathlib import Path
from typing import Any, cast

import yaml

from conqueror_ai.domain.balls import BallColour
from conqueror_ai.domain.vocabulary import Vocabulary


def _read(path: Path) -> dict[str, Any]:
    try:
        d: Any = yaml.safe_load(path.read_text())
    except yaml.YAMLError as e:
        raise ValueError(f"{path}: not valid YAML - {e}") from None
    if d is None:
        return {}
    if not isinstance(d, dict):
        raise ValueError(f"{path}: expected a mapping at the top")
    return cast("dict[str, Any]", d)


def load_vocabulary(path: Path) -> Vocabulary:
    d = _read(path)
    try:
        return Vocabulary.from_dict(d)
    except ValueError as e:
        raise ValueError(f"{path}: {e}") from None


def _ball(r: dict[str, Any]) -> BallColour:
    try:
        h: list[Any] = list(r["h"])
        return BallColour(
            str(r["name"]),
            int(h[0]),
            int(h[1]),
            int(r["s_min"]),
            int(r["v_max"]),
            float(r.get("r_min_px", 6.0)),
        )
    except KeyError as e:
        raise ValueError(f"it has no {e.args[0]}") from None
    except (TypeError, IndexError) as e:
        raise ValueError(f"a value has the wrong form ({e})") from None


def load_balls(path: Path) -> list[BallColour]:
    if not path.exists():
        return []
    rows: list[Any] = list(_read(path).get("balls") or [])
    out: list[BallColour] = []
    for i, r in enumerate(rows, 1):
        row = cast("dict[str, Any]", r) if isinstance(r, dict) else None
        name = str(row.get("name", f"number {i}")) if row is not None else f"number {i}"
        try:
            if row is None:
                raise ValueError("it is not a mapping")
            out.append(_ball(row))
        except ValueError as e:
            raise ValueError(f"{path}: ball {name}: {e}") from None
    return out


def save_ball(path: Path, ball: BallColour) -> None:
    """Adds the ball, or replaces the one of the same name; the others are kept."""
    balls = [b for b in load_balls(path) if b.name != ball.name] + [ball]
    rows: list[dict[str, Any]] = []
    for b in balls:
        r = asdict(b)
        rows.append(
            {
                "name": r["name"],
                "h": [r["h_lo"], r["h_hi"]],
                "s_min": r["s_min"],
                "v_max": r["v_max"],
                "r_min_px": r["r_min_px"],
            }
        )
    head = (
        "# Colour balls for the ball finder (AI engine LLD §16.1 item 3);"
        " written by cq-calibrate ball <name>.\n"
    )
    path.write_text(head + yaml.safe_dump({"balls": rows}, sort_keys=False))
