"""RoomFeed: the ROOM tab's map and room on the AI feed, names from the pages (AI engine LLD §23.2-§23.4)."""

from __future__ import annotations

from typing import Any

import numpy as np

from conqueror_ai.app.room_feed import RoomFeed, RoomWorld, word_of
from conqueror_ai.domain.feed import decode_map
from conqueror_ai.domain.memory import Remembered
from conqueror_ai.domain.places import Places
from conqueror_ai.domain.room_map import MapSnapshot
from conqueror_ai.domain.types import Pose

N, CELL = 160, 0.05


class FakeRooms:
    def __init__(self) -> None:
        self.on: set[int] = set()
        self.maps: list[bytes] = []
        self.sent: list[dict[str, Any]] = []

    def rooms(self) -> set[int]:
        return set(self.on)

    def room_wanted(self) -> bool:
        return bool(self.on)

    def room_frame(self, data: bytes) -> None:
        self.maps.append(data)

    def send_rooms(self, msg: dict[str, Any]) -> None:
        self.sent.append(msg)

    def rooms_sent(self) -> list[dict[str, Any]]:
        return [m for m in self.sent if m["t"] == "room"]


def a_snap(block: bool = True) -> MapSnapshot:
    b = np.zeros((N, N), np.bool_)
    s = np.zeros((N, N), np.bool_)
    s[60:110, 60:100] = True  # floor seen round (0, 0)
    if block:
        b[96:104, 76:84] = True  # a 40 cm block round (1, 0)
    return MapSnapshot(0, None, CELL, b, np.zeros((N, N), np.bool_), s, False, True, True)


class Room:
    def __init__(self, things: list[Remembered] | None = None, known: set[str] | None = None) -> None:
        self.snap = a_snap()
        self.things = things if things is not None else [Remembered("ball:red", 0.5, 0.5, 990.0, "r1")]
        self.known = known if known is not None else {"ball", "toy"}

    def __call__(self) -> RoomWorld:
        return RoomWorld(self.snap, None, self.things, self.known)


POSE = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


def feed(room: Room | None = None, home: str | None = "room2") -> tuple[RoomFeed, FakeRooms]:
    srv = FakeRooms()
    return RoomFeed(srv, room or Room(), Places(), home, wall=lambda: 1000.0), srv


def test_word_of() -> None:
    assert word_of("ball:red") == "ball" and word_of("toy") == "toy"


def test_nothing_without_room_page() -> None:
    f, srv = feed()
    for i in range(5):
        f.tick(i * 0.5, Pose(0, i * 0.1, 0.0, 0.0, 0.0, 0.0), None, {})
    assert srv.maps == [] and srv.sent == []
    assert len(f.trail.points()) == 5  # the trail grows anyway


def test_map_at_once_on_room_on() -> None:
    f, srv = feed()
    srv.on = {1}
    f.tick(0.0, POSE, None, {})
    assert len(srv.maps) == 1 and len(srv.rooms_sent()) == 1
    head, png = decode_map(srv.maps[0])
    assert head["kind"] == "map" and head["cell_m"] == CELL and head["w"] > 0 and png[:4] == b"\x89PNG"


def test_map_once_a_second_only_on_change() -> None:
    room = Room()
    f, srv = feed(room)
    srv.on = {1}
    f.tick(0.0, POSE, None, {})
    f.tick(0.5, POSE, None, {})  # nothing changed
    assert len(srv.maps) == 1
    room.snap = a_snap(block=False)
    f.tick(0.6, POSE, None, {})  # changed, but within 1 s
    assert len(srv.maps) == 1
    f.tick(1.1, POSE, None, {})
    assert len(srv.maps) == 2


def test_room_twice_a_second_only_on_change() -> None:
    f, srv = feed()
    srv.on = {1}
    f.tick(0.0, POSE, None, {})
    f.tick(0.2, POSE, (0.0, 0.0, 3.0), {})  # changed within 0.5 s
    assert len(srv.rooms_sent()) == 1
    f.tick(0.6, POSE, (0.0, 0.0, 3.0), {})
    assert len(srv.rooms_sent()) == 2 and srv.rooms_sent()[-1]["area"] == {"x": 0.0, "y": 0.0, "r": 3.0}
    f.tick(1.2, POSE, (0.0, 0.0, 3.0), {})
    assert len(srv.rooms_sent()) == 2


def test_a_new_room_page_gets_both_at_once() -> None:
    f, srv = feed()
    srv.on = {1}
    f.tick(0.0, POSE, None, {})
    srv.on = {1, 2}
    f.tick(0.1, POSE, None, {})
    assert len(srv.maps) == 2 and len(srv.rooms_sent()) == 2


