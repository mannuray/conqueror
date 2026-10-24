"""The colour ball finder (AI engine LLD §16.1 items 3 and 5): round blobs of known colours on the floor.

Plain OpenCV on arrays, no files: the largest round blob of each ball's colour below the horizon that is
bright enough and the right size for its distance. Measured on the tank (design-notes §152): the green ball
in 187 of 194 pictures, steady to +-1.6 px, 1.1 ms at VGA; the size and brightness checks (Plan Y Task 5)
keep 236 of 243 sightings and drop all 48 reflections on a steel bin."""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Detection

MIN_ROUND = 0.66  # convex hull area / (pi r^2) of the enclosing circle. The tank's green ball measured 0.73
#   (p10 0.70; its shaded side and pattern edge are not green), a square 0.64, a 2:3 box 0.59 (§152 / Plan Y)
SCORE_K = 1.3  # score = min(1, roundness x this): the ball ~0.95
HORIZON_AHEAD_M = 1000.0  # a floor point this far ahead lies on the horizon
MAX_HUE_SPREAD = 60  # a calibrated ball wider than this (of 180) is not one colour
SIZE_RANGE = (0.6, 1.5)  # a blob's size from its floor distance, as a share of the ball's calibrated size
V_MEAN_SHARE = (
    0.8  # calibration: the brightness limit is this share of the ball's mean value (bin 90-117, ball 162)
)


@dataclass(frozen=True)
class BallColour:
    name: str
    h_lo: int  # OpenCV hue, 0-179
    h_hi: int
    s_min: int
    v_max: int
    r_min_px: float = 6.0
    size_cm: float | None = None  # as the tank measures it (2 r x floor distance / fx); None: no size check
    v_mean_min: int = 0  # the blob's mean value (brightness) must be at least this

    def __post_init__(self) -> None:
        """h_lo > h_hi is a range that wraps through 0 (red)."""
        if not (0 <= self.h_lo <= 179 and 0 <= self.h_hi <= 179):
            raise ValueError(f"ball {self.name}: hue {self.h_lo}-{self.h_hi} is outside 0-179")
        if not 0 <= self.s_min <= 255:
            raise ValueError(f"ball {self.name}: saturation {self.s_min} is outside 0-255")
        if not 0 <= self.v_max <= 255:
            raise ValueError(f"ball {self.name}: value {self.v_max} is outside 0-255")
        if not self.r_min_px > 0:
            raise ValueError(f"ball {self.name}: r_min_px {self.r_min_px} must be above 0")
        if self.size_cm is not None and not self.size_cm > 0:
            raise ValueError(f"ball {self.name}: size_cm {self.size_cm} must be above 0")
        if not 0 <= self.v_mean_min <= 255:
            raise ValueError(f"ball {self.name}: v_mean_min {self.v_mean_min} is outside 0-255")


def horizon_row(cam: CameraModel) -> int | None:
    """The picture row of the horizon straight ahead of the camera: the floor is below it. None when it is
    above the picture (tilted far down: all of it is floor); the picture height when it is below the picture
    (tilted up: none of it is floor)."""
    c = cam.position()
    fwd = cam.axes()[2]  # right, down, forward
    n = math.hypot(float(fwd[0]), float(fwd[1]))
    if n < 1e-6:
        return None  # looking straight down
    x = float(c[0]) + float(fwd[0]) / n * HORIZON_AHEAD_M
    y = float(c[1]) + float(fwd[1]) / n * HORIZON_AHEAD_M
    q = cam.floor_to_pixel(x, y)
    if q is None:  # outside the lens's view: above the picture (all floor) or below it (no floor)
        down = cam.axes()[1]
        below = float((np.array([x, y, 0.0]) - c) @ down) > 0
        return cam.cal.size[1] if below else None
    row = round(q[1])
    if row < 0:
        return None
    return min(row, cam.cal.size[1])


U8 = NDArray[np.uint8]


