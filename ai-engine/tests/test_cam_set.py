"""BridgeLink.cam_set (bridge LLD R22): the camera to exact angles - the engine's only command."""

from __future__ import annotations

import asyncio

import pytest

from conqueror_ai.adapters.bridge_ws import BridgeLink, CommandRefused
from tests.fake_bridge import FakeBridge


async def _connected(link: BridgeLink) -> None:
    for _ in range(200):
        if link.connected:
            return
        await asyncio.sleep(0.01)
    raise AssertionError("never connected")


async def test_cam_set_sends_degrees_and_returns_on_the_ack() -> None:
    bridge = FakeBridge()
    await bridge.start()
    link = BridgeLink("127.0.0.1", bridge.ws_port)
    stop = asyncio.Event()
    task = asyncio.create_task(link.run(stop))
    try:
        await _connected(link)
        await link.cam_set(10.0, -15.0)
        sent = [m for m in bridge.received if m.get("t") == "cam_set"]
        assert len(sent) == 1
        assert sent[0]["pan"] == 10.0 and sent[0]["tilt"] == -15.0 and isinstance(sent[0]["id"], int)
    finally:
        stop.set()
        await link.close()
        await asyncio.wait_for(task, 3)
        await bridge.stop()


async def test_cam_set_out_of_range_raises_with_the_reason() -> None:
    bridge = FakeBridge()
    await bridge.start()
    link = BridgeLink("127.0.0.1", bridge.ws_port)
    stop = asyncio.Event()
    task = asyncio.create_task(link.run(stop))
    try:
        await _connected(link)
        with pytest.raises(CommandRefused) as e:
            await link.cam_set(0.0, -30.0)
        assert e.value.reason == "OUT_OF_RANGE"
        assert "-20..60" in str(e.value)
    finally:
        stop.set()
        await link.close()
        await asyncio.wait_for(task, 3)
        await bridge.stop()


async def test_cam_set_without_a_connection_raises() -> None:
    link = BridgeLink("127.0.0.1", 9)
    with pytest.raises(ConnectionError):
        await link.cam_set(0.0, 0.0)
