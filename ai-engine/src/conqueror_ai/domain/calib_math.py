"""The calibration tool's maths (AI engine LLD §8, design-notes §123). Pure numpy: pictures in, numbers out.

- The laser level's horizontal line on a wall at exactly the lens height is the horizon: its row gives the
  camera's real tilt. Its vertical line straight ahead gives the real pan (measured with the camera level).
- The servo map: commanded angle -> real angle, a straight-line fit.
- The floor-mark check, and the tilt study (blind zone and view for a few tilts)."""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.types import Pose

MIN_LINE_SCORE = 20.0  # mean red excess on the line's row / column; below it there is no line


def _redness(rgb: NDArray[np.uint8]) -> NDArray[np.float64]:
    """How much red stands out: R - max(G, B), never below 0."""
    f = rgb.astype(np.float64)
    return np.clip(f[:, :, 0] - np.maximum(f[:, :, 1], f[:, :, 2]), 0.0, None)


def _peak(profile: NDArray[np.float64]) -> float | None:
    """The sub-pixel centre of the strongest peak (weighted over +-3), or None when nothing stands out."""
    background = float(np.median(profile))
    p = profile - background
    i = int(np.argmax(p))
    if p[i] < MIN_LINE_SCORE:
        return None
    lo, hi = max(0, i - 3), min(len(p), i + 4)
    w = np.clip(p[lo:hi], 0.0, None)
    return float((np.arange(lo, hi) * w).sum() / w.sum())


def laser_row(rgb: NDArray[np.uint8]) -> float | None:
    """The row of a horizontal red laser line (mean over the middle half of the columns)."""
    w = rgb.shape[1]
    return _peak(_redness(rgb)[:, w // 4 : 3 * w // 4].mean(axis=1))


def laser_col(rgb: NDArray[np.uint8]) -> float | None:
    """The column of a vertical red laser line (mean over the middle half of the rows)."""
    h = rgb.shape[0]
    return _peak(_redness(rgb)[h // 4 : 3 * h // 4, :].mean(axis=0))


def _normalised(cal: Calibration, u: float, v: float) -> tuple[float, float]:
    return CameraModel(cal).undistort((u - cal.k[0, 2]) / cal.k[0, 0], (v - cal.k[1, 2]) / cal.k[1, 1])


def horizon_tilt_deg(cal: Calibration, row: float) -> float:
    """The camera's real tilt (+ = up) from the row where a level line at lens height appears."""
    _, yn = _normalised(cal, float(cal.k[0, 2]), row)
    return math.degrees(math.atan(yn))


def pan_from_col_deg(cal: Calibration, col: float) -> float:
    """The real pan (+ = right) from the column of a vertical line straight ahead, the camera level."""
    xn, _ = _normalised(cal, col, float(cal.k[1, 2]))
    return -math.degrees(math.atan(xn))


def fit_linear(commanded: Sequence[float], real: Sequence[float]) -> tuple[float, float]:
    """Least squares: real = gain x commanded + offset."""
    gain, offset = np.polyfit(np.asarray(commanded, dtype=np.float64), np.asarray(real, dtype=np.float64), 1)
    return float(gain), float(offset)


def floor_mark_error_m(cam: CameraModel, u: float, v: float, from_tracks_m: float) -> float | None:
    """How far the model puts a tape mark (clicked at u, v) from where it is - `from_tracks_m` ahead of
    the tracks' front, the tank at the start pose. None when the click is above the horizon."""
    p = cam.pixel_to_floor(u, v)
    if p is None:
        return None
    return math.dist(p, (cam.cal.track_front_m + from_tracks_m, 0.0))


@dataclass(frozen=True)
class TiltRow:
    tilt_deg: float  # commanded, from the centre (+ = up)
    blind_m: float  # nearest floor point in view, from the tracks' front (straight ahead)
    far_m: float  # farthest floor point in view (inf: the horizon is in the picture)
    horizon_in_view: bool  # walls and the room's far side are in view


def tilt_study(cal: Calibration, tilts: Sequence[float]) -> list[TiltRow]:
    """For each tilt: what the camera sees straight ahead (the bottom and top rows, centre column)."""
    w, h = cal.size
    rows: list[TiltRow] = []
    for t in tilts:
        cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, t)
        near = cam.pixel_to_floor(w / 2, h - 1)
        top = cam.pixel_to_floor(w / 2, 0)
        blind = (near[0] - cal.track_front_m) if near is not None else math.inf
        far = (top[0] - cal.track_front_m) if top is not None else math.inf
        rows.append(TiltRow(t, blind, far, top is None))
    return rows
