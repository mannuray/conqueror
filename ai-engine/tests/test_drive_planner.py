"""The planner: the fan of curves, scored (AI engine LLD §17.4)."""

from __future__ import annotations

import math

import pytest

from conqueror_ai.domain.drive.curves import STOP, Command, roll_out
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg

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
    turn = Command(
        25.0, 40.0
    )  # a curve to the left whose front passes 29 cm from the echo (its centre 31 cm)
    free = p.score((0.0, 0.0, 0.0), turn, turn, (0.0, 2.0), None)
    assert p.score((0.0, 0.0, 0.0), turn, turn, (0.0, 2.0), 0.57) == pytest.approx(free + p.w.blocked)
