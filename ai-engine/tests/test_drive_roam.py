"""RoamSkill, one tick at a time (AI engine LLD §18.7, §18.13, Plans Z5-Z6)."""

from __future__ import annotations

import math
import random
from collections.abc import Sequence
from dataclasses import replace
from typing import cast

import numpy as np
import pytest

from conqueror_ai.domain.depth import Marks
from conqueror_ai.domain.drive.candidates import REOPEN_MAX
from conqueror_ai.domain.drive.curves import STOP, Command, XYTh
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.roam import CANDIDATE_RETRY_S, START_STAND_S, Mode, RoamSkill
from conqueror_ai.domain.drive.sim import SimTank
from conqueror_ai.domain.drive.skill import SkillOut, make_input
from conqueror_ai.domain.memory import ThingMemory
from conqueror_ai.domain.objects import Glimpse
from conqueror_ai.domain.reach import CHECK_CLOSE_M
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap
from conqueror_ai.domain.types import ObjectEstimate, Pose

AT = (0.0, 0.0, 0.0)
POSE = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


def open_snap(trusted: bool = True, depth: bool = True, seen: bool = True) -> MapSnapshot:
    """An empty room, all seen (or none); depth: a depth picture within the last second."""
    room = RoomMap()
    room.seen[:] = seen
    z = np.zeros((0, 2))
    room.update_picture(Marks(z, z, z, z), 0)
    return room.snapshot(0 if depth else 5_000_000, POSE, False, trusted)


def rt(
    s: RoamSkill,
    now: float,
    at: XYTh | None,
    snap: MapSnapshot | None,
    fix: TargetFix | None,
    sonar: float | None,
    map_ok: bool,
    cam_still: bool = True,
    maybes: Sequence[tuple[float, float]] = (),
    used: bool = True,
    fresh: bool | None = None,
    glimpses: Sequence[Glimpse] = (),
) -> SkillOut:
    """The old call shape onto SkillInput (LLD §19.4)."""
    inp = make_input(
        at=at,
        snap=snap,
        map_ok=map_ok,
        fix=fix,
        sonar_m=sonar,
        cam_still=cam_still,
        used=used,
        maybes=maybes,
        fresh=fresh,
        glimpses=glimpses,
    )
    return s.tick(now, inp)


def room_with_marks(marks: list[tuple[float, float]]) -> MapSnapshot:
    room = RoomMap()
    room.seen[:] = True
    z = np.zeros((0, 2))
    room.update_picture(Marks(z, z, z, z), 0)
    c = room.cells(np.array(marks, dtype=np.float64))
    room.hit(c, "high")
    room.hit(c, "high")
    return room.snapshot(0, POSE, False, True)


def skill(mode: Mode = "wander", seconds: float | None = None, find: bool = False) -> RoamSkill:
    return RoamSkill(Planner(Limits()), "find" if find else mode, seconds=seconds, rng=random.Random(1))


def run(
    s: RoamSkill, snap: MapSnapshot, seconds: float, sonar: float | None = None, dt: float = 0.1
) -> list[SkillOut]:
    return [rt(s, i * dt, AT, snap, None, sonar, True) for i in range(round(seconds / dt))]


def test_echo_under_10_cm_ends_blocked() -> None:
    s = skill()
    snap = open_snap()
    run(s, snap, START_STAND_S + 0.5)
    assert rt(s, 10.0, AT, snap, None, 0.12, True).result is None
    assert rt(s, 10.1, AT, snap, None, 0.09, True).result == "blocked"


def test_stale_stops_at_once_and_ends_after_2_s() -> None:
    s = skill()
    snap = open_snap()
    run(s, snap, START_STAND_S + 1.5)
    assert rt(s, 20.0, AT, snap, None, None, False).cmd == STOP
    assert rt(s, 21.0, AT, snap, None, None, False).result is None
    assert rt(s, 22.0, AT, None, None, None, True).result == "stale"


def moving(s: RoamSkill, snap: MapSnapshot, seconds: float, dt: float = 0.1) -> list[SkillOut]:
    """Ticks with a simulated tank that follows the commands (at 18 cm/s the tank can't curve: it turns on the
    spot to face the aim point, then drives straight - a fixed pose would turn for ever)."""
    tank, outs = SimTank(), list[SkillOut]()
    for i in range(round(seconds / dt)):
        outs.append(rt(s, i * dt, tank.at, snap, None, None, True))
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
    out = rt(s, 5.0, AT, snap, TargetFix(1, 2.0, 0.0, 0.0), None, True)
    assert s.state == "chase" and out.result is None


