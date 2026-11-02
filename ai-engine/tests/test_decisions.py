"""The decisions log and replay (AI engine LLD §19.6, review F8; Plan Z7 Review Focus 4)."""

from __future__ import annotations

import random
from pathlib import Path
from typing import Any

import numpy as np

from conqueror_ai.adapters.decisions import DecisionLog, pack, read_decisions, replay, unpack
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.roam import RoamSkill
from conqueror_ai.domain.drive.sim import SimRoom, SimTank
from conqueror_ai.domain.drive.skill import Skill, SkillInput, SkillOut, make_input
from conqueror_ai.domain.room_map import RoomMap
from conqueror_ai.domain.types import Pose

MISSION: dict[str, Any] = {"action": "wander", "thing": None, "seconds": None, "area_m": 1.5}


def make(head: dict[str, Any], seed: int) -> Skill:
    return RoamSkill(Planner(Limits()), "wander", rng=random.Random(seed), area_m=1.5)


def logged_run(path: Path, seed: int = 3, ticks: int = 80) -> None:
    """A short simulated wander, every tick logged as cq-drive logs it."""
    sim, room, tank = SimRoom([], walls_m=2.0), RoomMap(), SimTank()
    skill, log = make(MISSION, seed), DecisionLog(path, MISSION, seed)
    for k in range(ticks):
        at, t_us = tank.at, k * 100_000
        room.update_picture(sim.marks(at), t_us)
        pose = Pose(t_us, at[0], at[1], at[2], 0.0, 0.0)
        snap = room.snapshot(t_us, pose, False, True)
        inp = make_input(at=at, snap=snap, map_ok=True, area=(0.0, 0.0, 1.5), sonar_m=sim.sonar(at))
        out = skill.tick(k * 0.1, inp)
        log.tick(k * 0.1, inp, out, skill.state, skill.debug())
        tank.step(out.cmd, 0.1)
    log.close()


def test_a_snapshot_round_trips() -> None:
    a = np.random.default_rng(1).random((160, 160)) > 0.7
    assert np.array_equal(unpack(pack(a)), a)


def test_a_logged_sim_run_replays_identically(tmp_path: Path) -> None:
    logged_run(tmp_path / "decisions.jsonl")
    head, ticks = read_decisions(tmp_path / "decisions.jsonl")
    assert head["mission"] == MISSION and head["seed"] == 3 and len(ticks) == 80
    report = replay(tmp_path / "decisions.jsonl", make)
    assert report.first_diff is None and report.states[0][1] == "start"


class Changed:
    """The same skill, its commands changed from tick 5 on."""

    def __init__(self, inner: Skill) -> None:
        self.inner, self.n = inner, 0

    @property
    def state(self) -> str:
        return self.inner.state

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        out, self.n = self.inner.tick(now_s, inp), self.n + 1
        return (
            out if self.n <= 5 else SkillOut(out.cmd.__class__(out.cmd.v_cms + 1.0, out.cmd.w_dps), out.cam)
        )

    def cancel(self) -> SkillOut:
        return self.inner.cancel()

    def debug(self) -> dict[str, object]:
        return self.inner.debug()


def test_a_changed_skill_shows_the_first_differing_tick(tmp_path: Path) -> None:
    logged_run(tmp_path / "decisions.jsonl")
    report = replay(tmp_path / "decisions.jsonl", lambda head, seed: Changed(make(head, seed)))
    assert report.first_diff == 5


def test_cq_drive_replay_reports_identical(tmp_path: Path) -> None:
    """`cq-drive --replay DIR`: the skill rebuilt from the logged mission, limits, cost settings and seed."""
    from conqueror_ai.app.drive import replay_text

    logged_run(tmp_path / "decisions.jsonl")
    text = replay_text(tmp_path)
    assert "identical" in text and "start" in text


def test_a_cut_off_last_line_or_no_log_is_said_plainly(tmp_path: Path) -> None:
    """Plan Z7 final review minor 7: a run killed mid-write leaves half a line; a run that never connected
    has no log. Both are said, not a traceback."""
    from conqueror_ai.app.drive import replay_text

    logged_run(tmp_path / "decisions.jsonl", ticks=20)
    with (tmp_path / "decisions.jsonl").open("a") as f:
        f.write('{"now_s": 2.0, "inp": {"at"')
    assert "identical" in replay_text(tmp_path)
    assert "no decisions.jsonl" in replay_text(tmp_path / "nothing")


