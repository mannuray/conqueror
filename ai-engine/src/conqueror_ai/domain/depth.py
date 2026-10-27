"""Seeing (AI engine LLD §18.4, spec 9 §3): depth to the camera's real tilt, metres, and heights."""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass(frozen=True)
class Heights:
    """Heights above the floor (owner, design-notes §159): under floor_m is floor; up to low_top_m a low
    obstacle (the sonar's beam vouches for it); up to clear_m a high one; above, passable under."""

    floor_m: float = 0.02
    low_top_m: float = 0.10
    clear_m: float = 0.20
    below_m: float = -0.05  # a point this far under the floor is bad depth


@dataclass(frozen=True)
class Ranges:
    mark_max_m: float = 2.0  # obstacles (low / high) are trusted this far
    free_max_m: float = 3.0  # floor and "above" this far


@dataclass(frozen=True)
class DepthSettings:
    every_n: int = 1  # depth on every n-th picture used
    tilt_offset_deg: float = 4.1  # the depth reads this much less down than the truth (design-notes §155)
    trust_deg: float = 3.0  # a fix bigger than this isn't trusted (spec 9 §5)


@dataclass(frozen=True)
class MapConfig:
    heights: Heights = field(default_factory=Heights)
    ranges: Ranges = field(default_factory=Ranges)
    depth: DepthSettings = field(default_factory=DepthSettings)
