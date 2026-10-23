"""The calibration tool's maths (AI engine LLD §8, design-notes §123). Pure numpy: pictures in, numbers out.

- The laser level's horizontal line on a wall at exactly the lens height is the horizon: its row gives the
  camera's real tilt. Its vertical line straight ahead gives the real pan (measured with the camera level).
- A line of any colour: red, green, or burnt out to near-white (design-notes §130).
- The servo map: commanded angle -> real angle, a straight-line fit. For the gains only, the line need not be
  exactly at lens height or straight ahead: only the change between pictures counts (§130).
- The floor-mark check, and the tilt study (blind zone and view for a few tilts)."""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.types import Pose

MIN_LINE_SCORE = 20.0  # mean excess (red, green or brightness) on the line's row / column; below it: no line
MAX_LINE_WIDTH_PX = 20  # wider at a quarter of its height: a red object, not a laser line (final review I5;
#                         a burnt-out line with its halo is ~15 px at QVGA)
SECOND_PEAK_RATIO = 0.5  # another peak this strong elsewhere: ambiguous, no answer
BASELINE_PX = (
    2 * MAX_LINE_WIDTH_PX + 1
)  # the local background: anything wider (the floor, a wall) is not a line
AGREE_PX = (
    3.0  # two measures that find lines further apart than this: ambiguous, no answer (Plan T review I3)
)
SERVO_POINTS = 5
INSIDE = 0.1  # servo points keep the line within 10..90 % of the picture (final review I2)


def _redness(rgb: NDArray[np.uint8]) -> NDArray[np.float64]:
    """How much red stands out: R - max(G, B), never below 0."""
    f = rgb.astype(np.float64)
    return np.clip(f[:, :, 0] - np.maximum(f[:, :, 1], f[:, :, 2]), 0.0, None)


def _greenness(rgb: NDArray[np.uint8]) -> NDArray[np.float64]:
    """How much green stands out: G - max(R, B), never below 0."""
    f = rgb.astype(np.float64)
    return np.clip(f[:, :, 1] - np.maximum(f[:, :, 0], f[:, :, 2]), 0.0, None)


def _brightness(rgb: NDArray[np.uint8]) -> NDArray[np.float64]:
    """The mean of the three colours: a line burnt out to near-white (§130: the owner's green laser)."""
    return rgb.astype(np.float64).mean(axis=2)


# Each measure with its smoothing: the colours are smoothed over 5 px, so a burnt-out line (colour only at its
# fringes) is still one peak; brightness is not, so a 1-px burnt-out core keeps its height (Plan T review I2).
_MEASURES = ((_redness, 5), (_greenness, 5), (_brightness, 1))


