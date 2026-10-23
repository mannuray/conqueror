"""The calibration tool's steps (AI engine LLD §8, design-notes §123), driven by a synthetic tank: pictures
from a known camera, scripted answers and clicks, a recorded cam_set."""

from __future__ import annotations

import math
from collections.abc import Iterator
from pathlib import Path

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import lens_opencv
from conqueror_ai.app import calibrate as tool
from conqueror_ai.domain import calib_math as cm
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Pose
from tests.test_calib_math import red_line_picture
from tests.test_camera import calib

TRUE_TILT0 = -8.0  # the camera's real tilt at the servo centre in the synthetic tank
TILT_GAIN, TILT_OFFSET = 0.97, 0.5  # the real servo: real change = 0.97 x commanded + 0.5
PAN_GAIN = 1.03


class FakeTank:
    """Answers cam_set by remembering the angles; makes the picture the camera would see."""

    def __init__(self, scene: str, line_off: float = 0.0, slack: float = 0.0) -> None:
        self.scene = scene  # "horizon" (laser line at lens height) or "vertical" (line straight ahead)
        self.line_off = line_off  # deg: the line is a bit off lens height / straight ahead (§130)
        self.slack = slack  # deg: the servo lands this much higher arriving from above (pan: from the right)
        self.pan = 0.0
        self.tilt = 0.0
        self.tilt_from = 0.0  # +1 arrived from above, -1 from below
        self.pan_from = 0.0
        self.sets: list[tuple[float, float]] = []
        self.cal = calib(tilt0=TRUE_TILT0)

    async def cam_set(self, pan: float, tilt: float) -> None:
        if tilt != self.tilt:
            self.tilt_from = 1.0 if tilt < self.tilt else -1.0
        if pan != self.pan:
            self.pan_from = 1.0 if pan < self.pan else -1.0
        self.pan, self.tilt = pan, tilt
        self.sets.append((pan, tilt))

    async def grab(self) -> NDArray[np.uint8]:
        k = self.cal.k
        if self.scene == "horizon":
            real = TRUE_TILT0 + TILT_GAIN * self.tilt + TILT_OFFSET
            real += self.line_off + self.slack * self.tilt_from
            return red_line_picture(row=float(k[1, 2] + k[1, 1] * math.tan(math.radians(real))))
        real_pan = PAN_GAIN * self.pan + self.line_off + self.slack * self.pan_from
        return red_line_picture(col=float(k[0, 2] - k[0, 0] * math.tan(math.radians(real_pan))))


def answers(*lines: str) -> tool.Ask:
    it: Iterator[str] = iter(lines)

    async def ask(_prompt: str) -> str:
        return next(it)

    return ask


async def test_the_mount_step_reads_the_tilt_from_the_laser_horizon() -> None:
    tank = FakeTank("horizon")
    base = calib(tilt0=0.0)  # what the lens step left: no tilt known yet
    cal = await tool.mount_tilt(base, tank.grab, tank.cam_set, answers(""))
    assert tank.sets[-1] == (0.0, 0.0)  # the camera at its centre first
    assert math.isclose(cal.tilt0_deg, TRUE_TILT0 + TILT_OFFSET, abs_tol=0.2)


async def test_the_floor_marks_are_checked_by_clicks() -> None:
    cal = calib(tilt0=-30.0)
    cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    marks = [cam.floor_to_pixel(cal.track_front_m + d, 0.0) for d in (0.50, 1.00)]
    clicks = iter([m for m in marks if m is not None])

    async def click(_img: NDArray[np.uint8], _what: str) -> tuple[float, float]:
        return next(clicks)

    async def grab() -> NDArray[np.uint8]:
        return np.zeros((240, 320, 3), dtype=np.uint8)

    async def still(_p: float, _t: float) -> None:
        return None

    errors = await tool.floor_marks(cal, grab, click, answers(""), still)
    assert len(errors) == 2 and all(e < 0.005 for e in errors)


async def test_the_servo_step_fits_the_real_servo() -> None:
    tank = FakeTank("horizon")
    cal = calib(tilt0=TRUE_TILT0 + TILT_OFFSET)  # the mount step's result
    cal = await tool.servo_tilt(cal, tank.grab, tank.cam_set, answers(""))
    gain, offset = cal.tilt_map
    assert math.isclose(gain, TILT_GAIN, abs_tol=0.02) and math.isclose(offset, 0.0, abs_tol=0.3)
    assert tank.sets[-1] == (0.0, 0.0)  # back to the centre
    tank.scene = "vertical"
    cal = await tool.servo_pan(cal, tank.grab, tank.cam_set, answers(""))
    assert math.isclose(cal.pan_map[0], PAN_GAIN, abs_tol=0.02)


