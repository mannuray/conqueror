# Plan D — Bench Tool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `bench`, a laptop tool that plays the ESP32's role on the body link (USB serial or the ESP32 WiFi bridge) to drive the tank, run the standard straight/circle/square/spin/speed/safety tests with measured pass/fail reports, calibrate (reference Python maths) and tune it.

**Architecture:** Ports & adapters in one Python package. A `Transport` port (serial, TCP, in-memory fake) feeds a `LinkSession` (heartbeat, handshake, acks, STOP fast path) which decodes frames with the generated `conqueror_protocol` package into a `TankModel`; a typed `CommandApi` sits on top. Services (teleop, test runner, calibration, tuning, probe) depend only on `Tank` (link + model + api) and a `Prompter` port; the Typer CLI and the Textual UI are thin shells over them. A `SimulatedUno` speaks the real protocol over the fake transport so almost every test runs without hardware.

**Tech Stack:** Python 3.12 via uv, `conqueror-protocol` (Plan A, path dependency), pyserial-asyncio, Textual, Typer, pydantic 2, PyYAML, numpy, matplotlib (Agg), zeroconf, pynput (optional extra); pytest + pytest-asyncio, ruff, pyright (strict).

**Spec:** `docs/lld/bench-tool-lld.md`; driving keys, standard tests and pass criteria from `docs/implementation-roadmap.md` (Bench tool / Bench UI); link rules from `docs/lld/uno-firmware-lld.md` §9.2, §9.4, §9.7, §9.11, §9.18; calculator maths from `docs/lld/esp32-firmware-lld.md` §7.3; odometry from `docs/lld/esp32-firmware-lld.md` §5.2 and §10.8; Python rules from `docs/standards/coding-standards.md` §5.

**Roadmap position:** Phase 3 (with Plan C, ESP32 bridge). Consumes Plan A's `conqueror_protocol`. Hardware acceptance (Task 16) needs Plan B (Uno firmware) flashed; WiFi runs need Plan C.

## Global Constraints

- **Platform:** commands are shown for macOS; on **Linux** use the equivalents in `docs/dev-setup.md` (uv install, serial port names `/dev/ttyACM0`/`/dev/ttyUSB0` + `dialout` group, PlatformIO udev rules, `avahi-browse` instead of `dns-sd`, hold-to-drive needs X11).
- Python **3.12**, uv project at `tools/bench/`, package `conqueror_bench`; `conqueror-protocol` from `../../protocol/python` (editable path source). Ruff + pyright strict clean; pytest with `asyncio_mode = "auto"`.
- Link: frames via `conqueror_protocol.framing` only (no hand-built frames); **UART 250000 baud**; TCP bridge default port **3333**, host `conqueror-bridge.local`.
- **Heartbeat every 100 ms** from its own task; **ack timeout 200 ms**; **STOP resent every 50 ms until acked, max 10 attempts**; handshake HELLO → (ENVELOPE if configured, must ack OK) → HELLO_ACK; HELLO_REQ if no HELLO within 1 s.
- Leaving a session for any reason (quit, Ctrl-C, exception) **sends STOP first** (best effort, ≤ 0.6 s), then closes the transport.
- Telemetry decoding per Uno LLD §9.11: yaw u16 0.01°, yaw_rate 0.1 °/s, pitch/roll 0.01°, pan/tilt 0.1°, temp 0.01 °C; `spd_act = -128` → `None`; `dist_mm 0xFFFF` → clear (`None`), `0xFFFE` → sensor fault; **CCW positive, `w > 0` turns left**.
- Never block the event loop: `input()` and matplotlib run in `asyncio.to_thread`.
- No `eval` of user/YAML text: test metrics are named functions from a registry.
- CAL_DATA channel layouts: the authoritative table is in `docs/lld/protocol-generator-lld.md` ("CAL_DATA channel layouts", written by Plan A Task 7); `calculators.py` (Task 11 docstring) mirrors it and Plan B's `CalScriptRunner` implements it. `CAL_RESULT` semantics (as the Uno sends them): `progress < 100` + `OK` = still running (sent at start and every 500 ms); `progress == 100` + `OK` = done; any other status = failed/cancelled. The bench never sends `IDLE` or `SERVO_NUDGE` (it uses telemetry and `CAM_NUDGE` for those items).

## Review Focus

1. **Uno already synced from an earlier session (no HELLO arrives)** → bench sends HELLO_REQ and still connects (Task 3 `test_handshake_with_already_synced_uno`).
2. **Ctrl-C or an exception while driving** → STOP reaches the tank before the transport closes (Task 3 `test_exit_sends_stop_even_on_error`).
3. **WiFi/link drops mid-move** → the test step fails with `LINK_LOST` instead of hanging (Task 9 `test_move_link_lost_reports_failure`).
4. **Yaw wrapping 359.99° → 0° during a spin** → heading change measures ~360°, not ~0° (Task 7 `test_heading_change_unwraps_full_turn`).
5. **Sentinel telemetry values** (no encoders `-128`, ultrasonic clear `0xFFFF` / fault `0xFFFE`) → `None`/fault flag, never bogus numbers (Task 4 `test_sentinels_decode_to_none_and_fault`).

---

## File Structure

```
tools/bench/
├── pyproject.toml
├── conqueror_bench/
│   ├── __init__.py
│   ├── transport.py      Transport port + FakeTransport/fake_pair + TcpTransport + SerialTransport (Task 1)
│   ├── sim.py            SimulatedUno + SimConfig (Task 2)
│   ├── link.py           LinkSession, AckResult, HelloInfo, LinkStats, HandshakeError, FrameSink (Task 3)
│   ├── model.py          FastSample, SlowSample, TankEvent, decode_fast/slow, TankModel (Task 4)
│   ├── api.py            CommandApi, PrimitiveDone, Tank, open_tank (Task 5)
│   ├── teleop.py         CruiseControl, KeyAction, DriveCmd, Teleop, hold_state, HoldKeyboard (Task 6)
│   ├── odometry.py       PathPoint, integrate, unwrap helpers (Task 7)
│   ├── metrics.py        heading/drift/circle/closure/spin/speed-hold/step metrics (Task 7)
│   ├── recorder.py       Recorder, read_raw, replay_into, load_fast_csv (Task 8)
│   ├── prompter.py       Prompter port, ConsolePrompter, ScriptedPrompter (Task 9)
│   ├── runner.py         TestDefinition, Criterion, METRICS, RunContext, TestRunner, TestResult (Task 9)
│   ├── reporter.py       write_report, compare_sessions (Task 10)
│   ├── calculators.py    reference calibration maths, CAL_DATA layouts (Task 11)
│   ├── calibration.py    CalibrationService (Task 12)
│   ├── tuning.py         TuningService (Task 13)
│   ├── probe.py          shield probe + mount check (Task 13)
│   ├── discovery.py      mDNS bridge discovery + serial port list (Task 14)
│   ├── console.py        console command parser + dispatcher (Task 14)
│   ├── cli.py            Typer app `bench` (Task 14)
│   └── app.py            Textual BenchApp (Task 15)
├── tests_defs/           straight_200cm.yaml circle_r50.yaml square_100.yaml spin_360.yaml
│                         speed_hold_30.yaml safety_checks.yaml (Task 9)
├── sessions/             runtime output (git-ignored)
└── tests/                pytest (conftest.py + one file per task)
```

---

### Task 1: Project scaffold and transports

**Files:**
- Create: `tools/bench/pyproject.toml`, `tools/bench/conqueror_bench/__init__.py`, `tools/bench/conqueror_bench/transport.py`, `tools/bench/tests/__init__.py`
- Modify: `.gitignore` (add `tools/bench/sessions/`)
- Test: `tools/bench/tests/test_transport.py`

**Interfaces:**
- Consumes: `conqueror_protocol.generated.BAUD` (Plan A).
- Produces: `Transport` (Protocol: `async open()`, `async read() -> bytes` (b"" = EOF), `async write(data: bytes)`, `async close()`); `FakeTransport` (`.written: list[bytes]`, `.opened`, `.closed`); `fake_pair() -> tuple[FakeTransport, FakeTransport]`; `TcpTransport(host: str, port: int = 3333, connect_timeout_s: float = 5.0)`; `SerialTransport(port: str, baud: int = BAUD)`.

- [ ] **Step 1: Create the project**

`tools/bench/pyproject.toml`:
```toml
[project]
name = "conqueror-bench"
version = "0.1.0"
description = "Conqueror tank bench tool"
requires-python = ">=3.12"
dependencies = [
    "conqueror-protocol",
    "pyserial-asyncio>=0.6",
    "textual>=0.80",
    "typer>=0.12",
    "pydantic>=2.9",
    "pyyaml>=6.0",
    "numpy>=2.0",
    "matplotlib>=3.9",
    "zeroconf>=0.132",
]

[project.optional-dependencies]
hold = ["pynput>=1.7"]

[project.scripts]
bench = "conqueror_bench.cli:main"

[tool.uv.sources]
conqueror-protocol = { path = "../../protocol/python", editable = true }

[dependency-groups]
dev = ["pytest>=8.3", "pytest-asyncio>=0.24", "ruff>=0.7", "pyright>=1.1.385", "types-PyYAML>=6.0"]

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["conqueror_bench"]

[tool.ruff]
line-length = 100
target-version = "py312"

[tool.ruff.lint]
select = ["E", "F", "I", "B", "UP", "SIM", "RUF", "ASYNC"]

[tool.pyright]
typeCheckingMode = "strict"
include = ["conqueror_bench", "tests"]
pythonVersion = "3.12"
reportMissingTypeStubs = false

[tool.pytest.ini_options]
testpaths = ["tests"]
asyncio_mode = "auto"
asyncio_default_fixture_loop_scope = "function"
```

`tools/bench/conqueror_bench/__init__.py`:
```python
"""Conqueror bench tool: drive, test, calibrate and tune the tank over the body link."""
```

`tools/bench/tests/__init__.py`: empty file.

Append to `.gitignore`:
```
tools/bench/sessions/
```

- [ ] **Step 2: Write the failing transport tests**

`tools/bench/tests/test_transport.py`:
```python
import asyncio

from conqueror_bench.transport import TcpTransport, fake_pair


async def test_fake_pair_round_trip() -> None:
    a, b = fake_pair()
    await a.open()
    await b.open()
    await a.write(b"\xaa\x01")
    assert await b.read() == b"\xaa\x01"
    await b.write(b"hi")
    assert await a.read() == b"hi"
    assert a.written == [b"\xaa\x01"]


async def test_fake_close_signals_eof_to_peer() -> None:
    a, b = fake_pair()
    await a.close()
    assert await b.read() == b""


async def test_tcp_transport_echo() -> None:
    async def echo(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        writer.write(await reader.read(16))
        await writer.drain()
        writer.close()

    server = await asyncio.start_server(echo, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    async with server:
        t = TcpTransport("127.0.0.1", port)
        await t.open()
        await t.write(b"ping")
        assert await t.read() == b"ping"
        await t.close()
```

- [ ] **Step 3: Run them to see them fail**

Run: `cd tools/bench && uv sync && uv run pytest tests/test_transport.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'conqueror_bench.transport'`.

- [ ] **Step 4: Implement the transports**

`tools/bench/conqueror_bench/transport.py`:
```python
"""Byte transports: in-memory fake (tests), TCP (ESP32 WiFi bridge), serial (USB)."""

from __future__ import annotations

import asyncio
import socket
from contextlib import suppress
from typing import Any, Protocol

from conqueror_protocol.generated import BAUD


class Transport(Protocol):
    async def open(self) -> None: ...

    async def read(self) -> bytes:
        """Return at least one byte, or b"" at end of stream."""
        ...

    async def write(self, data: bytes) -> None: ...

    async def close(self) -> None: ...


class FakeTransport:
    """One end of an in-memory byte pipe."""

    def __init__(self, inbox: asyncio.Queue[bytes], peer_inbox: asyncio.Queue[bytes]) -> None:
        self._inbox = inbox
        self._peer = peer_inbox
        self.written: list[bytes] = []
        self.opened = False
        self.closed = False

    async def open(self) -> None:
        self.opened = True

    async def read(self) -> bytes:
        return await self._inbox.get()

    async def write(self, data: bytes) -> None:
        if self.closed:
            raise ConnectionError("fake transport closed")
        self.written.append(data)
        await self._peer.put(data)

    async def close(self) -> None:
        if not self.closed:
            self.closed = True
            await self._peer.put(b"")


def fake_pair() -> tuple[FakeTransport, FakeTransport]:
    a: asyncio.Queue[bytes] = asyncio.Queue()
    b: asyncio.Queue[bytes] = asyncio.Queue()
    return FakeTransport(a, b), FakeTransport(b, a)


class _StreamTransport:
    _reader: asyncio.StreamReader
    _writer: asyncio.StreamWriter

    async def read(self) -> bytes:
        return await self._reader.read(512)

    async def write(self, data: bytes) -> None:
        self._writer.write(data)
        await self._writer.drain()

    async def close(self) -> None:
        self._writer.close()
        with suppress(Exception):
            await self._writer.wait_closed()


class TcpTransport(_StreamTransport):
    """TCP to the ESP32 WiFi bridge (transparent byte pipe to the Uno UART)."""

    def __init__(self, host: str, port: int = 3333, connect_timeout_s: float = 5.0) -> None:
        self.host = host
        self.port = port
        self._timeout = connect_timeout_s

    async def open(self) -> None:
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), self._timeout
        )
        sock: Any = self._writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


class SerialTransport(_StreamTransport):
    """USB serial to the Uno (shield switch S1 must disconnect the ESP32)."""

    def __init__(self, port: str, baud: int = BAUD) -> None:
        self.port = port
        self.baud = baud

    async def open(self) -> None:
        import serial_asyncio  # pyright: ignore[reportMissingImports]

        reader, writer = await serial_asyncio.open_serial_connection(  # pyright: ignore
            url=self.port, baudrate=self.baud
        )
        self._reader = reader  # pyright: ignore[reportAttributeAccessIssue]
        self._writer = writer  # pyright: ignore[reportAttributeAccessIssue]
```
A WebSocket transport for the full ESP32 gateway (roadmap phase 4) is out of scope; it will implement the same `Transport` protocol.

- [ ] **Step 5: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_transport.py -v`
Expected: 3 passed.

- [ ] **Step 6: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: clean (run `uv run ruff format .` first if needed).
```bash
git add tools/bench .gitignore
git commit -m "feat(bench): project scaffold and byte transports"
```

---

### Task 2: Simulated Uno

**Files:**
- Create: `tools/bench/conqueror_bench/sim.py`
- Test: `tools/bench/tests/test_sim.py`

**Interfaces:**
- Consumes: `Transport`, `fake_pair`; `conqueror_protocol` (`FrameParser`, `build_frame`, `decode_frame`, `generated as g`).
- Produces: `SimConfig` (dataclass fields shown below), `SimulatedUno(transport, config=None)` with `async run()`, `async emit_event(kind: g.EventKind, a: int = 0, b: int = 0)`, attributes `received: list[Message]`, `synced: bool`, `envelope: tuple[int,int,int] | None`, `brake_on: bool`, `yaw_deg`, `x_cm`, `y_cm`, `enc_l`, `enc_r`, `mode: str`, `settings: dict[int,int]`, `saved_settings: dict[int,int]`, `link_lost_count: int`.

- [ ] **Step 1: Write the failing simulator tests**

`tools/bench/tests/test_sim.py`:
```python
import asyncio
from contextlib import suppress

from conqueror_protocol import FrameParser, Message, build_frame, decode_frame
from conqueror_protocol import generated as g

from conqueror_bench.sim import SimConfig, SimulatedUno
from conqueror_bench.transport import FakeTransport, fake_pair


class Peer:
    """Minimal hand-driven ESP32 side for testing the simulator directly."""

    def __init__(self, t: FakeTransport) -> None:
        self.t = t
        self.parser = FrameParser()
        self.seq = 0
        self.inbox: list[Message] = []

    async def send(self, msg: Message) -> int:
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFF
        await self.t.write(build_frame(msg, seq))
        return seq

    async def pump(self, seconds: float) -> None:
        end = asyncio.get_running_loop().time() + seconds
        while (left := end - asyncio.get_running_loop().time()) > 0:
            try:
                data = await asyncio.wait_for(self.t.read(), left)
            except TimeoutError:
                return
            for raw in self.parser.feed(data):
                self.inbox.append(decode_frame(raw, "uno_to_esp32"))

    def of(self, cls: type[Message]) -> list[Message]:
        return [m for m in self.inbox if isinstance(m, cls)]


async def _start(cfg: SimConfig | None = None) -> tuple[Peer, SimulatedUno, asyncio.Task[None]]:
    esp, uno_side = fake_pair()
    uno = SimulatedUno(uno_side, cfg or SimConfig(hello_interval_s=0.05))
    task = asyncio.create_task(uno.run())
    return Peer(esp), uno, task


async def _stop(task: asyncio.Task[None]) -> None:
    task.cancel()
    with suppress(asyncio.CancelledError):
        await task


async def test_hello_until_hello_ack() -> None:
    peer, uno, task = await _start()
    await peer.pump(0.2)
    assert len(peer.of(g.Hello)) >= 2
    await peer.send(g.HelloAck(proto=g.PROTOCOL_VERSION))
    await peer.pump(0.05)
    count = len(peer.of(g.Hello))
    await peer.pump(0.2)
    assert len(peer.of(g.Hello)) == count
    assert uno.synced
    await _stop(task)


async def test_acks_and_settings() -> None:
    peer, uno, task = await _start()
    await peer.send(g.HelloAck(proto=g.PROTOCOL_VERSION))
    seq = await peer.send(g.SettingSet(id=int(g.SettingId.DEADMAN), value=500))
    bad = await peer.send(g.SettingSet(id=int(g.SettingId.DEADMAN), value=99999))
    await peer.send(g.SettingGet(id=int(g.SettingId.DEADMAN)))
    await peer.pump(0.1)
    acks = {a.ack_seq: a.status for a in peer.of(g.Ack)}  # type: ignore[attr-defined]
    assert acks[seq] == 0
    assert g.ACK_STATUS[acks[bad]] == "SETTING_OUT_OF_RANGE"
    values = peer.of(g.SettingValue)
    assert values and values[-1].value == 500  # type: ignore[attr-defined]
    assert uno.settings[int(g.SettingId.DEADMAN)] == 500
    await _stop(task)


async def test_move_reports_primitive_done() -> None:
    peer, uno, task = await _start(SimConfig(hello_interval_s=0.05, max_speed_cm_s=200.0))
    await peer.send(g.HelloAck(proto=g.PROTOCOL_VERSION))
    await peer.send(g.Move(src=1, dist_mm=200, speed=100, heading=32767))
    for _ in range(10):
        await peer.send(g.Heartbeat(esp_ms=0))
        await peer.pump(0.05)
    done = [e for e in peer.of(g.Event) if e.kind == g.EventKind.PRIMITIVE_DONE]  # type: ignore[attr-defined]
    assert done and (done[0].a >> 8) & 1 == 1  # type: ignore[attr-defined]
    assert 19.0 < uno.x_cm < 21.0
    assert uno.enc_l > 0 and uno.enc_r > 0
    await _stop(task)


async def test_link_loss_stops_and_reports() -> None:
    peer, uno, task = await _start(SimConfig(hello_interval_s=0.05))
    await peer.send(g.HelloAck(proto=g.PROTOCOL_VERSION))
    await peer.send(g.Drive(src=1, v=50, w=0))
    await peer.pump(0.5)
    kinds = {e.kind for e in peer.of(g.Event)}  # type: ignore[attr-defined]
    assert g.EventKind.LINK_LOST in kinds
    assert uno.mode == "idle"
    await _stop(task)


async def test_pwm_ramp_calibration_stream() -> None:
    peer, _uno, task = await _start()
    await peer.send(g.HelloAck(proto=g.PROTOCOL_VERSION))
    await peer.send(
        g.Calibrate(op=int(g.CalOp.START), script=int(g.CalScript.PWM_RAMP), param=0, profile=0)
    )
    await peer.pump(0.5)
    data = peer.of(g.CalData)
    assert len(data) > 100
    assert peer.of(g.CalResult)
    await _stop(task)
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_sim.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'conqueror_bench.sim'`.

- [ ] **Step 3: Implement the simulator**

`tools/bench/conqueror_bench/sim.py`:
```python
"""Simulated Uno speaking the real body-link protocol (tests, demos, `--sim`).

Kinematics are ideal (perfect heading hold, no slip); enough to exercise the bench end to end.
Calibration scripts implemented: STILL_SAMPLE, PWM_RAMP, DRIVE_COUNTS, SPIN_AND_ARC, FULL_SPIN;
other scripts finish immediately with no samples (their maths is unit-tested directly).
"""

from __future__ import annotations

import asyncio
import math
import time
from dataclasses import dataclass, field

from conqueror_protocol import FrameParser, Message, build_frame, decode_frame
from conqueror_protocol import generated as g

from .transport import Transport


def _status(name: str) -> int:
    return g.ACK_STATUS.index(name)


@dataclass
class SimConfig:
    encoders: bool = True
    counts_per_cm: float = 76.0
    max_speed_cm_s: float = 40.0
    max_rate_dps: float = 180.0
    track_width_cm: float = 15.0
    track_width_eff_cm: float = 18.0
    hello_interval_s: float = 0.5
    telem_interval_s: float = 0.05
    slow_interval_s: float = 1.0
    link_timeout_s: float = 0.3
    deadman_s: float = 0.3
    gyro_bias: tuple[int, int, int] = (12, -7, 35)
    deadband_start: int = 74
    deadband_keep: int = 58
    max_cps: tuple[int, int] = (2400, 2600)
    batt_mv: int = 7800
    cal_sample_interval_s: float = 0.0
    obstacle_cm: float | None = None
    forced_status: dict[int, int] = field(default_factory=dict[int, int])


