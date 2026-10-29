"""The standard ways out (AI engine LLD §18.13.4, Plan Z6): when no move is allowed, none makes progress, or
there is no route - look round with the camera, turn on the spot, back up a little over floor the tank knows,
give up. One sequence for every skill on the map, in place of per-case rules (design-notes §166)."""

from __future__ import annotations

import math
from typing import Literal

from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.curves import STOP, Command, XYTh
from conqueror_ai.domain.drive.footprint import Footprint
from conqueror_ai.domain.drive.limits import Limits

Step = Literal["look", "turn", "back", "blocked"]
STEPS: tuple[Step, ...] = ("look", "turn", "back", "blocked")
LOOK_PANS = (-90.0, -45.0, 45.0, 90.0, 0.0)  # the camera's look round, then ahead again
LOOK_STAND_S = 1.0  # standing at each, so the map takes still pictures
TURN_DEG = 90.0
TURN_S = 3.0  # a turn that hasn't turned its 90 deg by now ends (a slipping track: ~1.4 s at 65 deg/s)
BACK_S = 2.5  # backing up never longer than this
PROGRESS_M = 0.3  # stuck again this near the last start: the next step; further: start over
BACK_STEP_M = 0.025  # backing as far as known floor allows, in these steps ...
BACK_MIN_M = 0.05  # ... at least this far, or not at all

Out = tuple[Command, tuple[float, float] | None]


class Recovery:
    def __init__(self, lim: Limits, foot: Footprint, tilt_deg: float) -> None:
        self.lim = lim
        self.foot = foot
        self.tilt = tilt_deg
        self.step: Step = "look"
        self._from: XYTh | None = None  # where the sequence started
        self._t0 = 0.0
        self._pans: list[float] = []
        self._turn_w = 0.0  # + left, - right; 0: not chosen yet
        self._turned = 0.0  # measured from the pose (review M4)
        self._th: float | None = None
        self._backing: tuple[XYTh, float] | None = None  # where backing began and how far it may go (M2, M3)

    def start(self, now_s: float, at: XYTh) -> None:
        """Stuck: begin at "look", or at the next step when stuck again near where the last one began."""
        if self._from is not None and math.dist(self._from[:2], at[:2]) < PROGRESS_M:
            self.step = STEPS[min(STEPS.index(self.step) + 1, len(STEPS) - 1)]
        else:
            self.step, self._from = "look", at
        self._t0, self._pans, self._turn_w, self._turned = now_s, list(LOOK_PANS), 0.0, 0.0
        self._th, self._backing = None, None

    def progress(self, at: XYTh) -> None:
        """The tank moved on: far enough from where the sequence began, it starts over next time."""
        if self._from is not None and math.dist(self._from[:2], at[:2]) >= PROGRESS_M:
            self._from, self.step = None, "look"

    def tick(self, now_s: float, at: XYTh, cost: CostMap, dt_s: float) -> Out | None:
        """This step's command and camera (pan, tilt) or None; None: the step has finished (try driving)."""
        if self.step == "look":
            return self._look(now_s)
        if self.step == "turn":
            out = self._turn(now_s, at, cost)
            if out is not None or self.step == "turn":
                return out
            self._t0 = now_s  # no turn allowed: straight on to backing up
        if self.step == "back":
            return self._back(now_s, at, cost)
        return (STOP, None)  # blocked: the skill ends the run

    def _look(self, now_s: float) -> Out | None:
        if now_s - self._t0 < LOOK_STAND_S - 1e-9 and len(self._pans) < len(LOOK_PANS):
            return (STOP, None)
        if not self._pans:
            return None
        self._t0 = now_s
        return (STOP, (self._pans.pop(0), self.tilt))

    def _turn(self, now_s: float, at: XYTh, cost: CostMap) -> Out | None:
        """Measured from the pose: a tank turning slower than told still turns its 90 deg (review M4)."""
        if self._turn_w == 0.0:  # the allowed way, the cheaper first
            ways = [
                (self.foot.highest_cost(cost, at, self.foot.spin_poses(at, left)), left)
                for left in (True, False)
                if self.foot.allowed(cost, at, self.foot.spin_poses(at, left))
            ]
            if not ways:
                self.step = "back"
                return None
            self._turn_w = self.lim.spin_dps * (1.0 if min(ways)[1] else -1.0)
            self._t0 = now_s
        elif self._th is not None:
            self._turned += abs((math.degrees(at[2] - self._th) + 180.0) % 360.0 - 180.0)
        self._th = at[2]
        rest = TURN_DEG - self._turned
        if rest <= 1e-9 or now_s - self._t0 >= TURN_S - 1e-9:
            return None
        if not self.foot.allowed(cost, at, self.foot.spin_poses(at, self._turn_w > 0, rest)):
            return None
        return (Command(0.0, self._turn_w), None)

    def _back(self, now_s: float, at: XYTh, cost: CostMap) -> Out | None:
        """Straight back from where backing began (M2), as far as known floor allows up to back_m (M3)."""
        if self._backing is None:
            length = cost.settings.back_m
            while length >= BACK_MIN_M - 1e-9 and not self._back_ok(at, length, cost):
                length -= BACK_STEP_M
            if length < BACK_MIN_M - 1e-9:  # no known floor behind
                self.step = "blocked"
                return (STOP, None)
            self._backing, self._t0 = (at, length), now_s
        start, length = self._backing
        rest = length - math.dist(start[:2], at[:2])
        if rest <= 0.0 or now_s - self._t0 >= BACK_S - 1e-9 or not self._back_ok(at, rest, cost):
            return None
        return (Command(-self.lim.v_min_cms, 0.0), None)

    def _back_ok(self, at: XYTh, length_m: float, cost: CostMap) -> bool:
        """Straight back over known-free floor only (nothing senses behind the tank), the body's rule kept."""
        poses = self.foot.back_poses(at, length_m)
        for p in poses:
            ij = [cost.cell_of(float(x), float(y)) for x, y in self.foot.world(p)]
            if any(c is None or not cost.known_free[c] for c in ij):
                return False
        return self.foot.allowed(cost, at, poses)