def _hsv(rgb: U8) -> U8:
    return cast("U8", cv2.cvtColor(cv2.GaussianBlur(rgb, (5, 5), 0), cv2.COLOR_RGB2HSV))


def _in_range(
    hsv: U8, ranges: list[tuple[tuple[int, int, int], tuple[int, int, int]]], horizon: int | None
) -> U8:
    """The pixels inside any of the HSV ranges, below the horizon, specks removed."""
    m = np.zeros(hsv.shape[:2], np.uint8)
    for lo, hi in ranges:
        m |= cast("U8", cv2.inRange(hsv, np.array(lo, np.uint8), np.array(hi, np.uint8)))
    if horizon is not None:
        m[: max(0, horizon)] = 0  # only the floor
    return cast("U8", cv2.morphologyEx(m, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8)))


def _mask(hsv: U8, b: BallColour, horizon: int | None) -> U8:
    # h_lo > h_hi wraps through 0 (red)
    hues = [(b.h_lo, b.h_hi)] if b.h_lo <= b.h_hi else [(b.h_lo, 179), (0, b.h_hi)]
    return _in_range(hsv, [((lo, b.s_min, 0), (hi, 255, b.v_max)) for lo, hi in hues], horizon)


Blob = tuple[float, float, float, float]  # centre x, centre y, radius, roundness


def _round_blobs(mask: NDArray[np.uint8], r_min: float) -> list[tuple[Blob, float]]:
    """The round blobs, each with its area."""
    out: list[tuple[Blob, float]] = []
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    for c in contours:
        (x, y), r = cv2.minEnclosingCircle(c)
        if r < r_min or r <= 0:
            continue
        roundness = float(cv2.contourArea(cv2.convexHull(c))) / (math.pi * r * r)
        if roundness >= MIN_ROUND:
            out.append(((float(x), float(y), float(r), roundness), float(cv2.contourArea(c))))
    return out


@dataclass(frozen=True)
class _Seen:
    x: float
    y: float
    r: float
    roundness: float
    area: float
    v_mean: float  # the mean value (brightness) of its pixels of the ball's colour
    size_cm: float | None  # 2 r x floor distance / fx; inf off the floor; None without a camera


def _seen(hsv: U8, mask: U8, blob: Blob, area: float, cam: CameraModel | None) -> _Seen:
    x, y, r, roundness = blob
    circle = np.zeros(mask.shape, np.uint8)
    cv2.circle(circle, (round(x), round(y)), max(1, round(r)), (255,), -1)
    v = hsv[(mask > 0) & (circle > 0)][:, 2]
    size: float | None = None
    if cam is not None:
        d = cam.floor_distance(x, y + r)  # along the ray to where its bottom meets the floor
        size = 2.0 * r * d / float(cam.cal.k[0, 0]) * 100.0 if math.isfinite(d) else math.inf
    return _Seen(x, y, r, roundness, area, float(v.mean()) if len(v) else 0.0, size)


def _is_ball(s: _Seen, b: BallColour) -> bool:
    if s.v_mean < b.v_mean_min:
        return False
    if b.size_cm is None or s.size_cm is None:
        return True
    return SIZE_RANGE[0] * b.size_cm <= s.size_cm <= SIZE_RANGE[1] * b.size_cm


def find_balls(rgb: NDArray[np.uint8], balls: list[BallColour], cam: CameraModel | None) -> list[Detection]:
    """One detection per ball colour found (the largest round blob of it that is bright enough and the right
    size for its distance), never a guess. Without the picture's camera: all of the picture, no size check."""
    if not balls:
        return []
    hsv = _hsv(rgb)
    h, w = rgb.shape[:2]
    horizon = horizon_row(cam) if cam is not None else None
    out: list[Detection] = []
    for b in balls:
        m = _mask(hsv, b, horizon)
        seen = [_seen(hsv, m, blob, area, cam) for blob, area in _round_blobs(m, b.r_min_px)]
        ok = [s for s in seen if _is_ball(s, b)]
        if not ok:
            continue
        s = max(ok, key=lambda s: s.area)
        box = (max(0.0, s.x - s.r), max(0.0, s.y - s.r), min(float(w), s.x + s.r), min(float(h), s.y + s.r))
        out.append(Detection(f"ball:{b.name}", min(1.0, s.roundness * SCORE_K), box))
    return out


