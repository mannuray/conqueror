"""LeaseDriver: the Driver port under the bridge's lease (AI engine LLD §21.5, bridge LLD R26)."""

from __future__ import annotations

import asyncio
import itertools
import time
from collections.abc import AsyncIterator
from typing import Any

import pytest

from conqueror_ai.adapters.drive_link import drive_msg
from conqueror_ai.adapters.lease_driver import (
    FRESH_FOR_S,
    KEEP_S,
    MIN_GAP_S,
    STREAM_PER_S,
    LeaseDriver,
    wire_reason,
)
from conqueror_ai.adapters.skill_link import End, SkillLink
from conqueror_ai.domain.drive.curves import STOP, Command
from conqueror_ai.domain.drive.skill import Result
from conqueror_ai.domain.ports import Driver
from tests.fake_bridge import FakeBridge
from tests.test_skill_link import kill_the_socket


@pytest.fixture
async def bridge() -> AsyncIterator[FakeBridge]:
    b = FakeBridge()
    await b.start()
    yield b
    await b.stop()


async def connected(b: FakeBridge) -> SkillLink:
    link = SkillLink(f"ws://127.0.0.1:{b.ws_port}/ws", [])
    await link.connect()
    return link


def drives(b: FakeBridge) -> list[dict[str, Any]]:
    return [m for m in b.received if m.get("t") == "skill_drive"]


async def until(cond: Any, timeout_s: float = 2.0) -> None:
    async with asyncio.timeout(timeout_s):
        while not cond():
            await asyncio.sleep(0.01)


async def test_frames_carry_id(bridge: FakeBridge) -> None:
    link = await connected(bridge)
    try:
        d = LeaseDriver(link, 42)
        assert isinstance(d, Driver)
        cmd = Command(25.0, -12.34)
        await d.drive(cmd)
        await until(lambda: len(drives(bridge)) == 1)
        m = drives(bridge)[0]
        assert m["id"] == 42
        assert (m["v"], m["yaw_rate"]) == (
            drive_msg(cmd)["v"],
            drive_msg(cmd)["yaw_rate"],
        )  # DriveLink's table
        assert d.refused is None and d.connected
    finally:
        await link.close()


async def test_run_sends_the_first_frame_at_once_and_keeps_the_lease_alive(bridge: FakeBridge) -> None:
    link = await connected(bridge)
    stop = asyncio.Event()
    d = LeaseDriver(link, 3)
    task = asyncio.create_task(d.run(stop))
    try:
        await until(lambda: len(drives(bridge)) >= 1)
        assert drives(bridge)[0]["v"] == 0  # the first frame, within the bridge's 300 ms
        await d.drive(Command(20.0, 0.0))
        n = len(drives(bridge))
        await asyncio.sleep(KEEP_S * 4)
        assert len(drives(bridge)) >= n + 2  # repeated while the command is fresh
    finally:
        stop.set()
        await task
        await link.close()


async def test_end_stops_frames(bridge: FakeBridge) -> None:
    """Review Focus 3: once the bridge ended the lease, nothing more goes out - not a drive, not finish."""
    link = await connected(bridge)
    try:
        d = LeaseDriver(link, 9)
        await d.drive(Command(20.0, 0.0))
        await until(lambda: len(drives(bridge)) == 1)
        d.end("deadman")
        assert d.refused == "deadman"
        await d.drive(Command(20.0, 5.0))
        await d.cam(0.0, 10.0)
        await d.finish()
        d.end("stop")  # the first reason stays
        assert d.refused == "deadman"
        await asyncio.sleep(0.15)
        assert len(drives(bridge)) == 1
        assert not [m for m in bridge.received if m.get("t") == "cam_set"]
    finally:
        await link.close()


async def test_finish_once(bridge: FakeBridge) -> None:
    link = await connected(bridge)
    try:
        d = LeaseDriver(link, 5)
        await d.drive(Command(20.0, 0.0))
        await d.finish()
        await d.finish()
        await d.drive(Command(20.0, 0.0))  # after finish: nothing
        await asyncio.sleep(0.1)
        fs = drives(bridge)
        assert [m["v"] for m in fs] == [drive_msg(Command(20.0, 0.0))["v"], 0]
        assert d.cmd == STOP
    finally:
        await link.close()


async def test_a_lost_link_refuses_instead_of_raising(bridge: FakeBridge) -> None:
    link = await connected(bridge)
    d = LeaseDriver(link, 5)
    await link.close()
    await d.drive(Command(20.0, 0.0))
    assert d.refused == "lost"


async def test_cam_goes_through_the_link(bridge: FakeBridge) -> None:
    link = await connected(bridge)
    try:
        d = LeaseDriver(link, 5)
        await d.cam(5.0, 15.0)
        await until(lambda: any(m.get("t") == "cam_set" for m in bridge.received))
    finally:
        await link.close()


