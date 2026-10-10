# Plan A — Protocol Generator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `protogen`, which turns YAML definitions of every ESP32 ⇄ Uno message into a Python runtime package, a header-only C++ header for the Uno, golden test vectors, a C++ host test and a protocol reference doc — with validation and a staleness check.

**Architecture:** YAML defs (`protocol/defs/`) → pydantic model → validator → independent emitters (plain Python functions returning text). Three independent codec implementations — the generator's own encoder (used to produce vectors), the Python runtime (`conqueror_protocol`), and the generated C++ — must agree byte-for-byte on every vector; hand-computed golden vectors anchor all three.

**Tech Stack:** Python 3.12 via uv (workspace), pydantic 2, PyYAML, pytest, ruff, pyright (strict); Apple clang (C++17) for the host C++ vector test.

**Spec:** `docs/lld/protocol-generator-lld.md` (plus frame definitions from `docs/superpowers/specs/2026-10-10-01-protocol-design.md` §8, `docs/lld/uno-firmware-lld.md` §9.4/§9.7/§9.10/§9.11/§9.18/§9.21, `docs/lld/esp32-firmware-lld.md` §10.10/§10.14).

**Roadmap position:** Phase 0 of `docs/implementation-roadmap.md`. Plans B (Uno firmware), C (ESP32 bridge) and D (bench tool) consume its outputs.

## Global Constraints

- Frame layout: `0xAA | len | type | seq | payload | crc8`; CRC-8 poly `0x07`, init `0x00` (CRC-8/SMBUS, check value of `"123456789"` = `0xF4`) over `len, type, seq, payload`.
- Max payload **48** bytes; all integers **little-endian**.
- UART **250000** baud (recorded in `meta.yaml`; used by firmware and bench).
- Frame type ids: ESP32→Uno `0x01–0x7F`, Uno→ESP32 `0x80–0xFF`.
- Error codes: numeric per domain range (`AUTH 1000–1999`, `TANK 2000–2999`, `COMMAND 3000–3999`, `CALIBRATION 6000–6999`, `SETTINGS 8000–8999`, `SYSTEM 9000–9999`), never reused (`retired` list).
- Generated files start with a `DO NOT EDIT` header and are **committed** (firmware builds must not need Python).
- Uno C++ output: header-only, `<stdint.h>` only, no STL, no heap, tables marked `PROGMEM` (empty macro off-AVR), C++17.
- Python: 3.12, type hints everywhere, ruff + pyright strict clean (generated file excluded from ruff only).
- Deviation from the LLD, recorded in Task 7: Jinja2 templates replaced by small emitter functions (simpler, fully testable). No ESP32 C++ / TypeScript / client-JSON outputs in this plan (YAGNI until roadmap phase 4/5).

## Review Focus

1. **Bytes arriving in pieces or with noise before the sync byte** → the parser must still produce the frame once, intact (Task 5 test `test_parser_split_and_noise`).
2. **A corrupted frame followed immediately by a good one** → bad CRC counted, the good frame still decoded (Task 5 `test_parser_bad_crc_then_good`).
3. **Variable-length TONE with too many notes or a wrong length** → rejected cleanly in Python and C++, never an out-of-bounds read (Task 5 `test_tone_rejects_bad_count`; Task 6 vector `tone_too_long` check).
4. **Signed extremes** (`i32` min, `i8` −128, `u32` max) → round-trip identically in Python and C++, literals compile on host and AVR (`extremes` vectors in Tasks 4–6).
5. **Someone edits YAML and forgets to regenerate** → `protogen check` fails with the stale paths listed (Task 7 `test_check_detects_stale`).

---

## File Structure

```
protocol/
├── pyproject.toml                         workspace root + protogen package config (Task 1)
├── defs/                                  single source of truth (Task 2)
│   ├── meta.yaml  enums.yaml  body_frames.yaml  errors.yaml
│   ├── events.yaml  settings.yaml  golden.yaml
├── generator/
│   ├── protogen/
│   │   ├── __init__.py
│   │   ├── model.py        pydantic model of the defs (Task 1)
│   │   ├── loader.py       YAML → model (Task 2)
│   │   ├── validator.py    rules → list of issues (Task 3)
│   │   ├── naming.py       PascalCase / k-prefixed names (Task 4)
│   │   ├── encoding.py     generator's independent encoder + crc8 (Task 4)
│   │   ├── vectors.py      deterministic test vectors (Task 4)
│   │   ├── emit_python.py  → conqueror_protocol/generated.py (Task 5)
│   │   ├── emit_cpp.py     → firmware/uno/src/generated/cq_protocol.h (Task 6)
│   │   ├── emit_vectors.py → protocol/vectors/frames.json + C++ host test (Tasks 4, 6)
│   │   ├── emit_docs.py    → docs/protocol/reference.md (Task 7)
│   │   └── cli.py          protogen write | check | diff (Task 7, 8)
│   └── tests/              pytest
├── python/                                 workspace member: runtime package (Task 5)
│   ├── pyproject.toml
│   └── conqueror_protocol/
│       ├── __init__.py  wire.py  framing.py  (hand-written)
│       ├── generated.py                     (generated)
│       └── py.typed
├── vectors/frames.json                     (generated)
└── tests/cpp/generated_vectors_test.cpp    (generated)
firmware/uno/src/generated/cq_protocol.h    (generated)
docs/protocol/reference.md                  (generated)
scripts/check-protocol.sh  .githooks/pre-commit
```

---

### Task 1: Workspace scaffold and the definitions model

**Files:**
- Create: `protocol/pyproject.toml`, `protocol/python/pyproject.toml`, `protocol/python/conqueror_protocol/__init__.py`, `protocol/python/conqueror_protocol/py.typed`, `protocol/generator/protogen/__init__.py`, `protocol/generator/protogen/model.py`
- Test: `protocol/generator/tests/test_model.py`

**Interfaces:**
- Produces: `protogen.model` — `WireType`, `WIRE_SIZE: dict[str,int]`, `WIRE_RANGE: dict[str,tuple[int,int]]`, `ItemField`, `Field` (`.max_size()`, `.is_struct_array`), `Frame` (`.max_payload()`, `.fixed_size()`), `EnumDef`, `ErrorDef`, `ErrorDomain`, `EventDef`, `SettingDef`, `GoldenVector`, `Meta`, `ProtocolModel` (`.frame(name) -> Frame`).

- [ ] **Step 1: Install uv (once per machine)**

Run: `brew install uv && uv --version`
Expected: prints `uv 0.x.y`.

- [ ] **Step 2: Create the workspace files**

`protocol/pyproject.toml`:
```toml
[project]
name = "protogen"
version = "0.1.0"
description = "Conqueror protocol generator"
requires-python = ">=3.12"
dependencies = ["pydantic>=2.9", "pyyaml>=6.0", "conqueror-protocol"]

[project.scripts]
protogen = "protogen.cli:main"

[tool.uv.sources]
conqueror-protocol = { workspace = true }

[tool.uv.workspace]
members = ["python"]

[dependency-groups]
dev = ["pytest>=8.3", "ruff>=0.7", "pyright>=1.1.385", "types-PyYAML>=6.0"]

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["generator/protogen"]

[tool.ruff]
line-length = 100
target-version = "py312"
extend-exclude = ["python/conqueror_protocol/generated.py"]

[tool.ruff.lint]
select = ["E", "F", "I", "B", "UP", "SIM", "RUF"]

[tool.pyright]
typeCheckingMode = "strict"
include = ["generator", "python"]
pythonVersion = "3.12"

[tool.pytest.ini_options]
testpaths = ["generator/tests"]
```

`protocol/python/pyproject.toml`:
```toml
[project]
name = "conqueror-protocol"
version = "0.1.0"
description = "Conqueror body-link protocol runtime (generated + framing)"
requires-python = ">=3.12"
dependencies = []

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["conqueror_protocol"]
```

`protocol/python/conqueror_protocol/__init__.py`:
```python
"""Conqueror body-link protocol runtime."""
```

`protocol/python/conqueror_protocol/py.typed`: empty file.

`protocol/generator/protogen/__init__.py`:
```python
"""protogen — generates protocol code and docs from protocol/defs."""
```

- [ ] **Step 3: Write the failing model test**

`protocol/generator/tests/test_model.py`:
```python
import pytest
from pydantic import ValidationError

from protogen.model import Field, Frame, ItemField


def test_scalar_and_array_sizes() -> None:
    frame = Frame(
        name="X",
        type=0x10,
        dir="esp32_to_uno",
        fields=[
            Field(name="a", type="u8"),
            Field(name="b", type="i32"),
            Field(name="c", type="i16", count=8),
        ],
    )
    assert frame.max_payload() == 1 + 4 + 16
    assert frame.fixed_size() == 21


def test_struct_array_sizes() -> None:
    frame = Frame(
        name="TONE",
        type=0x32,
        dir="esp32_to_uno",
        fields=[
            Field(name="n", type="u8"),
            Field(
                name="notes",
                items=[ItemField(name="hz", type="u16"), ItemField(name="ms", type="u16")],
                count_field="n",
                max_count=10,
            ),
        ],
    )
    assert frame.max_payload() == 1 + 40
    assert frame.fixed_size() == 1
    assert frame.fields[1].is_struct_array


def test_field_needs_type_or_items() -> None:
    with pytest.raises(ValidationError):
        Field(name="bad")


def test_struct_array_needs_count_field() -> None:
    with pytest.raises(ValidationError):
        Field(name="bad", items=[ItemField(name="x", type="u8")], max_count=3)
```

- [ ] **Step 4: Run it to see it fail**

Run: `cd protocol && uv sync && uv run pytest generator/tests/test_model.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protogen.model'`.

- [ ] **Step 5: Implement the model**

`protocol/generator/protogen/model.py`:
```python
"""Pydantic model of protocol/defs."""

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, model_validator
from pydantic import Field as PField

WireType = Literal["u8", "i8", "u16", "i16", "u32", "i32"]
Direction = Literal["esp32_to_uno", "uno_to_esp32"]

WIRE_SIZE: dict[str, int] = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4}
WIRE_RANGE: dict[str, tuple[int, int]] = {
    "u8": (0, 0xFF),
    "i8": (-0x80, 0x7F),
    "u16": (0, 0xFFFF),
    "i16": (-0x8000, 0x7FFF),
    "u32": (0, 0xFFFFFFFF),
    "i32": (-0x80000000, 0x7FFFFFFF),
}


class ItemField(BaseModel):
    name: str
    type: WireType
    unit: str | None = None


class Field(BaseModel):
    """A payload field: scalar, fixed array (`count`) or struct array (`items`)."""

    name: str
    type: WireType | None = None
    count: int | None = None
    items: list[ItemField] | None = None
    count_field: str | None = None
    max_count: int | None = None
    unit: str | None = None
    scale: float | None = None
    range: tuple[float, float] | None = None
    enum: str | None = None
    sentinel: dict[int, str] | None = None
    doc: str | None = None

    @model_validator(mode="after")
    def _shape(self) -> Field:
        if self.items is None and self.type is None:
            raise ValueError(f"field {self.name}: needs 'type' or 'items'")
        if self.items is not None and (self.count_field is None or self.max_count is None):
            raise ValueError(f"field {self.name}: struct array needs count_field and max_count")
        return self

    @property
    def is_struct_array(self) -> bool:
        return self.items is not None

    def item_size(self) -> int:
        assert self.items is not None
        return sum(WIRE_SIZE[i.type] for i in self.items)

    def max_size(self) -> int:
        if self.items is not None:
            assert self.max_count is not None
            return self.max_count * self.item_size()
        assert self.type is not None
        return WIRE_SIZE[self.type] * (self.count or 1)


class Frame(BaseModel):
    name: str
    type: int
    dir: Direction
    ack: bool = False
    streaming: bool = False
    doc: str = ""
    fields: list[Field] = PField(default_factory=list[Field])

    def max_payload(self) -> int:
        return sum(f.max_size() for f in self.fields)

    def fixed_size(self) -> int:
        return sum(f.max_size() for f in self.fields if not f.is_struct_array)


class EnumDef(BaseModel):
    name: str
    values: dict[str, int]
    doc: str = ""


class ErrorDef(BaseModel):
    code: int
    reason: str
    http: int
    message: str


class ErrorDomain(BaseModel):
    prefix: str
    range: tuple[int, int]
    codes: list[ErrorDef] = PField(default_factory=list[ErrorDef])
    retired: list[int] = PField(default_factory=list[int])


class EventDef(BaseModel):
    name: str
    kind: int
    a: str | None = None
    b: str | None = None
    doc: str = ""


class SettingDef(BaseModel):
    id: int
    name: str
    scope: Literal["global", "profile"]
    type: WireType
    unit: str
    scale: float = 1.0
    min: int
    max: int
    default: int
    doc: str = ""


class GoldenVector(BaseModel):
    frame: str
    seq: int
    values: dict[str, Any]
    hex: str


class Meta(BaseModel):
    protocol_version: int
    sync: int
    max_payload: int
    crc_poly: int
    crc_init: int
    baud: int


class ProtocolModel(BaseModel):
    meta: Meta
    enums: list[EnumDef]
    frames: list[Frame]
    errors: list[ErrorDomain]
    ack_status: list[str]
    events: list[EventDef]
    settings: list[SettingDef]
    golden: list[GoldenVector]

    def frame(self, name: str) -> Frame:
        for f in self.frames:
            if f.name == name:
                return f
        raise KeyError(name)
```

- [ ] **Step 6: Run the test to see it pass**

Run: `cd protocol && uv run pytest generator/tests/test_model.py -v`
Expected: 4 passed.

- [ ] **Step 7: Lint and type-check**

