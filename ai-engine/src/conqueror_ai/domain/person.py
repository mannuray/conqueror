"""Where the person is (AI engine LLD §28.2, Plan G2): a person box turned into a distance from the tank's
front edge and a direction (shared with the seeing test, Plan P0), and PersonWatch - the game's estimate,
pessimistic: unsure means nearer. Only the people games read it; the tracker, the memory and the map never
see a person."""

from __future__ import annotations

import math
from collections import deque
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

from conqueror_ai.domain.camera import CameraModel

CUT_PX = 4.0  # a box whose bottom is this near the picture's bottom edge is cut, not standing on the floor
EDGE_PX = 4.0  # a box this near the picture's side edge: the person is at least that far round (LLD §28.12)
SONAR_DEG = 12.0  # within this of ahead the sonar's echo may be the person's
SONAR_MAX_M = 4.0
CUT_M = 0.35  # a box cut by the picture's bottom: close, below the picture
WINDOW_US = 300_000  # the distance's median window
COMING_US = 500_000  # ... and how far back "coming / going" looks
COMING_M = 0.05
SIGMA_NEAR_M = 0.10  # until P0 measures it: ±10 cm up to 1 m, ±20 % beyond (2sigma is taken off)
SIGMA_FAR = 0.20
CLOSE_M = 0.50  # last seen inside this: the distance counts down while unseen
CAUGHT_M = 0.30  # ... and reads this after COUNTDOWN_US
COUNTDOWN_US = 1_000_000


@dataclass(frozen=True)
class PersonRange:
    """One person box read: from the tank's front edge (m) and its direction (deg, + left), or why not."""

    dist_m: float | None
    bearing_deg: float | None
    reason: (
        str  # "" ranged, "cut" the box runs off the picture's bottom, "sky" its bottom is above the horizon
    )
    edge: int = 0  # +1 the box touches the picture's left edge, -1 its right, 0 neither (LLD §28.12)


def side_edge(cam: CameraModel, box: tuple[float, ...]) -> int:
    """+1 a box at the picture's left edge (the person further left), -1 the right, 0 neither."""
    if box[0] <= EDGE_PX:
        return 1
    return -1 if box[2] >= cam.cal.size[0] - EDGE_PX else 0


def person_range(cam: CameraModel, box: tuple[float, ...]) -> PersonRange:
    """The box's bottom centre (the hands on the floor) through the camera to the floor, in the tank's
    frame."""
    x1, _, x2, y2 = box
    edge = side_edge(cam, box)
    if y2 >= cam.cal.size[1] - CUT_PX:
        return PersonRange(None, None, "cut", edge)
    p = cam.pixel_to_floor((x1 + x2) / 2.0, y2)
    if p is None:
        return PersonRange(None, None, "sky", edge)
    pose = cam.pose
    dx, dy = p[0] - pose.x_m, p[1] - pose.y_m
    c, s = math.cos(pose.th_rad), math.sin(pose.th_rad)
    tx, ty = c * dx + s * dy, -s * dx + c * dy  # the tank's frame: x ahead, y left
    ax = tx - cam.cal.track_front_m  # from the front edge's middle, where the tape starts
    return PersonRange(math.hypot(ax, ty), math.degrees(math.atan2(ty, ax)), "", edge)


def pick_person(ranges: Sequence[PersonRange]) -> PersonRange | None:
    """The person at the mark: the nearest ranged one, else any seen one (cut / sky), else None."""
    ranged = [r for r in ranges if r.dist_m is not None]
    if ranged:
        return min(ranged, key=lambda r: r.dist_m if r.dist_m is not None else math.inf)
    return ranges[0] if ranges else None


def nearest_person(ranges: Sequence[PersonRange]) -> PersonRange | None:
    """The game's person (final review C1): a box cut by the picture's bottom is the nearest by definition
    (close, below the picture), then the nearest ranged one; a box above the horizon never hides either. P0's
    grading keeps `pick_person` (one person at the mark)."""
    cut = [r for r in ranges if r.reason == "cut"]
    if cut:
        return cut[0]
    ranged = [r for r in ranges if r.dist_m is not None]
    return min(ranged, key=lambda r: r.dist_m if r.dist_m is not None else math.inf) if ranged else None


def sigma_m(d: float) -> float:
    return SIGMA_NEAR_M if d <= 1.0 else SIGMA_FAR * d