async def test_the_lens_step_calibrates_from_board_pictures(tmp_path: Path) -> None:
    png = tmp_path / "board.png"
    lens_opencv.checkerboard_png(png, square_px=40)
    board = cv2.imread(str(png), cv2.IMREAD_GRAYSCALE)
    assert board is not None
    h, w = board.shape
    src = np.array([[0, 0], [w, 0], [w, h], [0, h]], dtype=np.float32)
    rng = np.random.default_rng(5)
    pics: list[NDArray[np.uint8]] = []
    for _ in range(14):  # the board seen from different angles
        dst = np.array([[30, 20], [290, 30], [295, 220], [25, 210]], dtype=np.float32) + rng.normal(
            0, 12, (4, 2)
        )
        g: NDArray[np.uint8] = cv2.warpPerspective(  # pyright: ignore[reportAssignmentType]
            board, cv2.getPerspectiveTransform(src, dst.astype(np.float32)), (320, 240), borderValue=(255.0,)
        )
        pics.append(np.asarray(cv2.cvtColor(g, cv2.COLOR_GRAY2RGB), dtype=np.uint8))
    it = iter(pics)

    async def grab() -> NDArray[np.uint8]:
        return next(it)

    script = ["25"] + [""] * 14 + ["q"]
    k, dist, rms, n = await tool.lens(grab, answers(*script), tmp_path, show=lambda _p: None)
    # random warps are not one real camera, so only the flow is checked here; the lens accuracy is checked by
    # test_calibration_recovers_a_known_lens (rms < 0.1 px)
    assert n == 14 and math.isfinite(rms) and k[0, 0] > 0 and dist.shape == (5,)


def test_the_tilt_study_table_has_one_line_per_tilt() -> None:
    text = tool.study_table(cm.tilt_study(calib(tilt0=-7.0), [0.0, -10.0, -20.0]))
    assert text.count("\n") >= 3 and "-20" in text and "blind" in text


# ---- final review I3: tilt0 and the servo offset never count twice, whatever the order of the steps
async def test_servos_before_mount_does_not_double_the_tilt() -> None:
    tank = FakeTank("horizon")
    cal = calib(tilt0=0.0)
    cal = await tool.servo_tilt(cal, tank.grab, tank.cam_set, answers(""))  # servos first: gains only
    cal = await tool.mount_tilt(cal, tank.grab, tank.cam_set, answers(""))  # then mount
    cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    assert math.isclose(cam.real_tilt_deg(), TRUE_TILT0 + TILT_OFFSET, abs_tol=0.3)


# ---- final review I4: pictures of another size are refused
async def test_a_picture_of_another_size_is_refused() -> None:
    async def vga() -> NDArray[np.uint8]:
        return np.zeros((480, 640, 3), dtype=np.uint8)

    tank = FakeTank("horizon")
    try:
        await tool.mount_tilt(calib(), vga, tank.cam_set, answers(""))
    except ValueError as e:
        assert "320 x 240" in str(e)
    else:
        raise AssertionError("a VGA picture was accepted")


# ---- final review I2: a missed point is skipped, the rest still fitted
async def test_a_missed_servo_point_is_skipped() -> None:
    tank = FakeTank("horizon")
    cal = calib(tilt0=TRUE_TILT0 + TILT_OFFSET)
    real_grab = tank.grab
    calls = {"n": 0}

    async def sometimes_blank() -> NDArray[np.uint8]:
        calls["n"] += 1
        if calls["n"] == 2:
            return red_line_picture()  # no line in this one
        return await real_grab()

    cal = await tool.servo_tilt(cal, sometimes_blank, tank.cam_set, answers(""))
    assert math.isclose(cal.tilt_map[0], TILT_GAIN, abs_tol=0.02)


# ---- design-notes §130: gains only - the line may be a few cm off lens height and not straight ahead
async def test_the_servo_step_keeps_the_saved_tilt_and_centre() -> None:
    tank = FakeTank("horizon", line_off=-1.2)  # the line ~2 cm below lens height at 1 m
    cal = calib(tilt0=-0.15)
    cal = await tool.servo_tilt(cal, tank.grab, tank.cam_set, answers(""))
    assert cal.tilt0_deg == -0.15  # kept (the tape marks' tilt)
    assert math.isclose(cal.tilt_map[0], TILT_GAIN, abs_tol=0.02) and cal.tilt_map[1] == 0.0
    tank.scene, tank.line_off = "vertical", -21.0  # the vertical line well to the right
    cal = await tool.servo_pan(cal, tank.grab, tank.cam_set, answers(""))
    assert math.isclose(cal.pan_map[0], PAN_GAIN, abs_tol=0.02)
    assert cal.pan_map[1] == 0.0  # the Uno's camera centre is kept
    assert tank.sets[-1] == (0.0, 0.0)


async def test_the_servo_step_sweeps_both_ways_and_reports_the_slack(capsys: object) -> None:
    tank = FakeTank("horizon", slack=0.8)
    cal = await tool.servo_tilt(calib(tilt0=0.0), tank.grab, tank.cam_set, answers(""))
    assert math.isclose(cal.tilt_map[0], TILT_GAIN, abs_tol=0.02)  # the slack averages out
    tilts = [t for _, t in tank.sets]
    assert tilts.index(min(tilts)) < len(tilts) - 2  # down, then back up
    assert tilts.index(max(tilts)) < len(tilts) - 2
    out = capsys.readouterr().out  # type: ignore[attr-defined]
    assert "slack" in out and "0.8" in out


async def test_the_servo_step_asks_for_the_horizontal_line_first() -> None:
    tank = FakeTank("horizon")
    asked: list[str] = []

    async def ask(prompt: str) -> str:
        asked.append(prompt)
        return ""

    await tool.servo_tilt(calib(tilt0=0.0), tank.grab, tank.cam_set, ask)
    assert len(asked) == 1 and "roughly at the lens height" in asked[0] and tank.sets[1:] != []
