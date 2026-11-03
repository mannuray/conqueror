"""SkillLink: the engine as the bridge's skill server (AI engine LLD §21.5, bridge LLD R26.2). The fixtures
are the bridge's own test messages (test_ws_codec, test_ws_json)."""

from __future__ import annotations

import asyncio
import json
from collections.abc import AsyncIterator
from typing import Any

import pytest
from websockets.exceptions import ConnectionClosedOK

from conqueror_ai.adapters.skill_link import (
    FRAME_MAX,
    Ask,
    End,
    Lost,
    SkillBusy,
    SkillLink,
    Start,
    ThingsRefused,
    things_parts,
)
from conqueror_ai.domain.drive.curves import Command
from tests.fake_bridge import FakeBridge

THINGS: list[dict[str, object]] = [
    {"word": "toy", "names": ["car", "racing car"]},
    {"word": "ball", "names": ["football"]},
]


@pytest.fixture
async def bridge() -> AsyncIterator[FakeBridge]:
    b = FakeBridge()
    await b.start()
    yield b
    await b.stop()


async def until(cond: Any, timeout_s: float = 2.0) -> None:
    async with asyncio.timeout(timeout_s):
        while not cond():
            await asyncio.sleep(0.01)


def skill_msgs(b: FakeBridge, kind: str) -> list[dict[str, Any]]:
    return [m for m in b.received if m.get("t") == kind]


async def test_hello_carries_things(bridge: FakeBridge) -> None:
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    await link.connect()
    try:
        await until(lambda: len(skill_msgs(bridge, "skill_things")) == 1)
        hello = skill_msgs(bridge, "hello")[0]
        assert hello == {"t": "hello", "client": "ai-skills", "proto": 1}  # no things in the hello
        assert skill_msgs(bridge, "skill_things")[0] == {
            "t": "skill_things",
            "part": 1,
            "of": 1,
            "id": 1,  # every part carries an id: the bridge acks (or nacks) each
            "things": THINGS,
        }
    finally:
        await link.close()


def test_a_long_list_goes_in_parts_each_within_the_frame() -> None:
    many: list[dict[str, object]] = [
        {"word": f"word{i:02d}", "names": [f"name-{i:02d}-{j}" for j in range(8)]} for i in range(48)
    ]
    frames = things_parts(many)
    assert 1 < len(frames) <= 48
    got: dict[str, list[str]] = {}
    for i, f in enumerate(frames, 1):
        assert len(f.encode()) <= FRAME_MAX
        m = json.loads(f)
        assert (m["part"], m["of"]) == (i, len(frames))
        for t in m["things"]:
            got.setdefault(t["word"], []).extend(t["names"])
    assert got == {str(t["word"]): t["names"] for t in many}


def test_a_word_too_long_for_one_frame_is_split_by_its_names() -> None:
    big: list[dict[str, object]] = [{"word": "w" * 24, "names": [f"{c}" * 24 for c in "abcdefgh"]}]
    frames = things_parts(big)
    assert len(frames) > 1
    names = [n for f in frames for t in json.loads(f)["things"] for n in t["names"]]
    assert names == big[0]["names"]
    assert all(len(f.encode()) <= FRAME_MAX for f in frames)


def test_an_empty_list_is_one_empty_part() -> None:
    assert json.loads(things_parts([])[0]) == {"t": "skill_things", "part": 1, "of": 1, "things": []}


async def test_busy_raises(bridge: FakeBridge) -> None:
    first = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    await first.connect()
    second = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    try:
        with pytest.raises(SkillBusy):
            await second.connect()
        assert not second.connected
    finally:
        await first.close()


