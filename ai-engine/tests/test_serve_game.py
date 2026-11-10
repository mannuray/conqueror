"""`cq-drive serve` plays (AI engine LLD §28, bridge LLD R34.3, Plan G2 Task 4): the hello's `people`, the
person stream only between `game on` and `game off`, back_ok from the trusted floor, the game words and the
game's person questions, a word ended `caught` quietly."""

from __future__ import annotations

import asyncio
import time
from collections.abc import AsyncIterator
from pathlib import Path
from typing import Any

import numpy as np
import pytest

from conqueror_ai.adapters.skill_link import SkillLink
from conqueror_ai.app import serve
from conqueror_ai.app.seeing_pass import SeeingPass, write_pass
from conqueror_ai.app.serve import ServeApp, SkillCall, skill_call
from conqueror_ai.app.world import World
from conqueror_ai.domain.drive.game_skills import EscapeSkill, FaceSkill, FollowSkill
from conqueror_ai.domain.drive.skill import Result
from tests.fake_bridge import FakeBridge
from tests.test_person_test import box_at, cam_down
from tests.test_room_map import marks
from tests.test_serve import (
    BRIDGE_NOW_S,
    FACING_X,
    NAMES,
    FakePerception,
    Maker,
    Scripted,
    Serving,
    msgs,
    start,
    until,
)
from tests.test_world import world

DET, CAL = "yoloworld-s-640.mlpackage", "2026-10-23"
S = 1_000_000


@pytest.fixture
async def bridge() -> AsyncIterator[FakeBridge]:
    b = FakeBridge()
    await b.start()
    yield b
    await b.stop()


async def serving(
    b: FakeBridge, w: World | None = None, maker: Maker | None = None, seeing: SeeingPass | None = None
) -> Serving:
    p = FakePerception()
    link = SkillLink(f"ws://127.0.0.1:{b.ws_port}/ws", NAMES.wire())
    app = ServeApp(link, p, w or world(), NAMES, None, None, skills=maker or Maker(), seeing=seeing)
    stop = asyncio.Event()
    task = asyncio.create_task(app.run(stop))
    await until(lambda: app.state == "idle")
    return Serving(app, task, stop, p)


def hellos(b: FakeBridge) -> list[dict[str, Any]]:
    return [m for m in b.received if m.get("t") == "hello" and m.get("client") == "ai-skills"]


def seen_person(w: World, d_m: float, t_s: float) -> None:
    """The World's person watch saw someone d_m ahead of the front edge in a picture at bridge time t_s."""
    cam = cam_down()
    w.person.update([box_at(cam, d_m, 0.0)], cam, None, round(t_s * S))


# ---- the hello
async def test_hello_people_false_without_a_pass(bridge: FakeBridge, tmp_path: Path) -> None:
    s = await serving(bridge, seeing=SeeingPass(tmp_path / "none.yaml", DET, CAL))
    try:
        assert hellos(bridge)[-1]["people"] is False
    finally:
        await s.close()


async def test_hello_people_true_with_a_matching_pass(bridge: FakeBridge, tmp_path: Path) -> None:
    write_pass(tmp_path / "p.yaml", DET, CAL, "2026-11-10", {})
    s = await serving(bridge, seeing=SeeingPass(tmp_path / "p.yaml", DET, CAL))
    try:
        assert hellos(bridge)[-1]["people"] is True
    finally:
        await s.close()


async def test_hello_people_false_for_another_detector(bridge: FakeBridge, tmp_path: Path) -> None:
    write_pass(tmp_path / "p.yaml", "yolo26n.pt", CAL, "2026-11-10", {})
    s = await serving(bridge, seeing=SeeingPass(tmp_path / "p.yaml", DET, CAL))
    try:
        assert hellos(bridge)[-1]["people"] is False
    finally:
        await s.close()


