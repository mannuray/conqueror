"""World: the run's pure state, handed out as a SkillInput (AI engine LLD §19.5, review F1)."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from conqueror_ai.adapters.session import replay
from conqueror_ai.app import map as app_map
from conqueror_ai.app.objects import decode
from conqueror_ai.app.world import World
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.memory import ThingMemory
from conqueror_ai.domain.room_map import RoomMap
from conqueror_ai.domain.types import Frame
from tests.test_map_app import CFG, FloorDepth, session
from tests.test_objects import CAL
from tests.test_objects_app import GREY_JPEG


def world(**kw: object) -> World:
    return World(CAL, CFG, FloorDepth(), frozenset(), {}, Limits(), None, **kw)  # type: ignore[arg-type]


def feed(w: World, folder: Path) -> None:
    for e in replay(folder):
        if e.text is not None:
            w.on_text(e.text)
        elif e.frame is not None:
            rgb = decode(e.frame)
            if rgb is not None:
                w.on_picture(e.frame, rgb, [], with_depth=True)


def test_world_gives_the_maps_answers(tmp_path: Path) -> None:
    """The same recording through World and through cq-map's replay: the same map."""
    session(tmp_path / "s", dist_mm=400)
    old = app_map.run_replay(tmp_path / "s", CAL, CFG, FloorDepth())
    w = world()
    feed(w, tmp_path / "s")
    assert np.array_equal(w.map_run.room.seen, old.room.seen)
    assert np.array_equal(w.map_run.room.low, old.room.low)


def test_used_is_true_once_per_used_picture(tmp_path: Path) -> None:
    """Plan Z7 Review Focus 3 (§190 I2): only a picture used since the last input counts as looked at."""
    session(tmp_path / "s")
    w = world()
    events = list(replay(tmp_path / "s"))
    frames = [e.frame for e in events if e.frame is not None]
    for e in events:
        if e.text is not None:
            w.on_text(e.text)
    rgb = decode(frames[-1])
    assert rgb is not None
    assert not isinstance(w.on_picture(frames[-1], rgb, []), str)
    assert w.input(1.0, 1.0, True).used
    assert not w.input(1.1, 1.1, True).used  # no new picture
    small = np.zeros((10, 10, 3), np.uint8)
    f = Frame(seq=999, cap_us=frames[-1].cap_us + 1, recv_s=9.0, jpeg=GREY_JPEG)
    assert isinstance(w.on_picture(f, small, []), str)  # skipped (its size)
    assert not w.input(1.2, 1.2, True).used


def test_a_handed_in_room_and_memory_are_used() -> None:
    room, memory = RoomMap(CFG.map), ThingMemory()
    w = world(room=room, memory=memory)
    assert w.map_run.room is room and w.memory is memory


def test_the_area_starts_at_the_first_pose(tmp_path: Path) -> None:
    session(tmp_path / "s")
    w = world(area_m=1.0)
    assert w.input(0.0, 0.0, True).costs is None  # no pose yet: no map handed out
    for e in replay(tmp_path / "s"):
        if e.text is not None:
            w.on_text(e.text)
    inp = w.input(1.0, 1.0, True)
    assert inp.costs is not None and inp.costs.area == (0.0, 0.0, 1.0)


def test_camera_pan_for_the_room_tab_uses_the_calibration() -> None:
    """Final review M5: the camera view on the ROOM map is the real pan (pan_map), left +, as the heading."""
    from dataclasses import replace

    from conqueror_ai.app.world import MapRun

    run = MapRun(replace(CAL, pan_map=(0.94, 0.0)), CFG, FloorDepth())
    assert run.camera_pan_left_deg() is None  # no telemetry yet
    run.pan_servo_deg = 60.0  # 30 to the right by command
    assert run.camera_pan_left_deg() == pytest.approx(-28.2)


def test_fresh_after_a_turning_picture_but_not_used(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Plan Z10 (LLD §25 A1): a picture skipped for turning gives find maybes - `fresh`, not `used`."""
    session(tmp_path / "s")
    w = world()
    events = list(replay(tmp_path / "s"))
    frames = [e.frame for e in events if e.frame is not None]
    for e in events:
        if e.text is not None:
            w.on_text(e.text)
    small = np.zeros((10, 10, 3), np.uint8)
    f = Frame(seq=999, cap_us=frames[-1].cap_us + 1, recv_s=9.0, jpeg=GREY_JPEG)
    monkeypatch.setattr(w.map_run.p, "fresh", lambda: True)  # as the pipeline says after a turning picture
    assert isinstance(w.on_picture(f, small, []), str)
    inp = w.input(1.0, 1.0, True)
    assert inp.fresh and not inp.used
    assert not w.input(1.1, 1.1, True).fresh  # once per picture


def test_glimpses_of_the_goal_kind_since_the_last_input(monkeypatch: pytest.MonkeyPatch) -> None:
    """Plan Z10 (LLD §25 A2): every picture's glimpses of the thing sought, collected until the next input."""
    from conqueror_ai.domain.objects import Glimpse

    w = World(CAL, CFG, FloorDepth(), frozenset(), {}, Limits(), lambda c: c == "toy")
    shown = [Glimpse("toy", (0.0, 0.0), 0.1, 0.2, 1), Glimpse("chair", (0.0, 0.0), 0.3, 0.4, 1)]
    monkeypatch.setattr(w.map_run.p, "glimpses", lambda: shown)
    small = np.zeros((10, 10, 3), np.uint8)
    for k in range(2):
        w.on_picture(Frame(seq=k, cap_us=1_000_000 + k, recv_s=1.0, jpeg=GREY_JPEG), small, [])
    assert [g.cls for g in w.input(1.0, 1.0, True).glimpses] == ["toy", "toy"]
    assert w.input(1.1, 1.1, True).glimpses == ()
