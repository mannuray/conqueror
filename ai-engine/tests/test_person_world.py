"""The person boxes reach PersonWatch and only PersonWatch (AI engine LLD §28.1, Plan G2 Review Focus 1)."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import numpy as np
import pytest

from conqueror_ai.adapters.session import replay
from conqueror_ai.app.objects import decode
from conqueror_ai.app.world import World
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Detection, Frame
from tests.test_map_app import session
from tests.test_person_test import box_at
from tests.test_world import world


def ready(tmp: Path) -> tuple[World, list[Frame]]:
    session(tmp / "s")
    w = world()
    events = list(replay(tmp / "s"))
    for e in events:
        if e.text is not None:
            w.on_text(e.text)
    return w, [e.frame for e in events if e.frame is not None]


def picture(w: World, f: Frame, dets: list[Detection]) -> CameraModel | str:
    rgb = decode(f)
    assert rgb is not None
    return w.on_picture(f, rgb, dets)


def test_a_toy_inside_a_person_never_reaches_the_tracker_but_the_person_is_watched(tmp_path: Path) -> None:
    w, frames = ready(tmp_path)
    f = frames[-1]
    cam = w.map_run.p.camera_at(f.cap_us)
    assert isinstance(cam, CameraModel)
    x1, y1, x2, y2 = box_at(cam, 0.9, 0.0)
    person = Detection("person", 0.9, (x1, y1, x2, y2))
    toy = Detection("ball", 0.9, (x1 + 10, y2 - 30, x1 + 30, y2 - 10))  # held in the person's box
    assert not isinstance(picture(w, f, [person, toy]), str)
    assert w.map_run.p.objects() == []
    e = w.person.latest(f.cap_us)
    assert e is not None and e.seen and e.d_m is not None and 0.5 < e.d_m < 0.9
    inp = w.input(1.0, 1.0, True)
    assert inp.person is not None and inp.person.seen


def test_no_person_box_keeps_the_last_estimate_unseen(tmp_path: Path) -> None:
    w, frames = ready(tmp_path)
    f = frames[-1]
    cam = w.map_run.p.camera_at(f.cap_us)
    assert isinstance(cam, CameraModel)
    assert w.input(0.5, 0.5, True).person is None  # nobody seen yet
    x1, y1, x2, y2 = box_at(cam, 1.2, 10.0)
    picture(w, f, [Detection("person", 0.9, (x1, y1, x2, y2))])
    g = Frame(seq=f.seq + 1, cap_us=f.cap_us + 100_000, recv_s=f.recv_s + 0.1, jpeg=f.jpeg)
    assert not isinstance(picture(w, g, []), str)
    p = w.input(1.0, 1.0, True).person
    assert p is not None and not p.seen and p.bearing_deg > 5.0


def test_a_skipped_picture_does_not_touch_the_watch(tmp_path: Path) -> None:
    w, frames = ready(tmp_path)
    f = frames[-1]
    small = np.zeros((10, 10, 3), np.uint8)
    g = Frame(seq=999, cap_us=f.cap_us + 1, recv_s=9.0, jpeg=f.jpeg)
    assert isinstance(w.on_picture(g, small, [Detection("person", 0.9, (1.0, 1.0, 5.0, 9.0))]), str)
    assert w.person.latest(g.cap_us) is None


def test_a_turning_picture_still_sees_the_person(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Owner, design-notes §229: a picture skipped only for turning places the person with the gyro's heading,
    so a turn keeps the estimate fresh (the bridge's 300 ms deadman); the map still skips it."""
    w, frames = ready(tmp_path)
    f = frames[-1]
    cam = w.map_run.p.camera_at(f.cap_us)
    assert isinstance(cam, CameraModel)

    def skipped(why: str) -> Callable[..., str]:
        return lambda *_: why

    def rough(cap_us: int) -> CameraModel:
        return cam

    monkeypatch.setattr(w.map_run, "gate", skipped("turning"))
    monkeypatch.setattr(w.map_run.p.gate, "camera_turning", rough)
    x1, y1, x2, y2 = box_at(cam, 0.9, 0.0)
    assert picture(w, f, [Detection("person", 0.9, (x1, y1, x2, y2))]) == "turning"
    e = w.person.latest(f.cap_us)
    assert e is not None and e.seen
    monkeypatch.setattr(w.map_run, "gate", skipped("camera moving"))  # any other skip: not used
    g = Frame(seq=f.seq + 1, cap_us=f.cap_us + 100_000, recv_s=f.recv_s + 0.1, jpeg=f.jpeg)
    picture(w, g, [])
    assert w.person.last_us != g.cap_us
