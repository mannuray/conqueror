"""config/calibration.yaml (AI engine LLD §8): what the calibration tool measured. The engine refuses to map
without it, and refuses a file made for another picture size."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np
import yaml

from conqueror_ai.domain.camera import Calibration


def save(cal: Calibration, path: Path) -> None:
    doc: dict[str, Any] = {
        "date": cal.date,
        "size": list(cal.size),
        "lens": {"k": cal.k.tolist(), "dist": cal.dist.tolist()},
        "mount": {
            "cam_height_m": cal.cam_height_m,
            "cam_x_m": cal.cam_x_m,
            "cam_y_m": cal.cam_y_m,
            "track_front_m": cal.track_front_m,
            "tilt0_deg": cal.tilt0_deg,
        },
        "servos": {"pan_map": list(cal.pan_map), "tilt_map": list(cal.tilt_map)},
        "readout_us": cal.readout_us,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(yaml.safe_dump(doc, sort_keys=False))


def load(path: Path, size: tuple[int, int]) -> Calibration:
    """The calibration for pictures of `size`; ValueError when the file is for another size."""
    d: dict[str, Any] = yaml.safe_load(path.read_text())
    got = (int(d["size"][0]), int(d["size"][1]))
    if got != size:
        raise ValueError(f"{path} is for {got[0]} x {got[1]} pictures, not {size[0]} x {size[1]}")
    m, s = d["mount"], d["servos"]
    return Calibration(
        k=np.asarray(d["lens"]["k"], dtype=np.float64),
        dist=np.asarray(d["lens"]["dist"], dtype=np.float64),
        size=got,
        cam_height_m=float(m["cam_height_m"]),
        cam_x_m=float(m["cam_x_m"]),
        cam_y_m=float(m["cam_y_m"]),
        track_front_m=float(m["track_front_m"]),
        tilt0_deg=float(m["tilt0_deg"]),
        pan_map=(float(s["pan_map"][0]), float(s["pan_map"][1])),
        tilt_map=(float(s["tilt_map"][0]), float(s["tilt_map"][1])),
        readout_us=int(d.get("readout_us", 0)),
        date=str(d.get("date", "")),
    )