Run: `cd protocol && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: `All checks passed!`, no formatting changes needed (run `uv run ruff format .` if needed), `0 errors`.

- [ ] **Step 8: Commit**

```bash
git add protocol/pyproject.toml protocol/uv.lock protocol/python protocol/generator
git commit -m "feat(protocol): uv workspace and protogen definitions model"
```

---

### Task 2: Protocol definitions (YAML) and the loader

**Files:**
- Create: `protocol/defs/meta.yaml`, `enums.yaml`, `body_frames.yaml`, `errors.yaml`, `events.yaml`, `settings.yaml`, `golden.yaml`; `protocol/generator/protogen/loader.py`
- Test: `protocol/generator/tests/test_loader.py`

**Interfaces:**
- Consumes: `protogen.model.ProtocolModel`.
- Produces: `protogen.loader.load(defs_dir: Path) -> ProtocolModel`; `protogen.loader.DEFS_FILES: tuple[str, ...]`. Frame names used by later plans: `HEARTBEAT, HELLO_ACK, HELLO_REQ, ESP_ACK, DRIVE, DRIVE_RATE, STOP, TURN, MOVE, CANCEL, CAM_JOY, CAM_SET, CAM_NUDGE, CAM_HOME, CAM_LOCK, LED, SOUND, TONE, AUDIO_MODE, BRAKE, ENVELOPE, OWNER_HINT, SETTING_SET, SETTING_GET, SETTINGS_SAVE, CALIBRATE, PROFILE, HELLO, ACK, TELEM_FAST, TELEM_SLOW, EVENT, SETTING_VALUE, CAL_RESULT, CAL_DATA, WIFI_RESET_REQ, ADMIN_RESET_REQ`.

- [ ] **Step 1: Write the failing loader test**

`protocol/generator/tests/test_loader.py`:
```python
from pathlib import Path

from protogen.loader import load

DEFS = Path(__file__).resolve().parents[2] / "defs"


def test_loads_real_defs() -> None:
    model = load(DEFS)
    assert model.meta.baud == 250000
    assert model.meta.max_payload == 48
    names = {f.name for f in model.frames}
    assert {"HEARTBEAT", "TURN", "TONE", "TELEM_FAST", "TELEM_SLOW", "CAL_DATA"} <= names


def test_payload_sizes_match_lld() -> None:
    model = load(DEFS)
    assert model.frame("TELEM_FAST").max_payload() == 40
    assert model.frame("TELEM_SLOW").max_payload() == 36
    assert model.frame("CAL_DATA").max_payload() == 20
    assert model.frame("TONE").max_payload() == 41
    assert model.frame("TURN").max_payload() == 7


def test_ack_status_starts_with_ok() -> None:
    model = load(DEFS)
    assert model.ack_status[0] == "OK"
```

- [ ] **Step 2: Run it to see it fail**

Run: `cd protocol && uv run pytest generator/tests/test_loader.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protogen.loader'`.

- [ ] **Step 3: Write the definitions**

`protocol/defs/meta.yaml`:
```yaml
protocol_version: 1
sync: 0xAA
max_payload: 48
crc_poly: 0x07
crc_init: 0x00
baud: 250000
```

`protocol/defs/enums.yaml`:
```yaml
- name: Source
  doc: Who sent a network motion command (ESP32 already arbitrated human vs AI).
  values: { NONE: 0, HUMAN: 1, AI: 2 }
- name: Owner
  doc: Motion owner as seen by the Uno.
  values: { NONE: 0, NETWORK: 1, IR: 2, CALIBRATION: 3 }
- name: OwnerHint
  doc: Network owner sent to the Uno for the LED colour.
  values: { NONE: 0, HUMAN: 1, AI: 2 }
- name: TurnMode
  doc: TURN.mode bits 0-6; bit 7 = pivot.
  values: { BY: 0, TO: 1 }
- name: LedMode
  values: { OFF: 0, SOLID: 1, BLINK: 2, PULSE: 3 }
- name: AudioMode
  values: { NORMAL: 0, QUIET: 1, MUTE: 2 }
- name: CalOp
  values: { START: 0, CANCEL: 1 }
- name: CalScript
  values:
    { STILL_SAMPLE: 0, PWM_RAMP: 1, FULL_SPIN: 2, SPIN_AND_ARC: 3, RATE_STEPS: 4,
      RELAY_OSCILLATION: 5, DRIVE_COUNTS: 6, SERVO_NUDGE: 7, IDLE: 8 }
- name: ProfileOp
  values: { SELECT: 0, CREATE: 1, DELETE: 2 }
- name: PrimitiveKind
  values: { NONE: 0, TELEOP: 1, TURN: 2, MOVE: 3 }
- name: ResetReason
  values: { UNKNOWN: 0, POWER_ON: 1, EXTERNAL: 2, BROWN_OUT: 3, WATCHDOG: 4 }
- name: SoundId
  doc: Sounds a client may trigger with SOUND.
  values:
    { STARTUP: 1, READY: 2, SELFTEST_FAIL: 3, CHIRP_UP: 4, CHIRP_DOWN: 5, BLIP: 6, TICK: 7,
      JINGLE: 8, THINKING: 9, OUCH: 10, ERROR: 11 }
- name: FastFlagBit
  doc: Bit positions in TELEM_FAST.flags.
  values:
    { BLOCKED: 0, LIFTED: 1, TILTED: 2, STALL_L: 3, STALL_R: 4, CAM_MOVING: 5, TURN_LIMITED: 6,
      CAPPED: 7, SOFT_STOP: 8, HALTED: 9, LOCKED: 10, IMU_FAULT: 11, US_FAULT: 12,
      ENCODER_FAULT: 13, CALIBRATING: 14, SYNCED: 15 }
- name: SlowFlagBit
  doc: Bit positions in TELEM_SLOW.flags2.
  values:
    { BATT_LOW: 0, BATT_CRITICAL: 1, LINK_LOST: 2, BRAKE_OFF: 3, MUTED: 4, QUIET: 5,
      ENVELOPE: 6, DEADMAN: 7 }
- name: HwBit
  doc: Bit positions in TELEM_SLOW.hw.
  values: { ENCODERS_ENABLED: 0, IMU_OK: 1, US_OK: 2 }
```

`protocol/defs/body_frames.yaml`:
```yaml
# ---------------- ESP32 -> Uno (0x01-0x7F) ----------------
- name: HEARTBEAT
  type: 0x01
  dir: esp32_to_uno
  doc: Keep-alive every 100 ms. No valid frame for 300 ms = link lost on the Uno.
  fields:
    - { name: esp_ms, type: u32, unit: ms }
- name: HELLO_ACK
  type: 0x02
  dir: esp32_to_uno
  doc: Sent after ENVELOPE is acknowledged; the Uno marks the link synced.
  fields:
    - { name: proto, type: u8 }
- name: HELLO_REQ
  type: 0x03
  dir: esp32_to_uno
  doc: Ask the Uno to re-send HELLO.
- name: ESP_ACK
  type: 0x04
  dir: esp32_to_uno
  doc: Acknowledges Uno-originated requests (WIFI_RESET_REQ, ADMIN_RESET_REQ).
  fields:
    - { name: ack_seq, type: u8 }
    - { name: status, type: u8, doc: index into ack_status }
- name: DRIVE
  type: 0x10
  dir: esp32_to_uno
  streaming: true
  doc: Streamed joystick drive (<= 20 Hz). w > 0 turns left (CCW).
  fields:
    - { name: src, type: u8, enum: Source }
    - { name: v, type: i8, unit: pct, range: [-100, 100] }
    - { name: w, type: i8, unit: pct, range: [-100, 100] }
- name: DRIVE_RATE
  type: 0x11
  dir: esp32_to_uno
  streaming: true
  doc: Streamed drive with closed-loop yaw rate.
  fields:
    - { name: src, type: u8, enum: Source }
    - { name: v, type: i8, unit: pct, range: [-100, 100] }
    - { name: yaw_rate, type: i16, unit: deg_s, scale: 0.1 }
- name: STOP
  type: 0x12
  dir: esp32_to_uno
  ack: true
  doc: Immediate smooth stop; cancels primitives. Accepted from any source.
  fields:
    - { name: src, type: u8, enum: Source }
- name: TURN
  type: 0x13
  dir: esp32_to_uno
  ack: true
  doc: Gyro turn by/to an angle; radius 0 = spin in place.
  fields:
    - { name: src, type: u8, enum: Source }
    - { name: mode, type: u8, doc: "bits 0-6 TurnMode, bit 7 pivot" }
    - { name: angle, type: i16, unit: deg, scale: 0.1 }
    - { name: speed, type: u8, unit: pct, range: [0, 100] }
    - { name: radius_cm, type: u16, unit: cm, sentinel: { 0: spin } }
- name: MOVE
  type: 0x14
  dir: esp32_to_uno
  ack: true
  doc: Drive a distance with heading hold (needs encoders).
  fields:
    - { name: src, type: u8, enum: Source }
    - { name: dist_mm, type: i32, unit: mm }
    - { name: speed, type: u8, unit: pct, range: [0, 100] }
    - { name: heading, type: i16, unit: deg, scale: 0.1, sentinel: { 32767: current } }
- name: CANCEL
  type: 0x15
  dir: esp32_to_uno
  ack: true
  doc: Cancel the running primitive.
  fields:
    - { name: src, type: u8, enum: Source }
- name: CAM_JOY
  type: 0x20
  dir: esp32_to_uno
  streaming: true
  doc: Absolute joystick camera; release returns home unless locked.
  fields:
    - { name: x, type: i8, unit: pct, range: [-100, 100] }
    - { name: y, type: i8, unit: pct, range: [-100, 100] }
    - { name: active, type: u8 }
- name: CAM_SET
  type: 0x21
  dir: esp32_to_uno
  ack: true
  fields:
    - { name: pan, type: i16, unit: deg, scale: 0.1 }
    - { name: tilt, type: i16, unit: deg, scale: 0.1 }
    - { name: speed, type: u8, unit: deg_s, scale: 2 }
- name: CAM_NUDGE
  type: 0x22
  dir: esp32_to_uno
  ack: true
  fields:
    - { name: dpan, type: i16, unit: deg, scale: 0.1 }
    - { name: dtilt, type: i16, unit: deg, scale: 0.1 }
- name: CAM_HOME
  type: 0x23
  dir: esp32_to_uno
  ack: true
- name: CAM_LOCK
  type: 0x24
  dir: esp32_to_uno
  ack: true
  fields:
    - { name: on, type: u8 }
- name: LED
  type: 0x30
  dir: esp32_to_uno
  ack: true
  doc: Custom LED layer (priority 4); alarms override.
  fields:
    - { name: mode, type: u8, enum: LedMode }
    - { name: r, type: u8 }
    - { name: g, type: u8 }
    - { name: b, type: u8 }
    - { name: period_ms, type: u16, unit: ms }
    - { name: duration_ms, type: u16, unit: ms, sentinel: { 0: until_changed } }
- name: SOUND
  type: 0x31
  dir: esp32_to_uno
  ack: true
  fields:
    - { name: id, type: u8, enum: SoundId }
- name: TONE
  type: 0x32
  dir: esp32_to_uno
  ack: true
  doc: Custom tone sequence, up to 10 notes.
  fields:
    - { name: n, type: u8 }
    - name: notes
      items: [{ name: hz, type: u16, unit: hz }, { name: ms, type: u16, unit: ms }]
      count_field: n
      max_count: 10
- name: AUDIO_MODE
  type: 0x33
  dir: esp32_to_uno
  ack: true
  fields:
    - { name: mode, type: u8, enum: AudioMode }
- name: BRAKE
  type: 0x40
  dir: esp32_to_uno
  ack: true
  doc: Obstacle brake on/off; refused (ENVELOPE_LOCKED) while lock_brake.
  fields:
    - { name: on, type: u8 }
- name: ENVELOPE
  type: 0x41
  dir: esp32_to_uno
  ack: true
  doc: Speed/accel caps; lock_brake=1 forces the obstacle brake on.
  fields:
    - { name: speed_cap, type: u8, unit: pct, range: [0, 100] }
    - { name: accel_cap, type: u8, unit: pct_s }
    - { name: lock_brake, type: u8 }
- name: OWNER_HINT
  type: 0x42
  dir: esp32_to_uno
  doc: Network owner for LED colour; re-sent on change and after re-sync.
  fields:
    - { name: owner, type: u8, enum: OwnerHint }
- name: SETTING_SET
  type: 0x50
  dir: esp32_to_uno
  ack: true
  fields:
    - { name: id, type: u8 }
    - { name: value, type: i32, doc: fixed-point per settings.yaml scale }
- name: SETTING_GET
  type: 0x51
  dir: esp32_to_uno
  ack: true
  doc: Reply is SETTING_VALUE.
  fields:
    - { name: id, type: u8 }
- name: SETTINGS_SAVE
  type: 0x52
  dir: esp32_to_uno
  ack: true
  doc: Persist to EEPROM (deferred until stopped); settings_saved event when done.
- name: CALIBRATE
  type: 0x60
  dir: esp32_to_uno
  ack: true
  doc: Run/cancel a calibration motion script; Uno streams CAL_DATA, ESP32 computes.
  fields:
    - { name: op, type: u8, enum: CalOp }
    - { name: script, type: u8, enum: CalScript }
    - { name: param, type: i32 }
    - { name: profile, type: u8 }
- name: PROFILE
  type: 0x61
  dir: esp32_to_uno
  ack: true
  fields:
    - { name: op, type: u8, enum: ProfileOp }
    - { name: index, type: u8, range: [0, 2] }

# ---------------- Uno -> ESP32 (0x80-0xFF) ----------------
- name: HELLO
  type: 0x80
  dir: uno_to_esp32
  doc: Sent at boot every 500 ms until HELLO_ACK, and on HELLO_REQ.
  fields:
    - { name: fw, type: u16, doc: "major << 8 | minor" }
    - { name: proto, type: u8 }
    - { name: caps, type: u8, doc: "bit0 encoders enabled" }
    - { name: reset_reason, type: u8, enum: ResetReason }
- name: ACK
  type: 0x81
  dir: uno_to_esp32
  fields:
    - { name: ack_seq, type: u8 }
    - { name: status, type: u8, doc: index into ack_status }
- name: TELEM_FAST
  type: 0x90
  dir: uno_to_esp32
  doc: 20 Hz motion telemetry.
  fields:
    - { name: uno_ms, type: u32, unit: ms }
    - { name: yaw, type: u16, unit: deg, scale: 0.01, range: [0, 35999], doc: CCW positive }
    - { name: yaw_rate, type: i16, unit: deg_s, scale: 0.1 }
    - { name: pitch, type: i16, unit: deg, scale: 0.01 }
    - { name: roll, type: i16, unit: deg, scale: 0.01 }
    - { name: spd_cmd_l, type: i8, unit: pct }
    - { name: spd_cmd_r, type: i8, unit: pct }
    - { name: spd_act_l, type: i8, unit: pct, sentinel: { -128: not_available } }
    - { name: spd_act_r, type: i8, unit: pct, sentinel: { -128: not_available } }
    - { name: pwm_l, type: i16, range: [-255, 255] }
    - { name: pwm_r, type: i16, range: [-255, 255] }
    - { name: dist_mm, type: u16, unit: mm, sentinel: { 65535: clear, 65534: sensor_fault } }
    - { name: closing_cm_s, type: i16, unit: cm_s }
    - { name: pan, type: i16, unit: deg, scale: 0.1 }
    - { name: tilt, type: i16, unit: deg, scale: 0.1 }
    - { name: enc_l, type: i32, unit: counts }
    - { name: enc_r, type: i32, unit: counts }
    - { name: flags, type: u16, doc: FastFlagBit }
    - { name: prim, type: u8, enum: PrimitiveKind }
    - { name: prim_pct, type: u8, unit: pct }
- name: TELEM_SLOW
  type: 0x91
  dir: uno_to_esp32
  doc: 1 Hz health/status telemetry.
  fields:
    - { name: uptime_s, type: u32, unit: s }
    - { name: reset_reason, type: u8, enum: ResetReason }
    - { name: batt_mv, type: u16, unit: mV }
    - { name: driving, type: u8 }
    - { name: temp, type: i16, unit: degC, scale: 0.01 }
    - { name: owner, type: u8, enum: Owner }
    - { name: flags2, type: u32, doc: SlowFlagBit }
    - { name: accel_var, type: u16 }
    - { name: loop_load, type: u8, unit: pct }
    - { name: loop_max_us, type: u16, unit: us }
    - { name: free_ram, type: u16, unit: bytes }
    - { name: err_frames, type: u16 }
    - { name: err_i2c, type: u16 }
    - { name: settings_crc, type: u16 }
    - { name: env_speed_cap, type: u8, unit: pct }
    - { name: env_accel_cap, type: u8, unit: pct_s }
    - { name: env_lock_brake, type: u8 }
    - { name: profile_idx, type: u8 }
    - { name: cal_status, type: u8, count: 3, doc: "2 bits per calibration item (missing/ok)" }
    - { name: hw, type: u8, doc: HwBit }
- name: EVENT
  type: 0x92
  dir: uno_to_esp32
  doc: Event kind from events.yaml with a/b packing.
  fields:
    - { name: kind, type: u8 }
    - { name: a, type: i16 }
    - { name: b, type: i16 }
- name: SETTING_VALUE
  type: 0x93
  dir: uno_to_esp32
  fields:
    - { name: id, type: u8 }
    - { name: value, type: i32 }
- name: CAL_RESULT
  type: 0x94
  dir: uno_to_esp32
  doc: Script progress/done/fail.
  fields:
    - { name: script, type: u8, enum: CalScript }
    - { name: stage, type: u8 }
    - { name: progress, type: u8, unit: pct }
    - { name: status, type: u8, doc: index into ack_status }
- name: CAL_DATA
  type: 0x95
  dir: uno_to_esp32
  doc: Raw calibration samples at 50 Hz (channel layout per script in docs).
  fields:
    - { name: script, type: u8, enum: CalScript }
    - { name: phase, type: u8 }
    - { name: t_ms, type: u16, unit: ms }
    - { name: v, type: i16, count: 8 }
- name: WIFI_RESET_REQ
  type: 0x96
  dir: uno_to_esp32
  ack: true
  doc: Mode button held 5 s at boot; re-sent every 500 ms until ESP_ACK.
- name: ADMIN_RESET_REQ
  type: 0x97
  dir: uno_to_esp32
  ack: true
  doc: Mode button held 15 s at boot; re-sent every 500 ms until ESP_ACK.
```

