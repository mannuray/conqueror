"""The colour ball finder (AI engine LLD §16.1 items 3 and 5): round blobs of known colours on the floor.

Plain OpenCV on arrays, no files: the largest round blob of each ball's colour below the horizon. Measured
on the
tank (design-notes §152): the green ball in 187 of 194 pictures, steady to +-1.6 px, 1.1 ms at VGA."""

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


@dataclass(frozen=True)
class BallColour:
    name: str
    h_lo: int  # OpenCV hue, 0-179
    h_hi: int
    s_min: int
    v_max: int
    r_min_px: float = 6.0


def horizon_row(cam: CameraModel) -> int | None:
    """The picture row of the horizon straight ahead of the camera, or None when it is off the picture."""
    c = cam.position()
    fwd = cam.axes()[2]  # right, down, forward
    n = math.hypot(float(fwd[0]), float(fwd[1]))
    if n < 1e-6:
        return None  # looking straight down
    x = float(c[0]) + float(fwd[0]) / n * HORIZON_AHEAD_M
    y = float(c[1]) + float(fwd[1]) / n * HORIZON_AHEAD_M
    q = cam.floor_to_pixel(x, y)
    if q is None:
        return None
    row = round(q[1])
    return row if 0 <= row < cam.cal.size[1] else None


U8 = NDArray[np.uint8]


def _hsv(rgb: U8) -> U8:
    return cast("U8", cv2.cvtColor(cv2.GaussianBlur(rgb, (5, 5), 0), cv2.COLOR_RGB2HSV))


def _in_range(hsv: U8, lo: tuple[int, int, int], hi: tuple[int, int, int], horizon: int | None) -> U8:
    """The pixels inside the HSV range, below the horizon, specks removed."""
    m = cast("U8", cv2.inRange(hsv, np.array(lo, np.uint8), np.array(hi, np.uint8)))
    if horizon is not None:
        m[: max(0, horizon)] = 0  # only the floor
    return cast("U8", cv2.morphologyEx(m, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8)))


def _mask(hsv: U8, b: BallColour, horizon: int | None) -> U8:
    return _in_range(hsv, (b.h_lo, b.s_min, 0), (b.h_hi, 255, b.v_max), horizon)


def _blob(mask: NDArray[np.uint8], r_min: float) -> tuple[float, float, float, float] | None:
    """The largest round blob: centre x, centre y, radius, roundness."""
    best: tuple[float, float, float, float] | None = None
    best_area = 0.0
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    for c in contours:
        area = float(cv2.contourArea(c))
        (x, y), r = cv2.minEnclosingCircle(c)
        if r < r_min or r <= 0:
            continue
        roundness = float(cv2.contourArea(cv2.convexHull(c))) / (math.pi * r * r)
        if roundness >= MIN_ROUND and area > best_area:
            best, best_area = (float(x), float(y), float(r), roundness), area
    return best


def find_balls(rgb: NDArray[np.uint8], balls: list[BallColour], horizon: int | None) -> list[Detection]:
    """One detection per ball colour found (the largest round blob of it), never a guess."""
    if not balls:
        return []
    hsv = _hsv(rgb)
    h, w = rgb.shape[:2]
    out: list[Detection] = []
    for b in balls:
        blob = _blob(_mask(hsv, b, horizon), b.r_min_px)
        if blob is None:
            continue
        x, y, r, roundness = blob
        box = (max(0.0, x - r), max(0.0, y - r), min(float(w), x + r), min(float(h), y + r))
        out.append(Detection(f"ball:{b.name}", min(1.0, roundness * SCORE_K), box))
    return out


def colour_from_samples(name: str, hsv_pixels: NDArray[np.uint8], r_min_px: float) -> BallColour:
    """A ball's colour range from its pixels (cq-calibrate ball): hue p5-p95 widened by 5, s_min = p10 - 10,
    v_max = p90 + 15 (design-notes §152 found the floor at S ~30, V 230-255 and the ball at S 72-108, V
    130-185)."""
    hsv = hsv_pixels.reshape(-1, 3).astype(np.float64)
    h5, h95 = np.percentile(hsv[:, 0], [5, 95])
    s10 = float(np.percentile(hsv[:, 1], 10))
    v90 = float(np.percentile(hsv[:, 2], 90))
    return BallColour(
        name,
        max(0, int(h5) - 5),
        min(179, math.ceil(h95) + 5),
        max(0, int(s10) - 10),
        min(255, math.ceil(v90) + 15),
        r_min_px,
    )


def most_coloured_blob(rgb: NDArray[np.uint8], horizon: int | None) -> NDArray[np.uint8] | None:
    """For calibration: the HSV pixels of the most saturated round blob below the horizon (the ball laid out),
    or None. Saturation >= 60 on a blurred picture, the largest round piece."""
    hsv = _hsv(rgb)
    m = _in_range(hsv, (0, 60, 0), (179, 255, 255), horizon)
    blob = _blob(m, 6.0)
    if blob is None:
        return None
    x, y, r, _ = blob
    disc = np.zeros(m.shape, np.uint8)
    cv2.circle(disc, (round(x), round(y)), max(1, round(r * 0.7)), (255,), -1)  # the middle: no edge pixels
    pix: U8 = hsv[(disc > 0) & (m > 0)]
    return pix if len(pix) else None
