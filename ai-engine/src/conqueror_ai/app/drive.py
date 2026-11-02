"""`cq-drive`: module 2, drive while thinking (AI engine LLD §17-§18, specs 8-9), on the room map.

    uv run cq-drive find ball                 # look round, search the room, drive to it; Ctrl-C stops
    uv run cq-drive find bottle --area 2      # any word in config/vocabulary.yaml that stands on the floor
    uv run cq-drive wander 120                # drive about for 2 minutes (no number: until stopped)
    uv run cq-drive explore                   # visit everything, then stop (needs the depth model)
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
import datetime
import time
from dataclasses import dataclass, replace
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
    detectors,
    load_depth,
)
from conqueror_ai.app.perception import (
    FIRST_PICTURE_S,
    STALE_PICTURE_S,
    STALE_TELEM_S,
    Perception,
    map_state,
)
from conqueror_ai.app.world import World, maybes_of
from conqueror_ai.domain.camera import Calibration
from conqueror_ai.domain.depth import MapConfig
from conqueror_ai.domain.drive.costmap import CostSettings
from conqueror_ai.domain.drive.goal import is_ball, kind_of
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.roam import RoamSkill
from conqueror_ai.domain.memory import ThingMemory
from conqueror_ai.domain.objects import Detector
from conqueror_ai.domain.vocabulary import Vocabulary

__all__ = [
    "FIRST_PICTURE_S",
    "STALE_PICTURE_S",
    "STALE_TELEM_S",
    "map_state",
    "maybes_of",
]  # the stale rule lives in app.map

DRIVE = Path(__file__).resolve().parents[3] / "config" / "drive.yaml"
TICK_S = 0.2  # tick at least this often, with or without a new picture
NEVER_GOTO = ("person",)


def goal_words(vocabulary: Vocabulary) -> list[str]:
    """What cq-drive can go to: "ball" and every word that stands on the floor (it has a place on the map) -
    never a person: a child is around (following a person is a later lesson with its own rules)."""
    return ["ball", *(w.word for w in vocabulary.words if w.floor and w.word not in NEVER_GOTO)]


Action = Literal["find", "wander", "explore"]
ACTIONS: tuple[Action, ...] = ("find", "wander", "explore")


@dataclass(frozen=True)
class Mission:
    action: Action
    thing: str | None  # find
    seconds: float | None  # wander: how long (None: until stopped)
    area_m: float | None = None  # stay this near the start (None: anywhere, §174)


def parse_mission(args: list[str], words: list[str], area_m: float | None = None) -> Mission:
    """`find <thing>`, `wander [seconds]`, `explore`, each with an optional play area (`--area` metres);
    anything else is a plain SystemExit."""
    if not args or args[0] not in ACTIONS:
        raise SystemExit(f"cq-drive does: {', '.join(ACTIONS)}")
    action = args[0]
    if area_m is not None and not area_m > 0:
        raise SystemExit("cq-drive --area: metres more than 0")
    return replace(_parse(action, args[1:], words), area_m=area_m)


def _parse(action: Action, rest: list[str], words: list[str]) -> Mission:
    if action == "find":
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
    """Why the mission can't start, or None (spec 9 §5.4: without depth only the sensor marks the map -
    explore has nothing to go to, and find's search only seen floor, so it would end at once, §188)."""
    if m.action in ("explore", "find") and not has_depth:
        return (
            f"{m.action} needs the depth model (models/DepthAnythingV2SmallF16.mlpackage): "
            "without it the map has only the distance sensor"
        )
    return None


def make_skill(
    m: Mission, lim: Limits, cost: CostSettings | None = None, memory: ThingMemory | None = None
) -> RoamSkill:
    planner = Planner(lim)
    if m.action == "find":  # remembered spots, the look round, candidates, then the search (§189)
        kind = is_ball if m.thing is None else kind_of(m.thing)
        return RoamSkill(planner, "find", cost=cost, area_m=m.area_m, memory=memory, kind=kind)
    return RoamSkill(planner, m.action, seconds=m.seconds, cost=cost, area_m=m.area_m)


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
    run_id = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")  # the run things were seen in (LLD §19.7)
    kind = None if m.action != "find" else (is_ball if m.thing is None else kind_of(m.thing))
    world = World(
        kit.cal, kit.cfg, kit.depth, kit.floor, kit.sizes, kit.lim, kind, area_m=m.area_m, run_id=run_id
    )
    skill = make_skill(m, kit.lim, kit.cfg.cost, world.memory)
    stop = asyncio.Event()
    p = Perception(link, cam, world, kit.det, kit.depth, kit.rec)
    p.start(stop, drive.run(stop))
    for _ in range(100):  # both links up before the first move
        if link.connected and drive.connected and p.last_telem_s > 0:
            break
        await asyncio.sleep(0.1)
    else:
        stop.set()
        await p.close()
        return "no connection to the tank - is it on?"
    await drive.cam(0.0, kit.lim.drive_tilt_deg)
    t0 = p.t0 = time.monotonic()
    result = ""
    try:
        while not result:
            if drive.refused is not None:
                result = f"refused by the tank ({drive.refused})"
                break
            if (failed := p.failure()) is not None:
                result = f"failed - {failed}"
                break
            await p.next_picture(TICK_S)
            now = time.monotonic()
            state = p.state(now)
            if state == "waiting":
                continue
            inp = world.input(now - t0, time.time(), state is None)
            # planning off the event loop (6-21 ms: the fan, routes; Z6 review 11) - the snapshot is a copy
            out = await asyncio.to_thread(skill.tick, now - t0, inp)
            await drive.drive(out.cmd)
            if out.cam is not None:
                await drive.cam(*out.cam)
            if out.result is not None:
                result = str(out.result)
                if result == "stale":
                    result = f"stale - {state or 'no pose'}"
            room = world.map_run.room
            seen_m2 = float(room.seen.sum()) * room.cell_m**2
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
        await p.close()
        print()
    return result


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
    ap.add_argument("what", nargs="+", help="find <thing> | wander [seconds] | explore")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--record", type=Path)
    ap.add_argument("--area", type=float, metavar="M", help="stay within M metres of the start")
    ap.add_argument(
        "--no-depth", action="store_true", help="leave the depth model out (the servo's tilt only)"
    )
    a = ap.parse_args()
    try:
        words = goal_words(load_vocabulary(VOCABULARY))
    except (FileNotFoundError, ValueError) as err:
        raise SystemExit(str(err)) from None
    m = parse_mission(a.what, words, a.area)
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
