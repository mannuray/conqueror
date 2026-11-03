"""`cq-drive serve`: the skill server for Tank Logo's AI words (AI engine LLD §21, bridge LLD R26).

    uv run cq-drive serve                     # FIND, WANDER, EXPLORE, SETXY, SEE?, HOWFAR from /logo
    uv run cq-drive serve --home room2        # start from the home mark; the home is saved after each word
    uv run cq-drive serve --record recordings/logo-1

One long session: perception and the map run all the time, so a question is answered from a fresh World
and a word starts from the map built so far. Each action word (`skill_start`) runs one skill under the
bridge's lease (LeaseDriver): it never drives outside one, and stops sending the moment the lease ends.
Ctrl-C ends it: a running word is cancelled (the bridge ends the program with 7052), the home is saved.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import datetime
import random
from collections.abc import Awaitable, Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Protocol

from conqueror_ai.adapters.decisions import DecisionLog
from conqueror_ai.adapters.home_store import HomeStore
from conqueror_ai.adapters.lease_driver import LeaseDriver, wire_reason
from conqueror_ai.adapters.skill_link import Ask, End, SkillBusy, SkillLink, Start
from conqueror_ai.app.questions import Questions
from conqueror_ai.app.runner import TICK_S, SkillRunner, TickView
from conqueror_ai.app.world import World
from conqueror_ai.domain.drive.goal import Goal, kind_of
from conqueror_ai.domain.drive.places import to_map
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.roam import RoamSkill
from conqueror_ai.domain.drive.skill import Result, Skill
from conqueror_ai.domain.home import Home
from conqueror_ai.domain.names import Names
from conqueror_ai.domain.types import Pose

if TYPE_CHECKING:
    from conqueror_ai.adapters.session import SessionRecorder
    from conqueror_ai.domain.camera import CameraModel

RECONNECT_S = 2.0  # the skill link is tried again this often after it was lost
SKILLS = ("find", "wander", "explore", "setxy")


@dataclass(frozen=True)
class SkillCall:
    """One action word, its places in the map frame (from the pose when `skill_start` arrived)."""

    skill: str  # find, wander, explore, setxy
    word: str | None  # find: the resolved word (never a person)
    seconds: float | None  # wander
    point: tuple[float, float] | None  # setxy: the map point
    area_m: float | None  # the play area's radius (None: anywhere, AREA 0)
    centre: tuple[float, float] | None  # its centre (None: round the word's first pose)


def skill_call(ev: Start, pose: Pose | None, names: Names) -> SkillCall | str:
    """The call for a `skill_start`, or the `skill_failed` reason it gets at once (no frame sent): a thing
    the engine doesn't know (a person never resolves) is `unknown_thing`; a word it can't start `stale`."""
    if ev.skill not in SKILLS:
        return "stale"
    word = None
    if ev.skill == "find":
        word = names.resolve(ev.thing or "")
        if word is None:
            return "unknown_thing"

    def place(fwd_cm: float, left_cm: float) -> tuple[float, float] | None:
        return None if pose is None else to_map(pose.x_m, pose.y_m, pose.th_rad, fwd_cm, left_cm)

    point = None
    if ev.skill == "setxy" and (ev.to is None or (point := place(*ev.to)) is None):
        return "stale"  # no place given, or no pose to place it from
    area_m, centre = None, None
    if ev.area is not None and ev.area[0] > 0:  # AREA 0 is anywhere
        area_m, centre = ev.area[0], place(ev.area[1], ev.area[2])
    return SkillCall(ev.skill, word, ev.seconds, point, area_m, centre)


def make_word_skill(call: SkillCall, world: World, seed: int) -> Skill:
    """The RoamSkill for a word (LLD §21.2); seed: wander's random choices (logged)."""
    planner, rng, cost = Planner(world.lim), random.Random(seed), world.cfg.cost
    area_m, centre = call.area_m, call.centre
    if call.skill == "find":
        kind = kind_of(call.word or "")
        return RoamSkill(
            planner, "find", rng=rng, cost=cost, area_m=area_m, centre=centre, memory=world.memory, kind=kind
        )
    if call.skill == "setxy":
        return RoamSkill(planner, "point", rng=rng, cost=cost, area_m=area_m, centre=centre, point=call.point)
    mode = "wander" if call.skill == "wander" else "explore"
    return RoamSkill(planner, mode, seconds=call.seconds, rng=rng, cost=cost, area_m=area_m, centre=centre)


SkillMaker = Callable[[SkillCall, World, int], Skill]


