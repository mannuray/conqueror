"""`cq-drive serve`: the skill server session (AI engine LLD §21) against the fake bridge, with a fake
perception (no camera, no tank) and scripted skills."""

from __future__ import annotations

import asyncio
import contextlib
import json
import math
import time
from collections.abc import AsyncIterator, Callable
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from typing import Any, cast

import pytest
from websockets.asyncio.client import connect

from conqueror_ai.adapters.feed_server import FeedServer
from conqueror_ai.adapters.home_store import HomeStore
from conqueror_ai.adapters.skill_link import SkillBusy, SkillLink, Start, ThingsRefused
from conqueror_ai.adapters.taught_words import TaughtWords
from conqueror_ai.app import serve
from conqueror_ai.app.feed import FeedPublisher
from conqueror_ai.app.serve import ServeApp, SkillCall, skill_call
from conqueror_ai.app.teach import Reply, Teacher
from conqueror_ai.app.world import World
from conqueror_ai.domain.drive.curves import STOP, Command
from conqueror_ai.domain.drive.roam import RoamSkill
from conqueror_ai.domain.drive.skill import Result, Skill, SkillInput, SkillOut
from conqueror_ai.domain.feed import decode_map
from conqueror_ai.domain.names import Names
from conqueror_ai.domain.places import Place
from conqueror_ai.domain.types import ObjectEstimate, Pose
from conqueror_ai.domain.vocabulary import Vocabulary, Word
from tests.fake_bridge import FakeBridge
from tests.test_drive_app import stored_home
from tests.test_world import world

NAMES_VOCAB = Vocabulary.from_dict(
    {
        "words": [
            {"word": "toy", "floor": True, "names": ["car", "racing car"]},
            {"word": "person", "floor": True, "names": ["man"]},
        ]
    }
)
NAMES = Names(NAMES_VOCAB, ["football"])
BRIDGE_NOW_S = 1000.0
FACING_X = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


class FakePerception:
    """No camera: a picture never comes (next_picture waits a little and gives None); the map is fresh."""

    def __init__(self) -> None:
        self.t0 = 0.0
        self.bridge_now: float | None = BRIDGE_NOW_S
        self.pulls = 0
        self.overlaps = 0  # two pullers of next_picture at once (there must be one)
        self.started = self.closed = False
        self._pulling = False

    def start(self, stop: asyncio.Event) -> None:
        self.t0 = time.monotonic()
        self.started = True

    async def next_picture(self, timeout_s: float) -> None:
        if self._pulling:
            self.overlaps += 1
        self._pulling = True
        try:
            self.pulls += 1
            await asyncio.sleep(min(timeout_s, 0.02))
        finally:
            self._pulling = False

    def state(self, now: float) -> str | None:
        return None

    def failure(self) -> str | None:
        return None

    def bridge_now_s(self) -> float | None:
        return self.bridge_now

    async def close(self) -> None:
        self.closed = True

    def reload_detector(self, det: object) -> None:
        self.reloaded = det


class Scripted:
    """Drives forward for `ticks` ticks, then ends with `result` (None: drives until stopped)."""

    def __init__(self, ticks: int | None = 5, result: Result = Result.DONE) -> None:
        self.ticks, self.result, self.n = ticks, result, 0

    @property
    def state(self) -> str:
        return "drive"

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        self.n += 1
        if self.ticks is not None and self.n > self.ticks:
            return SkillOut(STOP, result=self.result)
        return SkillOut(Command(20.0, 0.0))

    def cancel(self) -> SkillOut:
        return SkillOut(STOP)

    def debug(self) -> dict[str, object]:
        return {}


class Maker:
    def __init__(self, make: Callable[[], Skill] = Scripted) -> None:
        self.calls: list[SkillCall] = []
        self.make = make

    def __call__(self, call: SkillCall, w: World, seed: int) -> Skill:
        self.calls.append(call)
        return self.make()


@pytest.fixture
async def bridge() -> AsyncIterator[FakeBridge]:
    b = FakeBridge()
    await b.start()
    yield b
    await b.stop()


async def until(cond: Callable[[], bool], timeout_s: float = 3.0) -> None:
    async with asyncio.timeout(timeout_s):
        while not cond():
            await asyncio.sleep(0.01)


def msgs(b: FakeBridge, kind: str) -> list[dict[str, Any]]:
    return [m for m in b.received if m.get("t") == kind]


class Serving:
    def __init__(
        self, app: ServeApp, task: asyncio.Task[None], stop: asyncio.Event, p: FakePerception
    ) -> None:
        self.app, self.task, self.stop, self.p = app, task, stop, p

    async def close(self) -> None:
        self.stop.set()
        with contextlib.suppress(asyncio.CancelledError):
            await asyncio.wait_for(self.task, 3)


