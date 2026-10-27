"""A simulated tank for tests (AI engine LLD §17.2): it follows the commands exactly (the bridge's
gyro holds the turn rate), so whole chases run on the laptop."""

from __future__ import annotations

import math

from conqueror_ai.domain.drive.curves import Command, XYTh


class SimTank:
    def __init__(self, x: float = 0.0, y: float = 0.0, th: float = 0.0) -> None:
        self.at: XYTh = (x, y, th)

    def step(self, cmd: Command, dt: float) -> XYTh:
        x, y, th = self.at
        v, w = cmd.v_cms / 100.0, math.radians(cmd.w_dps)
        th_mid = th + w * dt / 2
        self.at = (x + v * dt * math.cos(th_mid), y + v * dt * math.sin(th_mid), th + w * dt)
        return self.at
