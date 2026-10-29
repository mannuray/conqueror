"""A simulated tank for tests (AI engine LLD §17.2): it follows the commands exactly (the bridge's
gyro holds the turn rate), so whole chases run on the laptop."""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.depth import Marks
from conqueror_ai.domain.drive.curves import Command, XYTh
from conqueror_ai.domain.room_map import RoomMap


class SimTank:
    def __init__(self, x: float = 0.0, y: float = 0.0, th: float = 0.0) -> None:
        self.at: XYTh = (x, y, th)

    def step(self, cmd: Command, dt: float) -> XYTh:
        x, y, th = self.at
        v, w = cmd.v_cms / 100.0, math.radians(cmd.w_dps)
        th_mid = th + w * dt / 2
        self.at = (x + v * dt * math.cos(th_mid), y + v * dt * math.sin(th_mid), th + w * dt)
        return self.at


# ---- a room for simulated drives on the map (Plan Z5)
CAM_X_M = 0.085  # the lens ahead of the tank's centre
SONAR_X_M = 0.13
SONAR_HALF_DEG = 12.0
SONAR_MAX_M = 4.0
SONAR_SEES_UNDER_M = 0.10  # the sonar's beam passes under anything starting this high
FRONT_X_M = 0.13
TANK_TOP_M = 0.18  # a box starting higher than this passes over the tank
POINT_STEP_M = 0.05
REACH_M = 0.15  # the tank's centre can't come nearer than this to a box or wall (for the reachable floor)


@dataclass(frozen=True)
class Box:
    """A box in the room: x0..x1, y0..y1 on the floor plan, z0..z1 in height (z0 > 0: an overhang)."""

    x0: float
    y0: float
    x1: float
    y1: float
    z0: float = 0.0
    z1: float = 0.15

    def ray(self, ox: float, oy: float, dx: float, dy: float) -> float | None:
        """Where a ray from (ox, oy) along the unit (dx, dy) enters the box, or None (slab method)."""
        lo, hi = 0.0, math.inf
        for o, d, a, b in ((ox, dx, self.x0, self.x1), (oy, dy, self.y0, self.y1)):
            if abs(d) < 1e-12:
                if not a <= o <= b:
                    return None
                continue
            t0, t1 = sorted(((a - o) / d, (b - o) / d))
            lo, hi = max(lo, t0), min(hi, t1)
        return lo if lo <= hi else None

    def gap(self, x: float, y: float) -> float:
        return math.hypot(max(self.x0 - x, 0.0, x - self.x1), max(self.y0 - y, 0.0, y - self.y1))


class SimRoom:
    """Boxes (some overhanging) inside square walls at +-walls_m: what the camera's height marks and the sonar
    would give from a pose, and how close the tank comes to anything."""

    def __init__(self, boxes: list[Box], walls_m: float = 3.0) -> None:
        self.boxes = boxes
        self.walls_m = walls_m
        w, t = walls_m, 1.0
        self.walls = [Box(w, -w - t, w + t, w + t, 0.0, 1.0), Box(-w - t, -w - t, -w, w + t, 0.0, 1.0),
                      Box(-w, w, w, w + t, 0.0, 1.0), Box(-w, -w - t, w, -w, 0.0, 1.0)]  # fmt: skip

    def _all(self) -> list[Box]:
        return self.boxes + self.walls

    def marks(self, at: XYTh, half_deg: float = 41.0, near_m: float = 0.30, far_m: float = 2.0) -> Marks:
        """Rays every 1 deg across the view, points every 5 cm: floor up to the first standing box, its face
        as low / high / above points by its height; an overhang's face is high / above, the floor goes on."""
        x, y, th = at
        cx, cy = x + CAM_X_M * math.cos(th), y + CAM_X_M * math.sin(th)
        floor: list[tuple[float, float]] = []
        low: list[tuple[float, float]] = []
        high: list[tuple[float, float]] = []
        above: list[tuple[float, float]] = []
        for k in range(-round(half_deg), round(half_deg) + 1):
            a = th + math.radians(k)
            dx, dy = math.cos(a), math.sin(a)
            stop = far_m
            hits = sorted(
                (t, b) for b in self._all() if (t := b.ray(cx, cy, dx, dy)) is not None and t <= far_m
            )
            for t, b in hits:
                p = (cx + t * dx, cy + t * dy)
                if b.z0 < 0.02:  # standing: its face, then nothing behind it
                    low.append(p)
                    if b.z1 > 0.10:
                        high.append(p)
                    if b.z1 > 0.20:
                        above.append(p)
                    stop = t
                    break
                if b.z0 <= 0.20:  # an overhang's front edge; the floor under it stays in view
                    high.append(p)
                if b.z1 > 0.20:
                    above.append(p)
            d = near_m
            while d < stop:
                floor.append((cx + d * dx, cy + d * dy))
                d += POINT_STEP_M

        def arr(v: list[tuple[float, float]]) -> FloatArray:
            return np.array(v, dtype=np.float64).reshape(-1, 2)

        return Marks(arr(floor), arr(low), arr(high), arr(above))

    def sonar(self, at: XYTh) -> float | None:
        """The nearest echo across the beam (+-12 deg) from the sonar, from boxes it can't pass under."""
        x, y, th = at
        sx, sy = x + SONAR_X_M * math.cos(th), y + SONAR_X_M * math.sin(th)
        best = math.inf
        for k in range(-round(SONAR_HALF_DEG), round(SONAR_HALF_DEG) + 1):
            a = th + math.radians(k)
            for b in self._all():
                if b.z0 < SONAR_SEES_UNDER_M and (t := b.ray(sx, sy, math.cos(a), math.sin(a))) is not None:
                    best = min(best, t)
        return best if best < SONAR_MAX_M else None

    def gap_m(self, at: XYTh) -> float:
        """The tank's centre to the nearest box or wall (overhangs too: they would hit the camera)."""
        return min(b.gap(at[0], at[1]) for b in self._all() if b.z0 < TANK_TOP_M)

    def front_inside(self, at: XYTh) -> bool:
        x, y, th = at
        fx, fy = x + FRONT_X_M * math.cos(th), y + FRONT_X_M * math.sin(th)
        return any(b.gap(fx, fy) == 0.0 for b in self._all() if b.z0 < TANK_TOP_M)

    def _reachable(self, room: RoomMap) -> NDArray[np.bool_]:
        c = (np.arange(room.n) + 0.5) * room.cell_m - room.half_m
        xs, ys = np.meshgrid(c, c, indexing="ij")
        ok = np.ones((room.n, room.n), np.bool_)
        for b in self._all():
            if b.z0 < TANK_TOP_M:
                gx = np.maximum(np.maximum(b.x0 - xs, 0.0), xs - b.x1)
                gy = np.maximum(np.maximum(b.y0 - ys, 0.0), ys - b.y1)
                ok &= np.hypot(gx, gy) >= REACH_M
        return ok

    def seen_share(self, room: RoomMap) -> float:
        """The share of the floor the tank's centre could reach that the map has seen."""
        ok = self._reachable(room)
        return float((room.seen & ok).sum()) / max(1, int(ok.sum()))
