"""`cq-drive`: module 2, drive while thinking (AI engine LLD §17-§18, specs 8-9), on the room map.

    uv run cq-drive goto ball                 # find the ball and drive to it, round obstacles; Ctrl-C stops
    uv run cq-drive goto bottle               # any word in config/vocabulary.yaml that stands on the floor
    uv run cq-drive wander 120                # drive about for 2 minutes (no number: until stopped)
    uv run cq-drive explore                   # visit everything, then stop (needs the depth model)
    uv run cq-drive find ball                 # wander until the ball is seen, then go to it
    uv run cq-drive wander --record recordings/wander-1

A ball's distance comes from its size in the picture; any other thing's from where it meets the floor (the
map's floor point, which depends on the camera's tilt). The room map (Plan Z4) fills from the depth picture
and the distance sensor; routes go round what it holds.

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
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Literal

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.detector_config import load_balls, load_vocabulary
from conqueror_ai.adapters.drive_config import load_drive
from conqueror_ai.adapters.drive_link import DriveLink
from conqueror_ai.adapters.map_config import load_map_config
from conqueror_ai.adapters.session import SessionRecorder

if TYPE_CHECKING:
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.app.map import (
    FIRST_PICTURE_S,
    STALE_PICTURE_S,
    STALE_TELEM_S,
    MapRun,
    map_state,
    task_failure,
)
from conqueror_ai.app.objects import (
    BALLS,
    CALIBRATION,
    DEFAULT_HOST,
    DEPTH_MODEL,
    MAP_CONFIG,
    MODEL,
    VGA,
    VOCABULARY,
    DepthFn,
    camera_of,
    decode,
    detectors,
    load_depth,
    see_off_loop,
)
from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.depth import MapConfig
from conqueror_ai.domain.drive.chase import DRIVE_TILT, ChaseSkill, SkillOut
from conqueror_ai.domain.drive.goal import Goal, TargetFix, is_ball, kind_of, size_positions
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.roam import RoamSkill
from conqueror_ai.domain.objects import Detector, ObjectPipeline
from conqueror_ai.domain.room_map import MapSnapshot
from conqueror_ai.domain.types import TelemSample
from conqueror_ai.domain.vocabulary import Vocabulary

__all__ = [
    "FIRST_PICTURE_S",
    "STALE_PICTURE_S",
    "STALE_TELEM_S",
    "map_state",
]  # the stale rule lives in app.map

DRIVE = Path(__file__).resolve().parents[3] / "config" / "drive.yaml"
TICK_S = 0.2  # tick at least this often, with or without a new picture
NEVER_GOTO = ("person",)
SKIPS_THAT_COUNT = ("turning", "camera moving")  # the map skips these pictures, but the camera is alive


def goal_words(vocabulary: Vocabulary) -> list[str]:
    """What cq-drive can go to: "ball" and every word that stands on the floor (it has a place on the map) -
    never a person: a child is around (following a person is a later lesson with its own rules)."""
    return ["ball", *(w.word for w in vocabulary.words if w.floor and w.word not in NEVER_GOTO)]


def picture_counts(used: CameraModel | str) -> bool:
    """A picture shows the camera is alive when the map used it, or skipped it only for the tank's or the
    camera's own motion (a spin, a camera sweep)."""
    return not isinstance(used, str) or used in SKIPS_THAT_COUNT


Action = Literal["goto", "find", "wander", "explore"]
ACTIONS: tuple[Action, ...] = ("goto", "find", "wander", "explore")


@dataclass(frozen=True)
class Mission:
    action: Action
    thing: str | None  # goto / find
    seconds: float | None  # wander: how long (None: until stopped)


def parse_mission(args: list[str], words: list[str]) -> Mission:
    """`goto <thing>`, `find <thing>`, `wander [seconds]`, `explore`; anything else is a plain SystemExit."""
    if not args or args[0] not in ACTIONS:
        raise SystemExit(f"cq-drive does: {', '.join(ACTIONS)}")
    action = args[0]
    rest = args[1:]
    if action in ("goto", "find"):
        if len(rest) != 1 or rest[0] not in words:
            raise SystemExit(f"cq-drive {action} needs one of: {', '.join(words)}")
        return Mission(action, rest[0], None)
    if action == "explore":
        if rest:
            raise SystemExit("cq-drive explore takes nothing more")
        return Mission(action, None, None)
    if len(rest) > 1:
        raise SystemExit("cq-drive wander takes at most a number of seconds")
    try:
        seconds = float(rest[0]) if rest else None
    except ValueError:
        raise SystemExit(f"cq-drive wander: {rest[0]!r} is not a number of seconds") from None
    if seconds is not None and seconds <= 0:
        raise SystemExit("cq-drive wander: the seconds must be more than 0")
    return Mission(action, None, seconds)


