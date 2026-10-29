"""The room map (AI engine LLD §18.5, spec 9 §3): one 8 x 8 m map of 5 cm cells, fixed where the tank started
(the start at the centre, x forward, y left). Each cell holds two log-odds layers - low (2-10 cm, the sonar's
beam vouches for it) and high (10-20 cm, the bed edge) - and whether the camera has seen it.

From a picture's height marks: floor points lower both layers in their cell, low / high points raise their
layer, every point marks its cell seen. A cell moves one step per picture however many points fall in it,
and a cell with a mark in this picture is not lowered by floor points in the same picture: the map never
invents free space. Marks fade only where the camera sees floor now; the blind zone keeps its memory.
Pure: no I/O."""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Literal

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.depth import MapSettings, Marks
from conqueror_ai.domain.types import Pose

Layer = Literal["low", "high"]
BoolArray = NDArray[np.bool_]
DEPTH_OK_US = 1_000_000  # depth_ok: a depth picture this recently
# the body is a floor sensor (AI engine LLD §18.13.1, Plan Z6): the floor under the tank is free
BODY_SHRINK_M = 0.02  # the footprint shrunk this much all round (pose drift)
BODY_EVERY_US = 100_000  # at most 10 a second, one step per cell (like the sonar)
BODY_STEP_M = 0.025

UNSEEN = (0, 0, 0)
SEEN = (110, 110, 110)
LOW = (220, 40, 40)
HIGH = (255, 150, 0)
TANK = (255, 255, 255)


@dataclass(frozen=True)
class MapSnapshot:
    """What the drive gets (spec 9 §3.8): copies of the map's layers, the pose and the flags."""

    t_us: int
    pose: Pose | None
    cell_m: float
    blocked: BoolArray  # low or high above the blocked level
    high: BoolArray  # high above the blocked level (the view; the sonar can't clear these)
    seen: BoolArray
    stale: bool  # no picture for 0.5 s or no telemetry for 0.2 s (spec 7 §4)
    depth_ok: bool  # a depth picture in the last 1 s
    angle_trusted: bool


