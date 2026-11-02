"""SkillRunner and the Driver port (AI engine LLD §19.5, review F9; Plan Z7 Review Focus 5)."""

from __future__ import annotations

import asyncio

from conqueror_ai.app.runner import SkillRunner
from conqueror_ai.domain.drive.curves import STOP, Command
from conqueror_ai.domain.drive.skill import Result, SkillInput, SkillOut, make_input
from conqueror_ai.domain.ports import Driver
from tests.test_drive_skill import open_snap

GO = Command(20.0, 0.0)


class FakeDriver:
    def __init__(self, refuse_after: int | None = None) -> None:
        self.connected = True
        self.refused: str | None = None
        self.cmds: list[Command] = []
        self.cams: list[tuple[float, float]] = []
        self.finished = False
        self._refuse_after = refuse_after

    async def run(self, stop: asyncio.Event) -> None:
        await stop.wait()

    async def drive(self, cmd: Command) -> None:
        self.cmds.append(cmd)
        if self._refuse_after is not None and len(self.cmds) >= self._refuse_after:
            self.refused = "NOT_OWNER"

    async def cam(self, pan_deg: float, tilt_deg: float) -> None:
        self.cams.append((pan_deg, tilt_deg))

    async def finish(self) -> None:
        self.finished = True


class FakePerception:
    def __init__(self, fail_after: int | None = None) -> None:
        self.t0 = 0.0
        self.n = 0
        self._fail_after = fail_after

    async def next_picture(self, timeout_s: float) -> None:
        self.n += 1
        await asyncio.sleep(0)

    def state(self, now: float) -> str | None:
        return None

    def failure(self) -> str | None:
        return (
            "OSError: stream closed" if self._fail_after is not None and self.n >= self._fail_after else None
        )


class FakeWorld:
    def input(self, now_s: float, wall_s: float, map_ok: bool) -> SkillInput:
        return make_input(at=(0.0, 0.0, 0.0), snap=open_snap(), map_ok=map_ok)


class Scripted:
    """Drives forward; ends with `result` after `n` ticks (None: never)."""

    def __init__(self, n: int | None = None, result: Result = Result.DONE) -> None:
        self.state, self.ticks, self._n, self._result = "going", 0, n, result

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        self.ticks += 1
        if self._n is not None and self.ticks >= self._n:
            return SkillOut(STOP, None, self._result)
        return SkillOut(GO, (0.0, -15.0) if self.ticks == 1 else None)

    def cancel(self) -> SkillOut:
        return SkillOut(STOP, None, Result.CANCELLED)

    def debug(self) -> dict[str, object]:
        return {"state": self.state}


class Log:
    def __init__(self) -> None:
        self.lines: list[tuple[float, str]] = []

    def tick(
        self, now_s: float, inp: SkillInput, out: SkillOut, state: str, debug: dict[str, object]
    ) -> None:
        self.lines.append((now_s, state))


def runner(
    driver: FakeDriver, perception: FakePerception, log: Log | None = None, status: list[str] | None = None
) -> SkillRunner:
    def on(state: str, cmd: Command) -> None:
        if status is not None:
            status.append(state)

    return SkillRunner(FakeWorld(), perception, driver, log, on, tick_s=0.01)


def test_the_drive_link_is_a_driver() -> None:
    from conqueror_ai.adapters.drive_link import DriveLink

    assert isinstance(DriveLink("127.0.0.1"), Driver) and isinstance(FakeDriver(), Driver)


async def test_a_refusal_ends_refused_and_finishes() -> None:
    d = FakeDriver(refuse_after=3)
    result, detail = await runner(d, FakePerception()).run(Scripted())
    assert result == Result.REFUSED and detail == "NOT_OWNER" and d.finished


async def test_a_failed_link_ends_failed_and_finishes() -> None:
    d = FakeDriver()
    result, detail = await runner(d, FakePerception(fail_after=2)).run(Scripted())
    assert result == Result.FAILED and "stream closed" in detail and d.finished


async def test_the_skills_result_is_passed_through() -> None:
    d = FakeDriver()
    result, _ = await runner(d, FakePerception()).run(Scripted(n=4, result=Result.NOT_FOUND))
    assert (
        result == Result.NOT_FOUND and d.finished and d.cmds[:3] == [GO, GO, GO] and d.cams == [(0.0, -15.0)]
    )


async def test_a_status_and_a_log_line_each_tick() -> None:
    log, status = Log(), list[str]()
    await runner(FakeDriver(), FakePerception(), log, status).run(Scripted(n=5))
    assert len(log.lines) == 5 and len(status) == 5


class BrokenDriver(FakeDriver):
    async def drive(self, cmd: Command) -> None:
        await super().drive(cmd)
        if len(self.cmds) > 3:
            raise ConnectionError("not connected to the bridge")


async def test_a_driver_error_ends_failed_not_a_traceback() -> None:
    """Plan Z7 final review minor 2: the link dropping while the runner waited for a picture raised out of
    drive() past failure(); cq-drive showed a traceback. Now it ends FAILED (and still finishes)."""
    d = BrokenDriver()
    result, detail = await runner(d, FakePerception()).run(Scripted())
    assert result == Result.FAILED and "not connected" in detail and d.finished
