"""`cq-map`: the room map (AI engine LLD §18.5, spec 9 §3) - the depth picture's height marks and the distance
sensor's echoes on one 8 x 8 m map fixed where the tank started. It only watches; it never drives: drive with
the phone and watch the map fill (red low, orange high, grey seen, black unseen, the tank white).

    uv run cq-map recordings/20261028-101500      # replay: every picture, in order; prints the map's numbers
    uv run cq-map --live [--record DIR]           # the tank: the live view of the map

Without the depth model (--no-depth, or no file) only the sensor's echoes mark the map.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import datetime
import json
import signal
import time
from collections.abc import Callable
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.map_config import load_map_config
from conqueror_ai.adapters.session import SessionRecorder, replay
from conqueror_ai.app.objects import (
    CALIBRATION,
    DEFAULT_HOST,
    DEPTH_MODEL,
    MAP_CONFIG,
    VGA,
    DepthFn,
    decode,
    depth_due,
    load_depth,
    see,
    see_off_loop,
)
from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.depth import MapConfig, Seeing, Seen
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.objects import ObjectPipeline
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap
from conqueror_ai.domain.sonar_model import SonarModel
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import Detection, Frame, TelemSample

if TYPE_CHECKING:
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource
    from conqueror_ai.adapters.rerun_view import RerunView

STATUS_S = 1.0  # a status line this often


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


class MapRun:
    """Telemetry and pictures in arrival order -> the room map (the depth model handed in)."""

    def __init__(
        self,
        cal: Calibration,
        cfg: MapConfig,
        depth: DepthFn | None,
        p: ObjectPipeline | None = None,
        lim: Limits | None = None,
    ) -> None:
        """p: cq-drive's pipeline with its detectors (Plan Z5); none: the pose and pictures' cameras only.
        lim: the tank's body for the floor under it (Plan Z6); the default body when none."""
        self.cfg = cfg
        self.lim = lim or Limits()
        self.depth = depth
        self.p = p or ObjectPipeline(cal, frozenset())
        self.room = RoomMap(cfg.map)
        self.sonar = SonarModel(max_m=cfg.ranges.sonar_max_m)
        self.seeing = Seeing(cfg)
        self.angle_trusted = True
        self.used = 0
        self.depth_pictures = 0
        self.marked = 0
        self.echoes = 0

    def on_calib(self, counts_per_m: int) -> None:
        self.p.on_calib(counts_per_m)

    def on_telem(self, s: TelemSample) -> None:
        self.p.on_telem(s)
        pose = self.p.pose()
        dist = None if s.dist_mm is None or s.dist_mm <= 0 else s.dist_mm / 1000.0
        if pose is not None and self.sonar.update(self.room, pose, dist, s.bus_us):
            self.echoes += 1
        if pose is not None:  # the floor under the tank is free (Plan Z6)
            lim = self.lim
            self.room.body_over(pose, lim.body_front_m, lim.body_rear_m, lim.body_half_w_m, s.bus_us)

    def on_text(self, text: str) -> None:
        """One recorded message: the tank's calibration or a telemetry sample."""
        m = json.loads(text)
        if m.get("t") == "calib" and isinstance(m.get("counts_per_m"), int):
            self.on_calib(int(m["counts_per_m"]))
            return
        s = parse_td(m)
        if s is not None:
            self.on_telem(s)

    def gate(
        self,
        f: Frame,
        rgb: NDArray[np.uint8],
        dets: list[Detection] | None = None,
        cam: CameraModel | str | None = None,
    ) -> CameraModel | str:
        """The picture (with its detections, and its camera if taken before detecting) into the pipeline: its
        camera when the map uses it, else why it was skipped ("turning", "camera moving", ...)."""
        used = self.p.on_frame(f, (rgb.shape[1], rgb.shape[0]), dets or [], cam or self.p.camera_at(f.cap_us))
        if not isinstance(used, str):
            self.used += 1
        return used

    def depth_due(self) -> bool:
        return self.depth is not None and depth_due(self.used, self.cfg.depth.every_n)

    def depth_due_next(self) -> bool:
        """Depth for the next picture if the map uses it (asked before detecting, as cq-drive does)."""
        return self.depth is not None and depth_due(self.used + 1, self.cfg.depth.every_n)

    def apply(self, seen: Seen, t_us: int) -> None:
        """A seen picture onto the map."""
        self.depth_pictures += 1
        self.angle_trusted = seen.fix.trusted
        if seen.marks is not None:
            self.room.update_picture(seen.marks, t_us)
            self.marked += 1

    def on_picture(self, f: Frame, rgb: NDArray[np.uint8]) -> Seen | None:
        """One picture, all here (the replay)."""
        cam = self.gate(f, rgb)
        if isinstance(cam, str) or self.depth is None or not self.depth_due():
            return None
        seen = see(self.seeing, self.p, cam, self.depth(rgb), f.cap_us)
        self.apply(seen, f.cap_us)
        return seen

    def snapshot(self, t_us: int, stale: bool) -> MapSnapshot:
        return self.room.snapshot(t_us, self.p.pose(), stale, self.angle_trusted)


