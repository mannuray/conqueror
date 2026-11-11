"""The simple seeing test (design-notes §234): one crawl straight ahead, the sonar as the ruler.

    uv run cq-person-test walk recordings/walk-20261110       # the laptop speaks the steps; ~50 s
    uv run cq-person-test walk-score recordings/walk-20261110

`walk`: the camera goes to the driving tilt (only the camera moves; nothing drives), then the laptop says
when to start, crawl in and crawl back while it records. `walk-score`: each picture with the sonar's
reading at that moment, in bands by the sonar's distance, P0's bar in each (`score_walk`); every band
passing writes the seeing pass (AI engine LLD §28.6), anything less removes it. Pictures stay here."""

from __future__ import annotations

import asyncio
import contextlib
import datetime
import json
import shutil
import subprocess
from collections.abc import Callable, Sequence
from pathlib import Path

from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.adapters.session import SessionRecorder, replay
from conqueror_ai.app.objects import DepthFn
from conqueror_ai.app.person_test import PERSON, DetectFn, Unlock, cam_tilt, kept_frames, picture_of
from conqueror_ai.app.record import NoView, record
from conqueror_ai.app.seeing_pass import write_pass
from conqueror_ai.domain.camera import Calibration
from conqueror_ai.domain.depth import Seeing
from conqueror_ai.domain.person import nearest_person, person_range
from conqueror_ai.domain.person_test import PersonRange, score_walk, walk_verdict
from conqueror_ai.domain.telemetry import parse_td

DRIVE_TILT_DEG = -15.0  # the camera's tilt while driving: the game's (design-notes §125)
# (seconds from the start, what to say): ~2 m out, in to a hand's width, a pause, back out
STEPS: tuple[tuple[float, str], ...] = (
    (0.0, "Get down about two metres straight in front of the tank, facing it."),
    (8.0, "Now crawl slowly towards the tank, straight at it."),
    (26.0, "Stop a hand's width from the tank. Stay still."),
    (31.0, "Now crawl slowly backwards, still facing the tank, back to where you started."),
    (49.0, "Done. Thank you."),
)
RECORD_S = 50.0


def speak(text: str) -> None:
    """The laptop says it (macOS `say`) and prints it; never waits for the voice to finish."""
    print(text, flush=True)
    if shutil.which("say"):
        with contextlib.suppress(OSError):
            subprocess.Popen(["say", text])


async def _say_steps(steps: Sequence[tuple[float, str]], say: Callable[[str], None]) -> None:
    t = 0.0
    for at, text in steps:
        await asyncio.sleep(max(0.0, at - t))
        t = at
        say(text)


async def walk_record(
    host: str, ws_port: int, stream_port: int, folder: Path, say: Callable[[str], None]
) -> None:
    """The camera to the driving tilt, then one recording while the steps are spoken."""
    say("The camera moves down a little. Nothing drives.")
    await cam_tilt(host, ws_port, DRIVE_TILT_DEG)
    await asyncio.sleep(1.0)
    rec = SessionRecorder(folder, {"what": "cq-person-test walk", "tilt_deg": DRIVE_TILT_DEG})
    link = BridgeLink(host, ws_port, client="p0-walk")
    cam = MjpegSource(host, stream_port)
    speaker = asyncio.create_task(_say_steps(STEPS, say))
    try:
        await record(link, cam, rec, NoView(), seconds=RECORD_S)
    finally:
        await speaker


def _sonar_times(folder: Path) -> list[tuple[float, float | None]]:
    """(when, the sonar in m or None for no echo) for every telemetry sample, in order."""
    out: list[tuple[float, float | None]] = []
    for e in replay(folder):
        if e.text is None or not e.text.startswith("{"):
            continue
        s = parse_td(json.loads(e.text))
        if s is not None:
            out.append((e.recv_s, None if s.dist_mm is None or s.dist_mm <= 0 else s.dist_mm / 1000.0))
    return out


def walk_samples(
    folder: Path, cal: Calibration, detect: DetectFn, seeing: Seeing | None, depth: DepthFn | None
) -> list[tuple[float | None, PersonRange | None]]:
    """Each scored picture: the sonar's latest reading before it, and the game's person in it."""
    sonar = _sonar_times(folder)
    out: list[tuple[float | None, PersonRange | None]] = []
    k = -1
    for f, cam in kept_frames(folder, cal, seeing, depth):
        while k + 1 < len(sonar) and sonar[k + 1][0] <= f.recv_s:
            k += 1
        rgb = picture_of(f, cal)
        if rgb is None:
            continue
        boxes = [d.box for d in detect(rgb, cam) if d.cls == PERSON]
        out.append((sonar[k][1] if k >= 0 else None, nearest_person([person_range(cam, b) for b in boxes])))
    return out


def walk_score_folder(
    folder: Path,
    cal: Calibration,
    detect: DetectFn,
    seeing: Seeing | None,
    depth: DepthFn | None,
    unlock: Unlock | None,
    name: str,
) -> str:
    """The bands' report (also folder/walk-report.txt); all passing writes the pass, else it is removed."""
    scores = score_walk(walk_samples(folder, cal, detect, seeing, depth))
    text = walk_verdict(name, scores)
    if unlock is not None:
        if all(s.passed for s in scores):
            today = datetime.date.today().isoformat()
            numbers = {"test": "walk", "bands_passed": len(scores)}
            write_pass(unlock.path, unlock.model, unlock.calibration, today, numbers)
            text += f"\n\ngames unlocked: every band passed ({unlock.path.name} written)"
        else:
            unlock.path.unlink(missing_ok=True)
            text += f"\n\ngames locked: not every band passed ({unlock.path.name} removed)"
    (folder / "walk-report.txt").write_text(text + "\n")
    return text
