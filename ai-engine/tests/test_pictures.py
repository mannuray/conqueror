"""Pictures for the /ai page: the camera and the depth, small (AI engine LLD §22.2)."""

from __future__ import annotations

from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.pictures import cam_jpeg, depth_jpeg


def _decode(b: bytes) -> NDArray[np.uint8]:
    img = cast("NDArray[np.uint8] | None", cv2.imdecode(np.frombuffer(b, np.uint8), cv2.IMREAD_COLOR))
    assert img is not None
    return img


def test_cam_is_320x240() -> None:
    rgb = np.zeros((480, 640, 3), np.uint8)
    rgb[:, :320] = (200, 40, 40)
    img = _decode(cam_jpeg(rgb))
    assert img.shape == (240, 320, 3)
    assert img[120, 80, 2] > 150  # red stays red (the JPEG is BGR inside)


def test_depth_near_is_bright_green() -> None:
    inv = np.tile(
        np.linspace(0.0, 10.0, 518, dtype=np.float32), (392, 1)
    )  # far on the left, near on the right
    img = _decode(depth_jpeg(inv))
    assert img.shape == (120, 160, 3)
    near, far = img[60, 155], img[60, 4]
    assert near[1] > 200 and far[1] < 40
    assert int(near[0]) <= 8 and int(near[2]) <= 8  # B and R stay (near) zero


def test_depth_flat_does_not_divide_by_zero() -> None:
    img = _decode(depth_jpeg(np.full((10, 10), 3.0, np.float32)))
    assert img.shape == (120, 160, 3)
