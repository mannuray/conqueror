# Plan N1 — Tank Logo reader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The ESP32 bridge firmware gains a host-tested Tank Logo reader — generator, lexer, two-pass parser, checker and the `reads` printer — that passes every v1 case in `logo/spec/tests/reading.yaml` and `errors.yaml`.

**Architecture:** `logo/spec/words.yaml` is turned by a small Python generator (`logogen`, a uv project like `protogen`) into a C++ header (word table, presets, error texts), a JSON word list for the page, and C++ test cases from the YAML tests. The reader is pure C++17 under `firmware/esp32/src/logo/` with fixed capacities and **no recursion** (explicit stacks), built and tested in `env:native`. Nothing runs on the tank in this plan.

**Tech Stack:** Python 3.12 + uv + PyYAML (generator, pytest/ruff/pyright), C++17 (gnu++17), PlatformIO `env:native`, Unity.

**Spec:** `docs/lld/tank-logo-lld.md` §0–§6, §15, §17; `logo/spec/grammar.md`; `logo/spec/words.yaml`; `logo/spec/tests/` (README + YAML); design-notes §82–§87.

## Global Constraints

- Coding standards §2.3 / §3: **no heap after start-up, no recursion, no `virtual`, no exceptions/RTTI**; fixed capacities as `constexpr`; `-Wall -Wextra -Werror` in `env:native`.
- Generated files are never edited by hand: `firmware/esp32/src/logo/generated/words.h`, `firmware/esp32/web/generated/words.json`, `firmware/esp32/test/test_logo_conformance/cases.h` (regenerate with `cd logo && uv run logogen write`).
- Line and column are **1-based**; a column is the token's first character; the **first error** is the earliest by (line, column), ties in the order found.
- Numbers are **1/1000 fixed point** (`int32_t` milli), |value| ≤ 1 000 000, ≤ 3 decimals; names and quoted words ≤ 32 characters; source ≤ 8192 bytes.
- v1 = levels 1–2 + `OUTPUT` + list literals; other level-3 words → `PROGRAM_7030`; AI words → `PROGRAM_7020` (checked before the level).
- Capacities: tokens 4096, nodes 4096, parser frames 64, expression stacks 128, procedures 64, diagnostics 16, message 192 bytes.
- Commits: Conventional Commits + `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch **`plan-n1`**; ask the owner before merging.

## Review Focus

1. **Brackets nested deeper than the parser's 64 frames** (`[[[[…`, `((((…`) → `PROGRAM_7010` "too deeply nested", never a stack overflow or a hang — test in Task 5 (`test_parser_deep_nesting_is_an_error`).
2. **A program with more than 4096 tokens or 4096 nodes** → `PROGRAM_7010`, never an out-of-bounds write — tests in Task 3 (`test_lex_token_overflow`) and Task 5 (`test_parser_node_pool_full`).
3. **Non-ASCII text** (an emoji in a comment or a quoted word, `SAY "héllo`) → no crash, the token keeps its bytes, later columns still point at the right token on that line — test in Task 3 (`test_lex_utf8_comment_and_quoted`).
4. **Every error path makes progress** (an error at `]`, at the end of text, at a command on the next line) → the parser always finishes; no infinite loop — test in Task 5 (`test_parser_always_terminates_on_garbage`, a fixed list of broken programs).
5. **An empty program, or only comments** → no errors and an empty `reads` — test in Task 5 (`test_parser_empty_program`).

---

## File Structure

| File | Responsibility |
|---|---|
| `logo/pyproject.toml`, `logo/generator/logogen/{__init__,cli,model,emit}.py`, `logo/tests/test_logogen.py` (create) | the generator: load + validate `words.yaml` and the tests, emit the three files, `write` / `check` |
| `logo/spec/words.yaml`, `logo/spec/tests/{errors,reading}.yaml`, `logo/spec/tests/README.md` (modify) | spec fixes found while planning (§ Task 1) |
| `firmware/esp32/src/logo/generated/words.h`, `web/generated/words.json`, `test/test_logo_conformance/cases.h` (generated) | |
| `firmware/esp32/src/logo/diag.h/.cpp` (create) | `Diagnostics`, message rendering |
| `firmware/esp32/src/logo/lexer.h/.cpp` (create) | `Token`, `TokenList`, `lex()`, text helpers |
| `firmware/esp32/src/logo/ast.h/.cpp` (create) | `Node`, `NodePool`, `Op` |
| `firmware/esp32/src/logo/words.h/.cpp` (create) | lookups over the generated table (word, alias, preset, reserved), edit distance |
| `firmware/esp32/src/logo/parser.h/.cpp` (create) | pass 1 + pass 2 (iterative) |
| `firmware/esp32/src/logo/printer.h/.cpp` (create) | the `reads` notation |
| `firmware/esp32/src/logo/checker.h/.cpp` (create) | `Caps`, `CheckResult`, `Checker` |
| `firmware/esp32/test/test_logo_{diag,lexer,parser,checker,conformance}/test_main.cpp` (create) | host tests |
| `firmware/esp32/platformio.ini`, `firmware/esp32/scripts/logogen_check.py`, `scripts/check-protocol.sh` (modify / create) | build + pre-commit wiring |
| `CLAUDE.md`, `docs/lld/tank-logo-lld.md` (modify) | generated-file list, plan pointer |

Branch: `git checkout main && git pull --ff-only && git checkout -b plan-n1`.

---

### Task 1: Generator `logogen` + spec fixes + build wiring

**Files:**
- Create: `logo/pyproject.toml`, `logo/generator/logogen/__init__.py`, `logo/generator/logogen/model.py`, `logo/generator/logogen/emit.py`, `logo/generator/logogen/cli.py`, `logo/tests/test_logogen.py`
- Modify: `logo/spec/words.yaml` (IFELSE `missing`), `logo/spec/tests/errors.yaml` (SQAURE case), `logo/spec/tests/reading.yaml` (SEE? case `check: false`), `logo/spec/tests/README.md` (`check`), `firmware/esp32/platformio.ini`, `scripts/check-protocol.sh`, `CLAUDE.md`
- Create: `firmware/esp32/scripts/logogen_check.py`, `firmware/esp32/test/test_logo_words/test_main.cpp`
- Generated: `firmware/esp32/src/logo/generated/words.h`, `firmware/esp32/web/generated/words.json`, `firmware/esp32/test/test_logo_conformance/cases.h`

**Interfaces:**
- Produces (C++, `words.h`, namespace `cq::logo::gen`): `enum class InType : uint8_t { kNumber, kCondition, kAny, kSide, kColour, kLook, kSound, kNotes, kText, kList, kBlock, kTemplate, kTemplateBlock }`; `enum class WordKind : uint8_t { kCommand, kReporter, kControl, kOperator, kConstant }`; `kNeedEncoders = 1`, `kNeedAi = 2`; `struct InputSpec { const char* name; InType type; bool has_range; int32_t min_milli; int32_t max_milli; const char* unit; const char* what; }`; `struct WordSpec { const char* name; WordKind kind; uint8_t level; uint8_t needs; uint8_t n_inputs; const InputSpec* inputs; const char* example; const char* missing; }`; `kWords[]`, `kWordCount`; `struct Alias { const char* alias; uint8_t word; }`, `kAliases[]`, `kAliasCount`; `kSides[]`, `kColours[]`, `kLooks[]`, `kSounds[]` with `k…Count`; `kReserved[]`, `kReservedCount`; `struct ErrorText { uint16_t code; const char* text; }`, `kErrors[]`, `kErrorCount`; limits `kSourceBytes`, `kMaxProcs`, `kNameChars`, `kNumberAbsMaxMilli`.
- Produces (C++, `cases.h`): `struct LogoCase { const char* name; const char* src; bool expect_ok; const char* reads; int code; int line; int col; const char* message_has; bool encoders; bool check; int handles_obstacles; const char* your_words; const char* tail_calls; }` (`tail_calls` "-" = not checked); `kCases[]`; macro `LOGO_CASES(X)` = `X(0) X(1) …`.
- Produces (Python): `logogen.model.load(root: Path) -> Spec`; `logogen.emit.render_words_h(spec) -> str`, `render_words_json(spec) -> str`, `render_cases_h(spec) -> str`, `normalize_reads(text: str) -> str`, `c_string(text: str) -> str`; CLI `logogen write|check`.

- [ ] **Step 1: Branch and spec fixes**

```bash
git checkout main && git pull --ff-only && git checkout -b plan-n1
```
In `logo/spec/words.yaml`, under `- name: IFELSE` add the line `    missing: "two [ ] blocks: one for yes, one for no"`.
In `logo/spec/tests/errors.yaml` replace the first case's `source: SQAURE 50` with:
```yaml
  source: |
    SQAURE 50
    TO SQUARE :s FD :s END
```
and delete its `note:` line. In `logo/spec/tests/reading.yaml`, in the case `quoted word ends at a bracket`, replace the `note:` line with `  check: false   # SEE? is an AI word: only the reading is tested here`. In `logo/spec/tests/README.md`, after the `version:` line of the format block add `  check: false                    # optional: only the reading is tested (the checker is not run)`.

- [ ] **Step 2: Generator project**

`logo/pyproject.toml`:
```toml
[project]
name = "logogen"
version = "0.1.0"
description = "Tank Logo generator: words.yaml and the conformance tests -> C++ / JSON"
requires-python = ">=3.12"
dependencies = ["pyyaml>=6.0"]

[project.scripts]
logogen = "logogen.cli:main"

[dependency-groups]
dev = ["pytest>=8.3", "ruff>=0.7", "pyright>=1.1.385", "types-PyYAML>=6.0"]

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["generator/logogen"]

[tool.ruff]
line-length = 100
target-version = "py312"
src = ["generator"]

[tool.pyright]
typeCheckingMode = "strict"
include = ["generator", "tests"]
```
`logo/generator/logogen/__init__.py`: `"""Tank Logo generator (Plan N1, LLD §15)."""`

- [ ] **Step 3: Write the failing generator tests**

`logo/tests/test_logogen.py`:
```python
from pathlib import Path

from logogen import emit, model

ROOT = Path(__file__).resolve().parents[2]


def spec() -> model.Spec:
    return model.load(ROOT)


def test_words_load_and_forward_has_its_range() -> None:
    s = spec()
    fd = s.word("FORWARD")
    assert fd.level == 1 and fd.kind == "command" and fd.needs == ["encoders"]
    assert fd.inputs[0].min_milli == 1000 and fd.inputs[0].max_milli == 500000
    assert s.alias_target("FD") == "FORWARD"


def test_presets_are_strings_and_off_survives_yaml() -> None:
    assert "OFF" in spec().presets["colour"]


def test_normalize_reads() -> None:
    assert emit.normalize_reads("(REPEAT 4 [ (FD 50)\n (RT 90) ])") == "(REPEAT 4 [(FD 50) (RT 90)])"
    assert emit.normalize_reads("(TO SIDE ( ) [ (OUTPUT 50) ])") == "(TO SIDE () [(OUTPUT 50)])"


def test_c_string_escapes_quotes_newlines_and_utf8() -> None:
    assert emit.c_string('a"b\\c\nd') == '"a\\"b\\\\c\\nd"'
    assert emit.c_string("—") == '"\\342\\200\\224"'


def test_cases_skip_v11_and_carry_expectations() -> None:
    cases = spec().cases
    names = [c.name for c in cases]
    assert "a template with two values" not in names  # version v1.1
    fd600 = next(c for c in cases if c.name == "a range known before running")
    assert (fd600.code, fd600.line, fd600.col) == (7025, 1, 4)
    sq = next(c for c in cases if c.name.startswith("the spec's first program"))
    assert sq.your_words == "SQUARE:1:0;"


def test_words_h_mentions_every_word_and_error() -> None:
    s = spec()
    text = emit.render_words_h(s)
    for w in s.words:
        assert f'"{w.name}"' in text
    assert "7029" in text and "kCases" not in text
```

- [ ] **Step 4: Run them to see them fail**

Run: `cd logo && uv sync && uv run pytest -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'logogen.model'` (or `cannot import name`).

- [ ] **Step 5: `model.py` — load and validate**

`logo/generator/logogen/model.py`:
```python
"""Load and validate logo/spec/words.yaml and the conformance tests (grammar.md, tests/README.md)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

KINDS = {"command", "reporter", "control", "operator", "constant"}
TYPES = {
    "number", "condition", "any", "side", "colour", "look", "sound", "notes", "text", "list",
    "block", "template", "template-block",
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
    aliases: list[str] = field(default_factory=list)
    inputs: list[Input] = field(default_factory=list)
    needs: list[str] = field(default_factory=list)
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
class Spec:
    words: list[Word]
    presets: dict[str, list[str]]
    reserved: list[str]
    errors: dict[int, str]
    limits: dict[str, int]
    cases: list[Case]

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
        inputs: list[Input] = []
        for i in w.get("inputs", []):
            if i["type"] not in TYPES:
                raise ValueError(f"{name}: unknown input type {i['type']}")
            rng = i.get("range")
            inputs.append(Input(
                name=str(i["name"]), type=str(i["type"]), unit=str(i.get("unit", "")),
                min_milli=_milli(rng[0]) if rng else None, max_milli=_milli(rng[1]) if rng else None,
            ))
        words.append(Word(
            name=name, kind=str(w["kind"]), level=int(w["level"]),
            aliases=[str(a) for a in w.get("aliases", [])], inputs=inputs,
            needs=[str(n) for n in w.get("needs", [])], example=str(w.get("example", "")),
            help=str(w.get("help", "")), missing=str(w.get("missing", "")),
        ))
    return words


def _your_words(d: dict[str, Any] | None) -> str:
    if not d:
        return ""
    return "".join(f"{k.upper()}:{v['inputs']}:{1 if v['gives_value'] else 0};" for k, v in d.items())


def _load_cases(path: Path) -> list[Case]:
    from logogen.emit import normalize_reads  # local import: emit imports model

    out: list[Case] = []
    for c in yaml.safe_load(path.read_text()):
        if str(c.get("version", "v1")) != "v1":
            continue
        exp = c["expect"]
        tank = c.get("tank", {})
        case = Case(
            name=str(c["name"]), src=str(c["source"]).rstrip("\n"), expect_ok=exp == "ok",
            encoders=bool(tank.get("encoders", True)), check=bool(c.get("check", True)),
            handles_obstacles=int(c["handles_obstacles"]) if "handles_obstacles" in c else -1,
            your_words=_your_words(c.get("your_words")),
            tail_calls=",".join(sorted(str(n).upper() for n in c["tail_calls"])) if "tail_calls" in c else "-",
            message_has=str(c.get("message_has", "")),
        )
        if case.expect_ok:
            case.reads = normalize_reads(str(c["reads"]))
        else:
            case.code, case.line, case.col = int(exp["error"]), int(exp["line"]), int(exp["col"])
        out.append(case)
    return out


def load(root: Path) -> Spec:
    spec_dir = root / "logo" / "spec"
    raw = yaml.safe_load((spec_dir / "words.yaml").read_text())
    presets = {k: [str(x) for x in v] for k, v in raw["presets"].items() if isinstance(v, list)}
    cases = _load_cases(spec_dir / "tests" / "reading.yaml") + _load_cases(spec_dir / "tests" / "errors.yaml")
    spec = Spec(
        words=_load_words(raw["words"]), presets=presets, reserved=[str(r) for r in raw["reserved"]],
        errors={int(k): str(v) for k, v in raw["errors"].items()},
        limits={str(k): int(v) for k, v in raw["limits"].items()}, cases=cases,
    )
    for c in spec.cases:
        if not c.expect_ok and c.code not in spec.errors:
            raise ValueError(f"case {c.name}: unknown error code {c.code}")
    return spec
```

- [ ] **Step 6: `emit.py` — the three outputs**

`logo/generator/logogen/emit.py`:
```python
"""Render words.h, words.json and the conformance cases.h (LLD §15). Output is deterministic."""

from __future__ import annotations

import json
import re

from logogen.model import Spec

HEADER = "// GENERATED by logogen from logo/spec - do not edit (cd logo && uv run logogen write).\n"
TYPE_ENUM = {
    "number": "kNumber", "condition": "kCondition", "any": "kAny", "side": "kSide", "colour": "kColour",
    "look": "kLook", "sound": "kSound", "notes": "kNotes", "text": "kText", "list": "kList",
    "block": "kBlock", "template": "kTemplate", "template-block": "kTemplateBlock",
}
KIND_ENUM = {"command": "kCommand", "reporter": "kReporter", "control": "kControl",
             "operator": "kOperator", "constant": "kConstant"}


def c_string(text: str) -> str:
    out = ['"']
    for b in text.encode("utf-8"):
        ch = chr(b)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif 32 <= b < 127:
            out.append(ch)
        else:
            out.append(f"\\{b:03o}")
    out.append('"')
    return "".join(out)


def normalize_reads(text: str) -> str:
    t = re.sub(r"\s+", " ", text.strip())
    t = re.sub(r"([(\[{]) ", r"\1", t)
    return re.sub(r" ([)\]}])", r"\1", t)


def _needs(needs: list[str]) -> str:
    bits = [b for n, b in (("encoders", "kNeedEncoders"), ("ai", "kNeedAi")) if n in needs]
    return " | ".join(bits) if bits else "0"


def render_words_h(spec: Spec) -> str:
    lines = [HEADER, "#pragma once\n", "#include <stdint.h>\n", "namespace cq {\nnamespace logo {\nnamespace gen {\n"]
    lines.append("enum class InType : uint8_t { " + ", ".join(TYPE_ENUM.values()) + " };\n")
    lines.append("enum class WordKind : uint8_t { " + ", ".join(KIND_ENUM.values()) + " };\n")
    lines.append("enum : uint8_t { kNeedEncoders = 1, kNeedAi = 2 };\n")
    lines.append("struct InputSpec { const char* name; InType type; bool has_range; int32_t min_milli; "
                 "int32_t max_milli; const char* unit; const char* what; };\n")
    lines.append("struct WordSpec { const char* name; WordKind kind; uint8_t level; uint8_t needs; "
                 "uint8_t n_inputs; const InputSpec* inputs; const char* example; const char* missing; };\n\n")
    for i, w in enumerate(spec.words):
        if not w.inputs:
            continue
        items = []
        for inp in w.inputs:
            has = inp.min_milli is not None
            items.append(f"{{{c_string(inp.name)}, InType::{TYPE_ENUM[inp.type]}, {'true' if has else 'false'}, "
                         f"{inp.min_milli or 0}, {inp.max_milli or 0}, {c_string(inp.unit)}, {c_string(inp.what)}}}")
        lines.append(f"inline constexpr InputSpec kIn{i}[] = {{{', '.join(items)}}};\n")
    lines.append("\ninline constexpr WordSpec kWords[] = {\n")
    for i, w in enumerate(spec.words):
        ins = f"kIn{i}" if w.inputs else "nullptr"
        lines.append(f"    {{{c_string(w.name)}, WordKind::{KIND_ENUM[w.kind]}, {w.level}, {_needs(w.needs)}, "
                     f"{len(w.inputs)}, {ins}, {c_string(w.example)}, {c_string(w.missing)}}},\n")
    lines.append("};\n")
    lines.append(f"inline constexpr uint8_t kWordCount = {len(spec.words)};\n\n")
    aliases = [(a, i) for i, w in enumerate(spec.words) for a in w.aliases]
    lines.append("struct Alias { const char* alias; uint8_t word; };\n")
    lines.append("inline constexpr Alias kAliases[] = {" + ", ".join(f"{{{c_string(a)}, {i}}}" for a, i in aliases) + "};\n")
    lines.append(f"inline constexpr uint8_t kAliasCount = {len(aliases)};\n\n")
    for key, cname in (("side", "Sides"), ("colour", "Colours"), ("look", "Looks"), ("sound", "Sounds")):
        vals = spec.presets[key]
        lines.append(f"inline constexpr const char* k{cname}[] = {{{', '.join(c_string(v) for v in vals)}}};\n")
        lines.append(f"inline constexpr uint8_t k{cname}Count = {len(vals)};\n")
    lines.append(f"\ninline constexpr const char* kReserved[] = {{{', '.join(c_string(r) for r in spec.reserved)}}};\n")
    lines.append(f"inline constexpr uint8_t kReservedCount = {len(spec.reserved)};\n\n")
    lines.append("struct ErrorText { uint16_t code; const char* text; };\ninline constexpr ErrorText kErrors[] = {\n")
    for code in sorted(spec.errors):
        lines.append(f"    {{{code}, {c_string(spec.errors[code])}}},\n")
    lines.append(f"}};\ninline constexpr uint8_t kErrorCount = {len(spec.errors)};\n\n")
    lim = spec.limits
    lines.append(f"inline constexpr uint16_t kSourceBytes = {lim['source_bytes']};\n")
    lines.append(f"inline constexpr uint8_t kMaxProcs = {lim['words_of_your_own']};\n")
    lines.append(f"inline constexpr uint8_t kNameChars = {lim['name_chars']};\n")
    lines.append(f"inline constexpr int32_t kNumberAbsMaxMilli = {lim['number_abs_max'] * 1000};\n")
    lines.append("\n}  // namespace gen\n}  // namespace logo\n}  // namespace cq\n")
    return "".join(lines)


def render_words_json(spec: Spec) -> str:
    words = [{
        "name": w.name, "aliases": w.aliases, "kind": w.kind, "level": w.level, "needs": w.needs,
        "inputs": [{"name": i.name, "type": i.type, "unit": i.unit} for i in w.inputs],
        "help": w.help, "example": w.example,
    } for w in spec.words]
    return json.dumps({"words": words, "presets": spec.presets}, indent=1, ensure_ascii=False) + "\n"


def render_cases_h(spec: Spec) -> str:
    lines = [HEADER, "#pragma once\n\n",
             "struct LogoCase { const char* name; const char* src; bool expect_ok; const char* reads; int code; "
             "int line; int col; const char* message_has; bool encoders; bool check; int handles_obstacles; "
             "const char* your_words; const char* tail_calls; };\n\n",
             "static const LogoCase kCases[] = {\n"]
    for c in spec.cases:
        lines.append(
            f"    {{{c_string(c.name)}, {c_string(c.src)}, {'true' if c.expect_ok else 'false'}, "
            f"{c_string(c.reads)}, {c.code}, {c.line}, {c.col}, {c_string(c.message_has)}, "
            f"{'true' if c.encoders else 'false'}, {'true' if c.check else 'false'}, {c.handles_obstacles}, "
            f"{c_string(c.your_words)}, {c_string(c.tail_calls)}}},\n")
    lines.append("};\n\n#define LOGO_CASES(X) " + " ".join(f"X({i})" for i in range(len(spec.cases))) + "\n")
    return "".join(lines)
```

- [ ] **Step 7: `cli.py`**

`logo/generator/logogen/cli.py`:
```python
"""logogen write | check (LLD §15)."""

from __future__ import annotations

import sys
from pathlib import Path

from logogen import emit, model

ROOT = Path(__file__).resolve().parents[3]
ESP = ROOT / "firmware" / "esp32"
OUTPUTS = {
    ESP / "src" / "logo" / "generated" / "words.h": emit.render_words_h,
    ESP / "web" / "generated" / "words.json": emit.render_words_json,
    ESP / "test" / "test_logo_conformance" / "cases.h": emit.render_cases_h,
}


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    spec = model.load(ROOT)
    stale = []
    for path, render in OUTPUTS.items():
        text = render(spec)
        if cmd == "write":
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        elif not path.exists() or path.read_text() != text:
            stale.append(path.relative_to(ROOT))
    if cmd == "write":
        print(f"logogen: wrote {len(OUTPUTS)} files")
        return 0
    for s in stale:
        print(f"logogen: stale {s} - run: cd logo && uv run logogen write")
    return 1 if stale else 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 8: Run the generator tests, generate**

Run: `cd logo && uv run pytest -q && uv run ruff check . && uv run pyright && uv run logogen write && uv run logogen check`
Expected: `6 passed`; ruff and pyright clean; `logogen: wrote 3 files`; check exits 0.

- [ ] **Step 9: Host test that the header compiles and holds the table**

`firmware/esp32/test/test_logo_words/test_main.cpp`:
```cpp
// Plan N1 Task 1: the generated word table compiles and carries the spec's facts.
#include <string.h>
#include <unity.h>

#include "logo/generated/words.h"

using namespace cq::logo::gen;

void setUp() {}
void tearDown() {}

void test_forward_entry() {
  const WordSpec* fd = nullptr;
  for (uint8_t i = 0; i < kWordCount; ++i) if (strcmp(kWords[i].name, "FORWARD") == 0) fd = &kWords[i];
  TEST_ASSERT_NOT_NULL(fd);
  TEST_ASSERT_EQUAL(1, fd->n_inputs);
  TEST_ASSERT_EQUAL(1000, fd->inputs[0].min_milli);
  TEST_ASSERT_EQUAL(500000, fd->inputs[0].max_milli);
  TEST_ASSERT_TRUE(fd->needs & kNeedEncoders);
}

void test_presets_and_errors() {
  TEST_ASSERT_EQUAL(24, kSoundsCount);
  TEST_ASSERT_EQUAL_STRING("OFF", kColours[kColoursCount - 1]);
  bool has7029 = false;
  for (uint8_t i = 0; i < kErrorCount; ++i) has7029 |= kErrors[i].code == 7029;
  TEST_ASSERT_TRUE(has7029);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_forward_entry);
  RUN_TEST(test_presets_and_errors);
  return UNITY_END();
}
```
In `firmware/esp32/platformio.ini`, `[env:native]`: `build_src_filter = +<core/> +<logo/>`.

- [ ] **Step 10: Run it**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_words`
Expected: `2 test cases: 2 succeeded`.

- [ ] **Step 11: Build and commit wiring**

`firmware/esp32/scripts/logogen_check.py` (same shape as `protogen_check.py`):
```python
"""PlatformIO pre-script: fail the build when the generated Tank Logo files are stale (Plan N1, LLD §15)."""

import pathlib
import shutil
import subprocess

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

if env.subst("$PIOENV") != "native":  # type: ignore[name-defined]  # noqa: F821
    logo = pathlib.Path(env.subst("$PROJECT_DIR")).parents[1] / "logo"  # type: ignore[name-defined]  # noqa: F821
    if shutil.which("uv") is None:
        print("logogen_check: uv not found (see docs/dev-setup.md)")
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
    if subprocess.run(["uv", "run", "logogen", "check"], cwd=logo, check=False).returncode != 0:
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
    print("logogen_check: generated files are fresh")
```
Add `    pre:scripts/logogen_check.py` under `extra_scripts` of `[esp32]` (after `pre:scripts/protogen_check.py`). Append to `scripts/check-protocol.sh`:
```bash
cd "$(dirname "$0")/../logo"
uv run logogen check
```
In `CLAUDE.md` "Hard rules for code", extend the generated-files list with `firmware/esp32/src/logo/generated/`, `firmware/esp32/web/generated/`, `firmware/esp32/test/test_logo_conformance/cases.h` and "change `logo/spec/*` and run `cd logo && uv run logogen write`".
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge` → Expected: `logogen_check: generated files are fresh`, `[SUCCESS]`.
```bash
git add logo firmware/esp32 scripts/check-protocol.sh CLAUDE.md
git commit -m "feat(logo): logogen generator - words.h, words.json and conformance cases from logo/spec

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `Diagnostics` and messages

**Files:** Create `firmware/esp32/src/logo/diag.h`, `diag.cpp`; Test `firmware/esp32/test/test_logo_diag/test_main.cpp`.

**Interfaces:**
- Consumes: `gen::kErrors`.
- Produces: `struct DiagArg { const char* key; const char* value; }`; `struct Diag { uint16_t code; uint16_t line; uint16_t col; bool warning; char msg[192]; }`; `class Diagnostics { static constexpr uint8_t kCap = 16; void add(uint16_t code, uint16_t line, uint16_t col, const DiagArg* args = nullptr, uint8_t n = 0, bool warning = false); void appendToLast(const char* text); uint8_t count() const; const Diag& at(uint8_t) const; uint8_t errorCount() const; const Diag* first() const; void clear(); }`; free `void renderMessage(const char* tmpl, const DiagArg* args, uint8_t n, char* out, size_t cap)`; `const char* errorTemplate(uint16_t code)`.

- [ ] **Step 1: Write the failing tests**

```cpp
// Plan N1 Task 2: messages from templates, sentence dropping, dedupe, first error by position.
#include <string.h>
#include <unity.h>

#include "logo/diag.h"

using namespace cq::logo;

void setUp() {}
void tearDown() {}

void test_render_fills_placeholders() {
  char out[192];
  const DiagArg a[] = {{"word", "FORWARD"}, {"what", "a distance"}, {"example", "FORWARD 50"}};
  renderMessage("{word} needs {what}, like {example}.", a, 3, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("FORWARD needs a distance, like FORWARD 50.", out);
}

void test_render_drops_the_sentence_with_an_empty_value() {
  char out[192];
  const DiagArg a[] = {{"word", "SQAURE"}, {"suggestion", ""}};
  renderMessage("I don't know how to {word}. Did you mean {suggestion}?", a, 2, out, sizeof out);
  TEST_ASSERT_EQUAL_STRING("I don't know how to SQAURE.", out);
}

void test_add_renders_from_the_generated_table_and_dedupes() {
  Diagnostics d;
  const DiagArg a[] = {{"value", "-5"}};
  d.add(7004, 1, 7, a, 1);
  d.add(7004, 1, 7, a, 1);
  TEST_ASSERT_EQUAL(1, d.count());
  TEST_ASSERT_EQUAL_STRING("You didn't say what to do with -5.", d.at(0).msg);
  d.appendToLast(" To subtract, write 10 - 5.");
  TEST_ASSERT_NOT_NULL(strstr(d.at(0).msg, "10 - 5"));
}

void test_first_is_earliest_error_not_warning() {
  Diagnostics d;
  d.add(7001, 3, 1);
  d.add(7019, 1, 1, nullptr, 0, true);
  d.add(7004, 2, 5);
  d.add(7016, 2, 5);
  TEST_ASSERT_EQUAL(3, d.errorCount());
  TEST_ASSERT_EQUAL(7004, d.first()->code);  // line 2 beats line 3; the warning never counts; ties keep order
}

void test_capacity_is_respected() {
  Diagnostics d;
  for (uint16_t i = 0; i < 40; ++i) d.add(7001, i + 1, 1);
  TEST_ASSERT_EQUAL(Diagnostics::kCap, d.count());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_render_fills_placeholders);
  RUN_TEST(test_render_drops_the_sentence_with_an_empty_value);
  RUN_TEST(test_add_renders_from_the_generated_table_and_dedupes);
  RUN_TEST(test_first_is_earliest_error_not_warning);
  RUN_TEST(test_capacity_is_respected);
  return UNITY_END();
}
```

- [ ] **Step 2: Run to see it fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_diag`
Expected: FAIL — `logo/diag.h: No such file or directory`.

- [ ] **Step 3: Implement**

`diag.h`:
```cpp
// Tank Logo diagnostics (LLD §5-§6, grammar.md §11): every refusal has one code, a line and a column, and a
// friendly message rendered from logo/spec/words.yaml `errors`. Pure C++, fixed capacity.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace logo {

struct DiagArg {
  const char* key;
  const char* value;
};

struct Diag {
  uint16_t code = 0;
  uint16_t line = 0;
  uint16_t col = 0;
  bool warning = false;
  char msg[192] = "";
};

// Fills {key} placeholders; a sentence whose placeholder is empty (or missing) is dropped.
void renderMessage(const char* tmpl, const DiagArg* args, uint8_t n, char* out, size_t cap);
const char* errorTemplate(uint16_t code);  // "" for an unknown code

class Diagnostics {
 public:
  static constexpr uint8_t kCap = 16;
  void add(uint16_t code, uint16_t line, uint16_t col, const DiagArg* args = nullptr, uint8_t n = 0,
           bool warning = false);
  void appendToLast(const char* text);
  uint8_t count() const { return m_n; }
  const Diag& at(uint8_t i) const { return m_d[i]; }
  uint8_t errorCount() const;
  const Diag* first() const;
  void clear() { m_n = 0; }

 private:
  Diag m_d[kCap];
  uint8_t m_n = 0;
  bool m_last_added = false;
};

}  // namespace logo
}  // namespace cq
```
`diag.cpp`:
```cpp
#include "logo/diag.h"

#include <string.h>

#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

const char* findArg(const DiagArg* args, uint8_t n, const char* key, size_t klen) {
  for (uint8_t i = 0; i < n; ++i) {
    if (strlen(args[i].key) == klen && strncmp(args[i].key, key, klen) == 0) return args[i].value;
  }
  return "";
}

bool sentenceEnd(const char* p) { return (*p == '.' || *p == '?' || *p == '!') && (p[1] == ' ' || p[1] == '\0'); }

}  // namespace

void renderMessage(const char* tmpl, const DiagArg* args, uint8_t n, char* out, size_t cap) {
  size_t o = 0, sentence = 0;
  bool drop = false;
  for (const char* p = tmpl; *p != '\0'; ++p) {
    if (*p == '{') {
      const char* close = strchr(p, '}');
      if (close == nullptr) break;
      const char* v = findArg(args, n, p + 1, static_cast<size_t>(close - p - 1));
      if (*v == '\0') drop = true;
      for (; *v != '\0' && o + 1 < cap; ++v) out[o++] = *v;
      p = close;
      continue;
    }
    if (o + 1 < cap) out[o++] = *p;
    if (sentenceEnd(p)) {
      if (drop) {
        o = sentence;
        while (o > 0 && out[o - 1] == ' ') --o;
        drop = false;
      }
      sentence = o + 1;  // after the following space
    }
  }
  if (drop) {
    o = sentence > 0 ? sentence - 1 : 0;
    while (o > 0 && out[o - 1] == ' ') --o;
  }
  out[o < cap ? o : cap - 1] = '\0';
}

const char* errorTemplate(uint16_t code) {
  for (uint8_t i = 0; i < gen::kErrorCount; ++i) {
    if (gen::kErrors[i].code == code) return gen::kErrors[i].text;
  }
  return "";
}

void Diagnostics::add(uint16_t code, uint16_t line, uint16_t col, const DiagArg* args, uint8_t n, bool warning) {
  m_last_added = false;
  for (uint8_t i = 0; i < m_n; ++i) {
    if (m_d[i].code == code && m_d[i].line == line && m_d[i].col == col) return;
  }
  if (m_n == kCap) return;
  Diag& d = m_d[m_n++];
  d.code = code;
  d.line = line;
  d.col = col;
  d.warning = warning;
  renderMessage(errorTemplate(code), args, n, d.msg, sizeof d.msg);
  m_last_added = true;
}

void Diagnostics::appendToLast(const char* text) {
  if (!m_last_added || m_n == 0) return;
  Diag& d = m_d[m_n - 1];
  strncat(d.msg, text, sizeof d.msg - strlen(d.msg) - 1);
}

uint8_t Diagnostics::errorCount() const {
  uint8_t c = 0;
  for (uint8_t i = 0; i < m_n; ++i) c += m_d[i].warning ? 0 : 1;
  return c;
}

const Diag* Diagnostics::first() const {
  const Diag* best = nullptr;
  for (uint8_t i = 0; i < m_n; ++i) {
    const Diag& d = m_d[i];
    if (d.warning) continue;
    if (best == nullptr || d.line < best->line || (d.line == best->line && d.col < best->col)) best = &d;
  }
  return best;
}

}  // namespace logo
}  // namespace cq
```

- [ ] **Step 4: Run to see it pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_diag`
Expected: `5 test cases: 5 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/logo/diag.* firmware/esp32/test/test_logo_diag
git commit -m "feat(logo): diagnostics - messages from words.yaml, sentence dropping, first error by position

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Lexer

**Files:** Create `firmware/esp32/src/logo/lexer.h`, `lexer.cpp`; Test `firmware/esp32/test/test_logo_lexer/test_main.cpp`.

**Interfaces:**
- Consumes: `Diagnostics`, `gen::kNumberAbsMaxMilli`, `gen::kNameChars`.
- Produces: `enum class Tok : uint8_t { kEnd, kNumber, kName, kInput, kQuoted, kPlaceholder, kLBracket, kRBracket, kLParen, kRParen, kPlus, kMinus, kNeg, kStar, kSlash, kEq, kNe, kLt, kGt, kLe, kGe }`; `struct Token { Tok kind; uint16_t start; uint16_t len; uint16_t line; uint16_t col; int32_t milli; uint8_t ph; }`; `struct TokenList { static constexpr uint16_t kCap = 4096; Token t[kCap]; uint16_t n; }`; `void lex(const char* src, uint16_t len, TokenList& out, Diagnostics& d)` (always ends with `kEnd`); `bool tokenIs(const char* src, const Token& t, const char* word)` (case-insensitive, NAME tokens); `void tokenText(const char* src, const Token& t, char* out, size_t cap)`.

- [ ] **Step 1: Write the failing tests**

```cpp
// Plan N1 Task 3: tokens (grammar.md §2), MINUS vs NEG, numbers in milli, errors 7015 / 7016 / 7010.
#include <string.h>
#include <unity.h>

#include "logo/lexer.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
void L(const char* s) { g_d.clear(); lex(s, static_cast<uint16_t>(strlen(s)), g_t, g_d); }
int K(Tok k) { return static_cast<int>(k); }
}  // namespace

void setUp() {}
void tearDown() {}

void test_lex_words_numbers_and_end() {
  L("FD 50 rt 2.5");
  TEST_ASSERT_EQUAL(5, g_t.n);
  TEST_ASSERT_EQUAL(K(Tok::kName), K(g_t.t[0].kind));
  TEST_ASSERT_EQUAL(50000, g_t.t[1].milli);
  TEST_ASSERT_TRUE(tokenIs("FD 50 rt 2.5", g_t.t[2], "RT"));
  TEST_ASSERT_EQUAL(2500, g_t.t[3].milli);
  TEST_ASSERT_EQUAL(K(Tok::kEnd), K(g_t.t[4].kind));
}

void test_lex_minus_and_neg() {
  L("10-5 10 - 5 10 -5 (-5 [-5 -:x");
  const Tok want[] = {Tok::kNumber, Tok::kMinus, Tok::kNumber, Tok::kNumber, Tok::kMinus, Tok::kNumber,
                      Tok::kNumber, Tok::kNeg,   Tok::kNumber, Tok::kLParen, Tok::kNeg,   Tok::kNumber,
                      Tok::kLBracket, Tok::kNeg, Tok::kNumber, Tok::kNeg,    Tok::kInput, Tok::kEnd};
  TEST_ASSERT_EQUAL(18, g_t.n);
  for (uint8_t i = 0; i < 18; ++i) TEST_ASSERT_EQUAL_MESSAGE(K(want[i]), K(g_t.t[i].kind), "token kind");
}

void test_lex_lines_columns_comments() {
  L("; hello\n  FD 50 ; one side\nRT 90");
  TEST_ASSERT_EQUAL(2, g_t.t[0].line);
  TEST_ASSERT_EQUAL(3, g_t.t[0].col);
  TEST_ASSERT_EQUAL(3, g_t.t[2].line);
  TEST_ASSERT_EQUAL(1, g_t.t[2].col);
}

void test_lex_names_inputs_quoted_placeholders_operators() {
  L("OBSTACLE? C#5 :size \"Done! ? ?1 ?2 <= >= <> = < > * /");
  const Tok want[] = {Tok::kName, Tok::kName, Tok::kInput, Tok::kQuoted, Tok::kPlaceholder, Tok::kPlaceholder,
                      Tok::kPlaceholder, Tok::kLe, Tok::kGe, Tok::kNe, Tok::kEq, Tok::kLt, Tok::kGt,
                      Tok::kStar, Tok::kSlash, Tok::kEnd};
  for (uint8_t i = 0; i < 16; ++i) TEST_ASSERT_EQUAL(K(want[i]), K(g_t.t[i].kind));
  TEST_ASSERT_EQUAL(2, g_t.t[6].ph);
  char buf[40];
  tokenText("OBSTACLE? C#5 :size \"Done! ?", g_t.t[3], buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("\"Done!", buf);
}

void test_lex_quoted_ends_at_bracket() {
  L("\"ball[ BEEP ]");
  TEST_ASSERT_EQUAL(K(Tok::kQuoted), K(g_t.t[0].kind));
  TEST_ASSERT_EQUAL(5, g_t.t[0].len);
  TEST_ASSERT_EQUAL(K(Tok::kLBracket), K(g_t.t[1].kind));
}

void test_lex_errors_keep_going() {
  L("FD 50 @ FD 10000000 FD 1.2345");
  TEST_ASSERT_EQUAL(7016, g_d.at(0).code);
  TEST_ASSERT_EQUAL(7, g_d.at(0).col);
  TEST_ASSERT_EQUAL(7015, g_d.at(1).code);  // too big: still a NUMBER token (clamped) so the parser goes on
  TEST_ASSERT_EQUAL(K(Tok::kNumber), K(g_t.t[3].kind));
  TEST_ASSERT_EQUAL(7015, g_d.at(2).code);  // 4 decimals
}

void test_lex_token_overflow() {
  static char big[9000];
  for (uint16_t i = 0; i < 8400; i += 2) { big[i] = 'A'; big[i + 1] = ' '; }  // 4200 words > 4096 tokens
  big[8400] = '\0';
  L(big);
  TEST_ASSERT_EQUAL(TokenList::kCap, g_t.n);
  TEST_ASSERT_EQUAL(K(Tok::kEnd), K(g_t.t[g_t.n - 1].kind));
  TEST_ASSERT_EQUAL(7010, g_d.at(0).code);
}

void test_lex_utf8_comment_and_quoted() {
  L("; 🐢 turtle\nSAY \"héllo FD 5");
  TEST_ASSERT_EQUAL(0, g_d.count());
  TEST_ASSERT_EQUAL(K(Tok::kQuoted), K(g_t.t[1].kind));
  TEST_ASSERT_EQUAL(K(Tok::kName), K(g_t.t[2].kind));
  TEST_ASSERT_EQUAL(2, g_t.t[2].line);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lex_words_numbers_and_end);
  RUN_TEST(test_lex_minus_and_neg);
  RUN_TEST(test_lex_lines_columns_comments);
  RUN_TEST(test_lex_names_inputs_quoted_placeholders_operators);
  RUN_TEST(test_lex_quoted_ends_at_bracket);
  RUN_TEST(test_lex_errors_keep_going);
  RUN_TEST(test_lex_token_overflow);
  RUN_TEST(test_lex_utf8_comment_and_quoted);
  return UNITY_END();
}
```

- [ ] **Step 2: Run to see it fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_lexer`
Expected: FAIL — `logo/lexer.h: No such file or directory`.

- [ ] **Step 3: Implement**

`lexer.h`:
```cpp
// Tank Logo tokens (grammar.md §2, LLD §4). The whole source is tokenised once; the list always ends with kEnd.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/diag.h"

namespace cq {
namespace logo {

enum class Tok : uint8_t {
  kEnd, kNumber, kName, kInput, kQuoted, kPlaceholder, kLBracket, kRBracket, kLParen, kRParen,
  kPlus, kMinus, kNeg, kStar, kSlash, kEq, kNe, kLt, kGt, kLe, kGe,
};

struct Token {
  Tok kind = Tok::kEnd;
  uint16_t start = 0;  // byte offset in the source
  uint16_t len = 0;
  uint16_t line = 1;
  uint16_t col = 1;
  int32_t milli = 0;   // kNumber
  uint8_t ph = 0;      // kPlaceholder: 0 = ?, 1 = ?1, 2 = ?2
};

struct TokenList {
  static constexpr uint16_t kCap = 4096;
  Token t[kCap];
  uint16_t n = 0;
};

void lex(const char* src, uint16_t len, TokenList& out, Diagnostics& diags);
bool tokenIs(const char* src, const Token& t, const char* word);  // case-insensitive, NAME tokens only
void tokenText(const char* src, const Token& t, char* out, size_t cap);

}  // namespace logo
}  // namespace cq
```
`lexer.cpp`:
```cpp
#include "logo/lexer.h"

#include <stdio.h>
#include <string.h>

#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

bool isLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isNameChar(char c) { return isLetter(c) || isDigit(c) || c == '_' || c == '#'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isDelim(char c) { return c == '\0' || isSpace(c) || c == '[' || c == ']' || c == '(' || c == ')'; }
char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

class Lex {
 public:
  Lex(const char* s, uint16_t n, TokenList& o, Diagnostics& d) : m_s(s), m_n(n), m_o(o), m_d(d) {}
  void run();

 private:
  char at(uint16_t i) const { return i < m_n ? m_s[i] : '\0'; }
  void push(Tok k, uint16_t start, uint16_t len, uint16_t col, int32_t milli = 0, uint8_t ph = 0);
  void advance(uint16_t n) { m_i = static_cast<uint16_t>(m_i + n); m_col = static_cast<uint16_t>(m_col + n); }
  void number(uint16_t start, uint16_t col);
  void tooBig(uint16_t col, uint16_t start, uint16_t len);

  const char* m_s;
  uint16_t m_n;
  TokenList& m_o;
  Diagnostics& m_d;
  uint16_t m_i = 0;
  uint16_t m_line = 1;
  uint16_t m_col = 1;
  bool m_full = false;
};

void Lex::push(Tok k, uint16_t start, uint16_t len, uint16_t col, int32_t milli, uint8_t ph) {
  if (m_o.n >= TokenList::kCap - 1) {  // keep the last slot for kEnd
    if (!m_full) {
      const DiagArg a[] = {{"what", "too many words"}};
      m_d.add(7010, m_line, col, a, 1);
    }
    m_full = true;
    return;
  }
  Token& t = m_o.t[m_o.n++];
  t.kind = k;
  t.start = start;
  t.len = len;
  t.line = m_line;
  t.col = col;
  t.milli = milli;
  t.ph = ph;
}

void Lex::tooBig(uint16_t col, uint16_t start, uint16_t len) {
  char tok[40];
  snprintf(tok, sizeof tok, "%.*s", len > 32 ? 32 : len, m_s + start);
  const DiagArg a[] = {{"token", tok}};
  m_d.add(7015, m_line, col, a, 1);
}

void Lex::number(uint16_t start, uint16_t col) {
  int64_t whole = 0;
  int32_t frac = 0;
  uint8_t decimals = 0;
  bool big = false;
  while (isDigit(at(m_i))) {
    whole = whole * 10 + (at(m_i) - '0');
    if (whole > 1000000000) big = true;
    advance(1);
  }
  if (at(m_i) == '.' && isDigit(at(m_i + 1))) {
    advance(1);
    while (isDigit(at(m_i))) {
      if (decimals < 3) frac = frac * 10 + (at(m_i) - '0');
      ++decimals;
      advance(1);
    }
  }
  for (uint8_t d = decimals; d < 3; ++d) frac *= 10;
  int64_t milli = whole * 1000 + frac;
  const uint16_t len = static_cast<uint16_t>(m_i - start);
  if (big || milli > gen::kNumberAbsMaxMilli || decimals > 3) {
    tooBig(col, start, len);
    if (milli > gen::kNumberAbsMaxMilli || big) milli = gen::kNumberAbsMaxMilli;
  }
  push(Tok::kNumber, start, len, col, static_cast<int32_t>(milli));
}

void Lex::run() {
  m_o.n = 0;
  while (m_i < m_n) {
    const char c = at(m_i);
    if (c == '\n') { ++m_i; ++m_line; m_col = 1; continue; }
    if (isSpace(c)) { advance(1); continue; }
    if (c == ';') { while (m_i < m_n && at(m_i) != '\n') advance(1); continue; }
    const uint16_t s = m_i, col = m_col;
    if (isDigit(c)) { number(s, col); continue; }
    if (isLetter(c)) {
      while (isNameChar(at(m_i))) advance(1);
      if (at(m_i) == '?') advance(1);
      const uint16_t len = static_cast<uint16_t>(m_i - s);
      if (len > gen::kNameChars) tooBig(col, s, len);
      push(Tok::kName, s, len, col);
      continue;
    }
    if (c == ':' && isLetter(at(m_i + 1))) {
      advance(1);
      while (isNameChar(at(m_i))) advance(1);
      if (at(m_i) == '?') advance(1);
      push(Tok::kInput, s, static_cast<uint16_t>(m_i - s), col);
      continue;
    }
    if (c == '"') {
      advance(1);
      while (!isDelim(at(m_i))) advance(1);
      const uint16_t len = static_cast<uint16_t>(m_i - s);
      if (len - 1 > gen::kNameChars) tooBig(col, s, len);
      push(Tok::kQuoted, s, len, col);
      continue;
    }
    if (c == '?') {
      const char n1 = at(m_i + 1);
      if ((n1 == '1' || n1 == '2') && isDelim(at(m_i + 2))) {
        advance(2);
        push(Tok::kPlaceholder, s, 2, col, 0, static_cast<uint8_t>(n1 - '0'));
        continue;
      }
      if (isDelim(n1)) { advance(1); push(Tok::kPlaceholder, s, 1, col); continue; }
    }
    if (c == '-') {
      const char prev = m_i == 0 ? ' ' : at(m_i - 1);
      const bool neg = (isSpace(prev) || prev == '[' || prev == '(') && m_i + 1 < m_n && !isSpace(at(m_i + 1));
      advance(1);
      push(neg ? Tok::kNeg : Tok::kMinus, s, 1, col);
      continue;
    }
    Tok k = Tok::kEnd;
    uint16_t len = 1;
    switch (c) {
      case '[': k = Tok::kLBracket; break;
      case ']': k = Tok::kRBracket; break;
      case '(': k = Tok::kLParen; break;
      case ')': k = Tok::kRParen; break;
      case '+': k = Tok::kPlus; break;
      case '*': k = Tok::kStar; break;
      case '/': k = Tok::kSlash; break;
      case '=': k = Tok::kEq; break;
      case '<':
        if (at(m_i + 1) == '=') { k = Tok::kLe; len = 2; }
        else if (at(m_i + 1) == '>') { k = Tok::kNe; len = 2; }
        else { k = Tok::kLt; }
        break;
      case '>':
        if (at(m_i + 1) == '=') { k = Tok::kGe; len = 2; } else { k = Tok::kGt; }
        break;
      default: break;
    }
    if (k == Tok::kEnd) {
      char ch[8];
      snprintf(ch, sizeof ch, "%c", c);
      char line[8];
      snprintf(line, sizeof line, "%u", static_cast<unsigned>(m_line));
      const DiagArg a[] = {{"char", ch}, {"line", line}};
      m_d.add(7016, m_line, col, a, 2);
      advance(1);
      continue;
    }
    advance(len);
    push(k, s, len, col);
  }
  Token& e = m_o.t[m_o.n++];
  e = Token();
  e.kind = Tok::kEnd;
  e.start = m_n;
  e.line = m_line;
  e.col = m_col;
}

}  // namespace

void lex(const char* src, uint16_t len, TokenList& out, Diagnostics& diags) {
  Lex l(src, len, out, diags);
  l.run();
}

bool tokenIs(const char* src, const Token& t, const char* word) {
  if (t.kind != Tok::kName || strlen(word) != t.len) return false;
  for (uint16_t i = 0; i < t.len; ++i) {
    if (upper(src[t.start + i]) != word[i]) return false;
  }
  return true;
}

void tokenText(const char* src, const Token& t, char* out, size_t cap) {
  if (t.kind == Tok::kEnd) { snprintf(out, cap, "the end"); return; }
  snprintf(out, cap, "%.*s", static_cast<int>(t.len), src + t.start);
}

}  // namespace logo
}  // namespace cq
```
(Note: the multi-byte UTF-8 characters of comments and quoted words are skipped / kept byte by byte; columns count bytes — enough to point at ASCII tokens on the same line.)

- [ ] **Step 4: Run to see it pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_lexer`
Expected: `8 test cases: 8 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/logo/lexer.* firmware/esp32/test/test_logo_lexer
git commit -m "feat(logo): lexer - tokens, MINUS vs NEG, milli numbers, 7015 / 7016 / 7010

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Tree, word lookups and pass 1 (definitions)

**Files:** Create `firmware/esp32/src/logo/ast.h`, `ast.cpp`, `words.h`, `words.cpp`, `parser.h` (pass 1 part), `parser.cpp` (pass 1 part); Test `firmware/esp32/test/test_logo_parser/test_main.cpp` (pass-1 tests).

**Interfaces:**
- Consumes: `TokenList`, `tokenIs`, `tokenText`, `Diagnostics`, `gen::*`.
- Produces (`ast.h`): `enum class NodeKind : uint8_t { kProgram, kDef, kParams, kBlock, kTemplate, kList, kCall, kProcCall, kNumber, kWord, kQuoted, kInput, kPlaceholder, kBool, kPreset, kUnary, kBinary }`; `enum class Op : uint8_t { kNone, kAdd, kSub, kMul, kDiv, kEq, kNe, kLt, kGt, kLe, kGe, kAnd, kOr, kNot, kNeg }`; `struct Node { NodeKind kind; Op op; bool tail; int16_t ref; uint16_t tok; uint8_t ph; int32_t milli; Node* first; Node* last; Node* next; uint8_t count; void add(Node*); }`; `class NodePool { NodePool(Node* buf, uint16_t cap); Node* make(NodeKind, uint16_t tok); uint16_t used() const; bool full() const; }`.
- Produces (`words.h`): `int16_t findWord(const char* src, const Token& t)` (name or alias → index in `gen::kWords`, −1); `int16_t wordIndex(const char* name)`; `bool isPresetOf(const char* src, const Token& t, gen::InType place)`; `bool isAnyPreset(const char* src, const Token& t)`; `bool isReserved(const char* src, const Token& t)`; `uint8_t editDistance(const char* a, uint16_t alen, const char* b, uint16_t blen)` (case-insensitive, capped at 3); `void presetChoices(gen::InType place, char* out, size_t cap)`.
- Produces (`parser.h`): `struct ProcInfo { uint16_t to_tok; uint16_t name_tok; uint16_t first_input_tok; uint8_t n_inputs; bool gives_value; bool valid; }`; `class Parser { static constexpr uint8_t kMaxProcs = 64; Parser(NodePool&, Diagnostics&); Node* parse(const char* src, const TokenList& toks); uint8_t procCount() const; const ProcInfo& proc(uint8_t) const; int16_t findProc(const Token&) const; }`.

- [ ] **Step 1: Write the failing pass-1 tests**

`firmware/esp32/test/test_logo_parser/test_main.cpp` (Task 5 adds more tests to this file):
```cpp
// Plan N1 Tasks 4-5: pass 1 (definitions) and pass 2 (statements, inputs, expressions) + the reads printer.
#include <string.h>
#include <unity.h>

#include "logo/lexer.h"
#include "logo/parser.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];
const char* g_src = "";
Parser* g_p = nullptr;
NodePool* g_pool = nullptr;
Node* parseText(const char* s) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  g_src = s;
  g_d.clear();
  pool.reset();
  lex(s, static_cast<uint16_t>(strlen(s)), g_t, g_d);
  g_p = &p;
  g_pool = &pool;
  return p.parse(s, g_t);
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_pass1_finds_definitions_before_use() {
  parseText("SQUARE 30\nTO SQUARE :s REPEAT 4 [ FD :s RT 90 ] END\nTO SIDE OUTPUT 50 END");
  TEST_ASSERT_EQUAL(2, g_p->procCount());
  TEST_ASSERT_EQUAL(1, g_p->proc(0).n_inputs);
  TEST_ASSERT_FALSE(g_p->proc(0).gives_value);
  TEST_ASSERT_TRUE(g_p->proc(1).gives_value);
  TEST_ASSERT_EQUAL(0, g_d.errorCount());
}

void test_pass1_errors() {
  parseText("TO A FD 10 END\nTO A FD 20 END");
  TEST_ASSERT_EQUAL(7007, g_d.first()->code);
  parseText("TO FORWARD :n BK :n END");
  TEST_ASSERT_EQUAL(7008, g_d.first()->code);
  parseText("TO RED BEEP END");
  TEST_ASSERT_EQUAL(7008, g_d.first()->code);
  parseText("REPEAT 2 [ TO A BEEP END ]");
  TEST_ASSERT_EQUAL(7017, g_d.first()->code);
  TEST_ASSERT_EQUAL(12, g_d.first()->col);
  parseText("TO SQUARE :s FD :s");
  TEST_ASSERT_EQUAL(7003, g_d.first()->code);
  TEST_ASSERT_EQUAL(1, g_d.first()->col);
}

void test_edit_distance() {
  TEST_ASSERT_EQUAL(2, editDistance("SQAURE", 6, "SQUARE", 6));
  TEST_ASSERT_EQUAL(1, editDistance("sise", 4, "size", 4));
  TEST_ASSERT_EQUAL(3, editDistance("A", 1, "FORWARD", 7));  // capped
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pass1_finds_definitions_before_use);
  RUN_TEST(test_pass1_errors);
  RUN_TEST(test_edit_distance);
  return UNITY_END();
}
```

- [ ] **Step 2: Run to see it fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_parser`
Expected: FAIL — `logo/parser.h: No such file or directory`.

- [ ] **Step 3: `ast.h/.cpp`**

```cpp
// Tank Logo syntax tree (LLD §5): nodes from a fixed pool, children as a singly linked list.
#pragma once

#include <stdint.h>

namespace cq {
namespace logo {

enum class NodeKind : uint8_t {
  kProgram, kDef, kParams, kBlock, kTemplate, kList, kCall, kProcCall,
  kNumber, kWord, kQuoted, kInput, kPlaceholder, kBool, kPreset, kUnary, kBinary,
};
enum class Op : uint8_t { kNone, kAdd, kSub, kMul, kDiv, kEq, kNe, kLt, kGt, kLe, kGe, kAnd, kOr, kNot, kNeg };

struct Node {
  NodeKind kind = NodeKind::kProgram;
  Op op = Op::kNone;
  bool tail = false;      // set by the checker (grammar.md §8)
  int16_t ref = -1;       // kCall: gen::kWords index; kProcCall / kDef: proc index; kPreset: gen::InType
  uint16_t tok = 0;       // the token the node starts at
  uint8_t ph = 0;         // kPlaceholder
  int32_t milli = 0;      // kNumber; kBool 0 / 1
  Node* first = nullptr;
  Node* last = nullptr;
  Node* next = nullptr;
  uint8_t count = 0;
  void add(Node* c) {
    if (first == nullptr) first = c; else last->next = c;
    last = c;
    ++count;
  }
};

class NodePool {
 public:
  NodePool(Node* buf, uint16_t cap) : m_buf(buf), m_cap(cap) {}
  Node* make(NodeKind k, uint16_t tok);  // nullptr when full
  uint16_t used() const { return m_used; }
  bool full() const { return m_used >= m_cap; }
  void reset() { m_used = 0; }

 private:
  Node* m_buf;
  uint16_t m_cap;
  uint16_t m_used = 0;
};

}  // namespace logo
}  // namespace cq
```
`ast.cpp`:
```cpp
#include "logo/ast.h"

namespace cq {
namespace logo {

Node* NodePool::make(NodeKind k, uint16_t tok) {
  if (m_used >= m_cap) return nullptr;
  Node* n = &m_buf[m_used++];
  *n = Node();
  n->kind = k;
  n->tok = tok;
  return n;
}

}  // namespace logo
}  // namespace cq
```

- [ ] **Step 4: `words.h/.cpp` — lookups**

```cpp
// Lookups over the generated word table (LLD §4): names, aliases, presets, reserved words, edit distance.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/generated/words.h"
#include "logo/lexer.h"

namespace cq {
namespace logo {

int16_t findWord(const char* src, const Token& t);  // name or alias -> gen::kWords index, -1
int16_t wordIndex(const char* name);                // exact canonical name -> index, -1
bool isPresetOf(const char* src, const Token& t, gen::InType place);
bool isAnyPreset(const char* src, const Token& t);
bool isReserved(const char* src, const Token& t);
uint8_t editDistance(const char* a, uint16_t alen, const char* b, uint16_t blen);  // case-insensitive, max 3
void presetChoices(gen::InType place, char* out, size_t cap);                      // "RED GREEN …"

}  // namespace logo
}  // namespace cq
```
`words.cpp`:
```cpp
#include "logo/words.h"

#include <string.h>

namespace cq {
namespace logo {
namespace {

struct List {
  const char* const* items;
  uint8_t n;
};

List presets(gen::InType place) {
  switch (place) {  // VARIANT POINT: preset places
    case gen::InType::kSide: return {gen::kSides, gen::kSidesCount};
    case gen::InType::kColour: return {gen::kColours, gen::kColoursCount};
    case gen::InType::kLook: return {gen::kLooks, gen::kLooksCount};
    case gen::InType::kSound: return {gen::kSounds, gen::kSoundsCount};
    default: return {nullptr, 0};
  }
}

bool inList(const char* src, const Token& t, List l) {
  for (uint8_t i = 0; i < l.n; ++i) {
    if (tokenIs(src, t, l.items[i])) return true;
  }
  return false;
}

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

}  // namespace

int16_t findWord(const char* src, const Token& t) {
  if (t.kind != Tok::kName) return -1;
  for (uint8_t i = 0; i < gen::kWordCount; ++i) {
    if (tokenIs(src, t, gen::kWords[i].name)) return i;
  }
  for (uint8_t i = 0; i < gen::kAliasCount; ++i) {
    if (tokenIs(src, t, gen::kAliases[i].alias)) return gen::kAliases[i].word;
  }
  return -1;
}

int16_t wordIndex(const char* name) {
  for (uint8_t i = 0; i < gen::kWordCount; ++i) {
    if (strcmp(gen::kWords[i].name, name) == 0) return i;
  }
  return -1;
}

bool isPresetOf(const char* src, const Token& t, gen::InType place) { return inList(src, t, presets(place)); }

bool isAnyPreset(const char* src, const Token& t) {
  return isPresetOf(src, t, gen::InType::kSide) || isPresetOf(src, t, gen::InType::kColour) ||
         isPresetOf(src, t, gen::InType::kLook) || isPresetOf(src, t, gen::InType::kSound);
}

bool isReserved(const char* src, const Token& t) { return inList(src, t, {gen::kReserved, gen::kReservedCount}); }

uint8_t editDistance(const char* a, uint16_t alen, const char* b, uint16_t blen) {
  if (alen > 32 || blen > 32) return 3;
  uint8_t prev[33], cur[33];
  for (uint16_t j = 0; j <= blen; ++j) prev[j] = static_cast<uint8_t>(j);
  for (uint16_t i = 1; i <= alen; ++i) {
    cur[0] = static_cast<uint8_t>(i);
    for (uint16_t j = 1; j <= blen; ++j) {
      const uint8_t cost = upper(a[i - 1]) == upper(b[j - 1]) ? 0 : 1;
      uint8_t v = static_cast<uint8_t>(prev[j] + 1);
      if (cur[j - 1] + 1 < v) v = static_cast<uint8_t>(cur[j - 1] + 1);
      if (prev[j - 1] + cost < v) v = static_cast<uint8_t>(prev[j - 1] + cost);
      cur[j] = v;
    }
    memcpy(prev, cur, blen + 1u);
  }
  return prev[blen] > 3 ? 3 : prev[blen];
}

void presetChoices(gen::InType place, char* out, size_t cap) {
  const List l = presets(place);
  out[0] = '\0';
  for (uint8_t i = 0; i < l.n && i < 8; ++i) {
    if (i > 0) strncat(out, " ", cap - strlen(out) - 1);
    strncat(out, l.items[i], cap - strlen(out) - 1);
  }
  if (l.n > 8) strncat(out, " …", cap - strlen(out) - 1);
}

}  // namespace logo
}  // namespace cq
```

- [ ] **Step 5: `parser.h` and pass 1 in `parser.cpp`**

`parser.h` (complete; pass 2 members are implemented in Task 5):
```cpp
// Tank Logo parser (grammar.md §3-§9, LLD §5): pass 1 finds every TO; pass 2 reads statements with an explicit
// frame stack (no recursion, coding standards §2.3). Errors go to Diagnostics; the tree is always returned.
#pragma once

#include <stdint.h>

#include "logo/ast.h"
#include "logo/diag.h"
#include "logo/generated/words.h"
#include "logo/lexer.h"

namespace cq {
namespace logo {

struct ProcInfo {
  uint16_t to_tok = 0;
  uint16_t name_tok = 0;
  uint16_t first_input_tok = 0;
  uint8_t n_inputs = 0;
  bool gives_value = false;
  bool valid = false;
};

class Parser {
 public:
  static constexpr uint8_t kMaxProcs = 64;
  static constexpr uint8_t kMaxFrames = 64;
  static constexpr uint8_t kMaxStack = 128;

  Parser(NodePool& pool, Diagnostics& diags) : m_pool(pool), m_d(diags) {}
  Node* parse(const char* src, const TokenList& toks);
  uint8_t procCount() const { return m_n_procs; }
  const ProcInfo& proc(uint8_t i) const { return m_procs[i]; }
  int16_t findProc(const Token& t) const;  // valid procedures only

 private:
  enum class FK : uint8_t { kSeq, kCall, kExpr, kList };
  enum class Close : uint8_t { kNone, kEof, kBracket, kEnd, kParen };
  struct Frame {
    FK kind = FK::kSeq;
    Close close = Close::kNone;
    Node* node = nullptr;
    uint16_t open_tok = 0;
    uint8_t next_input = 0;
    bool attached = true;     // kSeq / kList: already linked into the tree
    bool full = false;        // kExpr: a condition (comparisons, AND / OR)
    bool wrap_template = false;
    bool expect_operand = true;
    bool seen_compare = false;
    uint8_t opnd_base = 0;
    uint8_t op_base = 0;
  };
  struct OpEntry {
    Op op;
    uint16_t tok;
  };

  // pass 1
  void collectDefinitions();
  // pass 2 steps
  void stepSeq(Frame& f);
  void stepCall(Frame& f);
  void stepExpr(Frame& f);
  void stepList(Frame& f);
  void parseDef();
  void skipToEnd();
  // frames
  bool push(const Frame& f);
  void pushSeq(Close c, Node* n, uint16_t open);
  void pushExpr(bool full, Close c, bool wrap, uint16_t open);
  void pushList(Node* n, uint16_t open, bool attached);
  void startCall(int16_t word, uint16_t tok);
  void startProcCall(int16_t proc, uint16_t tok);
  void deliver(Node* n);
  void finishExpr();
  void reduceTop();
  bool pushOperand(Node* n);
  // helpers
  const Token& tok() const { return m_toks->t[m_i]; }
  const Token& tokAt(uint16_t i) const { return m_toks->t[i]; }
  Node* make(NodeKind k, uint16_t tok);
  bool startsValue(const Token& t) const;
  bool givesValue(const Token& t) const;
  bool isStatementStart(const Token& t) const;
  bool isCommandWord(const Token& t) const;
  void error(uint16_t code, uint16_t tok, const DiagArg* a = nullptr, uint8_t n = 0);
  void minusHint(uint16_t at);
  void unknownWord(uint16_t at);
  void leftover(uint16_t at);
  void missingInput(const Frame& call);
  void fail(bool skip);
  void unwindToSeq();
  void resync();
  void inputSpec(const Frame& call, gen::InType& type, const char*& what, const char*& example,
                 const char*& name) const;

  NodePool& m_pool;
  Diagnostics& m_d;
  const char* m_src = "";
  const TokenList* m_toks = nullptr;
  uint16_t m_i = 0;
  bool m_abort = false;
  uint16_t m_last_value_tok = 0xFFFF;
  ProcInfo m_procs[kMaxProcs];
  uint8_t m_n_procs = 0;
  Frame m_f[kMaxFrames];
  uint8_t m_nf = 0;
  Node* m_opnd[kMaxStack];
  uint8_t m_nopnd = 0;
  OpEntry m_ops[kMaxStack];
  uint8_t m_nops = 0;
  Node* m_root = nullptr;
};

}  // namespace logo
}  // namespace cq
```
`parser.cpp` — Task 4 part (pass 1, lookups, errors; the pass-2 functions come in Task 5; in Task 4 `parse()` only runs pass 1 and returns an empty program):
```cpp
#include "logo/parser.h"

#include <stdio.h>
#include <string.h>

#include "logo/words.h"

namespace cq {
namespace logo {

Node* Parser::make(NodeKind k, uint16_t tok) {
  Node* n = m_pool.make(k, tok);
  if (n == nullptr && !m_abort) {
    const DiagArg a[] = {{"what", "too many words"}};
    m_d.add(7010, tokAt(tok).line, tokAt(tok).col, a, 1);
    m_abort = true;
  }
  return n;
}

void Parser::error(uint16_t code, uint16_t at, const DiagArg* a, uint8_t n) {
  m_d.add(code, tokAt(at).line, tokAt(at).col, a, n);
}

int16_t Parser::findProc(const Token& t) const {
  if (t.kind != Tok::kName) return -1;
  for (uint8_t i = 0; i < m_n_procs; ++i) {
    const Token& n = tokAt(m_procs[i].name_tok);
    if (m_procs[i].valid && n.len == t.len && editDistance(m_src + n.start, n.len, m_src + t.start, t.len) == 0) {
      return i;
    }
  }
  return -1;
}

void Parser::collectDefinitions() {
  m_n_procs = 0;
  uint16_t i = 0;
  int depth = 0;
  while (tokAt(i).kind != Tok::kEnd) {
    const Token& t = tokAt(i);
    if (t.kind == Tok::kLBracket) ++depth;
    if (t.kind == Tok::kRBracket && depth > 0) --depth;
    if (!tokenIs(m_src, t, "TO")) { ++i; continue; }
    if (depth > 0) { error(7017, i); ++i; continue; }
    ProcInfo p;
    p.to_tok = i++;
    if (tokAt(i).kind != Tok::kName) {
      const DiagArg a[] = {{"word", "TO"}, {"what", "a name"}, {"example", "TO SQUARE :size"}};
      error(7002, p.to_tok, a, 3);
      continue;
    }
    p.name_tok = i;
    p.valid = true;
    const Token& name = tokAt(i);
    char nm[40];
    tokenText(m_src, name, nm, sizeof nm);
    if (findWord(m_src, name) >= 0 || isAnyPreset(m_src, name) || isReserved(m_src, name) ||
        tokenIs(m_src, name, "TO")) {
      const DiagArg a[] = {{"word", nm}};
      error(7008, i, a, 1);
      p.valid = false;
    } else {
      for (uint8_t k = 0; k < m_n_procs; ++k) {
        const Token& o = tokAt(m_procs[k].name_tok);
        if (o.len == name.len && editDistance(m_src + o.start, o.len, m_src + name.start, name.len) == 0) {
          char la[8], lb[8];
          snprintf(la, sizeof la, "%u", static_cast<unsigned>(o.line));
          snprintf(lb, sizeof lb, "%u", static_cast<unsigned>(name.line));
          const DiagArg a[] = {{"word", nm}, {"a", la}, {"b", lb}};
          error(7007, i, a, 3);
          p.valid = false;
        }
      }
    }
    ++i;
    p.first_input_tok = i;
    while (tokAt(i).kind == Tok::kInput) { ++p.n_inputs; ++i; }
    int d = 0;
    bool found = false;
    while (tokAt(i).kind != Tok::kEnd) {
      const Token& b = tokAt(i);
      if (b.kind == Tok::kLBracket) ++d;
      else if (b.kind == Tok::kRBracket) --d;
      else if (tokenIs(m_src, b, "TO")) error(7017, i);
      else if (tokenIs(m_src, b, "OUTPUT") || tokenIs(m_src, b, "OP")) p.gives_value = true;
      else if (tokenIs(m_src, b, "END") && d <= 0) { found = true; break; }
      ++i;
    }
    if (!found) {
      char line[8];
      snprintf(line, sizeof line, "%u", static_cast<unsigned>(tokAt(p.to_tok).line));
      const DiagArg a[] = {{"bracket", "TO"}, {"line", line}, {"closer", "END"}};
      error(7003, p.to_tok, a, 3);
    } else {
      ++i;
    }
    if (m_n_procs < kMaxProcs) {
      m_procs[m_n_procs++] = p;
    } else {
      const DiagArg a[] = {{"what", "more than 64 words of your own"}};
      error(7010, p.to_tok, a, 1);
    }
  }
}

Node* Parser::parse(const char* src, const TokenList& toks) {
  m_src = src;
  m_toks = &toks;
  m_i = 0;
  m_abort = false;
  m_nf = m_nopnd = m_nops = 0;
  m_last_value_tok = 0xFFFF;
  collectDefinitions();
  m_root = make(NodeKind::kProgram, 0);
  return m_root;  // Task 5 adds pass 2 here
}

}  // namespace logo
}  // namespace cq
```

- [ ] **Step 6: Run to see it pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_parser`
Expected: `3 test cases: 3 succeeded`. (The pass-2 member functions are declared but not yet defined; they are not called yet, so the test binary links.)

- [ ] **Step 7: Commit**

```bash
git add firmware/esp32/src/logo/ast.* firmware/esp32/src/logo/words.* firmware/esp32/src/logo/parser.* firmware/esp32/test/test_logo_parser
git commit -m "feat(logo): syntax tree, word lookups and parser pass 1 (definitions, 7002/7003/7007/7008/7017)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Parser pass 2 + the `reads` printer

**Files:** Modify `firmware/esp32/src/logo/parser.cpp`; Create `firmware/esp32/src/logo/printer.h`, `printer.cpp`; Test: add to `firmware/esp32/test/test_logo_parser/test_main.cpp`.

**Interfaces:**
- Consumes: everything from Tasks 2–4.
- Produces: `Parser::parse()` builds the full tree (grammar.md §3); `void printReads(const Node* program, const char* src, const TokenList& toks, const Parser& p, char* out, size_t cap)` (canonical form: single spaces, none after `( [ {` or before `) ] }` — the same as `logogen.emit.normalize_reads`).

- [ ] **Step 1: Add the failing tests**

Add to `test_logo_parser/test_main.cpp` (and `#include "logo/printer.h"`; register each with `RUN_TEST`):
```cpp
namespace {
const char* R(const char* s) {
  static char out[4096];
  Node* prog = parseText(s);
  printReads(prog, g_src, g_t, *g_p, out, sizeof out);
  return out;
}
}  // namespace

void test_reads_simple_statements() {
  TEST_ASSERT_EQUAL_STRING("(FORWARD 50) (RIGHT 90)", R("fd 50 RT 90"));
  TEST_ASSERT_EQUAL_STRING("(REPEAT 4 [(FORWARD 50) (RIGHT 90)])", R("REPEAT 4 [ FD 50 RT 90 ]"));
  TEST_ASSERT_EQUAL(0, g_d.errorCount());
}

void test_reads_expressions_and_precedence() {
  TEST_ASSERT_EQUAL_STRING("(FORWARD (+ 2 (* 3 4)))", R("FD 2 + 3 * 4"));
  TEST_ASSERT_EQUAL_STRING("(IF (= (RANDOM 2) 1) [(BEEP)])", R("IF RANDOM 2 = 1 [ BEEP ]"));
  TEST_ASSERT_EQUAL_STRING("(IF (OR (AND (NOT (OBSTACLE?)) (> (DISTANCE) 50)) (> (TIMER) 10)) [(BEEP)])",
                           R("IF NOT OBSTACLE? AND DISTANCE > 50 OR TIMER > 10 [ BEEP ]"));
  TEST_ASSERT_EQUAL_STRING("(LOOK {-30 10})", R("LOOK [ -30 10 ]"));
}

void test_reads_definitions_presets_wait_until() {
  TEST_ASSERT_EQUAL_STRING("(TO SIDE () [(OUTPUT 50)]) (FORWARD (SIDE))", R("TO SIDE OUTPUT 50 END FD SIDE"));
  TEST_ASSERT_EQUAL_STRING("(ARC RIGHT 90 50) (PIVOT LEFT 90) (LEFT 45)", R("ARC RIGHT 90 50 PIVOT LEFT 90 LEFT 45"));
  TEST_ASSERT_EQUAL_STRING("(WAIT-UNTIL (< (DISTANCE) 20)) (BEEP)", R("WAIT UNTIL DISTANCE < 20 BEEP"));
}

void test_parser_minus_cases() {
  R("FD 10 -5");
  TEST_ASSERT_EQUAL(7004, g_d.first()->code);
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, "10 - 5"));
  R("FD (10 -5)");
  TEST_ASSERT_EQUAL(7028, g_d.first()->code);
  R("REPEAT 10 -5 [ BEEP ]");
  TEST_ASSERT_EQUAL(7023, g_d.first()->code);
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, "10 - 5"));
}

void test_parser_empty_program() {
  TEST_ASSERT_EQUAL_STRING("", R("  ; only a comment\n"));
  TEST_ASSERT_EQUAL(0, g_d.count());
}

void test_parser_deep_nesting_is_an_error() {
  static char s[600];
  strcpy(s, "FD ");
  for (int i = 0; i < 200; ++i) strcat(s, "(");
  strcat(s, "1");
  R(s);
  TEST_ASSERT_EQUAL(7010, g_d.first()->code);
}

void test_parser_node_pool_full() {
  // nodes and tokens are about one to one, so the token cap is hit first with the real pool: use a small pool
  static Node small[50];
  NodePool pool(small, 50);
  Diagnostics d;
  Parser p(pool, d);
  static char s[300];
  s[0] = '\0';
  for (int i = 0; i < 40; ++i) strcat(s, "FD 1 ");  // 80 nodes
  lex(s, static_cast<uint16_t>(strlen(s)), g_t, d);
  p.parse(s, g_t);
  TEST_ASSERT_EQUAL(7010, d.first()->code);
}

void test_parser_always_terminates_on_garbage() {
  const char* bad[] = {"]", "[", ")", "(", "END", "TO", "TO X", "IF", "IFELSE [", "FD", "FD ]", "( ( (", "] ] ]",
                       "REPEAT 4 [ ] ]", "+ - * /", "FD 1 + ", "IF 1 < [ ]", "LED", "LED [", "WAIT UNTIL",
                       "TO A :x A :x", "\"", ":", "? ?1 ?2", "[ [ [ 1 ] ] ]"};
  for (const char* b : bad) R(b);  // must return; every case finishing is the test
  TEST_PASS();
}
```

- [ ] **Step 2: Run to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_parser`
Expected: FAIL — link errors: `undefined reference to printReads` (and pass 2 not run).

- [ ] **Step 3: Pass 2 in `parser.cpp`**

Replace the end of `Parser::parse()` with:
```cpp
  m_root = make(NodeKind::kProgram, 0);
  if (m_root == nullptr) return nullptr;
  pushSeq(Close::kEof, m_root, 0);
  while (m_nf > 0 && !m_abort) {
    Frame& f = m_f[m_nf - 1];
    switch (f.kind) {  // VARIANT POINT: parser frames
      case FK::kSeq: stepSeq(f); break;
      case FK::kCall: stepCall(f); break;
      case FK::kExpr: stepExpr(f); break;
      case FK::kList: stepList(f); break;
    }
  }
  return m_root;
}
```
and add (same file, inside the namespace):
```cpp
namespace {
int prec(Op o) {
  switch (o) {
    case Op::kNeg: return 1;
    case Op::kMul: case Op::kDiv: return 2;
    case Op::kAdd: case Op::kSub: return 3;
    case Op::kEq: case Op::kNe: case Op::kLt: case Op::kGt: case Op::kLe: case Op::kGe: return 4;
    case Op::kNot: return 5;
    case Op::kAnd: return 6;
    case Op::kOr: return 7;
    default: return 9;
  }
}
bool isCompare(Op o) { return prec(o) == 4; }
bool isUnary(Op o) { return o == Op::kNeg || o == Op::kNot; }
}  // namespace

