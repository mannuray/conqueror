"""RoamSkill, one tick at a time (AI engine LLD §18.7, §18.13, Plans Z5-Z6)."""

from __future__ import annotations

import math
import random

import numpy as np

from conqueror_ai.domain.depth import Marks
from conqueror_ai.domain.drive.chase import SkillOut
from conqueror_ai.domain.drive.curves import STOP, Command
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.roam import START_STAND_S, Mode, RoamSkill
from conqueror_ai.domain.drive.sim import SimTank
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap
from conqueror_ai.domain.types import Pose

AT = (0.0, 0.0, 0.0)
POSE = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


def open_snap(trusted: bool = True, depth: bool = True, seen: bool = True) -> MapSnapshot:
    """An empty room, all seen (or none); depth: a depth picture within the last second."""
    room = RoomMap()
    room.seen[:] = seen
    z = np.zeros((0, 2))
    room.update_picture(Marks(z, z, z, z), 0)
    return room.snapshot(0 if depth else 5_000_000, POSE, False, trusted)


def skill(mode: Mode = "wander", seconds: float | None = None, find: bool = False) -> RoamSkill:
    return RoamSkill(Planner(Limits()), mode, seconds=seconds, find=find, rng=random.Random(1))


def run(
    s: RoamSkill, snap: MapSnapshot, seconds: float, sonar: float | None = None, dt: float = 0.1
) -> list[SkillOut]:
    return [s.tick(i * dt, AT, snap, None, sonar, True) for i in range(round(seconds / dt))]


def test_echo_under_10_cm_ends_blocked() -> None:
    s = skill()
    snap = open_snap()
    run(s, snap, START_STAND_S + 0.5)
    assert s.tick(10.0, AT, snap, None, 0.12, True).result is None
    assert s.tick(10.1, AT, snap, None, 0.09, True).result == "blocked"


def test_stale_stops_at_once_and_ends_after_2_s() -> None:
    s = skill()
    snap = open_snap()
    run(s, snap, START_STAND_S + 1.5)
    assert s.tick(20.0, AT, snap, None, None, False).cmd == STOP
    assert s.tick(21.0, AT, snap, None, None, False).result is None
    assert s.tick(22.0, AT, None, None, None, True).result == "stale"


def moving(s: RoamSkill, snap: MapSnapshot, seconds: float, dt: float = 0.1) -> list[SkillOut]:
    """Ticks with a simulated tank that follows the commands (at 18 cm/s the tank can't curve: it turns on the
    spot to face the aim point, then drives straight - a fixed pose would turn for ever)."""
    tank, outs = SimTank(), list[SkillOut]()
    for i in range(round(seconds / dt)):
        outs.append(s.tick(i * dt, tank.at, snap, None, None, True))
        tank.step(outs[-1].cmd, dt)
    return outs


def test_slow_when_angle_untrusted_or_no_depth() -> None:
    for snap in (open_snap(trusted=False), open_snap(depth=False)):
        s = skill()
        outs = moving(s, snap, 10.0)
        assert max(o.cmd.v_cms for o in outs) <= 18.0 + 1e-9
        assert max(o.cmd.v_cms for o in outs) > 0
    fast = max(o.cmd.v_cms for o in moving(skill(), open_snap(), 10.0))
    assert fast > 18.0


def test_wander_ends_after_its_seconds() -> None:
    s = skill(seconds=5.0)
    outs = run(s, open_snap(), 6.0)
    ends = [i for i, o in enumerate(outs) if o.result]
    assert [outs[i].result for i in ends] == ["done"] and ends[0] == 50


def test_find_hands_over_to_the_chase() -> None:
    s = skill(find=True)
    snap = open_snap()
    run(s, snap, START_STAND_S + 1.0)
    out = s.tick(5.0, AT, snap, TargetFix(1, 2.0, 0.0, 0.0), None, True)
    assert s.state == "chase" and out.result is None


def test_cancel_stops() -> None:
    s = skill()
    snap = open_snap()
    run(s, snap, START_STAND_S + 1.5)
    out = s.cancel()
    assert out.cmd == STOP and out.result == "cancelled"
    assert s.tick(30.0, AT, snap, None, None, True).cmd == STOP


