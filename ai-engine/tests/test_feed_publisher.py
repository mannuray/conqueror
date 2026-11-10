"""FeedPublisher: what the /ai pages get from cq-drive serve - frames only with a page, status ~10 Hz, why and
things on change (AI engine LLD §22.5)."""

from __future__ import annotations

import asyncio
import time
from typing import Any

import numpy as np
import pytest

from conqueror_ai.app import feed as feed_mod
from conqueror_ai.app.feed import FeedPublisher, TickFan, make_feed
from conqueror_ai.domain.feed import decode_frame
from conqueror_ai.domain.names import Names
from conqueror_ai.domain.types import Detection, Pose
from tests.test_names import vocab


class FakeServer:
    def __init__(self, pages: int = 1) -> None:
        self.pages = pages
        self.frames: list[bytes] = []
        self.sent: list[dict[str, Any]] = []

    def frame(self, data: bytes) -> None:
        self.frames.append(data)

    def send(self, msg: dict[str, Any]) -> None:
        self.sent.append(msg)

    def kinds(self, t: str) -> list[dict[str, Any]]:
        return [m for m in self.sent if m["t"] == t]


THINGS = [{"word": "toy", "names": ["car"], "measured": True}]
RGB = np.zeros((480, 640, 3), np.uint8)


def telem() -> tuple[Pose | None, bool, str, float | None]:
    return Pose(0, 1.2, -0.4, 1.5708, 0.0, 0.0), False, "ok", -15.2