bool Parser::push(const Frame& f) {
  if (m_nf >= kMaxFrames) {
    const DiagArg a[] = {{"what", "too deeply nested"}};
    error(7010, m_i, a, 1);
    m_abort = true;
    return false;
  }
  m_f[m_nf++] = f;
  return true;
}

void Parser::pushSeq(Close c, Node* n, uint16_t open) {
  Frame f;
  f.kind = FK::kSeq;
  f.close = c;
  f.node = n;
  f.open_tok = open;
  push(f);
}

void Parser::pushExpr(bool full, Close c, bool wrap, uint16_t open) {
  Frame f;
  f.kind = FK::kExpr;
  f.close = c;
  f.full = full;
  f.wrap_template = wrap;
  f.open_tok = open;
  f.opnd_base = m_nopnd;
  f.op_base = m_nops;
  push(f);
}

void Parser::pushList(Node* n, uint16_t open, bool attached) {
  Frame f;
  f.kind = FK::kList;
  f.node = n;
  f.open_tok = open;
  f.attached = attached;
  push(f);
}

void Parser::startCall(int16_t word, uint16_t at) {
  Node* n = make(NodeKind::kCall, at);
  if (n == nullptr) return;
  n->ref = word;
  Frame f;
  f.kind = FK::kCall;
  f.node = n;
  f.open_tok = at;
  push(f);
}

