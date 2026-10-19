# Conqueror Tank — project guide for Claude Code

Hobby robot platform: ELEGOO Conqueror tank rebuilt as **Uno body + ESP32 gateway (all web pages) + Python AI engine**, with vision as the main input. Single tank, no separate hub (design-notes §79; spec 03 parked). Repo: `github.com/mannuray/conqueror`.

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
| Low-level designs | `docs/lld/` — `uno-firmware-lld.md` (§9 Revision 2 + appended decision notes are authoritative), `esp32-firmware-lld.md` (§10 Revision 2), `protocol-generator-lld.md`, `bench-tool-lld.md`, `esp32-bridge-lld.md` (R3–R3.11: phone page, stream, review fixes; R4–R4.17: motion service, WiFi / camera, floor-test fixes; R5 debug stream; R6 stall banner; R13 tank app; R14 one master always on; **R15 controllers and ports**; R16 Tank Logo on the bridge), **`uno-lite-lld.md`** (Uno Lite firmware, spec 02 Part D), **`tank-logo-lld.md`** (Tank Logo v1: reader, runner, saved programs, `/logo` page — draft), `base-board-lld.md` (parked) |
| Implementation plans | `docs/superpowers/plans/` — `2026-10-11-plan-{a,b,c,d}-*.md`, `2026-10-14-plan-e-uno-lite.md`, `2026-10-15-plan-f-cockpit-lite.md`, `2026-10-15-plan-g-stick-mixing.md`, `2026-10-15-plan-h-motion-service.md`, `2026-10-17-plan-i-encoder-phase1.md`, `2026-10-17-plan-j-straight-tuning.md`, `2026-10-17-plan-k-move-by-distance.md`, `2026-10-17-plan-l-arcs-pivots.md`, `2026-10-18-plan-m-wifi-settings.md`, `2026-10-19-plan-n1-logo-reader.md`, `2026-10-19-plan-n2-logo-runner.md`, `2026-10-19-plan-n3-logo-on-tank.md`, `2026-10-19-plan-o-tank-app.md`, `2026-10-19-plan-p-bridge-structure.md` |
| Roadmap & phase decisions | `docs/implementation-roadmap.md` |
| Coding standards | `docs/standards/coding-standards.md` |
| Hub UX + Stitch screens | `docs/design/hub-ux.md`, `docs/design/stitch/` |
| Developer machine setup (macOS / Linux) | `docs/dev-setup.md` |
| Hardware datasheets (encoder motor FIT0458, design-notes §56) | `docs/hardware/datasheets/` |

