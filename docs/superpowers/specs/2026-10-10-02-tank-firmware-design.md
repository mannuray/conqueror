# Conqueror Platform — Tank Firmware (Design Spec 2)

Status: DRAFT for review · Date: 2026-10-10 · Depends on: Spec 0, Spec 1

Two firmwares, both built with PlatformIO:
- `firmware/uno/` — Arduino Uno **body**: control loops, safety reflexes, sensors, outputs.
- `firmware/esp32/` — ESP32-WROVER **gateway**: WiFi, camera, client API, arbitration, navigation.

---

## Part A — Arduino Uno body

### A1. Pin, interrupt and timer allocation

| Pin | Function | Mechanism |
|---|---|---|
| D0/D1 | UART to ESP32 (and USB for upload, ESP32 disconnected) | hardware UART, 115200 |
| D2 | mode button (WiFi-reset hold at boot; otherwise unused) | polled, debounced |
| D3 | **left encoder A** | INT1, rising edge |
| D4 | WS2812 RGB LED (×1) | tiny bit-bang driver, `show()` only on change (~30 µs IRQ-off) |
| D5 / D6 | motor A (right) / B (left) PWM | Timer0 hardware PWM |
| D7 / D8 | motor B / A direction | GPIO |
| D9 | IR receiver | PCINT0 group (shared ISR with D12) |
| D10 / D11 | pan / tilt servos | Servo library on **Timer1** |
| D12 | ultrasonic echo | PCINT0 group |
| D13 | ultrasonic trigger | GPIO, 10 µs pulse |
| A0 | **passive buzzer** (assumption A2) | `tone()` on **Timer2** |
| A1 | **right encoder A** (assumption A4) | PCINT1 group (only source in that group) |
| A2 | spare (future right encoder B) | — |
| A3 | battery divider | ADC (scale from original: ADC × 0.0375 × 1.08, recalibrated) |
| A4 / A5 | I2C (MPU6050) | Wire, 400 kHz |

- **Timer0:** default prescaler (976 Hz PWM) so `millis()`/`micros()` stay correct. Build option `MOTOR_PWM_244HZ` restores the original 244 Hz with a scaled time base.
- **Timer2** is owned by `tone()`; therefore **IRremote is not used** — a small custom NEC decoder timestamps edges in the PCINT0 ISR (`micros()`), decodes 32-bit NEC frames + repeat codes. Remote "A" codes from the original firmware (e.g. `0x00FF629D` = ▲).
- `ISR(PCINT0_vect)` reads PINB once, diffs against the previous value, and dispatches to the IR decoder (D9) and the echo timer (D12).
- Libraries used: Arduino core, Wire, Servo. **Not used:** ArduinoJson, IRremote, FastLED, I2Cdev/MPU6050 libs, PinChangeInt, MsTimer2 (replaced by small drivers).

### A2. Scheduler (cooperative, no `delay()` anywhere)

| Task | Rate | Job |
|---|---|---|
| Link RX/TX | every loop | parse frames, send telemetry/acks |
| IMU | 200 Hz | MPU6050 raw read (14 bytes), gyro bias removal, yaw integration, complementary-filter pitch/roll, accel magnitude & variance, bump detection |
| Motor control | 100 Hz | speed controller, heading controller, primitives, ramps, deadband, battery compensation |
| Safety supervisor | 50 Hz | final gate (A6) |
| Servos | 50 Hz | trapezoidal profiles, `writeMicroseconds` |
| Ultrasonic | 20 Hz | trigger; echo measured by ISR; 3-sample median; timeout = no echo |
| LED / buzzer | 50 Hz | priority-layer sequencers |
| Battery | 10 Hz | EMA filter, low/critical detection under load |
| Telemetry | 20 Hz fast / 1 Hz slow | TELEM_FAST / TELEM_SLOW |

Watchdog: 1 s hardware WDT; reset reason captured from MCUSR at boot and reported in HELLO/TELEM_SLOW.

### A3. Modules

