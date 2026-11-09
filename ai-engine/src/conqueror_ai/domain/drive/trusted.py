"""Trusted floor (AI engine LLD §28.4, Spec 10 S7): a game backs only over floor the map saw free lately, from
near, with no low / high mark since - the bridge's back_ok question (R34.2) and ESCAPE's backing."""

from __future__ import annotations

import math

import numpy as np

from conqueror_ai.domain.drive.curves import XYTh
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.room_map import RoomMap

MAX_AGE_S = 5.0  # first guess from the map settings, to check on the floor
STEP_M = 0.02  # the swept strip sampled ~2 cm apart (cells are 10 cm)


class TrustedFloor:
    @staticmethod
    def ok(room: RoomMap, pose: XYTh, back_m: float, now_us: int, max_age_s: float = MAX_AGE_S) -> bool:
        """Every cell the body's back sweeps over backing `back_m` straight (behind its rear edge, its whole
        width) was seen free within `max_age_s` and is not blocked."""
        if back_m <= 0.0:
            return True
        lim = Limits()
        x, y, th = pose
        us = np.arange(-(lim.body_rear_m + back_m), -lim.body_rear_m + 1e-9, STEP_M)
        vs = np.linspace(-lim.body_half_w_m, lim.body_half_w_m, 1 + math.ceil(2 * lim.body_half_w_m / STEP_M))
        gu, gv = np.meshgrid(us, vs)
        c, s = math.cos(th), math.sin(th)
        xy = np.stack([x + gu.ravel() * c - gv.ravel() * s, y + gu.ravel() * s + gv.ravel() * c], 1)
        cells = room.cells(xy)
        if len(cells) < len(np.unique(np.floor((xy + room.half_m) / room.cell_m), axis=0)):
            return False  # part of the path is off the map
        free = room.free_us.reshape(-1)[cells]
        fresh = (free > 0) & (now_us - free <= max_age_s * 1e6)
        return bool(np.all(fresh) and not np.any(room.blocked().reshape(-1)[cells]))
