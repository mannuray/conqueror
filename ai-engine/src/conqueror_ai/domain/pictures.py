"""The pictures the /ai page gets (AI engine LLD §22.2): the camera at 320 x 240 and the depth at 160 x 120,
green and near bright (the page's palette). JPEG only; no model work."""

from __future__ import annotations

from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

CAM_SIZE = (320, 240)
CAM_QUALITY = 70
DEPTH_SIZE = (160, 120)
DEPTH_QUALITY = 80


def _jpeg(bgr: NDArray[np.uint8], quality: int) -> bytes:
    ok, buf = cv2.imencode(".jpg", bgr, [cv2.IMWRITE_JPEG_QUALITY, quality])
    if not ok:
        raise ValueError("the picture did not encode")
    return buf.tobytes()


def cam_jpeg(rgb: NDArray[np.uint8]) -> bytes:
    small = cv2.resize(rgb, CAM_SIZE, interpolation=cv2.INTER_AREA)
    return _jpeg(cast(NDArray[np.uint8], cv2.cvtColor(small, cv2.COLOR_RGB2BGR)), CAM_QUALITY)


def depth_jpeg(inv: NDArray[np.floating]) -> bytes:
    """Inverse depth (bigger = nearer) scaled between its 2nd and 98th percentiles into the green channel."""
    small = cast(
        NDArray[np.float32], cv2.resize(inv.astype(np.float32), DEPTH_SIZE, interpolation=cv2.INTER_AREA)
    )
    lo, hi = float(np.percentile(small, 2.0)), float(np.percentile(small, 98.0))
    g = np.clip((small - lo) / max(hi - lo, 1e-6) * 255.0, 0.0, 255.0).astype(np.uint8)
    bgr = np.zeros((g.shape[0], g.shape[1], 3), np.uint8)
    bgr[:, :, 1] = g
    return _jpeg(bgr, DEPTH_QUALITY)
