"""The chase skill's states (AI engine LLD §17.6)."""

from __future__ import annotations

import pytest

from conqueror_ai.domain.drive import chase as ch
from conqueror_ai.domain.drive.curves import STOP
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner, bearing_deg

LIM = Limits()


# ---- the chase skill
def skill() -> ch.ChaseSkill:
    return ch.ChaseSkill(Planner(LIM))


def test_no_target_starts_the_camera_sweep_then_the_spin_then_not_found() -> None:
    s = skill()
    outs = [s.tick(i * 0.1, (0.0, 0.0, 0.0), None, None, True) for i in range(40)]
    pans = [o.cam[0] for o in outs if o.cam is not None]
    assert pans[:5] == [-90.0, -45.0, 0.0, 45.0, 90.0] and all(o.cmd == STOP for o in outs[:39])
    outs = [s.tick(4.0 + i * 0.1, (0.0, 0.0, 0.0), None, None, True) for i in range(70)]
    assert outs[5].cmd.w_dps == LIM.spin_dps
    ends = [i for i, o in enumerate(outs) if o.result is not None]
    assert [outs[i].result for i in ends] == ["not_found"]  # once, after one full turn (360 / 65 = 5.5 s)
    assert 54 <= ends[0] <= 57


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
