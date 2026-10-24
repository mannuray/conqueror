"""`cq-calibrate ball <name>` (AI engine LLD §16.1 item 5): the ball's colour from a few pictures, written to
config/balls.yaml beside the others. Drawn pictures and a fake camera."""

from __future__ import annotations

import asyncio
from pathlib import Path

import cv2
import numpy as np
import pytest

from conqueror_ai.adapters.detector_config import load_balls, save_ball
from conqueror_ai.app import calibrate
from conqueror_ai.domain import balls as bl
from conqueror_ai.domain.types import Frame
from tests.test_detectors import disc, floor_picture


async def no_move(pan: float, tilt: float) -> None:
    pass


async def test_the_camera_is_set_level_and_centred_before_the_pictures() -> None:
    """At the driving tilt (-15 deg) a ball 1 m ahead is above the middle row and the lower-half search misses
    it (final review I3): the step sets the camera first, which also checks the tank is there."""
    log: list[object] = []

    async def cam_set(pan: float, tilt: float) -> None:
        log.append((pan, tilt))

    async def grab():  # type: ignore[no-untyped-def]
        log.append("picture")
        return disc(floor_picture(640, 480), (330, 360), 24, (64, 90, 170))

    await calibrate.ball_colour("green", grab, cam_set, n=3)
    assert log == [(0.0, 0.0), "picture", "picture", "picture"]


class Frames:
    def __init__(self, jpeg: bytes | None) -> None:
        self.jpeg = jpeg

    async def next_frame(self) -> Frame:
        if self.jpeg is None:
            await asyncio.sleep(10)  # the tank is off: no picture ever comes
        return Frame(0, 1, 0.0, self.jpeg or b"")


async def test_no_picture_is_a_plain_refusal_not_a_hang() -> None:
    with pytest.raises(calibrate.Refused, match="no picture"):
        await calibrate.grab_picture(Frames(None), timeout_s=0.05)


async def test_a_broken_picture_is_a_plain_refusal() -> None:
    with pytest.raises(calibrate.Refused, match="broken picture"):
        await calibrate.grab_picture(Frames(b"not a jpeg"), timeout_s=1.0)


async def test_a_picture_is_decoded_to_rgb() -> None:
    bgr = np.zeros((4, 6, 3), np.uint8)
    bgr[:, :, 2] = 255  # red in OpenCV's BGR
    ok, jpg = cv2.imencode(".jpg", bgr)
    assert ok
    rgb = await calibrate.grab_picture(Frames(jpg.tobytes()), timeout_s=1.0)
    assert rgb.shape == (4, 6, 3) and rgb[0, 0, 0] > 200 and rgb[0, 0, 2] < 50


async def test_the_ball_colour_comes_from_the_pictures_and_finds_that_ball() -> None:
    pics = [disc(floor_picture(640, 480), (330, 360), 24, (64, 90, 170)) for _ in range(15)]
    it = iter(pics)

    async def grab():  # type: ignore[no-untyped-def]
        return next(it)

    c = await calibrate.ball_colour("green", grab, no_move, n=15)
    assert c.name == "green" and c.h_lo <= 59 and c.h_hi >= 69
    assert len(bl.find_balls(pics[0], [c], horizon=240)) == 1
    assert bl.find_balls(floor_picture(640, 480), [c], horizon=240) == []


async def test_no_ball_in_view_is_a_plain_refusal() -> None:
    async def grab():  # type: ignore[no-untyped-def]
        return floor_picture(640, 480)

    with pytest.raises(calibrate.Refused, match="no coloured ball"):
        await calibrate.ball_colour("green", grab, no_move, n=5)


def test_saving_a_ball_keeps_the_others_and_replaces_its_own(tmp_path: Path) -> None:
    path = tmp_path / "balls.yaml"
    save_ball(path, bl.BallColour("green", 45, 90, 65, 200, 6.0))
    save_ball(path, bl.BallColour("red", 0, 8, 90, 230, 6.0))
    save_ball(path, bl.BallColour("green", 50, 80, 70, 190, 6.0))
    got = {b.name: b for b in load_balls(path)}
    assert set(got) == {"green", "red"} and got["green"].h_lo == 50 and got["red"].s_min == 90
