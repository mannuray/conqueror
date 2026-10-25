"""The chase skill's states (AI engine LLD §17.6)."""

from __future__ import annotations

import math

import pytest

from conqueror_ai.domain.drive import chase as ch
from conqueror_ai.domain.drive.curves import STOP, Command
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg

LIM = Limits()


# ---- the chase skill
def skill() -> ch.ChaseSkill:
    return ch.ChaseSkill(Planner(LIM))


def test_no_target_sweeps_the_camera_turns_half_round_sweeps_again_then_not_found() -> None:
    s = skill()
    outs = [s.tick(i * 0.1, (0.0, 0.0, 0.0), None, None, True) for i in range(40)]
    pans = [o.cam[0] for o in outs if o.cam is not None]
    assert pans[:5] == [-90.0, -45.0, 0.0, 45.0, 90.0] and all(o.cmd == STOP for o in outs[:39])
    outs = [s.tick(4.0 + i * 0.1, (0.0, 0.0, 0.0), None, None, True) for i in range(80)]
    assert outs[5].cmd.w_dps == LIM.spin_dps
    # half a turn (180 / 65 = 2.8 s): the map can't use pictures while spinning (faster than 45 deg/s),
    # so the tank stops and sweeps the camera again over what was behind it
    spun = [i for i, o in enumerate(outs) if o.cmd.w_dps != 0]
    assert 27 <= len(spun) <= 29 and outs[spun[-1] + 1].cmd == STOP
    pans = [o.cam[0] for o in outs[spun[-1] + 1 :] if o.cam is not None]
    assert pans[:5] == [-90.0, -45.0, 0.0, 45.0, 90.0]
    ends = [i for i, o in enumerate(outs) if o.result is not None]
    assert [outs[i].result for i in ends] == ["not_found"]  # once, after the second sweep
    assert 67 <= ends[0] <= 70


def test_the_ramp_follows_the_real_time_between_ticks() -> None:
    s = skill()
    fix = TargetFix(1, 3.0, 0.0, 0.0)
    assert s.tick(0.0, (0.0, 0.0, 0.0), fix, None, True).cmd.v_cms == LIM.v_min_cms
    # the real loop ticks once per picture (~0.2 s at VGA), not every 0.1 s
    assert s.tick(0.2, (0.0, 0.0, 0.0), fix, None, True).cmd.v_cms == pytest.approx(LIM.v_min_cms + 2.0)


def test_near_the_ball_but_off_to_the_side_it_turns_on_the_spot_to_face_it() -> None:
    s = skill()
    b = math.radians(20.0)
    d = LIM.stop_range_m + LIM.cam_x_m + 0.02  # in arrival range from the lens, 20 deg to the left
    out = s.tick(0.0, (0.0, 0.0, 0.0), TargetFix(1, d * math.cos(b), d * math.sin(b), 0.0), None, True)
    assert out.cmd == Command(0.0, LIM.spin_dps) and out.result is None


def test_a_stale_map_or_no_pose_ends_it_at_once() -> None:
    assert skill().tick(0.0, (0.0, 0.0, 0.0), None, None, False).result == "stale"
    assert skill().tick(0.0, None, None, None, True).result == "stale"


def test_something_close_ahead_that_isnt_the_ball_ends_it_blocked() -> None:
    s = skill()
    fix = TargetFix(1, 2.0, 0.0, 0.0)
    assert s.tick(0.0, (0.0, 0.0, 0.0), fix, 0.20, True).result == "blocked"


def test_a_target_unseen_for_3_s_starts_looking_again() -> None:
    s = skill()
    s.tick(0.0, (0.0, 0.0, 0.0), TargetFix(1, 2.0, 0.0, 0.0), None, True)
    out = s.tick(0.1, (0.0, 0.0, 0.0), TargetFix(1, 2.0, 0.0, 3.5), None, True)
    assert s.state == "sweep" and out.cam == (0.0, ch.DRIVE_TILT)


def test_cancel_stops_at_once() -> None:
    s = skill()
    s.tick(0.0, (0.0, 0.0, 0.0), TargetFix(1, 2.0, 0.0, 0.0), None, True)
    out = s.cancel()
    assert out.cmd == STOP and out.result == "cancelled"
    assert s.tick(0.1, (0.0, 0.0, 0.0), TargetFix(1, 2.0, 0.0, 0.0), None, True).cmd == STOP


def test_the_camera_follows_the_target_in_steps() -> None:
    s = skill()
    out = s.tick(0.0, (0.0, 0.0, 0.0), TargetFix(1, 1.5, 0.5, 0.0), None, True)
    assert out.cam is not None and out.cam[0] == pytest.approx(-bearing_deg((0.0, 0.0, 0.0), (1.5, 0.5)))
    assert (
        s.tick(0.1, (0.0, 0.0, 0.0), TargetFix(1, 1.5, 0.52, 0.0), None, True).cam is None
    )  # < 8 deg: no move
