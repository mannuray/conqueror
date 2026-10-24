"""The detector's config files (AI engine LLD §16.1): config/vocabulary.yaml (the words) and config/balls.yaml
(the colour balls). Plain YAML; the values are checked by the domain types."""

from __future__ import annotations

from dataclasses import asdict
from pathlib import Path
from typing import Any

import yaml

from conqueror_ai.domain.balls import BallColour
from conqueror_ai.domain.vocabulary import Vocabulary


def load_vocabulary(path: Path) -> Vocabulary:
    d: dict[str, Any] = yaml.safe_load(path.read_text()) or {}
    return Vocabulary.from_dict(d)


def load_balls(path: Path) -> list[BallColour]:
    if not path.exists():
        return []
    d: dict[str, Any] = yaml.safe_load(path.read_text()) or {}
    out: list[BallColour] = []
    rows: list[dict[str, Any]] = list(d.get("balls") or [])
    for r in rows:
        h: list[Any] = list(r["h"])
        out.append(
            BallColour(
                str(r["name"]),
                int(h[0]),
                int(h[1]),
                int(r["s_min"]),
                int(r["v_max"]),
                float(r.get("r_min_px", 6.0)),
            )
        )
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