## Plan execution order (roadmap phases 0–3)
1. **Plan A — protocol generator** (no hardware). Must be merged before B/C/D.
2. **Plan B — Uno firmware** (needs the tank on a stand for hardware steps; host tests run anywhere).
3. **Plan C — ESP32 WiFi bridge** (needs the ESP32 board).
4. **Plan D — bench tool** — **retired 2026-10-18**, never executed (design-notes §74); TCP 3333 passthrough removed from the bridge.
5. **Plan E — Uno Lite** (the ELEGOO Uno is the body board again, design-notes §43) — done; 3 m floor run passed (§52).
6. **Plan F — Cockpit-lite phone page** on the bridge — done (hardware checks §44–51).
7. **Plan G — stick steering by angle** — done on the stand; floor feel parked until encoders (§52).
8. **Plan H — bridge motion service** (TURN / DRIVE_RATE on the ESP32 for the Uno Lite, spec 02 Part E, bridge LLD R4) — done and merged 2026-10-17 (floor results design-notes §58; final review fixes R4.17).
9. **Debug metrics over WiFi** (`td` / `tds`, bridge LLD R5, `tools/smoke/td_record.py`) — done.
10. **Plan I — encoder features phase 1** (Uno Lite: track sync, stall by speed 30 %, runaway guard, stop-both latch, plain mix for AUTO; spec 02 Part F, Uno Lite LLD §19) — done, merged 2026-10-17 (floor results design-notes §63–§64).
11. **Plan J — straight-driving tuning** (heading-hold integral + `track_balance` 19 ‰, spec 02 §F5, Uno Lite LLD §20) — done, merged 2026-10-17: ~1 cm over 2.5 m (design-notes §65).
12. **Phone page stall / fault banner + "encoder off" note** (bridge LLD R6, design-notes §67) — done, merged 2026-10-17; every banner is one slim centred line.
13. **Plan K — MOVE by distance + counts_per_m calibration + learned early stop** (bridge only; spec 02 Part G incl. §G6, bridge LLD R7) — done, merged 2026-10-17, floor-accepted: ±1 cm (design-notes §69); `counts_per_m` 7514 in the bridge NVS; `tools/smoke/move.py`.
14. **Plan L — arcs, pivots, minimum drive_rate** (bridge only; spec 02 Part H, bridge LLD R8; Uno kMaxCps 2500) — done, merged 2026-10-18 (design-notes §71): pivots / refusals / slow-spin clamp pass; **arc path (±5 cm) re-test after the left road wheel repair**; `tools/smoke/turn.py`.
15. **Plan M — WiFi modes (double press) + `/settings` page** (bridge only; spec 02 Part I, bridge LLD R9) — done on the tank and merged 2026-10-18 (design-notes §73); ≤ 3 networks in NVS, button-confirmed changes, two tabs. A STOP from any device now holds until the driver releases its stick (§72, LLD R9.6).
16. **Plan O — the tank app** (☰ menu, four-tab `/settings`, Tune graphs + distance wizard, Log, camera settings; bridge LLD R13–R14) — merged 2026-10-19 (owner: before the tank checks), stand-checked (design-notes §93–§97); floor wizard check pending.
17. **Plan P — bridge structure refactor** (controllers + ports, size check, watchdog; bridge LLD R15) — merged 2026-10-19 (§98–§99; owner: merged before its tank checks); tank checks after the Logo work and the road-wheel fix.
18. **Plan N1 — Tank Logo reader** (lexer, iterative parser, checker, `logogen` generator; Tank Logo LLD) — done 2026-10-19: all 67 v1 conformance cases pass (design-notes §88). **Plan N2 — Tank Logo runner** (compiler, Vm, effects, RunController, simulator, running.yaml; Tank Logo LLD §19) — done: all 27 run tests pass (design-notes §101). **Plan N3 — Tank Logo on the tank** (worker with async `/logo` jobs, HTTP + WebSocket, saved programs on the new `logo` partition, LogoController, odometry, `tools/smoke/logo.py`; Tank Logo LLD §20, bridge LLD R16) — stand-checked 2026-10-19 (design-notes §105); floor checks (turns, square, obstacles, IR takeover) after the road-wheel fix. **Next:** Tank Logo Plan N4 (the `/logo` page); then the road-wheel fix, motor power, the pending tank checks of O, P and N3, the arc re-test. Uno Lite flash is at 96 % (~1.2 KB left).
Execute with the superpowers skills (`subagent-driven-development` recommended, or `executing-plans`).

## Hard rules for code
- **Generated files are never edited by hand** (`firmware/uno/src/generated/`, `firmware/esp32/src/generated/`, `protocol/python/conqueror_protocol/generated.py`, `protocol/vectors/`, `protocol/tests/cpp/`, `docs/protocol/reference.md`, `firmware/esp32/src/logo/generated/`, `firmware/esp32/web/generated/`, `firmware/esp32/test/test_logo_conformance/cases.h`, `firmware/esp32/test/test_logo_running/cases.h`). Change `protocol/defs/*.yaml`, run `cd protocol && uv run protogen write`; change `logo/spec/*`, run `cd logo && uv run logogen write`; the pre-commit hook runs `protogen check` (enable it on every fresh clone with `git config core.hooksPath .githooks` — see `docs/dev-setup.md` §1b).
- Uno: no heap, no `virtual`, no `delay()`, ISRs capture-only, domain code never includes `ports/` or `Arduino.h`; only `SafetySupervisor` can produce motor output (`PwmOutput`).
- Python: 3.12 via uv, type hints everywhere, ruff + pyright strict clean, asyncio without blocking the event loop.
- The obstacle brake, deadman, link-loss stop and child-play envelope are safety features: never weaken them to make a test pass. The only sanctioned relaxation is the designed **Override mode** (`docs/design-notes.md` §26: human-only, never in child play, auto-expires; deadman/link-loss/battery-critical/stall/lift/STOP always stay on).

