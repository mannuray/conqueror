"""Commands, the ramp, and where a command takes the tank (AI engine LLD §17.4)."""

from __future__ import annotations

import math
from dataclasses import dataclass

from conqueror_ai.domain.drive.limits import Limits


@dataclass(frozen=True)
class Command:
    v_cms: float  # forward only (spec 5 Rev 3: no driving backwards)
    w_dps: float  # + = left (counter-clockwise), as the bridge's drive_rate


STOP = Command(0.0, 0.0)

XYTh = tuple[float, float, float]


def _toward(cur: float, want: float, step: float) -> float:
    return want if abs(want - cur) <= step else cur + math.copysign(step, want - cur)


def ramp(now: Command, want: Command, dt: float, lim: Limits) -> Command:
    """One tick toward the wanted command, never faster than the ramps.

    The tracks can't run between 0 and v_min, so starting and stopping cross that band in one step; a spin in
    place can't be slower than spin_dps, so a spin starts and ends in one step too."""
    if want.v_cms <= 0:
        v = 0.0 if now.v_cms - lim.dv_cms_per_s * dt < lim.v_min_cms else now.v_cms - lim.dv_cms_per_s * dt
    elif now.v_cms < lim.v_min_cms:
        v = lim.v_min_cms
    else:
        v = max(lim.v_min_cms, _toward(now.v_cms, want.v_cms, lim.dv_cms_per_s * dt))
    if v == 0:  # standing: a spin is full speed or none (slower spins aren't smooth)
        w = (
            math.copysign(lim.spin_dps, want.w_dps)
            if want.v_cms <= 0 and abs(want.w_dps) >= lim.spin_dps
            else 0.0
        )
    elif (
        now.v_cms == 0 and abs(now.w_dps) >= lim.spin_dps
    ):  # from a spin into a drive: the spin's rate is gone
        w = _toward(0.0, want.w_dps, lim.dw_dps_per_s * dt)
    else:
        w = _toward(now.w_dps, want.w_dps, lim.dw_dps_per_s * dt)
    return Command(v, w)


def roll_out(start: XYTh, now: Command, want: Command, lim: Limits) -> list[XYTh]:
    """Where the tank goes in the next horizon_s if it ramps from now toward want: one pose per tick."""
    x, y, th = start
    cmd = now
    out: list[XYTh] = []
    for _ in range(round(lim.horizon_s / lim.dt_s)):
        cmd = ramp(cmd, want, lim.dt_s, lim)
        v, w = cmd.v_cms / 100.0, math.radians(cmd.w_dps)
        th_mid = th + w * lim.dt_s / 2
        x += v * lim.dt_s * math.cos(th_mid)
        y += v * lim.dt_s * math.sin(th_mid)
        th += w * lim.dt_s
        out.append((x, y, th))
    return out