def test_a_find_replays_identically(tmp_path: Path) -> None:
    """Plan Z7 final review minor 9: a find (look round, a candidate checked, the chase) logged and replayed,
    with the real config's cost settings and an area, as cq-drive logs it."""
    from conqueror_ai.domain.drive.costmap import CostSettings

    settings = CostSettings()
    mission: dict[str, Any] = {"action": "find", "thing": "ball", "seconds": None, "area_m": 2.0}

    def make_find(head: dict[str, Any], seed: int) -> Skill:
        return RoamSkill(
            Planner(Limits()),
            "find",
            rng=random.Random(seed),
            area_m=2.0,
            cost=settings,
            kind=lambda w: w == "ball",
        )

    sim, room, tank = SimRoom([], walls_m=2.5), RoomMap(), SimTank()
    skill, log = make_find(mission, 5), DecisionLog(tmp_path / "decisions.jsonl", mission, 5)
    for k in range(400):
        at, t_us = tank.at, k * 100_000
        room.update_picture(sim.marks(at), t_us)
        pose = Pose(t_us, at[0], at[1], at[2], 0.0, 0.0)
        snap = room.snapshot(t_us, pose, False, True)
        maybes = [(1.4, 0.8)] if 50 <= k < 60 else []
        inp = make_input(
            at=at,
            snap=snap,
            map_ok=True,
            area=(0.0, 0.0, 2.0),
            settings=settings,
            sonar_m=sim.sonar(at),
            maybes=maybes,
        )
        out = skill.tick(k * 0.1, inp)
        log.tick(k * 0.1, inp, out, skill.state, skill.debug())
        tank.step(out.cmd, 0.1)
        if out.result:
            break
    log.close()
    report = replay(tmp_path / "decisions.jsonl", make_find)
    assert report.first_diff is None and "check" in [s for _, s in report.states]


def test_a_find_with_a_remembered_spot_replays_identically(tmp_path: Path) -> None:
    """Plan Z7 final review minor 3: a find checks the remembered spots of its word first; the replay must
    start from the same memory, which the log's first line keeps."""
    from conqueror_ai.app.drive import replay_text, start_memory
    from conqueror_ai.domain.memory import ThingMemory
    from conqueror_ai.domain.types import ObjectEstimate

    mem = ThingMemory()
    mem.update([ObjectEstimate(1, "ball", 1.2, 0.4, 0.0, 0.0, 0.1, 0, 0, "seen")], 100.0, "earlier")
    mission: dict[str, Any] = {"action": "find", "thing": "ball", "seconds": None, "area_m": None}
    from conqueror_ai.app.drive import Mission, make_skill

    skill = make_skill(Mission("find", "ball", None, None), Limits(), None, mem, 4)
    log = DecisionLog(tmp_path / "decisions.jsonl", mission, 4)
    log.head(remembered=start_memory(mem))
    room, tank = RoomMap(), SimTank()
    room.seen[:] = True
    for k in range(60):
        t_us = k * 100_000
        pose = Pose(t_us, *tank.at, 0.0, 0.0)
        inp = make_input(at=tank.at, snap=room.snapshot(t_us, pose, False, True), map_ok=True)
        out = skill.tick(k * 0.1, inp)
        log.tick(k * 0.1, inp, out, skill.state, skill.debug())
        tank.step(out.cmd, 0.1)
    log.close()
    assert "identical" in replay_text(tmp_path)


async def test_a_home_run_replays_identically(tmp_path: Path) -> None:
    """Plan Z8: a recorded --home run (a loaded obstacle, a remembered spot) replays decision by decision."""
    from conqueror_ai.adapters.session import SessionRecorder
    from conqueror_ai.app.drive import replay_text
    from tests.test_drive_app import home_run, stored_home

    rec = SessionRecorder(tmp_path / "rec", {"what": "test"})
    await home_run(stored_home(tmp_path / "homes"), rec=rec)
    rec.close()
    assert "identical" in replay_text(tmp_path / "rec")