## Hardware facts (confirmed)
- Body board: **ELEGOO Uno R3 (Lite profile)** again from 2026-10-14 (design-notes §43: the Mega clone cannot start the ESP32-S3); the **Mega 2560** Full profile keeps building but waits for our own base board + ELEGOO **SmartCar Shield V1.1** with **TB6612FNG**: PWMA D5 (right), PWMB D6 (left), AIN1 D7, BIN1 D8, **STBY D3**. DRV8835 supported as a build option.
- **No hardware rework (design-notes §30):** the shield plugs onto the Mega as-is — no jumpers, soldering or rewiring, ever. Mega pin differences are handled in firmware (software I2C on A4/A5; 20 kHz timer sampling for IR on D9). Encoder cables (independent wires) plug directly into the Mega: left A/B → A8/A9, right A/B → A10/A11.
- FIT0458 TT encoder motors: encoders **Mega: left A8/A9, right A10/A11** (direct cables); Uno: left A1, right A2 via the shield's **P7 LineTrack** port, power shared through a mini breadboard (design-notes §53; never from the spare motor connectors); passive buzzer **A0** (shield port, both boards); ultrasonic trig D13 / echo D12 (forward); IR receiver D9 (soldered on shield); servos D10 pan / D11 tilt (camera) — shield servo **port 2 = pan, port 1 = tilt** (bench-confirmed); WS2812 ×1 on D4; mode button D2; battery A3; MPU6050 I2C A4/A5 (0x68); 2S Li-ion battery.
- **Camera on again (2026-10-18)** with its clock pin GPIO15 at the **weakest drive** (`board::kCamXclkDrive = 0`, bridge LLD R12, design-notes §76): at the default drive the XCLK edges drowned the WiFi (§57); now 0 % ping loss while streaming. XCLK 10 MHz. **Motors reversed in firmware** (`kDir*ForwardHigh = false`, §60). **Protocol version 3** — flash Uno and bridge together.
- ESP32 camera board: **ELEGOO ESP32S3-Camera V1.0** with **ESP32-S3-WROOM-1** module, USB-C (confirmed by photo 2026-10-12; earlier docs said ESP32-WROVER — superseded). Camera reported as **OV3660**. 8 MB flash + 8 MB octal PSRAM (N8R8). ESP32 ↔ Uno UART: **RX GPIO3 / TX GPIO40** (recovered from the old firmware), our protocol runs it at **250000 baud**; camera pins = **ESP32-S3-EYE map** (see `docs/design-notes.md` §24). The older WROVER pins (IO33 / IO4) do not apply. Shield switch **S1** disconnects the ESP32 for Uno USB uploads.
- **Flash layout (since Plan N3, one USB flash 2026-10-19):** app0 / app1 2.875 MB, source 0x5D0000, `logo` LittleFS 256 KB at 0x7B0000 (bridge LLD R2.2 / R16).
- Dropped / not used: compass, line-tracking module. Uno firmware updates are **USB only**.

## Safety when touching hardware
- Motor and calibration tests: tank **on a stand, tracks off the ground**, unless a step says floor.
- Keep a STOP within reach (bench Space key or IR remote OK).
- The ESP32 WiFi bridge has **no authentication** — home network, development only, never in release firmware.