`protocol/defs/errors.yaml`:
```yaml
domains:
  - prefix: AUTH
    range: [1000, 1999]
    codes:
      - { code: 1001, reason: AUTH_FAILED, http: 401, message: Invalid or revoked key }
      - { code: 1002, reason: ROLE_FORBIDDEN, http: 403, message: Role may not do this }
  - prefix: TANK
    range: [2000, 2999]
    codes:
      - { code: 2001, reason: TANK_OFFLINE, http: 503, message: Tank is not responding }
      - { code: 2002, reason: TANK_NOT_FOUND, http: 404, message: Unknown tank }
  - prefix: COMMAND
    range: [3000, 3999]
    codes:
      - { code: 3001, reason: NOT_OWNER, http: 409, message: Another source has control }
      - { code: 3002, reason: BLOCKED, http: 422, message: Blocked by a safety rule }
      - { code: 3003, reason: UNAVAILABLE, http: 422, message: Capability not available }
      - { code: 3004, reason: ENVELOPE_LOCKED, http: 409, message: Locked by the safety envelope }
      - { code: 3005, reason: BUSY, http: 409, message: Busy, try again }
      - { code: 3006, reason: INVALID_COMMAND, http: 422, message: Invalid command }
      - { code: 3007, reason: BODY_OFFLINE, http: 503, message: Body (Uno) offline }
      - { code: 3008, reason: BODY_INCOMPATIBLE, http: 503, message: Body firmware incompatible }
      - { code: 3009, reason: COMMAND_TIMEOUT, http: 504, message: No acknowledgement in time }
      - { code: 3010, reason: RATE_LIMITED, http: 429, message: Too many commands }
      - { code: 3011, reason: OWNER_LOST, http: 409, message: Controlling client lost }
      - { code: 3012, reason: LINK_LOST, http: 503, message: Link to the body lost }
      - { code: 3013, reason: CANCELLED, http: 409, message: Cancelled }
      - { code: 3014, reason: TIMEOUT, http: 504, message: Primitive timed out }
      - { code: 3015, reason: STALLED, http: 422, message: Track stalled }
  - prefix: CALIBRATION
    range: [6000, 6999]
    codes:
      - { code: 6001, reason: CAL_PRECONDITION, http: 409, message: Calibration preconditions not met }
      - { code: 6002, reason: CAL_FAILED, http: 422, message: Calibration failed }
      - { code: 6003, reason: CAL_CANCELLED, http: 409, message: Calibration cancelled }
  - prefix: SETTINGS
    range: [8000, 8999]
    codes:
      - { code: 8001, reason: SETTING_UNKNOWN, http: 404, message: Unknown setting }
      - { code: 8002, reason: SETTING_OUT_OF_RANGE, http: 422, message: Value out of range }
      - { code: 8003, reason: SETTINGS_SAVE_BUSY, http: 409, message: Save already in progress }
      - { code: 8004, reason: ENVELOPE_MISMATCH, http: 409, message: Envelope differs from stored }
  - prefix: SYSTEM
    range: [9000, 9999]
    codes:
      - { code: 9001, reason: INTERNAL, http: 500, message: Internal error }
      - { code: 9002, reason: CAMERA_TIMEOUT, http: 504, message: Camera timeout }
# On-wire ACK/CAL_RESULT status byte = index into this list. Never reorder; append only.
ack_status:
  - OK
  - NOT_OWNER
  - BLOCKED
  - UNAVAILABLE
  - ENVELOPE_LOCKED
  - BUSY
  - INVALID_COMMAND
  - CANCELLED
  - TIMEOUT
  - STALLED
  - LINK_LOST
  - SETTING_UNKNOWN
  - SETTING_OUT_OF_RANGE
  - SETTINGS_SAVE_BUSY
  - CAL_PRECONDITION
  - CAL_FAILED
  - CAL_CANCELLED
  - INTERNAL
```

`protocol/defs/events.yaml`:
```yaml
- { name: BUMP, kind: 1, a: "g-force in mg", b: "direction 0 front, 1 back" }
- { name: LIFTED, kind: 2 }
- { name: LANDED, kind: 3 }
- { name: TILTED, kind: 4, a: "pitch, 0.1 deg", b: "roll, 0.1 deg" }
- { name: LEVEL, kind: 5 }
- { name: STALL, kind: 6, a: "side 0 left, 1 right" }
- { name: BLOCKED, kind: 7, a: "distance mm" }
- { name: UNBLOCKED, kind: 8 }
- { name: CONTROL_CHANGED, kind: 9, a: "Owner" }
- { name: IR_OVERRIDE, kind: 10 }
- { name: DEADMAN_STOP, kind: 11 }
- { name: LINK_LOST, kind: 12 }
- { name: LINK_RESTORED, kind: 13 }
- { name: BATT_LOW, kind: 14, a: "mV" }
- { name: BATT_CRITICAL, kind: 15, a: "mV" }
- { name: PRIMITIVE_DONE, kind: 16, a: "PrimitiveKind | ok << 8", b: "ack_status index (reason)" }
- { name: SELFTEST, kind: 17, a: "failure bitmask", b: "critical 0/1" }
- { name: ENVELOPE_CHANGED, kind: 18, a: "speed_cap | accel_cap << 8", b: "lock_brake" }
- { name: ENCODER_FAULT, kind: 19, a: "side 0 left, 1 right" }
- { name: IMU_FAULT, kind: 20 }
- { name: US_FAULT, kind: 21 }
- { name: SETTINGS_SAVED, kind: 22 }
- { name: SETTINGS_RESET, kind: 23, a: "block mask" }
- { name: CAL_REQUEST, kind: 24 }
- { name: BUTTON_PRESS, kind: 25 }
- { name: REJECTED, kind: 26, a: "frame type", b: "ack_status index", doc: "rate-limited 1 per 500 ms per source" }
```

`protocol/defs/settings.yaml` (fixed-point integers on the wire; `scale` converts to `unit`):
```yaml
- { id: 1, name: encoders_enabled, scope: global, type: u8, unit: bool, min: 0, max: 1, default: 0 }
- { id: 2, name: counts_per_rev, scope: global, type: u16, unit: counts, min: 1, max: 10000, default: 960 }
- { id: 3, name: counts_per_cm, scope: global, type: u16, unit: counts_cm, scale: 0.01, min: 100, max: 60000, default: 7600 }
- { id: 4, name: motor_v_max, scope: global, type: u16, unit: mV, min: 3000, max: 12000, default: 6000 }
- { id: 5, name: accel_limit, scope: global, type: u16, unit: pct_s, min: 10, max: 1000, default: 200 }
- { id: 6, name: turn_rate_max, scope: global, type: u16, unit: deg_s, min: 10, max: 720, default: 180 }
- { id: 7, name: turn_rate_full_speed, scope: profile, type: u8, unit: pct, min: 10, max: 100, default: 40 }
- { id: 8, name: track_width_eff, scope: profile, type: u16, unit: mm, min: 50, max: 500, default: 150 }
- { id: 9, name: pivot_fallback, scope: global, type: u8, unit: bool, min: 0, max: 1, default: 1 }
- { id: 10, name: reverse_steer, scope: global, type: u8, unit: enum, min: 0, max: 1, default: 0, doc: "0 car, 1 tank" }
- { id: 11, name: obstacle_stop, scope: global, type: u8, unit: cm, min: 5, max: 100, default: 15 }
- { id: 12, name: obstacle_slow, scope: global, type: u8, unit: cm, min: 10, max: 200, default: 50 }
- { id: 13, name: tilt_limit, scope: global, type: u8, unit: deg, min: 10, max: 60, default: 35 }
- { id: 14, name: bump_threshold, scope: global, type: u16, unit: mg, min: 500, max: 4000, default: 1500 }
- { id: 15, name: deadman, scope: global, type: u16, unit: ms, min: 100, max: 2000, default: 300 }
- { id: 16, name: batt_low, scope: global, type: u16, unit: mV, min: 6000, max: 8400, default: 7000 }
- { id: 17, name: batt_crit, scope: global, type: u16, unit: mV, min: 5500, max: 8000, default: 6600 }
- { id: 18, name: batt_mv_per_count, scope: global, type: u16, unit: mV, scale: 0.01, min: 1000, max: 10000, default: 4050 }
- { id: 19, name: ir_handback, scope: global, type: u16, unit: ms, min: 500, max: 10000, default: 3000 }
- { id: 20, name: parking_beeps, scope: global, type: u8, unit: bool, min: 0, max: 1, default: 0 }
- { id: 21, name: cam_return_delay, scope: global, type: u16, unit: ms, min: 0, max: 5000, default: 500 }
- { id: 22, name: servo_idle_detach, scope: global, type: u8, unit: bool, min: 0, max: 1, default: 0 }
- { id: 23, name: pan_min, scope: global, type: u8, unit: deg, min: 0, max: 180, default: 0 }
- { id: 24, name: pan_max, scope: global, type: u8, unit: deg, min: 0, max: 180, default: 180 }
- { id: 25, name: tilt_min, scope: global, type: u8, unit: deg, min: 0, max: 180, default: 30 }
- { id: 26, name: tilt_max, scope: global, type: u8, unit: deg, min: 0, max: 180, default: 110 }
- { id: 27, name: pan_trim, scope: global, type: i16, unit: deg, scale: 0.1, min: -300, max: 300, default: 0 }
- { id: 28, name: tilt_trim, scope: global, type: i16, unit: deg, scale: 0.1, min: -300, max: 300, default: 0 }
- { id: 29, name: servo_speed, scope: global, type: u16, unit: deg_s, min: 10, max: 600, default: 120 }
- { id: 30, name: servo_accel, scope: global, type: u16, unit: deg_s2, min: 50, max: 3000, default: 400 }
- { id: 31, name: gyro_bias_x, scope: global, type: i16, unit: raw, min: -2000, max: 2000, default: 0 }
- { id: 32, name: gyro_bias_y, scope: global, type: i16, unit: raw, min: -2000, max: 2000, default: 0 }
- { id: 33, name: gyro_bias_z, scope: global, type: i16, unit: raw, min: -2000, max: 2000, default: 0 }
- { id: 34, name: level_pitch, scope: global, type: i16, unit: deg, scale: 0.01, min: -2000, max: 2000, default: 0 }
- { id: 35, name: level_roll, scope: global, type: i16, unit: deg, scale: 0.01, min: -2000, max: 2000, default: 0 }
- { id: 36, name: ultrasonic_offset, scope: global, type: u8, unit: mm, min: 0, max: 100, default: 0 }
- { id: 37, name: active_profile, scope: global, type: u8, unit: index, min: 0, max: 2, default: 0 }
- { id: 40, name: deadband_start_l_fwd, scope: profile, type: u8, unit: pwm, min: 0, max: 255, default: 70 }
- { id: 41, name: deadband_start_l_rev, scope: profile, type: u8, unit: pwm, min: 0, max: 255, default: 70 }
- { id: 42, name: deadband_start_r_fwd, scope: profile, type: u8, unit: pwm, min: 0, max: 255, default: 70 }
- { id: 43, name: deadband_start_r_rev, scope: profile, type: u8, unit: pwm, min: 0, max: 255, default: 70 }
- { id: 44, name: deadband_keep_l_fwd, scope: profile, type: u8, unit: pwm, min: 0, max: 255, default: 55 }
- { id: 45, name: deadband_keep_l_rev, scope: profile, type: u8, unit: pwm, min: 0, max: 255, default: 55 }
- { id: 46, name: deadband_keep_r_fwd, scope: profile, type: u8, unit: pwm, min: 0, max: 255, default: 55 }
- { id: 47, name: deadband_keep_r_rev, scope: profile, type: u8, unit: pwm, min: 0, max: 255, default: 55 }
- { id: 48, name: max_cps_l, scope: profile, type: u16, unit: counts_s, min: 100, max: 20000, default: 2500 }
- { id: 49, name: max_cps_r, scope: profile, type: u16, unit: counts_s, min: 100, max: 20000, default: 2500 }
- { id: 52, name: pid_speed_kp, scope: profile, type: i32, unit: gain, scale: 0.001, min: 0, max: 100000, default: 150 }
- { id: 53, name: pid_speed_ki, scope: profile, type: i32, unit: gain, scale: 0.001, min: 0, max: 100000, default: 800 }
- { id: 54, name: pid_speed_kd, scope: profile, type: i32, unit: gain, scale: 0.001, min: 0, max: 100000, default: 0 }
- { id: 55, name: pid_rate_kp, scope: profile, type: i32, unit: gain, scale: 0.001, min: 0, max: 100000, default: 1200 }
- { id: 56, name: pid_rate_ki, scope: profile, type: i32, unit: gain, scale: 0.001, min: 0, max: 100000, default: 400 }
- { id: 57, name: pid_rate_kd, scope: profile, type: i32, unit: gain, scale: 0.001, min: 0, max: 100000, default: 20 }
- { id: 58, name: pid_heading_kp, scope: profile, type: i32, unit: gain, scale: 0.001, min: 0, max: 100000, default: 3000 }
- { id: 59, name: pid_heading_ki, scope: profile, type: i32, unit: gain, scale: 0.001, min: 0, max: 100000, default: 0 }
- { id: 60, name: pid_heading_kd, scope: profile, type: i32, unit: gain, scale: 0.001, min: 0, max: 100000, default: 100 }
```