# ---- the person stream
async def test_the_person_stream_runs_only_between_game_on_and_off(bridge: FakeBridge) -> None:
    w = world()
    seen_person(w, 0.9, BRIDGE_NOW_S - 0.1)
    s = await serving(bridge, w)
    try:
        await asyncio.sleep(0.3)
        assert msgs(bridge, "person") == []  # no game: nothing streamed
        await bridge.to_skill_server({"t": "game", "on": True})
        await until(lambda: len(msgs(bridge, "person")) >= 1)
        n0, t0 = len(msgs(bridge, "person")), time.monotonic()
        await asyncio.sleep(1.0)
        rate = (len(msgs(bridge, "person")) - n0) / (time.monotonic() - t0)
        assert rate >= 5.0
        m = msgs(bridge, "person")[-1]
        assert m["seen"] is True and m["age_ms"] == 100 and 50 <= m["d_cm"] <= 90
        s.p.bridge_now = BRIDGE_NOW_S + 1.0  # the camera froze: no new picture, the bridge's clock goes on
        await until(lambda: msgs(bridge, "person")[-1]["age_ms"] >= 1100)  # Review Focus 4
        await bridge.to_skill_server({"t": "game", "on": False})
        await asyncio.sleep(0.15)
        n1 = len(msgs(bridge, "person"))
        await asyncio.sleep(0.4)
        assert len(msgs(bridge, "person")) == n1
    finally:
        await s.close()


async def test_nobody_seen_yet_streams_unseen(bridge: FakeBridge) -> None:
    s = await serving(bridge)
    try:
        await bridge.to_skill_server({"t": "game", "on": True})
        await until(lambda: len(msgs(bridge, "person")) >= 2)
        m = msgs(bridge, "person")[-1]
        assert m["seen"] is False and m["d_cm"] == -1
    finally:
        await s.close()


async def test_a_new_link_streams_nothing_until_game_on_again(bridge: FakeBridge) -> None:  # Review Focus 3
    s = await serving(bridge)
    try:
        await bridge.to_skill_server({"t": "game", "on": True})
        await until(lambda: len(msgs(bridge, "person")) >= 1)
        assert bridge.skill_ws is not None
        await bridge.skill_ws.close()
        await until(lambda: len(hellos(bridge)) == 2 and s.app.state == "idle", timeout_s=6.0)
        n = len(msgs(bridge, "person"))
        await asyncio.sleep(0.4)
        assert len(msgs(bridge, "person")) == n
    finally:
        await s.close()


# ---- back_ok
def trusted_room(w: World, t_s: float) -> None:
    pts = [(float(x), float(y)) for x in np.arange(-0.6, 0.0, 0.02) for y in np.arange(-0.2, 0.21, 0.02)]
    w.map_run.room.update_picture(marks(floor=pts), round(t_s * S), (0.0, 0.0))


@pytest.mark.parametrize("trusted", [True, False])
async def test_back_ok_answers_from_the_map(
    bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch, trusted: bool
) -> None:
    w = world()
    monkeypatch.setattr(w.map_run.p, "pose", lambda: FACING_X)
    if trusted:
        trusted_room(w, BRIDGE_NOW_S - 1.0)
    s = await serving(bridge, w)
    try:
        t0 = time.monotonic()
        await bridge.to_skill_server({"t": "skill_ask", "id": 7, "q": "back_ok", "cm": 30})
        await until(lambda: len(msgs(bridge, "skill_answer")) == 1)
        assert time.monotonic() - t0 < 0.3
        assert msgs(bridge, "skill_answer")[0] == {"t": "skill_answer", "id": 7, "value": trusted}
    finally:
        await s.close()


# ---- the game words
def test_the_game_words_take_only_a_person_word() -> None:
    c = skill_call(start("face", thing="Baby"), FACING_X, NAMES)
    assert isinstance(c, SkillCall) and c.skill == "face"
    assert skill_call(start("face", thing="teddy"), FACING_X, NAMES) == "unknown_thing"
    assert skill_call(start("find", thing="baby"), FACING_X, NAMES) == "unknown_thing"  # never a find
    assert skill_call(start("escape", thing="kid"), FACING_X, NAMES) == "stale"  # no cm


