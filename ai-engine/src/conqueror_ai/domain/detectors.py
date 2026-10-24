"""Several detectors as one (AI engine LLD §16.1 item 4): YOLO-World and the colour ball finder on one
picture."""

from __future__ import annotations

from collections.abc import Callable, Sequence

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Detection

SAME_BALL_IOU = 0.5

Box = tuple[float, float, float, float]
DetectFn = Callable[[NDArray[np.uint8], "CameraModel | None"], list[Detection]]


def iou(a: Box, b: Box) -> float:
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


class MultiDetector:
    """Runs each detector on the same picture and joins the lists. Any other box on a colour ball (IoU >= 0.5)
    is that ball, whatever its word (YOLO-World called the green ball "toy"): dropped - the colour finder is
    the steadier one, and one ball must be one thing."""

    def __init__(self, detectors: Sequence[DetectFn]) -> None:
        self._detectors = list(detectors)

    def detect(self, rgb: NDArray[np.uint8], cam: CameraModel | None) -> list[Detection]:
        out: list[Detection] = []
        for d in self._detectors:
            out.extend(d(rgb, cam))
        colour = [d for d in out if d.cls.startswith("ball:")]
        return [
            d
            for d in out
            if d.cls.startswith("ball:") or not any(iou(d.box, c.box) >= SAME_BALL_IOU for c in colour)
        ]
