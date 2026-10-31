"""Whole roams in a simulated room (AI engine LLD §18.10, spec 9 §7, Plan Z5): the map fills from what the
camera and the sonar would give, the skill drives, and the tank never comes near anything."""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field

import pytest

from conqueror_ai.domain.drive.chase import ChaseSkill
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.drive.roam import Mode, RoamSkill
from conqueror_ai.domain.drive.sim import Box, SimRoom, SimTank
from conqueror_ai.domain.objects import GATE_DPS
from conqueror_ai.domain.room_map import RoomMap
from conqueror_ai.domain.sonar_model import SonarModel
from conqueror_ai.domain.types import Pose

NEAREST_M = 0.12  # the tank's centre never nearer than this to anything (half-width 11.5 cm + a little)
BODY_M = 0.005  # and its body (front, back and sides, turning included) never touches anything (review C1)
LIM = Limits()
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
    body: float = math.inf  # the tank's body rectangle (Limits' footprint) to the nearest thing, every tick
    front_hit: bool = False
    path_m: float = 0.0
    states: list[str] = field(default_factory=lambda: list[str]())
    out_m: float = 0.0  # the body's furthest corner from the start (the play area, design-notes §174)
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
    start: tuple[float, float, float] = (0.0, 0.0, 0.0),
    area_m: float | None = None,
) -> Roam:
    sim = SimRoom(list(ROOM if boxes is None else boxes), walls_m=2.5)
    out = Roam()
    sonar_model = SonarModel()
    tank = SimTank(*start)
    skill = RoamSkill(
        Planner(Limits()), mode, seconds=seconds, find=find, rng=random.Random(seed), area_m=area_m
    )
    turning, last_seen, pan = False, None, 0.0
    t = 0.0
    while t < limit_s:
        if add is not None and t >= add[0] and add[1] not in sim.boxes:
            sim.boxes.append(add[1])
        at = tank.at
        t_us = round(t * 1e6)
        if not turning:  # the map skips pictures taken turning faster than 45 deg/s
            out.room.update_picture(sim.marks(at, pan_deg=pan), t_us)
        pose = Pose(t_us, at[0], at[1], at[2], 0.0, 0.0)
        echo = sim.sonar(at)
        sonar_model.update(out.room, pose, echo, t_us)
        out.room.body_over(pose, LIM.body_front_m, LIM.body_rear_m, LIM.body_half_w_m, t_us)  # Plan Z6
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
        if o.cam is not None:
            pan = o.cam[0]  # the camera turns where it's told (the real one looks where the tank is going)
        turning = abs(o.cmd.w_dps) > GATE_DPS
        before = tank.at
        tank.step(o.cmd, dt)
        out.path_m += math.dist(before[:2], tank.at[:2])
        out.nearest = min(out.nearest, sim.gap_m(tank.at))
        out.body = min(out.body, sim.body_gap(tank.at, LIM))
        out.front_hit = out.front_hit or sim.front_inside(tank.at)
        out.out_m = max(out.out_m, corners_from(tank.at, start))
        out.states.append(skill.state)
        if o.result is not None:
            out.result = o.result
            break
        t = round(t + dt, 6)
    return out


def corners_from(at: tuple[float, float, float], start: tuple[float, float, float]) -> float:
    """The body's furthest corner from the start (a circle holds the rectangle when it holds its corners)."""
    c, s = math.cos(at[2]), math.sin(at[2])
    return max(
        math.dist((at[0] + u * c - v * s, at[1] + u * s + v * c), start[:2])
        for u in (LIM.body_front_m, -LIM.body_rear_m)
        for v in (LIM.body_half_w_m, -LIM.body_half_w_m)
    )


@pytest.mark.parametrize("seed", [7, 3, 11])  # seed 3 coasted into a bed edge before the blocked-stop fix
def test_wander_60_s_keeps_moving_never_close(seed: int) -> None:
    r = roam("wander", 70.0, seconds=60.0, seed=seed)
    assert r.result == "done"
    assert r.nearest >= NEAREST_M and not r.front_hit and r.body >= BODY_M
    assert r.path_m > 3.0


def test_explore_covers_80_percent() -> None:
    r = roam("explore", 400.0, dt=0.2)
    sim = SimRoom(list(ROOM), walls_m=2.5)
    assert r.result == "done"
    assert sim.seen_share(r.room) >= 0.8
    assert r.nearest >= NEAREST_M and not r.front_hit and r.body >= BODY_M


def test_a_box_appears_on_the_route() -> None:  # Review Focus 4
    r = roam("wander", 40.0, seconds=30.0, add=(3.0, Box(0.9, -0.15, 1.1, 0.15, 0.0, 0.10)))
    assert r.result == "done"
    assert r.nearest >= NEAREST_M and not r.front_hit and r.body >= BODY_M


