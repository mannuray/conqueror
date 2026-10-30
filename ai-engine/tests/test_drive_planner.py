"""The planner: the fan of curves, scored (AI engine LLD §17.4)."""

from __future__ import annotations

import math

import numpy as np
import pytest

from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.curves import STOP, Command, roll_out
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg
from conqueror_ai.domain.room_map import RoomMap

LIM = Limits()


# ---- the planner
P = Planner(LIM)


def test_the_fan_never_offers_what_the_tank_cant_do() -> None:
    for target in [(1.5, 0.0), (0.6, 0.3), (1.0, 1.0), (-1.0, 0.2)]:
        for c in P.fan((0.0, 0.0, 0.0), target):
            if c.v_cms > 0:
                assert c.v_cms >= LIM.v_min_cms
                if c.w_dps != 0:
                    assert c.v_cms / 100 / math.radians(abs(c.w_dps)) >= LIM.r_min_m(c.v_cms) - 1e-9
            elif c.w_dps != 0:
                assert abs(c.w_dps) == LIM.spin_dps


def test_the_planner_picks_the_obvious_curve() -> None:
    at, cruise = (0.0, 0.0, 0.0), Command(25.0, 0.0)
    assert P.choose(at, cruise, (2.0, 0.0)) == Command(25.0, 0.0)  # ahead: straight on at cruise
    assert P.choose(at, cruise, (1.5, 1.2)).w_dps > 0  # ahead-left: a left arc
    assert P.choose(at, cruise, (1.5, -1.2)).w_dps < 0
    spin = P.choose(at, STOP, (-1.5, 0.3))
    assert spin.v_cms == 0 and spin.w_dps == LIM.spin_dps  # behind (a little left): turn on the spot
    assert P.choose(at, Command(25.0, 0.0), (0.6, 0.0)).v_cms < 25.0  # close: slower


def test_a_target_inside_the_tightest_circle_is_turned_to_on_the_spot() -> None:
    assert P.inside_turn((0.0, 0.0, 0.0), (0.1, 0.3))
    assert not P.inside_turn((0.0, 0.0, 0.0), (1.5, 0.3))
    assert P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (0.1, 0.3)).v_cms == 0


def test_a_target_well_to_one_side_is_turned_to_on_the_spot_first() -> None:
    """Floor chase 5 (the ball 60 cm ahead, 50 cm right of the lens): a 50 cm curve at cruise swept 59 cm past
    it. More than 35 deg off, the tank turns on the spot first, from standing or while driving."""
    ball = (0.085 + 0.60, -0.50)
    assert P.choose((0.0, 0.0, 0.0), STOP, ball) == Command(0.0, -LIM.spin_dps)
    assert P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), ball) == Command(0.0, -LIM.spin_dps)
    assert P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (1.5, -0.6)).v_cms > 0  # 22 deg off: an arc


def test_a_spin_ends_15_deg_early_the_tank_coasts_on() -> None:
    """Floor chase 6: spinning until 8 deg off turned 55 deg for a ball 40 deg off (the pose lags and the tank
    coasts after the stop). The spin ends with 15 deg left; the drive's arc aims the rest."""
    spinning = Command(0.0, -LIM.spin_dps)
    for deg, spins in [(-16.0, True), (-14.0, False)]:
        ball = (math.cos(math.radians(deg)), math.sin(math.radians(deg)))
        assert (P.choose((0.0, 0.0, 0.0), spinning, ball) == spinning) is spins


def test_an_echo_ahead_that_is_not_the_target_is_kept_clear_of() -> None:
    """Slow down rather than stop (spec 8 §3): the chosen curve keeps clear_m from the echo."""
    c = P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (2.0, 0.0), obstacle_m=0.6)
    path = roll_out((0.0, 0.0, 0.0), Command(25.0, 0.0), c, LIM)
    assert min(math.hypot(x - 0.6, y) for x, y, _ in path) >= LIM.clear_m
    assert P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (2.0, 0.0), obstacle_m=0.2) == STOP  # all blocked
    assert P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (2.0, 0.0), obstacle_m=None).w_dps == 0


def test_bearing_is_plus_left() -> None:
    assert bearing_deg((0.0, 0.0, 0.0), (0.0, 1.0)) == pytest.approx(90.0)
    assert bearing_deg((0.0, 0.0, math.pi), (1.0, 0.0)) == pytest.approx(-180.0)


def test_keep_clear_is_measured_from_the_tanks_front_like_the_sonar() -> None:
    p = Planner(LIM)
    turn = Command(25.0, 40.0)  # a curve to the left
    path = roll_out((0.0, 0.0, 0.0), turn, turn, LIM)
    fx = LIM.sonar_x_m

    def gap(ox: float, at_front: bool) -> float:  # the nearest pass of the front (or the centre) by an echo
        off = fx if at_front else 0.0
        return min(math.hypot(x + off * math.cos(t) - ox, y + off * math.sin(t)) for x, y, t in path)

    # an echo the front passes within clear_m while the centre stays beyond it
    ox = next(o / 100 for o in range(15, 80) if gap(o / 100, True) < LIM.clear_m <= gap(o / 100, False))
    free = p.score((0.0, 0.0, 0.0), turn, turn, (0.0, 2.0), None)
    assert p.score((0.0, 0.0, 0.0), turn, turn, (0.0, 2.0), ox) == pytest.approx(free + p.w.blocked)


