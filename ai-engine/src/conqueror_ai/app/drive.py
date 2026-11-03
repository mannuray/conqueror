"""`cq-drive`: module 2, drive while thinking (AI engine LLD §17-§18, specs 8-9), on the room map.

    uv run cq-drive find ball                 # look round, search the room, drive to it; Ctrl-C stops
    uv run cq-drive find bottle --area 2      # any word in config/vocabulary.yaml that stands on the floor
    uv run cq-drive wander 120                # drive about for 2 minutes (no number: until stopped)
    uv run cq-drive explore                   # visit everything, then stop (needs the depth model)
    uv run cq-drive wander --record recordings/wander-1
    uv run cq-drive serve --home room2         # the skill server for Tank Logo's AI words (app/serve.py)

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
import random
import sys
import time
from dataclasses import asdict, dataclass, replace
from pathlib import Path
from typing import TYPE_CHECKING, Any, Literal

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.decisions import DecisionLog, replay
from conqueror_ai.adapters.detector_config import load_balls, load_vocabulary
from conqueror_ai.adapters.drive_config import load_drive
from conqueror_ai.adapters.drive_link import DriveLink
from conqueror_ai.adapters.home_store import HomeStore
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
from conqueror_ai.app.runner import SkillRunner, TickView
from conqueror_ai.app.world import World, maybes_of
from conqueror_ai.domain.camera import Calibration
from conqueror_ai.domain.depth import MapConfig, MapSettings
from conqueror_ai.domain.drive.costmap import CostSettings
from conqueror_ai.domain.drive.curves import Command
from conqueror_ai.domain.drive.goal import is_ball, kind_of
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.roam import RoamSkill
from conqueror_ai.domain.drive.skill import Result, Skill
from conqueror_ai.domain.home import Home
from conqueror_ai.domain.memory import Remembered, ThingMemory
from conqueror_ai.domain.objects import Detector
from conqueror_ai.domain.room_map import RoomMap
from conqueror_ai.domain.vocabulary import Vocabulary

__all__ = [
    "FIRST_PICTURE_S",
    "STALE_PICTURE_S",
    "STALE_TELEM_S",
    "map_state",
    "maybes_of",
]  # the stale rule lives in app.map

DRIVE = Path(__file__).resolve().parents[3] / "config" / "drive.yaml"
HOMES = Path(__file__).resolve().parents[3] / "homes"  # git-ignored: each home's map and things (Plan Z8)
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
    m: Mission,
    lim: Limits,
    cost: CostSettings | None = None,
    memory: ThingMemory | None = None,
    seed: int = 0,
) -> RoamSkill:
    """seed: wander's random choices (logged, so a replay makes the same ones, LLD §19.6)."""
    planner, rng = Planner(lim), random.Random(seed)
    if m.action == "find":  # remembered spots, the look round, candidates, then the search (§189)
        kind = is_ball if m.thing is None else kind_of(m.thing)
        return RoamSkill(planner, "find", rng=rng, cost=cost, area_m=m.area_m, memory=memory, kind=kind)
    return RoamSkill(planner, m.action, seconds=m.seconds, rng=rng, cost=cost, area_m=m.area_m)


def start_memory(memory: ThingMemory) -> list[dict[str, Any]]:
    """The memory at a run's start, for the log's first line: a find checks these spots first, so a replay
    must start from them (Z7 final review minor 3)."""
    return [asdict(r) for r in memory.things()]


def replay_text(folder: Path, view: TickView | None = None) -> str:
    """`cq-drive --replay DIR`: the run's logged inputs through a fresh skill; the state timeline and whether
    every decision came out the same (LLD §19.6)."""
    path = folder / "decisions.jsonl"
    if not path.exists():
        return f"no decisions.jsonl in {folder} (a run records one with --record; did it connect?)"

    def make(head: dict[str, Any], seed: int) -> Skill:
        lim = Limits(**head.get("limits", {}))
        cost = CostSettings(**head["cost"]) if "cost" in head else None
        memory = ThingMemory([Remembered(**r) for r in head.get("remembered", [])])
        return make_skill(Mission(**head["mission"]), lim, cost, memory, seed)

    r = replay(path, make, view)
    lines = [f"{t:7.1f} s  {state}" for t, state in r.states]
    if r.first_diff is None:
        lines.append("replay: identical - every decision the same")
    else:
        lines.append(f"replay: tick {r.first_diff} differs - logged {r.logged}, replayed {r.replayed}")
    return "\n".join(lines)


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
    home: tuple[HomeStore, str] | None = None  # --home: the store and the home's name (Plan Z8)
    view: TickView | None = None  # --view: what the engine thinks, drawn each tick (Plan Z8)


