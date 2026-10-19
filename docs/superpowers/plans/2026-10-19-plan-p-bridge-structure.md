# Plan P — Bridge structure: controllers and ports — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. **Owner's choice: native (executing-plans), one fresh reviewer at the end.**

**Goal:** Move the bridge's application logic out of `net/link_task.cpp` into host-tested `app/` controllers behind small link-time ports, without changing behaviour (except R15.5 request queueing), and add the size guard and the task watchdog.

**Architecture:** Bridge LLD Revision 15 (owner: "A with B"). `core/` keeps the pure deciders; the new `app/` layer holds one controller per area and reaches the outside only through `ports/` headers (free functions in `cq::ports`, real versions in `net/ports_esp32.cpp`, fakes in `src/fakes/` compiled only for the native env); `net/link_task.cpp` becomes the composition root and the 2 ms loop.

**Tech Stack:** C++17 (ESP32-S3 Arduino 3.x / ESP-IDF 5, PlatformIO envs `bridge`, `bridge_ota`, `native`), Unity host tests, Python 3.12 + uv (protogen, `scripts/size_check.py` with `lizard` 1.24.1).

**Spec:** `docs/lld/esp32-bridge-lld.md` Revision 15 (R15.1–R15.11), design-notes §97–§98, `docs/standards/coding-standards.md` §1.1, §3, §3.1.

