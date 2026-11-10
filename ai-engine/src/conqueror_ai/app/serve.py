"""`cq-drive serve`: the skill server for Tank Logo's AI words (AI engine LLD §21, bridge LLD R26).

    uv run cq-drive serve                     # FIND, WANDER, EXPLORE, SETXY, SEE?, HOWFAR from /logo
    uv run cq-drive serve --home room2        # start from the home mark; the home is saved after each word
    uv run cq-drive serve --record recordings/logo-1
    uv run cq-drive serve --page-port 0       # no AI feed for the /ai pages (default port 8770)

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
import time
from collections.abc import Awaitable, Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, Protocol, cast

from conqueror_ai.adapters.decisions import DecisionLog
from conqueror_ai.adapters.feed_server import FeedServer
from conqueror_ai.adapters.home_store import HomeStore
from conqueror_ai.adapters.lease_driver import LeaseDriver, wire_reason
from conqueror_ai.adapters.skill_link import Ask, End, Game, SkillBusy, SkillLink, Start, ThingsRefused
from conqueror_ai.adapters.taught_words import TaughtWords
from conqueror_ai.app.feed import FeedPublisher, TickFan, make_feed
from conqueror_ai.app.game_serve import (
    GAME_SKILLS,
    PersonStream,
    back_ok,
    game_refusal,
    game_skill,
    is_person,
    person_answer,
    person_message,
)
from conqueror_ai.app.questions import Questions
from conqueror_ai.app.room_feed import RoomFeed, RoomWorld
from conqueror_ai.app.runner import TICK_S, SkillRunner, TickView
from conqueror_ai.app.seeing_pass import SeeingPass
from conqueror_ai.app.teach import Reply, Teacher, run_export
from conqueror_ai.app.world import World
from conqueror_ai.domain.drive.costmap import CostSettings
from conqueror_ai.domain.drive.goal import Goal, kind_of
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.places import to_map
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.roam import RoamSkill
from conqueror_ai.domain.drive.skill import Result, Skill
from conqueror_ai.domain.home import Home
from conqueror_ai.domain.memory import ThingMemory
from conqueror_ai.domain.names import Names
from conqueror_ai.domain.objects import Detector
from conqueror_ai.domain.places import Places
from conqueror_ai.domain.types import Pose
from conqueror_ai.domain.vocabulary import Vocabulary

if TYPE_CHECKING:
    from conqueror_ai.adapters.session import SessionRecorder
    from conqueror_ai.domain.camera import CameraModel

MakeDetector = Callable[[Path, Vocabulary], tuple[Detector, frozenset[str]]]
PAGE_PORT = 8770  # the /ai pages' AI feed (AI engine LLD §22.1)
RECONNECT_S = 2.0  # the skill link is tried again this often after it was lost
SKILLS = ("find", "wander", "explore", "setxy", *GAME_SKILLS)
NEEDS_DEPTH = ("find", "explore")  # without depth find would end at once, explore has nowhere to go


@dataclass(frozen=True)
class SkillCall:
    """One action word, its places in the map frame (from the pose when `skill_start` arrived)."""

    skill: str  # find, wander, explore, setxy; the game words face, escape, follow (LLD §28.3)
    word: str | None  # find: the resolved word (never a person; the game words' person is not a word)
    seconds: float | None  # wander
    point: tuple[float, float] | None  # setxy: the map point
    area_m: float | None  # the play area's radius (None: anywhere, AREA 0)
    centre: tuple[float, float] | None  # its centre (None: round the word's first pose)
    back_m: float | None = None  # escape: how far
    stop_m: float | None = None  # follow: the follow distance (None: the default)


def skill_call(ev: Start, pose: Pose | None, names: Names) -> SkillCall | str:
    """The call for a `skill_start`, or the `skill_failed` reason it gets at once (no frame sent): a thing
    the engine doesn't know (a person never resolves) is `unknown_thing`; a word it can't start `stale`."""
    if ev.skill not in SKILLS:
        return "stale"
    if ev.skill in GAME_SKILLS and (no := game_refusal(ev.skill, ev.thing, ev.cm, ev.seconds)) is not None:
        return no
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
        if centre is None:
            return "stale"  # no pose to place the area's centre from (as setxy)
    back_m = None if ev.cm is None else ev.cm / 100
    stop_m = None if ev.stop_cm is None else ev.stop_cm / 100
    return SkillCall(ev.skill, word, ev.seconds, point, area_m, centre, back_m, stop_m)


def word_skill(call: SkillCall, lim: Limits, cost: CostSettings, memory: ThingMemory, seed: int) -> Skill:
    """The skill for a word (LLD §21.2, the game words §28.3), from what the decisions log keeps (so a
    replay builds the same one); seed: wander's random choices."""
    if call.skill in GAME_SKILLS:
        return game_skill(call.skill, lim, call.back_m, call.seconds, call.stop_m)
    planner, rng = Planner(lim), random.Random(seed)
    area_m, centre = call.area_m, call.centre
    if call.skill == "find":
        kind = kind_of(call.word or "")
        return RoamSkill(
            planner, "find", rng=rng, cost=cost, area_m=area_m, centre=centre, memory=memory, kind=kind
        )
    if call.skill == "setxy":
        return RoamSkill(planner, "point", rng=rng, cost=cost, area_m=area_m, centre=centre, point=call.point)
    mode = "wander" if call.skill == "wander" else "explore"
    return RoamSkill(planner, mode, seconds=call.seconds, rng=rng, cost=cost, area_m=area_m, centre=centre)


