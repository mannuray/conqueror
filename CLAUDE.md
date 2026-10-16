# Conqueror Tank — project guide for Claude Code

Hobby robot platform: ELEGOO Conqueror tank rebuilt as **Uno body + ESP32 gateway + optional hub + Python AI engine**, with vision as the main input. Repo: `github.com/mannuray/conqueror`.

## How we work (owner's rules — follow them)
- **Design before code.** Order: specs → UX → LLD per component → implementation plan → code. Never start code for a component whose plan is not approved.
- **Document every agreed decision in the repo in the same turn**, then commit and push. Raw history: `docs/design-notes.md` (superseded by the specs where they differ).
- **LLDs must include classes and Mermaid diagrams** (class diagrams always; sequence/state diagrams where behaviour matters).
- Discuss options with the owner before deciding; give a recommendation; keep explanations plain.
- Commits: Conventional Commits (`feat(uno): …`, `fix(bench): …`, `docs: …`), one logical change each. When several machines work in parallel: **one branch per plan**, merge Plan A first.

## Where things are
| What | Path |
|---|---|
| System specs (authoritative) | `docs/superpowers/specs/` — `2026-10-10-0{0..4}-*-design.md` (system, protocol, tank firmware incl. Part D Uno Lite and Part E Uno/ESP32 feature split, hub, AI engine), `2026-10-11-05-tank-logo-design.md`, `2026-10-14-06-base-board-design.md` (parked) |
| Low-level designs | `docs/lld/` — `uno-firmware-lld.md` (§9 Revision 2 + appended decision notes are authoritative), `esp32-firmware-lld.md` (§10 Revision 2), `protocol-generator-lld.md`, `bench-tool-lld.md`, `esp32-bridge-lld.md` (R3–R3.11: phone page, stream, review fixes), **`uno-lite-lld.md`** (Uno Lite firmware, spec 02 Part D), `base-board-lld.md` (parked) |
| Implementation plans | `docs/superpowers/plans/` — `2026-10-11-plan-{a,b,c,d}-*.md`, `2026-10-14-plan-e-uno-lite.md`, `2026-10-15-plan-f-cockpit-lite.md`, `2026-10-15-plan-g-stick-mixing.md`, `2026-10-15-plan-h-motion-service.md` |
| Roadmap & phase decisions | `docs/implementation-roadmap.md` |
| Coding standards | `docs/standards/coding-standards.md` |
| Hub UX + Stitch screens | `docs/design/hub-ux.md`, `docs/design/stitch/` |
| Developer machine setup (macOS / Linux) | `docs/dev-setup.md` |
| Hardware datasheets (encoder motor FIT0458, design-notes §56) | `docs/hardware/datasheets/` |

## Plan execution order (roadmap phases 0–3)
1. **Plan A — protocol generator** (no hardware). Must be merged before B/C/D.
2. **Plan B — Uno firmware** (needs the tank on a stand for hardware steps; host tests run anywhere).
3. **Plan C — ESP32 WiFi bridge** (needs the ESP32 board).
4. **Plan D — bench tool** (simulated Uno for nearly all tests; hardware only for acceptance) — not started.
5. **Plan E — Uno Lite** (the ELEGOO Uno is the body board again, design-notes §43) — done; 3 m floor run passed (§52).
6. **Plan F — Cockpit-lite phone page** on the bridge — done (hardware checks §44–51).
7. **Plan G — stick steering by angle** — done on the stand; floor feel parked until encoders (§52).
8. **Plan H — bridge motion service** (TURN / DRIVE_RATE on the ESP32 for the Uno Lite, spec 02 Part E, bridge LLD R4) — planned, awaiting approval.
Execute with the superpowers skills (`subagent-driven-development` recommended, or `executing-plans`).

## Hard rules for code
- **Generated files are never edited by hand** (`firmware/uno/src/generated/`, `firmware/esp32/src/generated/`, `protocol/python/conqueror_protocol/generated.py`, `protocol/vectors/`, `protocol/tests/cpp/`, `docs/protocol/reference.md`). Change `protocol/defs/*.yaml`, run `cd protocol && uv run protogen write`; the pre-commit hook runs `protogen check` (enable it on every fresh clone with `git config core.hooksPath .githooks` — see `docs/dev-setup.md` §1b).
- Uno: no heap, no `virtual`, no `delay()`, ISRs capture-only, domain code never includes `ports/` or `Arduino.h`; only `SafetySupervisor` can produce motor output (`PwmOutput`).
- Python: 3.12 via uv, type hints everywhere, ruff + pyright strict clean, asyncio without blocking the event loop.
- The obstacle brake, deadman, link-loss stop and child-play envelope are safety features: never weaken them to make a test pass. The only sanctioned relaxation is the designed **Override mode** (`docs/design-notes.md` §26: human-only, never in child play, auto-expires; deadman/link-loss/battery-critical/stall/lift/STOP always stay on).

## Hardware facts (confirmed)
- Body board: **ELEGOO Uno R3 (Lite profile)** again from 2026-10-14 (design-notes §43: the Mega clone cannot start the ESP32-S3); the **Mega 2560** Full profile keeps building but waits for our own base board + ELEGOO **SmartCar Shield V1.1** with **TB6612FNG**: PWMA D5 (right), PWMB D6 (left), AIN1 D7, BIN1 D8, **STBY D3**. DRV8835 supported as a build option.
- **No hardware rework (design-notes §30):** the shield plugs onto the Mega as-is — no jumpers, soldering or rewiring, ever. Mega pin differences are handled in firmware (software I2C on A4/A5; 20 kHz timer sampling for IR on D9). Encoder cables (independent wires) plug directly into the Mega: left A/B → A8/A9, right A/B → A10/A11.
- FIT0458 TT encoder motors: encoders **Mega: left A8/A9, right A10/A11** (direct cables); Uno: left A1, right A2 via the shield's **P7 LineTrack** port, power shared through a mini breadboard (design-notes §53; never from the spare motor connectors); passive buzzer **A0** (shield port, both boards); ultrasonic trig D13 / echo D12 (forward); IR receiver D9 (soldered on shield); servos D10 pan / D11 tilt (camera) — shield servo **port 2 = pan, port 1 = tilt** (bench-confirmed); WS2812 ×1 on D4; mode button D2; battery A3; MPU6050 I2C A4/A5 (0x68); 2S Li-ion battery.
- ESP32 camera board: **ELEGOO ESP32S3-Camera V1.0** with **ESP32-S3-WROOM-1** module, USB-C (confirmed by photo 2026-10-12; earlier docs said ESP32-WROVER — superseded). Camera reported as **OV3660**. 8 MB flash + 8 MB octal PSRAM (N8R8). ESP32 ↔ Uno UART: **RX GPIO3 / TX GPIO40** (recovered from the old firmware), our protocol runs it at **250000 baud**; camera pins = **ESP32-S3-EYE map** (see `docs/design-notes.md` §24). The older WROVER pins (IO33 / IO4) do not apply. Shield switch **S1** disconnects the ESP32 for Uno USB uploads.
- Dropped / not used: compass, line-tracking module. Uno firmware updates are **USB only**.

## Safety when touching hardware
- Motor and calibration tests: tank **on a stand, tracks off the ground**, unless a step says floor.
- Keep a STOP within reach (bench Space key or IR remote OK).
- The ESP32 WiFi bridge has **no authentication** — home network, development only, never in release firmware.
