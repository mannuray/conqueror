"""Trusted floor (AI engine LLD §28.4, Spec 10 S7): a game backs only over floor the map saw free lately, from
near, with no low / high mark since - the bridge's back_ok question (R34.2) and ESCAPE's backing."""

from __future__ import annotations

import math

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.drive.curves import XYTh
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap

MAX_AGE_S = 5.0  # first guess from the map settings, to check on the floor
STEP_M = 0.02  # the swept strip sampled ~2 cm apart (cells are 10 cm)
COAST_M = 0.05  # the roll after the stop at v_min: the strip is this much longer (final review M4)


class TrustedFloor:
    @staticmethod
    def ok(
        room: RoomMap | MapSnapshot,
        pose: XYTh,
        back_m: float,
        now_us: int,
        max_age_s: float = MAX_AGE_S,
        lim: Limits | None = None,
    ) -> bool:
        """Every cell the body's back sweeps over backing `back_m` straight (behind its rear edge, its whole
        width) was seen free within `max_age_s` and is not blocked. A snapshot without free times: never."""
        if back_m <= 0.0:
            return True
        if isinstance(room, RoomMap):
            free_us, blocked = room.free_us, room.blocked()
        elif room.free_us is not None:
            free_us, blocked = room.free_us, room.blocked
        else:
            return False
        cells = _strip(pose, back_m, lim or Limits(), room.cell_m, free_us.shape[0])
        if cells is None:
            return False  # part of the path is off the map
        free = free_us.reshape(-1)[cells]
        # a time ahead of now is from before a bridge restart (its clock began again): not fresh (review I2)
        fresh = (free > 0) & (free <= now_us) & (now_us - free <= max_age_s * 1e6)
        return bool(np.all(fresh) and not np.any(blocked.reshape(-1)[cells]))


def _strip(pose: XYTh, back_m: float, lim: Limits, cell_m: float, n: int) -> NDArray[np.intp] | None:
    """The flat cells of the strip behind the rear edge, `back_m` long, the body's width; None off the map."""
    x, y, th = pose
    us = np.arange(-(lim.body_rear_m + back_m + COAST_M), -lim.body_rear_m + 1e-9, STEP_M)
    vs = np.linspace(-lim.body_half_w_m, lim.body_half_w_m, 1 + math.ceil(2 * lim.body_half_w_m / STEP_M))
    gu, gv = np.meshgrid(us, vs)
    c, s = math.cos(th), math.sin(th)
    xy = np.stack([x + gu.ravel() * c - gv.ravel() * s, y + gu.ravel() * s + gv.ravel() * c], 1)
    ij = np.floor((xy + n * cell_m / 2) / cell_m).astype(np.intp)
    if not np.all((ij >= 0) & (ij < n)):
        return None
    return np.unique(ij[:, 0] * n + ij[:, 1])