def test_find_looks_round_from_where_it_stands_first() -> None:
    """Find (design-notes §187): first the chase's look round from here (the camera ahead, then each side);
    nothing seen, it then searches the room."""
    s = skill("find")
    snap = open_snap()
    out = rt(s, 0.0, AT, snap, None, None, True)
    assert s.state == "chase" and s.chase.state == "sweep" and out.cam is not None and out.cmd == STOP
    tank, turned = SimTank(), 0.0
    for k in range(1, 400):  # the tank follows: the half turn is measured from the pose
        o = rt(s, k * 0.1, tank.at, snap, None, None, True)
        tank.step(o.cmd, 0.1)
        turned = max(turned, abs(math.degrees(tank.at[2])))
        if s.state != "chase":
            break
    assert s.state == "drive" and turned >= 170.0


def test_a_search_ends_not_found_once_every_spot_was_looked_at_from_near() -> None:
    """A search ends (owner, design-notes §188, option A): a spot is searched once the camera looked at it
    from within LOOK_M; every reachable spot searched and the thing never seen, find ends not_found. Floor run
    n1: a room all seen from the start ended after ~2 s of driving - now it drives round to look."""
    s = RoamSkill(Planner(Limits()), "find", rng=random.Random(1), area_m=1.5)
    snap = open_snap()
    tank, results, path = SimTank(), list[str](), 0.0
    for k in range(6000):
        o = rt(s, k * 0.1, tank.at, snap, None, None, True)
        before = tank.at
        tank.step(o.cmd, 0.1)
        path += math.dist(before[:2], tank.at[:2])
        if o.result:
            results.append(o.result)
            break
    assert results == ["not_found"]
    assert path > 1.0  # it went to look, not just turned round
    assert all(s.search.looked((x, y)) for x, y in ((1.0, 0.0), (-1.0, 0.0), (0.0, 1.0), (0.0, -1.0)))


def test_explore_ends_done() -> None:
    s = skill("explore")
    outs = run(s, open_snap(), START_STAND_S + 1.0)
    assert [o.result for o in outs if o.result] == ["done"]


def test_cancel_stops() -> None:
    s = skill()
    snap = open_snap()
    run(s, snap, START_STAND_S + 1.5)
    out = s.cancel()
    assert out.cmd == STOP and out.result == "cancelled"
    assert rt(s, 30.0, AT, snap, None, None, True).cmd == STOP


def test_a_blocked_way_stops_at_once_not_by_the_ramp() -> None:
    """Simulated wander, seed 3: every curve blocked by a bed edge ahead, the skill went to look - and the
    speed ramp (10 cm/s per s) coasted it 2 s on into the edge. A blocked way stops at once (Plan Z5)."""
    s = skill()
    tank = SimTank()
    snap = open_snap()
    for i in range(40):  # get going
        out = rt(s, i * 0.1, tank.at, snap, None, None, True)
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
    out = rt(s, 4.0, tank.at, wall, None, None, True)
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


def test_explore_picks_again_when_its_target_closes() -> None:
    """Explore keeps its target 3 s; the camera then marked a wall beside it and closed it - the run went
    into the ways out instead of picking again (Plan Z6 final review fix pass, the run-3 layout)."""
    s = skill("explore")
    snap = open_snap(seen=False)  # unseen: frontier everywhere at the edge of what's seen
    room = RoomMap()
    room.seen[:] = False
    room.seen.reshape(-1)[room.cells(np.array(area(-0.6, -0.6, 0.6, 0.6)))] = True
    room.update_picture(Marks(*(np.zeros((0, 2)),) * 4), 0)
    snap = room.snapshot(0, POSE, False, True)
    run(s, snap, START_STAND_S + 0.2)
    s.explore._dest, s.explore._picked_s = (3.95, 3.95), 0.9  # pyright: ignore[reportPrivateUsage]  # closed: the map's edge
    out = rt(s, 1.4, AT, snap, None, None, True)
    assert s.state == "drive" and s.dest != (3.95, 3.95) and out.result is None


# ---- candidates and the memory of things (owner, design-notes §189)
def finder(memory: ThingMemory | None = None) -> RoamSkill:
    return RoamSkill(
        Planner(Limits()),
        "find",
        rng=random.Random(1),
        area_m=2.0,
        memory=memory,
        kind=lambda c: c == "toy",
    )