def test_no_page_no_encoding(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[int] = []

    def counted(rgb: object) -> bytes:
        calls.append(1)
        return b""

    monkeypatch.setattr(feed_mod, "cam_jpeg", counted)
    srv = FakeServer(pages=0)
    FeedPublisher(srv, lambda: THINGS).on_frame(1_000_000, RGB, [], None)  # pyright: ignore[reportArgumentType]
    assert calls == [] and srv.frames == []


def test_frame_has_boxes_cam_and_depth() -> None:
    srv = FakeServer()
    pub = FeedPublisher(srv, lambda: THINGS)  # pyright: ignore[reportArgumentType]
    dets = [Detection("toy", 0.91, (64.0, 48.0, 128.0, 96.0))]
    pub.on_frame(2_500_000, RGB, dets, np.ones((392, 518), np.float32))
    pub.on_frame(2_600_000, RGB, dets, None)
    head, cam, depth = decode_frame(srv.frames[0])
    assert head["seq"] == 1 and head["t_ms"] == 2500 and head["boxes"] == [["toy", 0.91, 100, 100, 100, 100]]
    assert cam[:2] == b"\xff\xd8" and depth is not None and depth[:2] == b"\xff\xd8"
    head, _, depth = decode_frame(srv.frames[1])
    assert head["seq"] == 2 and depth is None


async def test_status_about_10hz() -> None:
    srv = FakeServer()
    pub = FeedPublisher(srv, lambda: THINGS)  # pyright: ignore[reportArgumentType]
    stop = asyncio.Event()
    task = asyncio.create_task(pub.run(stop, telem))
    t0 = time.monotonic()
    await asyncio.sleep(1.0)
    stop.set()
    await task
    n = len(srv.kinds("status"))
    assert 8 <= n <= 11, n
    s = srv.kinds("status")[-1]
    assert s["state"] == "ready" and s["say"] == "Ready to play!" and s["health"] == "ok"
    assert s["pose"] == {"x": 1.2, "y": -0.4, "h": 90}
    assert time.monotonic() - t0 < 1.5


async def test_no_page_no_status() -> None:
    srv = FakeServer(pages=0)
    pub = FeedPublisher(srv, lambda: THINGS)  # pyright: ignore[reportArgumentType]
    stop = asyncio.Event()
    task = asyncio.create_task(pub.run(stop, telem))
    await asyncio.sleep(0.3)
    stop.set()
    await task
    assert srv.sent == []


async def test_why_and_things_only_on_change() -> None:
    things = list(THINGS)
    srv = FakeServer()
    pub = FeedPublisher(srv, lambda: things)  # pyright: ignore[reportArgumentType]
    stop = asyncio.Event()
    task = asyncio.create_task(pub.run(stop, telem))
    pub.word("find", "toy", 7)
    pub.tick(1.0, None, None, "check", {"state": "check", "check": (1.2, 0.0)})  # pyright: ignore[reportArgumentType]
    await asyncio.sleep(0.35)
    assert len(srv.kinds("why")) == 1 and len(srv.kinds("things")) == 1
    things.append({"word": "cup", "names": [], "measured": False})
    await asyncio.sleep(0.25)
    assert len(srv.kinds("why")) == 1 and len(srv.kinds("things")) == 2
    s = srv.kinds("status")[-1]
    assert s["state"] == "checking" and s["word"] == "toy" and s["id"] == 7
    stop.set()
    await task


def test_greeting_has_status_why_things() -> None:
    pub = FeedPublisher(FakeServer(), lambda: THINGS)  # pyright: ignore[reportArgumentType]
    assert [m["t"] for m in pub.greeting()] == ["status", "why", "things"]
    assert pub.greeting()[2]["list"] == THINGS


def test_ended_and_learning_show() -> None:
    srv = FakeServer()
    pub = FeedPublisher(srv, lambda: THINGS)  # pyright: ignore[reportArgumentType]
    pub.word("find", "toy", 3)
    pub.ended("not_found")
    assert pub.greeting()[0]["say"] == "I couldn't find the toy."
    pub.learning("cup")
    st = pub.greeting()[0]
    assert st["state"] == "learning" and st["learning"] is True


def test_tick_fan_calls_every_view() -> None:
    got: list[str] = []

    class V:
        def __init__(self, n: str) -> None:
            self.n = n

        def tick(self, now_s: float, inp: object, out: object, state: str, debug: dict[str, object]) -> None:
            got.append(self.n)

    TickFan([V("a"), V("b")]).tick(0.0, None, None, "drive", {})  # pyright: ignore[reportArgumentType]
    assert got == ["a", "b"]


def test_page_port_zero_no_feed() -> None:
    names = Names(vocab(), [])
    assert make_feed(0, names.page_list) is None
    made = make_feed(8770, names.page_list)
    assert made is not None and made[0].port == 8770


async def test_status_has_pan() -> None:
    """LLD §23.2: the camera view on the map - whole degrees from straight ahead, left +."""
    srv = FakeServer()
    pub = FeedPublisher(srv, lambda: THINGS)  # pyright: ignore[reportArgumentType]
    stop = asyncio.Event()
    task = asyncio.create_task(pub.run(stop, telem))
    await asyncio.sleep(0.25)
    stop.set()
    await task
    assert srv.kinds("status")[-1]["pan"] == -15


def test_marks_only_when_given() -> None:  # Plan Z11 (LLD §26.2): a used picture's classes
    from conqueror_ai.domain.feed import decode_marks

    srv = FakeServer()
    pub = FeedPublisher(srv, lambda: THINGS)  # pyright: ignore[reportArgumentType]
    pub.on_frame(1_000_000, RGB, [], np.ones((392, 518), np.float32), np.ones((60, 80), np.uint8))
    pub.on_frame(1_100_000, RGB, [], None)
    one = decode_marks(srv.frames[0])
    assert one is not None and one[:2] == (80, 60)
    assert decode_marks(srv.frames[1]) is None


# ---- Plan G3: the GAMES card's numbers (AI engine LLD §28.1, design-notes §224)
def test_status_says_whether_people_may_play() -> None:
    pub = FeedPublisher(FakeServer(), lambda: THINGS)  # pyright: ignore[reportArgumentType]
    st = pub.greeting()[0]
    assert st["people"] is False and "person" not in st
    pub.people = True
    assert pub.greeting()[0]["people"] is True


def test_status_carries_the_person_only_in_a_game() -> None:
    pub = FeedPublisher(FakeServer(), lambda: THINGS)  # pyright: ignore[reportArgumentType]
    wire = {"t": "person", "seen": True, "d_cm": 72, "bearing": 5, "coming": 1, "sure": 0.9, "age_ms": 80}
    game = {"on": False}
    pub.person = lambda: wire if game["on"] else None
    assert "person" not in pub.greeting()[0]
    game["on"] = True
    assert pub.greeting()[0]["person"] == {"seen": True, "d_cm": 72, "coming": 1}  # the bridge's own number
