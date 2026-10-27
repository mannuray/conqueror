"""The camera model (AI engine LLD §5.3): pixel <-> floor point through the lens (pinhole + distortion),
the mount on the tank and the pan / tilt servos. Pure numpy: no OpenCV, no I/O.

Frames: the room (x forward at the start, y left, z up); the camera looks along its forward axis, image x to
right and image y down (OpenCV). Angles from the centre: pan + = right, tilt + = up (cam_set, Logo's LOOK)."""

from __future__ import annotations

import math
import threading
from dataclasses import dataclass, field, replace

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.types import Pose

FloatArray = NDArray[np.float64]
NEAR_M, FAR_M = 0.25, 2.5  # the floor fit is trusted between these (LLD §5.3)

# Per calibration (keyed by identity; the object is kept so its id is never reused while cached): the fold,
# and the expected floor depth per (pan, tilt) - it does not depend on the tank's pose (LLD §5.3).
_FOLDS: dict[int, tuple[Calibration, float, float]] = {}
_DEPTHS: dict[tuple[int, float, float, float], tuple[Calibration, FloatArray]] = {}
_MAX_DEPTHS = 16
_CACHE_LOCK = threading.Lock()
_GRIDS: dict[int, tuple[Calibration, FloatArray, FloatArray, NDArray[np.bool_]]] = {}


def _fold(cal: Calibration) -> tuple[float, float]:
    """(r, rd): the distortion r -> r (1 + k1 r^2 + k2 r^4 + k3 r^6) stops growing at r (its fold); beyond
    it a distorted radius has no answer (final review I1). inf, inf when it keeps growing within r 0..4."""
    hit = _FOLDS.get(id(cal))
    if hit is not None and hit[0] is cal:
        return hit[1], hit[2]
    k1, k2, _, _, k3 = (float(v) for v in cal.dist[:5])
    r = np.linspace(0.0, 4.0, 8001)
    f = r * (1 + k1 * r**2 + k2 * r**4 + k3 * r**6)
    falls = np.nonzero(np.diff(f) <= 0)[0]
    out = (float(r[falls[0]]), float(f[falls[0]])) if len(falls) else (math.inf, math.inf)
    _FOLDS[id(cal)] = (cal, out[0], out[1])
    return out


def from_servo(pan_servo_deg: float, tilt_servo_deg: float) -> tuple[float, float]:
    """Telemetry's servo angles (90 = centre) -> degrees from the centre (right / up = +)."""
    return 90.0 - pan_servo_deg, 90.0 - tilt_servo_deg