`protocol/defs/golden.yaml` (hand-computed; anchors every codec):
```yaml
- frame: HEARTBEAT
  seq: 0
  values: { esp_ms: 0x01020304 }
  hex: "aa 04 01 00 04 03 02 01 94"
- frame: STOP
  seq: 5
  values: { src: 1 }
  hex: "aa 01 12 05 01 24"
- frame: TURN
  seq: 7
  values: { src: 2, mode: 0x81, angle: -900, speed: 40, radius_cm: 0 }
  hex: "aa 07 13 07 02 81 7c fc 28 00 00 e5"
- frame: TONE
  seq: 9
  values: { n: 2, notes: [[440, 100], [880, 50]] }
  hex: "aa 09 32 09 02 b8 01 64 00 70 03 32 00 5c"
- frame: ACK
  seq: 3
  values: { ack_seq: 7, status: 0 }
  hex: "aa 02 81 03 07 00 35"
```

- [ ] **Step 4: Implement the loader**

`protocol/generator/protogen/loader.py`:
```python
"""Load protocol/defs/*.yaml into a ProtocolModel."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import yaml

from .model import ProtocolModel

DEFS_FILES: tuple[str, ...] = (
    "meta",
    "enums",
    "body_frames",
    "errors",
    "events",
    "settings",
    "golden",
)


def _read(path: Path) -> Any:
    with path.open(encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def load(defs_dir: Path) -> ProtocolModel:
    raw = {stem: _read(defs_dir / f"{stem}.yaml") for stem in DEFS_FILES}
    errors = raw["errors"]
    return ProtocolModel.model_validate(
        {
            "meta": raw["meta"],
            "enums": raw["enums"],
            "frames": raw["body_frames"],
            "errors": errors["domains"],
            "ack_status": errors["ack_status"],
            "events": raw["events"],
            "settings": raw["settings"],
            "golden": raw["golden"],
        }
    )
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cd protocol && uv run pytest generator/tests -v`
Expected: all passed (7 tests).

- [ ] **Step 6: Lint, type-check, commit**

Run: `cd protocol && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: clean.
```bash
git add protocol/defs protocol/generator/protogen/loader.py protocol/generator/tests/test_loader.py
git commit -m "feat(protocol): body-link definitions (frames, enums, errors, events, settings, golden) and loader"
```

---

### Task 3: Validator

**Files:**
- Create: `protocol/generator/protogen/validator.py`
- Test: `protocol/generator/tests/test_validator.py`

**Interfaces:**
- Consumes: `load()`, model classes.
- Produces: `protogen.validator.validate(model: ProtocolModel) -> list[str]` (empty = valid).

- [ ] **Step 1: Write the failing tests**

`protocol/generator/tests/test_validator.py`:
```python
from pathlib import Path

from protogen.loader import load
from protogen.model import ErrorDef, Field, ProtocolModel, SettingDef
from protogen.validator import validate

DEFS = Path(__file__).resolve().parents[2] / "defs"


def _model() -> ProtocolModel:
    return load(DEFS)


def test_real_defs_are_valid() -> None:
    assert validate(_model()) == []


def test_duplicate_frame_type() -> None:
    m = _model()
    m.frame("STOP").type = m.frame("DRIVE").type
    assert any("duplicate type" in i for i in validate(m))


def test_direction_range() -> None:
    m = _model()
    m.frame("HELLO").type = 0x05
    assert any("HELLO" in i and "range" in i for i in validate(m))


def test_payload_too_big() -> None:
    m = _model()
    m.frame("STOP").fields.append(Field(name="pad", type="i32", count=12))
    assert any("payload" in i and "STOP" in i for i in validate(m))


def test_error_code_outside_domain() -> None:
    m = _model()
    m.errors[0].codes.append(ErrorDef(code=2500, reason="X_BAD", http=400, message="x"))
    assert any("2500" in i for i in validate(m))


def test_retired_code_reused() -> None:
    m = _model()
    m.errors[0].retired.append(1001)
    assert any("1001" in i and "retired" in i for i in validate(m))


def test_ack_status_unknown_reason() -> None:
    m = _model()
    m.ack_status.append("NO_SUCH_REASON")
    assert any("NO_SUCH_REASON" in i for i in validate(m))


def test_setting_default_out_of_range() -> None:
    m = _model()
    m.settings.append(
        SettingDef(id=99, name="bad", scope="global", type="u8", unit="x", min=0, max=10, default=11)
    )
    assert any("bad" in i and "default" in i for i in validate(m))


def test_setting_range_exceeds_wire_type() -> None:
    m = _model()
    m.settings.append(
        SettingDef(id=98, name="wide", scope="global", type="u8", unit="x", min=0, max=300, default=1)
    )
    assert any("wide" in i and "wire" in i for i in validate(m))


def test_count_field_must_precede_array() -> None:
    m = _model()
    tone = m.frame("TONE")
    tone.fields.reverse()
    assert any("TONE" in i and "count_field" in i for i in validate(m))
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd protocol && uv run pytest generator/tests/test_validator.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protogen.validator'`.

- [ ] **Step 3: Implement the validator**

`protocol/generator/protogen/validator.py`:
```python
"""Validation rules for the protocol model. Returns human-readable issues."""

from __future__ import annotations

from .model import WIRE_RANGE, Frame, ProtocolModel

_DIR_RANGE = {"esp32_to_uno": (0x01, 0x7F), "uno_to_esp32": (0x80, 0xFF)}


def _frame_issues(frame: Frame, max_payload: int, enum_names: set[str]) -> list[str]:
    issues: list[str] = []
    lo, hi = _DIR_RANGE[frame.dir]
    if not lo <= frame.type <= hi:
        issues.append(f"frame {frame.name}: type 0x{frame.type:02X} outside {frame.dir} range")
    if frame.max_payload() > max_payload:
        issues.append(f"frame {frame.name}: payload {frame.max_payload()} > {max_payload}")
    seen: set[str] = set()
    for f in frame.fields:
        if f.name in seen:
            issues.append(f"frame {frame.name}: duplicate field {f.name}")
        if f.enum is not None and f.enum not in enum_names:
            issues.append(f"frame {frame.name}.{f.name}: unknown enum {f.enum}")
        if f.items is not None:
            earlier = [g for g in frame.fields if g.name in seen]
            match = [g for g in earlier if g.name == f.count_field]
            if not match or match[0].type != "u8":
                issues.append(
                    f"frame {frame.name}.{f.name}: count_field {f.count_field} must be an "
                    "earlier u8 field"
                )
        seen.add(f.name)
    return issues


def validate(model: ProtocolModel) -> list[str]:
    issues: list[str] = []
    enum_names = {e.name for e in model.enums}

    names: set[str] = set()
    types: set[tuple[str, int]] = set()
    for frame in model.frames:
        if frame.name in names:
            issues.append(f"frame {frame.name}: duplicate name")
        if (frame.dir, frame.type) in types:
            issues.append(f"frame {frame.name}: duplicate type 0x{frame.type:02X}")
        names.add(frame.name)
        types.add((frame.dir, frame.type))
        issues += _frame_issues(frame, model.meta.max_payload, enum_names)

    codes: set[int] = set()
    reasons: set[str] = set()
    for domain in model.errors:
        lo, hi = domain.range
        for err in domain.codes:
            if not lo <= err.code <= hi:
                issues.append(f"error {err.reason}: code {err.code} outside {domain.prefix} range")
            if err.code in domain.retired:
                issues.append(f"error {err.reason}: code {err.code} is retired")
            if err.code in codes:
                issues.append(f"error {err.reason}: duplicate code {err.code}")
            if err.reason in reasons:
                issues.append(f"error {err.reason}: duplicate reason")
            codes.add(err.code)
            reasons.add(err.reason)

    if not model.ack_status or model.ack_status[0] != "OK":
        issues.append("ack_status: first entry must be OK")
    if len(model.ack_status) > 256:
        issues.append("ack_status: more than 256 entries")
    for reason in model.ack_status[1:]:
        if reason not in reasons:
            issues.append(f"ack_status: unknown reason {reason}")

    kinds: set[int] = set()
    for ev in model.events:
        if not 0 <= ev.kind <= 0xFF or ev.kind in kinds:
            issues.append(f"event {ev.name}: kind {ev.kind} invalid or duplicate")
        kinds.add(ev.kind)

    ids: set[int] = set()
    setting_names: set[str] = set()
    for s in model.settings:
        if not 0 <= s.id <= 0xFF or s.id in ids:
            issues.append(f"setting {s.name}: id {s.id} invalid or duplicate")
        if s.name in setting_names:
            issues.append(f"setting {s.name}: duplicate name")
        wlo, whi = WIRE_RANGE[s.type]
        if s.min < wlo or s.max > whi:
            issues.append(f"setting {s.name}: min/max outside {s.type} wire range")
        if not s.min <= s.default <= s.max:
            issues.append(f"setting {s.name}: default {s.default} outside [{s.min}, {s.max}]")
        ids.add(s.id)
        setting_names.add(s.name)

    for g in model.golden:
        if g.frame not in names:
            issues.append(f"golden: unknown frame {g.frame}")
    return issues
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd protocol && uv run pytest generator/tests -v`
Expected: all passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd protocol && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add protocol/generator/protogen/validator.py protocol/generator/tests/test_validator.py
git commit -m "feat(protocol): definition validator"
```

---

### Task 4: Generator encoder, CRC, golden check and test vectors (JSON)

**Files:**
- Create: `protocol/generator/protogen/naming.py`, `encoding.py`, `vectors.py`, `emit_vectors.py`
- Test: `protocol/generator/tests/test_encoding.py`

**Interfaces:**
- Produces:
  - `naming.pascal(name: str) -> str` (`"TELEM_FAST"` → `"TelemFast"`), `naming.k_name(name: str) -> str` (`"TELEM_FAST"` → `"kTelemFast"`).
  - `encoding.crc8(data: bytes, poly: int = 0x07, init: int = 0x00) -> int`, `encoding.encode_payload(frame: Frame, values: Mapping[str, Any]) -> bytes`, `encoding.build_frame(meta: Meta, frame: Frame, seq: int, values: Mapping[str, Any]) -> bytes`.
  - `vectors.Vector(frame: str, variant: str, seq: int, values: dict[str, Any], data: bytes)`, `vectors.make_vectors(model) -> list[Vector]` (typical + extremes per frame, then goldens with variant `"golden"`).
  - `emit_vectors.emit_vectors_json(model) -> str`.

- [ ] **Step 1: Write the failing tests**

`protocol/generator/tests/test_encoding.py`:
```python
from pathlib import Path

from protogen.encoding import build_frame, crc8
from protogen.loader import load
from protogen.naming import k_name, pascal
from protogen.vectors import make_vectors

DEFS = Path(__file__).resolve().parents[2] / "defs"


def test_crc8_check_value() -> None:
    assert crc8(b"123456789") == 0xF4


def test_naming() -> None:
    assert pascal("TELEM_FAST") == "TelemFast"
    assert k_name("TELEM_FAST") == "kTelemFast"


def test_golden_vectors_match_generator_encoder() -> None:
    model = load(DEFS)
    for g in model.golden:
        data = build_frame(model.meta, model.frame(g.frame), g.seq, g.values)
        assert data.hex(" ") == g.hex, g.frame