def test_wire_reason_per_result() -> None:
    assert wire_reason(Result.DONE) is None
    assert wire_reason(Result.NOT_FOUND) == "not_found"
    assert wire_reason(Result.LOST) == "lost"
    assert wire_reason(Result.BLOCKED) == "blocked"
    assert wire_reason(Result.OUTSIDE_AREA) == "outside_area"
    assert wire_reason(Result.NO_WAY) == "no_way"
    assert wire_reason(Result.UNKNOWN_THING) == "unknown_thing"
    assert wire_reason(Result.STALE) == "stale"
    assert wire_reason(Result.REFUSED) == "stale"
    assert wire_reason(Result.FAILED) == "stale"
    assert wire_reason(Result.CANCELLED) == "stale"  # cancelled by the engine (Ctrl-C...)
    assert wire_reason(Result.CANCELLED, ended=True) is None  # the bridge ended it: it knows
    assert wire_reason(Result.CAUGHT) is None  # the bridge ended the lease caught (LLD §28.3)
    assert {wire_reason(r, ended=True) for r in Result} <= {
        None,
        "not_found",
        "lost",
        "blocked",
        "outside_area",
        "no_way",
        "unknown_thing",
        "stale",
    }


async def test_a_held_stop_is_resent_while_fresh(bridge: FakeBridge) -> None:
    """Review I2: holding still is not silence - the bridge ends a word whose frames stop for 300 ms."""
    link = await connected(bridge)
    stop = asyncio.Event()
    d = LeaseDriver(link, 3)
    task = asyncio.create_task(d.run(stop))
    try:
        await d.drive(Command(20.0, 0.0))
        await d.drive(STOP)
        await asyncio.sleep(0.4)  # fresh for 0.5 s after the last command
        vs = [m["v"] for m in drives(bridge)]
        moving = next(i for i, v in enumerate(vs) if v != 0)
        assert vs[moving + 1 :].count(0) >= 3  # a stop frame about every KEEP_S, never silence
    finally:
        stop.set()
        await task
        await link.close()


async def test_frames_keep_a_minimum_gap(bridge: FakeBridge) -> None:
    """Review minor d: the bridge refuses over 25 frames a second; DriveLink's gap is the rule here too."""
    link = await connected(bridge)
    stop = asyncio.Event()
    d = LeaseDriver(link, 3)
    task = asyncio.create_task(d.run(stop))
    try:
        for i in range(30):
            await d.drive(Command(20.0, float(i)))
            await asyncio.sleep(0.005)
        await asyncio.sleep(0.15)
        n = len(drives(bridge))
        assert n <= 0.35 / MIN_GAP_S + 2, n  # ~0.3 s of calls
        assert drives(bridge)[-1]["yaw_rate"] == 29.0  # the newest command still gets through
    finally:
        stop.set()
        await task
        await link.close()


async def test_end_while_run_keeps_a_moving_command_alive(bridge: FakeBridge) -> None:
    link = await connected(bridge)
    stop = asyncio.Event()
    d = LeaseDriver(link, 3)
    task = asyncio.create_task(d.run(stop))
    try:
        await d.drive(Command(20.0, 0.0))
        await until(lambda: len(drives(bridge)) >= 2)
        d.end("stop")
        await asyncio.sleep(0.05)
        n = len(drives(bridge))
        await asyncio.sleep(0.4)  # the command is still fresh: the keep-alive must still not send
        assert len(drives(bridge)) == n
        await asyncio.wait_for(task, 1)  # and run() ends by itself
    finally:
        stop.set()
        await link.close()


async def test_a_bridge_skill_end_reaches_the_driver(bridge: FakeBridge) -> None:
    """ServeApp maps the link's `End` event to `LeaseDriver.end(why)` (for the skill's own id)."""
    link = await connected(bridge)
    try:
        d = LeaseDriver(link, 7)
        await bridge.to_skill_server({"t": "skill_end", "id": 7, "why": "stop"})
        async with asyncio.timeout(2):
            ev = await anext(link.events())
        assert isinstance(ev, End) and ev == End(7, "stop")
        d.end(ev.why)
        assert d.refused == "stop"
        await d.drive(Command(20.0, 0.0))
        await asyncio.sleep(0.05)
        assert drives(bridge) == []
    finally:
        await link.close()


async def test_a_socket_closed_under_the_keep_alive_refuses(bridge: FakeBridge) -> None:
    """Review I1: the keep-alive task must not die with an exception; the driver refuses ("lost")."""
    link = await connected(bridge)
    stop = asyncio.Event()
    d = LeaseDriver(link, 3)
    task = asyncio.create_task(d.run(stop))
    try:
        await d.drive(Command(20.0, 0.0))
        kill_the_socket(link)
        await until(lambda: d.refused is not None)
        assert d.refused == "lost"
        await asyncio.wait_for(task, 1)  # ended cleanly, no exception
        await d.drive(Command(20.0, 0.0))  # nothing raised, nothing sent
        await d.finish()
        await d.cam(0.0, 0.0)
    finally:
        stop.set()
        await link.close()