@pytest.mark.parametrize("seed", [1, 2, 3, 4, 7])
def test_find_ball_out_of_sight(seed: int) -> None:
    """Behind the tank at the start. Wander is random: five paths, each finds it within 120 s (owner,
    design-notes §172: one path at 90 s was luck - over 10 paths the median sight was 26 s, seed 7 118 s)."""
    r = roam("wander", 120.0, find=True, ball=(-1.6, 0.2), seed=seed)
    assert r.result == "done" and "chase" in r.states
    assert r.nearest >= NEAREST_M and not r.front_hit and r.body >= BODY_M


def chase_on_map(
    ball: tuple[float, float], boxes: list[Box], limit_s: float = 60.0, dt: float = 0.1, use_map: bool = True
) -> Roam:
    """Slice 1's chase with the room map (Plan Z5 Task 7): the ball always known (seen over a low box)."""
    sim = SimRoom(boxes, walls_m=2.5)
    out = Roam()
    skill = ChaseSkill(Planner(Limits()))
    tank, turning, t = SimTank(), False, 0.0
    while t < limit_s:
        at, t_us = tank.at, round(t * 1e6)
        if not turning:
            out.room.update_picture(sim.marks(at), t_us)
        pose = Pose(t_us, at[0], at[1], at[2], 0.0, 0.0)
        out.room.body_over(pose, LIM.body_front_m, LIM.body_rear_m, LIM.body_half_w_m, t_us)  # Plan Z6
        snap = out.room.snapshot(t_us, pose, False, True) if use_map else None
        echo = sim.sonar(at)
        o = skill.tick(t, at, TargetFix(1, ball[0], ball[1], 0.0), echo, True, snap=snap)
        turning = abs(o.cmd.w_dps) > GATE_DPS
        tank.step(o.cmd, dt)
        out.nearest = min(out.nearest, sim.gap_m(tank.at))
        out.body = min(out.body, sim.body_gap(tank.at, LIM))
        out.front_hit = out.front_hit or sim.front_inside(tank.at)
        if o.result is not None:
            out.result = o.result
            break
        t = round(t + dt, 6)
    return out


def test_chase_goes_around_a_box() -> None:
    box = Box(0.8, -0.25, 1.1, 0.25, 0.0, 0.08)  # low: the camera sees the ball over it; the sonar hears it
    r = chase_on_map((2.0, 0.0), [box])
    assert r.result == "done"
    assert r.nearest >= NEAREST_M and not r.front_hit and r.body >= BODY_M


def test_find_ball_behind_a_box() -> None:
    r = roam("wander", 120.0, find=True, ball=(1.9, 0.0), boxes=[Box(0.9, -0.3, 1.3, 0.3, 0.0, 0.30)])
    assert r.result == "done" and "chase" in r.states
    assert r.nearest >= NEAREST_M and not r.front_hit and r.body >= BODY_M


# ---- final review (Plan Z5): the body, turning on the spot, a ball by a sofa, doorways
@pytest.mark.parametrize(
    ("gap", "th"),  # centre this far from a wall, facing along it
    [
        # the wall beside it unseen (the camera looks along it): a turn there came 4.6 mm off it until turns
        # swept only seen floor (owner: option b, design-notes §176)
        (0.15, math.pi / 2),
        (0.15, -math.pi / 2),
        (0.18, math.pi / 2),
        (0.18, -math.pi / 2),
    ],
)
def test_no_turn_on_the_spot_scrapes_a_wall(gap: float, th: float) -> None:
    """Review C1: the corners sweep the half-diagonal (~16-18 cm) when turning on the spot; the map kept the
    centre only 12-15 cm off, and spins were never checked against it."""
    r = roam("wander", 25.0, boxes=[], seconds=20.0, start=(2.5 - gap, 0.0, th))
    assert r.body >= BODY_M and not r.front_hit


@pytest.mark.parametrize("off", [0.06, 0.15])
def test_a_ball_by_a_sofa_is_reached(off: float) -> None:
    """Review I2: a ball 6 cm from a sofa's side ended blocked on the map (slice 1 reached it)."""
    sofa = Box(1.5 + off, -0.8, 2.0, 0.8, 0.0, 0.4)
    r = chase_on_map((1.5, 0.0), [sofa])
    assert r.result == "done" and r.body >= BODY_M


