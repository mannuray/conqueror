"""Ticks one skill on a run (AI engine LLD §19.5, review F1 / F9): the World's input each picture or TICK_S,
the skill off the event loop, the command through a Driver, a line to the decisions log and a status. Always
ends with the driver's finish (a stop sent, control handed back)."""

from __future__ import annotations

import asyncio
import time
from collections.abc import Callable
from typing import Protocol

from conqueror_ai.domain.drive.curves import Command
from conqueror_ai.domain.drive.skill import Result, Skill, SkillInput, SkillOut
from conqueror_ai.domain.ports import Driver

TICK_S = 0.2  # tick at least this often, with or without a new picture


class TickLog(Protocol):
    def tick(
        self, now_s: float, inp: SkillInput, out: SkillOut, state: str, debug: dict[str, object]
    ) -> None: ...


class TickView(Protocol):
    """Draws a tick (the viewer, Plan Z8): called after the command is sent, never before it."""

    def tick(
        self, now_s: float, inp: SkillInput, out: SkillOut, state: str, debug: dict[str, object]
    ) -> None: ...


class Pictures(Protocol):
    t0: float

    async def next_picture(self, timeout_s: float) -> object: ...

    def state(self, now: float) -> str | None: ...

    def failure(self) -> str | None: ...


class Inputs(Protocol):
    def input(self, now_s: float, wall_s: float, map_ok: bool) -> SkillInput: ...


class SkillRunner:
    def __init__(
        self,
        world: Inputs,
        perception: Pictures,
        driver: Driver,
        log: TickLog | None = None,
        on_status: Callable[[str, Command], None] | None = None,
        tick_s: float = TICK_S,
        view: TickView | None = None,
    ) -> None:
        self.world, self.perception, self.driver = world, perception, driver
        self.log, self.on_status, self.tick_s = log, on_status, tick_s
        self.finish_error: Exception | None = None  # the stop message failed (the deadman stops the tank)
        self.view = view
        self.view_errors = 0  # a broken viewer never ends a run (counted, said by the CLI)

    async def run(self, skill: Skill) -> tuple[Result, str]:
        """Until the skill ends, the tank refuses, or a link fails: the result and its detail (the refusal,
        the failure, why the map went stale)."""
        t0 = self.perception.t0
        try:
            while True:
                if self.driver.refused is not None:
                    return Result.REFUSED, self.driver.refused
                if (failed := self.perception.failure()) is not None:
                    return Result.FAILED, failed
                await self.perception.next_picture(self.tick_s)
                now = time.monotonic()
                state = self.perception.state(now)
                if state == "waiting":
                    continue
                inp = self.world.input(now - t0, time.time(), state is None)
                # planning off the event loop (6-21 ms: fan, routes; Z6 review 11) - the snapshot is a copy
                out = await asyncio.to_thread(skill.tick, now - t0, inp)
                try:
                    await self.driver.drive(out.cmd)
                    if out.cam is not None:
                        await self.driver.cam(*out.cam)
                except Exception as e:  # the link went down between checks (Z7 final review minor 2)
                    return Result.FAILED, f"{type(e).__name__}: {e}"
                if self.log is not None:
                    self.log.tick(now - t0, inp, out, skill.state, skill.debug())
                if self.view is not None:
                    try:
                        self.view.tick(now - t0, inp, out, skill.state, skill.debug())
                    except Exception:
                        self.view_errors += 1
                if self.on_status is not None:
                    self.on_status(skill.state, out.cmd)
                if out.result is not None:
                    detail = out.detail or (state or "no pose" if out.result == Result.STALE else "")
                    return out.result, detail
        finally:
            try:
                await self.driver.finish()
            except Exception as e:  # the clean-up goes on: the bridge stops the tank when the link drops
                self.finish_error = e
