# Conqueror Platform — System Overview (Design Spec 0)

Status: REVIEWED (open items closed 2026-10-10) · Date: 2026-10-10 · Source: brainstorming session, `docs/design-notes.md`

> **2026-10-18 (owner, design-notes §79): single tank, no separate hub.** All web pages (drive, settings, help, Tank Logo editor) and the Logo runner live on the ESP32 bridge; the only off-tank piece is the AI engine (Python, laptop / PC), which connects directly to the bridge. Spec 3 (hub) is parked; where this overview mentions the hub, read "the bridge" for pages and "the AI engine" for AI work.

Spec set:
| # | Spec | Scope |
|---|---|---|
| 0 | **System overview** (this file) | goals, architecture, roles, safety principles, hardware, modes |
| 1 | `2026-10-10-01-protocol-design.md` | client API (JSON/WS/HTTP) + Uno↔ESP32 binary link |
| 2 | `2026-10-10-02-tank-firmware-design.md` | Arduino Uno body + ESP32 gateway firmware |
| 3 | `2026-10-10-03-hub-design.md` | Node/TypeScript hub server + React UI |
| 4 | `2026-10-10-04-ai-engine-design.md` | Python AI engine |

---

## 1. Goal

Turn the ELEGOO Conqueror tank (original firmware `../ELEGOO-Conqueror-Robot-Tank-Kit/ConquerorCar_Driver_20210104`)
into a hobby robot platform that exploits current AI, with **vision as the main input**. Everything else on the tank
(motion, pan/tilt, LED, buzzer) is an output the AI and humans control.

Non-goals: cloud/internet remote access, multi-site deployment, multi-user accounts, SaaS, the mobile app (future, separate project).

## 1.1 What it is for: a fun way to learn real robotics (owner, 2026-10-24, design-notes §153)

The platform is a **fun learning tool for robotics, for children and hobbyists**. It keeps the fun and pushes as far
toward real robotics as hobby-grade sensors allow. It is not a mission-critical robot: it only has to play well.
These principles guide every later phase:

1. **Fun first, real robotics inside.** Every feature teaches a real idea (odometry, sensor fusion, maps,
   perception, planning), but the child meets it as play.
2. **Aim for accuracy, accept roughness.** Measure as well as the sensors allow, then work with loose tolerances
   ("about 1 m, a bit right"). The robot says how sure it is. Floor checks use play tolerances, not lab ones.
3. **General before special.** A home with a child is chaotic: hand-made rules for each case break faster than
   they can be written. Prefer approaches that cope with a messy room (depth, learning from data, a smart look now
   and then) over per-case rules. A rule is a last resort.
4. **Safety is never traded** (§3 below). A child is around. The safety layer stays on the body board and is
   never relaxed for a feature.
5. **The language is the child's door.** Tank Logo grows with the robot, from `FORWARD 50` to words that use
   perception (`LOOK FOR BALL`, `GO TO CHAIR`, `FOLLOW ME`). Lessons climb from moving, to sensing, to deciding.
   Play behaviours that make good lessons: chase or nudge the ball, follow a person, explore without bumping, go
   to a named thing - more to come.
6. **The robot type can change, the words stay.** Common words (look, find, go to, wait, repeat) plus words
   particular to each robot, so another robot (an arm: `REACH`, `GRAB`) can later use the same language.
7. **Everything can be seen and replayed.** The child and the adult see what the robot sees and thinks (the live
   view, recordings), so mistakes become lessons.

**What exists already (searched 2026-10-24; design-notes §153).**
- **Kid-level tools with camera AI do exist:**
  - block coding with object-detection or tracking blocks: PictoBlox with Quarky, the HuskyLens camera with
    Mind+ (Scratch-based), the Hiwonder DaDablock (ESP32);
  - they react to what is in the current picture (an object, where, how big).
- **Open, cheap robots with real perception exist for older students:**
  - OpenBot (a phone on a ~$50 body, person following, MIT licence, with a drag-and-drop Playground);
  - ROS2 kits with depth cameras and mapping, such as Hiwonder MentorPi (teens and adults).
- **Cozmo / Vector** recognised faces and objects and had a kid-friendly app. It was closed; today only a
  community server and SDKs remain.
- **What we did not find:** one open framework on cheap hardware where a child's simple language uses a
  remembered map of named things ("go to the ball" after it has left the view), with a safety layer that is
  always on, and words that carry over to another robot type. That combination is this project's aim. It is a
  gap we did not find filled, not a proven first.