def refusal(m: Mission, has_depth: bool) -> str | None:
    """Why the mission can't start, or None (spec 9 §5.4: without depth only the sensor marks the map)."""
    if m.action == "explore" and not has_depth:
        return (
            "explore needs the depth model (models/DepthAnythingV2SmallF16.mlpackage): "
            "without it the map has only the distance sensor"
        )
    return None


def make_skill(m: Mission, lim: Limits) -> ChaseSkill | RoamSkill:
    planner = Planner(lim)
    if m.action == "goto":
        return ChaseSkill(planner)
    if m.action == "find":
        return RoamSkill(planner, "wander", find=True)
    return RoamSkill(planner, m.action, seconds=m.seconds)


@dataclass
class Kit:
    """What a run needs besides the links: calibration, settings, the detector, the depth model."""

    cal: Calibration
    cfg: MapConfig
    det: Detector
    floor: frozenset[str]
    sizes: dict[str, float]
    lim: Limits
    depth: DepthFn | None
    rec: SessionRecorder | None = None


async def drive_session(link: BridgeLink, cam: MjpegSource, drive: DriveLink, m: Mission, kit: Kit) -> str:
    """One run on the tank; returns the result ("done" or why not). The links are handed in (tests aim them at
    a fake bridge)."""
    run = MapRun(kit.cal, kit.cfg, kit.depth, p=ObjectPipeline(kit.cal, kit.floor))
    skill = make_skill(m, kit.lim)
    goal = Goal(kind_of(m.thing)) if m.thing is not None else Goal(is_ball)
    stop = asyncio.Event()
    last: dict[str, float] = {"telem": 0.0, "picture": 0.0}
    sonar: list[float | None] = [None]
    rec = kit.rec
    if rec is not None:
        link.tap = rec.telemetry
    tasks = [asyncio.create_task(t) for t in (link.run(stop), cam.run(stop), drive.run(stop))]

    async def telemetry() -> None:
        calib_given = False
        while not stop.is_set():
            s: TelemSample = await link.samples.get()
            if not calib_given and link.counts_per_m is not None:
                run.on_calib(link.counts_per_m)
                calib_given = True
            run.on_telem(s)  # the pose, and the sensor onto the map
            last["telem"] = time.monotonic()
            sonar[0] = None if s.dist_mm is None or s.dist_mm <= 0 else s.dist_mm / 1000.0

    tasks.append(asyncio.create_task(telemetry()))
    for _ in range(100):  # both links up before the first move
        if link.connected and drive.connected and last["telem"] > 0:
            break
        await asyncio.sleep(0.1)
    else:
        stop.set()
        await _close(link, tasks)
        return "no connection to the tank - is it on?"
    await drive.cam(0.0, DRIVE_TILT)
    t0 = time.monotonic()
    result = ""
    by_size: dict[str, tuple[float, float]] = {}
    cam_still = True  # the newest picture wasn't skipped for the camera moving (the sweep looks only then)
    try:
        while not result:
            if drive.refused is not None:
                result = f"refused by the tank ({drive.refused})"
                break
            if (failed := task_failure(tasks)) is not None:
                result = f"failed - {failed}"
                break
            with contextlib.suppress(TimeoutError):
                f = await asyncio.wait_for(cam.next_frame(), TICK_S)
                rgb = await asyncio.to_thread(decode, f)
                if rgb is not None:
                    if rec is not None:
                        rec.frame(f)
                    cam_f = run.p.camera_at(f.cap_us)
                    want = kit.depth is not None and not isinstance(cam_f, str) and run.depth_due_next()
                    if want and kit.depth is not None:
                        dets, inv = await asyncio.gather(
                            asyncio.to_thread(kit.det.detect, rgb, camera_of(cam_f)),
                            asyncio.to_thread(kit.depth, rgb),
                        )
                    else:
                        dets, inv = await asyncio.to_thread(kit.det.detect, rgb, camera_of(cam_f)), None
                    used = run.gate(f, rgb, dets, cam_f)  # the map and the pose stay on the event loop
                    cam_still = used != "camera moving"
                    if picture_counts(used):
                        last["picture"] = time.monotonic()
                    by_size = {} if isinstance(used, str) else size_positions(dets, kit.sizes, used)
                    if not isinstance(used, str) and inv is not None:  # seeing off the loop, the map on it
                        run.apply(await see_off_loop(run.seeing, run.p, used, inv, f.cap_us), f.cap_us)
            now = time.monotonic()
            state = map_state(now, t0, last["picture"], last["telem"])
            if state == "waiting":
                continue
            pose = run.p.pose()
            at = None if pose is None else (pose.x_m, pose.y_m, pose.th_rad)
            fix = (
                None
                if pose is None
                else goal.update(run.p.objects(), (pose.x_m, pose.y_m), pose.t_us, by_size)
            )
            snap = None if pose is None else run.snapshot(pose.t_us, state is not None)  # stale: now
            out = _tick(skill, m, now - t0, at, snap, fix, sonar[0], state is None, cam_still)
            await drive.drive(out.cmd)
            if out.cam is not None:
                await drive.cam(*out.cam)
            if out.result is not None:
                result = out.result
                if result == "stale":
                    result = f"stale - {state or 'no pose'}"
            seen_m2 = float(run.room.seen.sum()) * run.room.cell_m**2
            print(
                f"\r{skill.state:12s} v {out.cmd.v_cms:4.1f} cm/s  w {out.cmd.w_dps:6.1f} deg/s  "
                f"seen {seen_m2:5.1f} m2 ",
                end="",
                flush=True,
            )
    finally:
        try:
            await drive.finish()
        except Exception as e:  # the clean-up goes on: the bridge stops the tank when the link drops
            print(f"\nthe stop message failed ({e}): the tank stops by the deadman")
        stop.set()
        await _close(link, tasks)
        print()
    return result


