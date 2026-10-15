# Conqueror Platform — Protocol & Interface Contract (Design Spec 1)

Status: REVIEWED — body link (§8) and hub pairing (§3) APPROVED 2026-10-10 · Date: 2026-10-10 · Depends on: Spec 0

Two layers:
- **Client API** (§2–§7): JSON over WebSocket + HTTP between clients (hub, app, AI engine via hub) and the **ESP32**.
- **Body link** (§8): compact binary frames over UART between the **ESP32** and the **Uno**. *(APPROVED: **250000 baud** — 0 % error on the 16 MHz Uno; revised 2026-10-10 after LLD review.)*

All message definitions live once in `protocol/` (schema source) and are generated into C (Uno), C++ (ESP32), TypeScript (hub) and Python (AI engine).
Protocol version: `proto = 1`. The ESP32 refuses Uno firmware with a different body-link major version and reports `body_incompatible`.

---

## 1. Conventions

- **Time:** `tank_ms` = ESP32 monotonic milliseconds (u32-safe, wraps after ~49 days; clients handle wrap). Every telemetry message, event and video frame carries `tank_ms`.
- **Angles:** degrees, float in JSON (0.1° resolution on the wire). Heading: 0–360, counter-clockwise positive, 0 = heading at boot / origin.
- **Speed:** percent of configured max, −100..100. **Turn input:** percent −100..100 (mapped to deg/s by setting `turn_rate_max`, default 180°/s) or explicit deg/s where stated.
- **Distance:** cm in JSON (mm on the wire). **Position:** cm, origin set by `nav_set_origin` (default: boot position).
- **Sequence:** tank messages carry `seq` (per-connection, increments); clients may detect gaps.
- **IDs:** client commands may carry `id` (int); the tank replies `ack`/`nack` with the same `id` for non-streaming commands.

## 2. Endpoints (per tank, host `conqueror-XXXX.local`)

| Endpoint | Purpose | Auth |
|---|---|---|
| `ws://<tank>/ws` | control, telemetry, events | key in `hello` |
| `http://<tank>:81/stream?key=K` | MJPEG video; each part has header `X-Timestamp: <tank_ms>` | viewer+ |
| `GET /api/capture?res=stream|full&key=K` | JPEG snapshot; `full` = sensor max (OV3660 2048×1536) | viewer+ |
| `GET /api/info` | id, name, fw versions, proto, sensor PID, capabilities | viewer+ |
| `GET/PUT /api/settings` | tank settings (§7) | GET human+, PUT admin |
| `GET/POST/DELETE /api/keys` | key management | admin |
| `GET/POST /api/wifi` | saved networks, scan, force AP | admin (open only during first-boot setup) |
| `POST /api/ota` | ESP32 firmware upload | admin |
| `GET/PUT/DELETE /api/missions` | saved waypoint missions | human+ |
| captive portal `http://192.168.4.1/` | first-boot setup, WiFi, OTA | setup / admin |

REST auth: `Authorization: Bearer <key>` header; `?key=` accepted **only** on `GET :81/stream` and `GET /api/capture` (never logged). Host/Origin checked (DNS-rebinding defence); no CORS except allowlist.
mDNS: service `_conqueror._tcp`, TXT records `id`, `name`, `proto`, `fw`.

## 3. Keys, roles, pairing

| Role | Can |
|---|---|
| `viewer` | video, telemetry, events, **stop** |
| `ai` | viewer + drive/camera/LED/sound/nav at **AI priority**; may only **tighten** the envelope |
| `human` | viewer + drive/camera/LED/sound/nav at **human priority**; set/clear envelope; toggle obstacle brake (unless envelope locks it); missions |
| `admin` | human + settings, calibration, keys, WiFi, OTA, camera config |

- Keys are random 128-bit tokens (base32). The ESP32 stores only salted hashes.
- **First boot:** setup page creates the **admin password** and the tank-AP WPA2 password; setup requires **pressing the tank's mode button** (physical confirmation); the open setup AP closes after 30 min. The admin password is used only for key minting (`POST /api/keys`) and the setup page; all other admin calls use admin keys.
- **Recovery:** hold the mode button 15 s at boot → admin password + keys cleared (WiFi kept).
- **Hub pairing:** hub discovers a tank → the user enters the tank admin password **once** in the hub → the hub calls `POST /api/keys` to mint `hub-admin` (admin), `hub-human` (human) and `hub-ai` (ai) keys, stores them server-side, and discards the password. Revoking these keys in the tank unpairs the hub.
- A WebSocket connection has exactly **one role** (its `hello` key). The hub therefore opens one connection per role it uses: `hub-human` (browser control), `hub-ai` (AI engine), and `hub-admin` on demand (settings/calibration).
- **App:** user enters/creates a `human` key (QR code on the hub/setup page for convenience).