def make_word_skill(call: SkillCall, world: World, seed: int) -> Skill:
    return word_skill(call, world.lim, world.cfg.cost, world.memory, seed)


def word_log(
    folder: Path, call: SkillCall, seed: int, lim: Limits, cost: CostSettings, memory: ThingMemory
) -> DecisionLog:
    """A word's decisions log: the mission, the limits, cost settings and memory, and the word's point and
    area centre - all `cq-drive --replay` needs to rebuild the skill (`call_of`)."""
    folder.mkdir(parents=True, exist_ok=True)
    mission = {"action": call.skill, "thing": call.word, "seconds": call.seconds, "area_m": call.area_m}
    log = DecisionLog(folder / "decisions.jsonl", mission, seed)
    remembered = [asdict(r) for r in memory.things()]
    log.head(
        limits=asdict(lim),
        cost=asdict(cost),
        remembered=remembered,
        point=call.point,
        centre=call.centre,
        back_m=call.back_m,
        stop_m=call.stop_m,
    )
    return log


def call_of(head: dict[str, Any]) -> SkillCall | None:
    """A serve word's call from its log's first line, None for a cq-drive run's (no `centre` key)."""
    if "centre" not in head:
        return None
    m = head["mission"]

    def xy(v: Any) -> tuple[float, float] | None:
        return None if v is None else (float(v[0]), float(v[1]))

    return SkillCall(
        m["action"],
        m["thing"],
        m["seconds"],
        xy(head["point"]),
        m["area_m"],
        xy(head["centre"]),
        head.get("back_m"),
        head.get("stop_m"),
    )


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

    def reload_detector(self, det: Detector) -> None: ...


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
    why: str = ""  # the bridge's reason when it ended the word (skill_end), "lost", "stop"


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
        feed: tuple[FeedServer, FeedPublisher] | None = None,
        seeing: SeeingPass | None = None,
    ) -> None:
        """home: the home's name (--home), in `store` (default: ai-engine/homes). record: each word's
        decisions in record/word-<id>/decisions.jsonl. skills: builds a word's skill (tests script one).
        feed: the /ai pages' AI feed and its publisher (§22; None: --page-port 0). seeing: the seeing test's
        pass (§28.6) - the hello's `people` (None: no games)."""
        self.link, self.perception, self.world, self.names = link, perception, world, names
        self.home, self.record, self.skills, self.view = home, record, skills, view
        self.store = store
        self.feed_server, self.publisher = feed if feed is not None else (None, None)
        self.teacher: Teacher | None = None  # set by _build: it calls back into this app
        self.make_detector: MakeDetector | None = None  # a taught word's model -> the detector (§22.4)
        self.ball_names: list[str] = []  # the names of the word "ball" (balls.yaml), kept when names change
        self._teaching: set[asyncio.Task[None]] = set()
        if not world.run_id:  # the session is one run of the home
            world.run_id = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        self.state = "connecting"  # connecting, idle, driving (LLD §21.6)
        self.questions = Questions()
        self._lock = asyncio.Lock()  # one puller of pictures: the pump between words, a word's runner
        self._word: _Word | None = None
        self._earlier: Home | None = None
        self._connected_once = False
        self._closing = False  # Ctrl-C / stop: run() saves the home once at the end
        self._seq = 0  # words in this session (the decisions folders)
        self.places = Places()  # the names on the map (LLD §23.3); a home's come back in _load_home
        self.seeing = seeing
        self.stream = PersonStream(world, self._send_person, perception.bridge_now_s)
        self.last_result: Result | None = None  # the last word's ending (CAUGHT when the bridge caught it)

    async def run(self, stop: asyncio.Event) -> None:
        """Until `stop` (or Ctrl-C: the task cancelled). SkillBusy when another skill server holds the bridge
        at the first connect; ThingsRefused when the bridge refuses the things list."""
        self._load_home()
        pstop = asyncio.Event()
        self.perception.start(pstop)
        pump = asyncio.create_task(self._pump(pstop))
        feed = self._feed_tasks(pstop)
        try:
            while not stop.is_set():
                self.state = "connecting"
                self.link.people = self.seeing is not None and self.seeing.passed()  # read at each connect
                if self.publisher is not None:  # the GAMES card (design-notes §224)
                    self.publisher.people, self.publisher.person = self.link.people, self._game_person
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
            # a word is only ever running inside _session, whose clean-up dropped it (silently: the bridge's
            # close rule ends the program with 7052); the home is saved here, once
            self._save_home()
            pstop.set()
            pump.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await pump
            for t in feed:
                t.cancel()
            await asyncio.gather(*feed, return_exceptions=True)
            await self.perception.close()
            await self.link.close()

    def _feed_tasks(self, stop: asyncio.Event) -> list[asyncio.Task[None]]:
        """The /ai pages' feed server and the status publisher (§22.1), until `stop`."""
        if self.feed_server is None or self.publisher is None:
            return []

        def telemetry() -> tuple[Pose | None, bool, str, float | None]:
            run = self.world.map_run
            return (
                run.p.pose(),
                False,
                self.perception.state(time.monotonic()) or "ok",
                run.camera_pan_left_deg(),
            )

        self.publisher.room = RoomFeed(self.feed_server, self._room_world, self.places, self.home)

        server = asyncio.create_task(self.feed_server.run(stop))
        server.add_done_callback(_said_if_failed)
        return [
            server,
            asyncio.create_task(self.publisher.run(stop, telemetry)),
            asyncio.create_task(self._page_requests()),
        ]

    async def _page_requests(self) -> None:
        """The pages' requests: teach a word (§22.4), each in its own task so the reading goes on."""
        if self.feed_server is None:
            return
        try:
            async for page, msg in self.feed_server.requests():
                if msg.get("t") == "teach":
                    task = asyncio.create_task(self._on_teach(page, msg))
                    self._teaching.add(task)
                    task.add_done_callback(self._teaching.discard)
                elif msg.get("t") in ("name", "unname"):  # names on the map (LLD §23.3)
                    room = None if self.publisher is None else self.publisher.room
                    if room is not None:
                        self.feed_server.reply(page, room.on_name(msg))
        finally:
            for task in list(self._teaching):  # Ctrl-C: the teach takes its word out again
                task.cancel()
            await asyncio.gather(*self._teaching, return_exceptions=True)

    async def _on_teach(self, page: int, msg: dict[str, Any]) -> None:
        word, names = msg.get("word"), cast("object", msg.get("names") or [])
        if self.teacher is None or not isinstance(word, str) or not isinstance(names, list):
            reply = Reply(False, "I can't learn words here.")
        else:
            reply = await self.teacher.teach(
                word,
                [str(n) for n in cast("list[Any]", names)],
                lambda n: self.names.resolve(n) is not None,
                busy=self._word is not None,
            )
        if self.feed_server is not None:
            self.feed_server.reply(page, {"t": "rep", "id": msg.get("id"), "ok": reply.ok, "msg": reply.msg})

    async def on_learning(self, word: str | None) -> None:
        """Around a teach: the pages see it, the bridge gives 7056 for AI words meanwhile."""
        if self.publisher is not None:
            self.publisher.learning(word)
        await self._send(self.link.learning(word is not None))

    async def apply_words(self, vocab: Vocabulary, model: Path) -> None:
        """Everything a word list touches (§22.4): the detector (swapped between two pictures), the names,
        the floor words, the things list the bridge checks (sent again on every reconnect) and the pages'."""
        if self.make_detector is None:
            raise RuntimeError("no detector maker")
        names = Names(vocab, self.ball_names)
        det, floor = await asyncio.to_thread(self.make_detector, model, vocab)
        # the tank's list first: a refused list (ThingsRefused) fails the teach with nothing swapped
        # (review M7); a lost link is fine - a reconnect sends link.things
        with contextlib.suppress(ConnectionError):
            await self.link.set_things(names.wire())
        self.link.things = names.wire()
        self.perception.reload_detector(det)
        self.names = names
        self.world.set_floor_classes(floor)
        if self.publisher is not None:
            self.publisher.things = self.names.page_list

    # ---- one connection
    async def _session(self, stop: asyncio.Event) -> None:
        self.stream.on = False  # a new link: no person until the bridge says game on (Review Focus 3)
        reader = asyncio.create_task(self._events())
        stopper = asyncio.create_task(stop.wait())
        streamer = asyncio.create_task(self.stream.run())
        try:
            await asyncio.wait({reader, stopper}, return_when=asyncio.FIRST_COMPLETED)
        finally:
            self.stream.on = False
            for t in (reader, stopper, streamer):
                t.cancel()
            await asyncio.gather(reader, stopper, streamer, return_exceptions=True)
            task = asyncio.current_task()
            if stop.is_set() or (task is not None and task.cancelling()):
                self._closing = True  # Ctrl-C or stop: run() saves the home once
            # the link is gone, or the session ends: stop sending, never resume the old lease
            await self._drop_word("stop" if self._closing else "lost")

    async def _events(self) -> None:
        async for ev in self.link.events():
            if isinstance(ev, Ask):
                await self._on_ask(ev)
            elif isinstance(ev, Start):
                await self._on_start(ev)
            elif isinstance(ev, End):
                await self._on_end(ev)
            elif isinstance(ev, Game):
                self.stream.on = ev.on
            else:  # Lost: the events end
                return

    async def _on_ask(self, ev: Ask) -> None:
        word, now = self.names.resolve(ev.thing), self.perception.bridge_now_s()
        value: bool | int
        if ev.q == "back_ok":  # a game's backward move (bridge LLD R34.2)
            value = not ev.arc and now is not None and back_ok(self.world, ev.cm, round(now * 1e6))
        elif ev.game and is_person(ev.thing):  # the game's person, from PersonWatch (LLD §28.1)
            value = (
                (-1 if ev.q == "howfar" else False)
                if now is None
                else person_answer(self.world, ev.q, round(now * 1e6))
            )
        elif ev.q == "howfar":
            value = -1 if word is None or now is None else self.questions.howfar(self.world, word, now)
        else:
            value = word is not None and now is not None and self.questions.see(self.world, word, now)
        await self._send(self.link.answer(ev.id, value))

    async def _on_start(self, ev: Start) -> None:
        if self.teacher is not None and self.teacher.learning is not None:  # checked before the teach (7056)
            await self._send(self.link.failed(ev.id, "learning", ev.thing or ""))
            return
        if self._word is not None:  # one word at a time
            await self._send(self.link.failed(ev.id, "stale", ev.thing or ""))
            return
        call = skill_call(ev, self.world.map_run.p.pose(), self.names)
        if not isinstance(call, str) and call.skill in GAME_SKILLS and not self.stream.on:
            call = "stale"  # a game word only inside the bridge's game mode (G3 final review M6)
        if isinstance(call, str):  # before any frame: the lease's first v 0 would be one
            await self._send(self.link.failed(ev.id, call, ev.thing or ""))
            return
        if call.skill in NEEDS_DEPTH and self.world.map_run.depth is None:  # as cq-drive's refusal (§188)
            print(
                f"word {ev.id} {call.skill} refused: it needs the depth model (the map has only the sensor)"
            )
            await self._send(self.link.failed(ev.id, "stale", ev.thing or ""))
            return
        driver = LeaseDriver(self.link, ev.id)
        keep = asyncio.Event()
        w = _Word(ev.id, driver, keep, asyncio.create_task(driver.run(keep)))  # the first frame at once
        w.task = asyncio.create_task(self._run_word(w, call, ev.thing or ""))
        if self.publisher is not None:
            self.publisher.word(call.skill, call.word, ev.id)
            area_m, centre = call.area_m, call.centre
            self.publisher.area = (
                (centre[0], centre[1], area_m) if area_m is not None and centre is not None else None
            )
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
        w.silent, w.why = True, why
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
            if w.why == "caught":  # the bridge caught the person: not an error, the program goes on
                self.last_result = Result.CAUGHT
            if self._word is w:
                self._word = None
            if self.state == "driving":
                self.state = "idle"
            if w.silent and not self._closing:  # ended or lost: the home still keeps what the word learned
                self._save_home()
        if w.why != "caught":
            self.last_result = result
        reason = wire_reason(result)
        if self.publisher is not None and w.silent:  # ended by the bridge or lost: back to ready
            self.publisher.word(None, None, None)
        elif self.publisher is not None:
            self.publisher.ended(reason or "done")
        if w.silent:
            return
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
            views = [v for v in (self.view, self.publisher) if v is not None]
            runner = SkillRunner(
                self.world,
                _OnePuller(self.perception, self._lock),
                w.driver,
                log,
                view=TickFan(views) if views else None,
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
        self._seq += 1  # the session's own number first: a restarted bridge reuses skill ids
        w = self.world
        return word_log(self.record / f"word-{self._seq}-{id}", call, seed, w.lim, w.cfg.cost, w.memory)

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

    def _game_person(self) -> dict[str, Any] | None:
        """The person as the bridge gets it, while its game mode is on (the GAMES card's distance)."""
        now = self.perception.bridge_now_s()
        if not self.stream.on or now is None:
            return None
        return person_message(self.world.person, round(now * 1e6))

    async def _send_person(self, msg: dict[str, Any]) -> None:
        await self._send(self.link.person(msg))

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
        if self._earlier is not None:
            self.places = Places(self._earlier.places)

    def _save_home(self) -> None:
        if self.home is None or not self._connected_once:  # a session that never connected learned nothing
            return
        from conqueror_ai.app.drive import end_home

        w = self.world
        print(
            end_home(
                self._home_store(),
                self.home,
                self._earlier,
                w.map_run.room,
                w.memory,
                w.run_id,
                self.places.all(),
            )
        )

    def _room_world(self) -> RoomWorld:
        """What the ROOM tab reads (LLD §23.4): the map now, the floor seen before, the remembered things, the
        words the things list knows."""
        run = self.world.map_run
        return RoomWorld(
            run.snapshot(0, False),
            run.room.seen_before,
            self.world.memory.of_kind(lambda _: True),
            {str(r["word"]) for r in self.names.page_list()},
        )


def _said_if_failed(task: asyncio.Task[None]) -> None:
    """The /ai pages' feed server ended with an error (port 8770 taken: another cq-drive serve) - said at once
    (final review M6); the words still work, only the /ai page has no AI feed."""
    if not task.cancelled() and task.exception() is not None:
        print(f"the AI feed for the /ai page could not start: {task.exception()} (try --page-port)")


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
    ap.add_argument(
        "--page-port",
        type=int,
        default=PAGE_PORT,
        metavar="N",
        help=f"serve the /ai pages' AI feed on this port (default {PAGE_PORT}; 0: off)",
    )
    a = ap.parse_args(argv)
    rec = None
    try:
        app, rec = _build(a.host, a.home, a.record, not a.no_depth, a.view, a.page_port)
        asyncio.run(app.run(asyncio.Event()))
    except KeyboardInterrupt:
        print("\nstopped (Ctrl-C)")
    except SkillBusy as e:
        raise SystemExit(f"another skill server is connected to the tank ({e})") from None
    except ThingsRefused as e:
        raise SystemExit(f"the tank refused the things list ({e})") from None
    except (FileNotFoundError, ValueError) as err:  # a HomeError, a bad config: said plainly
        raise SystemExit(str(err)) from None
    finally:
        if rec is not None:
            rec.close()


def _build(
    host: str, home: str | None, record: Path | None, use_depth: bool, view: bool, page_port: int
) -> tuple[ServeApp, SessionRecorder | None]:
    from conqueror_ai.adapters import calibration_store
    from conqueror_ai.adapters.bridge_ws import BridgeLink
    from conqueror_ai.adapters.detector_config import load_ball_names, load_balls
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
        TAUGHT,
        VGA,
        detector_words,
        detectors,
        load_depth,
    )
    from conqueror_ai.app.perception import Perception
    from conqueror_ai.app.seeing_pass import PASS_FILE

    model, vocab = detector_words()  # the taught words too, when their model is made (§22.4)
    names = Names(vocab, load_ball_names(BALLS))
    cal = calibration_store.load(CALIBRATION, VGA)
    det, floor = detectors(model, vocab, BALLS)
    sizes = {f"ball:{b.name}": b.size_cm for b in load_balls(BALLS) if b.size_cm is not None}
    lim, _ = load_drive(DRIVE)
    depth = load_depth(DEPTH_MODEL) if use_depth else None
    world = World(cal, load_map_config(MAP_CONFIG), depth, floor, sizes, lim, None)
    rec = None
    if record is not None:
        when = datetime.datetime.now().isoformat(timespec="seconds")
        rec = SessionRecorder(record, {"host": host, "what": "cq-drive serve", "date": when})
    feed = make_feed(page_port, names.page_list)  # the /ai pages (§22); 0: off
    on_frame = feed[1].on_frame if feed is not None else None
    perception = Perception(BridgeLink(host), MjpegSource(host), world, det, depth, rec, on_frame)
    tick_view = None
    if view:
        try:
            tick_view = live_view()
        except Exception as e:  # a viewer that won't start never stops the session
            print(f"the viewer did not start ({e}); serving without it")
    link = SkillLink(f"ws://{host}/ws", names.wire(), page_port=page_port or None)
    seeing = SeeingPass(PASS_FILE, MODEL.name, cal.date)  # games only after the seeing test (§28.6)
    app = ServeApp(link, perception, world, names, home, record, view=tick_view, feed=feed, seeing=seeing)
    app.ball_names = load_ball_names(BALLS)
    app.make_detector = lambda m, v: detectors(m, v, BALLS)

    async def installed() -> None:  # the new model is in taught/: the same choice as at start (§22.4)
        m, v = detector_words()
        await app.apply_words(v, m)

    app.teacher = Teacher(TaughtWords(TAUGHT), run_export, learning=app.on_learning, installed=installed)
    return app, rec
