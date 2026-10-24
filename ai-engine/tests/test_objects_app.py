"""`cq-objects <recording>` (AI engine LLD §15.6): a recorded session through the detector and the list,
here with a fake detector that 'sees' a ball on the floor 1 m ahead."""

from __future__ import annotations

import json
import math
from pathlib import Path

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.app import objects as app
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.objects import FLOOR_CLASSES
from conqueror_ai.domain.types import Detection, Frame
from tests.test_objects import CAL, box_on_floor, cam_at

GREY_JPEG = bytes(cv2.imencode(".jpg", np.full((240, 320, 3), 128, dtype=np.uint8))[1].tobytes())


class BallDetector:
    """Sees a ball 1 m ahead, 10 cm to the left, in every picture."""

    def __init__(self) -> None:
        self.box = box_on_floor(cam_at(), 1.0, 0.1)
        self.calls = 0

    def detect(self, rgb: NDArray[np.uint8], cam: CameraModel | None) -> list[Detection]:
        assert rgb.shape == (240, 320, 3)
        self.calls += 1
        return [self.box]


def td(t_ms: int) -> str:
    return json.dumps(
        {
            "t": "td",
            "seq": t_ms,
            "uno_ms": t_ms,
            "bus_us": t_ms * 1000,
            "enc": [0, 0],
            "pwm": [0, 0],
            "cmd": [0, 0],
            "yaw": 0.0,
            "rate": 0.0,
            "dist_mm": None,
            "pan": 90.0,
            "tilt": 90.0,
            "flags": 0,
        }
    )


def session(folder: Path) -> None:
    """2 s: the tank still, telemetry at 50 Hz, a picture every 100 ms (arriving 100 ms after capture)."""
    rec = SessionRecorder(folder, {"what": "test"})
    rec.telemetry(0.0, json.dumps({"t": "calib", "counts_per_m": 7514, "roll_mm": 27, "gyro_scale": 10310}))
    for t in range(0, 2000, 20):
        rec.telemetry(t / 1000, td(t))
        if t % 100 == 0 and t >= 100:
            rec.frame(Frame(seq=t, cap_us=(t - 100) * 1000, recv_s=t / 1000, jpeg=GREY_JPEG))
    rec.close()


def test_a_recording_lists_the_ball(tmp_path: Path) -> None:
    session(tmp_path / "s")
    det = BallDetector()
    res = app.run_replay(tmp_path / "s", CAL, det, FLOOR_CLASSES)
    assert det.calls == 19
    assert len(res.things) == 1
    th = next(iter(res.things.values()))
    assert (
        th.cls == "sports ball"
        and math.isclose(th.x_m, 1.0, abs_tol=0.01)
        and math.isclose(th.y_m, 0.1, abs_tol=0.01)
    )
    assert res.used == 19 and res.skips == {}


def test_the_report_says_what_was_found_and_skipped(tmp_path: Path) -> None:
    session(tmp_path / "s")
    text = app.report(app.run_replay(tmp_path / "s", CAL, BallDetector(), FLOOR_CLASSES))
    assert "sports ball" in text and "100 cm" in text and "19 pictures used" in text


def test_the_live_view_takes_boxes_and_the_plan(tmp_path: Path) -> None:
    from conqueror_ai.adapters.rerun_view import RerunView
    from conqueror_ai.domain.types import ObjectEstimate, Pose

    view = RerunView(spawn=False, save=tmp_path / "v.rrd")
    f = Frame(seq=1, cap_us=1_000_000, recv_s=1.1, jpeg=GREY_JPEG)
    view.frame(f, None)
    view.detections(f, [Detection("sports ball", 0.9, (10.0, 20.0, 30.0, 40.0))])
    seen = ObjectEstimate(1, "sports ball", 1.0, 0.1, 0.0, 0.0, 0.05, 0, 1_000_000, "seen")
    gone = ObjectEstimate(2, "person", 2.0, -0.5, 0.0, 0.0, 0.3, 0, 500_000, "remembered")
    view.plan(1_000_000, Pose(1_000_000, 0.0, 0.0, 0.0, 0.0, 0.0), [seen, gone], None)
    view.plan(1_100_000, None, [], "turning")
    assert (tmp_path / "v.rrd").exists()


def test_a_vga_recording_lists_the_colour_ball_once_with_a_word_box_on_it(tmp_path: Path) -> None:
    """LLD §16.5: a fake YOLO-World ("toy" on the ball, a chair elsewhere) and the real colour finder -> one
    ball."""
    import cv2

    from conqueror_ai.adapters import calibration_store
    from conqueror_ai.domain.balls import BallColour, find_balls, horizon_row
    from conqueror_ai.domain.detectors import MultiDetector
    from conqueror_ai.domain.types import Pose
    from tests.test_camera import calib

    cfile = tmp_path / "calibration.yaml"
    calibration_store.save(calib(), cfile)
    cal = calibration_store.load(cfile, (640, 480))
    cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    q = cam.floor_to_pixel(1.0, 0.1)  # the ball's bottom, 1 m ahead
    assert q is not None
    u, v = round(q[0]), round(q[1])
    hsv = np.zeros((480, 640, 3), np.uint8)
    hsv[:] = (55, 30, 240)
    pic = cv2.cvtColor(hsv, cv2.COLOR_HSV2BGR)
    green = cv2.cvtColor(np.array([[(64, 90, 170)]], np.uint8), cv2.COLOR_HSV2BGR)[0, 0]
    cv2.circle(pic, (u, v - 14), 14, tuple(int(c) for c in green), -1)
    jpeg = bytes(cv2.imencode(".jpg", pic, [cv2.IMWRITE_JPEG_QUALITY, 95])[1].tobytes())
    rec = SessionRecorder(tmp_path / "s", {"what": "test"})
    for t in range(0, 2000, 20):
        rec.telemetry(t / 1000, td(t))
        if t % 100 == 0 and t >= 100:
            rec.frame(Frame(seq=t, cap_us=(t - 100) * 1000, recv_s=t / 1000, jpeg=jpeg))
    rec.close()
    toy = Detection("toy", 0.4, (u - 15.0, v - 29.0, u + 15.0, v + 1.0))
    chair = Detection("chair", 0.8, (100.0, 220.0, 160.0, 330.0))  # ~1.2 m away, left
    green_ball = BallColour("green", 45, 90, 65, 200, 6.0)
    horizons: list[int | None] = []

    def balls(rgb: NDArray[np.uint8], c: CameraModel | None) -> list[Detection]:
        horizons.append(None if c is None else horizon_row(c))
        return find_balls(rgb, [green_ball], c)

    det = MultiDetector([lambda rgb, c: [toy, chair], balls])
    res = app.run_replay(tmp_path / "s", cal, det, frozenset({"toy", "chair", "ball:green"}))
    assert horizons and set(horizons) == {horizon_row(cam)} and horizon_row(cam) is not None  # review m3
    names = sorted(t.cls for t in res.things.values())
    assert names == ["ball:green", "chair"]
    ball = next(t for t in res.things.values() if t.cls == "ball:green")
    assert math.isclose(ball.x_m, 1.0, abs_tol=0.05) and math.isclose(ball.y_m, 0.1, abs_tol=0.05)