void Parser::startProcCall(int16_t proc, uint16_t at) {
  Node* n = make(NodeKind::kProcCall, at);
  if (n == nullptr) return;
  n->ref = proc;
  Frame f;
  f.kind = FK::kCall;
  f.node = n;
  f.open_tok = at;
  push(f);
}

bool Parser::pushOperand(Node* n) {
  if (m_nopnd >= kMaxStack) {
    const DiagArg a[] = {{"what", "too deeply nested"}};
    error(7010, m_i, a, 1);
    m_abort = true;
    return false;
  }
  m_opnd[m_nopnd++] = n;
  return true;
}

void Parser::deliver(Node* n) {
  if (m_nf == 0 || n == nullptr) return;
  Frame& p = m_f[m_nf - 1];
  switch (p.kind) {
    case FK::kSeq: case FK::kList: p.node->add(n); break;
    case FK::kCall: p.node->add(n); ++p.next_input; break;
    case FK::kExpr:
      pushOperand(n);
      p.expect_operand = false;
      m_last_value_tok = static_cast<uint16_t>(m_i - 1);
      break;
  }
}

bool Parser::givesValue(const Token& t) const {
  if (t.kind != Tok::kName) return false;
  if (tokenIs(m_src, t, "TRUE") || tokenIs(m_src, t, "FALSE")) return true;
  const int16_t w = findWord(m_src, t);
  if (w >= 0) return gen::kWords[w].kind == gen::WordKind::kReporter;
  const int16_t p = findProc(t);
  return p >= 0 && m_procs[p].gives_value;
}

