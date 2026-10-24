"""`cq-objects`: the things in the room (AI engine LLD §15.6, §16) - phase 4 of the map. It only watches; it
never
drives. What spots things: YOLO-World asked for the words in config/vocabulary.yaml, and the colour ball
finder for
the balls in config/balls.yaml (§16); the camera at VGA.

    uv run cq-objects recordings/20261024-101500      # replay: every picture, in order; prints what was found
    uv run cq-objects --live                          # the tank: the live view (picture + boxes, the plan)
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.session import replay
from conqueror_ai.domain.balls import BallColour, find_balls, horizon_row
from conqueror_ai.domain.camera import Calibration
from conqueror_ai.domain.detectors import MultiDetector
from conqueror_ai.domain.objects import FLOOR_CLASSES, Detector, ObjectPipeline
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import Frame, ObjectEstimate

ENGINE = Path(__file__).resolve().parents[3]
CALIBRATION = ENGINE / "config" / "calibration.yaml"
MODEL = ENGINE / "models" / "yoloworld-s-640.mlpackage"
VOCABULARY = ENGINE / "config" / "vocabulary.yaml"
BALLS = ENGINE / "config" / "balls.yaml"
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


def _note(res: ReplayResult, objs: list[ObjectEstimate]) -> None:
    for o in objs:
        res.things[o.oid] = Thing(o.cls, o.first_seen_us, o.last_seen_us, o.x_m, o.y_m, o.sigma_m)


def horizon_of(p: ObjectPipeline, f: Frame) -> int | None:
    """The picture's horizon row from its camera (the ball finder looks below it); None if unknown."""
    cam = p.camera_at(f.cap_us)
    return None if cam is None else horizon_row(cam)


def run_replay(
    folder: Path, cal: Calibration, det: Detector, floor_classes: frozenset[str] = FLOOR_CLASSES
) -> ReplayResult:
    """Every recorded picture, in order, through the detector and the list (the same answer every time)."""
    p = ObjectPipeline(cal, floor_classes)
    res = ReplayResult()
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
            cam = p.on_frame(e.frame, (rgb.shape[1], rgb.shape[0]), det.detect(rgb, horizon_of(p, e.frame)))
            if not isinstance(cam, str):
                res.used += 1
                _note(res, p.objects())
    res.skips = p.skips()
    return res


def report(res: ReplayResult) -> str:
    t0 = res.start_us or 0
    lines = [f"{res.used} pictures used" + (f"; skipped: {res.skips}" if res.skips else "")]
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
    host: str, cal: Calibration, det: Detector, floor_classes: frozenset[str] = FLOOR_CLASSES
) -> None:
    """The tank: telemetry and the newest picture; the detector in a worker thread (latest picture wins)."""
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource
    from conqueror_ai.adapters.rerun_view import RerunView

    link, cam = BridgeLink(host), MjpegSource(host)
    view = RerunView(spawn=True)
    p = ObjectPipeline(cal, floor_classes)
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
            dets = await asyncio.to_thread(
                det.detect, rgb, horizon_of(p, f)
            )  # meanwhile newer pictures replace the waiting one
            used = p.on_frame(f, (rgb.shape[1], rgb.shape[0]), dets)
            view.frame(f, None)
            view.detections(f, dets)
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
    multi = MultiDetector([world.detect, lambda rgb, h: find_balls(rgb, balls, h)])
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
    a = ap.parse_args()
    if a.live == (a.folder is not None):
        raise SystemExit("give a recording folder, or --live")
    try:
        cal = calibration_store.load(a.calibration, VGA)
        det, floor = detectors(a.model, a.vocabulary, a.balls)
    except (FileNotFoundError, ValueError, KeyError) as err:
        raise SystemExit(str(err)) from None
    if a.live:
        with contextlib.suppress(KeyboardInterrupt):
            asyncio.run(run_live(a.host, cal, det, floor))
    else:
        print(report(run_replay(a.folder, cal, det, floor)))


if __name__ == "__main__":
    main()