async def serving(
    b: FakeBridge,
    maker: Maker | None = None,
    w: World | None = None,
    home: str | None = None,
    store: HomeStore | None = None,
    feed: tuple[FeedServer, FeedPublisher] | None = None,
) -> Serving:
    p = FakePerception()
    link = SkillLink(f"ws://127.0.0.1:{b.ws_port}/ws", NAMES.wire())
    app = ServeApp(link, p, w or world(), NAMES, home, None, store=store, skills=maker or Maker(), feed=feed)
    stop = asyncio.Event()
    task = asyncio.create_task(app.run(stop))
    await until(lambda: app.state == "idle")
    return Serving(app, task, stop, p)


# ---- the skill call: the words and places (pure)
def start(skill: str, **kw: Any) -> Start:
    return Start(kw.get("id", 1), skill, kw.get("thing"), kw.get("seconds"), kw.get("to"), kw.get("area"))


def test_a_find_resolves_its_name_and_places_the_area() -> None:
    turned_left = Pose(0, 1.0, 2.0, math.pi / 2, 0.0, 0.0)
    c = skill_call(start("find", thing="Racing  Car", area=(2.0, 50.0, 0.0)), turned_left, NAMES)
    assert isinstance(c, SkillCall)
    assert (c.skill, c.word, c.area_m) == ("find", "toy", 2.0)
    assert c.centre is not None and c.centre == pytest.approx((1.0, 2.5))  # 50 cm ahead, facing +y


def test_setxy_goes_to_the_map_point() -> None:
    c = skill_call(start("setxy", to=(-50.0, 0.0)), Pose(0, 0.5, 0.0, 0.0, 0.0, 0.0), NAMES)
    assert isinstance(c, SkillCall) and c.point == pytest.approx((0.0, 0.0))  # FORWARD 50, SETXY 0 0: back
    c = skill_call(start("setxy", to=(0.0, -30.0)), FACING_X, NAMES)
    assert isinstance(c, SkillCall) and c.point == pytest.approx((0.0, -0.3))  # right of the tank


def test_area_zero_is_anywhere_and_wander_keeps_its_seconds() -> None:
    c = skill_call(start("wander", seconds=30.0, area=(0.0, 0.0, 0.0)), FACING_X, NAMES)
    assert isinstance(c, SkillCall) and (c.seconds, c.area_m, c.centre) == (30.0, None, None)
    c = skill_call(start("explore"), None, NAMES)  # no pose yet: the skill waits for one
    assert isinstance(c, SkillCall) and c.skill == "explore"


def test_what_cannot_start() -> None:
    for name in ("dragon", "person", "man", ""):
        assert skill_call(start("find", thing=name), FACING_X, NAMES) == "unknown_thing"
    assert skill_call(start("find"), FACING_X, NAMES) == "unknown_thing"
    assert skill_call(start("setxy", to=(10.0, 0.0)), None, NAMES) == "stale"  # no pose to place it from
    assert skill_call(start("setxy"), FACING_X, NAMES) == "stale"
    assert skill_call(start("dance"), FACING_X, NAMES) == "stale"
    # an area with no pose to place its centre from: refused (review minor 2), as setxy without a pose
    assert skill_call(start("wander", seconds=5.0, area=(2.0, 0.0, 0.0)), None, NAMES) == "stale"
    assert isinstance(skill_call(start("wander", seconds=5.0, area=(0.0, 0.0, 0.0)), None, NAMES), SkillCall)


def test_the_skills_are_roam_skills() -> None:
    w = world()
    for c in (
        SkillCall("find", "toy", None, None, 2.0, (0.0, 0.0)),
        SkillCall("wander", None, 30.0, None, None, None),
        SkillCall("explore", None, None, None, None, None),
        SkillCall("setxy", None, None, (1.0, 0.0), None, None),
    ):
        s = serve.make_word_skill(c, w, 0)
        assert isinstance(s, RoamSkill)
        assert s.mode == {"setxy": "point"}.get(c.skill, c.skill)
    s = serve.make_word_skill(SkillCall("setxy", None, None, (1.0, 0.5), 3.0, (1.0, 1.0)), w, 0)
    assert isinstance(s, RoamSkill) and s.point == (1.0, 0.5) and s.area_m == 3.0