## 4. WebSocket session

```
client → {"t":"hello","key":"…","client":"hub-ai","proto":1}
tank   → {"t":"welcome","role":"ai","tank":{"id":"a1b2","name":"conqueror-a1b2","fw":{"esp32":"1.0.0","uno":"1.0.0"},
          "proto":1,"caps":{"encoders":true,"nav":true,"distance":true,"sensor":"OV3660"}},"owner":"none"}
   or  → {"t":"error","code":"auth"}  (then close)
client → {"t":"subscribe","streams":["fast","slow","events"],"fast_hz":20}
```
- Heartbeat: client sends any message or `{"t":"ping","t0":<client_ms>}` at least every 1 s → `{"t":"pong","t0":…,"tank_ms":…}` (used for clock-offset estimation).
- Max WS clients: 6 (e.g. app + hub ×3 + spare). Max MJPEG viewers: 2.
- Ack: `{"t":"ack","id":7}` / `{"t":"nack","id":7,"code":"COMMAND_3001","reason":"NOT_OWNER","message":"…","details":{}}`.
  `code` (numeric, stable) + `reason` (symbolic) come from the shared error registry `protocol/errors` (ranges per domain; Spec 3 §8.2).
  Tank reasons include: `AUTH_FAILED`, `ROLE_FORBIDDEN`, `NOT_OWNER`, `BLOCKED` (safety), `UNAVAILABLE` (capability missing), `ENVELOPE_LOCKED`, `BUSY`, `INVALID_COMMAND`, `BODY_OFFLINE`.

## 5. Commands (client → tank)

Streaming commands (no ack; rejections reported once via `nack` without `id` + an event; nack codes per §4): `drive`, `drive_rate`, `cam_joy`.

| `t` | Fields | Min role | Notes |
|---|---|---|---|
| `stop` | — | **viewer** | immediate smooth stop; cancels primitives and nav |
| `drive` | `v` %, `w` % | ai | joystick/tilt input, stream ≤20 Hz; deadman 300 ms; heading hold when `w`=0 |
| `drive_rate` | `v` %, `yaw_rate` °/s | ai | closed-loop turn rate |
| `turn` | `by` ° **or** `to` ° (heading), `speed` %, `radius_cm`? (0/omitted = spin in place), `pivot`? bool | ai | gyro primitive; arc when radius > 0 (needs v > 0); ack on start, event `primitive_done` |
| `move` | `dist_cm`, `speed` %, `heading`? ° | ai | needs encoders else `unavailable` |
| `cancel` | — | ai | cancels current primitive |
| `release_control` | — | human | immediate human → AI handback |
| `cam_joy` | `x`,`y` −100..100, `active` bool | ai | absolute joystick mapping; on `active:false` → return home after `cam_return_delay_ms` unless locked |
| `cam_lock` | `on` | ai | keep current view; skip return-home |
| `cam_set` | `pan` °, `tilt` °, `speed`? °/s | ai | absolute target, **holds** |
| `cam_nudge` | `dpan` °, `dtilt` ° | ai | relative |
| `cam_home` | — | ai | |
| `led` | `mode` solid/blink/pulse/off, `rgb` [r,g,b], `period_ms`?, `duration_ms`? | ai | custom layer (priority 4); alarms override |
| `sound` | `name` (catalogue spec 2 §6) | ai | |
| `tone` | `notes` [[freq_hz, ms], …] ≤10 | ai | custom tone sequence |
| `audio_mode` | `normal`/`quiet`/`mute` | human | priority-1 alarms always play |
| `obstacle_brake` | `on` | ai | disabling rejected (`ENVELOPE_LOCKED`) while envelope `lock_brake`; re-enabled at reboot |
| `safety_override` | `on` bool | human (`on`); anyone (`off`) | spec 02 §A6.1: obstacle brake + bump stop off, tilt cut-off 65°; `ROLE_FORBIDDEN` for `ai` keys, `ENVELOPE_LOCKED` while the **child-play** envelope is active (an adult envelope does not block it; its speed/accel caps keep applying), `BUSY` while a Tank Logo program runs, `BLOCKED` while a hard fault is latched; ends after 60 s idle / 5 min / STOP / owner change / link loss / reboot; the client must ask for confirmation |
| `envelope` | `preset` (`child_play`/`none`) or `speed_cap` %, `accel_cap` %/s, `lock_brake` bool | human (set/loosen/clear); ai may only tighten | enforced on the Uno |
| `nav_goto` | `x`,`y` cm, `speed` % | ai | needs encoders |
| `nav_home` | `speed` % | ai | return to origin |
| `nav_waypoints` | `points` [[x,y],…] ≤32, `loop` bool, `speed` % | ai | |
| `nav_mission` | `mission_id` | human | run a saved mission |
| `nav_cancel` | — | ai | |
| `nav_set_origin` | — | human | pose reset to (0,0,0) |
| `calibrate` | `kind`: `gyro`/`level`/`battery`/`deadband`/`encoder_cpr`/`encoder_distance`/`balance`/`track_width`/`turn_curve`/`pid_autotune`/`servo_trim`/`ultrasonic_offset`, `profile`? | admin | event `calibration` progress/result |
| `surface_profile` | `select` name \| `create` name (copy of active) \| `rename` \| `delete` | human (select), admin (others) | ≤3 profiles |
| `selftest` | — | human | re-run the boot self-test (body `CALIBRATE` script `SELF_TEST`); `nack BUSY` while moving/calibrating/in Override; result event `selftest`. The only way besides a reboot to lift a critical self-test lock (spec 02 §A11) |
| `calibrate_op` | `op`: accept / retry / skip / cancel | admin | controls the running calibration job |
| `calibrate_input` | `kind`, value (e.g. `measured_cm`) | admin | completes interactive calibrations |
| `camera_config` | `framesize`, `quality`, `vflip`, `hmirror`, `brightness`, … | admin | |
| `subscribe` | `streams`, `fast_hz` (1–20) | viewer | |
| `ping` | `t0` | viewer | |