def ticks(
    s: RoamSkill, tank: SimTank, t0: float, n: int, maybes: list[tuple[float, float]] | None = None
) -> list[SkillOut]:
    outs: list[SkillOut] = []
    for k in range(n):
        o = rt(s, t0 + k * 0.1, tank.at, open_snap(), None, None, True, maybes=maybes or [])
        tank.step(o.cmd, 0.1)
        outs.append(o)
        if o.result:
            break
    return outs


def until_searching(s: RoamSkill, tank: SimTank) -> float:
    for k in range(600):  # the tank follows (the look round's half turn is measured from the pose)
        tank.step(rt(s, k * 0.1, tank.at, open_snap(), None, None, True).cmd, 0.1)
        if s.state == "drive":
            return k * 0.1 + 0.1
    raise AssertionError("never got to searching")


def test_a_candidate_is_checked_before_unlooked_floor() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    ticks(s, tank, t, 1, maybes=[(1.2, 0.6)])
    assert s.state == "check" and s._check is not None and s._check.spot == (1.2, 0.6)  # pyright: ignore[reportPrivateUsage]


def test_a_candidate_not_confirmed_is_ruled_out_and_the_search_goes_on() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    ticks(s, tank, t, 1, maybes=[(1.2, 0.6)])
    outs = ticks(s, tank, t + 0.1, 300)
    assert s.state != "check" and s.candidates.pending() == []
    assert not any(o.result for o in outs[:-1])
    ticks(s, tank, t + 40.0, 1, maybes=[(1.25, 0.6)])  # seen weakly there again: looked at again (§24 A)
    assert s.state == "check"


def test_a_remembered_spot_is_checked_first_and_marked_gone_when_not_there() -> None:
    mem = ThingMemory()
    mem.update([ObjectEstimate(1, "toy", 1.2, 0.4, 0.0, 0.0, 0.1, 0, 0, "seen")], 0.0, "r")
    s, tank = finder(mem), SimTank()
    ticks(s, tank, 0.0, 1)
    assert s.state == "check"  # no look round first: it knows where to look
    ticks(s, tank, 0.1, 300)
    assert mem.of_kind(lambda c: c == "toy") == [] and s.state != "check"


def test_the_search_marks_only_from_a_picture_used_this_tick() -> None:
    """Z6 second review I2: a picture skipped for turning (or no picture this tick) used to mark the camera's
    view as looked at, so find could end not_found without having looked."""
    s = finder()
    rt(s, 0.0, AT, open_snap(), None, None, True, used=False)  # the camera sent ahead
    rt(s, 0.1, AT, open_snap(), None, None, True, used=False)
    assert not s.search.looked((0.6, 0.0))
    rt(s, 0.2, AT, open_snap(), None, None, True, used=True)
    assert s.search.looked((0.6, 0.0))


def test_a_far_candidate_is_ruled_out_only_after_a_look_from_near() -> None:
    """Z6 second review I3: the check's 15 s covered the drive too, and going closer aimed at exactly 1 m, so
    a candidate 3 m off timed out at ~1.00 m and was ruled out without a near look."""
    s, tank = RoamSkill(Planner(Limits()), "find", rng=random.Random(1)), SimTank()
    t = until_searching(s, tank)
    spot = (tank.at[0] + 3.0 * math.cos(tank.at[2]), tank.at[1] + 3.0 * math.sin(tank.at[2]))
    ticks(s, tank, t, 1, maybes=[spot])
    assert s.state == "check"
    nearest = 9.0
    for k in range(1, 900):
        rt(s, t + k * 0.1, tank.at, open_snap(), None, None, True)
        tank.step(s.cmd, 0.1)
        nearest = min(nearest, math.dist(tank.at[:2], spot))
        if s.state != "check":
            break
    assert s.candidates.pending() == [] and nearest <= CHECK_CLOSE_M + 0.05  # §24 C: 0.5 m, not 1 m


