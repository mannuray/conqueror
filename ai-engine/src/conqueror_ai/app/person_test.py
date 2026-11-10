"""`cq-person-test`: can the tank see a crawling person? (Spec 10 phase 0, Plan P0.) Measurement only.

`record` guides you mark by mark: tape from the tank's front edge at 0.3 / 0.5 / 1 / 1.5 / 2 m straight ahead
and on a 30-degree line to the right; crawl to each mark, hands on the tape, still, then slowly across it.
Nothing drives: only the camera moves, to level and then to the driving tilt, and it says so first. `score`
replays a recording through the detectors (never the tracker, the memory or the map), turns each person box
into a distance from the front edge and a direction, and grades every mark against the owner's bar.

    uv run cq-person-test record recordings/p0-20261109 [--host 192.168.31.167]
    uv run cq-person-test score recordings/p0-20261109 [--world-model M --world-vocabulary V] [--no-depth]

The pictures stay on this machine (recordings/ is git-ignored); the report is numbers only. When the
engine's detector (YOLO-World) passes every mark, recorded whole, `score` writes config/person-seeing.yaml
and the games unlock (AI engine LLD §28.6); anything less removes it.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import datetime
import json
import threading
from collections.abc import Callable
from dataclasses import asdict, dataclass, replace
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.adapters.session import SessionRecorder, replay
from conqueror_ai.app.objects import (
    CALIBRATION,
    DEPTH_MODEL,
    MAP_CONFIG,
    MODEL,
    MODELS,
    VGA,
    VOCABULARY,
    DepthFn,
    decode,
    load_depth,
)
from conqueror_ai.app.record import NoView, record
from conqueror_ai.app.seeing_pass import PASS_FILE, write_pass
from conqueror_ai.domain.camera import Calibration, CameraModel, from_servo
from conqueror_ai.domain.depth import Seeing
from conqueror_ai.domain.objects import MAYBE_SCORE
from conqueror_ai.domain.person_test import (
    Mark,
    MarkScore,
    PersonRange,
    person_range,
    pick_person,
    score_mark,
    verdict,
)
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import Detection, Frame, Pose

DEFAULT_HOST = "192.168.31.167"
DetectFn = Callable[[NDArray[np.uint8], CameraModel | None], list[Detection]]
STILL_PICTURES = 10
SETTLE_S = 0.5  # pictures this soon after the start or a camera move are not scored
MOVED_DEG = 0.5  # a servo change beyond this is a camera move (smaller: jitter)
COCO_CONF = 0.25  # YOLO26n's person boxes from this score (the plan)
PERSON = "person"


@dataclass(frozen=True)
class Unlock:
    """The seeing test's pass (AI engine LLD §28.6): written when `detector` (the engine's) passes every
    `required` mark, recorded whole; removed on anything less, so a worse result locks the games again."""

    detector: str  # its name among the scored detectors
    path: Path
    model: str  # the model file's name the engine checks (SeeingPass)
    calibration: str  # the camera calibration's date
    required: tuple[str, ...]  # the mark ids a pass needs


def all_marks() -> list[Mark]:
    """40 marks: level, then the driving tilt (the camera moves twice); near to far; still, then crawling."""
    out: list[Mark] = []
    for tilt in (0.0, -15.0):
        for angle in (0.0, -30.0):
            for tape in (0.3, 0.5, 1.0, 1.5, 2.0):
                for still in (True, False):
                    mid = f"{tape:.1f}-{-angle:.0f}-{'still' if still else 'crawl'}-t{tilt:.0f}"
                    out.append(Mark(mid, tape, angle, still, tilt))
    return out


def _say(mark: Mark) -> str:
    where = (
        "straight ahead"
        if mark.angle_deg == 0.0
        else f"on the {-mark.angle_deg:.0f}-degree line to the right"
    )
    how = "stay still" if mark.still else "crawl slowly to and fro across the mark"
    return f"Crawl to {mark.tape_m:.1f} m {where}, hands on the tape, {how} - Enter to record, s to skip: "


async def _ask(ask: Callable[[str], str], prompt: str) -> str:
    """The prompt in a daemon thread: Ctrl-C while it waits ends the program at once (P0 review minor)."""
    loop = asyncio.get_running_loop()
    fut: asyncio.Future[str] = loop.create_future()

    def run() -> None:
        try:
            answer = ask(prompt)
        except BaseException as e:  # EOF, a closed terminal: passed to the waiting coroutine
            err = e
            with contextlib.suppress(RuntimeError):
                loop.call_soon_threadsafe(lambda: fut.done() or fut.set_exception(err))
            return
        with contextlib.suppress(RuntimeError):  # the loop already gone
            loop.call_soon_threadsafe(lambda: fut.done() or fut.set_result(answer))

    threading.Thread(target=run, daemon=True).start()
    return await fut


async def _cam_tilt(host: str, ws_port: int, tilt_deg: float) -> None:
    """The camera to (0, tilt) - only the camera moves."""
    link = BridgeLink(host, ws_port, client="p0-camera")
    stop = asyncio.Event()
    task = asyncio.create_task(link.run(stop))
    try:
        for _ in range(50):
            if link.connected:
                break
            await asyncio.sleep(0.1)
        await link.cam_set(0.0, tilt_deg)
    finally:
        stop.set()
        with contextlib.suppress(Exception):
            await link.close()
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


async def record_marks(
    host: str,
    ws_port: int,
    stream_port: int,
    folder: Path,
    marks: list[Mark],
    ask: Callable[[str], str],
    still_s: float = 2.0,
    crawl_s: float = 4.0,
    countdown_s: int = 3,
) -> list[str]:
    """Each mark into folder/<mark id>/ (cq-record's files and mark.json, `complete` once recorded whole).
    Returns the recorded marks' ids."""
    done: list[str] = []
    tilt: float | None = None
    for m in marks:
        if m.tilt_deg != tilt:
            await _ask(ask, f"The camera goes to {m.tilt_deg:.0f} degrees (only the camera moves) - Enter: ")
            await _cam_tilt(host, ws_port, m.tilt_deg)
            tilt = m.tilt_deg
        if (await _ask(ask, _say(m))).strip().lower() == "s":
            continue
        for n in range(countdown_s, 0, -1):  # time to get into place after Enter (P0 review I3)
            print(f"  {n}...", flush=True)
            await asyncio.sleep(1.0)
        rec = SessionRecorder(folder / m.mid, {"what": "cq-person-test", "mark": m.mid})
        about = folder / m.mid / "mark.json"
        about.write_text(json.dumps({**asdict(m), "complete": False}))
        link = BridgeLink(host, ws_port, client="p0-record")
        cam = MjpegSource(host, stream_port)
        await record(link, cam, rec, NoView(), seconds=SETTLE_S + (still_s if m.still else crawl_s))
        about.write_text(json.dumps({**asdict(m), "complete": True}))
        done.append(m.mid)
    return done


def _mark_of(path: Path) -> tuple[Mark, bool]:
    d = json.loads(path.read_text())
    m = Mark(str(d["mid"]), float(d["tape_m"]), float(d["angle_deg"]), bool(d["still"]), float(d["tilt_deg"]))
    return m, bool(d.get("complete", True))


def spread(n: int, k: int) -> list[int]:
    """k indexes spread evenly over n (all of them when n <= k): still pictures over the whole window."""
    if n <= k:
        return list(range(n))
    return [round(i * (n - 1) / (k - 1)) for i in range(k)]


def kept_frames(
    mdir: Path, cal: Calibration, seeing: Seeing | None, depth: DepthFn | None
) -> list[tuple[Frame, CameraModel]]:
    """The mark's pictures that are scored - from 0.5 s after the start and after any camera move - each with
    its camera, the depth floor's tilt fix applied as the engine does; the fix is warmed up on the settle
    window's pictures too (only while the camera is still)."""
    still0 = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)  # the tank stays on its mark
    servo: tuple[float, float] | None = None
    t0: float | None = None
    moved_at = -1e9
    fix_deg = 0.0
    out: list[tuple[Frame, CameraModel]] = []
    for e in replay(mdir):
        t0 = e.recv_s if t0 is None else t0
        if e.text is not None:
            s = parse_td(json.loads(e.text)) if e.text.startswith("{") else None
            if s is not None:
                if (
                    servo is not None
                    and max(abs(s.pan_deg - servo[0]), abs(s.tilt_deg - servo[1])) > MOVED_DEG
                ):
                    moved_at = e.recv_s
                    servo = (s.pan_deg, s.tilt_deg)
                elif servo is None:
                    servo = (s.pan_deg, s.tilt_deg)
            continue
        f = e.frame
        if f is None or servo is None or e.recv_s < moved_at + SETTLE_S:
            continue
        cam = CameraModel(cal).for_frame(still0, *from_servo(*servo))
        if seeing is not None and depth is not None:
            rgb = _picture(f, cal)
            if rgb is not None:
                fix_deg = seeing.angle_only(cam, depth(rgb), f.cap_us).fix.tilt_fix_deg
        if e.recv_s >= t0 + SETTLE_S:
            out.append((f, replace(cam, tilt_fix_deg=fix_deg)))
    return out


