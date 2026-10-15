# Implementation Roadmap

Status: phases 0–3 largely DONE (see "Where we are"), 2026-10-15 · first written 2026-10-11

## Where we are (2026-10-15)
| Item | State |
|---|---|
| Plan A protocol generator | done |
| Plan B Uno firmware (full) | done; kept building for the Mega (tag `full-firmware-2026-10-14`) |
| Plan C ESP32 bridge | done; hardware checks passed (design-notes §51) |
| Plan D bench tool | not started |
| Plan E Uno Lite (ELEGOO Uno is the body board, §43) | done; straight line 0–2 cm / 3 m (§52) |
| Plan F Cockpit-lite phone page on the bridge (WebSocket `/ws`, video and `/capture` / `/source.tar.gz` on port 81) | done |
| Plan G stick steering by angle | done; floor tuning parked until encoders |
| Next | ESP32 motion service — TURN / DRIVE_RATE on the bridge for the Uno Lite (spec 02 Part E); encoders wired via P7 + breadboard (§53); Uno R4 Minima discussed, not decided (§54) |
The phase table below is the original plan (2026-10-11); the bridge grew beyond the TCP passthrough into the phone page (Plan F), and the "Cockpit lite" of phase 5 means the React hub app.

Principle: start minimal, get the tank driving straight lines and circles with real measurements first; build the complex parts (hub backend, missions, AI) later on top, without throwaway work.

## Phases
| Phase | What gets built | Link to the tank | You can test |
|---|---|---|---|
| 0 Protocol foundation | `protocol/` definitions + generator (C for Uno, C++ for ESP32, Python; TypeScript later) + error registry | — | — |
| 1 Uno bring-up | skeleton with all drivers, flash/RAM measured, self-test, safety core, **shield probe** (verifies TB6612 standby on D3 and direction pins with tracks off the ground) | **USB**, tank on a stand | each sensor/motor; memory fit |
| 2 Uno motion core | drive + heading hold, turn (spin/arc), move-distance, deadband/ramps, encoders, obstacle brake, deadman | USB on a stand | motors, encoders, gyro, first PID tuning |
| 3 Bench tool + ESP32 WiFi bridge | Python bench tool; minimal ESP32 bridge firmware + **Basic Drive phone page** (design-notes §28) | **WiFi via the bridge (home network)** | free driving: straight line, circle, square, calibration, tuning |
| 4 Full ESP32 gateway | gateway per ESP32 LLD (WS API, auth, camera, arbitration, STOP fast path) | WiFi (WebSocket API) | replaces the bridge |
| 5 Cockpit lite (web) | first slice of the hub web app (React + MobX), connecting directly to the tank | WiFi, direct | phone/browser driving + test panel |
| 6+ Full system | ESP32 calibration (port of Python reference maths), hub backend, Timeline, missions, AI engine, full security | — | complete design |

## ESP32 WiFi bridge (phase 3, DECIDED)
- Minimal firmware: transparent **TCP (port 3333) ⇄ UART (250000 baud)** passthrough to the Uno, like the original ELEGOO relay; optionally ELEGOO's camera stream.
- **Station mode on the home network by default** (user requirement: laptop stays on the normal network + internet). Credentials in `secrets.h` (git-ignored).
- Discovery: mDNS `conqueror-bridge.local`; IP printed on ESP32 USB serial at boot; `bench discover` network scan.
- Fallback: home WiFi unreachable 30 s → own WPA2 hotspot `conqueror-bridge`.
- Caveat: guest/mesh networks with client isolation block device-to-device traffic.
- Safety: heartbeats come from the laptop over WiFi; any stall > 300 ms → Uno link-loss stop (turn/move cancelled, no resume); IR remote always overrides; bench STOP resends until acked.
- After phase 4: kept only as a **developer build option** (compiled in with a debug flag, never in release builds) — no authentication.

## Bench tool (phase 3)
Purpose: developer/test tool on the laptop playing the ESP32's role on the link (USB or bridge; later the gateway's WS API via the same transport interface).

Features: connect + handshake (heartbeat 100 ms, HELLO → ENVELOPE → HELLO_ACK) · live dashboard · manual driving · command console · YAML test runner with tape-measure prompts, metrics and pass/fail reports · run comparison · calibration (Python reference calculators) · live PID tuning with step-response plots · session recording + replay · plots incl. odometry XY path.