bool Parser::isCommandWord(const Token& t) const {
  if (t.kind != Tok::kName) return false;
  const int16_t w = findWord(m_src, t);
  if (w >= 0) {
    const gen::WordKind k = gen::kWords[w].kind;
    return k == gen::WordKind::kCommand || k == gen::WordKind::kControl;
  }
  const int16_t p = findProc(t);
  return p >= 0 && !m_procs[p].gives_value;
}

bool Parser::startsValue(const Token& t) const {
  switch (t.kind) {
    case Tok::kNumber: case Tok::kQuoted: case Tok::kInput: case Tok::kPlaceholder:
    case Tok::kLParen: case Tok::kLBracket: case Tok::kNeg: return true;
    case Tok::kName: return givesValue(t) || tokenIs(m_src, t, "NOT");
    default: return false;
  }
}

bool Parser::isStatementStart(const Token& t) const {
  return isCommandWord(t) || tokenIs(m_src, t, "END");
}

void Parser::minusHint(uint16_t at) {
  if (tokAt(at).kind != Tok::kNeg || m_last_value_tok == 0xFFFF || m_last_value_tok + 1 != at) return;
  char a[40], b[40], hint[96];
  tokenText(m_src, tokAt(m_last_value_tok), a, sizeof a);
  const Token& n = tokAt(static_cast<uint16_t>(at + 1));
  tokenText(m_src, n, b, sizeof b);
  snprintf(hint, sizeof hint, " To subtract, write %s - %s.", a, b);
  m_d.appendToLast(hint);
}

