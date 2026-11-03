"""The engine's WebSocket as the bridge's skill server (AI engine LLD §21.5, bridge LLD R26.2).

`hello ai-skills` (no things), then the things in `skill_things` parts; the bridge sends `skill_ask`,
`skill_start` and `skill_end`, the engine answers with `skill_answer`, `skill_drive`, `skill_done` and
`skill_failed`. Pings keep the link alive (the bridge drops a client silent for 3 s, design-notes §142).
The link never decides to drive: `LeaseDriver` does, and only while a `skill_start` lease is held."""

from __future__ import annotations

import asyncio
import contextlib
import json
import time
from collections.abc import AsyncIterator, Sequence
from dataclasses import dataclass
from typing import Any

from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import ConnectionClosed, WebSocketException

from conqueror_ai.adapters.drive_link import drive_msg
from conqueror_ai.domain.drive.curves import Command

FRAME_MAX = 190  # the bridge's WS path holds 200-byte messages (R26.2)
PARTS_MAX = 48
THING_MAX_BYTES = 24  # skill_failed's thing, as the bridge bounds it
PING_S = 1.0
HELLO_WAIT_S = 3.0


class SkillBusy(Exception):
    """The bridge already has a skill server (`nack BUSY`)."""


class ThingsRefused(Exception):
    """The bridge refused the things list (`nack TOO_BIG` / `BAD_FIELD`); it keeps the list it had."""


@dataclass(frozen=True)
class Ask:
    id: int
    q: str  # "see?" or "howfar"
    thing: str


@dataclass(frozen=True)
class Start:
    id: int
    skill: str  # find, wander, explore, setxy
    thing: str | None
    seconds: float | None
    to: tuple[float, float] | None  # setxy: (fwd_cm, left_cm), relative to the tank now
    area: tuple[float, float, float] | None  # (r_m, fwd_cm, left_cm): the centre relative to the tank now


@dataclass(frozen=True)
class End:
    id: int
    why: str  # stop, deadman, page, owner, ended


@dataclass(frozen=True)
class Lost:
    """The link closed: the bridge ends the program with 7052 by its own rule."""


SkillEvent = Ask | Start | End | Lost


def _frame(part: int, of: int, entries: list[dict[str, object]], id: int | None = None) -> str:
    m: dict[str, object] = {"t": "skill_things", "part": part, "of": of}
    if id is not None:
        m["id"] = id  # the bridge acks (or nacks) a frame that has one
    m["things"] = entries
    return json.dumps(m, separators=(",", ":"))


def things_parts(things: Sequence[dict[str, object]], first_id: int | None = None) -> list[str]:
    """The list as `skill_things` frames, each at most FRAME_MAX bytes. A word that doesn't fit whole is
    split by its names (it appears again in a later part, which adds names). With `first_id`, the parts
    carry the ids first_id, first_id + 1, ..."""
    worst = (PARTS_MAX, PARTS_MAX)  # measure with two-digit part / of
    groups: list[list[dict[str, object]]] = [[]]

    def fits(entries: list[dict[str, object]]) -> bool:
        return len(_frame(*worst, entries, 99999999).encode()) <= FRAME_MAX

    for t in things:
        word = str(t["word"])
        names = [str(n) for n in t.get("names", [])]  # type: ignore[attr-defined]
        whole: dict[str, object] = {"word": word, "names": names}
        if fits([whole]):
            if not fits([*groups[-1], whole]):
                groups.append([])
            groups[-1].append(whole)
            continue
        chunk: list[str] = []
        for n in names:
            if not fits([{"word": word, "names": [*chunk, n]}]):
                if not chunk:
                    raise ValueError(f"the word '{word}' and its name '{n}' do not fit one frame")
                groups.append([{"word": word, "names": chunk}])
                chunk = []
            chunk.append(n)
        groups.append([{"word": word, "names": chunk}])
    if len(groups) > PARTS_MAX:
        raise ValueError(f"the things need {len(groups)} parts; the tank takes at most {PARTS_MAX}")
    return [
        _frame(i + 1, len(groups), g, None if first_id is None else first_id + i)
        for i, g in enumerate(groups)
    ]


def _num(m: dict[str, Any], key: str) -> float | None:
    v = m.get(key)
    return float(v) if isinstance(v, int | float) and not isinstance(v, bool) else None


