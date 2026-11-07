"""The question words SEE? and HOWFAR (AI engine LLD §21.3), answered from the latest World: no model run.

`word` is a resolved word (Names.resolve), "ball" meaning every ball. `now_s` is on the same clock as the
objects' times (the bridge clock, seconds). A ball is the colour finder's ball:<name> or the word
"ball" (not a toy)."""

from __future__ import annotations

import math

from conqueror_ai.app.world import World
from conqueror_ai.domain.drive.goal import kind_of
from conqueror_ai.domain.types import ObjectEstimate

SEEN_S = 0.5  # SEE? is true for a thing seen this recently


class Questions:
    @staticmethod
    def _of(world: World, word: str) -> list[ObjectEstimate]:
        kind = kind_of(word)
        return [o for o in world.map_run.p.objects() if kind(o.cls)]

    def _seen(self, world: World, word: str, now_s: float) -> list[ObjectEstimate]:
        """The things of the word seen within SEEN_S (not remembered ones)."""
        return [
            o for o in self._of(world, word) if o.state == "seen" and now_s - o.last_seen_us / 1e6 <= SEEN_S
        ]

    def _near(self, world: World, word: str, now_s: float) -> list[tuple[float, float]]:
        """Where the word's things are: confirmed ones (within SEEN_S) and recent sightings (§25.9)."""
        kind = kind_of(word)
        sightings = [(s.x, s.y) for s in world.recent_sightings(round(now_s * 1e6)) if kind(s.cls)]
        return [(o.x_m, o.y_m) for o in self._seen(world, word, now_s)] + sightings

    def see(self, world: World, word: str, now_s: float) -> bool:
        return bool(self._near(world, word, now_s))

    def howfar(self, world: World, word: str, now_s: float) -> int:
        """The nearest such thing seen within SEEN_S, confirmed or a single sighting (as SEE?): its distance
        from the lens in cm; -1 when there is none (or no pose)."""
        pose = world.map_run.p.pose()
        if pose is None:
            return -1
        lx = pose.x_m + world.lim.cam_x_m * math.cos(pose.th_rad)
        ly = pose.y_m + world.lim.cam_x_m * math.sin(pose.th_rad)
        ds = [math.hypot(x - lx, y - ly) for x, y in self._near(world, word, now_s)]
        return round(min(ds) * 100) if ds else -1
