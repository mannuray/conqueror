"""The home spot (AI engine LLD §20, design-notes §189 step 2, §191, §193): what a run keeps for the next one
started from the same tape mark, and how it comes back. Safety (§191 decision 1): obstacles come back weakened
and fade (owner: A); free floor never comes back; `seen` only as `seen_before`, which nothing that drives
reads; the memory comes back pruned."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.depth import MapSettings
from conqueror_ai.domain.memory import Remembered, ThingMemory
from conqueror_ai.domain.places import Place
from conqueror_ai.domain.room_map import RoomMap

HOME_VERSION = 2  # 2: the names on the map (LLD §23.3); a version-1 home loads with none
HOME_VERSIONS = (1, 2)  # what load reads
FADE_PER_LOAD = 0.2  # each load lowers a kept obstacle this much: 1.1 -> 0.9 -> 0.7 (not blocked) - owner: A
FADE_EPS = 1e-3  # a faded value this near the blocked level counts as at it
KEEP_RUNS = 5  # a remembered thing not seen in the last this many runs is dropped
KEEP_PER_WORD = 5  # at most this many remembered things of one word (the most recent)


@dataclass(frozen=True)
class Home:
    low: NDArray[np.float32]  # obstacle evidence above the blocked level, else 0
    high: NDArray[np.float32]
    seen: NDArray[np.bool_]  # every cell any kept run has seen
    cell_m: float
    size_m: float
    runs: tuple[str, ...]  # the last KEEP_RUNS runs, oldest first
    things: tuple[Remembered, ...]
    places: tuple[Place, ...] = ()  # the names on the map (LLD §23.3)

    @staticmethod
    def from_run(
        room: RoomMap,
        memory: ThingMemory,
        earlier: Home | None,
        run_id: str,
        places: Sequence[Place] | None = None,
    ) -> Home:
        """What this run leaves for the next: its obstacles (above the blocked level), what it and earlier
        runs saw, the run list, the memory pruned, the names (None - a run with no names of its own, a plain
        cq-drive --home - carries the earlier home's through)."""
        b = room.settings.blocked
        low = np.where(room.low > b, room.low, 0.0).astype(np.float32)
        high = np.where(room.high > b, room.high, 0.0).astype(np.float32)
        seen = room.seen | room.seen_before
        runs = ((*earlier.runs, run_id) if earlier is not None else (run_id,))[-KEEP_RUNS:]
        kept = [t for t in memory.things() if not t.gone and t.run_id in runs]
        kept.sort(key=lambda t: -t.seen_wall_s)
        things: list[Remembered] = []
        for t in kept:
            if sum(1 for k in things if k.word == t.word) < KEEP_PER_WORD:
                things.append(t)
        kept_places = tuple(places) if places is not None else (earlier.places if earlier is not None else ())
        return Home(
            low, high, seen.copy(), room.cell_m, room.settings.size_m, runs, tuple(things), kept_places
        )

    def room_for_run(self, settings: MapSettings) -> RoomMap:
        """A run's starting map: each kept obstacle at min(value - FADE_PER_LOAD, blocked + |miss|) - still an
        obstacle, two looks at free floor clear it - or 0 once faded to the blocked level; nothing seen; what
        was seen before only as seen_before."""
        room = RoomMap(settings)
        cap = settings.blocked + abs(settings.miss)
        for kept, layer in ((self.low, room.low), (self.high, room.high)):
            v = np.minimum(kept - FADE_PER_LOAD, cap)
            layer[:] = np.where(v > settings.blocked + FADE_EPS, v, 0.0)  # float32: 0.7 + 1 ulp is faded too
        room.seen_before[:] = self.seen
        return room

    def memory(self) -> ThingMemory:
        return ThingMemory(self.things)