class SimulatedUno:
    def __init__(self, transport: Transport, config: SimConfig | None = None) -> None:
        self.t = transport
        self.cfg = config or SimConfig()
        self.received: list[Message] = []
        self.synced = False
        self.envelope: tuple[int, int, int] | None = None
        self.brake_on = True
        self.x_cm = 0.0
        self.y_cm = 0.0
        self.yaw_deg = 0.0
        self.enc_l = 0.0
        self.enc_r = 0.0
        self.mode = "idle"
        self.settings: dict[int, int] = {s.id: s.default for s in g.SETTINGS}
        self.saved_settings = dict(self.settings)
        self.link_lost_count = 0
        self._v_pct = 0.0
        self._w_pct = 0.0
        self._speed = 0
        self._remaining = 0.0
        self._radius_cm = 0
        self._prim = g.PrimitiveKind.NONE
        self._seq = 0
        self._last_rx = time.monotonic()
        self._last_drive = 0.0
        self._link_ok = True
        self._obstacle = self.cfg.obstacle_cm
        self._cal_task: asyncio.Task[None] | None = None

    async def run(self) -> None:
        async with asyncio.TaskGroup() as tg:
            tg.create_task(self._reader())
            tg.create_task(self._hello_loop())
            tg.create_task(self._physics_loop())
            tg.create_task(self._slow_loop())

    async def emit_event(self, kind: g.EventKind, a: int = 0, b: int = 0) -> None:
        await self._send(g.Event(kind=int(kind), a=a, b=b))

    # ---- transport -------------------------------------------------------------------
    async def _send(self, msg: Message) -> None:
        frame = build_frame(msg, self._seq)
        self._seq = (self._seq + 1) & 0xFF
        await self.t.write(frame)

    async def _reader(self) -> None:
        parser = FrameParser()
        while True:
            data = await self.t.read()
            if not data:
                return
            for raw in parser.feed(data):
                try:
                    msg = decode_frame(raw, "esp32_to_uno")
                except ValueError:
                    continue
                await self._handle(msg, raw.seq)

    async def _hello_loop(self) -> None:
        while True:
            if not self.synced:
                await self._send_hello()
            await asyncio.sleep(self.cfg.hello_interval_s)

    async def _send_hello(self) -> None:
        await self._send(
            g.Hello(
                fw=0x0100,
                proto=g.PROTOCOL_VERSION,
                caps=1 if self.cfg.encoders else 0,
                reset_reason=int(g.ResetReason.POWER_ON),
            )
        )

    # ---- command handling --------------------------------------------------------------
    async def _handle(self, msg: Message, seq: int) -> None:
        now = time.monotonic()
        self._last_rx = now
        self.received.append(msg)
        if not self._link_ok:
            self._link_ok = True
            await self.emit_event(g.EventKind.LINK_RESTORED)
        status = self.cfg.forced_status.get(msg.TYPE, 0)
        reply: Message | None = None
        if isinstance(msg, g.HelloAck):
            self.synced = True
        elif isinstance(msg, g.HelloReq):
            await self._send_hello()
        elif isinstance(msg, g.Envelope):
            self.envelope = (msg.speed_cap, msg.accel_cap, msg.lock_brake)
            if msg.lock_brake:
                self.brake_on = True
        elif isinstance(msg, (g.Drive, g.DriveRate)):
            if not self.synced:
                status = _status("BUSY")
            else:
                self.mode = "teleop"
                self._prim = g.PrimitiveKind.TELEOP
                self._v_pct = float(msg.v)
                if isinstance(msg, g.Drive):
                    self._w_pct = float(msg.w)
                else:
                    self._w_pct = msg.yaw_rate / 10.0 / self.cfg.max_rate_dps * 100.0
                self._last_drive = now
        elif isinstance(msg, (g.Stop, g.Cancel)):
            await self._stop_motion("CANCELLED")
        elif isinstance(msg, g.Turn):
            angle = msg.angle / 10.0
            delta = angle if (msg.mode & 0x7F) == int(g.TurnMode.BY) else _wrap(angle - self.yaw_deg)
            self._start_primitive("turn", g.PrimitiveKind.TURN, msg.speed, delta)
            self._radius_cm = msg.radius_cm
        elif isinstance(msg, g.Move):
            if not self.cfg.encoders:
                status = _status("UNAVAILABLE")
            else:
                self._start_primitive("move", g.PrimitiveKind.MOVE, msg.speed, msg.dist_mm / 10.0)
        elif isinstance(msg, g.Brake):
            if not msg.on and self.envelope is not None and self.envelope[2]:
                status = _status("ENVELOPE_LOCKED")
            else:
                self.brake_on = bool(msg.on)
        elif isinstance(msg, g.SettingSet):
            status = self._set_setting(msg.id, msg.value)
        elif isinstance(msg, g.SettingGet):
            if msg.id in self.settings:
                reply = g.SettingValue(id=msg.id, value=self.settings[msg.id])
            else:
                status = _status("SETTING_UNKNOWN")
        elif isinstance(msg, g.SettingsSave):
            self.saved_settings = dict(self.settings)
        elif isinstance(msg, g.Calibrate):
            if msg.op == int(g.CalOp.START):
                self._cal_task = asyncio.create_task(self._run_cal(msg.script, msg.param))
            elif self._cal_task is not None:
                self._cal_task.cancel()
        if msg.ACK:
            await self._send(g.Ack(ack_seq=seq, status=status))
        if reply is not None:
            await self._send(reply)
        if isinstance(msg, g.SettingsSave):
            await self.emit_event(g.EventKind.SETTINGS_SAVED)

    def _set_setting(self, sid: int, value: int) -> int:
        info = next((s for s in g.SETTINGS if s.id == sid), None)
        if info is None:
            return _status("SETTING_UNKNOWN")
        if not info.min <= value <= info.max:
            return _status("SETTING_OUT_OF_RANGE")
        self.settings[sid] = value
        return 0

    def _start_primitive(self, mode: str, kind: g.PrimitiveKind, speed: int, remaining: float) -> None:
        self.mode = mode
        self._prim = kind
        self._speed = speed
        self._remaining = remaining
        self._radius_cm = 0

    async def _finish(self, ok: bool, reason: str) -> None:
        kind = int(self._prim)
        self.mode = "idle"
        self._prim = g.PrimitiveKind.NONE
        self._v_pct = self._w_pct = 0.0
        await self.emit_event(g.EventKind.PRIMITIVE_DONE, kind | (int(ok) << 8), _status(reason) if not ok else 0)

    async def _stop_motion(self, reason: str) -> None:
        if self.mode in ("turn", "move"):
            await self._finish(False, reason)
        self.mode = "idle"
        self._prim = g.PrimitiveKind.NONE
        self._v_pct = self._w_pct = 0.0

    # ---- physics & telemetry --------------------------------------------------------------
    async def _physics_loop(self) -> None:
        while True:
            await asyncio.sleep(self.cfg.telem_interval_s)
            await self._tick(self.cfg.telem_interval_s)

    async def _tick(self, dt: float) -> None:
        now = time.monotonic()
        cfg = self.cfg
        if self.synced and self._link_ok and now - self._last_rx > cfg.link_timeout_s:
            self._link_ok = False
            self.link_lost_count += 1
            await self.emit_event(g.EventKind.LINK_LOST)
            await self._stop_motion("LINK_LOST")
        if self.mode == "teleop" and now - self._last_drive > cfg.deadman_s:
            await self._stop_motion("CANCELLED")
            await self.emit_event(g.EventKind.DEADMAN_STOP)
        v = 0.0
        w_dps = 0.0
        if self.mode == "teleop":
            v = self._v_pct / 100.0 * cfg.max_speed_cm_s
            w_dps = self._w_pct / 100.0 * cfg.max_rate_dps
        elif self.mode == "turn":
            rate = self._speed / 100.0 * cfg.max_rate_dps
            step = math.copysign(min(rate * dt, abs(self._remaining)), self._remaining)
            w_dps = step / dt
            v = abs(math.radians(w_dps)) * self._radius_cm
            self._remaining -= step
        elif self.mode == "move":
            v = self._speed / 100.0 * cfg.max_speed_cm_s
            v = min(v, self._remaining / dt)
            self._remaining -= v * dt
            if self._obstacle is not None:
                self._obstacle -= v * dt
                if self.brake_on and self._obstacle <= 15.0:
                    await self.emit_event(g.EventKind.BLOCKED, int(self._obstacle * 10))
                    await self._finish(False, "BLOCKED")
                    v = 0.0
        self._integrate(v, w_dps, dt)
        if self.mode == "turn" and abs(self._remaining) < 1e-6:
            await self._finish(True, "OK")
        if self.mode == "move" and self._remaining <= 1e-6:
            await self._finish(True, "OK")
        await self._send(self._telem_fast(v, w_dps))

    def _integrate(self, v: float, w_dps: float, dt: float) -> None:
        w = math.radians(w_dps)
        half = self.cfg.track_width_cm / 2.0
        theta_mid = math.radians(self.yaw_deg + w_dps * dt / 2.0)
        self.x_cm += v * dt * math.cos(theta_mid)
        self.y_cm += v * dt * math.sin(theta_mid)
        self.yaw_deg = (self.yaw_deg + w_dps * dt) % 360.0
        self.enc_l += (v - w * half) * dt * self.cfg.counts_per_cm
        self.enc_r += (v + w * half) * dt * self.cfg.counts_per_cm

    def _telem_fast(self, v: float, w_dps: float) -> g.TelemFast:
        cfg = self.cfg
        pct = int(max(-100.0, min(100.0, v / cfg.max_speed_cm_s * 100.0)))
        act = pct if cfg.encoders else -128
        dist = 0xFFFF if self._obstacle is None else max(0, min(0xFFFD, int(self._obstacle * 10)))
        flags = 1 << int(g.FastFlagBit.SYNCED) if self.synced else 0
        return g.TelemFast(
            uno_ms=int(time.monotonic() * 1000) & 0xFFFFFFFF,
            yaw=int(round(self.yaw_deg * 100)) % 36000,
            yaw_rate=int(round(w_dps * 10)),
            pitch=0,
            roll=0,
            spd_cmd_l=pct,
            spd_cmd_r=pct,
            spd_act_l=act,
            spd_act_r=act,
            pwm_l=int(pct * 2.55),
            pwm_r=int(pct * 2.55),
            dist_mm=dist,
            closing_cm_s=int(v) if self._obstacle is not None else 0,
            pan=900,
            tilt=900,
            enc_l=int(self.enc_l),
            enc_r=int(self.enc_r),
            flags=flags,
            prim=int(self._prim),
            prim_pct=0,
        )

    async def _slow_loop(self) -> None:
        while True:
            env = self.envelope or (100, 0, 0)
            await self._send(
                g.TelemSlow(
                    uptime_s=int(time.monotonic()) & 0xFFFFFFFF,
                    reset_reason=int(g.ResetReason.POWER_ON),
                    batt_mv=self.cfg.batt_mv,
                    driving=int(self.mode in ("teleop", "turn", "move")),
                    temp=2500,
                    owner=int(g.Owner.NETWORK if self.mode != "idle" else g.Owner.NONE),
                    flags2=0,
                    accel_var=0,
                    loop_load=30,
                    loop_max_us=900,
                    free_ram=300,
                    err_frames=0,
                    err_i2c=0,
                    settings_crc=0,
                    env_speed_cap=env[0],
                    env_accel_cap=env[1],
                    env_lock_brake=env[2],
                    profile_idx=0,
                    cal_status=(0, 0, 0),
                    hw=(1 if self.cfg.encoders else 0) | 0b110,
                )
            )
            await asyncio.sleep(self.cfg.slow_interval_s)

    # ---- calibration scripts ------------------------------------------------------------------
    async def _cal_sample(self, script: int, phase: int, t_ms: int, v: list[int]) -> None:
        values = [max(-32768, min(32767, int(x))) for x in v + [0] * (8 - len(v))]
        await self._send(g.CalData(script=script, phase=phase, t_ms=t_ms & 0xFFFF, v=tuple(values)))
        await asyncio.sleep(self.cfg.cal_sample_interval_s)

    async def _run_cal(self, script: int, param: int) -> None:
        cfg = self.cfg
        t = 0
        await self._send(g.CalResult(script=script, stage=0, progress=0, status=0))  # started (like the Uno)
        if script == int(g.CalScript.STILL_SAMPLE):
            bx, by, bz = cfg.gyro_bias
            for i in range(100):
                n = i % 3 - 1
                await self._cal_sample(script, 0, t, [bx + n, by + n, bz + n, 0, 0, 8192, 2500, cfg.batt_mv])
                t += 20
        elif script == int(g.CalScript.PWM_RAMP):
            cum = 0
            for phase, pwms, threshold in (
                (0, range(0, 256, 2), cfg.deadband_start),
                (1, range(254, -1, -2), cfg.deadband_keep),
            ):
                for pwm in pwms:
                    moving = pwm >= threshold
                    rate = 400 if moving else 0
                    cum += rate // 50
                    await self._cal_sample(script, phase, t, [pwm, 50 if moving else 0, cum, rate])
                    t += 20
        elif script == int(g.CalScript.DRIVE_COUNTS):
            for i in range(50):
                cum = param * i // 49
                await self._cal_sample(script, 0, t, [cum, cum, 0])
                t += 20
        elif script == int(g.CalScript.SPIN_AND_ARC):
            per_track = math.pi * cfg.track_width_eff_cm * cfg.counts_per_cm
            for i in range(51):
                f = i / 50
                await self._cal_sample(
                    script, 0, t, [-per_track * f, per_track * f, 3600 * f, 900]
                )
                t += 20
        elif script == int(g.CalScript.FULL_SPIN):
            for _ in range(50):
                await self._cal_sample(script, 0, t, [-255, 255, -cfg.max_cps[0], cfg.max_cps[1], 2000])
                t += 20
        await self._send(g.CalResult(script=script, stage=0, progress=100, status=0))


def _wrap(deg: float) -> float:
    return (deg + 180.0) % 360.0 - 180.0
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_sim.py -v`
Expected: 5 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/sim.py tools/bench/tests/test_sim.py
git commit -m "feat(bench): simulated Uno speaking the body-link protocol"
```

---

### Task 3: LinkSession — heartbeat, handshake, acks, STOP fast path

**Files:**
- Create: `tools/bench/conqueror_bench/link.py`, `tools/bench/tests/conftest.py`
- Test: `tools/bench/tests/test_link.py`

**Interfaces:**
- Consumes: `Transport`, `SimulatedUno`, `conqueror_protocol`.
- Produces: `AckResult(ok: bool, status: str)`; `HelloInfo(fw_major, fw_minor, proto, encoders: bool, reset_reason: str)`; `LinkStats(tx_frames, rx_frames, bad_crc, bad_len, decode_errors, ack_timeouts, stop_resends, suppressed)`; `HandshakeError(RuntimeError)`; `FrameSink` (Protocol: `tx(data: bytes)`, `rx(data: bytes)`); `LinkSession(transport, *, heartbeat_s=0.1, ack_timeout_s=0.2, hello_timeout_s=3.0, envelope: tuple[int,int,int] | None = None)` — async context manager; `.hello: HelloInfo | None`; `.stats`; `.closed: asyncio.Event`; `subscribe(cb: Callable[[Message, float], None])`; `set_sink(sink: FrameSink | None)`; `async send(msg) -> AckResult` (`"SENT"` for non-ack frames, `"TIMEOUT"` on no ack); `async stop(src: g.Source = g.Source.HUMAN) -> AckResult`; `async suspend(seconds: float)` (drops all outgoing bytes while suspended — simulated link loss).

- [ ] **Step 1: Write the shared fixture**

`tools/bench/tests/conftest.py`:
```python
import asyncio
from collections.abc import AsyncIterator
from contextlib import suppress
from dataclasses import dataclass

import pytest_asyncio

from conqueror_bench.sim import SimConfig, SimulatedUno
from conqueror_bench.transport import FakeTransport, fake_pair


@dataclass
class SimRig:
    bench: FakeTransport
    uno_side: FakeTransport
    uno: SimulatedUno


@pytest_asyncio.fixture
async def rig() -> AsyncIterator[SimRig]:
    bench, uno_side = fake_pair()
    uno = SimulatedUno(uno_side, SimConfig(hello_interval_s=0.05, max_speed_cm_s=200.0))
    task = asyncio.create_task(uno.run())
    try:
        yield SimRig(bench, uno_side, uno)
    finally:
        task.cancel()
        with suppress(asyncio.CancelledError):
            await task
```

- [ ] **Step 2: Write the failing link tests**

`tools/bench/tests/test_link.py`:
```python
import asyncio

import pytest
from conqueror_protocol import generated as g

from conqueror_bench.link import HandshakeError, LinkSession
from conqueror_bench.sim import SimConfig, SimulatedUno
from conqueror_bench.transport import fake_pair

from .conftest import SimRig


async def test_handshake_and_heartbeat(rig: SimRig) -> None:
    async with LinkSession(rig.bench) as link:
        assert link.hello is not None
        assert link.hello.proto == g.PROTOCOL_VERSION
        assert link.hello.encoders
        await asyncio.sleep(0.35)
        beats = [m for m in rig.uno.received if isinstance(m, g.Heartbeat)]
        assert len(beats) >= 3
        assert rig.uno.synced


async def test_envelope_sent_before_hello_ack(rig: SimRig) -> None:
    async with LinkSession(rig.bench, envelope=(20, 30, 1)):
        order = [type(m).__name__ for m in rig.uno.received if not isinstance(m, g.Heartbeat)]
        assert order.index("Envelope") < order.index("HelloAck")
        assert rig.uno.envelope == (20, 30, 1)


async def test_handshake_with_already_synced_uno() -> None:
    bench, uno_side = fake_pair()
    uno = SimulatedUno(uno_side, SimConfig(hello_interval_s=0.05))
    uno.synced = True  # earlier session already acked; no HELLO without HELLO_REQ
    task = asyncio.create_task(uno.run())
    async with LinkSession(bench) as link:
        assert link.hello is not None
        assert any(isinstance(m, g.HelloReq) for m in uno.received)
    task.cancel()


async def test_no_tank_raises_handshake_error() -> None:
    bench, _silent = fake_pair()
    with pytest.raises(HandshakeError):
        async with LinkSession(bench, hello_timeout_s=0.3):
            pass


async def test_ack_ok_and_failure(rig: SimRig) -> None:
    async with LinkSession(rig.bench) as link:
        ok = await link.send(g.SettingSet(id=int(g.SettingId.DEADMAN), value=400))
        bad = await link.send(g.SettingSet(id=int(g.SettingId.DEADMAN), value=1))
        sent = await link.send(g.Drive(src=1, v=0, w=0))
        assert ok.ok and ok.status == "OK"
        assert not bad.ok and bad.status == "SETTING_OUT_OF_RANGE"
        assert sent.ok and sent.status == "SENT"


async def test_ack_timeout(rig: SimRig) -> None:
    async with LinkSession(rig.bench, ack_timeout_s=0.05) as link:
        await link.suspend(0.0)
        link._suspended_until = asyncio.get_running_loop().time() + 10  # type: ignore[attr-defined]
        res = await link.send(g.CamHome())
        assert not res.ok and res.status == "TIMEOUT"
        assert link.stats.ack_timeouts == 1
        link._suspended_until = 0.0  # type: ignore[attr-defined]


async def test_stop_resends_until_acked(rig: SimRig) -> None:
    async with LinkSession(rig.bench) as link:
        rig.uno.cfg.forced_status[g.Stop.TYPE] = 0
        res = await link.stop()
        assert res.ok
        assert sum(isinstance(m, g.Stop) for m in rig.uno.received) >= 1


async def test_exit_sends_stop_even_on_error(rig: SimRig) -> None:
    with pytest.raises(RuntimeError):
        async with LinkSession(rig.bench) as link:
            await link.send(g.Drive(src=1, v=40, w=0))
            raise RuntimeError("boom")
    stops = [m for m in rig.uno.received if isinstance(m, g.Stop)]
    assert stops, "STOP must be sent on exit"
    assert rig.bench.closed


async def test_suspend_triggers_link_loss(rig: SimRig) -> None:
    async with LinkSession(rig.bench) as link:
        await link.suspend(0.5)
        await asyncio.sleep(0.1)
        assert rig.uno.link_lost_count == 1
        assert link.stats.suppressed > 0
```
`ack_timeout` uses `_suspended_until` directly because the test needs an ack-less frame; `suspend()` is the public path for link-loss simulation.

- [ ] **Step 3: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_link.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'conqueror_bench.link'`.

- [ ] **Step 4: Implement LinkSession**

`tools/bench/conqueror_bench/link.py`:
```python
"""Body-link session: heartbeat, handshake, acks, STOP fast path (bench plays the ESP32)."""

from __future__ import annotations

import asyncio
import time
from collections.abc import Callable
from contextlib import suppress
from dataclasses import dataclass
from types import TracebackType
from typing import Protocol, Self

from conqueror_protocol import FrameParser, Message, build_frame, decode_frame
from conqueror_protocol import generated as g

from .transport import Transport

STOP_RESEND_S = 0.05
STOP_MAX_ATTEMPTS = 10
EXIT_STOP_TIMEOUT_S = 0.6


@dataclass(frozen=True, slots=True)
class AckResult:
    ok: bool
    status: str


@dataclass(frozen=True, slots=True)
class HelloInfo:
    fw_major: int
    fw_minor: int
    proto: int
    encoders: bool
    reset_reason: str


@dataclass(slots=True)
class LinkStats:
    tx_frames: int = 0
    rx_frames: int = 0
    bad_crc: int = 0
    bad_len: int = 0
    decode_errors: int = 0
    ack_timeouts: int = 0
    stop_resends: int = 0
    suppressed: int = 0


class HandshakeError(RuntimeError):
    pass


class FrameSink(Protocol):
    def tx(self, data: bytes) -> None: ...

    def rx(self, data: bytes) -> None: ...


def status_name(index: int) -> str:
    return g.ACK_STATUS[index] if index < len(g.ACK_STATUS) else f"STATUS_{index}"


class LinkSession:
    def __init__(
        self,
        transport: Transport,
        *,
        heartbeat_s: float = 0.1,
        ack_timeout_s: float = 0.2,
        hello_timeout_s: float = 3.0,
        envelope: tuple[int, int, int] | None = None,
    ) -> None:
        self._t = transport
        self._heartbeat_s = heartbeat_s
        self._ack_timeout = ack_timeout_s
        self._hello_timeout = hello_timeout_s
        self._envelope = envelope
        self._parser = FrameParser()
        self._seq = 0
        self._pending: dict[int, asyncio.Future[int]] = {}
        self._subs: list[Callable[[Message, float], None]] = []
        self._hello_event = asyncio.Event()
        self._hello_msg: g.Hello | None = None
        self._suspended_until = 0.0
        self._tasks: list[asyncio.Task[None]] = []
        self._lock = asyncio.Lock()
        self._sink: FrameSink | None = None
        self.stats = LinkStats()
        self.hello: HelloInfo | None = None
        self.closed = asyncio.Event()

    # ---- lifecycle ---------------------------------------------------------------------
    async def __aenter__(self) -> Self:
        await self._t.open()
        self._tasks = [
            asyncio.create_task(self._reader(), name="link-reader"),
            asyncio.create_task(self._heartbeat(), name="link-heartbeat"),
        ]
        try:
            self.hello = await self._handshake()
        except BaseException:
            await self._shutdown(send_stop=False)
            raise
        return self

    async def __aexit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        await self._shutdown(send_stop=True)

    async def _shutdown(self, *, send_stop: bool) -> None:
        if send_stop and not self.closed.is_set():
            self._suspended_until = 0.0
            with suppress(Exception, asyncio.CancelledError):
                await asyncio.wait_for(self.stop(), EXIT_STOP_TIMEOUT_S)
        for task in self._tasks:
            task.cancel()
        for task in self._tasks:
            with suppress(BaseException):
                await task
        with suppress(Exception):
            await self._t.close()

    # ---- public API ------------------------------------------------------------------------
    def subscribe(self, cb: Callable[[Message, float], None]) -> None:
        self._subs.append(cb)

    def set_sink(self, sink: FrameSink | None) -> None:
        self._sink = sink

    async def send(self, msg: Message) -> AckResult:
        seq = self._next_seq()
        fut: asyncio.Future[int] | None = None
        if msg.ACK:
            fut = asyncio.get_running_loop().create_future()
            self._pending[seq] = fut
        await self._write(build_frame(msg, seq))
        if fut is None:
            return AckResult(True, "SENT")
        try:
            status = await asyncio.wait_for(fut, self._ack_timeout)
        except TimeoutError:
            self.stats.ack_timeouts += 1
            return AckResult(False, "TIMEOUT")
        finally:
            self._pending.pop(seq, None)
        return AckResult(status == 0, status_name(status))

    async def stop(self, src: g.Source = g.Source.HUMAN) -> AckResult:
        """STOP fast path: resend every 50 ms until acked (idempotent), max 10 attempts."""
        for attempt in range(STOP_MAX_ATTEMPTS):
            seq = self._next_seq()
            fut: asyncio.Future[int] = asyncio.get_running_loop().create_future()
            self._pending[seq] = fut
            await self._write(build_frame(g.Stop(src=int(src)), seq))
            try:
                status = await asyncio.wait_for(fut, STOP_RESEND_S)
                return AckResult(status == 0, status_name(status))
            except TimeoutError:
                if attempt + 1 < STOP_MAX_ATTEMPTS:
                    self.stats.stop_resends += 1
            finally:
                self._pending.pop(seq, None)
        return AckResult(False, "TIMEOUT")

    async def suspend(self, seconds: float) -> None:
        """Simulated link loss: drop every outgoing byte for `seconds`."""
        self._suspended_until = asyncio.get_running_loop().time() + seconds
        await asyncio.sleep(seconds)

    # ---- internals ---------------------------------------------------------------------------
    def _next_seq(self) -> int:
        seq = self._seq
        self._seq = (self._seq + 1) & 0xFF
        return seq

    async def _write(self, data: bytes) -> None:
        if asyncio.get_running_loop().time() < self._suspended_until:
            self.stats.suppressed += 1
            return
        async with self._lock:
            await self._t.write(data)
        self.stats.tx_frames += 1
        if self._sink is not None:
            self._sink.tx(data)

    async def _heartbeat(self) -> None:
        while True:
            esp_ms = int(time.monotonic() * 1000) & 0xFFFFFFFF
            with suppress(ConnectionError):
                await self._write(build_frame(g.Heartbeat(esp_ms=esp_ms), self._next_seq()))
            await asyncio.sleep(self._heartbeat_s)

    async def _reader(self) -> None:
        while True:
            data = await self._t.read()
            if not data:
                self.closed.set()
                return
            now = time.monotonic()
            if self._sink is not None:
                self._sink.rx(data)
            for raw in self._parser.feed(data):
                self.stats.rx_frames += 1
                try:
                    msg = decode_frame(raw, "uno_to_esp32")
                except ValueError:
                    self.stats.decode_errors += 1
                    continue
                if isinstance(msg, g.Ack):
                    fut = self._pending.get(msg.ack_seq)
                    if fut is not None and not fut.done():
                        fut.set_result(msg.status)
                elif isinstance(msg, g.Hello):
                    self._hello_msg = msg
                    self._hello_event.set()
                for cb in list(self._subs):
                    cb(msg, now)
            self.stats.bad_crc = self._parser.stats.bad_crc
            self.stats.bad_len = self._parser.stats.bad_len

    async def _handshake(self) -> HelloInfo:
        try:
            await asyncio.wait_for(self._hello_event.wait(), min(1.0, self._hello_timeout))
        except TimeoutError:
            await self.send(g.HelloReq())
            try:
                await asyncio.wait_for(self._hello_event.wait(), self._hello_timeout)
            except TimeoutError as exc:
                raise HandshakeError("no HELLO from the tank (power, cable, S1 switch?)") from exc
        hello = self._hello_msg
        assert hello is not None
        if hello.proto != g.PROTOCOL_VERSION:
            raise HandshakeError(
                f"protocol mismatch: tank {hello.proto}, bench {g.PROTOCOL_VERSION} (reflash)"
            )
        if self._envelope is not None:
            speed, accel, lock = self._envelope
            res = await self.send(g.Envelope(speed_cap=speed, accel_cap=accel, lock_brake=lock))
            if not res.ok:
                raise HandshakeError(f"envelope not accepted: {res.status}")
        await self.send(g.HelloAck(proto=g.PROTOCOL_VERSION))
        try:
            reset = g.ResetReason(hello.reset_reason).name
        except ValueError:
            reset = "UNKNOWN"
        return HelloInfo(hello.fw >> 8, hello.fw & 0xFF, hello.proto, bool(hello.caps & 1), reset)
```

- [ ] **Step 5: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_link.py -v`
Expected: 9 passed.

