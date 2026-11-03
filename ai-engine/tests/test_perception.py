"""Perception: one camera and telemetry loop for every tool (AI engine LLD §19.5, review F2)."""

from __future__ import annotations

import asyncio
import time

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.app.perception import Perception
from conqueror_ai.app.world import World
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.types import Detection
from tests.fake_bridge import FakeBridge
from tests.test_map_app import CFG
from tests.test_objects import CAL
from tests.test_objects_app import GREY_JPEG


class SlowDetector:
    def detect(self, rgb: NDArray[np.uint8], cam: CameraModel | None) -> list[Detection]:
        time.sleep(0.5)  # a slow model, in its worker thread
        return []


def world() -> World:
    return World(CAL, CFG, None, frozenset(), {}, Limits(), None)


async def test_telemetry_flows_while_the_detector_runs() -> None:
    bridge = FakeBridge(jpeg=GREY_JPEG, pan_tilt=(90.0, 90.0))
    await bridge.start()
    stop = asyncio.Event()
    p = Perception(
        BridgeLink("127.0.0.1", bridge.ws_port),
        MjpegSource("127.0.0.1", bridge.stream_port),
        world(),
        SlowDetector(),
        None,
        None,
    )
    try:
        p.start(stop)
        for _ in range(50):
            if p.last_telem_s > 0:
                break
            await asyncio.sleep(0.1)
        before = p.last_telem_s
        await p.next_picture(2.0)  # 0.5 s in the detector
        assert p.last_telem_s - before >= 0.3  # telemetry kept arriving meanwhile
    finally:
        stop.set()
        await p.close()
        await bridge.stop()


async def test_a_crashed_link_is_reported() -> None:
    cam = MjpegSource("127.0.0.1", 1)

    async def broken(stop: asyncio.Event) -> None:
        raise OSError("stream closed")

    cam.run = broken  # type: ignore[method-assign]
    stop = asyncio.Event()
    p = Perception(BridgeLink("127.0.0.1", 1), cam, world(), None, None, None)
    p.start(stop)
    await asyncio.sleep(0.2)
    failed = p.failure()
    stop.set()
    await p.close()
    assert failed is not None and "stream closed" in failed


async def test_one_picture_goes_through_once() -> None:
    bridge = FakeBridge(
        jpeg=GREY_JPEG, pan_tilt=(90.0, 90.0), frames_for_s=0.15
    )  # a picture or so, then none
    await bridge.start()
    stop = asyncio.Event()
    p = Perception(
        BridgeLink("127.0.0.1", bridge.ws_port),
        MjpegSource("127.0.0.1", bridge.stream_port),
        world(),
        None,
        None,
        None,
    )
    try:
        p.start(stop)
        got = [await p.next_picture(1.0) for _ in range(6)]
        assert any(g is not None for g in got) and got[-1] is None  # the stream stopped: a timeout, no repeat
    finally:
        stop.set()
        await p.close()
        await bridge.stop()


async def test_an_undecodable_picture_is_no_picture() -> None:
    """Plan Z7 final review minor 4: a picture that doesn't decode returned "undecodable" and the tools
    redrew the previous frame as a new one; now it is no picture (None), as a timeout."""
    from tests.fake_bridge import JPEG

    bridge = FakeBridge(jpeg=JPEG, pan_tilt=(90.0, 90.0))  # not a real JPEG
    await bridge.start()
    stop = asyncio.Event()
    p = Perception(
        BridgeLink("127.0.0.1", bridge.ws_port),
        MjpegSource("127.0.0.1", bridge.stream_port),
        world(),
        None,
        None,
        None,
    )
    try:
        p.start(stop)
        got = [await p.next_picture(0.5) for _ in range(3)]
        assert got == [None, None, None] and p.world.last_frame is None
    finally:
        stop.set()
        await p.close()
        await bridge.stop()
