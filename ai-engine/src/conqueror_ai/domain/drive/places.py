"""Places given relative to the tank, in the map frame (AI engine LLD §21.2): the bridge sends SETXY and AREA
as (forward, left) centimetres from the tank at the moment the skill starts."""

from __future__ import annotations

import math


def to_map(x_m: float, y_m: float, heading_rad: float, fwd_cm: float, left_cm: float) -> tuple[float, float]:
    """The map point fwd_cm ahead and left_cm to the left of the pose (the engine's frame: heading 0 along +x,
    left = +y)."""
    f, left = fwd_cm / 100.0, left_cm / 100.0
    c, s = math.cos(heading_rad), math.sin(heading_rad)
    return (x_m + f * c - left * s, y_m + f * s + left * c)