- [ ] **Step 6: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/link.py tools/bench/tests/conftest.py tools/bench/tests/test_link.py
git commit -m "feat(bench): link session with heartbeat, handshake, acks and STOP fast path"
```

---

### Task 4: Tank model — telemetry decoding

**Files:**
- Create: `tools/bench/conqueror_bench/model.py`
- Test: `tools/bench/tests/test_model.py`

**Interfaces:**
- Consumes: `conqueror_protocol.generated` (`TelemFast`, `TelemSlow`, `Event`, `SettingValue`, `CalData`, `CalResult`, enums).
- Produces: `FastSample` (fields: `host_t, uno_ms, yaw_deg, yaw_rate_dps, pitch_deg, roll_deg, spd_cmd: tuple[int,int], spd_act: tuple[int|None,int|None], pwm: tuple[int,int], dist_cm: float|None, us_fault: bool, closing_cm_s: int, pan_deg, tilt_deg, enc: tuple[int,int], flags: frozenset[str], prim: str, prim_pct: int`); `SlowSample` (`host_t, uptime_s, reset_reason, batt_v, driving, temp_c, owner, flags, accel_var, loop_load, loop_max_us, free_ram, err_frames, err_i2c, settings_crc, envelope: tuple[int,int,int], profile_idx, cal_status: tuple[int, ...], hw: frozenset[str]`); `TankEvent(host_t, kind: str, a: int, b: int)`; `decode_fast(m, host_t) -> FastSample`; `decode_slow(m, host_t) -> SlowSample`; `TankModel()` with `.fast`, `.slow`, `.events: deque[TankEvent]` (200), `.history: deque[FastSample]` (12000 ≈ 10 min), `apply(msg, host_t=None)`, `subscribe(cb: Callable[[str, Any], None])` topics `"fast" | "slow" | "event" | "setting_value" | "cal_data" | "cal_result"`.

- [ ] **Step 1: Write the failing model tests**

`tools/bench/tests/test_model.py`:
```python
from typing import Any

from conqueror_protocol import generated as g

from conqueror_bench.model import TankModel, decode_fast, decode_slow


def _fast(**overrides: int) -> g.TelemFast:
    base: dict[str, int] = dict(
        uno_ms=1000, yaw=35999, yaw_rate=-125, pitch=210, roll=-80, spd_cmd_l=40, spd_cmd_r=42,
        spd_act_l=38, spd_act_r=41, pwm_l=150, pwm_r=155, dist_mm=630, closing_cm_s=12,
        pan=900, tilt=750, enc_l=1000, enc_r=1010,
        flags=(1 << int(g.FastFlagBit.BLOCKED)) | (1 << int(g.FastFlagBit.SYNCED)),
        prim=int(g.PrimitiveKind.MOVE), prim_pct=50,
    )
    base.update(overrides)
    return g.TelemFast(**base)


def test_fast_scaling() -> None:
    s = decode_fast(_fast(), 1.0)
    assert s.yaw_deg == 359.99
    assert s.yaw_rate_dps == -12.5
    assert s.pitch_deg == 2.1 and s.roll_deg == -0.8
    assert s.dist_cm == 63.0
    assert s.pan_deg == 90.0 and s.tilt_deg == 75.0
    assert s.flags == frozenset({"blocked", "synced"})
    assert s.prim == "MOVE"


def test_sentinels_decode_to_none_and_fault() -> None:
    s = decode_fast(_fast(spd_act_l=-128, spd_act_r=-128, dist_mm=0xFFFF), 0.0)
    assert s.spd_act == (None, None)
    assert s.dist_cm is None and not s.us_fault
    f = decode_fast(_fast(dist_mm=0xFFFE), 0.0)
    assert f.dist_cm is None and f.us_fault


def test_slow_decoding() -> None:
    m = g.TelemSlow(
        uptime_s=10, reset_reason=int(g.ResetReason.WATCHDOG), batt_mv=7620, driving=1, temp=3140,
        owner=int(g.Owner.IR), flags2=1 << int(g.SlowFlagBit.BATT_LOW), accel_var=5, loop_load=38,
        loop_max_us=1200, free_ram=310, err_frames=1, err_i2c=0, settings_crc=0xBEEF,
        env_speed_cap=20, env_accel_cap=30, env_lock_brake=3, profile_idx=0,
        cal_status=(0b01000001, 0, 0), hw=0b011,
    )
    s = decode_slow(m, 2.0)
    assert s.batt_v == 7.62 and s.temp_c == 31.4
    assert s.reset_reason == "WATCHDOG" and s.owner == "IR"
    assert s.flags == frozenset({"batt_low"})
    assert s.envelope == (20, 30, 3)
    assert s.cal_status[:4] == (1, 0, 0, 1)
    assert s.hw == frozenset({"encoders_enabled", "imu_ok"})


def test_model_dispatch_and_history() -> None:
    model = TankModel()
    seen: list[tuple[str, Any]] = []
    model.subscribe(lambda topic, payload: seen.append((topic, payload)))
    model.apply(_fast(), 1.0)
    model.apply(g.Event(kind=int(g.EventKind.BUMP), a=1800, b=0), 1.1)
    model.apply(g.SettingValue(id=15, value=300), 1.2)
    assert model.fast is not None and len(model.history) == 1
    assert model.events[-1].kind == "BUMP" and model.events[-1].a == 1800
    assert [t for t, _ in seen] == ["fast", "event", "setting_value"]
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_model.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'conqueror_bench.model'`.

- [ ] **Step 3: Implement the model**

`tools/bench/conqueror_bench/model.py`:
```python
"""Decoded tank state (Uno LLD §9.11 scaling and sentinels)."""

from __future__ import annotations

import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from enum import IntEnum
from typing import Any

from conqueror_protocol import Message
from conqueror_protocol import generated as g

NO_ENCODER = -128
DIST_CLEAR = 0xFFFF
DIST_FAULT = 0xFFFE


@dataclass(frozen=True, slots=True)
class FastSample:
    host_t: float
    uno_ms: int
    yaw_deg: float
    yaw_rate_dps: float
    pitch_deg: float
    roll_deg: float
    spd_cmd: tuple[int, int]
    spd_act: tuple[int | None, int | None]
    pwm: tuple[int, int]
    dist_cm: float | None
    us_fault: bool
    closing_cm_s: int
    pan_deg: float
    tilt_deg: float
    enc: tuple[int, int]
    flags: frozenset[str]
    prim: str
    prim_pct: int


@dataclass(frozen=True, slots=True)
class SlowSample:
    host_t: float
    uptime_s: int
    reset_reason: str
    batt_v: float
    driving: bool
    temp_c: float
    owner: str
    flags: frozenset[str]
    accel_var: int
    loop_load: int
    loop_max_us: int
    free_ram: int
    err_frames: int
    err_i2c: int
    settings_crc: int
    envelope: tuple[int, int, int]
    profile_idx: int
    cal_status: tuple[int, ...]
    hw: frozenset[str]


@dataclass(frozen=True, slots=True)
class TankEvent:
    host_t: float
    kind: str
    a: int
    b: int


def enum_name(cls: type[IntEnum], value: int) -> str:
    try:
        return cls(value).name
    except ValueError:
        return f"UNKNOWN_{value}"


def bit_names(value: int, cls: type[IntEnum]) -> frozenset[str]:
    return frozenset(m.name.lower() for m in cls if value & (1 << m.value))


def _act(v: int) -> int | None:
    return None if v == NO_ENCODER else v


def decode_fast(m: g.TelemFast, host_t: float) -> FastSample:
    dist = None if m.dist_mm in (DIST_CLEAR, DIST_FAULT) else m.dist_mm / 10.0
    return FastSample(
        host_t=host_t,
        uno_ms=m.uno_ms,
        yaw_deg=m.yaw / 100.0,
        yaw_rate_dps=m.yaw_rate / 10.0,
        pitch_deg=m.pitch / 100.0,
        roll_deg=m.roll / 100.0,
        spd_cmd=(m.spd_cmd_l, m.spd_cmd_r),
        spd_act=(_act(m.spd_act_l), _act(m.spd_act_r)),
        pwm=(m.pwm_l, m.pwm_r),
        dist_cm=dist,
        us_fault=m.dist_mm == DIST_FAULT,
        closing_cm_s=m.closing_cm_s,
        pan_deg=m.pan / 10.0,
        tilt_deg=m.tilt / 10.0,
        enc=(m.enc_l, m.enc_r),
        flags=bit_names(m.flags, g.FastFlagBit),
        prim=enum_name(g.PrimitiveKind, m.prim),
        prim_pct=m.prim_pct,
    )


