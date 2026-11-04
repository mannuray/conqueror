"""The ROOM tab's data on the AI feed (AI engine LLD §23.2-§23.4): the map as a picture of cell kinds and one
`room` message (trail, home, play area, remembered things with where words, names with their GO THERE spot,
the running skill's why, the floor seen), only to pages showing ROOM - the map at most once a second and the
room twice a second, each only when it changed. Names from the pages are answered here. Nothing here drives
the tank: FIND and GO THERE are the page's own Logo lines through the tank."""

from __future__ import annotations

import math
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from typing import Any, Protocol

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.feed import encode_map
from conqueror_ai.domain.memory import NEVER, Remembered
from conqueror_ai.domain.places import Place, Places, Refusal, where_words
from conqueror_ai.domain.room_map import MapSnapshot, note_points
from conqueror_ai.domain.room_picture import Trail, crop, kinds, png
from conqueror_ai.domain.types import Pose

MAP_EVERY_S = 1.0
ROOM_EVERY_S = 0.5
GO_MOVE_M = 0.2  # the GO THERE spots are worked out again when the tank moved this far
MAX_THINGS = 20
NO_ROOM = "I can't see the room yet."


class RoomPages(Protocol):
    """FeedServer as RoomFeed uses it."""

    def rooms(self) -> set[int]: ...

    def room_frame(self, data: bytes) -> None: ...

    def send_rooms(self, msg: dict[str, Any]) -> None: ...


@dataclass(frozen=True)
class RoomWorld:
    """What RoomFeed reads from the engine each time it builds."""

    snap: MapSnapshot | None
    seen_before: NDArray[np.bool_] | None
    things: Sequence[Remembered]  # remembered, not gone, newest first
    known: set[str]  # the Logo words the things list knows


def word_of(cls: str) -> str:
    """The Logo word of a remembered class: a colour-finder ball (ball:<name>) is ball."""
    return "ball" if cls.startswith("ball:") else cls


def _xy(p: tuple[float, float]) -> list[float]:
    return [round(p[0], 2), round(p[1], 2)]