# ---- the session
async def test_ask_answered(bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch) -> None:
    w = world()
    toy = ObjectEstimate(1, "toy", 1.0, 0.0, 0.0, 0.0, 0.05, 0, round((BRIDGE_NOW_S - 0.1) * 1e6), "seen")
    monkeypatch.setattr(w.map_run.p, "objects", lambda: [toy])
    monkeypatch.setattr(w.map_run.p, "pose", lambda: FACING_X)
    s = await serving(bridge, w=w)
    try:
        asks = [
            (1, "see?", "car"),
            (2, "howfar", "Racing car"),
            (3, "see?", "dragon"),
            (4, "howfar", "man"),
            (5, "see?", "person"),
        ]
        for i, q, thing in asks:
            await bridge.to_skill_server({"t": "skill_ask", "id": i, "q": q, "thing": thing})
        await until(lambda: len(msgs(bridge, "skill_answer")) == 5)
        got = {m["id"]: m["value"] for m in msgs(bridge, "skill_answer")}
        lens = w.lim.cam_x_m
        assert got == {1: True, 2: round((1.0 - lens) * 100), 3: False, 4: -1, 5: False}
        s.p.bridge_now = None  # no bridge clock yet: nothing is seen now
        await bridge.to_skill_server({"t": "skill_ask", "id": 6, "q": "see?", "thing": "toy"})
        await until(lambda: len(msgs(bridge, "skill_answer")) == 6)
        assert msgs(bridge, "skill_answer")[-1]["value"] is False
        assert msgs(bridge, "skill_drive") == []
    finally:
        await s.close()


async def test_find_drives_then_done(bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch) -> None:
    w = world()
    monkeypatch.setattr(w.map_run.p, "pose", lambda: FACING_X)
    maker = Maker()
    s = await serving(bridge, maker, w)
    try:
        area = {"r_m": 2, "fwd_cm": 0, "left_cm": 0}
        t0 = time.monotonic()
        await bridge.to_skill_server(
            {"t": "skill_start", "id": 4, "skill": "find", "thing": "car", "area": area}
        )
        await until(lambda: len(msgs(bridge, "skill_drive")) >= 1)
        assert time.monotonic() - t0 < 0.3  # the first frame within the bridge's 300 ms
        assert msgs(bridge, "skill_drive")[0]["v"] == 0
        await until(lambda: len(msgs(bridge, "skill_done")) == 1)
        assert msgs(bridge, "skill_done") == [{"t": "skill_done", "id": 4}]
        frames = msgs(bridge, "skill_drive")
        assert {m["id"] for m in frames} == {4} and any(m["v"] > 0 for m in frames)
        assert frames[-1]["v"] == 0  # finish: one stop
        assert maker.calls == [SkillCall("find", "toy", None, None, 2.0, (0.0, 0.0))]
        assert msgs(bridge, "skill_failed") == []
        n = len(frames)
        await asyncio.sleep(0.35)
        assert len(msgs(bridge, "skill_drive")) == n  # nothing after the word
        assert s.app.state == "idle" and w.goal is None
        assert s.p.overlaps == 0 and s.p.pulls > 0  # one puller of pictures, between words too
    finally:
        await s.close()


async def test_a_word_reaches_the_feed(bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch) -> None:
    """§22.3: the pages hear the word, its phase from the runner's ticks, and how it ended."""
    w = world()
    monkeypatch.setattr(w.map_run.p, "pose", lambda: FACING_X)
    server = FeedServer("127.0.0.1", 0)
    pub = FeedPublisher(server, NAMES.page_list)
    ticks: list[str] = []
    real_tick = pub.tick
    monkeypatch.setattr(pub, "tick", lambda *a: ticks.append(a[3]) or real_tick(*a))  # pyright: ignore
    s = await serving(bridge, Maker(), w, feed=(server, pub))
    try:
        await until(lambda: server.bound_port != 0)
        await bridge.to_skill_server({"t": "skill_start", "id": 4, "skill": "find", "thing": "car"})
        await until(lambda: len(msgs(bridge, "skill_done")) == 1)
        assert ticks and set(ticks) == {"drive"}
        status = pub.greeting()[0]
        assert status["state"] == "done" and status["say"] == "I found the toy!" and status["id"] == 4
    finally:
        await s.close()


async def test_start_while_learning_fails_learning(bridge: FakeBridge) -> None:
    """Review Focus 2: a word checked just before the teach began ends 7056 at once, never a frame."""
    s = await serving(bridge)
    try:
        s.app.teacher = SimpleNamespace(learning="cup")  # pyright: ignore[reportAttributeAccessIssue]
        await bridge.to_skill_server({"t": "skill_start", "id": 9, "skill": "find", "thing": "car"})
        await until(lambda: len(msgs(bridge, "skill_failed")) == 1)
        assert msgs(bridge, "skill_failed")[0]["reason"] == "learning"
        assert msgs(bridge, "skill_drive") == []
    finally:
        await s.close()


