"""config/calibration.yaml (AI engine LLD §8): what the calibration tool measured. The engine refuses to map
without it, and refuses a file made for another picture size."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np
import yaml
from numpy.typing import NDArray

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


def _scaled_k(k: NDArray[np.float64], scale: float) -> NDArray[np.float64]:
    """fx, fy, cx, cy times the scale (pixel centres: cx' = (cx + 0.5) x s - 0.5)."""
    if scale == 1.0:
        return k
    out = k.copy()
    out[0, 0] *= scale
    out[1, 1] *= scale
    out[0, 2] = (k[0, 2] + 0.5) * scale - 0.5
    out[1, 2] = (k[1, 2] + 0.5) * scale - 0.5
    return out


def load(path: Path, size: tuple[int, int]) -> Calibration:
    """The calibration for pictures of `size`; ValueError when the file is for another size."""
    d: dict[str, Any] = yaml.safe_load(path.read_text())
    got = (int(d["size"][0]), int(d["size"][1]))
    scale = 1.0
    if got != size:
        if size != (got[0] * 2, got[1] * 2):
            raise ValueError(f"{path} is for {got[0]} x {got[1]} pictures, not {size[0]} x {size[1]}")
        scale = 2.0  # VGA from the QVGA file: the same field of view, twice the pixels (LLD §16.1 item 6)
    m, s = d["mount"], d["servos"]
    return Calibration(
        k=_scaled_k(np.asarray(d["lens"]["k"], dtype=np.float64), scale),
        dist=np.asarray(d["lens"]["dist"], dtype=np.float64),
        size=size,
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
