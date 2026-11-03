"""World: the run's pure state, handed out as a SkillInput (AI engine LLD §19.5, review F1)."""

from __future__ import annotations

from pathlib import Path

import numpy as np

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