async def test_events_parsed(bridge: FakeBridge) -> None:
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    await link.connect()
    try:
        # the bridge's own JSON (test_ws_json): ask, start of each skill, end
        await bridge.to_skill_server({"t": "skill_ask", "id": 5, "q": "see?", "thing": "toy"})
        await bridge.to_skill_server({"t": "skill_ask", "id": 6, "q": "howfar", "thing": 'red "ball"'})
        await bridge.to_skill_server({"t": "skill_start", "id": 7, "skill": "find", "thing": "toy"})
        await bridge.to_skill_server({"t": "skill_start", "id": 8, "skill": "wander", "s": 30})
        await bridge.to_skill_server({"t": "skill_start", "id": 9, "skill": "explore"})
        await bridge.to_skill_server(
            {
                "t": "skill_start",
                "id": 11,
                "skill": "setxy",
                "to": {"fwd_cm": -50, "left_cm": 20},
                "area": {"r_m": 2, "fwd_cm": 0, "left_cm": 5},
            }
        )
        await bridge.to_skill_server({"t": "skill_end", "id": 7, "why": "deadman"})
        await bridge.to_skill_server({"t": "skill_start", "id": 12, "skill": "bad", "to": {"fwd_cm": 1}})
        it = link.events()
        async with asyncio.timeout(2):
            got = [await anext(it) for _ in range(8)]
        assert got == [
            Ask(5, "see?", "toy"),
            Ask(6, "howfar", 'red "ball"'),
            Start(7, "find", "toy", None, None, None),
            Start(8, "wander", None, 30.0, None, None),
            Start(9, "explore", None, None, None, None),
            Start(11, "setxy", None, None, (-50.0, 20.0), (2.0, 0.0, 5.0)),
            End(7, "deadman"),
            Start(12, "bad", None, None, None, None),  # the bridge checks the skills: not our business
        ]
    finally:
        await link.close()


async def test_answers_and_drive_frames_match_the_bridge_parser(bridge: FakeBridge) -> None:
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    await link.connect()
    try:
        await link.answer(7, True)
        await link.answer(8, -1)
        await link.answer(9, 120)
        await link.drive(9, Command(25.0, -12.34))
        await link.done(4)
        await link.failed(4, "lost", "toy")
        await until(lambda: len(skill_msgs(bridge, "skill_failed")) == 1)
        sent = [
            m
            for m in bridge.received
            if str(m.get("t", "")).startswith("skill_") and m["t"] != "skill_things"
        ]
        assert sent == [
            {"t": "skill_answer", "id": 7, "value": True},
            {"t": "skill_answer", "id": 8, "value": -1},
            {"t": "skill_answer", "id": 9, "value": 120},
            {"t": "skill_drive", "id": 9, "v": 56, "yaw_rate": -12.3},
            {"t": "skill_done", "id": 4},
            {"t": "skill_failed", "id": 4, "reason": "lost", "thing": "toy"},
        ]
    finally:
        await link.close()


async def test_pings_answered(bridge: FakeBridge) -> None:
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    await link.connect()
    try:
        await bridge.to_skill_server({"t": "ping", "t0": 123.5})
        await until(lambda: any(m.get("t") == "pong" and m.get("t0") == 123.5 for m in bridge.received))
        # and it pings the bridge itself, so the bridge's 3 s silent-client rule never fires
        await until(lambda: len(skill_msgs(bridge, "ping")) >= 1)
    finally:
        await link.close()


async def test_a_closed_link_ends_the_events_with_lost(bridge: FakeBridge) -> None:
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    await link.connect()
    assert bridge.skill_ws is not None
    await bridge.skill_ws.close()
    it = link.events()
    async with asyncio.timeout(2):
        assert await anext(it) == Lost()
        with pytest.raises(StopAsyncIteration):
            await anext(it)
    assert not link.connected
    with pytest.raises(ConnectionError):
        await link.drive(1, Command(0.0, 0.0))
    await link.close()


def kill_the_socket(link: SkillLink) -> None:
    """The window between the socket closing and the reader's clean-up: _ws is still set, send raises."""

    async def closed(_msg: object) -> None:
        raise ConnectionClosedOK(None, None)

    assert link._ws is not None  # pyright: ignore[reportPrivateUsage]
    link._ws.send = closed  # type: ignore[method-assign]  # pyright: ignore[reportPrivateUsage, reportAttributeAccessIssue]


async def test_a_closed_socket_refuses_instead_of_raising(bridge: FakeBridge) -> None:
    """Review I1: websockets' ConnectionClosed is not a ConnectionError; between the close and the reader's
    clean-up every send must still end as a ConnectionError, which callers turn into a refusal."""
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    await link.connect()
    try:
        kill_the_socket(link)
        sends = (
            lambda: link.answer(1, True),
            lambda: link.done(1),
            lambda: link.failed(1, "lost", "toy"),
            lambda: link.drive(1, Command(10.0, 0.0)),
            lambda: link.cam(0.0, 0.0),
        )
        for send in sends:
            with pytest.raises(ConnectionError):
                await send()
    finally:
        await link.close()


