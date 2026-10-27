"""Whole chases on a simulated tank (AI engine LLD §17.9, spec 8 §6): one smooth move, arriving at
the ball."""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import pytest

from conqueror_ai.domain.drive.chase import ChaseSkill, SkillOut
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.sim import SimTank
from conqueror_ai.domain.objects import GATE_DPS

HALF_VIEW_DEG = 41.0  # the camera sees ~82 deg across
SEES_UP_TO_M = 4.0  # the ball is found up to about this far


@dataclass
class Run:
    outs: list[SkillOut] = field(default_factory=lambda: list[SkillOut]())
    path: list[tuple[float, float, float]] = field(default_factory=lambda: list[tuple[float, float, float]]())
    result: str | None = None


def chase(
    ball: tuple[float, float],
    th_deg: float = 0.0,
    moves: dict[float, tuple[float, float]] | None = None,
    seconds: float = 40.0,
    dt: float = 0.1,
) -> Run:
    lim = Limits()
    skill = ChaseSkill(Planner(lim))
    tank = SimTank(th=math.radians(th_deg))
    pan = 0.0
    turning = False
    last_seen: float | None = None
    run = Run()
    t = 0.0
    while t < seconds:
        for at_t, where in (moves or {}).items():
            if abs(t - at_t) < 1e-6:
                ball = where
        b = bearing_deg(tank.at, ball)
        near = math.hypot(ball[0] - tank.at[0], ball[1] - tank.at[1]) <= SEES_UP_TO_M
        if (
            abs(b - (-pan)) <= HALF_VIEW_DEG and near and not turning
        ):  # the map drops pictures taken turning fast
            last_seen = t
        fix = None if last_seen is None else TargetFix(1, ball[0], ball[1], t - last_seen)
        out = skill.tick(t, tank.at, fix, None, True)
        if out.cam is not None:
            pan = out.cam[0]
        turning = abs(out.cmd.w_dps) > GATE_DPS
        run.outs.append(out)
        run.path.append(tank.step(out.cmd, dt))
        if out.result is not None:
            run.result = out.result
            break
        t = round(t + dt, 6)
    return run


def moving_stops(outs: list[SkillOut]) -> int:
    """How often the tank stood still (v = 0, not spinning) between its first drive forward and the end. (The
    search before may stop: after half a turn on the spot the tank stands to sweep the camera.)"""
    started = False
    stops = 0
    for o in outs[:-1]:
        moving = o.cmd.v_cms > 0 or o.cmd.w_dps != 0
        if o.cmd.v_cms > 0:
            started = True
        elif started and not moving and o.result is None:
            stops += 1
    return stops


@pytest.mark.parametrize(
    ("ball", "th_deg"),
    [((1.6, 0.0), 0.0), ((1.2, 1.2), 0.0), ((0.0, -1.5), 0.0), ((-1.5, 0.0), 0.0), ((0.685, -0.5), 0.0)],
    ids=["ahead", "45-left", "90-right", "behind", "near-40-right"],  # near-40-right: floor chase 5
)
@pytest.mark.parametrize("dt", [0.1, 0.2], ids=["10Hz", "5Hz"])  # the real loop ticks once per picture
def test_a_chase_reaches_the_ball_in_one_smooth_move(
    ball: tuple[float, float], th_deg: float, dt: float
) -> None:
    run = chase(ball, th_deg, dt=dt)
    lim = Limits()
    assert run.result == "done"
    x, y, th = run.path[-1]
    d_lens = math.hypot(ball[0] - x, ball[1] - y) - lim.cam_x_m
    assert abs(d_lens - 0.40) <= 0.15
    assert abs(bearing_deg((x, y, th), ball)) <= 10.0
    assert moving_stops(run.outs) == 0
    for a, b in zip(run.outs, run.outs[1:], strict=False):
        assert abs(b.cmd.w_dps - a.cmd.w_dps) <= lim.dw_dps_per_s * dt + 1e-6 or (
            a.cmd.v_cms == 0 or b.cmd.v_cms == 0
        )


def test_a_ball_rolled_away_mid_way_is_reached_where_it_went_without_a_stop() -> None:
    run = chase((2.0, 0.0), moves={3.0: (2.0, 0.8)})
    assert run.result == "done"
    x, y, _ = run.path[-1]
    assert math.hypot(2.0 - x, 0.8 - y) - Limits().cam_x_m <= 0.55
    assert moving_stops(run.outs) == 0


def test_no_ball_anywhere_ends_not_found() -> None:
    run = chase((50.0, 50.0), seconds=30.0)  # too far away to be seen
    assert run.result == "not_found"


@pytest.mark.parametrize("ball", [(1.2, 1.2), (-1.5, 0.0), (0.0, -1.5)])
def test_commands_stay_inside_what_the_tank_can_do(ball: tuple[float, float]) -> None:
    lim = Limits()
    run = chase(ball)
    for o in run.outs:
        v, w = o.cmd.v_cms, o.cmd.w_dps
        assert v == 0 or v >= lim.v_min_cms - 1e-9
        assert v <= lim.v_cap_cms + 1e-9
        if v == 0 and w != 0:
            assert abs(w) == pytest.approx(lim.spin_dps)
        if v > 0:  # the inner track never below its slowest speed: no arc tighter than r_min
            assert v - abs(math.radians(w)) * lim.track_m / 2 * 100.0 >= lim.inner_min_cms - 1e-6