**A cheap tank with a borrowed brain (owner, 2026-10-24).**
- **Where the cost goes elsewhere:**
  - the smart robots above put the brain on board: a phone, a Raspberry Pi 5, a Jetson;
  - the cheap camera modules (HuskyLens on an Arduino, the ESP32 DaDablock) only answer "what is in the picture
    now": no map, no memory of things out of view, no idea where the robot is.
- **Ours splits the work:**
  - **the tank stays cheap** (the ELEGOO kit: Uno + ESP32 camera board). On its own it drives, runs Tank Logo,
    serves its pages and keeps every safety rule, with no computer at all.
  - **the heavy thinking runs on a home laptop or PC**: vision, the map, the pose filter, later the smart look. A
    home computer is standard now, so it costs nothing extra.
- With a computer, the tank also sees and remembers.
- The aim, stated precisely: **a cheap tank that does real perception and mapping by borrowing a home computer,
  with a child's language on top.**

## 2. Architecture

```
                 ┌─────────────── LAN / tank hotspot ───────────────┐
 Phone app ──────┤                                                   │
 (future)        │   ┌──────────── Tank ────────────┐                │
                 ├──►│ ESP32 gateway (camera, WiFi, │◄── UART ──► Arduino Uno body
 Browser ──► Hub ┤   │  API, navigation)            │               (motors, IMU, encoders,
 (Node/TS + React)   └──────────────────────────────┘                ultrasonic, servos,
        ▲        │                                                    LED, buzzer, IR, battery)
        │        └───────────────────────────────────────────────────┘
 Python AI engine (client of the hub)
```

| Tier | Runs on | Role |
|---|---|---|
| **Body** | Arduino Uno | Executes commands, runs control loops and **safety reflexes**, reports telemetry. No intelligence. |
| **Gateway** | ESP32-WROVER camera board | WiFi (AP + station), camera stream, client API, key auth, network arbitration, **navigation** (pose, go-to, waypoints, return home). |
| **Hub** (optional) | Any machine (Docker, multi-arch) | Rich web control, multi-tank, history, recordings, map, missions, holds tank keys. |
| **AI engine** (optional) | Any machine (auto-detected tier) | Perception, world model, skills, LLM planner. Client of the hub. |

### Operating modes
- **Standalone:** app ↔ tank directly (tank hotspot or home WiFi). Everything in Body + Gateway works, including navigation.
- **With hub:** adds history, recordings, map, mission editor, multi-tank, AI engine.
- **IR remote:** works with no WiFi at all (Uno only).

## 3. Safety principles (apply to every spec)

1. **The tank never depends on the hub or the AI.** Their failure never changes app/IR control behaviour.
2. **Driving never depends on the AI being healthy.** The LLM never drives motors directly; it picks skills; skills send commands; the tank's reflexes apply last.
3. **The Uno safety supervisor is the final gate** before the motors: deadman, obstacle brake, tilt, lift, bump, stall, battery, envelope limits.
4. **STOP is accepted from any source at any time.**
5. **The tank stops on every control-owner change** and never resumes stale commands.
6. **Honest enforcement boundaries:** the boards enforce speed/accel caps, the obstacle brake and contact/lift stops.
   Vision-dependent rules (e.g. "never drive toward the child") are **AI-level only** — the boards cannot see.

## 4. Control priority

**IR remote > human (app/web, `human`/`admin` key) > AI (`ai` key).**
- A higher source takes control on its first motion command.
- Handback: IR → others after 3 s idle or IR **OK**; human → AI after 5 s idle or an explicit "release to AI".
- The AI receives a `control_returned` event and must send fresh commands.
- Viewing (video, telemetry) is never blocked by control ownership (subject to key role).

## 5. Hardware baseline

| Item | Value | Status |
|---|---|---|
| MCU | Arduino Uno (ATmega328P, 32 KB flash, 2 KB RAM) | confirmed |
| Gateway | ELEGOO ESP32-WROVER-Camera **V1.0** (confirmed; no IO13 LED) | confirmed |
| Camera | **OV3660** (3 MP) on 24-pin FPC | confirmed by user |
| Uno shield / motor driver | **ELEGOO SmartCar Shield V1.1** (confirmed); driver **TB6612FNG (confirmed)**; DRV8835 kept as alternative build option | build option selects shield (Spec 2 hardware revision) |
| Motors | stock TT gear motors → **DFRobot FIT0458** TT encoder motors (6 V, 160 RPM, 120:1) | ordered |
| IMU | MPU6050 (gyro + accel, no magnetometer) | confirmed |
| Distance | ultrasonic, **fixed forward** (no pan/tilt) | confirmed |
| Pan/tilt | 2 servos (D10 pan, D11 tilt) carrying the **camera** | confirmed |
| LED | **1×** WS2812 RGB (D4) | confirmed |
| Buzzer | **passive** piezo in the old line-sensor port (A0–A2) | type confirmed |
| IR | NEC receiver (D9), kit remote "A" | confirmed |
| Compass | **none — dropped (out of scope, 2026-10-11)** | decided |