def size_and_brightness(
    rgb: NDArray[np.uint8], b: BallColour, cam: CameraModel
) -> tuple[float, float] | None:
    """For calibration: the size (cm, as the tank measures it) and mean value of the round blob of the ball's
    colour nearest the middle column, or None."""
    hsv = _hsv(rgb)
    m = _mask(hsv, b, horizon_row(cam))
    seen = [_seen(hsv, m, blob, area, cam) for blob, area in _round_blobs(m, b.r_min_px)]
    if not seen:
        return None
    mid = rgb.shape[1] / 2
    s = min(seen, key=lambda s: abs(s.x - mid))
    return (s.size_cm, s.v_mean) if s.size_cm is not None and math.isfinite(s.size_cm) else None


def colour_from_samples(name: str, hsv_pixels: NDArray[np.uint8], r_min_px: float) -> BallColour:
    """A ball's colour range from its pixels (cq-calibrate ball): hue p5-p95 widened by 5, s_min = p10 - 10,
    v_max = p90 + 15 (design-notes §152 found the floor at S ~30, V 230-255 and the ball at S 72-108, V
    130-185). Hue is a circle: a red ball's range wraps through 0 (h_lo > h_hi). A spread wider than
    MAX_HUE_SPREAD is refused - that is not one colour."""
    hsv = hsv_pixels.reshape(-1, 3).astype(np.float64)
    h = hsv[:, 0]
    lo, hi = (float(v) for v in np.percentile(h, [5, 95]))
    slo, shi = (float(v) for v in np.percentile((h + 90) % 180, [5, 95]))  # the circle cut at 90 instead of 0
    wraps = shi - slo < hi - lo
    if wraps:
        lo, hi = slo - 90, shi - 90
    spread = math.ceil(hi) - int(lo) + 10
    if spread > MAX_HUE_SPREAD:
        raise ValueError(
            f"the {name} ball's pixels are not one colour (hue spread {spread} of 180) -"
            " is the ball alone in the middle of the view?"
        )
    h_lo, h_hi = math.floor(lo) - 5, math.ceil(hi) + 5
    if wraps and (h_lo < 0 or h_hi > 179):
        h_lo, h_hi = h_lo % 180, h_hi % 180
    else:
        h_lo, h_hi = max(0, h_lo), min(179, h_hi)
    s10 = float(np.percentile(hsv[:, 1], 10))
    v90 = float(np.percentile(hsv[:, 2], 90))
    return BallColour(name, h_lo, h_hi, max(0, int(s10) - 10), min(255, math.ceil(v90) + 15), r_min_px)


def most_coloured_blob(rgb: NDArray[np.uint8], cam: CameraModel | None) -> NDArray[np.uint8] | None:
    """For calibration: the HSV pixels of the coloured round blob below the horizon nearest the middle column
    (the ball is laid out in the middle), or None. Saturation >= 60 on a blurred picture."""
    hsv = _hsv(rgb)
    m = _in_range(hsv, [((0, 60, 0), (179, 255, 255))], horizon_row(cam) if cam is not None else None)
    blobs = _round_blobs(m, 6.0)
    if not blobs:
        return None
    mid = rgb.shape[1] / 2
    x, y, r, _ = min(blobs, key=lambda b: abs(b[0][0] - mid))[0]
    disc = np.zeros(m.shape, np.uint8)
    cv2.circle(disc, (round(x), round(y)), max(1, round(r * 0.7)), (255,), -1)  # the middle: no edge pixels
    pix: U8 = hsv[(disc > 0) & (m > 0)]
    return pix if len(pix) else None
