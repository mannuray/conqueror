"""The game skills FACE, ESCAPE and FOLLOW in a simulated room (AI engine LLD §28.3, Plan G2 Task 3).

The person is a point on the floor; the estimate is made geometrically from the tank's front edge, seen only
inside the camera's view and never while the tank spins faster than the map's gate (as World: no picture is
used then). The map is fed with where the tank was (the trusted floor's 1.5 m), never while spinning."""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
import pytest

from conqueror_ai.domain.drive.curves import Command, XYTh
from conqueror_ai.domain.drive.game_skills import (
    ESCAPE_NEAR_M,
    FACE_LOOK_PAN_DEG,
    FACE_MIN_S,
    FACE_PAN_DEG,
    FOLLOW_MIN_M,
    EscapeSkill,
    FaceSkill,
    FollowSkill,
)
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.roam import POINT_DONE_M
from conqueror_ai.domain.drive.sim import Box, SimRoom, SimTank
from conqueror_ai.domain.drive.skill import Result, Skill, make_input
from conqueror_ai.domain.drive.trusted import TrustedFloor
from conqueror_ai.domain.objects import GATE_DPS
from conqueror_ai.domain.person import PersonEst
from conqueror_ai.domain.room_map import RoomMap
from conqueror_ai.domain.types import Pose
from tests.test_room_map import marks

LIM = Limits()
FRONT_M = LIM.body_front_m
VIEW_DEG = 41.0
T0_S = 1.0  # the sim clock starts here: 0 is the map's "never"


@dataclass
class Play:
    result: Result | None = None
    cmds: list[Command] = field(default_factory=lambda: list[Command]())
    cams: list[tuple[float, float]] = field(default_factory=lambda: list[tuple[float, float]]())
    at: list[XYTh] = field(default_factory=lambda: list[XYTh]())
    gaps: list[float] = field(default_factory=lambda: list[float]())  # the front edge to the person, true
    room: RoomMap = field(default_factory=RoomMap)
    goals: list[float] = field(default_factory=lambda: list[float]())  # FOLLOW's goal: front to the person
    in_view: list[bool] = field(default_factory=lambda: list[bool]())  # the person in the camera's view
    unseen_turning: list[float] = field(default_factory=lambda: list[float]())  # s unseen, per turn

    @property
    def moved(self) -> bool:
        return any(c.v_cms != 0 or c.w_dps != 0 for c in self.cmds)


def front_gap(at: XYTh, p: tuple[float, float]) -> tuple[float, float]:
    """The person from the front edge's middle: distance (m) and bearing (deg, + left, tank frame)."""
    x, y, th = at
    fx, fy = x + FRONT_M * math.cos(th), y + FRONT_M * math.sin(th)
    b = math.degrees(math.atan2(p[1] - fy, p[0] - fx) - th)
    return math.dist((fx, fy), p), (b + 180.0) % 360.0 - 180.0


def seed_behind(room: RoomMap, at: XYTh, t_us: int) -> None:
    """The floor behind the tank seen free from where it stands (it looked back a moment ago)."""
    x, y, th = at
    pts = [
        (x + u * math.cos(th) - v * math.sin(th), y + u * math.sin(th) + v * math.cos(th))
        for u in np.arange(-0.7, 0.0, 0.02).tolist()
        for v in np.arange(-0.25, 0.26, 0.02).tolist()
    ]
    room.update_picture(marks(floor=pts), t_us, (x, y))