async def test_apply_words_updates_names_floor_things_and_pages(bridge: FakeBridge) -> None:
    w = world()
    server = FeedServer("127.0.0.1", 0)
    pub = FeedPublisher(server, NAMES.page_list)
    s = await serving(bridge, w=w, feed=(server, pub))
    made: list[tuple[Path, list[str]]] = []

    def make_detector(model: Path, vocab: Vocabulary) -> tuple[object, frozenset[str]]:
        made.append((model, [x.word for x in vocab.words]))
        return "new detector", frozenset({"toy", "cup"})

    s.app.make_detector = make_detector  # pyright: ignore[reportAttributeAccessIssue]
    try:
        vocab = Vocabulary.of((*NAMES_VOCAB.words, Word("cup", True, names=("mug",), measured=False)))
        await s.app.apply_words(vocab, Path("/taught/model"))
        assert made == [(Path("/taught/model"), [x.word for x in vocab.words])]
        assert s.p.reloaded == "new detector"  # pyright: ignore[reportAttributeAccessIssue]
        assert s.app.names.resolve("mug") == "cup"
        assert w.map_run.p.floor_classes() == frozenset({"toy", "cup"})
        assert {"word": "cup", "names": ["mug"]} in s.app.link.things
        await until(
            lambda: any(t["word"] == "cup" for m in msgs(bridge, "skill_things") for t in m["things"])
        )
        listed = cast("list[dict[str, object]]", pub.greeting()[2]["list"])
        cup = [t for t in listed if t["word"] == "cup"]
        assert cup == [{"word": "cup", "names": ["mug"], "measured": False}]
    finally:
        await s.close()


async def test_a_page_teach_is_answered(bridge: FakeBridge) -> None:
    """A page's teach goes to the Teacher with what serve knows (names, busy) and its rep comes back."""
    server = FeedServer("127.0.0.1", 0)
    s = await serving(bridge, feed=(server, FeedPublisher(server, NAMES.page_list)))
    asked: list[tuple[str, list[str], bool, bool]] = []

    class FakeTeacher:
        learning: str | None = None

        async def teach(self, word: str, names: list[str], known: Callable[[str], bool], busy: bool) -> Reply:
            asked.append((word, names, known("racing car"), busy))
            return Reply(True, "I know cup now.")

    s.app.teacher = FakeTeacher()  # pyright: ignore[reportAttributeAccessIssue]
    try:
        await until(lambda: server.bound_port != 0)
        async with connect(f"ws://127.0.0.1:{server.bound_port}/feed") as ws:
            await ws.send(json.dumps({"t": "teach", "id": 5, "word": "cup", "names": ["mug"]}))
            async with asyncio.timeout(3):
                while (m := json.loads(await ws.recv()))["t"] != "rep":
                    pass
        assert m == {"t": "rep", "id": 5, "ok": True, "msg": "I know cup now."}
        assert asked == [("cup", ["mug"], True, False)]
    finally:
        await s.close()


async def test_a_refused_things_list_fails_the_teach(bridge: FakeBridge) -> None:
    """Final review M7: the tank did not take the new list - the teach fails and nothing is swapped."""
    s = await serving(bridge)
    s.app.make_detector = lambda m, v: ("new detector", frozenset({"toy", "cup"}))  # pyright: ignore[reportAttributeAccessIssue]
    try:
        bridge.things_nack = "TOO_BIG"
        vocab = Vocabulary.of((*NAMES_VOCAB.words, Word("cup", True, measured=False)))
        with pytest.raises(ThingsRefused):
            await s.app.apply_words(vocab, Path("/taught/model"))
        assert not hasattr(s.p, "reloaded") and s.app.names.resolve("cup") is None
        assert {"word": "cup", "names": []} not in s.app.link.things
    finally:
        bridge.things_nack = None
        await s.close()


async def test_a_busy_page_port_is_said(bridge: FakeBridge, capsys: pytest.CaptureFixture[str]) -> None:
    """Final review M6: port 8770 taken (another cq-drive serve) is said on the laptop, not silent."""
    import socket

    taken = socket.socket()
    taken.bind(("127.0.0.1", 0))
    taken.listen()
    port = taken.getsockname()[1]
    server = FeedServer("127.0.0.1", port)
    s = await serving(bridge, feed=(server, FeedPublisher(server, NAMES.page_list)))
    try:
        await until(lambda: "could not start" in capsys.readouterr().out, 3.0)
    finally:
        await s.close()
        taken.close()


