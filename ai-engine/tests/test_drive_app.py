"""cq-drive's own rules: when the map counts as fresh, and why a run ends (AI engine LLD §17.8)."""

from __future__ import annotations

import asyncio

from conqueror_ai.app.drive import FIRST_PICTURE_S, goal_words, map_state, picture_counts, task_failure
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Pose
from conqueror_ai.domain.vocabulary import Vocabulary
from tests.test_camera import calib


def test_the_run_waits_for_the_first_picture_then_needs_fresh_ones() -> None:
    assert map_state(1.0, 0.0, 0.0, 0.95) == "waiting"  # the stream is still starting
    assert map_state(FIRST_PICTURE_S + 0.1, 0.0, 0.0, FIRST_PICTURE_S) == "no pictures from the camera"
    assert map_state(10.0, 0.0, 9.8, 9.9) is None
    assert map_state(10.0, 0.0, 9.0, 9.9) == "no new picture for 1.0 s"
    assert map_state(10.0, 0.0, 9.8, 9.5) == "no telemetry for 0.5 s"


def test_pictures_skipped_while_turning_still_show_the_camera_is_alive() -> None:
    cam = CameraModel(calib()).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    assert picture_counts(cam) and picture_counts("turning") and picture_counts("camera moving")
    assert not picture_counts("no pose") and not picture_counts("no camera angles")


async def test_a_crashed_worker_ends_the_run_with_its_error() -> None:
    async def fine() -> None:
        await asyncio.sleep(1.0)

    async def broken() -> None:
        raise OSError("stream closed")

    tasks = [asyncio.create_task(fine()), asyncio.create_task(broken())]
    await asyncio.sleep(0.01)
    assert task_failure(tasks) == "OSError: stream closed"
    assert task_failure(tasks[:1]) is None
    tasks[0].cancel()


def test_it_goes_to_a_ball_or_any_word_that_stands_on_the_floor() -> None:
    v = Vocabulary.from_dict(
        {
            "words": [
                {"word": "person", "floor": True},
                {"word": "bottle", "floor": True},
                {"word": "door", "floor": False},
            ]
        }
    )
    # a door has no place on the floor map to drive to; never drive at a person (a child is around: following
    # a person is a later lesson with its own rules)
    assert goal_words(v) == ["ball", "bottle"]