## Global Constraints
- Branch `plan-p-bridge-structure` (from `plan-o`); Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; one logical change per commit.
- **Behaviour unchanged:** no Uno change, no protocol-byte change, no WebSocket-JSON change, no page change — the only deliberate change is R15.5 (acked requests queue instead of `BUSY`).
- STOP keeps BodyLink's urgent slot; STOP is never rate limited; motion posted before a STOP is dropped; R14 (one master, always on) unchanged.
- ESP32 rules: no `virtual` unless ≥ 2 implementations run at runtime; ports are link-time; allocation at startup only; no blocking in the link task; `app/` and `core/` never include Arduino / ESP-IDF / `net/` headers.
- Generated files are never edited by hand: change `protocol/generator`, run `cd protocol && uv run protogen write`.
- Sizes (standard §3.1): file ≤ 400 lines, function ≤ 40 lines of code (NLOC), cyclomatic complexity ≤ 10; exceptions only in `scripts/size_allow.txt` with a reason. At the end the bridge has **no** entries.
- Every task ends with: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native` green **and** `~/.platformio/penv/bin/pio run -e bridge` SUCCESS.
- Secrets are never printed; tank checks follow "stand or floor?" and STOP within reach.

## Review Focus
1. **A STOP arriving while a request (sound / camera home / setting) waits in the new queue** → the STOP still goes at once through the urgent slot and is answered; the queued request is answered later or dropped with BODY_OFFLINE — test `test_stop_overtakes_queued_requests` in Task 10.
2. **The link to the Uno drops while requests are queued or in flight** → every request with a client is answered BODY_OFFLINE once, none stays stuck, the next one after re-sync goes out — `test_body_offline_answers_and_clears_the_queue` in Task 10.
3. **The driver's phone closes while a move runs and another phone watches** → STOP sent, primitive ends (`primitive_done` broadcast), Log `client_lost`, master stays, watcher unaffected — `test_driver_close_mid_move` in Task 11.
4. **A command arrives for a client the root already removed (stale fd)** → no send to a reused fd, no crash — `test_reply_to_unknown_fd_is_dropped` in Task 4.
5. **OTA starts mid-drive with queued settings ops** → STOP sent, master stopped, every waiter answered (BUSY / TIMEOUT), clients closed — `test_ota_answers_every_waiter` in Task 12.

---

## Rulings made while planning (deviations from R15, owner informed with the plan)
- **R15.6 flat `WsCommand` kept.** Parsing becomes table-driven (one reader per key group, one completeness check per command), but `WsCommand` keeps its current flat POD fields: grouping fields into per-command structs would rewrite all 35 parser tests (our safety net) and every handler for no behaviour gain. The fat struct stays a documented exception in R15.6. Cost if wrong: the "interface segregation" point for the command struct stays open.
- **AckedOps' 600 ms timeout stays as a backstop** next to BodyLink's 200 ms (R15.5 says "one timeout"): BodyLink resets its slot without an event on `startMaster`/`stopMaster`; the backstop guarantees nothing waits forever. UnoChannel answers from the 200 ms event; the 600 ms path only fires if that event never comes.
- **Value types move to `core/`:** `WifiRequest`, `WifiResult`, `WifiSnapshot` (`core/wifi_types.h`) and `CamSettings`, `CamStats` (`core/cam_settings.h`) so `app/` never includes `net/`.
- **Logging is a port** (`ports/log_out.h`, `logLine(fmt, …)`), because the handlers log today.

## File structure
| File | Responsibility |
|---|---|
| `scripts/size_check.py`, `scripts/size_allow.txt`, `.githooks/pre-commit` | size guard (Task 1) |
| `firmware/esp32/src/core/ws_json_util.h/.cpp`, `ws_json_drive.cpp`, `ws_json_motion.cpp`, `ws_json_settings.cpp`, `ws_json_tank.cpp` (header `ws_json.h` kept) | JSON builders by page (Task 2) |
| `protocol/generator/protogen/emit_names.py` → `firmware/esp32/src/generated/cq_protocol_names.h` | generated name tables (Task 3) |
| `firmware/esp32/src/ports/*.h`, `src/net/ports_esp32.cpp`, `src/fakes/fake_ports.h/.cpp`, `src/core/wifi_types.h`, `src/core/cam_settings.h`, `src/app/replies.h/.cpp` | ports, fakes, shared reply helper (Task 4) |
| `src/core/ws_codec.cpp` | table-driven parsing (Task 5) |
| `src/app/tank_app_controller.h/.cpp` | Task 6 |
| `src/app/settings_controller.h/.cpp` | Task 7 |
| `src/app/telemetry_publisher.h/.cpp`, `src/app/uno_event_router.h/.cpp` | Task 8 |
| `src/app/motion_controller.h/.cpp` | Task 9 |
| `src/app/uno_channel.h/.cpp` (+ `core/acked_ops.h` grown) | Task 10 |
| `src/app/drive_controller.h/.cpp` | Task 11 |
| `src/app/command_router.h/.cpp`, `src/net/link_task.cpp` (root + loop), `src/main.cpp` (watchdog) | Task 12 |
| `docs/…`, `CLAUDE.md` | Task 13 |
| tests: `test/test_size_check` (Python, under `scripts/tests`), `test/test_replies`, `test/test_tank_app_controller`, `test/test_settings_controller`, `test/test_telemetry_publisher`, `test/test_uno_event_router`, `test/test_motion_controller`, `test/test_uno_channel`, `test/test_drive_controller`, `test/test_command_router` | |

## How a function "moves" (all controller tasks)
A moved function keeps its body; only these names change (the table is the contract — anything not in it stays as is):

| Today (`link_task.cpp`) | In a controller |
|---|---|
| `sendTo(fd, len)` | `m_out.to(fd, len)` |
| `broadcast(len)` | `m_out.all(len)` |
| `g_json, sizeof(g_json)` | `m_out.buf(), m_out.cap()` |
| `g_big, sizeof(g_big)` + `wsSend(fd, g_big, n)` | `m_out.big(), m_out.bigCap()` + `m_out.bigTo(fd, n)` |
| `nack(fd, id, r)` | `m_out.nack(fd, id, r)` |
| `if (id >= 0) sendTo(fd, jsonAck(…))` | `m_out.ack(fd, id)` |
| `streamReject(fd, r)` | `m_out.streamReject(fd, r)` |
| `logf(…)` | `ports::logLine(…)` |
| `tankMs()` | `m_uno.tankMs()` |
| `bodyOffline()` | `!m_uno.link().synced()` |
| `g_link.<x>` | `m_uno.link().<x>` |
| `g_ota_active` | `m_drive.otaActive()` (inside DriveController: `m_ota`) |
| `g_owner.<x>` | `m_drive.owner().<x>` (inside DriveController: `m_owner`) |
| `apply(a, now)` | `m_drive.applyOwner(a, now)` |
| `endMotion(e, now)` / `motionOwnedBy(fd)` | `m_motion.end(e, now)` / `m_motion.ownedBy(fd)` |
| `g_log.add(…)` | `m_log.add(…)` |
| `g_ws.<x>` | `m_ws.<x>` |
| `net::wifiPost/Poll/Snapshot` | `ports::wifiPost/Poll/Snapshot` |
| `net::cameraPost/Settings/Defaults/Stats`, `net::streamViewers()` | `ports::cameraPost/Settings/Defaults/Stats`, `ports::cameraViewers()` |
| `net::motionStoreSave/SaveRoll/Load/LoadRoll` | `ports::saveCpm/saveRoll/loadCpm/loadRoll` |
| `net::appAiUrlSave/Load` | `ports::saveAiUrl/loadAiUrl` |
| `millis()` in a handler | the `now` argument (pass it down) |
| `ESP.restart()` | `ports::systemRestart()` |

Each moved function must end ≤ 40 NLOC and CCN ≤ 10; split with a helper in the same controller when a body is longer (e.g. `handleTurn` → `checkTurn()` + `startTurn()`).

---

### Task 1: Size guard

**Files:**
- Create: `scripts/size_check.py`, `scripts/size_allow.txt`, `scripts/tests/test_size_check.py`
- Modify: `.githooks/pre-commit`, `docs/dev-setup.md` (one line: the hook needs `uv`)

**Interfaces:**
- Produces: `python3 scripts/size_check.py [--baseline]` → exit 0 / 1; allow-list lines `path:function  # reason` or `path  # reason` (file size).

- [ ] **Step 1: Write the failing test**

```python
# scripts/tests/test_size_check.py
import importlib.util
import pathlib

spec = importlib.util.spec_from_file_location("size_check", pathlib.Path(__file__).parents[1] / "size_check.py")
assert spec and spec.loader
sc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sc)


def test_function_over_limits_is_reported() -> None:
    rows = [sc.Fn(file="a.cpp", name="big", nloc=41, ccn=3), sc.Fn(file="a.cpp", name="ok", nloc=40, ccn=10)]
    assert sc.violations(rows, {"a.cpp": 100}, set()) == ["a.cpp:big  nloc 41 > 40"]


def test_complexity_and_file_size() -> None:
    rows = [sc.Fn(file="b.cpp", name="knot", nloc=10, ccn=11)]
    assert sc.violations(rows, {"b.cpp": 401}, set()) == ["b.cpp  401 lines > 400", "b.cpp:knot  ccn 11 > 10"]


def test_allow_list_silences() -> None:
    rows = [sc.Fn(file="a.cpp", name="big", nloc=90, ccn=30)]
    assert sc.violations(rows, {"a.cpp": 900}, {"a.cpp", "a.cpp:big"}) == []


def test_parse_allow_ignores_comments() -> None:
    assert sc.parse_allow("a.cpp:f  # long table\n\n# note\nb.cpp  # big\n") == {"a.cpp:f", "b.cpp"}
```

- [ ] **Step 2: Run it — fails**

Run: `uv run --with pytest pytest -q scripts/tests/test_size_check.py`
Expected: FAIL (`size_check.py` missing).

- [ ] **Step 3: Implement**

```python
#!/usr/bin/env python3
"""Size guard (coding standard §3.1, bridge LLD R15.10): files <= 400 lines, functions <= 40 NLOC, CCN <= 10.

Checks firmware/esp32/src and firmware/uno/src (generated code excluded) with lizard. Exceptions live in
scripts/size_allow.txt as `path:function  # reason` or `path  # reason`. --baseline prints today's offenders
in that format (used once to seed the list).
"""

from __future__ import annotations

import csv
import io
import pathlib
import subprocess
import sys
from dataclasses import dataclass

ROOT = pathlib.Path(__file__).resolve().parents[1]
DIRS = ["firmware/esp32/src", "firmware/uno/src"]
MAX_FILE, MAX_NLOC, MAX_CCN = 400, 40, 10


@dataclass(frozen=True)
class Fn:
    file: str
    name: str
    nloc: int
    ccn: int


def parse_allow(text: str) -> set[str]:
    out: set[str] = set()
    for line in text.splitlines():
        entry = line.split("#", 1)[0].strip()
        if entry:
            out.add(entry)
    return out


def violations(fns: list[Fn], file_lines: dict[str, int], allow: set[str]) -> list[str]:
    out: list[str] = []
    for path, n in sorted(file_lines.items()):
        if n > MAX_FILE and path not in allow:
            out.append(f"{path}  {n} lines > {MAX_FILE}")
    for f in fns:
        key = f"{f.file}:{f.name}"
        if key in allow:
            continue
        if f.nloc > MAX_NLOC:
            out.append(f"{key}  nloc {f.nloc} > {MAX_NLOC}")
        if f.ccn > MAX_CCN:
            out.append(f"{key}  ccn {f.ccn} > {MAX_CCN}")
    return out


def sources() -> list[pathlib.Path]:
    out: list[pathlib.Path] = []
    for d in DIRS:
        for p in sorted((ROOT / d).rglob("*")):
            if p.suffix in {".cpp", ".h"} and "generated" not in p.parts:
                out.append(p)
    return out


def measure(files: list[pathlib.Path]) -> tuple[list[Fn], dict[str, int]]:
    cmd = ["uvx", "lizard@1.24.1", "--csv", "-l", "cpp", *map(str, files)]
    text = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    fns: list[Fn] = []
    for row in csv.reader(io.StringIO(text)):  # NLOC,CCN,token,PARAM,length,location,file,function,long_name,start,end
        rel = str(pathlib.Path(row[6]).resolve().relative_to(ROOT))
        fns.append(Fn(file=rel, name=row[7], nloc=int(row[0]), ccn=int(row[1])))
    lines = {str(p.relative_to(ROOT)): len(p.read_text(encoding="utf-8").splitlines()) for p in files}
    return fns, lines


def main() -> int:
    fns, lines = measure(sources())
    if "--baseline" in sys.argv:
        for v in violations(fns, lines, set()):
            print(v.split("  ")[0] + "  # baseline 2026-10-19 (Plan P empties the bridge entries)")
        return 0
    allow = parse_allow((ROOT / "scripts" / "size_allow.txt").read_text(encoding="utf-8"))
    bad = violations(fns, lines, allow)
    for v in bad:
        print(f"size_check: {v}", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run tests — pass; seed the allow-list**

Run: `uv run --with pytest pytest -q scripts/tests/test_size_check.py` → Expected: 4 passed.
Run: `python3 scripts/size_check.py --baseline | sort -u > scripts/size_allow.txt && python3 scripts/size_check.py; echo rc=$?`
Expected: `rc=0`; `scripts/size_allow.txt` lists `firmware/esp32/src/net/link_task.cpp` and its long functions, `ws_json.cpp`, `ws_codec.cpp:readField`, and Uno entries. Prepend the header line `# Size exceptions (coding standard §3.1). Bridge entries are removed by Plan P; Uno entries stay until a Uno refactor.`

- [ ] **Step 5: Hook**

`.githooks/pre-commit` becomes:
```bash
#!/usr/bin/env bash
set -euo pipefail
top="$(git rev-parse --show-toplevel)"
"$top/scripts/check-protocol.sh"
python3 "$top/scripts/size_check.py"
```
Run: `git commit --allow-empty -m "chore: hook smoke"` then `git reset --soft HEAD~1` → Expected: the hook runs and passes.

- [ ] **Step 6: Commit**

```bash
git add scripts/size_check.py scripts/size_allow.txt scripts/tests/test_size_check.py .githooks/pre-commit docs/dev-setup.md
git commit -m "build: pre-commit size check (files 400, functions 40 NLOC / CCN 10) with a baseline allow-list (LLD R15.10)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `ws_json` by page

**Files:**
- Create: `firmware/esp32/src/core/ws_json_util.h`, `ws_json_util.cpp`, `ws_json_drive.cpp`, `ws_json_motion.cpp`, `ws_json_settings.cpp`, `ws_json_tank.cpp`
- Delete: `firmware/esp32/src/core/ws_json.cpp`
- Keep: `firmware/esp32/src/core/ws_json.h` (declarations unchanged) — tests unchanged.

**Interfaces:**
- Produces (`ws_json_util.h`, namespace `cq::core::detail`): `size_t fmt(char* out, size_t cap, const char* f, ...)`, `size_t jsonEscape(char* out, size_t cap, const char* in)`, `bool bit(uint32_t v, unsigned b)` — today's file-static helpers, moved verbatim.

- [ ] **Step 1: Split** — move each builder verbatim:
  - `ws_json_drive.cpp`: `jsonNack`, `jsonNackStatus`, `jsonNackMessage`, `jsonAck`, `jsonWelcome`, `jsonTf`, `jsonTs`, `jsonUnoEvent`, `jsonControlChanged`, `jsonBody`, `jsonPong`, `reasonName`/`ownerName` and the nack name tables.
  - `ws_json_motion.cpp`: `jsonPrimitiveDone`, `jsonCalib`, `primName`, end-reason names, `jsonTd`, `jsonTds`.
  - `ws_json_settings.cpp`: `jsonWifiStatus`, `jsonWifiScan`, `jsonConfirm`, `jsonConfirmed`, `jsonConfirmExpired`, `jsonSetting`.
  - `ws_json_tank.cpp`: `unoEventName`, `jsonTank`, `jsonLog`.
  Each file includes `core/ws_json.h` and `core/ws_json_util.h` and opens `namespace cq { namespace core {` with `using namespace detail;`.
- [ ] **Step 2: Run** `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native` → Expected: 246/246; `pio run -e bridge` SUCCESS; `python3 ../../scripts/size_check.py` passes after removing the `ws_json.cpp` lines from `scripts/size_allow.txt` (every new file < 400 lines).
- [ ] **Step 3: Commit** `refactor(bridge): ws_json split by page (drive, motion, settings, tank) + shared util (LLD R15.7)`.

---

### Task 3: Generated name tables

**Files:**
- Create: `protocol/generator/protogen/emit_names.py`, `protocol/generator/tests/test_emit_names.py`
- Generated (by `protogen write`): `firmware/esp32/src/generated/cq_protocol_names.h`
- Modify: `protocol/generator/protogen/cli.py` (OUTPUTS), `firmware/esp32/src/core/ws_json_tank.cpp` (`unoEventName`), `ws_json_drive.cpp` (`ownerName`, ack-status names if present)

**Interfaces:**
- Produces (C++, `namespace cq::proto`, header-only, ESP32 / host only — the Uno never includes it): `const char* eventName(uint8_t kind)` ("unknown" past the end), `const char* ownerName(Owner o)`, `const char* ackStatusName(uint8_t s)`; names are the YAML names in lower case (`HEADING_FAULT` → `heading_fault`).

- [ ] **Step 1: Failing generator test**

```python
# protocol/generator/tests/test_emit_names.py
from pathlib import Path

from protogen.cli import OUTPUTS
from protogen.emit_names import emit_names
from protogen.loader import load

ROOT = Path(__file__).resolve().parents[3]
MODEL = load(ROOT / "protocol" / "defs")


def test_registered_as_output() -> None:
    assert OUTPUTS["firmware/esp32/src/generated/cq_protocol_names.h"] is emit_names


def test_every_event_named_at_its_kind() -> None:
    text = emit_names(MODEL)
    for ev in MODEL.events:
        assert f'/* {ev.kind} */ "{ev.name.lower()}"' in text