```
drivers/   motors, encoders, imu, ultrasonic, servos, buzzer, led, ir_nec, battery, button
control/   speed_controller (OpenLoop | EncoderPid), heading_controller, primitives (turn, move), ramps
arbitration  IR vs network source, IR handback timer
safety/    supervisor (final gate), envelope
comms/     frame codec, dispatcher, telemetry builder
settings/  EEPROM store with CRC; defaults on corruption; versioned layout
ui/        sound catalogue, LED patterns (priority layers)
```
Data flow: `source (link | IR) → arbitration → primitive/drive → heading + speed controllers → safety supervisor → motor driver`.
Control logic (controllers, safety rules, arbitration, codec) is hardware-independent C++ so it can be unit-tested on a PC when wanted.

### A4. Motor control stack

1. **Command:** `v` % and `w` % (or `yaw_rate`) from the owner; primitives produce the same.
2. **Heading controller (gyro PID):** `w`=0 → heading hold on the yaw at the moment straight driving began; `w`≠0 → yaw-rate PID tracking `w × turn_rate_max`; primitives `turn by/to` with a deceleration profile into the target (±1°).
3. **Mixing:** left/right track targets from `v` and the heading-controller output.
4. **Speed controller** (one interface, two implementations):
   - `OpenLoop`: target % → PWM via deadband map.
   - `EncoderPid`: target % → counts/s (max from calibration); per-track PID at 100 Hz; speed estimate = counts per 20 ms window at high speed, pulse-period timing at low speed (auto-switch); direction inferred from commanded direction; counts trusted only while driven.
   - Encoder sanity: enabled but no pulses while |PWM| > deadband for 0.5 s → `encoder_fault` event, fall back to `OpenLoop`.
5. **Deadband compensation:** non-zero output mapped onto `[deadband_keep .. 255]`; from standstill a **kick** to `deadband_start` for 60 ms. Values per motor and direction (calibrated).
6. **Ramps:** slew-limit to `accel_limit` (and envelope `accel_cap`).
7. **Battery compensation + voltage cap:** `pwm × (V_nominal / V_batt)`, clamped so the effective motor voltage ≤ `motor_v_max` (6 V for FIT0458 → ~71 % duty at 8.4 V).
8. **Envelope:** final clamp of |speed| to `speed_cap`, accel to `accel_cap`.

**Turning (skid steering; all on the Uno):**
- Tanks turn by skidding, so commanded track speeds give surface-dependent curves. Turning is therefore **closed-loop on yaw rate (gyro)**, never open-loop track differences.
- Per control cycle: (1) **feedforward** track difference = ω × `track_width_eff` / 2 (learned by the turn-check calibration);
  (2) **gyro yaw-rate PID** corrects measured vs target ω (absorbs slip, surface, battery); (3) tracks `L = v − Δ`, `R = v + Δ` → encoder speed PID or open-loop compensation.
- Turn types: **spin in place** (L = −R) · **arc** (moving, both tracks forward at different speeds) · **arc to angle with radius** (ω = v / r, ends at the gyro heading) · **pivot on one track** (optional, inner track 0).
- **Deadband → pivot fallback:** if the inner track target is below its keep-moving threshold, set it to 0 (clean pivot) instead of stuttering (`pivot_fallback`, default on).
- **Turn-rate limit vs speed:** max ω reduced as |v| rises (`turn_rate_vs_speed` curve) to avoid wild skids.
- **Anti-windup:** PID output clamped; unreachable ω reported via flag `turn_limited`.
- **Exit:** when ω returns to 0, heading hold captures the **current** heading (clean straight-out, no overshoot).
- **Reverse steering = car-style** (DECIDED, setting `reverse_steer`, default `car`): stick back + right swings the tail right (ω sign inverted when v < 0). Alternative `tank` = stick right always rotates clockwise.
- Small safety deadzone on the Uno; stick deadzone/expo curves are applied by clients (app/hub).

**Primitives:** `TURN` (by/to angle, optional radius / pivot), `MOVE` (distance, needs encoders; heading hold at the given/current heading). One active at a time; new motion commands or `CANCEL`/`STOP` abort it; completion → `primitive_done`.