def test_a_check_from_one_metre_drives_to_half_a_metre_before_its_look() -> None:
    """LLD §24 C (owner, §215): a maybe 1 m off is not looked at from there - sure only at 40-50 cm."""
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    spot = (tank.at[0] + 1.0 * math.cos(tank.at[2]), tank.at[1] + 1.0 * math.sin(tank.at[2]))
    ticks(s, tank, t, 1, maybes=[spot])
    assert s.state == "check"
    nearest = 9.0
    for k in range(1, 600):
        rt(s, t + k * 0.1, tank.at, open_snap(), None, None, True)
        tank.step(s.cmd, 0.1)
        nearest = min(nearest, math.dist(tank.at[:2], spot))
        if s.state != "check":
            break
    assert nearest <= CHECK_CLOSE_M + 0.05 and s.candidates.ruled_out()


def close_check() -> tuple[RoamSkill, SimTank, float, tuple[float, float]]:
    """A find already searching, a maybe 0.48 m ahead (within the check's 0.5 m, beyond the chase's stop):
    its check goes straight to the look (§24 B tests)."""
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    spot = (tank.at[0] + 0.48 * math.cos(tank.at[2]), tank.at[1] + 0.48 * math.sin(tank.at[2]))
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[spot])
    assert s.state == "check"
    rt(s, t + 0.1, tank.at, open_snap(), None, None, True)  # the camera sent to the spot
    return s, tank, t + 0.2, spot


def test_two_pictures_with_the_thing_during_a_close_look_hand_over_to_the_chase() -> None:
    s, tank, t, spot = close_check()
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[spot])
    assert s.state == "check"
    rt(s, t + 0.1, tank.at, open_snap(), None, None, True, maybes=[(spot[0] + 0.05, spot[1])])
    assert s.state == "chase"


def test_one_picture_during_a_close_look_is_not_enough() -> None:
    s, tank, t, spot = close_check()
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[spot])
    for k in range(1, 40):
        rt(s, t + 0.1 * k, tank.at, open_snap(), None, None, True)
        assert s.state != "chase"
    assert s.candidates.ruled_out()


def test_the_same_picture_on_two_ticks_counts_once() -> None:
    s, tank, t, spot = close_check()
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[spot], used=True)
    rt(s, t + 0.1, tank.at, open_snap(), None, None, True, maybes=[spot], used=False)
    for k in range(2, 40):
        rt(s, t + 0.1 * k, tank.at, open_snap(), None, None, True)
        assert s.state != "chase"


def test_a_sighting_away_from_the_spot_is_no_hit() -> None:
    s, tank, t, spot = close_check()
    far = (spot[0] - 0.8 * math.sin(tank.at[2]), spot[1] + 0.8 * math.cos(tank.at[2]))  # 0.8 m to the side
    for k in range(40):
        rt(s, t + 0.1 * k, tank.at, open_snap(), None, None, True, maybes=[far])
        if s._check is None or s._check.spot != spot:  # pyright: ignore[reportPrivateUsage]
            break
        assert s.state != "chase"


def test_the_trackers_fix_replaces_the_made_up_one() -> None:
    s, tank, t, spot = close_check()
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[spot])
    rt(s, t + 0.1, tank.at, open_snap(), None, None, True, maybes=[spot])
    assert s.state == "chase"
    real = TargetFix(7, spot[0] + 0.1, spot[1], 0.0)
    rt(s, t + 0.2, tank.at, open_snap(), real, None, True)
    assert s.chase.debug()["target"] == (real.x_m, real.y_m)


def ahead(tank: SimTank, m: float) -> tuple[float, float]:
    return (tank.at[0] + m * math.cos(tank.at[2]), tank.at[1] + m * math.sin(tank.at[2]))


def test_two_pictures_from_one_metre_do_not_confirm_it_drives_closer() -> None:
    """Final review I1: hits only count from near (the owner: "only at this distance one can be sure")."""
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    spot = ahead(tank, 1.0)
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[spot])
    rt(s, t + 0.1, tank.at, open_snap(), None, None, True)  # aimed
    for k in range(2, 4):
        rt(s, t + 0.1 * k, tank.at, open_snap(), None, None, True, maybes=[spot])
    for k in range(4, 40):
        rt(s, t + 0.1 * k, tank.at, open_snap(), None, None, True)
        assert s.state != "chase"
        if s._check is not None and s._check.phase == "closer":  # pyright: ignore[reportPrivateUsage]
            break
    assert s._check is not None and s._check.phase == "closer"  # pyright: ignore[reportPrivateUsage]