def play(
    skill: Skill,
    person: tuple[float, float] | None,
    limit_s: float = 30.0,
    start: XYTh = (0.0, 0.0, 0.0),
    boxes: list[Box] | None = None,
    trusted_behind: bool = False,
    hide_from_s: float | None = None,
    pan: float = 0.0,
    edge_clip: bool = False,
    always_edge: bool = False,
    known_bearing: float | None = None,
    known_d: float = 1.0,
    blind_spin: bool = False,
    blind_driving: bool = False,
    delay_s: float = 0.0,
    hide_after_turn_s: float | None = None,
    walk: tuple[float, float] = (0.0, 0.0),
) -> Play:
    """edge_clip: a person up to 35 deg beyond the view is seen cut by the picture's side edge (their box's
    bottom centre at the edge, so the bearing reads short); always_edge: every sighting says edge; walk: the
    person's speed (m/s, x and y)."""
    sim = SimRoom(boxes or [], walls_m=2.5)
    tank = SimTank(*start)
    out = Play()
    last: PersonEst | None = (
        None if known_bearing is None else PersonEst(False, known_d, known_bearing, 0, 0.5, 0)
    )
    cmd = Command(0.0, 0.0)
    t = T0_S
    seen_t = T0_S
    made: list[tuple[float, PersonEst | None]] = []  # every tick's estimate, handed over delay_s later
    turned_t: float | None = None  # when the first turn began
    if trusted_behind:
        seed_behind(out.room, start, round(t * 1e6))
    while t < T0_S + limit_s:
        at = tank.at
        t_us = round(t * 1e6)
        turning = abs(cmd.w_dps) > GATE_DPS
        pose = Pose(t_us, at[0], at[1], at[2], 0.0, 0.0)
        if not turning:
            out.room.update_picture(sim.marks(at, pan_deg=pan), t_us, (at[0], at[1]))
        out.room.body_over(pose, LIM.body_front_m, LIM.body_rear_m, LIM.body_half_w_m, t_us)
        if person is not None:
            person = (person[0] + walk[0] * LIM.dt_s, person[1] + walk[1] * LIM.dt_s) if t > T0_S else person
            d, b = front_gap(at, person)
            out.gaps.append(d)
            hidden = (hide_from_s is not None and t >= hide_from_s) or (
                hide_after_turn_s is not None and turned_t is not None and t >= turned_t + hide_after_turn_s
            )
            hidden = hidden or (blind_spin and turning)  # the tank's pictures blur above GATE_DPS (§241)
            hidden = hidden or (blind_driving and cmd.v_cms != 0)  # lost from view on the leg (§241)
            in_view = abs(b + pan) <= VIEW_DEG and not hidden  # turning pictures count (design-notes §229)
            at_edge = edge_clip and not hidden and VIEW_DEG < abs(b + pan) <= VIEW_DEG + 35.0
            out.in_view.append(in_view)
            if in_view:
                last = PersonEst(True, d, b, 0, 0.9, t_us, 1 if always_edge else 0)
            elif at_edge:
                side = math.copysign(1.0, b + pan)
                last = PersonEst(True, d, side * (VIEW_DEG - 10.0) - pan, 0, 0.9, t_us, round(side))
            elif last is not None and last.seen:
                last = PersonEst(False, last.d_m, last.bearing_deg, 0, 0.5, last.t_us)
        snap = out.room.snapshot(t_us, pose, False, True)
        made.append((t, last))
        given = next((e for m_t, e in reversed(made) if m_t <= t - delay_s + 1e-9), made[0][1])  # the latency
        if given is not None and given.seen and given.t_us > 0:
            seen_t = given.t_us / 1e6  # the picture the bridge last saw them in
        inp = make_input(at=at, snap=snap, map_ok=True, sonar_m=sim.sonar(at), person=given)
        o = skill.tick(t, inp)
        goal = getattr(skill, "_goal", None)
        if isinstance(goal, tuple) and person is not None:
            out.goals.append(math.dist(goal, person) - FRONT_M)  # type: ignore[arg-type]
        cmd = o.cmd
        if cmd.w_dps != 0:
            turned_t = t if turned_t is None else turned_t
            out.unseen_turning.append(t - seen_t)
        out.cmds.append(cmd)
        out.at.append(at)
        if o.cam is not None:
            pan = o.cam[0]
            out.cams.append(o.cam)
        if o.result is not None:
            out.result = o.result
            break
        tank.step(cmd, LIM.dt_s)
        t += LIM.dt_s
    return out


def at_bearing(d: float, b_deg: float, at: XYTh = (0.0, 0.0, 0.0)) -> tuple[float, float]:
    """A floor point d from the front edge at the bearing (+ left)."""
    x, y, th = at
    fx, fy = x + FRONT_M * math.cos(th), y + FRONT_M * math.sin(th)
    a = th + math.radians(b_deg)
    return fx + d * math.cos(a), fy + d * math.sin(a)


def heading_deg(at: XYTh) -> float:
    return (math.degrees(at[2]) + 180.0) % 360.0 - 180.0


