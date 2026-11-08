"""`cq-person-test`: can the tank see a crawling person? (Spec 10 phase 0, Plan P0.) Measurement only.

`record` guides you mark by mark: tape from the tank's front edge at 0.3 / 0.5 / 1 / 1.5 / 2 m straight ahead
and on a 30-degree line to the right; crawl to each mark, hands on the tape, still, then slowly across it.
Nothing drives: only the camera moves, to level and then to the driving tilt, and it says so first. `score`
replays a recording through the detectors (never the tracker, the memory or the map), turns each person box
into a distance from the front edge and a direction, and grades every mark against the owner's bar.

    uv run cq-person-test record recordings/p0-20261109 [--host 192.168.31.167]
    uv run cq-person-test score recordings/p0-20261109 [--world-model M --world-vocabulary V] [--no-depth]

The pictures stay on this machine (recordings/ is git-ignored); the report is numbers only.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
from collections.abc import Callable
from dataclasses import asdict, replace
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
from conqueror_ai.domain.camera import Calibration, CameraModel, from_servo
from conqueror_ai.domain.depth import Seeing
from conqueror_ai.domain.person_test import Mark, PersonRange, person_range, pick_person, score_mark, verdict
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import Detection, Pose

DEFAULT_HOST = "192.168.31.167"
DetectFn = Callable[[NDArray[np.uint8], CameraModel | None], list[Detection]]
STILL_PICTURES = 10
SETTLE_S = 0.5  # pictures this soon after the start or a camera move are not scored
PERSON = "person"


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
    how = "stay still" if mark.still else "crawl slowly across the mark"
    return f"Crawl to {mark.tape_m:.1f} m {where}, hands on the tape, {how} - Enter to record, s to skip: "


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
) -> list[str]:
    """Each mark into folder/<mark id>/ (cq-record's files and mark.json). Returns the recorded marks' ids."""
    done: list[str] = []
    tilt: float | None = None
    for m in marks:
        if m.tilt_deg != tilt:
            await asyncio.to_thread(
                ask, f"The camera goes to {m.tilt_deg:.0f} degrees (only the camera moves) - Enter: "
            )
            await _cam_tilt(host, ws_port, m.tilt_deg)
            tilt = m.tilt_deg
        if (await asyncio.to_thread(ask, _say(m))).strip().lower() == "s":
            continue
        rec = SessionRecorder(folder / m.mid, {"what": "cq-person-test", "mark": m.mid})
        (folder / m.mid / "mark.json").write_text(json.dumps(asdict(m)))
        link = BridgeLink(host, ws_port, client="p0-record")
        cam = MjpegSource(host, stream_port)
        await record(link, cam, rec, NoView(), seconds=SETTLE_S + (still_s if m.still else crawl_s))
        done.append(m.mid)
    return done


def _mark_of(path: Path) -> Mark:
    d = json.loads(path.read_text())
    return Mark(
        str(d["mid"]), float(d["tape_m"]), float(d["angle_deg"]), bool(d["still"]), float(d["tilt_deg"])
    )


def _score_one(
    mdir: Path, cal: Calibration, detectors: dict[str, DetectFn], seeing: Seeing | None, depth: DepthFn | None
) -> dict[str, tuple[list[PersonRange | None], list[int | None]]]:
    """Per detector: the picked person on each scored picture, and the sonar over the scored time."""
    mark = _mark_of(mdir / "mark.json")
    still0 = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)  # the tank stays on its mark
    servo: tuple[float, float] | None = None
    t0: float | None = None
    moved_at = -1e9
    fix_deg = 0.0
    sonar: list[int | None] = []
    picks: dict[str, list[PersonRange | None]] = {name: [] for name in detectors}
    for e in replay(mdir):
        t0 = e.recv_s if t0 is None else t0
        if e.text is not None:
            s = parse_td(json.loads(e.text)) if e.text.startswith("{") else None
            if s is not None:
                if servo is not None and (s.pan_deg, s.tilt_deg) != servo:
                    moved_at = e.recv_s
                servo = (s.pan_deg, s.tilt_deg)
                if e.recv_s >= t0 + SETTLE_S:
                    sonar.append(s.dist_mm)
            continue
        f = e.frame
        if f is None or servo is None or e.recv_s < max(t0 + SETTLE_S, moved_at + SETTLE_S):
            continue
        if mark.still and all(len(p) >= STILL_PICTURES for p in picks.values()):
            continue
        rgb = decode(f)
        if rgb is None:
            continue
        cam = CameraModel(cal).for_frame(still0, *from_servo(*servo))
        if seeing is not None and depth is not None:  # the engine's own tilt fix from the depth floor
            fix_deg = seeing.angle_only(cam, depth(rgb), f.cap_us).fix.tilt_fix_deg
        cam = replace(cam, tilt_fix_deg=fix_deg)
        for name, det in detectors.items():
            boxes = [d.box for d in det(rgb, cam) if d.cls == PERSON]
            picks[name].append(pick_person([person_range(cam, b) for b in boxes]))
    return {name: (p, sonar) for name, p in picks.items()}


def score_folder(
    folder: Path,
    cal: Calibration,
    detectors: dict[str, DetectFn],
    depth: DepthFn | None,
    seeing: Seeing | None = None,
) -> str:
    """Every recorded mark, per detector; the report also written to folder/report.txt."""
    marks = sorted(p.parent for p in folder.glob("*/mark.json"))
    per: dict[str, list[object]] = {name: [] for name in detectors}
    for mdir in marks:
        mark = _mark_of(mdir / "mark.json")
        for name, (pics, sonar) in _score_one(mdir, cal, detectors, seeing, depth).items():
            per[name].append(score_mark(mark, pics, sonar))
    text = "\n\n".join(verdict(name, per[name]) for name in detectors)  # type: ignore[arg-type]
    (folder / "report.txt").write_text(text + "\n")
    return text


def _detectors(world_model: Path, world_vocab: Path) -> dict[str, DetectFn]:
    from conqueror_ai.adapters.detector_config import load_vocabulary
    from conqueror_ai.adapters.yolo_coreml import YoloCoreml
    from conqueror_ai.adapters.yolo_world import YoloWorld

    coco = YoloCoreml(MODELS / "yolo26n-640.mlpackage")
    world = YoloWorld(world_model, load_vocabulary(world_vocab), 0.05)
    return {"yolo26n person": coco.detect, f"yolo-world ({world_vocab.name})": world.detect}


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
    seeing = None if depth is None else Seeing(load_map_config(MAP_CONFIG))
    print(score_folder(a.folder, cal, _detectors(a.world_model, a.world_vocabulary), depth, seeing))