def _picture(f: Frame, cal: Calibration) -> NDArray[np.uint8] | None:
    rgb = decode(f)
    if rgb is not None and (rgb.shape[1], rgb.shape[0]) != cal.size:
        raise ValueError(
            f"the pictures are {rgb.shape[1]} x {rgb.shape[0]},"
            f" the calibration is {cal.size[0]} x {cal.size[1]}"
            " - record at VGA"
        )
    return rgb


def _sonar(mdir: Path) -> list[int | None]:
    """The sonar over the scored window (from 0.5 s after the start)."""
    t0: float | None = None
    out: list[int | None] = []
    for e in replay(mdir):
        t0 = e.recv_s if t0 is None else t0
        if e.text is not None and e.text.startswith("{") and e.recv_s >= t0 + SETTLE_S:
            s = parse_td(json.loads(e.text))
            if s is not None:
                out.append(s.dist_mm)
    return out


def _score_one(
    mdir: Path,
    mark: Mark,
    cal: Calibration,
    detectors: dict[str, DetectFn],
    seeing: Seeing | None,
    depth: DepthFn | None,
) -> dict[str, list[PersonRange | None]]:
    """Per detector: the picked person on each scored picture (still: 10 spread over the window)."""
    kept = kept_frames(mdir, cal, seeing, depth)
    chosen = [kept[i] for i in spread(len(kept), STILL_PICTURES)] if mark.still else kept
    picks: dict[str, list[PersonRange | None]] = {name: [] for name in detectors}
    for f, cam in chosen:
        rgb = _picture(f, cal)
        if rgb is None:
            continue
        for name, det in detectors.items():
            boxes = [d.box for d in det(rgb, cam) if d.cls == PERSON]
            picks[name].append(pick_person([person_range(cam, b) for b in boxes]))
    return picks


