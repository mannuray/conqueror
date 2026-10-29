"""Whole roams in a simulated room (AI engine LLD §18.10, spec 9 §7, Plan Z5): the map fills from what the
camera and the sonar would give, the skill drives, and the tank never comes near anything."""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field

from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.roam import Mode, RoamSkill
from conqueror_ai.domain.drive.sim import Box, SimRoom, SimTank
from conqueror_ai.domain.objects import GATE_DPS
from conqueror_ai.domain.room_map import RoomMap
from conqueror_ai.domain.sonar_model import SonarModel
from conqueror_ai.domain.types import Pose

NEAREST_M = 0.12  # the tank's centre never nearer than this to anything (ROBOT_R_M - 0.03)
ROOM = [
    Box(1.0, -0.6, 1.4, -0.2, 0.0, 0.08),  # a low box
    Box(-1.2, 0.8, -0.7, 1.3, 0.0, 0.40),  # a tall box
    Box(0.6, 1.0, 1.1, 1.5, 0.0, 0.45),  # a chair-sized box
    Box(-2.5, -2.5, 0.0, -1.2, 0.15, 0.30),  # a bed: its edge 15 cm up
]


@dataclass
class Roam:
    result: str | None = None
    nearest: float = math.inf
    front_hit: bool = False
    path_m: float = 0.0
    states: list[str] = field(default_factory=lambda: list[str]())
    room: RoomMap = field(default_factory=RoomMap)


def roam(
    mode: Mode,
    limit_s: float,
    boxes: list[Box] | None = None,
    seconds: float | None = None,
    find: bool = False,
    ball: tuple[float, float] | None = None,
    add: tuple[float, Box] | None = None,
    dt: float = 0.1,
    seed: int = 7,
) -> Roam:
    sim = SimRoom(list(ROOM if boxes is None else boxes), walls_m=2.5)
    out = Roam()
    sonar_model = SonarModel()
    tank = SimTank()
    skill = RoamSkill(Planner(Limits()), mode, seconds=seconds, find=find, rng=random.Random(seed))
    turning, last_seen = False, None
    t = 0.0
    while t < limit_s:
        if add is not None and t >= add[0] and add[1] not in sim.boxes:
            sim.boxes.append(add[1])
        at = tank.at
        t_us = round(t * 1e6)
        if not turning:  # the map skips pictures taken turning faster than 45 deg/s
            out.room.update_picture(sim.marks(at), t_us)
        pose = Pose(t_us, at[0], at[1], at[2], 0.0, 0.0)
        echo = sim.sonar(at)
        sonar_model.update(out.room, pose, echo, t_us)
        snap = out.room.snapshot(t_us, pose, False, True)
        fix = None
        if ball is not None:
            d = math.dist(ball, at[:2])
            in_view = abs(bearing_deg(at, ball)) <= 41.0 and d <= 4.0 and not turning
            if in_view and not any(
                b.z0 < 0.02
                and (h := b.ray(at[0], at[1], (ball[0] - at[0]) / d, (ball[1] - at[1]) / d)) is not None
                and h < d
                for b in sim.boxes
            ):
                last_seen = t
            fix = None if last_seen is None else TargetFix(1, ball[0], ball[1], t - last_seen)
        o = skill.tick(t, at, snap, fix, echo, True)
        turning = abs(o.cmd.w_dps) > GATE_DPS
        before = tank.at
        tank.step(o.cmd, dt)
        out.path_m += math.dist(before[:2], tank.at[:2])
        out.nearest = min(out.nearest, sim.gap_m(tank.at))
        out.front_hit = out.front_hit or sim.front_inside(tank.at)
        out.states.append(skill.state)
        if o.result is not None:
            out.result = o.result
            break
        t = round(t + dt, 6)
    return out


def test_wander_60_s_keeps_moving_never_close() -> None:
    r = roam("wander", 70.0, seconds=60.0)
    assert r.result == "done"
    assert r.nearest >= NEAREST_M and not r.front_hit
    assert r.path_m > 3.0


def test_explore_covers_80_percent() -> None:
    r = roam("explore", 400.0, dt=0.2)
    sim = SimRoom(list(ROOM), walls_m=2.5)
    assert r.result == "done"
    assert sim.seen_share(r.room) >= 0.8
    assert r.nearest >= NEAREST_M and not r.front_hit


def test_a_box_appears_on_the_route() -> None:  # Review Focus 4
    r = roam("wander", 40.0, seconds=30.0, add=(3.0, Box(0.9, -0.15, 1.1, 0.15, 0.0, 0.10)))
    assert r.result == "done"
    assert r.nearest >= NEAREST_M and not r.front_hit


def test_find_ball_out_of_sight() -> None:
    r = roam("wander", 90.0, find=True, ball=(-1.6, 0.2))  # behind the tank at the start
    assert r.result == "done" and "chase" in r.states
    assert r.nearest >= NEAREST_M and not r.front_hit
