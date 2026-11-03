"""The Driver port under the bridge's lease (AI engine LLD §21.5, bridge LLD R26).

Every frame is a `skill_drive` carrying the skill's id, the same speed table as `DriveLink`. The lease ends
from the tank's side (`skill_end`): `end(why)` then sets `refused` and nothing more is sent. The bridge ends
a word whose frames stop (300 ms, 7052), so `run` sends the first frame (v 0) at once and repeats the
engine's newest command (a held stop too) every KEEP_S while it is fresh, at most one frame per MIN_GAP_S.
A stuck engine is left to that deadman."""

from __future__ import annotations

import asyncio
import time

from conqueror_ai.adapters.skill_link import SkillLink
from conqueror_ai.domain.drive.curves import STOP, Command
from conqueror_ai.domain.drive.skill import Result

KEEP_S = 0.1  # a frame at least this often (the bridge's deadman is 0.3 s), a held stop too
MIN_GAP_S = 0.08  # at most ~12 frames a second, as DriveLink: the bridge refuses over 25 (ws_clients.h)
FRESH_FOR_S = 0.5  # the keep-alive repeats a command only this long: a stuck engine is left to the deadman


class LeaseDriver:
    def __init__(self, link: SkillLink, id: int) -> None:
        self.link, self.id = link, id
        self.cmd = STOP
        self._refused: str | None = None
        self._set_s = time.monotonic()  # the lease start counts as a command: the first v 0 is repeated
        self._sent_s = 0.0
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
        await self._send(STOP)
        while not stop.is_set() and self._refused is None and not self._finished:
            await asyncio.sleep(0.02)
            now = time.monotonic()
            gap = now - self._sent_s
            # a held stop is a frame too (the lease lives on frames); only a stale command is left to die
            if (self._pending and gap >= MIN_GAP_S) or (now - self._set_s <= FRESH_FOR_S and gap >= KEEP_S):
                await self._send(self.cmd)

    async def drive(self, cmd: Command) -> None:
        if self._refused is not None or self._finished:
            return
        moving_before = self.cmd != STOP
        self.cmd = cmd
        self._set_s = time.monotonic()
        if self._set_s - self._sent_s >= MIN_GAP_S or (cmd == STOP and moving_before):
            await self._send(cmd)  # a stop always at once
        else:
            self._pending = True  # run() sends it as soon as the gap has passed

    async def cam(self, pan_deg: float, tilt_deg: float) -> None:
        if self._refused is None and not self._finished:
            await self.link.cam(pan_deg, tilt_deg)

    async def finish(self) -> None:
        """One `v 0` frame, once; the stop itself is the bridge's (`endSkill`). None after the lease ended."""
        self.cmd = STOP
        if self._finished:
            return
        self._finished = True
        if self._refused is None:
            await self._send(STOP, force=True)

    async def _send(self, cmd: Command, force: bool = False) -> None:
        if self._refused is not None or (self._finished and not force):
            return
        try:
            await self.link.drive(self.id, cmd)
        except ConnectionError:
            self._refused = self._refused or "lost"
            return
        self._sent_s = time.monotonic()
        self._pending = False


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