def test_gaps_are_unknown_and_owner_names_present() -> None:
    text = emit_names(MODEL)
    assert '/* 0 */ "unknown"' in text  # kind 0 is not an event
    assert "inline const char* ownerName(Owner o)" in text
```

Run: `cd protocol && uv run pytest -q generator/tests/test_emit_names.py` → Expected: FAIL (module missing).

- [ ] **Step 2: Implement `emit_names.py`**

```python
"""C++ name tables for the bridge (bridge LLD R15.7): eventName, ownerName, ackStatusName. ESP32 / host only."""

from __future__ import annotations

from .model import ProtocolModel


def _table(fn: str, arg: str, names: list[str]) -> list[str]:
    lines = [f"inline const char* {fn}({arg} v) {{", "  static const char* const kNames[] = {"]
    lines += [f'      /* {i} */ "{n}",' for i, n in enumerate(names)]
    lines += ["  };", "  const unsigned i = static_cast<unsigned>(v);",
              '  return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "unknown";', "}", ""]
    return lines


def emit_names(model: ProtocolModel) -> str:
    top = max(ev.kind for ev in model.events)
    events = ["unknown"] * (top + 1)
    for ev in model.events:
        events[ev.kind] = ev.name.lower()
    owner = next(e for e in model.enums if e.name == "Owner")
    owners = ["unknown"] * (max(owner.values.values()) + 1)
    for k, v in owner.values.items():
        owners[v] = k.lower()
    lines = ["// generated by protogen (emit_names.py) - do not edit", "#pragma once", "#include <stdint.h>",
             '#include "cq_protocol.h"', "", "namespace cq {", "namespace proto {", ""]
    lines += _table("eventName", "uint8_t", events)
    lines += _table("ownerName", "Owner", owners)
    lines += _table("ackStatusName", "uint8_t", [s.lower() for s in model.ack_status])
    lines += ["}  // namespace proto", "}  // namespace cq", ""]
    return "\n".join(lines)