### A5. Pan/tilt servos
- Trapezoidal motion (max `servo_speed`, `servo_accel`), 50 Hz updates in µs; soft limits + trim (EEPROM).
- `CAM_JOY`: absolute stick→angle mapping inside the limits; on release → after `cam_return_delay_ms` glide home unless `CAM_LOCK`.
- `CAM_SET` holds; `CAM_NUDGE` relative; `CAM_HOME`.
- Powered while moving/holding; optional idle detach at home (`servo_idle_detach`, only if the tilt holds unpowered).

### A6. Safety supervisor (final gate, 50 Hz)

| Check | Trigger (setting) | Action |
|---|---|---|
| Deadman | no drive update `deadman_ms` (300) during `drive`/`drive_rate`; or link lost 300 ms | smooth stop, `deadman_stop` |
| Obstacle brake | `dist < obstacle_stop_cm` (15); proportional forward cap from `obstacle_slow_cm` (50) | forward motion limited/stopped; reverse/turn allowed; `blocked` |
| Tilt | pitch/roll > `tilt_limit_deg` (35) | stop + alarm; locked until level 1 s |
| Lift | **any of:** pickup transient (vertical accel spike while not driving over a bump), sudden tilt change, or (encoders) track free-spin — track speed at a given PWM well above the calibrated on-ground speed | stop + alarm; `lifted`; `landed` after level + steady 1 g for 1 s. Without encoders: transient/tilt only (best-effort) |
| Bump | accel spike > `bump_g` (1.5 g) | 300 ms stop, `bump{g}` |
| Stall | encoders: driven but no motion 0.5 s | cut that motor, `stall{side}` |
| Battery | < `batt_low_v` (warning) / < `batt_crit_v` (motors disabled) under load, 2 s sustained | events + alarms |
| Envelope | `speed_cap`, `accel_cap`; `lock_brake` forbids disabling the obstacle brake | clamp |
| Self-test critical failure | IMU dead or battery read implausible | motors locked |

- The obstacle brake can be disabled by `human`/`ai` (`BRAKE off`) unless the envelope locks it; it is always **on after reboot**.
- Ultrasonic limits are documented: narrow forward cone; misses thin/low/angled objects → best-effort.

### A7. Arbitration on the Uno
- Network commands arrive tagged `src` (human/ai) — the ESP32 already arbitrated between network clients.
- **IR beats network.** First IR motion key → owner = IR (`ir_override`, `control_changed`), stop, then IR control. Handback after `ir_handback_ms` (3 s) or IR **OK** → owner none, stop; the ESP32 is informed via TELEM_SLOW owner + event.
- `STOP` from any source always applies.

### A8. IR remote mapping (backup controller)
| Key | Action |
|---|---|
| ▲ ▼ | forward / backward while held (stop ~200 ms after last repeat) |
| ◀ ▶ | spin left / right while held |
| OK | STOP + hand control back |
| 1 / 3 | pan left / right while held · 2 / 8 tilt up / down while held · 5 camera home |
| 4 / 6 | speed −/+ 10 % (beep pitch confirms) |
| 7 | mute toggle · 9 battery check (LED + 3/2/1 beeps) · 0 gyro turn 180° |
| * | parking beeps on/off · # (hold 3 s) motor calibration |
IR driving uses heading hold and is subject to the safety supervisor.

### A9. Sound and LED catalogues (priority layers; higher interrupts lower, lower resumes)
Sounds (passive buzzer, non-blocking sequencer):
| Pri | Event | Sound | Repeat |
|---|---|---|---|
| 1 | tilt danger | fast two-tone siren | while tilted |
| 1 | lifted | rising whoop | once + every 3 s |
| 1 | battery critical | three long low tones | every 10 s |
| 2 | battery low | double beep | every 30 s |
| 2 | bump | short falling "ouch" | once |
| 2 | stall | low buzz | once |
| 2 | link lost (deadman) | falling three-note | once |
| 3 | reversing | beep… beep… | while reversing |
| 3 | start forward | short chirp | once per start |
| 3 | obstacle closing (`parking_beeps`) | rate ∝ 1/distance | while closing |
| 4 | startup / ready / self-test fail | boot melody / two rising chirps / low error tone | once |
| 4 | client connected/disconnected · owner changed · calibration start/done · "thinking" ack | up/down chirp · two-note blip · tick/jingle · soft blip | once |
| 4 | custom (`SOUND`, `TONE`) | from client | on request |
Audio modes: **mute** (only priority 1), **quiet** (priorities 1–2), normal.