async def drive_session(link: BridgeLink, cam: MjpegSource, drive: DriveLink, m: Mission, kit: Kit) -> str:
    """One run on the tank; returns the result ("done" or why not). The links are handed in (tests aim them at
    a fake bridge)."""
    run_id = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")  # the run things were seen in (LLD §19.7)
    kind = None if m.action != "find" else (is_ball if m.thing is None else kind_of(m.thing))
    room, memory, earlier = None, None, None
    if kit.home is not None:  # the home spot: what earlier runs learned (Plan Z8, LLD §20)
        room, memory, earlier, message = start_home(kit.home[0], kit.home[1], kit.cfg.map)
        print(message)
    world = World(
        kit.cal,
        kit.cfg,
        kit.depth,
        kit.floor,
        kit.sizes,
        kit.lim,
        kind,
        room=room,
        memory=memory,
        area_m=m.area_m,
        run_id=run_id,
    )
    connected = [False]  # the tank answered: only then has the run learned anything (Z8 review I1)
    try:
        return await _session(link, cam, drive, m, kit, world, connected)
    finally:  # whatever the result, Ctrl-C too: the home keeps what this run learned
        if kit.home is not None and connected[0]:
            print(end_home(kit.home[0], kit.home[1], earlier, world.map_run.room, world.memory, run_id))


async def _session(
    link: BridgeLink,
    cam: MjpegSource,
    drive: DriveLink,
    m: Mission,
    kit: Kit,
    world: World,
    connected: list[bool],
) -> str:
    seed = random.randrange(1 << 31)  # wander's choices: logged, so a replay makes the same ones
    skill = make_skill(m, kit.lim, kit.cfg.cost, world.memory, seed)
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
    connected[0] = True
    await drive.cam(0.0, kit.lim.drive_tilt_deg)
    p.t0 = time.monotonic()
    room = world.map_run.room

    def status(state: str, cmd: Command) -> None:
        seen_m2 = float(room.seen.sum()) * room.cell_m**2
        print(
            f"\r{state:12s} v {cmd.v_cms:4.1f} cm/s  w {cmd.w_dps:6.1f} deg/s  seen {seen_m2:5.1f} m2 ",
            end="",
            flush=True,
        )

    log = None
    if kit.rec is not None:  # every decision beside the recording (LLD §19.6)
        mission = {"action": m.action, "thing": m.thing, "seconds": m.seconds, "area_m": m.area_m}
        log = DecisionLog(kit.rec.folder / "decisions.jsonl", mission, seed)
        log.head(limits=asdict(kit.lim), cost=asdict(kit.cfg.cost), remembered=start_memory(world.memory))
    runner = SkillRunner(world, p, drive, log, status, view=kit.view)
    try:
        result, detail = await runner.run(skill)
    finally:
        stop.set()
        await p.close()
        if log is not None:
            log.close()
        print()
        if runner.finish_error is not None:  # said even when the run itself raised (Z7 minor 5)
            print(f"the stop message failed ({runner.finish_error}): the tank stops by the deadman")
        if runner.view_errors:
            print(f"the viewer failed on {runner.view_errors} ticks (the run went on)")
    return said(result, detail)


def start_home(
    store: HomeStore, name: str, settings: MapSettings
) -> tuple[RoomMap | None, ThingMemory | None, Home | None, str]:
    """The home's starting map and memory (LLD §20.2), the home itself, and what to say."""
    home = store.load(name, size_m=settings.size_m, cell_m=settings.cell_m)
    if home is None:
        return None, None, None, f"home '{name}': new - it is saved at the end of this run"
    room = home.room_for_run(settings)
    cells = int(((room.low > settings.blocked) | (room.high > settings.blocked)).sum())
    runs, things = len(home.runs), len(home.things)
    return (
        room,
        home.memory(),
        home,
        (
            f"home '{name}': {runs} run{'s' * (runs != 1)}, {cells} obstacle cell{'s' * (cells != 1)}, "
            f"{things} thing{'s' * (things != 1)} - "
            "put the tank on the home mark, facing the way it faced last time"
        ),
    )


