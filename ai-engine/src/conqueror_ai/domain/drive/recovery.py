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
MOVED_M = 0.05  # on the map: moving this far ...
TURNED_DEG = 15.0  # ... or turning this far is progress; none for stuck_s starts the ways out (Plan Z6)

Out = tuple[Command, tuple[float, float] | None]


def turned_deg(a_rad: float, b_rad: float) -> float:
    """The turn between two headings, 0..180 deg."""
    return abs((math.degrees(b_rad - a_rad) + 180.0) % 360.0 - 180.0)


class TurnMeter:
    """How far a turn has gone, measured from the pose (review M4): a tank turning slower than told (the
    ramp, a slipping track) still turns its whole angle."""

    def __init__(self) -> None:
        self.deg = 0.0
        self._th: float | None = None

    def reset(self) -> None:
        self.deg, self._th = 0.0, None

    def update(self, th_rad: float) -> float:
        if self._th is not None:
            self.deg += turned_deg(self._th, th_rad)
        self._th = th_rad
        return self.deg


class ProgressWatch:
    """Stuck: the tank has neither moved MOVED_M nor turned TURNED_DEG for stuck_s (every skill on the map,
    §18.13.4); progress also tells the recovery, which starts over once far enough from where it began."""

    def __init__(self, recovery: Recovery, stuck_s: float) -> None:
        self.recovery = recovery
        self.stuck_s = stuck_s
        self._last: tuple[XYTh, float] | None = None  # where and when it last made progress

    def reset(self) -> None:
        self._last = None

    def ok(self, now_s: float, at: XYTh) -> bool:
        if self._last is None:
            self._last = (at, now_s)
            return True
        p, t = self._last
        if math.dist(p[:2], at[:2]) >= MOVED_M or turned_deg(p[2], at[2]) >= TURNED_DEG:
            self._last = (at, now_s)
            self.recovery.progress(at)
            return True
        return now_s - t < self.stuck_s - 1e-9


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
        self._turned = TurnMeter()
        self._backing: tuple[XYTh, float] | None = None  # where backing began and how far it may go (M2, M3)

    def start(self, now_s: float, at: XYTh) -> None:
        """Stuck: begin at "look", or at the next step when stuck again near where the last one began."""
        if self._from is not None and math.dist(self._from[:2], at[:2]) < PROGRESS_M:
            self.step = STEPS[min(STEPS.index(self.step) + 1, len(STEPS) - 1)]
        else:
            self.step, self._from = "look", at
        self._t0, self._pans, self._turn_w, self._backing = now_s, list(LOOK_PANS), 0.0, None
        self._turned.reset()

    def progress(self, at: XYTh) -> None:
        """The tank moved on: far enough from where the sequence began, it starts over next time."""
        if self._from is not None and math.dist(self._from[:2], at[:2]) >= PROGRESS_M:
            self._from, self.step = None, "look"

    def tick(self, now_s: float, at: XYTh, cost: CostMap) -> Out | None:
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
                if self.foot.turn_ok(cost, at, left, seen=False)  # the last resort (§176)
            ]
            if not ways:
                self.step = "back"
                return None
            self._turn_w = self.lim.spin_dps * (1.0 if min(ways)[1] else -1.0)
            self._t0 = now_s
        rest = TURN_DEG - self._turned.update(at[2])
        if rest <= 1e-9 or now_s - self._t0 >= TURN_S - 1e-9:
            return None
        if not self.foot.turn_ok(cost, at, self._turn_w > 0, rest, seen=False):
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
        """Straight back over known-free floor only (nothing senses behind the tank), the body's rule kept. A
        mark the body already covers doesn't count: drawn on its corner after it got there (the camera's blind
        zone), it mustn't stop it backing off (floor run 2, design-notes §170); floor never seen still does
        (Z6 review 6)."""
        covered = {
            c
            for pts in (self.foot.outline, self.foot.body)
            for x, y in self.foot.world(at, pts)
            if (c := cost.cell_of(float(x), float(y))) is not None and cost.lethal[c]
        }
        poses = self.foot.back_poses(at, length_m)
        for p in poses:
            ij = [cost.cell_of(float(x), float(y)) for x, y in self.foot.world(p)]
            if any(c is None or (not cost.known_free[c] and c not in covered) for c in ij):
                return False
        return self.foot.allowed(cost, at, poses)