def undistorted_grid(cal: Calibration) -> tuple[FloatArray, FloatArray, NDArray[np.bool_]]:
    """Every pixel's normalised (undistorted) coordinates, and where they are good (inside the fold, and
    distorting back to the pixel) - per calibration, cached (it doesn't depend on pan, tilt or pose)."""
    hit = _GRIDS.get(id(cal))
    if hit is not None and hit[0] is cal:
        return hit[1], hit[2], hit[3]
    w, h = cal.size
    k = cal.k
    uu, vv = np.meshgrid(np.arange(w, dtype=np.float64), np.arange(h, dtype=np.float64))
    xd, yd = (uu - k[0, 2]) / k[0, 0], (vv - k[1, 2]) / k[1, 1]
    k1, k2, p1, p2, k3 = (float(v) for v in cal.dist[:5])
    rd = np.hypot(xd, yd)
    r = rd.copy()
    for _ in range(30):  # the same method as undistort, on whole arrays: Newton on the radius ...
        r2 = r * r
        f = r * (1 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2) - rd
        fp = 1 + 3 * k1 * r2 + 5 * k2 * r2 * r2 + 7 * k3 * r2 * r2 * r2
        with np.errstate(divide="ignore", invalid="ignore"):
            r = np.where(fp > 0, r - f / fp, np.nan)
    with np.errstate(divide="ignore", invalid="ignore"):
        s = np.where(rd > 0, r / rd, 1.0)
    xn, yn = xd * s, yd * s

    def dist2(x: FloatArray, y: FloatArray) -> tuple[FloatArray, FloatArray]:
        q2 = x * x + y * y
        rad = 1 + k1 * q2 + k2 * q2 * q2 + k3 * q2 * q2 * q2
        return x * rad + 2 * p1 * x * y + p2 * (q2 + 2 * x * x), y * rad + p1 * (
            q2 + 2 * y * y
        ) + 2 * p2 * x * y

    for _ in range(10):  # ... then the tangential part, a 2-D Newton step per pixel
        bx, by = dist2(xn, yn)
        ex, ey = bx - xd, by - yd
        h = 1e-7
        ax, ay = dist2(xn + h, yn)
        cx, cy = dist2(xn, yn + h)
        j11, j12, j21, j22 = (ax - bx) / h, (cx - bx) / h, (ay - by) / h, (cy - by) / h
        det = j11 * j22 - j12 * j21
        with np.errstate(divide="ignore", invalid="ignore"):
            xn = xn - (j22 * ex - j12 * ey) / det
            yn = yn - (-j21 * ex + j11 * ey) / det
    back_x, back_y = dist2(xn, yn)  # distort back: does each answer return to its pixel?
    with np.errstate(invalid="ignore"):
        good = (rd < _fold(cal)[1]) & (np.hypot(back_x - xd, back_y - yd) <= 1e-7)
    with _CACHE_LOCK:
        if len(_GRIDS) >= _MAX_DEPTHS:
            _GRIDS.pop(next(iter(_GRIDS)))
        _GRIDS[id(cal)] = (cal, xn, yn, good)
    return xn, yn, good


@dataclass(frozen=True)
class Calibration:
    """What the calibration tool measured (LLD §8); stored in config/calibration.yaml."""

    k: FloatArray  # 3 x 3: fx, fy, cx, cy
    dist: FloatArray  # k1 k2 p1 p2 k3 (OpenCV)
    size: tuple[int, int]  # width, height in pixels (QVGA 320 x 240)
    cam_height_m: float  # the lens above the floor
    cam_x_m: float  # the lens ahead of the tank's centre
    cam_y_m: float  # the lens left of the tank's centre
    track_front_m: (
        float  # the tracks' front ahead of the tank's centre (the blind zone is measured from here)
    )
    tilt0_deg: float  # the camera's real tilt with the servo at its centre (+ = up)
    pan_map: tuple[float, float] = (1.0, 0.0)  # real pan = gain x commanded + offset
    tilt_map: tuple[float, float] = (1.0, 0.0)  # real tilt change = gain x commanded + offset
    readout_us: int = 0
    date: str = ""