# ---- the fan on the cost map (AI engine LLD §18.13.3, Plan Z6)
def map_with(blocked: list[tuple[float, float]], unseen: bool = False) -> CostMap:
    room = RoomMap()
    room.seen[:] = not unseen
    if blocked:
        c = room.cells(np.array(blocked, dtype=np.float64))
        room.hit(c, "low")
        room.hit(c, "low")
        room.seen.reshape(-1)[c] = True
    return CostMap(room.snapshot(0, None, False, True))


def area(x0: float, y0: float, x1: float, y1: float) -> list[tuple[float, float]]:
    return [
        (float(x), float(y)) for x in np.arange(x0, x1 + 1e-9, 0.025) for y in np.arange(y0, y1 + 1e-9, 0.025)
    ]


def test_the_fan_keeps_off_marked_obstacles() -> None:
    cost = map_with(area(0.40, -0.15, 0.60, 0.15))  # inside the 1.5 s look-ahead (37 cm at cruise)
    c = P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (2.0, 0.0), cost=cost)
    assert c != Command(25.0, 0.0)
    path = roll_out((0.0, 0.0, 0.0), Command(25.0, 0.0), c, LIM)
    assert P.foot.allowed(cost, (0.0, 0.0, 0.0), path)


def test_all_ways_blocked_stops() -> None:
    ring = area(-0.4, 0.15, 0.4, 0.4) + area(-0.4, -0.4, 0.4, -0.15) + area(0.15, -0.4, 0.4, 0.4)
    ring += area(-0.4, -0.4, -0.15, 0.4)
    assert P.choose((0.0, 0.0, 0.0), Command(25.0, 0.0), (2.0, 0.0), cost=map_with(ring)) == STOP


def test_an_empty_map_changes_nothing() -> None:  # review minor 6: the old test compared a call with itself
    at, cruise, empty = (0.0, 0.0, 0.0), Command(25.0, 0.0), map_with([])
    for target in [(2.0, 0.0), (1.5, 1.2), (1.5, -1.2), (0.6, 0.0), (-1.5, 0.3)]:
        for now in (cruise, STOP):
            assert P.choose(at, now, target) == P.choose(at, now, target, cost=empty)


def test_speed_comes_from_the_destination_not_the_aim_point() -> None:
    """On a route the fan steers at a point 0.5 m ahead; slowing is for the end of the way (Plan Z5)."""
    at, cruise = (0.0, 0.0, 0.0), Command(25.0, 0.0)
    assert P.choose(at, cruise, (0.5, 0.0)).v_cms < 25.0  # as before: a near target slows it
    assert P.choose(at, cruise, (0.5, 0.0), far_m=2.0) == Command(25.0, 0.0)
    assert P.choose(at, cruise, (0.5, 0.0), far_m=0.45) == P.choose(at, cruise, (0.5, 0.0))  # near the end


def test_at_the_slowest_speed_a_target_nearly_ahead_is_driven_to_straight() -> None:
    """At 18 cm/s the tank can't curve at all, so every target slightly off-centre lay 'inside the tightest
    turn': it turned on the spot, overshot, turned back - for ever (Plan Z5 simulated explore). Within
    SPIN_UNTIL_DEG it drives straight; further off it still turns first."""
    slow = Planner(Limits(v_cap_cms=18.0))
    for deg in (4.0, -9.0, 14.0):
        t = (0.5 * math.cos(math.radians(deg)), 0.5 * math.sin(math.radians(deg)))
        assert slow.choose((0.0, 0.0, 0.0), STOP, t).v_cms > 0
    t = (0.5 * math.cos(math.radians(25.0)), 0.5 * math.sin(math.radians(25.0)))
    assert slow.choose((0.0, 0.0, 0.0), STOP, t).v_cms == 0


def test_a_spin_from_driving_coasts_and_is_checked_too() -> None:
    """From 25 cm/s a turn on the spot first coasts ~17 cm while the speed ramps down: that coast must be
    checked against the map and the echo like any curve (Plan Z5: it was not; it coasted into a wall)."""
    cost = map_with(area(0.25, -1.5, 0.30, 1.5))  # a wall 25 cm ahead
    spin = Command(0.0, LIM.spin_dps)
    free = P.score((0.0, 0.0, 0.0), STOP, spin, (-1.0, 0.0), None, map_with([]))
    coasting = P.score((0.0, 0.0, 0.0), Command(25.0, 0.0), spin, (-1.0, 0.0), None, cost)
    assert coasting <= free + P.w.blocked + 1e-9


def test_the_tanks_front_is_kept_clear_not_only_its_centre() -> None:
    """The front is 13 cm ahead of the centre: straight at a wall the front's keep-out stops it first."""
    cost = map_with(area(0.40, -1.5, 0.45, 1.5))  # a wall 40 cm ahead
    straight = Command(25.0, 0.0)  # in 1.5 s: 37 cm, the front to 50 cm
    free = P.score((0.0, 0.0, 0.0), straight, straight, (2.0, 0.0), None, map_with([]))
    assert P.score((0.0, 0.0, 0.0), straight, straight, (2.0, 0.0), None, cost) <= free + P.w.blocked + 1e-9


