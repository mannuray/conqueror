"""The calibration file (config/calibration.yaml) and the lens step with OpenCV (AI engine LLD §8)."""

from __future__ import annotations

import math
from pathlib import Path

import cv2
import numpy as np
import pytest
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store, lens_opencv
from tests.test_camera import calib


def test_the_calibration_file_round_trips(tmp_path: Path) -> None:
    c = calib(tilt0=-8.5, k1=-0.12)
    path = tmp_path / "calibration.yaml"
    calibration_store.save(c, path)
    back = calibration_store.load(path, size=(320, 240))
    assert np.allclose(back.k, c.k) and np.allclose(back.dist, c.dist)
    assert back.size == c.size and back.tilt0_deg == c.tilt0_deg and back.cam_height_m == c.cam_height_m
    assert back.track_front_m == c.track_front_m and back.pan_map == c.pan_map


def test_a_file_for_another_picture_size_is_refused(tmp_path: Path) -> None:
    path = tmp_path / "calibration.yaml"
    calibration_store.save(calib(), path)
    with pytest.raises(ValueError, match="320 x 240"):
        calibration_store.load(path, size=(800, 600))


def test_vga_is_the_qvga_file_at_twice_the_pixels(tmp_path: Path) -> None:
    """LLD §16.1 item 6: the same field of view, so a floor point lands on twice the pixel."""
    import math

    from conqueror_ai.domain.camera import CameraModel
    from conqueror_ai.domain.types import Pose

    path = tmp_path / "calibration.yaml"
    calibration_store.save(calib(), path)
    q, v = calibration_store.load(path, (320, 240)), calibration_store.load(path, (640, 480))
    assert v.size == (640, 480) and v.k[0, 0] == 2 * q.k[0, 0] and v.k[1, 1] == 2 * q.k[1, 1]
    pose = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)
    cq, cv = CameraModel(q).for_frame(pose, 0.0, -15.0), CameraModel(v).for_frame(pose, 0.0, -15.0)
    for x, y in ((1.0, 0.0), (0.6, 0.2), (2.0, -0.4)):
        a, b = cq.floor_to_pixel(x, y), cv.floor_to_pixel(x, y)
        assert a is not None and b is not None
        assert math.isclose((a[0] + 0.5) * 2 - 0.5, b[0], abs_tol=1e-6) and math.isclose(
            (a[1] + 0.5) * 2 - 0.5, b[1], abs_tol=1e-6
        )


def test_the_checkerboard_picture_and_its_corners_are_found_in_perspective(tmp_path: Path) -> None:
    png = tmp_path / "board.png"
    lens_opencv.checkerboard_png(png, square_px=40)
    board = cv2.imread(str(png), cv2.IMREAD_GRAYSCALE)
    assert board is not None
    h, w = board.shape
    src = np.array([[0, 0], [w, 0], [w, h], [0, h]], dtype=np.float32)
    dst = np.array([[30, 20], [290, 40], [300, 220], [20, 200]], dtype=np.float32)  # seen at a slant, QVGA
    seen: NDArray[np.uint8] = cv2.warpPerspective(  # pyright: ignore[reportAssignmentType]
        board, cv2.getPerspectiveTransform(src, dst), (320, 240), borderValue=(255.0,)
    )
    corners = lens_opencv.find_corners(seen)
    assert corners is not None and corners.shape == (54, 2)


def test_calibration_recovers_a_known_lens() -> None:
    k = np.array([[260.0, 0, 158.0], [0, 260.0, 121.0], [0, 0, 1]])
    dist = np.array([-0.15, 0.05, 0.0, 0.0, 0.0])
    obj = lens_opencv.board_points(0.025)
    sets: list[NDArray[np.float64]] = []
    rng = np.random.default_rng(3)
    for _ in range(15):  # the board held at different angles 30-45 cm away
        rvec = rng.normal(0, 0.35, 3)
        tvec = np.array([rng.normal(-0.1, 0.03), rng.normal(-0.06, 0.03), rng.uniform(0.30, 0.45)])
        img, _ = cv2.projectPoints(obj, rvec, tvec, k, dist)
        sets.append(np.asarray(img, dtype=np.float64).reshape(-1, 2))
    k2, dist2, rms = lens_opencv.calibrate(sets, 0.025, (320, 240))
    assert rms < 0.1
    assert math.isclose(k2[0, 0], 260.0, rel_tol=0.01) and math.isclose(k2[1, 2], 121.0, abs_tol=2.0)
    assert math.isclose(dist2[0], -0.15, abs_tol=0.03)


def test_the_checkerboard_is_written_into_a_folder_that_does_not_exist_yet(tmp_path: Path) -> None:
    png = tmp_path / "config" / "checkerboard.png"  # tank session 2026-10-23: config/ did not exist yet
    lens_opencv.checkerboard_png(png)
    assert png.exists()