def test_the_same_picture_again_does_not_reopen_a_ruled_out_spot() -> None:
    """Final review I2: a picture re-read on a tick with no new one is not "seen again" (§24 A)."""
    s, tank, t, spot = close_check()
    for k in range(40):
        rt(s, t + 0.1 * k, tank.at, open_snap(), None, None, True)
        if s.candidates.ruled_out():
            break
    assert s.candidates.ruled_out() and not s.candidates.pending()
    rt(s, t + 4.1, tank.at, open_snap(), None, None, True, maybes=[spot], used=False)
    assert not s.candidates.pending()
    rt(s, t + 4.2, tank.at, open_snap(), None, None, True, maybes=[spot], used=True)
    assert s.candidates.pending()


def test_the_made_up_fix_is_not_refreshed_by_a_re_read_picture() -> None:
    """Final review M1: only a new picture says the thing was seen now."""
    s, tank, t, spot = close_check()
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[spot])
    rt(s, t + 0.1, tank.at, open_snap(), None, None, True, maybes=[spot])
    assert s.state == "chase"
    rt(s, t + 0.5, tank.at, open_snap(), None, None, True, maybes=[spot], used=False)
    f = s._maybe_fix  # pyright: ignore[reportPrivateUsage]
    assert f is not None and f.seen_age_s > 0.3


def test_close_sightings_outside_the_play_area_are_never_chased() -> None:
    """Final review M2 (re-graded Important: the find ended blocked): two hits on a maybe just outside the
    area, within 0.5 m of a spot inside it, start no chase toward it and leave no candidate outside."""
    s = RoamSkill(Planner(Limits()), "find", rng=random.Random(1), area_m=1.0, kind=lambda c: c == "toy")
    tank = SimTank()
    t = until_searching(s, tank)
    centre = s._area  # pyright: ignore[reportPrivateUsage]
    assert centre is not None
    spot = ahead(tank, 0.9) if math.dist(ahead(tank, 0.9), centre[:2]) < 1.0 else ahead(tank, 0.5)
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[spot])
    assert s.state == "check"
    for _ in range(300):  # to the near look
        t += 0.1
        tank.step(rt(s, t, tank.at, open_snap(), None, None, True).cmd, 0.1)
        c = s._check  # pyright: ignore[reportPrivateUsage]
        if c is None or (c.phase == "look" and c.aimed and math.dist(tank.at[:2], spot) <= 0.55):
            break
    a = math.atan2(spot[1] - centre[1], spot[0] - centre[0])
    out = (centre[0] + 1.2 * math.cos(a), centre[1] + 1.2 * math.sin(a))  # 0.2 m outside
    assert math.dist(out, spot) <= 0.5
    for j in range(1, 30):
        o = rt(s, t + 0.1 * j, tank.at, open_snap(), None, None, True, maybes=[out])
        assert s.state != "chase" and o.result != "blocked"  # was: chased out, blocked
        assert all(math.dist(p.spot, centre[:2]) <= 1.0 for p in s.candidates.pending())
        if o.result:  # §196: seen outside the area, its edge looked at - "outside_area", not a chase
            assert o.result == "outside_area"
            break


def test_a_check_that_cannot_get_there_leaves_it_pending_and_not_gone() -> None:
    """Z6 second review I3: no way to it (the map closed it) is not a look from near: the remembered thing is
    not marked gone; tried again later, ruled out (not gone) after a few failed tries."""
    mem = ThingMemory()
    mem.update(
        [ObjectEstimate(1, "toy", 1.8, 0.0, 0.0, 0.0, 0.1, 0, 0, "seen")], 0.0, "r"
    )  # inside the 2 m area
    s, tank = finder(mem), SimTank()
    ticks(s, tank, 0.0, 1)
    assert s.state == "check"
    ring = [(1.8 + 1.3 * math.cos(a), 1.3 * math.sin(a)) for a in np.arange(0.0, 2 * math.pi, 0.02)]
    walled = room_with_marks(ring)  # a wall round the spot, 1.3 m out: no way to within 1 m
    for k in range(1, 400):  # the map now walls the spot off
        tank.step(rt(s, 0.1 * k, tank.at, walled, None, None, True).cmd, 0.1)
        if s.state != "check":
            break
    assert s.state != "check"
    assert len(s.candidates.pending()) == 1 and mem.of_kind(lambda c: c == "toy") != []