class RoomFeed:
    def __init__(
        self,
        server: RoomPages,
        world: Callable[[], RoomWorld],
        places: Places,
        home: str | None,
        wall: Callable[[], float] = time.time,
    ) -> None:
        self.server, self.world, self.places, self.home, self.wall = server, world, places, home, wall
        self.trail = Trail()
        self.seq = 0
        self._had: set[int] = set()
        self._kinds: NDArray[np.uint8] | None = None  # the last map sent
        self._map_s = -math.inf
        self._room: dict[str, Any] | None = None  # the last room sent
        self._room_s = -math.inf
        self._go: dict[int, tuple[float, float] | None] = {}
        self._go_at: tuple[float, float] | None = None
        self._go_stale = True

    def tick(
        self,
        now_s: float,
        pose: Pose | None,
        area: tuple[float, float, float] | None,
        notes: dict[str, object],
    ) -> None:
        """~10 Hz from FeedPublisher.run: the trail always; the map and room only for pages showing ROOM."""
        if pose is not None:
            self.trail.add(pose.x_m, pose.y_m)
        rooms = self.server.rooms()
        new = bool(rooms - self._had)
        self._had = rooms
        if not rooms:
            self._kinds, self._room = None, None
            return
        w = self.world()
        if w.snap is not None:
            k = kinds(w.snap, w.seen_before)
            changed = self._kinds is None or not np.array_equal(k, self._kinds)
            if changed:
                self._go_stale = True
            if new or (changed and now_s - self._map_s >= MAP_EVERY_S):
                c = crop(k, w.snap.cell_m, k.shape[0] * w.snap.cell_m)
                if c.w > 0:
                    self.seq += 1
                    head = {
                        "seq": self.seq,
                        "cell_m": w.snap.cell_m,
                        "x0": round(c.x0, 3),
                        "y0": round(c.y0, 3),
                    }
                    self.server.room_frame(encode_map({**head, "w": c.w, "h": c.h}, png(c)))
                self._kinds, self._map_s = k, now_s
        room = self._room_msg(w, pose, area, notes)
        if new or (room != self._room and now_s - self._room_s >= ROOM_EVERY_S):
            self.server.send_rooms(room)
            self._room, self._room_s = room, now_s

    def on_name(self, msg: dict[str, Any]) -> dict[str, object]:
        """A page's name / rename / unname, answered with a rep (the next room carries the change)."""
        rid, name, place = msg.get("id"), msg.get("name"), msg.get("place")
        r: Place | Refusal | bool
        if msg.get("t") == "unname" and isinstance(place, int):
            r = self.places.unname(place)
            text = "Took the name off." if r else "I can't find that name."
        elif not isinstance(name, str):
            r, text = False, "Names are letters, spaces and - (up to 24)."
        elif isinstance(place, int):
            r = self.places.rename(place, name)
            text = r.msg if isinstance(r, Refusal) else f"Renamed it {r.name}."
        else:
            x, y, snap = msg.get("x"), msg.get("y"), self.world().snap
            if not _num(x) or not _num(y):
                r, text = False, "Tap a red or amber block to name it."
            elif snap is None:
                r, text = False, NO_ROOM
            else:
                r = self.places.name_at(snap, float(x), float(y), name)  # pyright: ignore[reportArgumentType]
                text = r.msg if isinstance(r, Refusal) else f"Named it {r.name}."
        ok = r is True or isinstance(r, Place)
        if ok:
            self._go_stale = True
        return {"t": "rep", "id": rid, "ok": ok, "msg": text}

    # ---- the room message
    def _room_msg(
        self,
        w: RoomWorld,
        pose: Pose | None,
        area: tuple[float, float, float] | None,
        notes: dict[str, object],
    ) -> dict[str, Any]:
        places = self.places.all()
        things = [t for t in w.things if word_of(t.word) in w.known and t.word not in NEVER][:MAX_THINGS]
        now = self.wall()
        out_things: list[dict[str, object]] = []
        for t in things:
            others = [(word_of(o.word), o.x_m, o.y_m) for o in things if o is not t]
            out_things.append(
                {
                    "word": word_of(t.word),
                    "x": round(t.x_m, 2),
                    "y": round(t.y_m, 2),
                    "ago_s": max(0, round(now - t.seen_wall_s)),
                    "where": where_words(t.x_m, t.y_m, places, others, self.home is not None),
                }
            )
        check = note_points(notes.get("check"))
        return {
            "t": "room",
            "trail": [_xy(p) for p in self.trail.points()],
            "home": self.home,
            "area": None if area is None else {"x": round(area[0], 2), "y": round(area[1], 2), "r": area[2]},
            "things": out_things,
            "names": [
                {"id": p.id, "name": p.name, "x": p.x_m, "y": p.y_m, "go": self._go_of(w.snap, p, pose)}
                for p in places
            ],
            "why": {
                "route": [_xy(p) for p in note_points(notes.get("route"))],
                "pending": [_xy(p) for p in note_points(notes.get("pending"))],
                "check": _xy(check[0]) if check else None,
                "looked": [_xy(p) for p in note_points(notes.get("looked"))],
            },
            "seen_m2": 0.0 if w.snap is None else round(float(w.snap.seen.sum()) * w.snap.cell_m**2, 1),
        }

    def _go_of(self, snap: MapSnapshot | None, p: Place, pose: Pose | None) -> list[float] | None:
        """The name's GO THERE spot, worked out again when a name or the map changed or the tank moved."""
        at = None if pose is None else (pose.x_m, pose.y_m)
        moved = at is not None and (self._go_at is None or math.dist(at, self._go_at) >= GO_MOVE_M)
        if self._go_stale or moved or self.places.changed:
            self._go, self._go_at, self._go_stale, self.places.changed = {}, at, False, False
        if p.id not in self._go:
            self._go[p.id] = None if snap is None else self.places.go_spot(snap, p, pose)
        g = self._go[p.id]
        return None if g is None else _xy(g)


def _num(v: object) -> bool:
    return isinstance(v, int | float) and not isinstance(v, bool) and math.isfinite(v)