LED (single RGB):
| Pri | State | Pattern |
|---|---|---|
| 1 | tilt / lifted | fast red/blue alternate |
| 1 | battery critical | red slow pulse |
| 2 | battery low | amber blink every 2 s over base colour |
| 2 | bump / stall | white flash · link lost: orange blink |
| 3 | reversing | white/red blink · turning: amber blink |
| 4 | startup | rainbow sweep · idle no client: blue breathing |
| 4 | owner human / AI / IR | solid green / cyan / purple |
| 4 | calibrating | yellow pulse · custom `LED` command |

### A10. Calibration (admin; results in EEPROM)
1. **Gyro bias:** 2 s still (also automatic when stationary and idle ≥ 5 s).
2. **Motor deadband:** per track and direction, ramp PWM until the gyro detects rotation (start threshold), then lower until it stops (keep threshold). ~20 s, needs floor space.
3. **Encoder distance:** drive N counts straight; the user enters the measured cm → `counts_per_cm`.
4. **Turn check:** spin 360° and drive one arc by gyro; compare with encoder-implied rotation → slip factor and **`track_width_eff`** (feedforward).
5. **Servo trim:** nudge to centre, save.
6. **Encoder resolution check:** live count display while the user turns a shaft one revolution (confirms assumption A3).

### A11. Self-test at boot (~2 s; reported in `selftest` + beep/LED)
IMU WHO_AM_I + not saturated · battery plausible · ultrasonic echo · encoders (if enabled) clean idle · settings CRC · reset reason.
Critical (IMU, battery read) → motors locked. Non-critical (ultrasonic) → warning, obstacle brake reported unavailable.

### A12. Memory budget (risk)
32 KB flash / 2 KB RAM. Mitigations: no heavy libraries; binary protocol; F() strings; fixed-size buffers (RX frame 52 B).
**First implementation step builds a skeleton with all drivers and measures flash/RAM.** Derived metrics already live on the ESP32 (Spec 1 §6.2 ownership).

---

## Part B — ESP32 gateway

### B1. Tasks (FreeRTOS, Arduino-ESP32 core, PlatformIO; camera code adapted from the ELEGOO example)

| Task | Priority | Job |
|---|---|---|
| Body link | highest | Serial2 (IO33 RX, IO4 TX) 115200; frames; 100 ms heartbeat; latency; Uno HELLO/version check |
| Control (WS) | high | `/ws` sessions, key auth, JSON ⇄ frames, network arbitration, ack/nack |
| Navigation | high | pose integration and missions (B4) |
| Camera | medium | MJPEG :81 (≤2 viewers), `/api/capture` (stream or full res), per-sensor settings |
| WiFi manager | low | saved networks, AP fallback, captive setup, mDNS |
| REST + OTA | low | `/api/*`, OTA upload |

### B2. Network arbitration
- Among WS clients: owner = highest-priority role that sent a motion command (human > ai). Handback human → AI after `human_handback_ms` (5 s) or `release_control`.
- **Same role:** the current owner keeps control until its handback timer expires or it releases; other same-role clients get `nack not_owner`. STOP from anyone.
- Forwards **one** command stream to the Uno tagged with `src`; others get `nack not_owner`.
- On owner change: send STOP, emit `control_changed`; to the AI client emit `control_returned` when it regains control.
- IR ownership reported by the Uno overrides everything; network motion commands get `nack not_owner` meanwhile.

### B3. Camera
- Sensor PID read at boot (OV2640/OV3660/OV5640) → per-sensor defaults (flip, quality, framesize). Stock: **OV3660**.
- Default stream 640×480 (VGA) JPEG; configurable.
- **Frame timestamps:** each MJPEG part carries `X-Timestamp: <tank_ms>` derived from the frame-buffer capture time (same clock as telemetry). `/api/capture?res=full` switches temporarily to full resolution, returns one frame, restores the stream size.
- Max 2 concurrent MJPEG viewers; more → HTTP 503 with a hint to use the hub.