def _cal_status(raw: tuple[int, ...]) -> tuple[int, ...]:
    return tuple((raw[i // 4] >> ((i % 4) * 2)) & 0b11 for i in range(len(raw) * 4))


def decode_slow(m: g.TelemSlow, host_t: float) -> SlowSample:
    return SlowSample(
        host_t=host_t,
        uptime_s=m.uptime_s,
        reset_reason=enum_name(g.ResetReason, m.reset_reason),
        batt_v=m.batt_mv / 1000.0,
        driving=bool(m.driving),
        temp_c=m.temp / 100.0,
        owner=enum_name(g.Owner, m.owner),
        flags=bit_names(m.flags2, g.SlowFlagBit),
        accel_var=m.accel_var,
        loop_load=m.loop_load,
        loop_max_us=m.loop_max_us,
        free_ram=m.free_ram,
        err_frames=m.err_frames,
        err_i2c=m.err_i2c,
        settings_crc=m.settings_crc,
        envelope=(m.env_speed_cap, m.env_accel_cap, m.env_lock_brake),
        profile_idx=m.profile_idx,
        cal_status=_cal_status(m.cal_status),
        hw=bit_names(m.hw, g.HwBit),
    )


class TankModel:
    def __init__(self) -> None:
        self.fast: FastSample | None = None
        self.slow: SlowSample | None = None
        self.events: deque[TankEvent] = deque(maxlen=200)
        self.history: deque[FastSample] = deque(maxlen=12000)
        self._subs: list[Callable[[str, Any], None]] = []

    def subscribe(self, cb: Callable[[str, Any], None]) -> None:
        self._subs.append(cb)

    def _notify(self, topic: str, payload: Any) -> None:
        for cb in list(self._subs):
            cb(topic, payload)

    def apply(self, msg: Message, host_t: float | None = None) -> None:
        t = time.monotonic() if host_t is None else host_t
        if isinstance(msg, g.TelemFast):
            self.fast = decode_fast(msg, t)
            self.history.append(self.fast)
            self._notify("fast", self.fast)
        elif isinstance(msg, g.TelemSlow):
            self.slow = decode_slow(msg, t)
            self._notify("slow", self.slow)
        elif isinstance(msg, g.Event):
            event = TankEvent(t, enum_name(g.EventKind, msg.kind), msg.a, msg.b)
            self.events.append(event)
            self._notify("event", event)
        elif isinstance(msg, g.SettingValue):
            self._notify("setting_value", msg)
        elif isinstance(msg, g.CalData):
            self._notify("cal_data", msg)
        elif isinstance(msg, g.CalResult):
            self._notify("cal_result", msg)
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_model.py -v`
Expected: 4 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/model.py tools/bench/tests/test_model.py
git commit -m "feat(bench): tank model decoding telemetry, events and sentinels"
```

---

### Task 5: Command API and the `Tank` facade

**Files:**
- Create: `tools/bench/conqueror_bench/api.py`
- Test: `tools/bench/tests/test_api.py`

**Interfaces:**
- Consumes: `LinkSession`, `TankModel`, `TankEvent`, `AckResult`.
- Produces: `PrimitiveDone(kind: str, ok: bool, reason: str)`; `primitive_done_from(e: TankEvent) -> PrimitiveDone`; `setting_info(name: str) -> SettingInfo`; `CommandApi(link, model, src=g.Source.HUMAN)` with async methods `stop() -> AckResult`, `drive(v: int, w: int)`, `drive_rate(v: int, yaw_rate_dps: float)`, `turn(*, by_deg: float | None = None, to_deg: float | None = None, speed: int, radius_cm: int = 0, pivot: bool = False) -> AckResult`, `move(dist_cm: float, speed: int, heading_deg: float | None = None) -> AckResult`, `cancel()`, `cam_joy(x, y, active)`, `cam_set(pan_deg, tilt_deg, speed_dps=120)`, `cam_nudge(dpan_deg, dtilt_deg)`, `cam_home()`, `cam_lock(on)`, `led(mode: g.LedMode, r, g_, b, period_ms=0, duration_ms=0)`, `sound(sound: g.SoundId)`, `tone(notes: list[tuple[int,int]])`, `audio_mode(mode: g.AudioModeValue)`, `brake(on: bool)`, `envelope(speed_cap, accel_cap, lock_brake)`, `get_setting_raw(sid: int) -> int`, `set_setting_raw(sid: int, raw: int) -> AckResult`, `get_setting(name) -> float`, `set_setting(name, value: float) -> AckResult`, `settings_save(timeout_s=5.0) -> bool`, `calibrate(script: g.CalScript, param: int, profile: int = 0) -> AckResult`, `cal_cancel() -> AckResult`, `wait_primitive_done(timeout_s) -> PrimitiveDone`, `clear_primitive_results()`, `wait_event(kind: str, timeout_s: float, after_t: float = 0.0) -> TankEvent | None`; `Tank(link, model, api)`; `open_tank(transport, *, envelope=None, hello_timeout_s=3.0, heartbeat_s=0.1) -> AsyncContextManager[Tank]`.

- [ ] **Step 1: Write the failing API tests**

`tools/bench/tests/test_api.py`:
```python
import pytest
from conqueror_protocol import generated as g

from conqueror_bench.api import open_tank

from .conftest import SimRig


async def test_move_and_wait_done(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        tank.api.clear_primitive_results()
        res = await tank.api.move(20.0, speed=100)
        assert res.ok
        done = await tank.api.wait_primitive_done(3.0)
        assert done.ok and done.kind == "MOVE"
        assert 19.0 < rig.uno.x_cm < 21.0


async def test_turn_by_left(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        tank.api.clear_primitive_results()
        assert (await tank.api.turn(by_deg=90, speed=100)).ok
        assert (await tank.api.wait_primitive_done(3.0)).ok
        assert rig.uno.yaw_deg == pytest.approx(90.0, abs=0.5)


async def test_settings_by_name(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        assert await tank.api.get_setting("counts_per_cm") == pytest.approx(76.0)
        assert (await tank.api.set_setting("batt_low", 7100)).ok
        assert await tank.api.get_setting("batt_low") == 7100
        assert await tank.api.settings_save()
        assert rig.uno.saved_settings[int(g.SettingId.BATT_LOW)] == 7100


async def test_unknown_setting_name_raises(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        with pytest.raises(KeyError):
            await tank.api.get_setting("no_such_setting")


async def test_wait_event(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        await rig.uno.emit_event(g.EventKind.LIFTED)
        ev = await tank.api.wait_event("LIFTED", 1.0)
        assert ev is not None and ev.kind == "LIFTED"
        assert await tank.api.wait_event("TILTED", 0.1) is None
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_api.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'conqueror_bench.api'`.

- [ ] **Step 3: Implement the API**

`tools/bench/conqueror_bench/api.py`:
```python
"""Typed command API over the link + the Tank facade."""

from __future__ import annotations

import asyncio
from collections.abc import AsyncIterator, Callable
from contextlib import asynccontextmanager
from dataclasses import dataclass
from typing import Any

from conqueror_protocol import SettingInfo
from conqueror_protocol import generated as g

from .link import AckResult, LinkSession
from .model import TankEvent, TankModel, enum_name

HEADING_CURRENT = 32767


@dataclass(frozen=True, slots=True)
class PrimitiveDone:
    kind: str
    ok: bool
    reason: str


def primitive_done_from(e: TankEvent) -> PrimitiveDone:
    ok = bool((e.a >> 8) & 1)
    reason = "OK" if ok else (g.ACK_STATUS[e.b] if 0 <= e.b < len(g.ACK_STATUS) else str(e.b))
    return PrimitiveDone(enum_name(g.PrimitiveKind, e.a & 0xFF), ok, reason)


def setting_info(name: str) -> SettingInfo:
    for info in g.SETTINGS:
        if info.name == name:
            return info
    raise KeyError(f"unknown setting {name!r}")


class CommandApi:
    def __init__(self, link: LinkSession, model: TankModel, src: g.Source = g.Source.HUMAN) -> None:
        self._link = link
        self._model = model
        self._src = int(src)
        self._waiters: list[tuple[Callable[[str, Any], bool], asyncio.Future[Any]]] = []
        self._done: asyncio.Queue[PrimitiveDone] = asyncio.Queue()
        model.subscribe(self._on_update)

    def _on_update(self, topic: str, payload: Any) -> None:
        if topic == "event" and payload.kind == "PRIMITIVE_DONE":
            self._done.put_nowait(primitive_done_from(payload))
        for pred, fut in list(self._waiters):
            if not fut.done() and pred(topic, payload):
                fut.set_result(payload)

    async def _wait(self, pred: Callable[[str, Any], bool], timeout_s: float) -> Any | None:
        fut: asyncio.Future[Any] = asyncio.get_running_loop().create_future()
        entry = (pred, fut)
        self._waiters.append(entry)
        try:
            return await asyncio.wait_for(fut, timeout_s)
        except TimeoutError:
            return None
        finally:
            self._waiters.remove(entry)

    # ---- motion ------------------------------------------------------------------------------
    async def stop(self) -> AckResult:
        return await self._link.stop(g.Source(self._src))

    async def drive(self, v: int, w: int) -> None:
        await self._link.send(g.Drive(src=self._src, v=_clamp(v), w=_clamp(w)))

    async def drive_rate(self, v: int, yaw_rate_dps: float) -> None:
        await self._link.send(
            g.DriveRate(src=self._src, v=_clamp(v), yaw_rate=int(round(yaw_rate_dps * 10)))
        )

    async def turn(
        self,
        *,
        by_deg: float | None = None,
        to_deg: float | None = None,
        speed: int,
        radius_cm: int = 0,
        pivot: bool = False,
    ) -> AckResult:
        if (by_deg is None) == (to_deg is None):
            raise ValueError("give exactly one of by_deg / to_deg")
        mode = int(g.TurnMode.BY if by_deg is not None else g.TurnMode.TO) | (0x80 if pivot else 0)
        angle = by_deg if by_deg is not None else to_deg
        assert angle is not None
        return await self._link.send(
            g.Turn(src=self._src, mode=mode, angle=int(round(angle * 10)), speed=speed,
                   radius_cm=radius_cm)
        )

    async def move(self, dist_cm: float, speed: int, heading_deg: float | None = None) -> AckResult:
        heading = HEADING_CURRENT if heading_deg is None else int(round(heading_deg * 10))
        return await self._link.send(
            g.Move(src=self._src, dist_mm=int(round(dist_cm * 10)), speed=speed, heading=heading)
        )

    async def cancel(self) -> AckResult:
        return await self._link.send(g.Cancel(src=self._src))

    def clear_primitive_results(self) -> None:
        while not self._done.empty():
            self._done.get_nowait()

    async def wait_primitive_done(self, timeout_s: float) -> PrimitiveDone:
        try:
            return await asyncio.wait_for(self._done.get(), timeout_s)
        except TimeoutError:
            return PrimitiveDone("UNKNOWN", False, "TIMEOUT")

    async def wait_event(self, kind: str, timeout_s: float, after_t: float = 0.0) -> TankEvent | None:
        for e in self._model.events:
            if e.kind == kind and e.host_t >= after_t:
                return e
        result = await self._wait(
            lambda topic, p: topic == "event" and p.kind == kind and p.host_t >= after_t, timeout_s
        )
        return result if isinstance(result, TankEvent) else None

    # ---- camera / UI ------------------------------------------------------------------------------
    async def cam_joy(self, x: int, y: int, active: bool) -> None:
        await self._link.send(g.CamJoy(x=_clamp(x), y=_clamp(y), active=int(active)))

    async def cam_set(self, pan_deg: float, tilt_deg: float, speed_dps: int = 120) -> AckResult:
        return await self._link.send(
            g.CamSet(pan=int(pan_deg * 10), tilt=int(tilt_deg * 10), speed=min(255, speed_dps // 2))
        )

    async def cam_nudge(self, dpan_deg: float, dtilt_deg: float) -> AckResult:
        return await self._link.send(g.CamNudge(dpan=int(dpan_deg * 10), dtilt=int(dtilt_deg * 10)))

    async def cam_home(self) -> AckResult:
        return await self._link.send(g.CamHome())

    async def cam_lock(self, on: bool) -> AckResult:
        return await self._link.send(g.CamLock(on=int(on)))

    async def led(
        self, mode: g.LedMode, r: int, g_: int, b: int, period_ms: int = 0, duration_ms: int = 0
    ) -> AckResult:
        return await self._link.send(
            g.Led(mode=int(mode), r=r, g=g_, b=b, period_ms=period_ms, duration_ms=duration_ms)
        )

    async def sound(self, sound: g.SoundId) -> AckResult:
        return await self._link.send(g.Sound(id=int(sound)))

    async def tone(self, notes: list[tuple[int, int]]) -> AckResult:
        return await self._link.send(g.Tone(n=len(notes), notes=tuple(notes)))

    async def audio_mode(self, mode: g.AudioModeValue) -> AckResult:
        return await self._link.send(g.AudioMode(mode=int(mode)))

    async def brake(self, on: bool) -> AckResult:
        return await self._link.send(g.Brake(on=int(on)))

    async def envelope(
        self, speed_cap: int, accel_cap: int, lock_brake: bool, child_play: bool = False
    ) -> AckResult:
        # lock_brake on the wire is a bit set (EnvelopeFlagBit): CHILD_PLAY marks the preset (blocks Override).
        flags = (int(lock_brake) << int(g.EnvelopeFlagBit.LOCK_BRAKE)) | (
            int(child_play) << int(g.EnvelopeFlagBit.CHILD_PLAY)
        )
        return await self._link.send(
            g.Envelope(speed_cap=speed_cap, accel_cap=accel_cap, lock_brake=flags)
        )

    # ---- settings ----------------------------------------------------------------------------------
    async def get_setting_raw(self, sid: int) -> int:
        waiter = asyncio.ensure_future(
            self._wait(lambda t, p: t == "setting_value" and p.id == sid, 1.0)
        )
        res = await self._link.send(g.SettingGet(id=sid))
        if not res.ok:
            waiter.cancel()
            raise KeyError(f"setting {sid}: {res.status}")
        msg = await waiter
        if msg is None:
            raise TimeoutError(f"no SETTING_VALUE for {sid}")
        return int(msg.value)

    async def set_setting_raw(self, sid: int, raw: int) -> AckResult:
        return await self._link.send(g.SettingSet(id=sid, value=raw))

    async def get_setting(self, name: str) -> float:
        info = setting_info(name)
        return await self.get_setting_raw(info.id) * info.scale

    async def set_setting(self, name: str, value: float) -> AckResult:
        info = setting_info(name)
        return await self.set_setting_raw(info.id, int(round(value / info.scale)))

    async def settings_save(self, timeout_s: float = 5.0) -> bool:
        waiter = asyncio.ensure_future(
            self._wait(lambda t, p: t == "event" and p.kind == "SETTINGS_SAVED", timeout_s)
        )
        res = await self._link.send(g.SettingsSave())
        if not res.ok:
            waiter.cancel()
            return False
        return await waiter is not None

    # ---- calibration ------------------------------------------------------------------------------
    async def calibrate(self, script: g.CalScript, param: int, profile: int = 0) -> AckResult:
        return await self._link.send(
            g.Calibrate(op=int(g.CalOp.START), script=int(script), param=param, profile=profile)
        )

    async def cal_cancel(self) -> AckResult:
        return await self._link.send(
            g.Calibrate(op=int(g.CalOp.CANCEL), script=0, param=0, profile=0)
        )


def _clamp(value: int) -> int:
    return max(-100, min(100, int(value)))


@dataclass
class Tank:
    link: LinkSession
    model: TankModel
    api: CommandApi


@asynccontextmanager
async def open_tank(
    transport: Any,
    *,
    envelope: tuple[int, int, int] | None = None,
    hello_timeout_s: float = 3.0,
    heartbeat_s: float = 0.1,
) -> AsyncIterator[Tank]:
    model = TankModel()
    link = LinkSession(
        transport, envelope=envelope, hello_timeout_s=hello_timeout_s, heartbeat_s=heartbeat_s
    )
    link.subscribe(lambda msg, t: model.apply(msg, t))
    async with link:
        yield Tank(link, model, CommandApi(link, model))
```
Note: `get_setting_raw`/`settings_save` arm the waiter with `ensure_future` **before** sending so a fast reply is never missed.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_api.py -v`
Expected: 5 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/api.py tools/bench/tests/test_api.py
git commit -m "feat(bench): typed command API and Tank facade"
```

---

### Task 6: Teleop — cruise-control keyboard driving and hold mode

**Files:**
- Create: `tools/bench/conqueror_bench/teleop.py`
- Test: `tools/bench/tests/test_teleop.py`

**Interfaces:**
- Consumes: `CommandApi`.
- Produces: `DriveCmd(v: int, w: int)`; `KeyAction(kind: str, value: Any = None)` with kinds `"drive" | "stop" | "cam_nudge" | "cam_home" | "quick_test" | "none"`; `CruiseControl` (`SPEED_STEP=10, STEER_STEP=20, SPIN_SPEED=40, SPIN_HOLD_S=0.3, NUDGE_DEG=5.0`; `.speed`, `.steer`; `key(key: str, now: float) -> KeyAction`; `command(now: float) -> DriveCmd`); `Teleop(api, cruise=None, rate_hz=20.0)` (`async handle_key(key) -> KeyAction`, `async run()`, `hold: bool`, `async set_hold_keys(pressed: set[str])`); `hold_state(pressed: set[str], speed: int) -> DriveCmd`; `HoldKeyboard` (pynput wrapper, optional).

- [ ] **Step 1: Write the failing teleop tests**

`tools/bench/tests/test_teleop.py`:
```python
import asyncio

from conqueror_protocol import generated as g

from conqueror_bench.api import open_tank
from conqueror_bench.teleop import CruiseControl, DriveCmd, Teleop, hold_state

from .conftest import SimRig


def test_cruise_speed_steps_and_clamp() -> None:
    c = CruiseControl()
    for _ in range(12):
        c.key("w", 0.0)
    assert c.command(0.0) == DriveCmd(100, 0)
    c.key("s", 0.0)
    assert c.command(0.0).v == 90


def test_cruise_steer_left_is_positive_and_centre() -> None:
    c = CruiseControl()
    c.key("a", 0.0)
    assert c.command(0.0).w == 20
    c.key("d", 0.0)
    c.key("d", 0.0)
    assert c.command(0.0).w == -20
    c.key("c", 0.0)
    assert c.command(0.0).w == 0


def test_spin_is_temporary() -> None:
    c = CruiseControl()
    c.key("q", 10.0)
    assert c.command(10.1) == DriveCmd(0, 40)
    assert c.command(10.5) == DriveCmd(0, 0)


def test_space_stops_and_resets() -> None:
    c = CruiseControl()
    c.key("w", 0.0)
    c.key("a", 0.0)
    action = c.key("space", 0.0)
    assert action.kind == "stop"
    assert c.command(0.0) == DriveCmd(0, 0)


def test_camera_and_quick_test_keys() -> None:
    c = CruiseControl()
    assert c.key("left", 0.0).value == (5.0, 0.0)
    assert c.key("up", 0.0).value == (0.0, 5.0)
    assert c.key("h", 0.0).kind == "cam_home"
    assert c.key("3", 0.0).value == 3
    assert c.key("x", 0.0).kind == "none"


def test_hold_state() -> None:
    assert hold_state({"w"}, 50) == DriveCmd(50, 0)
    assert hold_state({"s", "a"}, 50) == DriveCmd(-50, 40)
    assert hold_state({"e"}, 50) == DriveCmd(0, -40)
    assert hold_state(set(), 50) == DriveCmd(0, 0)


async def test_teleop_streams_drive_and_stops(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        teleop = Teleop(tank.api)
        task = asyncio.create_task(teleop.run())
        await teleop.handle_key("w")
        await asyncio.sleep(0.2)
        drives = [m for m in rig.uno.received if isinstance(m, g.Drive)]
        assert len(drives) >= 3 and drives[-1].v == 10
        await teleop.handle_key("space")
        assert any(isinstance(m, g.Stop) for m in rig.uno.received)
        task.cancel()
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_teleop.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'conqueror_bench.teleop'`.

- [ ] **Step 3: Implement teleop**

`tools/bench/conqueror_bench/teleop.py`:
```python
"""Keyboard driving: cruise control (default) and optional hold-to-drive (pynput)."""

from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass
from typing import Any

from .api import CommandApi


@dataclass(frozen=True, slots=True)
class DriveCmd:
    v: int
    w: int


@dataclass(frozen=True, slots=True)
class KeyAction:
    kind: str
    value: Any = None


_NUDGES: dict[str, tuple[float, float]] = {
    "left": (1.0, 0.0),
    "right": (-1.0, 0.0),
    "up": (0.0, 1.0),
    "down": (0.0, -1.0),
}


class CruiseControl:
    """Terminal-friendly driving: each tap changes a held speed/steer (no key-up events needed)."""

    SPEED_STEP = 10
    STEER_STEP = 20
    SPIN_SPEED = 40
    SPIN_HOLD_S = 0.3
    NUDGE_DEG = 5.0

    def __init__(self) -> None:
        self.speed = 0
        self.steer = 0
        self._spin_dir = 0
        self._spin_until = 0.0

    def key(self, key: str, now: float) -> KeyAction:
        if key == "w":
            self.speed = min(100, self.speed + self.SPEED_STEP)
        elif key == "s":
            self.speed = max(-100, self.speed - self.SPEED_STEP)
        elif key == "a":
            self.steer = min(100, self.steer + self.STEER_STEP)
        elif key == "d":
            self.steer = max(-100, self.steer - self.STEER_STEP)
        elif key == "c":
            self.steer = 0
        elif key in ("q", "e"):
            self._spin_dir = 1 if key == "q" else -1
            self._spin_until = now + self.SPIN_HOLD_S
        elif key == "space":
            self.speed = self.steer = self._spin_dir = 0
            self._spin_until = 0.0
            return KeyAction("stop")
        elif key in _NUDGES:
            dp, dt = _NUDGES[key]
            return KeyAction("cam_nudge", (dp * self.NUDGE_DEG, dt * self.NUDGE_DEG))
        elif key == "h":
            return KeyAction("cam_home")
        elif key.isdigit() and key != "0":
            return KeyAction("quick_test", int(key))
        else:
            return KeyAction("none")
        return KeyAction("drive", self.command(now))

    def command(self, now: float) -> DriveCmd:
        if now < self._spin_until:
            return DriveCmd(0, self._spin_dir * self.SPIN_SPEED)
        return DriveCmd(self.speed, self.steer)


def hold_state(pressed: set[str], speed: int) -> DriveCmd:
    v = speed if "w" in pressed else (-speed if "s" in pressed else 0)
    w = 40 if "a" in pressed else (-40 if "d" in pressed else 0)
    if "q" in pressed:
        return DriveCmd(0, 40)
    if "e" in pressed:
        return DriveCmd(0, -40)
    return DriveCmd(v, w)


class Teleop:
    def __init__(self, api: CommandApi, cruise: CruiseControl | None = None, rate_hz: float = 20.0) -> None:
        self.api = api
        self.cruise = cruise or CruiseControl()
        self.hold = False
        self.hold_speed = 50
        self._pressed: set[str] = set()
        self._period = 1.0 / rate_hz
        self._was_active = False

    async def handle_key(self, key: str) -> KeyAction:
        action = self.cruise.key(key, time.monotonic())
        if action.kind == "stop":
            self._pressed.clear()
            await self.api.stop()
        elif action.kind == "cam_nudge":
            dpan, dtilt = action.value
            await self.api.cam_nudge(dpan, dtilt)
        elif action.kind == "cam_home":
            await self.api.cam_home()
        return action

    async def set_hold_keys(self, pressed: set[str]) -> None:
        self._pressed = set(pressed)

    def current(self) -> DriveCmd:
        if self.hold:
            return hold_state(self._pressed, self.hold_speed)
        return self.cruise.command(time.monotonic())

    async def run(self) -> None:
        while True:
            cmd = self.current()
            active = cmd != DriveCmd(0, 0)
            if active or self._was_active:
                await self.api.drive(cmd.v, cmd.w)
            self._was_active = active
            await asyncio.sleep(self._period)


class HoldKeyboard:
    """Real key-down/key-up via pynput (macOS needs Accessibility permission). Optional extra."""

    def __init__(self, teleop: Teleop, loop: asyncio.AbstractEventLoop) -> None:
        from pynput import keyboard  # pyright: ignore[reportMissingImports]

        self._teleop = teleop
        self._loop = loop
        self._pressed: set[str] = set()
        self._listener: Any = keyboard.Listener(  # pyright: ignore
            on_press=self._on_press, on_release=self._on_release
        )

    def start(self) -> None:
        self._teleop.hold = True
        self._listener.start()

    def stop(self) -> None:
        self._listener.stop()
        self._teleop.hold = False

    def _name(self, key: Any) -> str | None:
        char = getattr(key, "char", None)
        return char.lower() if isinstance(char, str) else None

    def _on_press(self, key: Any) -> None:
        if (name := self._name(key)) is not None:
            self._pressed.add(name)
            asyncio.run_coroutine_threadsafe(self._teleop.set_hold_keys(self._pressed), self._loop)

    def _on_release(self, key: Any) -> None:
        if (name := self._name(key)) is not None:
            self._pressed.discard(name)
            asyncio.run_coroutine_threadsafe(self._teleop.set_hold_keys(self._pressed), self._loop)
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_teleop.py -v`
Expected: 7 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/teleop.py tools/bench/tests/test_teleop.py
git commit -m "feat(bench): cruise-control keyboard driving and optional hold mode"
```

---

### Task 7: Odometry and metrics

**Files:**
- Create: `tools/bench/conqueror_bench/odometry.py`, `tools/bench/conqueror_bench/metrics.py`
- Test: `tools/bench/tests/test_metrics.py`

**Interfaces:**
- Consumes: `FastSample`.
- Produces: `odometry.PathPoint(t, x_cm, y_cm, theta_deg)`; `odometry.wrap_deg(a: float) -> float` (to (−180, 180]); `odometry.integrate(samples: Sequence[FastSample], counts_per_cm: float) -> list[PathPoint]` (midpoint integration, yaw from telemetry, unwrapped θ); `metrics.heading_change_deg(samples) -> float`; `metrics.path_length_cm(path) -> float`; `metrics.closure_cm(path) -> float`; `metrics.fit_circle(path) -> tuple[float, float, float]` (cx, cy, r); `metrics.speed_hold_pct(samples, settle_s=1.0) -> float`; `metrics.StepMetrics(rise_s, overshoot_pct, settling_s, steady_error)`; `metrics.step_metrics(t, y, target, band=0.05) -> StepMetrics`.

- [ ] **Step 1: Write the failing tests**

`tools/bench/tests/test_metrics.py`:
```python
import math

import pytest

from conqueror_bench.metrics import (
    closure_cm,
    fit_circle,
    heading_change_deg,
    path_length_cm,
    speed_hold_pct,
    step_metrics,
)
from conqueror_bench.model import FastSample
from conqueror_bench.odometry import PathPoint, integrate, wrap_deg


def sample(t: float, yaw: float, enc: tuple[int, int], act: tuple[int | None, int | None] = (30, 30),
           cmd: tuple[int, int] = (30, 30)) -> FastSample:
    return FastSample(
        host_t=t, uno_ms=int(t * 1000), yaw_deg=yaw % 360, yaw_rate_dps=0.0, pitch_deg=0.0,
        roll_deg=0.0, spd_cmd=cmd, spd_act=act, pwm=(0, 0), dist_cm=None, us_fault=False,
        closing_cm_s=0, pan_deg=90.0, tilt_deg=90.0, enc=enc, flags=frozenset(), prim="NONE",
        prim_pct=0,
    )


def test_wrap() -> None:
    assert wrap_deg(190) == -170
    assert wrap_deg(-190) == 170


def test_straight_line_odometry() -> None:
    samples = [sample(i * 0.05, 0.0, (i * 76, i * 76)) for i in range(101)]
    path = integrate(samples, 76.0)
    assert path[-1].x_cm == pytest.approx(100.0)
    assert path[-1].y_cm == pytest.approx(0.0)
    assert path_length_cm(path) == pytest.approx(100.0)


def test_heading_change_unwraps_full_turn() -> None:
    samples = [sample(i * 0.05, i * 4.0, (0, 0)) for i in range(91)]  # 0 → 360°
    assert heading_change_deg(samples) == pytest.approx(360.0)
    back = [sample(i * 0.05, 10.0 - i * 1.0, (0, 0)) for i in range(21)]  # 10° → 350°
    assert heading_change_deg(back) == pytest.approx(-20.0)


def test_circle_fit_and_closure() -> None:
    path = [
        PathPoint(i, 50 * math.sin(math.radians(i * 3)), 50 - 50 * math.cos(math.radians(i * 3)), 0)
        for i in range(121)
    ]
    cx, cy, r = fit_circle(path)
    assert r == pytest.approx(50.0, abs=0.01)
    assert (cx, cy) == pytest.approx((0.0, 50.0), abs=0.01)
    assert closure_cm(path) == pytest.approx(0.0, abs=1e-6)


def test_speed_hold() -> None:
    samples = [sample(i * 0.05, 0, (0, 0), act=(30 if i % 2 else 31, 29)) for i in range(80)]
    assert speed_hold_pct(samples) == pytest.approx(100 * 1 / 30, abs=0.01)


def test_speed_hold_without_encoders_is_nan() -> None:
    samples = [sample(i * 0.05, 0, (0, 0), act=(None, None)) for i in range(80)]
    assert math.isnan(speed_hold_pct(samples))


def test_step_metrics() -> None:
    t = [i * 0.01 for i in range(300)]
    y = [10 * (1 - math.exp(-ti / 0.1)) for ti in t]
    m = step_metrics(t, y, 10.0)
    assert m.overshoot_pct == pytest.approx(0.0, abs=0.01)
    assert m.rise_s == pytest.approx(0.1 * math.log(9), abs=0.02)
    assert m.settling_s == pytest.approx(0.1 * math.log(20), abs=0.02)
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_metrics.py -v`
Expected: FAIL — `ModuleNotFoundError`.

- [ ] **Step 3: Implement odometry and metrics**

`tools/bench/conqueror_bench/odometry.py`:
```python
"""Pose integration (same algorithm as the ESP32 PoseEstimator, ESP32 LLD §5.2)."""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass

from .model import FastSample


@dataclass(frozen=True, slots=True)
class PathPoint:
    t: float
    x_cm: float
    y_cm: float
    theta_deg: float


def wrap_deg(a: float) -> float:
    w = (a + 180.0) % 360.0 - 180.0
    return 180.0 if w == -180.0 else w


def unwrapped_yaw(samples: Sequence[FastSample]) -> list[float]:
    if not samples:
        return []
    out = [samples[0].yaw_deg]
    for prev, cur in zip(samples, samples[1:], strict=False):
        out.append(out[-1] + wrap_deg(cur.yaw_deg - prev.yaw_deg))
    return out


def integrate(samples: Sequence[FastSample], counts_per_cm: float) -> list[PathPoint]:
    if not samples:
        return []
    yaw = unwrapped_yaw(samples)
    x = y = 0.0
    path = [PathPoint(samples[0].host_t, 0.0, 0.0, yaw[0])]
    for i in range(1, len(samples)):
        prev, cur = samples[i - 1], samples[i]
        dl = cur.enc[0] - prev.enc[0]
        dr = cur.enc[1] - prev.enc[1]
        d = (dl + dr) / 2.0 / counts_per_cm
        theta = math.radians((yaw[i - 1] + yaw[i]) / 2.0 - yaw[0])
        x += d * math.cos(theta)
        y += d * math.sin(theta)
        path.append(PathPoint(cur.host_t, x, y, yaw[i]))
    return path
```

`tools/bench/conqueror_bench/metrics.py`:
```python
"""Test metrics: heading change, path, circle fit, closure, speed hold, step response."""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np

from .model import FastSample
from .odometry import PathPoint, unwrapped_yaw


def heading_change_deg(samples: Sequence[FastSample]) -> float:
    yaw = unwrapped_yaw(samples)
    return yaw[-1] - yaw[0] if yaw else 0.0


def path_length_cm(path: Sequence[PathPoint]) -> float:
    return sum(
        math.hypot(b.x_cm - a.x_cm, b.y_cm - a.y_cm) for a, b in zip(path, path[1:], strict=False)
    )


def closure_cm(path: Sequence[PathPoint]) -> float:
    if not path:
        return 0.0
    return math.hypot(path[-1].x_cm - path[0].x_cm, path[-1].y_cm - path[0].y_cm)


def fit_circle(path: Sequence[PathPoint]) -> tuple[float, float, float]:
    """Algebraic (Kåsa) least-squares circle fit."""
    xs = np.array([p.x_cm for p in path])
    ys = np.array([p.y_cm for p in path])
    a = np.column_stack([2 * xs, 2 * ys, np.ones_like(xs)])
    rhs = xs**2 + ys**2
    (cx, cy, c), *_ = np.linalg.lstsq(a, rhs, rcond=None)
    r = math.sqrt(float(c) + float(cx) ** 2 + float(cy) ** 2)
    return float(cx), float(cy), r


def speed_hold_pct(samples: Sequence[FastSample], settle_s: float = 1.0) -> float:
    """Worst track deviation from its mean actual speed in the steady window, % of command."""
    if not samples:
        return math.nan
    start = samples[0].host_t + settle_s
    steady = [s for s in samples if s.host_t >= start]
    worst = 0.0
    for side in (0, 1):
        acts = [s.spd_act[side] for s in steady]
        if not acts or any(a is None for a in acts):
            return math.nan
        values = [float(a) for a in acts if a is not None]
        cmd = abs(steady[0].spd_cmd[side]) or 1
        mean = sum(values) / len(values)
        worst = max(worst, max(abs(v - mean) for v in values) / cmd * 100.0)
    return worst


@dataclass(frozen=True, slots=True)
class StepMetrics:
    rise_s: float
    overshoot_pct: float
    settling_s: float
    steady_error: float


def step_metrics(t: Sequence[float], y: Sequence[float], target: float, band: float = 0.05) -> StepMetrics:
    t0 = t[0]
    rise_lo = next((ti for ti, yi in zip(t, y, strict=True) if yi >= 0.1 * target), math.nan)
    rise_hi = next((ti for ti, yi in zip(t, y, strict=True) if yi >= 0.9 * target), math.nan)
    peak = max(y)
    overshoot = max(0.0, (peak - target) / target * 100.0)
    settling = t0
    for ti, yi in zip(t, y, strict=True):
        if abs(yi - target) > band * abs(target):
            settling = ti
    tail = y[-max(1, len(y) // 10):]
    steady_error = target - sum(tail) / len(tail)
    return StepMetrics(rise_hi - rise_lo, overshoot, settling - t0, steady_error)
```
`step_metrics.settling_s` is the time of the last sample outside the ±5 % band (the system has settled after it).

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_metrics.py -v`
Expected: 7 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/odometry.py tools/bench/conqueror_bench/metrics.py tools/bench/tests/test_metrics.py
git commit -m "feat(bench): odometry and test metrics"
```

---

### Task 8: Session recorder and replay

**Files:**
- Create: `tools/bench/conqueror_bench/recorder.py`
- Test: `tools/bench/tests/test_recorder.py`

**Interfaces:**
- Consumes: `FrameSink`, `FastSample`, `TankEvent`, `TankModel`, `conqueror_protocol` parser.
- Produces: `RawRecord(t: float, direction: str, data: bytes)`; `Recorder(base_dir: Path, name: str, now: datetime | None = None)` implementing `FrameSink` (`tx`, `rx`) plus `on_model(topic, payload)`, `save_inputs(dict)`, `save_result(dict)`, `close()`, attribute `path: Path`; files `raw.bin, telemetry.csv, events.jsonl, inputs.json, result.json`, folder `plots/`; `read_raw(path) -> list[RawRecord]`; `async replay_into(session_dir, model, speed=0.0)` (speed 0 = as fast as possible, 1 = real time); `load_fast_csv(path) -> list[FastSample]`.

- [ ] **Step 1: Write the failing tests**

`tools/bench/tests/test_recorder.py`:
```python
import json
from datetime import datetime
from pathlib import Path

from conqueror_protocol import build_frame
from conqueror_protocol import generated as g

from conqueror_bench.model import TankEvent, TankModel, decode_fast
from conqueror_bench.recorder import Recorder, load_fast_csv, read_raw, replay_into


def _telem(enc: int) -> g.TelemFast:
    return g.TelemFast(
        uno_ms=1, yaw=100, yaw_rate=0, pitch=0, roll=0, spd_cmd_l=30, spd_cmd_r=30, spd_act_l=30,
        spd_act_r=30, pwm_l=80, pwm_r=80, dist_mm=0xFFFF, closing_cm_s=0, pan=900, tilt=900,
        enc_l=enc, enc_r=enc, flags=0, prim=0, prim_pct=0,
    )


def test_session_layout_and_round_trip(tmp_path: Path) -> None:
    rec = Recorder(tmp_path, "straight_200cm", now=datetime(2026, 10, 12, 14, 30))
    assert rec.path.name == "2026-10-12_1430_straight_200cm"
    frame = build_frame(_telem(500), 1)
    rec.tx(b"\xaa\x00\x01\x00\x00")
    rec.rx(frame)
    rec.on_model("fast", decode_fast(_telem(500), 2.0))
    rec.on_model("event", TankEvent(2.1, "BUMP", 1500, 0))
    rec.save_inputs({"lateral_cm": 2.5})
    rec.save_result({"passed": True})
    rec.close()
    for name in ("raw.bin", "telemetry.csv", "events.jsonl", "inputs.json", "result.json"):
        assert (rec.path / name).exists(), name
    assert (rec.path / "plots").is_dir()
    raws = read_raw(rec.path / "raw.bin")
    assert [r.direction for r in raws] == ["tx", "rx"]
    assert raws[1].data == frame
    fast = load_fast_csv(rec.path / "telemetry.csv")
    assert fast[0].enc == (500, 500) and fast[0].yaw_deg == 1.0
    assert json.loads((rec.path / "events.jsonl").read_text().splitlines()[0])["kind"] == "BUMP"


async def test_replay_rebuilds_model(tmp_path: Path) -> None:
    rec = Recorder(tmp_path, "x")
    for i in range(5):
        rec.rx(build_frame(_telem(i * 10), i))
    rec.close()
    model = TankModel()
    await replay_into(rec.path, model)
    assert len(model.history) == 5
    assert model.fast is not None and model.fast.enc == (40, 40)
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_recorder.py -v`
Expected: FAIL — `ModuleNotFoundError`.

- [ ] **Step 3: Implement recorder and replay**

`tools/bench/conqueror_bench/recorder.py`:
```python
"""Session recording (raw frames + decoded telemetry/events) and offline replay."""

from __future__ import annotations

import asyncio
import csv
import json
import struct
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, TextIO

from conqueror_protocol import FrameParser, decode_frame

from .model import FastSample, TankEvent, TankModel

_HEADER = struct.Struct("<dBH")
_DIRS = {"tx": 0, "rx": 1}
FAST_COLUMNS = [
    "host_t", "uno_ms", "yaw_deg", "yaw_rate_dps", "pitch_deg", "roll_deg", "spd_cmd_l",
    "spd_cmd_r", "spd_act_l", "spd_act_r", "pwm_l", "pwm_r", "dist_cm", "us_fault",
    "closing_cm_s", "pan_deg", "tilt_deg", "enc_l", "enc_r", "flags", "prim", "prim_pct",
]


@dataclass(frozen=True, slots=True)
class RawRecord:
    t: float
    direction: str
    data: bytes


class Recorder:
    def __init__(self, base_dir: Path, name: str, now: datetime | None = None) -> None:
        stamp = (now or datetime.now()).strftime("%Y-%m-%d_%H%M")
        self.path = base_dir / f"{stamp}_{name}"
        (self.path / "plots").mkdir(parents=True, exist_ok=True)
        self._raw = (self.path / "raw.bin").open("wb")
        self._csv_file: TextIO = (self.path / "telemetry.csv").open("w", newline="")
        self._csv = csv.writer(self._csv_file)
        self._csv.writerow(FAST_COLUMNS)
        self._events: TextIO = (self.path / "events.jsonl").open("w")

    def _write_raw(self, direction: str, data: bytes) -> None:
        self._raw.write(_HEADER.pack(time.time(), _DIRS[direction], len(data)) + data)

    def tx(self, data: bytes) -> None:
        self._write_raw("tx", data)

    def rx(self, data: bytes) -> None:
        self._write_raw("rx", data)

    def on_model(self, topic: str, payload: Any) -> None:
        if topic == "fast" and isinstance(payload, FastSample):
            s = payload
            self._csv.writerow([
                s.host_t, s.uno_ms, s.yaw_deg, s.yaw_rate_dps, s.pitch_deg, s.roll_deg,
                s.spd_cmd[0], s.spd_cmd[1], _opt(s.spd_act[0]), _opt(s.spd_act[1]), s.pwm[0],
                s.pwm[1], _opt(s.dist_cm), int(s.us_fault), s.closing_cm_s, s.pan_deg, s.tilt_deg,
                s.enc[0], s.enc[1], "|".join(sorted(s.flags)), s.prim, s.prim_pct,
            ])
        elif topic == "event" and isinstance(payload, TankEvent):
            self._events.write(json.dumps(asdict(payload)) + "\n")

    def save_inputs(self, inputs: dict[str, Any]) -> None:
        (self.path / "inputs.json").write_text(json.dumps(inputs, indent=2))

    def save_result(self, result: dict[str, Any]) -> None:
        (self.path / "result.json").write_text(json.dumps(result, indent=2, default=str))

    def close(self) -> None:
        for f in (self._raw, self._csv_file, self._events):
            f.close()
        for name in ("inputs.json", "result.json"):
            if not (self.path / name).exists():
                (self.path / name).write_text("{}")


def _opt(value: float | int | None) -> str:
    return "" if value is None else str(value)


def read_raw(path: Path) -> list[RawRecord]:
    data = path.read_bytes()
    out: list[RawRecord] = []
    pos = 0
    while pos + _HEADER.size <= len(data):
        t, d, n = _HEADER.unpack_from(data, pos)
        pos += _HEADER.size
        out.append(RawRecord(t, "tx" if d == 0 else "rx", data[pos : pos + n]))
        pos += n
    return out


async def replay_into(session_dir: Path, model: TankModel, speed: float = 0.0) -> None:
    parser = FrameParser()
    records = [r for r in read_raw(session_dir / "raw.bin") if r.direction == "rx"]
    t0 = records[0].t if records else 0.0
    wall0 = time.monotonic()
    for r in records:
        if speed > 0:
            await asyncio.sleep(max(0.0, (r.t - t0) / speed - (time.monotonic() - wall0)))
        for raw in parser.feed(r.data):
            try:
                model.apply(decode_frame(raw, "uno_to_esp32"), r.t)
            except ValueError:
                continue


def _int_or_none(text: str) -> int | None:
    return None if text == "" else int(float(text))


def load_fast_csv(path: Path) -> list[FastSample]:
    rows: list[FastSample] = []
    with path.open(newline="") as fh:
        for r in csv.DictReader(fh):
            rows.append(
                FastSample(
                    host_t=float(r["host_t"]), uno_ms=int(r["uno_ms"]), yaw_deg=float(r["yaw_deg"]),
                    yaw_rate_dps=float(r["yaw_rate_dps"]), pitch_deg=float(r["pitch_deg"]),
                    roll_deg=float(r["roll_deg"]),
                    spd_cmd=(int(r["spd_cmd_l"]), int(r["spd_cmd_r"])),
                    spd_act=(_int_or_none(r["spd_act_l"]), _int_or_none(r["spd_act_r"])),
                    pwm=(int(r["pwm_l"]), int(r["pwm_r"])),
                    dist_cm=None if r["dist_cm"] == "" else float(r["dist_cm"]),
                    us_fault=r["us_fault"] == "1", closing_cm_s=int(r["closing_cm_s"]),
                    pan_deg=float(r["pan_deg"]), tilt_deg=float(r["tilt_deg"]),
                    enc=(int(r["enc_l"]), int(r["enc_r"])),
                    flags=frozenset(f for f in r["flags"].split("|") if f),
                    prim=r["prim"], prim_pct=int(r["prim_pct"]),
                )
            )
    return rows
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_recorder.py -v`
Expected: 2 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/recorder.py tools/bench/tests/test_recorder.py
git commit -m "feat(bench): session recorder and replay"
```

---

### Task 9: Test runner, prompter and the six standard tests

**Files:**
- Create: `tools/bench/conqueror_bench/prompter.py`, `tools/bench/conqueror_bench/runner.py`, `tools/bench/tests_defs/straight_200cm.yaml`, `circle_r50.yaml`, `square_100.yaml`, `spin_360.yaml`, `speed_hold_30.yaml`, `safety_checks.yaml`
- Test: `tools/bench/tests/test_runner.py`

**Interfaces:**
- Consumes: `Tank`, `PrimitiveDone`, `Recorder`, `integrate`, metrics.
- Produces: `Prompter` (Protocol: `async ask(prompt: str) -> str`, `async say(text: str) -> None`); `ConsolePrompter`; `ScriptedPrompter(answers: dict[str, str] | None = None, default: str = "")` (matches answers by input id inside the prompt `[id]`, records `.said: list[str]`); `Criterion(lt, gt, abs_lt, eq)` with `passes(value) -> bool`; `InputDef(id, prompt, unit)`; `MetricDef(name, fn, args)`; `TestDefinition(name, description, setup_prompt, needs_encoders, steps, inputs, metrics, pass_)` (YAML key `pass`); `load_test(path) -> TestDefinition`; `RunContext`; `METRICS: dict[str, Callable[[RunContext, dict[str, Any]], float]]`; `TestResult(name, metrics, verdicts, passed, inputs, session_dir, notes)` with `to_dict()`; `TestRunner(tank, prompter, sessions_dir)` with `async run(definition) -> TestResult`; `DEFS_DIR: Path` (the `tests_defs` folder).

- [ ] **Step 1: Write the six test definitions**

`tools/bench/tests_defs/straight_200cm.yaml`:
```yaml
name: straight_200cm
description: Drive 200 cm straight at 40 %.
needs_encoders: true
setup_prompt: Place the tank at the tape start, facing along the tape, then press Enter.
steps:
  - set_origin: {}
  - move: { dist_cm: 200, speed: 40 }
  - wait_done: { timeout_s: 20 }
inputs:
  - { id: lateral_cm, prompt: "Sideways offset from the tape line (cm, left +)", unit: cm }
  - { id: distance_cm, prompt: "Distance travelled along the tape (cm)", unit: cm }
metrics:
  - { name: heading_error_deg, fn: heading_change, args: { target_deg: 0 } }
  - { name: drift_cm, fn: input, args: { id: lateral_cm, abs: true } }
  - { name: distance_error_pct, fn: pct_error, args: { id: distance_cm, target: 200 } }
  - { name: odometry_vs_tape_pct, fn: odometry_vs_input_pct, args: { id: distance_cm } }
  - { name: primitives_ok, fn: primitives_ok }
pass:
  drift_cm: { lt: 5 }
  heading_error_deg: { abs_lt: 2 }
  distance_error_pct: { abs_lt: 3 }
  primitives_ok: { eq: 1 }
```

`tools/bench/tests_defs/circle_r50.yaml`:
```yaml
name: circle_r50
description: Arc 360° on a 50 cm radius at 30 %.
needs_encoders: true
setup_prompt: Mark the start point under the tank's centre, then press Enter.
steps:
  - set_origin: {}
  - turn: { by_deg: 360, speed: 30, radius_cm: 50 }
  - wait_done: { timeout_s: 30 }
inputs:
  - { id: closure_tape_cm, prompt: "Distance from the start mark to the tank centre (cm)", unit: cm }
metrics:
  - { name: closure_tape_cm, fn: input, args: { id: closure_tape_cm, abs: true } }
  - { name: closure_odometry_cm, fn: closure_cm }
  - { name: radius_error_pct, fn: circle_radius_error_pct, args: { target_cm: 50 } }
  - { name: primitives_ok, fn: primitives_ok }
pass:
  closure_tape_cm: { lt: 10 }
  radius_error_pct: { abs_lt: 10 }
  primitives_ok: { eq: 1 }
```

`tools/bench/tests_defs/square_100.yaml`:
```yaml
name: square_100
description: Four times (move 100 cm + turn 90°).
needs_encoders: true
setup_prompt: Mark the start point and heading, then press Enter.
steps:
  - set_origin: {}
  - repeat:
      times: 4
      steps:
        - move: { dist_cm: 100, speed: 40 }
        - wait_done: { timeout_s: 15 }
        - turn: { by_deg: 90, speed: 40 }
        - wait_done: { timeout_s: 10 }
inputs:
  - { id: closure_tape_cm, prompt: "Distance from the start mark to the tank centre (cm)", unit: cm }
metrics:
  - { name: closure_tape_cm, fn: input, args: { id: closure_tape_cm, abs: true } }
  - { name: closure_odometry_cm, fn: closure_cm }
  - { name: primitives_ok, fn: primitives_ok }
pass:
  closure_tape_cm: { lt: 15 }
  primitives_ok: { eq: 1 }
```

`tools/bench/tests_defs/spin_360.yaml`:
```yaml
name: spin_360
description: Turn 360° in place at 40 %.
needs_encoders: false
setup_prompt: Put the tank on open floor, then press Enter.
steps:
  - set_origin: {}
  - turn: { by_deg: 360, speed: 40 }
  - wait_done: { timeout_s: 15 }
inputs: []
metrics:
  - { name: heading_error_deg, fn: heading_change, args: { target_deg: 360 } }
  - { name: primitives_ok, fn: primitives_ok }
pass:
  heading_error_deg: { abs_lt: 2 }
  primitives_ok: { eq: 1 }
```

`tools/bench/tests_defs/speed_hold_30.yaml`:
```yaml
name: speed_hold_30
description: Hold 30 % for 4 s; run once on tile and once on carpet and compare sessions.
needs_encoders: true
setup_prompt: Clear 2 m of floor ahead, then press Enter.
steps:
  - set_origin: {}
  - drive_for: { v: 30, w: 0, seconds: 4 }
inputs: []
metrics:
  - { name: speed_dev_pct, fn: speed_hold_pct }
pass:
  speed_dev_pct: { lt: 5 }
```

`tools/bench/tests_defs/safety_checks.yaml`:
```yaml
name: safety_checks
description: Link-loss stop, obstacle brake, lift detection.
needs_encoders: false
setup_prompt: Open floor, an obstacle (box) at hand, adult present. Press Enter.
steps:
  - expect_event:
      kind: LINK_LOST
      within_s: 2
      as: link_loss_stop
      during:
        - drive_for: { v: 20, w: 0, seconds: 0.5 }
        - simulate_link_loss: { seconds: 0.8 }
  - prompt: { text: "Place the box about 40 cm in front of the tank, then press Enter." }
  - expect_event:
      kind: BLOCKED
      within_s: 15
      as: obstacle_brake
      during:
        - move: { dist_cm: 100, speed: 30 }
        - wait_done: { timeout_s: 15 }
  - expect_event:
      kind: LIFTED
      within_s: 15
      as: lift_detected
      during:
        - prompt: { text: "Lift the tank off the floor now, then press Enter." }
inputs: []
metrics:
  - { name: link_loss_stop, fn: flag, args: { name: link_loss_stop } }
  - { name: obstacle_brake, fn: flag, args: { name: obstacle_brake } }
  - { name: lift_detected, fn: flag, args: { name: lift_detected } }
pass:
  link_loss_stop: { eq: 1 }
  obstacle_brake: { eq: 1 }
  lift_detected: { eq: 1 }
```

- [ ] **Step 2: Write the failing runner tests**

`tools/bench/tests/test_runner.py`:
```python
import asyncio
from pathlib import Path

import pytest
from conqueror_protocol import generated as g

from conqueror_bench.api import open_tank
from conqueror_bench.prompter import ScriptedPrompter
from conqueror_bench.runner import DEFS_DIR, Criterion, TestRunner, load_test

from .conftest import SimRig


def test_all_standard_definitions_load() -> None:
    names = {load_test(p).name for p in DEFS_DIR.glob("*.yaml")}
    assert names == {
        "straight_200cm", "circle_r50", "square_100", "spin_360", "speed_hold_30", "safety_checks"
    }


def test_criterion() -> None:
    assert Criterion(lt=5).passes(4.9) and not Criterion(lt=5).passes(5)
    assert Criterion(abs_lt=2).passes(-1.5) and not Criterion(abs_lt=2).passes(-2.5)
    assert Criterion(eq=1).passes(1.0)
    assert not Criterion(lt=5).passes(float("nan"))


async def test_straight_end_to_end(rig: SimRig, tmp_path: Path) -> None:
    prompter = ScriptedPrompter({"lateral_cm": "1.5", "distance_cm": "199"})
    async with open_tank(rig.bench) as tank:
        result = await TestRunner(tank, prompter, tmp_path).run(
            load_test(DEFS_DIR / "straight_200cm.yaml")
        )
    assert result.passed, result.verdicts
    assert result.metrics["drift_cm"] == 1.5
    assert result.metrics["odometry_vs_tape_pct"] == pytest.approx(0.5, abs=1.0)
    assert (result.session_dir / "result.json").exists()


async def test_spin_end_to_end(rig: SimRig, tmp_path: Path) -> None:
    async with open_tank(rig.bench) as tank:
        result = await TestRunner(tank, ScriptedPrompter(), tmp_path).run(
            load_test(DEFS_DIR / "spin_360.yaml")
        )
    assert result.metrics["heading_error_deg"] == pytest.approx(0.0, abs=1.0)
    assert result.passed


async def test_move_link_lost_reports_failure(rig: SimRig, tmp_path: Path) -> None:
    rig.uno.cfg.max_speed_cm_s = 10.0
    async with open_tank(rig.bench) as tank:
        runner = TestRunner(tank, ScriptedPrompter({"lateral_cm": "0", "distance_cm": "1"}), tmp_path)
        definition = load_test(DEFS_DIR / "straight_200cm.yaml")
        run = asyncio.create_task(runner.run(definition))
        await asyncio.sleep(0.3)
        await tank.link.suspend(0.6)
        result = await asyncio.wait_for(run, 30)
    assert not result.passed
    assert result.verdicts["primitives_ok"] is False
    assert any("LINK_LOST" in n for n in result.notes)


async def test_safety_link_loss_flag(rig: SimRig, tmp_path: Path) -> None:
    async def lift_later() -> None:
        await asyncio.sleep(0.2)
        await rig.uno.emit_event(g.EventKind.LIFTED)

    rig.uno.cfg.obstacle_cm = 40.0
    prompter = ScriptedPrompter(on_ask=lambda prompt: asyncio.ensure_future(lift_later())
                                if "Lift" in prompt else None)
    async with open_tank(rig.bench) as tank:
        result = await TestRunner(tank, prompter, tmp_path).run(
            load_test(DEFS_DIR / "safety_checks.yaml")
        )
    assert result.metrics == {"link_loss_stop": 1.0, "obstacle_brake": 1.0, "lift_detected": 1.0}
```

- [ ] **Step 3: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_runner.py -v`
Expected: FAIL — `ModuleNotFoundError`.

- [ ] **Step 4: Implement the prompter**

`tools/bench/conqueror_bench/prompter.py`:
```python
"""Prompter port: how services ask the human things (console, scripted for tests, UI)."""

from __future__ import annotations

import asyncio
import re
from collections.abc import Callable
from typing import Any, Protocol


class Prompter(Protocol):
    async def ask(self, prompt: str) -> str: ...

    async def say(self, text: str) -> None: ...


class ConsolePrompter:
    async def ask(self, prompt: str) -> str:
        return (await asyncio.to_thread(input, f"{prompt}: ")).strip()

    async def say(self, text: str) -> None:
        print(text)


class ScriptedPrompter:
    """Answers prompts tagged `[input_id]` from a dict; anything else gets `default`."""

    def __init__(
        self,
        answers: dict[str, str] | None = None,
        default: str = "",
        on_ask: Callable[[str], Any] | None = None,
    ) -> None:
        self.answers = answers or {}
        self.default = default
        self.on_ask = on_ask
        self.asked: list[str] = []
        self.said: list[str] = []

    async def ask(self, prompt: str) -> str:
        self.asked.append(prompt)
        if self.on_ask is not None:
            self.on_ask(prompt)
        match = re.search(r"\[(\w+)\]", prompt)
        if match and match.group(1) in self.answers:
            return self.answers[match.group(1)]
        return self.default

    async def say(self, text: str) -> None:
        self.said.append(text)
```

- [ ] **Step 5: Implement the runner**

`tools/bench/conqueror_bench/runner.py`:
```python
"""YAML-defined tests: steps → recorded run → named metrics → pass/fail."""

from __future__ import annotations

import asyncio
import math
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml
from pydantic import BaseModel, ConfigDict
from pydantic import Field as PField

from .api import PrimitiveDone, Tank
from .metrics import closure_cm, fit_circle, heading_change_deg, path_length_cm, speed_hold_pct
from .model import FastSample, TankEvent
from .odometry import integrate
from .prompter import Prompter
from .recorder import Recorder

DEFS_DIR = Path(__file__).resolve().parents[1] / "tests_defs"
DRIVE_PERIOD_S = 0.05


class Criterion(BaseModel):
    lt: float | None = None
    gt: float | None = None
    abs_lt: float | None = None
    eq: float | None = None

    def passes(self, value: float) -> bool:
        if math.isnan(value):
            return False
        checks = [
            self.lt is None or value < self.lt,
            self.gt is None or value > self.gt,
            self.abs_lt is None or abs(value) < self.abs_lt,
            self.eq is None or math.isclose(value, self.eq),
        ]
        return all(checks)


class InputDef(BaseModel):
    id: str
    prompt: str
    unit: str = ""


class MetricDef(BaseModel):
    name: str
    fn: str
    args: dict[str, Any] = PField(default_factory=dict[str, Any])


class TestDefinition(BaseModel):
    model_config = ConfigDict(populate_by_name=True)
    __test__ = False  # not a pytest class

    name: str
    description: str = ""
    setup_prompt: str = ""
    needs_encoders: bool = False
    steps: list[dict[str, Any]]
    inputs: list[InputDef] = PField(default_factory=list[InputDef])
    metrics: list[MetricDef]
    pass_: dict[str, Criterion] = PField(alias="pass")


def load_test(path: Path) -> TestDefinition:
    return TestDefinition.model_validate(yaml.safe_load(path.read_text()))


@dataclass
class RunContext:
    samples: list[FastSample] = field(default_factory=list[FastSample])
    events: list[TankEvent] = field(default_factory=list[TankEvent])
    primitives: list[PrimitiveDone] = field(default_factory=list[PrimitiveDone])
    inputs: dict[str, float] = field(default_factory=dict[str, float])
    flags: dict[str, bool] = field(default_factory=dict[str, bool])
    notes: list[str] = field(default_factory=list[str])
    origin_index: int = 0
    counts_per_cm: float = 76.0

    def window(self) -> list[FastSample]:
        return self.samples[self.origin_index :]


def _m_heading_change(ctx: RunContext, args: dict[str, Any]) -> float:
    return heading_change_deg(ctx.window()) - float(args.get("target_deg", 0.0))


def _m_input(ctx: RunContext, args: dict[str, Any]) -> float:
    value = ctx.inputs.get(str(args["id"]), math.nan)
    return abs(value) if args.get("abs") else value


def _m_pct_error(ctx: RunContext, args: dict[str, Any]) -> float:
    target = float(args["target"])
    return (ctx.inputs.get(str(args["id"]), math.nan) - target) / target * 100.0


def _m_odometry_vs_input(ctx: RunContext, args: dict[str, Any]) -> float:
    measured = ctx.inputs.get(str(args["id"]), math.nan)
    length = path_length_cm(integrate(ctx.window(), ctx.counts_per_cm))
    return (length - measured) / measured * 100.0


def _m_closure(ctx: RunContext, args: dict[str, Any]) -> float:
    return closure_cm(integrate(ctx.window(), ctx.counts_per_cm))


def _m_circle_radius_error(ctx: RunContext, args: dict[str, Any]) -> float:
    target = float(args["target_cm"])
    path = integrate(ctx.window(), ctx.counts_per_cm)
    if len(path) < 10:
        return math.nan
    return (fit_circle(path)[2] - target) / target * 100.0


def _m_speed_hold(ctx: RunContext, args: dict[str, Any]) -> float:
    return speed_hold_pct([s for s in ctx.window() if s.spd_cmd != (0, 0)])


def _m_flag(ctx: RunContext, args: dict[str, Any]) -> float:
    return 1.0 if ctx.flags.get(str(args["name"])) else 0.0


def _m_primitives_ok(ctx: RunContext, args: dict[str, Any]) -> float:
    return 1.0 if ctx.primitives and all(p.ok for p in ctx.primitives) else 0.0


METRICS: dict[str, Callable[[RunContext, dict[str, Any]], float]] = {
    "heading_change": _m_heading_change,
    "input": _m_input,
    "pct_error": _m_pct_error,
    "odometry_vs_input_pct": _m_odometry_vs_input,
    "closure_cm": _m_closure,
    "circle_radius_error_pct": _m_circle_radius_error,
    "speed_hold_pct": _m_speed_hold,
    "flag": _m_flag,
    "primitives_ok": _m_primitives_ok,
}


@dataclass
class TestResult:
    __test__ = False

    name: str
    metrics: dict[str, float]
    verdicts: dict[str, bool]
    passed: bool
    inputs: dict[str, float]
    session_dir: Path
    notes: list[str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "metrics": self.metrics,
            "verdicts": self.verdicts,
            "passed": self.passed,
            "inputs": self.inputs,
            "notes": self.notes,
        }


class TestRunner:
    __test__ = False

    def __init__(self, tank: Tank, prompter: Prompter, sessions_dir: Path) -> None:
        self.tank = tank
        self.prompter = prompter
        self.sessions_dir = sessions_dir

    async def run(self, definition: TestDefinition) -> TestResult:
        for name in [m.fn for m in definition.metrics]:
            if name not in METRICS:
                raise ValueError(f"unknown metric function {name!r}")
        ctx = RunContext()
        rec = Recorder(self.sessions_dir, definition.name)
        api = self.tank.api

        def collect(topic: str, payload: Any) -> None:
            if topic == "fast":
                ctx.samples.append(payload)
            elif topic == "event":
                ctx.events.append(payload)
            rec.on_model(topic, payload)

        self.tank.model.subscribe(collect)
        self.tank.link.set_sink(rec)
        try:
            ctx.counts_per_cm = await api.get_setting("counts_per_cm")
            if definition.setup_prompt:
                await self.prompter.ask(definition.setup_prompt)
            await self._steps(definition.steps, ctx)
            await api.stop()
            for inp in definition.inputs:
                answer = await self.prompter.ask(f"{inp.prompt} [{inp.id}]")
                try:
                    ctx.inputs[inp.id] = float(answer)
                except ValueError:
                    ctx.inputs[inp.id] = math.nan
                    ctx.notes.append(f"input {inp.id}: not a number ({answer!r})")
        finally:
            self.tank.link.set_sink(None)
            self.tank.model._subs.remove(collect)  # pyright: ignore[reportPrivateUsage]
        metrics = {m.name: METRICS[m.fn](ctx, m.args) for m in definition.metrics}
        verdicts = {
            name: crit.passes(metrics.get(name, math.nan)) for name, crit in definition.pass_.items()
        }
        result = TestResult(
            definition.name, metrics, verdicts, all(verdicts.values()), ctx.inputs, rec.path,
            ctx.notes,
        )
        rec.save_inputs(ctx.inputs)
        rec.save_result(result.to_dict())
        rec.close()
        return result

    async def _steps(self, steps: list[dict[str, Any]], ctx: RunContext) -> None:
        for step in steps:
            [(kind, args)] = step.items()
            await self._step(kind, args or {}, ctx)

    async def _step(self, kind: str, args: dict[str, Any], ctx: RunContext) -> None:
        api = self.tank.api
        if kind == "set_origin":
            ctx.origin_index = len(ctx.samples)
        elif kind == "move":
            api.clear_primitive_results()
            res = await api.move(float(args["dist_cm"]), int(args["speed"]), args.get("heading_deg"))
            if not res.ok:
                ctx.primitives.append(PrimitiveDone("MOVE", False, res.status))
                ctx.notes.append(f"move rejected: {res.status}")
        elif kind == "turn":
            api.clear_primitive_results()
            res = await api.turn(
                by_deg=args.get("by_deg"), to_deg=args.get("to_deg"), speed=int(args["speed"]),
                radius_cm=int(args.get("radius_cm", 0)), pivot=bool(args.get("pivot", False)),
            )
            if not res.ok:
                ctx.primitives.append(PrimitiveDone("TURN", False, res.status))
                ctx.notes.append(f"turn rejected: {res.status}")
        elif kind == "wait_done":
            done = await api.wait_primitive_done(float(args.get("timeout_s", 15)))
            ctx.primitives.append(done)
            if not done.ok:
                ctx.notes.append(f"{done.kind} failed: {done.reason}")
        elif kind == "drive_for":
            end = time.monotonic() + float(args["seconds"])
            while time.monotonic() < end:
                await api.drive(int(args["v"]), int(args.get("w", 0)))
                await asyncio.sleep(DRIVE_PERIOD_S)
            await api.drive(0, 0)
        elif kind == "wait":
            await asyncio.sleep(float(args["seconds"]))
        elif kind == "prompt":
            await self.prompter.ask(str(args["text"]))
        elif kind == "repeat":
            for _ in range(int(args["times"])):
                await self._steps(list(args["steps"]), ctx)
        elif kind == "simulate_link_loss":
            await self.tank.link.suspend(float(args["seconds"]))
        elif kind == "expect_event":
            start = time.monotonic()
            await self._steps(list(args.get("during", [])), ctx)
            left = max(0.0, float(args["within_s"]) - (time.monotonic() - start))
            event = await api.wait_event(str(args["kind"]), left, after_t=start)
            ctx.flags[str(args["as"])] = event is not None
            if event is None:
                ctx.notes.append(f"expected event {args['kind']} not seen")
        else:
            raise ValueError(f"unknown step {kind!r}")
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_runner.py -v`
Expected: 6 passed (the link-loss test takes a few seconds).

- [ ] **Step 7: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/prompter.py tools/bench/conqueror_bench/runner.py tools/bench/tests_defs tools/bench/tests/test_runner.py
git commit -m "feat(bench): YAML test runner with six standard tests"
```

---

### Task 10: Reports and run comparison

**Files:**
- Create: `tools/bench/conqueror_bench/reporter.py`
- Test: `tools/bench/tests/test_reporter.py`

**Interfaces:**
- Consumes: `TestResult`, `load_fast_csv`, `integrate`, `unwrapped_yaw`.
- Produces: `write_report(result: TestResult, counts_per_cm: float = 76.0) -> Path` (writes `report.md` and `plots/speed.png`, `plots/heading.png`, `plots/path_xy.png`); `async write_report_async(...)` (runs in a thread); `compare_sessions(a: Path, b: Path, out_dir: Path, counts_per_cm: float = 76.0) -> str` (markdown; writes `compare_path.png`).

- [ ] **Step 1: Write the failing tests**

`tools/bench/tests/test_reporter.py`:
```python
from pathlib import Path

from conqueror_bench.api import open_tank
from conqueror_bench.prompter import ScriptedPrompter
from conqueror_bench.reporter import compare_sessions, write_report
from conqueror_bench.runner import DEFS_DIR, TestRunner, load_test

from .conftest import SimRig


async def test_report_and_compare(rig: SimRig, tmp_path: Path) -> None:
    async with open_tank(rig.bench) as tank:
        runner = TestRunner(tank, ScriptedPrompter({"lateral_cm": "1", "distance_cm": "200"}), tmp_path)
        a = await runner.run(load_test(DEFS_DIR / "straight_200cm.yaml"))
        b = await runner.run(load_test(DEFS_DIR / "straight_200cm.yaml"))
    report = write_report(a)
    text = report.read_text()
    assert "straight_200cm" in text and "PASS" in text
    for name in ("speed.png", "heading.png", "path_xy.png"):
        assert (a.session_dir / "plots" / name).stat().st_size > 0
    md = compare_sessions(a.session_dir, b.session_dir, tmp_path)
    assert "drift_cm" in md
    assert (tmp_path / "compare_path.png").exists()
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_reporter.py -v`
Expected: FAIL — `ModuleNotFoundError`.

- [ ] **Step 3: Implement the reporter**

`tools/bench/conqueror_bench/reporter.py`:
```python
"""Markdown reports, plots (matplotlib Agg, off the event loop) and run comparison."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from .odometry import integrate, unwrapped_yaw  # noqa: E402
from .recorder import load_fast_csv  # noqa: E402
from .runner import TestResult  # noqa: E402


def _plots(session: Path, counts_per_cm: float) -> None:
    samples = load_fast_csv(session / "telemetry.csv")
    if not samples:
        return
    t = [s.host_t - samples[0].host_t for s in samples]
    fig, ax = plt.subplots()
    ax.plot(t, [s.spd_cmd[0] for s in samples], label="cmd L")
    ax.plot(t, [s.spd_cmd[1] for s in samples], label="cmd R")
    ax.plot(t, [s.spd_act[0] if s.spd_act[0] is not None else float("nan") for s in samples], label="act L")
    ax.plot(t, [s.spd_act[1] if s.spd_act[1] is not None else float("nan") for s in samples], label="act R")
    ax.set_xlabel("s")
    ax.set_ylabel("%")
    ax.legend()
    fig.savefig(session / "plots" / "speed.png")
    plt.close(fig)
    fig, ax = plt.subplots()
    ax.plot(t, unwrapped_yaw(samples))
    ax.set_xlabel("s")
    ax.set_ylabel("heading (deg, unwrapped)")
    fig.savefig(session / "plots" / "heading.png")
    plt.close(fig)
    path = integrate(samples, counts_per_cm)
    fig, ax = plt.subplots()
    ax.plot([p.x_cm for p in path], [p.y_cm for p in path])
    ax.set_aspect("equal")
    ax.set_xlabel("x cm")
    ax.set_ylabel("y cm")
    fig.savefig(session / "plots" / "path_xy.png")
    plt.close(fig)


def write_report(result: TestResult, counts_per_cm: float = 76.0) -> Path:
    _plots(result.session_dir, counts_per_cm)
    lines = [
        f"# {result.name} — {'PASS' if result.passed else 'FAIL'}",
        "",
        "| Metric | Value | Verdict |",
        "|---|---|---|",
    ]
    for name, value in result.metrics.items():
        verdict = result.verdicts.get(name)
        mark = "—" if verdict is None else ("PASS" if verdict else "FAIL")
        lines.append(f"| {name} | {value:.2f} | {mark} |")
    lines += ["", "## Inputs", ""]
    lines += [f"- {k}: {v}" for k, v in result.inputs.items()]
    if result.notes:
        lines += ["", "## Notes", ""] + [f"- {n}" for n in result.notes]
    lines += ["", "![speed](plots/speed.png) ![heading](plots/heading.png) ![path](plots/path_xy.png)", ""]
    out = result.session_dir / "report.md"
    out.write_text("\n".join(lines))
    return out


async def write_report_async(result: TestResult, counts_per_cm: float = 76.0) -> Path:
    return await asyncio.to_thread(write_report, result, counts_per_cm)


def compare_sessions(a: Path, b: Path, out_dir: Path, counts_per_cm: float = 76.0) -> str:
    ra = json.loads((a / "result.json").read_text())
    rb = json.loads((b / "result.json").read_text())
    names = sorted(set(ra.get("metrics", {})) | set(rb.get("metrics", {})))
    lines = [f"| Metric | {a.name} | {b.name} |", "|---|---|---|"]
    for n in names:
        va = ra.get("metrics", {}).get(n, float("nan"))
        vb = rb.get("metrics", {}).get(n, float("nan"))
        lines.append(f"| {n} | {va:.2f} | {vb:.2f} |")
    fig, ax = plt.subplots()
    for session in (a, b):
        path = integrate(load_fast_csv(session / "telemetry.csv"), counts_per_cm)
        ax.plot([p.x_cm for p in path], [p.y_cm for p in path], label=session.name)
    ax.set_aspect("equal")
    ax.legend()
    out_dir.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_dir / "compare_path.png")
    plt.close(fig)
    return "\n".join(lines)
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_reporter.py -v`
Expected: 1 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/reporter.py tools/bench/tests/test_reporter.py
git commit -m "feat(bench): markdown reports, plots and run comparison"
```

---

### Task 11: Reference calibration calculators

**Files:**
- Create: `tools/bench/conqueror_bench/calculators.py`
- Test: `tools/bench/tests/test_calculators.py`

**Interfaces:**
- Consumes: `FastSample`, `SlowSample`, `conqueror_protocol.generated.CalScript`.
- Produces: `CalSample(script: int, phase: int, t_ms: int, v: tuple[int, ...])`; `ScriptStep(script: g.CalScript, param: int = 0, label: str = "")`; `InputPrompt(id: str, prompt: str)`; `SettingWrite(name: str, raw: int)`; `CalOutcome(ok: bool, writes: tuple[SettingWrite, ...] = (), error: str = "", details: dict[str, float] = {})`; `CalContext(samples: dict[int, list[CalSample]], inputs: dict[str, float], settings: dict[str, int], fast: list[FastSample], slow: SlowSample | None)`; `Calculator` (Protocol: `item: str`, `settings_needed: tuple[str, ...]`, `scripts() -> list[ScriptStep]`, `prompts() -> list[InputPrompt]`, `compute(ctx) -> CalOutcome`); `CALCULATORS: dict[str, Calculator]` with keys `gyro_bias, level, battery_divider, deadband, encoder_cpr, distance, track_balance, track_width, turn_curve, pid_speed, pid_rate, pid_heading, servo_trim, ultrasonic_offset`; `WIZARD_ORDER: tuple[str, ...]`; `pwm_ramp_param(track: int, reverse: bool) -> int`.

- [ ] **Step 1: Write the failing tests**

`tools/bench/tests/test_calculators.py`:
```python
import math

import pytest

from conqueror_bench.calculators import CALCULATORS, LSB_PER_G, CalContext, CalSample, WIZARD_ORDER
from conqueror_bench.model import FastSample


def ctx(samples: dict[int, list[CalSample]], inputs: dict[str, float] | None = None,
        settings: dict[str, int] | None = None, fast: list[FastSample] | None = None) -> CalContext:
    return CalContext(samples, inputs or {}, settings or {}, fast or [], None)


def writes(outcome: object) -> dict[str, int]:
    return {w.name: w.raw for w in outcome.writes}  # type: ignore[attr-defined]


def test_gyro_bias_mean_and_moved_rejection() -> None:
    still = [CalSample(0, 0, i * 20, (12 + i % 3 - 1, -7, 35, 0, 0, 8192, 2500, 7800)) for i in range(100)]
    assert writes(CALCULATORS["gyro_bias"].compute(ctx({0: still}))) == {
        "gyro_bias_x": 12, "gyro_bias_y": -7, "gyro_bias_z": 35}
    moved = [CalSample(0, 0, i * 20, (0, 0, (i % 2) * 400, 0, 0, 8192, 2500, 7800)) for i in range(100)]
    out = CALCULATORS["gyro_bias"].compute(ctx({0: moved}))
    assert not out.ok and "moved" in out.error


def test_level() -> None:
    ax = int(-LSB_PER_G * math.sin(math.radians(2.0)))
    az = int(LSB_PER_G * math.cos(math.radians(2.0)))
    s = [CalSample(0, 0, 0, (0, 0, 0, ax, 0, az, 0, 0))] * 50
    w = writes(CALCULATORS["level"].compute(ctx({0: s})))
    assert w["level_pitch"] == pytest.approx(200, abs=2)
    assert w["level_roll"] == 0


def ramp(start: int, keep: int) -> list[CalSample]:
    out: list[CalSample] = []
    for phase, pwms, th in ((0, range(0, 256, 2), start), (1, range(254, -1, -2), keep)):
        for pwm in pwms:
            moving = pwm >= th
            out.append(CalSample(1, phase, 0, (pwm, 50 if moving else 0, 0, 400 if moving else 0)))
    return out


def test_deadband_four_runs() -> None:
    samples = {0: ramp(74, 58), 1: ramp(80, 60), 2: ramp(70, 52), 3: ramp(76, 58)}
    w = writes(CALCULATORS["deadband"].compute(ctx(samples)))
    assert w["deadband_start_l_fwd"] == 74 and w["deadband_keep_l_fwd"] == 58
    assert w["deadband_start_l_rev"] == 80 and w["deadband_keep_r_fwd"] == 52


def test_deadband_no_motion_fails() -> None:
    samples = {i: ramp(999, 999) for i in range(4)}
    assert not CALCULATORS["deadband"].compute(ctx(samples)).ok


def test_distance() -> None:
    s = [CalSample(6, 0, 0, (7600, 7600, 0))]
    w = writes(CALCULATORS["distance"].compute(ctx({0: s}, {"measured_cm": 100.0})))
    assert w["counts_per_cm"] == 7600


def test_track_width() -> None:
    per = math.pi * 18.0 * 76.0
    s = [CalSample(3, 0, 0, (int(-per), int(per), 3600, 0))]
    w = writes(CALCULATORS["track_width"].compute(ctx({0: s}, settings={"counts_per_cm": 7600})))
    assert w["track_width_eff"] == pytest.approx(180, abs=1)


def test_balance() -> None:
    s = [CalSample(2, 0, 0, (-255, 255, -2400, 2600, 0))] * 20
    assert writes(CALCULATORS["track_balance"].compute(ctx({0: s}))) == {
        "max_cps_l": 2400, "max_cps_r": 2600}


def test_pid_relay_tyreus_luyben() -> None:
    tu, a, d = 0.5, 20.0, 50.0
    s = []
    for i in range(300):
        t = i * 0.01
        meas = a * math.sin(2 * math.pi * t / tu)
        s.append(CalSample(5, 0, int(t * 1000), (0, int(meas * 10), int(d if meas < 0 else -d), 0)))
    w = writes(CALCULATORS["pid_rate"].compute(ctx({0: s})))
    ku = 4 * d / (math.pi * a)
    assert w["pid_rate_kp"] == pytest.approx(ku / 2.2 * 1000, rel=0.05)
    assert w["pid_rate_ki"] == pytest.approx(ku / 2.2 / (2.2 * tu) * 1000, rel=0.1)


def test_battery_divider() -> None:
    from conqueror_bench.model import SlowSample

    slow = SlowSample(0, 0, "", 7.5, False, 25, "NONE", frozenset(), 0, 0, 0, 0, 0, 0, 0, (100, 0, 0), 0, (), frozenset())
    c = CalContext({}, {"measured_v": 7.8}, {"batt_mv_per_count": 4050}, [], slow)
    assert writes(CALCULATORS["battery_divider"].compute(c)) == {"batt_mv_per_count": 4212}


def test_wizard_order_covers_items() -> None:
    assert set(WIZARD_ORDER) <= set(CALCULATORS)
    assert WIZARD_ORDER[0] == "battery_divider"
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_calculators.py -v`
Expected: FAIL — `ModuleNotFoundError`.

- [ ] **Step 3: Implement the calculators**

`tools/bench/conqueror_bench/calculators.py`:
```python
"""Reference calibration maths (the ESP32 C++ port must match these on recorded sessions).

CAL_DATA channel layouts (`v[0..7]`, int16, 50 Hz; unused channels 0). Mirrors the authoritative
table in docs/lld/protocol-generator-lld.md ("CAL_DATA channel layouts"); Plan B implements it.
Signs: CCW yaw positive; encoder counts/cps signed by the commanded direction unless marked |x|.
- STILL_SAMPLE / IDLE (param = duration ms, 0 → 3000): gx, gy, gz, ax, ay, az (raw MPU6050 counts,
    65.5 LSB per °/s = ±500 °/s, 8192 LSB per g = ±4 g), temp (0.01 °C), batt_mv
- PWM_RAMP (param = track | reverse << 1 | max_pwm << 8, max 0 → 255; phase 0 ramp up, 1 ramp down;
    5 PWM per 150 ms): |pwm|, yaw_rate (0.1 °/s), |encoder counts since start|, |cps| (selected track)
- FULL_SPIN (param = pwm, 0 → 255; CCW until 720°): pwm_l, pwm_r, cps_l, cps_r, yaw_rate (0.1 °/s),
    yaw since start (0.1°), enc_l, enc_r since start
- SPIN_AND_ARC (param bit 0 = CW; phase 0 = 360° spin at 90 °/s + 0.5 s settle, phase 1 = 90° arc at
    PWM 150 and 30 °/s): enc_l, enc_r, yaw (0.1°) — all since phase start — yaw_rate (0.1 °/s), pwm_l, pwm_r
- RATE_STEPS (param = speed steps 2–5, 0 → 4; phase = step index; step i drives speed 100·i/(n−1) % with a
    full CCW turn command for 1.5 s): speed %, w % ×10 (1000), yaw_rate (0.1 °/s), cps_l, cps_r, pwm_l, pwm_r
- RELAY_OSCILLATION (param = loop 0 speed, 1 rate, 2 heading; 8 s): setpoint ×10, measured ×10,
    relay output in the PID's output unit (speed: % of max_cps → %; rate: °/s → w %; heading: ° → °/s),
    pwm_l, pwm_r
- DRIVE_COUNTS (param = counts, 0 → 7600): enc_l, enc_r, yaw (0.1°) since start, pwm_l, pwm_r, cps_l, cps_r
- SERVO_NUDGE: reserved, the Uno answers UNAVAILABLE (servo trim uses CAM_NUDGE + telemetry)
"""

from __future__ import annotations

import math
import statistics
from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Protocol

from conqueror_protocol import generated as g

from .model import FastSample, SlowSample

LSB_PER_G = 8192.0  # MPU6050 at ±4 g (Uno imu_scale::kAccelLsbPerG)
MOTION_GYRO = 3  # 0.3 °/s in 0.1 °/s units
SUSTAIN = 5  # samples at 50 Hz = 100 ms


@dataclass(frozen=True, slots=True)
class CalSample:
    script: int
    phase: int
    t_ms: int
    v: tuple[int, ...]


@dataclass(frozen=True, slots=True)
class ScriptStep:
    script: g.CalScript
    param: int = 0
    label: str = ""


@dataclass(frozen=True, slots=True)
class InputPrompt:
    id: str
    prompt: str


@dataclass(frozen=True, slots=True)
class SettingWrite:
    name: str
    raw: int


@dataclass(frozen=True, slots=True)
class CalOutcome:
    ok: bool
    writes: tuple[SettingWrite, ...] = ()
    error: str = ""
    details: dict[str, float] = field(default_factory=dict[str, float])


@dataclass(frozen=True, slots=True)
class CalContext:
    samples: dict[int, list[CalSample]]
    inputs: dict[str, float]
    settings: dict[str, int]
    fast: list[FastSample]
    slow: SlowSample | None


class Calculator(Protocol):
    item: str
    settings_needed: tuple[str, ...]

    def scripts(self) -> list[ScriptStep]: ...

    def prompts(self) -> list[InputPrompt]: ...

    def compute(self, ctx: CalContext) -> CalOutcome: ...


def pwm_ramp_param(track: int, reverse: bool) -> int:
    return track | (int(reverse) << 1)


def _fail(error: str) -> CalOutcome:
    return CalOutcome(False, error=error)


def _ok(**writes: int) -> CalOutcome:
    return CalOutcome(True, tuple(SettingWrite(k, int(v)) for k, v in writes.items()),
                      details={k: float(v) for k, v in writes.items()})


class _Base:
    item = ""
    settings_needed: tuple[str, ...] = ()

    def scripts(self) -> list[ScriptStep]:
        return []

    def prompts(self) -> list[InputPrompt]:
        return []


class GyroBias(_Base):
    item = "gyro_bias"

    def scripts(self) -> list[ScriptStep]:
        return [ScriptStep(g.CalScript.STILL_SAMPLE, 0, "keep the tank still for 2 s")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        s = ctx.samples.get(0, [])
        if len(s) < 50:
            return _fail("not enough samples")
        axes = [[x.v[i] for x in s] for i in range(3)]
        if max(statistics.pstdev(a) for a in axes) > 20:
            return _fail("tank moved during sampling — keep it still and retry")
        bx, by, bz = (round(statistics.fmean(a)) for a in axes)
        return _ok(gyro_bias_x=bx, gyro_bias_y=by, gyro_bias_z=bz)


class Level(_Base):
    item = "level"

    def scripts(self) -> list[ScriptStep]:
        return [ScriptStep(g.CalScript.STILL_SAMPLE, 0, "tank on a flat floor")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        s = ctx.samples.get(0, [])
        if not s:
            return _fail("no samples")
        ax, ay, az = (statistics.fmean(x.v[i] for x in s) for i in (3, 4, 5))
        pitch = math.degrees(math.atan2(-ax, math.hypot(ay, az)))
        roll = math.degrees(math.atan2(ay, az))
        return _ok(level_pitch=round(pitch * 100), level_roll=round(roll * 100))


class BatteryDivider(_Base):
    item = "battery_divider"
    settings_needed = ("batt_mv_per_count",)

    def prompts(self) -> list[InputPrompt]:
        return [InputPrompt("measured_v", "Battery voltage measured with a multimeter (V)")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        if ctx.slow is None or ctx.slow.batt_v <= 0:
            return _fail("no battery telemetry")
        measured = ctx.inputs.get("measured_v", math.nan)
        if not 5.0 <= measured <= 9.0:
            return _fail("measured voltage outside 5–9 V")
        current = ctx.settings["batt_mv_per_count"]
        return _ok(batt_mv_per_count=round(current * measured / ctx.slow.batt_v))


def _moving(x: CalSample) -> bool:
    return abs(x.v[1]) > MOTION_GYRO or x.v[3] > 0


def _first_sustained(samples: Sequence[CalSample], want_motion: bool) -> int | None:
    for i in range(len(samples) - SUSTAIN + 1):
        if all(_moving(samples[j]) == want_motion for j in range(i, i + SUSTAIN)):
            return i
    return None


class Deadband(_Base):
    item = "deadband"
    _NAMES = ("l_fwd", "l_rev", "r_fwd", "r_rev")

    def scripts(self) -> list[ScriptStep]:
        return [
            ScriptStep(g.CalScript.PWM_RAMP, pwm_ramp_param(0, False), "left forward"),
            ScriptStep(g.CalScript.PWM_RAMP, pwm_ramp_param(0, True), "left reverse"),
            ScriptStep(g.CalScript.PWM_RAMP, pwm_ramp_param(1, False), "right forward"),
            ScriptStep(g.CalScript.PWM_RAMP, pwm_ramp_param(1, True), "right reverse"),
        ]

    def compute(self, ctx: CalContext) -> CalOutcome:
        writes: dict[str, int] = {}
        for idx, name in enumerate(self._NAMES):
            s = ctx.samples.get(idx, [])
            up = [x for x in s if x.phase == 0]
            down = [x for x in s if x.phase == 1]
            i_up = _first_sustained(up, True)
            i_down = _first_sustained(down, False)
            if i_up is None or i_down is None or i_down == 0:
                return _fail(f"no motion detected on {name} — tracks free? battery ok?")
            writes[f"deadband_start_{name}"] = up[i_up].v[0]
            writes[f"deadband_keep_{name}"] = down[i_down - 1].v[0]
        return _ok(**writes)


class EncoderCpr(_Base):
    item = "encoder_cpr"

    def scripts(self) -> list[ScriptStep]:
        return [ScriptStep(g.CalScript.IDLE, 0, "watch the live count")]

    def prompts(self) -> list[InputPrompt]:
        return [InputPrompt("done", "Turn the LEFT motor shaft exactly one revolution, then Enter")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        if len(ctx.fast) < 2:
            return _fail("no encoder telemetry")
        cpr = abs(ctx.fast[-1].enc[0] - ctx.fast[0].enc[0])
        if not 100 <= cpr <= 10000:
            return _fail(f"implausible count {cpr} (expected ~960 or ~1920)")
        return _ok(counts_per_rev=cpr)


class Distance(_Base):
    item = "distance"
    COUNTS = 7600

    def scripts(self) -> list[ScriptStep]:
        return [ScriptStep(g.CalScript.DRIVE_COUNTS, self.COUNTS, "drives ~1 m straight")]

    def prompts(self) -> list[InputPrompt]:
        return [InputPrompt("measured_cm", "Distance travelled, measured with a tape (cm)")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        s = ctx.samples.get(0, [])
        measured = ctx.inputs.get("measured_cm", math.nan)
        if not s or not measured > 0:
            return _fail("missing samples or measurement")
        counts = (s[-1].v[0] + s[-1].v[1]) / 2.0
        return _ok(counts_per_cm=round(counts / measured * 100))


class TrackBalance(_Base):
    item = "track_balance"

    def scripts(self) -> list[ScriptStep]:
        return [ScriptStep(g.CalScript.FULL_SPIN, 0, "full-speed spin in place")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        s = ctx.samples.get(0, [])
        if len(s) < 10:
            return _fail("not enough samples")
        def p90(values: list[int]) -> int:
            ordered = sorted(abs(v) for v in values)
            return ordered[int(0.9 * (len(ordered) - 1))]
        return _ok(max_cps_l=p90([x.v[2] for x in s]), max_cps_r=p90([x.v[3] for x in s]))


class TrackWidth(_Base):
    item = "track_width"
    settings_needed = ("counts_per_cm",)

    def scripts(self) -> list[ScriptStep]:
        return [ScriptStep(g.CalScript.SPIN_AND_ARC, 0, "spin 360° then one arc")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        s = [x for x in ctx.samples.get(0, []) if x.phase == 0]
        if not s:
            return _fail("no spin samples")
        cpcm = ctx.settings["counts_per_cm"] / 100.0
        dl, dr = s[-1].v[0] / cpcm, s[-1].v[1] / cpcm
        dtheta = math.radians(s[-1].v[2] / 10.0)
        if abs(dtheta) < math.radians(90):
            return _fail("spin too small")
        width_cm = (dr - dl) / dtheta
        return _ok(track_width_eff=round(width_cm * 10))


class TurnCurve(_Base):
    item = "turn_curve"

    def scripts(self) -> list[ScriptStep]:
        return [ScriptStep(g.CalScript.RATE_STEPS, 4, "turns at 4 speeds")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        s = ctx.samples.get(0, [])
        by_phase: dict[int, list[CalSample]] = {}
        for x in s:
            by_phase.setdefault(x.phase, []).append(x)
        if len(by_phase) < 2:
            return _fail("need at least two speed steps")
        ordered = sorted(by_phase.values(), key=lambda xs: abs(xs[0].v[0]))
        slow = max(abs(x.v[2]) for x in ordered[0])
        fast = max(abs(x.v[2]) for x in ordered[-1])
        if slow == 0:
            return _fail("no rotation measured")
        return _ok(turn_rate_full_speed=max(10, min(100, round(fast / slow * 100))))


class PidAutotune(_Base):
    def __init__(self, loop: str, loop_id: int) -> None:
        self.item = f"pid_{loop}"
        self._loop = loop
        self._loop_id = loop_id

    def scripts(self) -> list[ScriptStep]:
        return [ScriptStep(g.CalScript.RELAY_OSCILLATION, self._loop_id, f"{self._loop} relay test")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        s = ctx.samples.get(0, [])
        s = s[len(s) // 3 :]
        if len(s) < 20:
            return _fail("not enough oscillation")
        err = [(x.v[1] - x.v[0]) / 10.0 for x in s]
        t = [x.t_ms / 1000.0 for x in s]
        rises = [t[i] for i in range(1, len(err)) if err[i - 1] < 0 <= err[i]]
        if len(rises) < 3:
            return _fail("no sustained oscillation")
        tu = statistics.fmean(b - a for a, b in zip(rises, rises[1:], strict=False))
        a = (max(err) - min(err)) / 2.0
        outs = [x.v[2] for x in s]
        d = (max(outs) - min(outs)) / 2.0
        if a <= 0 or d <= 0:
            return _fail("flat response")
        ku = 4.0 * d / (math.pi * a)
        kp = ku / 2.2
        ti = 2.2 * tu
        td = tu / 6.3
        return _ok(**{
            f"pid_{self._loop}_kp": round(kp * 1000),
            f"pid_{self._loop}_ki": round(kp / ti * 1000),
            f"pid_{self._loop}_kd": round(kp * td * 1000),
        })


class ServoTrim(_Base):
    item = "servo_trim"

    def scripts(self) -> list[ScriptStep]:
        return [ScriptStep(g.CalScript.SERVO_NUDGE, 0, "nudge the camera to dead centre")]

    def prompts(self) -> list[InputPrompt]:
        return [InputPrompt("done", "Nudge pan/tilt (arrow keys) until centred, then Enter")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        if not ctx.fast:
            return _fail("no camera telemetry")
        last = ctx.fast[-1]
        pan = round((last.pan_deg - 90.0) * 10)
        tilt = round((last.tilt_deg - 90.0) * 10)
        if abs(pan) > 300 or abs(tilt) > 300:
            return _fail("trim larger than ±30°: check the servo horn mounting")
        return _ok(pan_trim=pan, tilt_trim=tilt)


class UltrasonicOffset(_Base):
    item = "ultrasonic_offset"

    def prompts(self) -> list[InputPrompt]:
        return [InputPrompt("measured_cm", "Distance from the tank's front to the wall (cm)")]

    def compute(self, ctx: CalContext) -> CalOutcome:
        readings = [s.dist_cm for s in ctx.fast if s.dist_cm is not None]
        measured = ctx.inputs.get("measured_cm", math.nan)
        if not readings or not measured > 0:
            return _fail("no ultrasonic readings or measurement")
        offset_mm = round((statistics.median(readings) - measured) * 10)
        if not 0 <= offset_mm <= 100:
            return _fail(f"offset {offset_mm} mm outside 0–100 mm")
        return _ok(ultrasonic_offset=offset_mm)


CALCULATORS: dict[str, Calculator] = {
    c.item: c
    for c in (
        GyroBias(), Level(), BatteryDivider(), Deadband(), EncoderCpr(), Distance(),
        TrackBalance(), TrackWidth(), TurnCurve(), PidAutotune("speed", 0),
        PidAutotune("rate", 1), PidAutotune("heading", 2), ServoTrim(), UltrasonicOffset(),
    )
}

WIZARD_ORDER: tuple[str, ...] = (
    "battery_divider", "level", "gyro_bias", "deadband", "encoder_cpr", "distance",
    "track_balance", "track_width", "turn_curve", "servo_trim", "ultrasonic_offset",
)
```
The wizard order follows ESP32 LLD §7 / UX hub doc (battery → level → gyro → deadband → encoders/distance → balance → track width → turn curve → servos → ultrasonic); PID auto-tune is optional and run on request.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_calculators.py -v`
Expected: 10 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/calculators.py tools/bench/tests/test_calculators.py
git commit -m "feat(bench): reference calibration calculators and CAL_DATA layouts"
```

---

### Task 12: Calibration service

**Files:**
- Create: `tools/bench/conqueror_bench/calibration.py`
- Test: `tools/bench/tests/test_calibration.py`

**Interfaces:**
- Consumes: `Tank`, `Prompter`, `CALCULATORS`, `CalSample`, `CalContext`, `CalOutcome`, `setting_info`.
- Produces: `CalibrationService(tank, prompter, profile: int = 0, script_timeout_s: float = 60.0)` with `async run(item: str) -> CalOutcome`, `async summary(outcome) -> list[tuple[str, float, float]]` (name, old, new in physical units), `async accept(outcome) -> bool` (SETTING_SET each write then SETTINGS_SAVE).

- [ ] **Step 1: Write the failing tests**

`tools/bench/tests/test_calibration.py`:
```python
import asyncio

from conqueror_protocol import generated as g

from conqueror_bench.api import open_tank
from conqueror_bench.calibration import CalibrationService
from conqueror_bench.prompter import ScriptedPrompter

from .conftest import SimRig


async def test_gyro_bias_run_and_accept(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        svc = CalibrationService(tank, ScriptedPrompter())
        outcome = await svc.run("gyro_bias")
        assert outcome.ok, outcome.error
        summary = await svc.summary(outcome)
        assert ("gyro_bias_z", 0.0, 35.0) in summary
        assert await svc.accept(outcome)
    assert rig.uno.saved_settings[int(g.SettingId.GYRO_BIAS_Z)] == 35


async def test_deadband_four_scripts(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        outcome = await CalibrationService(tank, ScriptedPrompter()).run("deadband")
    assert outcome.ok, outcome.error
    w = {x.name: x.raw for x in outcome.writes}
    assert w["deadband_start_r_rev"] == 74 and w["deadband_keep_l_fwd"] == 58
    calls = [m for m in rig.uno.received if isinstance(m, g.Calibrate)]
    assert [c.param for c in calls] == [0, 2, 1, 3]


async def test_distance_uses_prompt(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        outcome = await CalibrationService(
            tank, ScriptedPrompter({"measured_cm": "50"})
        ).run("distance")
    assert {w.name: w.raw for w in outcome.writes} == {"counts_per_cm": 15200}


async def test_refused_while_envelope_active(rig: SimRig) -> None:
    async with open_tank(rig.bench, envelope=(20, 30, 1)) as tank:
        await asyncio.sleep(1.2)  # first TELEM_SLOW predates the envelope; wait for the next one
        assert tank.model.slow is not None and tank.model.slow.envelope[0] == 20
        outcome = await CalibrationService(tank, ScriptedPrompter()).run("gyro_bias")
    assert not outcome.ok and "envelope" in outcome.error
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_calibration.py -v`
Expected: FAIL — `ModuleNotFoundError`.

- [ ] **Step 3: Implement the service**

`tools/bench/conqueror_bench/calibration.py`:
```python
"""Calibration orchestration: run Uno scripts, collect CAL_DATA, prompt, compute, write back."""

from __future__ import annotations

import asyncio
import math
import time
from typing import Any

from conqueror_protocol import generated as g

from .api import Tank, setting_info
from .calculators import CALCULATORS, CalContext, CalOutcome, CalSample
from .model import FastSample
from .prompter import Prompter

SLOW_WAIT_S = 1.5


class CalibrationService:
    def __init__(
        self, tank: Tank, prompter: Prompter, profile: int = 0, script_timeout_s: float = 60.0
    ) -> None:
        self.tank = tank
        self.prompter = prompter
        self.profile = profile
        self.script_timeout_s = script_timeout_s

    async def _precheck(self) -> str | None:
        model = self.tank.model
        deadline = time.monotonic() + SLOW_WAIT_S
        while model.slow is None and time.monotonic() < deadline:
            await asyncio.sleep(0.05)
        if model.fast is not None and model.fast.spd_cmd != (0, 0):
            return "tank is moving — stop first"
        if model.slow is not None and model.slow.envelope[0] < 100:
            return "safety envelope active — clear child-play limits first"
        return None

    async def run(self, item: str) -> CalOutcome:
        calc = CALCULATORS[item]
        if (problem := await self._precheck()) is not None:
            return CalOutcome(False, error=problem)
        api = self.tank.api
        settings = {name: await api.get_setting_raw(setting_info(name).id) for name in calc.settings_needed}
        samples: dict[int, list[CalSample]] = {}
        start_t = time.monotonic()
        for idx, step in enumerate(calc.scripts()):
            await self.prompter.say(f"{item}: {step.label}")
            if step.script in (g.CalScript.IDLE, g.CalScript.SERVO_NUDGE):
                continue
            result = await self._run_script(idx, int(step.script), step.param, samples)
            if result is not None:
                return CalOutcome(False, error=result)
        inputs: dict[str, float] = {}
        for p in calc.prompts():
            answer = await self.prompter.ask(f"{p.prompt} [{p.id}]")
            try:
                inputs[p.id] = float(answer)
            except ValueError:
                inputs[p.id] = math.nan
        fast: list[FastSample] = [s for s in self.tank.model.history if s.host_t >= start_t]
        ctx = CalContext(samples, inputs, settings, fast, self.tank.model.slow)
        return calc.compute(ctx)

    async def _run_script(
        self, idx: int, script: int, param: int, samples: dict[int, list[CalSample]]
    ) -> str | None:
        bucket = samples.setdefault(idx, [])
        done: asyncio.Future[Any] = asyncio.get_running_loop().create_future()

        def on_update(topic: str, payload: Any) -> None:
            if topic == "cal_data" and payload.script == script:
                bucket.append(CalSample(payload.script, payload.phase, payload.t_ms, tuple(payload.v)))
            elif topic == "cal_result" and payload.script == script and not done.done():
                if payload.status != 0 or payload.progress >= 100:  # progress updates are not the end
                    done.set_result(payload)

        self.tank.model.subscribe(on_update)
        try:
            res = await self.tank.api.calibrate(g.CalScript(script), param, self.profile)
            if not res.ok:
                return f"script rejected: {res.status}"
            try:
                result = await asyncio.wait_for(done, self.script_timeout_s)
            except TimeoutError:
                await self.tank.api.cal_cancel()
                return "script timed out"
            if result.status != 0:
                return f"script failed: {g.ACK_STATUS[result.status]}"
            return None
        finally:
            self.tank.model._subs.remove(on_update)  # pyright: ignore[reportPrivateUsage]

    async def summary(self, outcome: CalOutcome) -> list[tuple[str, float, float]]:
        rows: list[tuple[str, float, float]] = []
        for w in outcome.writes:
            info = setting_info(w.name)
            old = await self.tank.api.get_setting_raw(info.id)
            rows.append((w.name, old * info.scale, w.raw * info.scale))
        return rows

    async def accept(self, outcome: CalOutcome) -> bool:
        if not outcome.ok:
            return False
        for w in outcome.writes:
            res = await self.tank.api.set_setting_raw(setting_info(w.name).id, w.raw)
            if not res.ok:
                await self.prompter.say(f"{w.name}: {res.status}")
                return False
        return await self.tank.api.settings_save()
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_calibration.py -v`
Expected: 4 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/calibration.py tools/bench/tests/test_calibration.py
git commit -m "feat(bench): calibration service writing results back to the Uno"
```

---

### Task 13: Tuning, shield probe and mount check

**Files:**
- Create: `tools/bench/conqueror_bench/tuning.py`, `tools/bench/conqueror_bench/probe.py`
- Test: `tools/bench/tests/test_tuning_probe.py`

**Interfaces:**
- Consumes: `Tank`, `Prompter`, `step_metrics`, `heading_change_deg`.
- Produces: `TuningService(tank)` with `async step_test(loop: str, magnitude: float, duration_s: float = 2.0) -> StepMetrics` (`loop` in `"rate" | "speed" | "heading"`) and `async set_gains(loop, kp, ki, kd) -> bool`; `ProbeResult(name: str, ok: bool, detail: str)`; `async shield_probe(tank, prompter) -> list[ProbeResult]`; `async mount_check(tank, prompter) -> list[ProbeResult]`; pure helpers `judge_drive(enc_before, enc_after, encoders: bool, answer: str, expect: tuple[int, int]) -> ProbeResult` and `judge_yaw(change_deg: float) -> ProbeResult`.

- [ ] **Step 1: Write the failing tests**

`tools/bench/tests/test_tuning_probe.py`:
```python
import pytest

from conqueror_bench.api import open_tank
from conqueror_bench.probe import judge_drive, judge_yaw, mount_check
from conqueror_bench.prompter import ScriptedPrompter
from conqueror_bench.tuning import TuningService

from .conftest import SimRig


def test_judge_drive_with_encoders() -> None:
    assert judge_drive((0, 0), (500, 480), True, "", (1, 1)).ok
    bad = judge_drive((0, 0), (0, 0), True, "", (1, 1))
    assert not bad.ok and "D3" in bad.detail
    swapped = judge_drive((0, 0), (-500, 500), True, "", (1, 1))
    assert not swapped.ok and "direction" in swapped.detail


def test_judge_drive_without_encoders_uses_answer() -> None:
    assert judge_drive((0, 0), (0, 0), False, "y", (1, 1)).ok
    assert not judge_drive((0, 0), (0, 0), False, "n", (1, 1)).ok


def test_judge_yaw() -> None:
    assert judge_yaw(85.0).ok
    assert not judge_yaw(-90.0).ok
    assert not judge_yaw(10.0).ok


async def test_rate_step_on_sim(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        m = await TuningService(tank).step_test("rate", 90.0, duration_s=0.6)
    assert m.steady_error == pytest.approx(0.0, abs=5.0)


async def test_mount_check_level_ok(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        results = await mount_check(tank, ScriptedPrompter(), rotate_timeout_s=0.3)
    level = next(r for r in results if r.name == "level")
    assert level.ok
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_tuning_probe.py -v`
Expected: FAIL — `ModuleNotFoundError`.

- [ ] **Step 3: Implement tuning and probe**

`tools/bench/conqueror_bench/tuning.py`:
```python
"""Live PID tuning: set gains, run a step test, measure the response."""

from __future__ import annotations

import asyncio
import time

from .api import Tank
from .metrics import StepMetrics, step_metrics
from .odometry import unwrapped_yaw

PERIOD_S = 0.05


class TuningService:
    def __init__(self, tank: Tank) -> None:
        self.tank = tank

    async def set_gains(self, loop: str, kp: float, ki: float, kd: float) -> bool:
        api = self.tank.api
        for name, value in ((f"pid_{loop}_kp", kp), (f"pid_{loop}_ki", ki), (f"pid_{loop}_kd", kd)):
            if not (await api.set_setting(name, value)).ok:
                return False
        return True

    async def step_test(self, loop: str, magnitude: float, duration_s: float = 2.0) -> StepMetrics:
        api = self.tank.api
        start = time.monotonic()
        if loop == "heading":
            api.clear_primitive_results()
            await api.turn(by_deg=magnitude, speed=40)
            await asyncio.sleep(duration_s)
        else:
            end = start + duration_s
            while time.monotonic() < end:
                if loop == "rate":
                    await api.drive_rate(0, magnitude)
                else:
                    await api.drive(int(magnitude), 0)
                await asyncio.sleep(PERIOD_S)
        await api.stop()
        samples = [s for s in self.tank.model.history if s.host_t >= start]
        if not samples:
            raise RuntimeError("no telemetry during the step test")
        t = [s.host_t - start for s in samples]
        if loop == "rate":
            y = [s.yaw_rate_dps for s in samples]
        elif loop == "speed":
            y = [float(s.spd_act[0] if s.spd_act[0] is not None else s.spd_cmd[0]) for s in samples]
        else:
            yaw = unwrapped_yaw(samples)
            y = [v - yaw[0] for v in yaw]
        return step_metrics(t, y, magnitude)
```

`tools/bench/conqueror_bench/probe.py`:
```python
"""Bring-up checks on the stand: shield probe (TB6612 standby + direction) and mount check."""

from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass

from .api import Tank
from .metrics import heading_change_deg
from .prompter import Prompter

DRIVE_S = 1.0
MIN_COUNTS = 50


@dataclass(frozen=True, slots=True)
class ProbeResult:
    name: str
    ok: bool
    detail: str


def judge_drive(
    enc_before: tuple[int, int],
    enc_after: tuple[int, int],
    encoders: bool,
    answer: str,
    expect: tuple[int, int],
) -> ProbeResult:
    if not encoders:
        ok = answer.strip().lower().startswith("y")
        return ProbeResult("drive", ok, "confirmed by eye" if ok else "tracks did not move as expected")
    dl = enc_after[0] - enc_before[0]
    dr = enc_after[1] - enc_before[1]
    if abs(dl) < MIN_COUNTS and abs(dr) < MIN_COUNTS:
        return ProbeResult(
            "drive", False,
            "no motion: TB6612 needs D3 (standby) HIGH — check the SHIELD build option and wiring",
        )
    if (dl > 0) != (expect[0] > 0) or (dr > 0) != (expect[1] > 0):
        return ProbeResult("drive", False, f"wrong direction mapping (L {dl}, R {dr}) — check pin map")
    return ProbeResult("drive", True, f"L {dl} counts, R {dr} counts")


def judge_yaw(change_deg: float) -> ProbeResult:
    if 60.0 <= change_deg <= 120.0:
        return ProbeResult("yaw_sign", True, f"rotated {change_deg:.0f}° CCW as expected")
    if -120.0 <= change_deg <= -60.0:
        return ProbeResult("yaw_sign", False, "yaw sign inverted — fix the IMU axis map in board_config.h")
    return ProbeResult("yaw_sign", False, f"rotation {change_deg:.0f}° not ~90° — retry")


async def _drive_and_measure(tank: Tank, v: int, w: int) -> tuple[tuple[int, int], tuple[int, int]]:
    before = tank.model.fast.enc if tank.model.fast else (0, 0)
    end = time.monotonic() + DRIVE_S
    while time.monotonic() < end:
        await tank.api.drive(v, w)
        await asyncio.sleep(0.05)
    await tank.api.stop()
    await asyncio.sleep(0.2)
    after = tank.model.fast.enc if tank.model.fast else before
    return before, after


async def shield_probe(tank: Tank, prompter: Prompter) -> list[ProbeResult]:
    encoders = bool(tank.link.hello and tank.link.hello.encoders)
    await prompter.ask("Put the tank on the stand with the tracks OFF the ground, then Enter")
    results: list[ProbeResult] = []
    for label, v, w, expect in (("both forward", 30, 0, (1, 1)), ("spin left", 0, 40, (-1, 1))):
        before, after = await _drive_and_measure(tank, v, w)
        answer = "" if encoders else await prompter.ask(f"Did the tracks move: {label}? [y/n]")
        r = judge_drive(before, after, encoders, answer, expect)
        results.append(ProbeResult(label, r.ok, r.detail))
    return results


async def mount_check(tank: Tank, prompter: Prompter, rotate_timeout_s: float = 10.0) -> list[ProbeResult]:
    await prompter.ask("Place the tank on a flat floor, then Enter")
    await asyncio.sleep(0.3)
    fast = tank.model.fast
    results: list[ProbeResult] = []
    if fast is None:
        return [ProbeResult("level", False, "no telemetry")]
    level_ok = abs(fast.pitch_deg) < 10 and abs(fast.roll_deg) < 10
    results.append(ProbeResult(
        "level", level_ok,
        f"pitch {fast.pitch_deg:.1f}°, roll {fast.roll_deg:.1f}°"
        + ("" if level_ok else " — gravity axis wrong? check the IMU axis map"),
    ))
    start = time.monotonic()
    await prompter.ask("Rotate the tank by hand about 90° to the LEFT (counter-clockwise), then Enter")
    await asyncio.sleep(rotate_timeout_s)
    samples = [s for s in tank.model.history if s.host_t >= start]
    results.append(judge_yaw(heading_change_deg(samples)))
    return results
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_tuning_probe.py -v`
Expected: 5 passed.

- [ ] **Step 5: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/tuning.py tools/bench/conqueror_bench/probe.py tools/bench/tests/test_tuning_probe.py
git commit -m "feat(bench): PID step tests, shield probe and mount check"
```

---

### Task 14: Discovery, console commands and the `bench` CLI

**Files:**
- Create: `tools/bench/conqueror_bench/discovery.py`, `tools/bench/conqueror_bench/console.py`, `tools/bench/conqueror_bench/cli.py`
- Test: `tools/bench/tests/test_console_cli.py`

**Interfaces:**
- Consumes: everything above.
- Produces: `discovery.BridgeInfo(name, host, port)`; `discovery.discover_bridges(timeout_s=3.0) -> list[BridgeInfo]` (mDNS `_cqbridge._tcp.local.`); `discovery.serial_ports() -> list[str]`; `console.ParsedCommand(method: str, kwargs: dict[str, Any])`; `console.parse_command(line: str) -> ParsedCommand` (raises `ValueError` with usage text); `console.async execute(api, cmd) -> str`; `cli.app` (Typer) with commands `discover, connect, dash, drive, test, compare, calibrate, tune, replay, probe, stop`; global options `--port/-p`, `--host/-H`, `--sim`, `--envelope child_play|none`; `cli.make_transport(port, host, sim) -> tuple[Transport, Callable[[], Awaitable[None]] | None]`; `cli.main()`.

- [ ] **Step 1: Write the failing tests**

`tools/bench/tests/test_console_cli.py`:
```python
import pytest
from typer.testing import CliRunner

from conqueror_bench.cli import app
from conqueror_bench.console import parse_command


@pytest.mark.parametrize(
    ("line", "method", "kwargs"),
    [
        ("stop", "stop", {}),
        ("drive 30 -20", "drive", {"v": 30, "w": -20}),
        ("turn by 90 speed 40", "turn", {"by_deg": 90.0, "speed": 40, "radius_cm": 0, "pivot": False}),
        ("turn to 180 speed 30 radius 50", "turn", {"to_deg": 180.0, "speed": 30, "radius_cm": 50, "pivot": False}),
        ("turn by 45 pivot", "turn", {"by_deg": 45.0, "speed": 40, "radius_cm": 0, "pivot": True}),
        ("move 50 speed 30", "move", {"dist_cm": 50.0, "speed": 30, "heading_deg": None}),
        ("cam home", "cam_home", {}),
        ("cam nudge 5 -5", "cam_nudge", {"dpan_deg": 5.0, "dtilt_deg": -5.0}),
        ("brake off", "brake", {"on": False}),
        ("setting get deadman", "get_setting", {"name": "deadman"}),
        ("setting set batt_low 7100", "set_setting", {"name": "batt_low", "value": 7100.0}),
        ("settings save", "settings_save", {}),
        ("envelope child_play", "envelope",
         {"speed_cap": 20, "accel_cap": 30, "lock_brake": True, "child_play": True}),
    ],
)
def test_parse_command(line: str, method: str, kwargs: dict[str, object]) -> None:
    cmd = parse_command(line)
    assert cmd.method == method
    assert cmd.kwargs == kwargs


def test_parse_command_rejects_garbage() -> None:
    with pytest.raises(ValueError, match="usage"):
        parse_command("fly to the moon")


def test_cli_runs_straight_test_in_sim(tmp_path: object) -> None:
    result = CliRunner().invoke(
        app,
        ["--sim", "test", "straight_200cm", "--answers", "lateral_cm=1,distance_cm=200",
         "--sessions", str(tmp_path)],
    )
    assert result.exit_code == 0, result.output
    assert "PASS" in result.output


def test_cli_spin_test_in_sim(tmp_path: object) -> None:
    result = CliRunner().invoke(app, ["--sim", "test", "spin_360", "--sessions", str(tmp_path)])
    assert result.exit_code == 0, result.output
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/bench && uv run pytest tests/test_console_cli.py -v`
Expected: FAIL — `ModuleNotFoundError`.

- [ ] **Step 3: Implement discovery**

`tools/bench/conqueror_bench/discovery.py`:
```python
"""Find the ESP32 WiFi bridge (mDNS) and local serial ports."""

from __future__ import annotations

import socket
import time
from dataclasses import dataclass
from typing import Any

SERVICE = "_cqbridge._tcp.local."  # DNS-SD names max 15 chars (Plan C)


@dataclass(frozen=True, slots=True)
class BridgeInfo:
    name: str
    host: str
    port: int


def discover_bridges(timeout_s: float = 3.0) -> list[BridgeInfo]:
    from zeroconf import ServiceBrowser, ServiceListener, Zeroconf

    found: dict[str, BridgeInfo] = {}

    class Listener(ServiceListener):
        def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:
            info: Any = zc.get_service_info(type_, name, timeout=1000)
            if info is not None and info.addresses:
                host = socket.inet_ntoa(info.addresses[0])
                found[name] = BridgeInfo(name.split(".")[0], host, int(info.port))

        def update_service(self, zc: Zeroconf, type_: str, name: str) -> None:
            self.add_service(zc, type_, name)

        def remove_service(self, zc: Zeroconf, type_: str, name: str) -> None:
            found.pop(name, None)

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, SERVICE, Listener())
        time.sleep(timeout_s)
    finally:
        zc.close()
    return list(found.values())


def serial_ports() -> list[str]:
    from serial.tools import list_ports

    return [p.device for p in list_ports.comports() if "usb" in p.device.lower() or "acm" in p.device.lower()]
```

- [ ] **Step 4: Implement the console parser**

`tools/bench/conqueror_bench/console.py`:
```python
"""Typed console commands: parse a line into an API call (no eval)."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .api import CommandApi

USAGE = """usage:
  stop | cancel
  drive <v%> <w%>
  turn by|to <deg> [speed <%>] [radius <cm>] [pivot]
  move <cm> [speed <%>] [heading <deg>]
  cam home | cam nudge <dpan> <dtilt> | cam set <pan> <tilt> | cam lock on|off
  brake on|off
  envelope child_play | envelope none | envelope <speed%> <accel%/s> <lock 0|1>
  setting get <name> | setting set <name> <value> | settings save"""


@dataclass(frozen=True, slots=True)
class ParsedCommand:
    method: str
    kwargs: dict[str, Any]


def _options(tokens: list[str]) -> dict[str, str]:
    opts: dict[str, str] = {}
    i = 0
    while i < len(tokens):
        if tokens[i] == "pivot":
            opts["pivot"] = "1"
            i += 1
        else:
            opts[tokens[i]] = tokens[i + 1]
            i += 2
    return opts


def parse_command(line: str) -> ParsedCommand:
    t = line.strip().split()
    try:
        match t:
            case ["stop"]:
                return ParsedCommand("stop", {})
            case ["cancel"]:
                return ParsedCommand("cancel", {})
            case ["drive", v, w]:
                return ParsedCommand("drive", {"v": int(v), "w": int(w)})
            case ["turn", ("by" | "to") as how, deg, *rest]:
                o = _options(rest)
                key = "by_deg" if how == "by" else "to_deg"
                return ParsedCommand("turn", {
                    key: float(deg), "speed": int(o.get("speed", 40)),
                    "radius_cm": int(o.get("radius", 0)), "pivot": "pivot" in o,
                })
            case ["move", cm, *rest]:
                o = _options(rest)
                heading = float(o["heading"]) if "heading" in o else None
                return ParsedCommand("move", {
                    "dist_cm": float(cm), "speed": int(o.get("speed", 40)), "heading_deg": heading,
                })
            case ["cam", "home"]:
                return ParsedCommand("cam_home", {})
            case ["cam", "nudge", dp, dt]:
                return ParsedCommand("cam_nudge", {"dpan_deg": float(dp), "dtilt_deg": float(dt)})
            case ["cam", "set", pan, tilt]:
                return ParsedCommand("cam_set", {"pan_deg": float(pan), "tilt_deg": float(tilt)})
            case ["cam", "lock", ("on" | "off") as state]:
                return ParsedCommand("cam_lock", {"on": state == "on"})
            case ["brake", ("on" | "off") as state]:
                return ParsedCommand("brake", {"on": state == "on"})
            case ["envelope", "child_play"]:
                return ParsedCommand("envelope", {
                    "speed_cap": 20, "accel_cap": 30, "lock_brake": True, "child_play": True,
                })
            case ["envelope", "none"]:
                return ParsedCommand("envelope", {"speed_cap": 100, "accel_cap": 0, "lock_brake": False})
            case ["envelope", s, a, lock]:
                return ParsedCommand("envelope", {
                    "speed_cap": int(s), "accel_cap": int(a), "lock_brake": lock == "1",
                })
            case ["setting", "get", name]:
                return ParsedCommand("get_setting", {"name": name})
            case ["setting", "set", name, value]:
                return ParsedCommand("set_setting", {"name": name, "value": float(value)})
            case ["settings", "save"]:
                return ParsedCommand("settings_save", {})
            case _:
                pass
    except (ValueError, IndexError) as exc:
        raise ValueError(USAGE) from exc
    raise ValueError(USAGE)


async def execute(api: CommandApi, cmd: ParsedCommand) -> str:
    method = getattr(api, cmd.method)
    result = await method(**cmd.kwargs)
    return "ok" if result is None else str(result)
```

- [ ] **Step 5: Implement the CLI**

`tools/bench/conqueror_bench/cli.py`:
```python
"""`bench` command line (Typer)."""

from __future__ import annotations

import asyncio
import os
from collections.abc import Awaitable, Callable
from contextlib import suppress
from pathlib import Path
from typing import Annotated, Any

import typer

from .api import Tank, open_tank
from .calculators import CALCULATORS, WIZARD_ORDER
from .calibration import CalibrationService
from .discovery import discover_bridges, serial_ports
from .prompter import ConsolePrompter, Prompter, ScriptedPrompter
from .recorder import replay_into
from .reporter import compare_sessions, write_report_async
from .runner import DEFS_DIR, TestRunner, load_test
from .sim import SimConfig, SimulatedUno
from .transport import SerialTransport, TcpTransport, Transport, fake_pair

app = typer.Typer(no_args_is_help=True, help="Conqueror bench tool")
DEFAULT_SESSIONS = Path(__file__).resolve().parents[1] / "sessions"
ENVELOPES = {"child_play": (20, 30, 3)}  # lock_brake bits: LOCK_BRAKE | CHILD_PLAY


class Opts:
    port: str | None = None
    host: str | None = None
    sim: bool = False
    envelope: tuple[int, int, int] | None = None


OPTS = Opts()


@app.callback()
def _global(
    port: Annotated[str | None, typer.Option("--port", "-p", envvar="BENCH_PORT")] = None,
    host: Annotated[str | None, typer.Option("--host", "-H", envvar="BENCH_HOST")] = None,
    sim: Annotated[bool, typer.Option("--sim", help="use the simulated Uno")] = False,
    envelope: Annotated[str, typer.Option(help="child_play or none")] = "none",
) -> None:
    OPTS.port, OPTS.host, OPTS.sim = port, host, sim
    OPTS.envelope = ENVELOPES.get(envelope)


def make_transport(
    port: str | None, host: str | None, sim: bool
) -> tuple[Transport, Callable[[], Awaitable[None]] | None]:
    if sim:
        bench, uno_side = fake_pair()
        uno = SimulatedUno(uno_side, SimConfig(hello_interval_s=0.05, max_speed_cm_s=200.0))
        task = asyncio.get_running_loop().create_task(uno.run())

        async def cleanup() -> None:
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task

        return bench, cleanup
    if host:
        return TcpTransport(host), None
    if port:
        return SerialTransport(port), None
    raise typer.BadParameter("give --port, --host or --sim (try: bench discover)")


async def _with_tank(fn: Callable[[Tank], Awaitable[Any]]) -> Any:
    transport, cleanup = make_transport(OPTS.port, OPTS.host, OPTS.sim)
    hello_timeout = 5.0 if OPTS.port else 3.0
    try:
        async with open_tank(transport, envelope=OPTS.envelope, hello_timeout_s=hello_timeout) as tank:
            return await fn(tank)
    finally:
        if cleanup is not None:
            await cleanup()


def _answers(spec: str) -> dict[str, str]:
    return dict(pair.split("=", 1) for pair in spec.split(",") if "=" in pair)


@app.command()
def discover() -> None:
    """List WiFi bridges (mDNS) and USB serial ports."""
    for b in discover_bridges():
        typer.echo(f"bridge  {b.name}  {b.host}:{b.port}")
    for p in serial_ports():
        typer.echo(f"serial  {p}")


@app.command()
def connect() -> None:
    """Connect, print the HELLO and link stats, then disconnect (sends STOP)."""

    async def run(tank: Tank) -> None:
        typer.echo(f"connected: {tank.link.hello}")
        await asyncio.sleep(1.0)
        typer.echo(f"stats: {tank.link.stats}")

    asyncio.run(_with_tank(run))


@app.command()
def stop() -> None:
    """Send STOP (resent until acknowledged)."""
    asyncio.run(_with_tank(lambda tank: tank.api.stop()))


@app.command()
def test(
    name: str,
    answers: Annotated[str, typer.Option(help="scripted inputs id=value,...")] = "",
    sessions: Annotated[Path, typer.Option()] = DEFAULT_SESSIONS,
) -> None:
    """Run a standard test (or `all`) and write a report."""
    names = sorted(p.stem for p in DEFS_DIR.glob("*.yaml")) if name == "all" else [name]
    prompter: Prompter = ScriptedPrompter(_answers(answers)) if (answers or OPTS.sim) else ConsolePrompter()

    async def run(tank: Tank) -> bool:
        ok = True
        for n in names:
            result = await TestRunner(tank, prompter, sessions).run(load_test(DEFS_DIR / f"{n}.yaml"))
            report = await write_report_async(result, await tank.api.get_setting("counts_per_cm"))
            typer.echo(f"{n}: {'PASS' if result.passed else 'FAIL'}  ({report})")
            ok = ok and result.passed
        return ok

    if not asyncio.run(_with_tank(run)):
        raise typer.Exit(1)


@app.command()
def compare(a: Path, b: Path, out: Annotated[Path, typer.Option()] = Path(".")) -> None:
    """Compare two sessions (metrics table + overlaid paths)."""
    typer.echo(compare_sessions(a, b, out))


@app.command()
def calibrate(item: str) -> None:
    """Run one calibration item (or `wizard`), show new vs old, ask to accept."""
    items = list(WIZARD_ORDER) if item == "wizard" else [item]
    for i in items:
        if i not in CALCULATORS:
            raise typer.BadParameter(f"unknown item {i}; choose from {', '.join(CALCULATORS)}")
    prompter = ConsolePrompter()

    async def run(tank: Tank) -> None:
        svc = CalibrationService(tank, prompter)
        for i in items:
            outcome = await svc.run(i)
            if not outcome.ok:
                typer.echo(f"{i}: FAILED — {outcome.error}")
                continue
            for name, old, new in await svc.summary(outcome):
                typer.echo(f"  {name}: {old:g} → {new:g}")
            if (await prompter.ask(f"Accept {i}? [y/n]")).lower().startswith("y"):
                typer.echo("saved" if await svc.accept(outcome) else "save failed")

    asyncio.run(_with_tank(run))


@app.command()
def tune(loop: str, magnitude: float = 45.0) -> None:
    """Run a step test on a control loop (rate | speed | heading)."""
    from .tuning import TuningService

    async def run(tank: Tank) -> None:
        typer.echo(str(await TuningService(tank).step_test(loop, magnitude)))

    asyncio.run(_with_tank(run))


@app.command()
def probe() -> None:
    """Shield probe (TB6612 standby/direction) and IMU mount check, on the stand."""
    from .probe import mount_check, shield_probe

    async def run(tank: Tank) -> None:
        prompter = ConsolePrompter()
        for r in await shield_probe(tank, prompter) + await mount_check(tank, prompter):
            typer.echo(f"{'OK  ' if r.ok else 'FAIL'} {r.name}: {r.detail}")

    asyncio.run(_with_tank(run))


@app.command()
def replay(session: Path) -> None:
    """Replay a recorded session into a model and print a summary."""
    from .model import TankModel

    model = TankModel()
    asyncio.run(replay_into(session, model))
    typer.echo(f"{len(model.history)} telemetry samples, {len(model.events)} events")


@app.command()
def drive(hold: Annotated[bool, typer.Option("--hold")] = False) -> None:
    """Open the dashboard on the Drive screen (cruise control; --hold uses pynput)."""
    _run_app(start_screen="drive", hold=hold)


@app.command()
def dash() -> None:
    """Open the full Textual dashboard."""
    _run_app(start_screen="dashboard", hold=False)


def _run_app(start_screen: str, hold: bool) -> None:
    from .app import BenchApp

    async def run(tank: Tank) -> None:
        await BenchApp(tank, start_screen=start_screen, hold=hold).run_async()

    asyncio.run(_with_tank(run))


def main() -> None:
    os.environ.setdefault("MPLBACKEND", "Agg")
    app()
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_console_cli.py -v`
Expected: 16 passed.

- [ ] **Step 7: Lint, type-check, commit**

Run: `cd tools/bench && uv run ruff check . && uv run ruff format --check . && uv run pyright`
```bash
git add tools/bench/conqueror_bench/discovery.py tools/bench/conqueror_bench/console.py tools/bench/conqueror_bench/cli.py tools/bench/tests/test_console_cli.py
git commit -m "feat(bench): discovery, console commands and bench CLI"
```

---

### Task 15: Textual dashboard

**Files:**
- Create: `tools/bench/conqueror_bench/app.py`
- Test: `tools/bench/tests/test_app.py`

**Interfaces:**
- Consumes: `Tank`, `Teleop`, `CruiseControl`, `parse_command`, `execute`, `TestRunner`, `CalibrationService`, `TuningService`, `DEFS_DIR`, `CALCULATORS`.
- Produces: `BenchApp(tank, start_screen="dashboard", hold=False)` (Textual `App`) with screens `dashboard, drive, console, tests, calibrate, tune, sessions` (switch keys `F1`–`F7`); bindings on the Drive screen: `w s a d c q e space up down left right h 1-9`; `ModalPrompter` (Prompter implemented with a Textual input dialog). Quitting (`ctrl+q`) sends STOP before exit.

- [ ] **Step 1: Write the failing UI smoke test**

`tools/bench/tests/test_app.py`:
```python
from conqueror_protocol import generated as g

from conqueror_bench.api import open_tank
from conqueror_bench.app import BenchApp

from .conftest import SimRig


async def test_drive_keys_and_quit_sends_stop(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        app = BenchApp(tank, start_screen="drive")
        async with app.run_test() as pilot:
            await pilot.press("w", "w", "a")
            await pilot.pause(0.3)
            assert app.teleop.cruise.speed == 20
            assert app.teleop.cruise.steer == 20
            drives = [m for m in rig.uno.received if isinstance(m, g.Drive)]
            assert drives and drives[-1].v == 20 and drives[-1].w == 20
            await pilot.press("space")
            await pilot.pause(0.1)
            assert any(isinstance(m, g.Stop) for m in rig.uno.received)
            await pilot.press("ctrl+q")


async def test_dashboard_shows_telemetry(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        app = BenchApp(tank)
        async with app.run_test() as pilot:
            await pilot.pause(0.3)
            assert "batt" in app.status_text().lower()
            await pilot.press("ctrl+q")


async def test_console_screen_runs_command(rig: SimRig) -> None:
    async with open_tank(rig.bench) as tank:
        app = BenchApp(tank, start_screen="console")
        async with app.run_test() as pilot:
            await pilot.press("c", "a", "m", "space", "h", "o", "m", "e", "enter")
            await pilot.pause(0.2)
            assert any(isinstance(m, g.CamHome) for m in rig.uno.received)
            await pilot.press("ctrl+q")
```

- [ ] **Step 2: Run it to see it fail**

Run: `cd tools/bench && uv run pytest tests/test_app.py -v`
Expected: FAIL — `ModuleNotFoundError`.

- [ ] **Step 3: Implement the app (thin shell over services)**

`tools/bench/conqueror_bench/app.py`:
```python
"""Textual dashboard: thin UI over the services (all logic lives in the service modules)."""

from __future__ import annotations

import asyncio
from typing import Any, ClassVar

from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Vertical
from textual.screen import ModalScreen, Screen
from textual.widgets import Footer, Header, Input, Label, ListItem, ListView, Log, Static

from .api import Tank
from .calculators import CALCULATORS
from .calibration import CalibrationService
from .console import execute, parse_command
from .runner import DEFS_DIR, TestRunner, load_test
from .teleop import Teleop


class AskDialog(ModalScreen[str]):
    def __init__(self, prompt: str) -> None:
        super().__init__()
        self._prompt = prompt

    def compose(self) -> ComposeResult:
        yield Vertical(Label(self._prompt), Input(id="answer"))

    def on_input_submitted(self, event: Input.Submitted) -> None:
        self.dismiss(event.value)


class ModalPrompter:
    def __init__(self, app: BenchApp) -> None:
        self._app = app

    async def ask(self, prompt: str) -> str:
        return await self._app.push_screen_wait(AskDialog(prompt))

    async def say(self, text: str) -> None:
        self._app.log_line(text)


class StatusPanel(Static):
    pass


class DashboardScreen(Screen[None]):
    def compose(self) -> ComposeResult:
        yield Header()
        yield StatusPanel(id="status")
        yield Log(id="events")
        yield Footer()


class DriveScreen(Screen[None]):
    BINDINGS: ClassVar[list[Binding | tuple[str, str] | tuple[str, str, str]]] = [
        Binding(k, f"key('{k}')", show=False)
        for k in ("w", "s", "a", "d", "c", "q", "e", "space", "up", "down", "left", "right", "h",
                  "1", "2", "3", "4", "5", "6", "7", "8", "9")
    ]

    def compose(self) -> ComposeResult:
        yield Header()
        yield StatusPanel(id="status")
        yield Static("W/S speed ±10 · A/D steer · C centre · Q/E spin · SPACE STOP · arrows camera · H home",
                     id="help")
        yield Footer()

    async def action_key(self, key: str) -> None:
        app = self.app
        assert isinstance(app, BenchApp)
        action = await app.teleop.handle_key(key)
        if action.kind == "quick_test":
            names = sorted(p.stem for p in DEFS_DIR.glob("*.yaml"))
            if 0 < action.value <= len(names):
                app.run_worker(app.run_test_named(names[action.value - 1]))


class ConsoleScreen(Screen[None]):
    def compose(self) -> ComposeResult:
        yield Header()
        yield Log(id="console_log")
        yield Input(placeholder="command (try: turn by 90 speed 40)", id="cmd")
        yield Footer()

    async def on_input_submitted(self, event: Input.Submitted) -> None:
        app = self.app
        assert isinstance(app, BenchApp)
        log = self.query_one("#console_log", Log)
        try:
            out = await execute(app.tank.api, parse_command(event.value))
        except (ValueError, KeyError) as exc:
            out = str(exc)
        log.write_line(f"> {event.value}\n{out}")
        event.input.value = ""


class ListScreen(Screen[None]):
    def __init__(self, title: str, items: list[str], handler: str) -> None:
        super().__init__()
        self._title = title
        self._items = items
        self._handler = handler

    def compose(self) -> ComposeResult:
        yield Header()
        yield Label(self._title)
        yield ListView(*[ListItem(Label(i), name=i) for i in self._items])
        yield Footer()

    def on_list_view_selected(self, event: ListView.Selected) -> None:
        app = self.app
        assert isinstance(app, BenchApp)
        name = event.item.name or ""
        app.run_worker(getattr(app, self._handler)(name))


class BenchApp(App[None]):
    TITLE = "Conqueror bench"
    BINDINGS: ClassVar[list[Binding | tuple[str, str] | tuple[str, str, str]]] = [
        Binding("f1", "show('dashboard')", "Dashboard"),
        Binding("f2", "show('drive')", "Drive"),
        Binding("f3", "show('console')", "Console"),
        Binding("f4", "show('tests')", "Tests"),
        Binding("f5", "show('calibrate')", "Calibrate"),
        Binding("f6", "show('tune')", "Tune"),
        Binding("f7", "show('sessions')", "Sessions"),
        Binding("ctrl+q", "safe_quit", "Quit (STOP first)"),
    ]

    def __init__(self, tank: Tank, start_screen: str = "dashboard", hold: bool = False) -> None:
        super().__init__()
        self.tank = tank
        self.teleop = Teleop(tank.api)
        self.prompter = ModalPrompter(self)
        self._start = start_screen
        self._hold = hold
        self._teleop_task: asyncio.Task[None] | None = None

    def on_mount(self) -> None:
        self.install_screen(DashboardScreen(), "dashboard")
        self.install_screen(DriveScreen(), "drive")
        self.install_screen(ConsoleScreen(), "console")
        self.install_screen(ListScreen("Tests", sorted(p.stem for p in DEFS_DIR.glob("*.yaml")),
                                       "run_test_named"), "tests")
        self.install_screen(ListScreen("Calibration", list(CALCULATORS), "run_calibration"), "calibrate")
        self.install_screen(ListScreen("Tune (step test)", ["rate", "speed", "heading"], "run_tune"), "tune")
        self.install_screen(ListScreen("Sessions", [], "noop"), "sessions")
        self.push_screen(self._start)
        self._teleop_task = asyncio.create_task(self.teleop.run())
        if self._hold:
            from .teleop import HoldKeyboard

            HoldKeyboard(self.teleop, asyncio.get_running_loop()).start()
        self.tank.model.subscribe(self._on_update)
        self.set_interval(0.2, self._refresh)

    def status_text(self) -> str:
        f, s = self.tank.model.fast, self.tank.model.slow
        parts = [f"speed {self.teleop.cruise.speed}% steer {self.teleop.cruise.steer}%"]
        if f is not None:
            dist = "clear" if f.dist_cm is None else f"{f.dist_cm:.0f} cm"
            parts.append(f"yaw {f.yaw_deg:.1f}° rate {f.yaw_rate_dps:.0f}°/s dist {dist} enc {f.enc}")
        parts.append(f"batt {s.batt_v:.2f} V owner {s.owner}" if s is not None else "batt —")
        parts.append(f"link tx {self.tank.link.stats.tx_frames} rx {self.tank.link.stats.rx_frames}")
        return " · ".join(parts)

    def _refresh(self) -> None:
        for panel in self.screen.query(StatusPanel):
            panel.update(self.status_text())

    def _on_update(self, topic: str, payload: Any) -> None:
        if topic == "event":
            self.log_line(f"event {payload.kind} a={payload.a} b={payload.b}")

    def log_line(self, text: str) -> None:
        with_log = self.get_screen("dashboard").query("#events")
        for log in with_log:
            if isinstance(log, Log):
                log.write_line(text)

    def action_show(self, name: str) -> None:
        self.switch_screen(name)

    async def action_safe_quit(self) -> None:
        await self.tank.api.stop()
        if self._teleop_task is not None:
            self._teleop_task.cancel()
        self.exit()

    async def run_test_named(self, name: str) -> None:
        result = await TestRunner(self.tank, self.prompter, DEFS_DIR.parent / "sessions").run(
            load_test(DEFS_DIR / f"{name}.yaml")
        )
        self.log_line(f"{name}: {'PASS' if result.passed else 'FAIL'} {result.metrics}")

    async def run_calibration(self, item: str) -> None:
        svc = CalibrationService(self.tank, self.prompter)
        outcome = await svc.run(item)
        if not outcome.ok:
            self.log_line(f"{item}: FAILED — {outcome.error}")
            return
        rows = await svc.summary(outcome)
        answer = await self.prompter.ask(
            "Accept? " + ", ".join(f"{n} {o:g}→{v:g}" for n, o, v in rows) + " [y/n]"
        )
        if answer.lower().startswith("y"):
            self.log_line("saved" if await svc.accept(outcome) else "save failed")

    async def run_tune(self, loop: str) -> None:
        from .tuning import TuningService

        self.log_line(f"{loop}: {await TuningService(self.tank).step_test(loop, 45.0)}")

    async def noop(self, name: str) -> None:
        return None
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `cd tools/bench && uv run pytest tests/test_app.py -v`
Expected: 3 passed.

- [ ] **Step 5: Full suite, lint, type-check, commit**

Run: `cd tools/bench && uv run pytest && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: all tests pass; clean.
```bash
git add tools/bench/conqueror_bench/app.py tools/bench/tests/test_app.py
git commit -m "feat(bench): Textual dashboard over the bench services"
```

---

### Task 16: Hardware acceptance (needs Plan B firmware; WiFi part needs Plan C)

**Files:**
- Create: `tools/bench/README.md`

**Interfaces:**
- Consumes: the finished `bench` CLI; Uno flashed with Plan B firmware (`SHIELD_SMARTCAR_V11_TB6612`); for WiFi, ESP32 flashed with the Plan C bridge.

- [ ] **Step 1: Write the README**

`tools/bench/README.md`:
````markdown
# bench — Conqueror tank bench tool

```bash
cd tools/bench
uv sync                       # add --extra hold for pynput hold-to-drive
uv run bench --sim test all   # everything against the simulated Uno
uv run bench discover         # WiFi bridges + USB serial ports
uv run bench -p /dev/tty.usbserial-XXXX connect   # USB (shield switch S1 → USB)
uv run bench -H conqueror-bridge.local dash        # WiFi bridge
```
Driving (cruise control): W/S speed ±10 % · A/D steer · C centre · Q/E spin · SPACE STOP · arrows camera · H camera home · 1–9 quick tests · F1–F7 screens · Ctrl+Q quit (sends STOP).
Sessions land in `sessions/<date>_<time>_<test>/` (raw.bin, telemetry.csv, events.jsonl, inputs.json, result.json, report.md, plots/).
Safety: quitting, Ctrl-C or a crash sends STOP first; if the laptop dies the Uno stops by itself (no heartbeat for 300 ms). The IR remote always overrides.
````

- [ ] **Step 2: USB bring-up on the stand (tracks off the ground)**

Run: `cd tools/bench && uv run bench -p <port> connect`
Expected: `connected: HelloInfo(fw_major=1, …, encoders=True|False, reset_reason='POWER_ON' or 'UNKNOWN')`, then link stats with `bad_crc=0`.

Run: `uv run bench -p <port> probe`
Expected: `OK   both forward`, `OK   spin left`, `OK   level`, `OK   yaw_sign` (fix firmware pin map / IMU axis map on any FAIL before continuing).

- [ ] **Step 3: Calibrate on the floor (USB cable long enough, or after Plan C over WiFi)**

Run: `uv run bench -p <port> calibrate wizard` (or `-H conqueror-bridge.local`)
Expected: each item shows `old → new`, accepted items print `saved`.

- [ ] **Step 4: Standard tests over WiFi (Plan C bridge)**

Run: `uv run bench -H conqueror-bridge.local test all`
Expected: a report per test; target all PASS per the roadmap criteria (straight drift < 5 cm, heading < 2°, distance < 3 %; circle closure < 10 cm, radius < 10 %; square closure < 15 cm; spin < 2°; speed hold ±5 %; safety checks all seen). Re-run `speed_hold_30` on tile and carpet and `bench compare <A> <B>`.

- [ ] **Step 5: Commit**

```bash
git add tools/bench/README.md
git commit -m "docs(bench): usage and hardware acceptance"
```

---

## Done when

- `cd tools/bench && uv run pytest` passes (transports, sim, link, model, api, teleop, metrics, recorder, runner, reporter, calculators, calibration, tuning/probe, console/CLI, app) and ruff + pyright are clean.
- `uv run bench --sim test all` runs all six tests against the simulated Uno (scripted answers) and writes a report per test. `safety_checks` needs a lift that the sim cannot fake from the CLI, so it reports `lift_detected` FAIL there. That is expected; the runner test covers it with an injected event.
- Hardware acceptance (Task 16) completed once Plans B and C are flashed.