def test_vectors_cover_every_frame_twice() -> None:
    model = load(DEFS)
    vectors = make_vectors(model)
    for frame in model.frames:
        variants = {v.variant for v in vectors if v.frame == frame.name}
        assert {"typical", "extremes"} <= variants, frame.name


def test_extremes_hit_wire_limits() -> None:
    model = load(DEFS)
    vec = next(v for v in make_vectors(model) if v.frame == "MOVE" and v.variant == "extremes")
    assert vec.values["dist_mm"] == -0x80000000
    assert vec.values["src"] == 0xFF


def test_tone_extremes_use_max_count() -> None:
    model = load(DEFS)
    vec = next(v for v in make_vectors(model) if v.frame == "TONE" and v.variant == "extremes")
    assert vec.values["n"] == 10
    assert len(vec.values["notes"]) == 10
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd protocol && uv run pytest generator/tests/test_encoding.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protogen.encoding'`.

- [ ] **Step 3: Implement naming, encoding, vectors, JSON emitter**

`protocol/generator/protogen/naming.py`:
```python
"""Name conversions shared by emitters."""


def pascal(name: str) -> str:
    return "".join(part.capitalize() for part in name.lower().split("_"))


def k_name(name: str) -> str:
    return "k" + pascal(name)
```

`protocol/generator/protogen/encoding.py`:
```python
"""Generator-side encoder (independent of the runtime codecs it cross-checks)."""

from __future__ import annotations

import struct
from collections.abc import Mapping, Sequence
from typing import Any, cast

from .model import Frame, Meta

FMT: dict[str, str] = {"u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i"}


def crc8(data: bytes, poly: int = 0x07, init: int = 0x00) -> int:
    crc = init
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def encode_payload(frame: Frame, values: Mapping[str, Any]) -> bytes:
    out = bytearray()
    for f in frame.fields:
        value = values[f.name]
        if f.items is not None:
            fmt = "<" + "".join(FMT[i.type] for i in f.items)
            for item in cast(Sequence[Sequence[int]], value):
                out += struct.pack(fmt, *item)
        elif f.count is not None:
            assert f.type is not None
            out += struct.pack(f"<{f.count}{FMT[f.type]}", *cast(Sequence[int], value))
        else:
            assert f.type is not None
            out += struct.pack("<" + FMT[f.type], cast(int, value))
    return bytes(out)


def build_frame(meta: Meta, frame: Frame, seq: int, values: Mapping[str, Any]) -> bytes:
    payload = encode_payload(frame, values)
    head = bytes([len(payload), frame.type, seq & 0xFF])
    crc = crc8(head + payload, meta.crc_poly, meta.crc_init)
    return bytes([meta.sync]) + head + payload + bytes([crc])
```

`protocol/generator/protogen/vectors.py`:
```python
"""Deterministic test vectors: typical + extremes per frame, plus hand-written goldens."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .encoding import build_frame
from .model import WIRE_RANGE, Frame, ProtocolModel


@dataclass(frozen=True)
class Vector:
    frame: str
    variant: str
    seq: int
    values: dict[str, Any]
    data: bytes


def _scalar(wire: str, idx: int, variant: str) -> int:
    if variant == "typical":
        base = (idx + 1) * 3
        return -base if wire.startswith("i") and idx % 2 else base
    lo, hi = WIRE_RANGE[wire]
    return hi if idx % 2 == 0 else lo


def make_values(frame: Frame, variant: str) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for idx, f in enumerate(frame.fields):
        if f.items is not None:
            assert f.max_count is not None and f.count_field is not None
            n = min(2, f.max_count) if variant == "typical" else f.max_count
            values[f.name] = [
                [_scalar(it.type, idx * 10 + k + j, variant) for j, it in enumerate(f.items)]
                for k in range(n)
            ]
            values[f.count_field] = n
        elif f.count is not None:
            assert f.type is not None
            values[f.name] = [_scalar(f.type, idx * 10 + k, variant) for k in range(f.count)]
        else:
            assert f.type is not None
            values[f.name] = _scalar(f.type, idx, variant)
    return values


def make_vectors(model: ProtocolModel) -> list[Vector]:
    vectors: list[Vector] = []
    for i, frame in enumerate(model.frames):
        for j, variant in enumerate(("typical", "extremes")):
            seq = (2 * i + j) & 0xFF
            values = make_values(frame, variant)
            data = build_frame(model.meta, frame, seq, values)
            vectors.append(Vector(frame.name, variant, seq, values, data))
    for g in model.golden:
        data = build_frame(model.meta, model.frame(g.frame), g.seq, g.values)
        vectors.append(Vector(g.frame, "golden", g.seq, dict(g.values), data))
    return vectors
```