Structure (`tools/bench/`, Python 3.12, uv): `transport/` (serial, tcp, later websocket) · `link/` (generated codec, heartbeat, handshake, acks) · `session/` · `services/` (teleop, test runner, calibration, tuning, recorder) · `analysis/` (metrics, odometry, reference calculators) · `ui/` · `cli.py`. Same standards as the AI engine.

Standard tests (initial pass criteria):
| Test | How | Pass |
|---|---|---|
| Straight line | move 200 cm @ 40 % | drift < 5 cm, heading error < 2°, distance error < 3 % |
| Circle | arc 360° radius 50 cm | closes within 10 cm, radius error < 10 % |
| Square | 4 × (100 cm + turn 90°) | closes within 15 cm |
| Spin | turn 360° in place | heading error < 2° |
| Speed hold | 30 % tile vs carpet | track speed ±5 % |
| Safety | deadman, obstacle brake, lift, tilt | per safety checklist |

### Bench UI (DECIDED: option A, no gamepad)
- **Textual terminal dashboard** (telemetry, console, test runner, reports).
- **Default driving = cruise control** (no key-up events needed): W/S speed ±10 % per tap · A/D steering steps (C centres) · Q/E spin · **Space = STOP** (resent until acked) · arrows camera nudge · H camera home · 1–9 quick tests. Current speed/steering shown large; deadman still protects on laptop/WiFi loss.
- **Optional hold-to-drive** (`bench drive --hold`): `pynput` key-down/up events on macOS (one-time Accessibility permission).
- Gamepad support pluggable for later; IR remote remains a manual controller.

## Body board (DECIDED 2026-10-11)
- **Now: Arduino Uno** (as designed). Firmware stays board-agnostic (ports/drivers + `BOARD_*` build option; domain code untouched by board changes).
- **Later: Arduino Mega 2560** (owned). Port notes (superseded by design-notes §29–30: no jumpers, software I2C on A4/A5): I2C on pins 20/21 would have needed 2 jumpers to A4/A5 (shield wires the MPU6050 to A4/A5 only); encoders direct to A8–A11 (PCINT2, full quadrature); IR on D9 via timer sampling (no PCINT on D9); optional 2-wire move of the ESP32 link to Serial1 (USB + ESP32 simultaneously, no S1 switching); check physical fit (Mega is ~33 mm longer).
- **Sensors (DECIDED 2026-10-11):** IR remote receiver stays (soldered on the shield). Line-tracking module stays removed (needs too little ground clearance; unreliable). **Uno: keep the forward ultrasonic** (obstacle brake). **Mega (to decide then):** candidate swap to ToF — forward VL53L1X (obstacle brake) + downward VL53L0X at the front edge (cliff/stair detection, new safety rule "no floor ahead → stop, reverse allowed"), both I2C with XSHUT on the freed D12/D13. Firmware: generalise the `ultrasonic` port to `range_forward` (+ future `range_cliff`); encoders on the Mega use full quadrature (A8–A11).
- **Other upgrade candidate:** Arduino **Uno R4 Minima** (RA4M1 Cortex-M4 48 MHz, 32 KB RAM / 256 KB flash, **5 V logic**, same footprint) — new ARM driver set; verify interrupt-capable pins.
- **Avoid with this 5 V shield:** 3.3 V-only boards (Due, Zero/M0, Metro M4, ESP32 Uno-shape boards).

## Tank Logo (DECIDED 2026-10-11)
Logo-like language to program the tank (children, owner, AI). Details: `docs/design-notes.md` §20; spec `docs/superpowers/specs/2026-10-11-05-tank-logo-design.md`.
| When | What |
|---|---|
| Now | language spec only (Plans B–D unchanged) |
| After Plan D | Python `conqueror_logo` package + `bench logo run` (client-side execution) |
| Phase 4 | ESP32 program runner + program API (core programs run standalone) |
| Phase 5 | built-in IDE in Cockpit-lite (editor, preview, line highlight) |
| Phase 6 | program library, natural language → Logo, explanations, vision words (AI engine runs AI programs) |

## Out of the MVP (phases 0–5)
Hub backend, Timeline/recording, missions/waypoints UI, multi-tank, AI engine, voice, themes beyond Phosphor, surface profiles (one default), Uno OTA (ESP32 OTA is in Plan C, design-notes §28), full key management (admin key only), first-run polish.

## Open
- (none for phases 0–3)
