"""The skill interface (AI engine LLD §19.4, review F3): one input, one output, fixed results. Inputs are
assembled in one place, `make_input` - the run's World and the sims both use it."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from enum import StrEnum
from typing import Protocol, runtime_checkable

from conqueror_ai.domain.drive.costmap import CostSettings
from conqueror_ai.domain.drive.costmaps import CostMaps
from conqueror_ai.domain.drive.curves import Command, XYTh
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.route import XY
from conqueror_ai.domain.room_map import MapSnapshot


class Result(StrEnum):
    """How a skill ended - the words the CLI prints and (Plan Z2) skill_failed maps to Logo codes."""

    DONE = "done"
    NOT_FOUND = "not_found"
    LOST = "lost"
    BLOCKED = "blocked"
    STALE = "stale"
    CANCELLED = "cancelled"
    REFUSED = "refused"
    FAILED = "failed"


@dataclass(frozen=True)
class SkillInput:
    at: XYTh | None  # the pose (x, y, th); None: none yet
    snap: MapSnapshot | None  # the room map
    costs: CostMaps | None  # the snapshot's cost maps, the play area in them (None with no snapshot)
    fix: TargetFix | None  # the thing sought (find only)
    sonar_m: float | None  # the echo from the tank's front (None: none)
    map_ok: bool  # pictures and telemetry fresh
    cam_still: bool  # the newest picture wasn't skipped for the camera moving
    used: bool  # a picture arrived this tick and was used (detected, placed)
    maybes: tuple[XY, ...] = ()  # this picture's sightings of the thing sought, sure or not (find only)


@dataclass(frozen=True)
class SkillOut:
    cmd: Command
    cam: tuple[float, float] | None = None  # (pan, tilt) to send, or None
    result: Result | None = None
    detail: str = ""  # what failed, for the CLI and (Plan Z2) skill_failed


@runtime_checkable
class Skill(Protocol):
    state: str

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut: ...

    def cancel(self) -> SkillOut: ...

    def debug(self) -> dict[str, object]: ...


def make_input(
    *,
    at: XYTh | None,
    snap: MapSnapshot | None,
    map_ok: bool,
    area: tuple[float, float, float] | None = None,
    settings: CostSettings | None = None,
    fix: TargetFix | None = None,
    sonar_m: float | None = None,
    cam_still: bool = True,
    used: bool = True,
    maybes: Sequence[XY] = (),
) -> SkillInput:
    costs = None if snap is None else CostMaps(snap, settings, area)
    return SkillInput(at, snap, costs, fix, sonar_m, map_ok, cam_still, used, tuple(maybes))