class ServePerception(Protocol):
    """Perception as the session uses it (tests hand in a fake)."""

    t0: float

    def start(self, stop: asyncio.Event) -> None: ...

    async def next_picture(self, timeout_s: float) -> CameraModel | str | None: ...

    def state(self, now: float) -> str | None: ...

    def failure(self) -> str | None: ...

    def bridge_now_s(self) -> float | None: ...

    async def close(self) -> None: ...


class _OnePuller:
    """The runner's pictures through the session's lock: one puller of next_picture at a time."""

    def __init__(self, p: ServePerception, lock: asyncio.Lock) -> None:
        self.p, self.lock = p, lock
        self.t0 = p.t0  # one per word: perception's start, the skill's clock (it counts its own times)

    async def next_picture(self, timeout_s: float) -> object:
        async with self.lock:
            return await self.p.next_picture(timeout_s)

    def state(self, now: float) -> str | None:
        return self.p.state(now)

    def failure(self) -> str | None:
        return self.p.failure()


@dataclass
class _Word:
    id: int
    driver: LeaseDriver
    keep: asyncio.Event  # set: the lease's keep-alive stops
    keep_task: asyncio.Task[None]
    task: asyncio.Task[None] | None = None
    silent: bool = False  # the bridge ended it, the link was lost, or Ctrl-C: nothing is sent back