```
Register in `cli.py` OUTPUTS: `"firmware/esp32/src/generated/cq_protocol_names.h": emit_names,` (import `from .emit_names import emit_names`).

- [ ] **Step 3: Generate and pass**

Run: `cd protocol && uv run pytest -q && uv run protogen write && uv run protogen check` → Expected: all pass; new file written.

- [ ] **Step 4: Use it** — in `ws_json_tank.cpp` replace the body of `unoEventName(kind)` with `return proto::eventName(kind);` (keep the function — tests call it) and delete its hand table + `static_assert`; in `ws_json_drive.cpp` `ownerName(o)` returns `proto::ownerName(o)` **only if** the existing strings equal the YAML names lower-cased (check `test_ws_json`: if a test expects a different spelling, keep the hand table and note it in the ledger). Include `"generated/cq_protocol_names.h"`.
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native` → 246/246 (the existing `jsonLog` / event tests pin the names). `pio run -e bridge` SUCCESS. The Uno build is untouched: `cd ../uno && ~/.platformio/penv/bin/pio run -e uno | grep Flash` → Expected unchanged 31144 B.
- [ ] **Step 5: Commit** `feat(protocol): generated event / owner / ack-status name tables for the bridge (LLD R15.7)`.

---

### Task 4: Ports, fakes, value types, `Replies`

**Files:**
- Create: `firmware/esp32/src/ports/client_out.h`, `log_out.h`, `motion_store.h`, `app_store.h`, `wifi_control.h`, `camera_control.h`, `system_control.h`; `src/net/ports_esp32.cpp`; `src/fakes/fake_ports.h`, `src/fakes/fake_ports.cpp`; `src/core/wifi_types.h`, `src/core/cam_settings.h`; `src/app/replies.h`, `src/app/replies.cpp`; `test/test_replies/test_main.cpp`
- Modify: `firmware/esp32/platformio.ini` (`[esp32] build_src_filter = +<*> -<fakes/>`; `[env:native] build_src_filter = +<core/> +<app/> +<fakes/>`), `src/net/wifi_manager.h` and `src/net/camera_service.h` (types moved: `using WifiRequest = core::WifiRequest;` etc. so `net/` call sites stay unchanged)

**Interfaces (Produces — exact):**
```cpp
// ports/client_out.h
namespace cq { namespace ports {
bool clientSend(int fd, const char* json, size_t len);  // false: not queued (full / too big) - counted by the caller
bool clientClose(int fd);                                // false: no live WebSocket session on fd
}}
// ports/log_out.h
namespace cq { namespace ports { void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2))); }}
// ports/motion_store.h
namespace cq { namespace ports { uint16_t loadCpm(); bool saveCpm(uint16_t v); uint16_t loadRoll(); bool saveRoll(uint16_t v); }}
// ports/app_store.h
namespace cq { namespace ports { bool loadAiUrl(char* out, size_t cap); bool saveAiUrl(const char* url); }}
// ports/wifi_control.h
namespace cq { namespace ports {
bool wifiPost(const core::WifiRequest& r); bool wifiPoll(core::WifiResult& out); void wifiSnapshot(core::WifiSnapshot& out); }}
// ports/camera_control.h
namespace cq { namespace ports {
bool cameraPost(const core::CamSettings& s); core::CamSettings cameraSettings(); core::CamSettings cameraDefaults();
core::CamStats cameraStats(uint32_t now_ms); uint8_t cameraViewers(); }}
// ports/system_control.h
namespace cq { namespace ports { void systemRestart(); uint8_t resetReason(); }}

// app/replies.h
namespace cq { namespace app {
class Replies {
 public:
  explicit Replies(core::WsClients& ws) : m_ws(ws) {}
  char* buf() { return m_json; }
  size_t cap() const { return sizeof(m_json); }
  char* big() { return m_big; }
  size_t bigCap() const { return sizeof(m_big); }
  void to(int fd, size_t len);       // m_json -> fd; nothing when len == 0 or fd is not a client
  void bigTo(int fd, size_t len);    // m_big -> fd; logs "<kind> reply to <name> not sent" when it fails
  void all(size_t len);              // m_json -> every client
  void ack(int fd, int32_t id);      // only when id >= 0
  void nack(int fd, int32_t id, core::BridgeReason r);
  void nackStatus(int fd, int32_t id, uint8_t status);
  void streamReject(int fd, core::BridgeReason r);  // once per stream (WsClients::reportStreamReject)
 private:
  core::WsClients& m_ws;
  char m_json[core::kWsJsonMax];
  char m_big[core::kWsBigMax];
};
}}
// fakes/fake_ports.h (host only)
namespace cq { namespace fakes {
struct Sent { int fd; std::string json; };
struct World {
  std::vector<Sent> sent; std::vector<int> closed; std::vector<std::string> logs;
  bool send_ok = true; uint16_t cpm = 7514, roll = 27; bool save_ok = true; std::vector<uint16_t> saved_cpm, saved_roll;
  std::string ai_url; std::vector<core::WifiRequest> wifi_posted; std::deque<core::WifiResult> wifi_results;
  core::WifiSnapshot wifi; std::vector<core::CamSettings> cam_posted; core::CamSettings cam; core::CamStats cam_stats;
  uint8_t viewers = 0; bool cam_ok = true; int restarts = 0; uint8_t reset_reason = 1;
};
World& world();            // one per test binary
void reset();              // world() = World{}
std::string lastTo(int fd);  // last json sent to fd ("" if none)
}}
```

- [ ] **Step 1: Failing test** (`test/test_replies/test_main.cpp`)

```cpp
#include <unity.h>
#include "app/replies.h"
#include "fakes/fake_ports.h"
using namespace cq;

core::WsClients ws;
app::Replies out(ws);
void setUp() { fakes::reset(); ws = core::WsClients(); ws.add(3, 0); }
void tearDown() {}

void test_ack_only_with_an_id() {
  out.ack(3, -1);
  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());
  out.ack(3, 7);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ack\",\"id\":7}", fakes::lastTo(3).c_str());
}
void test_reply_to_unknown_fd_is_dropped() {  // Review Focus 4
  out.nack(9, 1, core::BridgeReason::kBusy);
  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());
}
void test_all_reaches_every_client() {
  ws.add(4, 0);
  const size_t n = static_cast<size_t>(snprintf(out.buf(), out.cap(), "{\"t\":\"x\"}"));
  out.all(n);
  TEST_ASSERT_EQUAL(2, fakes::world().sent.size());
}
void test_big_reply_failure_is_logged() {
  fakes::world().send_ok = false;
  const size_t n = static_cast<size_t>(snprintf(out.big(), out.bigCap(), "{\"t\":\"log\"}"));
  out.bigTo(3, n);
  TEST_ASSERT_EQUAL(1, fakes::world().logs.size());
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ack_only_with_an_id);
  RUN_TEST(test_reply_to_unknown_fd_is_dropped);
  RUN_TEST(test_all_reaches_every_client);
  RUN_TEST(test_big_reply_failure_is_logged);
  return UNITY_END();
}
```
Run: `pio test -e native -f test_replies` → Expected: FAIL (headers missing).