async def test_a_refused_things_list_raises_a_plain_error(bridge: FakeBridge) -> None:
    for reason in ("TOO_BIG", "BAD_FIELD"):
        bridge.things_nack = reason
        link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
        with pytest.raises(ThingsRefused, match=reason):  # the bridge's nack carries the part's id
            await link.connect()
        assert not link.connected
        await until(lambda: bridge.skill_ws is None)  # the failed connect let go of the server's place


async def test_every_part_is_waited_for(bridge: FakeBridge) -> None:
    many: list[dict[str, object]] = [
        {"word": f"word{i:02d}", "names": [f"name-{i:02d}-{j}" for j in range(8)]} for i in range(20)
    ]
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", many)
    await link.connect()
    try:
        parts = skill_msgs(bridge, "skill_things")
        assert len(parts) > 1 and [p["part"] for p in parts] == list(range(1, len(parts) + 1))
        assert len({p["id"] for p in parts}) == len(parts)
    finally:
        await link.close()


async def test_the_things_can_be_sent_again(bridge: FakeBridge) -> None:
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    await link.connect()
    try:
        await link.set_things([{"word": "box", "names": ["carton"]}])
        last = skill_msgs(bridge, "skill_things")[-1]
        assert last["things"] == [{"word": "box", "names": ["carton"]}] and last["part"] == 1
        bridge.things_nack = "TOO_BIG"
        with pytest.raises(ThingsRefused, match="TOO_BIG"):
            await link.set_things(THINGS)
        assert link.connected  # the bridge keeps the old list; the link stays
    finally:
        await link.close()


async def test_a_silent_bridge_is_lost(bridge: FakeBridge) -> None:
    """WiFi lost with the socket still open: nothing heard for silent_s puts Lost on the queue."""
    bridge.answer_pings = False
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS, silent_s=0.4)
    await link.connect()
    try:
        it = link.events()
        async with asyncio.timeout(3):
            assert await anext(it) == Lost()
        assert not link.connected
    finally:
        await link.close()


async def test_the_bridges_exact_things_replies(bridge: FakeBridge) -> None:
    """Bridge as built (command_router.cpp onThings): every part is answered at once with
    {"t":"ack","id":N} or {"t":"nack","id":N,"code":"COMMAND_3020","reason":"TOO_BIG",...} - never silence.
    connect() / set_things() take those exact texts; a nack's message is in the error."""
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    async with asyncio.timeout(2):
        await link.connect()  # an ack for the one part
    try:
        bridge.things_nack = "TOO_BIG"
        async with asyncio.timeout(2):
            with pytest.raises(ThingsRefused, match="TOO_BIG: Too many things or names"):
                await link.set_things(THINGS)
    finally:
        await link.close()


async def test_part_ids_never_collide_with_other_messages(bridge: FakeBridge) -> None:
    """Task 9 minor: a cam_set sent while the things list goes out in parts must not reuse a part's id (one
    counter that never goes backwards)."""
    many: list[dict[str, object]] = [
        {"word": f"word{i:02d}", "names": [f"name-{i:02d}-{j}" for j in range(8)]} for i in range(6)
    ]
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    await link.connect()
    try:
        bridge.things_delay_s = 0.05
        sending = asyncio.create_task(link.set_things(many))
        for _ in range(4):
            await asyncio.sleep(0.03)
            await link.cam(0.0, 0.0)
        await asyncio.wait_for(sending, 3)
        await link.cam(0.0, 0.0)
        await until(lambda: len(skill_msgs(bridge, "cam_set")) == 5)
        ids = [m["id"] for m in bridge.received if m.get("t") in ("skill_things", "cam_set")]
        assert len(ids) == len(set(ids)), ids
        cams = [m["id"] for m in skill_msgs(bridge, "cam_set")]
        assert cams == sorted(cams) and cams[-1] > max(p["id"] for p in skill_msgs(bridge, "skill_things"))
    finally:
        await link.close()


async def test_websockets_own_errors_are_connection_errors(bridge: FakeBridge) -> None:
    """Review I1: a refused handshake (InvalidStatus) and a socket closed during the hello
    (ConnectionClosedError) reach callers as ConnectionError - one error type to retry on."""
    bridge.reject_handshakes = 1
    link = SkillLink(f"ws://127.0.0.1:{bridge.ws_port}/ws", THINGS)
    with pytest.raises(ConnectionError):
        await link.connect()
    bridge.drop_hellos = 1
    with pytest.raises(ConnectionError):
        await link.connect()
    assert not link.connected
    await link.connect()  # and the next try connects
    try:
        assert link.connected
    finally:
        await link.close()
