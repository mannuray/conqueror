"""`cq-drive`: module 2, drive while thinking (AI engine LLD §17, spec 8). Slice 1: chase the ball.

    uv run cq-drive goto ball                 # find the ball and drive to it in one smooth move; Ctrl-C stops
    uv run cq-drive goto ball --record recordings/chase-1

It drives like the phone does (it takes control with its first command); STOP from any device ends it.
The Uno's safety rules all stay: the obstacle brake, deadman, link loss, stall, battery and the child-play
limits.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import datetime
import time
from pathlib import Path

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.detector_config import load_balls
from conqueror_ai.adapters.drive_config import load_drive
from conqueror_ai.adapters.drive_link import DriveLink
from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.app.objects import (
    BALLS,
    CALIBRATION,
    DEFAULT_HOST,
    MODEL,
    VGA,
    VOCABULARY,
    camera_of,
    decode,
    detectors,
)
from conqueror_ai.domain.drive.chase import DRIVE_TILT, ChaseSkill
from conqueror_ai.domain.drive.goal import Goal, size_position
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.objects import ObjectPipeline
from conqueror_ai.domain.types import TelemSample

DRIVE = Path(__file__).resolve().parents[3] / "config" / "drive.yaml"
TICK_S = 0.2  # tick at least this often, with or without a new picture
STALE_PICTURE_S = 0.5  # spec 7 §4: no driving on a stale map
STALE_TELEM_S = 0.2


async def goto_ball(host: str, rec: SessionRecorder | None) -> str:
    """Chase the ball; returns the result ("done" or why not)."""
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource

    cal = calibration_store.load(CALIBRATION, VGA)
    det, floor = detectors(MODEL, VOCABULARY, BALLS)
    sizes = {f"ball:{b.name}": b.size_cm for b in load_balls(BALLS) if b.size_cm is not None}
    lim, table = load_drive(DRIVE)
    link, cam, drive = BridgeLink(host), MjpegSource(host), DriveLink(host, table=table)
    p = ObjectPipeline(cal, floor)
    skill, goal = ChaseSkill(Planner(lim)), Goal()
    stop = asyncio.Event()
    last: dict[str, float] = {"telem": 0.0, "picture": 0.0}
    sonar: list[float | None] = [None]
    if rec is not None:
        link.tap = rec.telemetry
    tasks = [asyncio.create_task(t) for t in (link.run(stop), cam.run(stop), drive.run(stop))]

    async def telemetry() -> None:
        calib_given = False
        while not stop.is_set():
            s: TelemSample = await link.samples.get()
            if not calib_given and link.counts_per_m is not None:
                p.on_calib(link.counts_per_m)
                calib_given = True
            p.on_telem(s)
            last["telem"] = time.monotonic()
            sonar[0] = None if s.dist_mm is None or s.dist_mm <= 0 else s.dist_mm / 1000.0

    tasks.append(asyncio.create_task(telemetry()))
    for _ in range(100):  # both links up before the first move
        if link.connected and drive.connected and last["telem"] > 0:
            break
        await asyncio.sleep(0.1)
    else:
        stop.set()
        return "no connection to the tank - is it on?"
    await drive.cam(0.0, DRIVE_TILT)
    t0 = time.monotonic()
    result = ""
    by_size: dict[str, tuple[float, float]] = {}
    try:
        while not result:
            if drive.refused is not None:
                result = f"refused by the tank ({drive.refused})"
                break
            with contextlib.suppress(TimeoutError):
                f = await asyncio.wait_for(cam.next_frame(), TICK_S)
                rgb = decode(f)
                if rgb is not None:
                    if rec is not None:
                        rec.frame(f)
                    cam_f = p.camera_at(f.cap_us)
                    dets = await asyncio.to_thread(det.detect, rgb, camera_of(cam_f))
                    used = p.on_frame(f, (rgb.shape[1], rgb.shape[0]), dets, cam_f)
                    last["picture"] = time.monotonic()
                    by_size = {}
                    if not isinstance(used, str):
                        for d in dets:
                            size = sizes.get(d.cls)
                            pos = None if size is None else size_position(d, size, used)
                            if pos is not None:
                                by_size[d.cls] = pos
            now = time.monotonic()
            pose = p.pose()
            at = None if pose is None else (pose.x_m, pose.y_m, pose.th_rad)
            fix = None if pose is None else goal.update(p.objects(), (pose.x_m, pose.y_m), pose.t_us, by_size)
            fresh = now - last["picture"] <= STALE_PICTURE_S and now - last["telem"] <= STALE_TELEM_S
            out = skill.tick(now - t0, at, fix, sonar[0], fresh)
            await drive.drive(out.cmd)
            if out.cam is not None:
                await drive.cam(*out.cam)
            if out.result is not None:
                result = out.result
            print(
                f"\r{skill.state:12s} v {out.cmd.v_cms:4.1f} cm/s  w {out.cmd.w_dps:6.1f} deg/s ",
                end="",
                flush=True,
            )
    finally:
        await drive.finish()
        stop.set()
        await link.close()
        for t in tasks:
            t.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await asyncio.gather(*tasks, return_exceptions=True)
        print()
    return result


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["goto"])
    ap.add_argument("thing", choices=["ball"])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--record", type=Path)
    a = ap.parse_args()
    rec = None
    if a.record is not None:
        rec = SessionRecorder(
            a.record,
            {
                "host": a.host,
                "what": f"cq-drive {a.action} {a.thing}",
                "date": datetime.datetime.now().isoformat(timespec="seconds"),
            },
        )
    try:
        print(f"result: {asyncio.run(goto_ball(a.host, rec))}")
    except KeyboardInterrupt:
        print("\nstopped (Ctrl-C): the tank was told to stop")
    except (FileNotFoundError, ValueError) as err:
        raise SystemExit(str(err)) from None
    finally:
        if rec is not None:
            rec.close()