- [ ] **Step 2: Implement** — the port headers above; `core/wifi_types.h`, `core/cam_settings.h` (structs moved verbatim from `net/wifi_manager.h`, `net/camera_service.h`; `net/` headers keep `using` aliases); `fakes/fake_ports.cpp` defines every `cq::ports` function against `world()` (send appends `{fd, json}` when `send_ok`; `logLine` vsnprintf's into `logs`); `net/ports_esp32.cpp` defines every `cq::ports` function by forwarding to today's `net::` functions (`clientSend` → `wsSend`, `clientClose` → `wsClose`, `logLine` → `vlogf`/`logf` (add a `va_list` variant to `net/log.h` if missing), `systemRestart` → `ESP.restart()`, `resetReason` → `esp_reset_reason()`, `cameraViewers` → `net::streamViewers()`); `app/replies.cpp` implements `Replies` with the json builders (`core::jsonAck`, `jsonNack`, `jsonNackStatus`) and `ports::clientSend` / `ports::logLine`.
- [ ] **Step 3: Run** `pio test -e native` → Expected: 246 + 4 pass; `pio run -e bridge` SUCCESS (the bridge does not use `Replies` yet).
- [ ] **Step 4: Commit** `refactor(bridge): ports (link-time), host fakes, value types in core, Replies helper (LLD R15.3)`.

---

### Task 5: Table-driven command parsing

**Files:** Modify `firmware/esp32/src/core/ws_codec.cpp` only (header and tests unchanged).

**Interfaces:** `WsCommand parseWsCommand(const char* text, size_t len)` unchanged.

- [ ] **Step 1: Pin first** — run `pio test -e native -f test_ws_codec` → Expected: 37/37 pass (the safety net for this task).
- [ ] **Step 2: Restructure `readField`** (122 lines) into a table of key readers:
```cpp
using KeyReader = bool (*)(Reader& r, WsCommand& c, Seen& s);
struct KeyRow { const char* key; KeyReader read; };
// VARIANT POINT: one row per JSON key; a reader parses its value into the flat WsCommand (R15.6 ruling).
const KeyRow kKeys[] = {
  {"t", readType}, {"id", readId}, {"v", readV}, {"w", readW}, {"x", readX}, {"y", readY}, {"active", readActive},
  {"on", readOn}, {"name", readSoundName}, {"mode", readMode}, {"client", readClient}, {"proto", readProto},
  {"t0", readT0}, {"hz", readHz}, {"by", readMotion}, {"to", readMotion}, {"speed", readMotion}, {"radius_cm", readMotion},
  {"yaw_rate", readMotion}, {"pivot", readPivot}, {"dist_cm", readDist}, {"counts_per_m", readCpm}, {"reset", readReset},
  {"slot", readSlot}, {"ssid", readSsid}, {"password", readPassword}, {"setting_id", readSettingId}, {"value", readValue},
  {"since", readSince}, {"size", readCamSize}, {"quality", readCamQuality}, {"fps_cap", readCamFps},
  {"brightness", readCamBright}, {"contrast", readCamContrast}, {"low_light", readCamLow}, {"vflip", readCamVflip},
  {"hmirror", readCamHmirror}, {"ai_url", readAiUrl},
};
bool readField(Reader& r, const char* key, WsCommand& c, Seen& s) {
  for (const KeyRow& k : kKeys) if (strcmp(key, k.key) == 0) return k.read(r, c, s);
  return r.skip();  // unknown keys are ignored (as today)
}
```
Each `readX` is today's corresponding branch body, verbatim (the existing branch for that key). `complete(c, s)` becomes a table `{WsCmdKind, bool (*ok)(const WsCommand&, const Seen&)}` with today's per-kind conditions verbatim. The keys and conditions must match today's branches exactly — copy, do not rewrite.
- [ ] **Step 3: Run** `pio test -e native -f test_ws_codec` → Expected: 37/37; whole suite green; `python3 scripts/size_check.py` with `ws_codec.cpp:readField` removed from the allow-list → passes (every reader ≤ 40 NLOC, CCN ≤ 10).
- [ ] **Step 4: Commit** `refactor(bridge): command parsing by table - one reader per key, one check per command (LLD R15.6)`.

---

### Task 6: TankAppController

**Files:** Create `src/app/tank_app_controller.h/.cpp`, `test/test_tank_app_controller/test_main.cpp`; modify `src/net/link_task.cpp` (delete `sendTank`, `sendLog`, `handleCameraSet`, `handleSelfTest`, `handleAppSet`, `g_ai_url`, `g_big`; dispatch to the controller).

**Interfaces:**
- Consumes: `app::Replies`, `core::EventLog`, `core::WsClients`, ports `camera_control`, `app_store`; **until Task 10** the self-test request is posted through a callback `bool (*queueSelfTest)(int fd, int32_t id)` set by the root (link_task's existing `g_ops.push`), and the standing-still check through `bool (*canSelfTest)(int fd, int32_t id)` returning false after answering — both replaced by `UnoChannel` / `DriveController` calls in Tasks 10–11.
- Produces:
```cpp
class TankAppController {
 public:
  TankAppController(Replies& out, core::EventLog& log, const core::BodyLink& link);
  void begin();                                           // loads the AI address (ports::loadAiUrl)
  void onTank(int fd, uint32_t now);
  void onLog(int fd, uint32_t since, uint32_t now);
  void onCameraSet(int fd, int32_t id, const core::WsCommand& c, uint32_t now);
  void onAppSet(int fd, int32_t id, const core::WsCommand& c);
  void onSelfTest(int fd, int32_t id);                    // uses the two callbacks above until Task 10
  const char* aiUrl() const { return m_ai_url; }
  core::CamSettings merge(const core::CamFields& f) const; // pure: today's handleCameraSet field merge
  void setSelfTestHooks(bool (*can)(int, int32_t), bool (*queue)(int, int32_t));
};
```
- [ ] **Step 1: Failing tests** — `test_tank_reply_has_camera_and_ai_url`, `test_log_pages_and_reply_is_big` (35 entries → first reply 25), `test_camera_set_merges_only_sent_fields_and_acks_then_tank` (posts `{vflip:false}` → `world().cam_posted.back().vflip == false`, quality unchanged, replies `ack` then `tank`), `test_camera_set_without_camera_is_unavailable` (`cam_ok = false`), `test_app_set_saves_and_acks` / `test_app_set_save_failure_is_busy` (`save_ok = false`), `test_selftest_refused_while_driving_is_answered_once` (hook `can` returns false → no queue call). Write them with the fakes as in Task 4; each asserts the exact JSON via `fakes::lastTo(fd)`.
- [ ] **Step 2: Run** → FAIL (header missing).
- [ ] **Step 3: Implement** by moving the five functions per the mapping table (`millis()` → `now`; `sendTank`'s `posted` pointer becomes `onCameraSet` calling a private `sendTank(fd, now, &merged)`).
- [ ] **Step 4: Wire** — link_task: a static `app::Replies g_out(g_ws);` (shared from now on: `sendTo/broadcast/nack` in link_task call `g_out.to/all/nack` — keep the old function names as one-line wrappers until Task 12), `app::TankAppController g_tank(g_out, g_log, g_link);`, `g_tank.begin()` in `linkTaskStart`; dispatch cases `kTank/kLog/kCameraSet/kSelfTest/kAppSet` call the controller.
- [ ] **Step 5: Run** native suite + `pio run -e bridge` → green / SUCCESS.
- [ ] **Step 6: Commit** `refactor(bridge): TankAppController (tank, log, camera, self-test, AI address) behind ports (LLD R15.4)`.

---

### Task 7: SettingsController

**Files:** Create `src/app/settings_controller.h/.cpp`, `test/test_settings_controller/test_main.cpp`; modify `link_task.cpp` (delete `settingReadable`, `pushFeedback`, `wifiIsAp`, `sendWifiStatus`, `askConfirm`, `handleSettingsCommand`, `applyChange`, `onButtonPress`, the confirmation / WiFi-result / restart part of `settingsTick`, `handleCalib`'s confirmation part; `g_gate`, `g_button`, `g_restart_at`).

**Interfaces:**
- Consumes: `Replies`, `EventLog`, `WsClients`, ports `wifi_control`, `motion_store`, `system_control`; motion facts through `const MotionFacts& m` = `struct MotionFacts { bool (*active)(); uint16_t* cpm; uint16_t* roll_mm; int (*driverFd)(); void (*sendCalib)(int fd); };` filled by the root (replaced by `MotionController&` / `DriveController&` in Tasks 9 / 11); Uno requests through `bool (*queueOp)(const core::AckedOp&)` (replaced by `UnoChannel::request` in Task 10).
- Produces:
```cpp
class SettingsController {
 public:
  SettingsController(Replies& out, core::EventLog& log, core::WsClients& ws);
  void onCommand(int fd, int32_t id, const core::WsCommand& c, uint32_t now);  // wifi_*, ap_password, setting, restart
  void onCalib(int fd, int32_t id, const core::WsCommand& c, uint32_t now);    // read / set / reset (today's handleCalib)
  void onButtonPress(uint32_t now);
  void onClientClosed(int fd);                                                  // ConfirmGate::dropClient
  void tick(uint32_t now);  // confirmation expiry, WiFi results (+ Log kWifiMode), due restart
  void setHooks(const MotionFacts& m, bool (*queueOp)(const core::AckedOp&));
};
```
- [ ] **Step 1: Failing tests** — `test_change_waits_for_the_button_then_posts_wifi` (wifi_mode → `confirm`; `onButtonPress` → `world().wifi_posted.size()==1` and two feedback ops queued), `test_double_press_toggles_wifi_with_feedback` (two presses within 1 s, no change waiting), `test_another_devices_change_is_refused_busy`, `test_calib_set_only_for_the_driver_and_not_while_moving`, `test_calib_save_failure_is_busy` (`save_ok=false`), `test_expired_change_tells_its_client`, `test_wifi_mode_result_is_logged`, `test_restart_is_acked_logged_and_due_after_500_ms` (`tick(now+499)` → `restarts==0`; `tick(now+500)` → `1`), `test_setting_write_only_track_balance_in_range`.
- [ ] **Step 2–5:** run → FAIL; move per mapping; wire (`g_settings`, dispatch `kWifi*`, `kApPassword`, `kSetting`, `kRestart`, `kCalib`; `handleLinkEvents` button → `g_settings.onButtonPress`; `kWsClose` → `g_settings.onClientClosed`; loop → `g_settings.tick`; the acked-op sending part of `settingsTick` stays in link_task until Task 10 as `opsTick(now)`); suite + build green.
- [ ] **Step 6: Commit** `refactor(bridge): SettingsController (WiFi, calibration, confirmations, tank button, restart) (LLD R15.4)`.

---

### Task 8: TelemetryPublisher and UnoEventRouter

**Files:** Create `src/app/telemetry_publisher.h/.cpp`, `src/app/uno_event_router.h/.cpp`, `test/test_telemetry_publisher/test_main.cpp`, `test/test_uno_event_router/test_main.cpp`; modify `link_task.cpp` (delete `broadcastTs`, `sendDebug`, the broadcast part of `telemetry`, `handleHello`, `publishStatus`'s field gathering, `handleLinkEvents`' routing; seq counters).

**Interfaces:**
```cpp
class TelemetryPublisher {
 public:
  TelemetryPublisher(Replies& out, core::WsClients& ws, const core::BodyLink& link);
  void setOwnerView(core::OwnerView (*view)());        // DriveController::ownerView after Task 11
  void setPrimitiveView(core::PrimitiveView (*view)()); // MotionController after Task 9
  void tick(uint32_t now);                 // td / tds per client, tf every 5th new TELEM_FAST, ts at 1 Hz
  void sendTs();                           // broadcast ts now (owner changed)
  void hello(int fd, const char* fw, const char* sensor);  // welcome + ts to one client
  uint32_t nextEventSeq() { return ++m_ev_seq; }
};
class UnoEventRouter {
 public:
  struct Sinks {  // filled by the root; each may be null in tests
    void (*bodyOnline)(uint32_t now); void (*bodyOffline)(uint8_t code, int16_t a, uint32_t now);
    void (*unoOwner)(uint8_t owner, uint32_t now); void (*buttonPress)(uint32_t now);
    void (*motionEvent)(uint8_t uno_event, uint32_t now);  // stall / encoder fault
    void (*ackResult)(uint8_t seq, uint8_t status); void (*stopResult)(uint8_t status);
    void (*settingValue)(uint8_t id, int32_t value);
  };
  UnoEventRouter(Replies& out, core::EventLog& log, TelemetryPublisher& tel, const Sinks& sinks);
  void route(const core::LinkEvent& ev, uint32_t tank_ms, uint32_t now);  // broadcast + Log (minus button / unblocked) + sink
};
```
- [ ] **Step 1: Failing tests** — publisher: `test_tf_every_fifth_fast_frame`, `test_ts_once_per_second_even_without_frames`, `test_td_only_to_clients_that_asked_at_their_divider`, `test_hello_sends_welcome_then_ts`; router: `test_uno_event_broadcast_and_logged_except_button_and_unblocked`, `test_button_press_goes_to_the_button_sink`, `test_stall_and_encoder_fault_reach_motion`, `test_body_offline_broadcast_logged_and_sunk`, `test_control_changed_reaches_owner_sink`.
- [ ] **Step 2–5:** FAIL → move per mapping → wire (root fills `Sinks` with lambdas without captures calling the static controllers / today's link_task functions) → green.
- [ ] **Step 6: Commit** `refactor(bridge): TelemetryPublisher and UnoEventRouter (LLD R15.4)`.

---

### Task 9: MotionController

**Files:** Create `src/app/motion_controller.h/.cpp`, `test/test_motion_controller/test_main.cpp`; modify `link_task.cpp` (delete `runMotion`, `endMotion`, `motionOwnedBy`, `telemOf`, `handleTurn`, `handleDriveRate`, `handleMove`, `handleCancel`, `sendCalib`, the roll-on part of `telemetry`; `g_motion`, `g_roll`, `g_cpm`, `g_roll_mm`, `g_roll_*`).

**Interfaces:**
```cpp
class MotionController {
 public:
  MotionController(Replies& out, core::EventLog& log, core::BodyLink& link, core::LinkOwner& owner);
  void begin();  // loads cpm and roll-on (ports)
  void setOwnerHooks(core::Decision (*claim)(int fd, uint32_t now), bool (*ota)());  // DriveController after Task 11
  void onTurn(int fd, int32_t id, const core::WsCommand& c, uint32_t now);
  void onDriveRate(int fd, const core::WsCommand& c, uint32_t now);
  void onMove(int fd, int32_t id, const core::WsCommand& c, uint32_t now);
  void onCancel(int fd, int32_t id, uint32_t now);
  void onTelemetry(uint32_t now);              // new TELEM_FAST: roll-on observe + service step
  void tick(uint32_t now);                     // telemetry gap / drive_rate deadman
  void end(core::MotionEvent e, uint32_t now);
  bool ownedBy(int fd) const;
  bool active() const;
  void cancelRollOn();                          // a drive / new primitive
  uint16_t cpm() const; uint16_t rollMm() const;
  bool setCpm(uint16_t v);                      // saves through ports; false on failure
  void resetCpm(); void resetRoll();
  void sendCalib(int fd);
  core::PrimitiveView view() const;
};
```
- [ ] **Step 1: Failing tests** — `test_turn_needs_ownership_and_acks`, `test_turn_refused_busy_during_ota`, `test_arc_out_of_band_is_invalid_with_message`, `test_move_refused_with_encoder_fault`, `test_move_end_not_done_is_logged_and_broadcast`, `test_cancel_by_non_owner_is_not_owner`, `test_roll_on_learned_after_a_done_move_and_saved`, `test_end_on_stop_sends_stop_frame` (BodyLink fake via a real `core::BodyLink` over a `core::FrameTx` — assert a STOP frame queued with `tx.peek`).
- [ ] **Step 2–5:** FAIL → move (split `handleTurn` into `checkTurn` + `startTurn`) → wire (`setOwnerHooks` with link_task's `g_owner.onDrive`+`apply` wrapper until Task 11; `telemetry()` calls `g_motionc.onTelemetry/tick`; publisher's primitive view → `g_motionc.view()`; settings' `MotionFacts` → motion controller functions) → green.
- [ ] **Step 6: Commit** `refactor(bridge): MotionController (turn, move, drive_rate, cancel, roll-on, counts per metre) (LLD R15.4)`.

---

### Task 10: UnoChannel — one owner of the answer slot

**Files:** Create `src/app/uno_channel.h/.cpp`, `test/test_uno_channel/test_main.cpp`; modify `src/core/acked_ops.h` (AckedOp kinds `kCamHome`, `kCamLock`, `kAudioMode` added; fields unchanged), `link_task.cpp` (delete `sendAckedCommand`, `opsTick`, `stopLinkForOta`, the stop-waiter / cam-waiter / ack routing; `g_ops`, `g_cam_waiter`, `g_cam_seq`, `g_stop_waiters`).

**Interfaces:**
```cpp
class UnoChannel {
 public:
  UnoChannel(core::BodyLink& link, core::FrameTx& tx, Replies& out, core::WsClients& ws);
  bool request(const core::AckedOp& op);          // false: queue full (8) - the caller nacks BUSY
  void stop(int fd, int32_t id, uint32_t now);    // urgent STOP + waiter (answered on kStopResult)
  void stopNoReply(uint32_t now);                 // STOP from owner actions / motion
  void onAck(uint8_t seq, uint8_t status);
  void onStopResult(uint8_t status);
  void onSettingValue(uint8_t id, int32_t value);
  void onBodyOffline();                           // every queued / in-flight request answered BODY_OFFLINE and dropped
  void tick(uint32_t now);                        // send the next request; 600 ms backstop
  void stopForOta();                              // stopMaster, clear tx, answer all (BUSY / TIMEOUT)
  core::BodyLink& link() { return m_link; }
  const core::BodyLink& link() const { return m_link; }
  uint32_t tankMs() const;
};
```
Answer rules (R15.5): status 0 → `ack` to `fd ≥ 0`, except `kSettingGet` (answered by its VALUE); status ≠ 0 → `nackStatus`; BodyLink timeout event (`kStatusTimeout`) → `nack TIMEOUT`; body offline → `nack BODY_OFFLINE`.
- [ ] **Step 1: Failing tests** — `test_requests_go_one_at_a_time_in_order`, `test_drive_page_request_waits_instead_of_busy` (sound while LED in flight → no nack; answered `ack` after both ACKs), `test_ninth_request_is_refused` (`request()` false), `test_setting_get_answered_by_value`, `test_timeout_answers_timeout_and_moves_on`, **`test_stop_overtakes_queued_requests`** (Review Focus 1: 3 requests queued, `stop(fd,9)` → STOP frame first in `tx`, ack 9 on stop result; queued requests still sent after), **`test_body_offline_answers_and_clears_the_queue`** (Review Focus 2), `test_ota_answers_everything_and_stops_master`.
- [ ] **Step 2–5:** FAIL → implement (merge today's `handleCamHome`/`handleAcked` slot logic, `settingsTick` op sending, `kAckResult`/`kStopResult`/`kSettingValue` routing, `stopLinkForOta`) → wire (router sinks `ackResult/stopResult/settingValue` → channel; body offline sink also calls `onBodyOffline`; Settings/TankApp hooks `queueOp`/`queueSelfTest` → `channel.request`; `handleCamHome`/`handleAcked` push requests) → green.
- [ ] **Step 6: Commit** `refactor(bridge): UnoChannel - one queue for every acked Uno request; drive-page requests wait instead of BUSY (LLD R15.5)`.

---

### Task 11: DriveController (drive / STOP pinned first)

**Files:** Create `src/app/drive_controller.h/.cpp`, `test/test_drive_controller/test_main.cpp`; modify `link_task.cpp` (delete `ownerView`, `apply`, `reasonOf`, `streamReject`, `handleStop`, `admitted`, `handleDrive`, `handleCamJoy`, `handleCamHome`, `handleAcked`, `handleDebug`, `handleRelease`, `reapSilent`, the kWsOpen / kWsClose / kOtaStart branches; `g_owner`, `g_stop_barrier`, `g_ota_active`).

**Interfaces:**
```cpp
class DriveController {
 public:
  DriveController(Replies& out, core::WsClients& ws, core::EventLog& log, UnoChannel& uno,
                  MotionController& motion, SettingsController& settings, TelemetryPublisher& tel);
  void begin(uint32_t now);                                  // LinkOwner::begin (R14)
  core::Decision claim(int fd, uint32_t now, bool moving);  // onDrive + applyOwner (for motion)
  void applyOwner(const core::Actions& a, uint32_t now);
  core::OwnerView ownerView() const;
  core::LinkOwner& owner();
  bool otaActive() const;
  bool admitted(int fd, core::WsCmdKind kind, int32_t id, uint32_t post_seq, uint32_t now);
  void onStop(int fd, int32_t id, uint32_t post_seq, uint32_t now);
  void onDrive(int fd, const core::WsCommand& c, uint32_t now);
  void onCamJoy(int fd, const core::WsCommand& c);
  void onUnoRequest(int fd, int32_t id, const core::WsCommand& c);  // cam_home / cam_lock / sound / audio_mode
  void onRelease(int fd, int32_t id, uint32_t now);
  void onDebug(int fd, int32_t id, const core::WsCommand& c);
  void onClientOpen(int fd, uint32_t now);
  void onClientClose(int fd, uint32_t now);
  void onUnoOwner(uint8_t owner, uint32_t now); void onSynced(bool synced, uint32_t now);
  void startOta(uint32_t now);
  void tick(uint32_t now);  // owner handback, slow-frame owner, silent clients
};
```
- [ ] **Step 1: Pin today's rules first** (tests written against the controller API, all must pass right after the move with no logic edits):
  `test_stop_from_anyone_is_sent_resent_until_ack_and_answered`, `test_stop_is_never_rate_limited`, `test_motion_posted_before_a_stop_is_dropped`, `test_stop_from_non_driver_holds_until_stick_released` (§72), `test_stop_hold_is_logged_with_the_stoppers_name`, `test_handback_after_5_s_sends_stop`, `test_drive_before_sync_is_body_offline`, `test_second_phone_gets_not_owner_once_per_stream`, **`test_driver_close_mid_move`** (Review Focus 3: STOP, `primitive_done`, Log `client_lost`, master stays, watcher gets no nack), `test_release_ends_a_primitive`, `test_ir_takeover_drops_the_driver`, `test_ota_refuses_drive_busy`, `test_cam_home_waits_in_the_queue`.
- [ ] **Step 2:** run → FAIL (header missing).
- [ ] **Step 3: Move per mapping (no logic edits).**
- [ ] **Step 4:** wire; run native suite + build → green; the tests from Step 1 pass unchanged.
- [ ] **Step 5: Commit** `refactor(bridge): DriveController (drive, STOP, hold, release, camera, reaping, OTA) with today's rules pinned (LLD R15.4)`.

---

### Task 12: CommandRouter, the root, the watchdog

**Files:** Create `src/app/command_router.h/.cpp`, `test/test_command_router/test_main.cpp`; rewrite `src/net/link_task.cpp` (root + loop, ≤ 250 lines, no handler); modify `src/main.cpp` (watchdog for the loop task, OTA unsubscribe), `scripts/size_allow.txt` (remove every `firmware/esp32/` line).

**Interfaces:**
```cpp
class CommandRouter {
 public:
  CommandRouter(DriveController& d, MotionController& m, SettingsController& s, TankAppController& t,
                TelemetryPublisher& tel, Replies& out, core::WsClients& ws);
  void onText(int fd, const char* text, size_t len, uint32_t post_seq, uint32_t now);  // parse, STOP fast path, admit, table
};
// table: struct Route { core::WsCmdKind kind; void (*fn)(CommandRouter&, int fd, int32_t id, const core::WsCommand&, uint32_t now); };
```
- [ ] **Step 1: Failing tests** — `test_every_kind_has_a_route_or_is_invalid` (loop over every `WsCmdKind` value: routed or `nack INVALID_COMMAND`), `test_stop_bypasses_admission_for_unknown_clients`, `test_ping_answers_pong`, **`test_ota_answers_every_waiter`** (Review Focus 5: queued setting op + stop waiter + cam_home waiter → `startOta` → each answered once, STOP frame queued, master stopped, `clientClose` called for every client).
- [ ] **Step 2: Implement router; rewrite `link_task.cpp`:** static objects in dependency order (`g_uart, g_tx, g_link, g_ws, g_log, g_out, g_tel, g_uno, g_motion, g_settings, g_tank, g_drive, g_router, g_events`), the hook / sink wiring (captureless lambdas), the loop exactly as today's order: UART → link bytes; queue → `kWsOpen/kWsClose/kWsText/kOtaStart`; `g_drive.tick`; `g_link.step`; events → `g_events.route`; `g_motion.onTelemetry/tick` + `g_tel.tick`; `g_settings.tick`; `g_uno.tick`; `drainTx`; status every 100 ms.
- [ ] **Step 3: Watchdog** (`link_task.cpp` and `main.cpp`, ESP-IDF 5 / Arduino 3):
```cpp
#include <esp_task_wdt.h>
// main.cpp setup(), before linkTaskStart():
const esp_task_wdt_config_t wdt{.timeout_ms = 3000, .idle_core_mask = 0, .trigger_panic = true};
esp_task_wdt_reconfigure(&wdt);   // R15.8: 3 s
esp_task_wdt_add(nullptr);        // the Arduino loop task
// main.cpp loop(): esp_task_wdt_reset(); at the top of each loop
// OTA start (ota_service onStart): esp_task_wdt_delete(nullptr);  // the upload blocks loop() for ~30 s
// link task, before its for (;;): esp_task_wdt_add(nullptr); each tick: esp_task_wdt_reset();
```
- [ ] **Step 4: Run** native suite, `pio run -e bridge`, `python3 scripts/size_check.py` with **no `firmware/esp32/` line** left in `scripts/size_allow.txt` → all pass; `wc -l src/net/link_task.cpp` ≤ 250.
- [ ] **Step 5: Commit** `refactor(bridge): CommandRouter and link_task as composition root + loop; task watchdog 3 s (LLD R15.4, R15.8)`.

---

### Task 12a: Remaining bridge size offenders (added at Task 1, ledger ruling)
The Task 1 baseline found 11 bridge functions outside `link_task` / `ws_codec` / `ws_json` over the limits: `ArcController::step` (64 NLOC, CCN 22), `BodyLink::step` (CCN 12), `BodyLink::onFrame` (67 / 21), `FrameParser::step` (60 / 18), `MotionLink::onEvent` (CCN 11), `MotionService::stepRate` (CCN 17), `MoveController::step` (42 / 12), `WifiPolicy::step` (CCN 12), `net::cameraBegin` (55), `net::wsHandler` (CCN 11), `net::serveInfo` (71), `WifiManager::handle` (76 / 28), `WifiManager::step` (63 / 23).

**Files:** those sources only. **Method:** extract-function only (no logic change): each `switch` case / phase becomes a private helper in the same class or file; names say the phase (`onFrameHello`, `stepArcRamp`, …).
- [ ] **Step 1:** for each `core/` function: run its existing host tests first (`test_arc`, `test_body_link`, `test_frame_parser`, `test_motion`, `test_move`, `test_wifi_policy`) → green; split; rerun → green.
- [ ] **Step 2:** for each `net/` function (no host test): split; `pio run -e bridge` SUCCESS; behaviour is checked on the tank in Task 13 (WiFi connect / switch / fallback, `/info`, camera start).
- [ ] **Step 3:** remove their lines from `scripts/size_allow.txt`; `python3 scripts/size_check.py` passes with no `firmware/esp32/` line.
- [ ] **Step 4: Commit** `refactor(bridge): split the remaining long functions (extract-function only) - size check clean for the bridge`.

### Task 13: On the tank, docs, review

**Files:** `docs/lld/esp32-bridge-lld.md` (R15 "as built": file list, line counts, any deviation), `docs/design-notes.md` (§99 results), `CLAUDE.md` (Plan P in the plan list, `app/` + `ports/` in "Where things are", the R15 rule), `docs/standards/coding-standards.md` (only if a rule changed).

- [ ] **Step 1: OTA** the bridge (`scripts/ota.sh`, password from `secrets.h` into the env, never printed); `/info` shows the new commit.
- [ ] **Step 2: Stand checks** (ask "stand or floor?" first; say what moves; STOP within reach): drive stick + RC; STOP; STOP from a second phone (hold until released) and its Log row; camera stick, HOME, LOCK; sounds while a camera HOME waits (both answered, no BUSY); tank-button confirmation of a setting; WiFi double press with and without a phone (jingle + blink); Settings tabs (live sensors, self-test, camera, Log paging); `/info` after 10 min (no watchdog restart: boot count unchanged).
- [ ] **Step 3: Floor check** (owner present): `tools/smoke/move.py 100` (forward 1 m at 40 %) and `tools/smoke/turn.py 90` — results within today's tolerances (±1 cm, ±2°).
- [ ] **Step 4: Watchdog check** (stand, nothing moves): a debug build flag `CQ_WDT_TEST` that blocks the loop for 4 s once after 60 s → the bridge restarts, `/info` boot reason "task watchdog"; then rebuild without the flag.
- [ ] **Step 5: Docs** as listed; commit `docs: Plan P results, bridge LLD R15 as built, design-notes §99`.
- [ ] **Step 6: Final review** — whole-branch review package (`review-package`), most capable model, with the whole-file numbers (`python3 scripts/size_check.py --baseline` output must be empty for the bridge); fix Critical / Important test-first; ask the owner before merging `plan-p-bridge-structure` (after `plan-o`).
