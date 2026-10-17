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