@dataclass(frozen=True)
class CameraModel:
    """One camera pose: the tank pose and the commanded pan / tilt (for_frame), over a Calibration."""

    cal: Calibration
    pose: Pose = field(default_factory=lambda: Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0))
    pan_deg: float = 0.0
    tilt_deg: float = 0.0
    tilt_fix_deg: float = 0.0  # the depth floor's correction to the servo's tilt (AI engine LLD §18.4)

    def for_frame(self, pose: Pose, pan_deg: float, tilt_deg: float) -> CameraModel:
        return replace(self, pose=pose, pan_deg=pan_deg, tilt_deg=tilt_deg)

    # ---- the camera in the room
    def real_pan_deg(self) -> float:
        g, o = self.cal.pan_map
        return g * self.pan_deg + o

    def real_tilt_deg(self) -> float:
        g, o = self.cal.tilt_map
        return self.cal.tilt0_deg + g * self.tilt_deg + o + self.tilt_fix_deg

    def position(self) -> FloatArray:
        c, s = math.cos(self.pose.th_rad), math.sin(self.pose.th_rad)
        x = self.pose.x_m + c * self.cal.cam_x_m - s * self.cal.cam_y_m
        y = self.pose.y_m + s * self.cal.cam_x_m + c * self.cal.cam_y_m
        return np.array([x, y, self.cal.cam_height_m])

    def axes(self) -> tuple[FloatArray, FloatArray, FloatArray]:
        """The camera's right, down and forward directions in the room."""
        yaw = self.pose.th_rad - math.radians(self.real_pan_deg())  # pan right turns clockwise
        pitch = math.radians(self.real_tilt_deg())
        fwd = np.array([math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch)])
        right = np.array([math.sin(yaw), -math.cos(yaw), 0.0])
        down = np.cross(fwd, right)
        return right, down, fwd

    # ---- the lens
    def distort(self, xn: float, yn: float) -> tuple[float, float]:
        k1, k2, p1, p2, k3 = (float(v) for v in self.cal.dist[:5])
        r2 = xn * xn + yn * yn
        radial = 1 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2
        xd = xn * radial + 2 * p1 * xn * yn + p2 * (r2 + 2 * xn * xn)
        yd = yn * radial + p1 * (r2 + 2 * yn * yn) + 2 * p2 * xn * yn
        return xd, yd

    def undistort(self, xd: float, yd: float) -> tuple[float, float] | None:
        """Inverts distort: Newton on the radius (the lens's radial curve), then a 2-D Newton step for the
        small tangential part. The tank session (2026-10-23) found a fixed-point iteration did not settle at
        the real lens's corners. None past the lens's fold, or when the answer does not distort back (final
        review I1)."""
        _, rd_fold = _fold(self.cal)
        rd = math.hypot(xd, yd)
        if rd >= rd_fold:
            return None
        if rd == 0.0:
            return 0.0, 0.0
        k1, k2, _, _, k3 = (float(v) for v in self.cal.dist[:5])
        r = rd
        for _ in range(30):
            r2 = r * r
            f = r * (1 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2) - rd
            fp = 1 + 3 * k1 * r2 + 5 * k2 * r2 * r2 + 7 * k3 * r2 * r2 * r2
            if fp <= 0:
                return None
            r -= f / fp
        xn, yn = xd * r / rd, yd * r / rd
        for _ in range(10):  # the tangential part: Newton with a numerical Jacobian
            bx, by = self.distort(xn, yn)
            ex, ey = bx - xd, by - yd
            if math.hypot(ex, ey) < 1e-13:
                break
            h = 1e-7
            ax, ay = self.distort(xn + h, yn)
            cx, cy = self.distort(xn, yn + h)
            j11, j12, j21, j22 = (ax - bx) / h, (cx - bx) / h, (ay - by) / h, (cy - by) / h
            det = j11 * j22 - j12 * j21
            if det == 0:
                return None
            xn -= (j22 * ex - j12 * ey) / det
            yn -= (-j21 * ex + j11 * ey) / det
        bx, by = self.distort(xn, yn)
        if not (math.isfinite(xn) and math.isfinite(yn)) or math.hypot(bx - xd, by - yd) > 1e-7:
            return None
        return xn, yn

    def pixel_ray(self, u: float, v: float) -> FloatArray | None:
        """The pixel's ray in the room (not unit length), or None past the lens's fold."""
        k = self.cal.k
        n = self.undistort((u - k[0, 2]) / k[0, 0], (v - k[1, 2]) / k[1, 1])
        if n is None:
            return None
        right, down, fwd = self.axes()
        return n[0] * right + n[1] * down + fwd

    # ---- pixel <-> floor
    def pixel_to_floor(self, u: float, v: float) -> tuple[float, float] | None:
        """The floor point (room x, y) the pixel sees, or None at or above the horizon."""
        ray = self.pixel_ray(u, v)
        if ray is None or ray[2] >= -1e-9:
            return None
        c = self.position()
        t = -c[2] / ray[2]
        p = c + t * ray
        return float(p[0]), float(p[1])

    def floor_to_pixel(self, x: float, y: float) -> tuple[float, float] | None:
        """Where the floor point (room x, y) is in the picture, or None behind the camera."""
        d = np.array([x, y, 0.0]) - self.position()
        right, down, fwd = self.axes()
        z = float(d @ fwd)
        if z <= 1e-9:
            return None
        xn, yn = float(d @ right) / z, float(d @ down) / z
        if math.hypot(xn, yn) >= _fold(self.cal)[0]:
            return (
                None  # outside the lens's valid view: it would fold back into the picture (final review I1)
            )
        xd, yd = self.distort(xn, yn)
        k = self.cal.k
        return float(k[0, 0] * xd + k[0, 2]), float(k[1, 1] * yd + k[1, 2])

    def floor_distance(self, u: float, v: float) -> float:
        """Along the pixel's ray to the floor (m); inf at or above the horizon."""
        ray = self.pixel_ray(u, v)
        if ray is None or ray[2] >= -1e-9:
            return math.inf
        return float(-self.cal.cam_height_m / ray[2] * np.linalg.norm(ray))

    def expected_floor_depth(self) -> FloatArray:
        """For every pixel, the distance along its ray to the floor (inf above the horizon or past the lens's
        fold) - all at once, cached per (calibration, pan, tilt, tilt fix)."""
        key = (id(self.cal), self.pan_deg, self.tilt_deg, self.tilt_fix_deg)
        hit = _DEPTHS.get(key)
        if hit is not None and hit[0] is self.cal:
            return hit[1]
        out = self._floor_depths()
        with _CACHE_LOCK:  # the seeing runs in a worker thread (AI engine LLD §18.3)
            if len(_DEPTHS) >= _MAX_DEPTHS:
                _DEPTHS.pop(next(iter(_DEPTHS)))
            _DEPTHS[key] = (self.cal, out)
        return out

    def _floor_depths(self) -> FloatArray:
        xn, yn, good = undistorted_grid(self.cal)
        right, down, fwd = self.axes()
        rz = xn * right[2] + yn * down[2] + fwd[2]
        norm = np.sqrt(
            (xn * right[0] + yn * down[0] + fwd[0]) ** 2
            + (xn * right[1] + yn * down[1] + fwd[1]) ** 2
            + rz**2
        )
        with np.errstate(divide="ignore", invalid="ignore"):
            out = np.where(good & (rz < -1e-9), -self.cal.cam_height_m / rz * norm, np.inf)
        return out.astype(np.float64)

    def unit_rays(self, stride: int = 1) -> FloatArray:
        """Every stride-th pixel's unit ray in the room, (h / stride, w / stride, 3); nan past the fold. Not
        cached: it turns with the tank's heading."""
        xn, yn, good = undistorted_grid(self.cal)
        xn, yn, good = xn[::stride, ::stride], yn[::stride, ::stride], good[::stride, ::stride]
        right, down, fwd = self.axes()
        rays = xn[..., None] * right + yn[..., None] * down + fwd
        rays /= np.linalg.norm(rays, axis=-1, keepdims=True)
        return np.where(good[..., None], rays, np.nan)

    def ray_norms(self) -> FloatArray:
        """Per pixel, the length of its ray (x_n, y_n, 1): range = depth along the camera's axis x this. A
        depth model gives (inverse) depth along the axis; the floor's expected depth is a range (LLD §18.4).
        nan past the lens's fold."""
        xn, yn, good = undistorted_grid(self.cal)
        return np.where(good, np.sqrt(xn * xn + yn * yn + 1.0), np.nan)

    def floor_mask(self) -> NDArray[np.bool_]:
        d = self.expected_floor_depth()
        return (d >= NEAR_M) & (d <= FAR_M)

    def sees(self, x: float, y: float, max_m: float = FAR_M) -> bool:
        """The floor point is inside the picture and within max_m (default FAR_M) of the camera."""
        q = self.floor_to_pixel(x, y)
        if q is None:
            return False
        w, h = self.cal.size
        if not (0 <= q[0] < w and 0 <= q[1] < h):
            return False
        c = self.position()
        return math.dist((x, y, 0.0), (float(c[0]), float(c[1]), float(c[2]))) <= max_m
