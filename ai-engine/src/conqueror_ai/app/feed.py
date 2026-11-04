"""What cq-drive serve tells the /ai pages (AI engine LLD §22.1-§22.5): each picture with its boxes and
depth (only while a page is connected), the status about 10 times a second, Why? and the things list when
they change. It does no model work of its own - only JPEG encoding of what Perception and the skill made."""

from __future__ import annotations

import asyncio
import contextlib
import math
import time
from collections.abc import Callable, Sequence
from typing import TYPE_CHECKING, Protocol

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters.feed_server import FeedServer
from conqueror_ai.app.room_feed import RoomFeed
from conqueror_ai.app.runner import TickView
from conqueror_ai.domain.feed import encode_frame, page_boxes
from conqueror_ai.domain.pictures import cam_jpeg, depth_jpeg
from conqueror_ai.domain.thinking import Thinking, why_lines
from conqueror_ai.domain.types import Detection, Pose

if TYPE_CHECKING:
    from conqueror_ai.domain.drive.skill import SkillInput, SkillOut

STATUS_S = 0.1  # ~10 Hz
Things = Callable[[], list[dict[str, object]]]
# What the status needs from the tank each time: the pose, battery low, health ("ok" or why not), the camera's
# real pan from straight ahead, left + (None: no telemetry yet).
Telemetry = Callable[[], tuple[Pose | None, bool, str, float | None]]


class Pages(Protocol):
    """FeedServer as the publisher uses it."""

    @property
    def pages(self) -> int: ...

    def frame(self, data: bytes) -> None: ...

    def send(self, msg: dict[str, object]) -> None: ...


class TickFan:
    """Several TickViews as one (the runner has one view slot: the Rerun viewer and the pages)."""

    def __init__(self, views: Sequence[TickView]) -> None:
        self.views = list(views)

    def tick(
        self, now_s: float, inp: SkillInput, out: SkillOut, state: str, debug: dict[str, object]
    ) -> None:
        for v in self.views:
            v.tick(now_s, inp, out, state, debug)


class FeedPublisher:
    def __init__(self, server: Pages, things: Things) -> None:
        self.server, self.things = server, things
        self.thinking = Thinking()
        self.seq = 0
        self._word: str | None = None  # find, wander, explore, setxy
        self._thing: str | None = None
        self._id: int | None = None
        self._phase: str | None = None
        self._result: str | None = None
        self._learning: str | None = None
        self._debug: dict[str, object] = {}
        self._elapsed_s = 0.0
        self._pose: Pose | None = None
        self._battery_low = False
        self._health = "ok"
        self._sent_why: list[str] | None = None
        self._sent_things: list[dict[str, object]] | None = None
        self._pan: float | None = None
        self.room: RoomFeed | None = None  # the ROOM tab's data (LLD §23.4), set by serve
        self.area: tuple[float, float, float] | None = None  # the running (else last) word's play area

    # ---- what happens
    def on_frame(
        self, cap_us: int, rgb: NDArray[np.uint8], dets: list[Detection], inv: NDArray[np.floating] | None
    ) -> None:
        if self.server.pages == 0:
            return
        self.seq += 1
        h, w = rgb.shape[:2]
        head = {"seq": self.seq, "t_ms": cap_us // 1000, "boxes": page_boxes(dets, w, h)}
        self.server.frame(encode_frame(head, cam_jpeg(rgb), None if inv is None else depth_jpeg(inv)))

    def tick(
        self, now_s: float, inp: SkillInput, out: SkillOut, state: str, debug: dict[str, object]
    ) -> None:
        """The runner's view (TickView): the word's phase and notes, after each command."""
        self._phase, self._debug, self._elapsed_s = state, debug, now_s

    def word(self, word: str | None, thing: str | None, id: int | None) -> None:
        self._word, self._thing, self._id = word, thing, id
        self._phase, self._result, self._debug, self._elapsed_s = None, None, {}, 0.0

    def ended(self, result: str) -> None:
        self._result = result

    def learning(self, word: str | None) -> None:
        self._learning = word

    # ---- what the pages get
    def _status(self) -> dict[str, object]:
        th, _ = self.thinking.update(
            self._word,
            self._phase,
            self._result,
            self._thing,
            battery_low=self._battery_low,
            learning=self._learning,
        )
        msg: dict[str, object] = {"t": "status", "state": th.state, "mood": th.mood, "say": th.say}
        if self._thing is not None:
            msg["word"] = self._thing
        if self._id is not None:
            msg["id"] = self._id
        p = self._pose
        if p is not None:
            msg["pose"] = {"x": round(p.x_m, 2), "y": round(p.y_m, 2), "h": round(math.degrees(p.th_rad))}
        if self._pan is not None:  # the camera from straight ahead, left + (as the heading)
            msg["pan"] = round(self._pan)
        msg["health"] = self._health
        msg["learning"] = self._learning is not None
        return msg

    def _why(self) -> list[str]:
        if self._word is None or not self._debug:
            return []
        at = None if self._pose is None else (self._pose.x_m, self._pose.y_m)
        return why_lines(self._word, self._thing, self._debug, at, self._elapsed_s)

    def greeting(self) -> list[dict[str, object]]:
        return [self._status(), {"t": "why", "lines": self._why()}, {"t": "things", "list": self.things()}]

    async def run(self, stop: asyncio.Event, telemetry: Telemetry) -> None:
        """The status every 0.1 s while a page is connected; why and things when they change."""
        while not stop.is_set():
            self._pose, self._battery_low, self._health, self._pan = telemetry()
            if self.room is not None:  # the trail always; the map and room only for pages on ROOM
                notes = self._debug if self._word is not None and self._result is None else {}
                self.room.tick(time.monotonic(), self._pose, self.area, notes)
            if self.server.pages > 0:
                self.server.send(self._status())
                why = self._why()
                if why != self._sent_why:
                    self._sent_why = why
                    self.server.send({"t": "why", "lines": why})
                things = self.things()
                if things != self._sent_things:
                    self._sent_things = [dict(t) for t in things]  # a copy: the caller may change its list
                    self.server.send({"t": "things", "list": things})
            else:
                self._sent_why = self._sent_things = None  # a new page gets them in its greeting
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(stop.wait(), STATUS_S)


def make_feed(page_port: int, things: Things) -> tuple[FeedServer, FeedPublisher] | None:
    """--page-port N: the feed server and its publisher; 0 turns the feed off."""
    if page_port <= 0:
        return None
    holder: list[FeedPublisher] = []
    server = FeedServer("0.0.0.0", page_port, greeting=lambda: holder[0].greeting())
    holder.append(FeedPublisher(server, things))
    return server, holder[0]
