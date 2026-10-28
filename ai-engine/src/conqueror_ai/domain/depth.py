"""Seeing (AI engine LLD §18.4, spec 9 §3): depth to the camera's real tilt, metres, and heights."""

from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field, replace

import numpy as np

from conqueror_ai.domain.camera import CameraModel, FloatArray, undistorted_grid

MIN_ROWS = 20  # a picture's floor line needs this many rows ...
MIN_R2 = 0.98  # ... and to be this straight
MIN_ESTIMATES = 3  # a fix needs this many pictures' estimates in the window (one noisy picture isn't enough)
FIX_STEP_DEG = 0.05  # the fix comes in steps (so the floor-depth cache for a fixed camera can hit)
MIN_SCALE_R2 = 0.9  # the floor's depth must fit its expected depth this well (a wall close up doesn't)


@dataclass(frozen=True)
class Heights:
    """Heights above the floor (owner, design-notes §159): under floor_m is floor; up to low_top_m a low
    obstacle (the sonar's beam vouches for it); up to clear_m a high one; above, passable under."""

    floor_m: float = 0.02
    low_top_m: float = 0.10
    clear_m: float = 0.20
    below_m: float = -0.05  # a point this far under the floor is bad depth


@dataclass(frozen=True)
class Ranges:
    mark_max_m: float = 2.0  # obstacles (low / high) are trusted this far
    free_max_m: float = 3.0  # floor and "above" this far
    sonar_max_m: float = 4.0  # an echo this far or more marks nothing (spec 9 §5)


@dataclass(frozen=True)
class DepthSettings:
    every_n: int = 1  # depth on every n-th picture used
    tilt_offset_deg: float = 3.0  # the depth reads this much less down than the truth (design-notes §161)
    trust_deg: float = 3.0  # a fix bigger than this isn't trusted (spec 9 §5)


@dataclass(frozen=True)
class MapSettings:
    """The room map (AI engine LLD §18.5): 8 x 8 m of 5 cm cells, log-odds steps and the blocked level."""

    size_m: float = 8.0
    cell_m: float = 0.05
    hit: float = 0.85  # a mark seen in a cell
    miss: float = -0.4  # floor seen in a cell
    clamp: float = 4.0  # log-odds stay within +-clamp
    blocked: float = 0.7  # a layer above this is an obstacle


@dataclass(frozen=True)
class MapConfig:
    heights: Heights = field(default_factory=Heights)
    ranges: Ranges = field(default_factory=Ranges)
    depth: DepthSettings = field(default_factory=DepthSettings)
    map: MapSettings = field(default_factory=MapSettings)


# ---- the camera's real tilt (design-notes §155)
@dataclass(frozen=True)
class AngleFix:
    tilt_fix_deg: float  # add to the servo's tilt (0 when not trusted)
    trusted: bool
    depth_tilt_deg: float | None  # the newest picture's tilt from depth (+ up), None: no estimate
    n: int  # estimates in the window
    fresh: bool = False  # this picture gave an estimate


def _fit(x: FloatArray, y: FloatArray) -> tuple[float, float, float]:
    """Least squares y = p x + q, and R^2 (0 when x doesn't vary: nothing to fit)."""
    if float(np.ptp(x)) <= 1e-9 * max(1.0, float(np.abs(x).max())):
        return 0.0, float(y.mean()), 0.0
    p, q = (float(v) for v in np.polyfit(x, y, 1))
    res = y - (p * x + q)
    tot = float(np.sum((y - y.mean()) ** 2))
    return p, q, 1.0 - float(np.sum(res**2)) / tot if tot > 0 else 0.0


