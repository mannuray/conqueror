"""The tank's limits, the ramp and the roll-out (AI engine LLD §17.2, §17.4)."""

from __future__ import annotations

import math

import pytest

from conqueror_ai.domain.drive.curves import STOP, Command, ramp, roll_out
from conqueror_ai.domain.drive.limits import Limits

LIM = Limits()


# ---- the limits (design-notes §71)
def test_the_tightest_smooth_arc_by_speed() -> None:
    assert math.isinf(LIM.r_min_m(17.0))
    assert LIM.r_min_m(18.0) > 1.5  # near the inner track's lowest speed: straight only
    assert LIM.r_min_m(20.0) == pytest.approx(0.826, abs=0.01)
    assert LIM.r_min_m(25.0) == pytest.approx(0.325, abs=0.01)
    assert LIM.r_min_m(33.0) == pytest.approx(0.31)  # never under the 31 cm floor


def test_the_top_speed_falls_near_the_stop() -> None:
    assert LIM.top_speed(1.5) == 25.0
    assert LIM.top_speed(0.5) == 18.0
    assert 18.0 < LIM.top_speed(0.65) < 25.0


def test_the_body_is_the_measured_tank() -> None:
    """The owner's tape from the turning centre (design-notes §163): front 5 in, rear 3.5 in, width 9 in;
    each rounded up to the half centimetre."""
    assert (LIM.body_front_m, LIM.body_rear_m, LIM.body_half_w_m) == (0.13, 0.09, 0.115)
    assert LIM.body_front_m >= 5 * 0.0254 and LIM.body_rear_m >= 3.5 * 0.0254
    assert LIM.body_half_w_m >= 9 / 2 * 0.0254


# ---- the ramp
def test_the_ramp_crosses_the_dead_band_at_once_and_then_climbs_gently() -> None:
    c = ramp(STOP, Command(25.0, 0.0), 0.1, LIM)
    assert c.v_cms == 18.0  # 0 -> v_min in one step: the tracks can't go slower
    c = ramp(c, Command(25.0, 0.0), 0.1, LIM)
    assert c.v_cms == pytest.approx(19.0)


def test_the_ramp_turns_gently_and_spins_are_full_or_none() -> None:
    c = ramp(Command(25.0, 0.0), Command(25.0, 40.0), 0.1, LIM)
    assert c.w_dps == pytest.approx(6.0)
    assert ramp(STOP, Command(0.0, 65.0), 0.1, LIM).w_dps == 65.0
    assert ramp(STOP, Command(0.0, 30.0), 0.1, LIM).w_dps == 0.0  # a slow spin isn't smooth: none
    assert ramp(Command(0.0, 65.0), STOP, 0.1, LIM) == STOP


def test_the_ramp_never_asks_for_an_arc_tighter_than_the_tank_can_drive() -> None:
    def w_max(v: float) -> float:
        return math.degrees(v / 100.0 / LIM.r_min_m(v))

    c = ramp(Command(18.0, 0.0), Command(18.0, 40.0), 0.1, LIM)
    assert c.v_cms == 18.0 and abs(c.w_dps) <= w_max(18.0) + 1e-9  # at 18 cm/s barely a curve at all
    c = ramp(Command(25.0, 40.0), Command(18.0, 40.0), 0.1, LIM)  # slowing in a curve: the curve opens
    assert c.v_cms == pytest.approx(24.0) and abs(c.w_dps) <= w_max(24.0) + 1e-9
    c = ramp(Command(0.0, 65.0), Command(18.0, -40.0), 0.1, LIM)  # out of a spin into a drive
    assert abs(c.w_dps) <= w_max(18.0) + 1e-9


def test_a_roll_out_follows_the_ramp() -> None:
    path = roll_out((0.0, 0.0, 0.0), STOP, Command(25.0, 0.0), LIM)
    assert len(path) == 15 and path[-1][0] == pytest.approx(0.347, abs=0.005) and path[-1][1] == 0.0
