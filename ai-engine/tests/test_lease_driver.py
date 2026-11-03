"""LeaseDriver: the Driver port under the bridge's lease (AI engine LLD §21.5, bridge LLD R26)."""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator
from typing import Any

import pytest

from conqueror_ai.adapters.drive_link import drive_msg
from conqueror_ai.adapters.lease_driver import KEEP_S, LeaseDriver, wire_reason
from conqueror_ai.adapters.skill_link import SkillLink
from conqueror_ai.domain.drive.curves import STOP, Command
from conqueror_ai.domain.drive.skill import Result
from conqueror_ai.domain.ports import Driver
from tests.fake_bridge import FakeBridge


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