# ---- FACE
def test_face_at_20_deg_pans_and_does_not_drive() -> None:
    p = play(FaceSkill(LIM), at_bearing(1.0, 20.0))
    assert p.result == Result.DONE and not p.moved
    assert p.cams and p.cams[-1][0] == pytest.approx(-20.0, abs=2.0)  # pan + = right: the person is left


def test_face_already_facing_lasts_half_a_second() -> (
    None
):  # design-notes §251: a tight loop flooded the bridge
    p = play(FaceSkill(LIM), at_bearing(0.5, 0.0))
    assert p.result == Result.DONE and not p.moved
    assert len(p.cmds) * LIM.dt_s >= FACE_MIN_S - 1e-9  # not done at once
    assert len(p.cmds) * LIM.dt_s <= FACE_MIN_S + 2 * LIM.dt_s


@pytest.mark.parametrize("side", [1.0, -1.0])
def test_face_at_60_deg_turns_towards_them_until_within_10(side: float) -> None:
    person = at_bearing(1.0, 60.0 * side)
    p = play(FaceSkill(LIM), person, pan=-35.0 * side)  # the camera already their way (60 deg is out of view)
    assert p.result == Result.DONE
    spins = [c.w_dps for c in p.cmds if c.w_dps != 0]
    assert spins and all(math.copysign(1.0, w) == side for w in spins)  # left for + (counter-clockwise)
    assert all(c.v_cms == 0 for c in p.cmds)
    tank = SimTank(*p.at[-1])
    end = tank.step(p.cmds[-1], LIM.dt_s)
    assert abs(front_gap(end, person)[1]) <= 10.0 + 6.5  # within 10 deg (+ one tick's spin)


def test_face_close_beyond_the_pan_only_pans() -> None:  # R34.14: no turn on the spot nearer than 15 cm
    p = play(FaceSkill(LIM), at_bearing(0.12, 60.0), pan=-35.0)
    assert p.result == Result.DONE and not p.moved
    assert p.cams[-1][0] == pytest.approx(-35.0, abs=0.5)


def test_face_never_seen_sweeps_the_camera_then_is_lost_without_moving() -> None:  # §248
    p = play(FaceSkill(LIM), at_bearing(1.0, 150.0), limit_s=5.0)  # behind: never in view, nothing last seen
    assert p.result == Result.LOST and not p.moved
    assert [c[0] for c in p.cams] == [0.0, -FACE_LOOK_PAN_DEG, FACE_LOOK_PAN_DEG]  # ahead, left, right
    assert len(p.cmds) * LIM.dt_s <= 3.0  # inside the bridge's 3 s not-found sweep (R34.14)


def test_face_after_a_long_turn_does_not_end_lost_on_arrival() -> None:
    p = play(FaceSkill(LIM), at_bearing(1.2, 100.0), pan=-60.0)
    assert p.result == Result.DONE  # ~90 deg at 65 deg/s is ~1.4 s unseen while spinning: not lost


@pytest.mark.parametrize("side", [1.0, -1.0])
def test_face_on_a_side_edge_sighting_turns_the_tank(side: float) -> None:  # design-notes §236
    person = at_bearing(1.0, 70.0 * side)  # seen cut by the edge: it reads ~31 deg
    p = play(FaceSkill(LIM), person, edge_clip=True)
    assert p.result == Result.DONE and p.moved
    assert all(c.v_cms == 0 for c in p.cmds)
    tank = SimTank(*p.at[-1])
    end = tank.step(p.cmds[-1], LIM.dt_s)
    assert abs(front_gap(end, person)[1]) <= FACE_PAN_DEG  # the camera's pan reaches them now


def test_face_on_a_side_edge_sighting_close_only_pans() -> None:  # R34.14: no turn nearer than 15 cm
    p = play(FaceSkill(LIM), at_bearing(0.12, 70.0), edge_clip=True)
    assert p.result == Result.DONE and not p.moved


def test_face_gives_up_turning_after_three_looks() -> None:
    p = play(FaceSkill(LIM), at_bearing(1.0, 60.0), pan=-35.0, always_edge=True, limit_s=60.0)
    assert p.result == Result.DONE
    starts = sum(1 for a, b in zip(p.cmds, p.cmds[1:], strict=False) if a.w_dps == 0 and b.w_dps != 0)
    assert starts <= 3