def parse_event(m: dict[str, Any]) -> SkillEvent | None:
    """One bridge message as an event, or None for any other (and for a malformed one)."""
    kind, mid = m.get("t"), m.get("id")
    if not isinstance(mid, int) or isinstance(mid, bool):
        return None
    if kind == "skill_ask":
        q, thing = m.get("q"), m.get("thing")
        return Ask(mid, q, thing) if isinstance(q, str) and isinstance(thing, str) else None
    if kind == "skill_end":
        why = m.get("why")
        return End(mid, why) if isinstance(why, str) else None
    if kind == "skill_start":
        skill = m.get("skill")
        if not isinstance(skill, str):
            return None
        thing = m.get("thing")
        to: tuple[float, float] | None = None
        raw_to, raw_area = m.get("to"), m.get("area")
        if isinstance(raw_to, dict):
            f, lf = _num(raw_to, "fwd_cm"), _num(raw_to, "left_cm")  # pyright: ignore[reportUnknownArgumentType]
            to = (f, lf) if f is not None and lf is not None else None
        area: tuple[float, float, float] | None = None
        if isinstance(raw_area, dict):
            r, f, lf = (_num(raw_area, k) for k in ("r_m", "fwd_cm", "left_cm"))  # pyright: ignore[reportUnknownArgumentType]
            area = (r, f, lf) if r is not None and f is not None and lf is not None else None
        return Start(mid, skill, thing if isinstance(thing, str) else None, _num(m, "s"), to, area)
    return None