def _best_line(rgb: NDArray[np.uint8], axis: int) -> float | None:
    """The line in whichever measure shows it strongest; each measure must pass the peak rules on its own,
    and measures that find lines in different places give no answer - never a wrong row (Plan T review I3).
    axis 1: a row (mean over the middle half of the columns); axis 0: a column (the middle half of the
    rows)."""
    h, w = rgb.shape[:2]
    found: list[tuple[float, float]] = []
    for measure, smooth in _MEASURES:
        m = measure(rgb)
        profile = (
            m[:, w // 4 : 3 * w // 4].mean(axis=1) if axis == 1 else m[h // 4 : 3 * h // 4, :].mean(axis=0)
        )
        r = _scored_peak(profile, smooth)
        if r is not None:
            found.append(r)
    if not found:
        return None
    centres = [c for c, _ in found]
    if max(centres) - min(centres) > AGREE_PX:
        return None
    return max(found, key=lambda r: r[1])[0]


def _opening(p: NDArray[np.float64], width: int) -> NDArray[np.float64]:
    """Grey opening (a sliding min, then a sliding max): the profile with every peak narrower than `width`
    cut away - the local background under a line, following steps such as the floor's edge (review I1)."""
    half = width // 2
    lo = np.lib.stride_tricks.sliding_window_view(np.pad(p, half, mode="edge"), width).min(axis=1)
    return np.lib.stride_tricks.sliding_window_view(np.pad(lo, half, mode="edge"), width).max(axis=1)


def _scored_peak(profile: NDArray[np.float64], smooth: int = 5) -> tuple[float, float] | None:
    """The sub-pixel centre of the one narrow peak and its height above the local background, or None: nothing
    stands out, the peak is wide (a red object), or a second peak nearly as strong is elsewhere."""
    s = np.asarray(
        np.convolve(profile, np.ones(smooth) / smooth, mode="same") if smooth > 1 else profile,
        dtype=np.float64,
    )
    p = s - _opening(s, BASELINE_PX)
    i = int(np.argmax(p))
    top = float(p[i])
    if top < MIN_LINE_SCORE:
        return None
    lo = i  # the line's extent above a quarter of its height: a burnt-out core dips between two red humps
    while lo > 0 and p[lo - 1] >= top / 4:
        lo -= 1
    hi = i
    while hi < len(p) - 1 and p[hi + 1] >= top / 4:
        hi += 1
    if hi - lo + 1 > MAX_LINE_WIDTH_PX:
        return None
    rest = np.concatenate([p[: max(0, lo - 3)], p[hi + 4 :]])
    if len(rest) and float(rest.max()) >= SECOND_PEAK_RATIO * top:
        return None
    a, b = max(0, lo - 2), min(len(p), hi + 3)
    w = np.clip(p[a:b], 0.0, None)
    return float((np.arange(a, b) * w).sum() / w.sum()), top


def laser_row(rgb: NDArray[np.uint8]) -> float | None:
    """The row of a horizontal laser line of any colour (mean over the middle half of the columns)."""
    return _best_line(rgb, axis=1)


def laser_col(rgb: NDArray[np.uint8]) -> float | None:
    """The column of a vertical laser line of any colour (mean over the middle half of the rows)."""
    return _best_line(rgb, axis=0)


def _normalised(cal: Calibration, u: float, v: float) -> tuple[float, float]:
    n = CameraModel(cal).undistort((u - cal.k[0, 2]) / cal.k[0, 0], (v - cal.k[1, 2]) / cal.k[1, 1])
    if n is None:
        raise ValueError(f"pixel ({u:.0f}, {v:.0f}) is outside the lens's valid view")
    return n


def tilt_points(cal: Calibration, line_deg: float | None = None) -> list[float]:
    """Commanded tilts for the servo step that keep the horizontal line within 10..90 % of the picture's
    height (assuming about 1 deg per commanded deg) and inside the servo's -20..60 (final review I2).
    `line_deg`: where the line looks at command 0 (`horizon_tilt_deg` of its row, §130); None: a line at
    lens height (tilt0)."""
    h = cal.size[1]
    fy, cy = float(cal.k[1, 1]), float(cal.k[1, 2])
    b = cal.tilt0_deg if line_deg is None else line_deg
    lo = math.degrees(math.atan((INSIDE * h - cy) / fy)) - b
    hi = math.degrees(math.atan(((1 - INSIDE) * h - cy) / fy)) - b
    lo, hi = max(lo, -20.0), min(hi, 60.0)
    return [round(float(t), 1) for t in np.linspace(lo, hi, SERVO_POINTS)]


def pan_points(cal: Calibration, line_deg: float = 0.0) -> list[float]:
    """Commanded pans keeping the vertical line within 10..90 % of the picture's width and inside -90..90.
    `line_deg`: where the line looks at command 0 (`pan_from_col_deg` of its column, §130); 0 = straight
    ahead, and then the points are symmetric with 0 among them."""
    w = cal.size[0]
    fx, cx = float(cal.k[0, 0]), float(cal.k[0, 2])
    right = math.degrees(math.atan((cx - INSIDE * w) / fx))
    left = math.degrees(math.atan(((1 - INSIDE) * w - cx) / fx))
    m = min(right, left)
    lo, hi = max(-m - line_deg, -90.0), min(m - line_deg, 90.0)
    return [round(float(p), 1) for p in np.linspace(lo, hi, SERVO_POINTS)]


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
    blind_edge_m: float  # the same at the picture's bottom corner (barrel lenses see nearer there)
    far_m: float  # farthest floor point in view (inf: the horizon is in the picture)
    horizon_in_view: bool  # walls and the room's far side are in view


def tilt_study(cal: Calibration, tilts: Sequence[float]) -> list[TiltRow]:
    """For each tilt: what the camera sees straight ahead (the bottom and top rows, centre column)."""
    w, h = cal.size
    rows: list[TiltRow] = []
    for t in tilts:
        cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, t)
        near = cam.pixel_to_floor(w / 2, h - 1)
        edge = cam.pixel_to_floor(0, h - 1)
        top = cam.pixel_to_floor(w / 2, 0)
        blind = (near[0] - cal.track_front_m) if near is not None else math.inf
        blind_edge = (edge[0] - cal.track_front_m) if edge is not None else math.inf
        far = (top[0] - cal.track_front_m) if top is not None else math.inf
        rows.append(TiltRow(t, blind, blind_edge, far, top is None))
    return rows