def test_a_blocked_way_stops_at_once_not_by_the_ramp() -> None:
    """Simulated wander, seed 3: every curve blocked by a bed edge ahead, the skill went to look - and the
    speed ramp (10 cm/s per s) coasted it 2 s on into the edge. A blocked way stops at once (Plan Z5)."""
    s = skill()
    tank = SimTank()
    snap = open_snap()
    for i in range(40):  # get going
        out = s.tick(i * 0.1, tank.at, snap, None, None, True)
        tank.step(out.cmd, 0.1)
    assert s.cmd.v_cms > 18.0
    room = RoomMap()
    room.seen[:] = True
    x, y, th = tank.at
    ahead = [
        (x + d * math.cos(th) - k * math.sin(th), y + d * math.sin(th) + k * math.cos(th))
        for d in (0.25, 0.3)
        for k in np.arange(-1.5, 1.51, 0.025)
    ]
    c = room.cells(np.array(ahead, dtype=np.float64))
    room.hit(c, "high")
    room.update_picture(Marks(*(np.zeros((0, 2)),) * 4), 4_000_000)
    wall = room.snapshot(4_000_000, POSE, False, True)
    out = s.tick(4.0, tank.at, wall, None, None, True)
    assert out.cmd == STOP


# ---- stuck: the standard ways out (LLD §18.13.4, Plan Z6)
def test_starts_standing_with_the_camera_ahead_then_drives() -> None:
    s = skill()
    outs = run(s, open_snap(), START_STAND_S + 1.5)
    assert outs[0].cmd == STOP and outs[0].cam == (0.0, -15.0)
    assert all(o.cmd == STOP for o in outs[: round(START_STAND_S / 0.1)])
    assert s.state == "drive" and any(o.cmd.v_cms > 0 or o.cmd.w_dps != 0 for o in outs)


def test_nowhere_to_go_runs_the_ways_out_then_blocked() -> None:
    """Nothing seen: no destination - look round with the camera, turn, (nothing known behind) blocked."""
    s = skill()
    outs = run(s, open_snap(seen=False), 60.0)
    pans = [o.cam[0] for o in outs if o.cam is not None]
    assert [p for p in pans if p != 0.0][:4] == [-90.0, -45.0, 45.0, 90.0]
    assert any(o.cmd.v_cms == 0 and o.cmd.w_dps != 0 for o in outs)  # a turn on the spot
    assert all(o.cmd.v_cms >= 0 for o in outs)  # never backs onto unknown floor
    assert [o.result for o in outs if o.result] == ["blocked"]


def test_no_progress_is_stuck() -> None:
    """The fan refusing everything for stuck_s (3 s) starts the ways out; standing still, they end blocked."""
    s = skill()
    s.planner.choose = lambda *a, **k: STOP  # type: ignore[method-assign]
    outs = run(s, open_snap(), 60.0)
    assert [o.result for o in outs if o.result] == ["blocked"]
    first = next(i for i, o in enumerate(outs) if o.cam is not None and o.cam[0] == -90.0)
    assert 3.5 <= first * 0.1 <= 4.5  # 1 s start + 3 s stuck


def area(x0: float, y0: float, x1: float, y1: float) -> list[tuple[float, float]]:
    return [
        (float(x), float(y)) for x in np.arange(x0, x1 + 1e-9, 0.025) for y in np.arange(y0, y1 + 1e-9, 0.025)
    ]


def pocket(behind_known: bool) -> MapSnapshot:
    """Floor run 2's kind of spot: things 17.5 cm each side and 25 cm ahead (cell centres)."""
    room = RoomMap()
    room.seen[:] = True
    if not behind_known:
        room.seen.reshape(-1)[room.cells(np.array(area(-0.6, -0.15, -0.1, 0.15)))] = False
    side = area(-0.6, 0.175, 0.5, 0.3)
    c = room.cells(np.array(side + [(x, -y) for x, y in side] + area(0.25, -0.3, 0.5, 0.3)))
    room.hit(c, "low")
    room.hit(c, "low")
    room.update_picture(Marks(*(np.zeros((0, 2)),) * 4), 0)
    return room.snapshot(0, POSE, False, True)


def test_boxed_in_it_backs_out_over_known_floor() -> None:
    outs = moving(skill(), pocket(behind_known=True), 30.0)
    back = [o.cmd for o in outs if o.cmd.v_cms < 0]
    assert back and all(c == Command(-18.0, 0.0) for c in back)


def test_boxed_in_with_nothing_known_behind_it_never_backs() -> None:
    outs = moving(skill(), pocket(behind_known=False), 40.0)
    assert all(o.cmd.v_cms >= 0 for o in outs)
    assert [o.result for o in outs if o.result] == ["blocked"]
