"""What the tank confirmed, and where (design-notes §189, AI engine LLD §18.13.23): every floor thing the
tracker lists as seen, any word but a person, kept for the run - the tracker forgets one 20 s out of view.
Find starts with the remembered spots of its word; one looked at and not there is marked gone."""

from __future__ import annotations

import math
from collections.abc import Callable, Sequence
from dataclasses import dataclass

from conqueror_ai.domain.reach import SAME_SPOT_M
from conqueror_ai.domain.types import ObjectEstimate

NEVER = frozenset({"person"})  # never remembered as a thing to go to (find never targets a person)


@dataclass
class Remembered:
    word: str
    x_m: float
    y_m: float
    seen_wall_s: float  # the wall clock when last seen (the bridge's capture clock restarts each power-on)
    run_id: str  # the run it was last seen in
    gone: bool = False  # looked at from near and not there


class ThingMemory:
    def __init__(self, things: Sequence[Remembered] = ()) -> None:
        """things: remembered before (a replay restores the run's start, Plan Z8 a saved memory)."""
        self._things: list[Remembered] = list(things)

    def update(self, objects: Sequence[ObjectEstimate], wall_s: float, run_id: str) -> None:
        """The tracker's list now: things seen in the newest pictures update their remembered spot, stamped
        with the wall clock and the run (LLD §19.7, review F7.2)."""
        for o in objects:
            if o.state != "seen" or o.cls in NEVER:
                continue
            p = (o.x_m, o.y_m)
            same = [
                r for r in self._things if r.word == o.cls and math.dist((r.x_m, r.y_m), p) <= SAME_SPOT_M
            ]
            if same:
                r = min(same, key=lambda r: math.dist((r.x_m, r.y_m), (o.x_m, o.y_m)))
                r.x_m, r.y_m, r.seen_wall_s, r.run_id, r.gone = o.x_m, o.y_m, wall_s, run_id, False
            else:
                self._things.append(Remembered(o.cls, o.x_m, o.y_m, wall_s, run_id))

    def of_kind(self, kind: Callable[[str], bool]) -> list[Remembered]:
        """Remembered things of the kind not marked gone, the most recently seen first."""
        return sorted((r for r in self._things if kind(r.word) and not r.gone), key=lambda r: -r.seen_wall_s)

    def things(self) -> list[Remembered]:
        """Every remembered thing, gone ones too (for the log and, Plan Z8, saving)."""
        return list(self._things)

    def mark_gone(self, r: Remembered) -> None:
        r.gone = True
