# Implementation Roadmap

Status: IN DISCUSSION · Date: 2026-10-11

Principle: start minimal, get the tank driving straight lines and circles with real measurements first; build the complex parts (hub backend, missions, AI) later on top, without throwaway work.

## Phases
| Phase | What gets built | Link to the tank | You can test |
|---|---|---|---|
| 0 Protocol foundation | `protocol/` definitions + generator (C for Uno, C++ for ESP32, Python; TypeScript later) + error registry | — | — |
| 1 Uno bring-up | skeleton with all drivers, flash/RAM measured, self-test, safety core | **USB**, tank on a stand | each sensor/motor; memory fit |
| 2 Uno motion core | drive + heading hold, turn (spin/arc), move-distance, deadband/ramps, encoders, obstacle brake, deadman | USB on a stand | motors, encoders, gyro, first PID tuning |
| 3 Bench tool + ESP32 WiFi bridge | Python bench tool; minimal ESP32 bridge firmware | **WiFi via the bridge (home network)** | free driving: straight line, circle, square, calibration, tuning |
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

## Out of the MVP (phases 0–5)
Hub backend, Timeline/recording, missions/waypoints UI, multi-tank, AI engine, voice, themes beyond Phosphor, surface profiles (one default), OTA, full key management (admin key only), first-run polish.

## Open
- (none for phases 0–3)
