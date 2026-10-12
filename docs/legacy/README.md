# Legacy firmware (decoded 2026-10-12)

The tank arrived with an owner-modified firmware pair whose source code is lost. It was read back (read-only) and decoded by disassembly so the new design can learn from it. Both programs are replaced by Plans B (Uno) and C (ESP32).

| File | Content |
|---|---|
| `esp32s3-old-firmware.md` | ESP32-S3 board facts, recovered pin map (UART RX3/TX40, ESP32-S3-EYE camera map), HTTP routes, UDP 8888 bridge |
| `uno-old-protocol.md` | Old Uno JSON protocol (115200 baud, `N` selector), drive semantics, deadman, safety stops, telemetry |
| `uno-old-led-buzzer.md` | Old LED and buzzer sequences (added when decoded) |

Verified live on 2026-10-12: STOP, 0.3 s pulses, 1 s forward/back/spins at 140–150/255 (≈120°/s spin, straight within 1°), telemetry at 10 Hz on UDP port 8888.

## Old protocol vs the new design
| | Old (decoded) | New (Plan A protocol + Plan B firmware) |
|---|---|---|
| Format | JSON text line: `{"N":4,"D1":140,"D2":140}` | Binary frame `0xAA len type seq payload CRC-8`, ~10 B per drive command |
| Integrity | none (a garbled line may parse to other numbers) | CRC-8 per frame; parser resyncs on the next good frame |
| Delivery | UDP, no acks, no sequence numbers; a lost STOP is covered only by the 0.5 s timeout | sequence numbers + ACKs; STOP resent until acked |
| Link | 115200 baud; ArduinoJson parse every 20 ms (RAM/flash/CPU heavy) | 250000 baud; generated fixed layouts, no parser library |
| Telemetry | ~130 B JSON at 10 Hz (V, Y, P, R, distance, servos) | TELEM_FAST 40 B @ 20 Hz + TELEM_SLOW 36 B @ 1 Hz |
| Start-up | none — anything that sends JSON can drive | HELLO → ENVELOPE (safety limits) → HELLO_ACK before network motion |
| Commands | drive two tracks, gyro turn, camera servos, a few undecoded | DRIVE, DRIVE_RATE, TURN (spin/arc/pivot, by/to), MOVE distance, CANCEL, camera smooth/joystick, LED/SOUND/TONE, settings, calibration scripts, profiles |
| Errors | silently ignored | numbered error codes + reason (`COMMAND_3001 NOT_OWNER`) |
| Low speed | |speed| < 80 → 0; any motion jumps to ≥ 110/255 (~43 %) | deadband compensation with kick + per-track encoder speed PID → smooth crawl |
| Straight driving | gyro PID only while both tracks are nearly equal | heading controller (rate + hold) for teleop, TURN and MOVE; encoders for distance |
| Deadman | 0.5 s for every drive command | 0.3 s for network teleop; IR unaffected |
| Obstacle | stop < 25 cm when going forward | slow zone + stop zone (settings); degraded caps if the sensor fails |
| Tilt | stop above 35° | gyro + accelerometer attitude; stop above `tilt_limit` (45°) held 300 ms; lift detection |
| Control priority | last sender wins | IR > human > AI with handback timers; stop on every owner change, never resume stale commands |
| Child mode | none | child-play envelope (speed/accel caps enforced on the Uno) |
| ESP32 | UDP pass-through, no timeout, no stop, no authentication | Phase 3 dev bridge (TCP, no auth, home network only) → later gateway: WebSocket API, keys with roles, STOP fast path, arbitration |

## Open item found by decoding
The old Uno program drives **PWM D6 with direction D7** and **PWM D5 with direction D8** (medium confidence), while our hardware facts (ELEGOO shield docs) pair **D5 with D7** and **D6 with D8**. Plan B's shield probe (Task 7, on the stand) tests each track and direction before any driving and settles this.
