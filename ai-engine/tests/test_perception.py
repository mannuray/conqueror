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
from conqueror_ai.domain.types import Detection, Frame
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


class PersonAndToyDetector:
    def detect(self, rgb: NDArray[np.uint8], cam: CameraModel | None) -> list[Detection]:
        return [
            Detection("person", 0.9, (100.0, 0.0, 300.0, 400.0)),
            Detection("toy", 0.9, (150.0, 200.0, 200.0, 260.0)),  # inside the person
            Detection("box", 0.9, (400.0, 200.0, 450.0, 260.0)),
        ]


async def test_guard_runs_before_tracker() -> None:
    """A toy box over a person never reaches the tracker (design-notes §197)."""
    bridge = FakeBridge(jpeg=GREY_JPEG, pan_tilt=(90.0, 90.0))
    await bridge.start()
    stop = asyncio.Event()
    w = world()
    seen: list[list[Detection]] = []
    real = w.on_picture

    def spy(
        f: Frame, rgb: NDArray[np.uint8], dets: list[Detection], cam: CameraModel | str | None = None
    ) -> CameraModel | str:
        seen.append(list(dets))
        return real(f, rgb, dets, cam)

    w.on_picture = spy  # type: ignore[method-assign]
    p = Perception(
        BridgeLink("127.0.0.1", bridge.ws_port),
        MjpegSource("127.0.0.1", bridge.stream_port),
        w,
        PersonAndToyDetector(),
        None,
        None,
    )
    try:
        p.start(stop)
        await p.next_picture(2.0)
        assert seen and [d.cls for d in seen[0]] == ["person", "box"]
        assert [d.cls for d in p.last_dets] == ["person", "box"]
    finally:
        stop.set()
        await p.close()
        await bridge.stop()


async def test_on_frame_called_with_depth_or_none() -> None:
    bridge = FakeBridge(jpeg=GREY_JPEG, pan_tilt=(90.0, 90.0), frames_for_s=0.3)
    await bridge.start()
    stop = asyncio.Event()
    seen: list[tuple[int, tuple[int, ...], int, bool]] = []
    p = Perception(
        BridgeLink("127.0.0.1", bridge.ws_port),
        MjpegSource("127.0.0.1", bridge.stream_port),
        world(),
        None,
        None,
        None,
        on_frame=lambda cap_us, rgb, dets, inv, _cls: seen.append(
            (cap_us, rgb.shape, len(dets), inv is None)
        ),
    )
    try:
        p.start(stop)
        for _ in range(4):
            await p.next_picture(1.0)
        assert seen and all(s[1][2] == 3 and s[3] for s in seen)  # no depth model: inv None every picture
    finally:
        stop.set()
        await p.close()
        await bridge.stop()


async def test_on_frame_after_seeing_with_its_classes() -> None:
    """Plan Z11 (LLD §26.2): with a depth model the hook gets the picture's class picture (seen first)."""
    from tests.test_map_app import FloorDepth

    bridge = FakeBridge(jpeg=GREY_JPEG, pan_tilt=(90.0, 90.0), frames_for_s=0.3)
    await bridge.start()
    stop = asyncio.Event()
    got: list[bool] = []
    depth = FloorDepth()
    p = Perception(
        BridgeLink("127.0.0.1", bridge.ws_port),
        MjpegSource("127.0.0.1", bridge.stream_port),
        World(CAL, CFG, depth, frozenset(), {}, Limits(), None),
        None,
        depth,
        None,
        on_frame=lambda cap_us, rgb, dets, inv, classes: got.append(classes is not None),
    )
    try:
        p.start(stop)
        used: list[bool] = []
        for _ in range(6):
            u = await p.next_picture(1.0)
            used.append(u is not None and not isinstance(u, str))
        assert len(got) == 6  # once per picture, used or not
        assert any(used) and all(g == u for g, u in zip(got, used, strict=True))  # classes only when used
    finally:
        stop.set()
        await p.close()
        await bridge.stop()