### B4. Navigation (requires encoders)
- **Pose integration** at 20 Hz from TELEM_FAST: `Δd = mean(ΔencL, ΔencR) / counts_per_cm`; `θ` from Uno yaw (gyro is the heading truth; skid-steer slip makes encoder heading unreliable); `x += Δd·cosθ`, `y += Δd·sinθ`.
- **go-to(x,y):** bearing + distance → `TURN to bearing` → `MOVE dist at heading`; re-plan after each MOVE if residual > 5 cm (max 3 replans).
- **Smooth paths:** between waypoints, heading changes ≤ 90° use **arc turns** (`TURN` with radius, default 30 cm, limited by remaining distance); > 90° use spin in place.
- **Waypoints/missions:** sequential go-to; `loop` option; saved missions in NVS (≤ 8 × 32 points).
- **Return home:** go-to(0,0) then turn to θ=0.
- Blocked (obstacle brake) > 5 s → `nav_done{ok:false,reason:"blocked"}`. Link loss: the Uno finishes/aborts the current primitive and stops; navigation pauses.
- Without encoders: `nav_*` and `move` → `nack unavailable`.

### B5. Derived telemetry (owned by the ESP32)
`batt_pct` (2S discharge curve), `runtime_min` (voltage trend), `sag_v` (voltage drop vs driven/idle), `ttc_s`, `roughness` (scaled accel variance), `slip_pct`, `pose`, `lifetime` stats (NVS every 5 min), `esp` health (RSSI, fps, clients, temp, heap, link latency).

### B6. WiFi and identity
- Boot: try ≤3 saved networks ~15 s → station; else AP `conqueror-XXXX` (WPA2 after setup; open only before first setup) with captive portal at 192.168.4.1.
- mDNS `conqueror-XXXX.local` (XXXX from MAC; renameable), service `_conqueror._tcp`.
- **WiFi reset:** Uno D2 button held 5 s at boot → `WIFI_RESET_REQ` → clear saved networks (keys and admin password kept).

### B7. Storage (NVS)
WiFi networks, tank name, admin password hash, key hashes + roles + names, camera config, missions, lifetime stats, human handback timing,
**active envelope** (persisted so a brownout/power cycle cannot silently drop child-play limits; restored to the Uno on every HELLO; only `human`/`admin` can clear it).

### B8. Updates
- ESP32: OTA upload (admin) from the captive/setup page or `POST /api/ota`.
- Uno: USB only (disconnect ESP32 serial). The ESP32 checks Uno `HELLO` versions at boot → `body_incompatible` if the proto major differs.

### B9. Failure behaviour
| Failure | Behaviour |
|---|---|
| Uno link lost | camera continues; `body_offline`; motion commands `nack body_offline` |
| WiFi lost | Uno deadman stops (no frames/drive); ESP32 reconnects or falls back to AP |
| ESP32 crash | task watchdog restart; Uno already stopped by link loss; IR still works |
| Uno reset | HELLO re-sent; ESP32 re-sends the **envelope** from NVS. The obstacle brake is **not** re-sent: it stays **on** after any reset |

---

## Part C — Real-tank safety checklist (run before trusting new firmware, especially for child play)
1. Boot self-test passes; startup sound/LED.
2. App drive → lift thumb → stops within ~0.3 s (deadman).
3. Kill WiFi/ESP32 power while driving → stops.
4. Drive at a wall → slows from 50 cm, stops by ~15 cm; reverse allowed.
5. Lift the tank **while driving** → motors stop, alarm. Tilt > 35° → stop, alarm.
   Power-cycle with child-play active → envelope still active after boot.
6. Tap the front → bump event + brief stop.
7. Hold a track (encoders) → stall cut.
8. Child-play envelope: speed ≤ 20 %; AI cannot loosen it or disable the brake.
9. IR overrides app; OK hands back; app overrides AI.
10. Low-battery warning visible (bench supply or drained pack).
