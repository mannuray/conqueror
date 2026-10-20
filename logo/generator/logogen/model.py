"""Load and validate logo/spec/words.yaml and the conformance tests (grammar.md, tests/README.md)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, cast

import yaml

KINDS = {"command", "reporter", "control", "operator", "constant"}
# the editor's colour per word (LLD §21.3)
GROUPS = {"move", "turn", "flow", "show", "sense", "maths"}
TYPES = {
    "number",
    "condition",
    "any",
    "side",
    "colour",
    "look",
    "sound",
    "notes",
    "text",
    "list",
    "block",
    "template",
    "template-block",
}
WHAT = {  # how a missing input is named in messages ("FORWARD needs a distance, like FORWARD 50.")
    "times": "how many times",
    "steps": "its steps in [ ]",
    "yes": "steps in [ ]",
    "no": "steps in [ ]",
    "where": "where to look",
    "how": "a template in [ ]",
    "test": "a test",
}


@dataclass
class Input:
    name: str
    type: str
    unit: str = ""
    min_milli: int | None = None
    max_milli: int | None = None

    @property
    def what(self) -> str:
        if self.name in WHAT:
            return WHAT[self.name]
        article = "an" if self.name[0] in "aeiou" else "a"
        return f"{article} {self.name}"


@dataclass
class Word:
    name: str
    kind: str
    level: int
    group: str = ""
    aliases: list[str] = field(default_factory=list[str])
    inputs: list[Input] = field(default_factory=list[Input])
    needs: list[str] = field(default_factory=list[str])
    example: str = ""
    help: str = ""
    missing: str = ""


@dataclass
class Case:
    name: str
    src: str
    expect_ok: bool
    reads: str = ""
    code: int = 0
    line: int = 0
    col: int = 0
    message_has: str = ""
    encoders: bool = True
    check: bool = True
    handles_obstacles: int = -1
    your_words: str = ""
    tail_calls: str = "-"  # "-" = not checked; "" = no tail calls expected


@dataclass
class RunCase:
    """A run test (tests/README.md "Run tests"): what the program asks for, in order, and how it ends."""

    name: str
    src: str
    does: str  # the actions joined by " | "
    end_code: int = 0  # 0 = finished
    end_line: int = 0
    message_has: str = ""
    distance_cm: int = 0  # 0 = nothing seen
    cut_short: int = 0  # bit n-1: the n-th motion is cut short by the obstacle brake
    stuck: int = 0  # bit n-1: the n-th motion ends with a stuck track


@dataclass
class Spec:
    words: list[Word]
    presets: dict[str, list[str]]
    reserved: list[str]
    errors: dict[int, str]
    limits: dict[str, int]
    cases: list[Case]
    colour_rgb: dict[str, tuple[int, int, int]] = field(
        default_factory=dict[str, tuple[int, int, int]]
    )
    look_deg: dict[str, tuple[int, int]] = field(default_factory=dict[str, tuple[int, int]])
    run_cases: list[RunCase] = field(default_factory=list[RunCase])

    def word(self, name: str) -> Word:
        return next(w for w in self.words if w.name == name)

    def alias_target(self, alias: str) -> str:
        return next(w.name for w in self.words if alias in w.aliases)


def _milli(v: Any) -> int:
    return round(float(v) * 1000)


def _load_words(raw: list[dict[str, Any]]) -> list[Word]:
    words: list[Word] = []
    for w in raw:
        name = str(w["name"])
        if w["kind"] not in KINDS:
            raise ValueError(f"{name}: unknown kind {w['kind']}")
        if w.get("group") not in GROUPS:
            raise ValueError(f"{name}: unknown group {w.get('group')}")
        inputs: list[Input] = []
        for i in w.get("inputs", []):
            if i["type"] not in TYPES:
                raise ValueError(f"{name}: unknown input type {i['type']}")
            rng = i.get("range")
            inputs.append(
                Input(
                    name=str(i["name"]),
                    type=str(i["type"]),
                    unit=str(i.get("unit", "")),
                    min_milli=_milli(rng[0]) if rng else None,
                    max_milli=_milli(rng[1]) if rng else None,
                )
            )
        words.append(
            Word(
                name=name,
                kind=str(w["kind"]),
                level=int(w["level"]),
                group=str(w["group"]),
                aliases=[str(a) for a in w.get("aliases", [])],
                inputs=inputs,
                needs=[str(n) for n in w.get("needs", [])],
                example=str(w.get("example", "")),
                help=str(w.get("help", "")),
                missing=str(w.get("missing", "")),
            )
        )
    return words


def _your_words(d: dict[str, Any] | None) -> str:
    if not d:
        return ""
    return "".join(
        f"{k.upper()}:{v['inputs']}:{1 if v['gives_value'] else 0};" for k, v in d.items()
    )


def _load_cases(path: Path) -> list[Case]:
    from logogen.emit import normalize_reads  # local import: emit imports model

    out: list[Case] = []
    for c in yaml.safe_load(path.read_text()):
        if str(c.get("version", "v1")) != "v1":
            continue
        exp = c["expect"]
        tank = c.get("tank", {})
        case = Case(
            name=str(c["name"]),
            src=str(c["source"]).rstrip("\n"),
            expect_ok=exp == "ok",
            encoders=bool(tank.get("encoders", True)),
            check=bool(c.get("check", True)),
            handles_obstacles=int(c["handles_obstacles"]) if "handles_obstacles" in c else -1,
            your_words=_your_words(c.get("your_words")),
            tail_calls=",".join(sorted(str(n).upper() for n in c["tail_calls"]))
            if "tail_calls" in c
            else "-",
            message_has=str(c.get("message_has", "")),
        )
        if case.expect_ok:
            case.reads = normalize_reads(str(c["reads"]))
        else:
            case.code, case.line, case.col = int(exp["error"]), int(exp["line"]), int(exp["col"])
        out.append(case)
    return out


def _bits(motions: list[Any]) -> int:
    bits = 0
    for n in motions:
        if not 1 <= int(n) <= 64:
            raise ValueError(f"world: motion {n} is not from 1 to 64")
        bits |= 1 << (int(n) - 1)
    return bits


def _load_run_cases(path: Path) -> list[RunCase]:
    out: list[RunCase] = []
    for c in yaml.safe_load(path.read_text()):
        if str(c.get("version", "v1")) != "v1":
            continue
        world: dict[str, Any] = c.get("world", {})
        ends = c["ends"]
        case = RunCase(
            name=str(c["name"]),
            src=str(c["source"]).rstrip("\n"),
            does=" | ".join(str(d) for d in c["does"]),
            message_has=str(c.get("message_has", "")),
            distance_cm=int(world.get("distance", 0)),
            cut_short=_bits(world.get("cut_short", [])),
            stuck=_bits(world.get("stuck", [])),
        )
        if ends != "finished":
            case.end_code, case.end_line = int(ends["error"]), int(ends["line"])
        out.append(case)
    return out


def _check_preset_data(spec: Spec) -> None:
    """Every colour has its r g b (0-255); every LOOK preset but HOME has its angles."""
    for name in spec.presets["colour"]:
        rgb = spec.colour_rgb.get(name)
        if rgb is None or not all(0 <= x <= 255 for x in rgb):
            raise ValueError(f"colour_rgb: {name} needs [r, g, b] from 0 to 255")
    for name in spec.presets["look"]:
        if name != "HOME" and name not in spec.look_deg:
            raise ValueError(f"look_deg: {name} needs [pan, tilt]")


def load(root: Path) -> Spec:
    spec_dir = root / "logo" / "spec"
    raw = yaml.safe_load((spec_dir / "words.yaml").read_text())
    raw_presets: dict[str, Any] = raw["presets"]
    presets = {
        k: [str(x) for x in cast(list[Any], v)]
        for k, v in raw_presets.items()
        if isinstance(v, list)
    }
    cases = _load_cases(spec_dir / "tests" / "reading.yaml") + _load_cases(
        spec_dir / "tests" / "errors.yaml"
    )
    spec = Spec(
        words=_load_words(raw["words"]),
        presets=presets,
        reserved=[str(r) for r in raw["reserved"]],
        errors={int(k): str(v) for k, v in raw["errors"].items()},
        limits={str(k): int(v) for k, v in raw["limits"].items()},
        cases=cases,
        colour_rgb={
            str(k): (int(v[0]), int(v[1]), int(v[2])) for k, v in raw["colour_rgb"].items()
        },
        look_deg={str(k): (int(v[0]), int(v[1])) for k, v in raw["look_deg"].items()},
    )
    spec.run_cases = _load_run_cases(spec_dir / "tests" / "running.yaml")
    _check_preset_data(spec)
    for r in spec.run_cases:
        if r.end_code and r.end_code not in spec.errors:
            raise ValueError(f"run case {r.name}: unknown error code {r.end_code}")
    for c in spec.cases:
        if not c.expect_ok and c.code not in spec.errors:
            raise ValueError(f"case {c.name}: unknown error code {c.code}")
    return spec