def test_the_game_words_build_the_game_skills() -> None:
    w = world()
    face = skill_call(start("face", thing="baby"), FACING_X, NAMES)
    esc = skill_call(serve.Start(1, "escape", "baby", None, None, None, cm=40.0), FACING_X, NAMES)
    fol = skill_call(serve.Start(1, "follow", "person", 5.0, None, None, stop_cm=30.0), FACING_X, NAMES)
    assert isinstance(face, SkillCall) and isinstance(esc, SkillCall) and isinstance(fol, SkillCall)
    assert isinstance(serve.make_word_skill(face, w, 0), FaceSkill)
    e = serve.make_word_skill(esc, w, 0)
    assert isinstance(e, EscapeSkill) and e.back_m == pytest.approx(0.40)
    f = serve.make_word_skill(fol, w, 0)
    assert isinstance(f, FollowSkill) and f.seconds == 5.0 and f.stop_m == pytest.approx(0.50)  # never < 50


async def test_skill_start_face_runs_and_find_baby_is_unknown(bridge: FakeBridge) -> None:
    maker = Maker()
    s = await serving(bridge, maker=maker)
    try:
        await bridge.to_skill_server({"t": "skill_start", "id": 3, "skill": "find", "thing": "baby"})
        await until(lambda: len(msgs(bridge, "skill_failed")) == 1)
        assert msgs(bridge, "skill_failed")[0]["reason"] == "unknown_thing"
        await bridge.to_skill_server({"t": "game", "on": True})  # the bridge's game mode first (M6)
        await bridge.to_skill_server({"t": "skill_start", "id": 4, "skill": "face", "thing": "baby"})
        await until(lambda: len(msgs(bridge, "skill_done")) == 1)
        assert [c.skill for c in maker.calls] == ["face"]
    finally:
        await s.close()


# ---- the person questions
async def test_howfar_baby_answers_only_in_a_game(bridge: FakeBridge) -> None:
    w = world()
    seen_person(w, 0.9, BRIDGE_NOW_S - 0.1)
    s = await serving(bridge, w)
    try:
        ask = {"t": "skill_ask", "q": "howfar", "thing": "baby"}
        await bridge.to_skill_server({**ask, "id": 1, "game": True})
        await bridge.to_skill_server({**ask, "id": 2})
        await bridge.to_skill_server({"t": "skill_ask", "id": 3, "q": "see?", "thing": "baby", "game": True})
        await until(lambda: len(msgs(bridge, "skill_answer")) == 3)
        got = {m["id"]: m["value"] for m in msgs(bridge, "skill_answer")}
        e = w.person.latest(round(BRIDGE_NOW_S * S))
        assert e is not None and e.d_m is not None
        assert got == {1: round(e.d_m * 100), 2: -1, 3: True}
    finally:
        await s.close()


# ---- caught
async def test_skill_end_caught_ends_a_follow_quietly(bridge: FakeBridge) -> None:
    maker = Maker(lambda: Scripted(None))
    s = await serving(bridge, maker=maker)
    try:
        await bridge.to_skill_server({"t": "game", "on": True})
        await bridge.to_skill_server(
            {"t": "skill_start", "id": 5, "skill": "follow", "thing": "baby", "s": 10}
        )
        await until(lambda: len(msgs(bridge, "skill_drive")) >= 3)
        await bridge.to_skill_server({"t": "skill_end", "id": 5, "why": "caught"})
        await until(lambda: s.app.state == "idle")
        await asyncio.sleep(0.2)
        assert msgs(bridge, "skill_failed") == [] and msgs(bridge, "skill_done") == []
        assert s.app.last_result == Result.CAUGHT
    finally:
        await s.close()