def chase_and_lose(s: RoamSkill, tank: SimTank, t: float, fix: TargetFix) -> float:
    """Seen for sure (the chase), then never again until the chase gives up; the time after it."""
    for k in range(1, 4):
        tank.step(rt(s, t + 0.1 * k, tank.at, open_snap(), fix, None, True).cmd, 0.1)
    assert s.state == "chase"
    for k in range(4, 900):
        tank.step(rt(s, t + 0.1 * k, tank.at, open_snap(), None, None, True).cmd, 0.1)
        if s.state != "chase":
            return t + 0.1 * k
    raise AssertionError("the chase never gave up")


def test_a_lost_chase_leaves_a_candidate_at_the_last_seen_spot() -> None:
    """LLD §24 D (the room3 chair, §215): checked, seen, chased, lost - its last seen spot is checked again
    from near rather than dropped."""
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    ticks(s, tank, t, 1, maybes=[(1.2, 0.6)])
    assert s.state == "check"
    chase_and_lose(s, tank, t, TargetFix(1, 1.25, 0.6, 0.0))
    assert [k.spot for k in s.candidates.pending()] == [(1.25, 0.6)]


def test_a_thing_lost_again_and_again_is_dropped_after_two_reopens() -> None:
    """Z6 second review M1 kept: a candidate seen, chased and lost must not be checked and chased for ever -
    after the first loss and REOPEN_MAX more it is left."""
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    ticks(s, tank, t, 1, maybes=[(1.2, 0.6)])
    losses = 0
    for _ in range(6):
        for _k in range(300):  # to the next check of it
            if s.state == "check":
                break
            t += 0.1
            tank.step(rt(s, t, tank.at, open_snap(), None, None, True).cmd, 0.1)
        if s.state != "check":
            break
        t = chase_and_lose(s, tank, t, TargetFix(1, 1.2, 0.6, 0.0))
        losses += 1
    assert losses == 1 + REOPEN_MAX and s.candidates.pending() == []


def test_not_found_waits_for_a_pending_candidates_retry() -> None:
    """Z6 second review M3: with the search finished inside the 1 s retry window, find ended not_found while a
    candidate was still pending; it now waits for the retry."""
    s = RoamSkill(Planner(Limits()), "find", rng=random.Random(1), area_m=0.8)
    rt(s, 0.0, AT, open_snap(), None, None, True)
    s.state = "drive"
    for pan in range(-180, 180, 20):
        s.search.look(AT, float(pan), 0.0)  # everything in the small area looked at
    s.candidates.add((0.4, 0.3))
    s._retry_s = 10.0 + CANDIDATE_RETRY_S  # pyright: ignore[reportPrivateUsage]  # a try just failed
    assert rt(s, 10.0, AT, open_snap(), None, None, True).result is None
    rt(s, 10.0 + CANDIDATE_RETRY_S + 0.1, AT, open_snap(), None, None, True)
    assert s.state == "check"


def test_sightings_outside_the_play_area_are_not_candidates() -> None:  # Z6 second review M4
    s = RoamSkill(Planner(Limits()), "find", rng=random.Random(1), area_m=1.0)
    rt(s, 0.0, AT, open_snap(), None, None, True, maybes=[(2.0, 0.0), (0.5, 0.0)])
    assert [c.spot for c in s.candidates.pending()] == [(0.5, 0.0)]


# ---- the Skill interface (Plan Z7, LLD §19.4, §19.7)
def test_find_is_a_mode_not_a_flag() -> None:
    import inspect

    from conqueror_ai.domain.drive.skill import make_input

    assert "find" not in inspect.signature(RoamSkill).parameters
    s = RoamSkill(Planner(Limits()), "find", rng=random.Random(1))
    out = s.tick(0.0, make_input(at=AT, snap=open_snap(), map_ok=True))
    assert s.state == "chase" and out.cam is not None  # find starts with the look round


def test_the_goals_are_one_interface() -> None:
    from conqueror_ai.domain.drive.goals import DestinationGoal, ExploreGoal, SearchGoal, WanderGoal

    for g in (WanderGoal(random.Random(1)), ExploreGoal(), SearchGoal()):
        assert isinstance(g, DestinationGoal) and g.done is False


def test_roam_is_a_skill() -> None:
    from conqueror_ai.domain.drive.skill import Skill

    s = RoamSkill(Planner(Limits()), "wander", rng=random.Random(1))
    assert isinstance(s, Skill) and set(s.debug()) >= {"state", "dest", "pending", "check_phase"}


