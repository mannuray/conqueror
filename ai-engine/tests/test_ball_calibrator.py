"""`cq-calibrate ball <name>` (AI engine LLD §16.1 item 5): the ball's colour from a few pictures, written to
config/balls.yaml beside the others. Drawn pictures and a fake camera."""

from __future__ import annotations

from pathlib import Path

import pytest

from conqueror_ai.adapters.detector_config import load_balls, save_ball
from conqueror_ai.app import calibrate
from conqueror_ai.domain import balls as bl
from tests.test_detectors import disc, floor_picture


async def test_the_ball_colour_comes_from_the_pictures_and_finds_that_ball() -> None:
    pics = [disc(floor_picture(640, 480), (330, 360), 24, (64, 90, 170)) for _ in range(15)]
    it = iter(pics)

    async def grab():  # type: ignore[no-untyped-def]
        return next(it)

    c = await calibrate.ball_colour("green", grab, n=15)
    assert c.name == "green" and c.h_lo <= 59 and c.h_hi >= 69
    assert len(bl.find_balls(pics[0], [c], horizon=240)) == 1
    assert bl.find_balls(floor_picture(640, 480), [c], horizon=240) == []


async def test_no_ball_in_view_is_a_plain_refusal() -> None:
    async def grab():  # type: ignore[no-untyped-def]
        return floor_picture(640, 480)

    with pytest.raises(calibrate.Refused, match="no coloured ball"):
        await calibrate.ball_colour("green", grab, n=5)


def test_saving_a_ball_keeps_the_others_and_replaces_its_own(tmp_path: Path) -> None:
    path = tmp_path / "balls.yaml"
    save_ball(path, bl.BallColour("green", 45, 90, 65, 200, 6.0))
    save_ball(path, bl.BallColour("red", 0, 8, 90, 230, 6.0))
    save_ball(path, bl.BallColour("green", 50, 80, 70, 190, 6.0))
    got = {b.name: b for b in load_balls(path)}
    assert set(got) == {"green", "red"} and got["green"].h_lo == 50 and got["red"].s_min == 90