void Parser::unknownWord(uint16_t at) {
  const Token& t = tokAt(at);
  char nm[40], sug[40] = "";
  tokenText(m_src, t, nm, sizeof nm);
  uint8_t best = 3;
  for (uint8_t i = 0; i < m_n_procs; ++i) {
    const Token& p = tokAt(m_procs[i].name_tok);
    const uint8_t d = editDistance(m_src + t.start, t.len, m_src + p.start, p.len);
    if (m_procs[i].valid && d < best) { best = d; tokenText(m_src, p, sug, sizeof sug); }
  }
  for (uint8_t i = 0; i < gen::kWordCount; ++i) {
    const char* w = gen::kWords[i].name;
    const uint8_t d = editDistance(m_src + t.start, t.len, w, static_cast<uint16_t>(strlen(w)));
    if (d < best) { best = d; snprintf(sug, sizeof sug, "%s", w); }
  }
  if (best > 2) sug[0] = '\0';
  const DiagArg a[] = {{"word", nm}, {"suggestion", sug}};
  error(7001, at, a, 2);
}

void Parser::leftover(uint16_t at) {
  char v[40];
  if (tokAt(at).kind == Tok::kLBracket) snprintf(v, sizeof v, "a list [ … ]");
  else tokenText(m_src, tokAt(at), v, sizeof v);
  const DiagArg a[] = {{"value", v}};
  error(7004, at, a, 1);
  minusHint(at);
}

