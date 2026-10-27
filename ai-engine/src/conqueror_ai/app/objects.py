"""`cq-objects`: the things in the room (AI engine LLD §15.6, §16) - phase 4 of the map. It only watches; it
never
drives. What spots things: YOLO-World asked for the words in config/vocabulary.yaml, and the colour ball
finder for
the balls in config/balls.yaml (§16); the camera at VGA.

    uv run cq-objects recordings/20261024-101500      # replay: every picture, in order; prints what was found
    uv run cq-objects --live                          # the tank: the live view (picture + boxes, the plan)

With the depth model (models/DepthAnythingV2SmallF16.mlpackage, AI engine LLD §18) it also finds the camera's
real tilt, and shows the depth picture and every point's height; --no-depth leaves it out.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import math
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.map_config import load_map_config
from conqueror_ai.adapters.session import replay
from conqueror_ai.domain.balls import BallColour, find_balls
from conqueror_ai.domain.camera import Calibration, CameraModel, FloatArray
from conqueror_ai.domain.depth import MapConfig, Seeing, Seen
from conqueror_ai.domain.detectors import MultiDetector
from conqueror_ai.domain.objects import Detector, ObjectPipeline
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import Frame, ObjectEstimate

ENGINE = Path(__file__).resolve().parents[3]
CALIBRATION = ENGINE / "config" / "calibration.yaml"
MODEL = ENGINE / "models" / "yoloworld-s-640.mlpackage"
VOCABULARY = ENGINE / "config" / "vocabulary.yaml"
BALLS = ENGINE / "config" / "balls.yaml"
MAP_CONFIG = ENGINE / "config" / "map.yaml"
DEPTH_MODEL = ENGINE / "models" / "DepthAnythingV2SmallF16.mlpackage"
VGA = (640, 480)
DEFAULT_HOST = "192.168.31.167"


def decode(f: Frame) -> NDArray[np.uint8] | None:
    # OpenCV returns None for a broken JPEG (its type hints do not say so)
    bgr = cast(
        "NDArray[np.uint8] | None", cv2.imdecode(np.frombuffer(f.jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
    )
    if bgr is None:
        return None
    return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB).astype(np.uint8)


@dataclass
class Thing:
    """What the replay says about one thing it found."""

    cls: str
    first_us: int
    last_us: int
    x_m: float
    y_m: float
    sigma_m: float


@dataclass
class ReplayResult:
    things: dict[int, Thing] = field(default_factory=lambda: dict[int, Thing]())
    used: int = 0
    skips: dict[str, int] = field(default_factory=lambda: dict[str, int]())
    start_us: int | None = None
    depth: bool = False
    angle_fixes: list[float] = field(default_factory=lambda: list[float]())  # trusted fixes, per estimate
    estimates: int = 0  # pictures that gave a tilt from depth
    trusted: int = 0
    seconds: float = 0.0  # laptop time spent on the pictures


DepthFn = Callable[[NDArray[np.uint8]], FloatArray]


def load_depth(path: Path) -> DepthFn | None:
    """The depth model, or None (said plainly) when its file is missing: everything runs without depth."""
    if not path.exists():
        print(f"no depth model at {path} - running without depth (copy it from tools/measure/models/da2s)")
        return None
    from conqueror_ai.adapters.depth_coreml import DepthCoreml

    try:
        model = DepthCoreml(path)
    except Exception as e:  # a broken file or a CoreML error: say it, and go on without depth
        print(f"the depth model at {path} didn't load ({e}) - running without depth")
        return None
    return lambda rgb: model.infer(rgb).astype(np.float64)


def depth_due(used: int, every_n: int) -> bool:
    """Depth on every n-th picture used (config depth.every_n), the same rule in every loop."""
    return used % every_n == 0


def see(seeing: Seeing, p: ObjectPipeline, cam: CameraModel, inv: FloatArray, t_us: int) -> Seen:
    """One depth picture: the tilt fix goes to every picture from now on (the ball's distance too)."""
    seen = seeing.see(cam, inv, t_us)
    p.set_tilt_fix(seen.fix.tilt_fix_deg)
    return seen


async def see_off_loop(
    seeing: Seeing, p: ObjectPipeline, cam: CameraModel, inv: FloatArray, t_us: int, full: bool = True
) -> Seen:
    """see() in a worker thread, so telemetry keeps flowing (final review C1); the fix is set back here."""
    seen = await asyncio.to_thread(seeing.see if full else seeing.angle_only, cam, inv, t_us)
    p.set_tilt_fix(seen.fix.tilt_fix_deg)
    return seen


def _note_angle(res: ReplayResult, seen: Seen) -> None:
    if seen.fix.fresh:
        res.estimates += 1
        if seen.fix.trusted:
            res.trusted += 1
            res.angle_fixes.append(seen.fix.tilt_fix_deg)


def _note(res: ReplayResult, objs: list[ObjectEstimate]) -> None:
    for o in objs:
        res.things[o.oid] = Thing(o.cls, o.first_seen_us, o.last_seen_us, o.x_m, o.y_m, o.sigma_m)


def camera_of(cam: CameraModel | str) -> CameraModel | None:
    """The picture's camera for the detectors; None (all of it, no size check)
    for a picture the gate skips - its boxes are only shown, never placed."""
    return None if isinstance(cam, str) else cam


def run_replay(
    folder: Path,
    cal: Calibration,
    det: Detector,
    floor_classes: frozenset[str],
    depth: DepthFn | None = None,
    cfg: MapConfig | None = None,
) -> ReplayResult:
    """Every recorded picture, in order, through the detector and the list (the same answer every time); with
    depth, each used picture also through Seeing (the tilt fix)."""
    p = ObjectPipeline(cal, floor_classes)
    res = ReplayResult(depth=depth is not None)
    seeing = Seeing(cfg or MapConfig())
    every = (cfg or MapConfig()).depth.every_n
    t0 = time.monotonic()
    for e in replay(folder):
        if e.text is not None:
            m = json.loads(e.text)
            if m.get("t") == "calib" and isinstance(m.get("counts_per_m"), int):
                p.on_calib(int(m["counts_per_m"]))
                continue
            s = parse_td(m)
            if s is not None:
                if res.start_us is None:
                    res.start_us = s.bus_us
                p.on_telem(s)
        elif e.frame is not None:
            rgb = decode(e.frame)
            if rgb is None:
                continue
            cam = p.camera_at(e.frame.cap_us)
            dets = det.detect(rgb, camera_of(cam))
            cam = p.on_frame(e.frame, (rgb.shape[1], rgb.shape[0]), dets, cam)
            if not isinstance(cam, str):
                res.used += 1
                _note(res, p.objects())
                if depth is not None and depth_due(res.used, every):
                    _note_angle(res, see(seeing, p, cam, depth(rgb), e.frame.cap_us))
    res.skips = p.skips()
    res.seconds = time.monotonic() - t0
    return res


def report(res: ReplayResult) -> str:
    t0 = res.start_us or 0
    lines = [f"{res.used} pictures used" + (f"; skipped: {res.skips}" if res.skips else "")]
    if res.depth:
        if res.angle_fixes:
            med = float(np.median(res.angle_fixes))
            lines.append(f"camera angle: fix {med:+.1f} deg, trusted {res.trusted}/{res.estimates} estimates")
        else:
            lines.append(
                f"camera angle: no estimate ({res.estimates} untrusted)"
                if res.estimates
                else "camera angle: no estimate"
            )
        if res.seconds > 0:
            lines.append(f"laptop: {res.used / res.seconds:.1f} pictures/s with depth")
    if not res.things:
        lines.append("nothing found")
    for oid, th in sorted(res.things.items()):
        lines.append(
            f"#{oid} {th.cls}: seen {(th.first_us - t0) / 1e6:.1f}-{(th.last_us - t0) / 1e6:.1f} s, "
            f"last at x {th.x_m * 100:.0f} cm, y {th.y_m * 100:.0f} cm (+-{th.sigma_m * 100:.0f} cm), "
            f"{math.hypot(th.x_m, th.y_m) * 100:.0f} cm from the start"
        )
    return "\n".join(lines)


async def run_live(
    host: str,
    cal: Calibration,
    det: Detector,
    floor_classes: frozenset[str],
    depth: DepthFn | None = None,
    cfg: MapConfig | None = None,
) -> None:
    """The tank: telemetry and the newest picture; the detector in a worker thread (latest picture wins)."""
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource
    from conqueror_ai.adapters.rerun_view import RerunView

    link, cam = BridgeLink(host), MjpegSource(host)
    view = RerunView(spawn=True)
    p = ObjectPipeline(cal, floor_classes)
    seeing = Seeing(cfg or MapConfig())
    every, used_n = (cfg or MapConfig()).depth.every_n, 0
    stop = asyncio.Event()
    tasks = [asyncio.create_task(link.run(stop)), asyncio.create_task(cam.run(stop))]

    async def telemetry() -> None:
        calib_given = False
        while not stop.is_set():
            s = await link.samples.get()
            if not calib_given and link.counts_per_m is not None:
                p.on_calib(link.counts_per_m)
                calib_given = True
            p.on_telem(s)

    tasks.append(asyncio.create_task(telemetry()))

    def died(t: asyncio.Task[None]) -> None:  # a helper that stops with an error ends the run, plainly
        if not t.cancelled() and t.exception() is not None:
            print(f"stopped: {t.get_name()} failed - {t.exception()!r}")
            stop.set()

    for t in tasks:
        t.add_done_callback(died)
    try:
        while not stop.is_set():
            try:
                f = await asyncio.wait_for(cam.next_frame(), 1.0)
            except TimeoutError:
                continue
            rgb = decode(f)
            if rgb is None:
                continue
            cam_f = p.camera_at(f.cap_us)
            # meanwhile newer pictures replace the waiting one; the depth model runs beside the detector
            want_depth = depth is not None and not isinstance(cam_f, str) and depth_due(used_n + 1, every)
            if want_depth and depth is not None:
                dets, inv = await asyncio.gather(
                    asyncio.to_thread(det.detect, rgb, camera_of(cam_f)), asyncio.to_thread(depth, rgb)
                )
            else:
                dets, inv = await asyncio.to_thread(det.detect, rgb, camera_of(cam_f)), None
            used = p.on_frame(f, (rgb.shape[1], rgb.shape[0]), dets, cam_f)
            view.frame(f, None)
            view.detections(f, dets)
            if not isinstance(used, str):
                used_n += 1
                if inv is not None:
                    seen = await see_off_loop(seeing, p, used, inv, f.cap_us)
                    view.depth(f, None if seen.scaled is None else seen.scaled.range_m / seen.cam.ray_norms())
                    view.marks(f.cap_us, seen.marks)
                    view.angle(f.cap_us, seen.fix)
            view.plan(f.cap_us, p.pose(), p.objects(), used if isinstance(used, str) else None)
    finally:
        stop.set()
        await link.close()
        for t in tasks:
            t.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await asyncio.gather(*tasks, return_exceptions=True)


def detectors(model: Path, vocab_path: Path, balls_path: Path) -> tuple[Detector, frozenset[str]]:
    """YOLO-World and the colour ball finder as one detector, and the classes the floor trick places (§16)."""
    from conqueror_ai.adapters.detector_config import load_balls, load_vocabulary
    from conqueror_ai.adapters.yolo_world import YoloWorld

    vocab = load_vocabulary(vocab_path)
    world = YoloWorld(model, vocab)
    balls: list[BallColour] = load_balls(balls_path)
    multi = MultiDetector([world.detect, lambda rgb, cam: find_balls(rgb, balls, cam)])
    return multi, vocab.floor_classes | frozenset(f"ball:{b.name}" for b in balls)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path, nargs="?")
    ap.add_argument("--live", action="store_true")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--calibration", type=Path, default=CALIBRATION)
    ap.add_argument("--model", type=Path, default=MODEL)
    ap.add_argument("--vocabulary", type=Path, default=VOCABULARY)
    ap.add_argument("--balls", type=Path, default=BALLS)
    ap.add_argument("--depth-model", type=Path, default=DEPTH_MODEL)
    ap.add_argument("--no-depth", action="store_true", help="leave the depth model out")
    a = ap.parse_args()
    if a.live == (a.folder is not None):
        raise SystemExit("give a recording folder, or --live")
    try:
        cal = calibration_store.load(a.calibration, VGA)
        det, floor = detectors(a.model, a.vocabulary, a.balls)
        cfg = load_map_config(MAP_CONFIG)
    except (FileNotFoundError, ValueError, KeyError) as err:
        raise SystemExit(str(err)) from None
    depth = None if a.no_depth else load_depth(a.depth_model)
    if a.live:
        with contextlib.suppress(KeyboardInterrupt):
            asyncio.run(run_live(a.host, cal, det, floor, depth, cfg))
    else:
        print(report(run_replay(a.folder, cal, det, floor, depth, cfg)))


if __name__ == "__main__":
    main()