`protocol/generator/protogen/emit_vectors.py`:
```python
"""Vector outputs: JSON (for Python/TS tests)."""

from __future__ import annotations

import json

from .model import ProtocolModel
from .vectors import make_vectors

HEADER_NOTE = "DO NOT EDIT - generated by protogen from protocol/defs"


def emit_vectors_json(model: ProtocolModel) -> str:
    items = [
        {
            "frame": v.frame,
            "variant": v.variant,
            "seq": v.seq,
            "values": v.values,
            "hex": v.data.hex(" "),
        }
        for v in make_vectors(model)
    ]
    doc = {"_note": HEADER_NOTE, "protocol_version": model.meta.protocol_version, "vectors": items}
    return json.dumps(doc, indent=2) + "\n"
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd protocol && uv run pytest generator/tests -v`
Expected: all passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd protocol && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add protocol/generator
git commit -m "feat(protocol): generator encoder, CRC-8, test vectors"
```

---

### Task 5: Python runtime package (`conqueror_protocol`) and its emitter

**Files:**
- Create: `protocol/python/conqueror_protocol/wire.py`, `framing.py`; `protocol/generator/protogen/emit_python.py`; generated `protocol/python/conqueror_protocol/generated.py`
- Modify: `protocol/python/conqueror_protocol/__init__.py`
- Test: `protocol/generator/tests/test_python_runtime.py`

**Interfaces:**
- Consumes: `make_vectors`, `pascal`.
- Produces (used by Plan D bench tool):
  - `conqueror_protocol.wire`: `FieldSpec`, `Message` (`.encode() -> bytes`, `Message.decode(payload: bytes) -> Self`, ClassVars `TYPE, DIR, ACK, STREAMING, FIELDS`), `ErrorInfo(number, code, reason, http, message)`, `SettingInfo(id, name, scope, wire, unit, scale, min, max, default)`.
  - `conqueror_protocol.generated`: `PROTOCOL_VERSION, SYNC, MAX_PAYLOAD, BAUD`, one `IntEnum` per enum (`Source`, `Owner`, …, `EventKind`, `SettingId`), one dataclass per frame named `pascal(name)` (e.g. `Turn`, `TelemFast`, `HelloAck`), `REGISTRY: dict[tuple[str, int], type[Message]]`, `ERRORS: dict[str, ErrorInfo]`, `ACK_STATUS: tuple[str, ...]`, `SETTINGS: tuple[SettingInfo, ...]`.
  - `conqueror_protocol.framing`: `crc8(data: bytes) -> int`, `build_frame(msg: Message, seq: int) -> bytes`, `RawFrame(type, seq, payload)`, `ParserStats(frames, bad_crc, bad_len)`, `FrameParser().feed(data: bytes) -> list[RawFrame]`, `decode_frame(raw: RawFrame, direction: str) -> Message`, `UnknownFrameError`.
  - `protogen.emit_python.emit_python(model) -> str`.

- [ ] **Step 1: Write the runtime (hand-written part)**

`protocol/python/conqueror_protocol/wire.py`:
```python
"""Hand-written runtime used by the generated message classes."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, ClassVar, Self

FMT: dict[str, str] = {"u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i"}


@dataclass(frozen=True, slots=True)
class FieldSpec:
    name: str
    wire: str | None
    count: int | None = None
    items: tuple[tuple[str, str], ...] | None = None
    count_field: str | None = None
    max_count: int | None = None


@dataclass(frozen=True, slots=True)
class ErrorInfo:
    number: int
    code: str
    reason: str
    http: int
    message: str


@dataclass(frozen=True, slots=True)
class SettingInfo:
    id: int
    name: str
    scope: str
    wire: str
    unit: str
    scale: float
    min: int
    max: int
    default: int


class Message:
    """Base for generated frame dataclasses."""

    __slots__ = ()
    TYPE: ClassVar[int]
    DIR: ClassVar[str]
    ACK: ClassVar[bool]
    STREAMING: ClassVar[bool]
    FIELDS: ClassVar[tuple[FieldSpec, ...]]

    if TYPE_CHECKING:

        def __init__(self, **kwargs: Any) -> None: ...

    def encode(self) -> bytes:
        out = bytearray()
        for spec in self.FIELDS:
            value: Any = getattr(self, spec.name)
            if spec.items is not None:
                assert spec.max_count is not None
                if len(value) > spec.max_count:
                    raise ValueError(f"{type(self).__name__}.{spec.name}: too many items")
                fmt = "<" + "".join(FMT[w] for _, w in spec.items)
                for item in value:
                    out += struct.pack(fmt, *item)
            elif spec.count is not None:
                assert spec.wire is not None
                out += struct.pack(f"<{spec.count}{FMT[spec.wire]}", *value)
            else:
                assert spec.wire is not None
                out += struct.pack("<" + FMT[spec.wire], value)
        return bytes(out)

    @classmethod
    def decode(cls, payload: bytes) -> Self:
        values: dict[str, Any] = {}
        pos = 0
        try:
            for spec in cls.FIELDS:
                if spec.items is not None:
                    assert spec.count_field is not None and spec.max_count is not None
                    n = int(values[spec.count_field])
                    if n > spec.max_count:
                        raise ValueError(f"{cls.__name__}.{spec.name}: {n} > {spec.max_count}")
                    fmt = "<" + "".join(FMT[w] for _, w in spec.items)
                    size = struct.calcsize(fmt)
                    items: list[tuple[int, ...]] = []
                    for _ in range(n):
                        items.append(struct.unpack_from(fmt, payload, pos))
                        pos += size
                    values[spec.name] = tuple(items)
                elif spec.count is not None:
                    assert spec.wire is not None
                    fmt = f"<{spec.count}{FMT[spec.wire]}"
                    values[spec.name] = struct.unpack_from(fmt, payload, pos)
                    pos += struct.calcsize(fmt)
                else:
                    assert spec.wire is not None
                    fmt = "<" + FMT[spec.wire]
                    (values[spec.name],) = struct.unpack_from(fmt, payload, pos)
                    pos += struct.calcsize(fmt)
        except struct.error as exc:
            raise ValueError(f"{cls.__name__}: payload too short") from exc
        if pos != len(payload):
            raise ValueError(f"{cls.__name__}: {len(payload) - pos} unexpected trailing bytes")
        return cls(**values)
```

`protocol/python/conqueror_protocol/framing.py`:
```python
"""Frame building and streaming parsing: 0xAA | len | type | seq | payload | crc8."""

from __future__ import annotations

from dataclasses import dataclass

from .generated import MAX_PAYLOAD, REGISTRY, SYNC
from .wire import Message


class UnknownFrameError(ValueError):
    pass


def crc8(data: bytes) -> int:
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build_frame(msg: Message, seq: int) -> bytes:
    payload = msg.encode()
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} > {MAX_PAYLOAD}")
    head = bytes([len(payload), msg.TYPE, seq & 0xFF])
    return bytes([SYNC]) + head + payload + bytes([crc8(head + payload)])


@dataclass(frozen=True, slots=True)
class RawFrame:
    type: int
    seq: int
    payload: bytes


@dataclass(slots=True)
class ParserStats:
    frames: int = 0
    bad_crc: int = 0
    bad_len: int = 0


class FrameParser:
    """Byte-at-a-time state machine (same states as the Uno parser)."""

    def __init__(self) -> None:
        self._state = "sync"
        self._len = 0
        self._type = 0
        self._seq = 0
        self._buf = bytearray()
        self.stats = ParserStats()

    def feed(self, data: bytes) -> list[RawFrame]:
        frames: list[RawFrame] = []
        for byte in data:
            frame = self._feed_byte(byte)
            if frame is not None:
                frames.append(frame)
        return frames

    def _feed_byte(self, b: int) -> RawFrame | None:
        if self._state == "sync":
            if b == SYNC:
                self._state = "len"
            return None
        if self._state == "len":
            if b > MAX_PAYLOAD:
                self.stats.bad_len += 1
                self._state = "sync"
                return None
            self._len = b
            self._state = "type"
            return None
        if self._state == "type":
            self._type = b
            self._state = "seq"
            return None
        if self._state == "seq":
            self._seq = b
            self._buf.clear()
            self._state = "payload" if self._len else "crc"
            return None
        if self._state == "payload":
            self._buf.append(b)
            if len(self._buf) == self._len:
                self._state = "crc"
            return None
        self._state = "sync"
        expected = crc8(bytes([self._len, self._type, self._seq]) + bytes(self._buf))
        if b != expected:
            self.stats.bad_crc += 1
            return None
        self.stats.frames += 1
        return RawFrame(self._type, self._seq, bytes(self._buf))


def decode_frame(raw: RawFrame, direction: str) -> Message:
    cls = REGISTRY.get((direction, raw.type))
    if cls is None:
        raise UnknownFrameError(f"unknown {direction} frame type 0x{raw.type:02X}")
    return cls.decode(raw.payload)
```

`protocol/python/conqueror_protocol/__init__.py`:
```python
"""Conqueror body-link protocol runtime."""

from . import generated
from .framing import (
    FrameParser,
    ParserStats,
    RawFrame,
    UnknownFrameError,
    build_frame,
    crc8,
    decode_frame,
)
from .wire import ErrorInfo, FieldSpec, Message, SettingInfo

__all__ = [
    "ErrorInfo",
    "FieldSpec",
    "FrameParser",
    "Message",
    "ParserStats",
    "RawFrame",
    "SettingInfo",
    "UnknownFrameError",
    "build_frame",
    "crc8",
    "decode_frame",
    "generated",
]
```

- [ ] **Step 2: Write the Python emitter**

`protocol/generator/protogen/emit_python.py`:
```python
"""Emit protocol/python/conqueror_protocol/generated.py."""

from __future__ import annotations

from .model import EnumDef, Field, Frame, ProtocolModel
from .naming import pascal

HEADER = "# DO NOT EDIT - generated by protogen from protocol/defs\n"


def _py_type(f: Field) -> str:
    if f.items is not None:
        return "tuple[tuple[int, ...], ...]"
    if f.count is not None:
        return "tuple[int, ...]"
    return "int"


def _py_default(f: Field) -> str:
    if f.items is not None:
        return "()"
    if f.count is not None:
        return "(" + ", ".join(["0"] * f.count) + ",)"
    return "0"


def _spec(f: Field) -> str:
    if f.items is not None:
        items = ", ".join(f'("{i.name}", "{i.type}")' for i in f.items)
        return (
            f'FieldSpec("{f.name}", None, items=({items},), '
            f'count_field="{f.count_field}", max_count={f.max_count})'
        )
    if f.count is not None:
        return f'FieldSpec("{f.name}", "{f.type}", count={f.count})'
    return f'FieldSpec("{f.name}", "{f.type}")'


def _enum(e: EnumDef) -> list[str]:
    lines = ["", "", f"class {e.name}(IntEnum):"]
    if e.doc:
        lines.append(f'    """{e.doc}"""')
        lines.append("")
    lines += [f"    {name} = {value}" for name, value in e.values.items()]
    return lines


def _frame(fr: Frame) -> list[str]:
    specs = ", ".join(_spec(f) for f in fr.fields)
    if fr.fields:
        specs += ","
    lines = [
        "",
        "",
        "@dataclass(slots=True)",
        f"class {pascal(fr.name)}(Message):",
        f'    """{fr.doc or fr.name}"""',
        "",
        f"    TYPE: ClassVar[int] = 0x{fr.type:02X}",
        f'    DIR: ClassVar[str] = "{fr.dir}"',
        f"    ACK: ClassVar[bool] = {fr.ack}",
        f"    STREAMING: ClassVar[bool] = {fr.streaming}",
        f"    FIELDS: ClassVar[tuple[FieldSpec, ...]] = ({specs})",
    ]
    lines += [f"    {f.name}: {_py_type(f)} = {_py_default(f)}" for f in fr.fields]
    return lines


def emit_python(model: ProtocolModel) -> str:
    m = model.meta
    lines = [
        HEADER.rstrip("\n"),
        '"""Generated protocol definitions."""',
        "",
        "from __future__ import annotations",
        "",
        "from dataclasses import dataclass",
        "from enum import IntEnum",
        "from typing import ClassVar",
        "",
        "from .wire import ErrorInfo, FieldSpec, Message, SettingInfo",
        "",
        f"PROTOCOL_VERSION = {m.protocol_version}",
        f"SYNC = 0x{m.sync:02X}",
        f"MAX_PAYLOAD = {m.max_payload}",
        f"BAUD = {m.baud}",
    ]
    for e in model.enums:
        lines += _enum(e)
    lines += _enum(EnumDef(name="EventKind", values={ev.name: ev.kind for ev in model.events}))
    lines += _enum(EnumDef(name="SettingId", values={s.name.upper(): s.id for s in model.settings}))
    for fr in model.frames:
        lines += _frame(fr)
    lines += ["", "", "REGISTRY: dict[tuple[str, int], type[Message]] = {"]
    lines += [f'    ("{fr.dir}", 0x{fr.type:02X}): {pascal(fr.name)},' for fr in model.frames]
    lines += ["}", "", "ERRORS: dict[str, ErrorInfo] = {"]
    for d in model.errors:
        for e in d.codes:
            lines.append(
                f'    "{e.reason}": ErrorInfo({e.code}, "{d.prefix}_{e.code}", "{e.reason}", '
                f'{e.http}, "{e.message}"),'
            )
    lines += ["}", "", "ACK_STATUS: tuple[str, ...] = ("]
    lines += [f'    "{r}",' for r in model.ack_status]
    lines += [")", "", "SETTINGS: tuple[SettingInfo, ...] = ("]
    for s in model.settings:
        lines.append(
            f'    SettingInfo({s.id}, "{s.name}", "{s.scope}", "{s.type}", "{s.unit}", '
            f"{s.scale}, {s.min}, {s.max}, {s.default}),"
        )
    lines += [")", ""]
    return "\n".join(lines)
```

- [ ] **Step 3: Generate the Python module once (temporary bootstrap)**

Run:
```bash
cd protocol && uv run python -c "
from pathlib import Path
from protogen.loader import load
from protogen.emit_python import emit_python
Path('python/conqueror_protocol/generated.py').write_text(emit_python(load(Path('defs'))))
"
```
Expected: file `protocol/python/conqueror_protocol/generated.py` created (later produced by `protogen write`, Task 7).

- [ ] **Step 4: Write the runtime tests**

`protocol/generator/tests/test_python_runtime.py`:
```python
from pathlib import Path
from typing import Any

import pytest

from conqueror_protocol import FrameParser, build_frame, decode_frame, generated
from conqueror_protocol.framing import crc8
from protogen.loader import load
from protogen.naming import pascal
from protogen.vectors import make_vectors

DEFS = Path(__file__).resolve().parents[2] / "defs"
MODEL = load(DEFS)


def _norm(v: Any) -> Any:
    if isinstance(v, (list, tuple)):
        return [_norm(x) for x in v]  # pyright: ignore[reportUnknownVariableType]
    return v


@pytest.mark.parametrize("vec", make_vectors(MODEL), ids=lambda v: f"{v.frame}-{v.variant}")
def test_runtime_matches_vector(vec: Any) -> None:
    frame_def = MODEL.frame(vec.frame)
    parser = FrameParser()
    [raw] = parser.feed(vec.data)
    msg = decode_frame(raw, frame_def.dir)
    assert type(msg).__name__ == pascal(vec.frame)
    for name, expected in vec.values.items():
        assert _norm(getattr(msg, name)) == _norm(expected), name
    assert build_frame(msg, raw.seq) == vec.data


def test_crc_check_value() -> None:
    assert crc8(b"123456789") == 0xF4


def test_parser_split_and_noise() -> None:
    data = bytes.fromhex(MODEL.golden[2].hex.replace(" ", ""))
    parser = FrameParser()
    frames = []
    for b in b"\x00\x13\xff" + data:
        frames += parser.feed(bytes([b]))
    assert len(frames) == 1
    assert frames[0].type == 0x13


def test_parser_bad_crc_then_good() -> None:
    good = bytes.fromhex(MODEL.golden[1].hex.replace(" ", ""))
    bad = bytearray(good)
    bad[-1] ^= 0xFF
    parser = FrameParser()
    frames = parser.feed(bytes(bad) + good)
    assert parser.stats.bad_crc == 1
    assert [f.type for f in frames] == [0x12]


def test_parser_bad_len() -> None:
    parser = FrameParser()
    assert parser.feed(bytes([0xAA, 49, 0x10, 0])) == []
    assert parser.stats.bad_len == 1


def test_tone_rejects_bad_count() -> None:
    with pytest.raises(ValueError):
        generated.Tone.decode(bytes([11]) + bytes(44))
    with pytest.raises(ValueError):
        generated.Tone.decode(bytes([2]) + bytes(4))


def test_short_and_long_payloads_rejected() -> None:
    with pytest.raises(ValueError):
        generated.Turn.decode(bytes(6))
    with pytest.raises(ValueError):
        generated.Turn.decode(bytes(8))


def test_registry_and_tables() -> None:
    assert generated.REGISTRY[("esp32_to_uno", 0x13)] is generated.Turn
    assert generated.ACK_STATUS[1] == "NOT_OWNER"
    assert generated.ERRORS["NOT_OWNER"].code == "COMMAND_3001"
    assert generated.SettingId.DEADMAN == 15
    assert generated.BAUD == 250000
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cd protocol && uv run pytest generator/tests -v`
Expected: all passed (≈ 90 tests incl. one per vector).

- [ ] **Step 6: Lint, type-check, commit**

Run: `cd protocol && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: clean (generated.py excluded from ruff, included in pyright).
```bash
git add protocol/python protocol/generator
git commit -m "feat(protocol): Python runtime package and emitter, cross-checked against vectors"
```

---

### Task 6: Uno C++ header emitter and host C++ vector test

**Files:**
- Create: `protocol/generator/protogen/emit_cpp.py`; extend `protocol/generator/protogen/emit_vectors.py` with `emit_vectors_cpp`; generated `firmware/uno/src/generated/cq_protocol.h` and `protocol/tests/cpp/generated_vectors_test.cpp`
- Test: `protocol/generator/tests/test_cpp_vectors.py`

**Interfaces:**
- Consumes: `make_vectors`, `pascal`, `k_name`.
- Produces (used by Plan B Uno firmware), namespace `cq::proto`:
  - constants `kProtocolVersion, kSync, kMaxPayload, kBaud, kEncodeError (0xFF)`;
  - helpers `put_u8/put_u16/put_u32(uint8_t*& p, …)`, `get_u8/get_u16/get_u32(const uint8_t*& p)`, `crc8(const uint8_t* data, uint8_t len, uint8_t crc = 0x00)`;
  - `enum class FrameType : uint8_t { kHeartbeat = 0x01, … }`; one `enum class <EnumName> : uint8_t` per enum (members `kName`), plus `EventKind`, `SettingId`, `AckStatus`;
  - one struct `<Pascal>Msg` per frame with `static constexpr FrameType kType; static constexpr uint8_t kFixedSize, kMaxSize;` and fields named as in YAML; struct-array item struct `<Pascal><PascalField>Item`;
  - `uint8_t encode(const XMsg& m, uint8_t* out)` (returns payload length or `kEncodeError`), `bool decode(const uint8_t* in, uint8_t len, XMsg& m)`;
  - `struct SettingDescriptor { uint8_t id; SettingScope scope; WireType type; int32_t min; int32_t max; int32_t def; }`, `inline const SettingDescriptor kSettingDescriptors[] PROGMEM`, `constexpr uint8_t kSettingCount`, `inline const uint16_t kAckStatusCode[] PROGMEM`.
  - `protogen.emit_cpp.emit_cpp(model) -> str`, `protogen.emit_vectors.emit_vectors_cpp(model) -> str`.

- [ ] **Step 1: Write the failing C++ test driver**

`protocol/generator/tests/test_cpp_vectors.py`:
```python
import shutil
import subprocess
from pathlib import Path

import pytest

from protogen.emit_cpp import emit_cpp
from protogen.emit_vectors import emit_vectors_cpp
from protogen.loader import load

ROOT = Path(__file__).resolve().parents[3]
DEFS = ROOT / "protocol" / "defs"
CXX = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")


@pytest.mark.skipif(CXX is None, reason="no C++ compiler")
def test_cpp_codec_matches_all_vectors(tmp_path: Path) -> None:
    model = load(DEFS)
    inc = tmp_path / "inc"
    inc.mkdir()
    (inc / "cq_protocol.h").write_text(emit_cpp(model))
    src = tmp_path / "vectors_test.cpp"
    src.write_text(emit_vectors_cpp(model))
    exe = tmp_path / "vectors_test"
    assert CXX is not None
    subprocess.run(
        [CXX, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", f"-I{inc}", str(src),
         "-o", str(exe)],
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert ", 0 failed" in result.stdout


@pytest.mark.skipif(CXX is None, reason="no C++ compiler")
def test_cpp_tone_rejects_overlong(tmp_path: Path) -> None:
    model = load(DEFS)
    (tmp_path / "cq_protocol.h").write_text(emit_cpp(model))
    src = tmp_path / "tone.cpp"
    src.write_text(
        '#include "cq_protocol.h"\n'
        "using namespace cq::proto;\n"
        "int main() {\n"
        "  uint8_t bad_count[1 + 44] = {11};\n"
        "  ToneMsg m{};\n"
        "  if (decode(bad_count, sizeof bad_count, m)) { return 1; }\n"
        "  uint8_t bad_len[1 + 4] = {2};\n"
        "  if (decode(bad_len, sizeof bad_len, m)) { return 2; }\n"
        "  m.n = 11;\n"
        "  uint8_t out[kMaxPayload];\n"
        "  if (encode(m, out) != kEncodeError) { return 3; }\n"
        "  return 0;\n"
        "}\n"
    )
    exe = tmp_path / "tone"
    assert CXX is not None
    subprocess.run([CXX, "-std=c++17", "-Wall", "-Wextra", "-Werror", f"-I{tmp_path}", str(src),
                    "-o", str(exe)], check=True)
    assert subprocess.run([str(exe)]).returncode == 0
```

- [ ] **Step 2: Run it to see it fail**

Run: `cd protocol && uv run pytest generator/tests/test_cpp_vectors.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protogen.emit_cpp'`.

- [ ] **Step 3: Implement the C++ emitter**

`protocol/generator/protogen/emit_cpp.py`:
```python
"""Emit firmware/uno/src/generated/cq_protocol.h (header-only, AVR-friendly)."""

from __future__ import annotations

from .model import EnumDef, Field, Frame, ProtocolModel
from .naming import k_name, pascal

CTYPE: dict[str, str] = {
    "u8": "uint8_t", "i8": "int8_t", "u16": "uint16_t",
    "i16": "int16_t", "u32": "uint32_t", "i32": "int32_t",
}
UTYPE: dict[str, str] = {
    "u8": "uint8_t", "i8": "uint8_t", "u16": "uint16_t",
    "i16": "uint16_t", "u32": "uint32_t", "i32": "uint32_t",
}
SUFFIX: dict[str, str] = {"u8": "u8", "i8": "u8", "u16": "u16", "i16": "u16", "u32": "u32", "i32": "u32"}
WIRE_ENUM: dict[str, str] = {
    "u8": "kU8", "i8": "kI8", "u16": "kU16", "i16": "kI16", "u32": "kU32", "i32": "kI32",
}


def cpp_literal(value: int, wire: str) -> str:
    if wire == "i32" and value == -0x80000000:
        return "(-2147483647L - 1)"
    if wire == "u32":
        return f"{value}UL"
    if wire == "i32":
        return f"{value}L"
    return str(value)


def _enum(e: EnumDef) -> list[str]:
    lines = [f"enum class {e.name} : uint8_t {{"]
    lines += [f"  {k_name(k)} = {v}," for k, v in e.values.items()]
    lines += ["};", ""]
    return lines


def _item_struct(fr: Frame, f: Field) -> list[str]:
    assert f.items is not None
    name = f"{pascal(fr.name)}{pascal(f.name)}Item"
    lines = [f"struct {name} {{"]
    lines += [f"  {CTYPE[i.type]} {i.name};" for i in f.items]
    lines += ["};", ""]
    return lines


def _struct(fr: Frame) -> list[str]:
    cls = pascal(fr.name) + "Msg"
    lines: list[str] = []
    for f in fr.fields:
        if f.items is not None:
            lines += _item_struct(fr, f)
    lines += [
        f"struct {cls} {{",
        f"  static constexpr FrameType kType = FrameType::{k_name(fr.name)};",
        f"  static constexpr uint8_t kFixedSize = {fr.fixed_size()};",
        f"  static constexpr uint8_t kMaxSize = {fr.max_payload()};",
    ]
    for f in fr.fields:
        if f.items is not None:
            lines.append(f"  {pascal(fr.name)}{pascal(f.name)}Item {f.name}[{f.max_count}];")
        elif f.count is not None:
            assert f.type is not None
            lines.append(f"  {CTYPE[f.type]} {f.name}[{f.count}];")
        else:
            assert f.type is not None
            lines.append(f"  {CTYPE[f.type]} {f.name};")
    lines += ["};", ""]
    return lines


def _put(wire: str, expr: str) -> str:
    return f"put_{SUFFIX[wire]}(p, static_cast<{UTYPE[wire]}>({expr}));"


def _get(wire: str, target: str) -> str:
    return f"{target} = static_cast<{CTYPE[wire]}>(get_{SUFFIX[wire]}(p));"


def _encode(fr: Frame) -> list[str]:
    cls = pascal(fr.name) + "Msg"
    if not fr.fields:
        return [
            f"inline uint8_t encode(const {cls}& m, uint8_t* out) {{",
            "  (void)m;",
            "  (void)out;",
            "  return 0;",
            "}",
            "",
        ]
    lines = [f"inline uint8_t encode(const {cls}& m, uint8_t* out) {{"]
    for f in fr.fields:
        if f.items is not None:
            lines.append(f"  if (m.{f.count_field} > {f.max_count}) {{ return kEncodeError; }}")
    lines.append("  uint8_t* p = out;")
    for f in fr.fields:
        if f.items is not None:
            lines.append(f"  for (uint8_t i = 0; i < m.{f.count_field}; ++i) {{")
            lines += [f"    {_put(it.type, f'm.{f.name}[i].{it.name}')}" for it in f.items]
            lines.append("  }")
        elif f.count is not None:
            assert f.type is not None
            lines.append(f"  for (uint8_t i = 0; i < {f.count}; ++i) {{")
            lines.append(f"    {_put(f.type, f'm.{f.name}[i]')}")
            lines.append("  }")
        else:
            assert f.type is not None
            lines.append(f"  {_put(f.type, f'm.{f.name}')}")
    lines += ["  return static_cast<uint8_t>(p - out);", "}", ""]
    return lines


def _decode(fr: Frame) -> list[str]:
    cls = pascal(fr.name) + "Msg"
    if not fr.fields:
        return [
            f"inline bool decode(const uint8_t* in, uint8_t len, {cls}& m) {{",
            "  (void)in;",
            "  (void)m;",
            "  return len == 0;",
            "}",
            "",
        ]
    variable = any(f.items is not None for f in fr.fields)
    check = f"len < {cls}::kFixedSize" if variable else f"len != {cls}::kFixedSize"
    lines = [
        f"inline bool decode(const uint8_t* in, uint8_t len, {cls}& m) {{",
        f"  if ({check}) {{ return false; }}",
        "  const uint8_t* p = in;",
    ]
    for f in fr.fields:
        if f.items is not None:
            assert f.max_count is not None
            size = f.item_size()
            lines.append(
                f"  if (m.{f.count_field} > {f.max_count} || "
                f"len != {cls}::kFixedSize + m.{f.count_field} * {size}) {{ return false; }}"
            )
            lines.append(f"  for (uint8_t i = 0; i < m.{f.count_field}; ++i) {{")
            lines += [f"    {_get(it.type, f'm.{f.name}[i].{it.name}')}" for it in f.items]
            lines.append("  }")
        elif f.count is not None:
            assert f.type is not None
            lines.append(f"  for (uint8_t i = 0; i < {f.count}; ++i) {{")
            lines.append(f"    {_get(f.type, f'm.{f.name}[i]')}")
            lines.append("  }")
        else:
            assert f.type is not None
            lines.append(f"  {_get(f.type, f'm.{f.name}')}")
    lines += ["  return true;", "}", ""]
    return lines


RUNTIME = """\
inline void put_u8(uint8_t*& p, uint8_t v) { *p++ = v; }
inline void put_u16(uint8_t*& p, uint16_t v) {
  *p++ = static_cast<uint8_t>(v);
  *p++ = static_cast<uint8_t>(v >> 8);
}
inline void put_u32(uint8_t*& p, uint32_t v) {
  put_u16(p, static_cast<uint16_t>(v));
  put_u16(p, static_cast<uint16_t>(v >> 16));
}
inline uint8_t get_u8(const uint8_t*& p) { return *p++; }
inline uint16_t get_u16(const uint8_t*& p) {
  const uint16_t v = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
  p += 2;
  return v;
}
inline uint32_t get_u32(const uint8_t*& p) {
  const uint32_t lo = get_u16(p);
  const uint32_t hi = get_u16(p);
  return lo | (hi << 16);
}
inline uint8_t crc8(const uint8_t* data, uint8_t len, uint8_t crc = 0x00) {
  for (uint8_t i = 0; i < len; ++i) {
    crc = static_cast<uint8_t>(crc ^ data[i]);
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}
"""


def emit_cpp(model: ProtocolModel) -> str:
    m = model.meta
    lines = [
        "// DO NOT EDIT - generated by protogen from protocol/defs",
        "#pragma once",
        "#include <stdint.h>",
        "#ifdef __AVR__",
        "#include <avr/pgmspace.h>",
        "#else",
        "#ifndef PROGMEM",
        "#define PROGMEM",
        "#endif",
        "#endif",
        "",
        "namespace cq {",
        "namespace proto {",
        "",
        f"constexpr uint8_t kProtocolVersion = {m.protocol_version};",
        f"constexpr uint8_t kSync = 0x{m.sync:02X};",
        f"constexpr uint8_t kMaxPayload = {m.max_payload};",
        f"constexpr uint32_t kBaud = {m.baud}UL;",
        "constexpr uint8_t kEncodeError = 0xFF;",
        "",
        RUNTIME,
        "enum class FrameType : uint8_t {",
    ]
    lines += [f"  {k_name(fr.name)} = 0x{fr.type:02X}," for fr in model.frames]
    lines += ["};", ""]
    for e in model.enums:
        lines += _enum(e)
    lines += _enum(EnumDef(name="EventKind", values={ev.name: ev.kind for ev in model.events}))
    lines += _enum(EnumDef(name="SettingId", values={s.name.upper(): s.id for s in model.settings}))
    lines += _enum(EnumDef(name="AckStatus", values={r: i for i, r in enumerate(model.ack_status)}))
    for fr in model.frames:
        lines += _struct(fr)
        lines += _encode(fr)
        lines += _decode(fr)
    codes = {e.reason: e.code for d in model.errors for e in d.codes}
    ack_codes = ", ".join(str(codes.get(r, 0)) for r in model.ack_status)
    lines += [
        "enum class SettingScope : uint8_t { kGlobal = 0, kProfile = 1 };",
        "enum class WireType : uint8_t { kU8, kI8, kU16, kI16, kU32, kI32 };",
        "struct SettingDescriptor {",
        "  uint8_t id;",
        "  SettingScope scope;",
        "  WireType type;",
        "  int32_t min;",
        "  int32_t max;",
        "  int32_t def;",
        "};",
        "inline const SettingDescriptor kSettingDescriptors[] PROGMEM = {",
    ]
    for s in model.settings:
        scope = "SettingScope::kProfile" if s.scope == "profile" else "SettingScope::kGlobal"
        lines.append(
            f"  {{{s.id}, {scope}, WireType::{WIRE_ENUM[s.type]}, {cpp_literal(s.min, 'i32')}, "
            f"{cpp_literal(s.max, 'i32')}, {cpp_literal(s.default, 'i32')}}},"
        )
    lines += [
        "};",
        f"constexpr uint8_t kSettingCount = {len(model.settings)};",
        "",
        f"inline const uint16_t kAckStatusCode[] PROGMEM = {{{ack_codes}}};",
        "",
        "}  // namespace proto",
        "}  // namespace cq",
        "",
    ]
    return "\n".join(lines)
```

- [ ] **Step 4: Implement the C++ vector test emitter**

Append to `protocol/generator/protogen/emit_vectors.py`:
```python
from typing import Any

from .emit_cpp import CTYPE, cpp_literal
from .naming import pascal


def _checks(model: ProtocolModel, frame_name: str, values: dict[str, Any]) -> list[str]:
    frame = model.frame(frame_name)
    lines: list[str] = []
    for f in frame.fields:
        value = values[f.name]
        if f.items is not None:
            for k, item in enumerate(value):
                for j, it in enumerate(f.items):
                    lit = cpp_literal(int(item[j]), it.type)
                    lines.append(
                        f"  if (m.{f.name}[{k}].{it.name} != static_cast<{CTYPE[it.type]}>({lit})) "
                        "{ return false; }"
                    )
        elif f.count is not None:
            assert f.type is not None
            for k, element in enumerate(value):
                lit = cpp_literal(int(element), f.type)
                lines.append(
                    f"  if (m.{f.name}[{k}] != static_cast<{CTYPE[f.type]}>({lit})) {{ return false; }}"
                )
        else:
            assert f.type is not None
            lit = cpp_literal(int(value), f.type)
            lines.append(f"  if (m.{f.name} != static_cast<{CTYPE[f.type]}>({lit})) {{ return false; }}")
    return lines


def emit_vectors_cpp(model: ProtocolModel) -> str:
    lines = [
        "// DO NOT EDIT - generated by protogen from protocol/defs",
        "#include <stdio.h>",
        "#include <string.h>",
        '#include "cq_protocol.h"',
        "",
        "using namespace cq::proto;",
        "",
    ]
    names: list[str] = []
    for v in make_vectors(model):
        fn = f"vec_{v.frame.lower()}_{v.variant}_{v.seq}"
        names.append(fn)
        cls = pascal(v.frame) + "Msg"
        data = ", ".join(f"0x{b:02X}" for b in v.data)
        lines += [
            f"static bool {fn}() {{",
            f"  static const uint8_t frame[] = {{{data}}};",
            "  const uint8_t len = frame[1];",
            f"  if (frame[0] != kSync || frame[2] != static_cast<uint8_t>({cls}::kType)) {{ return false; }}",
            "  if (crc8(frame + 1, static_cast<uint8_t>(len + 3)) != frame[4 + len]) { return false; }",
            f"  {cls} m{{}};",
            "  if (!decode(frame + 4, len, m)) { return false; }",
        ]
        lines += _checks(model, v.frame, v.values)
        lines += [
            "  uint8_t buf[kMaxPayload];",
            "  const uint8_t n = encode(m, buf);",
            "  if (n != len || memcmp(buf, frame + 4, len) != 0) { return false; }",
            "  return true;",
            "}",
            "",
        ]
    lines += [
        "struct Case { const char* name; bool (*fn)(); };",
        "static const Case kCases[] = {",
    ]
    lines += [f'  {{"{n}", {n}}},' for n in names]
    lines += [
        "};",
        "",
        "int main() {",
        "  int failed = 0;",
        "  const int total = static_cast<int>(sizeof kCases / sizeof kCases[0]);",
        "  for (int i = 0; i < total; ++i) {",
        "    if (!kCases[i].fn()) { printf(\"FAIL %s\\n\", kCases[i].name); ++failed; }",
        "  }",
        "  printf(\"%d vectors, %d failed\\n\", total, failed);",
        "  return failed ? 1 : 0;",
        "}",
        "",
    ]
    return "\n".join(lines)
```
Move the new imports to the top of `emit_vectors.py` (ruff `I` will flag their position).

- [ ] **Step 5: Run the tests to see them pass**

Run: `cd protocol && uv run pytest generator/tests -v`
Expected: all passed, including `test_cpp_codec_matches_all_vectors` (stdout `79 vectors, 0 failed` — 37 frames × 2 + 5 goldens) and `test_cpp_tone_rejects_overlong`.

- [ ] **Step 6: Lint, type-check, commit**

Run: `cd protocol && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add protocol/generator
git commit -m "feat(protocol): AVR-friendly C++ header emitter and host C++ vector test"
```

---

### Task 7: Docs emitter, `protogen write` / `protogen check`, staleness hook

**Files:**
- Create: `protocol/generator/protogen/emit_docs.py`, `protocol/generator/protogen/cli.py`, `scripts/check-protocol.sh`, `.githooks/pre-commit`
- Generated (committed): `protocol/python/conqueror_protocol/generated.py`, `firmware/uno/src/generated/cq_protocol.h`, `protocol/vectors/frames.json`, `protocol/tests/cpp/generated_vectors_test.cpp`, `docs/protocol/reference.md`
- Modify: `docs/lld/protocol-generator-lld.md` (record emitter-function deviation)
- Test: `protocol/generator/tests/test_cli.py`

**Interfaces:**
- Produces: `protogen.cli.OUTPUTS: dict[str, Callable[[ProtocolModel], str]]`, `render_all(model) -> dict[str, str]`, `cmd_write(root: Path) -> int`, `cmd_check(root: Path) -> int`, `main(argv: list[str] | None = None) -> int`; console script `protogen write|check|diff`; `protogen.emit_docs.emit_docs(model) -> str`.

- [ ] **Step 1: Write the failing CLI tests**

`protocol/generator/tests/test_cli.py`:
```python
import shutil
from pathlib import Path

from protogen.cli import OUTPUTS, cmd_check, cmd_write

ROOT = Path(__file__).resolve().parents[3]


def _tmp_root(tmp_path: Path) -> Path:
    shutil.copytree(ROOT / "protocol" / "defs", tmp_path / "protocol" / "defs")
    return tmp_path


def test_write_then_check_clean(tmp_path: Path) -> None:
    root = _tmp_root(tmp_path)
    assert cmd_write(root) == 0
    for rel in OUTPUTS:
        assert (root / rel).exists(), rel
    assert cmd_check(root) == 0


def test_check_detects_stale(tmp_path: Path, capsys: object) -> None:
    root = _tmp_root(tmp_path)
    cmd_write(root)
    header = root / "firmware/uno/src/generated/cq_protocol.h"
    header.write_text(header.read_text() + "// edited by hand\n")
    assert cmd_check(root) == 1


def test_check_detects_missing_regeneration(tmp_path: Path) -> None:
    root = _tmp_root(tmp_path)
    cmd_write(root)
    meta = root / "protocol/defs/meta.yaml"
    meta.write_text(meta.read_text().replace("protocol_version: 1", "protocol_version: 2"))
    assert cmd_check(root) == 1


def test_write_refuses_invalid_defs(tmp_path: Path) -> None:
    root = _tmp_root(tmp_path)
    meta = root / "protocol/defs/meta.yaml"
    meta.write_text(meta.read_text().replace("max_payload: 48", "max_payload: 8"))
    assert cmd_write(root) == 1


def test_committed_outputs_are_fresh() -> None:
    assert cmd_check(ROOT) == 0
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd protocol && uv run pytest generator/tests/test_cli.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protogen.cli'`.

- [ ] **Step 3: Implement the docs emitter**

`protocol/generator/protogen/emit_docs.py`:
```python
"""Emit docs/protocol/reference.md."""

from __future__ import annotations

from .model import Field, ProtocolModel


def _field_type(f: Field) -> str:
    if f.items is not None:
        inner = ", ".join(f"{i.name} {i.type}" for i in f.items)
        return f"[{inner}] × {f.count_field} (≤ {f.max_count})"
    if f.count is not None:
        return f"{f.type}[{f.count}]"
    return str(f.type)


def _notes(f: Field) -> str:
    parts: list[str] = []
    if f.unit:
        parts.append(f"unit {f.unit}")
    if f.scale is not None:
        parts.append(f"scale {f.scale}")
    if f.range is not None:
        parts.append(f"range {f.range[0]}..{f.range[1]}")
    if f.enum:
        parts.append(f"enum {f.enum}")
    if f.sentinel:
        parts.append("sentinel " + ", ".join(f"{k}={v}" for k, v in f.sentinel.items()))
    if f.doc:
        parts.append(f.doc)
    return "; ".join(parts)


def emit_docs(model: ProtocolModel) -> str:
    m = model.meta
    out = [
        "<!-- DO NOT EDIT - generated by protogen from protocol/defs -->",
        "# Body-link protocol reference",
        "",
        f"Protocol version **{m.protocol_version}** · UART **{m.baud}** baud · "
        f"frame `0x{m.sync:02X} | len | type | seq | payload | crc8` · max payload **{m.max_payload}** · "
        f"CRC-8 poly 0x{m.crc_poly:02X} init 0x{m.crc_init:02X} · little-endian",
        "",
        "## Frames",
        "",
    ]
    for fr in model.frames:
        flags = [x for x, on in (("ack", fr.ack), ("streaming", fr.streaming)) if on]
        out += [
            f"### 0x{fr.type:02X} {fr.name} ({fr.dir}){' — ' + ', '.join(flags) if flags else ''}",
            "",
            fr.doc,
            "",
            f"Payload {fr.max_payload()} bytes max.",
            "",
        ]
        if fr.fields:
            out += ["| Field | Type | Notes |", "|---|---|---|"]
            out += [f"| {f.name} | {_field_type(f)} | {_notes(f)} |" for f in fr.fields]
            out.append("")
    out += ["## Enums", ""]
    for e in model.enums:
        values = ", ".join(f"{k}={v}" for k, v in e.values.items())
        out.append(f"- **{e.name}**: {values}{' — ' + e.doc if e.doc else ''}")
    out += ["", "## Events (EVENT.kind)", "", "| Kind | Name | a | b |", "|---|---|---|---|"]
    out += [f"| {ev.kind} | {ev.name} | {ev.a or ''} | {ev.b or ''} |" for ev in model.events]
    out += ["", "## Errors", "", "| Code | Reason | HTTP | Message |", "|---|---|---|---|"]
    for d in model.errors:
        out += [f"| {d.prefix}_{e.code} | {e.reason} | {e.http} | {e.message} |" for e in d.codes]
    out += ["", "## ACK status byte", ""]
    out += [f"{i}. {r}" for i, r in enumerate(model.ack_status)]
    out += [
        "",
        "## Settings",
        "",
        "| Id | Name | Scope | Wire | Unit | Scale | Min | Max | Default |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    out += [
        f"| {s.id} | {s.name} | {s.scope} | {s.type} | {s.unit} | {s.scale} | {s.min} | {s.max} | "
        f"{s.default} |"
        for s in model.settings
    ]
    out.append("")
    return "\n".join(out)
```

- [ ] **Step 4: Implement the CLI (write/check)**

`protocol/generator/protogen/cli.py`:
```python
"""protogen command line: write | check | diff."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from pathlib import Path

from .emit_cpp import emit_cpp
from .emit_docs import emit_docs
from .emit_python import emit_python
from .emit_vectors import emit_vectors_cpp, emit_vectors_json
from .loader import load
from .model import ProtocolModel
from .validator import validate

OUTPUTS: dict[str, Callable[[ProtocolModel], str]] = {
    "protocol/python/conqueror_protocol/generated.py": emit_python,
    "firmware/uno/src/generated/cq_protocol.h": emit_cpp,
    "protocol/vectors/frames.json": emit_vectors_json,
    "protocol/tests/cpp/generated_vectors_test.cpp": emit_vectors_cpp,
    "docs/protocol/reference.md": emit_docs,
}

DEFAULT_ROOT = Path(__file__).resolve().parents[3]


def _load_valid(root: Path) -> ProtocolModel | None:
    model = load(root / "protocol" / "defs")
    issues = validate(model)
    for issue in issues:
        print(f"protogen: {issue}", file=sys.stderr)
    return None if issues else model


def render_all(model: ProtocolModel) -> dict[str, str]:
    return {rel: emit(model) for rel, emit in OUTPUTS.items()}


def cmd_write(root: Path) -> int:
    model = _load_valid(root)
    if model is None:
        return 1
    for rel, text in render_all(model).items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    print(f"protogen: wrote {len(OUTPUTS)} files")
    return 0


def cmd_check(root: Path) -> int:
    model = _load_valid(root)
    if model is None:
        return 1
    stale = [
        rel
        for rel, text in render_all(model).items()
        if not (root / rel).exists() or (root / rel).read_text(encoding="utf-8") != text
    ]
    for rel in stale:
        print(f"protogen: stale or missing {rel} (run: uv run protogen write)", file=sys.stderr)
    return 1 if stale else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="protogen")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("write", help="regenerate all outputs")
    sub.add_parser("check", help="fail if outputs are stale or defs invalid")
    diff = sub.add_parser("diff", help="detect breaking changes against a git ref")
    diff.add_argument("--against", required=True)
    args = parser.parse_args(argv)
    root: Path = args.root
    if args.cmd == "write":
        return cmd_write(root)
    if args.cmd == "check":
        return cmd_check(root)
    from .diff import cmd_diff

    return cmd_diff(root, args.against)


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 5: Add the staleness script and hook**