void Parser::inputSpec(const Frame& call, gen::InType& type, const char*& what, const char*& example,
                       const char*& name) const {
  static char pex[48];
  if (call.node->kind == NodeKind::kCall) {
    const gen::WordSpec& w = gen::kWords[call.node->ref];
    const gen::InputSpec& in = w.inputs[call.next_input];
    type = in.type;
    what = w.missing[0] != '\0' ? w.missing : in.what;
    example = w.example;
    name = w.name;
  } else {
    const ProcInfo& p = m_procs[call.node->ref];
    type = gen::InType::kAny;
    what = "a value";
    char nm[40];
    tokenText(m_src, tokAt(p.name_tok), nm, sizeof nm);
    snprintf(pex, sizeof pex, "%s 10", nm);
    example = pex;
    name = "";
  }
}

void Parser::missingInput(const Frame& call) {
  gen::InType type;
  const char *what, *example, *name;
  inputSpec(call, type, what, example, name);
  char nm[40];
  if (name[0] == '\0') tokenText(m_src, tokAt(call.open_tok), nm, sizeof nm);
  else snprintf(nm, sizeof nm, "%s", name);
  const DiagArg a[] = {{"word", nm}, {"what", what}, {"example", example}};
  error(7002, call.open_tok, a, 3);
}

void Parser::unwindToSeq() {
  while (m_nf > 0 && m_f[m_nf - 1].kind != FK::kSeq) {
    const Frame& f = m_f[m_nf - 1];
    if (f.kind == FK::kExpr) { m_nopnd = f.opnd_base; m_nops = f.op_base; }
    --m_nf;
  }
}

void Parser::resync() {
  const uint16_t line0 = tok().line;
  int depth = 0;
  if (tok().kind != Tok::kEnd && tok().kind != Tok::kRBracket) {
    if (tok().kind == Tok::kLBracket) ++depth;
    ++m_i;
  }
  while (tok().kind != Tok::kEnd) {
    const Token& t = tok();
    if (depth == 0 && (t.kind == Tok::kRBracket || t.line > line0 || isStatementStart(t))) return;
    if (t.kind == Tok::kLBracket) ++depth;
    if (t.kind == Tok::kRBracket && depth > 0) --depth;
    ++m_i;
  }
}

void Parser::fail(bool skip) {
  unwindToSeq();
  if (skip) resync();
}

void Parser::skipToEnd() {
  while (tok().kind != Tok::kEnd && !tokenIs(m_src, tok(), "END")) ++m_i;
  if (tok().kind != Tok::kEnd) ++m_i;
}

void Parser::parseDef() {
  int16_t p = -1;
  for (uint8_t i = 0; i < m_n_procs; ++i) {
    if (m_procs[i].to_tok == m_i) p = i;
  }
  if (p < 0 || !m_procs[p].valid) { ++m_i; skipToEnd(); return; }  // pass 1 reported it
  const ProcInfo& info = m_procs[p];
  Node* d = make(NodeKind::kDef, m_i);
  Node* params = make(NodeKind::kParams, m_i);
  Node* body = make(NodeKind::kBlock, m_i);
  if (d == nullptr || params == nullptr || body == nullptr) return;
  d->ref = p;
  for (uint8_t k = 0; k < info.n_inputs; ++k) {
    Node* in = make(NodeKind::kInput, static_cast<uint16_t>(info.first_input_tok + k));
    if (in == nullptr) return;
    params->add(in);
  }
  d->add(params);
  d->add(body);
  m_root->add(d);
  const uint16_t to = m_i;
  m_i = static_cast<uint16_t>(info.first_input_tok + info.n_inputs);
  pushSeq(Close::kEnd, body, to);
}

void Parser::stepSeq(Frame& f) {
  const Token& t = tok();
  const uint16_t at = m_i;
  if (t.kind == Tok::kEnd) {
    if (f.close == Close::kEof) { --m_nf; return; }
    char line[8];
    snprintf(line, sizeof line, "%u", static_cast<unsigned>(tokAt(f.open_tok).line));
    const bool def = f.close == Close::kEnd;
    const DiagArg a[] = {{"bracket", def ? "TO" : "["}, {"line", line}, {"closer", def ? "END" : "]"}};
    error(7003, f.open_tok, a, 3);
    m_abort = true;
    return;
  }
  if (t.kind == Tok::kRBracket) {
    if (f.close == Close::kBracket) { ++m_i; --m_nf; return; }
    char line[8];
    snprintf(line, sizeof line, "%u", static_cast<unsigned>(t.line));
    const DiagArg a[] = {{"char", "]"}, {"line", line}};
    error(7016, at, a, 2);
    ++m_i;
    return;
  }
  if (t.kind != Tok::kName) { leftover(at); fail(true); return; }
  if (tokenIs(m_src, t, "END")) {
    if (f.close == Close::kEnd) { ++m_i; --m_nf; return; }
    unknownWord(at);
    ++m_i;
    return;
  }
  if (tokenIs(m_src, t, "TO")) {
    if (m_nf == 1) parseDef();
    else { ++m_i; skipToEnd(); }  // pass 1 reported 7017
    return;
  }
  if (tokenIs(m_src, t, "MAKE") || tokenIs(m_src, t, "RUN") || tokenIs(m_src, t, "FOR")) {
    char nm[40];
    tokenText(m_src, t, nm, sizeof nm);
    const DiagArg a[] = {{"word", nm}, {"hint", "Values come in as inputs of your own words: TO NAME :input."}};
    error(7027, at, a, 2);
    fail(true);
    return;
  }
  if (tokenIs(m_src, t, "WAIT") && tokenIs(m_src, tokAt(static_cast<uint16_t>(at + 1)), "UNTIL")) {
    m_i = static_cast<uint16_t>(m_i + 2);
    startCall(wordIndex("WAIT UNTIL"), at);
    return;
  }
  const int16_t w = findWord(m_src, t);
  if (w >= 0) {
    const gen::WordKind k = gen::kWords[w].kind;
    if (k == gen::WordKind::kCommand || k == gen::WordKind::kControl) { ++m_i; startCall(w, at); return; }
    leftover(at);
    fail(true);
    return;
  }
  const int16_t p = findProc(t);
  if (p >= 0) {
    if (m_procs[p].gives_value) { leftover(at); fail(true); return; }
    ++m_i;
    startProcCall(p, at);
    return;
  }
  unknownWord(at);
  fail(true);
}

void Parser::stepCall(Frame& f) {
  const uint8_t n_inputs = f.node->kind == NodeKind::kCall ? gen::kWords[f.node->ref].n_inputs
                                                            : m_procs[f.node->ref].n_inputs;
  if (f.next_input >= n_inputs) {
    Node* n = f.node;
    --m_nf;
    deliver(n);
    return;
  }
  gen::InType type;
  const char *what, *example, *name;
  inputSpec(f, type, what, example, name);
  const Token& t = tok();
  const uint16_t at = m_i;
  const bool ends = t.kind == Tok::kEnd || t.kind == Tok::kRBracket || t.kind == Tok::kRParen ||
                    tokenIs(m_src, t, "END") || (isCommandWord(t) && t.line > tokAt(f.open_tok).line);
  if (ends) { missingInput(f); fail(false); return; }
  switch (type) {  // VARIANT POINT: input types (grammar.md §5)
    case gen::InType::kBlock:
    case gen::InType::kTemplateBlock: {
      if (t.kind != Tok::kLBracket) {
        char nm[40];
        snprintf(nm, sizeof nm, "%s", name);
        const DiagArg a[] = {{"word", nm}, {"example", example}};
        error(7023, at, a, 2);
        minusHint(at);
        fail(true);
        return;
      }
      Node* b = make(NodeKind::kBlock, at);
      if (b == nullptr) return;
      f.node->add(b);
      ++f.next_input;
      ++m_i;
      pushSeq(Close::kBracket, b, at);
      return;
    }
    case gen::InType::kTemplate:
      if (t.kind == Tok::kLBracket) { ++m_i; pushExpr(true, Close::kBracket, true, at); }
      else pushExpr(false, Close::kNone, false, at);
      return;
    case gen::InType::kNotes: {
      if (t.kind != Tok::kLBracket) { missingInput(f); fail(true); return; }
      Node* l = make(NodeKind::kList, at);
      if (l == nullptr) return;
      ++m_i;
      pushList(l, at, false);
      return;
    }
    case gen::InType::kSide:
    case gen::InType::kColour:
    case gen::InType::kLook:
    case gen::InType::kSound:
      if (t.kind == Tok::kName && isPresetOf(m_src, t, type)) {
        Node* p = make(NodeKind::kPreset, at);
        if (p == nullptr) return;
        p->ref = static_cast<int16_t>(type);
        ++m_i;
        f.node->add(p);
        ++f.next_input;
        return;
      }
      if (type == gen::InType::kSide || (t.kind == Tok::kName && !givesValue(t))) {
        char nm[40], given[40], choices[96];
        snprintf(nm, sizeof nm, "%s", name);
        tokenText(m_src, t, given, sizeof given);
        presetChoices(type, choices, sizeof choices);
        const DiagArg a[] = {{"word", nm}, {"choices", choices}, {"given", given}};
        error(7029, at, a, 3);
        fail(true);
        return;
      }
      pushExpr(false, Close::kNone, false, at);
      return;
    case gen::InType::kCondition:
      pushExpr(true, Close::kNone, false, at);
      return;
    default:
      pushExpr(false, Close::kNone, false, at);
      return;
  }
}

void Parser::reduceTop() {
  const OpEntry e = m_ops[--m_nops];
  if (isUnary(e.op)) {
    Node* a = m_opnd[--m_nopnd];
    if (e.op == Op::kNeg && a->kind == NodeKind::kNumber) {
      a->milli = -a->milli;
      a->tok = e.tok;
      m_opnd[m_nopnd++] = a;
      return;
    }
    Node* u = make(NodeKind::kUnary, e.tok);
    if (u == nullptr) return;
    u->op = e.op;
    u->add(a);
    m_opnd[m_nopnd++] = u;
    return;
  }
  Node* b = m_opnd[--m_nopnd];
  Node* a = m_opnd[--m_nopnd];
  Node* n = make(NodeKind::kBinary, e.tok);
  if (n == nullptr) return;
  n->op = e.op;
  n->add(a);
  n->add(b);
  m_opnd[m_nopnd++] = n;
}

void Parser::finishExpr() {
  const Frame f = m_f[m_nf - 1];
  while (m_nops > f.op_base && !m_abort) reduceTop();
  if (m_abort) return;
  Node* n = m_nopnd > f.opnd_base ? m_opnd[m_nopnd - 1] : nullptr;
  m_nopnd = f.opnd_base;
  m_nops = f.op_base;
  --m_nf;
  if (n != nullptr && f.wrap_template) {
    Node* t = make(NodeKind::kTemplate, f.open_tok);
    if (t == nullptr) return;
    t->add(n);
    n = t;
  }
  deliver(n);
}