Control ownership: motion/camera/LED/sound commands are accepted only from the current owner or a higher-priority source (which then becomes owner).
**Same-role rule:** among clients of the same role (e.g. the app and the hub's human connection), the current owner keeps control until its handback timer expires (or it sends `release_control`); the others get `nack NOT_OWNER`. `stop` is accepted from everyone.
`child_play` preset = `speed_cap 20`, `accel_cap 30`, `lock_brake true`. On the body link the preset is marked explicitly: `ENVELOPE.lock_brake` is a bit set (`EnvelopeFlagBit`: bit 0 `LOCK_BRAKE`, bit 1 `CHILD_PLAY`, so child play = `3`); a custom (adult) envelope never sets `CHILD_PLAY`. The same bits come back in `TELEM_SLOW.env_lock_brake` and `envelope_changed`. Only `CHILD_PLAY` blocks Safety Override (owner decision 2026-10-12).

## 6. Tank → client messages

### 6.1 Fast telemetry `{"t":"tf", "seq", "tank_ms", …}` (≤20 Hz)
| Field | Unit | Source |
|---|---|---|
| `yaw`, `yaw_rate`, `pitch`, `roll` | °, °/s | Uno |
| `spd_cmd` [L,R], `spd_act` [L,R] | % (`act` null without encoders) | Uno |
| `pwm` [L,R] | −255..255 actual output | Uno |
| `dist_cm` | ultrasonic, null if no echo; `dist_fault: true` added when the sensor faults (2026-10-15) | Uno |
| `ttc_s` | time-to-collision, null if not closing | ESP32 |
| `pan`, `tilt`, `cam_moving` | °, bool | Uno |
| `enc` [L,R] | counts | Uno |
| `pose` {x,y,θ}, `home_dist_cm` | cm, ° | ESP32 (null without encoders) |
| `primitive` | `none`/`turn`/`move`/`nav` + progress % | Uno/ESP32 |

### 6.2 Slow telemetry `{"t":"ts", …}` (1 Hz)
| Field | Source |
|---|---|
| `batt_v`, `batt_pct`, `runtime_min`, `sag_v` | Uno (volts), ESP32 (pct, runtime, sag) |
| `owner` (`ir`/`human`/`ai`/`none`), `owner_client` | Uno + ESP32 |
| `flags`: turn_limited, blocked, lifted, tilted, stalled, batt_low, batt_critical, link_lost, brake_off, muted, quiet, envelope, safety_override, selftest_locked, body_offline | Uno/ESP32 |
| `envelope` {speed_cap, accel_cap, lock_brake} | Uno |
| `surface_profile` (active name), `calibration` {item: missing/ok/stale} | Uno |
| `hw`: encoders, imu_ok, sensor | Uno/ESP32 |
| `temp_c` (MPU6050) | Uno |
| `roughness` (0–100, from accel variance) | Uno variance → ESP32 scale |
| `slip_pct` | ESP32 (encoders vs gyro) |
| `uno`: uptime_s, loop_load %, loop_max_us, free_ram, err_frames, err_i2c, reset_reason, fw, settings_crc | Uno |
| `esp`: rssi, fps, clients, temp_c, link_latency_ms, free_heap | ESP32 |
| `lifetime`: distance_m, runtime_h, power_ons | ESP32 (NVS, saved every 5 min) |

### 6.3 Events `{"t":"ev","seq","tank_ms","kind",…}`
`bump{g}`, `lifted`, `landed`, `tilted{pitch,roll}`, `level`, `stall{side}`, `blocked{dist_cm}`, `unblocked`,
`control_changed{owner,client}`, `control_returned` (to the AI), `deadman_stop`, `link_lost`, `link_restored`, `body_offline`, `body_online`,
`batt_low{v}`, `batt_critical{v}`, `primitive_done{kind,ok,reason}`, `nav_done{ok,reason}`, `calibration{kind,stage,progress,result}`,
`selftest{ok,failures[],critical}`, `nav_progress{waypoint,of,remaining_cm}`, `settings_reset`, `pose_degraded`, `waypoint_inaccurate`, `camera_reset`, `envelope_mismatch`, `surface_suggest{profile}`, `envelope_changed{…}`, `encoder_fault{side}`, `ir_override`, `safety_override{on, reason}` (reason: network/ir/idle/max_time/stop/owner_change/link_lost/envelope).

## 7. Settings catalogue (persisted; Uno EEPROM or ESP32 NVS)

| Key | Default | Where |
|---|---|---|
| `encoders_enabled` | false | Uno |
| `surface_profiles[≤3]` {name, deadband, balance, track_width_eff, turn_curve, pid gains, roughness signature} | defaults | Uno (signature: ESP32) |
| `surface_auto_switch` | false | ESP32 |
| `counts_per_rev` | 960 | Uno |
| `counts_per_cm` | (calibrated) | Uno |
| `motor_v_max` | 6.0 V | Uno |
| `deadband_start[L+,L−,R+,R−]`, `deadband_keep[…]` | (calibrated) | Uno |
| `accel_limit` | 200 %/s | Uno |
| `turn_rate_max` | 180 °/s | Uno |
| `turn_rate_vs_speed` | curve: 100 % of max at v=0 → 40 % at v=100 | Uno |
| `track_width_eff` | (calibrated; starts at physical width) | Uno |
| `pivot_fallback` | true | Uno |
| `reverse_steer` | `car` (`car`/`tank`) | Uno |
| client: `stick_deadzone`, `stick_expo` | 5 %, 0.3 | app/hub |
| `pid_speed{kp,ki,kd}`, `pid_heading{…}`, `pid_turn{…}` | tuned defaults | Uno |
| `obstacle_stop_cm` / `obstacle_slow_cm` | 15 / 50 | Uno |
| `tilt_limit_deg` | 45 (must persist 300 ms) | Uno |
| `bump_g` | 1.5 | Uno |
| `deadman_ms` | 300 | Uno |
| `batt_low_v` / `batt_crit_v` / `batt_hyst` | 7.2 / 6.6 / 0.2 V (2S Li-ion; recovery above threshold + hysteresis) | Uno |
| `override_idle_s` / `override_max_s` / `override_tilt_deg` | 60 s / 300 s / 65° (≤ 70°) | Uno |
| `led_max` | 50 (of 255) | Uno |
| `servo_limits{pan_min,pan_max,tilt_min,tilt_max}` | 0/180/30/110 | Uno |
| `servo_trim{pan,tilt}`, `servo_speed`, `servo_accel` | 0,0 / 120 °/s / 400 °/s² | Uno |
| `cam_return_delay_ms` | 500 | Uno |
| `servo_idle_detach` | false | Uno |
| `parking_beeps` | off | Uno |
| `ir_handback_ms` / `human_handback_ms` | 3000 / 5000 | Uno / ESP32 |
| `tank_name`, WiFi networks, keys, camera config, missions | — | ESP32 |

## 8. Body link: ESP32 ↔ Uno (binary, UART 250000 8N1; ESP32 IO33=RX, IO4=TX)

Frame: `0xAA | len | type | seq | payload[len] | crc8`
- `len` = payload length (0–48). CRC-8 (poly 0x07, init 0x00) over `len, type, seq, payload`.
- Little-endian integers. Corrupt/unknown frames are dropped and counted (`err_frames`).
- Commands needing confirmation are acked by `ACK{seq,status}`; streaming types are not.
- Heartbeat: ESP32 → Uno every 100 ms. **No valid frame for 300 ms → Uno treats the link as lost (deadman stop).**
- Clock: Uno sends `uno_ms` in fast telemetry; the ESP32 maps it to `tank_ms` (offset filtered from heartbeats).

### ESP32 → Uno
| Type | Name | Payload |
|---|---|---|
| 0x01 | HEARTBEAT | esp_ms u32 |
| 0x02 | HELLO_ACK | proto u8 (sent after restoring ENVELOPE; Uno marks link synced) |
| 0x03 | HELLO_REQ | — (Uno re-sends HELLO) |
| 0x10 | DRIVE | src u8 (1 human, 2 ai), v i8, w i8 |
| 0x11 | DRIVE_RATE | src, v i8, yaw_rate i16 (0.1°/s) |
| 0x12 | STOP | src |
| 0x13 | TURN | src, mode u8 (0 by, 1 to; bit7 = pivot), angle i16 (0.1°), speed u8, radius_cm u16 (0 = spin) |
| 0x14 | MOVE | src, dist_mm i32, speed u8, heading i16 (0.1°; 0x7FFF = current) |
| 0x15 | CANCEL | src |
| 0x20 | CAM_JOY | x i8, y i8, active u8 |
| 0x21 | CAM_SET | pan i16, tilt i16 (0.1°), speed u8 (°/s/2) |
| 0x22 | CAM_NUDGE | dpan i16, dtilt i16 |
| 0x23 | CAM_HOME | — |
| 0x24 | CAM_LOCK | on u8 |
| 0x30 | LED | mode u8, r,g,b u8, period u16, duration u16 |
| 0x31 | SOUND | id u8 |
| 0x32 | TONE | n u8, notes[n] {freq u16, ms u16} (n ≤ 10) |
| 0x33 | AUDIO_MODE | mode u8 |
| 0x40 | BRAKE | on u8 |
| 0x41 | ENVELOPE | speed_cap u8, accel_cap u8, lock_brake u8 |
| 0x42 | OWNER_HINT | owner u8 (for LED colour) |
| 0x43 | SAFETY_OVERRIDE | src u8, on u8 (acked; refusals ROLE_FORBIDDEN / ENVELOPE_LOCKED (child play only) / NOT_OWNER / BLOCKED / BUSY) |
| 0x50 | SETTING_SET | id u8, value i32 |
| 0x51 | SETTING_GET | id u8 |
| 0x52 | SETTINGS_SAVE | — |
| 0x60 | CALIBRATE | op u8 (start/cancel), script u8, param i32, profile u8 — Uno runs motion scripts only; the **ESP32 computes results** and writes them via SETTING_SET |
| 0x61 | PROFILE | op u8 (select/create/delete), index u8 |

### Uno → ESP32
| Type | Name | Payload |
|---|---|---|
| 0x80 | HELLO | fw u16, proto u8, caps u8, reset_reason u8, commit u32 (first 8 hex digits of the git commit; 0 = unknown; protocol v2) |
| 0x81 | ACK | ack_seq u8, status u8 |
| 0x90 | TELEM_FAST | uno_ms u32, yaw u16, yaw_rate i16, pitch i16, roll i16, spd_cmd i8×2, spd_act i8×2, pwm i16×2, dist_mm u16, pan i16, tilt i16, enc i32×2, flags u16, prim u8, prim_pct u8 (~40 B) |
| 0x91 | TELEM_SLOW | uptime_s u32, reset_reason u8, batt_mv u16, driving u8, profile_idx u8, cal_status 3 B, hw u8, temp i16, owner u8, flags u32, accel_var u16, loop_load u8, loop_max_us u16, free_ram u16, err_frames u16, err_i2c u16, settings_crc u16, envelope u8×3 |
| 0x92 | EVENT | kind u8, a i16, b i16 |
| 0x93 | SETTING_VALUE | id u8, value i32 |
| 0x94 | CAL_RESULT | script u8, stage u8, progress u8, status u8 |
| 0x95 | CAL_DATA | script u8, phase u8, t_ms u16, v i16×8 (raw samples @ 50 Hz for ESP32-side computation) |
| 0x96 | WIFI_RESET_REQ | — (mode button held 5 s at boot; resent until ACK) |
| 0x97 | ADMIN_RESET_REQ | — (mode button held 15 s at boot; clears admin password + keys; resent until ACK) |

Bandwidth: TELEM_FAST at 20 Hz ≈ 0.9 KB/s of 11.5 KB/s available.
