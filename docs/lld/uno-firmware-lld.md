# Uno Firmware — Low-Level Design

Status: IN PROGRESS · Date: 2026-10-10 · Implements: Spec 2 Part A, Spec 1 §8 · Standards: `docs/standards/coding-standards.md` §2

Sections: 1 Structure & execution ✅ · 2 Ports · 3 Motion control · 4 Safety & arbitration · 5 Comms · 6 Sensors & drivers · 7 Settings, calibration, self-test · 8 Memory budget & error handling

---

## 1. Structure & execution model (APPROVED)

### 1.1 Directory layout (PlatformIO, `firmware/uno/`)
```
src/
├── main.cpp                    composition root: static objects, wiring, setup(), loop()
├── app/
│   ├── scheduler.h/.cpp        fixed task table, due-time dispatch, load measurement
│   ├── comms_dispatcher.*      frame → command routing (arbitration, settings, calibration…)
│   └── telemetry_builder.*     builds TELEM_FAST / TELEM_SLOW / EVENT frames
├── domain/                     pure C++, no Arduino.h
│   ├── motion/                 speed_controller, heading_controller, mixer, ramp, deadband,
│   │                           battery_comp, primitives, turn_profile
│   ├── safety/                 safety_supervisor, envelope
│   ├── arbitration/            arbitrator (IR vs network, handback timer)
│   ├── camera/                 servo_profile (trapezoid), camera_head (joy/set/nudge/lock/home)
│   ├── ui/                     sound_sequencer, led_sequencer, catalogues (PROGMEM tables)
│   ├── estimation/             attitude (complementary filter), yaw_integrator, encoder_speed,
│   │                           bump_detector, lift_detector
│   ├── calibration/            calibration_runner + one routine per item
│   ├── selftest/               self_test
│   └── util/                   pid.h, fixed math, ring_buffer.h, units.h, crc8.h
├── ports/                      headers only (contracts)
├── drivers/                    Uno implementations of the ports (Arduino.h allowed)
└── generated/                  protocol frames + error registry (from /protocol)
test-fakes/                     host implementations of the ports (link-time swap)
```

### 1.2 Object graph
All objects static, created in `main.cpp`. Drivers implement ports; domain uses ports; app orchestrates.
**Only the safety stage writes the motor output** (safety by structure).

### 1.3 Execution model
- `loop()` → `scheduler.run()`: fixed task table (period + next-due), run-to-completion, never blocking; time from `micros()` via the clock port, wrap-safe.
- **100 Hz control tick, fixed order:**
  1. `link.poll()` — parse RX bytes → queue commands
  2. `ir.poll()` — decoded keys → queue commands
  3. `imu.update()` — (200 Hz task) latest attitude/yaw
  4. `encoders.sample()` — counts → speed estimate
  5. `arbitration.update()` — active source (IR > network), handback timers
  6. `primitives.update()` — drive/turn/move state machine → target v, ω
  7. `heading.update()` — gyro PID → Δ correction
  8. `speed.update()` — per-track PID or open loop → raw PWM request
  9. `safety.update()` — final gate → motor output (only write)
- Other tasks: servos 50 Hz · ultrasonic 20 Hz · UI 50 Hz · battery 10 Hz · telemetry 20 Hz / 1 Hz · button 50 Hz.
- **Timing budget:** IMU ~0.4 ms @200 Hz, control chain ~1.5 ms @100 Hz, telemetry ~0.3 ms → expected load 30–40 %; measured `loop_load`, `loop_max_us` reported.
- **Overrun rule:** late task → next due = now + period (no catch-up bursts); overrun counter reported.
- **Watchdog:** 1 s, kicked by the scheduler after a full pass; reset reason reported at next boot.
