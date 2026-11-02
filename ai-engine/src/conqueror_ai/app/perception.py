"""One camera and telemetry loop for every tool (AI engine LLD §19.5, review F2): the bridge link, the camera
stream, telemetry into the World, each picture decoded, detected and depth-run in worker threads (the newest
picture wins), the gate and the map on the event loop, seeing off it. cq-map, cq-objects and cq-drive use it;
the staleness rule (spec 7 §4) lives here."""

from __future__ import annotations

import asyncio
import contextlib
import time
from typing import TYPE_CHECKING

from conqueror_ai.app.objects import DepthFn, camera_of, decode
from conqueror_ai.app.world import World
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.depth import Seen
from conqueror_ai.domain.objects import Detector
from conqueror_ai.domain.types import Detection, TelemSample

if TYPE_CHECKING:
    from collections.abc import Coroutine
    from typing import Any

    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource
    from conqueror_ai.adapters.session import SessionRecorder

SKIPS_THAT_COUNT = ("turning", "camera moving")  # the map skips these pictures, but the camera is alive


def picture_counts(used: CameraModel | str) -> bool:
    """A picture shows the camera is alive when the map used it, or skipped it only for the tank's or the
    camera's own motion (a spin, a camera sweep)."""
    return not isinstance(used, str) or used in SKIPS_THAT_COUNT


def task_failure(tasks: list[asyncio.Task[None]]) -> str | None:
    """The first worker (bridge link, camera, drive link, telemetry) that crashed, as "Kind: message"."""
    for t in tasks:
        if t.done() and not t.cancelled() and (e := t.exception()) is not None:
            return f"{type(e).__name__}: {e}"
    return None


STALE_PICTURE_S = 0.5  # spec 7 §4: no driving on a stale map
STALE_TELEM_S = 0.2
FIRST_PICTURE_S = 5.0  # the camera stream may take this long to start


def map_state(now: float, t0: float, last_picture: float, last_telem: float) -> str | None:
    """None when pictures and telemetry are fresh (spec 7 §4); "waiting" before the first picture; else why
    not. Times in seconds (time.monotonic); 0 = never."""
    if last_picture == 0.0:
        return "waiting" if now - t0 < FIRST_PICTURE_S else "no pictures from the camera"
    if now - last_picture > STALE_PICTURE_S:
        return f"no new picture for {now - last_picture:.1f} s"
    if now - last_telem > STALE_TELEM_S:
        return f"no telemetry for {now - last_telem:.1f} s"
    return None


class Perception:
    def __init__(
        self,
        link: BridgeLink,
        cam: MjpegSource,
        world: World,
        det: Detector | None,
        depth: DepthFn | None,
        rec: SessionRecorder | None,
    ) -> None:
        """det None: no detector (cq-map). depth None: no depth model (the sonar only)."""
        self.link, self.cam, self.world = link, cam, world
        self.det, self.depth, self.rec = det, depth, rec
        self.tasks: list[asyncio.Task[None]] = []
        self.t0 = 0.0
        self.last_picture_s = 0.0  # time.monotonic of the last picture that shows the camera is alive
        self.last_telem_s = 0.0
        self.last_dets: list[Detection] = []
        self.last_seen: Seen | None = None

    def start(self, stop: asyncio.Event, *more: Coroutine[Any, Any, None]) -> None:
        """The link, the stream and the telemetry tasks (and any more, e.g. the drive link) until `stop`."""
        if self.rec is not None:
            self.link.tap = self.rec.telemetry
        self.t0 = time.monotonic()
        jobs = (self.link.run(stop), self.cam.run(stop), self._telemetry(stop), *more)
        self.tasks = [asyncio.create_task(j) for j in jobs]

    async def _telemetry(self, stop: asyncio.Event) -> None:
        calib_given = False
        while not stop.is_set():
            s: TelemSample = await self.link.samples.get()
            if not calib_given and self.link.counts_per_m is not None:
                self.world.on_calib(self.link.counts_per_m)
                calib_given = True
            self.world.on_telem(s)  # the pose, and the sensor onto the map
            self.last_telem_s = time.monotonic()

    async def next_picture(self, timeout_s: float) -> CameraModel | str | None:
        """One new picture through the detector, the depth model (when due), the gate and seeing: its camera
        when the map used it, the reason when skipped, None when no picture came within `timeout_s` (or it
        didn't decode - no picture, not the last one again: Z7 final review minor 4)."""
        try:
            f = await asyncio.wait_for(self.cam.next_frame(), timeout_s)
        except TimeoutError:
            return None
        rgb = await asyncio.to_thread(decode, f)
        if rgb is None:
            return None
        if self.rec is not None:
            self.rec.frame(f)
        run = self.world.map_run
        cam_f = run.p.camera_at(f.cap_us)
        want = self.depth is not None and not isinstance(cam_f, str) and run.depth_due_next()
        detect = self.det.detect if self.det is not None else None
        if want and self.depth is not None:
            if detect is None:
                dets, inv = [], await asyncio.to_thread(self.depth, rgb)
            else:
                dets, inv = await asyncio.gather(
                    asyncio.to_thread(detect, rgb, camera_of(cam_f)), asyncio.to_thread(self.depth, rgb)
                )
        else:
            dets = [] if detect is None else await asyncio.to_thread(detect, rgb, camera_of(cam_f))
            inv = None
        used = self.world.on_picture(f, rgb, dets, cam_f)  # the map and the pose stay on the event loop
        self.last_dets, self.last_seen = dets, None
        if picture_counts(used):
            self.last_picture_s = time.monotonic()
        if not isinstance(used, str) and inv is not None:  # seeing off the loop, the map on it
            seen = await asyncio.to_thread(self.world.see, used, inv, f.cap_us)
            self.world.apply(seen, f.cap_us)
            self.last_seen = seen
        return used

    def state(self, now: float) -> str | None:
        """None when pictures and telemetry are fresh; "waiting" before the first picture; else why not."""
        return map_state(now, self.t0, self.last_picture_s, self.last_telem_s)

    def failure(self) -> str | None:
        return task_failure(self.tasks)

    async def close(self) -> None:
        with contextlib.suppress(Exception):
            await self.link.close()
        for t in self.tasks:
            t.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await asyncio.gather(*self.tasks, return_exceptions=True)