class SkillLink:
    def __init__(self, url: str, things: Sequence[dict[str, object]], silent_s: float = 3.0) -> None:
        self.url = url
        self.things = things
        self.last_nack: str | None = None  # the reason of the bridge's latest nack (TOO_BIG, BAD_FIELD...)
        self._ws: ClientConnection | None = None
        self._queue: asyncio.Queue[SkillEvent] = asyncio.Queue()
        self.silent_s = silent_s  # nothing heard from the bridge for this long: the link is lost
        self._tasks: list[asyncio.Task[None]] = []
        self._next_id = 0
        self._waiting: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._heard_s = time.monotonic()
        self._lost = False

    @property
    def connected(self) -> bool:
        return self._ws is not None

    async def connect(self) -> None:
        """Opens the link, registers as the skill server and sends the things. SkillBusy on `nack BUSY`,
        ThingsRefused on a refused list; every link failure (websockets' own errors too) is a
        ConnectionError."""
        things_parts(self.things)  # a list that cannot be sent is refused before connecting
        try:
            ws = await connect(self.url, ping_interval=None, open_timeout=5)
        except WebSocketException as e:  # a refused or broken handshake: one error type for every caller
            raise ConnectionError(f"the bridge refused the link: {e}") from e
        try:
            await ws.send(json.dumps({"t": "hello", "client": "ai-skills", "proto": 1}))
            async with asyncio.timeout(HELLO_WAIT_S):
                while True:
                    raw = await ws.recv()
                    m = _load(raw)
                    if m is None:
                        continue
                    if m.get("t") == "nack":
                        if m.get("reason") == "BUSY":
                            raise SkillBusy(str(m.get("message", "a skill server is already connected")))
                        raise ConnectionError(f"the bridge refused the hello: {m.get('reason')}")
                    if m.get("t") == "welcome":
                        break
        except WebSocketException as e:  # the socket closed during the hello (WiFi)
            await ws.close()
            raise ConnectionError(f"the bridge link closed during the hello: {e}") from e
        except BaseException:
            await ws.close()
            raise
        self._ws, self._lost, self._heard_s = ws, False, time.monotonic()
        self._queue = asyncio.Queue()  # a new session: nothing from the old socket
        self._tasks = [asyncio.create_task(self._read(ws)), asyncio.create_task(self._pings(ws))]
        try:
            await self.set_things(self.things)
        except BaseException:
            await self.close()
            raise

    async def events(self) -> AsyncIterator[SkillEvent]:
        """The bridge's questions, starts and ends, in order; ends after Lost."""
        while True:
            ev = await self._queue.get()
            yield ev
            if isinstance(ev, Lost):
                return

    async def set_things(self, things: Sequence[dict[str, object]]) -> None:
        """Sends the list in parts and waits for the bridge's ack of each; ThingsRefused on a nack (the
        bridge keeps its old list). Call it again when the list changes (Plan Z2b)."""
        parts = things_parts(things, self._next_id + 1)
        # the parts' ids are reserved before any wait: a cam_set sent meanwhile takes the next one
        self._next_id += len(parts)
        for raw in parts:
            mid = int(json.loads(raw)["id"])
            reply: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()
            self._waiting[mid] = reply
            try:
                await self._send_raw(raw)
                async with asyncio.timeout(HELLO_WAIT_S):
                    m = await reply
            except TimeoutError:
                raise ConnectionError("the bridge did not answer the things list") from None
            finally:
                self._waiting.pop(mid, None)
            if m.get("t") == "nack":
                raise ThingsRefused(f"{m.get('reason')}: {m.get('message', '')}")
        self.things = things

    async def close(self) -> None:
        ws, self._ws = self._ws, None
        for t in self._tasks:
            t.cancel()
        await asyncio.gather(*self._tasks, return_exceptions=True)
        self._tasks = []
        if ws is not None:
            await ws.close()

    async def answer(self, id: int, value: bool | int) -> None:
        await self._send({"t": "skill_answer", "id": id, "value": value})

    async def done(self, id: int) -> None:
        await self._send({"t": "skill_done", "id": id})

    async def failed(self, id: int, reason: str, thing: str) -> None:
        thing = thing.encode()[:THING_MAX_BYTES].decode("utf-8", "ignore")
        await self._send({"t": "skill_failed", "id": id, "reason": reason, "thing": thing})

    async def drive(self, id: int, cmd: Command) -> None:
        m = drive_msg(cmd)
        await self._send({"t": "skill_drive", "id": id, "v": m["v"], "yaw_rate": m["yaw_rate"]})

    async def cam(self, pan_deg: float, tilt_deg: float) -> None:
        """The camera, allowed to the lease holder (bridge LLD R26.1). Not waited for."""
        self._next_id += 1
        await self._send(
            {"t": "cam_set", "pan": round(pan_deg, 1), "tilt": round(tilt_deg, 1), "id": self._next_id}
        )

    async def _send(self, msg: dict[str, Any]) -> None:
        await self._send_raw(json.dumps(msg, separators=(",", ":")))

    async def _send_raw(self, raw: str) -> None:
        """Every send ends as a ConnectionError when the link is gone: between the socket closing and the
        reader's clean-up, websockets raises its own ConnectionClosed (review I1)."""
        ws = self._ws
        if ws is None:
            raise ConnectionError("not connected to the bridge")
        try:
            await ws.send(raw)
        except ConnectionClosed as e:
            raise ConnectionError("the bridge link closed") from e

    async def _read(self, ws: ClientConnection) -> None:
        try:
            async for raw in ws:
                m = _load(raw)
                if m is None:
                    continue
                self._heard_s = time.monotonic()
                mid = m.get("id")
                if m.get("t") == "ping":
                    await ws.send(json.dumps({"t": "pong", "t0": m.get("t0")}))
                elif m.get("t") in ("ack", "nack"):
                    if m.get("t") == "nack":
                        self.last_nack = str(m.get("reason") or "refused")
                    fut = self._waiting.get(mid) if isinstance(mid, int) else None
                    if fut is not None and not fut.done():
                        fut.set_result(m)
                elif (ev := parse_event(m)) is not None:
                    self._queue.put_nowait(ev)
        except ConnectionClosed:
            pass
        finally:
            self._lose()
            for t in self._tasks:
                if t is not asyncio.current_task():
                    t.cancel()

    def _lose(self) -> None:
        """The link is gone (closed, or silent): the events end with one Lost; waiting sends give up."""
        self._ws = None
        if not self._lost:
            self._lost = True
            self._queue.put_nowait(Lost())
        for fut in self._waiting.values():
            if not fut.done():
                fut.set_exception(ConnectionError("the bridge link closed"))

    async def _pings(self, ws: ClientConnection) -> None:
        with contextlib.suppress(ConnectionClosed):
            while True:
                await ws.send(json.dumps({"t": "ping", "t0": round(time.monotonic() * 1000, 3)}))
                await asyncio.sleep(min(PING_S, self.silent_s / 2))
                if time.monotonic() - self._heard_s > self.silent_s:  # WiFi lost, the socket still open
                    self._lose()
                    ws.transport.abort()
                    return


def _load(raw: str | bytes) -> dict[str, Any] | None:
    try:
        m: Any = json.loads(raw if isinstance(raw, str) else raw.decode("utf-8", "replace"))
    except ValueError:
        return None
    return m if isinstance(m, dict) else None  # pyright: ignore[reportUnknownVariableType]