def end_home(
    store: HomeStore, name: str, earlier: Home | None, room: RoomMap, memory: ThingMemory, run_id: str
) -> str:
    """Save what this run leaves for the next; a failed save is said, never raised (the result stays)."""
    try:
        store.save(name, Home.from_run(room, memory, earlier, run_id))
    except OSError as e:
        return f"home '{name}' not saved ({e})"
    return f"home '{name}' saved"


def live_view() -> TickView:
    """The Rerun viewer for --view (spawned on first use)."""
    from conqueror_ai.adapters.rerun_view import RerunView, SkillView

    return SkillView(RerunView(spawn=True))


def said(result: Result, detail: str) -> str:
    """The CLI's words for a run's end (as before Plan Z7)."""
    if result == Result.REFUSED:
        return f"refused by the tank ({detail})"
    if result == Result.FAILED:
        return f"failed - {detail}"
    if result == Result.STALE:
        return f"stale - {detail}"
    return str(result) if not detail else f"{result} - {detail}"  # e.g. not_found - seen outside the area


async def run_drive(
    host: str,
    m: Mission,
    rec: SessionRecorder | None,
    use_depth: bool = True,
    home: str | None = None,
    view: TickView | None = None,
) -> str:
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.mjpeg_source import MjpegSource

    cal = calibration_store.load(CALIBRATION, VGA)
    det, floor = detectors(MODEL, VOCABULARY, BALLS)
    sizes = {f"ball:{b.name}": b.size_cm for b in load_balls(BALLS) if b.size_cm is not None}
    lim, table = load_drive(DRIVE)
    depth = load_depth(DEPTH_MODEL) if use_depth else None
    if (why := refusal(m, depth is not None)) is not None:
        return f"refused - {why}"
    store = None if home is None else (HomeStore(HOMES), home)
    kit = Kit(cal, load_map_config(MAP_CONFIG), det, floor, sizes, lim, depth, rec, store, view)
    return await drive_session(BridgeLink(host), MjpegSource(host), DriveLink(host, table=table), m, kit)


def main() -> None:
    if sys.argv[1:2] == ["serve"]:  # the skill server has its own options (AI engine LLD §21)
        from conqueror_ai.app.serve import main as serve_main

        serve_main(sys.argv[2:])
        return
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "what", nargs="*", help="find <thing> | wander [seconds] | explore | serve (its own --help)"
    )
    ap.add_argument(
        "--replay", type=Path, metavar="DIR", help="re-run a recorded run's decisions (LLD §19.6)"
    )
    ap.add_argument("--view", action="store_true", help="show what the engine thinks in the Rerun viewer")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--record", type=Path)
    ap.add_argument("--area", type=float, metavar="M", help="stay within M metres of the start")
    ap.add_argument(
        "--home",
        nargs="?",
        const="home",
        metavar="NAME",
        help="start from the home mark: load and save its map",
    )
    ap.add_argument(
        "--no-depth", action="store_true", help="leave the depth model out (the servo's tilt only)"
    )
    a = ap.parse_args()
    if a.replay is not None:
        print(replay_text(a.replay, live_view() if a.view else None))  # a viewer that won't start: said
        return
    if not a.what:
        ap.error("say what to do: find <thing> | wander [seconds] | explore")
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
        view = None
        if a.view:
            try:
                view = live_view()
            except Exception as e:  # a viewer that won't start never stops the run (Z8 review minor 7)
                print(f"the viewer did not start ({e}); driving without it")
        print(f"result: {asyncio.run(run_drive(a.host, m, rec, not a.no_depth, a.home, view))}")
    except KeyboardInterrupt:
        print("\nstopped (Ctrl-C): the tank was told to stop")
    except (FileNotFoundError, ValueError) as err:  # a HomeError too: said plainly
        raise SystemExit(str(err)) from None
    finally:
        if rec is not None:
            rec.close()


if __name__ == "__main__":
    main()
