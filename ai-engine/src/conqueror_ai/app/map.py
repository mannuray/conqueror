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
import signal
import time
from collections.abc import Callable
from pathlib import Path
from typing import TYPE_CHECKING

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
    load_depth,
)
from conqueror_ai.app.perception import Perception
from conqueror_ai.app.world import MapRun, World
from conqueror_ai.domain.camera import Calibration
from conqueror_ai.domain.depth import MapConfig
from conqueror_ai.domain.types import Frame

if TYPE_CHECKING:
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource
    from conqueror_ai.adapters.rerun_view import RerunView

STATUS_S = 1.0  # a status line this often


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
    """The tank through Perception (LLD §19.5): telemetry feeds the pose and the sensor; each new picture (the
    newest wins) through the depth model and Seeing in worker threads and onto the map. Ends after `seconds`
    or when `stop` is set."""
    asked = stop or asyncio.Event()
    halt = asyncio.Event()
    p = Perception(link, cam, World.around(run), None, run.depth, rec)
    p.start(halt)
    t0 = next_status = time.monotonic()
    updates = 0
    try:
        while not asked.is_set() and (seconds is None or time.monotonic() - t0 < seconds):
            if (failed := p.failure()) is not None:  # say the real error, not "stale" for ever
                print(f"stopped: failed - {failed}", flush=True)
                break
            now = time.monotonic()
            stale = p.state(now)
            if now >= next_status:
                print(status(run, updates / STATUS_S, stale), flush=True)
                updates, next_status = 0, now + STATUS_S
            if await p.next_picture(0.2) is None:
                continue
            f = p.world.last_frame
            if f is None:
                continue
            snap = run.snapshot(f.cap_us, stale is not None)
            updates += 1
            if view is not None:
                view.frame(f, None)
                view.room_map(f.cap_us, snap)
    finally:
        halt.set()
        await p.close()
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