def test_a_reachable_candidate_behind_three_unreachable_ones_is_checked_before_not_found() -> None:
    """Plan Z7 final review I1: candidates ranked by straight line, routes for the nearest 3 - three maybes
    walled in beat a reachable one further off; with the search done, find ended not_found at once. Before
    not_found every pending candidate gets a route."""
    room = RoomMap()
    room.seen[:] = True
    z = np.zeros((0, 2))
    room.update_picture(Marks(z, z, z, z), 0)
    wall = [(x, y) for x in np.arange(1.3, 3.96, 0.05) for y in (-1.7, 1.7)]
    wall += [(x, y) for y in np.arange(-1.7, 1.71, 0.05) for x in (1.3, 3.95)]
    c = room.cells(np.array(wall))
    room.hit(c, "high")
    room.hit(c, "high")
    snap = room.snapshot(0, POSE, False, True)
    s = RoamSkill(Planner(Limits()), "find", rng=random.Random(1), kind=lambda w: w == "ball")
    s.goal.destination = lambda *a: None  # type: ignore[method-assign]  # the search has looked everywhere
    s.goal.done = True
    s.state = "drive"
    for spot in ((2.9, 0.0), (2.9, 0.6), (2.9, -0.6), (-3.2, 0.0)):
        s.candidates.add(spot)
    out = rt(s, 0.0, AT, snap, None, None, True)
    assert out.result is None and s.state == "check" and s._check is not None  # pyright: ignore[reportPrivateUsage]
    assert s._check.spot == (-3.2, 0.0)  # pyright: ignore[reportPrivateUsage]


def test_debug_carries_the_route_the_candidates_and_the_looked_floor() -> None:
    """Plan Z8 (LLD §20.7): what the viewer draws."""
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    ticks(s, tank, t, 1, maybes=[(1.2, 0.6)])
    d = s.debug()
    assert d["check"] == (1.2, 0.6) and d["pending"] == [(1.2, 0.6)] and d["ruled_out"] == []
    looked = d["looked"]
    assert isinstance(looked, list) and len(cast("list[object]", looked)) > 0  # the look round's views
    assert "route" in d


# ---- a thing seen outside the play area (owner, Plan Z8 floor run z8-f1, design-notes §196)
def outside_area_finder() -> RoamSkill:
    return RoamSkill(Planner(Limits()), "find", rng=random.Random(1), area_m=2.0, kind=lambda w: w == "toy")


def test_a_thing_seen_outside_the_area_is_not_chased_its_edge_is_checked() -> None:
    """Floor run z8-f1: the car 3.1 m off with a 3 m area - the chase ended blocked. Owner: don't chase it; go
    to the area's edge nearest where it was seen, look again, search on."""
    s, tank = outside_area_finder(), SimTank()
    t = until_searching(s, tank)
    fix = TargetFix(1, 3.0, 0.0, 0.0)  # 3 m off, the area 2 m
    out = s.tick(t, make_input(at=tank.at, snap=open_snap(), map_ok=True, fix=fix, area=None))
    assert s.state != "chase" and out.result is None
    (c,) = s.candidates.pending()
    assert (
        math.dist(c.spot, (0.0, 0.0)) < 2.0 and c.spot[0] > 1.5 and abs(c.spot[1]) < 0.05
    )  # the edge toward it


def test_outside_area_says_it_was_seen_outside_the_area() -> None:
    s, tank = outside_area_finder(), SimTank()
    t = until_searching(s, tank)
    rt(s, t, tank.at, open_snap(), TargetFix(1, 3.0, 0.0, 0.0), None, True)
    s.goal.destination = lambda *a: None  # type: ignore[method-assign]  # the search has looked everywhere
    s.goal.done = True
    for c in s.candidates.pending():
        s.candidates.rule_out(c)
    s.state = "drive"
    out = rt(s, t + 1.0, tank.at, open_snap(), None, None, True)
    assert out.result == "outside_area" and "seen outside the play area" in out.detail


def test_point_mode_needs_a_point() -> None:
    with pytest.raises(ValueError, match="needs a point"):
        RoamSkill(Planner(Limits()), "point")


# ---- Plan Z10 (LLD §25)
def test_a_turning_pictures_maybe_is_a_candidate() -> None:
    """A1: a maybe from a picture taken while turning (fresh, not used) is a candidate like any other."""
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[(1.2, 0.6)], used=False, fresh=True)
    assert [c.spot for c in s.candidates.pending()] == [(1.2, 0.6)]