async def test_stopping_during_a_teach_takes_the_word_out(bridge: FakeBridge, tmp_path: Path) -> None:
    """Review Focus 3 / final review M9: Ctrl-C (stop) through ServeApp.run while a word is being learnt - the
    taught entry is removed and the tank leaves learning (the off, or the link's close)."""
    server = FeedServer("127.0.0.1", 0)
    s = await serving(bridge, feed=(server, FeedPublisher(server, NAMES.page_list)))
    taught = TaughtWords(tmp_path / "taught")

    async def hangs(extra: Path, out: Path) -> int:
        await asyncio.sleep(30)
        return 0

    async def installed() -> None:
        raise AssertionError("never reached")

    teacher = Teacher(taught, hangs, learning=s.app.on_learning, installed=installed)
    s.app.teacher = teacher
    await until(lambda: server.bound_port != 0)
    async with connect(f"ws://127.0.0.1:{server.bound_port}/feed") as ws:
        await ws.send(json.dumps({"t": "teach", "id": 1, "word": "cup", "names": []}))
        await until(lambda: teacher.learning == "cup")
        assert [w.word for w in taught.words()] == ["cup"]
        await s.close()
    assert taught.words() == [] and teacher.learning is None
    learning = [m["on"] for m in msgs(bridge, "skill_learning")]
    assert learning[0] is True
    # the tank leaves learning by the "off" or by the server's close (bridge LLD R27.1: the close clears it)
    await until(lambda: learning[-1] is False or bridge.skill_ws is None)


async def test_a_find_gives_the_world_its_goal(bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch) -> None:
    w = world()
    monkeypatch.setattr(w.map_run.p, "pose", lambda: FACING_X)
    seen: list[bool] = []

    def make() -> Skill:
        seen.append(w.goal is not None and w.goal.kind("toy") and not w.goal.kind("box"))
        seen.append(w.area == (0.5, 0.0, 2.0))  # the World's cost maps carry the word's play area
        return Scripted()

    s = await serving(bridge, Maker(make), w)
    try:
        area = {"r_m": 2, "fwd_cm": 50, "left_cm": 0}
        await bridge.to_skill_server(
            {"t": "skill_start", "id": 4, "skill": "find", "thing": "car", "area": area}
        )
        await until(lambda: len(msgs(bridge, "skill_done")) == 1)
        assert seen == [True, True] and w.goal is None and w.area is None
    finally:
        await s.close()


async def test_unknown_thing_fails_without_frames(bridge: FakeBridge) -> None:
    """Review Focus 5: a thing computed at run time is checked here; never a frame for it."""
    maker = Maker()
    s = await serving(bridge, maker)
    try:
        for i, thing in enumerate(("dragon", "person", "man"), start=10):
            await bridge.to_skill_server({"t": "skill_start", "id": i, "skill": "find", "thing": thing})
        await until(lambda: len(msgs(bridge, "skill_failed")) == 3)
        assert [(m["id"], m["reason"], m["thing"]) for m in msgs(bridge, "skill_failed")] == [
            (10, "unknown_thing", "dragon"),
            (11, "unknown_thing", "person"),
            (12, "unknown_thing", "man"),
        ]
        await asyncio.sleep(0.1)
        assert msgs(bridge, "skill_drive") == [] and maker.calls == []
    finally:
        await s.close()


async def test_second_start_refused(bridge: FakeBridge) -> None:
    s = await serving(bridge, Maker(lambda: Scripted(None)))
    try:
        await bridge.to_skill_server({"t": "skill_start", "id": 5, "skill": "wander", "s": 30})
        await until(lambda: len(msgs(bridge, "skill_drive")) >= 2)
        await bridge.to_skill_server({"t": "skill_start", "id": 6, "skill": "explore"})
        await until(lambda: len(msgs(bridge, "skill_failed")) == 1)
        assert msgs(bridge, "skill_failed")[0] | {"thing": ""} == {
            "t": "skill_failed",
            "id": 6,
            "reason": "stale",
            "thing": "",
        }
        await asyncio.sleep(0.15)
        assert {m["id"] for m in msgs(bridge, "skill_drive")} == {5}  # none for 6; 5 drives on
        assert s.app.state == "driving"
    finally:
        await s.close()


async def test_end_from_bridge_stops_at_once(bridge: FakeBridge) -> None:
    s = await serving(bridge, Maker(lambda: Scripted(None)))
    try:
        await bridge.to_skill_server({"t": "skill_start", "id": 7, "skill": "wander", "s": 30})
        await until(lambda: len(msgs(bridge, "skill_drive")) >= 3)
        await bridge.to_skill_server({"t": "skill_end", "id": 99, "why": "stop"})  # an old id: ignored
        await asyncio.sleep(0.15)
        assert s.app.state == "driving"
        await bridge.to_skill_server({"t": "skill_end", "id": 7, "why": "stop"})
        await until(lambda: s.app.state == "idle", 1.0)
        await asyncio.sleep(0.03)
        n = len(msgs(bridge, "skill_drive"))
        await asyncio.sleep(0.4)
        assert len(msgs(bridge, "skill_drive")) == n  # nothing more, not even finish's stop
        assert msgs(bridge, "skill_failed") == [] and msgs(bridge, "skill_done") == []  # the tank knows
    finally:
        await s.close()


