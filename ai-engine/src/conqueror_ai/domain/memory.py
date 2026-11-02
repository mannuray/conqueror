"""What the tank confirmed, and where (design-notes §189, AI engine LLD §18.13.23): every floor thing the
tracker lists as seen, any word but a person, kept for the run - the tracker forgets one 20 s out of view.
Find starts with the remembered spots of its word; one looked at and not there is marked gone."""

from __future__ import annotations

import math
from collections.abc import Callable, Sequence
from dataclasses import dataclass

from conqueror_ai.domain.types import ObjectEstimate

MERGE_M = 0.5  # the same word this near a remembered thing is that thing (moved, or the floor point's error)
NEVER = frozenset({"person"})  # never remembered as a thing to go to (find never targets a person)


@dataclass
class Remembered:
    word: str
    x_m: float
    y_m: float
    last_seen_s: float  # the tracker's clock (capture time)
    gone: bool = False  # looked at from near and not there


class ThingMemory:
    def __init__(self) -> None:
        self._things: list[Remembered] = []

    def update(self, objects: Sequence[ObjectEstimate]) -> None:
        """The tracker's list now: things seen in the newest pictures update their remembered spot."""
        for o in objects:
            if o.state != "seen" or o.cls in NEVER:
                continue
            t = o.last_seen_us / 1e6
            p = (o.x_m, o.y_m)
            same = [r for r in self._things if r.word == o.cls and math.dist((r.x_m, r.y_m), p) <= MERGE_M]
            if same:
                r = min(same, key=lambda r: math.dist((r.x_m, r.y_m), (o.x_m, o.y_m)))
                r.x_m, r.y_m, r.last_seen_s, r.gone = o.x_m, o.y_m, t, False
            else:
                self._things.append(Remembered(o.cls, o.x_m, o.y_m, t))

    def of_kind(self, kind: Callable[[str], bool]) -> list[Remembered]:
        """Remembered things of the kind not marked gone, the most recently seen first."""
        return sorted((r for r in self._things if kind(r.word) and not r.gone), key=lambda r: -r.last_seen_s)

    def mark_gone(self, r: Remembered) -> None:
        r.gone = True