def score_folder(
    folder: Path,
    cal: Calibration,
    detectors: dict[str, DetectFn],
    depth: DepthFn | None,
    make_seeing: Callable[[], Seeing] | None = None,
    unlock: Unlock | None = None,
) -> str:
    """Every recorded mark, per detector, each mark with its own tilt fix (a fresh Seeing: P0 review I1); the
    report also written to folder/report.txt. unlock: write or remove the seeing pass (§28.6)."""
    per: dict[str, list[MarkScore]] = {name: [] for name in detectors}
    for mdir in sorted(p.parent for p in folder.glob("*/mark.json")):
        mark, complete = _mark_of(mdir / "mark.json")
        seeing = make_seeing() if make_seeing is not None and depth is not None else None
        sonar = _sonar(mdir)
        shown = mark if complete else replace(mark, mid=f"{mark.mid} (cut short)")
        for name, pics in _score_one(mdir, mark, cal, detectors, seeing, depth).items():
            per[name].append(score_mark(shown, pics, sonar))
    text = "\n\n".join(verdict(name, per[name]) for name in detectors)
    if unlock is not None:
        text += "\n\n" + _unlock(unlock, per.get(unlock.detector, []))
    (folder / "report.txt").write_text(text + "\n")
    return text


def _unlock(u: Unlock, scores: list[MarkScore]) -> str:
    """Every required mark recorded whole (a cut-short one is named differently) and passed: the pass file;
    else none (an old one removed)."""
    passed = {s.mark.mid for s in scores if s.passed}
    if scores and all(s.passed for s in scores) and set(u.required) <= passed:
        today = datetime.date.today().isoformat()
        write_pass(u.path, u.model, u.calibration, today, {"marks_passed": len(passed), "marks": len(scores)})
        return f"games unlocked: {u.detector} passed every mark ({u.path.name} written)"
    u.path.unlink(missing_ok=True)
    return f"games locked: {u.detector} did not pass every mark ({u.path.name} removed)"


def world_label(world_vocab: Path) -> str:
    return f"yolo-world ({world_vocab.name})"


def _detectors(world_model: Path, world_vocab: Path) -> dict[str, DetectFn]:
    from conqueror_ai.adapters.detector_config import load_vocabulary
    from conqueror_ai.adapters.yolo_coreml import YoloCoreml
    from conqueror_ai.adapters.yolo_world import YoloWorld

    coco = YoloCoreml(MODELS / "yolo26n-640.mlpackage", COCO_CONF)
    world = YoloWorld(world_model, load_vocabulary(world_vocab), MAYBE_SCORE)  # the engine's maybe score
    return {"yolo26n person": coco.detect, world_label(world_vocab): world.detect}


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["record", "score"])
    ap.add_argument("folder", type=Path)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--world-model", type=Path, default=MODEL)
    ap.add_argument("--world-vocabulary", type=Path, default=VOCABULARY)
    ap.add_argument("--no-depth", action="store_true", help="score without the depth floor's tilt fix")
    a = ap.parse_args()
    if a.what == "record":
        done = asyncio.run(record_marks(a.host, 80, 81, a.folder, all_marks(), input))
        print(f"recorded {len(done)} marks into {a.folder}")
        return
    from conqueror_ai.adapters.map_config import load_map_config

    cal = calibration_store.load(CALIBRATION, VGA)
    depth = None if a.no_depth else load_depth(DEPTH_MODEL)
    cfg = load_map_config(MAP_CONFIG)
    marks = tuple(m.mid for m in all_marks())
    unlock = Unlock(world_label(a.world_vocabulary), PASS_FILE, a.world_model.name, cal.date, marks)
    dets = _detectors(a.world_model, a.world_vocabulary)
    print(score_folder(a.folder, cal, dets, depth, lambda: Seeing(cfg), unlock))
