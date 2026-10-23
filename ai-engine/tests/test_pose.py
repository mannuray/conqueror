"""PoseFilter revision 2 and PoseHistory (AI engine LLD §14, design-notes §132): the tank's corrected heading
plus the encoders, with the PWM sign per track."""

from __future__ import annotations

import math
from dataclasses import replace

from conqueror_ai.domain.pose import PoseFilter, PoseHistory
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import Pose, TelemSample

CPM = 7514
STALL_L, STALL_R = 1 << 3, 1 << 4


def td(
    t_ms: float,
    enc: tuple[int, int],
    pwm: tuple[int, int],
    yaw: float,
    uno_ms: int | None = None,
    flags: int = 0,
) -> TelemSample:
    """A td sample at bridge time t_ms (the Uno's clock runs alongside unless given)."""
    return TelemSample(
        bus_us=int(t_ms * 1000),
        uno_ms=int(t_ms) if uno_ms is None else uno_ms,
        enc_l=enc[0],
        enc_r=enc[1],
        yaw_deg=yaw % 360.0,
        rate_dps=0.0,
        dist_mm=None,
        pan_deg=90.0,
        tilt_deg=90.0,
        cmd_l=0,
        cmd_r=0,
        flags=flags,
        pwm_l=pwm[0],
        pwm_r=pwm[1],
    )


def drive(
    f: PoseFilter,
    steps: int,
    d_counts: tuple[int, int],
    pwm: tuple[int, int],
    dyaw: float = 0.0,
    t0: float = 0.0,
    start: tuple[int, int] = (0, 0),
    yaw0: float = 0.0,
) -> Pose:
    """`steps` samples 20 ms apart, each adding d_counts and dyaw; returns the last pose."""
    p = None
    for i in range(steps + 1):
        p = f.update(
            td(t0 + 20 * i, (start[0] + d_counts[0] * i, start[1] + d_counts[1] * i), pwm, yaw0 + dyaw * i)
        )
    assert p is not None
    return p


def new() -> PoseFilter:
    f = PoseFilter()
    f.set_counts_per_m(CPM)
    return f


def test_straight_one_metre() -> None:
    p = drive(new(), 100, (75, 75), (120, 120))  # 7500 counts on each track
    assert math.isclose(p.x_m, 7500 / CPM, abs_tol=1e-9) and abs(p.y_m) < 1e-9 and p.th_rad == 0.0


def test_backwards() -> None:
    p = drive(new(), 100, (75, 75), (-120, -120))
    assert math.isclose(p.x_m, -7500 / CPM, abs_tol=1e-9)


def test_a_left_turn_while_driving_bends_the_path_left() -> None:
    p = drive(new(), 90, (75, 75), (120, 120), dyaw=1.0)  # 90 deg to the left over ~0.9 m
    assert math.isclose(math.degrees(p.th_rad), 90.0, abs_tol=1e-6)
    assert p.y_m > 0.5 and p.x_m > 0.5


def test_a_pivot_goes_nowhere() -> None:
    p = drive(new(), 50, (40, 40), (120, -120), dyaw=1.8)
    assert abs(p.x_m) < 1e-9 and abs(p.y_m) < 1e-9
    assert math.isclose(math.degrees(p.th_rad), 90.0, abs_tol=1e-6)


def test_coasting_keeps_the_last_direction() -> None:
    f = new()
    drive(f, 10, (75, 75), (-120, -120))
    p = drive(f, 10, (75, 75), (0, 0), t0=220, start=(750, 750))  # motor off, tracks still rolling back
    assert math.isclose(p.x_m, -(750 + 750) / CPM, abs_tol=1e-9)


def test_a_stalled_track_adds_nothing() -> None:
    f = new()
    p = None
    for i in range(11):
        p = f.update(td(20 * i, (75 * i, 75 * i), (120, 120), 0.0, flags=STALL_L | STALL_R))
    assert p is not None and p.x_m == 0.0


def test_the_heading_wraps_without_a_jump() -> None:
    p = drive(new(), 4, (0, 0), (0, 0), dyaw=0.5, yaw0=359.0)  # 359 -> 1 deg
    assert math.isclose(math.degrees(p.th_rad), 2.0, abs_tol=1e-6)


def test_an_uno_restart_keeps_the_pose() -> None:
    f = new()
    before = drive(f, 50, (75, 75), (120, 120), dyaw=0.4)
    # the Uno restarts: its clock, counts and heading start again from 0
    after = f.update(td(1020, (0, 0), (0, 0), 0.0, uno_ms=5))
    assert after is not None
    assert (after.x_m, after.y_m, after.th_rad) == (before.x_m, before.y_m, before.th_rad)
    nxt = f.update(td(1040, (75, 75), (120, 120), 0.0, uno_ms=25))
    assert nxt is not None and nxt.x_m > before.x_m  # and it carries on from there


def test_a_gap_is_not_stepped_across_and_grows_the_drift() -> None:
    f = new()
    x = drive(f, 10, (75, 75), (120, 120)).x_m
    d0 = f.drift()[0]
    p = f.update(td(1200, (5000, 5000), (120, 120), 0.0))  # 1 s silent, then far further on
    assert p is not None and p.x_m == x
    assert f.drift()[0] >= d0 + 0.3 - 1e-9


def test_no_counts_per_m_holds_the_position() -> None:
    p = drive(PoseFilter(), 50, (75, 75), (120, 120), dyaw=0.2)
    assert p.x_m == 0.0 and p.y_m == 0.0 and math.isclose(math.degrees(p.th_rad), 10.0, abs_tol=1e-6)


