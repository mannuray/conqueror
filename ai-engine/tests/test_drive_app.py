"""cq-drive's own rules: when the map counts as fresh, and why a run ends (AI engine LLD §17.8)."""

from __future__ import annotations

import asyncio
import math
from pathlib import Path

import numpy as np
import pytest
from numpy.typing import NDArray

from conqueror_ai.app.drive import (
    FIRST_PICTURE_S,
    Kit,
    Mission,
    drive_session,
    goal_words,
    make_skill,
    map_state,
    parse_mission,
    picture_counts,
    refusal,
    task_failure,
)
from conqueror_ai.app.objects import decode
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.roam import RoamSkill
from conqueror_ai.domain.types import Detection, Pose
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


# ---- Plan Z5: wander, explore, find and goto on the room map
def test_cli_words() -> None:
    words = ["ball", "bottle"]
    assert parse_mission(["wander", "60"], words) == Mission("wander", None, 60.0)
    assert parse_mission(["wander"], words) == Mission("wander", None, None)
    assert parse_mission(["explore"], words) == Mission("explore", None, None)
    assert parse_mission(["find", "ball"], words) == Mission("find", "ball", None)
    assert parse_mission(["goto", "bottle"], words) == Mission("goto", "bottle", None)
    for bad in (["find", "person"], ["goto"], ["wander", "soon"], ["explore", "ball"], ["dance"]):
        with pytest.raises(SystemExit):
            parse_mission(bad, words)


def test_cli_play_area() -> None:
    """--area (owner: only when asked, design-notes §174): wander, explore and find; not goto, not <= 0."""
    words = ["ball"]
    assert parse_mission(["wander", "60"], words, area_m=2.0) == Mission("wander", None, 60.0, 2.0)
    assert parse_mission(["explore"], words, area_m=1.5).area_m == 1.5
    assert parse_mission(["find", "ball"], words, area_m=1.0).area_m == 1.0
    bad_ones = ((["goto", "ball"], 2.0), (["wander"], 0.0), (["explore"], -1.0), (["wander"], math.nan))
    for bad, area in bad_ones:
        with pytest.raises(SystemExit):
            parse_mission(bad, words, area_m=area)
    lim = Limits()
    roam = make_skill(parse_mission(["find", "ball"], words, area_m=1.0), lim)
    assert isinstance(roam, RoamSkill) and roam.area_m == 1.0 and roam.find
    roam = make_skill(parse_mission(["wander", "30"], words), lim)
    assert isinstance(roam, RoamSkill) and roam.area_m is None


def test_explore_refuses_without_depth() -> None:
    reason = refusal(Mission("explore", None, None), has_depth=False)
    assert reason is not None and "explore needs the depth model" in reason
    assert refusal(Mission("explore", None, None), has_depth=True) is None
    assert refusal(Mission("wander", None, None), has_depth=False) is None  # the sonar only, 18 cm/s


def test_mapgate_passes_detections(tmp_path: Path) -> None:
    from conqueror_ai.adapters.session import replay
    from conqueror_ai.app.map import MapRun
    from conqueror_ai.domain.depth import MapConfig
    from conqueror_ai.domain.objects import FLOOR_CLASSES, ObjectPipeline
    from tests.test_objects import CAL, box_on_floor, cam_at
    from tests.test_objects_app import session

    session(tmp_path / "s")
    run = MapRun(CAL, MapConfig(), None, p=ObjectPipeline(CAL, FLOOR_CLASSES))
    ball = box_on_floor(cam_at(), 1.0, 0.1)
    for e in replay(tmp_path / "s"):
        if e.text is not None:
            run.on_text(e.text)
        elif e.frame is not None:
            rgb = decode(e.frame)
            assert rgb is not None
            run.gate(e.frame, rgb, [ball])
    assert [o.cls for o in run.p.objects()] == [ball.cls]


async def test_drive_run_never_moves_without_a_fresh_map() -> None:
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.drive_link import DriveLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource
    from conqueror_ai.domain.depth import MapConfig
    from conqueror_ai.domain.drive.limits import Limits
    from tests.fake_bridge import FakeBridge
    from tests.test_objects import CAL
    from tests.test_objects_app import GREY_JPEG

    bridge = FakeBridge(jpeg=GREY_JPEG, pan_tilt=(90.0, 90.0), frames_for_s=0.0)  # telemetry, no pictures
    await bridge.start()
    try:
        link = BridgeLink("127.0.0.1", bridge.ws_port)
        cam = MjpegSource("127.0.0.1", bridge.stream_port)
        drive = DriveLink("127.0.0.1", bridge.ws_port)
        kit = Kit(CAL, MapConfig(), NoDetector(), frozenset(), {}, Limits(), None)
        result = await drive_session(link, cam, drive, Mission("wander", None, None), kit)
    finally:
        await bridge.stop()
    assert "no pictures" in result
    moving = [m for m in bridge.received if m.get("t") == "drive_rate" and (m.get("v") or m.get("yaw_rate"))]
    assert moving == []


class NoDetector:
    def detect(self, rgb: NDArray[np.uint8], cam: CameraModel | None) -> list[Detection]:
        return []