@pytest.mark.parametrize("side", [1.0, -1.0])
def test_face_looks_toward_the_last_side_seen(side: float) -> None:  # design-notes §237
    person = at_bearing(1.0, 70.0 * side)  # out of view at pan 0, last seen there
    p = play(FaceSkill(LIM), person, known_bearing=70.0 * side)
    assert p.cams and math.copysign(1.0, -p.cams[0][0]) == side  # the camera that way first (pan + = right)
    assert p.result == Result.DONE and p.moved
    tank = SimTank(*p.at[-1])
    end = tank.step(p.cmds[-1], LIM.dt_s)
    assert abs(front_gap(end, person)[1]) <= FACE_PAN_DEG


def test_face_with_the_camera_aside_looks_straight_ahead_and_finds_them() -> None:  # design-notes §242
    p = play(FaceSkill(LIM), at_bearing(1.0, 0.0), pan=-50.0, limit_s=6.0)  # the camera 50 deg aside
    assert p.result == Result.DONE and not p.moved
    assert p.cams and p.cams[0][0] == pytest.approx(0.0, abs=1e-9)  # looked straight ahead first


def test_face_last_seen_near_sweeps_and_never_turns() -> None:  # §248: maybe right at the tank
    p = play(FaceSkill(LIM), None, known_bearing=70.0, known_d=0.29, limit_s=6.0)
    assert p.result == Result.LOST and not p.moved
    assert len(p.cams) == 3


@pytest.mark.parametrize("side", [1.0, -1.0])
def test_face_last_seen_far_steps_round_and_finds_them_behind(side: float) -> None:  # §248
    person = at_bearing(1.0, 160.0 * side)
    p = play(FaceSkill(LIM), person, known_bearing=100.0 * side, known_d=0.30, limit_s=30.0)
    assert p.result == Result.DONE and p.moved
    assert all(c.v_cms == 0 for c in p.cmds)  # on the spot only
    spins = [c.w_dps for c in p.cmds if c.w_dps != 0]
    assert all(math.copysign(1.0, w) == side for w in spins)  # toward the side last seen


def test_face_last_seen_far_and_nobody_turns_a_full_circle_then_is_lost() -> None:
    p = play(FaceSkill(LIM), None, known_bearing=70.0, known_d=1.0, limit_s=40.0)
    assert p.result == Result.LOST and p.moved
    turned = sum(abs(c.w_dps) * LIM.dt_s for c in p.cmds)
    assert 330.0 <= turned <= 420.0


# ---- ESCAPE
def test_escape_with_trusted_floor_backs_straight_and_never_turns() -> None:
    p = play(EscapeSkill(LIM, 0.30, TrustedFloor()), at_bearing(0.6, 0.0), trusted_behind=True)
    assert p.result == Result.DONE
    assert all(c.w_dps == 0 for c in p.cmds)
    assert any(c.v_cms < 0 for c in p.cmds)
    x_end = p.at[-1][0]
    assert -0.36 <= x_end <= -0.27  # back 30 cm by the odometry (one tick's slack)


def test_escape_untrusted_far_turns_drives_and_turns_back() -> None:
    person = at_bearing(0.9, 0.0)
    p = play(EscapeSkill(LIM, 0.30, TrustedFloor()), person, limit_s=40.0)
    assert p.result == Result.DONE, p.result
    assert not any(c.v_cms < 0 for c in p.cmds)  # never backs over untrusted floor
    assert any(abs(heading_deg(a)) > 75.0 for a in p.at)  # turned to the side
    assert all(abs(heading_deg(a)) <= 110.0 for a in p.at)  # never its back to them (design-notes §229)
    assert all(p.in_view)  # the person in view all the while: the bridge's hold-then-look never bites
    end = p.at[-1]
    assert abs(end[1]) >= 0.30  # stepped out of their way, sideways (design-notes §229)
    assert math.dist(end[:2], person) > math.dist((0.0, 0.0), person)  # and farther
    assert abs(front_gap(end, person)[1]) <= 15.0  # facing them again