class RoomMap:
    def __init__(self, settings: MapSettings | None = None) -> None:
        self.settings = settings or MapSettings()
        self.cell_m = self.settings.cell_m
        self.n = round(self.settings.size_m / self.cell_m)
        self.half_m = self.settings.size_m / 2
        self.low = np.zeros((self.n, self.n), np.float32)
        self.high = np.zeros((self.n, self.n), np.float32)
        self.seen = np.zeros((self.n, self.n), np.bool_)
        self._last_picture_us: int | None = None
        self._last_body_us: int | None = None

    def cells(self, xy: FloatArray) -> NDArray[np.intp]:
        """The unique flat cell indices (ix * n + iy) of room points; points outside the map are dropped."""
        if len(xy) == 0:
            return np.zeros(0, np.intp)
        ij = np.floor((np.asarray(xy, np.float64) + self.half_m) / self.cell_m)
        inside = np.all((ij >= 0) & (ij < self.n), axis=1)
        ij = ij[inside].astype(np.intp)
        return np.unique(ij[:, 0] * self.n + ij[:, 1])

    def _layer(self, layer: Layer) -> NDArray[np.float32]:
        return self.low if layer == "low" else self.high

    def _step(self, cells: NDArray[np.intp], layer: Layer, step: float) -> None:
        a = self._layer(layer).reshape(-1)  # a view: changes the map
        c = self.settings.clamp
        a[cells] = np.clip(a[cells] + step, -c, c)

    def hit(self, cells: NDArray[np.intp], layer: Layer) -> None:
        self._step(cells, layer, self.settings.hit)

    def miss(self, cells: NDArray[np.intp], layer: Layer) -> None:
        self._step(cells, layer, self.settings.miss)

    def update_picture(self, marks: Marks, t_us: int) -> None:
        low, high = self.cells(marks.low_xy), self.cells(marks.high_xy)
        floor = np.setdiff1d(self.cells(marks.floor_xy), np.union1d(low, high), assume_unique=True)
        above = self.cells(marks.above_xy)
        self.hit(low, "low")
        self.hit(high, "high")
        self.miss(floor, "low")
        self.miss(floor, "high")
        seen = self.seen.reshape(-1)
        for c in (low, high, floor, above):
            seen[c] = True
        self._last_picture_us = t_us

    def body_over(self, pose: Pose, front_m: float, rear_m: float, half_w_m: float, t_us: int) -> bool:
        """The floor under the tank's footprint (shrunk BODY_SHRINK_M) is seen and free: one miss on both
        layers per cell. At most once per BODY_EVERY_US; whether it applied."""
        if self._last_body_us is not None and 0 <= t_us - self._last_body_us < BODY_EVERY_US:
            return False
        self._last_body_us = t_us
        f, r, w = front_m - BODY_SHRINK_M, rear_m - BODY_SHRINK_M, half_w_m - BODY_SHRINK_M
        gx, gy = np.meshgrid(np.arange(-r, f + 1e-9, BODY_STEP_M), np.arange(-w, w + 1e-9, BODY_STEP_M))
        c, s = math.cos(pose.th_rad), math.sin(pose.th_rad)
        xy = np.stack(
            [pose.x_m + gx.ravel() * c - gy.ravel() * s, pose.y_m + gx.ravel() * s + gy.ravel() * c], axis=1
        )
        cells = self.cells(xy)
        self.miss(cells, "low")
        self.miss(cells, "high")
        self.seen.reshape(-1)[cells] = True
        return True

    def blocked(self) -> BoolArray:
        b = self.settings.blocked
        return (self.low > b) | (self.high > b)

    def is_blocked(self, x: float, y: float) -> bool:
        """True for a blocked cell, and everywhere outside the map (its edge is a soft fence)."""
        if not (math.isfinite(x) and math.isfinite(y)):
            return True  # a broken position: nothing is known free there
        ix, iy = (
            int(np.floor((x + self.half_m) / self.cell_m)),
            int(np.floor((y + self.half_m) / self.cell_m)),
        )
        if not (0 <= ix < self.n and 0 <= iy < self.n):
            return True
        b = self.settings.blocked
        return bool(self.low[ix, iy] > b or self.high[ix, iy] > b)

    def snapshot(self, t_us: int, pose: Pose | None, stale: bool, angle_trusted: bool) -> MapSnapshot:
        last = self._last_picture_us
        depth_ok = last is not None and 0 <= t_us - last <= DEPTH_OK_US
        return MapSnapshot(
            t_us,
            pose,
            self.cell_m,
            self.blocked(),
            self.high > self.settings.blocked,
            self.seen.copy(),
            stale,
            depth_ok,
            angle_trusted,
        )


def map_image(snap: MapSnapshot) -> NDArray[np.uint8]:
    """The map as a picture: up = +x (ahead at the start), right = -y; unseen black, seen grey, low red, high
    orange, the tank's cell and its neighbours white."""
    n = snap.seen.shape[0]
    img = np.zeros((n, n, 3), np.uint8)  # [ix, iy] first, turned for the picture at the end
    img[snap.seen] = SEEN
    img[snap.blocked & ~snap.high] = LOW
    img[snap.high] = HIGH
    if snap.pose is not None and math.isfinite(snap.pose.x_m) and math.isfinite(snap.pose.y_m):
        ix = int(np.floor(snap.pose.x_m / snap.cell_m + n / 2))
        iy = int(np.floor(snap.pose.y_m / snap.cell_m + n / 2))
        if 0 <= ix < n and 0 <= iy < n:  # off the map: no marker (a negative slice end would wrap round)
            img[max(0, ix - 1) : ix + 2, max(0, iy - 1) : iy + 2] = TANK
    return np.ascontiguousarray(img[::-1, ::-1])  # row n-1-ix, column n-1-iy