def _tick(
    skill: ChaseSkill | RoamSkill,
    m: Mission,
    now_s: float,
    at: tuple[float, float, float] | None,
    snap: MapSnapshot | None,
    fix: TargetFix | None,
    sonar_m: float | None,
    map_ok: bool,
    cam_still: bool,
) -> SkillOut:
    if isinstance(skill, ChaseSkill):
        return skill.tick(now_s, at, fix, sonar_m, map_ok, cam_still, snap)
    return skill.tick(now_s, at, snap, fix if m.action == "find" else None, sonar_m, map_ok, cam_still)


async def _close(link: BridgeLink, tasks: list[asyncio.Task[None]]) -> None:
    await link.close()
    for t in tasks:
        t.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await asyncio.gather(*tasks, return_exceptions=True)


async def run_drive(host: str, m: Mission, rec: SessionRecorder | None, use_depth: bool = True) -> str:
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource

    cal = calibration_store.load(CALIBRATION, VGA)
    det, floor = detectors(MODEL, VOCABULARY, BALLS)
    sizes = {f"ball:{b.name}": b.size_cm for b in load_balls(BALLS) if b.size_cm is not None}
    lim, table = load_drive(DRIVE)
    depth = load_depth(DEPTH_MODEL) if use_depth else None
    if (why := refusal(m, depth is not None)) is not None:
        return f"refused - {why}"
    kit = Kit(cal, load_map_config(MAP_CONFIG), det, floor, sizes, lim, depth, rec)
    return await drive_session(BridgeLink(host), MjpegSource(host), DriveLink(host, table=table), m, kit)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", nargs="+", help="goto <thing> | find <thing> | wander [seconds] | explore")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--record", type=Path)
    ap.add_argument(
        "--no-depth", action="store_true", help="leave the depth model out (the servo's tilt only)"
    )
    a = ap.parse_args()
    try:
        words = goal_words(load_vocabulary(VOCABULARY))
    except (FileNotFoundError, ValueError) as err:
        raise SystemExit(str(err)) from None
    m = parse_mission(a.what, words)
    if m.action == "explore" and a.no_depth:
        raise SystemExit(refusal(m, False))
    rec = None
    if a.record is not None:
        rec = SessionRecorder(
            a.record,
            {
                "host": a.host,
                "what": "cq-drive " + " ".join(a.what),
                "date": datetime.datetime.now().isoformat(timespec="seconds"),
            },
        )
    try:
        print(f"result: {asyncio.run(run_drive(a.host, m, rec, not a.no_depth))}")
    except KeyboardInterrupt:
        print("\nstopped (Ctrl-C): the tank was told to stop")
    except (FileNotFoundError, ValueError) as err:
        raise SystemExit(str(err)) from None
    finally:
        if rec is not None:
            rec.close()


if __name__ == "__main__":
    main()