@dataclass(frozen=True)
class PersonEst:
    """The game's estimate: d_m from the front edge, pessimistic (None: no distance); t_us the picture it
    rests on (its age is what the bridge's deadman reads, bridge LLD R34.9)."""

    seen: bool
    d_m: float | None
    bearing_deg: float
    coming: int
    sure: float
    t_us: int
    edge: int = 0  # the sighting's box at the picture's side edge (+1 left, -1 right): not on the wire

    def wire(self, now_us: int) -> dict[str, Any]:
        """The `person` message (bridge LLD R34.3)."""
        return {
            "t": "person",
            "seen": self.seen,
            "d_cm": -1 if self.d_m is None else min(1000, round(self.d_m * 100)),
            "bearing": max(-180, min(180, round(self.bearing_deg))),
            "coming": self.coming,
            "sure": round(max(0.0, min(1.0, self.sure)), 2),
            "age_ms": max(0, (now_us - self.t_us) // 1000),
        }


class PersonWatch:
    """Each picture's person boxes into one estimate: the nearest person, the sonar when near and ahead, a
    median over 0.3 s, coming / going over 0.5 s, minus 2 sigma; last seen close and gone, it counts down to
    30 cm."""

    def __init__(self) -> None:
        self._raw: deque[tuple[int, float]] = deque()  # (t_us, raw distance) of the median window
        self._med: deque[tuple[int, float]] = deque()  # (t_us, median) for coming / going
        self._seen: PersonEst | None = None  # the last estimate with the person in sight
        self._last_us: int | None = None  # the newest picture's time

    def update(
        self, boxes: Sequence[tuple[float, ...]], cam: CameraModel | None, sonar_m: float | None, t_us: int
    ) -> PersonEst | None:
        self._last_us = t_us
        r = nearest_person([person_range(cam, b) for b in boxes]) if cam is not None else None
        if r is None:
            return self.latest(t_us)
        d = r.dist_m if r.dist_m is not None else CUT_M
        bearing = r.bearing_deg if r.bearing_deg is not None else 0.0
        if sonar_m is not None and 0.0 < sonar_m < SONAR_MAX_M and abs(bearing) <= SONAR_DEG:
            d = min(d, sonar_m)
        med = self._window(t_us, d)
        rep = max(0.0, med - 2.0 * sigma_m(med))
        sure = 0.9 if r.reason == "" else 0.6
        self._seen = PersonEst(True, rep, bearing, self._coming(t_us, med), sure, t_us, r.edge)
        return self._seen

    @property
    def last_us(self) -> int | None:
        """The newest picture's time (with or without a person)."""
        return self._last_us

    def latest(self, now_us: int) -> PersonEst | None:
        """The estimate now; unseen since the last sighting: the countdown when it was close."""
        s = self._seen
        if s is None:
            return None
        last = self._last_us if self._last_us is not None else s.t_us
        if last == s.t_us:
            return s
        gone_s = (now_us - s.t_us) / 1e6
        d = s.d_m if s.d_m is not None else CAUGHT_M
        if d < CLOSE_M:
            d = max(0.0, d - self._closing_mps() * gone_s)
            if now_us - s.t_us >= COUNTDOWN_US:
                d = min(d, CAUGHT_M)
        return PersonEst(False, d, s.bearing_deg, s.coming, max(0.0, s.sure - gone_s), last)

    def _window(self, t_us: int, d: float) -> float:
        """The nearest over the 0.3 s window (final review I1: a median let a far parent hide a flickering
        near baby - unsure = nearer, S2)."""
        self._raw.append((t_us, d))
        while self._raw and t_us - self._raw[0][0] > WINDOW_US:
            self._raw.popleft()
        med = min(v for _, v in self._raw)
        self._med.append((t_us, med))
        while len(self._med) > 1 and t_us - self._med[1][0] >= COMING_US:
            self._med.popleft()
        return med

    def _coming(self, t_us: int, med: float) -> int:
        if not self._med or t_us - self._med[0][0] < COMING_US - WINDOW_US:
            return 0
        change = med - self._med[0][1]
        return 1 if change <= -COMING_M else (-1 if change >= COMING_M else 0)

    def _closing_mps(self) -> float:
        """How fast the person was coming in the last 0.5 s (never negative: unseen never rises)."""
        if len(self._med) < 2:
            return 0.0
        (t0, d0), (t1, d1) = self._med[0], self._med[-1]
        return max(0.0, (d0 - d1) / max(1e-6, (t1 - t0) / 1e6))
