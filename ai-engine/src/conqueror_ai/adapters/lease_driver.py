"""The Driver port under the bridge's lease (AI engine LLD §21.5, bridge LLD R26).

Every frame is a `skill_drive` carrying the skill's id, the same speed table as `DriveLink`. The lease ends
from the tank's side (`skill_end`): `end(why)` then sets `refused` and nothing more is sent. The bridge ends
a word whose frames stop (300 ms, 7052), so `run` sends the first frame (v 0) at once and repeats the
engine's newest command every KEEP_S while it is fresh, at most one frame per MIN_GAP_S (a stop after
motion sooner, but never two frames within STOP_GAP_S: the bridge admits 25 a second). A command that
went stale is replaced by STOP frames (always safe) while the lease lives; a hung loop sends nothing and
the bridge's deadman stops the tank."""

from __future__ import annotations

import asyncio
import time
from collections.abc import Callable

from conqueror_ai.adapters.skill_link import SkillLink
from conqueror_ai.domain.drive.curves import STOP, Command
from conqueror_ai.domain.drive.skill import Result

KEEP_S = 0.1  # a frame at least this often (the bridge's deadman is 0.3 s), a held stop too
MIN_GAP_S = 0.08  # at most ~12 frames a second, as DriveLink: the bridge refuses over 25 (ws_clients.h)
FRESH_FOR_S = 0.5  # a moving command is repeated only this long; then STOP frames
STREAM_PER_S = 25  # the bridge admits this many skill_drive frames a second, a v 0 frame too (ws_clients.cpp)
STOP_GAP_S = 1.0 / STREAM_PER_S + 0.005  # a stop after motion waits only this long since the last frame
SEND_GAP_NOTE_S = 0.25  # a gap between frames over this is printed (the bridge's deadman is 0.3 s, LLD §25.6)


class LeaseDriver:
    def __init__(self, link: SkillLink, id: int, note: Callable[[str], None] = print) -> None:
        self.link, self.id, self._note = link, id, note
        self.cmd = STOP
        self._refused: str | None = None
        self._set_s = time.monotonic()  # the lease start counts as a command: the first v 0 is repeated
        self._sent_s = 0.0
        self._sent = STOP  # the last frame sent: a stop goes sooner only after a moving one
        self._pending = False  # a newer command waits for MIN_GAP_S since the last frame
        self._finished = False

    @property
    def connected(self) -> bool:
        return self.link.connected

    @property
    def refused(self) -> str | None:
        return self._refused

    def end(self, why: str) -> None:
        """The bridge ended the lease (stop, deadman, page, owner, ended): nothing more goes out."""
        self._refused = self._refused or why

    async def run(self, stop: asyncio.Event) -> None:
        """The lease's keep-alive: the first frame at once, then the newest command while it is fresh."""
        if not self._sent_s:  # unless a drive() already sent the first frame
            await self._send(STOP)
        while not stop.is_set() and self._refused is None and not self._finished:
            await asyncio.sleep(0.02)
            now = time.monotonic()
            gap = now - self._sent_s
            if self._pending and gap >= self._gap_for(self.cmd):
                await self._send(self.cmd)
            elif gap >= KEEP_S:
                # a moving command is repeated only while fresh; after that (a picture gap) STOP frames go
                # on for as long as the lease lives - STOP is always safe, and the engine's own stale rule
                # ends the word. A hung event loop sends nothing: the bridge's deadman is the backstop.
                fresh = now - self._set_s <= FRESH_FOR_S
                await self._send(self.cmd if fresh else STOP)

    async def drive(self, cmd: Command) -> None:
        if self._refused is not None or self._finished:
            return
        self.cmd = cmd
        self._set_s = time.monotonic()
        if self._set_s - self._sent_s >= self._gap_for(cmd):
            await self._send(cmd)  # a stop after motion at once, within the bridge's stream limit
        else:
            self._pending = True  # run() sends it as soon as the gap has passed

    async def cam(self, pan_deg: float, tilt_deg: float) -> None:
        if self._refused is None and not self._finished:
            try:
                await self.link.cam(pan_deg, tilt_deg)
            except ConnectionError:  # as drive: the link went down, the lease with it
                self._refused = self._refused or "lost"

    async def finish(self) -> None:
        """One `v 0` frame, once; the stop itself is the bridge's (`endSkill`). None after the lease ended."""
        self.cmd = STOP
        if self._finished:
            return
        self._finished = True
        if self._refused is None:
            await self._send(STOP, force=True)

    def _gap_for(self, cmd: Command) -> float:
        """The gap a frame waits since the last one: shorter for a stop after a moving frame."""
        return STOP_GAP_S if cmd == STOP and self._sent != STOP else MIN_GAP_S

    async def _send(self, cmd: Command, force: bool = False) -> None:
        if self._refused is not None or (self._finished and not force):
            return
        # stamped before the await: a drive() during the send waits its gap instead of sending a second frame
        now = time.monotonic()
        if self._sent_s and now - self._sent_s > SEND_GAP_NOTE_S:  # where a 7052 came from (LLD §25.6)
            self._note(f"skill link: {(now - self._sent_s) * 1000:.0f} ms between drive frames")
        self._sent_s, self._sent, self._pending = now, cmd, False
        try:
            await self.link.drive(self.id, cmd)
        except ConnectionError:
            self._refused = self._refused or "lost"


def wire_reason(r: Result, ended: bool = False) -> str | None:
    """The `skill_failed` reason for a result, None when nothing is sent: DONE (`skill_done` goes instead)
    and a CANCELLED word the bridge itself ended (`ended`: the tank already knows). Every other way a skill
    can stop without an answer (REFUSED, FAILED, STALE, a CANCELLED by the engine) is `stale` (7052)."""
    if r == Result.DONE or (r == Result.CANCELLED and ended):
        return None
    return _WIRE.get(r, "stale")


_WIRE = {
    Result.NOT_FOUND: "not_found",
    Result.LOST: "lost",
    Result.BLOCKED: "blocked",
    Result.OUTSIDE_AREA: "outside_area",
    Result.NO_WAY: "no_way",
    Result.UNKNOWN_THING: "unknown_thing",
}