void Parser::stepExpr(Frame& f) {
  const Token& t = tok();
  const uint16_t at = m_i;
  if (f.expect_operand) {
    NodeKind leaf = NodeKind::kProgram;
    switch (t.kind) {
      case Tok::kNumber: leaf = NodeKind::kNumber; break;
      case Tok::kQuoted: leaf = NodeKind::kQuoted; break;
      case Tok::kInput: leaf = NodeKind::kInput; break;
      case Tok::kPlaceholder: leaf = NodeKind::kPlaceholder; break;
      case Tok::kLParen: ++m_i; pushExpr(true, Close::kParen, false, at); return;
      case Tok::kLBracket: {
        Node* l = make(NodeKind::kList, at);
        if (l == nullptr) return;
        ++m_i;
        pushList(l, at, false);
        return;
      }
      case Tok::kNeg:
        if (m_nops >= kMaxStack) { fail(true); return; }
        m_ops[m_nops++] = {Op::kNeg, at};
        ++m_i;
        return;
      case Tok::kName: {
        if (tokenIs(m_src, t, "TRUE") || tokenIs(m_src, t, "FALSE")) {
          Node* b = make(NodeKind::kBool, at);
          if (b == nullptr) return;
          b->milli = tokenIs(m_src, t, "TRUE") ? 1 : 0;
          ++m_i;
          deliver(b);
          return;
        }
        if (tokenIs(m_src, t, "NOT")) {
          if (!f.full) {
            const DiagArg a[] = {{"word", "NOT"}};
            error(7005, at, a, 1);
            fail(true);
            return;
          }
          m_ops[m_nops++] = {Op::kNot, at};
          ++m_i;
          return;
        }
        const int16_t w = findWord(m_src, t);
        if (w >= 0 && gen::kWords[w].kind == gen::WordKind::kReporter) {
          ++m_i;
          startCall(w, at);
          return;
        }
        const int16_t p = findProc(t);
        if (p >= 0 && m_procs[p].gives_value) { ++m_i; startProcCall(p, at); return; }
        if (w >= 0 || p >= 0) {
          char nm[40];
          tokenText(m_src, t, nm, sizeof nm);
          const DiagArg a[] = {{"word", w >= 0 ? gen::kWords[w].name : nm}};
          error(7005, at, a, 1);
          fail(true);
          return;
        }
        unknownWord(at);
        fail(true);
        return;
      }
      default: {
        char nm[40];
        if (m_nops > f.op_base) tokenText(m_src, tokAt(m_ops[m_nops - 1].tok), nm, sizeof nm);
        else snprintf(nm, sizeof nm, "( )");
        const DiagArg a[] = {{"word", nm}, {"what", "a value"}, {"example", "10 + 5"}};
        error(7002, m_nops > f.op_base ? m_ops[m_nops - 1].tok : f.open_tok, a, 3);
        fail(false);
        return;
      }
    }
    Node* n = make(leaf, at);
    if (n == nullptr) return;
    n->milli = t.milli;
    n->ph = t.ph;
    ++m_i;
    deliver(n);
    return;
  }
  Op op = Op::kNone;
  switch (t.kind) {
    case Tok::kPlus: op = Op::kAdd; break;
    case Tok::kMinus: op = Op::kSub; break;
    case Tok::kStar: op = Op::kMul; break;
    case Tok::kSlash: op = Op::kDiv; break;
    case Tok::kEq: op = Op::kEq; break;
    case Tok::kNe: op = Op::kNe; break;
    case Tok::kLt: op = Op::kLt; break;
    case Tok::kGt: op = Op::kGt; break;
    case Tok::kLe: op = Op::kLe; break;
    case Tok::kGe: op = Op::kGe; break;
    case Tok::kName:
      if (tokenIs(m_src, t, "AND")) op = Op::kAnd;
      else if (tokenIs(m_src, t, "OR")) op = Op::kOr;
      break;
    default: break;
  }
  if (op != Op::kNone) {
    if (!f.full && prec(op) >= 4) { finishExpr(); return; }  // grammar.md §6: inputs stop before comparisons
    if (isCompare(op)) {
      if (f.seen_compare) { error(7018, at); fail(true); return; }
      f.seen_compare = true;
    }
    if (op == Op::kAnd || op == Op::kOr) f.seen_compare = false;
    while (m_nops > f.op_base && prec(m_ops[m_nops - 1].op) <= prec(op) && !m_abort) reduceTop();
    if (m_nops >= kMaxStack) { fail(true); return; }
    m_ops[m_nops++] = {op, at};
    ++m_i;
    f.expect_operand = true;
    return;
  }
  if (f.close == Close::kParen || f.close == Close::kBracket) {
    const Tok want = f.close == Close::kParen ? Tok::kRParen : Tok::kRBracket;
    if (t.kind == want) { ++m_i; finishExpr(); return; }
    if (startsValue(t)) {
      char a1[40], b1[40];
      tokenText(m_src, tokAt(m_last_value_tok == 0xFFFF ? f.open_tok : m_last_value_tok), a1, sizeof a1);
      tokenText(m_src, t, b1, sizeof b1);
      if (f.close == Close::kParen) {
        const DiagArg a[] = {{"a", a1}, {"b", b1}};
        error(7028, at, a, 2);
      } else {
        const DiagArg a[] = {{"value", b1}};
        error(7004, at, a, 1);
      }
      minusHint(at);
      fail(true);
      return;
    }
    char line[8];
    snprintf(line, sizeof line, "%u", static_cast<unsigned>(tokAt(f.open_tok).line));
    const bool paren = f.close == Close::kParen;
    const DiagArg a[] = {{"bracket", paren ? "(" : "["}, {"line", line}, {"closer", paren ? ")" : "]"}};
    error(7003, f.open_tok, a, 3);
    fail(false);
    return;
  }
  finishExpr();
}

void Parser::stepList(Frame& f) {
  const Token& t = tok();
  const uint16_t at = m_i;
  if (t.kind == Tok::kEnd) {
    char line[8];
    snprintf(line, sizeof line, "%u", static_cast<unsigned>(tokAt(f.open_tok).line));
    const DiagArg a[] = {{"bracket", "["}, {"line", line}, {"closer", "]"}};
    error(7003, f.open_tok, a, 3);
    m_abort = true;
    return;
  }
  if (t.kind == Tok::kRBracket) {
    ++m_i;
    Node* n = f.node;
    const bool attached = f.attached;
    --m_nf;
    if (!attached) deliver(n);
    return;
  }
  if (t.kind == Tok::kLBracket) {
    Node* l = make(NodeKind::kList, at);
    if (l == nullptr) return;
    f.node->add(l);
    ++m_i;
    pushList(l, at, true);
    return;
  }
  if (t.kind == Tok::kNeg && tokAt(static_cast<uint16_t>(at + 1)).kind == Tok::kNumber) {
    Node* n = make(NodeKind::kNumber, at);
    if (n == nullptr) return;
    n->milli = -tokAt(static_cast<uint16_t>(at + 1)).milli;
    f.node->add(n);
    m_i = static_cast<uint16_t>(m_i + 2);
    return;
  }
  Node* n = make(t.kind == Tok::kNumber ? NodeKind::kNumber : NodeKind::kWord, at);
  if (n == nullptr) return;
  n->milli = t.milli;
  f.node->add(n);
  ++m_i;
}
```

- [ ] **Step 4: The printer**

`printer.h`:
```cpp
// The `reads` notation of logo/spec/tests/README.md, for the conformance tests (LLD §17). No recursion.
#pragma once

#include <stddef.h>

#include "logo/ast.h"
#include "logo/lexer.h"
#include "logo/parser.h"

namespace cq {
namespace logo {

void printReads(const Node* program, const char* src, const TokenList& toks, const Parser& p, char* out, size_t cap);

}  // namespace logo
}  // namespace cq
```
`printer.cpp`:
```cpp
#include "logo/printer.h"

#include <stdio.h>
#include <string.h>

#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

class Out {
 public:
  Out(char* b, size_t c) : m_b(b), m_c(c) { m_b[0] = '\0'; }
  void put(const char* s) {
    const size_t n = strlen(m_b);
    const char last = n > 0 ? m_b[n - 1] : '(';
    const bool space = last != '(' && last != '[' && last != '{' && s[0] != ')' && s[0] != ']' && s[0] != '}';
    if (space) strncat(m_b, " ", m_c - strlen(m_b) - 1);
    strncat(m_b, s, m_c - strlen(m_b) - 1);
  }

 private:
  char* m_b;
  size_t m_c;
};

void upperCopy(const char* src, const Token& t, char* out, size_t cap) {
  size_t i = 0;
  for (; i < t.len && i + 1 < cap; ++i) {
    const char c = src[t.start + i];
    out[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
  }
  out[i] = '\0';
}

void number(int32_t milli, char* out, size_t cap) {
  const bool neg = milli < 0;
  const int64_t v = neg ? -static_cast<int64_t>(milli) : milli;
  if (v % 1000 == 0) {
    snprintf(out, cap, "%s%lld", neg ? "-" : "", static_cast<long long>(v / 1000));
    return;
  }
  char frac[8];
  snprintf(frac, sizeof frac, "%03lld", static_cast<long long>(v % 1000));
  for (int i = 2; i > 0 && frac[i] == '0'; --i) frac[i] = '\0';
  snprintf(out, cap, "%s%lld.%s", neg ? "-" : "", static_cast<long long>(v / 1000), frac);
}

const char* opName(Op o) {
  switch (o) {
    case Op::kAdd: return "+";
    case Op::kSub: return "-";
    case Op::kMul: return "*";
    case Op::kDiv: return "/";
    case Op::kEq: return "=";
    case Op::kNe: return "<>";
    case Op::kLt: return "<";
    case Op::kGt: return ">";
    case Op::kLe: return "<=";
    case Op::kGe: return ">=";
    case Op::kAnd: return "AND";
    case Op::kOr: return "OR";
    case Op::kNot: return "NOT";
    case Op::kNeg: return "-";
    default: return "?";
  }
}

}  // namespace

void printReads(const Node* program, const char* src, const TokenList& toks, const Parser& p, char* out,
                size_t cap) {
  Out o(out, cap);
  if (program == nullptr) return;
  struct Item {
    const Node* n;
    const Node* child;
    bool opened;
  };
  Item st[128];
  uint8_t ns = 0;
  st[ns++] = {program, nullptr, false};
  char buf[64];
  while (ns > 0) {
    Item& it = st[ns - 1];
    const Node* n = it.n;
    if (!it.opened) {
      it.opened = true;
      it.child = n->first;
      const Token& t = toks.t[n->tok];
      switch (n->kind) {  // VARIANT POINT: node kinds
        case NodeKind::kProgram: break;
        case NodeKind::kDef:
          upperCopy(src, toks.t[p.proc(static_cast<uint8_t>(n->ref)).name_tok], buf + 4, sizeof buf - 4);
          memcpy(buf, "(TO ", 4);
          o.put(buf);
          break;
        case NodeKind::kParams: o.put("("); break;
        case NodeKind::kBlock: case NodeKind::kTemplate: o.put("["); break;
        case NodeKind::kList: o.put("{"); break;
        case NodeKind::kCall:
          snprintf(buf, sizeof buf, "(%s", strcmp(gen::kWords[n->ref].name, "WAIT UNTIL") == 0
                                               ? "WAIT-UNTIL" : gen::kWords[n->ref].name);
          o.put(buf);
          break;
        case NodeKind::kProcCall:
          buf[0] = '(';
          upperCopy(src, toks.t[p.proc(static_cast<uint8_t>(n->ref)).name_tok], buf + 1, sizeof buf - 1);
          o.put(buf);
          break;
        case NodeKind::kNumber: number(n->milli, buf, sizeof buf); o.put(buf); break;
        case NodeKind::kWord: case NodeKind::kQuoted: case NodeKind::kInput:
          tokenText(src, t, buf, sizeof buf);
          o.put(buf);
          break;
        case NodeKind::kPreset: upperCopy(src, t, buf, sizeof buf); o.put(buf); break;
        case NodeKind::kPlaceholder: o.put(n->ph == 0 ? "?" : (n->ph == 1 ? "?1" : "?2")); break;
        case NodeKind::kBool: o.put(n->milli != 0 ? "TRUE" : "FALSE"); break;
        case NodeKind::kUnary: case NodeKind::kBinary:
          snprintf(buf, sizeof buf, "(%s", opName(n->op));
          o.put(buf);
          break;
      }
      continue;
    }
    if (it.child != nullptr) {
      const Node* c = it.child;
      it.child = c->next;
      if (ns < 128) st[ns++] = {c, nullptr, false};
      continue;
    }
    switch (n->kind) {
      case NodeKind::kDef: case NodeKind::kParams: case NodeKind::kCall: case NodeKind::kProcCall:
      case NodeKind::kUnary: case NodeKind::kBinary: o.put(")"); break;
      case NodeKind::kBlock: case NodeKind::kTemplate: o.put("]"); break;
      case NodeKind::kList: o.put("}"); break;
      default: break;
    }
    --ns;
  }
}

}  // namespace logo
}  // namespace cq
```

- [ ] **Step 5: Run to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_parser`
Expected: `11 test cases: 11 succeeded`. If a `reads` string differs, compare against grammar.md §3/§6 and fix the parser, not the expected string.

- [ ] **Step 6: Commit**

```bash
git add firmware/esp32/src/logo/parser.cpp firmware/esp32/src/logo/printer.* firmware/esp32/test/test_logo_parser
git commit -m "feat(logo): parser pass 2 (iterative frames, precedence, presets, lists, minus hint) and the reads printer

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Checker

**Files:** Create `firmware/esp32/src/logo/checker.h`, `checker.cpp`; Test `firmware/esp32/test/test_logo_checker/test_main.cpp`.

**Interfaces:**
- Consumes: `Parser` (procs), `Node`, `TokenList`, `Diagnostics`, `gen::*`, `editDistance`.
- Produces: `struct Caps { bool encoders = true; bool ai = false; uint8_t version = 1; }`; `struct CheckResult { bool ok = false; bool handles_obstacles = false; uint64_t tail_callees = 0; }`; `class Checker { Checker(const Parser&, const char* src, const TokenList&, Diagnostics&); CheckResult check(Node* program, const Caps&); }` — marks `Node::tail`.

- [ ] **Step 1: Write the failing tests**

```cpp
// Plan N1 Task 6: scope, needs, level, ranges, OUTPUT, ?, BLOCKED?, tail positions (grammar.md §8, §11).
#include <string.h>
#include <unity.h>

#include "logo/checker.h"
#include "logo/lexer.h"
#include "logo/parser.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];
CheckResult C(const char* s, Caps caps = Caps()) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  g_d.clear();
  pool.reset();
  lex(s, static_cast<uint16_t>(strlen(s)), g_t, g_d);
  Node* prog = p.parse(s, g_t);
  Checker k(p, s, g_t, g_d);
  return k.check(prog, caps);
}
uint16_t first() { return g_d.first() ? g_d.first()->code : 0; }
}  // namespace

void setUp() {}
void tearDown() {}

void test_check_scope() {
  C("TO SQUARE :size REPEAT 4 [ FD :sise RT 90 ] END");
  TEST_ASSERT_EQUAL(7009, first());
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, ":size"));
  C("TO A :x FD :x END\nFD :x");
  TEST_ASSERT_EQUAL(7009, first());
  TEST_ASSERT_EQUAL(2, g_d.first()->line);
}

void test_check_needs_before_level() {
  C("IF SEE? \"ball [ BEEP ]");
  TEST_ASSERT_EQUAL(7020, first());
  C("FD FIRST [ 10 20 ]");
  TEST_ASSERT_EQUAL(7030, first());
  Caps no_enc;
  no_enc.encoders = false;
  C("FD 50", no_enc);
  TEST_ASSERT_EQUAL(7021, first());
  TEST_ASSERT_TRUE(C("TO D :x OUTPUT :x * 2 END FD D 5").ok);  // OUTPUT is in v1
}

void test_check_ranges_and_arc() {
  C("FD 600");
  TEST_ASSERT_EQUAL(7025, first());
  TEST_ASSERT_NOT_NULL(strstr(g_d.first()->msg, "1 to 500 cm"));
  C("ARC RIGHT 90 20");
  TEST_ASSERT_EQUAL(7022, first());
  TEST_ASSERT_EQUAL(14, g_d.first()->col);
  TEST_ASSERT_TRUE(C("FD 2.5 RT 720").ok);
}

void test_check_output_placeholder_blocked() {
  C("OUTPUT 5");
  TEST_ASSERT_EQUAL(7026, first());
  C("FD ?");
  TEST_ASSERT_EQUAL(7006, first());
  TEST_ASSERT_TRUE(C("FOREVER [ FD 50 IF BLOCKED? [ BK 10 RT 90 ] ]").handles_obstacles);
  TEST_ASSERT_FALSE(C("FD 100").handles_obstacles);
}

void test_check_tail_positions() {
  CheckResult r = C("TO COUNTDOWN :n IFELSE :n = 0 [ BEEP ] [ FD 10 COUNTDOWN :n - 1 ] END COUNTDOWN 500");
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_TRUE(r.tail_callees == 1);  // proc 0
  r = C("TO A :n REPEAT 2 [ A :n - 1 ] END");
  TEST_ASSERT_TRUE(r.tail_callees == 0);
  r = C("TO PING :n IF :n > 0 [ RT 10 PONG :n - 1 ] END TO PONG :n IF :n > 0 [ LT 10 PING :n - 1 ] END PING 3");
  TEST_ASSERT_TRUE(r.tail_callees == 3);
  r = C("TO SUM :n :acc IFELSE :n = 0 [ OUTPUT :acc ] [ OUTPUT SUM :n - 1 :acc + :n ] END FD SUM 20 0");
  TEST_ASSERT_TRUE(r.tail_callees == 1);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_check_scope);
  RUN_TEST(test_check_needs_before_level);
  RUN_TEST(test_check_ranges_and_arc);
  RUN_TEST(test_check_output_placeholder_blocked);
  RUN_TEST(test_check_tail_positions);
  return UNITY_END();
}
```

- [ ] **Step 2: Run to see it fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_checker`
Expected: FAIL — `logo/checker.h: No such file or directory`.

- [ ] **Step 3: Implement**

