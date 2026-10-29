# NewConquerorTank — Design Notes (living document)

> **Superseded by the specs in `docs/superpowers/specs/2026-10-10-0*-design.md`.** This file is the raw brainstorming record;
> where it conflicts with the specs (e.g. old "v1/v2" wording, compass auto-detect in §6, the single `conqueror.local` name), **the specs win**.

Running record of every decision and open question while we brainstorm.
Nothing here is implemented yet. Once complete, this becomes the design spec.

Last updated: 2026-10-10

---

## 0. Process status
- Rule (user): **complete ALL design (tank firmware + hub server) before any code.**
- Tank firmware design sections: §20 Uno architecture APPROVED. §25 ESP32 architecture APPROVED. §26 safety/calibration/self-test APPROVED. Testing deferred (§29). Still to present: repo layout; protocol message catalogue goes in the spec.
- AI engine design: §28 architecture APPROVED.
- Hub server design: §24 architecture APPROVED.

## 1. Goal

Rewrite the ELEGOO Conqueror tank (original firmware: `../ELEGOO-Conqueror-Robot-Tank-Kit/ConquerorCar_Driver_20210104`)
into a layered robot platform where intelligence lives off-board.

## 2. Architecture (DECIDED)

Three tiers:

| Tier | Hardware | Role |
|---|---|---|
| Body | Arduino Uno | Executes commands and reports telemetry. Contains only control loops and safety reflexes, no "intelligence". |
| Gateway | ESP32-WROVER camera board | Communication board: WiFi, camera stream, relays commands to the Uno and telemetry back. Controls the Uno. |
| Brain | Separate server on the WiFi network (AI engine), web page, mobile app | Sends high-level commands, consumes video and telemetry. Out of scope for now; we only design the interface it uses. |

### Uno responsibilities
- Move the tank (closed-loop where possible; see §5).
- Aim the camera with the pan/tilt servos (the ESP32 camera is mounted on them).
- RGB LED: colours and blink patterns.
- Buzzer: different sounds and tones.
- Report telemetry: battery voltage/level, pan/tilt angles, ultrasonic distance, IMU (heading, tilt), motor state, faults.
- **Local safety reflexes (DECIDED):**
  - Stop if commands stop arriving (deadman timeout, e.g. on WiFi loss).
  - Stop if the tank is lifted or tipped (IMU-based; the line sensor used for this is gone).
  - Refuse to drive on critically low battery.
- **Local manual override (DECIDED):** the IR remote drives the tank directly through the Uno, even with no WiFi or ESP32.

### ESP32 responsibilities
- **Two network modes (DECIDED):**
  - Direct: the ESP32 runs its own access point and a phone or laptop connects straight to the tank.
  - Network: the ESP32 joins existing WiFi (station mode) and any server, app or web page on the LAN can reach it.
- Serve the camera stream.
- Expose one control and telemetry API used by all clients (AI server, web UI, mobile app).
- Bridge to the Uno over UART.

### Control arbitration (DECIDED: priority levels)
- Priority, highest first: **IR remote > human (web/mobile app) > AI server**.
- A higher-priority source takes control immediately when it sends a motion command.
- Handback timing (DECIDED, adjustable settings):
  - IR → app/AI: 3 s after the last IR press, or immediately on IR **OK**.
  - Human → AI: 5 s after the last human input, or immediately via an app "release to AI" button.
  - On regaining control the AI gets a "control returned" event and must send a fresh command; the tank never resumes stale commands.
  - The tank **stops on every control change** (no surprise motion).
- **STOP / e-stop is accepted from any source at any time.**
- All clients can always view video and telemetry.

## 3. Protocol (DECIDED: new protocol)
- Drop the ELEGOO JSON `{"N":..}` protocol. The stock ELEGOO app will no longer work.
- The ESP32 firmware must be rewritten (it is currently an AP-only transparent relay with a `{Heartbeat}` keepalive).
- Uno to ESP32 link format: to be defined.
- Client to ESP32 API (e.g. WebSocket for control and telemetry, HTTP MJPEG for the camera): to be defined.

## 4. Hardware inventory and pin map

Pin assignments come from the original firmware and still need to be confirmed on the actual board.

| Function | Uno pin | Notes |
|---|---|---|
| Serial to ESP32 | D0/D1 | Shared with USB |
| Mode button | D2 | INT0 |
| (free) | D3 | INT1 — encoder candidate |
| RGB LED (WS2812) | D4 | 1 LED in firmware (needs verifying) |
| Motor A PWM (right) | D5 | Timer0 |
| Motor B PWM (left) | D6 | Timer0 |
| Motor B dir | D7 | DRV8835 |
| Motor A dir | D8 | DRV8835 |
| IR receiver | D9 | |
| Servo pan (z) | D10 | Moves the camera |
| Servo tilt (y) | D11 | Moves the camera |
| Ultrasonic echo | D12 | Fixed facing forward (no pan/tilt) |
| Ultrasonic trig | D13 | |
| Old line-sensor port | A0, A1, A2 | Line sensor removed; **buzzer now plugged into this port** |
| Battery voltage | A3 | Divider; original scale = ADC * 0.0375 * 1.08 |
| I2C (MPU6050) | A4/A5 | Additional I2C devices need no new pins |

Sensors: MPU6050 (3-axis gyro + 3-axis accelerometer; **no magnetometer**), HC-SR04-style ultrasonic.
Motor driver: DRV8835.
Motors: stock **TT-format** gear motors **without encoders**; upgrade to DFRobot FIT0458 TT encoder motors (see §6d).

## 5. Motor control (DECIDED direction, details open)

No "dumb PWM". Layers, bottom to top:

1. **Deadband / minimum PWM compensation.** Below a threshold the motor only hums (e.g. PWM 10 does nothing).
   - Per motor, per direction: a *start* threshold (static friction) and a lower *keep-moving* threshold.
   - Map every non-zero command onto `[threshold .. 255]`.
   - A short "kick" pulse to break static friction when starting.
2. **Auto-calibration.** Ramp PWM until the gyro detects motion; store thresholds in EEPROM; re-run on demand (e.g. after a surface change).
3. **Slew-rate limiting / acceleration ramps.** Avoid jerk, gearbox shock and current spikes that brown-out and reset the Uno.
4. **Battery-voltage compensation.** Scale PWM by V_nominal / V_battery so a given % speed stays consistent as the battery drains.
5. **Closed-loop PID on what we can measure (gyro):**
   - Turn-rate control (deg/s).
   - Heading hold while driving straight.
   - Turn to an exact angle (e.g. 90°).
6. **Command interface:** `speed (-100..100 %)` + `turn rate (deg/s)`, not raw PWM. Raw PWM only as a debug/calibration command.

Linear speed is **open-loop** with stock motors: there is no way to measure track speed, so per-track speed PID is impossible.

## 6. Heading sensing and future encoder upgrade

The user wants both: use heading sensing ("compass") now, and keep an option for encoder motors later.

- The motor layer must be designed so encoders can be added later **without changing the command interface**:
  - Without encoders, `speed %` is open-loop plus compensation.
  - With encoders, `speed %` becomes a true per-track speed PID, and distance commands are added.
- Constraint from the user: adding sensors is physically hard, and the old line-sensor port is now used by the buzzer.

### Heading sensing options
| Option | Gives | Pins | Caveat |
|---|---|---|---|
| MPU6050 gyro (fitted) | Precise turns, heading hold, tilt/lift detection | 0 | Heading drifts slowly over minutes |
| + magnetometer (QMC5883L/HMC5883L), optional | Absolute heading, corrects gyro drift | 0 (shared I2C) | Must be mounted away from motors and battery |

Recommendation: the gyro now; the magnetometer is auto-detected at boot if fitted later.

### Encoder upgrade (future)
- Free pins with the buzzer fitted: **2 line-port pins (A0–A2 minus the buzzer pin) + D3 = 3 pins**.
- **2 pins = one channel per motor: viable.** Direction is inferred from the commanded direction; counts are trusted only while the motor is driven.
- 4 pins = full quadrature: requires freeing D2 (move mode switching to IR/app). Optional.
- Alternative: an I2C encoder-counter co-processor (0 Uno pins).
- Motor options: encoder gear motors (e.g. JGA25-370 Hall), encoder disc + optical sensor on the stock motors, or the I2C counter board. The choice depends on the stock motor size (still unknown).

### Sensor fusion roles (target end state)
- **Encoders:** per-track speed PID, distance travelled (straight lines).
- **Gyro:** heading/turn source of truth, because skid-steer tracks slip heavily when turning.
- **Magnetometer (optional):** slow drift correction of the gyro heading.
- **Slip detection:** encoders say moving but the IMU says not moving → raise a "stalled/slipping" fault.
- The firmware has a "speed source" abstraction: open-loop + compensation now, encoder PID later, with the same command interface.

### Capability by hardware configuration (DECIDED: firmware supports all four)
The MPU6050 gyro is always fitted (stock), so every configuration includes it.
The firmware runs in whichever configuration it detects or is set to (see 6a).

| Capability | 1. Gyro only (stock) | 2. Gyro + encoders | 3. Gyro + compass | 4. Gyro + encoders + compass |
|---|---|---|---|---|
| Drive with deadband compensation and smooth ramps | ✅ | ✅ | ✅ | ✅ |
| Precise turns (90°), heading hold | ✅ | ✅ | ✅ | ✅ |
| Lift, tilt and bump detection | ✅ | ✅ | ✅ | ✅ |
| Constant track speed on any surface, load or battery | ≈ estimate (battery-compensated) | ✅ true speed PID | ≈ estimate | ✅ true speed PID |
| Smooth slow crawl | ≈ | ✅ | ≈ | ✅ |
| "Drive 50 cm" | ❌ time-based guess | ✅ | ❌ time-based guess | ✅ |
| Stall / track-slip detection | ❌ | ✅ | ❌ | ✅ |
| Heading stays correct over long runs | ❌ drifts (minutes) | ❌ drifts (minutes) | ✅ | ✅ |
| "Face north / go east" (absolute heading) | ❌ | ❌ | ✅ | ✅ |
| Position tracking (x, y) relative to start | ❌ | ≈ good over short runs; degrades as gyro heading drifts | ❌ | ✅ |
| Go to a point, return home, follow waypoints | ❌ | ≈ short missions | ❌ | ✅ |
| Rough obstacle map (ultrasonic + position) for the AI server | ❌ | ≈ short missions | ❌ | ✅ |

Notes:
- Config 2 (gyro + encoders) is real odometry: encoder distance + gyro heading. Accuracy is limited by gyro heading drift (minutes) and track slip.
- Config 4 adds the compass to bound heading drift, so position stays usable over longer missions.
- The stock motors are TT format, so the FIT0458 (also TT) is a drop-in fit.

Limits: odometry drifts with track slip (cm to tens of cm over metres; the AI server can correct it with the camera).
The magnetometer is unreliable indoors near steel or motors; mount it high and use it only for slow drift correction.

## 6.0 TARGET HARDWARE (DECIDED, supersedes compass plans below)
- **Final hardware: MPU6050 (already fitted) + 2x FIT0458 encoder motors.** No separate compass, no 9-axis IMU.
- Reason: mounting and wiring a compass (standoff, I2C address pins) is too much hassle for the user.
- Firmware configurations supported in v1:
  - Config 1: gyro only (stock motors).
  - Config 2: gyro + encoders (user's target).
- Compass support (configs 3/4, 9-axis IMUs such as ICM-20948/GY-87/BNO055) is **deferred and not in v1**.
  The interface (heading source abstraction) keeps the door open; §6c/6e are kept for reference only.

## 6.1 FUTURE OPTION: compass + gyro (recorded for later consideration)
Not in v1. Revisit if absolute heading or long-mission position accuracy is needed.
- **Option F1: separate compass + existing MPU6050.** GY-271 / "HMC5883L" board (almost always a QMC5883L clone; HMC vs QMC is effectively a tie; QMC: 16-bit, 200 Hz).
  Same I2C bus (A4/A5), 0 extra Uno pins; auto-detect 0x0D (QMC) / 0x1E (HMC). Must be mounted high (≥10 cm from motors/battery).
- **Option F2: one 9-axis board replacing the MPU6050** (gyro + accel + compass together, mount the whole board high):
  - ICM-20948 (Adafruit, 5 V-safe): best raw quality, current production.
  - GY-87 (MPU6050 + HMC/QMC5883L): easiest, since it reuses the MPU6050 driver.
  - BNO055: on-chip fusion, outputs a drift-corrected heading; tiny driver.
  - Avoid GY-9250/MPU-9250 (discontinued, fakes without a magnetometer). BNO085 is the best sensor but too heavy for the Uno.
  - If the onboard MPU6050 is soldered to the shield, the new board must use I2C address 0x69 (AD0 high).
- Compass can never replace the MPU6050 (no gyro means no turn PID; no accelerometer means no lift/tilt/bump safety and no tilt compensation).
- Gains: long-run heading accuracy, absolute heading, long-mission odometry (configs 3/4 in the capability table).
- Interference handling, self-calibration and fusion rules: see §6c.

## 6a. Hardware configurability (DECIDED)
The user has ordered an encoder motor and a compass. The firmware must still work on a stock tank (no upgrade required).

| Hardware | Detection | Fallback if absent |
|---|---|---|
| Compass | DEFERRED (not in v1) | Gyro-only heading |
| Encoders | EEPROM setting, changeable from the web UI (floating pins cannot be auto-detected) | Open-loop speed + compensation |
| Encoder sanity check | Enabled, but no pulses while the motor is driven → fault + automatic fallback to open-loop | — |

## 6d. Encoder motor: DFRobot FIT0458 (ORDERED)
"TT Geared Motor with Encoder (6V 160RPM 120:1 L Shape)" — https://wiki.dfrobot.com/fit0458/
- Rated 6 V, 160 RPM no-load @6V, 120:1 gearbox, TT form factor (L-shape, double-flat ~5.5 mm shaft).
- Quadrature Hall encoder (A/B), 4.5–7.5 V supply → powered from the Uno 5 V.
- Resolution: DFRobot wiki says 8 PPR motor shaft → **960 pulses per output rev per channel**; some retailers say 16 PPR / 1920.
  → `counts_per_rev` is an EEPROM setting (default 960), verified by turning the shaft one revolution by hand while watching the live count.
- **Voltage cap:** battery is 2S Li-ion 7.4–8.4 V → limit effective drive voltage to 6 V (max duty = 6 / Vbat, ~71% on a full battery). Applies to the stock motors too (configurable max voltage).
- Wiring (single channel per motor, direction from command): left A → D3 (INT1), right A → a free line-port pin (PCINT1). Channel B unconnected (optional later via freed D2).
- Interrupt load: ~2560 pulses/s per motor at full speed, single edge → fine on the Uno.
- Speed estimation: pulse counting per 20 ms cycle at high speed; pulse-period timing at low speed; auto-switch.
- Distance calibration routine: drive N counts, the user enters the measured cm → counts_per_cm stored in EEPROM (absorbs sprocket and track size).
- Fit: the stock motors are TT format, so the FIT0458 is a drop-in replacement (confirmed by the user).

## 6e. Compass module selection
| Module | Chip | ~Price (INR) | Notes | Verdict |
|---|---|---|---|---|
| GY-271 | QMC5883L (I2C 0x0D) | 150–300 | 5 V-tolerant board, 0 extra pins, tiny driver; often mislabelled HMC5883L | **Recommended** |
| GY-273 / genuine | HMC5883L (I2C 0x1E) | 300+ | Discontinued; mostly clones | Supported via auto-detect |
| BNO055 | 9-axis with on-chip fusion | 2000–3500 | Outputs a drift-corrected absolute heading; self-calibrating; offloads the Uno | Premium option (would need its own driver) |
| LIS3MDL / MMC5603 | High-grade magnetometers | 1000+ | Usually 3.3 V boards, scarce in India | Not needed |

Wiring: same I2C bus as the MPU6050 (A4 SDA, A5 SCL, 5V, GND). Mount on a standoff ≥10 cm from the motors and battery.

## 6c. Compass interference strategy (DECIDED)
- Why the MPU6050 is fine near the motors but the compass is not: the gyro and accelerometer measure rotation and gravity (unaffected by motor fields); the magnetometer measures the magnetic field (affected).
- Interference: hard-iron (constant offset, calibratable), soft-iron (distortion, calibratable), dynamic (motor current, NOT calibratable; only distance helps).
- Mounting: high, at least ~10 cm from the motors and battery, on a fixed standoff, **not on the pan/tilt**.
- Self-calibration: the tank spins 360° slowly by itself and fits hard/soft-iron offsets, stored in EEPROM, triggered from the web UI.
- Fusion: the gyro is primary; the compass is a slow drift correction only.
  - Reject compass readings when the compass heading changes but the gyro says no rotation.
  - Lower compass trust at high motor current; higher when stationary.
  - Field-magnitude check: abnormal field → flag "compass unreliable" in telemetry and ignore it.

## 6b. Buzzer (DECIDED: passive)
- Plays tones and melodies on one of the old line-port pins.
- Implementation constraint: Arduino `tone()` and the IRremote library both use Timer2 (duplicate ISR, link error).
  Plan: replace IRremote with a small custom pin-change NEC decoder so `tone()` can own Timer2.
- Sounds must be non-blocking (sequencer), never `delay()`.

## 8. Sounds and LED (DECIDED: catalogue accepted, 1 LED)
Hardware: **1 RGB LED** (WS2812 on D4). The original firmware wrote leds[1], an out-of-bounds bug; it must not be repeated.
Both outputs use **priority layers**: a higher priority interrupts a lower one, and the lower one resumes after. Everything is non-blocking.

### Sounds (passive buzzer)
| Pri | Event | Sound | Repeat |
|---|---|---|---|
| 1 | Tilt danger / tipping | fast two-tone siren | while tilted |
| 1 | Lifted off the ground | rising whoop | once + every 3 s while lifted |
| 1 | Battery critical (motors disabled) | three long low tones | every 10 s |
| 2 | Battery low | double beep | every 30 s |
| 2 | Collision / bump | short falling "ouch" | once |
| 2 | Stall / track slip | low buzz | once per event |
| 2 | Command link lost (deadman) | falling three-note | once |
| 3 | Reversing | truck beep… beep… | while reversing |
| 3 | Moving forward | short chirp on start | once per start |
| 3 | Obstacle ahead (optional, toggle) | parking-sensor beeps, faster when closer | while closing |
| 4 | Startup | rising boot melody | once |
| 4 | Ready (self-test OK) | two rising chirps | once |
| 4 | Self-test failed | low error tone | once |
| 4 | Client connected / disconnected | up-chirp / down-chirp | once |
| 4 | Control owner changed | two-note blip | once |
| 4 | Calibration start / done | tick / success jingle | once |
| 4 | Custom | tone sequence from server/app | on request |

Settings: **Mute** (only priority-1 alarms) and **Quiet** (alarms + warnings). No volume control (passive buzzer).

### LED (single RGB)
| Pri | Event | Pattern |
|---|---|---|
| 1 | Tilt danger / lifted | fast red/blue alternate |
| 1 | Battery critical | red slow pulse |
| 2 | Battery low | amber blink every 2 s over the base colour |
| 2 | Collision / stall | short white flash |
| 2 | Link lost | orange blink |
| 3 | Reversing | white/red blink |
| 3 | Turning | amber blink (single LED, so no left/right) |
| 4 | Startup | rainbow sweep |
| 4 | Idle, no client | blue breathing |
| 4 | Controlled by human (app/web) | solid green |
| 4 | Controlled by AI server | solid cyan |
| 4 | Controlled by IR remote | solid purple |
| 4 | Calibrating | yellow pulse |
| 4 | Custom | colour/blink/pulse from server/app |

Implementation note: only call FastLED.show() when the colour actually changes (it briefly disables interrupts, which can jitter the servos and encoder timing).

## 9. Pan/tilt camera servos (DECIDED)
Servos: pan D10, tilt D11; the ESP32 camera is mounted on them.

Motion:
- Always smooth: trapezoidal profile (accelerate, cruise, decelerate) with configurable max speed and acceleration.
- Updated at 50 Hz using writeMicroseconds (sub-degree steps), never instant jumps.
- Soft limits per axis (the original tilt limit was 30°–110°) and home trim offsets, both stored in EEPROM.

Control modes:
- **Joystick mode (human app/web):** absolute mapping. Stick position = camera angle within the limits.
  Holds while the stick is held; on release, after a configurable delay (~0.5 s), returns smoothly to home.
  Optional **lock** toggle: keep the current view and skip return-to-home; unlock → return home.
  (NOT mouse/rate mode.)
- **AI server / API mode:** separate commands. Absolute angle targets that **hold** (no auto-return),
  plus other control styles as the AI needs (e.g. relative nudges, speed-limited moves). Defined in the protocol section.
- `home` command available to any client.
- Telemetry: current pan/tilt angles and a moving flag.

Power:
- Servos stay powered while moving/holding (no droop).
- Optional detach at home when idle (stops buzz, saves power), only if the tilt holds without power (test on hardware).

## 10. Control inputs (DECIDED)

### Phone app / web (primary controller) — app is a separate future project
- Touch-based, analog; no hard buttons for motion.
- Left virtual joystick: drive. Y = speed, X = turn, combined (smooth arcs). Release → smooth stop.
- Right virtual joystick: camera pan/tilt (absolute, springs home on release; see §9).
- **Optional phone-tilt steering (app-side only):** the app converts phone tilt into the same speed/turn values.
  The tank boards are unaware and need nothing extra.
- Touch toggles for settings (camera lock, mute, quiet, obstacle beeps, etc.).

### Requirements on the tank side
- Continuous drive input: speed −100..100 %, turn −100..100 %, streamed ~20 Hz while the stick is touched.
- **Deadman:** no drive update for ~300 ms → smooth stop (thumb lifted, WiFi drop, app crash).
- All input passes through smoothing, deadband compensation, gyro/encoder PID; jittery input still gives smooth motion.

### IR remote (backup, hard buttons, highest priority, works without WiFi)
| Button | Action |
|---|---|
| ▲ / ▼ | forward / backward while held (stop ~200 ms after release) |
| ◀ / ▶ | spin left / right while held |
| OK | STOP + release control back to app/AI |
| 1 / 3 | camera pan left / right while held |
| 2 / 8 | camera tilt up / down while held |
| 5 | camera home |
| 4 / 6 | speed −/+ 10 % (beep pitch confirms) |
| 7 | mute toggle |
| 9 | battery check (LED colour + 3/2/1 beeps) |
| 0 | gyro turn 180° |
| * | obstacle beeps on/off |
| # (hold 3 s) | start motor calibration |
- IR driving also uses gyro heading hold; the ultrasonic sensor stops it before obstacles.
- Only remote "A" (NEC codes from the original firmware) is supported; remote "B" used IRremote hash codes and is dropped unless needed.

## 11. Obstacle braking (DECIDED: option B)
- Uno-side safety reflex using the forward ultrasonic sensor, applies to **all** controllers (IR, app, AI).
- Forward speed is limited progressively as the obstacle gets closer; full stop at ~15 cm (configurable).
- Reverse and turning remain allowed so the tank can escape.
- Telemetry flag `blocked`; parking-sensor beeps if enabled.
- **Can be disabled by a client (app or AI)** for intentional contact (pushing objects, docking).
  Re-enabled automatically at reboot (never persisted as off).
- Ultrasonic limits: fixed forward-facing, narrow cone; it misses low/thin objects and angled surfaces → best-effort, not a guarantee.

## 12. Telemetry (DECIDED: all items)
Every message carries a **sequence number + timestamp**.

### Fast stream (~20 Hz, motion)
- Heading (gyro yaw), turn rate, pitch/roll
- Left/right track speed: commanded vs actual (actual = encoders, if fitted)
- **Actual motor PWM output** (after deadband, battery cap, PID)
- Distance ahead (ultrasonic) + **time-to-collision** (distance ÷ closing speed)
- Pan/tilt angles + moving flag
- Encoder counts L/R
- Pose x, y and distance from home are computed and published by the **ESP32** (§17)

### Slow stream (~1 Hz, health/status)
- Battery voltage, %, **estimated runtime remaining**, **voltage sag under load**
- Control owner: app / AI / IR / none
- Flags: blocked, lifted, tilted, stalled, low battery, critical battery, link lost, obstacle brake off, muted, quiet
- Hardware config: encoders on/off, gyro OK
- **Temperature** (MPU6050 internal sensor)
- **Surface roughness** (accelerometer vibration variance)
- **Track slip %** (encoders vs gyro; encoders required)
- Uptime, loop health (load / max loop time), **free RAM**
- Error counters: bad commands, I2C errors; last reset reason
- **Firmware version + settings checksum**
- **Last command ID acknowledged** (also sent as an immediate ack/nack per command)

### Events (immediate)
bump (**with g-force**), lifted, tilted, stall, control changed, link lost/restored, calibration done,
battery low/critical, obstacle brake engaged, self-test result.

### Lifetime stats (EEPROM, saved every few minutes to limit wear)
Total distance, total run time, power-on count.

### ESP32-side telemetry (added by the ESP32, not the Uno)
WiFi RSSI, camera FPS, connected clients, ESP32 temperature, ESP32↔Uno link latency.

## 14. ESP32 WiFi setup (DECIDED: option A)
- First boot / no saved network → own AP `Conqueror-XXXX` + captive setup page (also at 192.168.4.1).
  The page scans networks; the user picks one and enters the password; saved in ESP32 flash (NVS).
- Up to 3 saved networks. At boot: try them for ~15 s → join (station mode); otherwise fall back to the own AP (direct mode).
- mDNS name `conqueror-XXXX.local` (unique per tank, see §23) for app, web and hub discovery.
- The setup page stays reachable later (add/remove networks, force direct mode).
- Credential reset: hold the Uno mode button (D2, otherwise unused now) for ~5 s at boot → Uno tells the ESP32 to clear saved networks.
  (Alternative if the ESP32 board has a usable button: TBD on hardware.)

## 15. Client API (DECIDED)
| Channel | Purpose |
|---|---|
| WebSocket `ws://conqueror-XXXX.local/ws` | Live control in (joystick, commands), telemetry + events out. JSON messages. |
| HTTP MJPEG `http://conqueror-XXXX.local:81/stream` + `/capture` snapshot | Camera video for app/web/AI. |
| HTTP REST `/api/...` | Settings, calibration, WiFi setup, keys, firmware info. |
- JSON is used only between clients and the ESP32. **The ESP32↔Uno link is compact** (the Uno never parses JSON).

### Security (DECIDED: option C, per-client keys with roles)
- Each client authenticates with its own key; **the key carries the role**, so priority is enforced by the key, not by a client claim.
- Roles: `admin` (settings, calibration, WiFi, key management, can also drive as human), `human` (drive, camera, LED/sound),
  `ai` (drive at AI priority, camera, LED/sound), `viewer` (video + telemetry only).
- Key management from the setup page (admin): create, name, revoke keys.
- First boot: the AP is open **only until setup completes**; setup forces creating the admin password and a WPA2 password for the tank's own AP.
- Limitation: plain HTTP/WS on the LAN, so keys can be sniffed by someone on the same network. Acceptable for home use; TLS on the ESP32 is a future option.
- Video stream access: requires a key (viewer or above).

## 17. Scope and navigation split (DECIDED)
- **Single release: all functions work**, including position tracking, drive-distance, go-to-point, return-home, waypoints.
  (Implementation is still built and tested in incremental steps.)
- Split by capability:
  - **Uno:** motion primitives (drive distance at heading, turn to heading/by angle, velocity drive), speed PID, heading PID,
    safety (obstacle brake applies to every primitive), encoder counts + yaw in fast telemetry.
  - **ESP32:** odometry/pose (x, y, θ) integration from Uno telemetry; go-to-point, return-home, waypoint missions
    (computes heading and distance → sends primitives); exposes pose and missions in the client API.
- Link loss mid-mission: the Uno completes or safely stops the current primitive; it never continues blindly.
- Requires encoders (config 2). With stock motors (config 1), navigation is time-based/unavailable and the API reports it.

## 19. Web control: standalone hub server (DECIDED: option B)
- **No rich UI on the ESP32.** The ESP32 keeps only a minimal captive page: WiFi setup, passwords, OTA upload.
- A **standalone hub server** on the LAN is the rich web control:
  - Holds **one** connection to the tank (1 video stream + 1 WebSocket) and fans out to any number of browsers.
  - Holds the tank keys; browsers log in to the hub with user accounts (browsers never see tank keys).
    The hub maps user roles to tank roles (e.g. hub user → `human` key, hub admin → `admin` key).
  - Features: live video + touch joysticks (incl. phone tilt), telemetry dashboard, **telemetry history + graphs**,
    **video recording + snapshots**, **pose map**, **mission/waypoint editor**, logs, **calibration wizards**,
    settings, key management, WiFi/OTA links.
  - The AI engine may run inside the hub or connect to it, sharing the video feed.
- **Direct mode still works without the hub:** the ESP32 API stays directly usable (future mobile app on the tank hotspot).
- ESP32 load: at most a couple of direct clients (hub, maybe AI/mobile app); MJPEG to >2 viewers is avoided by design.
- The hub is a **separate project** (own spec), after the tank firmware.
- **Host (DECIDED): runs anywhere** → packaged with Docker (multi-arch: amd64 + arm64), so Mac/Linux/Windows/Raspberry Pi all work; also runnable without Docker for development.

## 22. Hub + AI stack (DECIDED: hybrid)
```
Browsers / App ──► Node hub (TypeScript) ──► Tank (ESP32)
                         ▲
                         │ hub client API (AI role) + video frames
                   Python AI engine (main product offering)
```
- **Hub:** Node.js + TypeScript (real-time WebSocket fan-out, auth, UI serving, recording, history).
- **Web UI:** React + TypeScript (served by the hub).
- **AI engine:** Python, a **separate service and client of the hub API** with the `ai` role; gets video via the hub's re-stream.
- Rationale: isolation (AI load/crash never stalls driving or telemetry), AI independently versioned/swappable,
  can run on a separate GPU machine, best ecosystem per job.
- Message formats defined once (JSON Schema / OpenAPI) → generated TypeScript + Python types.
- Deployment: Docker Compose (hub + AI engine), multi-arch.
- Rejected: all-Python hub — its only gain (single deployment) is neutralised by Docker Compose; AI load would share the control process.
- Extended safety principle: **driving never depends on the AI (or hub) being healthy.**

## 23. Hub scale (DECIDED: multiple tanks, one site, hobby)
- Hobby project exploiting AI developments. User first chose one tank, then switched to **B: many tanks on one local site** since it is little extra work (~10–15% hub effort, near-zero tank effort) if designed in from day one.
- Tank identity: unique ID from the ESP32 MAC; mDNS name `conqueror-XXXX.local` (renameable); advertised as mDNS service `_conqueror._tcp`.
  (Supersedes the single `conqueror.local` name in §14/§15.)
- Hub: discovers all tanks, add/forget tanks, one connection per tank; **all data keyed by tank ID** (telemetry, recordings, missions, keys, settings).
- UI: tank picker + fleet overview (battery/status for all tanks).
- AI engine: addresses tanks by ID; may control one or several (e.g. cooperative exploring, follow-the-leader later).
- Still out of scope (YAGNI): cloud / internet remote access / multi-site / multi-user accounts.
- Users: one admin login (+ optional view-only guest).

## 24. Hub architecture (APPROVED)
Node/TypeScript modules:
| Module | Job |
|---|---|
| Tank manager | mDNS discovery, 1 WS + 1 video connection per tank, auto-reconnect, online/offline |
| Fan-out | re-share telemetry + video to any number of browsers and the AI engine |
| Control gateway | forward browser/AI commands with the matching tank key (human/ai role); the tank arbitrates |
| Auth | admin login (+ optional view-only guest), session cookies; tank keys server-side only |
| History | telemetry at full rate 7 days, downsampled to 1 Hz for 90 days; graphs |
| Recording | per-session video + telemetry, snapshots, disk cap, oldest-first deletion |
| Missions | saved waypoint routes per tank, sent to the tank's ESP32 |
| Events/logs | timeline: bumps, control changes, alarms, AI decisions |
| AI API | same commands as the app + video frames + event stream |

Storage: SQLite (single file) for settings, tanks, missions, events, telemetry history; video as MP4 (ffmpeg) on disk.

Web UI (React/TS) screens: Fleet overview · Drive (video, 2 joysticks, tilt, gauges) · Map (track + mission editor) ·
History (graphs, timeline) · Recordings (playback + synced telemetry) · Tank settings (calibration wizards, limits, sounds, keys, WiFi/OTA links) · AI (status, controlled tanks, decision log).

Deployment: Docker Compose (hub + AI engine), data in a mounted volume.

## 25. ESP32 firmware architecture (APPROVED, design section 2)
FreeRTOS tasks (Arduino-ESP32 core, PlatformIO; reuse ELEGOO camera code), PSRAM frame buffers:
| Task | Priority | Job |
|---|---|---|
| Uno link | highest | binary frames @115200, 100 ms heartbeat, latency measurement |
| Control/WS server | high | `/ws` clients, key auth, JSON ⇄ binary |
| Navigation | high | pose (x, y, θ) from Uno telemetry; return-home, waypoints, go-to-point |
| Camera | medium | MJPEG :81 (max 2 viewers), `/capture`; camera settings via API |
| WiFi manager | low | saved networks → AP fallback, captive setup page, mDNS `conqueror-XXXX` |
| OTA + REST | low | `/api/...`, firmware upload, info |

Arbitration split: **ESP32** arbitrates network clients (human vs AI by key role, handback timers) and forwards ONE command stream tagged with its source;
**Uno** arbitrates IR vs network and applies safety last (IR + safety survive an ESP32 freeze).
NVS storage: WiFi networks, tank name, keys (hashed), camera settings, saved missions.
Failures: Uno link lost → camera continues, "body offline", drive rejected · WiFi lost → Uno deadman stops, ESP32 reconnects/falls back to AP · ESP32 crash → watchdog restart; Uno already stopped; IR works.

## 26. Safety, calibration, self-test (APPROVED, design section 3)
Safety thresholds (all settings):
| Check | Trigger | Action |
|---|---|---|
| Tilt danger | pitch/roll > 35° | stop + alarm; locked until level for 1 s |
| Lifted | accel magnitude departs from 1 g (sustained) + free-spinning encoders | stop + alarm |
| Bump | accel spike > 1.5 g | brief stop + event with g-force |
| Stall | driven but no encoder motion 0.5 s | cut that motor + stall fault |
| Obstacle | < 15 cm | brake; proportional slowdown from 50 cm |
| Deadman | no command 300 ms | smooth stop |
| Battery (2S) | < 7.0 V low, < 6.6 V critical (under load) | warn / disable motors |

Calibration (from app/hub, stored): 1) gyro bias (2 s still; also auto when idle-still) · 2) motor deadband ramp (~20 s, needs floor space) ·
3) encoder distance (drive N counts, user enters measured cm) · 4) turn check (spin 360°, gyro vs encoders → slip) · 5) servo home trim.

Self-test at boot (~2 s; result reported + beep/LED): IMU alive/not saturated, battery plausible, ultrasonic echo, encoders clean (if enabled),
settings checksum, last reset reason. Critical (IMU, battery read) → driving locked. Non-critical (ultrasonic) → warning, obstacle brake disabled.

## 27. AI engine (IN PROGRESS)
### Requirements (user)
- **All** capability groups wanted: natural-language commands, vision perception, autonomy/mapping, watch & alert, personality & fun.
- **First priority: natural-language commands** (LLM agent with tank tools).
- Fun headline: **"don't let the baby catch you"** chase game.
- Add **depth perception** (Depth Anything or newer) and other recent AI capabilities.

### Child-play mode safety envelope (DECIDED principle; enforced below the AI)
- Speed cap (~20 %), gentle acceleration; never drives toward the child (retreat/circle only).
- Any bump / grab / lift → immediate stop, motors off.
- No cliff sensor: flat, user-chosen floor area only; camera depth helps but is NOT trusted for stairs.
- Requires track guards (pinch hazard) and adult supervision; the app asks "adult present?" before starting.
- Limits enforced by the Uno/ESP32 (reflex layer), so the AI cannot exceed them.

### Candidate technology (state as of 2026-10, verify versions at implementation time)
- Monocular depth: Depth Anything V3 (Nov 2025), V2 Small (proven, light), YOLO26 depth (fast; vendor claims) → "virtual laser scan", obstacle avoidance beyond the ultrasonic, partial drop-off cues.
- Open-vocabulary detect/segment/track: SAM 3 (Meta, Nov 2025, text-prompted concepts) + fast YOLO-family trackers; body pose for posture/gestures.
- Mapping: MASt3R-SLAM (~15 fps), VGGT-based SLAM, MAGiSt3R (Jul 2026) fused with encoder odometry; **semantic object memory** ("where did you see my keys?").
- Language: LLM agent (Claude) with tank tools; vision-language scene understanding; daily exploration diary; voice in/out via phone/hub (tank has only a buzzer).
- Fun modes: baby chase, follow-me, hide-and-seek, pet play, photographer mode, guard/patrol with alerts, personality engine.
- VLAs (SmolVLA, NORA, OpenVLA, π0): arm-oriented, need demos → **not now**; watch the space.

### Layered control (PROPOSED)
| Layer | Latency | Where | Examples |
|---|---|---|---|
| Reflexes | 1–20 ms | Uno (+ESP32) | obstacle brake, bump stop, child-mode limits |
| Skills | ~100 ms (5–15 fps) | AI engine, fast local models | follow, flee, track, depth avoidance |
| Planner | seconds | LLM agent | "play chase 5 min", "find the keys" |
The LLM never drives the motors directly; it selects and supervises skills.

### LLM provider strategy (DECIDED: switchable local ⇄ cloud, must be fast; all options on the table)
- Speed by architecture: the LLM is planner-only; skills run fast loops; a slow LLM never makes driving slow/unsafe.
- Instant "got it, thinking" feedback (sound/LED) on every command.
- Hard per-call time budget (e.g. 3 s for commands) → fallback cloud⇄local, or safe hold + "not sure, try again". Never waits indefinitely.
- Compact world-state text summary in prompts; camera images only when the LLM needs to look.
- **LLM router with profiles** (each user-configurable: provider, model, time budget):
  | Profile | Job | Target |
  |---|---|---|
  | Fast | everyday commands, reactions, chat | < 1–2 s |
  | Smart | multi-step missions | < 5 s |
  | Vision | scene questions | < 3 s |
- Cloud candidates (Anthropic SDK, current as of 2026-10): Claude Haiku 5.5 `claude-haiku-5-5` (fastest/cheapest; thinking can be disabled),
  Claude Sonnet 5.5 `claude-sonnet-5-5` (balanced; effort low or `between_tools`), Claude Opus 5.5 `claude-opus-5-5` (best planning; thinking can't be disabled → effort low; optional fast mode, Claude API only).
  Latency levers: streaming + eager tool-input streaming, prompt caching of system prompt + tools, low effort.
- Local candidates: SLMs with tool calling via Ollama / llama.cpp / MLX (Mac) / vLLM (NVIDIA); choose by benchmark at implementation time.
- Offline mode: fully local when there is no internet.

### Vision-first, hardware-agnostic engine (DECIDED principles)
User: "vision is the main input; the rest are just outputs." The whole engine (not only the LLM) is configurable to whatever hardware people have.

Camera bottleneck: stock OV3660 on ESP32 (3 MP sensor, but MJPEG, ~10–20 fps @ VGA, ~100–300 ms latency, narrow FOV (lens-dependent), low light limited).
Upgrade options: wide-angle OV2640 (120–160°, same connector, ~₹300, biggest win) · OV5640 (5 MP AF; may need ESP32-S3) ·
Pi Zero 2 W + camera (H.264, much better) · mounted phone.

Design:
1. **Camera source abstraction:** ESP32 MJPEG / H.264-RTSP / phone / USB.
2. **Pose-tagged frames:** every frame carries tank pose, heading, pan/tilt at capture time → image bearings become world bearings/positions.
3. **Metric depth:** relative monocular depth scaled to cm using the ultrasonic reading as an anchor; one-time checkerboard camera calibration (intrinsics).
4. **Active vision:** the AI steers pan/tilt (scan, keep target in view, glance toward turns).
5. **Configurable perception pipeline** (per stage: local/cloud/off, model, fps, resolution):
   detect+track (YOLO family, SAM 3) · depth (DA V2/V3, YOLO26-depth) · pose/gestures · 3D mapping (MASt3R/VGGT-SLAM) · scene understanding (Claude vision or local VLM).

Hardware tiers (auto-detected + benchmarked at first start; overridable):
| Tier | Hardware | Runs |
|---|---|---|
| 0 Cloud | any laptop, no GPU | tiny local detector; VLM/LLM via cloud |
| 1 CPU/Pi | Pi 5, old PC | + YOLO-nano tracking, low-res depth 2–5 fps |
| 2 Apple Silicon | M1–M4 | + real-time depth, SAM 3 on demand, local SLM (MLX) |
| 3 NVIDIA | RTX-class | everything local: real-time SAM 3, 3D mapping, bigger local LLM/VLM |
- Features declare requirements; unavailable ones are greyed out with the reason (never half-working).
- One config file + hub settings page; presets `cloud`, `pi`, `mac`, `nvidia`.

### Stock camera (CONFIRMED by user: OV3660)
- Sensor: **OV3660**, 3 MP (2048×1536), better low light and image quality than the OV2640.
  (My earlier OV2640 inference from schematic voltages was wrong; the ELEGOO code special-cases OV3660_PID in app_httpd.cpp:697.)
- Board: ESP32-WROVER-Camera-V1.2, 24-pin 0.5 mm FPC; code pin map = CAMERA_MODEL_M5STACK_WIDE; original default stream SVGA 800×600.
- Usage plan: stream ~640×480–800×600 for real-time vision (WiFi-bound); **full-res 3 MP snapshots on demand** for detail (labels, small objects).
- Firmware reads the sensor PID at boot and applies per-sensor settings (OV2640 / OV3660 / OV5640).
- Upgrades: wide-angle OV3660 (exists, less common) · wide-angle OV2640 (drop-in, lower quality) · OV5640 compatibility unknown until tested · Pi Zero 2 W = board replacement.
- Fisheye distortion (if a wide lens is fitted) is corrected by checkerboard calibration.
- ESP32↔Uno UART: ESP32 IO33 = RX, IO4 = TX on the 4-pin XH2.54 connector P1 (matches the original firmware's Serial2).

### Open
- [x] Camera: **keep stock OV3660**; user may upgrade later after comparing results.
  → Add a **camera benchmark tool** (hub/AI): records fps, end-to-end latency, low-light noise, detection confidence/recall on a fixed test scene, depth quality, FOV coverage → side-by-side report to compare cameras before/after an upgrade.
- [x] AI compute: **auto-detect** (Mac / NVIDIA / CPU) + cloud fallback; tiers per §27.

## 28. AI engine architecture (APPROVED, design section 5)
Pipeline: Hub (video + telemetry) → 1 Frame ingest → 2 Perception pipeline → 3 World model ← queries ← 5 Agent/LLM planner → picks/supervises 4 Skills → commands to hub (AI key) → tank (safety always applies).
1. **Frame ingest:** frames matched to telemetry (pose, heading, pan/tilt) via the ESP32 capture timestamp per frame.
2. **Perception pipeline:** configurable stages (§27); a budget scheduler runs cheap stages every frame, heavy ones on keyframes/on demand.
3. **World model:** tank state/pose; tracked objects with world positions + last-seen (persistent semantic memory); occupancy map (depth + ultrasonic + pose); people tracks (e.g. baby position/approach speed).
4. **Skills** (5–15 Hz loops; start/stop/status; declare requirements): look_around, track(target), follow(target, distance), keep_away(target),
   find(object), go_to(x,y) (delegates to ESP32 navigation), explore, patrol, photograph, avoid_with_depth.
5. **Agent/planner:** LLM router (fast/smart/vision profiles, time budgets, fallback); tools = skills + queries (what do you see, where is X, snapshot, say/beep/LED); runs on commands and events, not continuously.
Also: personality engine (mood → sounds/LED/motion style) · mode envelopes (child-play etc., duplicated here; tank enforces hard limits) ·
voice STT/TTS via the hub (local or cloud) · **replay mode** (re-run recorded sessions offline) · model registry (download/cache per tier) ·
observability (decision log, per-stage timing, shown on the hub AI page).

## 29. Testing (DEFERRED by user)
- Design section 6 (simulator, layered test strategy) was rejected as not useful for now. Testing is decided per task during implementation planning.
- Kept minimal: a short **real-tank safety checklist** (deadman, obstacle brake, lift, tilt, battery, child-play limits) run before trusting new firmware.

## 30. Repository (DECIDED)
Monorepo, remote `https://github.com/mannuray/conqueror.git`, branch `main`:
```
docs/            design notes, specs, wiring guide
protocol/        single message definitions → generated Uno C, ESP32 C++, TypeScript, Python
firmware/uno/    Arduino Uno body firmware (PlatformIO)
firmware/esp32/  ESP32 camera/comms firmware (PlatformIO)
hub/             Node/TypeScript server + React UI
ai-engine/       Python AI engine
docker-compose.yml
```

## 21. Operating modes: standalone vs hub (DECIDED by user requirement)
- **The tank must be fully controllable by the app without any server** ("standalone"). The hub is an optional add-on for rich features.
- Standalone (app ↔ ESP32 directly, tank AP or home WiFi): driving (joysticks, tilt), video, pan/tilt, all safety, LED/sound,
  live telemetry, gyro turns, drive-distance, **pose / return-home / waypoints (on the ESP32, §17)**, calibration and settings, IR remote.
- With hub: everything above + user accounts, multi-viewer fan-out, telemetry history/graphs, recording, map view, mission editor, logs, AI engine.
- **The tank is always the server**; the hub is a client that auto-discovers tanks via mDNS (`_conqueror._tcp`, §23) and connects.
- The app has two connection targets: "Tank (direct)" or "Hub"; same command set, so the app code is shared.
- App and hub may be connected simultaneously; priority arbitration decides control.
- ESP32 video budget ≈ 2 MJPEG viewers (e.g. app + hub); extra viewers go through the hub.
- **Design rule: the tank never depends on the hub.** Hub failure changes nothing for app/IR control.

## 18. Firmware updates (DECIDED: option A)
- ESP32: OTA over WiFi (upload from the setup page by admin; can also be pushed by an admin-key client).
- Uno: USB only, with the ESP32 serial link disconnected during upload (shared D0/D1), as in the original kit.
- The ESP32 checks the Uno firmware version at boot and warns in telemetry/UI if it is incompatible with the ESP32 protocol version.

## 16. ESP32↔Uno link (PROPOSED, internal)
- UART (Uno D0/D1 ↔ ESP32 Serial2), **115200 baud** (the old firmware used 9600; too slow for 20 Hz telemetry).
- **Binary frames:** `0xAA | len | type | seq | payload | CRC-8`. Small, fast to parse on the Uno, detects corruption.
- The ESP32 translates JSON (clients) ⇄ binary frames (Uno). Unknown/corrupt frames are counted and dropped.
- Link heartbeat both ways (~100 ms); the Uno treats link loss as a deadman stop.
- USB debugging shares D0/D1 → the debug console on the Uno uses the same framed protocol (a small PC tool decodes it), or the ESP32 is unplugged while debugging.

## 20. Uno firmware architecture (APPROVED, design section 1)
Cooperative, non-blocking scheduler (no delay()):
| Task | Rate | Job |
|---|---|---|
| Link | every loop | RX/validate frames, TX telemetry |
| IMU | 200 Hz | gyro/accel, heading, tilt, bump |
| Motor control | 100 Hz | speed PID (encoders) or open-loop comp, heading PID, ramps, deadband |
| Safety supervisor | 50 Hz | lift, tilt, obstacle brake, deadman, battery, stall; overrides everything |
| Servos | 50 Hz | smooth pan/tilt profiles |
| Ultrasonic | 20 Hz | non-blocking, interrupt-timed echo |
| LED/buzzer | 50 Hz | priority-layered patterns/tones |
| Battery / telemetry | 10 Hz / 1 Hz | filtering, health stats |

Modules: drivers (motors, encoders, IMU, ultrasonic, servos, buzzer, LED, IR decoder, battery);
control (speed controller interface with open-loop/encoder-PID implementations, heading controller, motion primitives);
arbitration (IR/human/AI, handback timers); safety supervisor (final gate); comms (binary frames); settings (EEPROM + checksum, defaults on corruption).

Data flow: source (link/IR) → arbitration → primitive → heading/speed controllers → **safety supervisor** → motor driver.
Watchdog reset with the reset reason reported at boot.
Timers: T0 motor PWM + timekeeping, T1 servos, T2 buzzer; IR + ultrasonic via pin-change interrupts.

## 13. Risks
- **Uno flash/RAM budget** (32 KB / 2 KB) with the full feature set. Mitigations: no ArduinoJson/IRremote/I2Cdev libraries;
  tiny single-LED WS2812 driver instead of FastLED; compact protocol; build a skeleton first and measure.
  Overflow plan: move derived metrics (runtime estimate, roughness, time-to-collision, lifetime stats) to the ESP32.

## 7. Open questions
- [x] Encoder motor: DFRobot FIT0458 (see 6d).
- [x] Compass: user is buying a cheap "HMC5883L" board (likely a QMC5883L clone). The firmware auto-detects 0x0D (QMC) / 0x1E (HMC) and reports which one at boot. Chip marking "DA 5883" = QMC, "L883" = genuine HMC.
- [x] FIT0458 fit: stock motors are TT format, so it is a drop-in.
- [ ] Battery pack voltage confirmation (assumed 2S Li-ion 7.4–8.4 V).
- [x] Stock motor: TT format (exact RPM unknown; not needed, since calibration measures it).
- [ ] "Compass": does this mean the MPU6050 gyro heading, or adding a real magnetometer? (See discussion.)
- [x] Buzzer: **passive** (tones and melodies possible).
- [ ] Buzzer pin: which of A0/A1/A2 (confirm during wiring).
- [x] LEDs: 1.
- [x] Control handback timing (§2).
- [x] Client API (§15).
- [x] Hub host: anywhere (Docker, multi-arch).
- [x] Hub stack (§19).
- [ ] Uno↔ESP32 link format.
- [x] Telemetry contents and rate (§12).
- [x] Sound and LED catalogue (§8).

## 20. Tank Logo — a Logo-like language to program the tank (DECIDED 2026-10-11)
- **Who:** all three — children learning to code, the owner, and the AI engine (which writes programs).
- **Authoring:** a small built-in code editor (IDE) in the web app (hub and standalone Cockpit-lite); no block editor for now.
- **Language:** turtle commands map onto existing primitives — `FORWARD`/`BACK` → MOVE (cm, needs encoders), `RIGHT`/`LEFT` → TURN, `ARC` → arc turn; plus `REPEAT`, `TO … END` procedures, variables, `IF`, `WAIT`, sensor values (`DISTANCE`, `TILT`, `BATTERY`), `BEEP`/`PLAY`, `LED`, `LOOK` (camera). No pen: the app draws the turtle path from odometry.
- **Split across tiers:**
  - **ESP32 gateway — program runner:** receives a validated, compiled step list; runs it on the tank, waits for each primitive, evaluates simple sensor conditions, enforces loop/time limits, aborts on STOP / IR / owner change, reports the current line. Works standalone (no hub, no AI).
  - **Hub / web app — IDE:** editor, syntax highlighting, child-friendly errors, path preview before running, step mode, live turtle path, saved-program library per child. Parses and compiles; uploads the step list.
  - **AI engine:** natural language → Logo program (shown and previewed before it runs), explains code, **vision words** (`FIND`, `FOLLOW`, `WAIT UNTIL SEE …`). Programs using vision words run in the AI engine, which sends core steps to the tank.
  - **Bench tool:** `bench logo run file.logo` — first runnable version, executes client-side command by command.
  - **Uno:** no change.
- **Two program kinds:** core programs run on the ESP32 standalone; AI programs (any vision word) run in the AI engine.
- **One language, two parsers** (Python for bench/AI, TypeScript for the IDE), kept identical by a shared grammar spec + shared conformance test programs with expected step lists (same approach as the protocol vectors).
- **Safety:** every step is an ordinary command, so obstacle brake, child-play envelope, deadman, tilt/lift all apply; STOP or IR aborts the whole program; loops have step and time limits; a program runs under its sender's priority (IR > human > AI).
- **Roadmap:** spec now (design only; Plans B–D unchanged) → after Plan D: Python package + `bench logo run` → phase 4: ESP32 runner + program API → phase 5: IDE in Cockpit-lite → phase 6: library, natural language → code, explanations, vision words.
- Next: language spec `docs/superpowers/specs/2026-10-11-05-tank-logo-design.md`.
- Spec approved 2026-10-11 (`docs/superpowers/specs/2026-10-11-05-tank-logo-design.md` §12): English keywords only; events later (v2); no timed fallback without encoders (clear error); saved programs on the tank run from IR keys 1–9 in v2; `STOP` ends the whole program.

## 21. Home deployment topology (DECIDED 2026-10-11)
| Device | Role |
|---|---|
| Raspberry Pi 3 Model B (1 GB, owner's, 2017) | **Hub**: NestJS backend + SQLite + serves the web app; always on |
| PC | **AI engine**; switched on when AI is wanted; connects to the hub as a client and may come and go |
| Phone (Redmi Note 8 Pro) | **Controller**: Cockpit/IDE in the browser, phone-tilt steering, IR blaster as backup NEC remote, optional overhead camera for path checks |
| Tank | Works standalone with the phone when the Pi and PC are off |
- The phone is not used as the AI engine (CPU-only Python in Termux, no GPU — too weak for vision-first) nor as the primary hub host (no Docker, MIUI background killing, battery wear when always charging).
- Hub on the Pi: arm64 Docker image if the Pi runs a 64-bit OS (Pi 3/4/5); armv7 (Pi 2 / 32-bit OS) would need an extra image or native install; armv6 (Pi 1/Zero) cannot run Node 24. **Confirmed: Pi 3 Model B (1 GB)** → 64-bit Raspberry Pi OS Lite + the arm64 hub Docker image as specced; prefer Ethernet (WiFi is 2.4 GHz only); hub budget ~200–300 MB RAM.
- Hub LLD notes for the Pi: never re-encode video on the Pi (store the MJPEG stream / remux only); recordings on a USB disk/SSD, not the SD card; `docker-compose` splits hub (Pi) and AI engine (PC) instead of one compose file.

## 22. Hardware correction: the ESP32 board is an ESP32-S3 (2026-10-12)
- Photo of the board: silkscreen **ESP32S3-Camera-V1.0**, module **ESP32-S3-WROOM-1**, USB-C connector, test points T1–T14. The tank joins the home network (owner-modified firmware) as `esp32s3-9e74b4.lan`.
- Every earlier statement "ESP32-WROVER camera board V1.0" (specs 00/02, ESP32 LLD, bridge LLD, Plan C, CLAUDE.md) is **superseded**.
- Impact: Uno and Plan B unchanged. Plan C and the ESP32 gateway LLD must change build target to `esp32s3`, the UART pins to the Uno and the camera pin map (WROVER pins IO33/IO4 do not apply), and check PSRAM size/mode (WROOM-1 Nx Ry variant, octal PSRAM if R8).
- Open: pin map source — owner's modified firmware source, ELEGOO's ESP32-S3 kit code, or a read-only esptool flash dump. Plan C's first step must verify the chip with esptool before flashing.

## 23. Uno read-back (2026-10-12, read-only, from the Linux dev machine)
- Board answers as **ATmega328P** with **Optiboot v4.4** at **115200** baud through a **CH340** USB chip (`/dev/ttyUSB0` on Linux). Plan B upload settings fit; Optiboot 4.4 does not pass MCUSR → Plan B ruling R16 (reset reason "unknown" unless plausible) applies to this board.
- Installed firmware: 31.9 KB of 32 KB, ArduinoJson-based ELEGOO-style JSON commands (`D1`/`D2` keys) but not byte-identical to the 2021 ELEGOO source (no `_ok}` replies) — a newer/modified build. Replaced by Plan B.
- The ESP32-S3 camera board is not detected over USB-C on either machine with cables that work for the Uno → suspect the board's USB-C power/data path; chip and pins to be confirmed another way (ELEGOO's current S3 code, or a different cable/adapter).

## 24. ESP32-S3 pin map and old-firmware interface (recovered 2026-10-12)
Source: read-only flash dump of the owner's board (source code lost) + disassembly of the running app. Chip via esptool: ESP32-S3 (QFN56) rev v0.2, 8 MB flash (GigaDevice), 8 MB embedded octal PSRAM, MAC 48:ca:43:9e:74:b4. Built as Arduino `esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi`.
| Function | Pins / values | Confidence |
|---|---|---|
| UART to Uno (`Serial1`) | **RX GPIO3, TX GPIO40**, old firmware at 115200 8N1 | high |
| Camera (= ESP32-S3-EYE map) | PWDN −1, RESET −1, XCLK 15, SDA 4, SCL 5, D7..D0 = 16, 17, 18, 12, 10, 8, 9, 11, VSYNC 6, HREF 7, PCLK 13; XCLK 20 MHz, JPEG | high |
| USB | native USB-Serial/JTAG (GPIO19/20), enumerates as 303a:1001 → `/dev/ttyACM0` | confirmed |
- GPIO3 is a strapping pin (JTAG source select, sampled at reset) — fine for UART RX after boot; note for the gateway LLD. GPIO 26–37 are taken by flash/octal PSRAM.
- Old firmware interface: `/stream` (port 80), `/control` + `/cam_status` (port 81, camera settings only: res, quality, fps); **motion = raw UDP bridge on port 8888** (each datagram forwarded unchanged to the Uno, Uno lines returned to the sender); no STOP, no timeout on the ESP32 side. AP mode only: open setup portal on 8080 + DNS 53.
- Old Uno protocol (ELEGOO JSON): `{"N":100}` = stop/standby; `{"N":2,"D1":dir,"D2":speed0-255,"T":ms}` = timed move (dir 1 left, 2 right, 3 forward, 4 backward).
- Impact: Plan C and the ESP32 LLDs switch to target `esp32s3`, UART RX 3 / TX 40 at 250000, S3-EYE camera map, octal PSRAM; flashing via USB-CDC (`/dev/ttyACM0`). Revise when polishing Plan C.

## 25. Catalogue update from the old firmware (DECIDED 2026-10-12)
Compared the decoded old LED/buzzer behaviour (`docs/legacy/uno-old-led-buzzer.md`) with §8. Changes to §8:
- **LED:** add *obstacle brake stop* → fast red blink (pri 2). Idle colours **breathe**; while driving, **brightness rises with speed**. Toggle confirmation: **3 quick blinks, green = on, red = off** (mute, quiet, parking beeps, override). Brightness cap setting **`led_max` default 50/255**. Startup stays rainbow; collision stays a short white flash (old never-clearing white flicker was a bug — discarded).
- **Sounds:** startup = the old **rising sweep** (200, 500, 1000, 1500, 2000, 2500, 3000 Hz, then 1500/2000 Hz). Tilt siren is built on the old **2500/3000/2500 Hz chirp**; the same chirp plays **once** when the obstacle brake stops the tank. The old **13 melodies** become named catalogue sounds (≈0.5 KB flash) usable by the app, AI and Tank Logo `SOUND`. Low/critical battery sounds stay as in §8. Old blocking player discarded (new player is non-blocking).
- **Battery:** low warning **7.2 V**, critical **6.6 V**, **0.2 V hysteresis** on both.

## 26. Safety Override mode (DECIDED 2026-10-12)
The old firmware could switch the safety stops off (`N=11`); the owner wants human full-control, but bounded:
- **Relaxes:** obstacle brake and bump stop **off**; tilt cut-off raised to **65°** (tank tips at ~75°; wide, low centre of gravity).
- **Never relaxed:** deadman / link-loss stop, battery critical, motor stall cut, lift detection, STOP.
- **Who:** human only — IR remote (hold a key 2 s) or app/web with a human/admin key and a confirmation. **Never the AI, never a Tank Logo program, never while the child-play envelope is active.**
- **Indication:** LED amber-red pulse over the base colour, chirp on enter/exit, telemetry flag, hub timeline entry.
- **Ends:** after **60 s without driving** or **5 min** maximum, and on STOP, owner change, link loss or reboot. Never persistent.
- CLAUDE.md safety rule reworded: never weakened **except through this designed Override**.
- Follow-up (design-first): protocol additions (override command/event/flag, settings), spec 02 + Uno LLD sections, then Plan B tasks.
- 2026-10-12 owner decisions on the Override design (Uno LLD §10.8): (1) flag in `flags2` bit 8 + event — accepted; (2) IR `*` held 2 s — accepted, code 0x42 confirmed at bring-up; (3) refused while a hard fault is latched — accepted; (4) **only the child-play envelope blocks Override** (marked by `ENVELOPE.lock_brake` bit 1 `CHILD_PLAY`); adult envelopes keep their speed/accel caps under Override; (5) Plan B only detects bumps — Override bump relaxation is a no-op until a bump stop exists; (6) **critical self-test failure (IMU missing/failed, battery reading implausible) locks all motion** until a passing `SELF_TEST` re-run or reboot, Override cannot lift it (LLD §10.9); (7) **EEPROM save staged per block** so the writer queue (128 → 80 B) can never overflow (LLD §10.10).
- 2026-10-12 owner: a self-test lock caused **only** by the battery-range check clears **automatically** once the battery reading has been 5-9 V continuously for 1 s (USB-only boot unlocks when the battery is switched on; event/LED/sound as for a passing re-run). An IMU failure still needs an explicit self-test re-run or reboot; Override still cannot unlock (Uno LLD §10.9).

## 27. Self-describing firmware (DECIDED 2026-10-12)
Lesson from the lost old firmware source: every board must be able to tell how to talk to it and where its code is.
- **ESP32 read-only pages:** `/help` (human page) and `/info` (JSON) — firmware name, version, **git commit**, build date, repo URL; WiFi mode, IP, mDNS, ports, Uno link baud; protocol cheat-sheet **generated from `protocol/defs`** (frame format, main commands with example bytes, STOP, error codes); hardware facts (board, chip, pin map, flash/PSRAM); reflash commands (esptool/pio; Uno: Optiboot 115200, S1 switch); Uno firmware version/commit from HELLO.
- **Embedded source:** the ESP32 build packs `git archive` of the whole repo (incl. Uno source; compressed, ~1–2 MB) into a separate flash partition, served read-only at `/source.tar.gz`.
- **Rules:** read-only (no control, no settings — the dev bridge has no auth); **no secrets** (`secrets.h` is git-ignored and never packed); static, small.
- **Habits:** embed the git commit + version in both firmwares (HELLO, `/info`, bench tool); tag every flashed release on GitHub with its binaries.
- **Where:** Plan C bridge gets `/info` + `/help`; phase 4 gateway gets the full page + `/source.tar.gz`; Plan B reports version + commit in HELLO. Applied when polishing Plan C and in the gateway LLD revision.
- 2026-10-13 applied: HELLO (0x80) carries `commit: u32` (first 8 hex digits of the git commit as a number, 0 = unknown); HELLO payload 5 → 9 bytes; **protocol_version 1 → 2** (breaking, nothing deployed). Uno build sets it via `scripts/git_commit.py` (PlatformIO pre-script, `-DCQ_GIT_COMMIT`); see Uno LLD §10.11.
- 2026-10-12 owner: Tank Logo **program runner stays on the ESP32** (confirmed); the `/help` page also carries a Tank Logo quick reference (words, limits, how to upload/run/stop a program).
- 2026-10-12 owner: motion must keep the old firmware's **gyro PID** behaviour and run on a **stock tank without encoders**; encoders only add features. Confirmed already in the design: setting `encoders_enabled` (default 0) — without encoders: gyro heading hold/turn-rate PID + open-loop speed with deadband compensation; with encoders: per-track speed PID, MOVE distance, stall/slip, odometry. Encoders enabled but silent while driving → fault + fallback to the gyro-only mode.

## 28. Basic Drive page on the ESP32 (DECIDED 2026-10-12)
A small phone/web driving page served by the ESP32, so the tank can be driven from a phone right after Plan C (no laptop/hub/Pi). It complements, not replaces, the rich hub/Cockpit UI (§ hub decisions stand).
- **Page:** touch joystick (release = stop) + optional **phone-tilt steering**; big **STOP**; speed slider; **camera view** + **camera pan/tilt** (returns home on release, as designed); live telemetry: battery, distance ahead, tilt, heading, current owner. **No** horn/LED buttons (sounds/colours stay in Cockpit and Tank Logo).
- **Under the hood:** phone → WebSocket (small JSON) → ESP32 → binary body-link frames (Plan A's `cq_protocol.h` compiled for ESP32) → Uno. The ESP32 performs the handshake and sends heartbeats while the page is connected; page gone/phone asleep → Uno deadman stops the tank (0.3 s). The WebSocket API is shaped as the **first slice of the phase-4 gateway API** (ESP32 LLD) so Cockpit-lite/hub reuse it.
- **Owner choices:** **no PIN/auth** ("keep it fun"; home network only, like the dev bridge); **one driver at a time** (first to drive owns control; others get BUSY; STOP accepted from anyone; IR remote beats all); **no extra speed cap** on the page (full capability; Uno safety — obstacle brake, tilt, deadman, envelope when set — still applies).
- **Where:** Plan C (bridge + Basic Drive page). Plan B unchanged. Order: ESP32 bridge LLD update (classes + diagrams) → Plan C polish (also ESP32-S3 pins, OTA, chip check, `/help` + `/info` + Logo quick reference).
- 2026-10-12 owner — Basic Drive page shape: **one HTML file**, inline CSS + **plain JS** (no framework, no npm, no build step), ~15–25 KB gzipped in flash, served at `/`; **responsive** (portrait: camera above joystick; landscape: camera centre, controls at the sides); styling borrowed from the Stitch mobile Cockpit (Phosphor) as plain CSS; the future phone app shows the same page in a WebView. Transport: one WebSocket `/ws` (drive ~20 Hz while held, STOP on release; telemetry ~10 Hz), camera as `<img>` on the MJPEG stream, pan/tilt via a small second joystick/arrows (returns home on release), pointer events for touch + mouse.
- Phone-tilt steering: browsers expose motion sensors only on https, so on `http://` the **toggle is shown only when the browser provides the sensor** (feature detection); full tilt steering comes with the phone app's WebView. No self-signed https.
- **Add to home screen:** tiny web manifest + icon so the page opens fullscreen like an app.
- 2026-10-12: designed in **`docs/lld/esp32-bridge-lld.md` Revision 2** (ESP32-S3 pins/partitions, build + chip check, `/info` `/help` `/source.tar.gz`, ArduinoOTA, link owner bench-vs-phone, frame-aligned UART writer, WS API slice); its R2.14 lists the pending owner confirmations.
- 2026-10-12 owner: **all 9 confirmations of ESP32 bridge LLD Revision 2 (R2.14) accepted** — link-owner rules (bench owns from TCP connect; phone owns from first drive until 5 s idle; STOP from anyone; master change re-handshakes; IR beats all), BUSY vs NOT_OWNER wording, **ArduinoOTA for the ESP32** with a password in `secrets.h`, only while nobody drives (Uno stays USB-only), source archive excludes the Stitch PNGs, no envelope sent by the bridge (full capability; child play set from hub/Cockpit later), max 2 camera viewers, telemetry 10 Hz + 1 Hz to the page, `drive{v,w}` with the speed slider on the page, protogen generates the `/help` protocol cheat-sheet.
- 2026-10-12: **buzzer confirmed passive** from the decoded old Uno firmware — Arduino `tone()` on A0 (Timer2 COMPA ISR) playing melodies of different frequencies; an active buzzer could only beep at one fixed pitch. Matches spec 0 (A0, passive).
- 2026-10-13 owner — Plan C rewrite confirmations accepted: calibration shown as owner "human" on the page; anyone may move the camera while nobody drives (only the driver while driving); Uno reset/admin requests answered UNAVAILABLE by the bridge (gateway feature); welcome carries `you`/`owner_client`; rate limits ≈25 drive msgs/s per client, max 4 WS clients; socket split 6 page/camera + 2 bench (lwIP 16). Controller ruling: STOP from the page jumps the TX queue (fast path), not FIFO.
- 2026-10-13 owner: bridge keeps the **1 s bench-silence drop**; Plan D's simulated WiFi-stall test uses **0.6 s** (still well above the Uno's 0.3 s link-loss stop). Plan D also reports "bridge busy" when the bridge closes the socket during the handshake.

## 29. Board switch: Arduino Mega 2560 now; Uno "Lite" profile for stock tanks (DECIDED 2026-10-13)
- **Why:** after Plan B Tasks 1–6 the Uno build used 24.2 KB of 30 KB flash and 1357 of 1750 B RAM, with Tasks 7–15 (encoders, calibration, PID, primitives, pipeline, servos, catalogue, IR driving) estimated at +12–18 KB. Compiler options saved only ~0.6 KB.
- **Decision (owner):** the body board becomes the **Mega 2560** (owner has one; SmartCar Shield V1.1 fits) — 256 KB flash, 8 KB RAM. Firmware stays board-agnostic (`BOARD_*`); domain code and Tasks 1–6 tests unchanged.
- **Hardware rework (Mega):** I2C jumpers **20→A4 (SDA), 21→A5 (SCL)** (shield wires the MPU6050 to A4/A5; firmware keeps A4/A5 as inputs); encoder signal wires moved from A1/A2 to **A8 (left) / A9 (right)** (PCINT2; A0–A7 have no pin-change interrupts on the Mega); IR receiver: jumper **D9→A10** (D9 has no PCINT on the Mega; D9 stays a high-impedance input). LED on D4 is PG5 on the Mega (driver change only). Motors D5/D6/D7/D8/D3, servos D10/D11, buzzer A0, ultrasonic D12/D13, battery A3, button D2 unchanged.
- **Stock tanks (owner question):** one codebase, two build profiles — **Full** (Mega: everything) and **Lite** (Uno: safety core, gyro heading/turn PID, DRIVE/TURN, sensors, IR, servos, basic sounds/LED; no encoder-only features — a stock tank has no encoders). HELLO `caps` is extended so clients hide/refuse unavailable features (as Tank Logo already does for `FORWARD` without encoders). Lite is designed and sized after Plan B is finished on the Mega.
- Next: Uno LLD "Mega 2560 board support" section → Plan B revision (Mega build env + board-support task before Task 7; Mega-aware drivers in Tasks 7, 12, 13) → continue execution.

- 2026-10-13 owner — Mega pins revised (LLD §11.1): IR jumper **D9→A14** (PK6); **buzzer moved off the shield to D24**; encoders (to be fitted later) **left A/B → A8/A9, right A/B → A10/A11** (channel A used now, B wired for quadrature later). Firmware starts with encoders disabled (stock gyro-only mode).
- 2026-10-13 owner: **buzzer stays on the shield A0 port** (its ground is already there); only IR (A14) and the encoder pins change on the Mega.

## 30. No hardware rework: the shield plugs onto the Mega as-is (DECIDED 2026-10-13)
- **Principle (owner):** users must never need jumpers, soldering or rewiring to make the project work — "no messy hardware changes that make or break the project". The SmartCar Shield V1.1 plugs onto the Mega exactly as onto the Uno; every pin difference is handled in firmware.
- **Supersedes** the jumper plan of §29 / LLD §11.1 (D20→A4, D21→A5, D9→A14, encoders on A8–A11): no jumpers at all.
- **IMU (I2C on A4/A5):** on the Mega, A4/A5 are not the I2C pins → a small **software I2C master** (bit-banged, open-drain via DDR) on A4 (PF4) / A5 (PF5). The Uno keeps the hardware TWI.
- **IR (D9):** on the Mega D9 has no pin-change interrupt → a **20 kHz timer sampler** (Timer1, free on the Mega) reads PH6 every 50 µs and reports falling edges to the NEC decoder. The Uno keeps its PCINT ISR. Echo (D12 = PB6) keeps PCINT0 on the Mega.
- **Encoders (owner, 2026-10-13):** encoder cables are independent wires, not shield rework, so on the Mega they plug **directly into the Mega header**: left A/B → **A8/A9**, right A/B → **A10/A11** (PCINT2, channel A used now, B reserved for quadrature). Uno: A1/A2 on the shield as before.
- Buzzer A0, motors, servos, LED, button, battery: unchanged (already worked).
- Costs: ~3–5 % CPU for the IR sampler; IMU burst read ≈ 0.5–0.7 ms (vs 0.38 ms) — inside the 5 ms IMU period; both documented in LLD §11.4.
- Order: LLD §11.4 → Plan B Task 6c (drivers + host test of the edge sampler) → code → hardware check (IMU answers, IR codes, encoders later).

## 31. Scheduler phases fix the once-a-second 6.8 ms pass (DECIDED 2026-10-13)
- On the Mega, max loop pass was 6.8 ms (limit 4 ms) while load was ~51 %. Cause: all tasks started at phase 0, so once a second all 14 tasks ran in one pass.
- Fix: each task has a start phase (IMU 0, control 2.5 ms, slow telemetry/camera 7.5 ms, UI 17.5 ms). Measured max pass 3.3 ms, load 52 %. Details: LLD §11.5.
- Investigated and rejected: removing the soft-I2C delays (breaks I2C timing), faster SCL polling (no gain).

## 32. Servo ports on the shield (CONFIRMED on the bench 2026-10-13)
- The SmartCar Shield V1.1 servo ports are labelled 1 and 2. Bench test (pan command only, then tilt only): **port 2 = D10 = pan (left/right)**, **port 1 = D11 = tilt (up/down)**.
- Plug the pan servo into port 2 and the tilt servo into port 1; brown/black wire to G/−. Swapping plugs is the fix if they are mixed up; firmware keeps D10 pan / D11 tilt (no software swap).

## 33. Ultrasonic: accept slow clone modules (DECIDED 2026-10-13)
- The owner's ultrasonic answers 2.2 ms after the trigger; the firmware only waited 2 ms, so it reported a fault although the sensor works (25 cm measured).
- The answer window is now 10 ms (still fits the 50 ms ping period). An unplugged or dead sensor still faults. LLD §11.6.

## 34. Shield probe on the owner's tank: both tracks forward = direction pin HIGH (CONFIRMED 2026-10-13)
- Probe on the stand (PWM ramp to 120 per track and direction): left forward/reverse correct; right ran backward with the ELEGOO starting guess (right forward = LOW).
- Fix in firmware only (`board_config.h`: `kDirRightForwardHigh = true`); motor wiring stays stock. Re-probe: all four cases PASS.
- Bench note: a calibration request sent right after boot was refused (`CAL_PRECONDITION`). Cause: "moving" read true for the first 300 ms after boot; fixed with `MotionTimer` (LLD §11.7). An IR key press during a script cancels it (by design: IR takes ownership).

## 35. Phase 2 floor tests run wirelessly after Plan C (DECIDED 2026-10-13)
- The owner has no USB cable long enough for floor driving, and on the stand the gyro cannot see the tank turn (tracks in the air), so turn/heading/MOVE accuracy can only be proved on the floor.
- Plan B is signed off on **Phase 1 (stand)** alone. **Phase 2 (floor)** runs after Plan C over WiFi: the ESP32 bridge passes TCP 3333 frame-aligned to the Mega, so the smoke tool only needs a TCP transport (pyserial `serial_for_url("socket://conqueror-bridge.local:3333")`) — or the Plan D bench tool. Shield switch S1 on the ESP32 side for these runs.

## 36. Plan B final-review minor items — what we did (DECIDED 2026-10-13)
| # | Item | Decision |
|---|---|---|
| 1 | Settings loader buffer was a hand-written 64 B | **Fixed:** sized from the settings structs (can never be too small) |
| 2 | "clearDirty after save can hide a change made mid-save" | **No change needed:** the dirty flag is not read anywhere; saving is explicit (SETTINGS_SAVE saves the values at that moment, a second SAVE during a save gets BUSY), so nothing is silently lost |
| 3 | Smoke tool termios2 also needs the input speed (CIBAUD/BOTHER<<16) | **Plan D:** the bench tool's `SerialTransport` does it right (Plan D Global Constraints) |
| 4 | Smoke tool's exit STOP is dropped while paused | **Leave:** firmware link-loss stops the tank anyway |
| 5 | Encoder interrupt duration not measured | **When the encoders are fitted** (GPIO toggle + scope or micros) |
| 6 | WS2812 low-time margin for some clone LEDs | **Leave** unless an LED misbehaves |
| 7 | IMU check accepted only MPU-6050 (0x68) | **Fixed:** also MPU-6500 (0x70) and MPU-9250 (0x71), same register map; host test `test_imu_id_accepts_mpu6050_family` |
| 8 | DRIVE_RATE UNAVAILABLE sends no REJECTED event | **Leave:** the ACK already says it |

## 37. Plan C review follow-ups (DECIDED 2026-10-13)
- **Deferred (owner):** moving `/source.tar.gz` off the port-80 control server (review I2 part c) — not urgent; revisit when the project is finished. *(Done 2026-10-15 after Plan C check 9 — bridge LLD R3.11.)* Parts a, b, d of I2 are done (1 s socket timeouts, a stuck client is closed, the page skips drives while its send buffer is full).
- **Fix now (owner):** all 12 minor review findings (M1–M12) of the Plan C final review.
- **USB stays plugged in** until the first OTA update has worked on the board and the hardware checklist has passed; after that the ESP32 is updated over WiFi (the Mega stays USB-only).

## 38. The ESP32 must run on tank power alone (REQUIREMENT, defect found 2026-10-13)
- Found on the bench: with the USB cable removed (it broke), the ESP32 shows its red LED and a **dim green LED** but never joins WiFi and never opens the `conqueror-bridge` fallback AP — it does not get through start-up. With USB power it runs fine. The old ELEGOO firmware ran on tank power with the same camera settings (`docs/legacy/esp32s3-old-firmware.md`), so this is our defect.
- Likely cause: brown-out resets — the 3.3 V supply sags when WiFi and the camera start; our bridge also keeps the radio fully on (`WiFi.setSleep(false)`), unlike the old firmware.
- Fix build (needs a USB-C data cable once): (1) boot diagnostics — boot counter and last reset reasons kept in NVS, shown in the boot log and `/info`; (2) staged start — camera only after WiFi is up (or after 10 s); (3) WiFi TX power capped at 15 dBm. If brown-outs remain: re-enable modem sleep and measure the drive latency.
- Side note from the old firmware: it set `vflip` and `hmirror` on the sensor — ours does not yet (picture may be upside down / mirrored); check on the page.

- **Camera image inverted (owner, 2026-10-15):** upside down and mirrored. Fix: OV3660 → vflip + hmirror at camera start, as ELEGOO did (ESP32 LLD decision note). Updated over WiFi (OTA).
- **Camera, correction (2026-10-15):** vflip + hmirror left the image mirrored → **vflip only** (the raw image was only flipped vertically).
- **Cockpit-lite (owner, 2026-10-15):** the ESP32 phone page follows the phone Cockpit design (hub-ux Screen 7b) within what the bridge can do; bridge adds sound / cam_lock / caps / yaw_rate / :81/capture (bridge LLD R3); Plan F written for approval. No new mock-up (owner).
## 39. IR driving and motor power fixes (DECIDED 2026-10-13, owner)
- **IR style stays hold-to-drive** (option A): the tank moves only while a key is held. The release window grows from 200 ms to **350 ms** (NEC repeats come every ~108 ms; one missed repeat already exceeded 200 ms and stopped the tank while the key was still held). A repeat after a short dropout resumes driving. Cruise/tap-to-go is not used (maybe later, adults only, never in child play).
- **Motor voltage cap removed** (owner): `motor_v_max` default 12000 mV (above any 2S pack) — the motors get the full battery like the old ELEGOO firmware. The setting stays for anyone who wants a cap.
- **Deadband defaults from the old firmware and the owner's tests** (old: below 80 off, lowest real PWM 110 — `docs/legacy/uno-old-protocol.md`; owner measured 80–110): `deadband_start` **110**, `deadband_keep` **90** (all four). Settings, so they can be tuned on the floor without reflashing.
- **Bug fixed — reverse heading hold spun the tank:** the mixer's car-style reverse flip (`reverse_steer` 0) also inverted the gyro heading-hold correction, turning it into positive feedback. Only the driver's steering is flipped now; the correction never is. `carSteer()` flips the teleop steering before heading hold; the mixer never flips. Side effect: DRIVE_RATE and primitives (MOVE backwards) are now always physical turn rates in reverse too (they were flipped before — the same bug).
- A tank that already saved settings keeps its old values: set the new values with SETTING + SETTINGS_SAVE once (defaults only apply to fresh boards).

## 40. Product idea: own base board + off-the-shelf camera head — requirements (IN DISCUSSION 2026-10-14)
Owner's goal: a cheaper, better local (India) replacement for the Uno/Mega + shield in kits like the ELEGOO Conqueror. Agreed so far:
- **Two boards.** **Head:** an existing ESP32-S3 camera board (WiFi, camera, phone page) on the pan-tilt gimbal, linked to the base over UART — any ESP32 pin can be the UART, so a board only needs 2 free GPIOs + GND + 5 V input exposed (candidates: Seeed XIAO ESP32S3 Sense with U.FL antenna, Freenove ESP32-S3 CAM; the AI-Thinker ESP32-CAM is not recommended). **Base:** one board replacing Uno/Mega + shield (MCU, motor drivers, sensor connectors, power).
- **Form factor:** Uno size (68.6 x 53.3 mm) and the Uno mounting holes only — the base does **not** need to accept Arduino shields.
- **Servos:** 2 servo outputs on the base **and** on the head (clean wiring: the camera board sits on the gimbal). A setting picks the servo host (`base` or `head`) — never both at once; camera behaviour and the protocol stay the same. Servo power should not come through the ESP32's supply (bursts of 0.5-1 A cause brown-outs): recommended 6-wire harness (VCC, GND, TX, RX + separate servo 5 V/GND) or at least a large capacitor at the head's servo pins — to be decided.
- Base must cover what the Mega does today: 2 motor channels with A/B encoder inputs, IMU (I2C), ultrasonic, IR receiver, WS2812, buzzer, battery sense, mode button, UART + 5 V to the head, USB-C programming, 2 servo outputs.
- Open questions: base MCU (CH32V003 and STM32F030K6 are too small for the current body firmware — RP2040 or STM32G0/CH32V203 class suggested), charging on board (TP4056/MCP73831 are single-cell, the pack is 2S), 2 or 4 motor channels, harness (4 or 6 wires), certification for sale in India (BIS toys/Li-ion, WPC, EPR).
- **Decided (owner, 2026-10-14):**
  - **6-wire harness** to the head: VCC 5 V + GND (ESP32 logic), TX, RX, servo 5 V + servo GND straight from the base regulator (servo bursts never reach the ESP32 supply). Connector e.g. 6-pin JST-PH/XH.
  - **Base MCU: RP2040** (the smaller chips cannot hold the body firmware). PIO for the 2 quadrature encoders, WS2812 and IR; needs external QSPI flash (e.g. W25Q16) + 12 MHz crystal; 3.3 V I/O → level shifting for the ultrasonic echo and the WS2812 data line (e.g. 74AHCT1G125). RP2350 noted as a same-class alternative.
  - **Charging on the board through the same USB-C** that programs the RP2040; Li-ion 2S. 5 V USB → 2S needs a boost charger (IP2326/CS5090E class, or TI BQ25887 with cell balancing); USB-C PD input (IP2368 class) would be faster. Needs: cell balancing (in the charger or a 2S protection board), pack protection, NTC temperature sensing, fuse.
  - **Fixed 2S Li-ion pack** with its own protection (not loose 18650 cells), **5 V USB-C boost charger** (works with any phone charger, ~3 h for 2500 mAh), **2 motor channels**.
- **"No voltage thinking" for users** is a board-design rule, not only an MCU choice: every connector is 5 V-safe (5 V power on each, 5 V-tolerant inputs, outputs that 5 V parts accept; the WS2812 data line gets a 74AHCT1G125 buffer).
- **MCU under discussion:** RP2040 (decided earlier) vs **RP2350A** (recommended: Cortex-M33 with FPU for the IMU/PID float maths, 520 KB RAM, 3 PIO blocks, 5 V-tolerant GPIO inputs; ~INR 20-30 more; use A4 stepping - erratum E9 on internal pull-downs) vs Renesas RA4M1 (true 5 V, FPU, official Arduino core, but ~3x the price, no PIO, 32 KB RAM).
- **Decided (owner, 2026-10-14): MCU = RP2350A** (A4 stepping).
- **Charging and cables (owner, 2026-10-14, supersedes the on-board charger above): USB-C everywhere, one cable type.** Today a user needs three cables (mini-USB battery charger, USB-B Uno/Mega, USB-C camera). New: the **2S Li-ion pack has its own USB-C charging port** and protection inside the pack; the **base board has no charger** (USB-C for programming only); the **camera board** keeps its USB-C (rarely needed - it updates over WiFi). Any phone charger charges the pack. Battery connector on the base: a common one (e.g. JST-XH 2-pin) because packs come from different suppliers. If USB is connected while the base is powered from the battery, the firmware blocks the motors.
- **Motor channels (owner): 2 channels standard + an unfitted footprint for a second driver** (4-channel mecanum variant from the same PCB).

### 40.1 Base board - requirement summary (2026-10-14)
| Group | Requirement |
|---|---|
| Size | Uno outline 68.6 x 53.3 mm, Uno mounting holes; not shield-compatible |
| MCU | RP2354B (RP2350B + 2 MB flash inside), USB-C (programming) |
| Motors | 4 channels (2x TB6612), MOTOR C/D ⇄ SERVO 3-6 switch |
| Encoders | 4 connectors (one per motor), A/B each |
| Servos | 2 dedicated + 4 shared with motors C/D = 6; dedicated servo buck, jumpers 5 V / 6 V / BATT; >6 via PCA9685 on Qwiic |
| Sensors | Ultrasonic (HC-SR04), IR receiver, gyro breakout socket (GY-521), battery voltage sense |
| Optional sensors | Compass breakout socket (GY-271) at the board edge; Qwiic port for any other I2C sensor (ToF, newer IMUs, remote compass) |
| Output | Passive buzzer, 1 RGB LED (WS2812, buffered data line) |
| Buttons | No extra user buttons (owner). RESET, MODE (5 s WiFi reset, 15 s admin reset) and BOOT (recovery only, recessed pinhole) - all right-angle, side-pressed, on the board edge |
| Status LEDs | GPIO status LEDs (owner, 2026-10-14 - idea from the Cytron Maker Pi RP2040: shows pin activity, helps learning and debugging) |
| Head link | 4-wire harness: 5 V, GND, TX, RX |
| RC input | One small connector (5 V, GND, 2 signal pins, 5 V-tolerant): hobby 2.4 GHz receivers (PPM/SBUS) or a toy 27 MHz decoder's on/off outputs (owner, 2026-10-14: no built-in 27 MHz receiver; Bluetooth gamepad via the ESP32 is a possible later firmware feature) |
| Power | 2S Li-ion pack (USB-C charging in the pack) or 7-12 V adapter (barrel jack); main switch, reverse-polarity protection, TVS, fuse; 5 V logic buck, servo buck, 3.3 V LDO |
| 5 V-safe | Every connector 5 V-safe for the user (5 V power, 5 V-tolerant inputs, WS2812 buffer) |
| Head | Off-the-shelf ESP32-S3 camera board on the gimbal (UART + servo pins, WiFi OTA) |

- **Compared with the Cytron Maker Pi RP2040** (2026-10-14): a general education board - 1S/3.6-6 V motors (cannot run a 2S pack), no encoders/IMU/IR/camera link, not Uno-sized. Borrowed: GPIO status LEDs. **No Python on the board** (owner): Tank Logo is the beginner language. **Goal (owner, 2026-10-14): a good general-purpose robot board** - ELEGOO kits are one reference, not a compatibility target. **Connectors (owner): JST-XH 2.54 mm for the robot connectors, no Grove** (Grove's bigger footprint brings no real advantage; XH is compact, latching, cheap, easy to crimp and widely sold). **I2C (owner, 2026-10-14): a Qwiic port for expansion + sockets for a gyro breakout and a compass breakout.** Sockets fixed to the common modules - gyro GY-521 (MPU6050, 8 pins: VCC GND SCL SDA XDA XCL AD0 INT), compass GY-271 (QMC5883L/HMC5883L, 5 pins: VCC GND SCL SDA DRDY); any other I2C sensor (newer IMUs, ToF) on Qwiic. Compass socket at the board edge, far from the motor connectors (motor magnets/current disturb it); for a reliable heading use a Qwiic compass on a short cable mounted high (e.g. the camera mast). Addresses do not clash: MPU6050 0x68, QMC5883L 0x0D / HMC5883L 0x1E, VL53L0X 0x29.
- **Enclosure rule (owner, 2026-10-14):** anything the user must reach or see sits on a board edge, facing out - the top is covered once the board is in a robot or box. Buttons are right-angle side-press switches; proposed "service edge" with USB-C, power switch, RESET and MODE (one cut-out in an enclosure). Proposed: IR receiver at the front edge plus a connector for an off-board receiver; RGB LED at an edge or on a connector (headlight); status LEDs along an edge or side-view so they show through a slot - settle in the layout stage.
- **Market research (2026-10-14):** `docs/research/2026-10-14-base-board-market-india.md` - verdict: unlikely as a mass-market cheap Uno+shield replacement (clone stacks ~INR 500); plausible as a feature-rich robotics controller for hobbyists/colleges and white-label for STEM/ATL companies (estimate: hundreds to low thousands of units a year). Suggested prices (estimates): board INR 1,999-2,499, board + camera head INR 2,999-3,499, tank kit INR 4,999-6,499. Launch as a 14+ electronics board without cells to defer BIS toys/Li-ion work (to confirm with a consultant).
- **Market (owner, 2026-10-14): worldwide, not only India.** Pitch: ELEGOO/DFRobot-class tracked mechanics are good, their electronics weak (Devastator chassis alone ~US$85; Conqueror kit US$91-130) - same class of mechanics + our electronics (closed loop, IMU, camera head, phone driving, Tank Logo) at a lower price. Certification then also CE (RED), FCC, UKCA, RoHS, EN 71 if sold for children.
- **Tracks (owner wants the ELEGOO Conqueror / DFRobot Devastator style:** plastic track shoes joined by pins, toothed sprocket, sprung road wheels; Devastator: 43 mm drive wheel, ~225 x 220 x 108 mm, 780 g). No public supplier found. Plan: image search on 1688/Alibaba, a sourcing agent with photos + measurements (shoe width and pitch, sprocket teeth, road-wheel diameter), DFRobot B2B, ELEGOO support for samples; at volume, own injection-moulded track shoe.
- Still open: certification for sale in India (BIS toys/Li-ion, WPC, EPR) - research task; optional wired ESP32 update through the base (RP2350 USB-to-UART bridge, ESP32-S3 software-forced download mode) - nice-to-have, OTA is the normal path.
- **Superseded (owner, 2026-10-14): no servo pins on the camera head and no 6-wire harness** - little use, more complication. Servos (pan/tilt) only on the base; head link is a **4-wire harness** (5 V, GND, TX, RX). The servo-host setting is dropped. The entries above about head servos / 6-wire harness are kept as history.
- **Outputs and power input (owner, 2026-10-14):** **both motor drivers fitted on every board**; a 2-pole **MOTOR C/D ⇄ SERVO 3-6 switch** (pole 1 disables driver 2 in hardware, pole 2 tells the firmware on GPIO29) → 4 motors + 2 servos, or 2 motors + 6 servos (car/tank, mecanum, arm); more than 6 servos via a PCA9685 on Qwiic. **Dedicated servo buck** (~5 A) with jumpers **5 V / 6 V / BATT** (BATT = HV servos on a 2S pack only). **Barrel jack** for a 7-12 V adapter (switched jack disconnects the battery when plugged) - input range 6-12 V, TVS, 25 V capacitors. Supersedes "2 channels + unfitted second driver".
- **Parked (owner, 2026-10-14):** the base board stays at spec 06 + LLD for now; the schematic (KiCad 10, starting from the Raspberry Pi minimal design) comes later. Back to the tank.
- **MCU changed to RP2354B (owner, 2026-10-14):** RP2350B package (QFN-80, 48 GPIO, ADC on GPIO 40-47) with 2 MB flash inside; only US$0.10 more than the A package; LCSC C39843328 in stock (~3,240, ~US$1.35 at volume, checked 2026-10-14; India: Hubtronics ~INR 130, stock varies). Gives encoders on all 4 motors and an expansion header (6 GPIO + 4 ADC). Supersedes RP2350A/RP2354A below.
- **LLD drafted (2026-10-14):** `docs/lld/base-board-lld.md` - RP2354A (A4, 2 MB internal flash; fallback RP2350A0A4 + W25Q16JV), MCU core copied from the Raspberry Pi reference design, final pin map (5 V signals only on GPIO 0-25 with 1 kOhm series; ADC pins 26-29 not 5 V-tolerant), P-FET reverse protection doubling as the main switch, TPS563201-class 5 V 3 A buck with a separate servo trace, diode-OR USB/5 V into the 3.3 V LDO, TB6612FNG IN/IN drive, SN74AHCT2G125 for WS2812 + ultrasonic trig, SN74LVC244 status LEDs, 4-layer layout rules, BOM estimate ~US$9-13. Part numbers are MPNs; LCSC numbers checked when the schematic is drawn. KiCad not installed yet (owner to approve install).
- **Spec written (2026-10-14):** `docs/superpowers/specs/2026-10-14-06-base-board-design.md` (draft for review) - next: LLD (exact parts, final pin map, layout rules), then a plan. The current tank development (Mega + shield + ESP32 bridge) stays unchanged.

## 41. IR turning: open bug and plan (PARKED 2026-10-14 until the ESP32 USB-C is repaired)
- **Bug (owner, floor):** a single tap of the IR left/right key turns the tank for several seconds, in steps. Expected: at most ~0.35 s (the release window), then a ~0.25 s ramp-down. Reading the code (IR release → `stopTeleop()`, heading controller resets at v = w = 0) does not explain it — must be measured.
- **Plan:** (1) recording with the Mega on USB, tank on the floor: one tap of ◀, then ◀ held 2 s; log PWM, yaw rate, flags and events every 50 ms → find what keeps the motors on; (2) fix with a host test; (3) proposed: **tap = nudge** — no NEC repeat within ~130 ms → a short ~0.15 s move, repeats → continuous hold (owner to confirm); (4) then tune the turn-rate gains (start kp 0.3, ki 0.2, kd 0) to remove the stepping.

## 42. Own tank chassis: acrylic plates + sourced running gear (PLAN, owner 2026-10-14)
- **Chassis:** replace ELEGOO's folded aluminium plates with **laser-cut acrylic**, flat plates joined by brackets (cheaper). Guidance: 3 mm acrylic for top/cover plates, 5 mm for side plates; holes ≥ 2x their diameter from edges; metal bushings or shoulder screws at suspension pivots and sprocket axles; metal or printed L-brackets + brass standoffs. Option: side plates in FR4 (JLCPCB "PCB" plate) or 1.5-2 mm aluminium, acrylic only for the deck (hybrid).
- **Source from China (running gear only, per tank, from the kit photo):** 2 pinned link track belts with tread, 2 drive sprockets (TT shaft), 8 road wheels, ~8 suspension swing arms, ~6 tension springs, wheel caps, shoulder screws/axles, 2 TT gear motors (encoder TT motors preferred). Supplier research: `docs/research/2026-10-14-track-suppliers-china.md` (no exact Conqueror-style set found; closest: Sinoning parts; RFQ text in EN/ZH inside). Before asking for quotes: measure the owner's tank (shoe width and pitch, sprocket teeth and diameter, road wheel diameter, axle spacing, motor shaft).
- **Recordings 2026-10-14 (Mega on USB, floor, IR):** every single tap (forward, reverse, spin left/right) drove **0.45-0.55 s** and stopped — the multi-second turn did not reproduce. Owner: a single signal should not drive for half a second. Cause: the 350 ms hold window (needed to survive lost repeats while a key is held) also applied to taps.
- **Decided (owner, 2026-10-14): tap = nudge.** NEC repeats arrive ~108 ms apart (the first decoded ~50 ms after the code). Drive keys: if **no repeat follows the code within 150 ms**, it was a tap → stop after a **0.15 s nudge** (at the current IR speed; a spin of roughly 10-15°). Once a repeat arrives it is a hold → continuous, released 350 ms after the last repeat (unchanged). Camera jog and `#`/`*` keys keep their current behaviour.
- **How ELEGOO's own Conqueror code does it** (`ELEGOO-Conqueror-Robot-Tank-Kit/ConquerorCar_Driver_20210104`, `ApplicationFunctionSet_IRrecv` + `DeviceDriverSet_IRrecv_Get`): every decoded IR signal, **including NEC repeats**, refreshes a timestamp; a direction key keeps the last direction until **300 ms pass with no IR signal**, then stops. **No tap/hold distinction** — a tap drives ~0.3 s. **Left/Right are open-loop spins** (one track forward, the other back, at the IR speed; **no gyro**); Forward/Backward use a gyro heading-hold loop. Default IR speed **250 PWM** (near full); speed keys step **±5 PWM** (50-255), so one press is barely noticeable.
- **Decided (owner, 2026-10-14): IR is "plain fun".** IR left/right = **open-loop spin** (one track forward, one back, at the IR speed — no gyro rate loop, like ELEGOO); IR forward/back keep gyro heading hold. **Default IR speed 80 %**, keys 6/4 step **10 %** (range 20-100 %). The gyro turn-rate tuning (option B) is done later for phone and AI turns; the AI never drives through IR.

- **Decided (owner, 2026-10-14): the gyro is only for driving straight.** "Gyro should be used only for forward or backward so tank stay straight for PID control." So:
  - **Hand driving (IR, phone, bench):** `HeadingController::teleop` holds the heading with the gyro only while there is **no steering input** (w = 0). Any steering (|w| ≥ 1 %) is passed through **open-loop** (no rate loop), and the held heading is reset. After steering stops, the controller outputs no correction while the tank is still turning faster than the capture rate (no "rate braking" back toward zero), then captures the new heading and holds it.
  - **TURN by angle (Tank Logo / AI)** — later step: open-loop power; the gyro only **measures** the angle turned and the turn slows down near the target.
  - **DRIVE_RATE** (closed-loop turn rate) is kept for the AI and tuned later (option B).
  - Host tests: `test_teleop_steering_is_open_loop`, `test_teleop_no_rate_braking_after_steering`; IR tests updated for the 80 % default.
- **Floor test 2026-10-14 (firmware `94b82a5`, IR, USB attached):** one held ▲ run of 7.1 s, about **3 m (118 in, owner)**, at 80 %. The heading stayed within **−4.3…0°** and ended at **−2.4°**; the owner saw it drift and correct, fairly straight. To hold the heading the gyro gave the right track a steady **~12 % more** than the left (command about 73 / 85 %, PWM about 210 / 232): the tank pulls to one side by itself (track drag, motor difference or the USB cable). A fixed left/right motor trim could take this load off the gyro later — not needed now. Taps (earlier recording): 0.14-0.26 s, about 7° each; held spins had no braking back after release. Encoder counts read 0 with `encoders_enabled` off. The battery reported **BATT_LOW at 7.14 V** during the run.
- **Branch review fix (2026-10-14):** after steering ends, the controller waits for the rotation to fall below 5 °/s before it captures the heading. A track that pulls the tank round (the floor test needed ~12 % difference to hold straight, which is roughly 15-20 °/s uncorrected) could keep the rate above 5 °/s and **keep heading hold off for good** — mainly after a phone curve while still driving. Fix: wait at most **0.3 s** (`HeadingController::kSettleS`), then capture anyway (`test_teleop_captures_after_settle_time_despite_pull`). The child-play / envelope speed and acceleration caps are applied after the mixer on track %, so open-loop steering still respects them; `turn_rate_max` no longer caps hand turns (only primitives and DRIVE_RATE).
- **How ELEGOO's code uses the gyro** (`ApplicationFunctionSet_ConquerorCarLinearMotionControl`, `MPU6050_getdata`): only the **Z gyro** (±250 °/s), bias averaged from 100 samples at boot, integrated to a yaw angle with a small dead zone. **Only Forward/Backward** use it: the yaw is stored when the direction changes (and again whenever the tank is lifted), then each track gets `speed ± Kp × (yaw − stored yaw)`, clamped to 10…upper limit. It is **P-only**: Kp = 10 PWM/° with upper limit 255 for the remote/joystick, Kp = 2 with limit 180 for obstacle-avoid, follow and app commands. No rate loop, no I or D term (so a steady pull leaves a small steady angle error, about 1.5° for a 30-PWM pull at Kp 10), no encoders. Left/Right and the diagonal moves (one track at half speed) are open-loop. Line tracking does not use it. Our controller does the same job as a cascade: heading PID → turn-rate target → rate PID, which removes the steady error and damps the wobble; encoders (per-track speed loops) are the next improvement.
- **Gain comparison with ELEGOO (2026-10-14).** Converted to the same unit, PWM per track per degree of heading error (PWM here = 90 + % × 1.65 while moving, deadband keep 90):

  | | ELEGOO (IR / joystick) | ELEGOO (auto modes) | Ours (defaults) |
  |---|---|---|---|
  | Loop | P on heading only | P only | heading PID (Kp 3.0 °/s per °, Kd 0.1) → turn-rate PID (Kp 1.2, Ki 0.4, Kd 0.02 %/(°/s)) + feed-forward |
  | P: PWM per track per ° | **±10** | ±2 | **±4.5 at 80 % speed**, ±8.7 when slow (the mixer's turn authority falls to 52 % at 80 % speed and also scales the gyro's correction) |
  | Damping (per °/s of rotation) | none | none | ≈ ±1.0 PWM |
  | Integral (removes steady error) | none | none | ≈ ±1.0 PWM per °·s (slow) |
  | Steady error for the 12 % pull seen on the floor | ≈ 1.0° | ≈ 5° | ≈ 2.2° from P alone (2.4° measured), shrinking slowly through the integral |

  So ELEGOO's remote mode is about 2× stiffer than ours at 80 % speed, with no damping. Ours is softer but damped, with an integral. The ~2-4° wobble seen on the floor fits the softer P. Tuning options (not decided): raise `pid_heading_kp` (3.0 → ~5), add a little `pid_heading_ki` (~0.5), and/or stop the mixer's turn authority from scaling the gyro's own correction (only the driver's steering should be scaled).
- **Decided (owner, 2026-10-14): stiffer heading hold, ELEGOO's ±10 PWM/° as the target** (branch `heading-stiffness`). ELEGOO's remote gain was tuned on this tank, so it shows how hard the hold can push without going unstable; we keep our full gyro (no 4 °/s dead zone), damping and integral. Their gyro drops any sample below 0.05° (read every ~12 ms), so rotation below about **4 °/s is never corrected** — the "1°" estimate above is optimistic; slow curving goes unseen.
  1. **Code:** `mix(v, w_steer, w_ctrl, …)` — the turn authority (less turning at speed, `turn_rate_full_speed`) now reduces only the **driver's open-loop steering**. Closed-loop outputs (heading hold, DRIVE_RATE's rate loop, MOVE/TURN primitives) go in unscaled. Heading hold at 80 % speed: ±4.5 → **±8.7 PWM per track per °**. Test `test_mixer_gyro_correction_not_scaled_by_authority`.
  2. **Setting only:** `pid_heading_kp` 3.0 → **3.5** on the owner's Mega (saved; the default in `settings.yaml` stays 3.0 until the floor test). Gives about **±10 PWM/°**, like ELEGOO.
  3. **Floor test** next: the same ~3 m held ▲ run, recorded, compared with the 2.4° / wobble of the earlier run. If it shakes, raise `pid_heading_kd`.
- **Floor test 2026-10-14, stiffer hold (firmware `f4f98f8`, `pid_heading_kp` 3.5, IR ▲ held, USB attached):** 6.9 s run at 80 %. Heading range **−0.7…+0.6°**, end **−0.6°** (before: −4.3…0°, end −2.4°). Turn-rate wobble rms **2.7 °/s**, max 8 °/s (before: 4.3 rms, max 16). The left/right command difference swung −16…+23 % around an average of +5.7 %. No BATT_LOW (battery charged). Better than ELEGOO's estimated ~1° and with no slow unseen curve.
- **Left drift, cause found (2026-10-14).** Owner, cable-free tape run with the stiffer hold: smooth, but **~5 cm left over 3 m (118 in)**. The gyro read **−0.20 °/s at rest** in every recording (telemetry `yaw_rate` is bias-corrected gz, not clamped). While moving the yaw integrator counts it, so heading hold really turns the tank left at ~0.2 °/s: ~1.4° over a 7 s run ≈ 4 cm sideways over 3 m — matches. The owner's Mega had **never been bias-calibrated** (`gyro_bias_x/y/z`, `level_pitch/roll` all 0). Ran `uno_smoke bias` (3 s still sample, saved): gyro_bias_x −86, y −57, **z −17 raw** (65.5 LSB per °/s), level_pitch 276 (2.76°), level_roll 26. Afterwards the rate at rest reads **0.0 °/s**. ELEGOO measures the gyro zero at every power-on; ours used a stored value. **Proposed permanent fix (not decided yet):** re-estimate the gyro zero automatically whenever the tank is stationary, so temperature drift is followed without a calibration step.
- **After bias calibration (owner tape runs, no cable, 2026-10-14):** left drift over ~3 m down from **~10 cm or more** (owner's revised estimate of the earlier runs) to **~4 cm** (≈ 0.75° path angle, about 1.3 % of the distance). Remaining causes, in order of likelihood: lining up at the start (1° of aim ≈ 5 cm over 3 m), sideways track slip (the gyro cannot see it), residual gyro zero (≤ 0.06 °/s ≈ 1 cm).
- **Decided (owner, 2026-10-14):** `pid_heading_kp` default **3.5** (was 3.0) in `protocol/defs/settings.yaml`; `heading-stiffness` merged into main. Next: automatic gyro zero (design first, Mega LLD).
- **Automatic gyro zero — design proposed (2026-10-14):** Mega LLD §11.8 (branch `gyro-auto-zero`). When the tank has stood still 1 s, measure the gyro over 2 s windows; two calm windows that agree within 0.3 °/s set the new zero (RAM only, starts from the stored calibration). Waiting for owner approval before code.
- **Automatic gyro zero built and bench-checked (2026-10-14, firmware `7533705`, branch `gyro-auto-zero`):** 7 host tests (179/179 total). On the owner's Mega, standing still, the stored `gyro_bias_z` was set to 0 (not saved): the rest reading went to **−0.22 °/s**, and **~5 s later read 0.00 °/s** (1 s settle + two 2 s windows) and stayed there. After a reset the saved calibration (−17) is back. Next: owner floor tape run.
- **ESP32 back (2026-10-14):** USB-C socket repaired. On USB it ran the old `985bb2b`, joined home WiFi (192.168.31.167, −45 dBm), camera capturing. Flashed the tank-power fix build **`bb7a619`** (branch `plan-c-exec`) over USB: app at 0x10000, `boot_app0.bin` at 0xE000 (resets otadata, the board had last booted app1 after the OTA test), source image at 0x610000 (no `secrets.h` inside). `/info` after boot: fw `bb7a619`, `matches_fw` true, boot diagnostics working (count 1, reason `usb`). Next: the tank-power test (S1 to ESP32, USB unplugged) and the rest of the Plan C hardware checklist.
- **ESP32 on tank power — brownout confirmed (2026-10-14, fw `bb7a619`).** Tank on, ESP32 on the shield, USB unplugged: only the red LED, not on WiFi. With USB then plugged into the Linux machine it started at once (WiFi up, **Mega link synced at 250000 baud**, Mega commit `75337057`, 0 CRC errors). Boot history in `/info`: `usb` (the flash) → **`power_on`** (tank power: our code started) → **`brownout`** (the following start was a brownout reset). So on tank power the ESP32 starts, then the supply dips below the brownout threshold during start-up (WiFi and/or camera current), and it cannot come back until USB adds power. Hardware rework is ruled out (§30), so the fix has to cut start-up current in firmware, or the cause is the shield's 5 V supply.
- **ELEGOO firmware on tank power, and the power cause (2026-10-14).** Option A: flashed the board's original image back (from `~/tank-dump`, never printed or copied). On USB it joined home WiFi and talked to the Mega; on tank power alone it **also did not come online**. With the ESP32 attached, the tank's WS2812 went **solid red** (critical self-test lock: IMU or battery check) and driving stopped; without the ESP32 it breathes amber (no ESP32) and IR works. Owner: with **USB plugged into the Mega** (extra 5 V) everything works. Conclusion: the tank's own supply (battery / shield 5 V) cannot carry the ESP32-S3 board; not our firmware. **Open item:** fix the tank power (battery health, shield 5 V regulator) — workaround for now: Mega on USB.
- **Our bridge restored and OTA verified (2026-10-14).** Full USB flash of `bb7a619` (bootloader 0x0, partitions 0x8000, otadata 0xE000, app 0x10000, source 0x610000) after erasing NVS 0x9000-0xDFFF (ELEGOO's saved data). Then `scripts/ota.sh` over WiFi only (app + source archive, password from `secrets.h`, never printed, log checked and deleted): both uploads OK; `/info` shows the new build time, boot history `usb → software → software`, `matches_fw` true. From now on the ESP32 is updated over WiFi; USB only for recovery.
- **Shield power path (from ELEGOO's `ConquerorCar-Shield-V1.0.pdf`, 2026-10-14):** the shield has **no 5 V regulator of its own**. Battery → POWER_IN → diode D1 (SS34, ~0.4 V drop) → `Vin` → Arduino `Vin` pin and the motor driver. **Every 5 V load** — ESP32 camera board (header P8), both servos, ultrasonic, IR, MPU6050, WS2812, buzzer — runs from the **Arduino board's own 5 V linear regulator** (a 1 A SOT-223 type on Uno/Mega boards; clones often use AMS1117). So swapping Uno → Mega swapped the regulator that feeds the ESP32. The Mega itself also draws a little more (~30-50 mA more than an Uno). A linear regulator also needs ~1.1-1.3 V headroom: battery − 0.4 V (diode) must stay above ~6.4 V under load, so a low pack (7.14 V seen earlier, sagging further under load) can drop the 5 V rail. Owner reports the ESP32 worked on the Uno. Test order: (1) fully charged pack; (2) multimeter on 5 V with/without the ESP32; (3) then decide (regulator vs. battery).
- **Mega board is the power problem — A/B test (2026-10-14).** Same shield, same ESP32-S3 (fw `bb7a619`), battery only, no USB anywhere:
  - **Mega:** ESP32 never comes online; its boot history shows brownout after brownout. With USB on the Mega only (battery off) it took 7 brownouts before one start survived.
  - **Uno (the kit's original):** ESP32 started **first time** (`power_on`, no brownout), still up 52 s later.
  - The ESP32's own USB-C always starts it first time.
  So the **Mega board's 5 V route** (regulator on battery, USB fuse/switch on USB, plus the Mega's own draw) cannot carry the ESP32-S3's start-up surge; the Uno's can. Options to decide: a Mega board with a stronger 5 V route; power the ESP32 from its own USB-C (power bank, no rework); soften the ESP32 start-up in firmware (WiFi start delay, 160 MHz CPU, camera on demand); long term the base board's own 5 V supply.

## 43. Back to the ELEGOO Uno, Lite profile (DECIDED 2026-10-14, owner)
- **Why:** the Mega clone's 5 V route cannot start the ESP32-S3 camera board (A/B test above: Mega = brownout loop on battery, 7 brownouts on USB; ELEGOO Uno = clean start first time). Owner: "go with elegoo uno and lite profile"; the Mega (and the full feature set) waits for our own base board (spec 06).
- **Board:** the ELEGOO Uno R3 that came with the kit (ATmega328P, 32 KB flash, 2 KB RAM, 1 KB EEPROM). The owner's other Uno R3 (FM-radio project) has the same chip; not used.
- **Firmware:** one codebase, two profiles as planned in §29: **Lite** (Uno) and **Full** (Mega, kept building and tested). The Lite feature list, sizes and HELLO `caps` are designed next (Uno LLD section) and approved before any code.
- Full build today: ~61 KB code, 2.4 KB RAM; Uno budget (size gate): 30 KB flash, 1750 B static RAM.
- **Uno firmware approach (owner, 2026-10-14): option B** — a new, small Uno app in the same project, reusing only small proven pieces, whole-number maths, same protocol to the ESP32; the full firmware stays (tag `full-firmware-2026-10-14`, Mega env keeps building). Spec 02 Part D (proposed) lists the features; owner to approve before the Uno Lite LLD.
- **Spec 02 Part D approved (owner, 2026-10-14), incl. one beep, small settings set, no TURN.** Uno Lite LLD written: `docs/lld/uno-lite-lld.md` (PROPOSED) — reuse the integer safety/protocol/IR code as-is, new integer classes for gyro zero, yaw, heading hold, mixing, tilt/lift, range, encoders, settings; HELLO caps bits (protocol change); budget ≈ 24 KB / 1.24 KB estimated against 28 KB / 1.5 KB; extras by space: smooth camera first, then encoder balance.
- **Uno Lite LLD approved (owner, 2026-10-14); Plan E written** (`docs/superpowers/plans/2026-10-14-plan-e-uno-lite.md`, 16 tasks: build split + skeleton measurement with a stop rule, 9 integer domain classes with host tests, HELLO CapBit protocol change, Lite app routes/sensors/control/UI, extras by space, hardware bring-up). Plan rulings: gyro zero Z only; motor trim deferred (no protocol setting; gyro hold absorbs the pull); heading gains reuse ids 58/55; lift thresholds copied from the full LiftDetector; host tests grouped in five suites.
- **LED states back to the spec (owner, 2026-10-14):** "no ESP32" amber pulse and "battery low" orange pulse were too alike — both had drifted from spec 02. Now: no ESP32 link = **blue breathing**; battery low = **short amber flash (150 ms) every 2 s** over the normal colour (pattern differs, not just colour). Mega LLD §11.9.
- **Uno Lite follow-ups (owner, 2026-10-15):** (1) Uno IMU on a register-level **hardware TWI** driver instead of software I2C (A4/A5 are the Uno's TWI pins; no Wire buffers) — done, Uno LLD §11; (2) coding-standard tidy-up (functions ≤ 40 lines, tilt table in PROGMEM, VARIANT POINT) — done; Uno Lite now 24,462 B flash / 1,403 B RAM; (3) alarm/status sounds + phone SOUND/TONE/AUDIO_MODE/LED — **proposed** in spec 02 §D4 / Uno Lite LLD §12, waiting for approval.
- **Uno Lite sounds/LED decided (owner, 2026-10-15): option A** — reuse the full firmware's `Annunciator` + `SoundSequencer` + melody table (spec 02 §A9), not a new mapping; IR sounds as the full input task; **owner addition: a click on every new IR key press**; phone SOUND/TONE/AUDIO_MODE/LED if RAM allows (≤ 1,500 B). Spec 02 §D4, Uno Lite LLD §12.
- **Uno Lite second review → owner decisions (2026-10-15):** fix mute/quiet reporting, boot-button feedback, stack high-water `free_ram`, `err_i2c`, camera return cancel; add stall detection (shared `EncoderHealth`, encoders single-channel on A1/A2 — speed only, direction from the motor command), 500 ms joystick timeout, servo idle detach after 2 s. Spec 02 §D5, Uno Lite LLD §13.
- **Owner, 2026-10-15:** camera wake-up bug fix (IR jog 1/3/2/8 did nothing after the 2 s servo detach); **IR keys: speed on 1/3, camera pan on 4/6** (both firmwares); **USB power shown as the normal colour + white flash every 2 s** (USB = < 5 V only while no battery seen since boot, so a sagging pack still triggers battery critical); Uno flash budget raised to **30 KB**. Spec 02 §A8, §D6; Uno Lite LLD §14.
- **Field bug (owner, 2026-10-15): periodic error tone + red LED on USB.** Moving a servo on USB made the back-fed battery reading jump to 5.07 V, which latched "battery seen" (threshold 5.0 V); every later reading < 5 V was then battery critical (tone every 30 s) until a restart. The earlier "chirp every few seconds" on the old build was the same tone (old rule: USB 4.7 V = critical). Fix: USB < 5.5 V, pack seen ≥ 6.0 V (spec 02 §D6.3).
- **Owner, 2026-10-15 (Uno Lite on the tank):** camera jog (4/6, 2/8) works; camera picture upright and correct (vflip only); self-test battery lower limit stays 5.0 V (owner: fine as is); 3 m floor run on the Uno later.

## 44. Plan F tank check fixes (DECIDED 2026-10-15, owner)
First tank check of the Cockpit-lite page (tank on the stand): drive, deadman (0.32 s), STOP (ack 22 ms), heading (real rotation), LOCK, SOUNDS (after unmute), MOVES hidden — all good. Found: SNAP and a second video viewer hung while one viewer watched (port 81 served one request at a time), and a sound on a muted tank answered only "Busy". Owner decisions: **B** — each video viewer gets its own task so the server stays free; the page shows the tank's audio state with a **speaker chip** that toggles mute ↔ normal (`audio_mode`, quiet stays IR/hub). Details: bridge LLD R3.1, hub-ux Screen 7b, Plan F Tasks 6–8.
Results after the fixes (2026-10-15, bridge `b4316f7`, tank on the stand): SNAP from the page while it streams → 200, 15.8 KB JPEG; second viewer streams (76 frames in 6 s) alongside the first; third viewer not accepted (socket budget, as designed); 10 viewer open/close cycles → viewer count back to 0, free heap steady (~150 KB). Speaker chip: mute/unmute in 0.3 s (on the ack), sound while muted → "Tank is muted - tap 🔊", sound after unmute plays. Open: range bar with a hand in front of the sensor (owner).

## 45. Camera tilt direction (DECIDED 2026-10-15, owner option A)
The Cockpit-lite camera ring showed the camera looking up while it looked down: stick up and IR 2 moved the tilt servo to a larger angle, which on this tank points the camera **down**. Pan was correct. Fix in the Uno camera layer (Lite and full): "up" = smaller tilt angle; servo-degree commands, limits and telemetry unchanged; the page ring uses up = smaller angle. A `tilt_reverse` setting only if another mount appears. Defaults 30–110 then give 60° up / 20° down — to tune on the tank. Details: Uno Lite LLD §15.
Result (2026-10-15, Uno `83982b1` code, bridge `4bdaa77`): stick up → camera looks up (video shows the ceiling side), ring ↑36° with the dot above centre; pan unchanged (stick left = camera left, owner confirmed). Cockpit-lite checks all pass, including range (owner: "working perfectly").

## 46. Page display filters (DECIDED 2026-10-15, owner)
Range and turn rate jumped on the Cockpit-lite page. Owner chose all filters: range median-of-5 + closer-at-once, battery 10 s average, turn rate 0.3 s average with a 2 °/s dead band, vitals text 4×/s, latency mean of 5; heading unfiltered. Display only — the Uno's safety logic keeps raw values. Details: bridge LLD R3.3.

## 47. Stick steering by angle (DECIDED 2026-10-15, owner)
A 45° stick push spun the tank: the arcade mix ran the inner track backwards from ~35°. Owner: design from the user's expectation — push the stick where you want to go; spin only when fully sideways; IR keeps its fixed-key behaviour. One new rule for everyone (no source branch): on the pure axes it gives exactly the old tracks, so IR is unchanged by construction (pinned by a test). Spin speed is limited by the phone's MAX slider for now; a Uno spin cap only if the floor test needs it; gyro turn-rate control (same turn on every floor) is the next step. Details: spec 02 §A4.1, hub-ux "Drive stick", Uno Lite LLD §16.
Stand result (2026-10-15, Uno `567c54d`): stick 45° half push → tracks 24/68 (both forward), fully sideways → −50/50 spin, straight 50/50, reversing 45° → −24/−68 (both backwards). Open: IR ▲◀▶ feel (owner), floor test (arcs, spin speed at MAX 60 %).

## 48. RC two-hand controls on the phone page (DECIDED 2026-10-15, owner)
Optional mode next to the single stick: left vertical throttle, right horizontal steering (car-like: steering = curve at any speed, zero throttle + steering = spin), camera by dragging the video (game style, stays where left, double-tap = home). Page only — Uno, bridge and IR unchanged. Details: hub-ux "Drive controls — RC two-hand mode", bridge LLD R3.4.
Stand result (2026-10-15, bridge `2503830`, Uno `567c54d`, MAX 60 %, real touch input): throttle 70 % → 36/30 straight; + full steering right → 36/0 (pivot, same curve at any throttle); steering only → half 22/−22, full 60/−60 (spin); release → STOP sent 16 ms after lift, tracks 0 within 0.23 s; drag on the video right+up → pan 57.6°, tilt 81.9° (looks right and up) and stays after lift; double-tap → home 90/90. Note: once, with a second page tab open (3 WebSocket clients + 2 video streams), a test monitor connection stopped receiving telemetry for a few seconds; not seen again with one tab — watch for it in the Plan C multi-client checks.

## 49. Phone page polish (DECIDED 2026-10-15, owner)
Sticks ~40 % larger; battery / range bars full height along the screen edges (sticks moved in); speaker and mode chips left of STOP, mode chip is the only mode switch; gauge values on top of the bars (voltage / cm); icons from Lucide, built into the page (no internet needed). Details: bridge LLD R3.5–R3.7.

## 50. Code review of the session's changes (DONE 2026-10-15, owner: "fix all", RC option A)
Fresh review of 38 commits (page, bridge, Uno): 0 critical, 5 important (stream sockets not closed → frozen video; camera input not ended on owner change; RC throttle lift sped the tank up; camera lock lost after a Uno reboot; faulted range sensor shown as CLEAR), 24 minor. All important and most minor fixed with tests (bridge 110, Uno 252 host tests; page checked on a recorded fake connection); deferred items listed in bridge LLD R3.10.

## 51. Plan C hardware checks (2026-10-15, bridge `9e9987d`, Uno `567c54d`, owner present)
| # | Check | Result |
|---|---|---|
| 1 | Phone lock while driving | n/a (owner: cannot lock while driving); the same page-hidden path is check 10, deadman behind it |
| 2 | Second phone while one drives | ✅ second phone's drive nacked NOT_OWNER (reported once); its STOP accepted (any viewer may stop, spec 01) |
| 3 | IR takeover | ✅ IR owner at once, phone NOT_OWNER; hand-back ~3.3 s after the last key, the holding phone resumes |
| 4 | Uno link cut (S1 to upload) | ✅ page "Tank body offline" (owner saw it); recovers by itself |
| 5 | Third video viewer | ✅ two viewers stream, the third is not served (2 stream sockets) |
| 6 | Bench TCP + second bench | ✅ heartbeating bench owns the link; second bench closed in 0.01 s, `/info` tcp_refused 1 "bench connected"; hand-back to web on close (a bench that sends nothing is dropped after 1 s, as designed) |
| 7 | OTA while driving | ✅ not accepted while driving (espota kept retrying); went through after the STOP |
| 8 | Bench killed mid-drive (RST, no STOP) | ✅ bridge noticed in 17 ms, injected STOP: tracks 0/0 within 95 ms |
| 9 | Source download while driving | ⚠️ download correct (954,502 B, 455 files, 1.8 s) but driving disturbed: the download and the page's WebSocket share the port-80 server, drives were delayed → 3 deadman stops (safe) and one RATE_LIMITED burst. Fixed the same day (owner): moved to port 81, port 80 redirects (bridge LLD R3.11); re-test below |
| 10 | Phone sleeps while driving (power button) | ✅ tank stopped (owner observed) |
| 9b | Source download while driving, after R3.11 (bridge `ad9e6c9`) | ✅ two downloads via the port-80 link (302 → :81), 956,548 B each, valid; driving: no deadman stop, no RATE_LIMITED |
| 11 | Wrong WiFi password → fallback AP | not run on the tank (owner, 2026-10-15: router cannot be switched off); covered by the WifiPolicy host tests (30 s → AP, home retry only without clients) |
Notes: during check 7 the tank drove into a wall on the floor and the owner lifted it; the logged −100/100 track targets were heading hold reacting to the lift (targets are before the lift cut-off). Review in the floor tests whether the heading-hold correction should be capped to the driver's speed.

## 52. Floor tests on the Uno Lite (2026-10-15, Uno `567c54d`, bridge `ad9e6c9`, owner placing / measuring)
| # | Test | Result |
|---|---|---|
| 12 | Straight line (Plan E Task 16), MAX/v 60 % | Run 1 (owner on the phone): 19 cm over 3.35 m, two obstacle-brake slow-downs mid-run; runs 2–3 (scripted v=60, w=0): **0 cm over ~3.1 m**, ~2 cm over 1.2 m (stopped by the obstacle brake 16 cm from a wall). Gyro heading held within ±1° in every run; heading hold ran the right track ~3–4 % faster on average (this tank pulls right). Passed — run 1 treated as a one-off. |
| 13 | Stick steering feel (scripted left turns, MAX 60 %, two runs) | Angle rule works as designed (arcs keep both tracks forward, spin only sideways): 25° → 37/63 ≈ 10 °/s; 45° → ≈ 10 °/s (both runs slowed by the obstacle brake near a wall); 70° → 4/59 ≈ 28 °/s; spin −60/60 → **110–137 °/s**; fast 45° (71/71) → 35/97 ≈ 14–19 °/s. Response very uneven on the floor (skid-steer scrub: arcs gentle, spin fast). **Parked (owner, 2026-10-15)** until encoders give real speed / radius measurements: candidates A (pivot point 75° → ~60°, spin limit) and B (gyro turn-rate control). Also parked for design: near an obstacle the brake stops a 70° turn entirely — idea: remove only the forward part and keep the turn (forward stays fully blocked); safety feature, design and tests first. |

## 53. Encoder wiring on the Uno (DECIDED 2026-10-15, owner: mini breadboard)
FIT0458 encoder motors: the two motor wires go to the shield's **Left / Right** motor connectors as now; the encoder has four more wires — **5 V, GND, A, B**. The Uno (and a later Uno R4) uses **channel A only** (one pulse train per side; direction from the motor command); **B is not connected** (end insulated).

**Shield port P7** (white 5-pin socket labelled **LineTrack**; the line-tracking module is dropped) — pins **GND, 5V, A2, A1, A0** (owner read the labels). The buzzer already uses **A0 and GND**. Two encoders need 2× 5 V and 2× GND, so P7's power is shared through a **mini breadboard** (170-point, self-adhesive, on the chassis) used as a power strip — no soldering, no board rework (§30):

| From | To |
|---|---|
| P7 **GND** | breadboard − rail |
| P7 **5V** | breadboard + rail |
| buzzer leg 1 | P7 **A0** (passive buzzer: either way round) |
| buzzer leg 2 | − rail |
| left encoder **5V** / **GND** | + rail / − rail |
| left encoder **A** | P7 **A1** |
| right encoder **5V** / **GND** | + rail / − rail |
| right encoder **A** | P7 **A2** |
| both encoders **B** | not connected (insulate) |

Parts: 1 mini breadboard; 1 × 5-pin **JST-XH 2.54 mm to Dupont** cable for P7 (or female–male Dupont jumpers if the socket grips them firmly); a few male–male jumpers. Load: two Hall encoders ≈ 10–20 mA each — no concern for the 5 V rail.

Rules:
- **Never take encoder power from the spare motor connectors** next to Left / Right: they are motor-driver outputs (battery voltage, PWM-switched, polarity reversing — wired in parallel with Left / Right on this 4WD shield); they would destroy an encoder.
- **Check each encoder cable's colours** (DFRobot FIT0458 page / motor label) before connecting 5 V and GND; swapped polarity can kill the encoder.
- The shield's right-edge Vin / GND / +5V pins are short stubs — only usable if a female Dupont really grips them; the breadboard avoids them.
- **Mega** (full profile): unchanged — A/B straight into the Mega's **A8–A11**, power from the Mega's spare 5V/GND pins (§29).

Bring-up after fitting: battery off while wiring; then turn each wheel by hand and watch the live counts (`counts_per_rev` default 960 — confirmed by the datasheet, §56), set `encoders_enabled`, check stall detection on the stand, then the parked steering tuning (§52) with real speed / radius data.

## 54. Uno R4 Minima — discussed, not decided (2026-10-15)
Owner raised the Arduino **Uno R4 Minima** as the next body board (the ESP32 brownout was the Mega clone's 5 V route, the ELEGOO Uno is fine). Points noted: same Uno shape and 5 V I/O (the shield fits, §30 kept); 256 KB flash / 32 KB RAM → the **full firmware** instead of Lite; USB separate from D0/D1 → no S1 flipping for uploads; switching 5 V regulator (current capacity to verify). Cost: port the hardware layer (PWM, encoder interrupts — R4 interrupt pins for A1/A2 to check, IR timing, I2C, WS2812, EEPROM emulation, servos); domain code and host tests carry over. If pursued: power test first (R4 + shield + ESP32 + servos on the battery), then spec / LLD / plan. Owner: discussion only for now.
R4 Minima pinout findings (lastminuteengineers.com pinout page, read 2026-10-15): 5 V from VIN by an ISL854102 buck, ~1.2 A (6–24 V in) — better than the R3 linear regulator, still bench-test ESP32 + servos; `Serial1` on D0/D1 independent of USB (no S1 flipping); PWM D3/D5/D6/D9/D10/D11; **attachInterrupt mainly D2/D3** (both taken: mode button, STBY) → encoders on A1/A2 via **timer sampling** (as the Mega's IR), no rework; **8 mA per pin, 60 mA total** → the passive buzzer driven directly from A0 may exceed it — check the buzzer type, or a series resistor on the breadboard; A0 DAC / A1–A3 op-amp / D4–D5 CAN only when enabled (no conflict as plain pins); I2C on A4/A5 without on-board pull-ups (the MPU6050 module has its own); 3.3 V only 50–100 mA (unused); EEPROM size not stated (emulated in data flash — verify).

## 55. Where features live: Uno = body + reflexes, ESP32 = everything WiFi-driven (2026-10-15)
**Decided (owner):** the IR remote will **never** do turn-by-angle or other measured moves — IR stays plain driving (keys = direct drive). So nothing forces TURN / MOVE / DRIVE_RATE onto the Uno: every user of them (phone, hub, AI engine, Tank Logo) arrives over WiFi through the ESP32; without WiFi none of them exists, and a WiFi loss mid-move ends in the Uno's deadman stop.
**Approved (owner, 2026-10-15) — written into spec 02 Part E and spec 01 §4 / §6.1 / §8:** the split below, capability routing (forward to the body when its HELLO caps report the feature, else the ESP32 runs it), and TELEM_FAST at 50 Hz. TURN (spin) and DRIVE_RATE need only the gyro and come first; MOVE, arcs with radius and pose follow the encoders.
- **Stays on the Uno** (must work with no ESP32 / no WiFi): safety gate and motor output, deadman / link-loss, IR driving, fast sensors (gyro, encoders, ultrasonic), heading hold for straight driving, servo / LED / buzzer timing, alarm sounds and LEDs, the small settings set needed at power-on.
- **On the ESP32:** TURN by angle, MOVE by distance, DRIVE_RATE (closed loop on `TELEM_FAST` at 20 Hz, slowing near the target; expected ±2–3°, to be floor-tested); position tracking and return-home; calibration routines and surface profiles (push results to the Uno); Tank Logo interpreter; full settings store / profiles / backup; fun melodies and LED shows sent as custom tones (alarm / status sounds stay on the Uno); telemetry recording.
- Why: the Uno is at 28.9 / 30.7 KB flash and 21 B spare RAM; moving these keeps the Uno Lite viable as the body board (the Uno R4 Minima becomes optional, §54).
- Open: the bench tool's TCP passthrough relays raw frames — measured moves from the bench would go through the ESP32 service instead (design detail).

## 56. Encoder motor datasheet facts (DFRobot FIT0458, recorded 2026-10-16)
Our motors: **DFRobot "TT Geared Motor with Encoder (6V 160RPM 120:1 L Shape)"**, sold also as "Micro DC Motor with Encoder-SJ02", SKU **FIT0458**. Datasheet saved in the repo: [`docs/hardware/datasheets/dfrobot-fit0458-tt-encoder-motor.pdf`](hardware/datasheets/dfrobot-fit0458-tt-encoder-motor.pdf) (source: DFRobot via DigiKey, https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/5786/FIT0458.pdf; product page https://www.dfrobot.com/product-1458.html).

| Item | Value |
|---|---|
| Gear ratio | 120:1 |
| No-load speed | 160 rpm @ 6 V, 60 rpm @ 3 V (output shaft) |
| No-load current | 0.17 A @ 6 V, 0.14 A @ 3 V |
| **Stall current** | **2.8 A** (max) |
| Stall / rated torque | 0.8 / 0.2 kgf·cm |
| Motor voltage | 3–7.5 V (rated 6 V) |
| Encoder | Hall, **quadrature A/B**, **8 pulses per motor-shaft turn on one channel → 960 per output-shaft turn** |
| Encoder voltage | **4.5–7.5 V** (Uno 5 V is fine; not 3.3 V) |
| Connectors | **2-pin motor plug** (+, −) and a separate **4-pin encoder plug** (A, B, GND, VCC — labels printed on the encoder board; go by the labels, colours vary) |
| Weight | 50 g |

Counts per output (wheel) turn by counting method:

| Counting | Per motor turn | Per wheel turn |
|---|---|---|
| A rising edges only (**Uno Lite `EncoderLite`, default `counts_per_rev` = 960**) | 8 | **960** |
| A both edges | 16 | 1920 |
| A + B all edges (quadrature ×4) | 32 | 3840 |

- **Bench check (2026-10-16, spare Uno, throw-away sketch):** turning the motor shaft one turn by hand gave 29 and 34 quadrature counts (datasheet: 32) — consistent; direction decoded correctly; no counts at rest.
- **Pulse rate at the rated 160 rpm:** 960 × 160 / 60 ≈ **2 560 rising edges / s per motor** (≈ 5 100 / s both motors) — light load for the Uno's pin-change interrupt.
- **Driver current:** 2.8 A stall is above the TB6612FNG's 1.2 A continuous (3.2 A peak) per channel → the Uno's stall detection must cut power quickly (it is designed to); never hold a stalled track.
- The datasheet's example wiring uses D2/D3 interrupts and an L298; on the tank we keep §53 (A only on A1 / A2 via P7).

## 57. WiFi lost to the camera — camera off for now; fallback AP after 10 s (owner, 2026-10-16)
- **Symptom (from 2026-10-15):** bridge on the home WiFi with a strong signal (−35…−53 dBm) but 90–100 % ping loss, 1–2 s delays, phones dropped after 3–7 s; the same on the bridge's own AP with the phone next to it, on battery and on USB, with the unchanged main firmware that had passed the Plan C checks. The Uno link was fine (the blue LED = not synced only because no phone stayed connected).
- **Bisected over USB** (throw-away sketches, bridge LLD R4.14): WiFi alone clean (19.5 and 15 dBm); **camera running → bad on most boots** in every mode (QVGA–SVGA, 10/20 MHz, PSRAM/DRAM, continuous or on request); bridge with the camera not started → clean on every boot. Bridge-feature leads (Uno link task, TCP poll, link tick) did not hold up across boots. Conclusion: an electrical / RF coupling problem of the camera hardware; reseating the ribbon did not help.
- **Decisions (owner):** run **without the camera** until the module is replaced — the camera returns with the AI work (`board::kCameraEnabled = false`, LLD R4.15); camera XCLK **10 MHz** kept for when it returns (R4.14); the bridge opens its own network `conqueror-bridge` **10 s** (was 30 s) after the home WiFi is unreachable (R4.13). With the camera off: 45/45 pings at ~9–23 ms, page in 0.05 s, also after a restart.
- **AP password** changed by the owner to an easy one in the local `secrets.h` (never committed).

## 58. Plan H Task 7 — floor results, marble (2026-10-16)
Firmware: Uno and bridge `6cdf7c3` / bridge `ddfc17f` (branch `small-fixes` on `plan-h-motion`), camera off.
- **Stand checks:** body frames **51.0/s** (50 TELEM_FAST + 1 slow), `tf` to the page 10.1/s; a second (non-owner) client's `turn` and `drive_rate` refused `NOT_OWNER`; bad CRC 0. IR-mid-turn check not run separately (IR ownership is covered by host tests and the earlier IR checks).
- **First floor run found hunting:** `turn by 90` reached 90° in 1.1 s, then shook left-right until the 7.5 s timeout (momentary gyro ±55 °/s on target; phantom latency corrections). Fixed the same day — heading-only window, w = 0 inside, done on the heading's average rate (bridge LLD R4.16).
- **10 × 90° at speed 40 (5 left, 5 right), gyro-measured:** all **done** in 1.5–2.0 s; errors −1.96, −1.69, +0.44, +1.99, −1.94 (left), −0.57, +0.28, +0.33, −0.07, −0.88 (right); **mean |error| 1.0°, max 2.0°**; net heading after the 10 turns −4.1°; **`turn to` the start heading: 1.07° off** in 0.93 s. Owner: smooth turns, **a single small jerk at the end** of each (w drops from w_min 12 % to 0, then STOP) — accepted for now; easing w to 0 over 100–200 ms is a later tuning option (with encoders, to measure the coasting). Tape check of the final heading not recorded.
- **drive_rate, v = 0:** 30 °/s → average 29.5 (gyro) / 30.2 °/s (heading) but **uneven (gyro sd ±25 °/s, stick-slip on marble at ~4–5 % command)**; 60 °/s → 61.8 / 61.7 °/s, **smooth (sd ±3.3)**; 90 % of the rate within 0.1–0.2 s; ended by STOP, no diverged / stalled. Open: a minimum drive_rate command (like the turn's 12 %) or accept rough low rates on slippery floors. No rug available: second surface not tested.
- **Extra checks (owner asked, not in the plan's list):** slow `turn by 90` at speed 12 → **89.03°, done in 2.20 s**; `drive_rate` straight forward (v 30, rate 0, 3 s) → heading drift 2.6° (avg 1.2 °/s); **straight reverse (v −30, rate 0) → drift −1.7°, peak 6 °/s: no runaway — the review C1 fix (DRIVE src AUTO) confirmed on the tank**; arc (v 30, +20 °/s, 3 s) → **32.9° instead of ~60° (avg 10.4 °/s, about half)**.
- **Arc shortfall — cause and decision:** the bridge's AUTO frames go through the Uno's driver steering mix (stick by angle, spec 02 §A4.1), which at forward speed turns much less for a small w than the plain mix (L = v − w, R = v + w); the rate loop's gains (tuned for spins) do not make up the difference in 3 s. Spins and straight lines are unaffected. **Owner decision: fix it in the arcs / encoders plan** (planned fix: the Uno uses the plain mix for src AUTO, the angle mix stays for humans; then re-test arcs). Nothing uses drive_rate arcs yet.
- `motion_config.h` unchanged by the floor test except the R4.16 settling constants.

## 59. Encoder bring-up on the Uno Lite (2026-10-16)
- **Wiring as §53** (P7 LineTrack, breadboard power rails, A only). The new FIT0458 motors' power leads come as a 2-pin cable with the **small motor-side plug and a bare end**; the shield takes **JST-XH 2.54 mm 2-pin**. Owner: soldered each new lead to the XH-plug end of an old motor cable (red–red, black–black, heat-shrink); crimped XH housings + XH terminals (not Dupont) later — the Dupont crimper (SN-28B type) normally does XH too.
- **No test sketch needed:** the Lite firmware counts both encoders (A1 / A2, rising edges) and sends them in `TELEM_FAST` `enc` at 50 Hz whatever `encoders_enabled` says (that setting only switches on stall / fault use). Checked over the Uno USB (S1 in the upload position, ESP32, ultrasonic and servos unplugged during the rebuild — the US_FAULT / self-test 10 at that time are expected).
- **Motors unmounted, 30 % for 2 s:** forward L 2393 / R 2337 counts, backward 2331 / 2636, spin 2492 / 2554 → **~1200 counts/s ≈ 75 rpm at the wheel (960 counts/rev, §56), no load**; steady, no dropouts.
- **Side check by a curve (v 30, w 30 → L 15 %, R 42 %):** first L 4029 > R 3063 → **encoder A wires were swapped**; after swapping A1 ↔ A2 at P7: **L 3002 < R 4081 — correct**. Rule: the motor in the shield's **left** socket has its encoder A on **A1**, right on **A2** (label both cables per side before mounting). Direction comes from the command (single channel).
- Next (stand, motors mounted): `encoders_enabled` = 1, stall detection check, then the parked steering tuning (§52) and the MOVE / arcs plan.

## 60. FIT0458 motors reversed in firmware (owner, 2026-10-17)
After mounting the encoder motors, every command ran backwards (forward → backward, spin left → spin right): the new motors' power leads have the opposite polarity to the old motors, and the motor-end plug cannot be re-pinned. Symptom on the stand: the heading hold's corrections turned the tank the wrong way and grew to a **full-power spin (−100 / +100, 111 °/s)** within ~0.6 s — stopped by the test script's safety stop.
- **Owner decision: fix it in software,** not by re-soldering: `board_config.h` `kDirLeftForwardHigh = kDirRightForwardHigh = false` (was `true`, §39 probe). Only the direction-pin level flips in `motor_uno.cpp`; safety, deadman, heading hold and all domain code are unchanged. Applies to Lite and full builds (same shield, same motors).
- If the motors are ever re-wired (e.g. when crimping the XH plugs with the pins swapped), flip both back to `true`.
- Check after any motor or wiring change: a gentle forward and a spin-left on the stand must move the right way **before** any closed-loop test.
- **Gap found (to decide):** a reversed motor turns the Uno's heading hold into positive feedback with nothing to stop it. Proposed guard: heading error growing while the correction is saturated → stop + fault event.

## 61. After the encoder bring-up: encoder plan next, gyro tuning after it (owner, 2026-10-17)
- **Straight driving drifts** with the new motors (owner's IR straight run). Today the encoders only count — straight driving is the gyro heading hold (tuned on the old motors, §52); the settings were also running on defaults (self-test bit 3) until the 2026-10-17 save, gyro zero offset included. **Owner: leave the gyro for now; do the encoder plan first, then fine-tune the gyro.**
- **Stall detection with these encoders (found 2026-10-17):** `EncoderHealth` needs *no edge at all* for 500 ms on a driven side; at 960 counts / wheel turn a hand-held track still creeps, so no stall is declared; on a stand a stopped side with the other moving and no yaw is classified as an *encoder fault*. → **Into the encoder plan:** a speed-based stall (measured speed < ~15 % of the speed expected for the applied power for 0.5 s).
- **Also into the encoder plan:** encoder straight driving (track-speed matching), MOVE by distance and arcs, the plain mix for src AUTO (arc shortfall, §58), the minimum drive_rate command on slippery floors (§58), and a **heading-hold runaway guard** (error growing while the correction is saturated → stop + fault, §60).
- **Debug metrics over WiFi** requested by the owner (no USB for floor tests) — design proposed (bridge-only opt-in `td` / `tds` stream + a recorder tool), awaiting approval.

## 62. Encoder features approved (owner, 2026-10-17)
Spec 02 Part F approved with: straight driving by **encoders plus gyro** (encoder distance sync inner, gyro heading hold outer); on a stall **stop both tracks** until the driver commands again; heading-hold runaway guard **30° / 0.5 s**, no motion until STOP or a new command; **phase 1 = Uno** (F1.1 straight sync, F1.2 speed-based stall, F1.3 runaway guard, F1.4 plain mix for src AUTO) as one plan, **phase 2 = ESP32** (MOVE, arcs, minimum drive_rate command, calibration) as a second plan, phase 3 pose / go-to / gyro tuning. Next: Uno Lite LLD section for phase 1.

## 63. Plan I floor results so far (2026-10-17)
Uno `d4994e0f` / bridge `f9068cc`, encoders on, marble floor.
- **Straight, 40 % × 8 s (two runs, recorded over WiFi with `td_record.py`):** no events; heading change −0.8° / −0.4°; encoder counts L/R 12 028 / 12 174 (1.2 %) and 11 973 / 12 213 (2.0 %). Owner measured **160 cm (63 in) travelled, 2.5 cm (1 in) drift to the left** (≈ 0.9°, matching the gyro). → **~75.6 counts / cm, ~12.7 cm per wheel turn** (for phase 2 MOVE). The remaining drift is heading capture / gyro zero: **gyro tuning** (owner: after the encoder plan).
- **Held track, 25 %:** first run missed a stuck left track while the heading hold flipped its PWM → fixed (stall time adds up, LLD §19.6). Then right held: `stall` right, left held: `stall` left — **both tracks cut and held while the stick stayed on, STOP freed them**. A held track keeps slipping at ~40 % of its expected speed, so at 15 % it took 3–4.5 s → **owner: threshold 30 %** (`kSlowPct`).
- The heading hold reverses the free track when one is held (pivoting the tank) until the stall trips — to revisit in gyro tuning.
- **Deferred (owner):** the unplugged-encoder fault test — needs the tank opened; do it next time it is open.
- **Arc (F1.4 plain mix), drive_rate v 30 / +20 °/s, 3 s:** heading 44.7° (was 32.9°), mean rate 14.6 °/s after 0.7 s (was 10.4) — better; the rest is the bridge rate loop's gains (tuned for spins) → **phase 2 arcs (F2.2)**.
- **Runaway guard (F1.3), wrong-direction test build, forward 25 %:** full-power spin; **`heading_fault` cut both tracks ~0.5 s after the error passed 30°** (≈ −100°, coasted to −110°). **Found:** after the STOP the motors ran again at −249 / 249 for 0.3 s — the ramps kept their full-spin targets while the latch cut the output and ramped down from them on release. **Fixed** (ramps + heading hold reset while latched); re-test: after the STOP the PWM stayed 0 / 0.
- **Normal build `7b11e8a7`** (correct directions, stall 30 %, ramp fix): forward 25 % × 10 s straight, PWM ~125 / 130, counts equal. The faster 30 % held-track trip was not re-run (owner: skip; covered by `test_stall_slipping_track_at_25_pct_trips`).

## 64. Encoders-only straight runs: the tank is mechanically biased right (owner experiment, 2026-10-17)
Question (owner): with both tracks turning exactly the same amount, does the tank go straight? Method: heading hold gains set to 0 in RAM over the bench port (`pid_heading_kp`, `pid_rate_kp`; restored to 3500 / 1200 after each run, never saved), only the encoder sync (§F1.1) active; 40 % × 8 s on marble, recorded with `td_record.py`, owner measured the drift.

| Run | Counts L / R | End heading | Heading through the run (25 / 50 / 75 / 100 %) | Drift (owner) |
|---|---|---|---|---|
| 1 | 12 521 / 12 555 | −6.4° | — | — |
| 2 | 12 500 / 12 556 | −3.9° | — | 1 cm right (168 cm) |
| 3 | 12 487 / 12 521 | −3.6° | +0.7 / +1.6 / −0.8 / −3.4 | ≈ same, right |
| 4 | 12 537 / 12 581 | −5.3° | −0.4 / −2.6 / −4.4 / −5.3 | 3 cm right |
| 5 | 12 585 / 12 602 | −6.8° | −0.1 / −2.2 / −4.8 / −6.8 | 5 cm right |
| gyro on (§63) | 12 028 / 12 174 | −0.8° / −0.4° | — | 2.5 cm **left** (160 cm) |

- Equal track turns (0.1–0.3 %) still curve **right 4–7° per ~1.6 m** — a repeatable mechanical bias (the left track carries the tank further per turn: tension, sprocket or grip). The encoders cannot see it (they count turns, not ground distance).
- The gyro hold keeps the heading within 1° but settles slightly off (2.5 cm left): zero offset / heading captured after the start.
- **Next (proposed):** a left/right **balance factor** learned from encoder runs (cancels the steady right bias at the source) plus **gyro tuning** (zero recalibration, capture the heading at the start, gain), each change checked with the tape and the recorder; target ≤ 1 cm per 1.6 m.

## 65. Plan J floor results — straight driving to ~1 cm over 2.5 m (2026-10-17)
Uno `38ee2380` (Plan J: heading-hold integral, `track_balance`, plus the Plan I review fixes), bridge `f9068cc`, encoders on, 40 % × 14 s on marble, recorded with `td_record.py`, reported with `straight_report.py`.
- **Gyro checks first:** stillness 65 s → heading 0.000° (by design: below 0.5 °/s is ignored while not driven, so resting drift is hidden); **hand rotations:** back to the line −0.24° / −0.29°, 90° → 90.56°, **180° → 179.97°**, full 360° → 357.85° (2.15° short: hand placement or ~0.6 % scale — matters for `turn by` / pose, not for straight lines). Right turns read negative (correct).
- **Measurement lesson:** placing the tank "on the tape" by eye varied by ~2.4° between placements (part placement, part slow nudges below the 0.5 °/s rest filter) — worth ±6 cm over 2.8 m, more than the drift being measured; the tape itself was straight (start / end 0.3–0.7°). Starting against a wall guided the tank (it rubbed and turned back) — fixed by a **5 cm spacer start**: parallel to the wall, spacers removed, gap measured at the end.
- **Integral only (balance 0):** heading within ±0.5…0.7° of the start, small steady right lean; spacer run ≈ 2 cm right over 248 cm (within line-up accuracy).
- **Integral + `track_balance` = +19 ‰** (from the count ratio of normal runs, `straight_report.py`): runs 1, 2, 4 — heading within **±0.2…0.3°** (one brief +1.1°), end vs start direction −0.06° / +0.01° / −0.11°, estimated sideways 0.0 / 0.2 / 0.2 cm; **owner: ~1 cm right, ~1 cm right, "almost straight"** over ~2.5 m. Run 3 hit the wall at the end (discarded). **Target met.**
- Saved on the Uno (SETTINGS_SAVE): `track_balance` 19, `encoders_enabled` 1, heading gains 3500 / 1200.
- Before / after (≈ 2.5–3 m): encoders only ~30 cm → gyro P-only 4 cm → **integral + balance ~1 cm**.
- **Held-track recheck (Plan I review fixes on the tank):** 25 %, right track held → `stall` right ~0.5 s after the hold (30 % threshold), both tracks 0 / 0 while the stick stayed on; a 0.6 s stream gap fired `deadman_stop` and the resumed stick still found 0 / 0 (review I1 fixed — before, the deadman released the latch); STOP released it.

## 66. Do the encoders help straight driving? A/B test (owner, 2026-10-17)
Same set-up as §65 (5 cm spacer start along a wall, 40 % × 14 s, ~246–249 cm, integral + `track_balance` 19 ‰); `encoders_enabled` switched to 0 in RAM for B (restored to 1 afterwards; stall detection is off while it is 0).
| | A: encoders on (sync + balance + integral) | B: encoders off (balance + integral) |
|---|---|---|
| Owner gap | ~1, ~1, "almost straight" cm right (≈ 0.8 avg) | ~1 R, 2.5 R, **0.5 L** cm (≈ 1.3 avg size) |
| Heading range | ±0.2–0.3° around 0 (one brief +1.1°) | steady right lean −0.2…−0.4° (run 3: −0.83° at the stop) |
| Estimated sideways (`straight_report.py`) | 0.0 / 0.2 / 0.2 cm | 0.8 / 0.8 / 0.9 cm |
| Count ratio R/L | 1.7–1.8 % | 1.8–1.9 % (counted, not used) |
- **Reading (3 runs each):** both ≈ 1 cm over 2.5 m — the gyro hold + integral + balance do the work (30 cm → ~1 cm); the difference (0.8 vs 1.3 cm) is within the ±1 cm measurement noise. The heading logs show the sync's effect more clearly: with it the heading centres on 0, without it a small steady right lean the integral works off. On a known floor the encoder sync is a modest extra; its value is stall detection, distance / speed (phase 2) and learning the balance on new floors.
- Why encoders alone cannot hold a line: they measure sprocket turns, not ground travel (slip, track tension / effective size, skid) — equal turns still curved 30 cm (§64); the gyro measures the actual heading.
- Weak spot of the current design: the sync enforces a **fixed** ratio (1 + balance); on another floor / battery level the right ratio changes and the sync then pushes against the gyro until the integral catches up. **Candidate improvement (owner to decide):** learn `track_balance` automatically from the count ratio the gyro needs while holding a straight line (slow, bounded), so sync and gyro agree on any floor.


## 67. Phone page shows stall / fault stops (owner, 2026-10-17)
- Deferred Plan I minor done first: on a Uno stall / encoder fault / steering runaway stop the phone page shows a **slim red line in the middle of the screen** (owner: "show the banner in the middle", "let it be slim") until the driver releases — bridge LLD R6. Page only; no Uno or protocol change.
- **Idea parked for later (owner):** use the shield's mode button (D2), unused on the Lite, to switch the bridge between home WiFi and its own access point. Open points to discuss: which press (short press ignored while driving vs long press) and whether the choice is remembered after a restart.
- **Held-track check on the stand (bridge `2ad6ed8`, 2026-10-17):** banner in the middle, cleared on release — works. First try: a **firmly held** track was classed as an **encoder fault**, not a stall (on the stand: held side 0 counts, other track turning, straight, no rotation = the fault rule of spec 02 §F1). The fault is sticky until a restart and that side is then ignored, so a second hold gave no stop and no banner. After a restart, slowing the track hard (not dead-stopping it) gave "track stuck" every time — "working perfectly" (owner). Stand artefact, no code change; for stand stall tests slow the track, do not stop it. Optional later: a page note while an encoder is switched off ("restart to re-arm").
- **All banners in the middle (owner):** the S1 / offline / protocol / busy banners use the same slim centred line as the stall banner — one banner style (bridge LLD R6.1).
- **Encoder fault stays until a restart (owner: "keep restart")**, and the page now shows a small amber "Right encoder off - restart the tank to re-arm" line while it is set (bridge LLD R6.5).

## 68. Encoder phase 2 split; Plan K = MOVE + counts-per-cm calibration (owner, 2026-10-17)
- **Generic "straight tracking" advice reviewed (owner pasted it):** (1) gyro steers, encoders only stabilise — already our structure (gyro hold + integral; encoders hold a ratio, not tick-for-tick); the one real point is §66's weak spot (fixed ratio vs changing floor) → later auto-learned balance / softer sync. (2) ±0.5–1° heading deadband — **rejected**: 1° uncorrected ≈ 4.4 cm over 2.5 m, we are at ±0.3° / ~1 cm without zig-zag. (3) Kd — not needed (no oscillation straight; turn hunting fixed in the bridge turn controller). (4) PWM ramp — already there (`accel_limit`). Phase 2 proceeds unchanged.
- **Phase 2 split:** Plan K = MOVE + counts-per-cm calibration; Plan L = arcs, minimum drive_rate, `max_cps` calibration.
- Owner choices: moves from a **laptop script** only in Plan K (no phone control yet); stop by **slowing to a crawl over the last 10 cm** (early stop by learned roll-on only if needed); calibration by **measuring a real `move 100`** on a tape, averaged over 2–3 runs, saved on the bridge.
- Design agreed section by section → spec 02 **Part G** (written for owner review).

## 69. Plan K floor results — MOVE by distance to ±1 cm (2026-10-17)
Bridge only (Uno unchanged). Back edge of the tank on a floor mark, tape read at the back edge.
- **Stand / first checks:** `move 50` ends `done`; STOP mid-move ends `stop`; `counts_per_m` survives a restart (NVS).
- **Review C1 (before the floor runs):** the Uno counts rising edges only, so the counts rise in reverse too — reverse moves would never have ended `done`; fixed in the bridge (raw deltas) and the tool. Reverse then: `move -50` → `done`.
- **Calibration:** three tape readings of "113 cm" were a misread **103** (owner); with that, every run fits **75.1 counts/cm → `counts_per_m` 7514** (two clean runs 7528 / 7501). The old 75.6 (§63) was close.
- **Roll-on:** the count stop was accurate (100.0–100.3 cm counted) but the tracks rolled on **2.4–2.8 cm** after the STOP: the 15 % crawl is lifted by the Uno's dead-band compensation to PWM ~116 (~19 cm/s), it cannot go slower → **spec §G6 learned early stop** (owner): stop by the learned roll-on (start 25 mm). Floor fix on the way: the learner first discarded every reading (TELEM_FAST frames sent before the STOP took effect still show the old PWM) → "driven again" only after the PWM dropped to 0. Learned: 32 mm reading at 70 % → average 27 mm.
- **Accuracy with the early stop (owner tape):**

| Run | Asked | Tape | Sideways |
|---|---|---|---|
| 3 × 40 % | 100 cm | 101, 101, 101 | none |
| 40 % | 200 cm | 201 | 2 cm left |
| 2 × reverse 40 % | −50 cm | 50, 49.5 | none |
| 3 × 70 % | 100 cm | 100, 101, 101 (1 unmeasured) | none |

  **Pass:** ±1 cm at 40 % and at 70 %; heading within ±0.6° at every end.
- **Held track:** dead-stopped → `encoder_fault` (on the floor too: the held tank barely rotated, −3.7°; sticky until restart, the phone shows "encoder off"); slowed hard → **`stalled`** at 51.5 cm. Pass.
- **Box ahead:** the Uno's obstacle slow-down glided the tank to a stop ~15 cm short **without its blocked flag**, so the move first ended `stalled` → bridge rule: no progress on a forward move with the ultrasonic < 25 cm = **`blocked`** (spec G3). Then: `blocked` at 43.6 cm. Pass.
- **Open:** the 200 cm run drifted 2 cm left (stick driving ~1 cm / 2.5 m) — AUTO frames skip the Uno's `track_balance` feed-forward (review I2; owner: test as is first). Watch it; the fix is one Uno line (apply the balance to straight AUTO frames). Review minors deferred (ledger).
- **Likely cause of the right-hand pull (owner, 2026-10-17):** a broken **left middle road wheel** (drag on the left side) — probably the source of the ~1.9 % count difference behind `track_balance` 19 ‰ (§64–§65). Owner will repair it. **After the repair:** re-measure with `td_record.py` + `straight_report.py` and reset `track_balance` (may drop to ~0), then re-check `move 200`; the one-line Uno change (track balance for straight AUTO frames, Plan K review I2) only if moves still drift.

## 70. Plan L design discussion — arcs (owner, 2026-10-17)
- Arc maths: ω = v / R; outer / inner track v·(1 ± B/2R). Real tank: skid (effective B 1.3–1.6× geometric, floor-dependent) and the inner track cannot run slowly — from the recorded run the dead-band lift gives ~17.7 cm/s at 15 % vs ~22 cm/s at 40 % (0 or ≥ ~17.7 cm/s) → tightest smooth radius R_min = (B/2)·v/(v − v_min) (~38 cm at 40 % for B = 15 cm).
- **Owner: option 1 — gyro-closed arcs:** v measured by the encoders (counts_per_m), target ω = v / R, the bridge's drive_rate loop tracks it on the gyro, end at the target heading. **Later: a learned head start** (feed-forward "steering per turn rate" learned from driven arcs, like the roll-on learning).
- Track width B is not in the control; needed only for the R_min check — one ruler measurement (centre to centre of the tracks), stored as a constant.
- **Turn kinds (owner):** spin (done, Plan H), arc, **pivot** (inner track stopped, outer drives: radius B/2 — smooth, 0 is a speed the Uno does well), and the offset spin (tracks opposite at different speeds, 0 < R < B/2 — left out: no practical use on this tank). One formula: R = (B/2)(v_R + v_L)/(v_R − v_L). Radii in the gap (B/2 < R < R_min) or under B/2 → refused with a helpful message (use pivot / faster / wider).
- **Plan L scope (owner, agreed):** arcs (gyro-closed) + pivots + the "too tight" refusal + minimum turn rate for `drive_rate` (F2.3) + top-speed (`max_cps`) calibration (F2.4). Learned head start: a later plan.
- **Top-speed calibration (owner: measure first, decide after — option 3):** the Uno Lite uses a fixed `kMaxCps` 4000 counts/s at PWM 255 for stall detection and `spd_act`; protocol settings `max_cps_l/_r` (ids 48/49) exist but the Lite does not implement them. Next session (tank on the stand): run each track at full power ~2 s, measure counts/s; within ~15 % of 4000 → bridge-only use; far off → small Uno change (accept ids 48/49, ~150–250 B flash) + USB flash.
- **Also next session:** owner measures the track width **B** (centre to centre) for the R_min check. Then continue the Plan L design (arc command fields, refusal message, minimum drive_rate, tests) → spec → LLD → plan.
- **Top speed measured (stand, 2026-10-17):** a straight 100 % run is distorted (stand vibration reads as turn rate, the heading hold pulls a track back); a **full-power spin** (heading hold off) gives PWM 255 on both: **left 2535, right 2490 counts/s** (~33.5 cm/s, battery 7.84 V under load). The Uno's `kMaxCps` 4000 was ~37 % high (false stalls on hard-working tracks more likely, `spd_act` reading ~60 % at full power) → **owner: option 1, constant 4000 → 2500** (one value, no flash cost), USB flash.
- **Track width (owner):** tracks 3.5 cm wide, 15.5 cm inside gap → **B = 19 cm** centre to centre (pivot radius 9.5 cm).
- **After the Uno flash (kMaxCps 2500, floor):** full-power spin `spd_act` ≈ ±100 % (was ~60 %); held track at 25 %: slipping through the hand → no stall (the motor still turns at speed, correct), firm grip → **`stall` right** at 3.3 s, both tracks stopped. Pass.
- **Arc command (owner):** follows the robotics conventions (ROS REP 103/105: + = counter-clockwise / left, reference point = the robot's centre; continuous driving = velocity command, our `drive_rate`). Arc primitive = `{"t":"turn","by"|"to":deg,"radius_cm":R,"speed":%}` (radius to the tank's centre, speed = centre speed, forward only), pivot = `"pivot":true`; JSON to the bridge, the Uno keeps getting binary DRIVE frames (protocol 3 unchanged). Units stay cm / degrees / % (friendly for Tank Logo; a ROS layer could convert later).
- **Minimum drive_rate (owner: option 1):** asked rates below the slowest smooth turn rate are **clamped to it** and flagged `turn_limited`; the minimum is measured on the floor (spins at low steering, gyro + encoders).
- **Minimum spin measured (floor, owner present):** steering 8/10/12/15/20/25 % → 65 / 68 / 68 / 74 / 76 / 87 °/s, all smooth (owner) → slowest smooth spin **≈ 65 °/s**. With B = 19 cm, v_min 17.7 and v_max 33.5 cm/s every radius has a usable **speed band**, and **arcs under ~31 cm are impossible smoothly at any speed** → spec 02 **Part H** written for owner review.

## 71. Plan L floor results — arcs, pivots, minimum spin (2026-10-18)
Bridge only (Uno unchanged since kMaxCps 2500). Owner measured with marks: S (tank centre at the start) and C (arc centre 60 cm to the side; ~51 cm from the outer edge of the track on that side).
- **Refusals:** `turn 90 radius 20` → "radius 20 cm too tight (min 31): use pivot"; `radius 40` at 40 % → "radius 40 cm: use speed 43-64 %, or pivot". Pass.
- **Arc 1 (R 60, 40 %, left):** w ≈ 20 % gave only 16–18 °/s (target 21.7) → **1 % of steering ≈ 0.85 °/s in an arc** (both tracks forward; a spin gets ~2.2) — the spin's feed-forward 0.45 left the arc too straight (review I1); also ended `blocked` at 66° (an obstacle). → **arc feed-forward 1.15 %/(°/s)**.
- **Arcs with the fix (90°, R 60, 40 %):** heading ~90° by eye (gyro 93.6–97.1°, the stop leads ~3–7° — gyro scale and spin-down); path: **left ended 6 cm outside** (C → left edge 51 → 57 cm), **right 10 cm inside** (51 → 41 cm). Opposite errors = the tank slides **right** while arcing — matches the right pull / suspected broken **left middle road wheel** (§69). **Arc path acceptance (±5 cm) deferred until the wheel is repaired** (owner); then repeat 90° L/R at R 60 and R 100 and add a correction only if still needed. The encoder + gyro estimate (turn.py) said ±2–4 cm — it cannot see a sideways slide.
- **Pivots (90°, 40 %):** first run: the inside track moved 6.6 cm when the controller corrected back past the target (it swung round the other track, review M5) → fix v = ±w by pivot side. Then: **left +87.7°, right −87.7°, inside track 0 counts / PWM 0**. Pass (±3°).
- **Slow-spin clamp:** `drive_rate` 10 °/s → `tf.primitive.limited` in 30 / 30 frames, spun ~75 °/s smooth. Pass.

## 72. WiFi mode switch + settings page — design discussion (owner, 2026-10-18)
- Existing: the Uno reports a short press of D2 as `EVENT button_press`; a 5 s / 15 s hold at boot sends `WIFI_RESET_REQ` / `ADMIN_RESET_REQ` (the bridge ignores them today); the bridge's WiFi comes from `secrets.h` (home, else own AP after 10 s); the full-firmware design (spec 02 / spec 01) plans ≤ 3 saved networks, a captive setup page and a physical button confirmation.
- **Use cases (owner):** switch between **home WiFi and the tank's own AP** deliberately — outside without home WiFi, and **also at home** (phone straight to the tank). In AP mode the bridge stays on its AP until switched back.
- **Switch:** **double press** of the tank's button (two short presses within ~1 s) — detected on the bridge from the Uno's existing events, no Uno change. Proposed default (to confirm): the mode is remembered across restarts.
- **Settings page `/settings` (owner):** first version = **WiFi** (status, switch mode, **up to 3 home networks** with name + password, scan, own-AP password) + **tank info** (versions, battery, uptime, restart reasons, encoder status) + **motion calibration** (`counts_per_m`, learned roll-on: view / reset; `track_balance` view / edit; arc model view) + **maintenance** (restart bridge, forget WiFi networks, download a short telemetry CSV). **Second step:** driving feel (max speed, `accel_limit`, heading gain, reverse steering, sound mode). Safety distances (`obstacle_stop` / `obstacle_slow`) read-only. Not now: OTA from the page, accounts, camera.
- **Who may change settings (owner: option 1):** viewing is open; a change (save a network, AP password, calibration reset, forget networks) waits for a **single press of the tank's button within ~30 s** as physical confirmation. While a confirmation waits, a single press confirms; the double press switches WiFi only when nothing waits.
- **Mode after a restart (owner: option 2, not remembered):** every boot starts with home WiFi (fallback AP after 10 s, as now); a double-press switch lasts until power-off. A switch plays a sound and sets the LED (blue = home, white = own AP).
- **Settings page UX (owner):** phone = **tabs** (WiFi · Tank · Calib · Maint, option B of the browser mock-ups); desktop = the same sections as a left navigation, Stitch screen "Conqueror Bridge WiFi Settings" (project "Conqueror Hub", design system Phosphor Cockpit) → `docs/design/stitch/12-bridge-settings-desktop-v1.png` (as generated) and **`…-v2.png`** (owner fixes: no "Apply changes" / "Factory reset" — every change is confirmed on its own by the tank's button; no captive DNS; real bridge / Uno values; restart bridge only; **the Emergency STOP stays** on the settings page). v2 HTML kept in `docs/design/stitch/html/` (Stitch's edit did not reach its stored file, the fixes were applied to the HTML by hand).
- **Design approved (owner):** WebSocket commands (not REST), ≤ 3 networks in NVS seeded from `secrets.h`, strongest-visible join, 30 s button confirmation, the page records the CSV itself → **spec 02 Part I** written for owner review.
- **Settings page tabs (owner, first look on the tank):** four tabs were too thin → **two tabs: WiFi, and Tank** (info + calibration + maintenance on one tab). Page only.
- **Safety finding (Plan M floor check, 2026-10-18):** the settings page's STOP while another client drives stopped the tank for only one frame — `LinkOwner::onStop` sends STOP but keeps the driver, whose next DRIVE (≤ 50 ms) restarts the tank. Pre-existing (Plans C/H: "STOP from anyone" never said the stop holds); the drive page's own STOP works only because that driver stops sending. Owner to choose the fix (recommended: the bridge holds the stop until the driver releases its stick, as the Uno's stall latch).
- **Owner: remove the STOP from the settings page** (a STOP from a non-driving device makes no sense there). **Open item:** the same gap between two drive pages — a second phone's STOP is undone by the driving phone's next DRIVE (≤ 50 ms); fix later (recommended: the bridge holds a STOP until the driver releases its stick).

## 73. Plan M results on the tank (2026-10-18)
- First boot of the new bridge: the `secrets.h` network seeded into slot 1, rejoined home WiFi in ~5 s.
- **Double press:** home → own AP (white LED, phone joined `conqueror-bridge`, page showed OWN WIFI) → home (blue LED). Pass.
- **Add a network + button confirmation:** phone hotspot saved as slot 2 after one press ("Done ✓"). Found on the way: scan results were hidden in the name field's suggestions → a visible tappable list; the Scan button could stay disabled when the page lost its connection (a phone running a hotspot drops its own WiFi) → 10 s timeout + re-enable on reconnect; the tank sees **2.4 GHz only** (hotspot must use 2.4 GHz). Router-off failover test skipped (owner).
- **Unconfirmed change expires** (track_balance 18 without a press → "Not confirmed", stayed 19). **Confirmed edits** 18 → 19 read back from the Uno. Pass.
- **CSV download**, **restart** (software reset, uptime reset) — restart had no feedback → "Restarting … / back online ✓". Pass.
- **Tabs:** four were too thin → two (WiFi, Tank).
- **STOP on the settings page removed** (owner) after the safety finding in §72; the cross-device STOP-hold remains an open item.
- **STOP hold fixed and floor-checked (2026-10-18, bridge a045935):** a script drove at 20 % streaming DRIVE at 20 Hz; the owner pressed STOP on the phone's drive page at 1.08 s → `nack STOPPED`, tracks stopped at 1.09 s and stayed stopped for the remaining ~5 s (0 of 246 telemetry frames moving) although the script kept sending. Open item §72 closed (bridge LLD R9.6).
- **Settings page restyled (owner, 2026-10-18)** to the Stitch Phosphor phone designs (`docs/design/stitch/13-…wifi-mobile-v1.png`, `14-…tank-mobile-v1.png`): inline Lucide icons, bracketed card headings, signal bars, CONNECTED / STANDBY / OPEN / ON AIR badges, battery bar, big-number distance cards with reset buttons, a −/+ stepper for track_balance, a confirm box with a countdown bar, footer "PHYSICAL CONFIRMATION". Made-up details in the Stitch screens (port, channel, UART rate, battery %) left out. Same commands as before; still no STOP on this page. /settings is 10.4 KB gzipped (budget 24 KB).

## 74. Plan D (bench tool) retired; TCP 3333 passthrough removed (owner, 2026-10-18)
- **Decision:** Plan D is retired without being executed. Written 2026-10-11, before the phone page, the motion service and the encoders; everything it was for now exists elsewhere — driving: phone drive page; straight / turn / spin tests with numbers: `tools/smoke/move.py`, `turn.py`, `straight_report.py`; calibration and tuning: on the bridge (counts_per_m, learned roll-on, track_balance) and `/settings`; recording: `td_record.py` and the CSV button; a simulated Uno: the bridge's native tests. The plan files stay for history, marked retired.
- **Also removed (owner):** the bridge's TCP 3333 passthrough (raw protocol frames straight to the Uno, bypassing the motion service), its `_cqbridge._tcp` mDNS entry and `tools/smoke/bridge_echo.py`. Reason: an unused, unauthenticated second way to the motors. Design and results: bridge LLD R10.
- **Tank Logo (spec 05):** was to run inside the bench tool; its runner's home (hub or AI engine) is decided when Logo work starts.
- **Checked on the tank (2026-10-18, bridge 8361f00, stand):** `bridge_echo.py no-tcp` → port 3333 refused; `/info` lists ports http / ws / stream / ota only; drive page drives and stops on stick release; a STOP from the laptop while the phone kept pushing held the tracks ("Stopped by another device") until the stick was released. Fresh review of the removal: nothing Critical / Important; minors (stale comments, dev-setup, help text) fixed.

## 75. 24th sound `horn` (owner, 2026-10-18)
- The phone's sound sheet showed 23 buttons in a 3-column grid (one gap); owner: add one more sound so the grid is even. Options: (A) a real new sound, (B) a page-only "random" button. **Owner: A, a horn.**
- `SoundId` gains **`HORN: 24`** (not breaking: enum additions need no protocol version bump, protocol-generator LLD). Melody: two short honks, 450 Hz 150 ms · rest 70 ms · 450 Hz 300 ms; +15 B Uno flash (Uno Lite had ~1.2 KB left).
- The same change gives the sound buttons their green outline (they showed the browser's default grey).
- Flash the Uno (USB) and the bridge (OTA); an old Uno answers an unknown sound id with an ACK error, nothing else breaks.
- **Checked on the tank (2026-10-18, stand):** Uno flashed (31 044 B, 96.2 %), sound sheet shows 8 × 3 buttons with the green outline, `horn` plays two short honks.

## 76. Camera re-check with ELEGOO's original firmware (owner, 2026-10-18)
- **Why:** before calling the camera off for good, compare with the firmware it came with. Differences from ours (`docs/legacy/esp32s3-old-firmware.md`): camera started **before** WiFi, WiFi power save left on, default TX power, XCLK 20 MHz, arduino-esp32 3.3.7 (ours 3.3.11). Web: arduino-esp32 issue #11846 (camera streaming + network broken after 3.3.x, closed as stale); an old Espressif forum thread blaming a 20 MHz camera clock coupling into the antenna.
- **Test:** full 8 MB backup of the board (`~/tank-dump/bridge-backup-2026-10-18.bin` on the Linux machine, never copied off it), then the original ELEGOO flash image written back.
- **Result:** ELEGOO's firmware prints **"Camera OK"**, then **fails to join the home network on 3 of 3 boots** (10 s, then its setup AP), with the network name confirmed by hash to be the same 2.4 GHz network (channel 6, signal 100 %) our bridge joins in ~5 s with the camera off. A password typo cannot be fully excluded, but the owner re-entered it carefully.
- **Conclusion:** ELEGOO's own firmware has the same trouble on this board → the cause is the camera hardware (module / ribbon / coupling into the antenna), not our firmware. Our firmware restored from the backup (bridge 8361f00, settings and calibration intact).

## 77. LED white ↔ blue + beep every 4 s with no phone in use — a background browser tab (2026-10-18)
- **Symptom (owner):** with nobody driving, the tank's LED kept going white then blue with a beep; "the ESP–Uno connection is fluctuating".
- **Cause:** the UART link was fine (≈ 55 frames/s, 0 bad CRC). The bridge log showed a WebSocket client connecting every ~4 s and being reaped "silent for 3 s"; `lsof` found Google Chrome on the Mac holding the connections — the drive page left open in a background tab after the STOP-hold check. Chrome throttles repeating timers in hidden tabs to once a minute, so the 1 s ping stopped → reaped after 3 s → the page's one-shot 1 s reconnect timer (not throttled) reconnected. Each connect = Uno synced (white LED + "ready" sound), each reap = link lost (blue).
- **Fix (owner: "investigate and fix"):** a hidden page holds no link. The drive page, when hidden, still releases the controls and sends STOP, then closes its WebSocket and the MJPEG stream and does not reconnect; when shown again it reconnects at once. `/settings` the same (a pending button confirmation is lost if the page is hidden — open it again). Bridge LLD R11.
- **Follow-up tests the same day (throw-away sketches over USB, Linux host pinging 70× per phase):**
  - Camera alone (radio off): OV3660 healthy — 13.5 fps at XCLK 10 MHz, 27.5 fps at 20 MHz, 0 bad frames, a clear photo. WiFi scan (listen only) with the camera off / on: same signal (±2 dB) → reception is not drowned.
  - Joined to the home WiFi: **just starting the camera** (no frames fetched) → 47–100 % ping loss; frames in internal RAM, WiFi power save on, TX 19.5 dBm: no fix; the bad state can outlast stopping the camera.
  - **Only the 10 MHz XCLK square wave** on GPIO15 (sensor not configured) → 70–100 % loss; **with the camera unplugged** still 16–54 % → the clock signal on the ESP32 board itself reaches the antenna; the camera / ribbon makes it worse.
  - **GPIO15 drive strength** (camera unplugged): drive 3 (strongest) 74–100 % loss; **drive 0 (weakest) 0 %**; drive 1 ≈ 0–4 % but ~1 s delays. The camera driver leaves the pin at drive 2.
  - **Camera plugged in and streaming, fresh boot:** driver default drive 2 → 83–100 % loss; **drive 0 → 0 % loss, 6–8 ms, 20–25 fps, 0 failed frames, clean picture** (2 of 2 boots); drive 0 also cleared the bad state left by drive 2.
- **Revised conclusion:** the sharp edges of the camera clock (XCLK) couple into the antenna on this board; **gentler edges (GPIO15 drive 0) fix it in firmware** — no new camera or board needed. Proposed bridge change (owner to approve): set GPIO15 to drive 0 right after `esp_camera_init`, then turn the camera back on and re-test on the tank. Test photos deleted (they showed a person).

## 78. RC mode: camera HOME button did nothing (owner, 2026-10-18)
- **Cause:** in RC mode the LOCK / HOME column is moved (absolute position) over the camera stick's box; the stick box sat on top, so a tap on HOME hit the box and sent nothing (page test: `elementFromPoint` = `.stick` at 844×390 and 1200×700).
- **Fix:** `body.rc .right .col { z-index: 2 }`. Page test at 844×390, 667×375, 932×430, 1200×700: HOME sends `cam_home` in single-stick and RC; the steering slider still gets its taps.
- Found during the camera-back-on stand check (bridge LLD R12): driving, release, STOP and turning with live video all fine.

## 79. Single tank, no separate hub (owner, 2026-10-18)
- **Decision (owner):** "let's cut it — no multi tank, it's overkill"; with that the separate hub (Spec 3: NestJS + React + SQLite + Docker; fleet, fan-out, logins, history, recordings, missions, AI API) goes away. Spec 3 is **parked**, not deleted.
- **Where things live now:**
  - **ESP32 bridge:** every web page — drive, `/settings`, `/help`, the **Tank Logo editor** (new page), later a small AI-status panel — and the **Logo runner** (as Spec 5 already planned); short telemetry history in PSRAM (minutes, CSV download). Room: the bridge app uses ~35 % of its 3 MB slot, pages are 10–20 KB gzipped, 8 MB PSRAM mostly free.
  - **AI engine (Python, laptop / PC):** the only off-tank piece; connects **directly** to the bridge (WebSocket + MJPEG) and has its own small local page (status, decision log, voice). Owner asked why not run it on the ESP32 too: vision models and a language model need far more compute than a 240 MHz microcontroller (it can run only tiny detectors at a few fps at low resolution, no LLM), and heavy work there would compete with the WiFi and the drive link that keep driving safe. A small on-tank detector for one simple job (e.g. "person right ahead → slow down") stays a possible later extra.
- **Consequences:** Spec 0 / 4 / 5 headers note the change; Spec 4 is revised when the AI engine design is reopened (direct bridge link, who wins between phone and AI, the 2-viewer MJPEG limit: phone + AI engine). Open question to the owner: Python-only off the tank (FastAPI in the AI engine) instead of the NestJS house standard for any web server.

## 80. The tank app: 5 screens, Settings with 4 tabs (owner, 2026-10-18)
- **Function list (owner):** live sensors — already on Drive, deeper ones on Settings; graphs — Settings; calibration wizard — Settings; event log — Settings; firmware upload from the browser — not needed; camera options — Settings; child-play mode — Settings; moves panel — not needed, Logo does it; AI — the only big new page.
- **Structure:** one tank app on the bridge, Drive as home, a ☰ MENU (replacing ⚙ SET) to **Logo** (new), **Settings** (tabs **WiFi · Tank · Tune · Log**; graphs on Tune — owner OK), **AI** (served by the AI engine on the laptop, the app links to it — owner OK), **Help**. Plain HTML/CSS/JS, no React (the React plan went with the hub).
- Document: `docs/design/bridge-ux.md` (supersedes `hub-ux.md` for one tank). Next: Stitch screens for Logo.

## 81. Tank Logo screens — two decisions (owner, 2026-10-18)
- Screens v2 (laptop + phone): `docs/design/stitch/15-tank-logo-desktop-v2.png`, `16-tank-logo-mobile-v2.png` (Stitch, corrected by hand).
- **Programs are saved on the tank** — one list for every device (already in v1; Spec 5 §12 item 7).
- **A running program stops** when its page is hidden or disconnects, or when another device takes control — like driving by hand; STOP from anywhere ends it at once (Spec 5 §12 item 8).

## 82. Tank Logo language — first decisions for spec 05 v2 (owner, 2026-10-18/19)
Before the grammar, the word table and the test cases are written (owner: "the CFG, spec and grammar first"):
- **(a)** `WAIT UNTIL cond` for every program (≤ 60 s), not only AI programs.
- **(b)** No `FOR` loop in v1 (`REPEAT` + `REPCOUNT`).
- **(d)** Recursion allowed — "recursion is Logo's main feature".
- **(e)** Procedures can **optionally** give back a value (`OUTPUT`).
- **(f)** New reporter `OBSTACLE?` — something closer than the obstacle-stop distance.
- **(g)** New reporter `TIMER` — seconds since the program started.
- **(h) Option C:** when a move is stopped by an obstacle (the Uno brake, unchanged), a program that uses `BLOCKED?` anywhere carries on (`BLOCKED?` = the last move was cut short); a program that does not ends with "Something is in the way on line n". Stall, tilt, low battery and STOP still end every program.
- **Style: functional at the core (owner: option B).** No `MAKE`: values come in as procedure inputs and go out with `OUTPUT`; lists are values (list words and higher-order words such as `MAP`, `FILTER`); motion / light / sound stay commands executed in order. This reverses **(c)** (values were to be numbers only) and brings `OUTPUT` into v1. Follow-on choices (lambda form, tail calls, memory on the ESP32, which list words) are being decided next.
- Still open from 2026-10-18: turn sign (classic right = positive), `ARC LEFT/RIGHT a r` with the 31 cm limit, `PIVOT LEFT/RIGHT a`, real `SOUND` names, parse / check / compile on the ESP32.

## 83. Tank Logo v2 — three levels, functional core settled (owner, 2026-10-19)
- **Owner worry:** "will this not be too much for a kid?" → **one language, three levels** (owner: yes): **1 Starter** (about 5–7: `FD BK LT RT`, `REPEAT`, `LED`, `BEEP`, `SOUND`, `WAIT`, `LOOK` — no values needed), **2 Explorer** (about 8–10: own words with inputs, `IF`/`IFELSE`, sensors incl. `OBSTACLE?` `BLOCKED?` `TIMER`, `WHILE`, `WAIT UNTIL`, recursion), **3 Expert** (older children, owner, AI: `OUTPUT`, lists, `MAP` `FILTER` `REDUCE` `FOREACH`). The editor shows words by level; the language is the same at every level.
- **Build order:** grammar, word table and tests for all three levels now; **levels 1–2 in v1** (no lists → no memory pool / garbage collection on the ESP32), **level 3 in v1.1**.
- **Functional core (all recommendations accepted):** values = number, `TRUE`/`FALSE`, word, list (nested); code passed to `MAP`/`FILTER`/`REDUCE`/`FOREACH` as a procedure name (`"DOUBLE`) or a `?` template (`[? * 2]`); list words `FIRST LAST BUTFIRST/BF BUTLAST/BL ITEM COUNT EMPTY? MEMBER? LIST FPUT LPUT SENTENCE/SE`; no `RUN`; `REPEAT FOREVER WHILE WAIT UNTIL` stay; **tail calls use no stack**, other recursion ≤ 100 levels; lists in a fixed PSRAM pool (≈ 50 000 cells) with garbage collection; dynamic types with friendly run-time messages, everything checkable checked before anything moves; `=` compares numbers, words and whole lists; no global variables (constants are no-input procedures: `TO SIDE OUTPUT 50 END`).
- **The five points of 2026-10-18 (recommendations accepted):** turning right is positive in Logo (`RIGHT 90` → `HEADING 90`; converted to the tank's left-positive inside); `ARC LEFT a r` / `ARC RIGHT a r` (smallest circle 31 cm, speed moved into the radius's band with a note); `PIVOT LEFT a` / `PIVOT RIGHT a`; `SOUND` uses the tank's real 24 sound names; the **ESP32 parses, checks and compiles** the program (C++, host-tested) — the page sends text and gets back errors and the planned path; the AI engine sends text too.
- Written down in `logo/spec/grammar.md`, `logo/spec/words.yaml`, `logo/spec/tests/` and spec 05 Revision 2.

## 84. Tank Logo grammar — review fixes (owner, 2026-10-19)
A review of the v2 grammar (pasted by the owner) raised four points; all applied (owner: yes):
1. **Minus:** any error right after a complete value followed by a negative number (`-5` with a space before, none after) carries the hint "to subtract, write 10 - 5", whatever its code; new `PROGRAM_7028` for `(10 -5)`; `[ 10 -5 ]` in a data list is two numbers (grammar.md §9.1).
2. **List literals are in v1** (constants stored with the program: `LED [255 128 0]`, `LOOK`, `PLAY`, `SAY`, inputs to your own words, `=`); only building / taking lists apart at run time waits for v1.1 → v1 has no list pool and no garbage collector (grammar.md §1).
3. **Tail position defined exactly** (grammar.md §8): last statement of a word; the last statement of each block of an `IF`/`IFELSE` in tail position; `OUTPUT` of a single call; never inside a loop or template body (the review's suggestion to count loop bodies was wrong — the loop goes round again); any of your words, not only self-calls (two words calling each other).
4. **Every refusal has exactly one code** (grammar.md §11); new `PROGRAM_7029` for an unknown preset (`LED PINK`); malformed templates map to 7005 / 7004. Tests added for each.

## 85. UI closed except the AI screen (owner, 2026-10-19)
- **STOP in the Tune wizard: OK (owner).** The calibration step drives the tank, so it has its own red STOP next to DRIVE 1 m — the only STOP on Settings; the rule "no STOP on Settings" (§72) stays for everything that cannot move the tank.
- **Close the UI completely except the AI screen (owner):** the Settings → Tank additions (live sensors, self-test, camera options, child-play card) are designed now; the AI screen comes with the AI work.
- **Done (2026-10-19):** Settings → Tank v2 designed on the real page (`docs/design/stitch/20-bridge-settings-tank-mobile-v2.png`): new cards Live sensors, Self-test, Camera, Child play; the Distance and Straight-driving cards move to Tune (Tank = what the tank is and does, Tune = what you adjust). **The UI is designed except the AI screen**: Drive (+ ☰ menu), Logo, Settings (WiFi · Tank · Tune · Log), Help. Child-play limits and rules are placeholders until its own safety design.

## 86. Tank Logo LLD drafted (2026-10-19)
`docs/lld/tank-logo-lld.md`: reader on the ESP32 (lexer, two-pass iterative parser — the firmware rules forbid recursion, checker, compiler to a small instruction set with tail calls), runner in the link task in ≤ 500-instruction slices, effects mapped onto the existing motion service and Uno commands (no new safety rules; new BodyLink `sendTone` / `sendCamSet` for frames that already exist), planned path by simulation and driven path by odometry, saved programs in a new 256 KB LittleFS partition (app slots 3.0 → 2.875 MB; needs one USB flash), WebSocket `logo_*` commands and events, the `/logo` page, code generated from `words.yaml`. Open for the owner: the partition change, compiling on a separate task, a plain textarea editor.

## 87. Tank Logo LLD decisions (owner, 2026-10-19)
- **Partition change: yes** (256 KB LittleFS `logo`, app slots 2.875 MB, one USB flash).
- **Editor: colouring, easy for children** (owner: "it's for the kids so it should be easy for them"): colour by kind of word (moves, turns, loops/choices, light/sound/look, numbers, your own words, comments, unknown words underlined), big text, word buttons that insert the word with its example value selected, errors under the line with a ▲ under the word (LLD §14).
- Compile on a separate task: recommendation kept (not objected to). LLD marked approved.

## 88. Plan N1 — Tank Logo reader (2026-10-19)
- Generator `logogen` (logo/), reader in `firmware/esp32/src/logo/` (lexer, two-pass iterative parser, checker, reads printer); host tests incl. every v1 case of `logo/spec/tests` (count: 67 — 35 reading + 32 errors; the 2 v1.1 cases are skipped by design). Whole native suite 423 / 423; the bridge builds with the reader compiled in (nothing calls it yet).
- Spec fixes found while planning: the "unknown word" case now defines SQUARE; the SEE? reading case is reading-only (`check: false`); IFELSE has a `missing` text; a too-big number stays a NUMBER token (clamped) so 7015 is the first error.
- Size limits (coding standards §3.1) applied to the plan's code: the parser is split into four files and the checker into small helpers; precedence, operator names and node brackets are tables. Each split was checked against a golden record (every conformance case plus ~50 broken texts, both capability sets): output byte-identical before and after.
- Two small fixes beyond the plan: a too-deep operator stack (more than 128 waiting operators) is now 7010 "too deeply nested" like the operand stack (before: silent, or an overrun for NOT); the minus-hint buffer is 112 bytes (a compiler truncation warning).
- Final review fixes: the checker walks the tree one level per stack entry, so a long program is checked to its last line (before: past ~256 waiting statements the rest was skipped, and e.g. FD with encoders off passed); a tree deeper than 256 levels (a 300-term sum) is 7010, never passed unchecked. Columns count characters, not bytes (LLD §4), so an accent or emoji earlier on the line no longer shifts the error mark. A non-ASCII character outside quotes (a curly quote from the phone keyboard, ✓) is one 7016 showing the whole character, not one per byte.
- Deferred minors (review): `FD - 5` says "( ) needs a value"; the reads printer cuts trees deeper than 128 without a mark; messages can cut a UTF-8 character at 32 / 40 bytes; `Node::count` is 8-bit (make it 16-bit before N2 uses it); one more nesting / next-line test each; `static char pex` in `Parser::inputSpec` (one parser at a time); a hint for curly quotes. Owner to rule: `TO A :x :x` (same input name twice) is accepted.

## 89. Build order: the tank app first, then Logo (owner, 2026-10-19)
- Owner: "first build the tank app, then Logo, and then the Logo UI." Order: **Plan O — the tank app** (☰ menu on Drive; Settings in 4 tabs WiFi · Tank · Tune · Log with the new cards — live sensors, self-test, camera, child-play card, Tune graphs + calibration wizard, the event log; bridge work behind them) → **Plans N1–N3 — the Logo engine** (reader, runner, on the tank) → **Plan N4 — the Logo page** inside the finished app.
- Plan N1 (`docs/superpowers/plans/2026-10-19-plan-n1-logo-reader.md`) stays written; it runs after Plan O. Next: bridge LLD Revision 13 (the tank app), then Plan O.

## 90. Tank app — facts that change the designed screens (2026-10-19)
From the code (research for bridge LLD R13):
- **No IMU temperature and no tilt angles on the Uno Lite** (TELEM_SLOW `temp` and TELEM_FAST `pitch`/`roll` are always 0). The Live sensors card drops temperature and shows the *tilted / lifted* alarms instead of angles. (Logo's `TILT` word needs a decision in Plan N2: give 0 / refuse on the Lite.)
- **The self-test cannot be re-run on the Lite** (CALIBRATE → UNAVAILABLE): the card shows the last start-up result and says a restart runs the checks again; no button.
- **Camera settings are compile-time constants today** → new run-time setters + NVS `cq_cam`.
- **The bridge keeps no event history** (logs go to USB only) → new `EventLog` ring (200 entries) feeding the Log tab; several Uno events are dropped today (SELFTEST, IMU/US fault, envelope changed …) and will be logged.
- **Child play:** the Uno Lite supports ENVELOPE (incl. the CHILD_PLAY bit) but the bridge never sends it; the Tank card stays read-only until the child-play safety design.
- Live graphs use the existing `debug` stream (`td` 10 Hz: encoder counts) — no new telemetry.

## 91. Tank app decisions (owner, 2026-10-19)
- **Child-play card read-only** for now (the turn-on flow with its own safety design). **Calibration saves need the tank's button** (also `move.py`). **The AI address is saved on the bridge.**
- Owner questioned §90: "do we have a temperature sensor, I think we have tilt, why can't the self-test run". Answer: the MPU6050 has a die thermometer the Uno already reads but never sends; tilt is measured but only as an alarm (the Lite has no float `atan2`); the self-test exists but the Lite dropped the CALIBRATE family to save flash. **Owner: yes — add all three to the Uno Lite** (board temperature, pitch/roll with integer maths, CALIBRATE SELF_TEST re-runs the boot self-test; ≤ 400 B of the ~1.2 KB left, one USB flash; Uno Lite LLD §21).
- **Camera settings (owner: yes):** picture size QVGA / VGA / SVGA / HD, quality 10–40, frame-rate limit max / 15 / 10 / 5, brightness and contrast −2…+2, low-light mode, flip, mirror, reset to defaults; the page shows the frame rate and data rate next to them; the WiFi is re-checked on the tank at the larger settings.
- LLD R13 approved.

## 92. Uno Lite: self-test on request only (owner, 2026-10-19)
- Built in Plan O Task 1: board temperature, integer pitch/roll and the self-test on request cost **+632 B** even with 16-bit-only maths (angles 470, self-test 120, temperature 40) → 580 B left. Owner: "it's getting too tight — use only what is useful".
- Usefulness: tilt angles low (safety already has the tilted/lifted alarms; flat floors read 0–2°), temperature low (the chip's own), self-test medium (re-check after a fix without a power cycle). **Owner: self-test only** → **31 144 / 32 256 B** (1 112 B free).
- Consequences: the Tank tab's Live sensors card shows the tilted / lifted alarms, no angles or temperature; `ts.temp_c` is not added; Logo's `TILT` becomes **`TILTED?`** (true while tilted past the limit or lifted). Angles return with a bigger body board.

## 93. Settings → Tank → Camera shows the live picture (owner, 2026-10-19)
On the stand check of Plan O the owner asked to see the camera feed next to the camera settings, so a change (size, quality, brightness, flip …) is seen at once.
- Options discussed: **A** live preview always on while the Tank tab is open; B a PREVIEW button (on demand); C a still picture after each change.
- **Owner: A.** The settings page does the important work; seeing the effect matters more than saving one viewer place and some WiFi.
- How: the Camera card shows the MJPEG stream (`:81/stream`) under its title while the Tank tab is open and the page is visible; it stops when the tab changes or the page is hidden. It counts as one of the 2 viewers — with the Drive page also streaming the limit is reached and the card says so. Page-only change, no bridge code.

## 94. Camera chip and sheet on the Drive page (owner, 2026-10-19)
Asked during the Plan O stand checks: show the camera's state while driving and let the driver change it there.
- **Chip** in the bottom row next to SNAP / MAX: `▣ VGA · 14 fps`, amber below 8 fps (the WiFi is struggling). The Drive page asks `{"t":"tank"}` every 2 s while it is visible — page only, no bridge change (also keeps the bridge's fps / kbit/s window short, review M3).
- **CAMERA sheet** on a tap: picture size QVGA · VGA · SVGA · HD and frame-rate limit MAX · 15 · 10 · 5 (`camera_set`), plus a link to Settings → Tank → Camera. Brightness, contrast, low light, flip and mirror stay in Settings (set once).
- Anyone on the Drive page may change it (as in Settings); saved on the tank, so it applies for every viewer. Locked like the other adult settings when child play comes.
- Measured on the stand, one viewer (Plan O): QVGA 13.9 fps 0.3 Mbit/s · VGA 13.9 fps 1.0 · SVGA 13.9 fps 1.4 · HD 8.7 fps 1.6; the limits 5 / 10 give 5.0 / 10.1 fps; at HD 40 pings 0 % loss, 7 ms average, 15 ms worst.

## 95. One master of the Uno link, always on (owner, 2026-10-19)
Found on the Plan O stand check: the WiFi double press switched the network but often gave no jingle / blink. Cause: the bridge was the Uno link's master **only while a phone or page was connected** (bridge LLD R2.7 "Idle", made so the bench tool on TCP 3333 could take over). With no client the bridge stopped its heartbeats: the Uno reported *link lost*, the bridge no longer heard the button, and feedback frames were dropped as *body offline*. A WiFi switch drops every phone, so the feedback depended on timing; every page connect / leave also caused a *link lost → link restored* blink and beep and two Log rows.
- The bench tool and port 3333 are retired (§74), so the reason for Idle is gone.
- **Owner: the bridge is the only master and is always connected.** It starts the handshake at boot and keeps its heartbeats with or without phones. Phones are clients of the bridge only.
- A phone leaving: if it was driving → STOP + driver released (as before); otherwise nothing happens to the link.
- Safety unchanged: driving still needs a connected phone holding the stick; the Uno's deadman (no DRIVE for 300 ms) and link-loss stop (no heartbeat for 300 ms — ESP32 hung / restarting) stay; the IR remote still overrides.
- **OTA (owner: yes):** the bridge still stops being master during a WiFi firmware update; the Uno sees *link lost* while standing still (harmless) and the bridge takes over again after its restart.
- **No extra LED for "no phone connected"** (owner: no).
- No Uno firmware change. Bridge LLD Revision 14.

## 96. Camera low-light mode does more than raise the gain limit (owner, 2026-10-19)
Stand check: switching *low light* showed no change. It only raised the sensor's gain ceiling (4× → 32×), which matters only in real darkness.
- Options: A night mode + 32× gain ceiling + one step brighter exposure; B gain ceiling only (as built); C remove the switch.
- **Owner: A.** Low light on = `set_aec2(1)` (night mode: the sensor may use longer exposures, so the frame rate drops in the dark), `set_gainceiling(32X)`, `set_ae_level(+1)`; off = `set_aec2(0)`, `4X`, `set_ae_level(0)`. Saved with the other camera settings.

## 97. Firmware review: small fixes now, bridge refactor before anything new (owner, 2026-10-19)
A fresh SOLID / coding-standards review of the bridge (and the Uno Lite change) after the Plan O stand checks: `core/` fits the standard (pure, host-tested, no heap / virtual); `net/link_task.cpp` does not — 34 globals, 53 functions, 6 over ~40 lines; it is composition root, application logic and I/O glue at once (standard S and D rules), and none of its rules are host-tested. No safety or hard-rule break; nothing blocks the tank tests.
- **Fix on `plan-o` now:** (1) errors no longer dropped silently (big replies, WiFi mode result queue, NVS saves), (2) a size guard in the log reply, (3) the dead `kStopMaster` path after R14 removed, (4) compile-time checks on the event / log name tables, (5) LLD drift corrected.
- **Owner: refactor before any new feature or work.** One "bridge structure" plan next: split `link_task.cpp` into host-tested `core/` controllers, one owner of the shared acked slot, table-driven command parsing, `ws_json` by page, the task watchdog, generated event names. Logo N2–N4 and the AI phase wait for it.
- **Camera low light (§96) showed no visible change on the stand** — parked until the AI phase (owner): check then whether the OV3660 driver accepts night mode / exposure level.

## 98. Bridge structure refactor — design (owner, 2026-10-19)
Follows §97. Branch `plan-p-bridge-structure`, started from `plan-o` (owner) and merged after it.
- **Why it got messy despite the rules:** the rules fixed the order of work (design → plan → code) but not the shape of the code. Every bridge LLD revision since R3 said "link_task handles X", so each plan approved adding there; the standard's size limits were never checked by a tool; per-plan reviews read diffs, not whole files; and the implementer took the convenient path (globals reachable from one file).
- **Approach (owner: "A with B"):** application controllers in a new `app/` layer, reaching the outside only through small port headers with link-time implementations (as the Uno; standard §1.1); the pure deciders (`LinkOwner`, `ConfirmGate`, `AckedOps`, `EventLog`, …) stay in `core/`. `link_task.cpp` becomes the composition root and loop only.
- **Controllers:** CommandRouter, UnoChannel (one owner of the Uno answer slot), DriveController, MotionController, SettingsController, TankAppController, UnoEventRouter, TelemetryPublisher. Ports: ClientOut, MotionStore, AppStore, WifiControl, CameraControl, SystemControl.
- **Order (owner: ok):** size guard first, `ws_json` by page, generated name tables, ports, command parser by table, then controllers from low to high risk (tank app, settings, telemetry / events, motion, UnoChannel, drive), root + task watchdog last, then stand + floor checks. Drive / STOP rules are pinned by host tests before the drive code moves.
- **Guards:** pre-commit size check (file ≤ 400 lines, function ≤ 40, complexity ≤ 10, allow-list emptied for the bridge by this plan); LLD rule "new behaviour names its controller"; whole-file numbers in every final review (coding standard §3, §3.1).
- Design: bridge LLD Revision 15.

## 99. Plan P built: bridge structure refactor (2026-10-19)
- Done on branch `plan-p-bridge-structure` (Tasks 1–12 + 12a; bridge LLD R15.12 as built): `net/link_task.cpp` 1176 → 215 lines; eight `app/` controllers behind link-time ports; 324 host tests (was 246); the pre-commit size check passes with no bridge exceptions (it blocked three of the executor's own commits during the work).
- Final review: 0 critical, 1 important fixed (a confirmed setting could be half applied when the phones' requests filled the shared queue - now it needs 3 free slots), minors listed in the plan ledger (STOP-waiter TIMEOUT during OTA, reply codes in rare cases, queued requests outliving their phone, docs drift fixed in R15.12).
- **Watchdog (owner: on):** besides the link task and the loop (3 s), core 0's idle-task check stays on, as before Plan P.
- **Tank checks (owner): after the Logo work and the road-wheel fix,** when the motor power is adjusted anyway: R15.11 stand + floor lists and the `CQ_WDT_TEST` restart check. The tank already runs the refactored bridge (`f60fdd5`): `/info` has the same 59 fields, the Uno syncs at boot, the Log works.
- Merge order unchanged: `plan-o` first, then `plan-p-bridge-structure`.
- **Merged (owner, 2026-10-19):** `plan-o` and `plan-p-bridge-structure` merged into `main` before their tank checks (all host suites green on `main`); Plan N1 (Tank Logo reader) starts from `main`.

## 100. Tank Logo after N1: the split of N2-N4, run tests, duplicate inputs (owner, 2026-10-19)
- **Plan N1 merged into `main`** (owner: yes, 2026-10-19).
- **N2 = everything testable on the laptop:** compiler, runner (VM), effect mapping, run states and endings (RunController with a fake motion layer), planned-path simulator — all pure C++ in `src/logo/`, host-tested; no tank needed. **N3 = on the tank:** the logo worker task, saved programs (the `logo` partition, one USB flash), the WebSocket `logo_*` commands, BodyLink `sendTone` / `sendCamSet`, odometry, link-task wiring, stand + floor checks. **N4 = the `/logo` page.**
- **Run tests in the spec:** new `logo/spec/tests/running.yaml` — a program → the list of things the tank is asked to do and how the program ends; generated into C++ tests by `logogen` like the reading cases, so any later runner (the Python one for the AI engine) must pass the same file.
- **The same input name twice (`TO A :x :x`) is refused** with a new code **7032** "{word} has two inputs called :{name} — give each its own name.", at the second one (grammar.md §11: one code per kind of mistake). Done in N2.
- `TILT` needs no decision: the word table already has `TILTED?` (the Uno's tilted / lifted alarm, §92).

## 101. Plan N2 — the Tank Logo runner (2026-10-19)
- Built on the laptop, nothing on the tank yet (§100): compiler, Vm, effect mapping, RunController, simulator (Tank Logo LLD §19). Run tests in `logo/spec/tests/running.yaml` (count: 27), generated into `test_logo_running`.
- **Rules decided while planning (owner reviewed the plan):**
  - `REPCOUNT` is the round of the innermost REPEAT / FOREVER that is running, also from inside a word it calls; 0 outside any loop.
  - `BLOCKED?` is true after a move cut short by the obstacle brake, until the next move, turn, arc or pivot ends.
  - `REPEAT 2.5` runs 2 rounds (the whole part); a negative count is 7025.
  - Moves are whole centimetres (`FD 2.5` → 3 cm); turns keep 0.01°.
  - `=` compares words without regard to case and lists by their items.
  - New codes: **7033** "A number got too big on line n (more than 1000000)." (any result past ±1 000 000); **7034** "The tank's body stopped answering." (link lost, deadman, OTA, body offline). A move that times out or diverges is 7044 (stuck).
  - LED colours (r g b): RED 255 0 0, GREEN 0 255 0, BLUE 0 0 255, YELLOW 255 200 0, PURPLE 160 0 255, WHITE 255 255 255, ORANGE 255 100 0, OFF 0 0 0. LOOK presets (degrees from the centre, right / up = +): LEFT −60 0, RIGHT 60 0, UP 0 30, DOWN 0 −30; HOME = the camera's home. Both in `words.yaml` (`colour_rgb`, `look_deg`).
  - Notes: C4–B6 (equal temperament, A4 440 Hz), lengths 0.25–16 quarter beats (1 = 250 ms), at most 64 notes; a `PLAY` list is checked before anything moves (7029 / 7025). The runner waits for the whole tune; N3 sends it in TONE frames of up to 10 notes.
  - `LED`, `BEEP`, `SOUND`, `LOOK`, `SAY` do not wait; `WAIT`, `PLAY` and every motion do.
- **Reader fixes found on the way:** 7025 for a word without a unit (`REPEAT 20000`, `RANDOM 0`) gave an empty message — templates now have optional placeholders `{unit?}`; IFELSE's inputs were read by YAML as `True` / `False` (now `"yes"` / `"no"`, so its message says "steps in [ ]"); a block can hold more than 255 statements (`Node::count` 16-bit); the same input name twice is 7032 (§100).
- **Final review fix:** a word that gives back a value but ends with a call of a command word crashed the runner (the call was treated as a tail call and left without a value). A tail call now needs both words of the same kind; that program ends with 7031. Second guard: taking a value from an empty stack ends the run with 7014. Deferred to N3: Program "ok" flag (never run a failed compile), stack sizes on the tasks, rate-limiting effects that do not wait (`FOREVER [ LED RED ]`), a deeper value stack (so 100 calls of a 3-input word reach 7011, not 7014), one WAIT UNTIL deadline per wait.

## 102. Tank Logo N3: program text over HTTP, control over the WebSocket (owner, 2026-10-19)
- **Owner: option A.** `POST /logo/check` carries the program text; a low-priority worker task reads, checks, compiles and plans the path, and the one reply carries the errors, your words, the planned path and a **program number**. `logo_run {prog}` on the WebSocket runs exactly that compiled program (if another check replaced it: "check again"). STOP, step, the line, the pose, SAY and the ending stay on the WebSocket; saved programs are plain HTTP (list / load / save / delete).
- Why: the link task (2 ms loop, STOP path) never handles program text and its messages stay ≤ 200 B; the tank runs what the child saw checked; big replies need no splitting. Changes Tank Logo LLD §13 (was WebSocket only) - the N3 LLD revision says how.
- **Owner: pacing option A.** A program's LED / BEEP / SOUND / LOOK / PLAY (TONE) waits for the Uno's ACK before it goes on (paces itself, about 30-50 a second at most; no flooding of the 8-slot request queue); no ACK within the request queue's 0.6 s backstop ends the program with 7034; a refusal (e.g. camera locked) is logged and the program goes on. SAY goes to the page and is followed by a fixed 0.2 s pause.
- **Tank Logo LLD §20 approved (owner, "write the plan"),** including: the driver's own `drive` / `turn` / `move` during a program ends it (7045); `BEEP` = SOUND BLIP; no saving / deleting while a program runs; OTA never starts during a program (it ends it first, 7034); encoders from the Uno's HELLO caps.

## 103. The tank's source archive was nearly full (2026-10-19)
- The bridge packs `git archive HEAD` (gzip) into the 1.875 MB `source` partition (bridge LLD R2.3). On `main` it reached 1,952,601 of 1,966,016 bytes; the N3 plan or N3's code would have broken the bridge build ("pack_source: does not fit").
- Fix: `docs/hardware/datasheets/**` is `export-ignore` (third-party PDFs, available online; they stay in git). The archive drops to about 1.5 MB. Nothing of our own work leaves it.

## 104. Plan N3 confirmed (owner, 2026-10-19)
- Owner: "confirm and native" — Plan N3 (`docs/superpowers/plans/2026-10-19-plan-n3-logo-on-tank.md`) and its ten rules are confirmed: async `/logo/*` jobs on the worker; the program keeps the drive with `keepDriving`; the ending's STOP only while it still owns the drive; bridge codes 3016 CHECK_AGAIN / 3017 NOT_RUNNING; LOOK → servo angles (900 centre, right / up smaller), BEEP = BLIP; PLAY in timed TONE chunks; batt_critical → 7042, tilted / lifted → 7043; memory as built; source offset from the partition table, flash partitions first; Log kind `logo`, the smoke tool pings. Executed natively; stand checks (Task 12) with the owner; floor checks (Task 13) after the road-wheel fix.

## 105. Plan N3 stand checks (owner present, 2026-10-19)
- **USB flash** of `plan-n3` (dca8fe3) with the new layout: bootloader, partitions (app0 / app1 2.875 MB, source 0x5D0000, logo 0x7B0000), boot_app0, app, source — every hash verified; boots from app0; `logo: worker ready (319 KB PSRAM)`, `logo: program store mounted`; NVS kept (WiFi, counts per metre 7514, roll-on 27 mm); Uno synced. The boot line `nvs_open failed: NOT_FOUND` is the Plan O AI-address load (namespace `cq_app` never written) - harmless, not N3. Note: opening the bridge's USB serial port resets it - the Log tab (WebSocket `log`) is used during runs instead.
- **Checks:** examples listed; the square checks OK (9 points back at the start); errors 7001 / 7025 with line and column. **An ~8 KB `FOREVER` check takes 41-43 ms** on the bridge (≈ 0.3 s round trip with its 1332-point reply) - far below the 3 s core-0 watchdog (Review Focus 5).
- **STOP during checks (owner):** the phone's STOP stayed **instant** while 202 heavy checks ran back to back (all answered OK) - Review Focus 1.
- **Runs on the stand:** `REPEAT 2 [ FD 30 BK 30 ]` finished; LED / BEEP / SOUND / PLAY / LOOK / SAY all done (owner saw and heard each); a saved program run by name finished. The square ended 7024 once (something in front of the sensor) and 7044 once (a spin cannot turn the body on the stand - the gyro sees no turn, the motion service ends it "stuck" after 1.5 s, as a page's turn does there): **turns are floor checks** (Task 13).
- **Endings:** a waiting program kept the drive for over a minute (no 5 s handback); `logo_stop` from a second connection → 7040; the phone's STOP → 7040 (Log: stop_hold); the smoke tool killed → 7046 within ~3 s; the 5-minute limit → 7041 (twice). A second phone could not take the drive while a program ran (tracks stayed still) - by design, only the IR remote can override; **the IR takeover check (7045, no STOP) is still to do** (no remote at hand).
- **Saved programs:** save / list / load / run by name / delete ok; save while running → RUNNING; delete an example → EXAMPLE; `../x` → NAME.
- Left for the floor (Task 13, after the road-wheel fix): the 50 cm square back to its start, turns, `avoid_walls` with a box, a program without `BLOCKED?` meeting a box (7024), the IR takeover.

## 106. Tank Logo N4 — the `/logo` page: first decisions (owner, 2026-10-19)
- **Why our own language (owner question):** Tank Logo is a Logo dialect (Logo, 1967), not a new language; ready-made relatives exist (UCBLogo / FMSLogo / Turtle Academy draw on a screen; Scratch / Blockly; MicroPython / Lua on an ESP32). We built our own version because each word must become an existing tank move under the Uno's safety rules (no new Uno firmware, no new safety rules, §20); the program is checked, explained in a child's words and its path drawn **before** anything moves, and words the tank cannot do are refused clearly; it must fit the bridge's firmware rules (fixed memory, time limit, step mode, never blocking STOP); and the owner chose no Python on the board, one language in three levels (§83, §915). Cost: we maintain the language ourselves.
- **No design spec for N4:** the page is already designed (screens 15 / 16, bridge UX §2, Tank Logo LLD §14); N4 gets an LLD revision §21 that fits §14 to the N3 transport (§102), then a plan.
- **No speed-limit control on the run bar in v1 (owner: option A):** programs set their own speed with `SPEED`; no bridge change in N4.
- **Unsaved changes are kept as a draft on the device (owner: option A):** the page keeps what is typed in that browser (phone or laptop) and restores it when the page opens again; a dot by the program name shows "not saved on the tank". No warning dialog, no automatic writes to the tank's flash.
- **No PAUSE button in v1 (owner: option A):** the run bar keeps RUN (straight through), STEP (starts the program in step mode; each press runs the next line) and the big STOP. A program cannot switch into step mode while it runs, so PAUSE would need a bridge change; N4 stays page-only. The SPD control of screens 15 / 16 is gone too (above). The screens' heading `-90°` after a right turn is superseded by §83 (right = positive: `RIGHT 90` → heading 90).
- **Design approved in three parts (owner, 2026-10-19):** (1) one page `/logo`, words built in, HTTP check 600 ms after the last key, run by check number over the WebSocket, programs on the tank (rename = save + delete, examples read-only), "a program is running" from another device; (2) the child-friendly editor (colour by group, tap-to-insert with the value selected, levels, mistakes under the line, locked while running) and the path view (planned dashed after a sensor read, driven on top, fits itself); a `group` per word in `words.yaml`; (3) tests: the page's deciding code in `web/logo.js` with Node's built-in test runner, generator test, budget 24 KB, Playwright browser check, stand checks. Written down as Tank Logo LLD §21.

## 107. AI engine reopened: smooth navigation, sensors (owner, 2026-10-20)
- **Research first (owner):** what hobby / open-source robots do with LLMs and VLMs, and what runs on the laptop (Apple M1, 16 GB): `docs/research/2026-10-20-ai-landscape.md`. Spec 04 will change — the robot changed a lot (closed-loop moves, Tank Logo, no hub).
- **Goal (owner):** the tank must **drive smoothly and navigate a room**, aware of its surroundings — not "drive, stop, take a picture, think, drive" (the owner built that earlier: slow and boring). So the language model is not in the driving loop; a fast loop on the laptop (camera → objects + depth → a local map → steering → continuous drive commands) drives, and the model only sets goals.
- **Sensors (owner):** design with **today's sensors** — camera (OV3660, pan/tilt), forward ultrasonic, gyro (MPU6050), track encoders. If a ToF is added later (single beam or 8×8 multizone), **the ultrasonic is removed** (no pin for both). Noted for that day: the Uno's obstacle brake must keep a sensor of its own that does not depend on WiFi or the laptop; an I2C ToF shares the gyro's I2C pins on the Uno, but the Uno has ~1.2 KB flash left and an 8×8 ToF loads ~84 KB of firmware at start (so it would sit on the ESP32 or a future body board).
- **Today's facts for the design:** telemetry 50 Hz (encoders, heading, rates), ultrasonic 20 Hz, camera MJPEG ~10-20 fps, 0.1-0.3 s old on arrival; **frames carry no time stamp yet** (needed to pair each picture with the tank's pose).
- **Three modules (owner, 2026-10-20):** (1) sensor fusion + space map (where am I, what's around me), (2) drive the tank (a local planner sending speed + turn to DRIVE_RATE 10-20 times a second; the Uno's rules underneath), (3) plan (A* path, behaviour tree, frontier exploration; a language model only on top, turning words into goals). **Built as our own small Python engine on the laptop** using the industry methods (filter, occupancy grid, DWA, A*, behaviour tree, frontiers) — not ROS 2 / Nav2 (owner: option A). Module 1 is designed first.
- **Module 1 approach approved (owner, 2026-10-20):** a pose filter (encoders + gyro, 50 Hz) with a short pose history; YOLO + tracker with the **floor trick** (an object's floor contact row → distance from camera height / tilt / lens); free floor from Depth Anything + a fitted floor plane; the ultrasonic as "free up to d, blocked at d" and as a depth scale anchor; a 3 × 3 m grid around the tank (5 cm cells, old marks fade) plus a list of objects with room positions; one-time calibrations (lens checkerboard, camera mount, servo → angle); **the bridge adds a time stamp to each camera frame** (small bridge change). Optical flow (stuck detection) second; wall markers and room memory later. Next: one more research pass on doing it better, then the module 1 spec.
- **Research changes accepted (owner, 2026-10-20):** the bridge stamps Uno telemetry with the ESP32 timer on UART arrival (same clock as the camera's frame stamp) — replaces the plain "time stamp per frame"; the floor is the main depth scale anchor (ultrasonic = cross-check + "blocked at d"); obstacles by the expected-floor-depth test; the ultrasonic never marks free on "no echo"; mark to ~1.5-2 m, clear to ~3 m, fade only in view; gyro bias re-learned at standstill; blurred frames skipped while turning fast; objects tracked in room coordinates; record and replay (Rerun) from day one; YOLOE and a metric indoor depth model as experiments; thin / low obstacles need the later ToF or a line laser (`docs/research/2026-10-20-module1-mapping.md`). Written as **Spec 7** (`docs/superpowers/specs/2026-10-20-07-ai-module1-map-design.md`, draft for owner review).
- **Spec 7 approved (owner, 2026-10-20).** Module 1 has no product screen (only the Rerun live view for us), so no UX step; next the LLD `docs/lld/ai-engine-lld.md` (draft): `ai-engine/` project layout, bridge revision R17 (`bus_us` on `td` / `tf` stamped on UART arrival, `X-Frame-Us` / `X-Frame-Seq` on MJPEG parts, `bridge_us` in `pong`), classes and diagrams, the algorithms with their parameters, concurrency (one inference worker, latest frame wins), calibration tool, recording / replay, tests, phase 0 tools. The engine reads only `td` at 50 Hz (it already carries encoders, heading, rate, distance, pan / tilt, command, flags).
- **AI engine LLD approved (owner, 2026-10-20: "write the plan").** First plan: **Plan Q0 — phase 0 measurements** (`docs/superpowers/plans/2026-10-20-plan-q0-ai-measure.md`). Drafting it measured the Neural Engine budget on the owner's M1 (16 GB): YOLO26n via CoreML 4.4 ms at 320 px / 10.6 ms at 640 px; Apple's CoreML Depth Anything V2 Small 32.7 ms; both one after the other 41 ms (320) / 46 ms (640) → 22-24 maps per second possible (target ≥ 10). Tooling facts: coremltools 9 needs torch 2.7 and numpy < 2.3 for the YOLO CoreML export.

## 108. Plan N4 — the `/logo` page (2026-10-20)
- **Rules confirmed by the owner at plan review ("native"):** the thirteen rules of `docs/superpowers/plans/2026-10-19-plan-n4-logo-page.md` — mistakes under the editor, one `/logo` request at a time, the step-mode pause rule, drafts never hidden, SAVE AS asks before replacing, level-3 words "later", sense / maths colours, the short ☰ MENU, Help's Logo section for v1, `hello` client `logo` and `?name=`, `pack_web` loadable, the hook fix, no `http_server.cpp` change.
- **Built:** `web/logo.html` + `web/logo.js` (46 Node tests); `/logo` 20.0 KB gzipped. The final review (fresh reviewer) found STOP could scroll off a phone's screen or sit under a sheet (critical), a CHECK_AGAIN rerun surviving an edit, SAVE dropping text typed while it waited, opening a program before its text arrived, DELETE clearing a new program without asking, per-key work, and no request timeout — all fixed test-first (browser checks kept in `web/test/`).
- **Tank checks (owner present, mostly on the floor):** the OTA flash; the page from the Drive menu; a mistake shown and fixed, the planned path; RUN, STEP (NEXT stays greyed while the tank moves), STOP mid-move (7040); a hidden page ends the run (7046); a second device sees "a program is running", its STOP ends it; save / rename / draft after a reload / an example asks for a new name / delete asks / the list matches on a second device; an 8 KB program types smoothly. **All pass.**
- **Bug found on the floor and fixed (N3 code):** `FD 30 BK 30` × 2 was drawn as 120 cm forward — the Uno counts rising encoder edges only (counts rise in reverse too) and the Logo odometry assumed falling counts. The direction now comes from each track's motor command; re-run on the floor: drawn up and back to the start (owner: "ran perfectly"). Tank Logo LLD §21.10.
- **Seen on the floor, separate issue:** a square ran FD / RT three times, then the third turn wobbled around its target and ended "A track got stuck" (7044). The page was right (heading −90° = 270° after three right turns). The turn controller needs a recorded diagnosis — likely the left road wheel / motor power (both pending).
- **Owner feedback:** SAVE + RENAME are fine (SAVE asks for a name only for a new program or an example — examples are read-only); undo (Cmd-Z) is fine.

## 109. Plan Q0 — phase 0 measurements (owner present, 2026-10-20)
- **Neural Engine budget (owner's M1 16 GB, CoreML):** YOLO26n 4.1-4.4 ms at 320 px, 10.6-12.3 ms at 640 px; Depth Anything V2 Small 31-33 ms; both one after the other 39-46 ms → **22-26 maps per second** possible (target ≥ 10). Tooling: coremltools 9 needs torch 2.7 and numpy < 2.3.
- **Camera mount:** lens **17.8 cm** above the floor (7 in), about 2.5 cm behind the front of the tracks; **blind zone 25.4 cm** from the front of the tracks (27.9 cm from the lens; the floor strip as the marker); the bottom edge of the picture looks 32.5° below level → the camera points about 4-9° down (the lens calibration fixes it). Bright daylight gives a **pink / magenta cast** (no IR filter or white balance) — noted for the camera settings; a dark room is a low-light case.
- **Picture age on arrival** (screen-flash method, lights on, 20 flashes each): **QVGA 11.8 pictures/s, median 131 ms** (91-227); **VGA 6.9 pictures/s, median 156 ms** (108-245). → `camera.size` **QVGA for driving**; VGA only for "look closely". (The owner's camera stays on VGA otherwise.) These ages are **upper estimates** (review): they include the laptop screen's own delay (~16-50 ms) and up to one frame of waiting; the minimums (91 / 108 ms) are nearer the fixed latency — Plan Q1's engine clock measured a median of 94-96 ms.
- **Depth vs tape (wood, a ~10 cm box, distances from the lens):** Apple's CoreML output is **inverse depth** (plain-depth fit far worse). One fit across the four pictures: 0.3 m −0.7 %, 0.6 m +2.3 %, 1.0 m +8.8 %, 2.0 m −16 %. Pessimistic: without the 2.0 m picture (a different view) the fit is 1/d = 2.877 r + 0.503 with +0.1 / −0.4 / +0.5 % (review re-fit); Depth Anything scales each picture on its own and the 2.0 m picture was a different view — the engine fits the scale per frame on the floor (Spec 7). Measured with `depth_point.py` (the depth at the box's own pixel: the middle of the picture is above a low box). Rug skipped (owner).
- **Ultrasonic:** on the box 296 / 570 / 963 / 1985 mm at 0.3 / 0.6 / 1.0 / 2.0 m (−1 to −5 %, every reading valid). At ~50 cm: hard wall 490 mm (no echo 0 %, spread 4 mm), wall at 45° 402 mm (0 %, the nearest part of the beam), curtain 438 mm (0 %, spread 69 mm), chair leg 484 mm (0 %), soft toy 501 mm (0 %, spread 9 mm). Sofa skipped (none). Every surface seen at 50 cm **after the Uno's filter** (median of 3, a missing echo keeps the last distance until 3 misses in a row; `td` repeats each 20 Hz ping) — single misses are hidden, which is also what the engine gets; the "never clear on no echo" rule stays as a safety net. The tape was laid from the lens; the sensor sits a few cm further forward, so part of the −1 to −5 % is that offset.
- **Gyro at rest (5 min, 15 008 samples):** heading constant (rate 0.0): **the Uno's GyroZeroLite re-learns the bias at rest and holds the heading** → the AI engine's pose filter can rely on it (its own bias state is optional). Drift while moving needs a drive-and-return test after the turn fix.
- **Engine parameters set:** `camera.size` QVGA; `depth.every_n` 1 (the budget allows it); `gate_dps` 45 (kept); sonar beam half-angle 12° (kept); `readout_us` measured with Plan Q1's frame stamps. Spec 7 §7 targets stand.
- **Not measured:** the OV3660 readout time (needs Q1's stamps), the rug, the sofa, drift while moving.
- **Explanations (owner questions, 2026-10-20):**
  - **QVGA** = 320 × 240 pixels ("quarter VGA"); **VGA** = 640 × 480. Same view, a quarter of the pixels: quicker to send and to process.
  - **How the camera tilt was worked out:** the bottom edge of the picture meets the floor 27.9 cm ahead of the lens, 17.8 cm high → it looks atan(17.8 / 27.9) = **32.5° below level**. The bottom edge is half the picture's height below the camera's centre line; for an OV3660 lens 60-72° wide that half is 23-29°, so the centre line points **4-9° down**. The spread is only the unknown lens width — the lens calibration (checkerboard, Spec 7 phase 2) makes it one exact number.
  - **The gimbal:** the camera sits on the pan / tilt servos. **The design already uses them** (the engine reads `pan` / `tilt` from telemetry with every picture; the camera model and the servo → angle calibration, AI engine LLD §5.3 / §8). **Q0 measured only at the tilt's centre:** the 25 cm blind zone is for that position.
  - **Tilting further down while driving** shrinks the blind zone and still sees the room (rough, for a 66° lens): centre (~7° down) ~25 cm from the tracks; ~15° down ~18 cm (far walls still in view); ~25° down ~12 cm (the horizon near the top edge). **Open decision** for after the lens calibration: the driving tilt (recommendation then: about 15° down).
## 110. Plan Q1 — one clock, recording and the live view (owner present, 2026-10-20)
- **Built:** bridge R17 (one ESP32 clock: `bus_us` on `td` / `tf`, `X-Frame-Us` / `X-Frame-Seq` on camera parts, `bridge_us` in `pong`) and the AI engine's base (`ai-engine/`: `cq-record`, `cq-replay`, the Rerun live view; read only). The plan's nine rules were confirmed by the owner at plan review ("nativ").
- **Final review (fresh reviewer):** the camera stream is HTTP chunked and the engine did not remove it (every picture would have been broken — critical); Ctrl-C lost the summary; a stale clock after a restart; a cut-off recording could crash replay — all fixed test-first; the six minors fixed at the owner's request ("fix the minor one").
- **On the tank:** OTA flash (the owner switched the tank off right after; it booted fine); stamps on the wire on one clock; the phone's camera unaffected; recording standing still 13.8 pictures/s, 50 telemetry/s, all whole JPEGs, picture age median **96 ms** by the engine's clock; recording while driving **8.2 pictures/s** (phone also viewing); replay OK. **Bug found and fixed:** after a bridge restart the camera came back 1.5 s before the WebSocket and pictures got 12.7 s ages — a capture time jumping back now drops the clock at once; re-run: max 336 ms.
- Notes for the map plans: the Uno already re-learns the gyro bias at rest (§109); pictures come slower while driving; the live view's viewer holds the terminal pipe until closed.

## 111. Two lasers for calibration and testing (owner, 2026-10-20)
- **Owner bought:** a **laser pointer** (single dot) and a **laser level** (projects straight lines / a cross on the floor and walls).
- **Planned uses:**
  - **Laser level, straight driving:** a line along the floor as a 3 m straight reference — sideways drift visible to the millimetre (the camera can measure the offset too).
  - **Laser level, squares and turns:** its cross as an exact 90° reference on the floor.
  - **Laser level, camera tilt:** a horizontal line on a wall **at exactly the lens height (17.8 cm)** is the horizon in the picture — its pixel row gives the camera's tilt directly (with the lens calibration's focal length) and checks the servo → angle table. Goes into the lens-calibration plan (Spec 7 phase 2).
  - **Pointer on the tank (taped, pointing forward), heading checker:** the dot on a wall 2-3 m away magnifies heading errors (1° ≈ 5 cm at 3 m) — 360° spins back to a mark, the turn wobble seen directly next to a 50 Hz recording, drift after a lap. Goes into the turn-wobble diagnosis.
- **Not covered:** a structured-light obstacle sensor (cables, chair legs) needs a small **line-laser module** on the tank — a cheap later add-on if the camera alone struggles with thin obstacles (research `2026-10-20-module1-mapping.md`).
- **Safety:** low-power lasers only (class 2, < 1 mW); never at eyes or faces; dot and lines at floor / wall height — especially with the child around.

## 112. The turn wobble - recorded diagnosis (owner on the floor, 2026-10-21)
- **Recorded:** three 90° spins (`tools/smoke/turn.py`, speed 40) with `td_record.py` at 50 Hz alongside: right, right, left.
- **Results:** right −87.6°, right −87.5°, left +91.0°. Each turn: the approach takes ~0.9 s to about 72-73° at ~90 °/s; the command then falls to the minimum (12 %), which is still **PWM ≈ 109 (the motor deadband)**, so the tracks keep **~50 % of their speed** and the tank **coasts past the target by 3.5-3.8°**; then it **wobbles** — every correction is again the minimum power, which swings the tank 4-6°, more than the done window (enter ±1.5° while nearly still, stay ±2.5°) — for **1.9-6.6 s** before it settles 1-2.5° off. In the floor square one wobble lasted long enough to end "a track got stuck" (7044).
- **Not the cause:** the left road wheel (left and right tracks equal: 56-57 % mean speed in all three turns; left and right turns overshoot alike), nor the gyro (smooth, steady readings).
- **Cause:** the turn controller's ending — the smallest correction it can make is bigger than the accuracy it asks for. **Fix (owner: "do the two turns then write R18"):** bridge LLD revision R18 — stop early by the learned coast, at most one correction, done when stopped and within ±3°.
- Pushes per turn (direction changes + 1): right 10, right 8, left 25. **R18 approved (owner, 2026-10-21) with the owner's addition: slow down smoothly as the target nears — as far as the motors allow (the slowest smooth spin, 65 °/s, under rate control) — then the learned cut** (bridge LLD): approach, cut at the predicted stop (learned `T_coast`, default 60 ms, NVS `cq_motion/coast_ms`), coast, at most one nudge, done when stopped; spin TURN and PIVOT (arcs unchanged).
- `tools/smoke/turn.py` prints a wrong end position for spins (both encoders count up — the same blind spot as the Logo odometry, §108); its heading is right.

## 113. Plan R1 smooth turns - floor results (owner on the floor, 2026-10-21)
- **Method:** the bridge was flashed over WiFi with Plan R1 (bridge LLD R18). The learned coast started at its default, 60 ms. Each turn was sent with `tools/smoke/turn.py` (speed 40) while `td_record.py` recorded at 50 Hz. Pushes = times the Uno's command went from 0 to turning; a wobble = a change of turn direction faster than 15 °/s; the ends are `turn.py`'s gyro heading change. The square was `REPEAT 4 [FORWARD 30 RIGHT 90]`, run with `tools/smoke/logo.py run`, the same recording alongside.
- **Turns in order** (the coast learning a quarter of the way after each done turn):

  | Turn | End | Pushes |
  |---|---|---|
  | R90 | −80.4° | 2 |
  | R90 | −82.9° | 2 |
  | R90 | −85.7° | 2 |
  | L90 | +84.4° | 2 |
  | L90 | +91.3° | 2 |
  | L90 | +92.3° | 1 |
  | 360° | +362.0° | 1 |

  From the sixth turn on: **one push, no nudge, no wobble.** Before R1 (§112), every turn made 8–25 pushes over 1.9–6.6 s.
- **Logo square:** finished with **no 7044**. Each corner turned with one push and no wobble: 91.7°, 91.7°, 91.5°, 91.6°. The program ended 7.1° off its start heading (4 × ~1.7°).
- **Why the first turns overshot (read from the recording):**
  - The Uno ramps its command down about 4–8 % per 20 ms frame, so after the bridge's w = 0 the motors still drive for ~60–80 ms; then there is the 40 ms link delay and the glide itself.
  - Turn 1 was cut at −86.6° turning 81 °/s and glided on to −98.4° (11.7° ≈ 144 ms of turning). Its nudge back was cut at 66 °/s and glided 10.8° (≈ 164 ms).
  - So this floor's real coast is **~150 ms**, not the 60 ms default. The nudge, cut with the same short coast, overshot back the other way: that is why the first turns ended *short*.
  - The learning closed the gap in about five turns, as designed.
- **Slow zone:** the rate loop held ~80 °/s, not 65. The command needed for 65 °/s is lower than the feed-forward assumes, and the integral pulls it down only slowly. Harmless: the cut works at any rate.
- **Left over:** a steady ~1.5–2.3° overshoot once learned. This is the cut-timing bias the final review noted (R18.5 / ledger minor 1): the cut fires only after the predicted stop has passed the target (0–1.3° at 50 Hz), and the Uno's ramp adds a little more.
- **Open for the owner:**
  - (a) Default coast 150 ms instead of 60, so a fresh bridge or a reset does not start with five long turns.
  - (b) Cut half a frame earlier (or subtract a learned bias) to centre the ~1.7°.

## 114. R18.7 on the floor - a 150 ms default coast and a half-frame cut lead (owner on the floor, 2026-10-21)
- **Method:** the same as §113. The bridge was flashed with R18.7; then the owner reset the roll-on in Settings → Tune, confirmed with the tank's button, which put the learned coast back to its new 150 ms default and the roll-on to 25 mm. Each turn was sent with `tools/smoke/turn.py` (speed 40) and recorded at 50 Hz with `td_record.py`; the square was `REPEAT 4 [FORWARD 30 RIGHT 90]` with `tools/smoke/logo.py run`.
- **Turns, straight after the reset:** R90 −90.3°, R90 −90.7°, L90 +90.1°, L90 +91.0°. **Every one took one push, with no nudge and no wobble**, including the very first turn. (Before R18.7 the first five turns overshot and nudged: −80.4° … +91.3°, §113.)
- **Logo square:** finished; the heading at the start of each side: 0.0°, −89.4°, −179.4°, +90.3°; the program ended **0.9°** off its start heading. Before R18.7 it was 7.1° (§113).
- **How the numbers were read:** the turn ends are `turn.py`'s gyro heading change; pushes = times the Uno's command went from 0 to turning in the recording; the square's corners are the yaw (`td` column `yaw`, relative to the first row) on the first frame of each forward side; its end is `td_record.py`'s heading change.
- **Conclusion:** the turn ending is solved for the floor tried. The 1.5-2.3° overshoot of §113 came mostly from the 60 ms default still being learned, plus the half-frame cut delay.

## 115. The pending tank checks no longer wait for the road wheel (owner, 2026-10-21)
- **Owner:** "wheel fix will take time so let go that way, assume the wheel will not get fixed." The broken left middle road wheel (§64, §69) stays as it is. Every check that waited for it is done now, on the tank as it is.
- **What changes:**
  - **Straight driving:** `track_balance` 19 ‰ stays. It is the correction for the wheel's drag, not a stopgap. Re-check `move 200` for ±1 cm instead of re-measuring a repaired tank.
  - **Arcs (Plan L):** re-test 90° L/R at R 60 and R 100 now. If they are still outside ±5 cm (§71: left 6 cm outside, right 10 cm inside, the tank sliding right), an arc correction for the sideways slide becomes a design item, with options for the owner. It is not a reason to wait.
  - **Plan O wizard:** calibrate counts per metre on the tank as it is (it is the tank that drives).
  - **Plan P** (R15.11 stand + floor lists, the `CQ_WDT_TEST` watchdog check) and **Plan N3** floor checks (the square back to its start, `avoid_walls`, a box stop → 7024, the IR takeover): now.
  - **Motor power** stays a separate, open item.
- **Order:**
  1. Plan P stand list + the watchdog (stand).
  2. N3 safety: the IR takeover and the box stop (floor).
  3. `move 200` and the Plan O wizard (floor).
  4. The arc re-test (floor).
  5. The N3 square and `avoid_walls` (floor).

## 116. Pending tank checks on the floor (owner, 2026-10-21; the order of §115)
- **N3 IR takeover:** skipped. The remote could not be found; it stays pending.
- **N3 box stop - pass:** `FORWARD 100` with a box ~60 cm ahead, run with `tools/smoke/logo.py run` and recorded with `td_record.py` at 50 Hz. The program ended **7024 "Something is in the way on line 1"**. The tank drove 45.8 cm (the encoders: the mean of the left and right counts / 75.14 counts per cm) and stopped **12.9 cm from the box** (the ultrasonic `dist_mm` at the end: 128-129 mm), without touching it.
- **`move 200` with the wheel as it is - pass:** `tools/smoke/move.py 200 --speed 40`. The bridge counted 198.0 cm at the stop and 200.2 cm after the roll-on, heading change 0.0° (15040 counts at `counts_per_m` 7514). **Measured by the owner with a tape, from the start mark to the front edge: 200 cm, 1 cm to the right of the line.** So `track_balance` 19 ‰ and `counts_per_m` 7514 stay as they are (§115).
- **Plan O wizard - pass:** Settings → Tune → DRIVE 1 m on the phone; the owner measured 100 cm with a tape, the wizard showed **7514**, SAVE asked for the tank's button, and the bridge's `calib` reply afterwards: `counts_per_m` 7514 (read with `move.py`'s `Session` over WebSocket). The learned roll-on, reset in §114, now reads 22 mm (2 readings from 6 moves watched).
- **Plan L arc re-test with the wheel as it is - pass, all four within ±5 cm** (closes the deferred arc acceptance of §71):
  - **Method as §71:** the owner marked C, the arc centre, 51 cm out from the outer edge of the track on the arc's side (60 cm from the tank's centre; 91 cm / 100 cm for R 100). They measured C → that track's outer edge before and after each 90° arc. Each arc was sent with `tools/smoke/turn.py ±90 --radius R --speed 40`.
  - **Results:**

    | Arc | C → track edge | Heading (owner) | Gyro | Encoder estimate |
    |---|---|---|---|---|
    | Right R 60 | 51 → **51 cm** | ~90° | −95.8° | x 60.7 y −60.9 |
    | Left R 60 | 51 → **54 cm** (3 out) | ~90° | +92.5° | x 62.4 y 62.4 |
    | Right R 100 | 91 → **88 cm** (3 in) | ~90° | −92.1° | x 100.8 y −101.1 |
    | Left R 100 | 91 → **92 cm** (1 out) | — | +90.9° | x 108.2 y 108.2 |

  - **Before (§71, R 60):** left 6 cm outside, right 10 cm inside.
  - **The encoder estimate is not a path measure:** it said 8 cm outside for the left R 100 where the tape said 1 cm. It cannot see sideways slide (§71); the tape is the judge. No arc correction is needed (§115).
- **N3 square back to its start (`REPEAT 4 [FORWARD 50 RIGHT 90]`):** finished.
  - **The gyro** (`td` yaw, 50 Hz, relative to the first row) turned the four corners −90.4°, −90.5°, −89.7°, −89.9° and ended at **+0.1°**.
  - **The owner's tape:** the front edge 3 cm ahead of its mark, the left track's edge 4 cm right of its mark, and the tank visibly **facing right**.
  - **From the owner's photo:** the tracks against the floor's straight inlay strip are about 20 px closer at the rear than the front over a ~330 px track, so **~3–4° to the tank's right** (estimate; the photo is slightly oblique).
  - **Finding: the gyro under-reads by about 1 %.** The tank really turns ~91° when the gyro says 90°. The turn controller stops where the gyro says, so the error is the gyro's scale, not the turn ending; the MPU6050's factory scale tolerance is about ±3 %. The R18.7 square's "0.9°" (§114) was gyro-measured, so it was probably about 4° in reality.
  - **Next (owner: go):** measure the scale with 4 full spins (1440° by the gyro) against the strip, left and right. Then a bridge-side gyro scale setting (LLD revision first; the Uno is 96 % full).
- **Gyro scale measured with 4-turn spins (the owner's photos, straight down, the tank lined up with the floor's inlay strip):**
  - **Method:**
    - The owner lined the tank's right track along the strip, its rear against the crossing strip, and took a start photo.
    - Two 720° spins were sent with `tools/smoke/turn.py ±720 --speed 40`; 720° is the protocol's limit per turn, and a 1440° turn is refused unanswered.
    - The owner took an end photo; the tank's angle to the same strip was read from the photo.
    - The phone was held at a different angle for the end photos, so the angle was always read against the strip, never the picture's edges.
  - **Left:** the gyro summed 719.9° + 720.2° = 1440.1°. The end photo: the strip at ~33° from the picture's vertical, the tracks ~6-7° the other way, so **~39° further left**: really ~1479°, **the gyro ~2.7 % short**. The owner's eye said ~45°.
  - **Right:** the gyro summed 719.7° + 719.4° = 1439.1°. The end photo: the strip at ~34° below the horizontal, the tank ~6° below, so **~28° further right**: really ~1467°, **~1.9 % short**.
  - **Reading:** both directions under-read, so this is the gyro's scale: **~2.3 % on average, scale ≈ 1.023**, inside the MPU6050's ±3 % tolerance. The left / right gap (39° vs 28°) is within the photo-reading error (about ±5° each, slanted photos); a small motion offset cannot be ruled out. The square's 3-4° estimate above was rougher still.
  - **Effects:** a Logo `RIGHT 90` really turns ~92°, a square ends ~8° off, and the AI map would bend.
  - **Decision (owner: "yes write R19 for option a"):** correct it on the bridge, where the Uno's heading arrives (bridge LLD R19), not in the Uno (96 % full, USB-only updates).

## 117. Plan R2 - the gyro scale on the floor (owner, 2026-10-22)
- **Method:** the bridge was flashed with R19 over WiFi. The scale was saved with `tools/smoke/move.py --gyro-scale N`, confirmed with the tank's button, and read back from the `calib` reply. Then, as in §116: the tank was lined up with the floor's inlay strip, two `turn.py ±720 --speed 40` spins were run, and the owner took start / end photos from straight above. The angle was read against the strip and the owner judged it by eye.
- **Scale steps** (degrees too far over 4 turns):

  | gyro_scale | Left | Right | Notes |
  |---|---|---|---|
  | 10000 (none, §116) | ~39° | ~28° | |
  | 10230 | ~15° | ~10–15° | both still too far: the scale was too low |
  | 10330 | slightly too little | — | owner: "almost there, reduce by half percent" |
  | 10280 | — | ~3–5° too far | owner: the first 720 almost perfect, the second a bit over |
  | **10310** | — | — | **kept**: between left (wants < 10330) and right (wants > 10280) |

  - At 10280 / 10330 left and right differ by about 0.1-0.3 %, a small fixed gyro error while turning that one scale cannot remove. It is below the ±1° a single turn ending varies by.
  - The scale that fits this tank (**×1.031**) is above §116's photo estimate (×1.023): the first photos were read at changing phone angles. A higher scale makes the tank turn less, because the bridge counts the turn as bigger and stops sooner.
- **Logo square** (`REPEAT 4 [FORWARD 50 RIGHT 90]`, 10310, recorded at 50 Hz): finished.
  - The corrected gyro turned the corners 91.3°, 90.9°, 90.5°, 91.6° and ended 4.3° right. The ~1° per corner is the R18 turn coast re-learning after the scale change (the corrected rate reads ~3 % higher), shrinking corner by corner.
  - **The owner: "almost straight", the end 1.5 cm to the right of the start mark.** Before R19 (§116) it ended 3 cm ahead, 4 cm right, visibly facing right.
- **Saved on the tank:** `gyro_scale` 10310 (`cq_motion` NVS).
- **Owner (2026-10-22): skip the N3 IR takeover check** (no remote at hand). Still to do from §115: `avoid_walls` with a box (floor), Plan P's R15.11 stand list and the `CQ_WDT_TEST` watchdog check.

## 118. `GO` and `HOLD` - driving on until something happens (owner, 2026-10-22)
- **Seen on the floor:** `avoid_walls` drove around without bumping and turned away from every wall (the closest the sensor read was 3.2 cm; two obstacle turns in 40 s). But "power delivery was not smooth and it seemed in a cycle" (owner). Each `FORWARD 20` is a separate move: it speeds up, drives 20 cm, stops, then the program checks `BLOCKED?`. Tank Logo v1 has no word that keeps driving.
- **Options discussed:**
  - (a) a longer step in the example;
  - (b) a continuous-drive word;
  - (c) leave smooth driving to the AI drive module.
- **Owner: (b)** — "a go word is a good addition for the tank".
- **Decisions:**
  - `GO` drives straight forward only (no `GO BACK`: the sensor looks forward).
  - A new **`HOLD`** stops the tank while the program goes on; `STOP` still ends everything. The owner chose the name HOLD over HALT.
- **Owner's direction for the AI words:** "drive forward till you find a ball or see people". `GO … WAIT UNTIL SEE? "ball … HOLD` covers this without a motion word per thing; AI words stay questions (`SEE?`, `HOWMANY`).
- **Safety:** `GO` is refreshed at least every 0.3 s by whoever runs the program (the tank's runner, or the AI engine over WiFi), else the tank stops. That is the manual-driving rule; no new rule.
- **Written as Spec 5 Revision 3** (owner: "yes", 2026-10-22). Next: the Tank Logo LLD revision, then the plan.
- **LLD approved (owner: "yes", 2026-10-22):** Tank Logo LLD §22 and bridge LLD R20.
  - A GO ends at once when the obstacle brake stops it.
  - The Uno's heading hold keeps it straight (as MOVE does).
  - The AI engine's `go` / `hold` messages wait for its Logo runner.

## 119. Plan N5 - `GO` and `HOLD` on the floor (owner, 2026-10-22)
- **Method:** the bridge was flashed with Plan N5 over WiFi. Programs were run with `tools/smoke/logo.py run` and recorded with `td_record.py` at 50 Hz. The distances are the encoders' mean count / 75.17 counts per cm; the distance ahead is the ultrasonic `dist_mm`.
- **`GO  WAIT UNTIL DISTANCE < 30  HOLD  BEEP`**, box ~1 m ahead: **pass.** The tank drove 72.3 cm in 3.4 s in one smooth run, the power went off at 28.8 cm from the box, and it glided to a stop at 25.0 cm, beeped and finished. The owner confirmed.
- **`FOREVER [ GO  WAIT UNTIL BLOCKED?  BACK 10  RIGHT 90 ]`**, 40 s in the `avoid_walls` room (§118): **the smooth drive works.**
  - 5 rounds, with straight runs of 2.3 s, 4.3 s and 10.1 s without stopping, and pauses only at the walls. The 20 cm stop-start cycle of §118 is gone.
  - Around 25 s the Uno's own slow-down (obstacle within 50 cm) briefly lowered the power, as designed.
- **Found: stuck against a table leg the sensor could not see** (owner: "a track hit the table leg and the tank stayed straight"). From 26.8 s to 31.5 s:
  - the heading stayed at −176.5° and the distance ahead at ~43 cm;
  - the left encoder ran ~1750 counts/s (the track slipping) while the right one barely moved;
  - the Uno's heading hold drove the left track at ~60 % (PWM ~195) and the right one at 0.
  - **Nothing noticed:** the no-progress rule averages the two tracks (the slipping one looks like progress), and the Uno's stall check sees a turning track. The tank got free only when the leg came into view at 3.9 cm.
  - `FORWARD` has the same gap. **To design** (owner: "do it"): "pushing but not moving" (no turn on the gyro, no change in the distance ahead, the tracks disagreeing) for ~1.5 s → blocked.
- **The `/logo` page check** (the word help and the dashed preview) is still to do by the owner.
- **Owner: "yes" (2026-10-22)** — the stuck-against-the-side fix is written as bridge LLD R21 (draft): "pushing but not moving". The tracks' speed difference should turn the tank; if the gyro does not see it for 1.5 s, the drive ends blocked.

## 120. Camera servos re-mounted at 90° (owner, 2026-10-23)
- **Found by the owner:** the pan and tilt servo horns had been mounted wrong. The angles in firmware were right: home = 90° for both (`CamLite::kHome` 900, a 1.47 ms pulse).
- **Re-mounted** with both servos driven to 90° first; the Uno's telemetry read `pan 90.0, tilt 90.0`.
- **Notes:**
  - A hobby servo has no spring and no "natural" middle: unpowered, it stays where it was last driven, held only by gear friction. The Uno releases both servos 2 s after they stop (spec 02 §D5.4), so a shaft turned by hand after that is no longer at the commanded angle.
  - The Q0 tilt estimate at home (4-9° down, §109) may have included this mounting error. The laser-level horizon check of the map work will measure it again.

## 121. Plan R3 - pushing but not moving, on the floor (owner, 2026-10-23)
- **Method:** the bridge was flashed with Plan R3 (bridge LLD R21). Runs used `tools/smoke/logo.py run` / `move.py` with `td_record.py` at 50 Hz. Track speeds are encoder counts per s over 0.5 s windows; the expected turn is (right − left) ÷ 75.17 counts/cm ÷ 19 cm × 57.3; the gyro is the `td` `rate` column.
- **Table leg beside a track, out of the sensor's view** (`FOREVER [ GO WAIT UNTIL BLOCKED? BACK 10 RIGHT 90 ]`): **pass.**
  - The first try met obstacles in front instead (stops at 15.7 and 15.3 cm by the existing near-obstacle rule; the tracks stayed equal).
  - The second try caught the leg with a track at ~4.3 s: the left track ran 1300-1900 counts/s, the right 175-620, the gyro −17…+5 °/s against 60-66 °/s expected.
  - **The GO ended blocked at 6.3 s, ~2 s after contact.** The distance ahead stayed ~44 cm and there was no obstacle flag, so only R21 could end it. Then it backed off 10 cm and turned right. Before R21 (§119) it pushed for 4.7 s.
  - It then drove 8.8 s on open floor with no false stop.
- **`move.py 200` on open floor:** done (198.0 cm at the stop, 200.4 cm after the roll-on, heading −0.1°). Owner's tape: 2 m, ~1.5 cm right. Unchanged from §116; no false "blocked".
- **Split surface** (the right track climbing onto a rug, `GO WAIT 3 HOLD`; the final review's risk I1): **finished, no false blocked.** 63 cm in 3 s; the tracks stayed within 2 % (≤ 1.1 °/s expected). The guard I1 suggested is not needed for this rug.

## 122. Plan P's tank checks, done (owner, 2026-10-23)
- **Watchdog (`CQ_WDT_TEST`), nothing moves:** pass.
  - A test build (`PLATFORMIO_BUILD_FLAGS="-DCQ_WDT_TEST"`, OTA) blocks the loop for 4 s once, 60 s after boot.
  - `/info` before: boot count 273, reason `software`. The bridge went silent at ~60 s and came back as **boot 274, reason `task_wdt`**.
  - The normal build was flashed back (boot 275, `software`). There was no restart afterwards: still boot 275 at 441 s and at the 10-minute check (below).
- **Stand list (bridge LLD R15.11; the owner on the phone, tracks off the ground):** all **pass**.
  1. drive stick;
  2. STOP;
  3. STOP from a second device (held until the stick is released, with its Log row);
  4. camera stick, HOME, LOCK;
  5. a sound while a camera HOME waits (both answered, no BUSY);
  6. a tank-button confirmation of a setting;
  7. the WiFi double press with and without a phone (jingle and blink);
  8. the Settings tabs (live sensors, self-test, camera, Log paging).
- **Floor list (a 1 m move, a 90° turn):** covered many times since, e.g. `move 200` at ±1.5 cm (§121) and the turns of §114 / §117.
- **`/logo` page for `GO` / `HOLD`** (Plan N5 Task 4 Step 4): pass. Both are in the word help, and the preview of `GO WAIT 1 HOLD RIGHT 90` draws a dashed 1 m line.
- **Plan P's tank checks are complete.** Still open from the old list: the N3 IR takeover (no remote at hand, §117).
- **Motor power: parked (owner, 2026-10-23).**
  - The motors' low-speed weakness (deadband PWM ~90-110; the slowest smooth spin ~65 °/s) no longer limits the tank: turns end within ~1°, the 4-turn spins within a few degrees, the Logo square closes within 1.5 cm.
  - Revisit only if the AI drive module needs slow, precise approaches. The options then: a higher gear ratio, less track friction, start-up kick pulses on the Uno (little room left).

## 123. The calibration plan (Spec 7 phase 2) - decisions (owner, 2026-10-23)
- **Lens:** a 9 × 6 checkerboard shown full-screen on the laptop; the owner measures one square with a ruler and types it in, and holds the tank's camera toward the screen in ~20 poses. No printout needed; the screen is flat.
- **Mount:** measured again after the servo re-mounting (§120): the lens height and its position on the tank, typed in, then checked by tape marks at 50 / 100 cm on the floor (within 2 cm). **Plus the laser level:** a horizontal line on a wall at exactly the lens height is the horizon; its pixel row gives the tilt at the servo's centre directly (§111).
- **Picture size:** QVGA (320 × 240), the driving size (§109). VGA only if needed later.
- **Driving tilt:** once the lens is calibrated, the tool measures the blind zone and what is in view at a few tilts. The owner decides the driving tilt from those numbers (the §109 open decision).

## 124. A camera-angle command on the bridge (owner, 2026-10-23)
- **Need:** the calibration tool (servo step, tilt study) and later module 2 must set exact camera angles. Pages could only move the camera with the stick or send it home; exact angles existed only inside the bridge for Logo's `LOOK`.
- **Options:**
  - (a) the tool runs tiny `LOOK` programs (no firmware change);
  - (b) a real bridge command.
- **Owner: "b is the correct solution — discuss before any implementation."**
- **Agreed (owner: "yes all three as recommended"):**
  1. degrees from the centre, right / up = +, like `LOOK`;
  2. the camera stick's permission rule (only the driver while someone drives, anyone when nobody does; never takes ownership);
  3. **a tilt outside −20…+60° is refused at the bridge** ("so one knows the limit"), as is a pan outside ±90°, with the limits in the message.
- **Written as bridge LLD R22 (draft).** The calibration plan (Spec 7 phase 2, §123) builds on it.
- **R22 approved (owner: "yes, fold it into the calibration plan", 2026-10-23):** `cam_set` becomes the first task of the calibration plan (Plan Q2).

## 125. The camera's pan is 5° off centre (owner, 2026-10-23)
- **Found during the calibration session:** at `cam_set` pan 0 (servo 90°) the camera did not look dead ahead. The owner steered it with `cam_set` +2°, +4°, +5° and judged **+5° (servo 85°) dead ahead.**
- **Why not re-seat the horn:** the servo's splines step about 14° per tooth, so 5° cannot be fixed by moving the horn.
- **Owner: "do d now then we will do b":**
  - **now (d):** the AI calibration carries it. The servo step fits `pan_map` from the laser level's vertical line straight ahead, so the offset is in `pan_map`'s offset. No firmware change. The phone driver's view at HOME stays 5° off until (b).
  - **next (b):** a pan trim on the Uno (a saved "camera centre" setting), so HOME, the stick, `LOOK` and `cam_set` all treat the true centre as 0. Design first; the Uno's flash is at 96 %.

## 126. The driving tilt: 15° down (owner, 2026-10-23)
- **The §109 open decision.** After the lens and mount calibration (§125, Plan Q2), the tilt study (`cq-calibrate study`, with the saved `config/calibration.yaml`) gave, for each tilt from the centre:

  | Tilt | Blind ahead | Blind at the picture's edge | Floor reaches | Walls in view |
  |---|---|---|---|---|
  | 0° | 21.5 cm | 23.0 cm | the horizon | yes |
  | −5° | 17.2 cm | 18.1 cm | the horizon | yes |
  | −10° | 13.7 cm | 14.3 cm | the horizon | yes |
  | −15° | 10.8 cm | 11.2 cm | the horizon | yes |
  | −20° | 8.3 cm | 8.5 cm | the horizon | yes |
  | −25° | 6.0 cm | 6.2 cm | the horizon | yes |
  | −30° | 4.0 cm | 4.1 cm | the horizon | yes |

- **How it was worked out:**
  - "Blind" is the nearest floor point in view, measured from the tracks' front (13 cm ahead of the tank's centre). It comes from the calibrated camera (fx 182.5, distortion, height 17.8 cm, lens 8.5 cm ahead, mount tilt −0.15°), with no servo map yet (`tilt_map` 1, 0).
  - "Floor reaches the horizon" means the top of the picture is still above level, so the floor runs to the far walls.
  - The height the camera sees at 2 m (rough, from the vertical half-view of about 33°): 0° ~1.5 m, −10° ~1.0 m, −15° ~0.8 m, −20° ~0.6 m, −30° ~0.3 m.
- **Options discussed:**
  - −10° keeps people in view;
  - −15° gives more floor;
  - −20° and below gain only a few centimetres. The ultrasonic covers that close range, and the top of the view falls to a few degrees above level.
- **Owner: "chose what will be best for navigation" → −15°.**
  - The map is built from the floor, and −15° gives the most floor pixels near and mid-range while the horizon stays in view, so far obstacles are still seen.
  - Anything low enough to block the ~20 cm tall tank is in view at any of these tilts.
- **Not fixed for ever.** Module 2 can look up with `cam_set` when a task needs it, e.g. a later "see people" word.

## 127. Camera calibration results (Plan Q2 tank session, 2026-10-23)
**What calibration means here:** the AI turns a pixel into a point on the floor. To do that it needs:
- the lens (how wide it sees and how it bends straight lines);
- where the camera sits on the tank;
- which way the servos point it.

All the numbers below are for QVGA (320 × 240), the picture size the map uses.

- **Lens:**
  - **How:** a 9 × 6 checkerboard filled the laptop screen. The owner measured one square: 20 mm. The laptop stayed still in front of the tank, and the tool swept the camera with `cam_set` (pan 5 ± 12°, tilt ± 9°), so every view came at a different angle. Corners were found in **35 of 35 views**. OpenCV `calibrateCamera` gave the fit.
  - **Fit error:** 0.170 px rms (accept < 0.5).
  - **Result:** fx 182.5, fy 181.8 px, so the camera sees **about 82° across**. That is wider than the 66° assumed in §109.
  - **Check:** OpenCV `solvePnP` put the screen 42.9 cm from the lens; the owner's tape said 42.5 cm.
- **Mount (owner's tape):**
  - lens 17.8 cm above the floor;
  - lens 8.5 cm ahead of the tank's centre, on the centre line;
  - tracks' front 13 cm ahead of the centre, so the tracks reach 4.5 cm past the lens.
- **Mount tilt (`tilt0_deg` −0.15°):**
  - **Planned:** a laser line on the wall at lens height. There is no laser level yet.
  - **Instead:** tape marks on the floor straight ahead at 50 cm and 100 cm. The owner clicked them on a VGA picture shown large (VGA halved matches QVGA, correlation 0.995). The tilt was fitted to those two marks.
  - **Result:** the marks come out at 48.9 cm and 98.8 cm. Sideways they were 8-10 cm off; the owner said the clicks were not on the marks' centres, so that was accepted as click error.
  - **The tilt is about 0°:** the re-mounted servo at 90° holds the camera level.
- **Pan:** −5° offset in `pan_map`, judged by eye at `cam_set` +5° (§125).
- **Tilt map:** 1:1 with no offset, assumed. **Still to do:** the servo step with the laser level (`pan_map` / `tilt_map` from the line) when the laser turns up.
- **Fixes found on the tank:**
  - the checkerboard picture was not written when its folder was missing;
  - the corners of the picture had no floor point (the undistortion did not settle) → Newton's method.
  - Both were test-first (AI engine LLD §13).
- **Tilt study and the driving tilt:** §126 (15° down).
- **Session notes:**
  - the tool refuses a VGA picture. `camera_set` needs `"QVGA"` in upper case; a lower-case value is refused as `INVALID_COMMAND`.
  - The camera was set back to VGA afterwards.
  - The pictures were deleted after the numbers were taken (they show the home).

## 128. Camera centre on the Uno: the design (owner, 2026-10-23)
- **The §125 (b) follow-up.** The owner approved the design ("yes"):
  1. **The trim lives on the Uno.** The Uno sends the camera home by itself (HOME, the IR HOME key, the stick let go), so a bridge-only trim would miss those. It uses the protocol's existing `pan_trim` / `tilt_trim` settings (ids 27 and 28). The Mega firmware already uses them; the Uno Lite gains them. No protocol change.
  2. **Pan and tilt both.** Tilt measures about 0 today; the laser may say otherwise.
  3. **The tank reports the corrected angle.** Telemetry 90° = truly straight ahead, so the AI calibration's `pan_map` goes back to (1, 0) once the trim is saved.
  4. **Set by eye on the Settings page,** Camera card: ◀ ▶ ▲ ▼ in 0.5° steps over the live picture, then Save.
  5. **Limits:** ±30°, the setting's range. The servo's 0–180° stop takes up to the trim off one end.
- **Detail settled while writing the LLDs:**
  - a **step is a "try"**: it changes the Uno's RAM only and needs no button, because it only moves the camera and a restart forgets it;
  - **Save** is button-confirmed like `track_balance`, and the Uno then saves its whole block, tilt trim included.
- **Uno flash:** budget ≤ 200 B (31 144 B / 96.6 % before).
- **Written as:** Uno Lite LLD §22 and bridge LLD R23 — **approved (owner: "seems good", 2026-10-23).** Next: the plan (Plan S).

## 129. Camera centre on the tank (Plan S, 2026-10-23)
- **What was done:** the Uno Lite now keeps a saved camera centre (`pan_trim` / `tilt_trim`, Uno Lite LLD §22), and the Settings page's Camera card sets it by eye (bridge LLD R23). This closes §125 (b).
- **How it was set:**
  - The bridge was flashed over WiFi. The Uno was flashed over USB from the Linux machine (avrdude, `/dev/ttyUSB0`). The first try failed with "not in sync" while S1 was in the run position; the second wrote and verified 31 376 B.
  - The owner stepped ▶ on the phone until the picture looked dead ahead: **pan −5.0°**, the same as the `cam_set` +5° found in §125. Tilt stayed 0. Saved with the tank's button.
- **How it was checked:**
  - after a power cycle the Uno reads back −50 / 0 (saved in EEPROM);
  - the owner saw no jump at power-on;
  - dead ahead at rest, after HOME, after the stick is let go, and after `cam_set 30 0` → `cam_set 0 0` from the laptop.
  - Telemetry reports 90° for dead ahead, the logical angle.
- **Skipped on the tank (owner: "skip it"):** a try refused while another client drives. Holding the drive stick can move the tracks; the bridge host test covers it.
- **The AI calibration:** `pan_map` back to (1, 0), because the trim now carries the 5° (AI engine LLD §13). `tilt_map` (1, 0) and `tilt0_deg` −0.15 are unchanged (no tilt trim).
- **Final review:** no Critical. One Important (on the page, quick refused steps) and six Minor findings, all fixed test-first (bridge LLD R23.5, Uno Lite LLD §22.1). The Uno costs +232 B (budget 200 B; 880 B free).

## 130. Servo gains from a laser cross line (owner, 2026-10-23)
- **Why:** the map needs the camera's real angle for each commanded angle. Cheap servos often turn 5–10 % more or less than asked. Until now `tilt_map` / `pan_map` were assumed 1:1.
- **Why not the planned laser step (AI engine LLD §8):** it needs the horizontal line at exactly the lens height. The owner has no stand for that ("too complicated"). **Settled:** a line a few cm off is fine, because a gain only uses the *change* between pictures. The line's height and direction shift only the offset, which is taken from elsewhere: tilt0 −0.15° from the tape marks (§127), the pan centre by eye (§129).
- **Setup:** the owner's laser level (a **green** cross line) on a wall in front of the tank, the horizontal line roughly at lens height, the vertical line ~22° right of straight ahead (the laser stood off to one side).
  - The green line is so bright it burns out to near-white (1–2 px), so the tool's red finder saw nothing. A session script found the lines by **brightness** instead: line ~128 against the wall's ~55.
- **How it was measured** (session script `laser_sweep.py`, QVGA, `cam_set`, 1.5 s settle, the 3rd picture):
  - tilt 8 commands, +15…−20° at pan 0: the horizontal line's row → angle (`horizon_tilt_deg`);
  - pan 11 commands, −10…+40° at tilt 0: the vertical line's column → angle (`pan_from_col_deg`);
  - a straight-line fit for each;
  - run twice, sweeping in both directions.

  | | sweep down / right | sweep up / left | result |
  |---|---|---|---|
  | tilt gain (real ° per commanded °) | 0.919 | 0.903 | **0.91** |
  | tilt offset | +1.07° | −0.57° | slack **±0.8°** |
  | pan gain | 0.940 | 0.945 | **0.94** |
  | pan offset (only where the laser stood) | −21.08° | −21.83° | slack **±0.4°** |

  - Largest leftover error per fit: 0.7–1.2°, mostly the slack.
- **Saved** in `ai-engine/config/calibration.yaml`: `tilt_map` (0.91, 0), `pan_map` (0.94, 0); `tilt0_deg` −0.15 unchanged.
- **What it changes:** a commanded −15° (the driving tilt, §126) is really −13.8°, so the blind zone ahead is 11.5 cm (was 10.8 cm with 1:1). At 0°: 21.6 cm.
- **Open for module 2 (drive) — the servo slack:** the tilt lands ~0.8° higher when it arrives from above than from below (pan ~0.4°). No gain fixes that.
  - Possible fixes: always reach the driving tilt from the same side (e.g. a small overshoot down, then back up), or use the measured direction.
  - At −15° tilt, 0.8° is ~8 % of the distance to a floor point 1 m away.
- **Next (small plan):** make `cq-calibrate servos` do this itself: find a line of any colour (brightness), keep the saved tilt0 and centre (gains only), sweep both ways, and prompt "the line roughly at lens height, ±5 cm". The session scripts stay local (scratchpad).
- The picture taken to find the lines was deleted after the numbers were written down. The camera was set back to VGA.

## 131. `cq-calibrate servos` does the §130 measurement itself (Plan T, 2026-10-23)
- **What:** the tool's servo step now works the way the session script did (AI engine LLD §8 amendment):
  - a laser cross line of **any colour**, burnt-out white included;
  - placed **roughly** at lens height (±5 cm) and roughly straight ahead;
  - **both ways**, **gains only**: tilt0 and the camera centre are kept;
  - the slack printed with its direction.
- **Final review:** no Critical. Three Important, all in the line finder:
  - the floor hid the line at low tilts;
  - a 1-px burnt-out line barely passed;
  - a bright strip could be taken for the line.
  - These and the eight Minors were fixed test-first (LLD §8 "As built"). AI engine 83/83 tests.
- **Not run on the tank:** the owner skipped it, because the calibration is already done (§130: tilt 0.91, pan 0.94, saved). If the servos are changed or re-mounted, run `cq-calibrate servos` with the laser.

## 132. The pose filter: the tank's heading plus the encoders (owner, 2026-10-23)
- **Map phase 3 (Spec 7 §9).** The filter answers about 50 times a second where the tank is, which way it faces and how fast it moves. It only watches; it never drives.
- **Owner: "yes all as recommended":**
  1. **Heading:** the tank's corrected heading from `td`. The Uno already re-learns the gyro bias at rest, and the bridge already applies the gyro scale (×1.031), so a second bias estimate would fight them. The engine unwraps it and starts the room at 0°.
  2. **Distance:** the mean of the two encoders ÷ `counts_per_m` (7514, read from the bridge at connect). The direction comes from each track's PWM output; while coasting, the last direction is kept. A pivot gives about zero.
  3. **A stalled track adds no distance.** Pushing without a stall flag can still over-count; that's accepted until the camera map gives a second opinion.
  4. **Uncertainty:** a simple "how far off could we be" number: 3 % of the distance and 1° per 360°. A full Kalman filter comes with a second position source (wall markers, later).
  5. **Restarts:** an Uno or bridge restart, or a gap over 0.5 s, re-anchors the counts and the heading. The pose keeps x, y and θ and doesn't jump.
  6. **Check:** host tests, then the 50 cm Logo square and a 2 m out-and-back, recorded and replayed, against the tape. Targets: ≤ 3 % of the distance, ≤ 3° per 360°.
- **Written as AI engine LLD §14** (draft; replaces §5.2).

## 133. The pose filter is built (Plan U, 2026-10-23)
- **What:** AI engine LLD §14 as built.
  - The laptop knows where the tank is, which way it faces and how fast it moves, 50 times a second, from the tank's corrected heading and the encoders.
  - **`cq-pose <recording>`** replays a recording and says where the tank ended, for the floor check.
  - It only watches and never drives.
- **Found while drafting:** a sample's encoder counts happened under the *previous* sample's motor output. With the new one's, every change of direction (a square's corner) lost a step.
- **Final review:** no Critical. Two Important, both fixed test-first:
  - a bridge restart broke the speed and the 2 s history;
  - a WiFi pause over 0.5 s threw away the turn made in it.
  - **Ruling:** the heading is kept across such a pause, because the tank's heading is absolute; only the distance is unknown. This changes LLD §14's "re-anchor" wording for gaps.
  - Five Minors also fixed or noted. AI engine 110 tests.
- **Floor check postponed to 2026-10-24** (owner: "we will test tomorrow"):
  - the Logo 50 cm square and `FORWARD 200 RIGHT 180 FORWARD 200`, recorded with `cq-record` and replayed with `cq-pose`, against the tape;
  - targets: square ≤ 6 cm, out-and-back ≤ 12 cm, heading ≤ 3° per 360°.

## 134. Phase 4 design: things in the room (owner, 2026-10-23)
- **What:** for each picture the AI finds things (person, ball, chair, cat, …) and works out where each one stands in the room. It keeps a list that the camera can look away from (a ball stays ≥ 10 s). Later the drive module and Logo words ("drive until you see a ball") read it.
- **Owner: "yes all as recommended":**
  1. **Detector:** YOLO26n at QVGA on the Neural Engine (CoreML, ~4 ms, §109). The model is exported once by a setup script and kept out of git. At run time only Apple's CoreML runtime and our own pre- and post-processing are used (the model needs no extra filtering step).
  2. **Where a thing is:** the bottom middle of its box is where it touches the floor (the floor trick).
     - Only for things that stand on the floor; table things (cup, bottle) are left out.
     - A box cut by the picture's bottom edge gets a direction but no distance.
  3. **Tracking:** matched in room positions (the same class, near the prediction). No image tracker unless recordings show swaps.
  4. **Pictures skipped:** turning faster than 45°/s, the camera moving, or no pose at the capture time. Pan and tilt come from the telemetry at the capture time.
  5. **Shown and printed:** the live view (the picture with boxes; a top-down plan with the path and the things) and `cq-objects <recording>`.
  6. **Checked:** host tests with a fake detector; on the floor, a ball at 50 cm, 1 m and 2 m (≤ 5 cm to 1 m, ≤ 15 cm at 2 m), and the ball remembered after 12 s away.
- **Written as AI engine LLD §15** (draft).

## 135. Arduino UNO R4 Minima reviewed - not used (owner, 2026-10-23)
- **Why it came up:** the Uno Lite is at 97 % flash (31 376 / 32 256 B, 880 B free) and its settings buffer is full (31 / 32 B). The R4 Minima has the same shape, 256 KB flash, 32 KB RAM, a 48 MHz Cortex-M4 and USB separate from D0 / D1 (no S1 flipping), so it could run the full firmware.
- **Owner's condition:** every pin the shield uses must do the same job, because the shield's mapping can't change (no rework, §30). Otherwise stay as is.
- **The research** (Arduino's MINIMA pin file `variants/MINIMA/pinmux.inc` + `variant.cpp`, the RA4M1 datasheet, Arduino's docs and forum):
  - ✓ D1 → ESP32 (`Serial1` on SCI2), D5 / D6 hardware PWM, D3 / D7 / D8 / D13 digital, D12 its own interrupt (IRQ3), A2 its own interrupt (IRQ7), D10 / D11 servos (Servo library), A3 analog, A4 / A5 I2C (IIC1).
  - ❌ **D0 ← ESP32 TX:** an RA4M1 input needs ≥ 0.8 × VCC = **4.0 V** for a "high" on a 5 V board; the ESP32 sends 3.3 V. The ATmega328P needs only 0.6 × VCC = 3.0 V, which is why the Uno works. A forum report says the shield has no level shifting on that line. The fix (a level shifter) is shield rework, which is not allowed. Running out of spec on the safety link is not acceptable.
  - ⚠ D9 (IR) has no interrupt (timer sampling, as the Mega build does); A1 shares IRQ6 with D0 (a forum user saw D15 count by itself; timer sampling); the WS2812 library failed Arduino's own R4 hardware test; GPIO is limited to **8 mA per pin** (Uno 20 mA), a risk if the shield drives the buzzer straight from A0.
  - ⚠ Power: VIN 6–24 V vs the 2S battery's 6.0–8.4 V; the 5 V regulator's current is not published.
- **Decision (owner, on the research): stay with the Uno R3 as is.** The bigger board remains the planned Mega 2560 full firmware on our own base board (§30, §43).

## 136. The camera's motion is not smooth: measured, and the fix goes on the Uno (owner, 2026-10-23)
- **Owner:** "it's not jitter, just the movement doesn't feel smooth like what we got from an RC transmitter."
- **Measured on the stand:**
  - `servo_record.py` (session script) recorded `td.pan` / `tilt` at 50 Hz for 30 s while the owner moved the phone's camera stick slowly, then quickly: 1350 of 1500 samples.
  - **Slow moves are stop-and-go** (`M..M..M.M.M.....M`): 151 pauses of 1–4 ticks inside the motion. Steps come in 0.9° multiples, the stick's whole percents, arriving ~15–25 times a second.
  - **Fast moves are capped at 2.4° per tick** with hard starts and stops: 155 of the 389 moving steps.
- **Options discussed:**
  - (a) the phone sends faster and finer (a protocol change; WiFi bunching remains);
  - (b) the bridge smooths and re-sends at 50 Hz (more serial traffic, bridge code; the IR remote and HOME don't benefit);
  - (c) **the Uno smooths at its 50 Hz servo loop:** a target follow (a quarter of the gap per tick) plus a speed cap and an acceleration limit. That's the standard method, as RC "servo speed" settings and our full firmware's `servo_speed` / `servo_accel` do.
- **Owner: "let's do uno"** → Uno Lite LLD §23 (draft).
- **Also found (open):** while the phone was using the stick and its video, **the recorder's WebSocket was dropped 9 times in 30 s** (none with the phone idle). The AI engine is such a second client, so the cause (the bridge closing a slow client? WiFi load from the video) must be looked at before phase 4's live tests.

## 137. Smooth camera motion, as built (Plan W, 2026-10-24)
- **First stand check (one stage, Uno Lite LLD §23):**
  - The 30 s stick recording (`servo_record.py`, `td.pan` at 50 Hz): **pauses inside the motion 145 → 14**, median step 1.8° → 0.5°. The stop-and-go was gone.
  - Owner: "it is good but i see jitter on pan" → "the whole setup shakes while panning".
- **How it was traced:**
  - The commanded pan never reversed tick to tick and was exactly constant whenever the stick was still, so the Uno wasn't asking for a wobble.
  - Sweeps from the laptop (`pan_sweep.py`: ±60° at 120 / 60 / 30°/s, targets sent 20 times a second) → the owner saw **the most shaking at 60°/s**.
  - The 60°/s recording showed the speed **pulsing every 5 ticks** (0.9 / 1.2 / 1.2 / 1.5 / 1.2° per tick): the target updates (every 50 ms; the phone's stick is similar) passed through the one-stage follow as a 10 Hz push. The owner said the mount screws are tight.
- **Options simulated** (pulsing / coasting on a full-speed reversal / time to 90 % of a 30° move):
  - one stage: 1.9 / 0° / 320 ms;
  - two follow stages ¼ + ¼: 0 / 13° / 340 ms;
  - **a 5-tick average + follow: 0 / 9.6° / 320 ms**;
  - ½ + ¼: 0.9 / 5.8° / 320 ms.
  - The physics: any smoothing that hides the steps delays the camera, so a quick reversal coasts. The root is the stick's 15–25 updates a second in 0.9° steps.
- **Owner: "add a second stage" → the 5-tick average** (budget 300 B approved): 31 602 B, +226 B.
- **Second stand check:** the 60°/s cruise is a steady +1.2° per tick (spread **0.19° → 0.07°**). Owner: **"it's as good as it could get, i think it's due to the weight."**
- **Why the rest is mechanical** (from the owner's photo of the head, the common SG90 pan/tilt kit):
  - the pan servo turns the whole head (itself, the cradle, the tilt servo, the camera) on its own output shaft, with no bearing;
  - the shaft is near one end of the servo, so the rotating mass (~45–60 g, estimated) sits ~1–2 cm off the axis, on a ~5–6 cm tall lever;
  - each start and stop rocks it through the plastic gears' play (±0.4° on pan, §130).
  - **Later, if wanted (hardware, not the shield):** MG90S metal-gear servos (the same size and plugs), a pan base with a bearing, or balancing the camera over the pan shaft.
- **Still open:** the bridge dropped the recorder's second WebSocket 9–12 times per 30–40 s while the phone or the sweeps were busy (§136).

## 138. A better pan / tilt mechanism (owner, 2026-10-24) - open
- **Owner: "we need better pan and tilt mechanism."** The SG90 kit's pan servo carries the whole tall head on its own plastic-geared shaft (§137), and software smoothing has gone as far as it can.
- **To discuss and design later** (hardware; the shield and its servo ports stay as they are, §30):
  - an MG90S for pan only, keeping the SG90 for tilt (metal gears, a ball-bearing shaft; +4–5 g near the pan axis);
  - a pan base with its own bearing, so the servo shaft only turns the head;
  - a lower, balanced head with the camera's weight over the pan axis;
  - or a ready-made bearing pan / tilt kit for 9 g servos.
- After any change: re-check the camera centre (§129) and the servo gains (`cq-calibrate servos`, §131).

## 139. Camera improvement (parked, owner 2026-10-24) and what the phase 4 night test showed
- **Parked earlier (camera discussion, 2026-10-23):**
  - a **white-balance** setting on the Settings page's Camera card (the picture has a colour cast; §109 noted magenta in daylight, green under the room lights);
  - a **camera-clock test** (XCLK 16 or 20 MHz instead of 10 MHz, with a WiFi ping check, §57 / §76) for more pictures per second.
- **Added by the night test (Plan V Task 4):** exposure and low-light settings for dim rooms.
  - With the room lights on, the QVGA picture's median brightness was **44 / 255**.
  - YOLO's "person" score had a median of 0.0–0.08, with only 9 of 140 pictures at or above 0.35.
  - A plush (Pikachu) was seen as "fire hydrant" (0.77–0.92) up close and not at all at 2 m.
- **Also found that night:**
  - **The mount tilt was 2.7° off:** −0.15° (clicked tape marks) → **+2.55°**, fitted from the plush's box bottom at 50 cm and 1 m with the camera arriving at −15° from above (49.6 / 99.9 cm; the height fitted free came out 18.1 cm against the measured 17.8). Saved on the Plan V branch.
  - With it, a person's feet were placed at 99.5 cm from the lens against the 100 cm mark.
  - **The tilt servo's slack near 0° is larger** than the laser's ±0.8°: commanded 0 → −5 moved only 1.4° when arriving from below. The drive module must reach the driving tilt from above (§130); a better mechanism is §138.
  - **The camera doesn't sag** when its servos are released: < 0.3° over 6 s (phase correlation).
- **Order (owner): the camera improvement first** (design, then a plan), then Plan V's floor check again, in daylight.

## 140. Camera improvement: the design (owner, 2026-10-24)
- **Found while designing:** the bridge's gain limit has been wrong since §96. The OV3660 driver takes a **raw** gain (16 = 1×, up to ≈ 64×), but we sent the library's names, 1 for "4×" and 4 for "32×". So the auto gain could barely raise the picture in a dim room, which explains §139's 44 / 255.
- **Owner: "yes all as recommended, carry over old settings":**
  1. **The gain limit fixed:** normal 8× (raw 128), low light 32× (raw 512).
  2. **Low light for driving:** the higher gain plus one step brighter, **without** the sensor's night mode. Night mode (longer exposures, fewer pictures) becomes its own "Night (slow)" switch for looking around, not for driving.
  3. **White balance:** Auto / Sunny / Cloudy / Office / Home (the OV3660's presets) on the Camera card, saved on the tank.
  4. **The camera-clock test** (10 / 16 / 20 MHz: pictures per second and WiFi loss) is a measurement session in daylight; the result becomes a firmware constant.
  5. **The saved camera settings are carried over** into the new format (not reset).
- **Written as bridge LLD R24** (draft).

## 141. Camera improvement on the tank (Plan X, 2026-10-24, night)
- **How it was measured:**
  - a session script (`cam_bright.py`) set QVGA and took 20 pictures with low light off, then 20 with it on (3 s settle each);
  - it reports the median grey-level brightness (0–255) and the pictures per second from the capture stamps;
  - the same room light before and after the flash.

  | | Before (old firmware) | After (Plan X) |
  |---|---|---|
  | low light off | 87 / 255, 13.9 / s | **201 / 255, 13.9 / s** |
  | low light on | 87 / 255, 12.6 / s | **207 / 255, 13.9 / s** |

  - Before, low light changed nothing: the gain ceiling was near zero in both modes (the §96 bug).
  - After, the picture is more than twice as bright in the same light, and low light no longer costs frame rate.
  - **Night** on (the owner tried it from the card): 4.6 pictures / s, longer exposures, as designed (for looking, not driving).
- **The saved settings were carried over** across the flash: QVGA, quality 12, brightness +1, contrast +2, vflip; plus white balance auto and night off. The owner's +1 / +2 were set while the picture was too dark; they may want 0 now.
- **YOLO with the owner as the subject** (2 m; standing, then sitting with a laptop): no person found.
  - Not the light: the picture was 170–178 / 255 and sharp.
  - **Standing:** at the driving tilt the top of the view reaches only ~1 m high at 2 m (knees to waist), and legs alone aren't a "person" to YOLO from that low angle.
  - **Sitting:** ~55 × 55 px at QVGA, partly behind the laptop; YOLO26n-320's best guess was "suitcase" at 0.16.
  - **For phase 4 / module 2:** people need **a look up** (a tilt-up scan when a task needs them), or more pixels (Spec 7 §10's open QVGA / VGA question: VGA would make a person at 2 m about 110 px tall, at ~7 pictures / s, with a VGA calibration). To try in Plan V's floor check (Task 4).
- **Still to do:**
  - the white balance by eye **in daylight** (owner), a choice on the Camera card with no code;
  - the camera-clock test (§140 item 4).
- The camera was set back to VGA; the pictures and recordings were deleted.

## 142. The "dropped second WebSocket" (§136) was our scripts, not the bridge (2026-10-24)
- **Symptom (§136, §137, §141):** the session recorders' WebSocket closed 9–12 times per 30–40 s.
- **Cause:** the bridge closes any client silent for 3 s (`WsClients::silent`, `kWsSilentMs`; a deliberate rule, bridge LLD: a silent driver means a lost phone). The pages and the AI engine's `BridgeLink` ping every 1 s. The session scripts (`servo_record.py`, `cam_bright.py`) only listened, so they were closed every ~3.3 s. The phone's activity played no part: the earlier "idle phone: 0 drops" was a 2–3 s probe, shorter than the 3 s rule.
- **Measured** (`ping_test.py`, a listening client with `debug` 50 Hz):
  - never sending: **4 drops in 15 s**, 703 `td` samples;
  - pinging every 1 s: **0 drops**, 749 samples.
- **Decision:** no firmware change. The AI engine is not affected. Session scripts use `BridgeLink` (or ping every 1 s).

## 143. The camera clock: 20 MHz (owner, 2026-10-24; §140 item 4)
- **Why it was 10 MHz:** in 2026-10-16 (§57, LLD R4.14) the camera at 20 MHz made the WiFi lose half its packets. Later (§76) the cause turned out to be the clock pin's **drive strength**; the weakest drive (`kCamXclkDrive` 0) gave 0 % loss, but the clock stayed at 10 MHz.
- **How it was measured** (`clk_measure.py`, the tank standing):
  - the camera streaming for 20 s or 60 s;
  - the capture rate from the stream's `X-Frame-Us` stamps and the arrival rate;
  - meanwhile the Mac pinged the bridge 5 times a second (loss and round trip);
  - each clock built and flashed over WiFi (only `kCamXclkHz` changed).

  | Clock | QVGA pictures / s | Ping loss | Round trip avg / max |
  |---|---|---|---|
  | 10 MHz | 13.8 | 0 % | 7.3 / 18 ms |
  | 16 MHz | 17.7 | 0 % | 7.2 / 43 ms |
  | **20 MHz** | **27.5** (60 s: 27.3) | **0 %** (0 / 300) | 7.0 / 24 ms (60 s: 7.4 / 59) |
  | **20 MHz, VGA, 60 s** | **23.6** (§109: 6.9 at 10 MHz) | **0 %** (0 / 300) | 7.4 / 56 ms |

- **Owner: "yes" → `kCamXclkHz` = 20 MHz** (`include/board_config.h`); the drive stays at 0.
- **For phase 4:** VGA at ~24 pictures / s makes the VGA detector trial (§141: people are too small at QVGA) practical.

## 144. The pose filter on the floor (Plan U Task 3, 2026-10-24, daylight)
- **How:** the owner ran a Logo program from `/logo` while `cq-record` recorded the session. `cq-pose` replayed it, and the owner measured the tank's end against the tape start mark (cm with a tape, the heading by eye).
- **Results** (AI engine LLD §14.6; targets from Spec 7 §7: 3 % of the distance, 3° per 360°):

  | Run | `cq-pose` end | Tape end | Pose vs tape | Target |
  |---|---|---|---|---|
  | `REPEAT 4 [ FORWARD 50 RIGHT 90 ]` (202 cm driven, 2299 samples) | 2.0 cm right, +0.6° (2.1 cm from the start; "could be off" 6.1 cm / 1.1°) | 6 cm right, about −2° | 4 cm, ~2.6° | 6 cm, 3° |
  | `FORWARD 200 RIGHT 180 FORWARD 200` (401 cm, 3819 samples; farthest 200.5 cm) | 0.5 cm left, 179.9° ("could be off" 12.0 cm / 0.6°) | 5 cm left, about 178° | 4.5 cm, ~2° | 12 cm, 3° |

- **Passed.** The heading gap is within the eye's reading error.
- **Two runs did not count:**
  - the first recording (60 s) ended before the square's last turn: RUN came 44.6 s in;
  - one run was stopped from the phone after 23.8 cm (7040 "Stopped by you").
  - From now on the recording is started long (180 s) and stopped once the tank stops.
- An unrecorded square ended 4 cm right, about −3°.
- The Logo moves each reported 47.9-48.0 cm (50 cm) and 197.8-198.0 cm (200 cm) at the cut; the coast makes up the rest (§69).
- The recordings were deleted.

## 145. The board machine is now the Raspberry Pi `yantra` (owner, 2026-10-24)
- **Owner:** "from now on we will use mannu@yantra.lan to connect to uno and esp32" — the Pi (Debian 12, arm64), replacing the Linux laptop at 192.168.31.8. The laptop's sleep was turned back on (the four sleep targets unmasked; it suspends with the lid shut again).
- **Set up without sudo:**
  - `uv`, PlatformIO 6.2 and esptool 5.6 in `~/.local/bin`;
  - avrdude 8.1 at `~/.platformio/packages/tool-avrdude/avrdude`;
  - firmware files go in `~/cq/fw`.
  - A plain SSH command does not have `~/.local/bin` on its PATH, so call the tools by full path.
- **Boards** (`/dev/serial/by-id/`):
  - the Uno: CH340, `/dev/ttyUSB0`, signature `1E 95 0F` (ATmega328P);
  - the ESP32-S3: native USB, `/dev/ttyACM0`, ESP32-S3 rev v0.2, 8 MB flash, 8 MB PSRAM.
- **The Uno flash needs shield switch S1 off** (the ESP32 disconnected). With S1 on, avrdude never syncs: the ESP32's bytes are on the line. Turn S1 back on afterwards.

## 146. The camera module can tilt in its snap-fit (owner, 2026-10-24)
- **Owner:** the camera snaps into its holder, so it can tilt between runs. This may explain the angle errors between runs, such as the tilt0 fitted at −0.15° (§127) and again at +2.55° (Plan V branch, from a plush at 50 / 100 cm).
- **Decision:** the owner drills a hole and fixes the camera so it cannot move, before Plan V's floor check.
- **After the fix, re-check before trusting the camera's angles:**
  1. the camera centre (pan trim, Settings page, §129);
  2. `tilt0` from a thing on the floor at a known distance (as on the Plan V branch);
  3. `cq-calibrate servos` only if the servo horns were moved (§131).
  - The lens numbers (K, distortion, §127) stay, unless the lens itself was turned in its thread (focus).

## 147. The Logo page as an editor on the laptop (owner, 2026-10-24)
- **Owner:** the Logo page should behave like a real code editor (an IDE), with no page scroll, and it has no camera. The phone may scroll, and Logo is not for the phone, so the laptop layout is designed on its own.
- **Owner: "yes all as recommended"** (Tank Logo LLD §23, revision N6):
  - **900 px or wider:** the page fills the window and never scrolls. Three fixed columns: programs · the editor with a PROBLEMS / MESSAGES panel under it · the camera above the path.
  - A **status bar** at the bottom: the running line or the word help, with STEP and RUN. **STOP** stays in the top bar.
  - The **camera** has the sonar distance on the picture and a HIDE button, and streams only while shown.
  - **Shortcuts:** Esc = STOP, Ctrl/⌘ + Enter = RUN, F10 = STEP, Ctrl/⌘ + S = save.
  - **Fixed pane sizes** (no dividers for now). Under 900 px, the phone layout is unchanged.
  - Only `logo.html` / `logo.js` change; the bridge and the Logo engine don't.
- An earlier idea, one page for the whole app (Drive · Logo · Settings), was **not** taken: the owner meant only Logo.

## 148. One shared icon file for the tank's pages (owner, 2026-10-24; Plan N7, after N6)
- **Owner:** the Logo page uses its own made-up icons (the WiFi as `▂▄▆█` text, the battery as plain volts, `● UNO`, `▶ RUN`) instead of standard ones. The Drive and Settings pages already use **Lucide** (lucide-static 0.460.0, ISC), pasted into each page by hand.
- **Owner: "3", then "yes all as recommended":**
  - **one icon file**, `firmware/esp32/web/icons.json`: every Lucide icon the app uses, one version, the licence noted once;
  - **fixed icons:** a page writes `<!--icon:name-->`, and `pack_web.py` puts the icon in when packing (still no JavaScript and no network needed);
  - **changing icons** (WiFi strength, battery): `pack_web.py` adds a small table with only the icons that page uses, read by `icon(name)`;
  - **an unknown icon name stops the build**, with a test for that;
  - **each page carries only its own icons**;
  - **Drive and Settings:** their hand-pasted icons become markers and look the same;
  - **Logo:** gets Drive's WiFi / battery / board icons, play / step-forward / octagon on RUN / STEP / STOP, camera / eye-off for HIDE, and a sonar icon;
  - **the `/logo` budget becomes 26 KB gzipped.**
- **Order:** Plan N6 is finished first (floor check, docs, merge). Then Plan N7: bridge LLD R25 "shared icons", a tested draft, the plan, the build, and a look at all three pages on the tank.

## 149. The Logo page as an editor on the laptop: built and tank-checked (Plan N6, 2026-10-24)
- **What:** Tank Logo LLD §23, as built in §23.8. On a laptop the `/logo` page fills the window and never scrolls:
  - programs · the editor with a PROBLEMS / MESSAGES panel · the camera with the sonar above the path;
  - a status bar;
  - Esc = STOP, ⌘/Ctrl + Enter = RUN, F10 = STEP, ⌘/Ctrl + S = save.
  - The phone layout is unchanged.
- **Checks:** Node 52/52. The laptop check passes at four window sizes; the phone check (N4) is unchanged. `/logo` 22 990 B gzipped.
- **The final review fixed:** the run pane overflowing short windows; held keys reaching the browser; Esc repeating 30 times a second; IME keys; the camera after a dropped link.
- **The owner moved ☰ to the far right.**
- **On the tank (owner):** "square good, esc stopped".
- **Open:** the top-bar battery / WiFi chips and the sonar read `--` in a quick test browser. To check with the icon work (Plan N7, §148).

## 150. One shared icon file: built and checked on the tank (Plan N7, 2026-10-24)
- **What:** bridge LLD R25, as built in R25.5.
  - Every icon on Drive, Settings and Logo comes from `firmware/esp32/web/icons.json` (63 Lucide 0.460.0 icons). They are put into each page when the firmware is packed: `<!--icon:NAME-->` markers, and one `/*CQ:ICONS …*/` table per page for icons that change.
  - **To add an icon:** `python3 tools/icons/add_icon.py NAME`.
  - **An unreadable marker, or an `icon()` name a page doesn't list, fails the build.**
- **Looks:**
  - Drive and Settings are pixel-identical apart from three icons (sparkles, send, ruler).
  - Settings' signal bars and Logo's text icons (`▂▄▆█`, `● UNO`, `▶ RUN`) became Lucide WiFi / battery / chip / play / step / octagon icons.
  - The WiFi and battery rules are Drive's on every page: a 10-reading battery average, and the warning from the tank's flags.
- **Final review:** no Critical; 1 Important and 8 Minor, all fixed test-first.
- **Sizes:** `/logo` 24.9 of 26 KB.
- **On the tank (owner):** "all good". This also closes §149's open item: the battery / WiFi readouts and the sonar show real values.

## 151. Things in the room from 18 cm: the detector is weak, we keep it for now (Plan V, 2026-10-24)
- **What we measured on the floor** (recorded with `cq-record`, replayed through the detector; pictures looked at, then deleted):
  - **A green patterned ball**, ~25 px wide at ~1 m and ~10 px at ~3 m: never "sports ball". With the ball area doubled in size the guesses were "suitcase" (0.61–0.68) and "potted plant", not on the ball.
  - **An office chair** at ~2.8 m (the floor trick; one pixel ≈ 0.5 m at that range with the camera level): "chair" 0.09 at best, in 9 of 56 pictures (the list needs 0.35). Other weak guesses: the cabinet as "bench" 0.13, the dark window as "tv" 0.11.
  - **Earlier:** the plush as "fire hydrant", and a person in a dark room scoring 0.0–0.08.
  - **The pictures** are pale and flat (median brightness 186 / 255), and the camera looks level from 18 cm.
- **Checked against pictures from the net:** the same adapter and model on the Ultralytics test pictures (bus, zidane) and COCO val 139 / 39769 finds a chair 0.75, a TV 0.89, cats 0.79 and people 0.84–0.94, at full size and at 320 × 240. The code is right; our view is the problem.
- **Owner:** from 18 cm "you will find nothing of the sort of chair … but legs". At the driving tilt the camera sees up to only ~65 cm high at 1.5 m.
- **Research** (owner: "search net … or we are cranks"): not cranks. This is the robot-vacuum problem.
  - **iRobot (j7) and Ecovacs** train their own detectors on their own floor-level pictures. Ecovacs names lack of data as one of its main problems.
  - **Public floor-level data is scarce:** ODSR-IHS (6 000 sweeping-robot pictures, 14 kinds), TiROD (a 24 cm rover).
  - **Obstacles:** the field uses geometry. Floor segmentation works better with a low camera; Depth Anything V2 runs ~20 / s with the best accuracy of the models compared; both are combined with the floor plane and sonar.
  - **Open-vocabulary detectors** (YOLO-World) are fast enough for robots (GPL-3.0).
  - **YOLOv5 is not the answer** (owner asked): the paper that used it trained it on its own 300 floor-level pictures. A COCO-only v5 / v8 / 26 sees from human height alike.
- **Owner: "leave it, go with what we have."** YOLO26n (COCO) stays. Plan V is finished with today's results.
- **For a later design:**
  - **obstacles first:** the floor plane + depth + sonar, which is map phase 5;
  - **names only for people and balls:**
    - a detector trained on the tank's own labelled pictures (perhaps starting from ODSR-IHS);
    - or YOLO-World asked in words;
    - or a colour-and-shape ball finder.


## 152. Spotting things from 18 cm: camera settings, five options, and YOLO-World (2026-10-24, daylight)
- **Setup:** the tank still, the camera level (servo 90 / 90) at VGA. A green patterned ball about 1 m from the lens (~44 px wide), a plastic bottle beside it (~40 × 120 px), an office chair ~2.8 m away. Recorded 8 s at each of three camera settings with `cq-record`, then compared offline on the Mac. Every 4th picture went to the detectors, every picture to the colour finder. Pictures looked at, then deleted.
- **Camera settings** (all VGA, low light on, white balance "sunny"):
  - A = brightness 0 / contrast +1 (as it was), median brightness 188;
  - **B = −1 / +2**, median 190;
  - C = −2 / +2, median 165.
  - B is the steadiest for the colour finder. **Owner: set the camera to B** (saved on the tank).
- **YOLO26n (COCO), best score over a recording; the list needs 0.35:**

  | Option | Ball | Bottle |
  |---|---|---|
  | 320 model | ≤ 0.09 | 0 |
  | 640 model | ≤ 0.33 | ≤ 0.11 |
  | zoom into the floor strip (320 × 240 cut) | ≤ 0.32 | ≤ 0.23 |
  | contrast fixed in software (CLAHE on L) | ≤ 0.30 | ≤ 0.12 |
  | zoom + contrast | 0.37 in 1 of 49 | ≤ 0.22 |

  More pixels help (0.09 → 0.33), but it stays unsure.
- **A colour-and-shape finder** (OpenCV, no AI):
  - **how it works:** the largest round blob below the horizon in H 45–90, S ≥ 65, V ≤ 200. The ball sampled at H 54–74, S 72–108, V 130–185; the floor is the same hue (the "sunny" tint) but S ~30, V 230–255.
  - **found the ball in 187 of 194 pictures at B**, centre steady to ±1.6 px, 1.1 ms a picture.
  - **placed it 97 cm from the lens** from the ball's bottom edge (the owner set ~1 m). The QVGA calibration was used at half the VGA pixels, assuming the same field of view.
- **YOLO-World** (yolov8s-worldv2, GPL-3.0; the owner allowed the one-time install of Ultralytics' CLIP and the model download), asked for "ball", "green ball", "bottle", "chair", "person" on 20 pictures from B:

  | Asked for | Found (score ≥ 0.25) | Best |
  |---|---|---|
  | chair | 20 / 20 | 0.82 (YOLO26n: 0.09) |
  | green ball | 8 / 20 | 0.36, on the ball |
  | ball | — | 0.18 |
  | bottle | — | 0.11 |

  82 ms a picture with PyTorch on the M1 (CoreML not tried yet).
- **Proposal (owner "yes" to a design):**
  - YOLO-World replaces YOLO26n under the same `Detector` port;
  - a colour ball finder runs beside it for the child's balls;
  - Plan V's tracker stays as it is;
  - obstacles still come from the floor plane + depth + sonar (later).
  - Design: AI engine LLD §16.

## 153. What the platform is for: a fun way to learn real robotics (owner, 2026-10-24)
- **The trigger:** Plan Y's floor checks kept turning up new cases, each with a new hand-made rule (roundness, colour range, brightness, size, how fast a remembered thing gets vague).
- **The owner:**
  - the surroundings are chaotic, and rules will break down faster than we can build them;
  - we don't need precise measurement: strive for maximum accuracy, but accept loose tolerances. The tank isn't roving on Mars or doing anything mission-critical; it is to play around with;
  - it is a fun learning tool for robotics, where we keep the fun and push toward real robotics as far as hobby-grade sensors allow;
  - Tank Logo can grow, and the same language might later drive a robot arm;
  - this philosophy guides future phases.
- **Agreed: seven principles,** written into spec 0 §1.1:
  1. fun first, real robotics inside;
  2. aim for accuracy, accept roughness;
  3. general before special;
  4. safety is never traded;
  5. the language is the child's door;
  6. the robot type can change, the words stay;
  7. everything can be seen and replayed.
- **Play behaviours that make good lessons (owner: good cases to teach children robotics, more to add):**
  - chase or nudge the ball;
  - follow a person;
  - explore without bumping;
  - go to a named thing.
- **Does such a framework exist? Searched 2026-10-24:**
  - **Kid block coding with camera AI exists:**
    - PictoBlox with its Object Detection extension (about 90 objects, location and size; Quarky compatible);
    - HuskyLens / HuskyLens 2 in Mind+ (Scratch 3.0 based; a block that checks for a tracked object and gives its centre, width and height);
    - the Hiwonder DaDablock (ESP32, offline AI vision, Scratch and Python).
    - These react to the current picture. No remembered map, no "go to" a thing out of view.
  - **OpenBot (Intel Labs, MIT licence):** a phone on a ~$50 body, with person following (a detector plus visual servoing to keep the person centred) and autonomous navigation. It has a drag-and-drop "Playground"; whether that has a follow block wasn't confirmed. Aimed at students and researchers.
  - **ROS2 kits with depth cameras and mapping:** Hiwonder MentorPi (RTAB-VSLAM 3D mapping and navigation); Yahboom Raspbot (OpenCV / MediaPipe tracking); Luxonis rae (stereo depth, open). Aimed at teens and adults.
  - **Cozmo / Vector:** vision and an app for children, but closed. Now only community software (the wire-pod server, Python and Go SDKs) with vision and mapping through the SDK. No kid blocks found.
  - **Conclusion:** the pieces exist apart. We did not find them together: an open framework on cheap hardware, a child's simple language that uses a remembered map of named things, always-on safety, and words that carry over to another robot type. That is the aim. It is stated as a gap we did not find filled, not as a proven first.
  - **Sources:**
    - PictoBlox: thestempedia.com/product/pictoblox;
    - HuskyLens 2 + Mind+: wiki.dfrobot.com (sen0638 object tracking);
    - OpenBot: arxiv.org/pdf/2008.10631 and github.com/ob-f/OpenBot (open-code README);
    - MentorPi: openelab.io/products/mentorpi-m1-car;
    - DaDablock: openelab.io/a/s/products/hiwonder-dadablock-ai-vision-kit;
    - wire-pod Vector SDK: pypi.org/project/wirepod-vector-sdk;
    - rae: malwaretips.com/threads/robotics-access-for-everyone-rae.118582.
- **A cheap tank with a borrowed brain (owner: "pieces exist but are expensive - one uses a phone - nothing cheap running on the Uno and ESP32"; and "a laptop or home computer is standard now, so it's ok"):**
  - the smart robots put the brain on board (phone, Pi 5, Jetson); the cheap modules (HuskyLens on an Arduino, the ESP32 DaDablock) only see the current picture;
  - ours keeps the tank cheap (Uno + ESP32): it drives, runs Logo and stays safe with no computer;
  - the heavy thinking borrows a home laptop or PC;
  - **the aim:** a cheap tank that does real perception and mapping by borrowing a home computer, with a child's language on top (spec 0 §1.1).
- **What changes now:**
  - **Plan Y:** finish with play tolerances (±15 cm at 1 m, ±30 cm at 2 m) and add no more rules: the memory, chair / person and live-view checks remain. Plan Y's as-built note moves to §154.
  - **The next design** is play behaviours as lessons: the four cases above, the perception words in Tank Logo they need, and general layers underneath:
    - "something is there" from sonar plus a depth model;
    - names matched loosely (similar words as one kind, ~30–40 cm as the same thing);
    - a fuzzy map (about ±20 cm / ±10°).
  - **Later:** a slow smart look now and then, from a vision-language model.
- **Follow-up the same day: the camera's real tilt changes between sessions (Plan Y floor memory test).**
  - **What happened:**
    - after a battery change, the ball at 1 m (by its size in the picture: 22 px radius) was placed at ~148 cm with the camera tilted 15° down;
    - the picture fits ~14° down, but the model assumed ~10.4° (tilt0 +3.3° + 0.91 × −15°);
    - before the battery change, the same tilt gave ~103 cm;
    - after panning away and back the head also lands ~1° differently (SG90 kit slack, §137).
  - **Owner: a general fix, not re-measuring by hand.** A camera angle estimator keeps a running best guess of the real tilt, and how sure it is, from what the camera sees:
    - mainly the floor plane from a depth model;
    - floor motion against the wheels while driving;
    - things of known size when in view;
    - the servo command and calibration stay the starting guess.
  - **Order (owner: as recommended):**
    1. finish Plan Y with the camera level (where the tilt is right), noting the session-to-session tilt as a known limit;
    2. a spike measuring how well Depth Anything V2 *small* (Apache-2.0; the owner allowed the download) finds the tilt from the floor at 0°, −15° and −30° with the ball at a taped 1 m;
    3. the estimator goes into the next design, with the depth layer (map phase 5).

## 154. What spots things: YOLO-World and a colour ball finder, built and floor-checked (Plan Y, 2026-10-24)
- **Built** (AI engine LLD §16.7):
  - YOLO-World asked by name: 10 words in `config/vocabulary.yaml`, 6.7 ms a picture on the M1;
  - a colour ball finder for the child's balls, measured once with `cq-calibrate ball <name>`;
  - both under one detector plug, `detect(picture, camera)`; the camera at VGA.
  - 194 tests; matches Ultralytics on COCO 139.
- **The final review** (0 Critical, 4 Important, 9 Minor) was all fixed test-first:
  - a box is only its best word;
  - one ball stays one thing across pictures;
  - `cq-calibrate ball` sets the camera level first;
  - red hues wrap;
  - plain config errors;
  - one camera for a picture's horizon and its place in the map;
  - and smaller items.
- **Floor checks:**
  - **VGA lens:** the doubled QVGA lens holds; the tape decided against a fresh screen-board fit that was 5 % off.
  - **tilt0** refit after the mount fix: **+3.3°** (was +2.55°). The ball read +3 cm at 1 m, +10 cm at 1.5 m, +2 cm at 2 m.
  - **The colour finder** reaches ~1.5 m; beyond that YOLO-World's "toy" covers the ball.
  - **The first memory test failed:**
    - a remembered ball grew vague too fast, so a green reflection 1.5 m away took it;
    - the steel bin's reflections looked like balls.
  - **Owner-approved fixes:**
    - a thing unseen for 0.5 s is remembered from its last sighting;
    - a colour ball must be bright enough and the right size for its distance (236 / 243 sightings kept, 48 / 48 reflections dropped).
    - The redo passed: the ball kept its number through a 12 s look-away.
  - **Chair and person** listed with the camera level (the person ~2.3 m by tape).
  - **The live view** showed chair, person, ball and bottle; Ctrl-C ends cleanly.
  - Other balls weren't calibrated (deferred).
- **Known limits:**
  - **the camera's real tilt drifts between sessions:** after a battery change distances read ~1.3–1.4 × long. The general fix is the camera angle estimator (§153 follow-up);
  - **the chair at the driving tilt** scores 0.23 (its top is cut);
  - **the cabinet flips between bed and sofa.**
- **On the way, the principles were agreed (§153):** play tolerances, and no more per-case rules.


## 155. Depth spike: the floor in a depth picture gives the camera's real tilt (2026-10-24)
- **Question (§153 follow-up):** can Depth Anything V2 Small find the camera's real tilt from the floor? The model is Apple's CoreML F16 build, already downloaded for Q0 (§109), so nothing new was installed.
- **Method** (throwaway script, outside the repo):
  - **Depth:** the model's output is relative inverse depth. Over flat floor it falls in a straight line down the picture. Its zero row is the horizon, which gives the tilt. The line is fitted per picture to the rows between the tank and the ball, columns 220–420.
  - **Truth:** the ball's centre is taped 1 m from the lens. The ray through the ball outline's bottom pixel meets the floor at a known distance (99.4 cm), which gives the true tilt.
  - **Recordings:** 4 s at commanded tilts 0°, −15° and −20° (the servo stops at −20°), in two scenes. Only the camera moved. Recordings deleted.
- **Results** (pitch down, degrees):

  | Scene, command | Truth | Calibration + servo | Depth | Truth − depth |
  |---|---|---|---|---|
  | 1, level | −0.8 | −3.3 | −5.6 | 4.8 |
  | 1, −15° | 11.7 | 10.4 | 7.5 | 4.2 |
  | 1, −20° | 15.9 | 14.9 | 11.5 | 4.4 |
  | 2 (other wall, bright light), level | −1.7 | −3.3 | −5.7 | 3.9 |
  | 2, −15° | 11.4 | 10.4 | 7.8 | 3.6 |
  | 2, −20° | 16.0 | 14.9 | 12.5 | 3.5 |

  - **The depth floor is steady:** 0.1–0.2° from picture to picture, and the line fits to 0.2–0.4 %.
  - **It follows tilt changes** within 2.5 %.
  - **Offset 4.1° ± 0.45° over both scenes** (the model's unknown shift). With one fixed correction the tilt is good to about ±0.7°: about ±7 cm at 1 m and ±25 cm at 2 m, inside the play tolerances.
  - **Calibration + servo** is 1–2.5° off today (3° after this morning's battery change). The servo really moves 0.835° per commanded degree, not 0.91.
- **Side finding:** in scene 2's bright light the colour finder missed the ball. The lit side reached value 219, over the green ball's `v_max` 200, so only 30 % of the ball was in range. This is the kind of per-case rule that breaks (§153). The finder stays a helper; re-running `cq-calibrate ball` in bright light would widen its range.
- **Owner: adopted (2026-10-24, "yes").** The camera angle estimator's main clue is the depth-floor tilt with a fixed ~4.1° correction, starting from calibration + servo. It is designed together with the depth layer ("something is there") and the play behaviours as the next design.
- **To tighten it later:**
  - the sonar's one true distance ahead (fixes the offset per scene);
  - floor motion against the wheels while driving;
  - the metric model.

## 156. Module 2: drive while thinking; thin slices, chase the ball first (owner, 2026-10-25)
- **Order (owner): thin slices, one behaviour at a time, starting with "chase the ball"** (not bottom-up). The slices:
  1. chase the ball;
  2. explore without bumping (depth, the camera angle estimator, the grid);
  3. follow a person;
  4. go to a named thing.
- **The end goal (owner):** "most robots I see move jerkily: drive, stop, drive, stop. If there is a toy or an obstacle in front, the tank should, while driving, keep in motion, scan the path and manoeuvre in one smooth move."
- **Agreed (owner: "yes, go as recommended, write the spec"):** a **drive-while-thinking loop** in the AI engine, about 10 times a second:
  1. its pose and the target from the map (lag covered by the pose history);
  2. a fan of curves within what the tank can do (speed ~18–33 cm/s, tightest smooth curve ~31–38 cm, slowest smooth spin ~65 °/s);
  3. each curve scored on progress, clearance and smoothness;
  4. one `drive_rate` (speed + turn rate) per tick, with gentle ramps.
  - It slows when tight and stops only when every curve is blocked. The camera pans into the turn.
  - The Uno brake stays the last net, and the planner keeps a margin so it rarely fires.
  - The smoothness lives in the planner, not in the child's program.
- **Spec 8 written** (`docs/superpowers/specs/2026-10-25-08-drive-while-thinking-design.md`, draft for review). It covers the module-2 architecture and slice 1. For the owner's review:
  - **new Logo words:** `SEE?`, `FIND`, `GOTO` (not `GO TO`, because `TO` starts a procedure), `DISTANCE`;
  - **a change to spec 5 §6:** every program runs on the ESP32's runner, and the AI engine is a skill server (eyes and careful driver) with a drive lease. This resolves the clash between §6 (an AI-engine runner) and Revision 2 (the only parser on the ESP32);
  - **the ball's distance from its size** (independent of the tilt drift) as well as the floor trick.
- **LLD written (owner: "write impl"; the LLD comes first by the project's order):** AI engine LLD §17, bridge LLD R26, Tank Logo LLD §24, all drafts for review. Findings from the code that changed spec 8:
  - **`DISTANCE` is already the sonar's word**, so the AI word is **`HOWFAR "ball`**;
  - **`SEE?` / `FIND` already existed at level 3** (not in v1) and move to level 2;
  - **a laptop-answered question can't use the instant sensor op:** it becomes an asking effect with a 300 ms wait (no answer = FALSE / −1);
  - **today a second client's `drive_rate` is refused (or ends the program):** the drive lease is a new path, the bridge's `SkillHub` plus a `kSkill` motion primitive with the same endings and deadman as `GO`;
  - **the AI engine's `BridgeLink` stays motion-free:** driving goes through a separate `DriveLink`;
  - **new errors 7050–7053:** not found, lost sight, the AI helper stopped answering, only "ball for now.

## 157. Plan Z1 floor checks: chase the ball (2026-10-27)
Plan Z1 (spec 8 slice 1, AI engine LLD §17.10 "As built") on the floor with the owner, the green ball, STOP in hand. Summary: every chase reached the ball (§157.1-§157.4); the box, the phone STOP and Ctrl-C passed (§157.5-§157.6); the speed table holds (§157.7). The fixes found on the floor: turn on the spot beyond 35°, end the spin 15° early, a stale map pauses, standing blocked 3 s ends the run, a quiet link close. Before this, the final review's fixes and the bridge STOP fix (ledger; LLD §17.10).


### 157.1 Turn on the spot first when the ball is well off to one side (owner, 2026-10-27)
- **Floor chase 5:** the green ball 60 cm ahead and 50 cm right of the lens (78 cm, ~40° right).
  - It found the ball on the first look ahead; the camera followed it right; the drive was smooth (2.5 s, no stops).
  - **But it swept past:** the planner chose a 50 cm curve at 25 cm/s (28.6 °/s). By the encoders and gyro it drove 59 cm and turned 47° right, ending ~25 cm from the ball with the ball ~20° right of the nose. That is inside the camera's blind zone, so it lost the ball, and the sonar (28-30 cm) ended it `blocked`.
- **Why slowing doesn't help here:** the inner track can't run slower than ~17.7 cm/s, so a slower tank curves *wider*, not tighter (`r_min`, LLD §17.4). Turning on the spot is the only tight turn.
- **Decision:** a target more than **35°** off gets only a spin in place (`SPIN_FROM_DEG` 70 → 35, and the spin is the only choice, not just offered). The tank turns to face the ball, then drives in.
- **Simulator** (same setup, `near-40-right` case in `test_drive_sim.py`): before, it ended 0.29 m from the ball; after, 0.44 m, done in 2.3 s. The other cases are unchanged (ahead 4.8 s, 90° right 9.4 s, behind 11.8 s), except 45° left, which takes 6.9 s instead of 6.5 s because it now turns first.

### 157.2 A short WiFi gap pauses the chase; the spin ends 15° early (owner, 2026-10-27)
- **Floor chase 6** (the same setup as chase 5, after §157.1):
  - It drove straight ~5 cm while the camera turned to the ball, then turned on the spot to the right: **55°** for a ball ~40° off (stopping at 8° left; the pose lags and the tank coasts on after the stop).
  - It then curved left smoothly (1.7 s, no stops) and was facing the ball when it ended.
  - **It ended `stale - no telemetry for 0.2 s`** about a second before arriving: the sonar read 33 cm to the ball's surface (~41 cm lens to centre, the stop range). Telemetry over WiFi often arrives up to 0.12 s apart (three gaps of 0.116-0.123 s in this run), so 0.2 s is crossed now and then.
- **Decisions:**
  - **A stale map pauses, it doesn't end the run.** The tank gets a STOP at once (not a ramp: it can't see), and drives on, ramping from standing, when pictures and telemetry are fresh again. Only 2 s of it ends the run (`STALE_GIVE_UP_S`, reason `stale`). The 0.2 s / 0.5 s staleness limits and the bridge's deadman are unchanged: it still never drives on a stale map.
  - **The spin ends with 15° left** (`SPIN_UNTIL_DEG` 8 → 15); the drive's arc aims the rest.
- **Simulator:** ahead 4.8 s, 45° left 6.7 s, 90° right 9.4 s, behind 11.8 s, the chase-5 setup 2.1 s; all end 0.43-0.45 m from the ball.


### 157.3 Floor chase 7: ball 40° right at 78 cm - done (2026-10-27)
- **Result `done` in 3.3 s;** the owner: "it looked good". The sonar read 336 mm at the end (~41 cm lens to the ball's centre; target 40 ± 15). It ended facing 43° right of its start, where the ball was.
- **The spin overshot:** right to ~69°, then back left to ~22°, then a smooth arc right at up to 25 cm/s. It's likely the first fix of the ball's position, taken while the camera was still turning toward it with the ball at the edge of the picture; to be checked against the recording.
- **WiFi gaps up to 0.32 s** (13 over 0.1 s) didn't end the run (§157.2).
- **Next (owner, 2026-10-27):** finish Plan Z1's floor checks; then slice 2 (explore without bumping) before nudging the ball. Nudging needs its own safety design: the Uno's obstacle brake stops 15 cm from anything and is locked on in child play.

### 157.4 Floor chases 8-10: 90° right, behind, a rolling ball - all done (2026-10-27)
| Chase | Setup | Result | What it did | End (sonar → lens to ball centre*) |
|---|---|---|---|---|
| 8 | ball 1.2 m right, level with the lens | done | looked ~4.5 s (found at the 90° right look); spun right ~101° in one turn; 48 cm nearly straight at up to 23 cm/s; a short spin left at the end to face it | 291 mm → ~37 cm |
| 9 | ball 1.5 m straight behind | done | looked ~5 s; the half turn spun **~210°, not 180°** (it's timed at 65 °/s, the tank spins a bit faster); looked again ~3.5 s, found at a 45° look; one smooth 103 cm curve at up to 25 cm/s; ~16 s in all | 415 mm → ~49 cm |
| 10 | ball 2 m ahead; it started rolling (erratically) after the tank set off | done | followed it 3.4 m over 14 s, curving right, then left, then right; one 0.15 s stop at a WiFi gap (the §157.2 pause, then on); owner: "it was cool, very cool ... the tank was still able to follow" | 495 mm → ~58 cm (just outside 40 ± 15) |

\* sonar + 4.5 cm (the sonar ahead of the lens) + ~3.5 cm (the ball's radius).

- **Noted, not fixed:** the search half turn is timed, so it overshoots (~210°); a gyro-measured half turn would be exact. It found the ball anyway.

### 157.5 Floor chase 11: a box in the path; blocked ends after 3 s; Ctrl-C exits cleanly (owner, 2026-10-27)
- **Setup:** the ball 2 m ahead, a low box (~10 cm, wider than the tank) about 1 m ahead (sonar 797 mm at the start).
- **Safety passed:** it drove 41 cm (2 s, up to 21 cm/s), slowed, and stopped **~38 cm before the box** (sonar 382 mm); it never touched it.
- **But it stood there ~4 minutes:** every curve was blocked, so the planner chose STOP each tick; the `blocked` ending fired only under `clear_m` (30 cm), which the stopped tank never reached. Stopped by hand (SIGINT).
- **And Ctrl-C crashed on the way out:** the bridge had reset the camera-side link meanwhile; `BridgeLink.close()` raised sending "debug off" instead of printing the result.
- **Owner's discussion (the practical cases):** someone stepping in the way, a box between the tank and the ball, the ball against a wall or under a chair, a chair leg, an obstacle hiding the ball. Going around blind risks clipping a chair leg; going around properly needs the obstacle map. **Decision:** leave going around (and waiting for the way to clear, and telling the cases apart) to **slice 2**; now only:
  - **standing with every curve blocked for 3 s ends the run `blocked`** (`BLOCKED_GIVE_UP_S`): slice 2 needs it too, when there is no way round;
  - **closing a link the bridge already dropped is quiet**, so Ctrl-C prints its line.


### 157.6 Floor chase 12 (the box again), the phone STOP and Ctrl-C (2026-10-27)
- **Box again** (same setup as §157.5): drove ~35 cm straight, then curved ~13° left looking for a way past, slowing to ~20 cm/s; the sonar fell from 31 cm to under `clear_m` and it ended **`blocked` at 2.7 s** (the 30 cm rule, not the 3 s wait). It never touched the box. The program ended on its own.
- **Phone STOP mid-chase** (the owner pressed it): the run ended `refused by the tank (STOPPED)` after ~2 s at 25 cm/s.
- **Ctrl-C mid-chase** (SIGINT 2 s after the tracks started, watched by a second telemetry client): the **tracks stopped 0.20 s** after it; the program printed "stopped (Ctrl-C): the tank was told to stop" and exited 0 (§157.5's fix).

### 157.7 The speed table on the floor (2026-10-27)
Straight on, 1.5 s at each speed without stopping (`drive_rate` through `config/drive.yaml`'s table); speed = the encoders' distance (7514 counts/m) over the last 0.8 s of each step:

| Asked | `drive_rate` | Measured |
|---|---|---|
| 18 cm/s | 17 % | 16.5 cm/s |
| 22 cm/s | 40 % | 22.1 cm/s |
| 25 cm/s | 56 % | 26.5 cm/s |

Within ±1.5 cm/s (≤ 8 %): **the table stays** (loose tolerances, spec 0 §1.1; the loop re-plans from the measured pose every tick).

**Plan Z1's floor checks are done** (§157.1-§157.7): ball ahead-right, 90° right, behind, rolled away, a box in the path, the phone STOP, Ctrl-C, the speed table. No stutter (no tick gaps over 0.5 s; the only stops were the §157.2 WiFi pauses). Not run on the floor: "no ball anywhere" (the same search as chase 9's first sweep and half turn; the simulator covers the `not_found` end).

## 158. The camera servo's speed: slow on purpose, decide in slice 2 (owner, 2026-10-27)
- **Owner:** "the servo is slow - is that its natural speed or are we running it slow?"
- **We run it slow:** the SG90-type servos manage ~600 °/s (0.1 s per 60°); Plan W (Uno Lite LLD §23, design-notes §137) caps the camera at 120 °/s, with a 0.16 s soft start, a quarter-of-the-gap soft stop and a 100 ms target average, for an RC-transmitter feel on the phone's stick. Measured in the chases: ~0.5 s per 45°, ~0.8 s per 90°.
- **Cost to the chase:** ~1.5-2.5 s of a ~11 s full search; most of the search is the 0.8 s of still pictures per look.
- **Options:** leave it; a fast mode (~300 °/s) only for moves the laptop asks for (the sweep, Logo `LOOK`), the stick unchanged (the Uno has 654 B of flash left; the servo wobbles after a fast stop); faster for everything (undoes Plan W).
- **Decision:** leave it for now; decide in slice 2, after measuring on the stand how long the camera wobbles after a ~300 °/s move.

## 159. The base board made arm-ready: servo feedback on every servo header (owner, 2026-10-27)
- **Context:** a separate project, a general-purpose platform for cheap hobby servo arms (`../arm-robot`, design discussion only so far): "what a $30 arm can do against a $300 machine (SO-101)". Cheap servos (MG996R, SG90) report nothing; a wire soldered to each servo's pot wiper (the "feedback mod") gives real joint angles. Two arm profiles: Basic (servos untouched, the angle estimated from the commanded pulse + a per-servo model) and Feedback (the pot wire), set per joint. Boards for that project: Uno R4 recommended, our base board (spec 06) the best one.
- **Owner:** keep the servos on the RP2354B's **native PWM** (finer than a PCA9685, no I2C to hang); the board plays **one role at a time** (tank/car *or* arm), keep v1 simple.
- **Decided (spec 06 Rev 1, §12):** 4-pin servo headers (signal, V+, GND, feedback); servo 1-2 feedback on ADC6-7 (the future motor current sense dropped: the tank's encoders already find a stall); servo 3-6 feedback on ADC2-5, shared with the expansion analog inputs (use one or the other, no switches); every analog input through a 1/3 divider (0-9.9 V, 5 V-safe; the pot swings to the servo rail, up to 8.4 V on BATT); servo rail current on ADC1; VBUS moves to GPIO37 as a digital input (the GY-271 DRDY is not needed, the compass is polled).
- **Not taken:** a TLA2528 8-channel ADC (~US$1) for the pots — only needed for an arm on a tank or analog add-ons on an arm.
- **Open (spec 06 O8, proposed):** a firmware-controlled servo rail switch, a start without a jump, a 6-8 A servo buck for arms.

## 160. The base board as the arm platform's best tier: USB link, STOP freezes (owner, 2026-10-27)
- The arm project's own record starts in `../arm-robot/docs/design-notes.md` (no git yet): purpose, servos Basic / Feedback, the owner's arm, arms as description files, research, boards (Uno R4 recommended, this board best), USB first, STOP freezes, electronics reference builds.
- **Spec 06 §12.1:** in the arm role USB is the data link to the laptop (USB CDC serial) — rule 3.1.4 stays for motors only, servos move with USB attached; **STOP freezes the arm** (the servo rail stays on, every servo holds its last pulse; the straining / go-limp details belong to the arm's protocol); a 1000-2200 µF bulk capacitor on the servo rail.

## 159. Slice 2 (explore without bumping): what "explore" means (owner, 2026-10-27)
- **Design started** (brainstorming; architectural, so: questions → approaches → design → spec 9 → plans). Most parts were already designed in spec 7 (depth → "nearer than the floor", the sonar model, a 3 × 3 m log-odds grid, 5 cm cells, fading only in view) and §155 (the camera angle from the depth floor).
- **Proposed split:** A. depth + the camera angle estimator; B. the obstacle grid; C. the drive on the grid.
- **Owner's answer:** first **wander and turn away** (drives about, avoids what it meets, until stopped), then **visit everything** (goes where it hasn't seen yet, until the area is covered, then stops). **"Wander toward something"** (roam until a ball or a named thing is spotted) should come **for free** from those two plus the chase, not as its own build.
- **So:** the explore behaviours are goals for the same drive loop; "find X" is explore + the detector's sighting, handing over to the chase.
- **Where it may go (owner, 2026-10-27):** **the whole room, as far as it can see** (option 2); no fence for now. A fence (a circle around the start, or one drawn on the phone) is decided later. Known gap: the tank can't see drops (stairs, a step edge); floor runs stay with the owner present and STOP in hand.
- **Overhangs (owner, 2026-10-27: "a bed - the camera will get stuck there"):** the sonar is low and looks under a bed; the camera (lens 17.8 cm up) is the tank's highest part; close up the edge leaves the view at the driving tilt; and a plain grid would let the sonar's echo from the wall behind erase the bed edge. **Decision (general, no "bed rule"):**
  - **marks carry a height:** any point the depth picture places between **2 cm and 20 cm above the floor** is an obstacle, standing or hanging (owner: "keep 20 cm safe"); things higher than 20 cm can be driven under;
  - the grid remembers them out of view (spec 7);
  - **the sonar may clear only low marks**, never a high one;
  - the backstop stays: pushing but not moving (Plan R3) ends a drive in ~2 s, and STOP.
  - To check when building: the tank's highest point (camera holder) must sit well under 20 cm; if not, the limit rises.
- **How wandering ends (owner, 2026-10-27: option 3):** after a time if one is given (e.g. `WANDER 60`), otherwise when stopped (STOP, Ctrl-C, the program ending). Boxed in (a corner): it turns on the spot and carries on. "Visit everything" ends when nothing unseen is left.
- **The map (owner, 2026-10-27):** **one room-sized map fixed where the tank started**, ~8 × 8 m, 5 cm cells (~26 000), holding obstacles (with their height, above) and "already seen", for the whole run; it replaces spec 7's 3 × 3 m grid that moves with the tank. Pose drift (~3 % of the distance, ~15 cm across a 5 m room) is accepted for play; the map's edge is treated as a wall (a soft fence until the real one).
- **Finding the way (owner, 2026-10-27):** **B, a route on the room map, followed by the fan of curves**: several times a second a route around the marked obstacles (keeping the tank's half-width + a margin) to where it wants to go; the fan drives smoothly toward the route's next point. The same route serves wander, "visit everything" (the nearest unseen place) and the chase (around the box instead of `blocked`). Rejected for now: A, the fan alone (trapped behind wide or U-shaped obstacles). **C** (plan the whole smooth path at once) is **to look at later**.
- **Spec 9 written** (`docs/superpowers/specs/2026-10-27-09-explore-without-bumping-design.md`): the four sections approved by the owner one by one (seeing and the room map; goals, routes and the fan; safety; testing and the split into Plans Z3 seeing, Z4 the room map, Z5 driving on the map). Waiting for the owner's review of the written spec.
- **Spec 9 approved by the owner (2026-10-27).** Next: the LLD (AI engine LLD §18, all of slice 2 so Z3-Z5 share one set of interfaces), then Plan Z3.
- **LLD written (AI engine LLD §18, draft for review):** Z3 seeing (`DepthCoreml` 518 × 392, `CameraAngle` from the depth floor's horizon + 4.1°, median over 1 s, trusted within 3°; `DepthScaler`; `HeightPoints` floor < 2 cm, low 2-10 cm, high 10-20 cm, above > 20 cm), Z4 (`RoomMap` 160 × 160 log-odds low / high + seen; `SonarModel` touches only low; `cq-map`), Z5 (`ClearanceMap` by OpenCV distance transform, A* on a 10 cm grid, robot radius 15 cm, aim 0.5 m along the route; wander / explore / find goals; `RoamSkill`). Low / high is split by height at 10 cm, where the sonar's beam stops vouching. No new installs.
- **LLD §18 approved (owner, 2026-10-27: "implement").** Next: Plan Z3 (seeing), branch `plan-z3`.
- **Plan Z3 approved (owner, 2026-10-27: "yes"),** run natively on branch `plan-z3`.
- **Plan Z4 written and approved (owner, 2026-10-28: "yes"),** run natively on branch `plan-z4` (`docs/superpowers/plans/2026-10-28-plan-z4-room-map.md`). Two choices beyond LLD §18.5: a cell moves **one step per picture** however many points fall in it, and **a hit beats a miss** in the same cell in the same picture (the map never invents free space).
- **Plan Z5 written and approved (owner, 2026-10-29),** run natively on branch `plan-z5` (`docs/superpowers/plans/2026-10-29-plan-z5-drive-on-the-map.md`). **The sonar's "last word" distance is 10 cm, not 30 cm** (owner: 30 cm is too much; `Limits.clear_m` 0.30 → 0.10, for the chase too): an echo under 10 cm from the tank's front that isn't the target ends the run. The Uno's own 15 cm brake is unchanged and will usually stop the tank first; its firings are logged as near misses on the floor.

## 161. Plan Z3 floor checks: depth, the camera's real angle, heights (2026-10-27/28)

Plan Z3 Task 8, the owner present, the tank standing still (only the camera tilted, to the driving −15°). Recordings with `cq-record`, numbers from `cq-objects` replays and scratch scripts over the same recordings; all recordings deleted afterwards (they show the home). (§160 and a second §159 are the base-board notes, so these are §161.)

### 161.1 Rate: depth on every picture
- 20 s standing at −15°: 421 pictures (21.0 / s from the tank).
- Replay with depth: **18.8 pictures / s** on the M1 (`cq-objects`' own "laptop" line: pictures used / replay time); without depth 419 pictures in 6.8 s wall including start-up (> 60 / s).
- **Pass** (≥ 5 / s, LLD §18.4): `depth.every_n` stays 1.

### 161.2 The camera's real angle: the correction is 3.0°, the servo's level point 0.7°
- **Set-up:** the green ball taped straight ahead of the lens at 1.0 and 2.0 m; place A (hall, toward a door) and place B (another floor patch and direction); 10 s each; five recordings (place A 1 m was recorded twice because the tank was switched off and on between the first 1 m and the 2 m).
- **The truth:** in pictures 20, 40 … 240 of each recording, the ball's round green blob; its bottom (centre + enclosing radius) put on the floor with `pixel_to_floor`, as the object pipeline does; the tilt that puts it at the tape distance from the lens (x − 8.5 cm), found by bisection; the median of the 12.
- **The depth:** `depth_tilt_deg` with no correction on the same pictures (median); the correction that fits = depth − truth.

| Recording | Servo + tilt0 3.3 | True tilt | Depth (no correction) | Correction that fits |
|---|---|---|---|---|
| A 1 m (first power-on) | −10.35° | −11.08° | −8.70° | 2.38° |
| A 2 m (second power-on) | −10.35° | −12.67° | −9.86° | 2.81° |
| A 1 m (second power-on) | −10.35° | −12.93° | −9.83° | 3.10° |
| B 1 m | −10.35° | −14.31° | −11.26° | 3.06° |
| B 2 m | −10.35° | −13.89° | −10.42° | 3.47° |

- **The correction: 3.0°** (mean 2.96°), not §155's 4.1° (the spike found the floor line another way; ruling M2 of Plan Z3 said to measure it again with this code). With 3.0° the depth tilt is within **−0.62, −0.19, +0.10, +0.05, −0.47°** of the truth: **pass** (spec 9 §6.1, within 1°).
- **The camera sits 0.7-4.0° lower than the servo reports**, and it changes between power-ons (A: −11.1° then −12.7° for the same command). On the servo alone the 1 m ball read 108-166 cm and the 2 m ball 3.6-6.5 m (the ball finder's size check then refused it). The depth follows the change.
- **tilt0 3.3° → 0.7° (owner, 2026-10-28: yes):** 3.3 + the median true fix (−2.58°). With 3.3° two of five fixes were past the 3° trust limit and dropped; with 0.7° the true fixes are +1.85…−1.38°, all inside. The trust limit stays 3° (owner: a calibration that is off is not a reason to loosen a safety check).
- **Replays with both** (`cq-objects`, ball x − 8 cm from the lens): A 1 m **93 cm** and **101 cm**, B 2 m **223 cm** (pass: 10 cm at 1 m, 25 cm at 2 m); the fix trusted on every picture of all five recordings. The old values (4.1°, 3.3°) gave 85 cm and 135 cm and dropped the fix in four of five.
- **The ball finder missed the ball in two recordings even at the right tilt** (A 2 m, B 1 m): the size is right (13.3-14.0 cm), but the roundness was 0.55 and 0.66 against `MIN_ROUND` 0.66 (the ball's blue spots leave holes in the green mask). A Plan Y limit, not depth: **a small follow-up after Plan Z3** (fill the holes before measuring roundness).

### 161.3 Heights: the bed edge and a box
- **Method:** `Seeing` over 20 pictures of a 10 s recording; points within 60 cm either side of straight ahead, counted by class in 10 cm bands of distance from the lens.
- **The bed** (the owner: front 1.0 m from the lens, the panel's underside **15 cm** up): floor only to 0.80 m; **0.80-1.00 m: ~11 000 points high (10-20 cm)** (the panel's lower edge), ~94 000 above 20 cm (the panel), floor seen through the gap under it; low and high marks behind it (under the bed). Earlier at 1.4 m: the same pattern at 1.10-1.40 m.
- **A box** (~10 cm, by its size in the picture) at ~70 cm, left of centre, with the bed behind: **60-80 cm: ~2 900 points low, ~120 high**.
- **Pass** (spec 9 §6.2 for this plan): neither the box nor the bed edge shows as floor within 1 m.
- **The depth puts things ~10 cm near:** the bed at 0.85-0.95 m for 1.0 m and 1.20-1.35 m for 1.4 m - on the safe side and inside spec 9's 15 cm. No chair was tried (the bed covers "above 20 cm").

### 161.4 The safe band (owner, 2026-10-28)
- **The "high" band stays 2-20 cm** (`heights.clear_m` 0.20): anything up to 20 cm above the floor is an obstacle; the tank's height was not measured against it (owner: keep 20 cm).

### 161.5 Found on the way
- **A switch on the tank toggled wrong** (owner) made `cam_set` answer `COMMAND_TIMEOUT` while telemetry still flowed; once it was set right the camera moved. A restart also puts the camera back to level, so a check at the driving tilt sets it again after every power-on.

## 162. Plan Z4 floor checks: the room map (2026-10-28/29)

Plan Z4 Task 6, the owner present and driving with the phone; `cq-map --live --record` only watched (it never drives; the camera tilted to −15° first). Numbers from `cq-map`'s once-a-second status lines and its report, and from scratch scripts replaying the same recordings (the map printed in 10 cm squares around the start at chosen pictures); recordings deleted afterwards (they show the home).

### 162.1 Rate
- **10-22 map updates a second** (status lines: mostly 12-17 / s in the first run, 19-21 / s standing in the second, every picture through depth): **pass** (spec 9 §3.8: 10 or more). Not stale after the first second.

### 162.2 A drive with a box and the bed (recording A)
- About 90 s standing at the start, then a short drive (the path from the pose: x 0 to 1.1 m, y 0 to −1.9 m, ending at (0.40, −0.46) turned +57°); 2 205 pictures (all with depth and height marks), 1 587 echoes used; 20.9 m² seen, 531 low and 1 332 high cells blocked.
- **The box** (~10 cm, ~65 cm ahead, left of centre): marked low at x 0.6-0.8 m, y +0.1-0.3 m while standing (a few high cells on its top, within the depth's noise at the 10 cm boundary); **still marked at the end**, after the tank had moved and turned (the sensor's last echo, 56 cm, is the box): **pass** (memory).
- **The bed edge**: a high band ~1 m ahead while standing; at the end the band lay 10-20 cm farther (x 1.0-1.3), never shown as floor (see 162.3 for why it moves).
- **The same answer twice:** two replays of recording A gave the same report (2 165 pictures used, 1 587 echoes, 20.9 m², 536 low, 1 333 high; 198 s on the laptop). (The live run used 40 more pictures: the newest picture wins while depth runs.)

### 162.3 The bed edge and the distance sensor (recording B)
- The tank ~1 m in front of the bed, facing it (panel underside 15 cm up, §161), ~90 s standing, then 39 cm forward and standing.
- While standing the bed band was first at x 1.0-1.1 m and **settled at 1.2-1.3 m after ~30 s** (the depth picture's scale fit moving; §161 found depth ~10 cm near at this range); the nearer marks were cleared by floor seen there. After the drive it stayed at 1.2-1.3 m and **was never cleared**.
- **The truth:** the sensor read 1.11 m at the start and 0.71 m at the end, both putting what it hears at 1.23 m from the start; **the owner's tape: tank front to the panel 70 cm**, so the panel is at 0.39 + 0.13 + 0.70 = **1.22 m**. The map is right: **pass** (spec 9 §6.2 for this plan: the overhang is never shown free; the sensor never clears a high mark).
- A small high patch appeared ~45 cm ahead on the left while standing (the owner's foot or hand) and stayed (out of view after the drive): memory works both ways.

### 162.4 Not checked on the floor
- **Stale:** covering the camera doesn't make the map stale (dark pictures still arrive); the flag is checked by `test_live_stale_reported` and the same rule (`map_state`) was floor-checked in Plan Z1 (§157.2).
- **A two-minute loop of the room:** the drives were short; Plan Z5's wander covers the room.

## 163. The tank's body, measured (owner, 2026-10-29)

Plan Z5's final review (C1) found turns on the spot unchecked against the body; the fix used cautious placeholders (front 14, rear 14, half-width 11 cm). The owner measured the tank with a tape, from the turning centre (halfway between the tracks, the pose's point), tracks included:

| | Owner (inches) | cm | `Limits` (rounded up to 0.5 cm) | Placeholder |
|---|---|---|---|---|
| Front (frontmost point) | 5 | 12.7 | `body_front_m` 0.13 | 0.14 |
| Rear (rearmost point) | 3.5 | 8.9 | `body_rear_m` 0.09 | 0.14 |
| Width (outer track edges) | 9 | 22.9 → half 11.4 | `body_half_w_m` 0.115 | 0.11 |

- The sides were **wider than the placeholder** (11.4 vs 11 cm): the fan's 12 cm map check on the tank's centre leaves 0.5 cm at each side. A turn on the spot now needs 19.4 cm clear round the centre (the front corner, hypot(13, 11.5) = 17.4 cm, + 2 cm), was 19.8.
- `drive.yaml` may still only make the body bigger. A body wider than 11.5 cm now widens the fan's map check to its half-width + 0.5 cm (`SIDE_MARGIN_M`), so the sides stay clear (`test_a_wider_body_keeps_its_sides_clear`). Routes keep `ROBOT_R_M` 15 cm (a constant): with a much wider body the fan refuses what a route allows and the run ends `blocked`, never a touch.

### 163.1 Two behaviours beyond AI engine LLD §18.7, approved (owner, 2026-10-29)
Added in Plan Z5's final-review fix pass (review C1); the owner approved both before the floor runs:
- **Too near to turn, the look turns the camera:** a look whose turn on the spot isn't safe pans the camera through −45°, 45°, −90°, 90° instead (`LOOK_PANS`).
- **Too near to turn, drive out first:** a turn on the spot is refused there, and the fan offers its forward curves; the tank drives out to where it can turn. It never reverses (no rear sensor).

## 164. Floor runs 1-2 and the body checked along every move (owner, 2026-10-29)

**Run 1, `cq-drive wander 120`** (recording replayed with the map, path from the pose filter): `done` after 120 s, **no touches** (owner), smooth driving; 23.6 m driven but all inside ~2 × 2.4 m by one corner of the bed, back and forth between two ends ~2.3 m apart (12 short stands, mostly turns on the spot at the ends). The map: 24.6 m² seen, 7.7 m² of it marked. Wander only picks spots on floor already seen, 1.5-2 m away, mostly ahead - it shuttles in a hemmed-in patch (owner: it should go round the room; a wander change is to discuss later).

**Run 2, `cq-drive explore`:** `blocked` after ~30 s and 0.7 m. The camera pictures show the tank beside the bed: its side panel ~15 cm to the right, its corner leg ahead-right, open floor ahead-left and left. The map's marks were right (high: the panel hangs ~15 cm up). The way on was behind-left (bearing 142°): the full-circle turn check (19.4 cm needed, 15 cm there) refused turning on the spot, and every forward curve passed the leg ~11 cm from the centre (the body 11.5 cm): refused, rightly. Three looks from the same spot ended it.

**Decision (owner: option A):** check the tank's real shape along every move. For each curve and each turn on the spot, the body's rectangle (front 13, rear 9, half-width 11.5 cm, §163) is rolled out along the move and its outline checked against the room map: the front edge keeps 5 cm (as the front point did), the rest of the outline 2 cm. A turn on the spot is checked only for the way it turns (here left passes, right is refused). The tank already that close may still move as long as no part of it gets nearer. This replaces the three rough checks (the centre 12 cm off, the front point, the whole turning circle). Option B (a short slow reverse along the way it came) left out unless A isn't enough: nothing sees behind the tank.

- **As built:** the outline (front edge, rear edge, both sides, a point every 2.5 cm) is checked at poses at most 5° / 2.5 cm apart along the 1.5 s roll-out; a turn on the spot 90° round (the chase's last turn: as far as it turns), each way on its own. "No part nearer": the sum over the outline of how far each point is inside its margin never grows. Checked on the map as given and on its marks alone: unseen floor grown round a seen mark reads 0 everywhere when the tank stands in it, and every turn passed (found writing the tests). The look turns left, or right when only that way is clear, or pans the camera. Tests: `test_beside_the_bed_it_turns_away_not_into_it`, `test_driving_on_past_the_bed_leg_is_refused`, `test_the_rear_is_checked_when_turning`, `test_standing_in_grown_unseen_space_still_checks_the_marks`, `test_the_look_turns_the_way_that_is_free`; all simulated rooms pass.
- **Correction (replaying run 2's spot through it):** turning left there is **refused too, rightly**. Turning left on the spot swings the tank's rear to the right: the rear-right corner sweeps out to 14.6 cm from the centre, and the bed panel was ~15 cm off (the map's cells are 5 cm). My first reading ("turning left moves every corner away") was wrong. At that exact spot only backing out was safe; the next step is the owner's (keep routes where the tank can turn round, and/or a slow reverse along its own track).

## 165. Routes keep room to turn round; a short slow reverse out of a pocket (owner: C and B, 2026-10-29)

After §164: at floor run 2's spot every move but backing out was rightly refused. The owner chose both:

- **C - routes only where the tank can turn round.** A route keeps the tank's centre at least the body's turning circle + 2 cm from anything: hypot(13, 11.5) + 2 = **19.4 cm** (was `ROBOT_R_M` 15 cm), worked out from `Limits` so a bigger body in `drive.yaml` widens it too. Explore's frontier uses the same distance (a frontier must be reachable). Near the start the escape rule stays (cells no nearer than the start may be used), so a tank put down in a tight spot can still route out. Cost: gaps under ~39 cm get no route; a 46 cm doorway leaves 23 cm each side.
- **B - a short slow reverse, only when boxed in.** When every forward curve has been blocked for 3 s (`BLOCKED_GIVE_UP_S`) and turning on the spot is refused both ways, the tank backs straight up at the slowest smooth speed (18 cm/s), at most **20 cm** or 2.5 s, then looks again. Allowed only where it just drove: every pose of the 20 cm lies within 5 cm of the tank's own recent track (the last ~1 m driven forward), and the body's outline along it passes the §164 check. Nothing sees behind the tank, so: never faster, never further, never off its own track, stop at once if the check fails part way; two reverses starting within 50 cm of each other end the run `blocked`. The camera stays ahead (it can't see behind either). STOP, deadman, link loss and the Uno's rules are unchanged; the Uno's brake only looks forward.
- The laptop side gains reverse: the speed ramp handles a backward command the way it handles forward ones (the dead-band crossed in one step), and `drive_rate` gets a negative `v` (the bridge takes −100…100 already, bridge LLD R4).

### 165.1 C made soft (owner: C-soft, 2026-10-29)
- **The hard limit closed doorways:** with 19.4 cm required, simulated explore no longer went through 46 or even 60 cm doorways (77 % seen). The 5 cm cells round a doorway's edges outward by up to a cell, and the 10 cm route grid takes the worst of each 2 × 2: ~5-7 cm lost each side, so a doorway needs ~55-65 cm. My "46 cm still passes" was wrong.
- **As built (owner):** routes may still come to 15 cm (`ROBOT_R_M`) from things, but cells nearer than the turning room (`turn_room_m(lim)` = 19.4 cm) cost **4×** (`TIGHT_W`): a route keeps room to turn round wherever there is another way, and still takes a narrow doorway when there isn't. Explore's frontier stays at 15 cm. All 16 simulated rooms pass, doorways included. `test_routes_keep_room_to_turn_round_where_they_can`.
- **What really made run 2's pocket:** replayed at 8.5 s, the route was fine (25 cm from the bed); the way on was 132° to the left, the old full-circle check refused turning on the spot, and the fan drove forward into the pocket instead. With §164's check the planner now turns left on the spot there (`choose` → spin left; right refused). So A prevents that pocket; C keeps routes out of narrow strips; B backs out when it happens anyway.
- **B as built:** `RoamSkill` keeps its last ~1 m driven forward (`track`); a reverse starts after 3 s with every curve blocked, or after a full look round with no way on, only if neither turn on the spot is allowed. Straight back at v_min (the ramp: from standing to −18 cm/s in one step, never a turn, stops at once), at most 20 cm or 2.5 s, re-checked every tick; then a look. The chase doesn't back up (its 3 s blocked rule stays). Tests: `test_boxed_in_it_backs_out_slowly_along_its_own_track`, `test_no_track_behind_no_reverse`, `test_backing_out_twice_in_the_same_place_ends_it`, `test_backing_up_is_the_slowest_speed_straight_and_stops_at_once`, `test_backing_up_sends_a_negative_speed`.

### 165.2 Floor run 3, `cq-drive explore` again: stuck in its own blind zone (2026-10-29) - open
- `blocked` after ~25 s, 0.2 m driven, 8.5 m² seen; it never backed up. The tracks moved (encoders) for the first 5 s; then the skill sent only STOP for 12 s.
- **Cause:** the camera never sees the floor within ~25 cm of the tank, and on the fan's map unseen floor within 30 cm (`GROW_UNSEEN_M`) of a seen mark counts as the mark - here a 2-cell mark 28 cm ahead-left grew over the floor round the tank. Every turn took the outline's front edge into that grown space (left after 40°, right after 5°), so §164's check refused both; forward too. On the marks alone, both turns pass.
- **Tried, not kept:** (1) the grown map checked more loosely (only the nearest point never nearer): brought back §164's hole - the simulated tank beside an unseen wall turned its corner to 4.6 mm (`test_no_turn_on_the_spot_scrapes_a_wall`). (2) Floor the body has covered counted as known free (no growing into it): right in itself, but not enough here - the front edge still meets grown space just ahead. Smaller grow distances (20, 15 cm) didn't free it either.
- **B didn't fire** because it backs straight along the current heading over the tank's own track; the tank had turned ~70° since driving in, so straight back wasn't on its track. As built, B only helps when the tank hasn't turned since it came in.
- Next: the owner's decision (see the conversation of 2026-10-29).

## 166. No more per-case rules: a cost map for driving on the map; LeRobot later (owner, 2026-10-29)
- **The owner:** "we can't make it rule based - read our guideline doc" (spec 0 §1.1 principle 3, §153). Floor runs 1-3 had each been answered with a new rule (grown unseen space, the escape exception, the "no deeper in" sums, the 4× weight, the reverse's track rules), and each broke another case. Today's uncommitted experiments (far-side growth, looks at the start and when boxed in, footprint clearing) are dropped.
- **Agreed: a cost map, the standard way (ROS 2 Nav2's costmap + local planner + recovery):** one cost per cell from the map's evidence - known obstacle forbidden, near one costly by distance, never seen costly but not forbidden; one body check, the outline swept along each move, forbidden only by known obstacles; routes and the fan use the same costs; speed falls where the way ahead is unknown or tight; when nothing is possible, one recovery sequence (look round, back up a little, `blocked`). The sonar's stop and the Uno's brake, deadman, link loss and STOP stay. Accepted trade-off: a thing never seen can't forbid a move, so in a spot it never looked at the tank may touch it slowly, never head-on. Next: AI engine LLD §18.6-§18.7 rewritten with diagrams, then a plan; no code before.
- **Hugging Face LeRobot (discussed):** an open library for robots that learn from recorded examples (record driving by hand, train, the model drives); SmolVLA is its small vision-language-action model (450 M parameters). Mostly arms; no good small-mobile-robot navigation examples found. **Agreed: later, as a "teach by showing" layer on top of the cost map and the safety rules** (e.g. nudge the ball, follow me), plus a converter from `cq-record` recordings to its dataset format; its SO-101 arm fits principle 6.
- **Training on the owner's laptop (Apple M1, 7-core GPU, 16 GB):** LeRobot's own reference times are on an NVIDIA A100 - ACT 100k steps ~1.5 h, SmolVLA 20k steps ~5 h. The M1's GPU is roughly 30-60× slower than an A100 for this (rough: ~2.6 vs ~150+ TFLOPS, and PyTorch's Mac backend less efficient), so: a small ACT policy on fewer steps - hours to about a day; SmolVLA fine-tuning - weeks, not practical; running a trained model on the M1 - fine (a community MLX port reports ~0.1-0.2 s per 50-action chunk on an M5 Pro; the M1 slower). Practical path: train in the cloud (Colab / HF Jobs), run on the laptop. Estimates to be replaced by a timing run of a few hundred steps when we get there.
- **Cloud training cost (searched 2026-10-29, list prices from secondary sources - check before paying):** Hugging Face Jobs: T4 $0.40/h, L4 $0.80/h, A100 ~$2.50/h (A100 row seen only on mirrors). Google Colab: Pro $9.99/month for 100 compute units ($0.10 each); a T4 burns ~1.76 units/h (~$0.18/h), an A100 ~5.4-15 units/h (~$0.54-1.50/h), A100s not guaranteed; the free tier has T4s with session limits. Per run (LeRobot's A100 times × these prices; a T4 taken as ~6-8× slower than an A100): ACT ~$1-4, SmolVLA fine-tune ~$3-13; a first experiment fits in one $9.99 Colab month.

### 166.1 The cost-map design approved; a new plan (owner, 2026-10-30)
- AI engine LLD §18.13 approved ("1 ok"): costs per cell (lethal / near / unknown / free), the body as a floor sensor, one per-point keep-out of 7.5 cm, speed from cost, routes on the same costs, recovery look → turn → back over known-free floor → `blocked`. The simulator's bar stays: the body never nearer than 5 mm to any box.
- Checked first on the floor-run recordings with a scratch script (LLD §18.13.10): run 1's path at a median 25 cm/s; run 3's spot free to turn; run 2's spot refused everywhere (it wouldn't have been entered).
- **Plan Z6 - the cost map** ("2 yes a new plan"): branch `plan-z6` from `plan-z5`; Z5's floor checks folded into Z6's; both merge to `main` together after Z6's floor checks.