def test_a_re_read_pictures_maybe_is_still_no_candidate() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    rt(s, t, tank.at, open_snap(), None, None, True, maybes=[(1.2, 0.6)], used=False, fresh=False)
    assert s.candidates.pending() == []


def glimpse_at(tank: SimTank, lo_deg: float, hi_deg: float) -> Glimpse:
    """A glimpse lo..hi degrees left of the tank's heading (right = negative)."""
    th = tank.at[2]
    return Glimpse("toy", tank.at[:2], th + math.radians(lo_deg), th + math.radians(hi_deg), 0)


def look_ticks(s: RoamSkill, tank: SimTank, t0: float, n: int, **kw: object) -> list[SkillOut]:
    outs: list[SkillOut] = []
    for k in range(n):
        o = rt(s, t0 + k * 0.1, tank.at, open_snap(), None, None, True, **kw)  # type: ignore[arg-type]
        tank.step(o.cmd, 0.1)
        outs.append(o)
    return outs


def test_glimpse_left_stops_and_looks_there() -> None:
    """A2: stop, the camera to the middle of the range (45 deg left = pan -45), 1.5 s of still pictures."""
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    (o,) = look_ticks(s, tank, t, 1, glimpses=[glimpse_at(tank, 30.0, 60.0)])
    assert s.state == "second" and o.cmd == STOP
    assert o.cam is not None and math.isclose(o.cam[0], -45.0, abs_tol=0.5)
    outs = look_ticks(s, tank, t + 0.1, 14)
    assert s.state == "second" and all(x.cmd == STOP for x in outs)
    look_ticks(s, tank, t + 1.5, 3)
    assert s.state == "drive"


def test_a_maybe_in_the_second_look_is_a_candidate() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    look_ticks(s, tank, t, 1, glimpses=[glimpse_at(tank, 30.0, 60.0)])
    look_ticks(s, tank, t + 0.1, 1, maybes=[(0.8, 0.8)])
    assert [c.spot for c in s.candidates.pending()] == [(0.8, 0.8)]


def test_wide_glimpse_gets_two_looks() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    (o,) = look_ticks(s, tank, t, 1, glimpses=[glimpse_at(tank, -40.0, 40.0)])
    assert o.cam is not None and math.isclose(abs(o.cam[0]), 40.0, abs_tol=0.5)
    first = o.cam[0]
    pans = [x.cam[0] for x in look_ticks(s, tank, t + 0.1, 20) if x.cam is not None]
    assert pans and math.isclose(pans[0], -first, abs_tol=0.5)


def test_glimpse_behind_turns_first() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    (o,) = look_ticks(s, tank, t, 1, glimpses=[glimpse_at(tank, 140.0, 160.0)])
    assert s.state == "second" and o.cmd.w_dps > 0.0  # turning left, toward it
    outs = look_ticks(s, tank, t + 0.1, 40)
    cams = [x.cam for x in outs if x.cam is not None]
    assert cams and abs(cams[0][0]) <= 90.0


def test_the_same_glimpse_again_is_ignored() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    g = glimpse_at(tank, 30.0, 60.0)  # map bearings: the same direction whichever way the tank faces later
    look_ticks(s, tank, t, 1, glimpses=[g])
    look_ticks(s, tank, t + 0.1, 20)
    assert s.state == "drive"
    look_ticks(s, tank, t + 5.0, 1, glimpses=[replace(g, at=tank.at[:2])])
    assert s.state != "second"


def test_a_glimpse_45_degrees_away_is_looked_at_after_3_s() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    g = glimpse_at(tank, 30.0, 60.0)
    other = replace(g, lo_rad=g.lo_rad - math.radians(45.0), hi_rad=g.hi_rad - math.radians(45.0))
    look_ticks(s, tank, t, 1, glimpses=[g])
    look_ticks(s, tank, t + 0.1, 20)
    look_ticks(s, tank, t + 2.2, 1, glimpses=[other])
    assert s.state != "second"  # within 3 s of the last second look
    look_ticks(s, tank, t + 3.2, 1, glimpses=[other])
    assert s.state == "second"


def test_no_second_look_during_the_close_look() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    look_ticks(s, tank, t, 1, maybes=[(0.3, 0.0)])  # near: the check looks at once
    assert s.state == "check" and s._check is not None and s._check.phase == "look"  # pyright: ignore[reportPrivateUsage]
    look_ticks(s, tank, t + 0.1, 1, glimpses=[glimpse_at(tank, 30.0, 60.0)])
    assert s.state == "check"
