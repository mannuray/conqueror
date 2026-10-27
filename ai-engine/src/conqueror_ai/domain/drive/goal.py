"""Which thing is the target and where it is now (AI engine LLD §17.5, spec 8 §4.1)."""

from __future__ import annotations

import math
from collections import Counter
from collections.abc import Callable, Sequence
from dataclasses import dataclass

import numpy as np

from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Detection, ObjectEstimate

BALL_WORDS = frozenset({"ball", "toy"})  # YOLO-World's words for a ball (it named the green ball "toy", §152)
MIN_SIZE_R_PX = 8.0  # a ball smaller than this in the picture: its size gives no good range
ECHO_RANGE_M = 0.15  # a sonar echo this near the target's range ...
ECHO_BEARING_DEG = 12.0  # ... with the target this near straight ahead, is the target


def is_ball(cls: str) -> bool:
    """The kind "ball": any colour ball, or YOLO-World's ball / toy."""
    return cls.startswith("ball:") or cls in BALL_WORDS


def kind_of(thing: str) -> Callable[[str], bool]:
    """The target kind for a word: "ball" is every ball (any colour, YOLO-World's ball / toy); any other
    word is that thing (spec 8 §4.1: GOTO takes a thing's name)."""
    if thing == "ball":
        return is_ball
    return lambda cls: cls == thing


@dataclass(frozen=True)
class TargetFix:
    oid: int
    x_m: float
    y_m: float
    seen_age_s: float  # since the target was last seen in a picture


def size_position(det: Detection, size_cm: float, cam: CameraModel) -> tuple[float, float] | None:
    """The ball's room position from its size in the picture (tilt-free, spec 8 §4.1): its centre lies
    size x fx / (2 r) along the ray through the box's centre. None when it is too small to tell."""
    x1, y1, x2, y2 = det.box
    r = (x2 - x1) / 2
    if r < MIN_SIZE_R_PX:
        return None
    ray = cam.pixel_ray((x1 + x2) / 2, (y1 + y2) / 2)
    if ray is None:
        return None
    rng = size_cm / 100.0 * float(cam.cal.k[0, 0]) / (2 * r)
    p = cam.position() + ray / float(np.linalg.norm(ray)) * rng
    return float(p[0]), float(p[1])


def size_positions(
    dets: Sequence[Detection], sizes: dict[str, float], cam: CameraModel
) -> dict[str, tuple[float, float]]:
    """Room positions from the sizes of the balls in one picture, by class. A class seen twice is left out:
    which of the two is the target isn't known here, so the map's position is used (no jumping between)."""
    seen = Counter(d.cls for d in dets)
    out: dict[str, tuple[float, float]] = {}
    for d in dets:
        size = sizes.get(d.cls)
        if size is None or seen[d.cls] > 1:
            continue
        pos = size_position(d, size, cam)
        if pos is not None:
            out[d.cls] = pos
    return out


class Goal:
    """The nearest thing of the kind, then locked to it while it is listed (no hopping between balls)."""

    def __init__(self, kind: Callable[[str], bool] = is_ball) -> None:
        self.kind = kind
        self.oid: int | None = None

    def update(
        self,
        objects: Sequence[ObjectEstimate],
        at: tuple[float, float],
        now_us: int,
        by_size: dict[str, tuple[float, float]] | None = None,
    ) -> TargetFix | None:
        """by_size: room positions from the balls' sizes in the newest picture, by class."""
        cands = [o for o in objects if self.kind(o.cls)]
        t = next((o for o in cands if o.oid == self.oid), None)
        if t is None:
            t = min(cands, key=lambda o: math.hypot(o.x_m - at[0], o.y_m - at[1]), default=None)
            self.oid = None if t is None else t.oid
        if t is None:
            return None
        x, y = (by_size or {}).get(t.cls, (t.x_m, t.y_m))
        return TargetFix(t.oid, x, y, max(0.0, (now_us - t.last_seen_us) / 1e6))


def echo_is_target(echo_m: float, target_m: float, bearing: float) -> bool:
    """A sonar echo at the target's range, with the target straight ahead, is the target, not an obstacle."""
    return abs(echo_m - target_m) <= ECHO_RANGE_M and abs(bearing) <= ECHO_BEARING_DEG