### Hardware facts and remaining assumptions
- **Battery: 2S Li-ion (7.4 V nominal, 8.4 V full) — CONFIRMED by user.** Battery thresholds 7.0 V low / 6.6 V critical; 6 V motor cap.
- Remaining assumptions below are confirmed during wiring/calibration (buzzer pin, encoder resolution via calibration item 5, encoder pin, UART speed with 57600 fallback):
- **A2 Buzzer pin is A0.** Configurable at build time; confirm when wiring.
- **A3 FIT0458 resolution is 960 counts per output revolution per channel** (DFRobot wiki; some retailers state 1920). Stored as a setting; verified by hand-turning one revolution.
- **A4 Encoder pins: left A1, right A2** (D3 is TB6612 standby on the V1.1 shield); buzzer A0.
- **A5 UART at 250000 baud works over the stock shield's link** (0 % error on 16 MHz; revised from 115200) between Uno and ESP32 (the original ran at 9600). Fallback: 57600 with reduced fast-telemetry rate.

## 6. Hardware configurations supported

| # | Configuration | Status |
|---|---|---|
| 1 | Gyro only, stock motors | supported (no distance/navigation) |
| 2 | **Gyro + encoders** | **target** |
| 3/4 | + compass | **dropped** |

Firmware runs on both 1 and 2. Encoders are enabled by a setting; if enabled but silent while driven → fault + fallback to open-loop.

Capabilities by configuration:
| Capability | 1 Gyro | 2 Gyro + encoders |
|---|---|---|
| Deadband compensation, ramps, battery compensation | ✅ | ✅ |
| Precise turns, heading hold | ✅ | ✅ |
| Tilt / bump detection | ✅ | ✅ |
| Lift detection | ≈ pickup transient + tilt only | ✅ + track free-spin |
| True per-track speed PID, smooth crawl | ≈ open-loop | ✅ |
| Drive a distance | ❌ (not offered) | ✅ |
| Stall / slip detection | ❌ | ✅ |
| Pose (x, y), go-to-point, return home, waypoints | ❌ (API reports unavailable) | ✅ short/medium missions (heading drifts over minutes) |

## 7. Out of scope / deferred
- Compass (magnetometer or 9-axis IMU) — **dropped** (more nuisance than help; gyro + encoders suffice; camera can correct long-run drift). §8 kept as historical record only.
- Mobile app (separate future project; uses the same client API).
- Uno over-the-air updates (USB only).
- Formal test strategy / simulator (deferred by user; a real-tank safety checklist is kept — spec 2 §9).
- Cloud, internet access, multi-user accounts, TLS (LAN plain HTTP/WS accepted for home use).
- VLA robot-action models (arm-oriented; revisit later).

## 8. Compass (DROPPED 2026-10-11 — historical record only)
- **F1:** GY-271 "HMC5883L" board (usually a QMC5883L clone; equivalent for this use) on the shared I2C bus; auto-detect 0x0D/0x1E; mount ≥10 cm from motors.
- **F2:** one 9-axis board replacing the MPU6050 role (ICM-20948 best; GY-87 easiest; BNO055 on-chip fusion). Use address 0x69 if the onboard MPU6050 stays.
- A compass can never replace the MPU6050 (turn PID and lift/tilt safety need the gyro/accelerometer).
- Fusion rules if added: gyro primary; compass slow drift correction; reject when inconsistent with gyro, at high motor current, or abnormal field magnitude; self-calibration by a slow 360° spin.

## 9. Mounting (DECIDED 2026-10-11)
- Uno + SmartCar Shield V1.1 stays in the **stock baseplate position** (all switches/sockets accessible).
- Single mount → **no mount build option**: IMU axis mapping is a constant in `drivers/board_config.h`, measured once at bring-up; the bench tool's mount check (gravity axis + hand-rotate yaw sign) verifies it on every connection.
