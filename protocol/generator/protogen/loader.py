"""Load protocol/defs/*.yaml into a ProtocolModel."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import yaml

from .model import ProtocolModel

DEFS_FILES: tuple[str, ...] = (
    "meta",
    "enums",
    "body_frames",
    "errors",
    "events",
    "settings",
    "golden",
)


def _read(path: Path) -> Any:
    with path.open(encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def load(defs_dir: Path) -> ProtocolModel:
    raw = {stem: _read(defs_dir / f"{stem}.yaml") for stem in DEFS_FILES}
    errors = raw["errors"]
    return ProtocolModel.model_validate(
        {
            "meta": raw["meta"],
            "enums": raw["enums"],
            "frames": raw["body_frames"],
            "errors": errors["domains"],
            "ack_status": errors["ack_status"],
            "events": raw["events"],
            "settings": raw["settings"],
            "golden": raw["golden"],
        }
    )