def test_escape_turns_in_steps_and_sees_the_person_between_them() -> None:  # design-notes §241
    person = at_bearing(1.0, 0.0)
    p = play(EscapeSkill(LIM, 0.30, TrustedFloor()), person, limit_s=60.0, blind_spin=True)
    assert p.result == Result.DONE, p.result
    assert any(abs(heading_deg(a)) > 75.0 for a in p.at)  # still turned to the side
    assert p.unseen_turning and max(p.unseen_turning) < 1.0  # the bridge's unseen 1 s never bites a turn
    assert abs(p.at[-1][1]) >= 0.30 and abs(front_gap(p.at[-1], person)[1]) <= 15.0


def test_escape_turns_back_only_from_a_fresh_sighting() -> None:  # design-notes §241, second run
    person = at_bearing(1.0, 0.0)
    p = play(
        EscapeSkill(LIM, 0.30, TrustedFloor()),
        person,
        limit_s=60.0,
        blind_spin=True,
        blind_driving=True,
        delay_s=0.2,
    )
    assert p.result == Result.DONE, p.result
    assert p.unseen_turning and max(p.unseen_turning) < 1.0  # waited to see them before turning back
    assert abs(front_gap(p.at[-1], person)[1]) <= 15.0


def test_escape_step_without_a_sighting_is_lost() -> None:  # design-notes §241
    esc = EscapeSkill(LIM, 0.30, TrustedFloor())
    p = play(esc, at_bearing(1.0, 0.0), limit_s=20.0, hide_after_turn_s=0.1)
    assert p.result == Result.LOST
    assert p.cmds[-1].w_dps == 0 and p.cmds[-1].v_cms == 0
    assert all(c.v_cms == 0 for c in p.cmds)  # never drove the leg
    assert max(abs(heading_deg(a)) for a in p.at) < 50.0  # stopped after the first step


def test_escape_untrusted_and_near_stays_no_way() -> None:  # S11: never the back to a near person
    p = play(EscapeSkill(LIM, 0.30, TrustedFloor()), at_bearing(ESCAPE_NEAR_M - 0.2, 0.0))
    assert p.result == Result.NO_WAY and not p.moved


def test_escape_unseen_never_backs() -> None:  # Review Focus 5
    p = play(EscapeSkill(LIM, 0.30, TrustedFloor()), at_bearing(0.6, 150.0), trusted_behind=True)
    assert p.result == Result.NO_WAY and not p.moved
    q = play(EscapeSkill(LIM, 0.30, TrustedFloor()), None, trusted_behind=True)
    assert q.result == Result.NO_WAY and not q.moved


# ---- FOLLOW
def test_follow_from_2_m_stops_by_the_follow_distance() -> None:
    person = (2.0 + FRONT_M, 0.0)
    p = play(FollowSkill(LIM, 20.0, 1.0), person, limit_s=25.0)
    assert p.result == Result.DONE
    assert min(p.gaps) >= FOLLOW_MIN_M
    assert p.gaps[-1] <= 1.0 + POINT_DONE_M  # stopped near the follow distance (setxy's tolerance)
    assert p.goals and min(p.goals) >= 1.0 - 1e-6  # the goal is the follow distance short of them
    assert max(c.v_cms for c in p.cmds) <= 25.0 + 1e-9
    assert p.cmds[-1].v_cms == 0


def test_follow_never_nearer_than_50_cm_whatever_the_setting() -> None:
    person = (1.5 + FRONT_M, 0.3)
    p = play(FollowSkill(LIM, 20.0, 0.3), person, limit_s=25.0)
    assert p.result == Result.DONE
    assert min(p.gaps) >= FOLLOW_MIN_M - 0.02
    assert p.goals and min(p.goals) >= FOLLOW_MIN_M - 1e-6  # never a goal inside 50 cm
    assert max(c.v_cms for c in p.cmds) <= 25.0 + 1e-9


def test_follow_keeps_driving_after_a_crawler_moving_away() -> None:
    """§247: a new route each 15 cm the person moves must not stand 1 s first (the floor run's stop-go)."""
    p = play(FollowSkill(LIM, 8.0, 0.5), (1.5 + FRONT_M, 0.0), limit_s=9.0, walk=(0.12, 0.0))
    first = next(i for i, c in enumerate(p.cmds) if c.v_cms > 0)
    cmds = enumerate(p.cmds[first:], first)
    stood = [i for i, c in cmds if c.v_cms == 0 and c.w_dps == 0 and p.gaps[i] > 0.9]
    assert first * LIM.dt_s <= 0.3  # starts at once: the map is already running
    assert stood == []


