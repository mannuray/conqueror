"""The lens step with OpenCV (AI engine LLD §8, design-notes §123): the checkerboard picture for the laptop
screen, its corners in a camera picture, and the lens fit (focal length, centre, distortion)."""

from __future__ import annotations

from collections.abc import Sequence
from pathlib import Path
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

PATTERN = (9, 6)  # inner corners (10 x 7 squares)


def checkerboard_png(path: Path, square_px: int = 120) -> None:
    """A 10 x 7 black-and-white board with a white margin of one square, for showing full-screen."""
    cols, rows = PATTERN[0] + 1, PATTERN[1] + 1
    img = np.full(((rows + 2) * square_px, (cols + 2) * square_px), 255, dtype=np.uint8)
    for r in range(rows):
        for c in range(cols):
            if (r + c) % 2 == 0:
                y, x = (r + 1) * square_px, (c + 1) * square_px
                img[y : y + square_px, x : x + square_px] = 0
    cv2.imwrite(str(path), img)


def board_points(square_m: float) -> NDArray[np.float64]:
    """The inner corners on the board, in metres (z = 0)."""
    pts = np.zeros((PATTERN[0] * PATTERN[1], 3), dtype=np.float64)
    pts[:, :2] = np.mgrid[0 : PATTERN[0], 0 : PATTERN[1]].T.reshape(-1, 2) * square_m
    return pts


def find_corners(gray: NDArray[np.uint8]) -> NDArray[np.float64] | None:
    """The 54 inner corners (sub-pixel), or None when the whole board is not in the picture."""
    found, corners = cv2.findChessboardCorners(gray, PATTERN, flags=cv2.CALIB_CB_ADAPTIVE_THRESH)
    if not found:
        return None
    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.01)
    fine = cv2.cornerSubPix(gray, corners, (5, 5), (-1, -1), crit)
    return np.asarray(fine, dtype=np.float64).reshape(-1, 2)


def calibrate(
    corner_sets: Sequence[NDArray[np.float64]], square_m: float, size: tuple[int, int]
) -> tuple[NDArray[np.float64], NDArray[np.float64], float]:
    """K (3 x 3), dist (k1 k2 p1 p2 k3) and the RMS reprojection error in pixels."""
    obj = board_points(square_m).astype(np.float32)
    objs = [obj for _ in corner_sets]
    imgs = [np.asarray(c, dtype=np.float32).reshape(-1, 1, 2) for c in corner_sets]
    k0 = np.zeros((3, 3), dtype=np.float64)
    d0 = np.zeros(5, dtype=np.float64)
    out = cast(
        "tuple[float, NDArray[np.float64], NDArray[np.float64], object, object]",
        cv2.calibrateCamera(objs, imgs, size, k0, d0),  # pyright: ignore[reportCallIssue, reportArgumentType]
    )
    rms, k, dist = out[0], out[1], out[2]
    return np.asarray(k, dtype=np.float64), np.asarray(dist, dtype=np.float64).reshape(-1)[:5], float(rms)