def test_the_drift_grows_with_the_distance_and_the_turn() -> None:
    f = new()
    drive(f, 500, (75, 75), (120, 120))  # ~5 m
    assert math.isclose(f.drift()[0], 0.03 * 500 * 150 / 2 / CPM, rel_tol=1e-6)
    f2 = new()
    drive(f2, 360, (0, 0), (120, -120), dyaw=1.0)  # one full turn on the spot
    assert math.isclose(f2.drift()[1], 1.0, rel_tol=1e-6)


def test_the_speed_and_turn_rate() -> None:
    f = new()
    p = drive(f, 50, (75, 75), (120, 120))
    assert math.isclose(p.v_mps, 75 / CPM / 0.02, rel_tol=1e-6)
    s = td(1020, (3825, 3825), (120, 120), 0.0)
    q = f.update(replace(s, rate_dps=90.0))
    assert q is not None and math.isclose(q.w_rps, math.pi / 2)


def test_history_interpolates_and_knows_its_window() -> None:
    h = PoseHistory()
    h.push(Pose(1_000_000, 0.0, 0.0, math.radians(179.0), 0.0, 0.0))
    h.push(Pose(1_020_000, 0.2, 0.0, math.radians(-179.0), 0.0, 0.0))
    p = h.at(1_010_000)
    assert p is not None and math.isclose(p.x_m, 0.1)
    assert math.isclose(abs(math.degrees(p.th_rad)), 180.0, abs_tol=1e-6)  # across +-180, not through 0
    assert h.at(900_000) is None and h.at(1_030_000) is None


def test_history_keeps_two_seconds() -> None:
    h = PoseHistory()
    for i in range(150):
        h.push(Pose(i * 20_000, float(i), 0.0, 0.0, 0.0, 0.0))
    assert h.at(0) is None and h.at(149 * 20_000) is not None
    oldest = h.at(50 * 20_000)
    assert oldest is not None and oldest.x_m == 50.0


def test_the_parser_reads_the_pwm() -> None:
    s = parse_td(
        {
            "t": "td",
            "seq": 1,
            "uno_ms": 5,
            "bus_us": 9,
            "enc": [1, 2],
            "pwm": [-80, 95],
            "cmd": [0, 0],
            "yaw": 1.0,
            "rate": 0.0,
            "dist_mm": None,
            "pan": 90.0,
            "tilt": 90.0,
            "flags": 0,
        }
    )
    assert s is not None and (s.pwm_l, s.pwm_r) == (-80, 95)


def test_the_counts_go_with_the_output_they_happened_under() -> None:
    """A sample's count change happened during the 20 ms before it, under the previous sample's PWM."""
    f = new()
    f.update(td(0, (0, 0), (120, 120), 0.0))
    p = f.update(
        td(20, (75, 75), (120, -120), 0.0)
    )  # the output just changed to a pivot; the 75 were forward
    assert math.isclose(p.x_m, 75 / CPM, abs_tol=1e-12)


# ---- Plan U final review
def test_after_a_bridge_restart_the_speed_and_history_work_again() -> None:  # I1
    f, h = new(), PoseHistory()
    for i in range(51):  # an hour into the bridge's uptime
        h.push(f.update(td(3_600_000 + 20 * i, (75 * i, 75 * i), (120, 120), 0.0, uno_ms=10_000 + 20 * i)))
    for i in range(51):  # the bridge restarted: its clock starts again (the Uno's runs on)
        h.push(
            f.update(
                td(1_000 + 20 * i, (3825 + 75 * i, 3825 + 75 * i), (120, 120), 0.0, uno_ms=12_000 + 20 * i)
            )
        )
    p = f.pose()
    assert p is not None and math.isclose(p.v_mps, 75 / CPM / 0.02, rel_tol=1e-6)
    assert h.at((1_000 + 20 * 50) * 1000) is not None and h.at((1_000 + 20 * 25) * 1000) is not None  # µs


def test_a_gap_keeps_the_turn_made_during_it() -> None:  # I2: the heading is absolute within one Uno run
    f = new()
    drive(f, 10, (0, 0), (0, 0))
    d0 = f.drift()[1]
    p = f.update(td(800, (0, 0), (120, -120), 90.0))  # 0.6 s silent, turned 90 deg meanwhile
    assert math.isclose(math.degrees(p.th_rad), 90.0, abs_tol=1e-6)
    assert f.drift()[1] > d0


def test_a_long_fast_gap_marks_the_heading_unsure() -> None:  # I2: the wrap could hide a whole turn
    f = new()
    f.update(td(0, (0, 0), (120, -120), 0.0))
    q = f.update(replace(td(3000, (0, 0), (120, -120), 90.0), rate_dps=200.0))  # 3 s at 200 deg/s
    assert q is not None and f.drift()[1] >= 360.0


def test_the_heading_is_kept_between_minus_and_plus_pi() -> None:  # M2: one convention for every Pose
    p = drive(new(), 360, (0, 0), (120, -120), dyaw=2.0)  # two whole turns
    assert -math.pi <= p.th_rad < math.pi and abs(p.th_rad) < 1e-6


def test_a_bad_counts_per_m_is_ignored() -> None:  # M3
    f = PoseFilter()
    f.set_counts_per_m(0)
    p = drive(f, 10, (75, 75), (120, 120))
    assert p.x_m == 0.0


def test_a_pivot_back_to_forward_counts_the_change_right() -> None:  # M5
    f = new()
    f.update(td(0, (0, 0), (-120, -120), 0.0))
    f.update(td(20, (75, 75), (0, 0), 0.0))  # coasting backwards
    p = f.update(td(40, (150, 150), (120, 120), 0.0))  # the output turned forward; these 75 were still back
    assert math.isclose(p.x_m, -150 / CPM, abs_tol=1e-12)
    q = f.update(td(60, (225, 225), (120, 120), 0.0))
    assert math.isclose(q.x_m, -75 / CPM, abs_tol=1e-12)
