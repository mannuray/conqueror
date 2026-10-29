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
    assert pans[:5] == [0.0, -45.0, 45.0, -90.0, 90.0] and all(o.cmd == STOP for o in outs[:39])
    outs = [s.tick(4.0 + i * 0.1, (0.0, 0.0, 0.0), None, None, True) for i in range(80)]
    assert outs[6].cmd.w_dps == LIM.spin_dps  # 5 positions x (0.1 s to see it still + 0.8 s look) = 4.5 s
    # half a turn (180 / 65 = 2.8 s): the map can't use pictures while spinning (faster than 45 deg/s),
    # so the tank stops and sweeps the camera again over what was behind it
    spun = [i for i, o in enumerate(outs) if o.cmd.w_dps != 0]
    assert 27 <= len(spun) <= 29 and outs[spun[-1] + 1].cmd == STOP
    pans = [o.cam[0] for o in outs[spun[-1] + 1 :] if o.cam is not None]
    assert pans[:5] == [0.0, -45.0, 45.0, -90.0, 90.0]
    ends = [i for i, o in enumerate(outs) if o.result is not None]
    assert [outs[i].result for i in ends] == ["not_found"]  # once, after the second sweep
    assert 76 <= ends[0] <= 80


def test_the_sweep_looks_ahead_first_and_counts_its_look_from_when_the_camera_is_still() -> None:
    s = skill()
    outs = [s.tick(i * 0.1, (0.0, 0.0, 0.0), None, None, True, cam_still=i >= 5) for i in range(14)]
    assert outs[0].cam == (0.0, ch.DRIVE_TILT)  # straight ahead first: a thing in front is found at once
    # moving 0.5 s, then 0.8 s of still pictures before the next position
    assert [o.cam for o in outs[1:13]] == [None] * 12 and outs[13].cam == (-45.0, ch.DRIVE_TILT)
    s = skill()
    outs = [s.tick(i * 0.1, (0.0, 0.0, 0.0), None, None, True, cam_still=False) for i in range(26)]
    assert outs[25].cam == (-45.0, ch.DRIVE_TILT)  # a camera that never settles: on after 2.5 s


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


def test_a_stale_map_or_no_pose_stops_the_tank_at_once_and_ends_it_after_two_seconds() -> None:
    """Floor chase 6: a 0.2 s WiFi gap ended a run 1 s before arriving. Now the tank stops at once (STOP, not
    a ramp: it can't see) and drives on when telemetry is back; only STALE_GIVE_UP_S of it ends the run."""
    s = skill()
    fix = TargetFix(1, 1.6, 0.0, 0.0)
    for i in range(10):
        s.tick(i * 0.1, (0.0, 0.0, 0.0), fix, None, True)
    assert s.cmd.v_cms > 0
    held = s.tick(1.0, (0.0, 0.0, 0.0), fix, None, False)
    assert held.cmd == STOP and held.result is None
    assert s.tick(1.3, None, fix, None, True) == held  # no pose is the same
    back = s.tick(1.4, (0.0, 0.0, 0.0), fix, None, True)
    assert back.cmd.v_cms > 0 and back.result is None  # telemetry back: drives on
    s.tick(2.0, (0.0, 0.0, 0.0), fix, None, False)
    assert s.tick(3.9, (0.0, 0.0, 0.0), fix, None, False).result is None
    assert s.tick(4.0, (0.0, 0.0, 0.0), fix, None, False).result == "stale"


def test_something_close_ahead_that_isnt_the_ball_ends_it_blocked() -> None:
    s = skill()
    fix = TargetFix(1, 2.0, 0.0, 0.0)
    assert s.tick(0.0, (0.0, 0.0, 0.0), fix, 0.12, True).result is None  # owner: 10 cm (design-notes §159)
    assert s.tick(0.1, (0.0, 0.0, 0.0), fix, 0.09, True).result == "blocked"


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


def test_standing_blocked_for_3_s_ends_it_blocked() -> None:
    """Floor chase 11: a box just beyond clear_m blocked every curve; the tank stood for 4 minutes.
    Going around is slice 2; until then (and when there's no way round) it ends after BLOCKED_GIVE_UP_S."""
    s = skill()
    fix = TargetFix(1, 2.0, 0.0, 0.0)
    outs = [s.tick(i * 0.1, (0.0, 0.0, 0.0), fix, 0.15, True) for i in range(40)]
    ends = [i for i, o in enumerate(outs) if o.result is not None]
    assert all(o.cmd == STOP for o in outs) and [outs[i].result for i in ends] == ["blocked"]
    assert 30 <= ends[0] <= 31
