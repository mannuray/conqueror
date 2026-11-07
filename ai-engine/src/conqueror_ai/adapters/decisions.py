"""Every drive run's decisions (AI engine LLD §19.6, review F8): `decisions.jsonl` beside the recording, one
line a tick - the whole SkillInput (the snapshot's layers packed), the SkillOut, the skill's state and debug
notes - and a replay that feeds the logged inputs to a fresh skill and finds the first tick it decides
differently. Local only, like the recordings (they show the home)."""

from __future__ import annotations

import base64
import json
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import IO, Any, Protocol, cast

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.drive.costmap import CostSettings
from conqueror_ai.domain.drive.costmaps import CostMaps
from conqueror_ai.domain.drive.curves import Command
from conqueror_ai.domain.drive.goal import TargetFix
from conqueror_ai.domain.drive.skill import Result, Skill, SkillInput, SkillOut
from conqueror_ai.domain.objects import Glimpse
from conqueror_ai.domain.room_map import MapSnapshot
from conqueror_ai.domain.types import Pose


def pack(a: NDArray[np.bool_]) -> dict[str, Any]:
    return {"shape": list(a.shape), "bits": base64.b64encode(np.packbits(a).tobytes()).decode()}


def unpack(d: dict[str, Any]) -> NDArray[np.bool_]:
    shape = tuple(int(n) for n in d["shape"])
    bits = np.frombuffer(base64.b64decode(d["bits"]), np.uint8)
    return np.unpackbits(bits)[: int(np.prod(shape))].reshape(shape).astype(bool)


def _snap_out(s: MapSnapshot) -> dict[str, Any]:
    p = s.pose
    return {
        "t_us": s.t_us,
        "pose": None if p is None else [p.t_us, p.x_m, p.y_m, p.th_rad, p.v_mps, p.w_rps],
        "cell_m": s.cell_m,
        "blocked": pack(s.blocked),
        "high": pack(s.high),
        "seen": pack(s.seen),
        "stale": s.stale,
        "depth_ok": s.depth_ok,
        "angle_trusted": s.angle_trusted,
        "driven": None if s.driven is None else pack(s.driven),
    }


def _snap_in(d: dict[str, Any]) -> MapSnapshot:
    p = d["pose"]
    pose = (
        None
        if p is None
        else Pose(int(p[0]), float(p[1]), float(p[2]), float(p[3]), float(p[4]), float(p[5]))
    )
    return MapSnapshot(
        int(d["t_us"]),
        pose,
        float(d["cell_m"]),
        unpack(d["blocked"]),
        unpack(d["high"]),
        unpack(d["seen"]),
        bool(d["stale"]),
        bool(d["depth_ok"]),
        bool(d["angle_trusted"]),
        None if d.get("driven") is None else unpack(d["driven"]),  # none in a log from before Plan Z10d
    )


def _input_out(inp: SkillInput) -> dict[str, Any]:
    c = inp.costs
    return {
        "at": None if inp.at is None else list(inp.at),
        "snap": None if inp.snap is None else _snap_out(inp.snap),
        "area": None if c is None or c.area is None else list(c.area),
        "settings": None if c is None or c.settings is None else asdict(c.settings),
        "fix": None if inp.fix is None else asdict(inp.fix),
        "sonar_m": inp.sonar_m,
        "map_ok": inp.map_ok,
        "cam_still": inp.cam_still,
        "used": inp.used,
        "maybes": [list(m) for m in inp.maybes],
        "fresh": inp.fresh,
        "glimpses": [[g.cls, g.at[0], g.at[1], g.lo_rad, g.hi_rad, g.t_us] for g in inp.glimpses],
    }


def _input_in(d: dict[str, Any]) -> SkillInput:
    snap = None if d["snap"] is None else _snap_in(d["snap"])
    area = None if d["area"] is None else cast("tuple[float, float, float]", tuple(d["area"]))
    settings = None if d["settings"] is None else CostSettings(**d["settings"])
    at = None if d["at"] is None else cast("tuple[float, float, float]", tuple(d["at"]))
    fix = None if d["fix"] is None else TargetFix(**d["fix"])
    maybes = tuple(cast("tuple[float, float]", tuple(m)) for m in d["maybes"])
    costs = None if snap is None else CostMaps(snap, settings, area)
    fresh = d.get("fresh", d["used"])  # a log from before Plan Z10: a used picture was the only fresh one
    glimpses = tuple(  # none in a log from before Plan Z10
        Glimpse(str(c), (float(x), float(y)), float(lo), float(hi), int(t))
        for c, x, y, lo, hi, t in d.get("glimpses", [])
    )
    return SkillInput(
        at, snap, costs, fix, d["sonar_m"], d["map_ok"], d["cam_still"], d["used"], maybes, fresh, glimpses
    )