async def test_back_ok_for_an_arc_or_without_cm_is_no(
    bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch
) -> None:
    w = world()  # final review I3, M5: an arc is not checked yet; a malformed ask still gets its no at once
    monkeypatch.setattr(w.map_run.p, "pose", lambda: FACING_X)
    trusted_room(w, BRIDGE_NOW_S - 1.0)
    s = await serving(bridge, w)
    try:
        arc = {"t": "skill_ask", "id": 8, "q": "back_ok", "cm": 30, "arc": {"r_cm": 40, "deg": 45}}
        await bridge.to_skill_server(arc)
        await bridge.to_skill_server({"t": "skill_ask", "id": 9, "q": "back_ok"})
        await until(lambda: len(msgs(bridge, "skill_answer")) == 2)
        assert {m["id"]: m["value"] for m in msgs(bridge, "skill_answer")} == {8: False, 9: False}
    finally:
        await s.close()


async def test_a_stream_that_breaks_says_so_and_goes_on(
    bridge: FakeBridge, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:  # final review M3
    from conqueror_ai.app import game_serve

    calls = {"n": 0}
    real = game_serve.person_message

    def flaky(watch: object, now_us: int) -> dict[str, Any]:
        calls["n"] += 1
        if calls["n"] == 2:
            raise ValueError("boom")
        return real(watch, now_us)  # type: ignore[arg-type]

    monkeypatch.setattr(game_serve, "person_message", flaky)
    s = await serving(bridge)
    try:
        await bridge.to_skill_server({"t": "game", "on": True})
        await until(lambda: len(msgs(bridge, "person")) >= 3)
        assert "person stream" in capsys.readouterr().out
    finally:
        await s.close()


# ---- Plan G3: the GAMES card's numbers on the AI feed
async def test_the_feed_gets_people_and_the_person_in_a_game(bridge: FakeBridge, tmp_path: Path) -> None:
    from conqueror_ai.app.feed import FeedPublisher
    from tests.test_feed_publisher import FakeServer

    write_pass(tmp_path / "p.yaml", DET, CAL, "2026-11-10", {})
    w = world()
    seen_person(w, 0.9, BRIDGE_NOW_S - 0.1)
    p = FakePerception()
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", NAMES.wire())
    app = ServeApp(
        link, p, w, NAMES, None, None, skills=Maker(), seeing=SeeingPass(tmp_path / "p.yaml", DET, CAL)
    )
    pub = FeedPublisher(FakeServer(), lambda: [])  # pyright: ignore[reportArgumentType]
    app.publisher = pub
    stop = asyncio.Event()
    task = asyncio.create_task(app.run(stop))
    s = Serving(app, task, stop, p)
    try:
        await until(lambda: app.state == "idle")
        st = pub.greeting()[0]
        assert st["people"] is True and "person" not in st
        await bridge.to_skill_server({"t": "game", "on": True})
        await until(lambda: "person" in pub.greeting()[0])
        await until(lambda: len(msgs(bridge, "person")) >= 1)
        shown = pub.greeting()[0]["person"]
        assert isinstance(shown, dict) and shown["d_cm"] == msgs(bridge, "person")[-1]["d_cm"]
    finally:
        await s.close()


def test_the_person_words_are_tank_logos() -> None:  # G3 final review M4: one list, two places
    import yaml

    from conqueror_ai.app.game_serve import PERSON_WORDS

    spec = Path(__file__).resolve().parents[2] / "logo" / "spec" / "words.yaml"
    assert set(yaml.safe_load(spec.read_text())["person_words"]) == set(PERSON_WORDS)


async def test_a_game_word_without_game_mode_is_refused(bridge: FakeBridge) -> None:  # G3 final review M6
    maker = Maker()
    s = await serving(bridge, maker=maker)
    try:
        await bridge.to_skill_server({"t": "skill_start", "id": 9, "skill": "face", "thing": "baby"})
        await until(lambda: len(msgs(bridge, "skill_failed")) == 1)
        assert msgs(bridge, "skill_failed")[0]["reason"] == "stale" and maker.calls == []
    finally:
        await s.close()