def test_things_logo_words() -> None:
    things = [
        Remembered("ball:red", 0.5, 0.5, 990.0, "r1"),
        Remembered("cup", 0.2, 0.0, 980.0, "r1"),  # no longer a known word
        Remembered("person", 0.3, 0.0, 970.0, "r1"),
        Remembered("toy", 2.0, 0.0, 400.0, "r1"),
    ]
    f, srv = feed(Room(things, {"ball", "toy", "person"}))
    srv.on = {1}
    f.tick(0.0, POSE, None, {})
    got = srv.rooms_sent()[0]["things"]
    assert [t["word"] for t in got] == ["ball", "toy"]
    assert got[0]["ago_s"] == 0 and got[0]["x"] == 0.5  # 10 s: "just now" (final review M6)
    assert got[1]["where"] == "2.0 m from home"


def test_name_then_room_carries_it() -> None:
    f, srv = feed()
    srv.on = {1}
    rep = f.on_name({"t": "name", "id": 7, "x": 1.0, "y": 0.3, "name": "Bed"})
    assert rep == {"t": "rep", "id": 7, "ok": True, "msg": "Named it bed."}
    f.tick(0.0, POSE, None, {})
    (name,) = srv.rooms_sent()[0]["names"]
    assert name["name"] == "bed" and name["go"] is not None and name["go"][0] < 0.8
    assert srv.rooms_sent()[0]["things"][0]["where"] == "near the bed"


def test_name_refused_reply() -> None:
    f, _ = feed()
    rep = f.on_name({"t": "name", "id": 8, "x": -1.0, "y": -1.0, "name": "bed"})
    assert rep == {"t": "rep", "id": 8, "ok": False, "msg": "Tap a red or amber block to name it."}
    assert f.on_name({"t": "name", "id": 9, "x": 1.0, "y": 0.0, "name": 3})["ok"] is False


def test_rename_and_unname() -> None:
    f, srv = feed()
    f.on_name({"t": "name", "id": 1, "x": 1.0, "y": 0.0, "name": "bed"})
    (p,) = f.places.all()
    assert f.on_name({"t": "name", "id": 2, "place": p.id, "name": "cot"})["msg"] == "Renamed it cot."
    assert f.on_name({"t": "unname", "id": 3, "place": p.id}) == {
        "t": "rep",
        "id": 3,
        "ok": True,
        "msg": "Took the name off.",
    }
    assert f.on_name({"t": "unname", "id": 4, "place": p.id})["ok"] is False
    srv.on = {1}
    f.tick(0.0, POSE, None, {})
    assert srv.rooms_sent()[0]["names"] == []


def test_why_empty_between_words() -> None:
    f, srv = feed()
    srv.on = {1}
    f.tick(0.0, POSE, None, {"route": [(0.0, 0.0), (1.0, 0.5)], "pending": [(2.0, 0.0)], "check": (2.0, 0.0)})
    why = srv.rooms_sent()[0]["why"]
    assert why == {
        "route": [[0.0, 0.0], [1.0, 0.5]],
        "pending": [[2.0, 0.0]],
        "check": [2.0, 0.0],
        "looked": [],
    }
    f.tick(1.0, POSE, None, {})
    assert srv.rooms_sent()[-1]["why"] == {"route": [], "pending": [], "check": None, "looked": []}


def test_room_message_shape() -> None:
    f, srv = feed(home=None)
    srv.on = {1}
    f.tick(0.0, POSE, None, {})
    m = srv.rooms_sent()[0]
    assert set(m) == {"t", "trail", "home", "area", "things", "names", "why", "seen_m2"}
    assert m["home"] is None and m["area"] is None and m["seen_m2"] == round(50 * 40 * CELL * CELL, 1)
    assert m["things"][0]["where"].endswith("from the start")


def test_go_spots_at_most_twice_a_second_while_the_map_changes() -> None:
    """Final review I1: the GO THERE spots (~11 ms each) are worked out only when a room message is due."""
    room = Room()
    f, srv = feed(room)
    f.on_name({"t": "name", "id": 1, "x": 1.0, "y": 0.3, "name": "bed"})
    calls: list[int] = []
    real = f.places.go_spot

    def counted(*a: Any) -> Any:
        calls.append(1)
        return real(*a)

    f.places.go_spot = counted  # type: ignore[method-assign]
    srv.on = {1}
    for i in range(20):  # 2 s at 10 Hz, the map different on every tick
        room.snap = a_snap(block=i % 2 == 0)
        f.tick(i * 0.1, Pose(0, i * 0.01, 0.0, 0.0, 0.0, 0.0), None, {})
    assert len(calls) <= 5, len(calls)


def test_ago_in_the_words_steps() -> None:
    """Final review M6: ago_s in the steps the page shows, so an idle room is not sent again every second."""
    from conqueror_ai.app.room_feed import ago_step

    assert [ago_step(s) for s in (5, 59, 61, 150, 3599, 3601, 90000)] == [0, 0, 60, 120, 3540, 3600, 86400]


def test_unname_without_a_place_is_said_plainly() -> None:
    f, _ = feed()
    assert f.on_name({"t": "unname", "id": 3, "place": "x"})["msg"] == "I can't find that name."