class ServeApp:
    def __init__(
        self,
        link: SkillLink,
        perception: ServePerception,
        world: World,
        names: Names,
        home: str | None,
        record: Path | None,
        *,
        store: HomeStore | None = None,
        skills: SkillMaker = make_word_skill,
        view: TickView | None = None,
    ) -> None:
        """home: the home's name (--home), in `store` (default: ai-engine/homes). record: each word's
        decisions in record/word-<id>/decisions.jsonl. skills: builds a word's skill (tests script one)."""
        self.link, self.perception, self.world, self.names = link, perception, world, names
        self.home, self.record, self.skills, self.view = home, record, skills, view
        self.store = store
        if not world.run_id:  # the session is one run of the home
            world.run_id = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.state = "connecting"  # connecting, idle, driving (LLD §21.6)
        self.questions = Questions()
        self._lock = asyncio.Lock()  # one puller of pictures: the pump between words, a word's runner
        self._word: _Word | None = None
        self._earlier: Home | None = None
        self._connected_once = False

    async def run(self, stop: asyncio.Event) -> None:
        """Until `stop` (or Ctrl-C: the task cancelled). SkillBusy when another skill server holds the bridge
        at the first connect; ThingsRefused when the bridge refuses the things list."""
        self._load_home()
        pstop = asyncio.Event()
        self.perception.start(pstop)
        pump = asyncio.create_task(self._pump(pstop))
        try:
            while not stop.is_set():
                self.state = "connecting"
                try:
                    await self.link.connect()
                except SkillBusy:
                    if not self._connected_once:
                        raise  # another cq-drive serve: a plain error
                    print("skill link: the bridge still holds the old session - trying again")
                except (OSError, TimeoutError, ConnectionError) as e:  # the tank is off, or the WiFi
                    print(f"skill link: {type(e).__name__} - trying again in {RECONNECT_S:.0f} s")
                else:
                    if not self._connected_once:
                        print("serving the tank's AI words (Ctrl-C ends)")
                    self._connected_once = True
                    self.state = "idle"
                    await self._session(stop)
                    await self.link.close()
                    if stop.is_set():
                        break
                    print("skill link lost - trying again")
                with contextlib.suppress(TimeoutError):
                    await asyncio.wait_for(stop.wait(), RECONNECT_S)
        finally:
            await self._drop_word("stop")  # Ctrl-C: cancelled, nothing sent (the bridge's close rule: 7052)
            self._save_home()
            pstop.set()
            pump.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await pump
            await self.perception.close()
            await self.link.close()

    # ---- one connection
    async def _session(self, stop: asyncio.Event) -> None:
        reader = asyncio.create_task(self._events())
        stopper = asyncio.create_task(stop.wait())
        try:
            await asyncio.wait({reader, stopper}, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for t in (reader, stopper):
                t.cancel()
            await asyncio.gather(reader, stopper, return_exceptions=True)
            await self._drop_word("lost")  # the link is gone: stop sending, never resume the old lease

    async def _events(self) -> None:
        async for ev in self.link.events():
            if isinstance(ev, Ask):
                await self._on_ask(ev)
            elif isinstance(ev, Start):
                await self._on_start(ev)
            elif isinstance(ev, End):
                await self._on_end(ev)
            else:  # Lost: the events end
                return

    async def _on_ask(self, ev: Ask) -> None:
        word, now = self.names.resolve(ev.thing), self.perception.bridge_now_s()
        value: bool | int
        if ev.q == "howfar":
            value = -1 if word is None or now is None else self.questions.howfar(self.world, word, now)
        else:
            value = word is not None and now is not None and self.questions.see(self.world, word, now)
        await self._send(self.link.answer(ev.id, value))

    async def _on_start(self, ev: Start) -> None:
        if self._word is not None:  # one word at a time
            await self._send(self.link.failed(ev.id, "stale", ev.thing or ""))
            return
        call = skill_call(ev, self.world.map_run.p.pose(), self.names)
        if isinstance(call, str):  # before any frame: the lease's first v 0 would be one
            await self._send(self.link.failed(ev.id, call, ev.thing or ""))
            return
        driver = LeaseDriver(self.link, ev.id)
        keep = asyncio.Event()
        w = _Word(ev.id, driver, keep, asyncio.create_task(driver.run(keep)))  # the first frame at once
        w.task = asyncio.create_task(self._run_word(w, call, ev.thing or ""))
        self._word, self.state = w, "driving"

    async def _on_end(self, ev: End) -> None:
        if self._word is not None and self._word.id == ev.id:  # an old id is not this word's
            await self._drop_word(ev.why)

    async def _drop_word(self, why: str) -> None:
        """The word ends without an answer: the lease ended (`why`: the bridge's reason, "lost", "stop" for
        Ctrl-C) - nothing more goes out, not a frame, not the finish's stop; the bridge stops the tank."""
        w = self._word
        if w is None:
            return
        w.silent = True
        w.driver.end(why)
        if w.task is not None:
            w.task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await w.task

    # ---- one word
    async def _run_word(self, w: _Word, call: SkillCall, thing: str) -> None:
        try:
            result = await self._drive_word(w, call)
        except Exception as e:  # a skill that breaks answers stale (7052) instead of going quiet
            print(f"word {w.id} {call.skill} failed: {type(e).__name__}: {e}")
            result = Result.FAILED
        finally:
            if self._word is w:
                self._word = None
            if self.state == "driving":
                self.state = "idle"
            if w.silent:  # cancelled: the home still keeps what the word learned
                self._save_home()
        if w.silent:
            return
        reason = wire_reason(result)
        await self._send(self.link.done(w.id) if reason is None else self.link.failed(w.id, reason, thing))
        self._save_home()

    async def _drive_word(self, w: _Word, call: SkillCall) -> Result:
        seed = random.randrange(1 << 31)
        log = self._log(w.id, call, seed)
        try:
            self.world.goal = Goal(kind_of(call.word)) if call.word is not None else None  # find's fix
            if call.area_m is not None and call.centre is not None:  # the World's cost maps carry the area
                self.world.area = (call.centre[0], call.centre[1], call.area_m)
            skill = self.skills(call, self.world, seed)
            await w.driver.cam(0.0, self.world.lim.drive_tilt_deg)
            runner = SkillRunner(
                self.world, _OnePuller(self.perception, self._lock), w.driver, log, view=self.view
            )
            result, detail = await runner.run(skill)
            if detail:
                print(f"word {w.id} {call.skill}: {result} - {detail}")
            return result
        finally:
            self.world.goal, self.world.area = None, None
            w.keep.set()
            with contextlib.suppress(asyncio.CancelledError):
                await w.keep_task
            if log is not None:
                log.close()

    def _log(self, id: int, call: SkillCall, seed: int) -> DecisionLog | None:
        if self.record is None:
            return None
        folder = self.record / f"word-{id}"
        folder.mkdir(parents=True, exist_ok=True)
        mission = {"action": call.skill, "thing": call.word, "seconds": call.seconds, "area_m": call.area_m}
        log = DecisionLog(folder / "decisions.jsonl", mission, seed)
        remembered = [asdict(r) for r in self.world.memory.things()]
        log.head(limits=asdict(self.world.lim), cost=asdict(self.world.cfg.cost), remembered=remembered)
        log.head(point=call.point, centre=call.centre)  # setxy and the area's centre (not replayed yet)
        return log

    # ---- between words
    async def _pump(self, stop: asyncio.Event) -> None:
        """Pictures into the World between words (a word's runner pulls its own)."""
        while not stop.is_set():
            if self._word is not None:
                await asyncio.sleep(TICK_S)
                continue
            try:
                async with self._lock:
                    await self.perception.next_picture(TICK_S)
            except Exception as e:  # a picture that fails is skipped; the next one may not
                print(f"picture failed: {type(e).__name__}: {e}")
                await asyncio.sleep(TICK_S)

    async def _send(self, msg: Awaitable[None]) -> None:
        """An answer to the bridge; a link that went down meanwhile is the events' Lost to handle."""
        with contextlib.suppress(ConnectionError):
            await msg

    # ---- the home
    def _home_store(self) -> HomeStore:
        if self.store is None:
            from conqueror_ai.app.drive import HOMES

            self.store = HomeStore(HOMES)
        return self.store

    def _load_home(self) -> None:
        if self.home is None:
            return
        from conqueror_ai.app.drive import start_home

        room, memory, self._earlier, message = start_home(self._home_store(), self.home, self.world.cfg.map)
        print(message)
        if room is not None:
            self.world.map_run.room = room
        if memory is not None:
            self.world.memory = memory

    def _save_home(self) -> None:
        if self.home is None or not self._connected_once:  # a session that never connected learned nothing
            return
        from conqueror_ai.app.drive import end_home

        w = self.world
        print(end_home(self._home_store(), self.home, self._earlier, w.map_run.room, w.memory, w.run_id))


def main(argv: list[str]) -> None:
    """`cq-drive serve [--home NAME] [--record DIR] [--view] [--host H]`."""
    from conqueror_ai.app.objects import DEFAULT_HOST

    ap = argparse.ArgumentParser(
        prog="cq-drive serve", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--record", type=Path, metavar="DIR", help="record pictures, telemetry and decisions")
    ap.add_argument("--view", action="store_true", help="show what the engine thinks in the Rerun viewer")
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
    a = ap.parse_args(argv)
    rec = None
    try:
        app, rec = _build(a.host, a.home, a.record, not a.no_depth, a.view)
        asyncio.run(app.run(asyncio.Event()))
    except KeyboardInterrupt:
        print("\nstopped (Ctrl-C)")
    except SkillBusy as e:
        raise SystemExit(f"another skill server is connected to the tank ({e})") from None
    except (FileNotFoundError, ValueError) as err:  # ThingsRefused, a HomeError, a bad config: said plainly
        raise SystemExit(str(err)) from None
    finally:
        if rec is not None:
            rec.close()


def _build(
    host: str, home: str | None, record: Path | None, use_depth: bool, view: bool
) -> tuple[ServeApp, SessionRecorder | None]:
    from conqueror_ai.adapters import calibration_store
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.detector_config import load_ball_names, load_balls, load_vocabulary
    from conqueror_ai.adapters.drive_config import load_drive
    from conqueror_ai.adapters.map_config import load_map_config
    from conqueror_ai.adapters.mjpeg_source import MjpegSource
    from conqueror_ai.adapters.session import SessionRecorder
    from conqueror_ai.app.drive import DRIVE, live_view
    from conqueror_ai.app.objects import (
        BALLS,
        CALIBRATION,
        DEPTH_MODEL,
        MAP_CONFIG,
        MODEL,
        VGA,
        VOCABULARY,
        detectors,
        load_depth,
    )
    from conqueror_ai.app.perception import Perception

    names = Names(load_vocabulary(VOCABULARY), load_ball_names(BALLS))
    cal = calibration_store.load(CALIBRATION, VGA)
    det, floor = detectors(MODEL, VOCABULARY, BALLS)
    sizes = {f"ball:{b.name}": b.size_cm for b in load_balls(BALLS) if b.size_cm is not None}
    lim, _ = load_drive(DRIVE)
    depth = load_depth(DEPTH_MODEL) if use_depth else None
    world = World(cal, load_map_config(MAP_CONFIG), depth, floor, sizes, lim, None)
    rec = None
    if record is not None:
        when = datetime.datetime.now().isoformat(timespec="seconds")
        rec = SessionRecorder(record, {"host": host, "what": "cq-drive serve", "date": when})
    perception = Perception(BridgeLink(host), MjpegSource(host), world, det, depth, rec)
    tick_view = None
    if view:
        try:
            tick_view = live_view()
        except Exception as e:  # a viewer that won't start never stops the session
            print(f"the viewer did not start ({e}); serving without it")
    link = SkillLink(f"ws://{host}/ws", names.wire())
    return ServeApp(link, perception, world, names, home, record, view=tick_view), rec