def _out_out(o: SkillOut) -> dict[str, Any]:
    return {
        "cmd": [o.cmd.v_cms, o.cmd.w_dps],
        "cam": None if o.cam is None else list(o.cam),
        "result": None if o.result is None else str(o.result),
        "detail": o.detail,
    }


def _out_in(d: dict[str, Any]) -> SkillOut:
    cam = None if d["cam"] is None else cast("tuple[float, float]", tuple(d["cam"]))
    result = None if d["result"] is None else Result(d["result"])
    return SkillOut(Command(float(d["cmd"][0]), float(d["cmd"][1])), cam, result, d["detail"])


class DecisionLog:
    def __init__(self, path: Path, mission: dict[str, Any], seed: int) -> None:
        self._f: IO[str] = path.open("w", encoding="utf-8")
        self._head: dict[str, Any] = {"mission": mission, "seed": seed}
        self._wrote_head = False

    def head(self, **more: Any) -> None:
        """More for the first line (the limits, the cost settings) - before the first tick."""
        self._head.update(more)

    def _first(self) -> None:
        if not self._wrote_head:
            self._f.write(json.dumps(self._head) + "\n")
            self._wrote_head = True

    def tick(
        self, now_s: float, inp: SkillInput, out: SkillOut, state: str, debug: dict[str, object]
    ) -> None:
        self._first()
        notes = {k: v for k, v in debug.items() if k != "looked"}  # big; the viewer gets it live (Z8 minor 3)
        line = {"now_s": now_s, "inp": _input_out(inp), "out": _out_out(out), "state": state, "debug": notes}
        self._f.write(json.dumps(line, default=str) + "\n")

    def close(self) -> None:
        self._first()
        self._f.close()


@dataclass(frozen=True)
class LoggedTick:
    now_s: float
    inp: SkillInput
    out: SkillOut
    state: str


def read_decisions(path: Path) -> tuple[dict[str, Any], list[LoggedTick]]:
    with path.open(encoding="utf-8") as f:
        head = cast("dict[str, Any]", json.loads(f.readline()))
        ticks: list[LoggedTick] = []
        for raw in f:
            try:
                d = json.loads(raw)
            except json.JSONDecodeError:  # half a line: the run was cut off mid-write (Z7 minor 7)
                break
            ticks.append(
                LoggedTick(float(d["now_s"]), _input_in(d["inp"]), _out_in(d["out"]), str(d["state"]))
            )
    return head, ticks


@dataclass(frozen=True)
class ReplayReport:
    first_diff: int | None  # the first tick whose output differs from the log (None: identical)
    states: list[tuple[float, str]]  # the logged state timeline (time, state) at each change
    logged: SkillOut | None = None  # at the first difference
    replayed: SkillOut | None = None


class _View(Protocol):
    def tick(
        self, now_s: float, inp: SkillInput, out: SkillOut, state: str, debug: dict[str, object]
    ) -> None: ...


def replay(
    path: Path, make: Callable[[dict[str, Any], int], Skill], view: _View | None = None
) -> ReplayReport:
    """The logged inputs into a fresh skill, made from the log's first line (mission, limits, cost settings)
    and its seed."""
    head, ticks = read_decisions(path)
    skill = make(head, int(head["seed"]))
    states: list[tuple[float, str]] = []
    for i, t in enumerate(ticks):
        if not states or states[-1][1] != t.state:
            states.append((t.now_s, t.state))
        out = skill.tick(t.now_s, t.inp)
        if view is not None:  # --replay --view: drawn as it goes
            view.tick(t.now_s, t.inp, out, skill.state, skill.debug())
        if out != t.out:
            return ReplayReport(i, states, t.out, out)
    return ReplayReport(None, states)