@pytest.mark.parametrize(
    "door",
    [
        0.46,  # failed (design-notes §168) until the body's gaps were measured exactly (§171)
        0.50,
        0.60,
    ],
)
def test_explore_goes_through_a_doorway(door: float) -> None:
    """Review I3: unseen space grown round seen marks closed doorways, and explore said done at 74 %."""
    lo, hi = 0.8 - door / 2, 0.8 + door / 2
    walls = [Box(1.0, -2.5, 1.1, lo, 0.0, 0.6), Box(1.0, hi, 1.1, 2.5, 0.0, 0.6)]
    r = roam("explore", 900.0, boxes=walls, dt=0.2)
    assert r.result == "done" and SimRoom(walls, walls_m=2.5).seen_share(r.room) >= 0.8
    assert r.body >= BODY_M


# ---- the floor runs' layouts (Plan Z6, design-notes §164-§165.2)
@pytest.mark.parametrize(
    "small",
    [
        Box(0.37, 0.12, 0.47, 0.17, 0.0, 0.06),  # run 3: 40 cm ahead-left
        Box(0.33, -0.05, 0.43, 0.05, 0.0, 0.06),  # 35 cm ahead
        Box(0.40, -0.20, 0.50, -0.12, 0.0, 0.06),  # 45 cm ahead-right
    ],
)
def test_run_3_layout_a_small_box_near_the_start(small: Box) -> None:
    """Floor run 3 stood 12 s: a small mark near the tank, unseen floor round it. Explore goes on."""
    r = roam("explore", 300.0, boxes=[small], dt=0.2)
    assert r.result == "done" and SimRoom([small], walls_m=2.5).seen_share(r.room) >= 0.8
    assert r.body >= BODY_M and not r.front_hit


def test_run_2_layout_beside_a_bed() -> None:
    """Floor run 2: a bed's panel (15 cm up) along the right, its leg ahead-right, the way on behind."""
    bed = [Box(-1.5, -1.2, 0.30, -0.25, 0.15, 0.5), Box(0.20, -0.32, 0.30, -0.22, 0.0, 0.15)]
    r = roam("explore", 300.0, boxes=bed, dt=0.2, start=(0.0, 0.0, 0.0))
    assert r.result == "done" and r.body >= BODY_M and not r.front_hit


# ---- the play area (owner: only when asked, design-notes §174)
@pytest.mark.parametrize("seed", [7, 3])
def test_wander_in_a_play_area_never_leaves_it(seed: int) -> None:
    r = roam("wander", 70.0, seconds=60.0, seed=seed, area_m=1.0)
    assert r.result == "done" and r.path_m > 2.0
    assert r.out_m <= 1.0 and r.body >= BODY_M and not r.front_hit


def test_explore_in_a_play_area_is_done_inside_it() -> None:
    r = roam("explore", 300.0, dt=0.2, area_m=1.0)
    assert r.result == "done" and r.out_m <= 1.0
    assert SimRoom(list(ROOM), walls_m=2.5).seen_share(r.room) < 0.8  # the far side of the room left alone


def test_find_a_ball_outside_the_play_area_ends_blocked() -> None:
    r = roam("wander", 120.0, find=True, ball=(-1.6, 0.2), seed=1, area_m=1.0)
    assert r.result == "blocked" and r.out_m <= 1.0


def test_goto_a_ball_behind_at_the_start_curves_round() -> None:
    """The chase's search turn at the start sweeps floor never seen: refused (§176), it curves round instead,
    finds the ball behind and reaches it (slice 1's floor check "behind", design-notes §157)."""
    sim, ball = SimRoom([], walls_m=2.5), (-1.5, 0.0)
    room, skill, tank, turning, t = RoomMap(), ChaseSkill(Planner(Limits())), SimTank(), False, 0.0
    last_seen: float | None = None
    body, result = math.inf, None
    while t < 60.0:
        at, t_us = tank.at, round(t * 1e6)
        if not turning:
            room.update_picture(sim.marks(at), t_us)
        pose = Pose(t_us, at[0], at[1], at[2], 0.0, 0.0)
        room.body_over(pose, LIM.body_front_m, LIM.body_rear_m, LIM.body_half_w_m, t_us)
        if abs(bearing_deg(at, ball)) <= 41.0 and not turning:
            last_seen = t
        fix = None if last_seen is None else TargetFix(1, ball[0], ball[1], t - last_seen)
        o = skill.tick(t, at, fix, sim.sonar(at), True, snap=room.snapshot(t_us, pose, False, True))
        turning = abs(o.cmd.w_dps) > GATE_DPS
        tank.step(o.cmd, 0.1)
        body = min(body, sim.body_gap(tank.at, LIM))
        if o.result is not None:
            result = o.result
            break
        t = round(t + 0.1, 6)
    assert result == "done" and body >= BODY_M