`scripts/check-protocol.sh`:
```bash
#!/usr/bin/env bash
# Fails if protocol/defs changed without regenerating outputs.
set -euo pipefail
cd "$(dirname "$0")/../protocol"
uv run protogen check
```

`.githooks/pre-commit`:
```bash
#!/usr/bin/env bash
set -euo pipefail
"$(git rev-parse --show-toplevel)/scripts/check-protocol.sh"
```

Run: `chmod +x scripts/check-protocol.sh .githooks/pre-commit && git config core.hooksPath .githooks`
Expected: no output.

- [ ] **Step 6: Generate everything and run all tests**

Run: `cd protocol && uv run protogen write && uv run pytest generator/tests -v`
Expected: `protogen: wrote 5 files`; all tests pass, including `test_committed_outputs_are_fresh`.

- [ ] **Step 7: Record the LLD deviation**

Append to `docs/lld/protocol-generator-lld.md`:
```markdown

## Implementation note (Plan A)
- Jinja2 templates were replaced by small emitter functions (`emit_*.py`) that return text — simpler and unit-testable; no `templates/` folder.
- ESP32 C++, TypeScript and client-JSON outputs are deferred to roadmap phases 4/5 (YAGNI); the Uno header is reused by the ESP32 bridge only as raw bytes (the bridge does not parse frames).
- C++ host test: `protocol/tests/cpp/generated_vectors_test.cpp` (generated) is compiled and run by `pytest` (`test_cpp_vectors.py`).
```