async def test_a_stale_moving_command_is_followed_by_stop_frames(bridge: FakeBridge) -> None:
    """A picture gap (the runner's "waiting") holds the tank still under the lease: a moving command is
    repeated only while fresh, then STOP frames at least every 0.3 s for as long as the lease lives."""
    link = await connected(bridge)
    stop = asyncio.Event()
    d = LeaseDriver(link, 3)
    task = asyncio.create_task(d.run(stop))
    try:
        await d.drive(Command(20.0, 0.0))
        await asyncio.sleep(FRESH_FOR_S + 3.0)
        vs = [m["v"] for m in drives(bridge)]
        last_moving = max(i for i, v in enumerate(vs) if v != 0)
        after = vs[last_moving + 1 :]
        assert len(after) >= 3.0 / 0.3 and set(after) == {0}  # stop frames, never silence
        assert len(vs) <= (FRESH_FOR_S + 3.0) / KEEP_S + 5  # and not a flood
        d.end("stop")
        await asyncio.sleep(0.05)
        n = len(drives(bridge))
        await asyncio.sleep(0.4)
        assert len(drives(bridge)) == n  # end() still stops everything
    finally:
        stop.set()
        await task
        await link.close()


async def test_a_lost_link_on_cam_refuses_instead_of_raising(bridge: FakeBridge) -> None:
    """Task 9 minor: cam() closes the same gap as drive() - a ConnectionError is a refusal ("lost")."""
    link = await connected(bridge)
    try:
        d = LeaseDriver(link, 5)
        kill_the_socket(link)
        await d.cam(0.0, 10.0)
        assert d.refused == "lost"
    finally:
        await link.close()


async def test_stops_after_motion_keep_the_bridges_stream_limit(bridge: FakeBridge) -> None:
    """Task 9 minor: the bridge admits at most 25 skill_drive frames a second (ws_clients.cpp admit; a v 0
    frame counts like any other). A stop after motion goes at once only when the gap allows it."""
    link = await connected(bridge)
    stop = asyncio.Event()
    d = LeaseDriver(link, 3)
    sent: list[float] = []
    real = link.drive

    async def timed(id: int, cmd: Command) -> None:
        sent.append(time.monotonic())
        await real(id, cmd)

    link.drive = timed  # type: ignore[method-assign]
    task = asyncio.create_task(d.run(stop))
    try:
        t_end = time.monotonic() + 1.2
        while time.monotonic() < t_end:
            await d.drive(Command(20.0, 0.0))
            await asyncio.sleep(0.005)
            await d.drive(STOP)
            await asyncio.sleep(0.005)
        await asyncio.sleep(0.1)
        busiest = max(sum(1 for t in sent if s <= t < s + 1.0) for s in sent)
        assert busiest <= STREAM_PER_S, busiest
        assert drives(bridge)[-1]["v"] == 0  # the last stop still went out
    finally:
        stop.set()
        await task
        await link.close()


async def test_only_a_stop_after_motion_skips_the_minimum_gap(bridge: FakeBridge) -> None:
    """Task 9 minor: a stop goes sooner only after a moving frame; a stop after a stop keeps MIN_GAP_S."""
    link = await connected(bridge)
    stop = asyncio.Event()
    d = LeaseDriver(link, 3)
    sent: list[float] = []
    real = link.drive

    async def timed(id: int, cmd: Command) -> None:
        sent.append(time.monotonic())
        await real(id, cmd)

    link.drive = timed  # type: ignore[method-assign]
    task = asyncio.create_task(d.run(stop))
    try:
        t_end = time.monotonic() + 0.6
        while time.monotonic() < t_end:
            await d.drive(STOP)
            await asyncio.sleep(0.005)
        gaps = [b - a for a, b in itertools.pairwise(sent)]
        assert min(gaps) >= MIN_GAP_S - 0.005, min(gaps)
    finally:
        stop.set()
        await task
        await link.close()


async def test_a_send_gap_over_a_quarter_second_is_noted(bridge: FakeBridge) -> None:
    """LLD §25.6 (the room4 7052): a gap between drive frames over 250 ms is printed, to find where it was."""
    link = await connected(bridge)
    try:
        notes: list[str] = []
        d = LeaseDriver(link, 42, note=notes.append)
        await d.drive(Command(20.0, 0.0))
        await asyncio.sleep(0.12)
        await d.drive(Command(21.0, 0.0))
        assert notes == []
        await asyncio.sleep(0.32)
        await d.drive(Command(22.0, 0.0))
        assert len(notes) == 1 and "ms between drive frames" in notes[0]
    finally:
        await link.close()