def depth_tilt_deg(cam: CameraModel, inv: FloatArray, offset_deg: float) -> float | None:
    """The camera's pitch (+ up) from the floor in a depth picture (relative inverse depth along the camera's
    axis), or None. Over a flat floor the inverse depth is a straight line in the undistorted image height
    y_n; it reaches 0 at the horizon, whose y_n is tan(pitch). The depth reads offset_deg less down."""
    w = inv.shape[1]
    col = min(w - 1, max(0, round(float(cam.cal.k[0, 2]))))
    c0, c1 = w // 4, 3 * w // 4
    _, yn, good = undistorted_grid(cam.cal)
    rows = np.nonzero(cam.floor_mask()[:, col] & good[:, col])[0]
    seg = inv[rows, c0:c1]
    ok = np.isfinite(seg) & (seg > 0)
    enough = ok.sum(axis=1) >= (c1 - c0) // 2
    if int(enough.sum()) < MIN_ROWS:
        return None
    with np.errstate(invalid="ignore"):
        meds = np.nanmedian(np.where(ok, seg, np.nan)[enough], axis=1)
    x, y = yn[rows[enough], col], meds
    p, q, _ = _fit(x, y)
    res = y - (p * x + q)
    keep = np.abs(res) <= 3 * max(float(res.std()), 1e-12)
    if keep.sum() < MIN_ROWS:
        return None
    p, q, r2 = _fit(x[keep], y[keep])
    if p <= 0 or r2 < MIN_R2:  # inverse depth must grow toward the bottom of the picture
        return None
    return math.degrees(math.atan(-q / p)) - offset_deg


class CameraAngle:
    """The servo's tilt corrected by the depth floor: the median over the last window_s of (depth tilt -
    servo tilt), trusted within trust_deg (spec 9 §5; otherwise 0 and the drive slows)."""

    def __init__(self, settings: DepthSettings, window_s: float = 1.0) -> None:
        self.settings = settings
        self.window_us = round(window_s * 1e6)
        self._diffs: deque[tuple[int, float]] = deque()
        self._fix = AngleFix(0.0, True, None, 0)

    def fix(self) -> AngleFix:
        return self._fix

    def update(self, cam: CameraModel, inv: FloatArray, t_us: int) -> AngleFix:
        servo = replace(cam, tilt_fix_deg=0.0)
        est = depth_tilt_deg(servo, inv, self.settings.tilt_offset_deg)
        if est is not None:
            self._diffs.append((t_us, est - servo.real_tilt_deg()))
        while self._diffs and self._diffs[0][0] < t_us - self.window_us:
            self._diffs.popleft()
        if est is None or not self._diffs:
            return replace(self._fix, fresh=False)  # no estimate in this picture: the last fix stays
        if len(self._diffs) < MIN_ESTIMATES:
            return replace(self._fix, depth_tilt_deg=est, n=len(self._diffs), fresh=True)
        med = float(np.median([d for _, d in self._diffs]))
        trusted = abs(med) <= self.settings.trust_deg
        fix = round(med / FIX_STEP_DEG) * FIX_STEP_DEG if trusted else 0.0
        self._fix = AngleFix(fix, trusted, est, len(self._diffs), fresh=True)
        return self._fix


# ---- depth in metres (spec 7's DepthScaler)
@dataclass(frozen=True)
class ScaledDepth:
    range_m: FloatArray  # per pixel, metres along its ray; inf where unknown
    a: float  # 1 / axis depth = a * inv + c
    c: float
    n: int  # floor pixels in the fit