Show = Callable[[MapRun, Frame], None]


def run_replay(
    folder: Path, cal: Calibration, cfg: MapConfig, depth: DepthFn | None, show: Show | None = None
) -> MapRun:
    """Every recorded message and picture, in order (the same map every time)."""
    run = MapRun(cal, cfg, depth)
    for e in replay(folder):
        if e.text is not None:
            run.on_text(e.text)
        elif e.frame is not None:
            rgb = decode(e.frame)
            if rgb is None:
                continue
            run.on_picture(e.frame, rgb)
            if show is not None:
                show(run, e.frame)
    return run


def report(run: MapRun) -> str:
    room = run.room
    seen_m2 = float(room.seen.sum()) * room.cell_m**2
    b = run.cfg.map.blocked
    low = int(((room.low > b) & ~(room.high > b)).sum())
    high = int((room.high > b).sum())
    return "\n".join(
        [
            f"pictures used {run.used}, with depth {run.depth_pictures}, with marks {run.marked}; "
            f"echoes used {run.echoes}",
            f"map: seen {seen_m2:.1f} m², blocked low {low} cells, high {high} cells",
        ]
    )


def status(run: MapRun, per_s: float, stale: str | None) -> str:
    room, b = run.room, run.cfg.map.blocked
    low = int(((room.low > b) & ~(room.high > b)).sum())
    line = (
        f"map: {per_s:.0f}/s, seen {float(room.seen.sum()) * room.cell_m**2:.1f} m², "
        f"blocked low {low} high {int((room.high > b).sum())}"
    )
    return line if stale is None else f"{line} - stale: {stale}"