`checker.h`:
```cpp
// Tank Logo checker (LLD §6, grammar.md §8 / §11): scope, value vs command, needs, level, constant ranges,
// templates, BLOCKED?, tail positions. Walks the tree with an explicit stack (no recursion).
#pragma once

#include <stdint.h>

#include "logo/ast.h"
#include "logo/diag.h"
#include "logo/lexer.h"
#include "logo/parser.h"

namespace cq {
namespace logo {

struct Caps {
  bool encoders = true;
  bool ai = false;      // an AI program (vision words allowed)
  uint8_t version = 1;  // v1: level-3 words other than OUTPUT are reserved (7030)
};

struct CheckResult {
  bool ok = false;
  bool handles_obstacles = false;  // uses BLOCKED? (design-notes §82 h)
  uint64_t tail_callees = 0;       // bit i: procedure i is called in tail position
};

class Checker {
 public:
  Checker(const Parser& p, const char* src, const TokenList& toks, Diagnostics& d)
      : m_p(p), m_src(src), m_toks(toks), m_d(d) {}
  CheckResult check(Node* program, const Caps& caps);

 private:
  void visitCall(Node* n, int16_t def, const Caps& caps, CheckResult& r);
  void visitInput(const Node* n, int16_t def);
  void markTails(Node* body, CheckResult& r);
  void error(uint16_t code, uint16_t tok, const DiagArg* a = nullptr, uint8_t n = 0);

  const Parser& m_p;
  const char* m_src;
  const TokenList& m_toks;
  Diagnostics& m_d;
};

}  // namespace logo
}  // namespace cq
```
`checker.cpp`:
```cpp
#include "logo/checker.h"

#include <stdio.h>
#include <string.h>

#include "logo/generated/words.h"
#include "logo/words.h"

namespace cq {
namespace logo {
namespace {

void fmtMilli(int32_t m, char* out, size_t cap) {
  if (m % 1000 == 0) snprintf(out, cap, "%ld", static_cast<long>(m / 1000));
  else snprintf(out, cap, "%.3g", m / 1000.0);
}

bool templateInput(gen::InType t) { return t == gen::InType::kTemplate || t == gen::InType::kTemplateBlock; }

}  // namespace

void Checker::error(uint16_t code, uint16_t tok, const DiagArg* a, uint8_t n) {
  m_d.add(code, m_toks.t[tok].line, m_toks.t[tok].col, a, n);
}

void Checker::visitInput(const Node* n, int16_t def) {
  const Token& t = m_toks.t[n->tok];
  if (def >= 0) {
    const ProcInfo& p = m_p.proc(static_cast<uint8_t>(def));
    for (uint8_t k = 0; k < p.n_inputs; ++k) {
      const Token& in = m_toks.t[p.first_input_tok + k];
      if (in.len == t.len && editDistance(m_src + in.start, in.len, m_src + t.start, t.len) == 0) return;
    }
  }
  char name[40], sug[40] = "";
  snprintf(name, sizeof name, "%.*s", static_cast<int>(t.len - 1), m_src + t.start + 1);
  if (def >= 0) {
    const ProcInfo& p = m_p.proc(static_cast<uint8_t>(def));
    uint8_t best = 3;
    for (uint8_t k = 0; k < p.n_inputs; ++k) {
      const Token& in = m_toks.t[p.first_input_tok + k];
      const uint8_t d = editDistance(m_src + in.start + 1, static_cast<uint16_t>(in.len - 1), name,
                                     static_cast<uint16_t>(strlen(name)));
      if (d < best) { best = d; snprintf(sug, sizeof sug, "%.*s", static_cast<int>(in.len - 1), m_src + in.start + 1); }
    }
    if (best > 2) sug[0] = '\0';
  }
  const DiagArg a[] = {{"name", name}, {"suggestion", sug}};
  error(7009, n->tok, a, 2);
}

void Checker::visitCall(Node* n, int16_t def, const Caps& caps, CheckResult& r) {
  const gen::WordSpec& w = gen::kWords[n->ref];
  const DiagArg wa[] = {{"word", w.name}};
  if ((w.needs & gen::kNeedAi) != 0 && !caps.ai) {
    error(7020, n->tok, wa, 1);
  } else if ((w.needs & gen::kNeedEncoders) != 0 && !caps.encoders) {
    error(7021, n->tok, wa, 1);
  } else if (w.level == 3 && strcmp(w.name, "OUTPUT") != 0 && caps.version == 1) {
    error(7030, n->tok, wa, 1);
  }
  if (strcmp(w.name, "OUTPUT") == 0 && def < 0) error(7026, n->tok);
  if (strcmp(w.name, "BLOCKED?") == 0) r.handles_obstacles = true;
  uint8_t i = 0;
  for (const Node* c = n->first; c != nullptr && i < w.n_inputs; c = c->next, ++i) {
    const gen::InputSpec& in = w.inputs[i];
    if (!in.has_range || c->kind != NodeKind::kNumber) continue;
    if (c->milli >= in.min_milli && c->milli <= in.max_milli) continue;
    char mn[16], mx[16], v[16];
    fmtMilli(in.min_milli, mn, sizeof mn);
    fmtMilli(in.max_milli, mx, sizeof mx);
    fmtMilli(c->milli, v, sizeof v);
    if (strcmp(w.name, "ARC") == 0 && strcmp(in.name, "radius") == 0 && c->milli < in.min_milli) {
      char word[24], ang[16];
      tokenText(m_src, m_toks.t[n->first->tok], ang, sizeof ang);
      snprintf(word, sizeof word, "ARC %s", ang);
      fmtMilli(n->first->next->milli, ang, sizeof ang);
      const DiagArg a[] = {{"word", word}, {"angle", ang}, {"radius", v}};
      error(7022, c->tok, a, 3);
      continue;
    }
    const DiagArg a[] = {{"word", w.name}, {"what", in.what}, {"min", mn}, {"max", mx}, {"unit", in.unit}};
    error(7025, c->tok, a, 5);
  }
}

void Checker::markTails(Node* body, CheckResult& r) {
  Node* st[64];
  uint8_t ns = 0;
  if (body->last != nullptr) st[ns++] = body->last;
  while (ns > 0) {
    Node* n = st[--ns];
    if (n->kind == NodeKind::kProcCall) {
      n->tail = true;
      if (n->ref < 64) r.tail_callees |= (1ULL << n->ref);
      continue;
    }
    if (n->kind != NodeKind::kCall) continue;
    const char* name = gen::kWords[n->ref].name;
    if (strcmp(name, "IF") == 0 && n->count == 2 && n->last->last != nullptr && ns < 64) {
      st[ns++] = n->last->last;
    } else if (strcmp(name, "IFELSE") == 0 && n->count == 3) {
      const Node* yes = n->first->next;
      if (yes->last != nullptr && ns < 64) st[ns++] = yes->last;
      if (n->last->last != nullptr && ns < 64) st[ns++] = n->last->last;
    } else if (strcmp(name, "OUTPUT") == 0 && n->first != nullptr && n->first->kind == NodeKind::kProcCall &&
               ns < 64) {
      st[ns++] = n->first;
    }
  }
}

CheckResult Checker::check(Node* program, const Caps& caps) {
  CheckResult r;
  if (program == nullptr) return r;
  struct Item {
    Node* n;
    int16_t def;
    bool in_template;
  };
  Item st[256];
  uint16_t ns = 0;
  st[ns++] = {program, -1, false};
  while (ns > 0) {
    const Item it = st[--ns];
    Node* n = it.n;
    int16_t def = it.def;
    switch (n->kind) {  // VARIANT POINT: checked node kinds
      case NodeKind::kDef: def = n->ref; markTails(n->last, r); break;
      case NodeKind::kCall: visitCall(n, def, caps, r); break;
      case NodeKind::kInput:
        if (it.def != -2) visitInput(n, def);  // -2: a definition's own parameter list
        break;
      case NodeKind::kPlaceholder:
        if (!it.in_template) error(7006, n->tok);
        break;
      case NodeKind::kList:
        for (const Node* c = n->first; c != nullptr; c = c->next) {
          if (c->kind == NodeKind::kWord && m_src[m_toks.t[c->tok].start] == ':') {
            m_d.add(7019, m_toks.t[c->tok].line, m_toks.t[c->tok].col, nullptr, 0, true);
            break;
          }
        }
        continue;  // data: nothing inside is looked up
      default: break;
    }
    uint8_t i = 0;
    for (Node* c = n->first; c != nullptr; c = c->next, ++i) {
      if (ns >= 256) break;
      bool tpl = it.in_template;
      if (n->kind == NodeKind::kCall && i < gen::kWords[n->ref].n_inputs) {
        tpl = tpl || templateInput(gen::kWords[n->ref].inputs[i].type);
      }
      const int16_t cdef = (n->kind == NodeKind::kDef && c->kind == NodeKind::kParams) ? -2 : def;
      st[ns++] = {c, cdef, tpl};
    }
  }
  r.ok = m_d.errorCount() == 0;
  return r;
}

}  // namespace logo
}  // namespace cq
```

- [ ] **Step 4: Run to see it pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_checker`
Expected: `5 test cases: 5 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/logo/checker.* firmware/esp32/test/test_logo_checker
git commit -m "feat(logo): checker - scope, needs before level, ranges, ARC 7022, OUTPUT, ?, BLOCKED?, tail positions

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: Conformance suite + docs

**Files:** Create `firmware/esp32/test/test_logo_conformance/test_main.cpp`; Modify `docs/lld/tank-logo-lld.md` (plan pointer), `docs/design-notes.md` (§88 results).

**Interfaces:**
- Consumes: `kCases` / `LOGO_CASES` from `cases.h`; `lex`, `Parser`, `Checker`, `printReads`, `Caps`.
- Produces: one Unity test per v1 case.

- [ ] **Step 1: Write the harness**

```cpp
// Plan N1 Task 7: every v1 case of logo/spec/tests (reading.yaml, errors.yaml), generated into cases.h.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "cases.h"
#include "logo/checker.h"
#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/printer.h"

using namespace cq::logo;

namespace {
TokenList g_t;
Diagnostics g_d;
Node g_nodes[4096];

void yourWords(const Parser& p, const char* src, char* out, size_t cap) {
  out[0] = '\0';
  for (uint8_t i = 0; i < p.procCount(); ++i) {
    const ProcInfo& pi = p.proc(i);
    if (!pi.valid) continue;
    char nm[40], one[64];
    const Token& t = g_t.t[pi.name_tok];
    for (uint16_t k = 0; k < t.len && k + 1 < sizeof nm; ++k) {
      const char c = src[t.start + k];
      nm[k] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
      nm[k + 1] = '\0';
    }
    snprintf(one, sizeof one, "%s:%u:%d;", nm, pi.n_inputs, pi.gives_value ? 1 : 0);
    strncat(out, one, cap - strlen(out) - 1);
  }
}

void tailNames(const Parser& p, const char* src, uint64_t bits, char* out, size_t cap) {
  // sorted, comma-separated upper-case names (like logogen)
  char names[64][40];
  uint8_t n = 0;
  for (uint8_t i = 0; i < p.procCount() && i < 64; ++i) {
    if ((bits & (1ULL << i)) == 0) continue;
    const Token& t = g_t.t[p.proc(i).name_tok];
    uint16_t k = 0;
    for (; k < t.len && k < 39; ++k) {
      const char c = src[t.start + k];
      names[n][k] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    }
    names[n++][k] = '\0';
  }
  for (uint8_t i = 1; i < n; ++i) {  // insertion sort
    for (uint8_t j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; --j) {
      char tmp[40];
      strcpy(tmp, names[j]);
      strcpy(names[j], names[j - 1]);
      strcpy(names[j - 1], tmp);
    }
  }
  out[0] = '\0';
  for (uint8_t i = 0; i < n; ++i) {
    if (i > 0) strncat(out, ",", cap - strlen(out) - 1);
    strncat(out, names[i], cap - strlen(out) - 1);
  }
}

void runCase(const LogoCase& c) {
  static NodePool pool(g_nodes, 4096);
  static Parser p(pool, g_d);
  char why[256];
  g_d.clear();
  pool.reset();
  lex(c.src, static_cast<uint16_t>(strlen(c.src)), g_t, g_d);
  Node* prog = p.parse(c.src, g_t);
  CheckResult r;
  if (c.check) {
    Caps caps;
    caps.encoders = c.encoders;
    Checker k(p, c.src, g_t, g_d);
    r = k.check(prog, caps);
  }
  const Diag* f = g_d.first();
  if (c.expect_ok) {
    snprintf(why, sizeof why, "%s: unexpected %u at %u:%u %s", c.name, f ? f->code : 0, f ? f->line : 0,
             f ? f->col : 0, f ? f->msg : "");
    TEST_ASSERT_NULL_MESSAGE(f, why);
    static char out[4096];
    printReads(prog, c.src, g_t, p, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.reads, out, c.name);
    if (c.your_words[0] != '\0') {
      char yw[256];
      yourWords(p, c.src, yw, sizeof yw);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(c.your_words, yw, c.name);
    }
    if (c.handles_obstacles >= 0) TEST_ASSERT_EQUAL_MESSAGE(c.handles_obstacles, r.handles_obstacles ? 1 : 0, c.name);
    if (strcmp(c.tail_calls, "-") != 0) {  // "-" = not checked
      char tn[256];
      tailNames(p, c.src, r.tail_callees, tn, sizeof tn);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(c.tail_calls, tn, c.name);
    }
    return;
  }
  TEST_ASSERT_NOT_NULL_MESSAGE(f, c.name);
  snprintf(why, sizeof why, "%s: got %u at %u:%u %s", c.name, f->code, f->line, f->col, f->msg);
  TEST_ASSERT_EQUAL_MESSAGE(c.code, f->code, why);
  TEST_ASSERT_EQUAL_MESSAGE(c.line, f->line, why);
  TEST_ASSERT_EQUAL_MESSAGE(c.col, f->col, why);
  if (c.message_has[0] != '\0') TEST_ASSERT_NOT_NULL_MESSAGE(strstr(f->msg, c.message_has), why);
}
}  // namespace

void setUp() {}
void tearDown() {}

#define DEFINE_CASE(i) \
  void test_case_##i() { runCase(kCases[i]); }
LOGO_CASES(DEFINE_CASE)

int main(int, char**) {
  UNITY_BEGIN();
#define RUN_CASE(i) RUN_TEST(test_case_##i);
  LOGO_CASES(RUN_CASE)
  return UNITY_END();
}
```

- [ ] **Step 2: Run the conformance suite**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_conformance`
Expected: every case passes (the count printed equals the number of v1 cases in `reading.yaml` + `errors.yaml`). For each failure the message names the case and shows the code / position / text found: fix the **reader** where it disagrees with grammar.md; if a test itself contradicts grammar.md, fix the YAML case **and** record the change in design-notes §88 (spec authority: grammar.md).

- [ ] **Step 3: Whole native suite + firmware build**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native > /tmp/n1.log 2>&1; tail -3 /tmp/n1.log && ~/.platformio/penv/bin/pio run -e bridge | tail -3`
Expected: all suites pass (228 earlier + the new logo suites); `bridge` builds `[SUCCESS]` (the reader is compiled in; nothing calls it yet).

- [ ] **Step 4: Docs and commit**

In `docs/lld/tank-logo-lld.md` under §0 add: `Implementation: Plan N1 (reader) docs/superpowers/plans/2026-10-19-plan-n1-logo-reader.md — done when all v1 conformance cases pass.` Append to `docs/design-notes.md`:
```markdown
## 88. Plan N1 — Tank Logo reader (2026-10-19)
- Generator `logogen` (logo/), reader in `firmware/esp32/src/logo/` (lexer, two-pass iterative parser, checker, reads printer); host tests incl. every v1 case of `logo/spec/tests` (count: N).
- Spec fixes found while planning: the "unknown word" case now defines SQUARE; the SEE? reading case is reading-only (`check: false`); IFELSE has a `missing` text; a too-big number stays a NUMBER token (clamped) so 7015 is the first error.
```
(replace N with the number of cases the suite ran.)
```bash
git add firmware/esp32/test/test_logo_conformance docs/lld/tank-logo-lld.md docs/design-notes.md
git commit -m "test(logo): conformance suite - every v1 case of logo/spec/tests passes

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git push -u origin plan-n1
```

---

## Self-review notes (writing-plans)

- **Spec coverage:** LLD §4 lexer → Task 3; §5 parser (pass 1 Task 4, pass 2 Task 5, no recursion, 64 frames); §6 checker → Task 6; §15 generator → Task 1; §17 conformance → Task 7; grammar.md §2 tokens (Task 3), §3 grammar + §4 input counting (Tasks 4–5), §5 input types (Task 5 `stepCall`), §6 precedence incl. "inputs are arithmetic" (Task 5), §7 templates (`?` placement in Task 6; template parsing in Task 5), §8 scope / OUTPUT / tail positions (Task 6), §9 tricky cases incl. §9.1 minus hint (Task 5 + conformance), §11 one code per refusal (Diagnostics + every error path). Not in N1 by design: compiler, VM, runner, store, API, page (Plans N2–N4).
- **Types:** `Parser::proc()`, `ProcInfo` fields, `Node` fields, `Caps`, `CheckResult`, `printReads` signature are used identically in Tasks 4–7.
- **Review Focus:** the five lines each have their test (Tasks 3 and 5).