class DepthScaler:
    """Fits the depth picture's unknown scale and shift on the floor, picture by picture: on floor pixels
    1 / axis depth (known from the camera) = a * inv + c (one 3-sigma trim)."""

    def __init__(self, min_pixels: int = 500, stride: int = 3) -> None:
        self.min_pixels = min_pixels
        self.stride = stride  # fit on every stride-th floor pixel each way: as good, ~10x less work

    def fit(self, cam: CameraModel, inv: FloatArray) -> ScaledDepth | None:
        norms = cam.ray_norms()
        good = np.isfinite(inv) & (inv > 0)
        use = cam.floor_mask() & good & np.isfinite(norms)
        thin = np.zeros_like(use)
        thin[:: self.stride, :: self.stride] = True
        use &= thin
        if int(use.sum()) < self.min_pixels:
            return None
        x = inv[use]
        y = norms[use] / cam.expected_floor_depth()[use]  # 1 / axis depth
        a, c, _ = _fit(x, y)
        res = y - (a * x + c)
        keep = np.abs(res) <= 3 * max(float(res.std()), 1e-12)
        if int(keep.sum()) < self.min_pixels:
            return None
        a, c, r2 = _fit(x[keep], y[keep])
        if a <= 0 or r2 < MIN_SCALE_R2:
            return None
        with np.errstate(divide="ignore", invalid="ignore"):
            den = a * inv + c
            rng = np.where(good & (den > 0) & np.isfinite(norms), norms / den, np.inf)
        return ScaledDepth(rng.astype(np.float64), a, c, int(keep.sum()))


# ---- every point's height (spec 9 §3.4)
@dataclass(frozen=True)
class Marks:
    """Room points (x, y in metres), by height: floor, low / high obstacles, and above (passable under)."""

    floor_xy: FloatArray
    low_xy: FloatArray
    high_xy: FloatArray
    above_xy: FloatArray


class HeightPoints:
    def __init__(self, heights: Heights, ranges: Ranges, stride: int = 4) -> None:
        self.heights = heights
        self.ranges = ranges
        self.stride = stride

    def classify(self, cam: CameraModel, range_m: FloatArray) -> Marks:
        """Every stride-th pixel becomes a point (the lens + its unit ray x its range) with a height."""
        hs, rs = self.heights, self.ranges
        rays = cam.unit_rays(self.stride)
        rng = range_m[:: self.stride, :: self.stride]
        lens = cam.position()
        with np.errstate(invalid="ignore"):
            pts = lens + rays * rng[..., None]
            z = pts[..., 2]
            dist = np.hypot(pts[..., 0] - lens[0], pts[..., 1] - lens[1])
            ok = np.isfinite(z) & np.isfinite(dist) & (rng > 0) & (z >= hs.below_m)
            near, far = ok & (dist <= rs.mark_max_m), ok & (dist <= rs.free_max_m)
            floor = far & (z < hs.floor_m)
            low = near & (z >= hs.floor_m) & (z < hs.low_top_m)
            high = near & (z >= hs.low_top_m) & (z <= hs.clear_m)
            above = far & (z > hs.clear_m)
        xy = pts[..., :2]
        return Marks(xy[floor], xy[low], xy[high], xy[above])


# ---- one picture, end to end (AI engine LLD §18.3)
@dataclass(frozen=True)
class Seen:
    fix: AngleFix
    cam: CameraModel  # the picture's camera with the fix
    scaled: ScaledDepth | None  # None: no floor to scale on (a wall close up)
    marks: Marks | None


class Seeing:
    """A depth picture to the tilt fix, metres and heights."""

    def __init__(self, cfg: MapConfig) -> None:
        self.cfg = cfg
        self.angle = CameraAngle(cfg.depth)
        self.scaler = DepthScaler()
        self.heights = HeightPoints(cfg.heights, cfg.ranges)

    def angle_only(self, cam: CameraModel, inv: FloatArray, t_us: int) -> Seen:
        """Only the tilt fix (cq-drive in Z3: no map yet)."""
        fix = self.angle.update(cam, inv, t_us)
        return Seen(fix, replace(cam, tilt_fix_deg=fix.tilt_fix_deg), None, None)

    def see(self, cam: CameraModel, inv: FloatArray, t_us: int) -> Seen:
        fix = self.angle.update(cam, inv, t_us)
        fixed = replace(cam, tilt_fix_deg=fix.tilt_fix_deg)
        scaled = self.scaler.fit(fixed, inv)
        marks = None if scaled is None else self.heights.classify(fixed, scaled.range_m)
        return Seen(fix, fixed, scaled, marks)