async def run_live(
    link: BridgeLink,
    cam: MjpegSource,
    run: MapRun,
    rec: SessionRecorder | None,
    view: RerunView | None,
    seconds: float | None = None,
    stop: asyncio.Event | None = None,
) -> MapRun:
    """The tank: telemetry feeds the pose and the sensor; each new picture (the newest wins) goes through the
    depth model and Seeing in worker threads and onto the map. Ends after `seconds` or when `stop` is set."""
    asked = stop or asyncio.Event()
    halt = asyncio.Event()
    if rec is not None:
        link.tap = rec.telemetry
    last = {"telem": 0.0, "picture": 0.0}

    async def telemetry() -> None:
        calib_given = False
        while not halt.is_set():
            s = await link.samples.get()
            if not calib_given and link.counts_per_m is not None:
                run.on_calib(link.counts_per_m)
                calib_given = True
            run.on_telem(s)
            last["telem"] = time.monotonic()

    tasks = [asyncio.create_task(t) for t in (link.run(halt), cam.run(halt), telemetry())]
    t0 = next_status = time.monotonic()
    updates = 0
    try:
        while not asked.is_set() and (seconds is None or time.monotonic() - t0 < seconds):
            if (failed := task_failure(tasks)) is not None:  # say the real error, not "stale" for ever
                print(f"stopped: failed - {failed}", flush=True)
                break
            now = time.monotonic()
            stale = map_state(now, t0, last["picture"], last["telem"])
            if now >= next_status:
                print(status(run, updates / STATUS_S, stale), flush=True)
                updates, next_status = 0, now + STATUS_S
            try:
                f = await asyncio.wait_for(cam.next_frame(), 0.2)
            except TimeoutError:
                continue
            last["picture"] = time.monotonic()
            if rec is not None:
                rec.frame(f)
            rgb = decode(f)
            if rgb is None:
                continue
            used = run.gate(f, rgb)
            if not isinstance(used, str) and run.depth is not None and run.depth_due():
                inv = await asyncio.to_thread(run.depth, rgb)
                run.apply(await see_off_loop(run.seeing, run.p, used, inv, f.cap_us), f.cap_us)
            snap = run.snapshot(f.cap_us, stale is not None)
            updates += 1
            if view is not None:
                view.frame(f, None)
                view.room_map(f.cap_us, snap)
    finally:
        halt.set()
        with contextlib.suppress(Exception):
            await link.close()
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
    return run


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path, nargs="?")
    ap.add_argument("--live", action="store_true")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--record", type=Path, help="with --live: record the session to this folder")
    ap.add_argument("--calibration", type=Path, default=CALIBRATION)
    ap.add_argument("--depth-model", type=Path, default=DEPTH_MODEL)
    ap.add_argument("--no-depth", action="store_true", help="leave the depth model out (echoes only)")
    ap.add_argument("--no-view", action="store_true", help="no live view (the numbers only)")
    a = ap.parse_args()
    if a.live == (a.folder is not None):
        raise SystemExit("give a recording folder, or --live")
    try:
        cal = calibration_store.load(a.calibration, VGA)
        cfg = load_map_config(MAP_CONFIG)
    except (FileNotFoundError, ValueError, KeyError) as err:
        raise SystemExit(str(err)) from None
    depth = None if a.no_depth else load_depth(a.depth_model)
    if a.live:
        live(a, cal, cfg, depth)
        return
    show: Show | None = None
    if not a.no_view:
        from conqueror_ai.adapters.rerun_view import RerunView

        view = RerunView(spawn=True)

        def to_view(run: MapRun, f: Frame) -> None:
            view.frame(f, None)
            view.room_map(f.cap_us, run.snapshot(f.cap_us, False))

        show = to_view

    t = time.monotonic()
    with contextlib.suppress(KeyboardInterrupt):
        run = run_replay(a.folder, cal, cfg, depth, show)
        print(report(run))
        print(f"laptop: {time.monotonic() - t:.1f} s")


def live(a: argparse.Namespace, cal: Calibration, cfg: MapConfig, depth: DepthFn | None) -> None:
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource
    from conqueror_ai.adapters.rerun_view import RerunView

    rec = None
    if a.record is not None:
        when = datetime.datetime.now().isoformat(timespec="seconds")
        rec = SessionRecorder(a.record, {"host": a.host, "what": "cq-map --live", "date": when})
    view = None if a.no_view else RerunView(spawn=True)
    run = MapRun(cal, cfg, depth)
    print("watching the map - drive with the phone; Ctrl-C ends it (cq-map never drives)")

    async def until_ctrl_c() -> MapRun:
        stop = asyncio.Event()
        asyncio.get_running_loop().add_signal_handler(signal.SIGINT, stop.set)
        return await run_live(BridgeLink(a.host), MjpegSource(a.host), run, rec, view, stop=stop)

    try:
        print(report(asyncio.run(until_ctrl_c())))
    finally:
        if rec is not None:
            rec.close()
            print(
                f"recorded {rec.frame_count} pictures and {rec.telem_count} telemetry messages in {a.record}"
            )


if __name__ == "__main__":
    main()
