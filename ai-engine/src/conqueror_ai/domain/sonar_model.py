"""The distance sensor on the room map (AI engine LLD §18.5, spec 9 §5): an echo marks "something low at this
distance" on the beam's arc and clears low marks in the narrow cone before it. It never touches the high
layer (the beam is low on the tank's front: it can't vouch for a bed edge), never marks seen (only the camera
does), and no echo - or one at the sensor's limit - changes nothing. Pure: no I/O."""

from __future__ import annotations

import math

import numpy as np

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.room_map import RoomMap
from conqueror_ai.domain.types import Pose


class SonarModel:
    def __init__(
        self,
        sonar_x_m: float = 0.13,  # the sensor ahead of the tank's centre (as the drive's Limits)
        half_deg: float = 12.0,  # the beam's half-angle (Plan Q0): the arc an echo may come from
        cone_deg: float = 4.0,  # the narrow cone cleared before the echo
        near_m: float = 0.10,  # the cone starts this far out
        max_m: float = 4.0,  # an echo this far or more marks nothing
        every_us: int = 100_000,  # at most 10 samples a second used
    ) -> None:
        self.sonar_x_m = sonar_x_m
        self.half_rad = math.radians(half_deg)
        self.cone_rad = math.radians(cone_deg)
        self.near_m = near_m
        self.max_m = max_m
        self.every_us = every_us
        self._last_us: int | None = None

    def update(self, room: RoomMap, pose: Pose, dist_m: float | None, t_us: int) -> bool:
        """One telemetry sample: True when it was used (an echo, and 0.1 s since the last one used)."""
        if dist_m is None or dist_m >= self.max_m or dist_m < 0:
            return False
        if self._last_us is not None and 0 <= t_us - self._last_us < self.every_us:
            return False
        self._last_us = t_us
        sx = pose.x_m + self.sonar_x_m * math.cos(pose.th_rad)
        sy = pose.y_m + self.sonar_x_m * math.sin(pose.th_rad)
        step = room.cell_m / 4  # points closer than a cell, so no cell on the arc is skipped
        arc = room.cells(self._fan(sx, sy, pose.th_rad, np.array([dist_m]), self.half_rad, step))
        room.hit(arc, "low")
        far = dist_m - 0.10
        if far > self.near_m:
            radii = np.arange(self.near_m, far + 1e-9, step, dtype=np.float64)
            cone = room.cells(self._fan(sx, sy, pose.th_rad, radii, self.cone_rad, step))
            room.miss(np.setdiff1d(cone, arc, assume_unique=True), "low")
        return True

    @staticmethod
    def _fan(sx: float, sy: float, th: float, radii: FloatArray, half: float, step: float) -> FloatArray:
        """Room points on arcs of the given radii, +-half around the heading, about `step` apart."""
        pts: list[FloatArray] = []
        for r in radii:
            k = max(2, math.ceil(2 * half * max(float(r), step) / step) + 1)
            a = th + np.linspace(-half, half, k)
            pts.append(np.stack([sx + r * np.cos(a), sy + r * np.sin(a)], axis=1))
        return np.concatenate(pts)