def test_a_turn_on_the_spot_too_near_something_is_refused() -> None:
    """Turning on the spot sweeps the corners (~17 cm out): with a wall 15 cm off, both ways are refused and
    the fan offers forward curves instead (drive out first, design-notes §163.1)."""
    beside = map_with(area(-0.6, 0.15, 0.6, 0.3))  # cells from 15 cm to the left of the centre
    assert not P.can_spin((0.0, 0.0, 0.0), beside, True) and not P.can_spin((0.0, 0.0, 0.0), beside, False)
    c = P.choose((0.0, 0.0, 0.0), STOP, (-1.5, 0.3), cost=beside)
    assert not (c.v_cms == 0 and c.w_dps != 0)
    assert P.can_spin((0.0, 0.0, 0.0), map_with([]))
    assert P.choose((0.0, 0.0, 0.0), STOP, (-1.5, 0.3), cost=map_with([])).w_dps != 0


def test_a_wider_body_keeps_its_sides_clear() -> None:
    """drive.yaml may make the body wider (design-notes §163): the outline is the body, so the keep-out
    follows it. A shelf ahead-left 22.5 cm from the line straight ahead (cell centres)."""
    shelf = map_with(area(0.30, 0.225, 1.5, 0.4))
    at, straight = (0.0, 0.0, 0.0), Command(25.0, 0.0)
    free_ok = P.score(at, straight, straight, (2.0, 0.0), None, shelf) > P.w.blocked / 2
    wide = Planner(Limits(body_half_w_m=0.16))
    assert free_ok and wide.score(at, straight, straight, (2.0, 0.0), None, shelf) <= wide.w.blocked / 2


def test_beside_the_bed_it_turns_away_not_into_it() -> None:
    """A turn is checked the way it turns: a panel ~22.5 cm to the right (cell centres) and a leg ahead-
    right - turning left (the rear swings toward the panel, but stays 7.5 cm off) is allowed, right (the front
    into the leg) refused (design-notes §164)."""
    bed = map_with(area(-0.6, -0.40, 0.10, -0.225) + area(0.20, -0.40, 0.30, -0.175))
    assert P.can_spin((0.0, 0.0, 0.0), bed, left=True)
    assert not P.can_spin((0.0, 0.0, 0.0), bed, left=False)
    c = P.choose((0.0, 0.0, 0.0), STOP, (-0.24, 0.38), cost=bed, far_m=math.inf)  # the way on behind-left
    assert c.v_cms == 0 and c.w_dps > 0


def test_speed_from_cost() -> None:
    """25 cm/s on open seen floor, 25 - 7 x the highest cost ahead elsewhere, never under 18 (§18.13.3)."""
    at, far = (0.0, 0.0, 0.0), (3.0, 0.0)
    assert P.choose(at, Command(25.0, 0.0), far, cost=map_with([]), far_m=math.inf).v_cms == 25.0
    unseen = P.choose(at, Command(25.0, 0.0), far, cost=map_with([], unseen=True), far_m=math.inf)
    assert unseen.v_cms == pytest.approx(25.0 - 7.0 * 0.3, abs=0.1)
    walls = map_with(area(-1.0, 0.25, 3.0, 0.4) + area(-1.0, -0.4, 3.0, -0.25))  # a corridor 50 cm wide
    near = P.choose(at, Command(25.0, 0.0), far, cost=walls, far_m=math.inf)
    assert 18.0 <= near.v_cms < 25.0


def test_cost_steers_away_from_things() -> None:
    """A target 18 deg left: on open floor the fan curves left; with a wall on that side (its cells 25 cm
    off) it keeps further from it, and slower."""
    at, now, target = (0.0, 0.0, 0.0), Command(25.0, 0.0), (1.5, 0.5)
    free = P.choose(at, now, target, cost=map_with([]), far_m=math.inf)
    walled = P.choose(at, now, target, cost=map_with(area(0.0, 0.25, 2.0, 0.40)), far_m=math.inf)
    assert free.w_dps > 0 and walled.w_dps < free.w_dps and walled.v_cms < free.v_cms


def test_stop_only_when_every_move_is_forbidden() -> None:  # review M8
    """The way on behind, no turn on the spot allowed (walls 15 cm each side): forward curves are allowed but
    dear and away from the aim - the fan still offers one (driving out), it doesn't stop."""
    # the right wall from -0.175: from -0.15 its cells overlapped the track (design-notes §171)
    corridor = map_with(area(-1.0, 0.15, 2.0, 0.3) + area(-1.0, -0.3, 2.0, -0.175))
    assert not P.can_spin((0.0, 0.0, 0.0), corridor, True) and not P.can_spin(
        (0.0, 0.0, 0.0), corridor, False
    )
    assert P.choose((0.0, 0.0, 0.0), STOP, (-1.5, 0.0), cost=corridor, far_m=math.inf).v_cms > 0