def test_follow_up_close_and_aside_holds_with_the_camera_on_them() -> None:  # design-notes §250 (owner "1")
    """The floor run stopped and spun whenever FOLLOW's spot was close and a little aside (inside the tightest
    circle); now the tank holds and the camera turns."""
    person = (0.75 + FRONT_M, 0.30)  # ~20 deg left: the spot soon inside the tightest circle
    skill = FollowSkill(LIM, 3.0, 0.3)
    p = play(skill, person, limit_s=4.0)
    assert not any(c.v_cms == 0 and c.w_dps != 0 for c in p.cmds)  # never a turn on the spot
    assert p.cmds[-1] == Command(0.0, 0.0) and p.cams and p.cams[-1][0] < -15.0  # holds, the camera on them


def test_follow_turns_on_the_spot_only_beyond_the_cameras_reach() -> None:
    """A crawler going aside: the tank holds while the camera reaches them, then curves after them; a turn on
    the spot only beyond 60 deg."""
    start, walk = (0.9 + FRONT_M, 0.0), (0.0, 0.15)
    p = play(FollowSkill(LIM, 9.0, 0.3), start, limit_s=10.0, walk=walk)
    for i, (c, at) in enumerate(zip(p.cmds, p.at, strict=True)):
        if c.v_cms == 0 and abs(c.w_dps) >= LIM.spin_dps:
            px, py = start[0] + walk[0] * LIM.dt_s * i, start[1] + walk[1] * LIM.dt_s * i
            b = math.degrees(math.remainder(math.atan2(py - at[1], px - at[0]) - at[2], math.tau))
            assert abs(b) > 50.0, (i, b)
    assert max(p.gaps) < 1.0  # it went after them (the floor run's watch-only bug kept 1.1 m and growing)


def test_follow_unseen_is_lost() -> None:
    p = play(FollowSkill(LIM, 20.0, 1.0), (2.0, 0.0), hide_from_s=T0_S + 1.0, limit_s=10.0)
    assert p.result == Result.LOST


def test_follow_seconds_end_done() -> None:
    p = play(FollowSkill(LIM, 2.0, 1.0), (0.9 + FRONT_M, 0.0), limit_s=5.0)  # already near: waits facing
    assert p.result == Result.DONE and not p.moved
    assert len(p.cmds) == pytest.approx(21, abs=1)


def test_escape_never_starts_on_an_old_estimate() -> None:  # final review M1: a frozen camera
    skill = EscapeSkill(LIM, 0.30, TrustedFloor())
    room = RoomMap()
    seed_behind(room, (0.0, 0.0, 0.0), 1 * 1_000_000)
    pose = Pose(2_000_000, 0.0, 0.0, 0.0, 0.0, 0.0)
    old = PersonEst(True, 0.6, 0.0, 0, 0.9, 1_000_000)  # 1 s old, still "seen"
    inp = make_input(
        at=(0.0, 0.0, 0.0), snap=room.snapshot(2_000_000, pose, False, True), map_ok=True, person=old
    )
    out = skill.tick(T0_S, inp)
    assert out.result == Result.NO_WAY and out.cmd.v_cms == 0


def test_escape_takes_the_side_with_a_way() -> None:
    wall_left = [Box(-1.0, 0.35, 0.6, 0.8, 0.0, 0.4)]  # a tall box along the left (beyond the blind zone)
    person = at_bearing(0.9, 0.0)
    p = play(EscapeSkill(LIM, 0.30, TrustedFloor()), person, limit_s=40.0, boxes=wall_left)
    assert p.result == Result.DONE, p.result
    assert p.at[-1][1] < -0.2  # went right


def test_escape_with_no_way_either_side_turns_to_check_and_faces_them_again() -> None:
    walls = [Box(-1.0, 0.35, 0.6, 0.8, 0.0, 0.4), Box(-1.0, -0.8, 0.6, -0.35, 0.0, 0.4)]
    person = at_bearing(0.9, 0.0)
    p = play(EscapeSkill(LIM, 0.30, TrustedFloor()), person, boxes=walls)
    assert p.result == Result.NO_WAY
    assert all(c.v_cms == 0 for c in p.cmds)  # the sonar said no each side: never drove
    assert all(p.in_view) and abs(front_gap(p.at[-1], person)[1]) <= 15.0  # facing them again
