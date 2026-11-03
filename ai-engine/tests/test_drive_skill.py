"""The skill interface: SkillInput, Result, the Skill protocol (AI engine LLD §19.4, review F3)."""

from __future__ import annotations

import numpy as np

from conqueror_ai.domain.depth import Marks
from conqueror_ai.domain.drive import chase as ch
from conqueror_ai.domain.drive.curves import STOP
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.skill import Result, Skill, make_input
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap
from conqueror_ai.domain.types import Pose


def open_snap() -> MapSnapshot:
    r = RoomMap()
    r.seen[:] = True
    z = np.zeros((0, 2))
    r.update_picture(Marks(z, z, z, z), 0)
    return r.snapshot(0, Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), False, True)


def test_results_are_their_old_strings() -> None:
    """Logs, the CLI and (Plan Z2) skill_failed read the same words as before."""
    assert [r.value for r in Result] == [
        "done",
        "not_found",
        "lost",
        "blocked",
        "stale",
        "cancelled",
        "refused",
        "failed",
        "outside_area",
        "no_way",
        "unknown_thing",
    ]
    assert Result.NOT_FOUND == "not_found"


def test_make_input_builds_cost_maps_only_with_a_map() -> None:
    snap = open_snap()
    inp = make_input(at=(0.0, 0.0, 0.0), snap=snap, area=(0.0, 0.0, 1.0), map_ok=True)
    assert inp.costs is not None and inp.costs.snap is snap and inp.costs.area == (0.0, 0.0, 1.0)
    assert make_input(at=(0.0, 0.0, 0.0), snap=None, map_ok=True).costs is None


def test_the_chase_is_a_skill() -> None:
    assert isinstance(ch.ChaseSkill(Planner(Limits())), Skill)


def test_the_chase_has_no_map_free_path() -> None:
    """No GOTO (design-notes §191): every skill drives on the map. With no map it stops, as when stale."""
    assert not hasattr(ch, "BLOCKED_GIVE_UP_S")
    s = ch.ChaseSkill(Planner(Limits()))
    out = s.tick(0.0, make_input(at=(0.0, 0.0, 0.0), snap=None, fix=TargetFix(1, 2.0, 0.0, 0.0), map_ok=True))
    assert out.cmd == STOP and out.result is None
    out = s.tick(2.0, make_input(at=(0.0, 0.0, 0.0), snap=None, fix=TargetFix(1, 2.0, 0.0, 0.0), map_ok=True))
    assert out.result == Result.STALE