async def test_link_lost_mid_skill_reconnects_idle(
    bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Review Focus 3: the WiFi drops while the tank drives - stop sending, reconnect, never resume."""
    monkeypatch.setattr(serve, "RECONNECT_S", 0.1)
    s = await serving(bridge, Maker(lambda: Scripted(None)))
    try:
        await bridge.to_skill_server({"t": "skill_start", "id": 8, "skill": "wander", "s": 30})
        await until(lambda: len(msgs(bridge, "skill_drive")) >= 3)
        hellos = len(msgs(bridge, "hello"))
        assert bridge.skill_ws is not None
        await bridge.skill_ws.close()
        await until(lambda: len(msgs(bridge, "hello")) > hellos)  # it came back
        await until(lambda: s.app.state == "idle")
        n = len(msgs(bridge, "skill_drive"))
        await asyncio.sleep(0.4)
        assert len(msgs(bridge, "skill_drive")) == n  # the old lease is not resumed
        assert msgs(bridge, "skill_failed") == [] and msgs(bridge, "skill_done") == []
        await bridge.to_skill_server({"t": "skill_ask", "id": 9, "q": "see?", "thing": "toy"})
        await until(lambda: len(msgs(bridge, "skill_answer")) == 1)  # and it serves again
    finally:
        await s.close()


async def test_home_saved_after_each_word(bridge: FakeBridge, tmp_path: Path) -> None:
    store = stored_home(tmp_path)
    saves: list[tuple[str, ...]] = []
    real = store.save

    def counted(name: str, home: Any) -> None:
        saves.append(home.runs)
        real(name, home)

    store.save = counted  # type: ignore[method-assign]
    s = await serving(bridge, home="h", store=store)
    try:
        for i in (1, 2):
            await bridge.to_skill_server({"t": "skill_start", "id": i, "skill": "wander", "s": 5})
            await until(lambda i=i: len(msgs(bridge, "skill_done")) == i)
            await until(lambda i=i: len(saves) == i)
        await bridge.to_skill_server({"t": "skill_ask", "id": 3, "q": "see?", "thing": "toy"})
        await until(lambda: len(msgs(bridge, "skill_answer")) == 1)
        assert len(saves) == 2  # a question is not an action word
        run_id = s.app.world.run_id
        assert run_id and saves == [("r0", run_id), ("r0", run_id)]  # one run: the session
        home = store.load("h")
        assert home is not None and [t.word for t in home.things] == ["ball"]  # the memory kept
    finally:
        await s.close()


async def test_ctrl_c_saves_the_home_and_sends_nothing(bridge: FakeBridge, tmp_path: Path) -> None:
    store = stored_home(tmp_path)
    s = await serving(bridge, Maker(lambda: Scripted(None)), home="h", store=store)
    await bridge.to_skill_server({"t": "skill_start", "id": 3, "skill": "wander", "s": 30})
    await until(lambda: len(msgs(bridge, "skill_drive")) >= 3)
    saves: list[str] = []
    real = store.save

    def counted(name: str, home: Any) -> None:
        saves.append(name)
        real(name, home)

    store.save = counted  # type: ignore[method-assign]
    s.task.cancel()  # asyncio.run cancels the main task on Ctrl-C
    with contextlib.suppress(asyncio.CancelledError):
        await asyncio.wait_for(s.task, 3)
    n = len(msgs(bridge, "skill_drive"))
    await asyncio.sleep(0.4)
    assert len(msgs(bridge, "skill_drive")) == n  # no frame after the cancel
    assert saves == ["h"]  # saved once
    home = store.load("h")
    assert home is not None and home.runs == ("r0", s.app.world.run_id)
    assert msgs(bridge, "skill_failed") == [] and msgs(bridge, "skill_done") == []  # the close rule: 7052
    assert s.p.closed
    await until(lambda: bridge.skill_ws is None)  # the link closed


async def test_another_server_is_a_plain_error(bridge: FakeBridge) -> None:
    s = await serving(bridge)
    try:
        p = FakePerception()
        other = ServeApp(
            SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", NAMES.wire()), p, world(), NAMES, None, None
        )
        with pytest.raises(SkillBusy):
            await asyncio.wait_for(other.run(asyncio.Event()), 3)
        assert p.closed
    finally:
        await s.close()


def test_serve_help(capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch) -> None:
    import sys

    from conqueror_ai.app import drive

    monkeypatch.setattr(sys, "argv", ["cq-drive", "serve", "--help"])
    with pytest.raises(SystemExit) as e:
        drive.main()
    assert e.value.code == 0
    out = capsys.readouterr().out
    assert "cq-drive serve" in out and "--home" in out and "--record" in out and "--view" in out


async def test_a_broken_skill_answers_stale(bridge: FakeBridge) -> None:
    def broken() -> Skill:
        raise RuntimeError("no planner")

    s = await serving(bridge, Maker(broken))
    try:
        await bridge.to_skill_server({"t": "skill_start", "id": 2, "skill": "explore"})
        await until(lambda: len(msgs(bridge, "skill_failed")) == 1)
        assert msgs(bridge, "skill_failed")[0]["reason"] == "stale"
        assert s.app.state == "idle"
    finally:
        await s.close()


def test_a_refused_things_list_is_a_plain_error(monkeypatch: pytest.MonkeyPatch) -> None:
    from conqueror_ai.adapters.skill_link import ThingsRefused

    class Refused:
        async def run(self, stop: asyncio.Event) -> None:
            raise ThingsRefused("TOO_BIG: Too many things or names, or one is too long")

    def build(*a: object) -> tuple[Refused, None]:
        return Refused(), None

    monkeypatch.setattr(serve, "_build", build)
    with pytest.raises(SystemExit, match="TOO_BIG: Too many things"):
        serve.main([])


async def test_find_and_explore_need_the_depth_model(
    bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch
) -> None:
    """As cq-drive's refusal: without depth only the sensor marks the map - find and explore would end at
    once. Refused before any frame; wander and setxy still go."""
    from conqueror_ai.domain.drive.limits import Limits
    from tests.test_world import CAL, CFG

    w = World(CAL, CFG, None, frozenset(), {}, Limits(), None)
    monkeypatch.setattr(w.map_run.p, "pose", lambda: FACING_X)
    maker = Maker()
    s = await serving(bridge, maker, w)
    try:
        await bridge.to_skill_server({"t": "skill_start", "id": 1, "skill": "find", "thing": "toy"})
        await bridge.to_skill_server({"t": "skill_start", "id": 2, "skill": "explore"})
        await until(lambda: len(msgs(bridge, "skill_failed")) == 2)
        assert [(m["id"], m["reason"]) for m in msgs(bridge, "skill_failed")] == [(1, "stale"), (2, "stale")]
        assert msgs(bridge, "skill_drive") == [] and maker.calls == []
        await bridge.to_skill_server({"t": "skill_start", "id": 3, "skill": "wander", "s": 5})
        await until(lambda: len(msgs(bridge, "skill_done")) == 1)
    finally:
        await s.close()


async def test_websocket_errors_while_reconnecting_are_retried(
    bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Review I1: a refused handshake and a socket closed during the hello, on flaky WiFi - serve retries
    every RECONNECT_S and later connects, never a traceback."""
    monkeypatch.setattr(serve, "RECONNECT_S", 0.1)
    s = await serving(bridge)
    try:
        bridge.reject_handshakes, bridge.drop_hellos = 1, 1
        hellos = len(msgs(bridge, "hello"))
        assert bridge.skill_ws is not None
        await bridge.skill_ws.close()
        await until(lambda: len(msgs(bridge, "hello")) >= hellos + 2)  # the dropped one, then the good one
        await until(lambda: s.app.state == "idle")
        assert not s.task.done() and bridge.reject_handshakes == 0 and bridge.drop_hellos == 0
        await bridge.to_skill_server({"t": "skill_ask", "id": 1, "q": "see?", "thing": "toy"})
        await until(lambda: len(msgs(bridge, "skill_answer")) == 1)
    finally:
        await s.close()


async def test_decisions_folders_never_overwrite(bridge: FakeBridge, tmp_path: Path) -> None:
    """Review minor 5: a bridge restart reuses skill ids; each word's folder has the session's own number."""
    p = FakePerception()
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", NAMES.wire())
    app = ServeApp(link, p, world(), NAMES, None, tmp_path, skills=Maker())
    stop = asyncio.Event()
    task = asyncio.create_task(app.run(stop))
    try:
        await until(lambda: app.state == "idle")
        for n in (1, 2):
            await bridge.to_skill_server({"t": "skill_start", "id": 7, "skill": "wander", "s": 5})
            await until(lambda n=n: len(msgs(bridge, "skill_done")) == n)
        assert sorted(d.name for d in tmp_path.iterdir()) == ["word-1-7", "word-2-7"]
        assert all((d / "decisions.jsonl").exists() for d in tmp_path.iterdir())
    finally:
        stop.set()
        await asyncio.wait_for(task, 3)


def test_a_setxy_word_with_an_area_centre_replays_identically(tmp_path: Path) -> None:
    """Review minor 6: `cq-drive --replay` rebuilds a serve word - setxy's point and the area's centre come
    from the decisions log, not the first pose."""
    from conqueror_ai.app.drive import replay_text
    from conqueror_ai.domain.drive.costmap import CostSettings
    from conqueror_ai.domain.drive.limits import Limits
    from conqueror_ai.domain.drive.sim import SimRoom, SimTank
    from conqueror_ai.domain.drive.skill import make_input
    from conqueror_ai.domain.memory import ThingMemory
    from conqueror_ai.domain.room_map import RoomMap

    lim, cost, memory = Limits(), CostSettings(), ThingMemory()
    call = SkillCall("setxy", None, None, (1.0, 0.4), 1.5, (0.6, 0.3))  # the centre is not the first pose
    folder = tmp_path / "word-1-4"
    log = serve.word_log(folder, call, 9, lim, cost, memory)
    skill = serve.word_skill(call, lim, cost, memory, 9)
    sim, room, tank = SimRoom([], walls_m=2.5), RoomMap(), SimTank()
    result = None
    for k in range(300):
        at, t_us = tank.at, k * 100_000
        room.update_picture(sim.marks(at), t_us)
        snap = room.snapshot(t_us, Pose(t_us, at[0], at[1], at[2], 0.0, 0.0), False, True)
        inp = make_input(at=at, snap=snap, map_ok=True, settings=cost, sonar_m=sim.sonar(at))
        out = skill.tick(k * 0.1, inp)
        log.tick(k * 0.1, inp, out, skill.state, skill.debug())
        tank.step(out.cmd, 0.1)
        if (result := out.result) is not None:
            break
    log.close()
    assert result == Result.DONE
    text = replay_text(folder)
    assert "identical" in text, text


async def test_names_from_home_and_saved(bridge: FakeBridge, tmp_path: Path) -> None:
    """LLD §23.3: a home's names come back on the ROOM tab; a new name is saved with the home."""
    store = stored_home(tmp_path)
    earlier = store.load("h")
    assert earlier is not None
    store.save("h", replace(earlier, places=(Place(1, "bed", 1.0, 0.0),)))
    server = FeedServer("127.0.0.1", 0)
    s = await serving(bridge, home="h", store=store, feed=(server, FeedPublisher(server, NAMES.page_list)))
    try:
        await until(lambda: server.bound_port != 0)
        async with connect(f"ws://127.0.0.1:{server.bound_port}/feed") as ws:
            await ws.send(json.dumps({"t": "room", "on": True}))
            async with asyncio.timeout(3):
                while not isinstance(raw := await ws.recv(), str) or json.loads(raw)["t"] != "room":
                    pass
            assert [n["name"] for n in json.loads(raw)["names"]] == ["bed"]
            await ws.send(json.dumps({"t": "name", "id": 2, "place": 1, "name": "cot"}))
            async with asyncio.timeout(3):
                while not isinstance(raw := await ws.recv(), str) or json.loads(raw)["t"] != "rep":
                    pass
            assert json.loads(raw) == {"t": "rep", "id": 2, "ok": True, "msg": "Renamed it cot."}
    finally:
        await s.close()
    home = store.load("h")
    assert home is not None and [p.name for p in home.places] == ["cot"]


async def test_room_on_again_after_reconnect(bridge: FakeBridge) -> None:
    """A new page connection starts with room off; after its room on it gets the map and room at once."""
    w = world()
    w.map_run.room.seen[70:90, 70:90] = True
    server = FeedServer("127.0.0.1", 0)
    s = await serving(bridge, w=w, feed=(server, FeedPublisher(server, NAMES.page_list)))
    try:
        await until(lambda: server.bound_port != 0)
        for _ in range(2):
            async with connect(f"ws://127.0.0.1:{server.bound_port}/feed") as ws:
                await ws.send(json.dumps({"t": "room", "on": True}))
                got: set[str] = set()
                async with asyncio.timeout(3):
                    while got != {"map", "room"}:
                        raw = await ws.recv()
                        if isinstance(raw, bytes):
                            got.add(str(decode_map(raw)[0]["kind"]))
                        elif json.loads(raw)["t"] == "room":
                            got.add("room")
    finally:
        await s.close()