- [ ] **Step 8: Lint, type-check, commit**

Run: `cd protocol && uv run ruff check . && uv run ruff format --check . && uv run pyright && ../scripts/check-protocol.sh`
```bash
git add protocol scripts .githooks firmware/uno/src/generated docs/protocol docs/lld/protocol-generator-lld.md
git commit -m "feat(protocol): docs emitter, protogen write/check, staleness hook, generated outputs"
```

---

### Task 8: `protogen diff` — breaking-change detection

**Files:**
- Create: `protocol/generator/protogen/diff.py`
- Test: `protocol/generator/tests/test_diff.py`

**Interfaces:**
- Consumes: `load`, model classes.
- Produces: `protogen.diff.breaking_changes(old: ProtocolModel, new: ProtocolModel) -> list[str]`, `protogen.diff.cmd_diff(root: Path, ref: str) -> int` (fails if breaking changes exist and `protocol_version` was not bumped).

- [ ] **Step 1: Write the failing tests**

`protocol/generator/tests/test_diff.py`:
```python
from pathlib import Path

from protogen.diff import breaking_changes
from protogen.loader import load
from protogen.model import Field

DEFS = Path(__file__).resolve().parents[2] / "defs"


def test_identical_is_not_breaking() -> None:
    assert breaking_changes(load(DEFS), load(DEFS)) == []


def test_removed_frame_is_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frames = [f for f in new.frames if f.name != "CAM_HOME"]
    assert any("CAM_HOME" in c and "removed" in c for c in breaking_changes(old, new))


def test_changed_field_type_is_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("TURN").fields[2].type = "i32"
    assert any("TURN" in c for c in breaking_changes(old, new))


def test_appended_field_is_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("STOP").fields.append(Field(name="extra", type="u8"))
    assert any("STOP" in c for c in breaking_changes(old, new))


def test_doc_change_is_not_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("TURN").fields[0].doc = "changed wording"
    assert breaking_changes(old, new) == []


def test_new_frame_is_not_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    old.frames = [f for f in old.frames if f.name != "CAM_HOME"]
    assert breaking_changes(old, new) == []
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd protocol && uv run pytest generator/tests/test_diff.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'protogen.diff'`.

- [ ] **Step 3: Implement diff**

`protocol/generator/protogen/diff.py`:
```python
"""Breaking-change detection between two versions of protocol/defs."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

from .loader import DEFS_FILES, load
from .model import Field, ProtocolModel


def _shape(f: Field) -> tuple[object, ...]:
    items = tuple((i.name, i.type) for i in f.items) if f.items else None
    return (f.name, f.type, f.count, items, f.count_field, f.max_count)


def breaking_changes(old: ProtocolModel, new: ProtocolModel) -> list[str]:
    changes: list[str] = []
    new_frames = {f.name: f for f in new.frames}
    for of in old.frames:
        nf = new_frames.get(of.name)
        if nf is None:
            changes.append(f"frame {of.name} removed")
            continue
        if (nf.type, nf.dir) != (of.type, of.dir):
            changes.append(f"frame {of.name}: type/direction changed")
        if [_shape(f) for f in nf.fields] != [_shape(f) for f in of.fields]:
            changes.append(f"frame {of.name}: fields changed")
    old_status = old.ack_status
    if new.ack_status[: len(old_status)] != old_status:
        changes.append("ack_status reordered or entries removed (append only)")
    return changes


def _load_ref(root: Path, ref: str, tmp: Path) -> ProtocolModel:
    for stem in DEFS_FILES:
        rel = f"protocol/defs/{stem}.yaml"
        text = subprocess.run(
            ["git", "-C", str(root), "show", f"{ref}:{rel}"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        (tmp / f"{stem}.yaml").write_text(text, encoding="utf-8")
    return load(tmp)


def cmd_diff(root: Path, ref: str) -> int:
    with tempfile.TemporaryDirectory() as tmp:
        old = _load_ref(root, ref, Path(tmp))
    new = load(root / "protocol" / "defs")
    changes = breaking_changes(old, new)
    for change in changes:
        print(f"protogen: breaking: {change}", file=sys.stderr)
    if changes and new.meta.protocol_version == old.meta.protocol_version:
        print("protogen: bump protocol_version in protocol/defs/meta.yaml", file=sys.stderr)
        return 1
    return 0
```

- [ ] **Step 4: Run all tests and a real diff**

Run: `cd protocol && uv run pytest generator/tests -v && uv run protogen diff --against HEAD`
Expected: all tests pass; diff prints nothing and exits 0.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd protocol && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add protocol/generator
git commit -m "feat(protocol): protogen diff breaking-change detection"
```

---

## Done when

- `cd protocol && uv run pytest generator/tests` passes (model, loader, validator, encoder/golden, Python runtime per vector, C++ per vector, CLI, diff).
- `scripts/check-protocol.sh` exits 0 and the pre-commit hook is active.
- `firmware/uno/src/generated/cq_protocol.h` exists and is ready for Plan B; `conqueror_protocol` is importable for Plan D.
