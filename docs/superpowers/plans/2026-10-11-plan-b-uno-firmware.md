# Plan B — Uno Firmware (Roadmap Phases 1–2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Arduino Uno "body" firmware: hardware drivers, safety supervisor, gyro/encoder motion control, IR override, camera servos, sounds/LED, settings and calibration scripts, speaking the Plan A body-link protocol at 250000 baud — verified on the stand (phase 1) and on the floor (phase 2).

**Architecture:** Functional core / imperative shell. `src/domain/` is pure C++ (no Arduino.h, no ports) and is TDD-tested on the PC with PlatformIO's `native` env + Unity. `src/app/` task functions read ports, call domain objects and write ports; `src/drivers/` implement `src/ports/` (namespaced free functions, link-time selected; no `virtual`). Only `SafetySupervisor` can construct a moving `PwmOutput`, and only `app/control_task.cpp` includes the motor port. A fixed-table cooperative scheduler runs everything without `delay()`.

**Tech Stack:** PlatformIO (`atmelavr` + Arduino core, avr-gcc 7.3, C++17 `gnu++17`), Arduino `Servo` + `Wire` (only external libraries), Unity (native host tests), Python 3.12 + uv + Plan A's `conqueror_protocol` + pyserial for the stand smoke script.

**Spec:** `docs/lld/uno-firmware-lld.md` (§1–§8 plus **§9 Revision 2** and the appended "Hardware revision" / "Decisions" notes — authoritative where they differ), `docs/superpowers/specs/2026-10-10-02-tank-firmware-design.md` Part A (+ appended notes), `docs/superpowers/specs/2026-10-10-01-protocol-design.md` §8, `docs/standards/coding-standards.md` §1–§2.

**Roadmap position:** Phases 1–2 of `docs/implementation-roadmap.md`. Depends on Plan A (`firmware/uno/src/generated/cq_protocol.h`, `protocol/python/conqueror_protocol`). Plan C (WiFi bridge) and Plan D (bench tool) drive this firmware.

## Global Constraints

- **Platform:** commands are shown for macOS; on **Linux** use the equivalents in `docs/dev-setup.md` (uv install, serial port names `/dev/ttyACM0`/`/dev/ttyUSB0` + `dialout` group, PlatformIO udev rules, `avahi-browse` instead of `dns-sd`, hold-to-drive needs X11).
- Hardware (confirmed): Arduino Uno + ELEGOO **SmartCar Shield V1.1, TB6612FNG**: PWM right (A) **D5**, PWM left (B) **D6**, AIN1 **D7**, BIN1 **D8**, STBY **D3** (HIGH = enabled, LOW = coast). Build option `-DSHIELD_SMARTCAR_V11_TB6612` (default) or `-DSHIELD_TANK_DRV8835` (PH/EN: PWM D5/D6, PH right D8, PH left D7, no STBY). Board option `-DBOARD_UNO`.
- Pins: encoders (FIT0458, channel A only) left **A1**, right **A2** (PCINT1, rising edges only); passive buzzer **A0** (`tone()`, Timer2); ultrasonic trig **D13**, echo **D12**; IR receiver **D9** (custom NEC decoder, no IRremote); PCINT0 ISR shared by D9 + D12; servos pan **D10**, tilt **D11** (Servo lib, Timer1); WS2812 ×1 on **D4** (own bit-bang, no FastLED); button **D2**; battery **A3**; MPU6050 on I2C A4/A5 @ **0x68**.
- UART **250000** baud on D0/D1 (shield switch **S1** disconnects the ESP32 for USB upload).
- Frame `0xAA | len | type | seq | payload | crc8`, max payload 48, CRC-8 poly 0x07 — all from `cq_protocol.h`; never hand-code frame layouts.
- No heap, no `String`, no STL, no exceptions/RTTI, no `virtual`, no `delay()`/`pulseIn()`, busy-waits ≤ 50 µs except the documented MPU6050 burst read (~380 µs) and I2C bus recovery (~100 µs, only after a Wire timeout).
- ISRs capture only (< 10 µs); shared data `volatile` + `ATOMIC_BLOCK` for multi-byte reads.
- Members use `m_` prefix; `enum class` everywhere; units in names (`_ms`, `_us`, `_deg`, `_pct`, `_mm`, `_cps`).
- Domain code compiles on host (`CQ_HOST_TEST`) and AVR; it includes `types/`, `domain/`, `generated/` only.
- Build gates: static RAM ≤ **1750 B**, flash ≤ **30 KB** (`scripts/size_gate.py`).
- Timing: IMU 200 Hz, control/safety 100 Hz, servos/UI/button 50 Hz, ultrasonic 20 Hz, battery 10 Hz, telemetry fast 20 Hz / slow 1 Hz; watchdog 1 s.
- Safety rules (LLD §9.2): link loss stops only Network/Calibration motion; deadman 300 ms only for network Teleop; link loss cancels primitives (no resume); IR works with no ESP32.
- MVP scope: one surface profile (index 0). `PROFILE` → `UNAVAILABLE` except `SELECT 0`. Calibration maths runs off-board; the Uno runs motion scripts and streams `CAL_DATA`.
- Correction recorded (from the original ELEGOO firmware constants): IR codes **5 = 0x00FF18E7, 8 = 0x00FF38C7, 9 = 0x00FF5AA5, 0 = 0x00FF4AB5, * = 0x00FF42BD, # = 0x00FF52AD** (LLD §9.8 listed three of these wrongly; Task 15 fixes the LLD).

## Review Focus

1. **ESP32 absent or crashed while driving with the IR remote** → the tank keeps obeying IR; it must not SoftStop (Task 6 test `test_ir_owner_ignores_link_loss`).
2. **Link drops in the middle of a TURN/MOVE** → the primitive is cancelled with `LINK_LOST` and does **not** resume when the link returns (Task 10 test `test_link_loss_cancels_and_does_not_resume`, wired in Task 11).
3. **Child-play envelope set to 20 %** → actual track targets never exceed 20 % and the speed PID does not wind up (Task 8 test `test_envelope_caps_targets`, Task 9 test `test_integrator_frozen_when_capped`).
4. **A held track vs a broken encoder** → a held track is a stall (the supervisor cuts that track; cleared on reversal or after 1 s), a silent encoder while the gyro shows motion is a latched encoder fault (Task 7 test `test_fault_vs_stall`).
5. **Settings saved while the tank is driving or right after a firmware update** → EEPROM writes are spread one byte per pass (no watchdog reset, no lost UART bytes) and a bad CRC block falls back to defaults (Task 3 tests `test_writer_one_byte_per_step`, `test_serialize_roundtrip_and_bad_crc_loads_defaults`).

---

## File Structure

```
firmware/uno/
├── platformio.ini                       envs: uno (avr), native (host tests)        (Task 1)
├── scripts/size_gate.py                 RAM/flash gate on the ELF                     (Task 1)
├── src/
│   ├── main.cpp                         composition root: App, task table, setup/loop (Task 1, grows)
│   ├── generated/cq_protocol.h          from Plan A (do not edit)
│   ├── types/
│   │   ├── hw_types.h                   EncoderSnapshot, ImuRaw, IrEvent, Rgb, EchoState, Axis (Task 1)
│   │   └── pwm_output.h                 PwmOutput (only SafetySupervisor builds non-zero)     (Task 6)
│   ├── ports/                           clock, watchdog, link, eeprom, imu, ultrasonic, ir, battery_adc,
│   │                                    button, motor, encoders, servos, tone, led           (Tasks 1–13)
│   ├── drivers/                         board_config.h + *_uno.cpp implementations        (Tasks 1–13)
│   ├── domain/
│   │   ├── util/      math_util.h, scheduler.*, pid.*                                     (Tasks 1, 8)
│   │   ├── comms/     frame_parser.*, frame_writer.*, event_queue.*, link_session.*        (Task 2)
│   │   ├── settings/  settings.*, eeprom_writer.*, crc16.h                                 (Task 3)
│   │   ├── estimation/ attitude.*, yaw_integrator.*, bump_detector.*, lift_detector.*,
│   │   │              accel_variance.*, imu_scale.h, ultrasonic_filter.*, battery_monitor.*,
│   │   │              encoder_track.*, encoder_health.*                                    (Tasks 4, 5, 7)
│   │   ├── input/     nec_decoder.*, ir_codes.h, ir_controller.*, boot_button.*             (Tasks 5, 14)
│   │   ├── safety/    envelope.*, safety_supervisor.*                                       (Task 6)
│   │   ├── arbitration/ arbitrator.*                                                       (Task 6)
│   │   ├── motion/    ramp.*, deadband.*, battery_comp.h, mixer.*, target_limiter.*,
│   │   │              heading_controller.*, speed_controller.*, motion_setpoint.h,
│   │   │              turn_profile.*, move_profile.*, primitives.*                         (Tasks 8–10)
│   │   ├── calibration/ cal_script_runner.*, cal_status.*                                  (Task 7)
│   │   ├── selftest/  self_test.h                                                          (Task 7)
│   │   ├── camera/    servo_profile.*, camera_head.*                                       (Task 12)
│   │   └── ui/        sound_sequencer.*, led_sequencer.*, annunciator.*, catalogues.*      (Task 13)
│   └── app/
│       ├── app.h                        App context struct (grows)                         (Task 2+)
│       ├── comms.h/.cpp                 RX pump + prioritised TX pump, telemetry stamping  (Task 2+)
│       ├── routes.cpp                   PROGMEM routing table (rows added per task)        (Task 2+)
│       ├── tasks.h                      declarations of app task/init entry points         (Tasks 3–14)
│       ├── motion_api.h                 control/calibration entry points + motion handlers (Tasks 6, 7, 11, 14)
│       ├── camera_api.h                 configureCamera + CAM_* handlers                   (Task 12)
│       ├── ui_api.h                     LED/SOUND/TONE/AUDIO_MODE handlers                 (Task 13)
│       ├── apply_settings.cpp           pushes settings into domain objects               (Task 3+)
│       └── *_task.cpp                   settings, imu, sensor, control, calibration, camera,
│                                        ui, input tasks                                    (Tasks 3–14)
└── test/test_<unit>/test_main.cpp       Unity host tests
tools/smoke/pyproject.toml               uv project shared with Plan C (Task 2)
tools/smoke/uno_smoke.py                 stand/floor smoke script (Task 2, grows)
```

All commands run from `firmware/uno/` unless stated.

---

### Task 1: PlatformIO skeleton, board config, scheduler, watchdog, size gate

**Files:**
- Create: `firmware/uno/platformio.ini`, `scripts/size_gate.py`, `src/main.cpp`, `src/types/hw_types.h`, `src/domain/util/math_util.h`, `src/domain/util/scheduler.h`, `src/domain/util/scheduler.cpp`, `src/ports/clock.h`, `src/ports/watchdog.h`, `src/drivers/board_config.h`, `src/drivers/clock_uno.cpp`, `src/drivers/watchdog_uno.cpp`
- Test: `test/test_scheduler/test_main.cpp`

**Interfaces:**
- Consumes: `cq::proto::ResetReason` from `generated/cq_protocol.h`.
- Produces:
  - `cq::TaskFn = void (*)()`, `struct cq::Task { TaskFn fn; uint32_t period_us; uint32_t next_due_us; }`, `class cq::Scheduler(Task* tasks, uint8_t count)` with `void start(uint32_t now_us)`, `uint8_t runDue(uint32_t now_us)`, `uint16_t overruns() const`.
  - `class cq::LoadMeter` with `void add(uint32_t busy_us, uint32_t now_us)`, `uint8_t loadPct() const`, `uint16_t maxUs() const`.
  - `cq::clampf, signf, absf, wrap180, wrap360, elapsed(uint32_t now, uint32_t since, uint32_t interval)`.
  - Ports: `cq::port::clock::nowUs() -> uint32_t`, `nowMs() -> uint32_t`; `cq::port::watchdog::enable()`, `kick()`, `resetReason() -> proto::ResetReason`, `markIntentionalReset()`.
  - `cq::board` constants (all pins, `kHasStandby`, direction polarity, IMU axis map).
  - Value types in `types/hw_types.h` (see code).

- [ ] **Step 1: Install PlatformIO**

Run: `uv tool install platformio && pio --version`
Expected: `PlatformIO Core, version 6.x.y`.

- [ ] **Step 2: Create the project config**

`firmware/uno/platformio.ini`:
```ini
[platformio]
default_envs = uno

[env]
build_unflags = -std=gnu++11
build_flags =
  -std=gnu++17
  -Isrc
  -DSHIELD_SMARTCAR_V11_TB6612
  -DBOARD_UNO

[env:uno]
platform = atmelavr@^5
board = uno
framework = arduino
lib_deps = arduino-libraries/Servo@^1.2.2
build_flags = ${env.build_flags} -Os -flto -fno-exceptions -fno-rtti
build_src_flags = -Wall -Wextra -Werror
upload_speed = 115200
monitor_speed = 250000

[env:native]
platform = native@^1
build_src_filter = +<domain/> +<types/>
test_build_src = yes
build_flags = ${env.build_flags} -DCQ_HOST_TEST -lm
build_src_flags = -Wall -Wextra -Werror
```
(`-Werror` applies to our sources only, not to Unity or the Arduino core.)

- [ ] **Step 2a: First-build gate — the generated header compiles for AVR with `gnu++17`**

Plan A only compiled `cq_protocol.h` with the host compiler. Before writing firmware code, prove avr-gcc 7.3 accepts it (it uses C++17 `inline` variables and `<avr/pgmspace.h>`). The toolchain lives under PlatformIO and exists only after the project packages are installed:
```sh
cd firmware/uno && pio pkg install -e uno
echo '#include "generated/cq_protocol.h"' | ~/.platformio/packages/toolchain-atmelavr/bin/avr-g++ \
  -std=gnu++17 -mmcu=atmega328p -Os -fno-exceptions -fno-rtti -Wall -Wextra -Werror \
  -Isrc -x c++ -c - -o /dev/null && echo "cq_protocol.h OK on AVR"
```
Expected: `cq_protocol.h OK on AVR`, no warnings. If it fails, fix the emitter in `protocol/generator/` and regenerate (`cd protocol && uv run protogen write`); never edit the generated header.

- [ ] **Step 3: Write the failing scheduler test**

`test/test_scheduler/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/util/scheduler.h"

namespace {
int g_a = 0;
int g_b = 0;
void taskA() { ++g_a; }
void taskB() { ++g_b; }
}  // namespace

void setUp() { g_a = 0; g_b = 0; }
void tearDown() {}

void test_runs_each_task_at_its_period() {
  cq::Task tasks[] = {{taskA, 10000, 0}, {taskB, 20000, 0}};
  cq::Scheduler s(tasks, 2);
  s.start(0);
  for (uint32_t t = 0; t <= 100000; t += 1000) {
    s.runDue(t);
  }
  TEST_ASSERT_EQUAL(11, g_a);
  TEST_ASSERT_EQUAL(6, g_b);
}

void test_overrun_reschedules_from_now_without_bursts() {
  cq::Task tasks[] = {{taskA, 10000, 0}};
  cq::Scheduler s(tasks, 1);
  s.start(0);
  s.runDue(0);
  s.runDue(55000);  // very late: runs once, next due = 65000
  s.runDue(60000);
  TEST_ASSERT_EQUAL(2, g_a);
  TEST_ASSERT_EQUAL(1, s.overruns());
  s.runDue(65000);
  TEST_ASSERT_EQUAL(3, g_a);
}

void test_wraparound_is_safe() {
  cq::Task tasks[] = {{taskA, 10000, 0}};
  cq::Scheduler s(tasks, 1);
  const uint32_t near_wrap = 0xFFFFFFFFu - 4000u;
  s.start(near_wrap);
  s.runDue(near_wrap);
  s.runDue(near_wrap + 10000u);  // wraps past zero
  TEST_ASSERT_EQUAL(2, g_a);
}

void test_load_meter() {
  cq::LoadMeter m;
  m.add(250000, 500000);
  m.add(250000, 1000000);  // window closes: 500 ms busy of 1000 ms
  TEST_ASSERT_EQUAL(50, m.loadPct());
  TEST_ASSERT_EQUAL(0xFFFF, m.maxUs());  // 250 ms clamps to the u16 maximum
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_runs_each_task_at_its_period);
  RUN_TEST(test_overrun_reschedules_from_now_without_bursts);
  RUN_TEST(test_wraparound_is_safe);
  RUN_TEST(test_load_meter);
  return UNITY_END();
}
```

- [ ] **Step 4: Run it to see it fail**

Run: `pio test -e native -f test_scheduler`
Expected: FAIL — `domain/util/scheduler.h: No such file or directory`.

- [ ] **Step 5: Implement math utilities and the scheduler**

`src/domain/util/math_util.h`:
```cpp
#pragma once
#include <math.h>
#include <stdint.h>

namespace cq {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float signf(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
inline float absf(float v) { return v < 0.0f ? -v : v; }

inline float wrap180(float deg) {
  while (deg > 180.0f) deg -= 360.0f;
  while (deg <= -180.0f) deg += 360.0f;
  return deg;
}

inline float wrap360(float deg) {
  while (deg >= 360.0f) deg -= 360.0f;
  while (deg < 0.0f) deg += 360.0f;
  return deg;
}

// Wrap-safe "has `interval` passed since `since`".
inline bool elapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(now - since) >= interval;
}

}  // namespace cq
```

`src/domain/util/scheduler.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

using TaskFn = void (*)();

struct Task {
  TaskFn fn;
  uint32_t period_us;
  uint32_t next_due_us;
};

// Fixed task table, run-to-completion, no catch-up bursts (late task: next = now + period).
class Scheduler {
 public:
  Scheduler(Task* tasks, uint8_t count) : m_tasks(tasks), m_count(count) {}
  void start(uint32_t now_us);
  uint8_t runDue(uint32_t now_us);
  uint16_t overruns() const { return m_overruns; }

 private:
  Task* m_tasks;
  uint8_t m_count;
  uint16_t m_overruns = 0;
};

// Busy time per 1 s window → load %; longest single busy pass.
class LoadMeter {
 public:
  void add(uint32_t busy_us, uint32_t now_us);
  uint8_t loadPct() const { return m_load_pct; }
  uint16_t maxUs() const { return m_max_us; }

 private:
  uint32_t m_window_start_us = 0;
  uint32_t m_busy_us = 0;
  uint8_t m_load_pct = 0;
  uint16_t m_max_us = 0;
};

}  // namespace cq
```

`src/domain/util/scheduler.cpp`:
```cpp
#include "domain/util/scheduler.h"

namespace cq {

void Scheduler::start(uint32_t now_us) {
  for (uint8_t i = 0; i < m_count; ++i) {
    m_tasks[i].next_due_us = now_us;
  }
}

uint8_t Scheduler::runDue(uint32_t now_us) {
  uint8_t ran = 0;
  for (uint8_t i = 0; i < m_count; ++i) {
    Task& t = m_tasks[i];
    if (static_cast<int32_t>(now_us - t.next_due_us) < 0) {
      continue;
    }
    t.fn();
    ++ran;
    const uint32_t next = t.next_due_us + t.period_us;
    if (static_cast<int32_t>(now_us - next) >= 0) {
      t.next_due_us = now_us + t.period_us;
      ++m_overruns;
    } else {
      t.next_due_us = next;
    }
  }
  return ran;
}

void LoadMeter::add(uint32_t busy_us, uint32_t now_us) {
  m_busy_us += busy_us;
  if (busy_us > m_max_us) {
    m_max_us = busy_us > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(busy_us);
  }
  const uint32_t window = now_us - m_window_start_us;
  if (window >= 1000000u) {
    const uint32_t pct = (m_busy_us * 100u) / window;
    m_load_pct = pct > 100u ? 100u : static_cast<uint8_t>(pct);
    m_busy_us = 0;
    m_window_start_us = now_us;
  }
}

}  // namespace cq
```

- [ ] **Step 6: Run the test to see it pass**

Run: `pio test -e native -f test_scheduler`
Expected: `4 Tests 0 Failures 0 Ignored` — `PASSED`.

- [ ] **Step 7: Board config, value types, clock and watchdog ports + drivers**

`src/types/hw_types.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

struct EncoderSnapshot {
  int32_t count_l;
  int32_t count_r;
  uint32_t period_l_us;     // time between the last two rising edges (0 = none yet)
  uint32_t period_r_us;
  uint32_t last_edge_l_us;
  uint32_t last_edge_r_us;
  uint32_t t_us;
};

struct ImuRaw {  // tank frame (x forward, y left, z up) after the board axis map
  int16_t ax, ay, az;
  int16_t gx, gy, gz;
  int16_t temp;
  uint32_t t_us;
};

struct IrEvent {
  uint32_t code;
  bool repeat;
  uint32_t t_ms;
};

struct Rgb {
  uint8_t r, g, b;
};

inline bool operator==(const Rgb& a, const Rgb& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
inline bool operator!=(const Rgb& a, const Rgb& b) { return !(a == b); }

enum class EchoState : uint8_t { kPending, kReady, kClear, kNoEcho };
enum class Axis : uint8_t { kPan = 0, kTilt = 1 };

}  // namespace cq
```

`src/drivers/board_config.h`:
```cpp
#pragma once
#include <stdint.h>

#if !defined(BOARD_UNO)
#error "select a board: -DBOARD_UNO"
#endif

namespace cq {
namespace board {

#if defined(SHIELD_SMARTCAR_V11_TB6612)
constexpr uint8_t kPinPwmRight = 5;   // PWMA
constexpr uint8_t kPinPwmLeft = 6;    // PWMB
constexpr uint8_t kPinDirRight = 7;   // AIN1
constexpr uint8_t kPinDirLeft = 8;    // BIN1
constexpr uint8_t kPinStandby = 3;    // STBY, HIGH = enabled
constexpr bool kHasStandby = true;
#elif defined(SHIELD_TANK_DRV8835)
constexpr uint8_t kPinPwmRight = 5;   // AIN2/EN
constexpr uint8_t kPinPwmLeft = 6;    // BIN2/EN
constexpr uint8_t kPinDirRight = 8;   // AIN1/PH
constexpr uint8_t kPinDirLeft = 7;    // BIN1/PH
constexpr uint8_t kPinStandby = 0xFF;
constexpr bool kHasStandby = false;
#else
#error "select a shield: -DSHIELD_SMARTCAR_V11_TB6612 or -DSHIELD_TANK_DRV8835"
#endif

// Direction pin level that drives each track forward. Measured by the shield probe (Task 7);
// starting values from the ELEGOO tank firmware (right forward = LOW, left forward = HIGH).
constexpr bool kDirRightForwardHigh = false;
constexpr bool kDirLeftForwardHigh = true;

constexpr uint8_t kPinButton = 2;
constexpr uint8_t kPinLed = 4;        // PD4
constexpr uint8_t kPinIr = 9;         // PB1, PCINT1 (vector PCINT0)
constexpr uint8_t kPinServoPan = 10;
constexpr uint8_t kPinServoTilt = 11;
constexpr uint8_t kPinEcho = 12;      // PB4, PCINT4 (vector PCINT0)
constexpr uint8_t kPinTrig = 13;
constexpr uint8_t kPinBuzzer = 14;    // A0
constexpr uint8_t kPinEncLeft = 15;   // A1 = PC1 (vector PCINT1)
constexpr uint8_t kPinEncRight = 16;  // A2 = PC2 (vector PCINT1)
constexpr uint8_t kPinBattery = 17;   // A3

constexpr uint8_t kImuAddress = 0x68;
// Tank axis <- sensor axis (1 = X, 2 = Y, 3 = Z; negative = inverted). Measured by the mount check.
constexpr int8_t kImuMapX = 1;
constexpr int8_t kImuMapY = 2;
constexpr int8_t kImuMapZ = 3;

constexpr uint32_t kBaud = 250000UL;
constexpr uint16_t kFwVersion = 0x0100;  // 1.0

}  // namespace board
}  // namespace cq
```

`src/ports/clock.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace clock {
uint32_t nowUs();
uint32_t nowMs();
}  // namespace clock
}  // namespace port
}  // namespace cq
```

`src/ports/watchdog.h`:
```cpp
#pragma once
#include "generated/cq_protocol.h"

namespace cq {
namespace port {
namespace watchdog {
void enable();   // 1 s
void kick();
proto::ResetReason resetReason();
void markIntentionalReset();  // sets a .noinit marker so the next boot reports kWatchdog reliably
}  // namespace watchdog
}  // namespace port
}  // namespace cq
```

`src/drivers/clock_uno.cpp`:
```cpp
#include <Arduino.h>

#include "ports/clock.h"

namespace cq {
namespace port {
namespace clock {
uint32_t nowUs() { return micros(); }
uint32_t nowMs() { return millis(); }
}  // namespace clock
}  // namespace port
}  // namespace cq
```

`src/drivers/watchdog_uno.cpp`:
```cpp
#include <avr/io.h>
#include <avr/wdt.h>

#include "ports/watchdog.h"

namespace {
// Survive resets; filled before C++ init.
uint8_t g_mcusr __attribute__((section(".noinit")));
uint8_t g_r2 __attribute__((section(".noinit")));
uint8_t g_marker __attribute__((section(".noinit")));
constexpr uint8_t kIntentionalMarker = 0xA5;
}  // namespace

// Optiboot >= 6 passes MCUSR in r2. The stock Uno bootloader clears MCUSR and leaves r2 as garbage,
// so r2 is trusted only when it holds nothing but reset-flag bits; otherwise the reason is kUnknown.
extern "C" void cqCaptureR2() __attribute__((naked, used, section(".init0")));
extern "C" void cqCaptureR2() { asm volatile("sts %0, r2" : "=m"(g_r2)); }

extern "C" void cqCaptureMcusr() __attribute__((naked, used, section(".init3")));
extern "C" void cqCaptureMcusr() {
  g_mcusr = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

namespace cq {
namespace port {
namespace watchdog {

void enable() { wdt_enable(WDTO_1S); }
void kick() { wdt_reset(); }

proto::ResetReason resetReason() {
  constexpr uint8_t kResetFlagBits = _BV(WDRF) | _BV(BORF) | _BV(EXTRF) | _BV(PORF);  // 0x0F
  const bool r2_plausible = g_r2 != 0 && (g_r2 & static_cast<uint8_t>(~kResetFlagBits)) == 0;
  const uint8_t flags = g_mcusr != 0 ? g_mcusr : (r2_plausible ? g_r2 : 0);
  const bool intentional = g_marker == kIntentionalMarker;
  g_marker = 0;  // one-shot; also clears power-on garbage
  if (intentional) return proto::ResetReason::kWatchdog;
  if (flags & _BV(WDRF)) return proto::ResetReason::kWatchdog;
  if (flags & _BV(BORF)) return proto::ResetReason::kBrownOut;
  if (flags & _BV(EXTRF)) return proto::ResetReason::kExternal;
  if (flags & _BV(PORF)) return proto::ResetReason::kPowerOn;
  return proto::ResetReason::kUnknown;
}

void markIntentionalReset() { g_marker = kIntentionalMarker; }

}  // namespace watchdog
}  // namespace port
}  // namespace cq
```

`src/main.cpp` (skeleton; grows in later tasks):
```cpp
#include <Arduino.h>

#include "domain/util/scheduler.h"
#include "drivers/board_config.h"
#include "ports/clock.h"
#include "ports/watchdog.h"

namespace {
void heartbeatTask() {}

cq::Task g_tasks[] = {
    {heartbeatTask, 1000000UL, 0},
};
cq::Scheduler g_scheduler(g_tasks, static_cast<uint8_t>(sizeof g_tasks / sizeof g_tasks[0]));
cq::LoadMeter g_load;
}  // namespace

void setup() {
  const cq::proto::ResetReason reason = cq::port::watchdog::resetReason();
  Serial.begin(cq::board::kBaud);
  Serial.print(F("CQ boot reset="));
  Serial.println(static_cast<uint8_t>(reason));
  cq::port::watchdog::enable();
  g_scheduler.start(cq::port::clock::nowUs());
}

void loop() {
  const uint32_t t0 = cq::port::clock::nowUs();
  if (g_scheduler.runDue(t0) > 0) {
    const uint32_t t1 = cq::port::clock::nowUs();
    g_load.add(t1 - t0, t1);
  }
  cq::port::watchdog::kick();
}
```

- [ ] **Step 8: Size gate script**

`firmware/uno/scripts/size_gate.py`:
```python
#!/usr/bin/env python3
"""Fail if the Uno firmware exceeds the RAM/flash budget (LLD §9.20)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

FLASH_LIMIT = 30 * 1024
RAM_LIMIT = 1750
ELF = Path(__file__).resolve().parents[1] / ".pio" / "build" / "uno" / "firmware.elf"
AVR_SIZE = Path.home() / ".platformio" / "packages" / "toolchain-atmelavr" / "bin" / "avr-size"


def main() -> int:
    out = subprocess.run([str(AVR_SIZE), "-A", str(ELF)], check=True, capture_output=True,
                         text=True).stdout
    sections: dict[str, int] = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith(".") and parts[1].isdigit():
            sections[parts[0]] = int(parts[1])
    flash = sections.get(".text", 0) + sections.get(".data", 0)
    ram = sections.get(".data", 0) + sections.get(".bss", 0) + sections.get(".noinit", 0)
    print(f"size_gate: flash {flash} / {FLASH_LIMIT} B, static RAM {ram} / {RAM_LIMIT} B")
    if flash > FLASH_LIMIT or ram > RAM_LIMIT:
        print("size_gate: OVER BUDGET", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 9: Build, gate, upload, look at the boot line**

Run: `pio run -e uno && python3 scripts/size_gate.py`
Expected: `SUCCESS`; `size_gate: flash ~2000 / 30720 B, static RAM ~200 / 1750 B`. A `cc1: warning: command-line option '-std=gnu++17' is valid for C++ but not for C` line per Arduino-core C file is expected and harmless (`[env] build_flags` reach the core's C files; `-Werror` is only in `build_src_flags`).
Flip shield switch **S1** to "upload", connect USB, run: `pio run -e uno -t upload && pio device monitor -b 250000`
Expected: with Optiboot ≥ 6, `CQ boot reset=1` (power-on) or `2` (reset button); with the stock Uno bootloader (Optiboot 4.4 clears MCUSR and does not pass it in r2) `CQ boot reset=0` (unknown) is correct — note which bootloader the board has. Exit the monitor (Ctrl-C).

- [ ] **Step 10: Commit**

```bash
git add firmware/uno
git commit -m "feat(uno): PlatformIO skeleton, scheduler, watchdog, board config, size gate"
```

---

### Task 2: Body link — parser, writer, queues, link session, TX scheduler, smoke script

**Files:**
- Create: `src/domain/comms/frame_parser.{h,cpp}`, `frame_writer.{h,cpp}`, `event_queue.{h,cpp}`, `link_session.{h,cpp}`; `src/ports/link.h`; `src/drivers/link_uno.cpp`; `src/app/app.h`, `src/app/comms.h`, `src/app/comms.cpp`, `src/app/routes.cpp`; `tools/smoke/pyproject.toml`, `tools/smoke/uno_smoke.py`
- Modify: `src/main.cpp`
- Test: `test/test_comms/test_main.cpp`

**Interfaces:**
- Consumes: `cq_protocol.h` (`crc8`, `kSync`, `kMaxPayload`, `FrameType`, `*Msg`, `encode/decode`, `AckStatus`, `EventKind`).
- Produces:
  - `cq::FrameParser`: `bool feed(uint8_t b)` (queue one received byte; legal only when `!pending()`, otherwise returns false and changes nothing — pending replay bytes are never dropped), `bool next(uint8_t& budget)` (run at most `budget` parser steps, decrementing it; true when a frame is ready — the rest of a replay stays pending for the next call), `bool pending() const` (fed or replay bytes still to be stepped), `uint8_t type() const`, `uint8_t seq() const`, `uint8_t len() const`, `const uint8_t* payload() const` (valid until the next `feed`/`next`), `ParseErrors& errors()`, host-test only `uint32_t stepsTaken() const`; `struct ParseErrors { uint16_t bad_crc, bad_len, unknown_type, tx_dropped; }` — `bad_crc`/`bad_len` count rejection events (a replayed byte can be rejected and counted again). Resync rules match `conqueror_protocol.framing` (Uno LLD §5.2): same frames, order and counters for any stream and any budget split.
  - On `proto::decode()` returning false, the message contents are unspecified — handlers must not use them.
  - `cq::FrameWriter`: `uint8_t build(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t* out)` (returns total bytes), `void setSeq(uint8_t)`, `uint8_t lastSeq() const`.
  - `cq::EventQueue`: `bool push(proto::EventKind k, int16_t a = 0, int16_t b = 0)`, `bool peek(Event& out) const`, `bool pop(Event& out)`, `bool empty() const`, `uint16_t dropped() const`; `struct Event { uint8_t kind; int16_t a; int16_t b; }`.
  - `cq::AckQueue`: `bool push(uint8_t seq, proto::AckStatus s)`, `bool peek(uint8_t& seq, proto::AckStatus& s) const`, `bool pop(uint8_t& seq, proto::AckStatus& s)`. The TX pump peeks, sends, and pops only after the send succeeded, so a full TX buffer never reorders ACKs or events.
  - `cq::RejectLimiter`: `bool allow(uint8_t source, uint32_t now_ms)` (one per 500 ms per source 0–2).
  - `cq::LinkSession`: `onValidFrame(uint32_t now_ms)`, `bool alive(uint32_t now_ms) const`, `LinkChange poll(uint32_t now_ms)` (10 ms link watch: `kLost` on alive→dead, also calls `onLinkLost()`; `kRestored` on dead→alive **after a loss** — the first contact after boot returns `kNone`; `enum class LinkChange : uint8_t { kNone, kLost, kRestored }`), `onLinkLost()`, `onHelloAck()`, `onHelloReq()`, `bool synced() const`, `bool helloDue(uint32_t now_ms)`, `setRequest(uint8_t frame_type)`, `bool requestDue(uint32_t now_ms, uint8_t& frame_type)`, `onRequestSent(uint8_t seq, uint32_t now_ms)`, `onEspAck(uint8_t ack_seq)`.
  - Port `cq::port::link`: `begin(uint32_t baud)`, `bool readByte(uint8_t& b)`, `bool write(const uint8_t* data, uint8_t len)`, `uint8_t txFree()`.
  - `cq::App` (app/app.h) with members listed in code (`parser, writer, events, acks, rejects, link, fast, slow, fast_due, slow_due, cal_data(_pending), cal_result(_pending), setting_value(_pending), load, scheduler_overruns, reset_reason, encoders_enabled`); `cq::g_app`; `cq::app::commsPoll()`, `cq::app::txPump()`, `cq::app::telemetryFastDue()`, `cq::app::telemetrySlowDue()`; routing: `cq::app::Route { uint8_t type; proto::AckStatus (*handler)(const uint8_t*, uint8_t); bool needs_ack; }`, `cq::app::kRoutes[] PROGMEM`, `cq::app::kRouteCount`.
  - Smoke script CLI (in `tools/smoke/`): `uv run python uno_smoke.py --port <dev> watch`.

- [ ] **Step 1: Write the failing comms tests**

`test/test_comms/test_main.cpp`:
```cpp
#include <string.h>
#include <unity.h>

#include "domain/comms/event_queue.h"
#include "domain/comms/frame_parser.h"
#include "domain/comms/frame_writer.h"
#include "domain/comms/link_session.h"

using namespace cq;

void setUp() {}
void tearDown() {}

// Golden frames from protocol/defs/golden.yaml
static const uint8_t kStop[] = {0xAA, 0x01, 0x12, 0x05, 0x01, 0x24};
static const uint8_t kTurn[] = {0xAA, 0x07, 0x13, 0x07, 0x02, 0x81, 0x7C, 0xFC, 0x28, 0x00, 0x00, 0xE5};
static const uint8_t kAck[] = {0xAA, 0x02, 0x81, 0x03, 0x07, 0x00, 0x35};

// Feeds bytes the way commsPoll does (feed only when nothing is pending, then run next() with a
// step budget until the byte and its replay are done) and records frame types. A small budget
// splits the replay across many next() calls; results must not depend on it.
struct Got {
  uint8_t types[8];
  uint8_t n;
};
static uint8_t feedAll(FrameParser& p, const uint8_t* data, uint8_t len, Got& got,
                       uint8_t budget = 64) {
  uint8_t frames = 0;
  for (uint8_t i = 0; i < len; ++i) {
    if (!p.feed(data[i])) return 0xFF;  // refused only while pending(); the loop drains first
    while (p.pending()) {
      uint8_t left = budget;
      if (p.next(left)) {
        if (got.n < sizeof got.types) got.types[got.n++] = p.type();
        ++frames;
      }
    }
  }
  return frames;
}

void test_parser_decodes_golden_turn_with_noise() {
  FrameParser p;
  Got got{};
  const uint8_t noise[] = {0x00, 0x13, 0xFF};
  TEST_ASSERT_EQUAL(0, feedAll(p, noise, sizeof noise, got));
  TEST_ASSERT_EQUAL(0, feedAll(p, kTurn, sizeof kTurn - 1, got));
  TEST_ASSERT_TRUE(p.feed(kTurn[sizeof kTurn - 1]));
  uint8_t budget = 64;
  TEST_ASSERT_TRUE(p.next(budget));
  TEST_ASSERT_EQUAL(63, budget);  // one byte, one step: no replay
  TEST_ASSERT_FALSE(p.pending());
  TEST_ASSERT_EQUAL_HEX8(0x13, p.type());
  TEST_ASSERT_EQUAL(7, p.len());
  proto::TurnMsg m{};
  TEST_ASSERT_TRUE(proto::decode(p.payload(), p.len(), m));
  TEST_ASSERT_EQUAL(-900, m.angle);
  TEST_ASSERT_FALSE(p.next(budget));
}

void test_parser_bad_crc_then_good() {
  FrameParser p;
  uint8_t bad[sizeof kTurn];
  memcpy(bad, kTurn, sizeof kTurn);
  bad[sizeof bad - 1] ^= 0xFF;  // no 0xAA after the sync, so nothing to replay
  Got got{};
  TEST_ASSERT_EQUAL(0, feedAll(p, bad, sizeof bad, got));
  TEST_ASSERT_EQUAL(1, feedAll(p, kTurn, sizeof kTurn, got));
  TEST_ASSERT_EQUAL_HEX8(0x13, got.types[0]);
  TEST_ASSERT_EQUAL(1, p.errors().bad_crc);
}

void test_parser_rejects_long_len() {
  FrameParser p;
  const uint8_t data[] = {0xAA, 49, 0x10, 0x00};
  Got got{};
  TEST_ASSERT_EQUAL(0, feedAll(p, data, sizeof data, got));
  TEST_ASSERT_EQUAL(1, p.errors().bad_len);
}

// Resync tests mirror protocol/generator/tests/test_python_runtime.py.
void test_parser_resyncs_after_stray_sync() {
  FrameParser p;
  Got got{};
  const uint8_t stray = proto::kSync;
  feedAll(p, &stray, 1, got);
  TEST_ASSERT_EQUAL(1, feedAll(p, kStop, sizeof kStop, got));  // its 0xAA is a bad len, then a sync
  TEST_ASSERT_EQUAL_HEX8(0x12, got.types[0]);
  TEST_ASSERT_EQUAL(1, p.errors().bad_len);
}

void test_parser_recovers_good_frame_after_truncated_header() {
  const uint8_t junk_a[] = {0xAA, 0x05};
  const uint8_t junk_b[] = {0xAA, 0x03, 0x10, 0x00, 0x01};
  const uint8_t* junks[] = {junk_a, junk_b};
  const uint8_t sizes[] = {sizeof junk_a, sizeof junk_b};
  for (uint8_t j = 0; j < 2; ++j) {
    FrameParser p;
    Got got{};
    // The false header waits for its claimed length, so the stream continues past it: the
    // trailing frames complete the false one, then the replay recovers every good frame.
    feedAll(p, junks[j], sizes[j], got);
    for (uint8_t k = 0; k < 3; ++k) feedAll(p, kStop, sizeof kStop, got);
    TEST_ASSERT_EQUAL(3, got.n);
    for (uint8_t k = 0; k < 3; ++k) TEST_ASSERT_EQUAL_HEX8(0x12, got.types[k]);
    TEST_ASSERT_EQUAL(1, p.errors().bad_crc);
  }
}

void test_parser_noise_with_many_syncs_then_good_frames() {
  FrameParser p;
  Got got{};
  const uint8_t s = proto::kSync;
  const uint8_t noise[] = {s, s, 0x00, s, proto::kMaxPayload + 1, s, 0x02, s};
  feedAll(p, noise, sizeof noise, got);
  feedAll(p, kStop, sizeof kStop, got);
  feedAll(p, kTurn, sizeof kTurn, got);
  feedAll(p, kStop, sizeof kStop, got);
  TEST_ASSERT_EQUAL(3, got.n);
  TEST_ASSERT_EQUAL_HEX8(0x12, got.types[0]);
  TEST_ASSERT_EQUAL_HEX8(0x13, got.types[1]);
  TEST_ASSERT_EQUAL_HEX8(0x12, got.types[2]);
}

void test_parser_one_byte_can_release_two_frames() {
  FrameParser p;
  Got got{};
  // A false header claiming 9 payload bytes swallows exactly two STOP frames (3 + 9 = 12 bytes);
  // the CRC fails on the last byte and the replay completes both frames from that one byte.
  const uint8_t junk[] = {0xAA, 0x09};
  feedAll(p, junk, sizeof junk, got);
  feedAll(p, kStop, sizeof kStop, got);
  TEST_ASSERT_EQUAL(0, feedAll(p, kStop, sizeof kStop - 1, got));
  TEST_ASSERT_EQUAL(2, feedAll(p, &kStop[sizeof kStop - 1], 1, got));
  TEST_ASSERT_EQUAL(1, p.errors().bad_crc);
}

// Replay stays pending across next() calls; feed() refuses a byte while it is pending and the
// replay survives. Same scenario as above, drained one step per call.
void test_parser_pending_replay_survives_across_calls() {
  FrameParser p;
  Got got{};
  const uint8_t junk[] = {0xAA, 0x09};
  feedAll(p, junk, sizeof junk, got);
  feedAll(p, kStop, sizeof kStop, got);
  feedAll(p, kStop, sizeof kStop - 1, got);
  TEST_ASSERT_TRUE(p.feed(kStop[sizeof kStop - 1]));
  uint8_t one = 1;
  TEST_ASSERT_FALSE(p.next(one));  // the CRC byte: mismatch, replay of 11 bytes queued
  TEST_ASSERT_EQUAL(0, one);
  TEST_ASSERT_TRUE(p.pending());
  TEST_ASSERT_FALSE(p.feed(0x00));  // refused: nothing pending is dropped or overwritten
  TEST_ASSERT_TRUE(p.pending());
  uint8_t frames = 0;
  uint8_t calls = 0;
  while (p.pending()) {
    uint8_t budget = 1;
    if (p.next(budget)) ++frames;
    ++calls;
  }
  TEST_ASSERT_EQUAL(2, frames);
  TEST_ASSERT_EQUAL(11, calls);  // 0xAA..0x24 twice, minus the leading sync already scanned
  TEST_ASSERT_EQUAL(1, p.errors().bad_crc);
}

// Nested false headers (... 2e aa 2c aa 2a aa ...) maximise replay. Expected frames and
// counters come from conqueror_protocol.framing.FrameParser on the same bytes; they must not
// depend on how the step budget splits the work, and no next() call may exceed its budget.
static const uint8_t kAdversarial[] = {
    0x25, 0x58, 0x2a, 0x19, 0x1f, 0xaa, 0x07, 0x44, 0x07, 0xaa, 0xfd, 0xaa, 0x2f, 0x55, 0xaa,
    0x3d, 0x51, 0xaa, 0xad, 0x2a, 0x09, 0x1b, 0xe3, 0xaa, 0xaa, 0x13, 0x64, 0xa9, 0xaa, 0x6d,
    0x44, 0x82, 0xaa, 0xeb, 0xaa, 0xaa, 0x25, 0x21, 0x03, 0x25, 0xfb, 0xaa, 0x30, 0xaa, 0x2e,
    0xaa, 0x2c, 0xaa, 0x2a, 0xaa, 0x28, 0xaa, 0x26, 0xaa, 0x24, 0xaa, 0x22, 0xaa, 0x20, 0xaa,
    0x1e, 0xaa, 0x1c, 0xaa, 0x1a, 0xaa, 0x18, 0xaa, 0x16, 0xaa, 0x14, 0xaa, 0x12, 0xaa, 0x10,
    0xaa, 0x0e, 0xaa, 0x0c, 0xaa, 0x0a, 0xaa, 0x08, 0xaa, 0x06, 0xaa, 0x04, 0xaa, 0x02, 0xaa,
    0x00, 0xaa, 0x58, 0xaa, 0xaa, 0x60, 0x64, 0xaa, 0x1e, 0x00, 0x3e, 0xf5, 0x8a, 0x9d, 0xaa,
    0xe8, 0x29, 0x32, 0x0d, 0xe3, 0x1c, 0xaa, 0x1e, 0x11, 0x45, 0x1d, 0x0c, 0xaa, 0x38, 0x06};

void test_parser_adversarial_stream_matches_reference_under_any_budget() {
  const uint8_t budgets[] = {1, 7, 64, 255};
  for (uint8_t budget : budgets) {
    FrameParser p;
    Got got{};
    uint32_t max_one_byte = 0;  // steps spent on one received byte, replay included
    for (uint8_t i = 0; i < sizeof kAdversarial; ++i) {
      const uint32_t before = p.stepsTaken();
      feedAll(p, &kAdversarial[i], 1, got, budget);
      if (p.stepsTaken() - before > max_one_byte) max_one_byte = p.stepsTaken() - before;
    }
    // Good frames after the attack are all recovered, as in the reference.
    for (uint8_t k = 0; k < 3; ++k) {
      feedAll(p, kStop, sizeof kStop, got, budget);
      feedAll(p, kTurn, sizeof kTurn, got, budget);
    }
    feedAll(p, kStop, sizeof kStop, got, budget);
    TEST_ASSERT_EQUAL(7, got.n);
    for (uint8_t k = 0; k < 7; ++k) TEST_ASSERT_EQUAL_HEX8(k % 2 ? 0x13 : 0x12, got.types[k]);
    TEST_ASSERT_EQUAL(31, p.errors().bad_crc);
    TEST_ASSERT_EQUAL(12, p.errors().bad_len);
    TEST_ASSERT_LESS_OR_EQUAL(700, max_one_byte);  // the proven per-byte bound
    TEST_ASSERT_TRUE(max_one_byte > 255);          // so a budget really does split it
  }
}

void test_parser_next_never_exceeds_its_budget() {
  FrameParser p;
  for (uint8_t i = 0; i < sizeof kAdversarial; ++i) {
    TEST_ASSERT_TRUE(p.feed(kAdversarial[i]));
    while (p.pending()) {
      const uint8_t given = static_cast<uint8_t>(1 + (i % 16));
      uint8_t left = given;
      const uint32_t before = p.stepsTaken();
      p.next(left);
      const uint32_t used = p.stepsTaken() - before;
      TEST_ASSERT_LESS_OR_EQUAL(given, used);
      TEST_ASSERT_EQUAL(given - left, used);  // the budget accounts for every step taken
      TEST_ASSERT_TRUE(used > 0);             // progress while pending
    }
  }
}

void test_writer_matches_golden_ack() {
  FrameWriter w;
  w.setSeq(3);
  proto::AckMsg a{};
  a.ack_seq = 7;
  a.status = 0;
  uint8_t payload[proto::kMaxPayload];
  const uint8_t n = proto::encode(a, payload);
  uint8_t out[proto::kMaxPayload + 5];
  const uint8_t total = w.build(static_cast<uint8_t>(proto::FrameType::kAck), payload, n, out);
  TEST_ASSERT_EQUAL(sizeof kAck, total);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(kAck, out, total);
  TEST_ASSERT_EQUAL(3, w.lastSeq());
}

void test_event_queue_overflow_counts_drops() {
  EventQueue q;
  for (int i = 0; i < 10; ++i) q.push(proto::EventKind::kBump, static_cast<int16_t>(i));
  TEST_ASSERT_EQUAL(2, q.dropped());
  Event e{};
  TEST_ASSERT_TRUE(q.peek(e));  // peek leaves it queued
  TEST_ASSERT_EQUAL(0, e.a);
  TEST_ASSERT_TRUE(q.pop(e));
  TEST_ASSERT_EQUAL(0, e.a);
  TEST_ASSERT_TRUE(q.pop(e));
  TEST_ASSERT_EQUAL(1, e.a);
}

void test_reject_limiter() {
  RejectLimiter r;
  TEST_ASSERT_TRUE(r.allow(1, 1000));
  TEST_ASSERT_FALSE(r.allow(1, 1200));
  TEST_ASSERT_TRUE(r.allow(2, 1200));
  TEST_ASSERT_TRUE(r.allow(1, 1500));
}

void test_link_session_alive_and_hello() {
  LinkSession s;
  TEST_ASSERT_FALSE(s.alive(0));
  TEST_ASSERT_TRUE(s.helloDue(0));      // first HELLO immediately
  TEST_ASSERT_FALSE(s.helloDue(100));
  TEST_ASSERT_TRUE(s.helloDue(600));    // every 500 ms until acked
  s.onValidFrame(600);
  s.onHelloAck();
  TEST_ASSERT_TRUE(s.synced());
  TEST_ASSERT_FALSE(s.helloDue(2000));
  TEST_ASSERT_TRUE(s.alive(850));
  TEST_ASSERT_FALSE(s.alive(901));      // 300 ms timeout
  s.onLinkLost();
  TEST_ASSERT_FALSE(s.synced());
  TEST_ASSERT_TRUE(s.helloDue(901));
}

// First contact after boot is not a restore (LINK_RESTORED only after a LINK_LOST).
void test_link_session_first_contact_is_not_a_restore() {
  LinkSession s;
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(0));
  s.onValidFrame(10);
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(10));    // first contact: no event
  s.onHelloAck();
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(300));
  TEST_ASSERT_EQUAL(LinkChange::kLost, s.poll(400));   // 300 ms silence
  TEST_ASSERT_FALSE(s.synced());                       // poll() ran onLinkLost()
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(410));
  s.onValidFrame(500);
  TEST_ASSERT_EQUAL(LinkChange::kRestored, s.poll(500));
  TEST_ASSERT_EQUAL(LinkChange::kNone, s.poll(510));
}

void test_link_session_request_resent_until_acked() {
  LinkSession s;
  uint8_t type = 0;
  s.setRequest(0x96);
  TEST_ASSERT_TRUE(s.requestDue(0, type));
  TEST_ASSERT_EQUAL_HEX8(0x96, type);
  s.onRequestSent(42, 0);
  TEST_ASSERT_FALSE(s.requestDue(100, type));
  TEST_ASSERT_TRUE(s.requestDue(500, type));
  s.onRequestSent(43, 500);
  s.onEspAck(42);  // stale ack
  TEST_ASSERT_TRUE(s.requestDue(1000, type));
  s.onEspAck(43);
  TEST_ASSERT_FALSE(s.requestDue(2000, type));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parser_decodes_golden_turn_with_noise);
  RUN_TEST(test_parser_bad_crc_then_good);
  RUN_TEST(test_parser_rejects_long_len);
  RUN_TEST(test_parser_resyncs_after_stray_sync);
  RUN_TEST(test_parser_recovers_good_frame_after_truncated_header);
  RUN_TEST(test_parser_noise_with_many_syncs_then_good_frames);
  RUN_TEST(test_parser_one_byte_can_release_two_frames);
  RUN_TEST(test_parser_pending_replay_survives_across_calls);
  RUN_TEST(test_parser_adversarial_stream_matches_reference_under_any_budget);
  RUN_TEST(test_parser_next_never_exceeds_its_budget);
  RUN_TEST(test_writer_matches_golden_ack);
  RUN_TEST(test_event_queue_overflow_counts_drops);
  RUN_TEST(test_reject_limiter);
  RUN_TEST(test_link_session_alive_and_hello);
  RUN_TEST(test_link_session_first_contact_is_not_a_restore);
  RUN_TEST(test_link_session_request_resent_until_acked);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_comms`
Expected: FAIL — `domain/comms/event_queue.h: No such file or directory`.

- [ ] **Step 3: Implement the comms domain**

`src/domain/comms/frame_parser.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

// Counters. bad_crc / bad_len count rejection events: a replayed byte can be rejected again
// and is then counted again.
struct ParseErrors {
  uint16_t bad_crc;
  uint16_t bad_len;
  uint16_t unknown_type;
  uint16_t tx_dropped;
};

// Byte-at-a-time parser for 0xAA | len | type | seq | payload | crc8, resyncing like the Plan A
// reference (conqueror_protocol.framing.FrameParser):
//  - a length byte > kMaxPayload that is itself 0xAA is taken as a new sync (stay in kLen);
//  - after a CRC mismatch the bytes received after the false sync are rescanned for the next
//    0xAA and replayed, so a good frame hidden behind a bad one is not lost.
// One buffer holds the raw bytes after sync (len, type, seq, payload, crc; at most 52); bytes
// waiting to be replayed live in the same buffer, behind the write position. The frame accessors
// read that buffer (dispatch by reference, no copy) and are valid only until the next
// feed()/next() call.
//
// Work is resumable and bounded by the caller. One received byte can cost up to ~700 parser
// steps and ~26 CRC passes (~676 CRC bytes) of replay, so next() runs at most `budget` steps
// (decremented in place) and keeps the rest pending for the next call:
//   if (!parser.pending()) parser.feed(b);           // feed() only when nothing is pending
//   if (parser.next(budget)) dispatch(parser);       // repeat while budget && pending()
// feed() never discards pending replay bytes: while pending() it refuses the byte (returns
// false) and changes nothing. Same frames, order and counters as the Python parser for any
// stream and any budget split.
class FrameParser {
 public:
  static constexpr uint8_t kRawMax = proto::kMaxPayload + 4;  // len, type, seq, payload, crc

  bool feed(uint8_t b);           // queue one received byte; false (byte refused) if pending()
  bool next(uint8_t& budget);     // run <= budget steps (decremented); true when a frame is ready
  bool pending() const { return m_rd < m_end; }  // replay or fed bytes still to be stepped
  uint8_t len() const { return m_raw[0]; }
  uint8_t type() const { return m_raw[1]; }
  uint8_t seq() const { return m_raw[2]; }
  const uint8_t* payload() const { return m_raw + 3; }
  ParseErrors& errors() { return m_errors; }
#ifdef CQ_HOST_TEST
  uint32_t stepsTaken() const { return m_steps; }  // host-test instrumentation only
#endif

 private:
  enum class State : uint8_t { kSync, kLen, kType, kSeq, kPayload, kCrc };
  bool step(uint8_t b);
  void resync();

  State m_state = State::kSync;
  uint8_t m_n = 0;    // raw bytes stored since the (possibly false) sync
  uint8_t m_rd = 0;   // next byte to step
  uint8_t m_end = 0;  // end of the bytes waiting to be stepped; while pending, m_n <= m_rd
  uint8_t m_raw[kRawMax] = {};
  ParseErrors m_errors{};
#ifdef CQ_HOST_TEST
  uint32_t m_steps = 0;
#endif
};

}  // namespace cq
```

`src/domain/comms/frame_parser.cpp`:
```cpp
#include "domain/comms/frame_parser.h"

#include <string.h>

namespace cq {

bool FrameParser::feed(uint8_t b) {
  // Appending behind a replay could need a 53rd slot, so feed() is only legal when nothing is
  // pending; a refused byte leaves the replay untouched (the caller drains first).
  if (pending()) return false;
  // At rest m_n <= 51 (a stored 52nd byte is always a CRC byte, which resets m_n).
  m_raw[m_n] = b;  // the slot it will be stored in anyway
  m_rd = m_n;
  m_end = static_cast<uint8_t>(m_n + 1);
  return true;
}

bool FrameParser::next(uint8_t& budget) {
  // Each step consumes one pending byte; each resync shrinks (stored + pending, <= kRawMax) by
  // at least one, so a received byte costs at most ~700 steps and ~26 CRC passes in total.
  while (budget != 0 && m_rd < m_end) {
    --budget;
#ifdef CQ_HOST_TEST
    ++m_steps;
#endif
    if (step(m_raw[m_rd++])) return true;
  }
  return false;
}

bool FrameParser::step(uint8_t b) {
  if (m_state == State::kSync) {
    if (b == proto::kSync) {
      m_state = State::kLen;
      m_n = 0;
    }
    return false;
  }
  m_raw[m_n++] = b;  // m_n <= m_rd, so this never overwrites a byte still waiting to be stepped
  switch (m_state) {
    case State::kLen:
      if (b > proto::kMaxPayload) {
        ++m_errors.bad_len;
        if (b != proto::kSync) m_state = State::kSync;  // 0xAA: a fresh sync, stay in kLen
        m_n = 0;
        return false;
      }
      m_state = State::kType;
      return false;
    case State::kType:
      m_state = State::kSeq;
      return false;
    case State::kSeq:
      m_state = m_raw[0] ? State::kPayload : State::kCrc;
      return false;
    case State::kPayload:
      if (m_n == 3 + m_raw[0]) m_state = State::kCrc;
      return false;
    case State::kCrc:
      m_state = State::kSync;
      if (proto::crc8(m_raw, static_cast<uint8_t>(m_n - 1)) == b) {
        m_n = 0;  // the frame stays readable in m_raw until the next feed()/next()
        return true;
      }
      ++m_errors.bad_crc;
      resync();
      return false;
    case State::kSync:
      break;
  }
  return false;
}

void FrameParser::resync() {
  uint8_t k = 0;
  while (k < m_n && m_raw[k] != proto::kSync) ++k;
  if (k < m_n) {
    // Step m_raw[k+1 .. m_n) next, then the still-pending tail; move the tail down to follow.
    const uint8_t tail = static_cast<uint8_t>(m_end - m_rd);
    memmove(m_raw + m_n, m_raw + m_rd, tail);
    m_end = static_cast<uint8_t>(m_n + tail);
    m_rd = static_cast<uint8_t>(k + 1);
    m_state = State::kLen;
  }
  m_n = 0;
}

}  // namespace cq
```

`src/domain/comms/frame_writer.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

class FrameWriter {
 public:
  // Writes sync, len, type, seq, payload, crc into `out`; returns the total length (len + 5).
  uint8_t build(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t* out);
  void setSeq(uint8_t seq) { m_seq = seq; }
  uint8_t lastSeq() const { return m_last_seq; }

 private:
  uint8_t m_seq = 0;
  uint8_t m_last_seq = 0;
};

}  // namespace cq
```

`src/domain/comms/frame_writer.cpp`:
```cpp
#include "domain/comms/frame_writer.h"

#include "generated/cq_protocol.h"

namespace cq {

uint8_t FrameWriter::build(uint8_t type, const uint8_t* payload, uint8_t len, uint8_t* out) {
  m_last_seq = m_seq;
  out[0] = proto::kSync;
  out[1] = len;
  out[2] = type;
  out[3] = m_seq++;
  for (uint8_t i = 0; i < len; ++i) out[4 + i] = payload[i];
  out[4 + len] = proto::crc8(out + 1, static_cast<uint8_t>(len + 3));
  return static_cast<uint8_t>(len + 5);
}

}  // namespace cq
```

`src/domain/comms/event_queue.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

struct Event {
  uint8_t kind;
  int16_t a;
  int16_t b;
};

class EventQueue {
 public:
  bool push(proto::EventKind kind, int16_t a = 0, int16_t b = 0);
  bool peek(Event& out) const;  // oldest event, not removed
  bool pop(Event& out);
  bool empty() const { return m_count == 0; }
  uint16_t dropped() const { return m_dropped; }

 private:
  static constexpr uint8_t kSize = 8;
  Event m_ring[kSize] = {};
  uint8_t m_head = 0;
  uint8_t m_count = 0;
  uint16_t m_dropped = 0;
};

class AckQueue {
 public:
  bool push(uint8_t seq, proto::AckStatus status);
  bool peek(uint8_t& seq, proto::AckStatus& status) const;  // oldest ACK, not removed
  bool pop(uint8_t& seq, proto::AckStatus& status);

 private:
  static constexpr uint8_t kSize = 4;
  uint8_t m_seq[kSize] = {};
  proto::AckStatus m_status[kSize] = {};
  uint8_t m_head = 0;
  uint8_t m_count = 0;
};

// At most one rejection event per 500 ms per source (0 none, 1 human, 2 ai).
class RejectLimiter {
 public:
  bool allow(uint8_t source, uint32_t now_ms);

 private:
  uint32_t m_last_ms[3] = {};
  bool m_seen[3] = {};
};

}  // namespace cq
```

`src/domain/comms/event_queue.cpp`:
```cpp
#include "domain/comms/event_queue.h"

#include "domain/util/math_util.h"

namespace cq {

bool EventQueue::push(proto::EventKind kind, int16_t a, int16_t b) {
  if (m_count == kSize) {
    ++m_dropped;
    return false;
  }
  const uint8_t idx = static_cast<uint8_t>((m_head + m_count) % kSize);
  m_ring[idx] = Event{static_cast<uint8_t>(kind), a, b};
  ++m_count;
  return true;
}

bool EventQueue::peek(Event& out) const {
  if (m_count == 0) return false;
  out = m_ring[m_head];
  return true;
}

bool EventQueue::pop(Event& out) {
  if (m_count == 0) return false;
  out = m_ring[m_head];
  m_head = static_cast<uint8_t>((m_head + 1) % kSize);
  --m_count;
  return true;
}

bool AckQueue::push(uint8_t seq, proto::AckStatus status) {
  if (m_count == kSize) return false;
  const uint8_t idx = static_cast<uint8_t>((m_head + m_count) % kSize);
  m_seq[idx] = seq;
  m_status[idx] = status;
  ++m_count;
  return true;
}

bool AckQueue::peek(uint8_t& seq, proto::AckStatus& status) const {
  if (m_count == 0) return false;
  seq = m_seq[m_head];
  status = m_status[m_head];
  return true;
}

bool AckQueue::pop(uint8_t& seq, proto::AckStatus& status) {
  if (m_count == 0) return false;
  seq = m_seq[m_head];
  status = m_status[m_head];
  m_head = static_cast<uint8_t>((m_head + 1) % kSize);
  --m_count;
  return true;
}

bool RejectLimiter::allow(uint8_t source, uint32_t now_ms) {
  if (source > 2) source = 0;
  if (m_seen[source] && !elapsed(now_ms, m_last_ms[source], 500)) return false;
  m_seen[source] = true;
  m_last_ms[source] = now_ms;
  return true;
}

}  // namespace cq
```

`src/domain/comms/link_session.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

enum class LinkChange : uint8_t { kNone, kLost, kRestored };

// Link liveness (300 ms), HELLO handshake (every 500 ms until HELLO_ACK) and one
// Uno-originated request (WIFI/ADMIN reset) resent every 500 ms until ESP_ACK.
class LinkSession {
 public:
  static constexpr uint16_t kLinkTimeoutMs = 300;
  static constexpr uint16_t kRepeatMs = 500;

  // Link watch (every 10 ms): reports alive/dead edges and runs onLinkLost() on a loss.
  // The first contact after boot is not a restore: LINK_RESTORED only follows a LINK_LOST.
  LinkChange poll(uint32_t now_ms);

  void onValidFrame(uint32_t now_ms) {
    m_last_rx_ms = now_ms;
    m_seen = true;
  }
  bool alive(uint32_t now_ms) const;
  void onLinkLost() {
    m_synced = false;
    m_hello_due = true;
  }
  void onHelloAck() { m_synced = true; }
  void onHelloReq() { m_hello_due = true; }
  bool synced() const { return m_synced; }
  bool helloDue(uint32_t now_ms);

  void setRequest(uint8_t frame_type) {
    m_req_type = frame_type;
    m_req_sent = false;
  }
  bool requestDue(uint32_t now_ms, uint8_t& frame_type);
  void onRequestSent(uint8_t seq, uint32_t now_ms) {
    m_req_seq = seq;
    m_req_last_ms = now_ms;
    m_req_sent = true;
  }
  void onEspAck(uint8_t ack_seq) {
    if (m_req_type != 0 && m_req_sent && ack_seq == m_req_seq) m_req_type = 0;
  }

 private:
  uint32_t m_last_rx_ms = 0;
  bool m_seen = false;
  bool m_was_alive = false;   // alive at the previous poll()
  bool m_ever_alive = false;  // any contact since boot
  bool m_synced = false;
  bool m_hello_due = true;
  uint32_t m_last_hello_ms = 0;
  uint8_t m_req_type = 0;
  uint8_t m_req_seq = 0;
  bool m_req_sent = false;
  uint32_t m_req_last_ms = 0;
};

}  // namespace cq
```

`src/domain/comms/link_session.cpp`:
```cpp
#include "domain/comms/link_session.h"

#include "domain/util/math_util.h"

namespace cq {

bool LinkSession::alive(uint32_t now_ms) const {
  return m_seen && !elapsed(now_ms, m_last_rx_ms, static_cast<uint32_t>(kLinkTimeoutMs) + 1);
}

LinkChange LinkSession::poll(uint32_t now_ms) {
  const bool now_alive = alive(now_ms);
  const bool was_alive = m_was_alive;
  m_was_alive = now_alive;
  if (was_alive && !now_alive) {
    onLinkLost();
    return LinkChange::kLost;
  }
  if (!was_alive && now_alive) {
    const bool first_contact = !m_ever_alive;
    m_ever_alive = true;
    return first_contact ? LinkChange::kNone : LinkChange::kRestored;
  }
  return LinkChange::kNone;
}

bool LinkSession::helloDue(uint32_t now_ms) {
  if (m_hello_due || (!m_synced && elapsed(now_ms, m_last_hello_ms, kRepeatMs))) {
    m_hello_due = false;
    m_last_hello_ms = now_ms;
    return true;
  }
  return false;
}

bool LinkSession::requestDue(uint32_t now_ms, uint8_t& frame_type) {
  if (m_req_type == 0) return false;
  if (m_req_sent && !elapsed(now_ms, m_req_last_ms, kRepeatMs)) return false;
  frame_type = m_req_type;
  return true;
}

}  // namespace cq
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_comms`
Expected: `16 Tests 0 Failures`.

- [ ] **Step 5: Link port and UART driver**

`src/ports/link.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace link {
void begin(uint32_t baud);
bool readByte(uint8_t& b);
bool write(const uint8_t* data, uint8_t len);  // false = no room for the whole frame
uint8_t txFree();
}  // namespace link
}  // namespace port
}  // namespace cq
```

`src/drivers/link_uno.cpp`:
```cpp
#include <Arduino.h>

#include "ports/link.h"

namespace cq {
namespace port {
namespace link {

void begin(uint32_t baud) { Serial.begin(baud); }

bool readByte(uint8_t& b) {
  const int v = Serial.read();
  if (v < 0) return false;
  b = static_cast<uint8_t>(v);
  return true;
}

uint8_t txFree() {
  const int n = Serial.availableForWrite();
  return n > 255 ? 255 : static_cast<uint8_t>(n);
}

bool write(const uint8_t* data, uint8_t len) {
  if (txFree() < len) return false;
  Serial.write(data, len);
  return true;
}

}  // namespace link
}  // namespace port
}  // namespace cq
```

- [ ] **Step 6: App context, routing, RX pump, TX scheduler**

`src/app/app.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/comms/event_queue.h"
#include "domain/comms/frame_parser.h"
#include "domain/comms/frame_writer.h"
#include "domain/comms/link_session.h"
#include "domain/util/scheduler.h"
#include "generated/cq_protocol.h"

namespace cq {

// Composition-root state shared by app tasks. Members are added by later tasks; each module
// writes its own telemetry fields into `fast`/`slow`, the TX scheduler only stamps and sends.
struct App {
  // comms
  FrameParser parser;
  FrameWriter writer;
  EventQueue events;
  AckQueue acks;
  RejectLimiter rejects;
  LinkSession link;
  // telemetry staging
  proto::TelemFastMsg fast{};
  proto::TelemSlowMsg slow{};
  bool fast_due = false;
  bool slow_due = false;
  proto::CalDataMsg cal_data{};
  bool cal_data_pending = false;
  proto::CalResultMsg cal_result{};
  bool cal_result_pending = false;
  proto::SettingValueMsg setting_value{};
  bool setting_value_pending = false;
  // health
  LoadMeter load;
  uint16_t scheduler_overruns = 0;
  proto::ResetReason reset_reason = proto::ResetReason::kUnknown;
  bool encoders_enabled = false;
};

extern App g_app;

}  // namespace cq
```

`src/app/comms.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace app {

struct Route {
  uint8_t type;
  proto::AckStatus (*handler)(const uint8_t* payload, uint8_t len);
  bool needs_ack;
};

extern const Route kRoutes[];
extern const uint8_t kRouteCount;

void commsPoll();   // every loop: RX bytes → parser → route → ACK queue
void txPump();      // every loop: ACK > requests > EVENT > HELLO > CAL > FAST > SLOW
void telemetryFastDue();  // 20 Hz task: marks fast telemetry due
void telemetrySlowDue();  // 1 Hz task: fills health fields, marks slow due
uint16_t freeRam();

}  // namespace app
}  // namespace cq
```

`src/app/routes.cpp` (link-level routes; later tasks append rows and handlers):
```cpp
#include <avr/pgmspace.h>

#include "app/app.h"
#include "app/comms.h"
#include "ports/clock.h"

namespace cq {
namespace app {
namespace {

proto::AckStatus onHeartbeat(const uint8_t* p, uint8_t len) {
  proto::HeartbeatMsg m{};
  return proto::decode(p, len, m) ? proto::AckStatus::kOk : proto::AckStatus::kInvalidCommand;
}

proto::AckStatus onHelloAck(const uint8_t* p, uint8_t len) {
  proto::HelloAckMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.link.onHelloAck();
  return proto::AckStatus::kOk;
}

proto::AckStatus onHelloReq(const uint8_t* p, uint8_t len) {
  proto::HelloReqMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.link.onHelloReq();
  return proto::AckStatus::kOk;
}

proto::AckStatus onEspAck(const uint8_t* p, uint8_t len) {
  proto::EspAckMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.link.onEspAck(m.ack_seq);
  return proto::AckStatus::kOk;
}

}  // namespace

// ROUTES-BEGIN (later tasks add rows before ROUTES-END)
const Route kRoutes[] PROGMEM = {
    {static_cast<uint8_t>(proto::FrameType::kHeartbeat), onHeartbeat, false},
    {static_cast<uint8_t>(proto::FrameType::kHelloAck), onHelloAck, false},
    {static_cast<uint8_t>(proto::FrameType::kHelloReq), onHelloReq, false},
    {static_cast<uint8_t>(proto::FrameType::kEspAck), onEspAck, false},
    // ROUTES-END
};
const uint8_t kRouteCount = sizeof kRoutes / sizeof kRoutes[0];

}  // namespace app
}  // namespace cq
```

`src/app/comms.cpp`:
```cpp
#include <avr/pgmspace.h>
#include <string.h>

#include "app/app.h"
#include "app/comms.h"
#include "drivers/board_config.h"
#include "ports/clock.h"
#include "ports/link.h"

// avr-libc heap symbols. Declared at global scope with C linkage: a block-scope `extern` inside
// cq::app would name cq::app::__heap_start and fail to link.
extern "C" {
extern int __heap_start;
extern char* __brkval;
}

namespace cq {

App g_app;

namespace app {

// Parser work per loop pass is bounded in parser steps, not received bytes: one hostile byte
// can need ~700 steps and ~26 CRC passes of replay (input reachable through the unauthenticated
// WiFi bridge), so the rest of a replay stays pending in the parser for the next pass.
// Worst case per pass at 16 MHz (host model: ~40 cycles/step, ~70 cycles/CRC byte): CRC bytes in
// one pass <= steps + 51 carried-over stored bytes, so 64 x 40 + (64 + 51) x 70 ~= 10.6 k cycles
// ~= 0.66 ms (adversarial search incl. resync scan/memmove: 10.8 k cycles ~= 0.67 ms) <= ~1 ms.
// Normal traffic costs one step per byte: commands + 10 Hz heartbeats are well under 1 kB/s
// (< 10 bytes per 10 ms tick) against >= 64 bytes per pass; at line rate (25 kB/s) the 64-byte
// RX buffer fills in ~2.5 ms, i.e. one full buffer per pass. Telemetry out (~20 Hz) does not use
// the parser. A flood of crafted bytes overflows the RX buffer instead of stalling the loop;
// the link-loss stop still applies.
constexpr uint8_t kPollSteps = 64;

void commsPoll() {
  uint8_t budget = kPollSteps;
  uint8_t b = 0;
  while (budget != 0) {
    // Finish pending replay work first; read a new byte only when none is left.
    if (!g_app.parser.pending()) {
      if (!port::link::readByte(b)) break;
      g_app.parser.feed(b);  // always accepted: nothing is pending
    }
    if (!g_app.parser.next(budget)) continue;  // next() consumed >= 1 step; the budget bounds the loop
    g_app.link.onValidFrame(port::clock::nowMs());
    const uint8_t type = g_app.parser.type();
    bool routed = false;
    for (uint8_t i = 0; i < kRouteCount; ++i) {
      Route r;
      memcpy_P(&r, &kRoutes[i], sizeof r);
      if (r.type != type) continue;
      routed = true;
      const proto::AckStatus st = r.handler(g_app.parser.payload(), g_app.parser.len());
      if (r.needs_ack) g_app.acks.push(g_app.parser.seq(), st);
      break;
    }
    if (!routed) ++g_app.parser.errors().unknown_type;
  }
}

namespace {

uint8_t g_tx[proto::kMaxPayload + 5];

template <typename Msg>
bool sendMsg(const Msg& m) {
  uint8_t payload[proto::kMaxPayload];
  const uint8_t n = proto::encode(m, payload);
  if (n == proto::kEncodeError) return false;
  const uint8_t total =
      g_app.writer.build(static_cast<uint8_t>(Msg::kType), payload, n, g_tx);
  if (port::link::txFree() < total) {
    g_app.writer.setSeq(g_app.writer.lastSeq());  // reuse the seq next time
    ++g_app.parser.errors().tx_dropped;
    return false;
  }
  return port::link::write(g_tx, total);
}

bool sendRaw(uint8_t type) {
  const uint8_t total = g_app.writer.build(type, nullptr, 0, g_tx);
  if (port::link::txFree() < total) {
    g_app.writer.setSeq(g_app.writer.lastSeq());
    return false;
  }
  return port::link::write(g_tx, total);
}

}  // namespace

void txPump() {
  const uint32_t now = port::clock::nowMs();
  uint8_t seq = 0;
  proto::AckStatus st{};
  // Peek, send, then pop: a full TX buffer leaves the item at the head, so order is kept.
  while (g_app.acks.peek(seq, st)) {
    proto::AckMsg a{};
    a.ack_seq = seq;
    a.status = static_cast<uint8_t>(st);
    if (!sendMsg(a)) return;
    g_app.acks.pop(seq, st);
  }
  uint8_t req = 0;
  if (g_app.link.requestDue(now, req)) {
    if (!sendRaw(req)) return;
    g_app.link.onRequestSent(g_app.writer.lastSeq(), now);
  }
  Event e{};
  while (g_app.events.peek(e)) {
    proto::EventMsg m{};
    m.kind = e.kind;
    m.a = e.a;
    m.b = e.b;
    if (!sendMsg(m)) return;
    g_app.events.pop(e);
  }
  if (g_app.link.helloDue(now)) {
    proto::HelloMsg h{};
    h.fw = board::kFwVersion;
    h.proto = proto::kProtocolVersion;
    h.caps = g_app.encoders_enabled ? 1 : 0;
    h.reset_reason = static_cast<uint8_t>(g_app.reset_reason);
    sendMsg(h);
  }
  if (g_app.setting_value_pending && sendMsg(g_app.setting_value)) g_app.setting_value_pending = false;
  if (g_app.cal_result_pending && sendMsg(g_app.cal_result)) g_app.cal_result_pending = false;
  if (g_app.cal_data_pending && sendMsg(g_app.cal_data)) g_app.cal_data_pending = false;
  if (g_app.fast_due) {
    g_app.fast.uno_ms = now;
    if (sendMsg(g_app.fast)) g_app.fast_due = false;
  }
  if (g_app.slow_due && sendMsg(g_app.slow)) g_app.slow_due = false;
}

void telemetryFastDue() { g_app.fast_due = true; }

void telemetrySlowDue() {
  proto::TelemSlowMsg& s = g_app.slow;
  s.uptime_s = port::clock::nowMs() / 1000u;
  s.reset_reason = static_cast<uint8_t>(g_app.reset_reason);
  s.loop_load = g_app.load.loadPct();
  s.loop_max_us = g_app.load.maxUs();
  s.free_ram = freeRam();
  // bad_crc / bad_len count rejection events; a resync replay can count the same bytes again.
  s.err_frames = static_cast<uint16_t>(g_app.parser.errors().bad_crc + g_app.parser.errors().bad_len +
                                       g_app.parser.errors().unknown_type);
  g_app.slow_due = true;
}

uint16_t freeRam() {
  int top = 0;
  const int free_bytes = ::__brkval == nullptr
                             ? reinterpret_cast<int>(&top) - reinterpret_cast<int>(&::__heap_start)
                             : reinterpret_cast<int>(&top) - reinterpret_cast<int>(::__brkval);
  return free_bytes < 0 ? 0 : static_cast<uint16_t>(free_bytes);
}

}  // namespace app
}  // namespace cq
```

- [ ] **Step 7: Wire comms into main**

Replace `src/main.cpp` with:
```cpp
#include <Arduino.h>

#include "app/app.h"
#include "app/comms.h"
#include "domain/util/scheduler.h"
#include "drivers/board_config.h"
#include "ports/clock.h"
#include "ports/link.h"
#include "ports/watchdog.h"

namespace {

void linkWatchTask() {
  // poll() runs onLinkLost() on a loss; the first contact after boot reports kNone.
  switch (cq::g_app.link.poll(cq::port::clock::nowMs())) {
    case cq::LinkChange::kLost:
      cq::g_app.events.push(cq::proto::EventKind::kLinkLost);
      break;
    case cq::LinkChange::kRestored:
      cq::g_app.events.push(cq::proto::EventKind::kLinkRestored);
      break;
    case cq::LinkChange::kNone:
      break;
  }
}

// TASKS-BEGIN (later tasks add rows before TASKS-END)
cq::Task g_tasks[] = {
    {linkWatchTask, 10000UL, 0},
    {cq::app::telemetryFastDue, 50000UL, 0},
    {cq::app::telemetrySlowDue, 1000000UL, 0},
    // TASKS-END
};
cq::Scheduler g_scheduler(g_tasks, static_cast<uint8_t>(sizeof g_tasks / sizeof g_tasks[0]));

}  // namespace

void setup() {
  cq::g_app.reset_reason = cq::port::watchdog::resetReason();
  cq::port::link::begin(cq::board::kBaud);
  // SETUP-INIT (later tasks add driver/domain init before SETUP-END)
  // SETUP-END
  cq::port::watchdog::enable();
  g_scheduler.start(cq::port::clock::nowUs());
}

void loop() {
  const uint32_t t0 = cq::port::clock::nowUs();
  cq::app::commsPoll();
  const uint8_t ran = g_scheduler.runDue(t0);
  cq::app::txPump();
  const uint32_t t1 = cq::port::clock::nowUs();
  if (ran > 0) cq::g_app.load.add(t1 - t0, t1);
  cq::g_app.scheduler_overruns = g_scheduler.overruns();
  cq::port::watchdog::kick();
}
```

- [ ] **Step 8: Stand smoke script (v1: handshake + watch)**

`tools/smoke/pyproject.toml` (one uv project for all smoke scripts; Plan C adds `bridge_echo.py` here — whichever plan runs first creates this file with exactly this content):
```toml
[project]
name = "conqueror-smoke"
version = "0.1.0"
description = "Stand and bridge smoke scripts (Plans B and C)"
requires-python = ">=3.12"
dependencies = ["conqueror-protocol", "pyserial>=3.5"]

[tool.uv]
package = false

[tool.uv.sources]
conqueror-protocol = { path = "../../protocol/python", editable = true }

[dependency-groups]
dev = ["ruff>=0.7", "pyright>=1.1", "types-pyserial>=3.5"]

[tool.ruff]
line-length = 100
target-version = "py312"

[tool.ruff.lint]
select = ["E", "F", "I", "B", "UP", "SIM", "RUF"]

[tool.pyright]
pythonVersion = "3.12"
typeCheckingMode = "strict"
reportMissingTypeStubs = false
```

`tools/smoke/uno_smoke.py`:
```python
#!/usr/bin/env python3
"""Stand/floor smoke tool for the Uno body firmware (Plan B). Plays the ESP32's role.

Run from tools/smoke/:  uv run python uno_smoke.py --port PORT CMD ...
"""

from __future__ import annotations

import argparse
import threading
import time
from collections.abc import Callable

import serial  # pyserial
from conqueror_protocol import FrameParser, build_frame, decode_frame
from conqueror_protocol import generated as g

UNO = "uno_to_esp32"


class Link:
    def __init__(self, port: str) -> None:
        self.ser = serial.Serial(port, g.BAUD, timeout=0.01)
        self.parser = FrameParser()
        self.seq = 0
        self.lock = threading.Lock()
        self.running = True
        self.handlers: list[Callable[[object], None]] = []
        self.synced = False
        self.last: dict[str, object] = {}
        threading.Thread(target=self._rx, daemon=True).start()
        threading.Thread(target=self._heartbeat, daemon=True).start()

    def send(self, msg: object) -> int:
        with self.lock:
            seq = self.seq
            self.seq = (self.seq + 1) & 0xFF
            self.ser.write(build_frame(msg, seq))  # type: ignore[arg-type]
            return seq

    def _heartbeat(self) -> None:
        while self.running:
            self.send(g.Heartbeat(esp_ms=int(time.monotonic() * 1000) & 0xFFFFFFFF))
            time.sleep(0.1)

    def _rx(self) -> None:
        while self.running:
            data = self.ser.read(256)
            for raw in self.parser.feed(data):
                try:
                    msg = decode_frame(raw, UNO)
                except ValueError as exc:
                    print(f"decode error: {exc}")
                    continue
                self.last[type(msg).__name__] = msg
                if isinstance(msg, g.Hello):
                    print(f"HELLO fw={msg.fw:#06x} proto={msg.proto} caps={msg.caps} "
                          f"reset={g.ResetReason(msg.reset_reason).name}")
                    self.send(g.HelloAck(proto=g.PROTOCOL_VERSION))
                    self.synced = True
                for h in self.handlers:
                    h(msg)

    def close(self) -> None:
        self.send(g.Stop(src=g.Source.HUMAN))
        self.running = False
        time.sleep(0.15)
        self.ser.close()


def fmt_event(m: g.Event) -> str:
    try:
        name = g.EventKind(m.kind).name
    except ValueError:
        name = str(m.kind)
    return f"EVENT {name} a={m.a} b={m.b}"


def cmd_watch(link: Link, args: argparse.Namespace) -> None:
    def show(msg: object) -> None:
        if isinstance(msg, g.TelemFast):
            print(f"FAST yaw={msg.yaw/100:.1f} rate={msg.yaw_rate/10:.1f} "
                  f"pitch={msg.pitch/100:.1f} roll={msg.roll/100:.1f} "
                  f"pwm={msg.pwm_l},{msg.pwm_r} enc={msg.enc_l},{msg.enc_r} "
                  f"dist={msg.dist_mm} flags={msg.flags:#06x} "
                  f"pan={msg.pan/10:.0f} tilt={msg.tilt/10:.0f}")
        elif isinstance(msg, g.TelemSlow):
            print(f"SLOW up={msg.uptime_s}s batt={msg.batt_mv}mV temp={msg.temp/100:.1f}C "
                  f"owner={g.Owner(msg.owner).name} load={msg.loop_load}% max={msg.loop_max_us}us "
                  f"ram={msg.free_ram} errs={msg.err_frames} flags2={msg.flags2:#x}")
        elif isinstance(msg, g.Event):
            print(fmt_event(msg))
        elif isinstance(msg, g.Ack):
            print(f"ACK seq={msg.ack_seq} {g.ACK_STATUS[msg.status]}")
        elif isinstance(msg, (g.WifiResetReq, g.AdminResetReq)):
            print(type(msg).__name__)

    link.handlers.append(show)
    time.sleep(args.seconds)


COMMANDS: dict[str, Callable[[Link, argparse.Namespace], None]] = {"watch": cmd_watch}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("watch")
    w.add_argument("--seconds", type=float, default=10.0)
    args = ap.parse_args()
    link = Link(args.port)
    try:
        COMMANDS[args.cmd](link, args)
    finally:
        link.close()


if __name__ == "__main__":
    main()
```

- [ ] **Step 9: Build, gate, flash, watch**

Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py`
Expected: all native tests pass; `SUCCESS`; gate OK.
Run (from `tools/smoke/`): `uv run ruff check . && uv run pyright`
Expected: `All checks passed!`, `0 errors, 0 warnings, 0 informations`.
S1 → upload, `pio run -e uno -t upload`. Keep USB connected (S1 stays on "upload": the USB adapter now plays the ESP32).
Run (from `tools/smoke/`): `uv run python uno_smoke.py --port /dev/cu.usbmodemXXXX watch --seconds 5` (genuine Uno: `usbmodem…`; CH340 clones: `usbserial…`)
Expected: one `HELLO fw=0x0100 proto=1 caps=0 reset=…`, then `FAST …` lines at ~20 Hz (zeros), `SLOW up=… load=…% ram=…` each second, and **no** `EVENT LINK_RESTORED` (the first contact after boot is not a restore; LINK_RESTORED only follows a LINK_LOST). Stop the script: within 0.3 s the Uno raises `LINK_LOST` (not visible here — reopening the port resets the Uno).

- [ ] **Step 10: Commit**

```bash
git add firmware/uno tools/smoke
git commit -m "feat(uno): body link - parser, writer, queues, link session, TX scheduler, smoke script"
```

---

### Task 3: Settings store, non-blocking EEPROM writer, SETTING routes

**Files:**
- Create: `src/domain/settings/crc16.h`, `settings.{h,cpp}`, `eeprom_writer.{h,cpp}`; `src/ports/eeprom.h`; `src/drivers/eeprom_uno.cpp`; `src/app/tasks.h`, `src/app/settings_task.cpp`, `src/app/apply_settings.cpp`
- Modify: `src/app/app.h` (members), `src/app/routes.cpp` (rows), `src/app/comms.cpp` (`settings_crc` in slow telemetry), `src/main.cpp` (init + task)
- Test: `test/test_settings/test_main.cpp`

**Interfaces:**
- Consumes: `proto::SettingId`, `proto::kSettingDescriptors`, `proto::kSettingCount`, `proto::SettingDescriptor`, `proto::WireType`, `proto::SettingScope`.
- Produces:
  - `struct cq::GlobalSettings` / `cq::ProfileSettings` (packed; field names = settings.yaml names), `class cq::Settings`: `void loadDefaults()`, `bool get(uint8_t id, int32_t& value) const`, `proto::AckStatus set(uint8_t id, int32_t value)` (`kSettingUnknown`; `kSettingOutOfRange` when outside the descriptor range **or** when the value would break an ordering invariant against the current values — `pan_min ≤ pan_max`, `tilt_min ≤ tilt_max`, `obstacle_stop < obstacle_slow`, `batt_crit < batt_low`; the hub orders its writes), `const GlobalSettings& global() const`, `const ProfileSettings& profile() const`, `uint16_t crc() const`, `uint8_t serialize(uint8_t block, uint8_t* out) const`, `bool deserialize(uint8_t block, const uint8_t* data, uint8_t len, uint16_t stored_crc)`, `static constexpr uint8_t kBlockGlobal = 0, kBlockProfile = 1`, `bool dirty() const`, `void clearDirty()`.
  - `cq::crc16(const uint8_t* data, uint16_t len, uint16_t crc = 0xFFFF) -> uint16_t` (CCITT-FALSE).
  - `struct cq::EepromIo { uint8_t (*read)(uint16_t); bool (*writeAsync)(uint16_t, uint8_t); bool (*busy)(); }`, `class cq::EepromWriter`: `bool enqueue(uint16_t addr, const uint8_t* data, uint8_t len)`, `bool step(const EepromIo& io)` (≤ 1 byte written; returns true when idle), `bool idle() const`.
  - EEPROM layout: `kEeMagicAddr = 0` (`'C','Q'`, layout version 1); global block at 8: `[len][data…][crc lo][crc hi]`; profile block at 80.
  - Port `cq::port::eeprom`: `uint8_t read(uint16_t addr)`, `bool writeByteAsync(uint16_t addr, uint8_t v)`, `bool busy()`.
  - App members `Settings settings; EepromWriter eewriter; bool save_requested; bool save_in_progress; bool moving;` (`settings_reset_mask` comes in Task 7) and functions `cq::app::settingsLoad()`, `cq::app::settingsTask()`, `cq::app::applySettings()` (applies values to domain objects — extended by later tasks), declared in the new `src/app/tasks.h` — the one header for app task/init entry points (later tasks add their declarations before `// TASK-DECLS-END`).

- [ ] **Step 1: Write the failing settings tests**

`test/test_settings/test_main.cpp`:
```cpp
#include <string.h>
#include <unity.h>

#include "domain/settings/crc16.h"
#include "domain/settings/eeprom_writer.h"
#include "domain/settings/settings.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_defaults_match_descriptors() {
  Settings s;
  s.loadDefaults();
  int32_t v = 0;
  TEST_ASSERT_TRUE(s.get(static_cast<uint8_t>(proto::SettingId::kDeadman), v));
  TEST_ASSERT_EQUAL(300, v);
  TEST_ASSERT_TRUE(s.get(static_cast<uint8_t>(proto::SettingId::kCountsPerRev), v));
  TEST_ASSERT_EQUAL(960, v);
  TEST_ASSERT_TRUE(s.get(static_cast<uint8_t>(proto::SettingId::kPidHeadingKp), v));
  TEST_ASSERT_EQUAL(3000, v);
  TEST_ASSERT_EQUAL(300, s.global().deadman);
}

void test_every_descriptor_has_a_slot() {
  Settings s;
  s.loadDefaults();
  for (uint8_t i = 0; i < proto::kSettingCount; ++i) {
    int32_t v = 0;
    TEST_ASSERT_TRUE_MESSAGE(s.get(proto::kSettingDescriptors[i].id, v), "missing slot");
    TEST_ASSERT_EQUAL(proto::kSettingDescriptors[i].def, v);
  }
}

void test_set_validates_range_and_unknown() {
  Settings s;
  s.loadDefaults();
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(s.set(static_cast<uint8_t>(proto::SettingId::kTiltLimit), 40)));
  TEST_ASSERT_EQUAL(40, s.global().tilt_limit);
  TEST_ASSERT_TRUE(s.dirty());
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kSettingOutOfRange),
                    static_cast<int>(s.set(static_cast<uint8_t>(proto::SettingId::kTiltLimit), 99)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kSettingUnknown),
                    static_cast<int>(s.set(200, 1)));
}

// Ordering invariants are checked against the current values; the hub orders its writes.
void test_set_rejects_cross_field_inversions() {
  Settings s;
  s.loadDefaults();  // pan 0..180, tilt 30..110, obstacle stop 15 / slow 50, batt crit 6600 / low 7000
  const auto set = [&s](proto::SettingId id, int32_t v) {
    return static_cast<int>(s.set(static_cast<uint8_t>(id), v));
  };
  const int ok = static_cast<int>(proto::AckStatus::kOk);
  const int range = static_cast<int>(proto::AckStatus::kSettingOutOfRange);
  TEST_ASSERT_EQUAL(ok, set(proto::SettingId::kPanMax, 90));
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kPanMin, 91));   // min above max
  TEST_ASSERT_EQUAL(ok, set(proto::SettingId::kPanMin, 90));      // equal is allowed
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kPanMax, 89));   // max below min
  TEST_ASSERT_EQUAL(90, s.global().pan_max);                      // rejected value not stored
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kTiltMax, 29));
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kTiltMin, 111));
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kObstacleSlow, 15));  // slow must be > stop
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kObstacleStop, 50));
  TEST_ASSERT_EQUAL(ok, set(proto::SettingId::kObstacleStop, 49));
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kBattCrit, 7000));    // crit must be < low
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kBattLow, 6600));
  TEST_ASSERT_EQUAL(ok, set(proto::SettingId::kBattLow, 6601));
}

void test_serialize_roundtrip_and_bad_crc_loads_defaults() {
  Settings a;
  a.loadDefaults();
  a.set(static_cast<uint8_t>(proto::SettingId::kPanTrim), -25);
  uint8_t buf[96];
  const uint8_t n = a.serialize(Settings::kBlockGlobal, buf);
  const uint16_t crc = crc16(buf, n);
  Settings b;
  b.loadDefaults();
  TEST_ASSERT_TRUE(b.deserialize(Settings::kBlockGlobal, buf, n, crc));
  TEST_ASSERT_EQUAL(-25, b.global().pan_trim);
  Settings c;
  c.loadDefaults();
  TEST_ASSERT_FALSE(c.deserialize(Settings::kBlockGlobal, buf, n, static_cast<uint16_t>(crc ^ 1)));
  TEST_ASSERT_EQUAL(0, c.global().pan_trim);  // defaults kept
}

void test_crc16_check_value() {
  const uint8_t msg[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16(msg, 9));  // CRC-16/CCITT-FALSE
}

namespace {
uint8_t g_mem[1024];
bool g_busy = false;
int g_writes = 0;
uint8_t fakeRead(uint16_t a) { return g_mem[a]; }
bool fakeWrite(uint16_t a, uint8_t v) {
  if (g_busy) return false;
  g_mem[a] = v;
  ++g_writes;
  g_busy = true;
  return true;
}
bool fakeBusy() { return g_busy; }
}  // namespace

void test_writer_one_byte_per_step() {
  memset(g_mem, 0xFF, sizeof g_mem);
  g_writes = 0;
  EepromWriter w;
  const uint8_t data[] = {1, 2, 3, 0xFF};
  TEST_ASSERT_TRUE(w.enqueue(10, data, 4));
  const EepromIo io{fakeRead, fakeWrite, fakeBusy};
  int steps = 0;
  while (!w.idle() && steps < 50) {
    w.step(io);
    g_busy = false;  // write completes between scheduler passes
    ++steps;
    TEST_ASSERT_TRUE(g_writes <= steps);
  }
  TEST_ASSERT_EQUAL(3, g_writes);  // unchanged 0xFF byte skipped
  TEST_ASSERT_EQUAL(1, g_mem[10]);
  TEST_ASSERT_EQUAL(3, g_mem[12]);
}

void test_writer_waits_while_busy() {
  memset(g_mem, 0, sizeof g_mem);
  g_writes = 0;
  EepromWriter w;
  const uint8_t data[] = {7};
  w.enqueue(0, data, 1);
  g_busy = true;
  const EepromIo io{fakeRead, fakeWrite, fakeBusy};
  w.step(io);
  TEST_ASSERT_EQUAL(0, g_writes);
  g_busy = false;
  w.step(io);
  TEST_ASSERT_EQUAL(1, g_writes);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defaults_match_descriptors);
  RUN_TEST(test_every_descriptor_has_a_slot);
  RUN_TEST(test_set_validates_range_and_unknown);
  RUN_TEST(test_set_rejects_cross_field_inversions);
  RUN_TEST(test_serialize_roundtrip_and_bad_crc_loads_defaults);
  RUN_TEST(test_crc16_check_value);
  RUN_TEST(test_writer_one_byte_per_step);
  RUN_TEST(test_writer_waits_while_busy);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_settings`
Expected: FAIL — `domain/settings/crc16.h: No such file or directory`.

- [ ] **Step 3: Implement CRC-16, settings and the writer**

`src/domain/settings/crc16.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF); check("123456789") = 0x29B1.
inline uint16_t crc16(const uint8_t* data, uint16_t len, uint16_t crc = 0xFFFF) {
  for (uint16_t i = 0; i < len; ++i) {
    crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

}  // namespace cq
```

`src/domain/settings/settings.h`:
```cpp
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

// Field names and types mirror protocol/defs/settings.yaml (fixed-point integers).
struct __attribute__((packed)) GlobalSettings {
  uint8_t encoders_enabled;
  uint16_t counts_per_rev;
  uint16_t counts_per_cm;      // ×0.01
  uint16_t motor_v_max;        // mV
  uint16_t accel_limit;        // %/s
  uint16_t turn_rate_max;      // °/s
  uint8_t pivot_fallback;
  uint8_t reverse_steer;       // 0 car, 1 tank
  uint8_t obstacle_stop;       // cm
  uint8_t obstacle_slow;       // cm
  uint8_t tilt_limit;          // °
  uint16_t bump_threshold;     // mg
  uint16_t deadman;            // ms
  uint16_t batt_low;           // mV
  uint16_t batt_crit;          // mV
  uint16_t batt_mv_per_count;  // ×0.01 mV
  uint16_t ir_handback;        // ms
  uint8_t parking_beeps;
  uint16_t cam_return_delay;   // ms
  uint8_t servo_idle_detach;
  uint8_t pan_min, pan_max, tilt_min, tilt_max;  // °
  int16_t pan_trim, tilt_trim;                   // ×0.1 °
  uint16_t servo_speed;        // °/s
  uint16_t servo_accel;        // °/s²
  int16_t gyro_bias_x, gyro_bias_y, gyro_bias_z;  // raw
  int16_t level_pitch, level_roll;                // ×0.01 °
  uint8_t ultrasonic_offset;   // mm
  uint8_t active_profile;
};

struct __attribute__((packed)) ProfileSettings {
  uint8_t turn_rate_full_speed;  // %
  uint16_t track_width_eff;      // mm
  uint8_t deadband_start[4];     // l_fwd, l_rev, r_fwd, r_rev
  uint8_t deadband_keep[4];
  uint16_t max_cps_l, max_cps_r;
  int32_t pid_speed_kp, pid_speed_ki, pid_speed_kd;      // ×0.001
  int32_t pid_rate_kp, pid_rate_ki, pid_rate_kd;
  int32_t pid_heading_kp, pid_heading_ki, pid_heading_kd;
};

class Settings {
 public:
  static constexpr uint8_t kBlockGlobal = 0;
  static constexpr uint8_t kBlockProfile = 1;

  void loadDefaults();
  bool get(uint8_t id, int32_t& value) const;
  proto::AckStatus set(uint8_t id, int32_t value);
  const GlobalSettings& global() const { return m_global; }
  const ProfileSettings& profile() const { return m_profile; }
  uint8_t serialize(uint8_t block, uint8_t* out) const;
  bool deserialize(uint8_t block, const uint8_t* data, uint8_t len, uint16_t stored_crc);
  uint16_t crc() const;
  bool dirty() const { return m_dirty; }
  void clearDirty() { m_dirty = false; }

 private:
  bool slot(uint8_t id, uint8_t*& ptr, proto::WireType& type, proto::SettingDescriptor& desc) const;
  bool keepsOrder(uint8_t id, int32_t value) const;  // ordering invariants vs current values
  GlobalSettings m_global{};
  ProfileSettings m_profile{};
  bool m_dirty = false;
};

}  // namespace cq
```

`src/domain/settings/settings.cpp`:
```cpp
#include "domain/settings/settings.h"

#include <string.h>

#include "domain/settings/crc16.h"

// PROGMEM itself comes from generated/cq_protocol.h (defined empty on host).
#ifdef CQ_HOST_TEST
#define CQ_READ_P(dst, src, n) memcpy((dst), (src), (n))
#else
#include <avr/pgmspace.h>
#define CQ_READ_P(dst, src, n) memcpy_P((dst), (src), (n))
#endif

namespace cq {
namespace {

struct Slot {
  uint8_t id;
  uint8_t block;
  uint8_t offset;
};

#define G(field, sid) {static_cast<uint8_t>(proto::SettingId::sid), Settings::kBlockGlobal, \
                       static_cast<uint8_t>(offsetof(GlobalSettings, field))}
#define P(field, sid) {static_cast<uint8_t>(proto::SettingId::sid), Settings::kBlockProfile, \
                       static_cast<uint8_t>(offsetof(ProfileSettings, field))}

const Slot kSlots[] PROGMEM = {
    G(encoders_enabled, kEncodersEnabled), G(counts_per_rev, kCountsPerRev),
    G(counts_per_cm, kCountsPerCm), G(motor_v_max, kMotorVMax), G(accel_limit, kAccelLimit),
    G(turn_rate_max, kTurnRateMax), P(turn_rate_full_speed, kTurnRateFullSpeed),
    P(track_width_eff, kTrackWidthEff), G(pivot_fallback, kPivotFallback),
    G(reverse_steer, kReverseSteer), G(obstacle_stop, kObstacleStop), G(obstacle_slow, kObstacleSlow),
    G(tilt_limit, kTiltLimit), G(bump_threshold, kBumpThreshold), G(deadman, kDeadman),
    G(batt_low, kBattLow), G(batt_crit, kBattCrit), G(batt_mv_per_count, kBattMvPerCount),
    G(ir_handback, kIrHandback), G(parking_beeps, kParkingBeeps),
    G(cam_return_delay, kCamReturnDelay), G(servo_idle_detach, kServoIdleDetach),
    G(pan_min, kPanMin), G(pan_max, kPanMax), G(tilt_min, kTiltMin), G(tilt_max, kTiltMax),
    G(pan_trim, kPanTrim), G(tilt_trim, kTiltTrim), G(servo_speed, kServoSpeed),
    G(servo_accel, kServoAccel), G(gyro_bias_x, kGyroBiasX), G(gyro_bias_y, kGyroBiasY),
    G(gyro_bias_z, kGyroBiasZ), G(level_pitch, kLevelPitch), G(level_roll, kLevelRoll),
    G(ultrasonic_offset, kUltrasonicOffset), G(active_profile, kActiveProfile),
    P(deadband_start[0], kDeadbandStartLFwd), P(deadband_start[1], kDeadbandStartLRev),
    P(deadband_start[2], kDeadbandStartRFwd), P(deadband_start[3], kDeadbandStartRRev),
    P(deadband_keep[0], kDeadbandKeepLFwd), P(deadband_keep[1], kDeadbandKeepLRev),
    P(deadband_keep[2], kDeadbandKeepRFwd), P(deadband_keep[3], kDeadbandKeepRRev),
    P(max_cps_l, kMaxCpsL), P(max_cps_r, kMaxCpsR),
    P(pid_speed_kp, kPidSpeedKp), P(pid_speed_ki, kPidSpeedKi), P(pid_speed_kd, kPidSpeedKd),
    P(pid_rate_kp, kPidRateKp), P(pid_rate_ki, kPidRateKi), P(pid_rate_kd, kPidRateKd),
    P(pid_heading_kp, kPidHeadingKp), P(pid_heading_ki, kPidHeadingKi),
    P(pid_heading_kd, kPidHeadingKd),
};
#undef G
#undef P

// Ordering invariants between settings: lo <= hi, or lo < hi when strict.
struct Order {
  uint8_t lo;
  uint8_t hi;
  bool strict;
};

const Order kOrders[] PROGMEM = {
    {static_cast<uint8_t>(proto::SettingId::kPanMin), static_cast<uint8_t>(proto::SettingId::kPanMax), false},
    {static_cast<uint8_t>(proto::SettingId::kTiltMin), static_cast<uint8_t>(proto::SettingId::kTiltMax), false},
    {static_cast<uint8_t>(proto::SettingId::kObstacleStop), static_cast<uint8_t>(proto::SettingId::kObstacleSlow),
     true},
    {static_cast<uint8_t>(proto::SettingId::kBattCrit), static_cast<uint8_t>(proto::SettingId::kBattLow), true},
};

bool inOrder(int32_t lo, int32_t hi, bool strict) { return strict ? lo < hi : lo <= hi; }

uint8_t wireSize(proto::WireType t) {
  switch (t) {
    case proto::WireType::kU8:
    case proto::WireType::kI8: return 1;
    case proto::WireType::kU16:
    case proto::WireType::kI16: return 2;
    default: return 4;
  }
}

int32_t loadValue(const uint8_t* p, proto::WireType t) {
  switch (t) {
    case proto::WireType::kU8: return p[0];
    case proto::WireType::kI8: return static_cast<int8_t>(p[0]);
    case proto::WireType::kU16: return static_cast<uint16_t>(p[0] | (p[1] << 8));
    case proto::WireType::kI16: return static_cast<int16_t>(p[0] | (p[1] << 8));
    case proto::WireType::kU32:
    case proto::WireType::kI32: {
      int32_t v = 0;
      memcpy(&v, p, 4);
      return v;
    }
  }
  return 0;
}

void storeValue(uint8_t* p, proto::WireType t, int32_t v) {
  const uint8_t n = wireSize(t);
  for (uint8_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
}

bool findDescriptor(uint8_t id, proto::SettingDescriptor& out) {
  for (uint8_t i = 0; i < proto::kSettingCount; ++i) {
    CQ_READ_P(&out, &proto::kSettingDescriptors[i], sizeof out);
    if (out.id == id) return true;
  }
  return false;
}

}  // namespace

bool Settings::slot(uint8_t id, uint8_t*& ptr, proto::WireType& type,
                    proto::SettingDescriptor& desc) const {
  if (!findDescriptor(id, desc)) return false;
  for (uint8_t i = 0; i < sizeof kSlots / sizeof kSlots[0]; ++i) {
    Slot s;
    CQ_READ_P(&s, &kSlots[i], sizeof s);
    if (s.id != id) continue;
    uint8_t* base = s.block == kBlockGlobal ? reinterpret_cast<uint8_t*>(const_cast<GlobalSettings*>(&m_global))
                                            : reinterpret_cast<uint8_t*>(const_cast<ProfileSettings*>(&m_profile));
    ptr = base + s.offset;
    type = desc.type;
    return true;
  }
  return false;
}

void Settings::loadDefaults() {
  for (uint8_t i = 0; i < proto::kSettingCount; ++i) {
    proto::SettingDescriptor d;
    CQ_READ_P(&d, &proto::kSettingDescriptors[i], sizeof d);
    uint8_t* p = nullptr;
    proto::WireType t{};
    proto::SettingDescriptor dd;
    if (slot(d.id, p, t, dd)) storeValue(p, t, d.def);
  }
  m_dirty = false;
}

bool Settings::get(uint8_t id, int32_t& value) const {
  uint8_t* p = nullptr;
  proto::WireType t{};
  proto::SettingDescriptor d;
  if (!slot(id, p, t, d)) return false;
  value = loadValue(p, t);
  return true;
}

proto::AckStatus Settings::set(uint8_t id, int32_t value) {
  uint8_t* p = nullptr;
  proto::WireType t{};
  proto::SettingDescriptor d;
  if (!slot(id, p, t, d)) return proto::AckStatus::kSettingUnknown;
  if (value < d.min || value > d.max) return proto::AckStatus::kSettingOutOfRange;
  if (!keepsOrder(id, value)) return proto::AckStatus::kSettingOutOfRange;
  storeValue(p, t, value);
  m_dirty = true;
  return proto::AckStatus::kOk;
}

bool Settings::keepsOrder(uint8_t id, int32_t value) const {
  for (uint8_t i = 0; i < sizeof kOrders / sizeof kOrders[0]; ++i) {
    Order o;
    CQ_READ_P(&o, &kOrders[i], sizeof o);
    int32_t other = 0;
    if (id == o.lo && get(o.hi, other) && !inOrder(value, other, o.strict)) return false;
    if (id == o.hi && get(o.lo, other) && !inOrder(other, value, o.strict)) return false;
  }
  return true;
}

uint8_t Settings::serialize(uint8_t block, uint8_t* out) const {
  if (block == kBlockGlobal) {
    memcpy(out, &m_global, sizeof m_global);
    return sizeof m_global;
  }
  memcpy(out, &m_profile, sizeof m_profile);
  return sizeof m_profile;
}

bool Settings::deserialize(uint8_t block, const uint8_t* data, uint8_t len, uint16_t stored_crc) {
  const uint8_t expected = block == kBlockGlobal ? sizeof m_global : sizeof m_profile;
  if (len != expected || crc16(data, len) != stored_crc) return false;
  if (block == kBlockGlobal) {
    memcpy(&m_global, data, len);
  } else {
    memcpy(&m_profile, data, len);
  }
  return true;
}

uint16_t Settings::crc() const {
  const uint16_t c = crc16(reinterpret_cast<const uint8_t*>(&m_global), sizeof m_global);
  return crc16(reinterpret_cast<const uint8_t*>(&m_profile), sizeof m_profile, c);
}

}  // namespace cq
```
`src/domain/settings/eeprom_writer.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

struct EepromIo {
  uint8_t (*read)(uint16_t addr);
  bool (*writeAsync)(uint16_t addr, uint8_t value);  // false if the previous write is still running
  bool (*busy)();
};

// Writes queued byte ranges at most one byte per step(); unchanged bytes are skipped.
// RAM: 128 B data + 6 ranges × 4 B (one EEPROM address per range, not per byte).
class EepromWriter {
 public:
  static constexpr uint8_t kMaxBytes = 128;
  static constexpr uint8_t kMaxRanges = 6;
  bool enqueue(uint16_t addr, const uint8_t* data, uint8_t len);  // false if full
  bool step(const EepromIo& io);                                   // true when idle
  bool idle() const { return m_range_count == 0; }

 private:
  struct Range {
    uint16_t addr;
    uint8_t offset;  // into m_data
    uint8_t len;
  };
  uint8_t m_data[kMaxBytes] = {};
  Range m_ranges[kMaxRanges] = {};
  uint8_t m_range_count = 0;
  uint8_t m_used = 0;    // bytes of m_data in use
  uint8_t m_cursor = 0;  // next byte inside m_ranges[0]
};

}  // namespace cq
```

`src/domain/settings/eeprom_writer.cpp`:
```cpp
#include "domain/settings/eeprom_writer.h"

namespace cq {

bool EepromWriter::enqueue(uint16_t addr, const uint8_t* data, uint8_t len) {
  if (m_range_count == kMaxRanges || static_cast<uint16_t>(m_used) + len > kMaxBytes) return false;
  for (uint8_t i = 0; i < len; ++i) m_data[m_used + i] = data[i];
  m_ranges[m_range_count++] = Range{addr, m_used, len};
  m_used = static_cast<uint8_t>(m_used + len);
  return true;
}

bool EepromWriter::step(const EepromIo& io) {
  while (m_range_count > 0) {
    Range& r = m_ranges[0];
    if (m_cursor == r.len) {  // range finished: drop it
      for (uint8_t i = 1; i < m_range_count; ++i) m_ranges[i - 1] = m_ranges[i];
      --m_range_count;
      m_cursor = 0;
      if (m_range_count == 0) m_used = 0;  // ranges are only appended between saves
      continue;
    }
    if (io.busy()) return false;
    const uint16_t addr = static_cast<uint16_t>(r.addr + m_cursor);
    const uint8_t value = m_data[r.offset + m_cursor];
    if (io.read(addr) == value) {  // unchanged byte: skip, try the next one this pass
      ++m_cursor;
      continue;
    }
    if (!io.writeAsync(addr, value)) return false;
    ++m_cursor;
    return false;  // one write per step; the next step drops a finished range
  }
  return true;
}

}  // namespace cq
```

Note: a full save is magic (3 B) + global (1 + 56 + 2) + profile (1 + 51 + 2) = 116 B in 5 ranges, within 128 B / 6 ranges.

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_settings`
Expected: `8 Tests 0 Failures`.

- [ ] **Step 5: EEPROM port and driver**

`src/ports/eeprom.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace eeprom {
uint8_t read(uint16_t addr);                       // waits for a running write (only at boot / idle)
bool writeByteAsync(uint16_t addr, uint8_t value);  // false if a write is running
bool busy();
}  // namespace eeprom
}  // namespace port
}  // namespace cq
```

`src/drivers/eeprom_uno.cpp`:
```cpp
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/io.h>

#include "ports/eeprom.h"

namespace cq {
namespace port {
namespace eeprom {

bool busy() { return (EECR & _BV(EEPE)) != 0; }

uint8_t read(uint16_t addr) { return eeprom_read_byte(reinterpret_cast<const uint8_t*>(addr)); }

bool writeByteAsync(uint16_t addr, uint8_t value) {
  if (busy()) return false;
  EEAR = addr;
  EEDR = value;
  const uint8_t sreg = SREG;
  cli();
  EECR |= _BV(EEMPE);
  EECR |= _BV(EEPE);
  SREG = sreg;
  return true;
}

}  // namespace eeprom
}  // namespace port
}  // namespace cq
```

- [ ] **Step 6: Settings app glue (load at boot, save when stopped, routes)**

Add to `src/app/app.h` includes and members (inside `struct App`, after `encoders_enabled`):
```cpp
#include "domain/settings/eeprom_writer.h"
#include "domain/settings/settings.h"
```
```cpp
  // settings
  Settings settings;
  EepromWriter eewriter;
  bool save_requested = false;
  bool save_in_progress = false;
  bool moving = false;  // set by the control task; saves are deferred while moving
```

`src/app/tasks.h` (one declaration per app entry point; later tasks add theirs before `// TASK-DECLS-END`):
```cpp
#pragma once

namespace cq {
namespace app {

void settingsLoad();   // boot: EEPROM → settings (defaults + SETTINGS_RESET on bad CRC/magic)
void settingsTask();   // 50 Hz: deferred save, one EEPROM byte per pass
void applySettings();  // pushes settings into domain objects (app/apply_settings.cpp)
// TASK-DECLS-END

}  // namespace app
}  // namespace cq
```

`src/app/settings_task.cpp`:
```cpp
#include "app/app.h"
#include "app/tasks.h"
#include "domain/settings/crc16.h"
#include "ports/eeprom.h"

namespace cq {
namespace app {
namespace {

constexpr uint16_t kMagicAddr = 0;
constexpr uint8_t kLayoutVersion = 1;
constexpr uint16_t kGlobalAddr = 8;
constexpr uint16_t kProfileAddr = 80;

const EepromIo kIo{port::eeprom::read, port::eeprom::writeByteAsync, port::eeprom::busy};

bool loadBlock(uint8_t block, uint16_t addr) {
  uint8_t buf[64];
  const uint8_t len = port::eeprom::read(addr);
  if (len == 0 || len > sizeof buf) return false;
  for (uint8_t i = 0; i < len; ++i) buf[i] = port::eeprom::read(static_cast<uint16_t>(addr + 1 + i));
  const uint16_t crc = static_cast<uint16_t>(port::eeprom::read(static_cast<uint16_t>(addr + 1 + len)) |
                                             (port::eeprom::read(static_cast<uint16_t>(addr + 2 + len)) << 8));
  return g_app.settings.deserialize(block, buf, len, crc);
}

void queueBlock(uint8_t block, uint16_t addr) {
  uint8_t buf[66];
  const uint8_t len = g_app.settings.serialize(block, buf + 1);
  buf[0] = len;
  const uint16_t crc = crc16(buf + 1, len);
  g_app.eewriter.enqueue(addr, buf, static_cast<uint8_t>(len + 1));  // data first …
  const uint8_t crc_bytes[2] = {static_cast<uint8_t>(crc), static_cast<uint8_t>(crc >> 8)};
  g_app.eewriter.enqueue(static_cast<uint16_t>(addr + 1 + len), crc_bytes, 2);  // … CRC last
}

}  // namespace

void settingsLoad() {
  g_app.settings.loadDefaults();
  uint8_t reset_mask = 0;
  const bool magic_ok = port::eeprom::read(kMagicAddr) == 'C' && port::eeprom::read(kMagicAddr + 1) == 'Q' &&
                        port::eeprom::read(kMagicAddr + 2) == kLayoutVersion;
  if (!magic_ok || !loadBlock(Settings::kBlockGlobal, kGlobalAddr)) reset_mask |= 0x01;
  if (!magic_ok || !loadBlock(Settings::kBlockProfile, kProfileAddr)) reset_mask |= 0x02;
  if (reset_mask != 0) {
    g_app.events.push(proto::EventKind::kSettingsReset, reset_mask);
    g_app.save_requested = true;  // persisted lazily by the writer, never synchronously at boot
  }
  g_app.settings.clearDirty();
  applySettings();
}

// 50 Hz: deferred save when stopped; one EEPROM byte per pass.
void settingsTask() {
  if (g_app.save_requested && !g_app.moving && !g_app.save_in_progress) {
    const uint8_t magic[3] = {'C', 'Q', kLayoutVersion};
    g_app.eewriter.enqueue(kMagicAddr, magic, 3);
    queueBlock(Settings::kBlockGlobal, kGlobalAddr);
    queueBlock(Settings::kBlockProfile, kProfileAddr);
    g_app.save_requested = false;
    g_app.save_in_progress = true;
  }
  const bool idle = g_app.eewriter.step(kIo);
  if (g_app.save_in_progress && idle) {
    g_app.save_in_progress = false;
    g_app.settings.clearDirty();
    g_app.events.push(proto::EventKind::kSettingsSaved);
  }
}

}  // namespace app
}  // namespace cq
```

`src/app/apply_settings.cpp` (later tasks add lines inside `applySettings()` before `// APPLY-END`):
```cpp
#include "app/app.h"
#include "app/tasks.h"

namespace cq {
namespace app {

void applySettings() {
  const GlobalSettings& gs = g_app.settings.global();
  g_app.encoders_enabled = gs.encoders_enabled != 0;
  // APPLY-END
}

}  // namespace app
}  // namespace cq
```

Add to `src/app/routes.cpp`, inside the anonymous namespace (after `onEspAck`):
```cpp
proto::AckStatus onSettingSet(const uint8_t* p, uint8_t len) {
  proto::SettingSetMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = g_app.settings.set(m.id, m.value);
  if (st == proto::AckStatus::kOk) applySettings();
  return st;
}

proto::AckStatus onSettingGet(const uint8_t* p, uint8_t len) {
  proto::SettingGetMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  int32_t v = 0;
  if (!g_app.settings.get(m.id, v)) return proto::AckStatus::kSettingUnknown;
  g_app.setting_value.id = m.id;
  g_app.setting_value.value = v;
  g_app.setting_value_pending = true;
  return proto::AckStatus::kOk;
}

proto::AckStatus onSettingsSave(const uint8_t* p, uint8_t len) {
  proto::SettingsSaveMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (g_app.save_in_progress) return proto::AckStatus::kSettingsSaveBusy;
  g_app.save_requested = true;
  return proto::AckStatus::kOk;
}
```
Also add `#include "app/tasks.h"` to the includes of `routes.cpp` (for `applySettings()`).
Add rows before `// ROUTES-END`:
```cpp
    {static_cast<uint8_t>(proto::FrameType::kSettingSet), onSettingSet, true},
    {static_cast<uint8_t>(proto::FrameType::kSettingGet), onSettingGet, true},
    {static_cast<uint8_t>(proto::FrameType::kSettingsSave), onSettingsSave, true},
```

`src/app/comms.cpp` — in `telemetrySlowDue()` add before `g_app.slow_due = true;` (CRC-16 over 107 B costs ~0.5 ms on AVR, so it runs at 1 Hz, not every settings pass):
```cpp
  s.settings_crc = g_app.settings.crc();
```

In `src/main.cpp` add `#include "app/tasks.h"` to the includes.
Add before `// SETUP-END`: `cq::app::settingsLoad();`
Add task row before `// TASKS-END`: `{cq::app::settingsTask, 20000UL, 0},`

- [ ] **Step 7: Smoke: setting get/set/save**

Add to `tools/smoke/uno_smoke.py` (new command functions and registry entries):
```python
def cmd_setting(link: Link, args: argparse.Namespace) -> None:
    info = next(s for s in g.SETTINGS if s.name == args.name)
    done = threading.Event()

    def show(msg: object) -> None:
        if isinstance(msg, g.SettingValue) and msg.id == info.id:
            print(f"{info.name} = {msg.value} ({msg.value * info.scale:g} {info.unit})")
            done.set()
        elif isinstance(msg, g.Ack):
            print(f"ACK {g.ACK_STATUS[msg.status]}")
        elif isinstance(msg, g.Event):
            print(fmt_event(msg))

    link.handlers.append(show)
    time.sleep(0.6)
    if args.value is not None:
        link.send(g.SettingSet(id=info.id, value=args.value))
        time.sleep(0.2)
    link.send(g.SettingGet(id=info.id))
    done.wait(1.0)
    if args.save:
        link.send(g.SettingsSave())
        time.sleep(1.5)
```
Register: `COMMANDS["setting"] = cmd_setting`, and in `main()`:
```python
    s = sub.add_parser("setting")
    s.add_argument("name")
    s.add_argument("value", nargs="?", type=int)
    s.add_argument("--save", action="store_true")
```

- [ ] **Step 8: Build, flash, verify persistence**

Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py && pio run -e uno -t upload`
Then (from `tools/smoke/`): `uv run python uno_smoke.py --port PORT setting tilt_limit 40 --save`
Expected: `ACK OK`, `tilt_limit = 40 (40 deg)`, `EVENT SETTINGS_SAVED`. (The first boot right after `-t upload` already emitted `SETTINGS_RESET a=3` and saved the defaults; opening the port reboots the Uno with valid EEPROM, so that event is not seen here. To see it, run `… watch` straight after an upload that changed the layout, or erase the magic first.) Press the Uno reset button and run `… setting tilt_limit` → `tilt_limit = 40` and no `SETTINGS_RESET`. Cross-field check: `… setting obstacle_stop 60` → `ACK SETTING_OUT_OF_RANGE` (would not be below `obstacle_slow` = 50).

- [ ] **Step 9: Commit**

```bash
git add firmware/uno tools/smoke
git commit -m "feat(uno): settings store with PROGMEM descriptors, non-blocking EEPROM writer, setting routes"
```

---

### Task 4: IMU — MPU6050 driver, attitude, yaw, bump/lift/tilt, IMU task

**Files:**
- Create: `src/domain/estimation/imu_scale.h`, `attitude.{h,cpp}`, `yaw_integrator.{h,cpp}`, `bump_detector.{h,cpp}`, `lift_detector.{h,cpp}`, `accel_variance.{h,cpp}`; `src/ports/imu.h`; `src/drivers/imu_uno.cpp`; `src/app/imu_task.cpp`
- Modify: `src/app/app.h`, `src/app/tasks.h`, `src/main.cpp`
- Test: `test/test_estimation/test_main.cpp`

**Interfaces:**
- Consumes: `ImuRaw` (Task 1), `GlobalSettings` (Task 3), `EventQueue` (Task 2).
- Produces:
  - `cq::imu_scale::kAccelLsbPerG = 8192.0f`, `kGyroLsbPerDps = 65.5f`, `tempCentiC(int16_t raw) -> int16_t`.
  - `cq::Attitude`: `void reset(float pitch_deg, float roll_deg)`, `void update(float ax_g, float ay_g, float az_g, float gx_dps, float gy_dps, float dt_s)` (called at 200 Hz: gyro integration every call, accel correction — atan2/sqrt — every 4th call = 50 Hz, LLD §9.13), `void integrateGyro(float gx_dps, float gy_dps, float dt_s)`, `void correctAccel(float ax_g, float ay_g, float az_g)`, `float pitchDeg() const`, `float rollDeg() const`. Pitch nose-up + (`ax = +sin θ` nose up: the accelerometer reads specific force, +1 g up when level), roll right-side-down +.
  - `cq::YawIntegrator`: `void update(float rate_dps, float dt_s, bool stationary)`, `float yawDeg() const` (0..360, CCW +), `float rateDps() const`, `float totalDeg() const` (unwrapped), `void reset()`.
  - `cq::BumpDetector`: `bool update(float ax_g, float ay_g, uint16_t threshold_mg, uint32_t now_ms, int16_t& mg, int16_t& dir)`.
  - `cq::LiftDetector`: `enum class Change : uint8_t { kNone, kLifted, kLanded, kTilted, kLevel }`; `Change update(float az_g, float pitch_deg, float roll_deg, uint8_t tilt_limit_deg, uint32_t now_ms)`, `bool lifted() const`, `bool tilted() const`.
  - `cq::AccelVariance`: `void add(float mag_g, uint32_t now_ms)`, `uint16_t value() const` (mg², 1 s window).
  - Port `cq::port::imu`: `bool begin()` (also re-initialises the TWI; used for the 1 s re-init), `bool read(ImuRaw& out)` (axis-mapped), `uint16_t i2cErrors()`. On a Wire timeout the driver runs I2C bus recovery (9 SCL clocks + STOP + `Wire.begin()`, LLD §9.13) before returning false.
  - App members `Attitude attitude; YawIntegrator yaw; BumpDetector bump; LiftDetector lift; AccelVariance accel_var; ImuRaw imu_raw; bool imu_ok; uint8_t imu_fail_streak; float yaw_rate_dps; bool stationary; uint16_t fast_flags; uint32_t slow_flags;`; `cq::app::imuInit()`, `cq::app::imuTask()` (200 Hz), declared in `app/tasks.h`. `imu_ok` goes false after 10 consecutive failed reads (`IMU_FAULT` event + `kImuFault` flag) and the task then retries `begin()` once per second; a successful re-init sets `imu_ok` and clears the flag.

- [ ] **Step 1: Write the failing estimation tests**

`test/test_estimation/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/estimation/accel_variance.h"
#include "domain/estimation/attitude.h"
#include "domain/estimation/bump_detector.h"
#include "domain/estimation/lift_detector.h"
#include "domain/estimation/yaw_integrator.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_attitude_level_stays_zero() {
  Attitude a;
  a.reset(0, 0);
  for (int i = 0; i < 200; ++i) a.update(0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.005f);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, a.pitchDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, a.rollDeg());
}

void test_attitude_converges_to_accel_tilt() {
  Attitude a;
  a.reset(0, 0);
  // nose up 20 deg: the accelerometer reads specific force (+1 g up when level), so it has a
  // positive x component
  const float s = 0.3420f, c = 0.9397f;
  for (int i = 0; i < 1000; ++i) a.update(s, 0.0f, c, 0.0f, 0.0f, 0.005f);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 20.0f, a.pitchDeg());
}

// LLD §9.13: gyro integration at 200 Hz (every call), atan2/sqrt correction at 50 Hz (every 4th).
void test_attitude_accel_correction_every_fourth_call() {
  Attitude a;
  a.reset(0, 0);
  const float s = 0.3420f, c = 0.9397f;  // accel says nose up 20 deg
  for (int i = 0; i < 3; ++i) a.update(s, 0.0f, c, 0.0f, 0.0f, 0.005f);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, a.pitchDeg());  // calls 1-3: gyro only
  a.update(s, 0.0f, c, 0.0f, 0.0f, 0.005f);             // call 4: accel blend
  TEST_ASSERT_FLOAT_WITHIN(0.01f, (1.0f - Attitude::kAlpha) * 20.0f, a.pitchDeg());
  a.reset(0, 0);
  a.update(0.0f, 0.0f, 1.0f, 0.0f, -10.0f, 0.005f);  // gy -10 dps = nose up, integrated at once
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.05f, a.pitchDeg());
}

void test_yaw_integrates_and_wraps() {
  YawIntegrator y;
  for (int i = 0; i < 200; ++i) y.update(90.0f, 0.005f, false);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 90.0f, y.yawDeg());
  for (int i = 0; i < 400; ++i) y.update(-90.0f, 0.005f, false);  // -180 → net -90
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 270.0f, y.yawDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, -90.0f, y.totalDeg());
}

void test_yaw_ignores_small_rate_when_stationary() {
  YawIntegrator y;
  for (int i = 0; i < 2000; ++i) y.update(0.3f, 0.005f, true);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, y.yawDeg());
  for (int i = 0; i < 200; ++i) y.update(0.3f, 0.005f, false);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.3f, y.yawDeg());
}

void test_bump_detects_front_hit_once() {
  BumpDetector b;
  int16_t mg = 0, dir = 0;
  for (uint32_t t = 0; t < 100; t += 5) TEST_ASSERT_FALSE(b.update(0.0f, 0.0f, 1500, t, mg, dir));
  TEST_ASSERT_TRUE(b.update(-2.0f, 0.0f, 1500, 100, mg, dir));  // deceleration = hit at the front
  TEST_ASSERT_EQUAL(0, dir);
  TEST_ASSERT_TRUE(mg >= 1900);
  TEST_ASSERT_FALSE(b.update(-2.0f, 0.0f, 1500, 200, mg, dir));  // refractory 500 ms
  TEST_ASSERT_TRUE(b.update(2.0f, 0.0f, 1500, 700, mg, dir));
  TEST_ASSERT_EQUAL(1, dir);
}

void test_lift_and_land() {
  LiftDetector l;
  uint32_t t = 0;
  for (; t < 200; t += 5) TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kNone),
                                            static_cast<int>(l.update(1.0f, 0, 0, 35, t)));
  LiftDetector::Change c = LiftDetector::Change::kNone;
  for (; t < 300; t += 5) {
    const LiftDetector::Change x = l.update(0.4f, 0, 0, 35, t);
    if (x != LiftDetector::Change::kNone) c = x;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kLifted), static_cast<int>(c));
  TEST_ASSERT_TRUE(l.lifted());
  c = LiftDetector::Change::kNone;
  for (; t < 1000; t += 5) {
    const LiftDetector::Change x = l.update(1.0f, 0, 0, 35, t);
    if (x != LiftDetector::Change::kNone) c = x;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kLanded), static_cast<int>(c));
  TEST_ASSERT_FALSE(l.lifted());
}

void test_tilt_hysteresis() {
  LiftDetector l;
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kTilted),
                    static_cast<int>(l.update(1.0f, 36.0f, 0, 35, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kNone),
                    static_cast<int>(l.update(1.0f, 32.0f, 0, 35, 5)));  // inside hysteresis
  TEST_ASSERT_EQUAL(static_cast<int>(LiftDetector::Change::kLevel),
                    static_cast<int>(l.update(1.0f, 29.0f, 0, 35, 10)));
}

void test_accel_variance_window() {
  AccelVariance v;
  for (uint32_t t = 0; t <= 1000; t += 5) v.add((t / 5) % 2 ? 1.1f : 0.9f, t);
  TEST_ASSERT_UINT16_WITHIN(1000, 10000, v.value());  // ±100 mg → 10000 mg²
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_attitude_level_stays_zero);
  RUN_TEST(test_attitude_converges_to_accel_tilt);
  RUN_TEST(test_attitude_accel_correction_every_fourth_call);
  RUN_TEST(test_yaw_integrates_and_wraps);
  RUN_TEST(test_yaw_ignores_small_rate_when_stationary);
  RUN_TEST(test_bump_detects_front_hit_once);
  RUN_TEST(test_lift_and_land);
  RUN_TEST(test_tilt_hysteresis);
  RUN_TEST(test_accel_variance_window);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_estimation`
Expected: FAIL — `domain/estimation/accel_variance.h: No such file or directory`.

- [ ] **Step 3: Implement the estimators**

`src/domain/estimation/imu_scale.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {
namespace imu_scale {
constexpr float kAccelLsbPerG = 8192.0f;  // ±4 g
constexpr float kGyroLsbPerDps = 65.5f;   // ±500 °/s
// MPU6050: T = raw / 340 + 36.53 °C → returned in 0.01 °C
inline int16_t tempCentiC(int16_t raw) { return static_cast<int16_t>((static_cast<int32_t>(raw) * 100) / 340 + 3653); }
}  // namespace imu_scale
}  // namespace cq
```

`src/domain/estimation/attitude.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Complementary filter for pitch (nose up +) and roll (right side down +), tank frame.
// update() runs at 200 Hz: the gyro is integrated every call, the accelerometer correction
// (atan2f x2 + sqrtf, the expensive part on AVR) every kAccelEvery-th call = 50 Hz (LLD §9.13).
class Attitude {
 public:
  static constexpr uint8_t kAccelEvery = 4;
  static constexpr float kAlpha = 0.92f;  // per 50 Hz correction (~0.98^4: ~0.25 s time constant)
  void reset(float pitch_deg, float roll_deg) {
    m_pitch_deg = pitch_deg;
    m_roll_deg = roll_deg;
    m_div = 0;
  }
  void update(float ax_g, float ay_g, float az_g, float gx_dps, float gy_dps, float dt_s);
  void integrateGyro(float gx_dps, float gy_dps, float dt_s);
  void correctAccel(float ax_g, float ay_g, float az_g);
  float pitchDeg() const { return m_pitch_deg; }
  float rollDeg() const { return m_roll_deg; }

 private:
  float m_pitch_deg = 0.0f;
  float m_roll_deg = 0.0f;
  uint8_t m_div = 0;
};

}  // namespace cq
```

`src/domain/estimation/attitude.cpp`:
```cpp
#include "domain/estimation/attitude.h"

#include <math.h>

namespace cq {

namespace {
constexpr float kRadToDeg = 57.29578f;
}

void Attitude::update(float ax_g, float ay_g, float az_g, float gx_dps, float gy_dps, float dt_s) {
  integrateGyro(gx_dps, gy_dps, dt_s);
  if (++m_div >= kAccelEvery) {
    m_div = 0;
    correctAccel(ax_g, ay_g, az_g);
  }
}

void Attitude::integrateGyro(float gx_dps, float gy_dps, float dt_s) {
  // gyro y (left axis) positive = nose down in a right-handed x-forward/z-up frame
  m_pitch_deg -= gy_dps * dt_s;
  m_roll_deg += gx_dps * dt_s;
}

void Attitude::correctAccel(float ax_g, float ay_g, float az_g) {
  // Specific force: +1 g up when level, so nose up gives ax = +sin(pitch).
  const float acc_pitch = atan2f(ax_g, sqrtf(ay_g * ay_g + az_g * az_g)) * kRadToDeg;
  const float acc_roll = atan2f(ay_g, az_g) * kRadToDeg;
  m_pitch_deg = kAlpha * m_pitch_deg + (1.0f - kAlpha) * acc_pitch;
  m_roll_deg = kAlpha * m_roll_deg + (1.0f - kAlpha) * acc_roll;
}

}  // namespace cq
```

`src/domain/estimation/yaw_integrator.h`:
```cpp
#pragma once

namespace cq {

// Integrates bias-corrected gyro z (CCW +). While stationary, |rate| below the clamp is treated as 0.
class YawIntegrator {
 public:
  static constexpr float kStationaryClampDps = 0.5f;
  void update(float rate_dps, float dt_s, bool stationary);
  float yawDeg() const { return m_yaw_deg; }
  float rateDps() const { return m_rate_dps; }
  float totalDeg() const { return m_total_deg; }
  void reset() {
    m_yaw_deg = 0.0f;
    m_total_deg = 0.0f;
  }

 private:
  float m_yaw_deg = 0.0f;
  float m_total_deg = 0.0f;
  float m_rate_dps = 0.0f;
};

}  // namespace cq
```

`src/domain/estimation/yaw_integrator.cpp`:
```cpp
#include "domain/estimation/yaw_integrator.h"

#include "domain/util/math_util.h"

namespace cq {

void YawIntegrator::update(float rate_dps, float dt_s, bool stationary) {
  if (stationary && absf(rate_dps) < kStationaryClampDps) rate_dps = 0.0f;
  m_rate_dps = rate_dps;
  const float d = rate_dps * dt_s;
  m_total_deg += d;
  m_yaw_deg = wrap360(m_yaw_deg + d);
}

}  // namespace cq
```

`src/domain/estimation/bump_detector.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Horizontal acceleration spike above a slow baseline; 500 ms refractory.
class BumpDetector {
 public:
  bool update(float ax_g, float ay_g, uint16_t threshold_mg, uint32_t now_ms, int16_t& mg, int16_t& dir);

 private:
  float m_base_x = 0.0f;
  float m_base_y = 0.0f;
  uint32_t m_last_ms = 0;
  bool m_fired = false;
};

}  // namespace cq
```

`src/domain/estimation/bump_detector.cpp`:
```cpp
#include "domain/estimation/bump_detector.h"

#include <math.h>

#include "domain/util/math_util.h"

namespace cq {

bool BumpDetector::update(float ax_g, float ay_g, uint16_t threshold_mg, uint32_t now_ms, int16_t& mg,
                          int16_t& dir) {
  const float dx = ax_g - m_base_x;
  const float dy = ay_g - m_base_y;
  m_base_x += 0.02f * dx;
  m_base_y += 0.02f * dy;
  const float mag_mg = sqrtf(dx * dx + dy * dy) * 1000.0f;
  if (mag_mg < threshold_mg) return false;
  if (m_fired && !elapsed(now_ms, m_last_ms, 500)) return false;
  m_fired = true;
  m_last_ms = now_ms;
  mg = mag_mg > 32767.0f ? 32767 : static_cast<int16_t>(mag_mg);
  dir = dx < 0.0f ? 0 : 1;  // sudden deceleration = front hit
  return true;
}

}  // namespace cq
```

`src/domain/estimation/lift_detector.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Lifted: |az| < 0.6 g for 60 ms. Landed: 0.85..1.15 g for 500 ms. Tilt with 5° hysteresis.
class LiftDetector {
 public:
  enum class Change : uint8_t { kNone, kLifted, kLanded, kTilted, kLevel };
  Change update(float az_g, float pitch_deg, float roll_deg, uint8_t tilt_limit_deg, uint32_t now_ms);
  bool lifted() const { return m_lifted; }
  bool tilted() const { return m_tilted; }

 private:
  bool m_lifted = false;
  bool m_tilted = false;
  bool m_cond = false;
  uint32_t m_cond_since_ms = 0;
};

}  // namespace cq
```

`src/domain/estimation/lift_detector.cpp`:
```cpp
#include "domain/estimation/lift_detector.h"

#include "domain/util/math_util.h"

namespace cq {

LiftDetector::Change LiftDetector::update(float az_g, float pitch_deg, float roll_deg, uint8_t tilt_limit_deg,
                                          uint32_t now_ms) {
  const float tilt = absf(pitch_deg) > absf(roll_deg) ? absf(pitch_deg) : absf(roll_deg);
  if (!m_tilted && tilt > tilt_limit_deg) {
    m_tilted = true;
    return Change::kTilted;
  }
  if (m_tilted && tilt < static_cast<float>(tilt_limit_deg) - 5.0f) {
    m_tilted = false;
    return Change::kLevel;
  }
  const bool cond = m_lifted ? (az_g > 0.85f && az_g < 1.15f) : (absf(az_g) < 0.6f);
  if (cond != m_cond) {
    m_cond = cond;
    m_cond_since_ms = now_ms;
  }
  if (!cond) return Change::kNone;
  const uint32_t hold_ms = m_lifted ? 500u : 60u;
  if (!elapsed(now_ms, m_cond_since_ms, hold_ms)) return Change::kNone;
  m_lifted = !m_lifted;
  m_cond = false;
  return m_lifted ? Change::kLifted : Change::kLanded;
}

}  // namespace cq
```

`src/domain/estimation/accel_variance.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Variance of |a| (mg²) over 1 s windows (Welford); used as a surface roughness / vibration metric.
class AccelVariance {
 public:
  void add(float mag_g, uint32_t now_ms);
  uint16_t value() const { return m_value; }

 private:
  uint32_t m_window_start_ms = 0;
  uint16_t m_n = 0;
  float m_mean = 0.0f;
  float m_m2 = 0.0f;
  uint16_t m_value = 0;
};

}  // namespace cq
```

`src/domain/estimation/accel_variance.cpp`:
```cpp
#include "domain/estimation/accel_variance.h"

#include "domain/util/math_util.h"

namespace cq {

void AccelVariance::add(float mag_g, uint32_t now_ms) {
  const float x = mag_g * 1000.0f;
  ++m_n;
  const float d = x - m_mean;
  m_mean += d / m_n;
  m_m2 += d * (x - m_mean);
  if (elapsed(now_ms, m_window_start_ms, 1000) && m_n > 1) {
    const float var = m_m2 / (m_n - 1);
    m_value = var > 65535.0f ? 65535 : static_cast<uint16_t>(var);
    m_n = 0;
    m_mean = 0.0f;
    m_m2 = 0.0f;
    m_window_start_ms = now_ms;
  }
}

}  // namespace cq
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_estimation`
Expected: `9 Tests 0 Failures`.

- [ ] **Step 5: IMU port and MPU6050 driver**

`src/ports/imu.h`:
```cpp
#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {
namespace port {
namespace imu {
bool begin();               // WHO_AM_I check + configuration; false = not present
bool read(ImuRaw& out);     // one 14-byte burst (~380 µs at 400 kHz), mapped to the tank frame
uint16_t i2cErrors();
}  // namespace imu
}  // namespace port
}  // namespace cq
```

`src/drivers/imu_uno.cpp`:
```cpp
#include <Arduino.h>
#include <Wire.h>

#include "drivers/board_config.h"
#include "ports/imu.h"

namespace cq {
namespace port {
namespace imu {
namespace {

uint16_t g_errors = 0;

void configureWire() {
  Wire.begin();
  Wire.setClock(400000UL);
  Wire.setWireTimeout(3000, true);  // 3 ms; resets the TWI on a stuck bus
}

// I2C bus recovery (LLD §9.13): a slave stuck mid-byte holds SDA low; clock SCL up to 9 times
// until it lets go, then send a STOP and restart the TWI. Rare (only after a Wire timeout);
// ~100 µs of 5 µs busy-waits, a documented exception like the burst read.
void recoverBus() {
  Wire.end();
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, OUTPUT);
  for (uint8_t i = 0; i < 9 && digitalRead(SDA) == LOW; ++i) {
    digitalWrite(SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(SDA, OUTPUT);  // STOP: SDA low → high while SCL is high
  digitalWrite(SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(SCL, HIGH);
  delayMicroseconds(5);
  pinMode(SDA, INPUT_PULLUP);
  configureWire();
}

void onFailure() {
  ++g_errors;
  if (Wire.getWireTimeoutFlag()) {
    Wire.clearWireTimeoutFlag();
    recoverBus();
  }
}

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(board::kImuAddress);
  Wire.write(reg);
  Wire.write(value);
  if (Wire.endTransmission() != 0) {
    onFailure();
    return false;
  }
  return true;
}

int16_t pick(const int16_t v[3], int8_t map) {
  const int16_t x = v[(map < 0 ? -map : map) - 1];
  return map < 0 ? static_cast<int16_t>(-x) : x;
}

}  // namespace

bool begin() {
  configureWire();
  Wire.beginTransmission(board::kImuAddress);
  Wire.write(0x75);  // WHO_AM_I
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(board::kImuAddress, static_cast<uint8_t>(1)) != 1) {
    onFailure();
    return false;
  }
  if (Wire.read() != 0x68) return false;
  return writeReg(0x6B, 0x01)     // PWR_MGMT_1: wake, PLL with gyro X
         && writeReg(0x19, 0x04)  // SMPLRT_DIV: 1 kHz / 5 = 200 Hz
         && writeReg(0x1A, 0x03)  // CONFIG: DLPF 44 Hz
         && writeReg(0x1B, 0x08)  // GYRO_CONFIG: ±500 °/s
         && writeReg(0x1C, 0x08); // ACCEL_CONFIG: ±4 g
}

bool read(ImuRaw& out) {
  Wire.beginTransmission(board::kImuAddress);
  Wire.write(0x3B);  // ACCEL_XOUT_H
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(board::kImuAddress, static_cast<uint8_t>(14)) != 14) {
    onFailure();
    return false;
  }
  int16_t v[7];
  for (uint8_t i = 0; i < 7; ++i) {
    const uint8_t hi = static_cast<uint8_t>(Wire.read());
    const uint8_t lo = static_cast<uint8_t>(Wire.read());
    v[i] = static_cast<int16_t>((hi << 8) | lo);
  }
  const int16_t acc[3] = {v[0], v[1], v[2]};
  const int16_t gyr[3] = {v[4], v[5], v[6]};
  out.ax = pick(acc, board::kImuMapX);
  out.ay = pick(acc, board::kImuMapY);
  out.az = pick(acc, board::kImuMapZ);
  out.gx = pick(gyr, board::kImuMapX);
  out.gy = pick(gyr, board::kImuMapY);
  out.gz = pick(gyr, board::kImuMapZ);
  out.temp = v[3];
  out.t_us = micros();
  return true;
}

uint16_t i2cErrors() { return g_errors; }

}  // namespace imu
}  // namespace port
}  // namespace cq
```

- [ ] **Step 6: IMU task and app wiring**

Add to `src/app/app.h` includes:
```cpp
#include "domain/estimation/accel_variance.h"
#include "domain/estimation/attitude.h"
#include "domain/estimation/bump_detector.h"
#include "domain/estimation/lift_detector.h"
#include "domain/estimation/yaw_integrator.h"
#include "types/hw_types.h"
```
Add members to `struct App`:
```cpp
  // imu
  Attitude attitude;
  YawIntegrator yaw;
  BumpDetector bump;
  LiftDetector lift;
  AccelVariance accel_var;
  ImuRaw imu_raw{};
  bool imu_ok = false;
  uint8_t imu_fail_streak = 0;
  float yaw_rate_dps = 0.0f;
  bool stationary = true;  // set by the control task: no PWM output for 300 ms
  uint16_t fast_flags = 0;
  uint32_t slow_flags = 0;
```

`src/app/imu_task.cpp`:
```cpp
#include <math.h>

#include "app/app.h"
#include "domain/estimation/imu_scale.h"
#include "domain/util/math_util.h"
#include "ports/clock.h"
#include "ports/imu.h"

namespace cq {
namespace app {
namespace {

constexpr float kDt = 0.005f;
constexpr uint8_t kFaultStreak = 10;     // 50 ms of failed reads
constexpr uint32_t kReinitMs = 1000;     // begin() retry period while !imu_ok (<= ~3 ms per try)

uint32_t g_last_init_ms = 0;

void setFlag(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_app.fast_flags = on ? static_cast<uint16_t>(g_app.fast_flags | m) : static_cast<uint16_t>(g_app.fast_flags & ~m);
}

}  // namespace

void imuInit() {
  g_last_init_ms = port::clock::nowMs();
  g_app.imu_ok = port::imu::begin();
  if (g_app.imu_ok) g_app.imu_fail_streak = 0;
  setFlag(proto::FastFlagBit::kImuFault, !g_app.imu_ok);
}

void imuTask() {
  const uint32_t now = port::clock::nowMs();
  if (!g_app.imu_ok) {
    // Absent or faulted IMU: retry begin() once per second, never at 200 Hz (a stuck bus costs
    // a 3 ms Wire timeout per transaction).
    if (elapsed(now, g_last_init_ms, kReinitMs)) imuInit();
    g_app.slow.err_i2c = port::imu::i2cErrors();
    return;
  }
  ImuRaw& r = g_app.imu_raw;
  if (!port::imu::read(r)) {
    if (g_app.imu_fail_streak < 255) ++g_app.imu_fail_streak;  // saturates: no wrap, no repeat event
    if (g_app.imu_fail_streak == kFaultStreak) {
      setFlag(proto::FastFlagBit::kImuFault, true);
      g_app.events.push(proto::EventKind::kImuFault);
      g_app.imu_ok = false;  // the 1 s re-init above takes over
      g_last_init_ms = now;
    }
    g_app.slow.err_i2c = port::imu::i2cErrors();
    return;
  }
  g_app.imu_fail_streak = 0;

  const GlobalSettings& gs = g_app.settings.global();
  const float ax = r.ax / imu_scale::kAccelLsbPerG;
  const float ay = r.ay / imu_scale::kAccelLsbPerG;
  const float az = r.az / imu_scale::kAccelLsbPerG;
  // float maths: int16 - int16 in 16-bit int would overflow for a saturated gyro with a bias
  const float gx = (static_cast<float>(r.gx) - gs.gyro_bias_x) / imu_scale::kGyroLsbPerDps;
  const float gy = (static_cast<float>(r.gy) - gs.gyro_bias_y) / imu_scale::kGyroLsbPerDps;
  const float gz = (static_cast<float>(r.gz) - gs.gyro_bias_z) / imu_scale::kGyroLsbPerDps;

  g_app.attitude.update(ax, ay, az, gx, gy, kDt);  // accel correction inside at 50 Hz
  g_app.yaw.update(gz, kDt, g_app.stationary);
  g_app.yaw_rate_dps = g_app.yaw.rateDps();
  g_app.accel_var.add(sqrtf(ax * ax + ay * ay + az * az), now);

  const float pitch = g_app.attitude.pitchDeg() - gs.level_pitch / 100.0f;
  const float roll = g_app.attitude.rollDeg() - gs.level_roll / 100.0f;

  int16_t mg = 0, dir = 0;
  if (g_app.bump.update(ax, ay, gs.bump_threshold, now, mg, dir)) g_app.events.push(proto::EventKind::kBump, mg, dir);

  switch (g_app.lift.update(az, pitch, roll, gs.tilt_limit, now)) {
    case LiftDetector::Change::kLifted: g_app.events.push(proto::EventKind::kLifted); break;
    case LiftDetector::Change::kLanded: g_app.events.push(proto::EventKind::kLanded); break;
    case LiftDetector::Change::kTilted:
      g_app.events.push(proto::EventKind::kTilted, static_cast<int16_t>(pitch * 10), static_cast<int16_t>(roll * 10));
      break;
    case LiftDetector::Change::kLevel: g_app.events.push(proto::EventKind::kLevel); break;
    case LiftDetector::Change::kNone: break;
  }
  setFlag(proto::FastFlagBit::kLifted, g_app.lift.lifted());
  setFlag(proto::FastFlagBit::kTilted, g_app.lift.tilted());

  proto::TelemFastMsg& f = g_app.fast;
  f.yaw = static_cast<uint16_t>(g_app.yaw.yawDeg() * 100.0f) % 36000u;
  f.yaw_rate = static_cast<int16_t>(clampf(gz * 10.0f, -32767.0f, 32767.0f));
  f.pitch = static_cast<int16_t>(pitch * 100.0f);
  f.roll = static_cast<int16_t>(roll * 100.0f);
  f.flags = g_app.fast_flags;
  g_app.slow.temp = imu_scale::tempCentiC(r.temp);
  g_app.slow.accel_var = g_app.accel_var.value();
  g_app.slow.err_i2c = port::imu::i2cErrors();
}

}  // namespace app
}  // namespace cq
```

In `src/app/tasks.h` add before `// TASK-DECLS-END`:
```cpp
void imuInit();  // boot: MPU6050 begin(); also the 1 s re-init while !imu_ok
void imuTask();  // 200 Hz
```
In `src/main.cpp`: before `// SETUP-END` add `cq::app::imuInit();`; insert `{cq::app::imuTask, 5000UL, 0},` as the **first** row, directly after `cq::Task g_tasks[] = {` (so it runs first when several tasks are due).

- [ ] **Step 7: Build, flash, check attitude on the stand**

Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py && pio run -e uno -t upload`
Then: `uv run python uno_smoke.py --port PORT watch --seconds 15` (from `tools/smoke/`).
Expected: tank level → `pitch≈0 roll≈0` (± a few degrees until the level calibration in Task 7), `flags` without bit 11 (`IMU_FAULT`). Lift the front by hand: `pitch` goes positive. Rotate the tank CCW (seen from above) by hand: `yaw` increases and `rate` is positive; a quick knock on the front bumper prints `EVENT BUMP … b=0`; lifting the tank clear prints `EVENT LIFTED` then `EVENT LANDED` when set down. If pitch/yaw signs are wrong, Task 7's mount check sets the axis map. I2C recovery: pull the MPU6050's SDA wire (or the module) for a moment → `EVENT IMU_FAULT` and `flags` bit 11 set, `load` stays normal (one re-init try per second, not 200 Hz); reconnect → bit 11 clears within ~1 s.

- [ ] **Step 8: Commit**

```bash
git add firmware/uno
git commit -m "feat(uno): MPU6050 driver, attitude/yaw/bump/lift estimators, 200 Hz IMU task"
```

---

### Task 5: Sensors — ultrasonic, NEC IR decoder, battery, button (PCINT0 hub, async ADC)

**Files:**
- Create: `src/domain/estimation/ultrasonic_filter.{h,cpp}`, `battery_monitor.{h,cpp}`; `src/domain/input/nec_decoder.{h,cpp}`; `src/ports/ultrasonic.h`, `ir.h`, `battery_adc.h`, `button.h`; `src/drivers/pcint0_hub.cpp`, `battery_adc_uno.cpp`, `button_uno.cpp`; `src/app/sensor_tasks.cpp`
- Modify: `src/app/app.h`, `src/app/tasks.h`, `src/app/comms.cpp`, `src/main.cpp`
- Test: `test/test_sensors/test_main.cpp`

**Interfaces:**
- Consumes: `EchoState`, `IrEvent` (Task 1), `GlobalSettings` (Task 3), `EventQueue`.
- Produces:
  - `cq::UltrasonicFilter`: `enum class Change : uint8_t { kNone, kFault, kRecovered }`; `Change onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm, uint32_t now_ms)`, `uint16_t distMm() const` (`kClear = 0xFFFF`, `kFault = 0xFFFE`), `int16_t closingCmS() const`, `bool fault() const`, `bool valid() const`. Fault semantics (LLD §9.16): only a *failed measurement* counts — `kNoEcho` (no rising edge within 2 ms of the trigger) or a `kPending` beyond `kMaxPending` ticks (echo line stuck high > 250 ms); 3 consecutive → fault. `kClear` (echo high > 25 ms: nothing in range) is never a fault.
  - `cq::BatteryMonitor`: `enum class Level : uint8_t { kUsb, kOk, kLow, kCritical }`; `Level update(uint16_t adc, uint16_t mv_per_count_x100, uint16_t low_mv, uint16_t crit_mv, uint32_t now_ms)` (returns the new level on change, else the current; the voltage is the plain average of the last 4 samples, one sample per 10 Hz tick — LLD §9.13), `bool changed() const`, `uint16_t mv() const`, `Level level() const`.
  - `cq::NecDecoder`: `bool feed(uint32_t t_us, uint32_t now_ms, IrEvent& out)`.
  - Ports: `cq::port::ultrasonic::{begin(), bool trigger(), EchoState poll(uint16_t& echo_us)}` (`trigger()` sends no ping and returns false while the echo line is still high; the next `poll()` then returns `kPending`); `cq::port::ir::{begin(), bool popEdge(uint32_t& t_us)}`; `cq::port::battery_adc::{begin(), start(), bool ready(uint16_t& adc)}`; `cq::port::button::{begin(), bool pressed()}`.
  - App members `UltrasonicFilter us; BatteryMonitor batt; NecDecoder nec; IrEvent ir_event; bool ir_pending;` and tasks `cq::app::ultrasonicTask()` (20 Hz), `batteryTask()` (10 Hz), `irTask()` (200 Hz), `sensorsInit()`, declared in `app/tasks.h`.

- [ ] **Step 1: Write the failing sensor tests**

`test/test_sensors/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/estimation/battery_monitor.h"
#include "domain/estimation/ultrasonic_filter.h"
#include "domain/input/nec_decoder.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_ultrasonic_distance_and_median() {
  UltrasonicFilter f;
  f.onReading(EchoState::kReady, 2915, 0, 0);    // 500 mm
  f.onReading(EchoState::kReady, 20000, 0, 50);  // glitch 3430 mm
  f.onReading(EchoState::kReady, 2915, 0, 100);
  TEST_ASSERT_UINT16_WITHIN(3, 500, f.distMm());
  TEST_ASSERT_TRUE(f.valid());
}

void test_ultrasonic_offset_and_clear() {
  UltrasonicFilter f;
  for (uint32_t t = 0; t < 150; t += 50) f.onReading(EchoState::kReady, 2915, 20, t);
  TEST_ASSERT_UINT16_WITHIN(3, 480, f.distMm());
  for (uint32_t t = 150; t < 300; t += 50) f.onReading(EchoState::kClear, 0, 20, t);
  TEST_ASSERT_EQUAL_HEX16(UltrasonicFilter::kClear, f.distMm());
}

void test_ultrasonic_closing_speed() {
  UltrasonicFilter f;
  uint32_t t = 0;
  uint16_t us = 5830;  // 1000 mm, approaching 300 mm/s = 15 mm per 50 ms ≈ 87 µs
  for (int i = 0; i < 20; ++i, t += 50, us = static_cast<uint16_t>(us - 87)) f.onReading(EchoState::kReady, us, 0, t);
  TEST_ASSERT_INT16_WITHIN(5, 30, f.closingCmS());
}

void test_ultrasonic_fault_after_three_no_echo_and_recovery() {
  UltrasonicFilter f;
  UltrasonicFilter::Change c = UltrasonicFilter::Change::kNone;
  for (int i = 0; i < 2; ++i) c = f.onReading(EchoState::kNoEcho, 0, 0, static_cast<uint32_t>(i) * 50);
  TEST_ASSERT_FALSE(f.fault());
  c = f.onReading(EchoState::kNoEcho, 0, 0, 100);  // 3rd consecutive failed measurement (LLD §9.16)
  TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kFault), static_cast<int>(c));
  TEST_ASSERT_EQUAL_HEX16(UltrasonicFilter::kFault, f.distMm());
  c = f.onReading(EchoState::kReady, 2915, 0, 150);
  TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kRecovered), static_cast<int>(c));
}

void test_ultrasonic_open_space_and_clone_hold_are_not_faults() {
  UltrasonicFilter f;
  uint32_t t = 0;
  for (int i = 0; i < 100; ++i, t += 50) {  // open room: echo high > 25 ms every time
    TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kNone),
                      static_cast<int>(f.onReading(EchoState::kClear, 0, 0, t)));
  }
  for (int i = 0; i < 20; ++i) {  // clone holding the echo 200 ms: Clear, 4 skipped pings, Clear, …
    f.onReading(EchoState::kClear, 0, 0, t);
    t += 50;
    for (int k = 0; k < 4; ++k, t += 50) f.onReading(EchoState::kPending, 0, 0, t);
  }
  TEST_ASSERT_FALSE(f.fault());
  TEST_ASSERT_EQUAL_HEX16(UltrasonicFilter::kClear, f.distMm());
  f.onReading(EchoState::kNoEcho, 0, 0, t);  // failures must be consecutive
  f.onReading(EchoState::kNoEcho, 0, 0, t + 50);
  f.onReading(EchoState::kClear, 0, 0, t + 100);
  f.onReading(EchoState::kNoEcho, 0, 0, t + 150);
  TEST_ASSERT_FALSE(f.fault());
}

void test_ultrasonic_stuck_high_echo_faults() {
  UltrasonicFilter f;  // unplugged: the pull-up holds the echo line high, so no ping is ever sent
  UltrasonicFilter::Change c = UltrasonicFilter::Change::kNone;
  uint32_t t = 0;
  for (uint8_t i = 0; i < UltrasonicFilter::kMaxPending + 2; ++i, t += 50) {
    TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kNone),
                      static_cast<int>(f.onReading(EchoState::kPending, 0, 0, t)));
  }
  c = f.onReading(EchoState::kPending, 0, 0, t);  // 3rd pending beyond 250 ms → fault (400 ms in all)
  TEST_ASSERT_EQUAL(static_cast<int>(UltrasonicFilter::Change::kFault), static_cast<int>(c));
}

namespace {
// Builds the falling-edge times of one NEC frame (MSB-first code as printed by IRremote 2.x).
int necEdges(uint32_t code, uint32_t t0, uint32_t* out) {
  int n = 0;
  uint32_t t = t0;
  out[n++] = t;
  t += 13500;
  out[n++] = t;
  for (int i = 31; i >= 0; --i) {
    t += ((code >> i) & 1u) ? 2250 : 1125;
    out[n++] = t;
  }
  return n;
}
}  // namespace

void test_nec_decodes_frame_and_repeat() {
  NecDecoder d;
  uint32_t edges[40];
  const int n = necEdges(0x00FF629DUL, 1000000, edges);
  IrEvent e{};
  int frames = 0;
  for (int i = 0; i < n; ++i) frames += d.feed(edges[i], edges[i] / 1000, e) ? 1 : 0;
  TEST_ASSERT_EQUAL(1, frames);
  TEST_ASSERT_EQUAL_HEX32(0x00FF629DUL, e.code);
  TEST_ASSERT_FALSE(e.repeat);
  // repeat frame 108 ms after the frame start: 9 ms + 2.25 ms then the stop mark
  const uint32_t r0 = 1000000 + 108000;
  TEST_ASSERT_FALSE(d.feed(r0, r0 / 1000, e));
  TEST_ASSERT_TRUE(d.feed(r0 + 11250, (r0 + 11250) / 1000, e));
  TEST_ASSERT_TRUE(e.repeat);
  TEST_ASSERT_EQUAL_HEX32(0x00FF629DUL, e.code);
}

void test_nec_rejects_bad_inverse_and_noise() {
  NecDecoder d;
  uint32_t edges[40];
  const int n = necEdges(0x00FF6200UL, 0, edges);  // command inverse wrong
  IrEvent e{};
  int frames = 0;
  for (int i = 0; i < n; ++i) frames += d.feed(edges[i], edges[i] / 1000, e) ? 1 : 0;
  TEST_ASSERT_EQUAL(0, frames);
  TEST_ASSERT_FALSE(d.feed(500000, 500, e));  // lone repeat without a recent frame
  TEST_ASSERT_FALSE(d.feed(511250, 511, e));
}

void test_battery_levels_with_persistence() {
  BatteryMonitor b;
  // Default scale 4050 = 40.5 mV per count (10-bit ADC behind the divider): 198 counts ≈ 8019 mV
  for (uint32_t t = 0; t < 3000; t += 100) b.update(198, 4050, 7000, 6600, t);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kOk), static_cast<int>(b.level()));
  TEST_ASSERT_UINT16_WITHIN(20, 8000, b.mv());
  for (uint32_t t = 3000; t < 4000; t += 100) b.update(170, 4050, 7000, 6600, t);  // 6885 mV, < 2 s
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kOk), static_cast<int>(b.level()));
  for (uint32_t t = 4000; t < 9000; t += 100) b.update(170, 4050, 7000, 6600, t);
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kLow), static_cast<int>(b.level()));
  for (uint32_t t = 9000; t < 14000; t += 100) b.update(160, 4050, 7000, 6600, t);  // 6480 mV
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kCritical), static_cast<int>(b.level()));
}

// LLD §9.13: one sample per tick, plain average of the last 4 ticks.
void test_battery_four_sample_average() {
  BatteryMonitor b;
  b.update(200, 4050, 7000, 6600, 0);  // 8100 mV; the first sample alone is the average
  TEST_ASSERT_EQUAL(8100, b.mv());
  for (uint32_t t = 100; t < 400; t += 100) b.update(200, 4050, 7000, 6600, t);
  TEST_ASSERT_EQUAL(8100, b.mv());
  b.update(100, 4050, 7000, 6600, 400);  // 4050 mV: (3 x 8100 + 4050) / 4
  TEST_ASSERT_EQUAL(7087, b.mv());
  for (uint32_t t = 500; t < 800; t += 100) b.update(100, 4050, 7000, 6600, t);
  TEST_ASSERT_EQUAL(4050, b.mv());  // the old samples are gone after 4 ticks
}

void test_battery_usb_power_is_not_an_alarm() {
  BatteryMonitor b;
  for (uint32_t t = 0; t < 5000; t += 100) b.update(10, 4050, 7000, 6600, t);  // 405 mV: no battery
  TEST_ASSERT_EQUAL(static_cast<int>(BatteryMonitor::Level::kUsb), static_cast<int>(b.level()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ultrasonic_distance_and_median);
  RUN_TEST(test_ultrasonic_offset_and_clear);
  RUN_TEST(test_ultrasonic_closing_speed);
  RUN_TEST(test_ultrasonic_fault_after_three_no_echo_and_recovery);
  RUN_TEST(test_ultrasonic_open_space_and_clone_hold_are_not_faults);
  RUN_TEST(test_ultrasonic_stuck_high_echo_faults);
  RUN_TEST(test_nec_decodes_frame_and_repeat);
  RUN_TEST(test_nec_rejects_bad_inverse_and_noise);
  RUN_TEST(test_battery_levels_with_persistence);
  RUN_TEST(test_battery_four_sample_average);
  RUN_TEST(test_battery_usb_power_is_not_an_alarm);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_sensors`
Expected: FAIL — `domain/estimation/battery_monitor.h: No such file or directory`.

- [ ] **Step 3: Implement the sensor domain**

`src/domain/estimation/ultrasonic_filter.h`:
```cpp
#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {

// Median of the last 3 readings, offset, clear (> 4 m or echo high > 25 ms: nothing in range) and
// fault (LLD §9.16): 3 consecutive failed measurements. A failed measurement is kNoEcho (no rising edge
// within 2 ms of the trigger) or a kPending beyond kMaxPending ticks (echo line stuck high > 250 ms:
// clones hold it up to 200 ms; an unplugged sensor reads high through the pull-up). kClear never faults.
class UltrasonicFilter {
 public:
  static constexpr uint16_t kClear = 0xFFFF;
  static constexpr uint16_t kFault = 0xFFFE;
  static constexpr uint16_t kMaxMm = 4000;
  static constexpr uint8_t kFaultAfter = 3;
  static constexpr uint8_t kMaxPending = 5;  // 5 × 50 ms = 250 ms of skipped pings
  enum class Change : uint8_t { kNone, kFault, kRecovered };

  Change onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm, uint32_t now_ms);
  uint16_t distMm() const { return m_fault ? kFault : m_dist_mm; }
  int16_t closingCmS() const { return static_cast<int16_t>(m_closing_cm_s); }
  bool fault() const { return m_fault; }
  bool valid() const { return !m_fault && m_dist_mm != kClear; }

 private:
  Change countFailure();
  uint16_t m_hist[3] = {kClear, kClear, kClear};
  uint8_t m_idx = 0;
  uint16_t m_dist_mm = kClear;
  uint16_t m_prev_mm = kClear;
  uint32_t m_prev_ms = 0;
  float m_closing_cm_s = 0.0f;
  uint8_t m_failures = 0;  // consecutive failed measurements
  uint8_t m_pending = 0;   // consecutive ticks without a ping (echo still high)
  bool m_fault = false;
};

}  // namespace cq
```

`src/domain/estimation/ultrasonic_filter.cpp`:
```cpp
#include "domain/estimation/ultrasonic_filter.h"

namespace cq {

namespace {
uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
  if (a > b) {
    const uint16_t t = a;
    a = b;
    b = t;
  }
  if (b > c) b = c;
  return a > b ? a : b;
}
}  // namespace

UltrasonicFilter::Change UltrasonicFilter::countFailure() {
  if (m_failures < 255) ++m_failures;
  if (m_failures == kFaultAfter && !m_fault) {
    m_fault = true;
    return Change::kFault;
  }
  return Change::kNone;
}

UltrasonicFilter::Change UltrasonicFilter::onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm,
                                                     uint32_t now_ms) {
  Change change = Change::kNone;
  if (st == EchoState::kPending) {  // no ping sent: echo still high from the previous one
    if (m_pending < 255) ++m_pending;
    return m_pending > kMaxPending ? countFailure() : change;
  }
  m_pending = 0;
  if (st == EchoState::kNoEcho) return countFailure();
  m_failures = 0;  // the sensor answered (kReady or kClear)
  if (m_fault) {
    m_fault = false;
    change = Change::kRecovered;
  }
  uint16_t mm = kClear;
  if (st == EchoState::kReady) {
    const uint32_t raw = (static_cast<uint32_t>(echo_us) * 343u) / 2000u;  // speed of sound 343 m/s
    if (raw <= kMaxMm) mm = raw > offset_mm ? static_cast<uint16_t>(raw - offset_mm) : 0;
  }
  m_hist[m_idx] = mm;
  m_idx = static_cast<uint8_t>((m_idx + 1) % 3);
  m_dist_mm = median3(m_hist[0], m_hist[1], m_hist[2]);

  if (m_dist_mm != kClear && m_prev_mm != kClear && now_ms != m_prev_ms) {
    const float v = (static_cast<float>(m_prev_mm) - m_dist_mm) * 100.0f / static_cast<float>(now_ms - m_prev_ms);
    m_closing_cm_s += 0.3f * (v - m_closing_cm_s);  // mm/ms × 100 = cm/s
  } else if (m_dist_mm == kClear) {
    m_closing_cm_s = 0.0f;
  }
  m_prev_mm = m_dist_mm;
  m_prev_ms = now_ms;
  return change;
}

}  // namespace cq
```

`src/domain/estimation/battery_monitor.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Pack voltage = average of the last 4 samples (one per 10 Hz tick, LLD §9.13); low/critical need
// 2 s below the threshold, recovery needs +150 mV for 2 s.
// Below 3 V the board is USB powered (no pack): level kUsb, never an alarm.
class BatteryMonitor {
 public:
  enum class Level : uint8_t { kUsb, kOk, kLow, kCritical };
  static constexpr uint8_t kAvgSamples = 4;
  Level update(uint16_t adc, uint16_t mv_per_count_x100, uint16_t low_mv, uint16_t crit_mv, uint32_t now_ms);
  bool changed() const { return m_changed; }
  uint16_t mv() const { return m_mv; }
  Level level() const { return m_level; }

 private:
  uint16_t m_samples_mv[kAvgSamples] = {};
  uint8_t m_idx = 0;
  uint8_t m_count = 0;  // samples so far, up to kAvgSamples (the average primes on the first one)
  uint16_t m_mv = 0;
  Level m_level = Level::kOk;
  Level m_candidate = Level::kOk;
  uint32_t m_candidate_since_ms = 0;
  bool m_changed = false;
};

}  // namespace cq
```

`src/domain/estimation/battery_monitor.cpp`:
```cpp
#include "domain/estimation/battery_monitor.h"

#include "domain/util/math_util.h"

namespace cq {

BatteryMonitor::Level BatteryMonitor::update(uint16_t adc, uint16_t mv_per_count_x100, uint16_t low_mv,
                                             uint16_t crit_mv, uint32_t now_ms) {
  const uint32_t mv = (static_cast<uint32_t>(adc) * mv_per_count_x100 + 50u) / 100u;
  m_samples_mv[m_idx] = mv > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(mv);
  m_idx = static_cast<uint8_t>((m_idx + 1) % kAvgSamples);
  if (m_count < kAvgSamples) ++m_count;
  uint32_t sum = 0;
  for (uint8_t i = 0; i < m_count; ++i) sum += m_samples_mv[i];  // slots 0..m_count-1 are filled
  m_mv = static_cast<uint16_t>(sum / m_count);

  Level target;
  if (m_mv < 3000u) {
    target = Level::kUsb;
  } else if (m_mv < crit_mv) {
    target = Level::kCritical;
  } else if (m_mv < low_mv) {
    target = Level::kLow;
  } else if (m_level == Level::kLow || m_level == Level::kCritical) {
    target = m_mv > static_cast<uint32_t>(low_mv) + 150u ? Level::kOk : m_level;  // hysteresis on recovery
  } else {
    target = Level::kOk;
  }
  if (m_level == Level::kCritical && target == Level::kLow) target = Level::kCritical;

  m_changed = false;
  if (target == m_level) {
    m_candidate = target;
    return m_level;
  }
  if (target != m_candidate) {
    m_candidate = target;
    m_candidate_since_ms = now_ms;
  }
  const bool immediate = target == Level::kUsb && m_level == Level::kOk;
  if (immediate || elapsed(now_ms, m_candidate_since_ms, 2000)) {
    m_level = target;
    m_changed = true;
  }
  return m_level;
}

}  // namespace cq
```

`src/domain/input/nec_decoder.h`:
```cpp
#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {

// NEC decoder fed with falling-edge timestamps (receiver output is active low, falling = mark start).
// Code is MSB-first as received (matches the ELEGOO/IRremote 2.x constants). Inverse command checked.
class NecDecoder {
 public:
  bool feed(uint32_t t_us, uint32_t now_ms, IrEvent& out);

 private:
  enum class State : uint8_t { kIdle, kData };
  State m_state = State::kIdle;
  uint32_t m_last_us = 0;
  uint32_t m_bits = 0;
  uint8_t m_count = 0;
  uint32_t m_last_code = 0;
  uint32_t m_last_frame_ms = 0;
  bool m_have_code = false;
};

}  // namespace cq
```

`src/domain/input/nec_decoder.cpp`:
```cpp
#include "domain/input/nec_decoder.h"

#include "domain/util/math_util.h"

namespace cq {

namespace {
bool within(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }
}  // namespace

bool NecDecoder::feed(uint32_t t_us, uint32_t now_ms, IrEvent& out) {
  const uint32_t dt = t_us - m_last_us;
  m_last_us = t_us;
  if (within(dt, 12500, 14500)) {  // 9 ms mark + 4.5 ms space: frame start
    m_state = State::kData;
    m_bits = 0;
    m_count = 0;
    return false;
  }
  if (within(dt, 10500, 12000)) {  // 9 ms mark + 2.25 ms space: repeat
    m_state = State::kIdle;
    if (!m_have_code || elapsed(now_ms, m_last_frame_ms, 200)) return false;
    m_last_frame_ms = now_ms;
    out = IrEvent{m_last_code, true, now_ms};
    return true;
  }
  if (m_state != State::kData) return false;
  uint32_t bit;
  if (within(dt, 800, 1500)) {
    bit = 0;
  } else if (within(dt, 1800, 2700)) {
    bit = 1;
  } else {
    m_state = State::kIdle;
    return false;
  }
  m_bits = (m_bits << 1) | bit;
  if (++m_count < 32) return false;
  m_state = State::kIdle;
  const uint8_t cmd = static_cast<uint8_t>(m_bits >> 8);
  const uint8_t inv = static_cast<uint8_t>(m_bits);
  if (static_cast<uint8_t>(cmd ^ inv) != 0xFF) return false;
  m_last_code = m_bits;
  m_have_code = true;
  m_last_frame_ms = now_ms;
  out = IrEvent{m_bits, false, now_ms};
  return true;
}

}  // namespace cq
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_sensors`
Expected: `11 Tests 0 Failures`.

- [ ] **Step 5: Ports and drivers (PCINT0 hub for IR + echo, async ADC, button)**

`src/ports/ultrasonic.h`:
```cpp
#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {
namespace port {
namespace ultrasonic {
void begin();
bool trigger();                       // 10 µs pulse + arms the echo capture; false (no ping) while echo is high
EchoState poll(uint16_t& echo_us);    // result of the previous trigger; kPending if it sent no ping
}  // namespace ultrasonic
}  // namespace port
}  // namespace cq
```

`src/ports/ir.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace ir {
void begin();
bool popEdge(uint32_t& t_us);  // falling-edge timestamps captured by the ISR (16-entry ring)
}  // namespace ir
}  // namespace port
}  // namespace cq
```

`src/ports/battery_adc.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace battery_adc {
void begin();
void start();                 // starts one conversion on A3 (no busy wait)
bool ready(uint16_t& adc);    // true once the conversion finished
}  // namespace battery_adc
}  // namespace port
}  // namespace cq
```

`src/ports/button.h`:
```cpp
#pragma once

namespace cq {
namespace port {
namespace button {
void begin();
bool pressed();  // raw level (active low, pull-up); debounced in the domain
}  // namespace button
}  // namespace port
}  // namespace cq
```

`src/drivers/pcint0_hub.cpp`:
```cpp
#include <Arduino.h>
#include <util/atomic.h>

#include "drivers/board_config.h"
#include "ports/ir.h"
#include "ports/ultrasonic.h"

// PCINT0 serves D9 (PB1, IR) and D12 (PB4, echo). Capture only; < 10 µs.
namespace {
constexpr uint8_t kIrBit = _BV(PB1);
constexpr uint8_t kEchoBit = _BV(PB4);
constexpr uint8_t kRing = 16;

volatile uint8_t g_prev_pinb = 0xFF;
volatile uint32_t g_ir_ring[kRing];
volatile uint8_t g_ir_head = 0;
volatile uint8_t g_ir_count = 0;
volatile uint32_t g_echo_rise_us = 0;
volatile uint32_t g_echo_fall_us = 0;
volatile bool g_echo_rose = false;
volatile bool g_echo_fell = false;
uint32_t g_trig_us = 0;  // main context only
bool g_armed = false;    // the last trigger() sent a ping
}  // namespace

ISR(PCINT0_vect) {
  const uint32_t now = micros();
  const uint8_t pinb = PINB;
  const uint8_t changed = pinb ^ g_prev_pinb;
  g_prev_pinb = pinb;
  if ((changed & kIrBit) && !(pinb & kIrBit) && g_ir_count < kRing) {
    g_ir_ring[(g_ir_head + g_ir_count) % kRing] = now;
    ++g_ir_count;
  }
  if (changed & kEchoBit) {
    if (pinb & kEchoBit) {
      g_echo_rise_us = now;
      g_echo_rose = true;
    } else if (g_echo_rose) {
      g_echo_fall_us = now;
      g_echo_fell = true;
    }
  }
}

namespace cq {
namespace port {

namespace ir {
void begin() {
  pinMode(board::kPinIr, INPUT);
  g_prev_pinb = PINB;
  PCMSK0 |= _BV(PCINT1);
  PCICR |= _BV(PCIE0);
}

bool popEdge(uint32_t& t_us) {
  bool ok = false;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    if (g_ir_count > 0) {
      t_us = g_ir_ring[g_ir_head];
      g_ir_head = static_cast<uint8_t>((g_ir_head + 1) % kRing);
      --g_ir_count;
      ok = true;
    }
  }
  return ok;
}
}  // namespace ir

namespace ultrasonic {
void begin() {
  pinMode(board::kPinTrig, OUTPUT);
  digitalWrite(board::kPinTrig, LOW);
  // Pull-up: HC-SR04 echo is push-pull, so harmless when connected; unplugged it reads a steady
  // HIGH (no edges, no PCINT storm) → kNoEcho → US_FAULT.
  pinMode(board::kPinEcho, INPUT_PULLUP);
  g_prev_pinb = PINB;
  PCMSK0 |= _BV(PCINT4);
  PCICR |= _BV(PCIE0);
}

bool trigger() {
  // LLD §9.16: no new ping while the echo is still high (clone modules hold it up to 200 ms).
  if (PINB & kEchoBit) {
    g_armed = false;
    return false;
  }
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    g_echo_rose = false;
    g_echo_fell = false;
  }
  digitalWrite(board::kPinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(board::kPinTrig, LOW);
  g_trig_us = micros();
  g_armed = true;
  return true;
}

EchoState poll(uint16_t& echo_us) {
  uint32_t rise = 0, fall = 0;
  bool rose = false, fell = false;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    rise = g_echo_rise_us;
    fall = g_echo_fall_us;
    rose = g_echo_rose;
    fell = g_echo_fell;
  }
  if (!g_armed) return EchoState::kPending;  // previous trigger() sent no ping
  // No rising edge within 2 ms of the trigger → the sensor did not answer (a failed measurement).
  if (!rose || static_cast<uint32_t>(rise - g_trig_us) > 2000UL) return EchoState::kNoEcho;
  if (!fell) return EchoState::kClear;    // echo still high after 50 ms: nothing in range
  const uint32_t w = fall - rise;
  if (w > 25000UL) return EchoState::kClear;  // LLD §9.16: echo high > 25 ms = nothing in range
  echo_us = static_cast<uint16_t>(w);
  return EchoState::kReady;
}
}  // namespace ultrasonic

}  // namespace port
}  // namespace cq
```

`src/drivers/battery_adc_uno.cpp`:
```cpp
#include <avr/io.h>

#include "ports/battery_adc.h"

namespace cq {
namespace port {
namespace battery_adc {

void begin() { ADMUX = _BV(REFS0) | 3; }  // AVcc reference, channel A3 (core already set ADEN, /128)

void start() {
  ADMUX = _BV(REFS0) | 3;
  ADCSRA |= _BV(ADSC);
}

bool ready(uint16_t& adc) {
  if (ADCSRA & _BV(ADSC)) return false;
  adc = ADC;
  return true;
}

}  // namespace battery_adc
}  // namespace port
}  // namespace cq
```

`src/drivers/button_uno.cpp`:
```cpp
#include <Arduino.h>

#include "drivers/board_config.h"
#include "ports/button.h"

namespace cq {
namespace port {
namespace button {
void begin() { pinMode(board::kPinButton, INPUT_PULLUP); }
bool pressed() { return digitalRead(board::kPinButton) == LOW; }
}  // namespace button
}  // namespace port
}  // namespace cq
```

- [ ] **Step 6: Sensor tasks and wiring**

Add to `src/app/app.h` includes:
```cpp
#include "domain/estimation/battery_monitor.h"
#include "domain/estimation/ultrasonic_filter.h"
#include "domain/input/nec_decoder.h"
```
Members:
```cpp
  // sensors
  UltrasonicFilter us;
  BatteryMonitor batt;
  NecDecoder nec;
  IrEvent ir_event{};
  bool ir_pending = false;  // consumed by the IR controller (Task 14)
```

`src/app/sensor_tasks.cpp`:
```cpp
#include "app/app.h"
#include "ports/battery_adc.h"
#include "ports/button.h"
#include "ports/clock.h"
#include "ports/ir.h"
#include "ports/ultrasonic.h"

namespace cq {
namespace app {
namespace {

void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_app.fast_flags = on ? static_cast<uint16_t>(g_app.fast_flags | m) : static_cast<uint16_t>(g_app.fast_flags & ~m);
}

void setSlow(proto::SlowFlagBit bit, bool on) {
  const uint32_t m = 1UL << static_cast<uint8_t>(bit);
  g_app.slow_flags = on ? (g_app.slow_flags | m) : (g_app.slow_flags & ~m);
}

}  // namespace

void sensorsInit() {
  port::ultrasonic::begin();
  port::ir::begin();
  port::battery_adc::begin();
  port::button::begin();
  port::ultrasonic::trigger();
  port::battery_adc::start();
}

// 20 Hz: read the previous ping, trigger the next one (skipped while the echo is still high).
void ultrasonicTask() {
  uint16_t echo_us = 0;
  const EchoState st = port::ultrasonic::poll(echo_us);
  const uint32_t now = port::clock::nowMs();
  switch (g_app.us.onReading(st, echo_us, g_app.settings.global().ultrasonic_offset, now)) {
    case UltrasonicFilter::Change::kFault: g_app.events.push(proto::EventKind::kUsFault); break;
    case UltrasonicFilter::Change::kRecovered:
    case UltrasonicFilter::Change::kNone: break;
  }
  setFast(proto::FastFlagBit::kUsFault, g_app.us.fault());
  g_app.fast.dist_mm = g_app.us.distMm();
  g_app.fast.closing_cm_s = g_app.us.closingCmS();
  port::ultrasonic::trigger();
}

// 10 Hz battery.
void batteryTask() {
  uint16_t adc = 0;
  if (!port::battery_adc::ready(adc)) return;
  const GlobalSettings& gs = g_app.settings.global();
  const BatteryMonitor::Level lvl =
      g_app.batt.update(adc, gs.batt_mv_per_count, gs.batt_low, gs.batt_crit, port::clock::nowMs());
  if (g_app.batt.changed()) {
    if (lvl == BatteryMonitor::Level::kLow) g_app.events.push(proto::EventKind::kBattLow, g_app.batt.mv());
    if (lvl == BatteryMonitor::Level::kCritical) g_app.events.push(proto::EventKind::kBattCritical, g_app.batt.mv());
  }
  setSlow(proto::SlowFlagBit::kBattLow, lvl == BatteryMonitor::Level::kLow);
  setSlow(proto::SlowFlagBit::kBattCritical, lvl == BatteryMonitor::Level::kCritical);
  g_app.slow.batt_mv = g_app.batt.mv();
  port::battery_adc::start();
}

// 200 Hz: drain IR edges into the decoder.
void irTask() {
  uint32_t t_us = 0;
  IrEvent e{};
  while (port::ir::popEdge(t_us)) {
    if (g_app.nec.feed(t_us, port::clock::nowMs(), e)) {
      g_app.ir_event = e;
      g_app.ir_pending = true;
    }
  }
}

}  // namespace app
}  // namespace cq
```

`src/app/comms.cpp` — in `telemetrySlowDue()` add before `g_app.slow_due = true;`:
```cpp
  s.flags2 = g_app.slow_flags;
```

In `src/app/tasks.h` add before `// TASK-DECLS-END`:
```cpp
void sensorsInit();     // boot: ultrasonic, IR, battery ADC, button ports
void ultrasonicTask();  // 20 Hz
void batteryTask();     // 10 Hz
void irTask();          // 200 Hz
```
In `src/main.cpp`: before `// SETUP-END` add `cq::app::sensorsInit();`; rows before `// TASKS-END`:
```cpp
    {cq::app::irTask, 5000UL, 0},
    {cq::app::ultrasonicTask, 50000UL, 0},
    {cq::app::batteryTask, 100000UL, 0},
```

- [ ] **Step 7: Build, flash, check on the stand**

Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py && pio run -e uno -t upload`
Then `… uno_smoke.py --port PORT watch --seconds 15`.
Expected: hand at 30 cm in front of the sensor → `dist≈300` (± 10 mm); nothing in front → `dist=65535`; open room / nothing in range for a minute → never `US_FAULT`; unplug the ultrasonic sensor → within 0.5 s `EVENT US_FAULT` (0.15 s with the echo line low, 0.4 s when the pull-up holds it high) and `dist=65534`, flags bit 12; plug it back → `dist` valid again on the next ping. Add to the hardware checklist: with a clone module that holds the echo 200 ms, no `US_FAULT` in an open room. With the battery switched on: `SLOW batt≈7400-8400mV`; USB only: `batt` < 3000 and no `BATT_*` event. If the battery reading disagrees with a multimeter by more than 2 %, set `batt_mv_per_count` = 4050 × (meter mV / reported mV) with `… setting batt_mv_per_count <value> --save`.

- [ ] **Step 8: Commit**

```bash
git add firmware/uno
git commit -m "feat(uno): ultrasonic/IR via PCINT0 hub, NEC decoder, async battery ADC, button port"
```

---

### Task 6: Safety core — PwmOutput, envelope, arbitrator, supervisor, motor driver, open-loop DRIVE/STOP

**Files:**
- Create: `src/types/pwm_output.h`; `src/domain/safety/envelope.{h,cpp}`, `safety_supervisor.{h,cpp}`; `src/domain/arbitration/arbitrator.{h,cpp}`; `src/ports/motor.h`; `src/drivers/motor_uno.cpp`; `src/app/motion_api.h`; `src/app/control_task.cpp` (v1, replaced in Task 11)
- Modify: `src/app/app.h`, `src/app/tasks.h`, `src/app/routes.cpp`, `src/app/apply_settings.cpp`, `src/main.cpp`, `tools/smoke/uno_smoke.py`
- Test: `test/test_safety/test_main.cpp`

**Interfaces:**
- Consumes: `proto::Owner`, `proto::AckStatus`, `proto::EventKind`, `proto::FastFlagBit`; App sensor state (Tasks 4–5).
- Produces:
  - `cq::PwmOutput`: `static PwmOutput zero()`, `int16_t left() const`, `int16_t right() const`, `bool enabled() const` (false = driver standby/coast); non-zero construction only via `friend class SafetySupervisor`.
  - `cq::Envelope`: `proto::AckStatus set(uint8_t speed_cap, uint8_t accel_cap, uint8_t lock_brake)`, `proto::AckStatus setBrake(bool on)`, `uint8_t speedCap() const`, `uint8_t accelCap() const`, `bool lockBrake() const`, `bool brakeOn() const`, `bool active() const`.
  - `cq::Arbitrator`: `enum class Action : uint8_t { kNone, kDeadmanStop, kLinkLostStop, kIrReleased }`; `struct Config { uint16_t deadman_ms; uint16_t ir_handback_ms; }`; `void setSynced(bool)` (from `LinkSession::synced()` every control tick; boot = not synced), `proto::AckStatus requestNetwork(bool teleop, uint32_t now_ms)` (BUSY before sync — LLD §9.4), `void onNetworkStop()`, `bool onIrInput(uint32_t now_ms)` (true = owner changed to IR; works without sync), `void releaseIr()` (IR OK key: owner → None), `proto::AckStatus beginCalibration()` (BUSY before sync), `void endCalibration()`, `Action tick(uint32_t now_ms, bool link_alive, bool primitive_busy, const Config& cfg)` (also releases Network → None after `kNetworkIdleMs` = 1 s without a network motion command while no primitive runs — LLD §9.18), `proto::Owner owner() const`, `bool takeOwnerChanged()`.
  - `cq::SafetyInputs { bool lifted, tilted, batt_critical, brake_on; uint16_t dist_mm; uint8_t stop_cm; uint8_t stall_mask; }` (`stall_mask` bit 0 left, bit 1 right: that track's output is cut — LLD §9.6; fed by Task 7), `cq::SafetyStatus { bool blocked, halted, coast; }`, `cq::SafetySupervisor::apply(int16_t pwm_l, int16_t pwm_r, const SafetyInputs&, SafetyStatus&) -> PwmOutput`. `dist_mm = 0xFFFE` (sensor fault) never blocks: the brake is unavailable and the degraded 50 % cap is applied to the targets by the `TargetLimiter` (Task 8, wired in Task 11).
  - Port `cq::port::motor`: `begin()`, `apply(const PwmOutput&)` — included only by `app/control_task.cpp`.
  - `app/tasks.h`: `controlInit()`, `controlTask()`; `app/motion_api.h`: route handlers `onDrive`, `onStop`, `onBrake`, `onEnvelope`, `onOwnerHint` (signature `proto::AckStatus (const uint8_t*, uint8_t)`).
  - App members `int16_t req_pwm_l, req_pwm_r` (PWM asked of the supervisor, before the gate — the direction of a stalled track for Task 7) next to `out_pwm_l/r`.
  - Smoke commands `drive V W --seconds S`, `stop`, `envelope CAP ACCEL LOCK`, `deadman`.

- [ ] **Step 1: Write the failing safety tests**

`test/test_safety/test_main.cpp`:
```cpp
#include <type_traits>
#include <unity.h>

#include "domain/arbitration/arbitrator.h"
#include "domain/safety/envelope.h"
#include "domain/safety/safety_supervisor.h"
#include "types/pwm_output.h"

using namespace cq;

void setUp() {}
void tearDown() {}

static_assert(!std::is_constructible<PwmOutput, int16_t, int16_t, bool>::value,
              "only SafetySupervisor may build a moving PwmOutput");

namespace {
const Arbitrator::Config kCfg{300, 3000};
SafetyInputs clearInputs() { return SafetyInputs{false, false, false, true, 0xFFFF, 15, 0}; }
Arbitrator syncedArbitrator() {
  Arbitrator a;
  a.setSynced(true);  // HELLO_ACK received
  return a;
}
}  // namespace

void test_ir_owner_ignores_link_loss() {
  Arbitrator a;  // never synced: IR works with no ESP32
  TEST_ASSERT_TRUE(a.onIrInput(0));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kIr), static_cast<int>(a.owner()));
  // ESP32 absent: link never alive
  for (uint32_t t = 0; t < 2000; t += 10) {
    a.onIrInput(t);  // user keeps pressing
    TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(a.tick(t, false, false, kCfg)));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kIr), static_cast<int>(a.owner()));
}

void test_ir_hands_back_after_idle() {
  Arbitrator a;
  a.onIrInput(0);
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(a.tick(2999, true, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kIrReleased), static_cast<int>(a.tick(3000, true, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(a.owner()));
}

void test_network_rejected_while_ir_owns() {
  Arbitrator a = syncedArbitrator();
  a.onIrInput(0);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kNotOwner), static_cast<int>(a.requestNetwork(true, 10)));
}

void test_link_loss_stops_network_owner() {
  Arbitrator a = syncedArbitrator();
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(a.requestNetwork(true, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kLinkLostStop), static_cast<int>(a.tick(50, false, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(a.owner()));
  Arbitrator c = syncedArbitrator();  // calibration motion was started from the network: stops too
  c.beginCalibration();
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kLinkLostStop), static_cast<int>(c.tick(50, false, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(c.owner()));
}

void test_deadman_only_for_teleop() {
  Arbitrator a = syncedArbitrator();
  a.requestNetwork(true, 0);
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(a.tick(299, true, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kDeadmanStop), static_cast<int>(a.tick(300, true, false, kCfg)));
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(a.tick(310, true, false, kCfg)));  // once
  Arbitrator b = syncedArbitrator();
  b.requestNetwork(false, 0);  // TURN/MOVE primitive running: no stream, no deadman, no idle release
  for (uint32_t t = 0; t < 10000; t += 10) {
    TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kNone), static_cast<int>(b.tick(t, true, true, kCfg)));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNetwork), static_cast<int>(b.owner()));
}

void test_network_motion_busy_until_synced() {
  Arbitrator a;  // boot: no HELLO_ACK yet (LLD §9.4)
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kBusy), static_cast<int>(a.requestNetwork(true, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kBusy), static_cast<int>(a.beginCalibration()));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(a.owner()));
  TEST_ASSERT_TRUE(a.onIrInput(10));  // IR is allowed before sync
  a.releaseIr();
  a.setSynced(true);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(a.requestNetwork(true, 20)));
}

void test_network_released_after_1s_idle() {
  Arbitrator a = syncedArbitrator();
  a.requestNetwork(true, 0);
  a.takeOwnerChanged();
  TEST_ASSERT_EQUAL(static_cast<int>(Arbitrator::Action::kDeadmanStop), static_cast<int>(a.tick(300, true, false, kCfg)));
  a.tick(999, true, false, kCfg);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNetwork), static_cast<int>(a.owner()));
  a.tick(1000, true, false, kCfg);  // LLD §9.18: 1 s without a network motion command, primitives idle
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(a.owner()));
  TEST_ASSERT_TRUE(a.takeOwnerChanged());
  Arbitrator b = syncedArbitrator();
  b.requestNetwork(false, 0);  // a 5 s MOVE keeps the owner
  for (uint32_t t = 0; t < 5000; t += 10) b.tick(t, true, true, kCfg);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNetwork), static_cast<int>(b.owner()));
  b.tick(5000, true, false, kCfg);  // primitive done, last command long ago → released
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kNone), static_cast<int>(b.owner()));
}

void test_calibration_busy_and_ir_override() {
  Arbitrator a = syncedArbitrator();
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(a.beginCalibration()));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kBusy), static_cast<int>(a.requestNetwork(true, 0)));
  TEST_ASSERT_TRUE(a.onIrInput(5));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::Owner::kIr), static_cast<int>(a.owner()));
}

void test_envelope_lock_forces_brake() {
  Envelope e;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(e.setBrake(false)));
  TEST_ASSERT_FALSE(e.brakeOn());
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(e.set(20, 50, 1)));
  TEST_ASSERT_TRUE(e.brakeOn());
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kEnvelopeLocked), static_cast<int>(e.setBrake(false)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand), static_cast<int>(e.set(101, 0, 0)));
  TEST_ASSERT_TRUE(e.active());
}

void test_supervisor_blocks_forward_only() {
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.dist_mm = 100;  // 10 cm < 15 cm
  PwmOutput o = s.apply(150, 150, in, st);
  TEST_ASSERT_EQUAL(0, o.left());
  TEST_ASSERT_TRUE(st.blocked);
  o = s.apply(-150, -150, in, st);
  TEST_ASSERT_EQUAL(-150, o.left());
  o = s.apply(-150, 150, in, st);  // spin in place is allowed
  TEST_ASSERT_EQUAL(150, o.right());
  in.brake_on = false;
  o = s.apply(150, 150, in, st);
  TEST_ASSERT_EQUAL(150, o.left());
  TEST_ASSERT_FALSE(st.blocked);
}

void test_fault_range_does_not_block() {
  // 0xFFFE = ultrasonic fault: the obstacle brake is unavailable (no phantom obstacle). The degraded
  // 50 % cap is applied to the targets by the TargetLimiter (Task 8), not here.
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.dist_mm = 0xFFFE;
  PwmOutput o = s.apply(150, 150, in, st);
  TEST_ASSERT_EQUAL(150, o.left());
  TEST_ASSERT_EQUAL(150, o.right());
  TEST_ASSERT_FALSE(st.blocked);
}

void test_supervisor_cuts_stalled_track() {
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.stall_mask = 0x01;  // left stalled (LLD §9.6): only that track is cut
  PwmOutput o = s.apply(150, 150, in, st);
  TEST_ASSERT_EQUAL(0, o.left());
  TEST_ASSERT_EQUAL(150, o.right());
  TEST_ASSERT_TRUE(o.enabled());
  in.stall_mask = 0x02;
  o = s.apply(-150, 150, in, st);
  TEST_ASSERT_EQUAL(-150, o.left());
  TEST_ASSERT_EQUAL(0, o.right());
}

void test_supervisor_lift_coasts_tilt_halts_clamps() {
  SafetySupervisor s;
  SafetyStatus st{};
  SafetyInputs in = clearInputs();
  in.lifted = true;
  PwmOutput o = s.apply(200, 200, in, st);
  TEST_ASSERT_FALSE(o.enabled());
  TEST_ASSERT_TRUE(st.coast);
  in = clearInputs();
  in.tilted = true;
  o = s.apply(200, 200, in, st);
  TEST_ASSERT_EQUAL(0, o.right());
  TEST_ASSERT_TRUE(st.halted);
  in = clearInputs();
  o = s.apply(400, -400, in, st);
  TEST_ASSERT_EQUAL(255, o.left());
  TEST_ASSERT_EQUAL(-255, o.right());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ir_owner_ignores_link_loss);
  RUN_TEST(test_ir_hands_back_after_idle);
  RUN_TEST(test_network_rejected_while_ir_owns);
  RUN_TEST(test_link_loss_stops_network_owner);
  RUN_TEST(test_deadman_only_for_teleop);
  RUN_TEST(test_network_motion_busy_until_synced);
  RUN_TEST(test_network_released_after_1s_idle);
  RUN_TEST(test_calibration_busy_and_ir_override);
  RUN_TEST(test_envelope_lock_forces_brake);
  RUN_TEST(test_supervisor_blocks_forward_only);
  RUN_TEST(test_fault_range_does_not_block);
  RUN_TEST(test_supervisor_cuts_stalled_track);
  RUN_TEST(test_supervisor_lift_coasts_tilt_halts_clamps);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_safety`
Expected: FAIL — `domain/arbitration/arbitrator.h: No such file or directory`.

- [ ] **Step 3: Implement PwmOutput, envelope, arbitrator, supervisor**

`src/types/pwm_output.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

class SafetySupervisor;

// The only value the motor port accepts. Anything but zero() must come from SafetySupervisor.
class PwmOutput {
 public:
  static PwmOutput zero() { return PwmOutput(0, 0, true); }
  int16_t left() const { return m_left; }
  int16_t right() const { return m_right; }
  bool enabled() const { return m_enabled; }  // false = driver standby (tracks coast)

 private:
  PwmOutput(int16_t left, int16_t right, bool enabled) : m_left(left), m_right(right), m_enabled(enabled) {}
  int16_t m_left;
  int16_t m_right;
  bool m_enabled;
  friend class SafetySupervisor;
};

}  // namespace cq
```

`src/domain/safety/envelope.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

// Safety envelope from the ESP32 (child-play etc.). lock_brake forces the obstacle brake on.
class Envelope {
 public:
  proto::AckStatus set(uint8_t speed_cap, uint8_t accel_cap, uint8_t lock_brake);
  proto::AckStatus setBrake(bool on);
  uint8_t speedCap() const { return m_speed_cap; }
  uint8_t accelCap() const { return m_accel_cap; }  // %/s, 0 = no cap
  bool lockBrake() const { return m_lock_brake; }
  bool brakeOn() const { return m_brake_on || m_lock_brake; }
  bool active() const { return m_speed_cap < 100 || m_accel_cap != 0 || m_lock_brake; }

 private:
  uint8_t m_speed_cap = 100;
  uint8_t m_accel_cap = 0;
  bool m_lock_brake = false;
  bool m_brake_on = true;
};

}  // namespace cq
```

`src/domain/safety/envelope.cpp`:
```cpp
#include "domain/safety/envelope.h"

namespace cq {

proto::AckStatus Envelope::set(uint8_t speed_cap, uint8_t accel_cap, uint8_t lock_brake) {
  if (speed_cap > 100 || lock_brake > 1) return proto::AckStatus::kInvalidCommand;
  m_speed_cap = speed_cap;
  m_accel_cap = accel_cap;
  m_lock_brake = lock_brake != 0;
  return proto::AckStatus::kOk;
}

proto::AckStatus Envelope::setBrake(bool on) {
  if (!on && m_lock_brake) return proto::AckStatus::kEnvelopeLocked;
  m_brake_on = on;
  return proto::AckStatus::kOk;
}

}  // namespace cq
```

`src/domain/arbitration/arbitrator.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

// Motion ownership (LLD §9.2): IR > calibration > network. Link loss stops only network/calibration
// motion; the deadman applies only to streamed network teleop (DRIVE/DRIVE_RATE), never primitives.
// Before sync (HELLO_ACK) network motion and calibration get BUSY; IR works (LLD §9.4). Network → None
// after 1 s without a network motion command while no primitive runs (LLD §9.18).
class Arbitrator {
 public:
  enum class Action : uint8_t { kNone, kDeadmanStop, kLinkLostStop, kIrReleased };
  struct Config {
    uint16_t deadman_ms;
    uint16_t ir_handback_ms;
  };
  static constexpr uint16_t kNetworkIdleMs = 1000;

  void setSynced(bool synced) { m_synced = synced; }
  proto::AckStatus requestNetwork(bool teleop, uint32_t now_ms);
  void onNetworkStop();
  bool onIrInput(uint32_t now_ms);
  void releaseIr() {
    if (m_owner == proto::Owner::kIr) setOwner(proto::Owner::kNone);
  }
  proto::AckStatus beginCalibration();
  void endCalibration();
  Action tick(uint32_t now_ms, bool link_alive, bool primitive_busy, const Config& cfg);
  proto::Owner owner() const { return m_owner; }
  bool takeOwnerChanged() {
    const bool c = m_changed;
    m_changed = false;
    return c;
  }

 private:
  void setOwner(proto::Owner o) {
    if (o != m_owner) m_changed = true;
    m_owner = o;
  }
  proto::Owner m_owner = proto::Owner::kNone;
  bool m_changed = false;
  bool m_synced = false;
  bool m_teleop = false;
  uint32_t m_last_teleop_ms = 0;
  uint32_t m_last_network_ms = 0;
  uint32_t m_last_ir_ms = 0;
};

}  // namespace cq
```

`src/domain/arbitration/arbitrator.cpp`:
```cpp
#include "domain/arbitration/arbitrator.h"

#include "domain/util/math_util.h"

namespace cq {

proto::AckStatus Arbitrator::requestNetwork(bool teleop, uint32_t now_ms) {
  if (m_owner == proto::Owner::kIr) return proto::AckStatus::kNotOwner;
  if (m_owner == proto::Owner::kCalibration || !m_synced) return proto::AckStatus::kBusy;
  setOwner(proto::Owner::kNetwork);
  m_teleop = teleop;
  m_last_network_ms = now_ms;
  if (teleop) m_last_teleop_ms = now_ms;
  return proto::AckStatus::kOk;
}

void Arbitrator::onNetworkStop() {
  m_teleop = false;
  if (m_owner == proto::Owner::kNetwork) setOwner(proto::Owner::kNone);
}

bool Arbitrator::onIrInput(uint32_t now_ms) {
  m_last_ir_ms = now_ms;
  if (m_owner == proto::Owner::kIr) return false;
  m_teleop = false;
  setOwner(proto::Owner::kIr);
  return true;
}

proto::AckStatus Arbitrator::beginCalibration() {
  if (m_owner == proto::Owner::kIr) return proto::AckStatus::kNotOwner;
  if (!m_synced) return proto::AckStatus::kBusy;  // calibration is started from the network
  m_teleop = false;
  setOwner(proto::Owner::kCalibration);
  return proto::AckStatus::kOk;
}

void Arbitrator::endCalibration() {
  if (m_owner == proto::Owner::kCalibration) setOwner(proto::Owner::kNone);
}

Arbitrator::Action Arbitrator::tick(uint32_t now_ms, bool link_alive, bool primitive_busy, const Config& cfg) {
  switch (m_owner) {
    case proto::Owner::kIr:
      if (elapsed(now_ms, m_last_ir_ms, cfg.ir_handback_ms)) {
        setOwner(proto::Owner::kNone);
        return Action::kIrReleased;
      }
      return Action::kNone;
    case proto::Owner::kNetwork:
    case proto::Owner::kCalibration:
      if (!link_alive) {
        m_teleop = false;
        setOwner(proto::Owner::kNone);
        return Action::kLinkLostStop;
      }
      if (m_teleop && elapsed(now_ms, m_last_teleop_ms, cfg.deadman_ms)) {
        m_teleop = false;
        return Action::kDeadmanStop;
      }
      if (m_owner == proto::Owner::kNetwork && !m_teleop && !primitive_busy &&
          elapsed(now_ms, m_last_network_ms, kNetworkIdleMs)) {
        setOwner(proto::Owner::kNone);  // bookkeeping only (CONTROL_CHANGED); nothing is moving
      }
      return Action::kNone;
    case proto::Owner::kNone:
      return Action::kNone;
  }
  return Action::kNone;
}

}  // namespace cq
```

`src/domain/safety/safety_supervisor.h`:
```cpp
#pragma once
#include <stdint.h>

#include "types/pwm_output.h"

namespace cq {

struct SafetyInputs {
  bool lifted;
  bool tilted;
  bool batt_critical;
  bool brake_on;       // obstacle brake enabled (envelope may force it)
  uint16_t dist_mm;    // 0xFFFF clear, 0xFFFE sensor fault (brake unavailable, never blocks)
  uint8_t stop_cm;
  uint8_t stall_mask;  // bit 0 left, bit 1 right: stalled track, output cut (LLD §9.6)
};

struct SafetyStatus {
  bool blocked;
  bool halted;
  bool coast;
};

// Last gate before the motor port. Owns PwmOutput construction.
class SafetySupervisor {
 public:
  PwmOutput apply(int16_t pwm_l, int16_t pwm_r, const SafetyInputs& in, SafetyStatus& st) const;
};

}  // namespace cq
```

`src/domain/safety/safety_supervisor.cpp`:
```cpp
#include "domain/safety/safety_supervisor.h"

namespace cq {

namespace {
int16_t clamp255(int16_t v) { return v > 255 ? 255 : (v < -255 ? -255 : v); }
}  // namespace

PwmOutput SafetySupervisor::apply(int16_t pwm_l, int16_t pwm_r, const SafetyInputs& in, SafetyStatus& st) const {
  st = SafetyStatus{false, false, false};
  if (in.lifted) {
    st.coast = true;
    return PwmOutput(0, 0, false);  // tracks free-wheel while carried
  }
  if (in.tilted || in.batt_critical) {
    st.halted = true;
    return PwmOutput(0, 0, true);
  }
  const int16_t l = (in.stall_mask & 0x01) ? 0 : clamp255(pwm_l);
  const int16_t r = (in.stall_mask & 0x02) ? 0 : clamp255(pwm_r);
  const bool range_valid = in.dist_mm < 0xFFFE;
  const bool too_close = range_valid && in.dist_mm < static_cast<uint16_t>(in.stop_cm) * 10u;
  const int16_t forward = static_cast<int16_t>((l + r) / 2);
  if (in.brake_on && too_close && forward > 10) {
    st.blocked = true;
    return PwmOutput(0, 0, true);
  }
  return PwmOutput(l, r, true);
}

}  // namespace cq
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_safety`
Expected: `13 Tests 0 Failures`.

- [ ] **Step 5: Motor port and driver (both shields)**

`src/ports/motor.h`:
```cpp
#pragma once
#include "types/pwm_output.h"

namespace cq {
namespace port {
namespace motor {
void begin();                       // outputs low, driver in standby
void apply(const PwmOutput& out);   // sign = direction, |value| = duty
}  // namespace motor
}  // namespace port
}  // namespace cq
```

`src/drivers/motor_uno.cpp`:
```cpp
#include <Arduino.h>

#include "drivers/board_config.h"
#include "ports/motor.h"

namespace cq {
namespace port {
namespace motor {
namespace {

void side(uint8_t pwm_pin, uint8_t dir_pin, bool fwd_high, int16_t value) {
  const bool forward = value >= 0;
  digitalWrite(dir_pin, forward == fwd_high ? HIGH : LOW);
  analogWrite(pwm_pin, static_cast<uint8_t>(forward ? value : -value));
}

}  // namespace

void begin() {
  pinMode(board::kPinPwmLeft, OUTPUT);
  pinMode(board::kPinPwmRight, OUTPUT);
  pinMode(board::kPinDirLeft, OUTPUT);
  pinMode(board::kPinDirRight, OUTPUT);
  analogWrite(board::kPinPwmLeft, 0);
  analogWrite(board::kPinPwmRight, 0);
  if (board::kHasStandby) {
    pinMode(board::kPinStandby, OUTPUT);
    digitalWrite(board::kPinStandby, LOW);
  }
}

void apply(const PwmOutput& out) {
  if (board::kHasStandby) digitalWrite(board::kPinStandby, out.enabled() ? HIGH : LOW);
  const int16_t l = out.enabled() ? out.left() : 0;
  const int16_t r = out.enabled() ? out.right() : 0;
  side(board::kPinPwmLeft, board::kPinDirLeft, board::kDirLeftForwardHigh, l);
  side(board::kPinPwmRight, board::kPinDirRight, board::kDirRightForwardHigh, r);
}

}  // namespace motor
}  // namespace port
}  // namespace cq
```

- [ ] **Step 6: Control task v1 (open loop) and motion routes**

Add to `src/app/app.h` includes and members:
```cpp
#include "domain/arbitration/arbitrator.h"
#include "domain/safety/envelope.h"
#include "domain/safety/safety_supervisor.h"
```
```cpp
  // safety / arbitration
  Envelope envelope;
  Arbitrator arbitrator;
  SafetySupervisor supervisor;
  SafetyStatus safety{};
  proto::OwnerHintValue owner_hint = proto::OwnerHintValue::kNone;
  int8_t teleop_v = 0;   // DRIVE, %
  int8_t teleop_w = 0;
  int16_t req_pwm_l = 0;  // asked of the supervisor (before the gate): a stalled track's direction
  int16_t req_pwm_r = 0;
  int16_t out_pwm_l = 0;  // applied by the supervisor
  int16_t out_pwm_r = 0;
  uint32_t last_motion_ms = 0;
```

`src/app/motion_api.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace app {

// Route handlers (controlInit/controlTask are declared in app/tasks.h).
proto::AckStatus onDrive(const uint8_t* p, uint8_t len);
proto::AckStatus onStop(const uint8_t* p, uint8_t len);
proto::AckStatus onBrake(const uint8_t* p, uint8_t len);
proto::AckStatus onEnvelope(const uint8_t* p, uint8_t len);
proto::AckStatus onOwnerHint(const uint8_t* p, uint8_t len);

}  // namespace app
}  // namespace cq
```

`src/app/control_task.cpp` (v1):
```cpp
#include "app/app.h"
#include "app/motion_api.h"
#include "app/tasks.h"
#include "domain/util/math_util.h"
#include "ports/clock.h"
#include "ports/motor.h"

namespace cq {
namespace app {
namespace {

// Same 3-line helper as in imu_task/sensor_tasks: accepted duplication (file-local, no shared state).
void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_app.fast_flags = on ? static_cast<uint16_t>(g_app.fast_flags | m) : static_cast<uint16_t>(g_app.fast_flags & ~m);
}

void zeroTeleop() {
  g_app.teleop_v = 0;
  g_app.teleop_w = 0;
}

void rejectEvent(uint8_t frame_type, uint8_t src, proto::AckStatus st) {
  if (g_app.rejects.allow(src, port::clock::nowMs())) {
    g_app.events.push(proto::EventKind::kRejected, frame_type, static_cast<int16_t>(st));
  }
}

}  // namespace

void controlInit() {
  port::motor::begin();
  port::motor::apply(PwmOutput::zero());
}

proto::AckStatus onDrive(const uint8_t* p, uint8_t len) {
  proto::DriveMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = g_app.arbitrator.requestNetwork(true, port::clock::nowMs());
  if (st != proto::AckStatus::kOk) {
    rejectEvent(static_cast<uint8_t>(proto::FrameType::kDrive), m.src, st);
    return st;
  }
  g_app.teleop_v = m.v;
  g_app.teleop_w = m.w;
  return st;
}

proto::AckStatus onStop(const uint8_t* p, uint8_t len) {
  proto::StopMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  zeroTeleop();
  g_app.arbitrator.onNetworkStop();
  return proto::AckStatus::kOk;
}

proto::AckStatus onBrake(const uint8_t* p, uint8_t len) {
  proto::BrakeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  return g_app.envelope.setBrake(m.on != 0);
}

proto::AckStatus onEnvelope(const uint8_t* p, uint8_t len) {
  proto::EnvelopeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = g_app.envelope.set(m.speed_cap, m.accel_cap, m.lock_brake);
  if (st == proto::AckStatus::kOk) {
    const uint16_t packed = static_cast<uint16_t>(static_cast<uint16_t>(m.speed_cap) |
                                                  static_cast<uint16_t>(static_cast<uint16_t>(m.accel_cap) << 8u));
    g_app.events.push(proto::EventKind::kEnvelopeChanged, static_cast<int16_t>(packed), m.lock_brake);
    g_app.slow.env_speed_cap = m.speed_cap;
    g_app.slow.env_accel_cap = m.accel_cap;
    g_app.slow.env_lock_brake = m.lock_brake;
  }
  return st;
}

proto::AckStatus onOwnerHint(const uint8_t* p, uint8_t len) {
  proto::OwnerHintMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.owner_hint = static_cast<proto::OwnerHintValue>(m.owner);
  return proto::AckStatus::kOk;
}

void controlTask() {
  const uint32_t now = port::clock::nowMs();
  const GlobalSettings& gs = g_app.settings.global();
  const Arbitrator::Config cfg{gs.deadman, gs.ir_handback};
  // Network motion is BUSY until HELLO_ACK (LLD §9.4); a DRIVE in the same 10 ms as the HELLO_ACK may
  // still get BUSY — the next one of the stream is accepted.
  g_app.arbitrator.setSynced(g_app.link.synced());
  switch (g_app.arbitrator.tick(now, g_app.link.alive(now), false /* v1: no primitives yet */, cfg)) {
    case Arbitrator::Action::kDeadmanStop:
      zeroTeleop();
      g_app.events.push(proto::EventKind::kDeadmanStop);
      break;
    case Arbitrator::Action::kLinkLostStop:
    case Arbitrator::Action::kIrReleased:
      zeroTeleop();
      break;
    case Arbitrator::Action::kNone:
      break;
  }
  if (g_app.arbitrator.takeOwnerChanged()) {
    g_app.events.push(proto::EventKind::kControlChanged, static_cast<int16_t>(g_app.arbitrator.owner()));
  }

  // Only a network owner's DRIVE values count: an idle release, IR or calibration never resumes an old DRIVE.
  if (g_app.arbitrator.owner() != proto::Owner::kNetwork) zeroTeleop();

  // v1 open loop: arcade mix, envelope cap in %, linear % → PWM.
  const float cap = g_app.envelope.speedCap();
  float l = clampf(static_cast<float>(g_app.teleop_v) - g_app.teleop_w, -100.0f, 100.0f);
  float r = clampf(static_cast<float>(g_app.teleop_v) + g_app.teleop_w, -100.0f, 100.0f);
  l = clampf(l, -cap, cap);
  r = clampf(r, -cap, cap);
  const int16_t pwm_l = static_cast<int16_t>(l * 2.55f);
  const int16_t pwm_r = static_cast<int16_t>(r * 2.55f);

  g_app.req_pwm_l = pwm_l;
  g_app.req_pwm_r = pwm_r;
  const SafetyInputs in{g_app.lift.lifted(), g_app.lift.tilted(),
                        g_app.batt.level() == BatteryMonitor::Level::kCritical, g_app.envelope.brakeOn(),
                        g_app.us.distMm(), gs.obstacle_stop, 0 /* stall_mask: Task 7 */};
  const bool was_blocked = g_app.safety.blocked;
  const PwmOutput out = g_app.supervisor.apply(pwm_l, pwm_r, in, g_app.safety);
  port::motor::apply(out);

  if (g_app.safety.blocked && !was_blocked) g_app.events.push(proto::EventKind::kBlocked, static_cast<int16_t>(g_app.us.distMm()));
  if (!g_app.safety.blocked && was_blocked) g_app.events.push(proto::EventKind::kUnblocked);
  setFast(proto::FastFlagBit::kBlocked, g_app.safety.blocked);
  setFast(proto::FastFlagBit::kHalted, g_app.safety.halted);
  setFast(proto::FastFlagBit::kLocked, g_app.envelope.lockBrake());
  setFast(proto::FastFlagBit::kSynced, g_app.link.synced());

  g_app.out_pwm_l = out.left();
  g_app.out_pwm_r = out.right();
  if (out.left() != 0 || out.right() != 0) g_app.last_motion_ms = now;
  g_app.moving = !elapsed(now, g_app.last_motion_ms, 300);
  g_app.stationary = !g_app.moving;

  proto::TelemFastMsg& f = g_app.fast;
  f.pwm_l = out.left();
  f.pwm_r = out.right();
  f.spd_cmd_l = static_cast<int8_t>(l);
  f.spd_cmd_r = static_cast<int8_t>(r);
  f.spd_act_l = -128;
  f.spd_act_r = -128;
  f.flags = g_app.fast_flags;
  g_app.slow.owner = static_cast<uint8_t>(g_app.arbitrator.owner());
  g_app.slow.driving = g_app.moving ? 1 : 0;
}

}  // namespace app
}  // namespace cq
```

In `src/app/routes.cpp` add `#include "app/motion_api.h"` and rows before `// ROUTES-END`:
```cpp
    {static_cast<uint8_t>(proto::FrameType::kDrive), onDrive, false},
    {static_cast<uint8_t>(proto::FrameType::kStop), onStop, true},
    {static_cast<uint8_t>(proto::FrameType::kBrake), onBrake, true},
    {static_cast<uint8_t>(proto::FrameType::kEnvelope), onEnvelope, true},
    {static_cast<uint8_t>(proto::FrameType::kOwnerHint), onOwnerHint, false},
```
In `src/app/tasks.h` add before `// TASK-DECLS-END`:
```cpp
void controlInit();  // boot: motor port in standby, outputs zero
void controlTask();  // 100 Hz
```
In `src/main.cpp` (already includes `app/tasks.h`): insert `cq::app::controlInit();` immediately after the `// SETUP-INIT` comment line, before `settingsLoad()`/`imuInit()`/`sensorsInit()`, so the motors are safe first; insert the task row `{cq::app::controlTask, 10000UL, 0},` directly after the `imuTask` row (second row).

- [ ] **Step 7: Smoke commands for driving and safety**

Add to `tools/smoke/uno_smoke.py` (`wait_synced` replaces a fixed sleep: opening the port reboots the Uno, and network motion gets `BUSY` until the HELLO exchange — LLD §9.4):
```python
def wait_synced(link: Link, timeout: float = 4.0) -> None:
    end = time.monotonic() + timeout
    while not link.synced and time.monotonic() < end:
        time.sleep(0.05)
    if not link.synced:
        print("no HELLO from the Uno - motion commands will be answered BUSY")
    time.sleep(0.1)  # HELLO_ACK reaches the control task within one tick


def print_ack(m: object) -> None:
    if isinstance(m, g.Ack):
        print(f"ACK {g.ACK_STATUS[m.status]}")


def cmd_drive(link: Link, args: argparse.Namespace) -> None:
    link.handlers.append(lambda m: print(fmt_event(m)) if isinstance(m, g.Event) else None)
    wait_synced(link)
    # same run: opening the port reboots the Uno, which clears the envelope
    if args.envelope is not None:
        link.send(g.Envelope(speed_cap=args.envelope, accel_cap=0, lock_brake=0))
        time.sleep(0.1)
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        link.send(g.Drive(src=g.Source.HUMAN, v=args.v, w=args.w))
        fast = link.last.get("TelemFast")
        if isinstance(fast, g.TelemFast):
            print(f"pwm={fast.pwm_l},{fast.pwm_r} enc={fast.enc_l},{fast.enc_r} "
                  f"yaw={fast.yaw/100:.1f} dist={fast.dist_mm} flags={fast.flags:#06x}")
        time.sleep(0.05)


def cmd_stop(link: Link, args: argparse.Namespace) -> None:
    link.handlers.append(print_ack)
    wait_synced(link)
    link.send(g.Stop(src=g.Source.HUMAN))
    time.sleep(0.3)


def cmd_envelope(link: Link, args: argparse.Namespace) -> None:
    link.handlers.append(print_ack)
    wait_synced(link)
    link.send(g.Envelope(speed_cap=args.cap, accel_cap=args.accel, lock_brake=args.lock))
    time.sleep(0.3)


def cmd_deadman(link: Link, args: argparse.Namespace) -> None:
    """Drive 30 % for 1 s, then stop streaming DRIVE (heartbeats go on): expect DEADMAN_STOP."""
    link.handlers.append(lambda m: print(fmt_event(m)) if isinstance(m, g.Event) else None)
    wait_synced(link)
    t0 = time.monotonic()
    for _ in range(20):
        link.send(g.Drive(src=g.Source.HUMAN, v=30, w=0))
        t0 = time.monotonic()  # time of the last DRIVE
        time.sleep(0.05)
    while time.monotonic() - t0 < 1.5:
        fast = link.last.get("TelemFast")
        if isinstance(fast, g.TelemFast) and fast.pwm_l == 0 and fast.pwm_r == 0:
            print(f"motors stopped {int((time.monotonic() - t0) * 1000)} ms after the last DRIVE")
            break
        time.sleep(0.01)
    time.sleep(max(0.0, t0 + 1.3 - time.monotonic()))  # keep printing events: idle release at ~1 s
```
Register in `COMMANDS` (`"drive": cmd_drive, "stop": cmd_stop, "envelope": cmd_envelope, "deadman": cmd_deadman`) and in `main()`:
```python
    d = sub.add_parser("drive")
    d.add_argument("v", type=int)
    d.add_argument("w", type=int)
    d.add_argument("--seconds", type=float, default=2.0)
    d.add_argument("--envelope", type=int, default=None, help="speed cap %% sent before driving")
    sub.add_parser("stop")
    e = sub.add_parser("envelope")
    e.add_argument("cap", type=int)
    e.add_argument("accel", type=int)
    e.add_argument("lock", type=int)
    sub.add_parser("deadman")
```

- [ ] **Step 8: Build, flash, stand safety checks (tracks off the ground)**

Put the tank **on the stand** (tracks free), battery on, S1 on "upload"/USB.
Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py && pio run -e uno -t upload`
Then from `tools/smoke/`:
1. `… drive 40 0 --seconds 2` → both tracks turn; `pwm=102,102`. Note which way each track turns (forward = top of the track moving toward the front). If a track runs backwards, flip its `kDir…ForwardHigh` in `board_config.h`, rebuild, re-check. If neither track moves while `pwm` is non-zero, the STBY pin assumption (D3) is wrong — stop and report (shield probe failure).
2. `… drive 0 40 --seconds 2` → left track backwards, right forwards (CCW spin).
3. `… deadman` → `EVENT DEADMAN_STOP` and `motors stopped ~300-330 ms after the last DRIVE`, then `EVENT CONTROL_CHANGED a=0` about 1 s after the last DRIVE (Network → None, idle release).
4. Hold a hand 10 cm in front of the sensor, `… drive 40 0` → `EVENT BLOCKED`, `pwm=0,0`; `… drive -40 0` → reverse turns.
5. `… drive 100 0 --envelope 20` → `pwm=51,51` (the envelope is sent in the same run: every smoke run reopens the port, which reboots the Uno and clears the envelope); `… envelope 20 0 0` alone → `ACK OK`.
6. Start `… drive 40 0 --seconds 5` and pull the USB cable after 2 s → tracks stop within 0.3 s (link lost).

- [ ] **Step 9: Commit**

```bash
git add firmware/uno tools/smoke
git commit -m "feat(uno): safety core - PwmOutput, envelope, arbitrator, supervisor, motor driver, open-loop drive"
```

---

### Task 7: Encoders, encoder health, calibration scripts, self-test, shield probe and mount check

**Files:**
- Create: `src/domain/estimation/encoder_track.{h,cpp}`, `encoder_health.{h,cpp}`; `src/domain/calibration/cal_script_runner.{h,cpp}`, `cal_status.{h,cpp}`; `src/domain/selftest/self_test.h`; `src/ports/encoders.h`; `src/drivers/encoders_uno.cpp`; `src/app/calibration_task.cpp`, `src/app/selftest_task.cpp`
- Modify: `src/app/app.h`, `src/app/tasks.h`, `src/app/motion_api.h`, `src/app/control_task.cpp`, `src/app/routes.cpp`, `src/app/settings_task.cpp`, `src/app/comms.cpp`, `src/main.cpp`, `tools/smoke/uno_smoke.py`
- Test: `test/test_encoders/test_main.cpp`, `test/test_calibration/test_main.cpp`

**Interfaces:**
- Consumes: `EncoderSnapshot` (Task 1), `Arbitrator::beginCalibration/endCalibration` (Task 6), `Settings` (Task 3), `proto::CalScript`, `proto::CalOp`, `proto::CalDataMsg`, `proto::CalResultMsg`.
- Produces:
  - `cq::EncoderTrack`: `void update(uint32_t edges, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us, int8_t dir)`, `int32_t count() const`, `float cps() const`, `bool recent() const` (edge within 100 ms).
  - `cq::EncoderHealth`: `struct Inputs { int16_t pwm_l, pwm_r; int16_t req_l, req_r; bool edges_l, edges_r; float yaw_rate_dps; uint8_t keep_l, keep_r; }` (`pwm_*` applied last tick, `req_*` asked of the supervisor last tick); `void update(const Inputs&, uint32_t now_ms)`, `bool stalled(uint8_t side) const`, `uint8_t stalledMask() const` (→ `SafetyInputs.stall_mask`), `bool faulted(uint8_t side) const`, `uint8_t takeNewStalls()`, `uint8_t takeNewFaults()` (bit 0 left, bit 1 right), `void clearFaults()`. LLD §9.6: driven above the deadband for 0.5 s with no edges → fault (other evidence of motion, latched) or stall; a stall clears when the requested direction reverses or after 1 s, and then needs another 0.5 s to re-detect.
  - `cq::CalInputs`, `cq::CalConfig { uint16_t max_cps; uint16_t turn_rate_max_dps; uint8_t keep_pwm; }`, `cq::CalOutputs`, `cq::CalScriptRunner`: `void configure(const CalConfig&)`, `proto::AckStatus start(proto::CalScript s, int32_t param, bool encoders, uint32_t now_ms)`, `void cancel()`, `bool running() const`, `proto::CalScript script() const`, `void tick(uint32_t now_ms, const CalInputs&, CalOutputs&)`.
  - `cq::calStatus(const GlobalSettings&, const ProfileSettings&, uint8_t out[3])`.
  - `cq::SelfTest`: `enum class Check : uint8_t { kImu, kUltrasonic, kBattery, kSettings }`, `void report(Check, bool ok)`, `uint8_t failMask() const`, `bool critical() const`.
  - Port `cq::port::encoders`: `begin()`, `snapshot(EncoderSnapshot&)`.
  - App: `cq::app::onCalibrate`, `calibrationTick(int16_t& pwm_l, int16_t& pwm_r) -> bool` (true while a script owns the motors), `calibrationAbort(proto::AckStatus)`, `encodersTick()` (in `app/motion_api.h`); `selfTestTask()` (in `app/tasks.h`).
  - Smoke commands `calibrate SCRIPT [--param N] [--csv PATH]`, `probe`, `mount`, `bias`.

- [ ] **Step 1: Write the failing encoder tests**

`test/test_encoders/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/estimation/encoder_health.h"
#include "domain/estimation/encoder_track.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_track_counts_signed_and_speed() {
  EncoderTrack t;
  t.update(0, 0, 0, 0, 1);
  t.update(100, 1000, 99000, 100000, 1);  // 1 ms between edges → 1000 cps
  TEST_ASSERT_EQUAL(100, t.count());
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 1000.0f, t.cps());
  t.update(150, 1000, 149000, 150000, -1);
  TEST_ASSERT_EQUAL(50, t.count());
  TEST_ASSERT_FLOAT_WITHIN(1.0f, -1000.0f, t.cps());
  t.update(150, 1000, 149000, 260000, -1);  // no edge for 111 ms
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, t.cps());
  TEST_ASSERT_FALSE(t.recent());
}

namespace {
uint8_t runHealth(EncoderHealth& h, EncoderHealth::Inputs in, uint32_t from_ms, uint32_t to_ms) {
  uint8_t stalls = 0;
  for (uint32_t t = from_ms; t < to_ms; t += 10) {
    h.update(in, t);
    stalls |= h.takeNewStalls();
  }
  return stalls;
}
}  // namespace

void test_fault_vs_stall() {
  // A: both tracks held against a wall, straight drive, no yaw → stall after 0.5 s, never a fault
  EncoderHealth a;
  const EncoderHealth::Inputs held{150, 150, 150, 150, false, false, 0.0f, 55, 55};
  TEST_ASSERT_EQUAL(0, runHealth(a, held, 0, 500));  // t = 0…490 ms: not yet
  TEST_ASSERT_EQUAL(0x03, runHealth(a, held, 500, 510));
  TEST_ASSERT_EQUAL(0x03, a.stalledMask());
  TEST_ASSERT_EQUAL(0, a.takeNewFaults());

  // B: right encoder ticks, left silent, tank goes straight → left encoder broken (latched)
  EncoderHealth b;
  runHealth(b, {150, 150, 150, 150, false, true, 0.5f, 55, 55}, 0, 510);
  TEST_ASSERT_EQUAL(0x01, b.takeNewFaults());
  runHealth(b, {0, 0, 0, 0, false, false, 0.0f, 55, 55}, 510, 1000);
  TEST_ASSERT_TRUE(b.faulted(0));  // latched

  // C: left track held while the right drives → tank pivots (yaw) → left is a stall
  EncoderHealth c;
  TEST_ASSERT_EQUAL(0x01, runHealth(c, {150, 150, 150, 150, false, true, 35.0f, 55, 55}, 0, 510));
  TEST_ASSERT_EQUAL(0, c.takeNewFaults());

  // D: spin, both encoders silent but the gyro shows rotation → both faulted
  EncoderHealth d;
  runHealth(d, {-150, 150, -150, 150, false, false, 60.0f, 55, 55}, 0, 510);
  TEST_ASSERT_EQUAL(0x03, d.takeNewFaults());
}

void test_stall_clears_on_reverse_or_after_1s() {
  const EncoderHealth::Inputs held{150, 150, 150, 150, false, false, 0.0f, 55, 55};
  const EncoderHealth::Inputs cut{0, 0, 150, 150, false, false, 0.0f, 55, 55};  // supervisor cuts, still asked
  EncoderHealth h;
  runHealth(h, held, 0, 510);  // stalled at t = 500
  TEST_ASSERT_TRUE(h.stalled(0));
  runHealth(h, cut, 510, 1500);  // the cut output (PWM 0) does not clear it
  TEST_ASSERT_TRUE(h.stalled(0));
  runHealth(h, cut, 1500, 1510);  // 1 s after the stall
  TEST_ASSERT_FALSE(h.stalled(0));
  TEST_ASSERT_EQUAL(0, runHealth(h, held, 1510, 2000));  // re-detection needs another 0.5 s of drive
  TEST_ASSERT_EQUAL(0x03, runHealth(h, held, 2000, 2020));

  EncoderHealth r;
  runHealth(r, held, 0, 510);
  runHealth(r, {0, 0, -150, -150, false, false, 0.0f, 55, 55}, 510, 520);  // asked to back away
  TEST_ASSERT_EQUAL(0, r.stalledMask());
}

void test_low_pwm_is_not_a_stall() {
  EncoderHealth h;
  TEST_ASSERT_EQUAL(0, runHealth(h, {60, 60, 60, 60, false, false, 0.0f, 55, 55}, 0, 1000));  // below keep + 15
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_track_counts_signed_and_speed);
  RUN_TEST(test_fault_vs_stall);
  RUN_TEST(test_stall_clears_on_reverse_or_after_1s);
  RUN_TEST(test_low_pwm_is_not_a_stall);
  return UNITY_END();
}
```

- [ ] **Step 2: Write the failing calibration tests**

`test/test_calibration/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/calibration/cal_script_runner.h"
#include "domain/calibration/cal_status.h"
#include "domain/selftest/self_test.h"
#include "domain/settings/settings.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
CalInputs still() {
  CalInputs in{};
  in.gz = 12;
  in.az = 8192;
  in.batt_mv = 7800;
  return in;
}
}  // namespace

void test_still_sample_streams_50hz_then_ok() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(r.start(proto::CalScript::kStillSample, 1000, false, 0)));
  int samples = 0;
  CalOutputs out{};
  for (uint32_t t = 0; t <= 1100 && r.running(); t += 10) {
    r.tick(t, still(), out);
    TEST_ASSERT_EQUAL(0, out.pwm_l);
    if (out.sample) {
      ++samples;
      TEST_ASSERT_EQUAL(12, out.data.v[2]);
      TEST_ASSERT_EQUAL(8192, out.data.v[5]);
    }
  }
  TEST_ASSERT_INT_WITHIN(1, 50, samples);
  TEST_ASSERT_TRUE(out.finished);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
  TEST_ASSERT_EQUAL(100, out.progress);
}

void test_pwm_ramp_up_then_down_on_selected_track() {
  CalScriptRunner r;
  const int32_t param = 0 | (100 << 8);  // track 0 = left, forward, max 100
  r.start(proto::CalScript::kPwmRamp, param, false, 0);
  CalOutputs out{};
  int16_t max_seen = 0;
  bool up = false, down = false;
  for (uint32_t t = 0; t < 10000 && r.running(); t += 10) {
    r.tick(t, still(), out);
    TEST_ASSERT_EQUAL(0, out.pwm_r);
    if (out.pwm_l > max_seen) max_seen = out.pwm_l;
    if (out.sample) {
      TEST_ASSERT_EQUAL(out.pwm_l, out.data.v[0]);  // v[0] = |pwm| of the selected track
      up = up || out.data.phase == 0;
      down = down || out.data.phase == 1;
    }
  }
  TEST_ASSERT_EQUAL(100, max_seen);
  TEST_ASSERT_TRUE(up && down);  // phase 0 ramp up, phase 1 ramp down
  TEST_ASSERT_EQUAL(0, out.pwm_l);
  TEST_ASSERT_TRUE(out.finished);
  TEST_ASSERT_EQUAL(100, out.progress);
}

void test_pwm_ramp_reverse_bit() {
  CalScriptRunner r;
  r.start(proto::CalScript::kPwmRamp, 0x01 | 0x02 | (50 << 8), false, 0);  // right, reverse, max 50
  CalOutputs out{};
  int16_t min_seen = 0;
  int16_t max_v0 = 0;
  for (uint32_t t = 0; t < 10000 && r.running(); t += 10) {
    r.tick(t, still(), out);
    TEST_ASSERT_EQUAL(0, out.pwm_l);
    if (out.pwm_r < min_seen) min_seen = out.pwm_r;
    if (out.sample && out.data.v[0] > max_v0) max_v0 = out.data.v[0];
  }
  TEST_ASSERT_EQUAL(-50, min_seen);
  TEST_ASSERT_EQUAL(50, max_v0);  // magnitude on the wire
}

void test_full_spin_needs_encoders_and_stops_at_720() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCalPrecondition),
                    static_cast<int>(r.start(proto::CalScript::kFullSpin, 150, false, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(r.start(proto::CalScript::kFullSpin, 150, true, 0)));
  CalInputs in = still();
  CalOutputs out{};
  uint32_t t = 0;
  for (; t < 20000 && r.running(); t += 10) {
    in.yaw_total_deg = t * 0.09f;  // 90 °/s
    r.tick(t, in, out);
    if (r.running()) {
      TEST_ASSERT_EQUAL(-150, out.pwm_l);
      TEST_ASSERT_EQUAL(150, out.pwm_r);
    }
    if (out.sample) {
      TEST_ASSERT_EQUAL(-150, out.data.v[0]);  // pwm_l pwm_r cps_l cps_r yaw_rate×10 …
      TEST_ASSERT_EQUAL(150, out.data.v[1]);
    }
  }
  TEST_ASSERT_UINT32_WITHIN(20, 8000, t);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
}

void test_spin_and_arc_spins_360_then_arcs() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCalPrecondition),
                    static_cast<int>(r.start(proto::CalScript::kSpinAndArc, 0, false, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(r.start(proto::CalScript::kSpinAndArc, 0, true, 0)));
  CalInputs in = still();
  CalOutputs out{};
  float yaw = 0.0f;
  int16_t last0[3] = {0, 0, 0};
  int arc_samples = 0;
  bool arced = false;
  for (uint32_t t = 0; t < 20000 && r.running(); t += 10) {
    // toy plant: spin (l < 0 < r) turns at 90 °/s, a forward arc (0 < l < r) at 30 °/s
    in.yaw_rate_dps = out.pwm_r > out.pwm_l ? (out.pwm_l < 0 ? 90.0f : 30.0f) : 0.0f;
    yaw += in.yaw_rate_dps * 0.01f;
    in.yaw_total_deg = yaw;
    in.enc_l += out.pwm_l / 10;
    in.enc_r += out.pwm_r / 10;
    r.tick(t, in, out);
    if (!out.sample) continue;
    if (out.data.phase == 0) {
      for (uint8_t i = 0; i < 3; ++i) last0[i] = out.data.v[i];
    } else {
      ++arc_samples;
      arced = arced || (out.data.v[4] > 0 && out.data.v[5] > out.data.v[4]);  // pwm_l, pwm_r
    }
  }
  TEST_ASSERT_TRUE(last0[0] < 0 && last0[1] > 0);  // enc_l, enc_r since phase start (CCW spin)
  TEST_ASSERT_INT_WITHIN(20, 3600, last0[2]);     // yaw ×10 at the end of the spin
  TEST_ASSERT_TRUE(arced);
  TEST_ASSERT_TRUE(arc_samples > 10);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
  TEST_ASSERT_EQUAL(0, out.pwm_l);
}

void test_rate_steps_one_phase_per_speed() {
  CalScriptRunner r;
  r.configure(CalConfig{2500, 180, 55});
  r.start(proto::CalScript::kRateSteps, 0, false, 0);  // 0 → 4 speed steps
  CalOutputs out{};
  uint8_t max_phase = 0;
  for (uint32_t t = 0; t < 10000 && r.running(); t += 10) {
    r.tick(t, still(), out);
    if (!out.sample) continue;
    if (out.data.phase > max_phase) max_phase = out.data.phase;
    TEST_ASSERT_EQUAL(1000, out.data.v[1]);  // full turn command, w % ×10
    if (out.data.v[0] == 0) TEST_ASSERT_TRUE(out.pwm_l == -255 && out.pwm_r == 255);  // spin in place
    if (out.data.v[0] == 100) TEST_ASSERT_TRUE(out.pwm_l == 0 && out.pwm_r == 255);   // pivot
  }
  TEST_ASSERT_EQUAL(3, max_phase);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
}

void test_relay_oscillation_switches_on_measured_side() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCalPrecondition),
                    static_cast<int>(r.start(proto::CalScript::kRelayOscillation, 0, false, 0)));  // speed loop
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(r.start(proto::CalScript::kRelayOscillation, 3, true, 0)));
  r.configure(CalConfig{2500, 180, 55});
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(r.start(proto::CalScript::kRelayOscillation, 1, false, 0)));  // rate loop
  CalInputs in = still();
  CalOutputs out{};
  in.yaw_rate_dps = 60.0f;  // below the 90 °/s setpoint → +d
  r.tick(0, in, out);
  TEST_ASSERT_TRUE(out.sample);
  TEST_ASSERT_EQUAL(900, out.data.v[0]);  // setpoint ×10
  TEST_ASSERT_EQUAL(600, out.data.v[1]);  // measured ×10
  TEST_ASSERT_EQUAL(20, out.data.v[2]);   // relay output (w %)
  TEST_ASSERT_TRUE(out.pwm_r > 0 && out.pwm_l == -out.pwm_r);
  const int16_t high = out.pwm_r;
  in.yaw_rate_dps = 120.0f;  // above → −d
  r.tick(10, in, out);
  r.tick(20, in, out);
  TEST_ASSERT_EQUAL(-20, out.data.v[2]);
  TEST_ASSERT_TRUE(out.pwm_r < high);
  uint32_t t = 30;
  for (; t < 12000 && r.running(); t += 10) r.tick(t, in, out);
  TEST_ASSERT_UINT32_WITHIN(20, 8000, t);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk), static_cast<int>(out.status));
}

void test_drive_counts_corrects_heading() {
  CalScriptRunner r;
  r.start(proto::CalScript::kDriveCounts, 1000, true, 0);
  CalInputs in = still();
  CalOutputs out{};
  in.yaw_total_deg = 5.0f;  // drifted left (CCW) after start
  r.tick(0, still(), out);   // captures the start heading 0
  r.tick(10, in, out);
  TEST_ASSERT_TRUE(out.pwm_l > out.pwm_r);  // speed up left to turn back right
  in.enc_l = 1000;
  in.enc_r = 1000;
  r.tick(20, in, out);
  TEST_ASSERT_TRUE(out.finished);
  TEST_ASSERT_EQUAL(0, out.pwm_l);
}

void test_cancel_and_unavailable() {
  CalScriptRunner r;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kUnavailable),
                    static_cast<int>(r.start(proto::CalScript::kServoNudge, 0, true, 0)));  // reserved
  r.start(proto::CalScript::kIdle, 5000, false, 0);
  r.cancel();
  CalOutputs out{};
  r.tick(10, still(), out);
  TEST_ASSERT_FALSE(r.running());
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCalCancelled), static_cast<int>(out.status));
}

void test_cal_status_bits() {
  Settings s;
  s.loadDefaults();
  uint8_t st[3];
  calStatus(s.global(), s.profile(), st);
  TEST_ASSERT_EQUAL(0, st[0] | st[1] | st[2]);
  s.set(static_cast<uint8_t>(proto::SettingId::kGyroBiasZ), -15);
  s.set(static_cast<uint8_t>(proto::SettingId::kCountsPerCm), 7400);
  calStatus(s.global(), s.profile(), st);
  TEST_ASSERT_EQUAL_HEX8(0x01, st[0] & 0x03);         // item 0 gyro bias ok
  TEST_ASSERT_EQUAL_HEX8(0x01, (st[1] >> 0) & 0x03);  // item 4 counts_per_cm ok
}

void test_self_test_mask() {
  SelfTest t;
  t.report(SelfTest::Check::kImu, true);
  t.report(SelfTest::Check::kUltrasonic, false);
  TEST_ASSERT_EQUAL_HEX8(0x02, t.failMask());
  TEST_ASSERT_FALSE(t.critical());
  t.report(SelfTest::Check::kImu, false);
  TEST_ASSERT_TRUE(t.critical());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_still_sample_streams_50hz_then_ok);
  RUN_TEST(test_pwm_ramp_up_then_down_on_selected_track);
  RUN_TEST(test_pwm_ramp_reverse_bit);
  RUN_TEST(test_full_spin_needs_encoders_and_stops_at_720);
  RUN_TEST(test_spin_and_arc_spins_360_then_arcs);
  RUN_TEST(test_rate_steps_one_phase_per_speed);
  RUN_TEST(test_relay_oscillation_switches_on_measured_side);
  RUN_TEST(test_drive_counts_corrects_heading);
  RUN_TEST(test_cancel_and_unavailable);
  RUN_TEST(test_cal_status_bits);
  RUN_TEST(test_self_test_mask);
  return UNITY_END();
}
```

- [ ] **Step 3: Run them to see them fail**

Run: `pio test -e native -f test_encoders -f test_calibration`
Expected: FAIL — `domain/estimation/encoder_health.h: No such file or directory` and `domain/calibration/cal_script_runner.h: No such file or directory`.

- [ ] **Step 4: Implement encoder tracking and health**

`src/domain/estimation/encoder_track.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Single-channel encoder: direction comes from the commanded PWM sign (`dir`, ±1).
class EncoderTrack {
 public:
  static constexpr uint32_t kTimeoutUs = 100000;
  void update(uint32_t edges, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us, int8_t dir);
  int32_t count() const { return m_count; }
  float cps() const { return m_cps; }
  bool recent() const { return m_recent; }

 private:
  uint32_t m_prev_edges = 0;
  bool m_primed = false;
  int32_t m_count = 0;
  float m_cps = 0.0f;
  bool m_recent = false;
};

}  // namespace cq
```

`src/domain/estimation/encoder_track.cpp`:
```cpp
#include "domain/estimation/encoder_track.h"

namespace cq {

void EncoderTrack::update(uint32_t edges, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us, int8_t dir) {
  if (!m_primed) {
    m_prev_edges = edges;
    m_primed = true;
  }
  const uint32_t delta = edges - m_prev_edges;
  m_prev_edges = edges;
  m_count += dir < 0 ? -static_cast<int32_t>(delta) : static_cast<int32_t>(delta);
  m_recent = edges != 0 && static_cast<uint32_t>(now_us - last_edge_us) < kTimeoutUs;
  m_cps = (m_recent && period_us > 0) ? (dir < 0 ? -1e6f : 1e6f) / static_cast<float>(period_us) : 0.0f;
}

}  // namespace cq
```

`src/domain/estimation/encoder_health.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// LLD §9.6. Silent encoder with drive PWM above the deadband for 0.5 s: a latched FAULT when other
// evidence says the track moves (other track ticking + no yaw while driving straight, or a spin seen by
// the gyro with both encoders silent); otherwise a STALL. The supervisor cuts a stalled track; the stall
// clears when the requested direction reverses or after 1 s (then 0.5 s of drive to re-detect).
class EncoderHealth {
 public:
  struct Inputs {
    int16_t pwm_l;  // applied last tick (after the safety gate)
    int16_t pwm_r;
    int16_t req_l;  // asked of the supervisor last tick (a cut track still shows its direction)
    int16_t req_r;
    bool edges_l;
    bool edges_r;
    float yaw_rate_dps;
    uint8_t keep_l;
    uint8_t keep_r;
  };
  static constexpr uint16_t kDetectMs = 500;
  static constexpr uint16_t kStallHoldMs = 1000;
  static constexpr uint8_t kMargin = 15;

  void update(const Inputs& in, uint32_t now_ms);
  bool stalled(uint8_t side) const { return (m_stalled >> side) & 1u; }
  uint8_t stalledMask() const { return m_stalled; }
  bool faulted(uint8_t side) const { return (m_faulted >> side) & 1u; }
  uint8_t takeNewStalls() {
    const uint8_t v = m_new_stalls;
    m_new_stalls = 0;
    return v;
  }
  uint8_t takeNewFaults() {
    const uint8_t v = m_new_faults;
    m_new_faults = 0;
    return v;
  }
  void clearFaults() { m_faulted = 0; }

 private:
  bool m_silent[2] = {false, false};
  uint32_t m_since_ms[2] = {0, 0};
  uint32_t m_stall_ms[2] = {0, 0};
  int8_t m_stall_dir[2] = {0, 0};
  uint8_t m_stalled = 0;
  uint8_t m_faulted = 0;
  uint8_t m_new_stalls = 0;
  uint8_t m_new_faults = 0;
};

}  // namespace cq
```

`src/domain/estimation/encoder_health.cpp`:
```cpp
#include "domain/estimation/encoder_health.h"

#include "domain/util/math_util.h"

namespace cq {

void EncoderHealth::update(const Inputs& in, uint32_t now_ms) {
  const int16_t pwm[2] = {in.pwm_l, in.pwm_r};
  const int16_t req[2] = {in.req_l, in.req_r};
  const bool edges[2] = {in.edges_l, in.edges_r};
  const uint8_t keep[2] = {in.keep_l, in.keep_r};
  const bool straight = (in.pwm_l > 0 && in.pwm_r > 0) || (in.pwm_l < 0 && in.pwm_r < 0);
  const bool spin = (in.pwm_l > 0 && in.pwm_r < 0) || (in.pwm_l < 0 && in.pwm_r > 0);
  const float yaw = absf(in.yaw_rate_dps);

  for (uint8_t s = 0; s < 2; ++s) {
    const uint8_t bit = static_cast<uint8_t>(1u << s);
    if (m_stalled & bit) {  // held while the supervisor cuts the track (its applied PWM is 0)
      const bool reversed = (m_stall_dir[s] > 0 && req[s] < 0) || (m_stall_dir[s] < 0 && req[s] > 0);
      if (reversed || elapsed(now_ms, m_stall_ms[s], kStallHoldMs)) {
        m_stalled = static_cast<uint8_t>(m_stalled & ~bit);
        m_silent[s] = false;  // a new stall needs another kDetectMs of drive
      }
      continue;
    }
    const int16_t mag = pwm[s] < 0 ? static_cast<int16_t>(-pwm[s]) : pwm[s];
    const bool driven = mag >= keep[s] + kMargin;
    const bool silent = driven && !edges[s];
    if (!silent) {
      m_silent[s] = false;
      continue;
    }
    if (!m_silent[s]) {
      m_silent[s] = true;
      m_since_ms[s] = now_ms;
    }
    if (!elapsed(now_ms, m_since_ms[s], kDetectMs) || (m_faulted & bit)) continue;
    const uint8_t other = static_cast<uint8_t>(1 - s);
    const bool moving_evidence = (straight && edges[other] && yaw < 10.0f) || (spin && !edges[other] && yaw > 20.0f);
    if (moving_evidence) {
      m_faulted = static_cast<uint8_t>(m_faulted | bit);
      m_new_faults = static_cast<uint8_t>(m_new_faults | bit);
    } else {
      m_stalled = static_cast<uint8_t>(m_stalled | bit);
      m_new_stalls = static_cast<uint8_t>(m_new_stalls | bit);
      m_stall_ms[s] = now_ms;
      m_stall_dir[s] = pwm[s] > 0 ? 1 : -1;
    }
  }
}

}  // namespace cq
```

- [ ] **Step 5: Implement the calibration runner, cal status and self-test**

`src/domain/calibration/cal_script_runner.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

struct CalInputs {
  int16_t gx, gy, gz, ax, ay, az;  // raw counts, tank frame (65.5 LSB per °/s, 8192 LSB per g)
  int16_t temp_centi_c;            // 0.01 °C
  float yaw_total_deg;             // unwrapped, CCW +
  float yaw_rate_dps;              // CCW +
  int32_t enc_l, enc_r;            // signed by the commanded direction
  float cps_l, cps_r;              // counts/s, signed
  uint16_t batt_mv;
};

// Profile values the closed-loop scripts need; set by the app before start().
struct CalConfig {
  uint16_t max_cps;            // mean of max_cps_l / max_cps_r
  uint16_t turn_rate_max_dps;  // feed-forward scale for w %
  uint8_t keep_pwm;            // mean deadband keep value (percent → PWM map, like deadbandPwm)
};

struct CalOutputs {
  int16_t pwm_l;
  int16_t pwm_r;
  bool sample;            // `data` holds a new CAL_DATA sample (50 Hz)
  proto::CalDataMsg data;
  bool finished;          // script ended this tick; `status` is the result
  proto::AckStatus status;
  uint8_t progress;       // %
  uint8_t stage;          // = CAL_DATA.phase
};

// Motion scripts for off-board calibration maths (LLD §9.7). Runs at the 100 Hz control rate and
// streams a CAL_DATA sample every second tick (50 Hz). The v[0..7] layouts below are the contract
// with the bench calculators (Plan D); authoritative table: docs/lld/protocol-generator-lld.md
// "CAL_DATA channel layouts". Unused channels are 0.
//   STILL_SAMPLE, IDLE  param ms (0 → 3000)      gx gy gz ax ay az temp_centi_c batt_mv
//   PWM_RAMP            track | rev<<1 | max<<8  |pwm| yaw_rate×10 |enc| |cps|  (selected track; ph 0 up, 1 down)
//   FULL_SPIN           pwm (0 → 255)            pwm_l pwm_r cps_l cps_r yaw_rate×10 yaw×10 enc_l enc_r
//   SPIN_AND_ARC        bit0 = CW                enc_l enc_r yaw×10 yaw_rate×10 pwm_l pwm_r (since phase
//                                                start; ph 0 = 360° spin + settle, ph 1 = 90° arc)
//   RATE_STEPS          speed steps (0 → 4)      speed% w%×10 yaw_rate×10 cps_l cps_r pwm_l pwm_r (ph = step)
//   RELAY_OSCILLATION   loop 0 speed/1 rate/2 hdg setpoint×10 measured×10 relay_output pwm_l pwm_r
//   DRIVE_COUNTS        counts (0 → 7600)        enc_l enc_r yaw×10 pwm_l pwm_r cps_l cps_r
//   SERVO_NUDGE         reserved → UNAVAILABLE (servo trim is done host-side with CAM_NUDGE)
class CalScriptRunner {
 public:
  void configure(const CalConfig& cfg) { m_cfg = cfg; }
  proto::AckStatus start(proto::CalScript script, int32_t param, bool encoders, uint32_t now_ms);
  void cancel() { m_cancel = true; }
  bool running() const { return m_running; }
  proto::CalScript script() const { return m_script; }
  void tick(uint32_t now_ms, const CalInputs& in, CalOutputs& out);

 private:
  void finish(CalOutputs& out, proto::AckStatus status);
  void emit(CalOutputs& out, uint32_t t_ms, const int16_t v[8]);
  void setOrigin(const CalInputs& in);
  int16_t pctToPwm(float pct) const;
  void tickSpinAndArc(uint32_t now_ms, const CalInputs& in, CalOutputs& out, int16_t& l, int16_t& r);
  void tickRelay(const CalInputs& in, float yaw, uint32_t t, CalOutputs& out, int16_t& l, int16_t& r);

  CalConfig m_cfg{2500, 180, 55};
  proto::CalScript m_script = proto::CalScript::kIdle;
  int32_t m_param = 0;
  bool m_running = false;
  bool m_cancel = false;
  bool m_primed = false;
  uint32_t m_start_ms = 0;
  uint32_t m_duration_ms = 0;
  uint8_t m_tick = 0;
  uint8_t m_step = 0;        // SPIN_AND_ARC: 0 spin, 1 settle, 2 arc
  uint32_t m_step_ms = 0;
  float m_yaw0 = 0.0f;       // origin: script start (phase start for SPIN_AND_ARC)
  int32_t m_enc0_l = 0;
  int32_t m_enc0_r = 0;
};

}  // namespace cq
```

`src/domain/calibration/cal_script_runner.cpp`:
```cpp
#include "domain/calibration/cal_script_runner.h"

#include "domain/util/math_util.h"

namespace cq {
namespace {

constexpr uint16_t kRampStepMs = 150;
constexpr uint8_t kRampStep = 5;
constexpr uint16_t kRateStepMs = 1500;
constexpr uint16_t kRatePauseMs = 500;
constexpr uint32_t kSpinTimeoutMs = 20000;
constexpr uint32_t kDriveTimeoutMs = 15000;
constexpr int16_t kDrivePwm = 150;
constexpr float kDriveKp = 4.0f;  // PWM per degree of heading error
// SPIN_AND_ARC: gyro turn-rate control in both phases
constexpr float kSpinRateDps = 90.0f;
constexpr float kSpinFfPwm = 140.0f;
constexpr float kSpinKp = 1.5f;  // PWM per °/s of rate error
constexpr uint16_t kSettleMs = 500;
constexpr float kArcPwm = 150.0f;  // fixed v
constexpr float kArcRateDps = 30.0f;  // fixed ω
constexpr float kArcFfDiff = 40.0f;
constexpr float kArcKp = 1.5f;
constexpr float kArcDeg = 90.0f;
// RELAY_OSCILLATION: relay ±d around the setpoint, output in the PID's own output unit
constexpr uint32_t kRelayMs = 8000;
constexpr float kRelaySpeedPct = 50.0f;   // speed loop: % of max_cps, output % correction
constexpr float kRelaySpeedD = 15.0f;
constexpr float kRelayRateDps = 90.0f;    // rate loop: °/s, output w %
constexpr float kRelayRateD = 20.0f;
constexpr float kRelayHeadingD = 60.0f;   // heading loop: setpoint 0° (start heading), output °/s

int16_t sat16(float v) { return static_cast<int16_t>(clampf(v, -32767.0f, 32767.0f)); }
int16_t sat16(int32_t v) { return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32767 ? -32767 : v)); }
int32_t absi(int32_t v) { return v < 0 ? -v : v; }
int16_t rampMax(int32_t param) {
  const uint8_t raw = static_cast<uint8_t>((param >> 8) & 0xFF);
  return raw == 0 ? 255 : raw;
}
uint8_t rateSteps(int32_t param) { return param <= 0 ? 4 : static_cast<uint8_t>(param < 2 ? 2 : (param > 5 ? 5 : param)); }

}  // namespace

proto::AckStatus CalScriptRunner::start(proto::CalScript script, int32_t param, bool encoders, uint32_t now_ms) {
  if (m_running) return proto::AckStatus::kBusy;
  switch (script) {
    case proto::CalScript::kStillSample:
    case proto::CalScript::kIdle:
      m_duration_ms = param <= 0 ? 3000u : static_cast<uint32_t>(param > 30000 ? 30000 : param);
      break;
    case proto::CalScript::kPwmRamp: {
      const uint32_t steps = static_cast<uint32_t>((rampMax(param) + kRampStep - 1) / kRampStep) + 1;
      m_duration_ms = 2u * steps * kRampStepMs;  // up, then down
      break;
    }
    case proto::CalScript::kRateSteps:
      m_duration_ms = rateSteps(param) * static_cast<uint32_t>(kRateStepMs + kRatePauseMs);
      break;
    case proto::CalScript::kFullSpin:
    case proto::CalScript::kSpinAndArc:
      if (!encoders) return proto::AckStatus::kCalPrecondition;
      m_duration_ms = kSpinTimeoutMs;
      break;
    case proto::CalScript::kRelayOscillation:
      if (param < 0 || param > 2) return proto::AckStatus::kInvalidCommand;
      if (param == 0 && !encoders) return proto::AckStatus::kCalPrecondition;  // speed loop needs encoders
      m_duration_ms = kRelayMs;
      break;
    case proto::CalScript::kDriveCounts:
      if (!encoders) return proto::AckStatus::kCalPrecondition;
      m_duration_ms = kDriveTimeoutMs;
      break;
    default:
      return proto::AckStatus::kUnavailable;  // SERVO_NUDGE: reserved
  }
  m_script = script;
  m_param = param;
  m_running = true;
  m_cancel = false;
  m_primed = false;
  m_start_ms = now_ms;
  m_tick = 0;
  m_step = 0;
  return proto::AckStatus::kOk;
}

void CalScriptRunner::finish(CalOutputs& out, proto::AckStatus status) {
  m_running = false;
  out.pwm_l = 0;
  out.pwm_r = 0;
  out.finished = true;
  out.status = status;
  out.progress = status == proto::AckStatus::kOk ? 100 : out.progress;
}

void CalScriptRunner::emit(CalOutputs& out, uint32_t t_ms, const int16_t v[8]) {
  if ((m_tick++ & 1u) != 0) return;
  out.sample = true;
  out.data.script = static_cast<uint8_t>(m_script);
  out.data.phase = out.stage;
  out.data.t_ms = static_cast<uint16_t>(t_ms);  // wraps after 65.5 s; the host unwraps
  for (uint8_t i = 0; i < 8; ++i) out.data.v[i] = v[i];
}

void CalScriptRunner::setOrigin(const CalInputs& in) {
  m_yaw0 = in.yaw_total_deg;
  m_enc0_l = in.enc_l;
  m_enc0_r = in.enc_r;
}

int16_t CalScriptRunner::pctToPwm(float pct) const {
  const float mag = absf(pct);
  if (mag < 0.5f) return 0;
  const float base = m_cfg.keep_pwm;
  const int16_t v = static_cast<int16_t>(clampf(base + (mag / 100.0f) * (255.0f - base), 0.0f, 255.0f) + 0.5f);
  return pct < 0.0f ? static_cast<int16_t>(-v) : v;
}

void CalScriptRunner::tickSpinAndArc(uint32_t now_ms, const CalInputs& in, CalOutputs& out, int16_t& l, int16_t& r) {
  const float dir = (m_param & 0x01) ? -1.0f : 1.0f;  // 0 = CCW (yaw +)
  const float rate = dir * in.yaw_rate_dps;
  if (m_step == 0 && absf(in.yaw_total_deg - m_yaw0) >= 360.0f) {
    m_step = 1;
    m_step_ms = now_ms;
  }
  if (m_step == 1 && elapsed(now_ms, m_step_ms, kSettleMs)) {
    m_step = 2;
    setOrigin(in);  // phase 1 counts from the end of the settle
  }
  const float yaw = in.yaw_total_deg - m_yaw0;
  if (m_step == 2 && absf(yaw) >= kArcDeg) return finish(out, proto::AckStatus::kOk);
  if (m_step == 0) {
    const float pwm = clampf(kSpinFfPwm + kSpinKp * (kSpinRateDps - rate), 0.0f, 255.0f);
    l = static_cast<int16_t>(-dir * pwm);
    r = static_cast<int16_t>(dir * pwm);
    out.progress = static_cast<uint8_t>(absf(yaw) / 6.0f);  // 0–60 %
  } else if (m_step == 2) {
    const float diff = clampf(kArcFfDiff + kArcKp * (kArcRateDps - rate), -100.0f, 100.0f);
    l = static_cast<int16_t>(kArcPwm - dir * diff);
    r = static_cast<int16_t>(kArcPwm + dir * diff);
    out.progress = static_cast<uint8_t>(60.0f + absf(yaw) * 40.0f / kArcDeg);
  } else {
    out.progress = 60;
  }
  out.stage = m_step == 2 ? 1 : 0;
  const int16_t v[8] = {sat16(in.enc_l - m_enc0_l), sat16(in.enc_r - m_enc0_r), sat16(yaw * 10.0f),
                        sat16(in.yaw_rate_dps * 10.0f), l, r, 0, 0};
  emit(out, now_ms - m_start_ms, v);
}

void CalScriptRunner::tickRelay(const CalInputs& in, float yaw, uint32_t t, CalOutputs& out, int16_t& l, int16_t& r) {
  float sp = 0.0f, meas = 0.0f, u = 0.0f;
  if (m_param == 0) {  // speed loop: both tracks, % of max_cps
    sp = kRelaySpeedPct;
    meas = (in.cps_l + in.cps_r) * 50.0f / m_cfg.max_cps;
    u = meas < sp ? kRelaySpeedD : -kRelaySpeedD;
    l = r = pctToPwm(sp + u);
  } else if (m_param == 1) {  // rate loop: spin, w % = feed-forward + relay
    sp = kRelayRateDps;
    meas = in.yaw_rate_dps;
    u = meas < sp ? kRelayRateD : -kRelayRateD;
    r = pctToPwm(sp * 100.0f / m_cfg.turn_rate_max_dps + u);
    l = static_cast<int16_t>(-r);
  } else {  // heading loop: hold the start heading, relay on the rate target (feed-forward only)
    meas = wrap180(yaw);
    u = meas < sp ? kRelayHeadingD : -kRelayHeadingD;
    r = pctToPwm(u * 100.0f / m_cfg.turn_rate_max_dps);
    l = static_cast<int16_t>(-r);
  }
  const int16_t v[8] = {sat16(sp * 10.0f), sat16(meas * 10.0f), sat16(u), l, r, 0, 0, 0};
  emit(out, t, v);
}

void CalScriptRunner::tick(uint32_t now_ms, const CalInputs& in, CalOutputs& out) {
  out.sample = false;
  out.finished = false;
  out.stage = 0;
  if (!m_running) {
    out.pwm_l = 0;
    out.pwm_r = 0;
    return;
  }
  if (m_cancel) {
    finish(out, proto::AckStatus::kCalCancelled);
    return;
  }
  if (!m_primed) {
    m_primed = true;
    setOrigin(in);
  }
  const uint32_t t = now_ms - m_start_ms;
  out.progress = static_cast<uint8_t>(t >= m_duration_ms ? 99 : (t * 100u) / m_duration_ms);
  const float yaw = in.yaw_total_deg - m_yaw0;
  const int32_t dl = in.enc_l - m_enc0_l;
  const int32_t dr = in.enc_r - m_enc0_r;
  const int16_t batt = static_cast<int16_t>(in.batt_mv);
  int16_t l = 0, r = 0;

  switch (m_script) {
    case proto::CalScript::kStillSample:
    case proto::CalScript::kIdle: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kOk);
      const int16_t v[8] = {in.gx, in.gy, in.gz, in.ax, in.ay, in.az, in.temp_centi_c, batt};
      emit(out, t, v);
      break;
    }
    case proto::CalScript::kPwmRamp: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kOk);
      const uint32_t half = m_duration_ms / 2;
      const bool down = t >= half;
      const int16_t max_pwm = rampMax(m_param);
      int16_t mag = static_cast<int16_t>(((down ? t - half : t) / kRampStepMs) * kRampStep);
      if (mag > max_pwm) mag = max_pwm;
      if (down) mag = static_cast<int16_t>(max_pwm - mag);
      const int16_t pwm = (m_param & 0x02) ? static_cast<int16_t>(-mag) : mag;
      const bool right = (m_param & 0x01) != 0;
      l = right ? 0 : pwm;
      r = right ? pwm : 0;
      out.stage = down ? 1 : 0;
      const int16_t v[8] = {mag, sat16(in.yaw_rate_dps * 10.0f), sat16(absi(right ? dr : dl)),
                            sat16(absf(right ? in.cps_r : in.cps_l)), 0, 0, 0, 0};
      emit(out, t, v);
      break;
    }
    case proto::CalScript::kRateSteps: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kOk);
      const uint8_t n = rateSteps(m_param);
      const uint8_t idx = static_cast<uint8_t>(t / (kRateStepMs + kRatePauseMs));
      const bool pause = (t % (kRateStepMs + kRatePauseMs)) >= kRateStepMs;
      const int16_t speed = static_cast<int16_t>(100 * idx / (n - 1));
      float lp = speed - 100.0f;  // full CCW turn command (w = 100 %) at this forward speed
      float rp = speed + 100.0f;
      const float m = absf(lp) > absf(rp) ? absf(lp) : absf(rp);
      if (m > 100.0f) {
        lp = lp * 100.0f / m;
        rp = rp * 100.0f / m;
      }
      out.stage = idx;
      if (pause) break;  // stopped between steps, no samples
      l = pctToPwm(lp);
      r = pctToPwm(rp);
      const int16_t v[8] = {speed, 1000, sat16(in.yaw_rate_dps * 10.0f), sat16(in.cps_l), sat16(in.cps_r), l, r, 0};
      emit(out, t, v);
      break;
    }
    case proto::CalScript::kFullSpin: {
      if (absf(yaw) >= 720.0f) return finish(out, proto::AckStatus::kOk);
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kCalFailed);
      const int16_t pwm = m_param <= 0 ? 255 : static_cast<int16_t>(m_param > 255 ? 255 : m_param);
      l = static_cast<int16_t>(-pwm);
      r = pwm;
      out.progress = static_cast<uint8_t>(absf(yaw) / 7.2f);
      const int16_t v[8] = {l, r, sat16(in.cps_l), sat16(in.cps_r), sat16(in.yaw_rate_dps * 10.0f),
                            sat16(yaw * 10.0f), sat16(dl), sat16(dr)};
      emit(out, t, v);
      break;
    }
    case proto::CalScript::kSpinAndArc: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kCalFailed);
      tickSpinAndArc(now_ms, in, out, l, r);
      if (out.finished) return;
      break;
    }
    case proto::CalScript::kRelayOscillation: {
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kOk);
      tickRelay(in, yaw, t, out, l, r);
      break;
    }
    case proto::CalScript::kDriveCounts: {
      const int32_t target = m_param <= 0 ? 7600 : m_param;
      if ((dl + dr) / 2 >= target) return finish(out, proto::AckStatus::kOk);
      if (t >= m_duration_ms) return finish(out, proto::AckStatus::kCalFailed);
      const float corr = clampf(kDriveKp * yaw, -60.0f, 60.0f);  // yaw > 0 = drifted left → push left harder
      l = static_cast<int16_t>(kDrivePwm + corr);
      r = static_cast<int16_t>(kDrivePwm - corr);
      out.progress = static_cast<uint8_t>(((dl + dr) / 2 * 100) / target);
      const int16_t v[8] = {sat16(dl), sat16(dr), sat16(yaw * 10.0f), l, r, sat16(in.cps_l), sat16(in.cps_r), 0};
      emit(out, t, v);
      break;
    }
    default:
      return finish(out, proto::AckStatus::kInternal);
  }
  out.pwm_l = l;
  out.pwm_r = r;
}

}  // namespace cq
```

`src/domain/calibration/cal_status.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/settings/settings.h"

namespace cq {

// TELEM_SLOW.cal_status: 2 bits per item (0 missing = factory default, 1 calibrated).
// Items: 0 gyro bias, 1 level, 2 deadband, 3 max_cps, 4 counts_per_cm, 5 track width,
//        6 battery scale, 7 servo trims, 8 ultrasonic offset, 9 speed PID.
void calStatus(const GlobalSettings& g, const ProfileSettings& p, uint8_t out[3]);

}  // namespace cq
```

`src/domain/calibration/cal_status.cpp`:
```cpp
#include "domain/calibration/cal_status.h"

namespace cq {

void calStatus(const GlobalSettings& g, const ProfileSettings& p, uint8_t out[3]) {
  bool deadband = false;
  for (uint8_t i = 0; i < 4; ++i) {
    deadband = deadband || p.deadband_start[i] != 70 || p.deadband_keep[i] != 55;
  }
  const bool items[10] = {
      g.gyro_bias_x != 0 || g.gyro_bias_y != 0 || g.gyro_bias_z != 0,
      g.level_pitch != 0 || g.level_roll != 0,
      deadband,
      p.max_cps_l != 2500 || p.max_cps_r != 2500,
      g.counts_per_cm != 7600,
      p.track_width_eff != 150,
      g.batt_mv_per_count != 4050,
      g.pan_trim != 0 || g.tilt_trim != 0,
      g.ultrasonic_offset != 0,
      p.pid_speed_kp != 150 || p.pid_speed_ki != 800,
  };
  out[0] = out[1] = out[2] = 0;
  for (uint8_t i = 0; i < 10; ++i) {
    if (items[i]) out[i / 4] = static_cast<uint8_t>(out[i / 4] | (1u << ((i % 4) * 2)));
  }
}

}  // namespace cq
```

`src/domain/selftest/self_test.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Boot self-test result; an IMU failure is critical (no heading hold, no tilt/lift safety).
class SelfTest {
 public:
  enum class Check : uint8_t { kImu = 0, kUltrasonic = 1, kBattery = 2, kSettings = 3 };
  void report(Check c, bool ok) {
    const uint8_t bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(c));
    m_fail = ok ? static_cast<uint8_t>(m_fail & ~bit) : static_cast<uint8_t>(m_fail | bit);
  }
  uint8_t failMask() const { return m_fail; }
  bool critical() const { return (m_fail & 0x01) != 0; }

 private:
  uint8_t m_fail = 0;
};

}  // namespace cq
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `pio test -e native -f test_encoders -f test_calibration`
Expected: `test_encoders 4 Tests 0 Failures`, `test_calibration 11 Tests 0 Failures`.

- [ ] **Step 7: Encoder port and driver (PCINT1, rising edges)**

`src/ports/encoders.h`:
```cpp
#pragma once
#include "types/hw_types.h"

namespace cq {
namespace port {
namespace encoders {
void begin();
void snapshot(EncoderSnapshot& out);  // atomic copy; counts are raw rising edges (unsigned)
}  // namespace encoders
}  // namespace port
}  // namespace cq
```

`src/drivers/encoders_uno.cpp`:
```cpp
#include <Arduino.h>
#include <util/atomic.h>

#include "drivers/board_config.h"
#include "ports/encoders.h"

namespace {
constexpr uint8_t kLeftBit = _BV(PC1);   // A1
constexpr uint8_t kRightBit = _BV(PC2);  // A2
volatile uint8_t g_prev = 0;
volatile uint32_t g_edges_l = 0, g_edges_r = 0;
volatile uint32_t g_last_l = 0, g_last_r = 0;
volatile uint32_t g_period_l = 0, g_period_r = 0;
}  // namespace

ISR(PCINT1_vect) {
  const uint32_t now = micros();
  const uint8_t pins = PINC;
  const uint8_t rising = static_cast<uint8_t>((pins ^ g_prev) & pins);
  g_prev = pins;
  if (rising & kLeftBit) {
    g_period_l = now - g_last_l;
    g_last_l = now;
    ++g_edges_l;
  }
  if (rising & kRightBit) {
    g_period_r = now - g_last_r;
    g_last_r = now;
    ++g_edges_r;
  }
}

namespace cq {
namespace port {
namespace encoders {

void begin() {
  pinMode(board::kPinEncLeft, INPUT_PULLUP);
  pinMode(board::kPinEncRight, INPUT_PULLUP);
  g_prev = PINC;
  PCMSK1 |= _BV(PCINT9) | _BV(PCINT10);
  PCICR |= _BV(PCIE1);
}

void snapshot(EncoderSnapshot& out) {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    out.count_l = static_cast<int32_t>(g_edges_l);
    out.count_r = static_cast<int32_t>(g_edges_r);
    out.period_l_us = g_period_l;
    out.period_r_us = g_period_r;
    out.last_edge_l_us = g_last_l;
    out.last_edge_r_us = g_last_r;
  }
  out.t_us = micros();
}

}  // namespace encoders
}  // namespace port
}  // namespace cq
```

- [ ] **Step 8: Calibration and self-test app glue**

Add to `src/app/app.h` includes and members:
```cpp
#include "domain/calibration/cal_script_runner.h"
#include "domain/estimation/encoder_health.h"
#include "domain/estimation/encoder_track.h"
#include "domain/selftest/self_test.h"
```
```cpp
  // encoders / calibration / self-test
  EncoderTrack enc_l;
  EncoderTrack enc_r;
  EncoderHealth enc_health;
  CalScriptRunner cal;
  SelfTest self_test;
  uint8_t settings_reset_mask = 0;
  bool self_test_done = false;
```
In `src/app/settings_task.cpp` `settingsLoad()`, after computing `reset_mask`, add: `g_app.settings_reset_mask = reset_mask;`.

Add to `src/app/motion_api.h`:
```cpp
proto::AckStatus onCalibrate(const uint8_t* p, uint8_t len);
bool calibrationTick(int16_t& pwm_l, int16_t& pwm_r);  // true while a script drives the motors
void calibrationAbort(proto::AckStatus reason);
void encodersTick();  // 100 Hz, called by the control task before control
```
In `src/app/tasks.h` add before `// TASK-DECLS-END`:
```cpp
void selfTestTask();  // 1 Hz: hw bits, cal status, one-shot boot self-test
```

`src/app/calibration_task.cpp`:
```cpp
#include "app/app.h"
#include "app/motion_api.h"
#include "app/tasks.h"
#include "domain/calibration/cal_status.h"
#include "domain/estimation/imu_scale.h"
#include "domain/util/math_util.h"
#include "ports/clock.h"
#include "ports/encoders.h"

namespace cq {
namespace app {
namespace {

void sendResult(proto::CalScript script, uint8_t stage, uint8_t progress, proto::AckStatus status) {
  g_app.cal_result.script = static_cast<uint8_t>(script);
  g_app.cal_result.stage = stage;
  g_app.cal_result.progress = progress;
  g_app.cal_result.status = static_cast<uint8_t>(status);
  g_app.cal_result_pending = true;
}

int8_t dirOf(int16_t pwm, int8_t prev) { return pwm > 0 ? 1 : (pwm < 0 ? -1 : prev); }
int8_t g_dir_l = 1;
int8_t g_dir_r = 1;
uint32_t g_last_progress_ms = 0;
uint8_t g_last_progress = 0;  // reported by calibrationAbort()

}  // namespace

void encodersTick() {
  EncoderSnapshot s{};
  port::encoders::snapshot(s);
  g_dir_l = dirOf(g_app.out_pwm_l, g_dir_l);
  g_dir_r = dirOf(g_app.out_pwm_r, g_dir_r);
  g_app.enc_l.update(static_cast<uint32_t>(s.count_l), s.period_l_us, s.last_edge_l_us, s.t_us, g_dir_l);
  g_app.enc_r.update(static_cast<uint32_t>(s.count_r), s.period_r_us, s.last_edge_r_us, s.t_us, g_dir_r);

  const ProfileSettings& ps = g_app.settings.profile();
  if (g_app.encoders_enabled) {
    // keep values by direction: deadband_keep = {left fwd, left rev, right fwd, right rev}
    const EncoderHealth::Inputs hi{g_app.out_pwm_l, g_app.out_pwm_r, g_app.req_pwm_l, g_app.req_pwm_r,
                                   g_app.enc_l.recent(), g_app.enc_r.recent(), g_app.yaw_rate_dps,
                                   ps.deadband_keep[g_app.out_pwm_l < 0 ? 1 : 0],
                                   ps.deadband_keep[g_app.out_pwm_r < 0 ? 3 : 2]};
    g_app.enc_health.update(hi, port::clock::nowMs());
    const uint8_t stalls = g_app.enc_health.takeNewStalls();
    const uint8_t faults = g_app.enc_health.takeNewFaults();
    for (uint8_t sd = 0; sd < 2; ++sd) {
      if (stalls & (1u << sd)) g_app.events.push(proto::EventKind::kStall, sd);
      if (faults & (1u << sd)) g_app.events.push(proto::EventKind::kEncoderFault, sd);
    }
  }
  proto::TelemFastMsg& f = g_app.fast;
  f.enc_l = g_app.enc_l.count();
  f.enc_r = g_app.enc_r.count();
  const bool usable = g_app.encoders_enabled;
  f.spd_act_l = usable ? static_cast<int8_t>(clampf(g_app.enc_l.cps() * 100.0f / ps.max_cps_l, -127, 127)) : -128;
  f.spd_act_r = usable ? static_cast<int8_t>(clampf(g_app.enc_r.cps() * 100.0f / ps.max_cps_r, -127, 127)) : -128;
}

proto::AckStatus onCalibrate(const uint8_t* p, uint8_t len) {
  proto::CalibrateMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::CalScript script = static_cast<proto::CalScript>(m.script);
  if (m.op == static_cast<uint8_t>(proto::CalOp::kCancel)) {
    if (!g_app.cal.running()) return proto::AckStatus::kOk;
    g_app.cal.cancel();
    return proto::AckStatus::kOk;
  }
  if (m.profile != 0) return proto::AckStatus::kUnavailable;  // MVP: one profile
  if (g_app.cal.running()) return proto::AckStatus::kBusy;
  if (g_app.lift.lifted() || g_app.lift.tilted() || g_app.moving || !g_app.imu_ok || g_app.envelope.active()) {
    return proto::AckStatus::kCalPrecondition;  // LLD §9.7: refused while an envelope is active
  }
  const proto::AckStatus own = g_app.arbitrator.beginCalibration();
  if (own != proto::AckStatus::kOk) return own;
  g_app.teleop_v = 0;  // an old DRIVE never resumes after the script
  g_app.teleop_w = 0;
  const ProfileSettings& ps = g_app.settings.profile();
  const uint8_t keep = static_cast<uint8_t>(
      (ps.deadband_keep[0] + ps.deadband_keep[1] + ps.deadband_keep[2] + ps.deadband_keep[3]) / 4);
  g_app.cal.configure(CalConfig{static_cast<uint16_t>((ps.max_cps_l + ps.max_cps_r) / 2),
                                g_app.settings.global().turn_rate_max, keep});
  const proto::AckStatus st = g_app.cal.start(script, m.param, g_app.encoders_enabled, port::clock::nowMs());
  if (st != proto::AckStatus::kOk) {
    g_app.arbitrator.endCalibration();
    return st;
  }
  sendResult(script, 0, 0, proto::AckStatus::kOk);
  return st;
}

void calibrationAbort(proto::AckStatus reason) {
  if (!g_app.cal.running()) return;
  const proto::CalScript script = g_app.cal.script();
  g_app.cal.cancel();
  CalOutputs out{};
  const CalInputs in{};
  g_app.cal.tick(port::clock::nowMs(), in, out);
  g_app.arbitrator.endCalibration();
  sendResult(script, 0, g_last_progress, reason);  // progress reached before the abort
}

bool calibrationTick(int16_t& pwm_l, int16_t& pwm_r) {
  if (!g_app.cal.running()) return false;
  if (g_app.arbitrator.owner() != proto::Owner::kCalibration) {  // IR took over
    calibrationAbort(proto::AckStatus::kCalCancelled);
    return false;
  }
  const ImuRaw& r = g_app.imu_raw;
  const CalInputs in{r.gx, r.gy, r.gz, r.ax, r.ay, r.az, imu_scale::tempCentiC(r.temp), g_app.yaw.totalDeg(), g_app.yaw_rate_dps,
                     g_app.enc_l.count(), g_app.enc_r.count(), g_app.enc_l.cps(), g_app.enc_r.cps(), g_app.batt.mv()};
  CalOutputs out{};
  const uint32_t now = port::clock::nowMs();
  const proto::CalScript script = g_app.cal.script();
  g_app.cal.tick(now, in, out);
  g_last_progress = out.progress;
  if (out.sample) {
    g_app.cal_data = out.data;
    g_app.cal_data_pending = true;  // 50 Hz × 25 B fits easily in 250 kbaud next to telemetry
  }
  if (out.finished) {
    g_app.arbitrator.endCalibration();
    sendResult(script, out.stage, out.progress, out.status);
  } else if (elapsed(now, g_last_progress_ms, 500)) {
    g_last_progress_ms = now;
    sendResult(script, out.stage, out.progress, proto::AckStatus::kOk);
  }
  pwm_l = out.pwm_l;
  pwm_r = out.pwm_r;
  return !out.finished;
}

void selfTestTask() {
  const uint32_t now = port::clock::nowMs();
  calStatus(g_app.settings.global(), g_app.settings.profile(), g_app.slow.cal_status);
  uint8_t hw = 0;
  if (g_app.encoders_enabled) hw |= 1u << static_cast<uint8_t>(proto::HwBit::kEncodersEnabled);
  if (g_app.imu_ok && g_app.imu_fail_streak == 0) hw |= 1u << static_cast<uint8_t>(proto::HwBit::kImuOk);
  if (!g_app.us.fault()) hw |= 1u << static_cast<uint8_t>(proto::HwBit::kUsOk);
  g_app.slow.hw = hw;
  if (g_app.self_test_done || now < 2000) return;
  g_app.self_test_done = true;
  g_app.self_test.report(SelfTest::Check::kImu, g_app.imu_ok);
  g_app.self_test.report(SelfTest::Check::kUltrasonic, !g_app.us.fault() && g_app.us.distMm() != UltrasonicFilter::kFault);
  g_app.self_test.report(SelfTest::Check::kBattery, g_app.batt.level() != BatteryMonitor::Level::kCritical);
  g_app.self_test.report(SelfTest::Check::kSettings, g_app.settings_reset_mask == 0);
  g_app.events.push(proto::EventKind::kSelftest, g_app.self_test.failMask(), g_app.self_test.critical() ? 1 : 0);
}

}  // namespace app
}  // namespace cq
```

In `src/app/control_task.cpp` (v1):
- in `controlInit()` add `port::encoders::begin();` (and `#include "ports/encoders.h"`);
- at the top of `controlTask()` add `encodersTick();`;
- in the arbitrator `switch`, add `calibrationAbort(proto::AckStatus::kLinkLost);` to the `kLinkLostStop` case (before `zeroTeleop()`);
- in `onStop()` add `calibrationAbort(proto::AckStatus::kCalCancelled);` before `g_app.arbitrator.onNetworkStop();` (LLD §9.7: STOP aborts a script);
- in the `SafetyInputs` initializer replace `0 /* stall_mask: Task 7 */` with `g_app.enc_health.stalledMask()` (LLD §9.6: a stalled track is cut);
- replace the two lines
```cpp
  const int16_t pwm_l = static_cast<int16_t>(l * 2.55f);
  const int16_t pwm_r = static_cast<int16_t>(r * 2.55f);
```
with
```cpp
  int16_t pwm_l = static_cast<int16_t>(l * 2.55f);
  int16_t pwm_r = static_cast<int16_t>(r * 2.55f);
  const bool calibrating = calibrationTick(pwm_l, pwm_r);
  setFast(proto::FastFlagBit::kCalibrating, calibrating);
```

In `src/app/routes.cpp` add the row: `{static_cast<uint8_t>(proto::FrameType::kCalibrate), onCalibrate, true},`.
In `src/main.cpp` add the task row `{cq::app::selfTestTask, 1000000UL, 0},` before `// TASKS-END`.
In `src/app/comms.cpp` `telemetrySlowDue()` add `s.profile_idx = 0;`.

- [ ] **Step 9: Smoke commands — calibrate, probe, mount, bias**

Add to `tools/smoke/uno_smoke.py`:
```python
import csv
import statistics


def run_script(link: Link, script: g.CalScript, param: int, csv_path: str | None = None,
               timeout: float = 30.0) -> list[g.CalData]:
    samples: list[g.CalData] = []
    done = threading.Event()
    status: list[str] = []

    def on(msg: object) -> None:
        if isinstance(msg, g.CalData):
            samples.append(msg)
        elif isinstance(msg, g.CalResult):
            st = g.ACK_STATUS[msg.status]
            name = g.CalScript(msg.script).name
            print(f"CAL_RESULT {name} stage={msg.stage} {msg.progress}% {st}")
            if msg.progress == 100 or st != "OK":
                status.append(st)
                done.set()
        elif isinstance(msg, g.Ack) and msg.status != 0:
            print(f"ACK {g.ACK_STATUS[msg.status]}")
            status.append(g.ACK_STATUS[msg.status])
            done.set()

    link.handlers.append(on)
    wait_synced(link)
    link.send(g.Calibrate(op=g.CalOp.START, script=script, param=param, profile=0))
    done.wait(timeout)
    link.handlers.remove(on)
    if csv_path:
        with open(csv_path, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["t_ms", "phase", *[f"v{i}" for i in range(8)]])
            for s in samples:
                w.writerow([s.t_ms, s.phase, *s.v])
    print(f"{len(samples)} samples, result {status[0] if status else 'TIMEOUT'}")
    return samples


def cmd_calibrate(link: Link, args: argparse.Namespace) -> None:
    run_script(link, g.CalScript[args.script.upper()], args.param, args.csv)


def ask(question: str) -> bool:
    return input(f"{question} [y/n] ").strip().lower().startswith("y")


def cmd_probe(link: Link, args: argparse.Namespace) -> None:
    """Shield probe (tank on the stand): STBY on D3 + direction pin polarity per track."""
    results: dict[str, bool] = {}
    # PWM_RAMP param: track (0 left, 1 right) | reverse << 1 | max PWM << 8
    for name, param in (("left forward", 0x00), ("left reverse", 0x02), ("right forward", 0x01),
                        ("right reverse", 0x03)):
        track = name.split()[0]
        input(f"Watch the {track} track, Enter runs '{name}' (ramp to PWM 120 and back)…")
        samples = run_script(link, g.CalScript.PWM_RAMP, param | (120 << 8), timeout=15)
        edges = max((s.v[2] for s in samples), default=0)  # |encoder counts| of the selected track
        moved = ask(f"Did the {name.split()[0]} track move {name.split()[1]}?")
        results[name] = moved
        print(f"  encoder edges seen: {edges}")
    print("PROBE RESULT:", "PASS" if all(results.values()) else f"FAIL {results}")
    if not all(results.values()):
        print("Nothing moved → STBY (D3) wrong. Moved the wrong way → flip kDir*ForwardHigh.")


def cmd_mount(link: Link, args: argparse.Namespace) -> None:
    """IMU mount check: level, then nose up, then CCW rotation by hand."""
    input("Tank level and still. Enter…")
    s = run_script(link, g.CalScript.IDLE, 2000)
    if not s:
        print("no samples")
        return
    ax, ay, az = (statistics.mean(x.v[i] for x in s) / 8192 for i in (3, 4, 5))
    print(f"level: ax={ax:+.2f} g ay={ay:+.2f} g az={az:+.2f} g  (expect 0, 0, +1)")
    input("Lift the FRONT ~3 cm and hold. Enter…")
    s = run_script(link, g.CalScript.IDLE, 1500)
    if not s:
        print("no samples")
        return
    ax2 = statistics.mean(x.v[3] for x in s) / 8192
    print(f"nose up: ax={ax2:+.2f} g  (expect positive: specific force, +sin(pitch))")
    input("Put it down. Press Enter, then rotate the tank CCW (seen from above) by hand for 3 s…")
    s = run_script(link, g.CalScript.IDLE, 3000)
    gz = max((x.v[2] for x in s), key=abs, default=0) / 65.5
    print(f"CCW rotation: peak gz={gz:+.0f} °/s  (expect positive)")
    ok = az > 0.8 and ax2 > 0.03 and gz > 20
    print("MOUNT CHECK:", "PASS" if ok else "FAIL → adjust kImuMapX/Y/Z in board_config.h")


def cmd_bias(link: Link, args: argparse.Namespace) -> None:
    """Reference calculator: gyro bias + level offsets from a 3 s still sample, written + saved."""
    input("Tank on a level floor, motors off, do not touch. Enter…")
    s = run_script(link, g.CalScript.STILL_SAMPLE, 3000)
    if not s:
        print("no samples")
        return
    gx, gy, gz = (round(statistics.mean(x.v[i] for x in s)) for i in (0, 1, 2))
    ax, ay, az = (statistics.mean(x.v[i] for x in s) / 8192 for i in (3, 4, 5))
    import math
    pitch = math.degrees(math.atan2(ax, math.hypot(ay, az)))  # same convention as Attitude (Task 4)
    roll = math.degrees(math.atan2(ay, az))
    values = {g.SettingId.GYRO_BIAS_X: gx, g.SettingId.GYRO_BIAS_Y: gy, g.SettingId.GYRO_BIAS_Z: gz,
              g.SettingId.LEVEL_PITCH: round(pitch * 100),
              g.SettingId.LEVEL_ROLL: round(roll * 100)}
    for sid, v in values.items():
        link.send(g.SettingSet(id=sid, value=v))
        print(f"{sid.name.lower()} = {v}")
        time.sleep(0.05)
    link.send(g.SettingsSave())
    time.sleep(1.5)
```
Register `"calibrate": cmd_calibrate, "probe": cmd_probe, "mount": cmd_mount, "bias": cmd_bias` and in `main()`:
```python
    c = sub.add_parser("calibrate")
    c.add_argument("script", choices=[s.name.lower() for s in g.CalScript])
    c.add_argument("--param", type=int, default=0)
    c.add_argument("--csv")
    sub.add_parser("probe")
    sub.add_parser("mount")
    sub.add_parser("bias")
```
Move `import csv`, `import math` and `import statistics` to the top-level import block (ruff `I` order).

- [ ] **Step 10: Build, flash, stand checks**

Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py && pio run -e uno -t upload`
Enable encoders once: `… setting encoders_enabled 1 --save`.
On the stand (tracks free), from `tools/smoke/`:
1. `… probe` → answer per track; expect `PROBE RESULT: PASS` and non-zero `encoder edges seen` for every run.
2. `… mount` → `MOUNT CHECK: PASS` (otherwise fix the axis map, rebuild, repeat).
3. On the floor, level: `… bias` → prints five values and `EVENT SETTINGS_SAVED`; afterwards `watch` shows yaw drift < 1° per minute standing still.
4. `… calibrate pwm_ramp --param 0 --csv ramp.csv` on the stand (left track forward, max 255) → phase 0 ramps up in 52 steps, phase 1 back down; CSV `v3` (|cps|) rises once `v0` (PWM) passes the deadband.
5. After boot, `watch` shows `EVENT SELFTEST a=0 b=0` (a = 0x02 if the ultrasonic is unplugged).
6. Stall cut (stand): `… drive 50 0 --seconds 4` and press a block of wood firmly against the left track until it stops (never grip a running track by hand — the sprocket is a pinch point) → after ~0.5 s `EVENT STALL a=0`, `pwm=0,<n>` (left cut, right still driving) for 1 s, then the left track is driven again (and stalls again after 0.5 s while still held). Clearing on reversal is covered by the host test.

- [ ] **Step 11: Commit**

```bash
git add firmware/uno tools/smoke
git commit -m "feat(uno): encoders + health, calibration script runner, self-test, shield probe and mount check"
```

---

### Task 8: Motion building blocks — PID, ramp, deadband, battery compensation, mixer, target limiter

**Files:**
- Create: `src/domain/util/pid.{h,cpp}`; `src/domain/motion/ramp.{h,cpp}`, `deadband.{h,cpp}`, `battery_comp.h`, `mixer.{h,cpp}`, `target_limiter.{h,cpp}`
- Modify: `src/domain/calibration/cal_script_runner.cpp` (Task 7: `pctToPwm` → `deadbandPwm`)
- Test: `test/test_motion_blocks/test_main.cpp`

**Interfaces:**
- Consumes: nothing outside `domain/util/math_util.h`.
- Produces:
  - `cq::PidGains { float kp, ki, kd; }`, `cq::Pid`: `void setGains(const PidGains&)`, `void setLimits(float out_min, float out_max)`, `float update(float error, float dt_s, bool freeze_integrator)`, `void reset()`, `float integrator() const`; `cq::gainsFromMilli(int32_t kp, int32_t ki, int32_t kd) -> PidGains` (settings are ×0.001).
  - `cq::Ramp`: `float step(float target, float dt_s, float accel_per_s, float decel_per_s)`, `float value() const`, `void reset(float v = 0)`.
  - `cq::deadbandPwm(float pct, bool moving, uint8_t start_pwm, uint8_t keep_pwm) -> int16_t`.
  - `cq::batteryScale(uint16_t batt_mv, uint16_t motor_v_max_mv) -> float`.
  - `cq::MixerConfig { uint8_t turn_full_speed_pct; bool car_reverse; bool pivot_fallback; float keep_pct; }`, `cq::mix(float v_pct, float w_pct, const MixerConfig&, float& l_pct, float& r_pct)` — LLD §3.4: turning priority (when a track would exceed 100 %, |v| is reduced so the difference L−R is kept) and pivot fallback (setting `pivot_fallback`: a non-zero inner target below `keep_pct` → 0). `keep_pct` = the track speed % at the deadband keep PWM, ≈ `keep_pwm × 100 / 255` (Task 11 passes the mean of `deadband_keep`).
  - `cq::Limits { uint8_t speed_cap_pct; uint16_t dist_mm; uint8_t stop_cm; uint8_t slow_cm; bool brake_on; uint8_t degraded_cap_pct; }` (`degraded_cap_pct`: 100 none, 50 ultrasonic fault, 30 IMU fault — min of both; LLD §9.5/§9.16), `cq::Limited { float l, r; bool capped; bool slowed; }`, `cq::limitTargets(float l_pct, float r_pct, const Limits&) -> Limited` (cap = min(envelope, degraded); `capped` → `FastFlagBit::kCapped` in Task 11).

- [ ] **Step 1: Write the failing tests**

`test/test_motion_blocks/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/motion/battery_comp.h"
#include "domain/motion/deadband.h"
#include "domain/motion/mixer.h"
#include "domain/motion/ramp.h"
#include "domain/motion/target_limiter.h"
#include "domain/util/pid.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_pid_p_only_and_limits() {
  Pid p;
  p.setGains({2.0f, 0.0f, 0.0f});
  p.setLimits(-10.0f, 10.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 6.0f, p.update(3.0f, 0.01f, false));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, p.update(30.0f, 0.01f, false));
}

void test_pid_integrates_and_freezes() {
  Pid p;
  p.setGains({0.0f, 1.0f, 0.0f});
  p.setLimits(-100.0f, 100.0f);
  for (int i = 0; i < 100; ++i) p.update(1.0f, 0.01f, false);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, p.integrator());
  for (int i = 0; i < 100; ++i) p.update(1.0f, 0.01f, true);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, p.integrator());
}

void test_pid_anti_windup_at_saturation() {
  Pid p;
  p.setGains({1.0f, 10.0f, 0.0f});
  p.setLimits(-5.0f, 5.0f);
  for (int i = 0; i < 1000; ++i) p.update(10.0f, 0.01f, false);  // saturated the whole time
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, p.integrator());  // P alone saturates: conditional integration adds nothing
  const float out = p.update(-1.0f, 0.01f, false);  // error reverses → output leaves the limit at once
  TEST_ASSERT_TRUE(out < 5.0f);
}

void test_ramp_accel_and_faster_decel() {
  Ramp r;
  for (int i = 0; i < 10; ++i) r.step(100.0f, 0.01f, 200.0f, 400.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, r.value());
  r.step(0.0f, 0.01f, 200.0f, 400.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 16.0f, r.value());
  for (int i = 0; i < 100; ++i) r.step(-50.0f, 0.01f, 200.0f, 400.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -50.0f, r.value());
}

void test_deadband_kick_then_keep() {
  TEST_ASSERT_EQUAL(0, deadbandPwm(0.2f, false, 70, 55));
  TEST_ASSERT_INT_WITHIN(1, 70, deadbandPwm(0.5f, false, 70, 55));  // at rest: start value
  TEST_ASSERT_INT_WITHIN(1, 55, deadbandPwm(0.5f, true, 70, 55));   // moving: keep value
  TEST_ASSERT_EQUAL(255, deadbandPwm(100.0f, true, 70, 55));
  TEST_ASSERT_EQUAL(-155, deadbandPwm(-50.0f, true, 70, 55));
}

void test_battery_scale() {
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.75f, batteryScale(8000, 6000));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, batteryScale(5500, 6000));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, batteryScale(500, 6000));  // USB powered: no battery
}

void test_mixer_turn_authority_and_car_reverse() {
  const MixerConfig car{40, true, false, 0.0f};
  float l = 0, r = 0;
  mix(0.0f, 50.0f, car, l, r);  // spin CCW
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -50.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 50.0f, r);
  mix(100.0f, 50.0f, car, l, r);  // full speed: turn reduced to 40 % of 50 = 20; turning priority → v 80
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 100.0f, r);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 60.0f, l);  // difference 40 kept (LLD §3.4)
  mix(-50.0f, 20.0f, car, l, r);  // car-style reverse: steering inverted
  TEST_ASSERT_TRUE(l > r);
  const MixerConfig tank{40, false, false, 0.0f};
  mix(-50.0f, 20.0f, tank, l, r);
  TEST_ASSERT_TRUE(r > l);
}

void test_mixer_pivot_fallback() {
  const MixerConfig pivot{100, false, true, 20.0f};  // full turn authority, keep threshold 20 %
  float l = 0, r = 0;
  mix(100.0f, 30.0f, pivot, l, r);  // turning priority: v 100 → 70, difference 60 kept
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 40.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 100.0f, r);
  mix(30.0f, 25.0f, pivot, l, r);  // inner 5 % < keep 20 % → 0: pivot on the left track
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 55.0f, r);
  mix(0.0f, 10.0f, pivot, l, r);  // spin in place has no inner track
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -10.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 10.0f, r);
  const MixerConfig off{100, false, false, 20.0f};
  mix(30.0f, 25.0f, off, l, r);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 5.0f, l);
}

void test_envelope_caps_targets() {
  const Limits child{20, 0xFFFF, 15, 50, true, 100};
  Limited o = limitTargets(100.0f, 100.0f, child);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, o.l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, o.r);
  TEST_ASSERT_TRUE(o.capped);
  o = limitTargets(100.0f, -50.0f, child);  // ratio kept: the arc shape does not change
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, o.l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -10.0f, o.r);
  o = limitTargets(10.0f, 15.0f, child);
  TEST_ASSERT_FALSE(o.capped);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 15.0f, o.r);
}

void test_obstacle_slow_zone_scales_forward_only() {
  const Limits l{100, 325, 15, 50, true, 100};  // halfway between 15 and 50 cm
  Limited o = limitTargets(80.0f, 80.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 40.0f, o.l);
  TEST_ASSERT_TRUE(o.slowed);
  o = limitTargets(-80.0f, -80.0f, l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -80.0f, o.l);
  const Limits off{100, 325, 15, 50, false, 100};
  o = limitTargets(80.0f, 80.0f, off);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 80.0f, o.l);
}

void test_degraded_caps() {
  const Limits us_fault{100, 0xFFFE, 15, 50, true, 50};  // brake unavailable → 50 % (LLD §9.16)
  Limited o = limitTargets(80.0f, 80.0f, us_fault);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 50.0f, o.l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 50.0f, o.r);
  TEST_ASSERT_TRUE(o.capped);
  TEST_ASSERT_FALSE(o.slowed);
  const Limits imu_fault{100, 0xFFFF, 15, 50, true, 30};  // IMU fault → 30 %
  o = limitTargets(-60.0f, 60.0f, imu_fault);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -30.0f, o.l);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 30.0f, o.r);
  const Limits child_and_fault{20, 0xFFFF, 15, 50, true, 30};  // the tighter cap wins
  o = limitTargets(100.0f, 100.0f, child_and_fault);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 20.0f, o.l);
  const Limits healthy{100, 0xFFFF, 15, 50, true, 100};
  o = limitTargets(80.0f, -40.0f, healthy);
  TEST_ASSERT_FALSE(o.capped);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 80.0f, o.l);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_pid_p_only_and_limits);
  RUN_TEST(test_pid_integrates_and_freezes);
  RUN_TEST(test_pid_anti_windup_at_saturation);
  RUN_TEST(test_ramp_accel_and_faster_decel);
  RUN_TEST(test_deadband_kick_then_keep);
  RUN_TEST(test_battery_scale);
  RUN_TEST(test_mixer_turn_authority_and_car_reverse);
  RUN_TEST(test_mixer_pivot_fallback);
  RUN_TEST(test_envelope_caps_targets);
  RUN_TEST(test_obstacle_slow_zone_scales_forward_only);
  RUN_TEST(test_degraded_caps);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_motion_blocks`
Expected: FAIL — `domain/motion/battery_comp.h: No such file or directory`.

- [ ] **Step 3: Implement the blocks**

`src/domain/util/pid.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

struct PidGains {
  float kp;
  float ki;
  float kd;
};

inline PidGains gainsFromMilli(int32_t kp, int32_t ki, int32_t kd) {
  return PidGains{kp * 0.001f, ki * 0.001f, kd * 0.001f};
}

// PID with conditional integration (no integration while saturated in the error direction or frozen),
// integrator clamped to the output range, derivative on error with a light low-pass.
class Pid {
 public:
  void setGains(const PidGains& g) { m_g = g; }
  void setLimits(float out_min, float out_max) {
    m_min = out_min;
    m_max = out_max;
  }
  float update(float error, float dt_s, bool freeze_integrator);
  void reset() {
    m_i = 0.0f;
    m_prev_err = 0.0f;
    m_d = 0.0f;
    m_primed = false;
  }
  float integrator() const { return m_i; }

 private:
  PidGains m_g{0.0f, 0.0f, 0.0f};
  float m_min = -1.0f;
  float m_max = 1.0f;
  float m_i = 0.0f;
  float m_prev_err = 0.0f;
  float m_d = 0.0f;
  bool m_primed = false;
};

}  // namespace cq
```

`src/domain/util/pid.cpp`:
```cpp
#include "domain/util/pid.h"

#include "domain/util/math_util.h"

namespace cq {

float Pid::update(float error, float dt_s, bool freeze_integrator) {
  if (dt_s <= 0.0f) dt_s = 1e-3f;
  const float raw_d = m_primed ? (error - m_prev_err) / dt_s : 0.0f;
  m_primed = true;
  m_prev_err = error;
  m_d += 0.5f * (raw_d - m_d);
  const float p = m_g.kp * error;
  const float d = m_g.kd * m_d;
  const float unsat = p + m_i + d;
  const bool pushing_high = unsat >= m_max && error > 0.0f;
  const bool pushing_low = unsat <= m_min && error < 0.0f;
  if (!freeze_integrator && !pushing_high && !pushing_low) {
    m_i = clampf(m_i + m_g.ki * error * dt_s, m_min, m_max);
  }
  return clampf(p + m_i + d, m_min, m_max);
}

}  // namespace cq
```

`src/domain/motion/ramp.h`:
```cpp
#pragma once

namespace cq {

// Rate limiter: accelerating (|target| above |value|, same sign) uses accel, everything else decel.
class Ramp {
 public:
  float step(float target, float dt_s, float accel_per_s, float decel_per_s);
  float value() const { return m_value; }
  void reset(float v = 0.0f) { m_value = v; }

 private:
  float m_value = 0.0f;
};

}  // namespace cq
```

`src/domain/motion/ramp.cpp`:
```cpp
#include "domain/motion/ramp.h"

#include "domain/util/math_util.h"

namespace cq {

float Ramp::step(float target, float dt_s, float accel_per_s, float decel_per_s) {
  // From rest (value 0) any target is an acceleration in its own direction.
  const bool same_sign = m_value == 0.0f || (target >= 0.0f) == (m_value > 0.0f);
  const bool accelerating = same_sign && absf(target) > absf(m_value);
  const float max_step = (accelerating ? accel_per_s : decel_per_s) * dt_s;
  const float diff = target - m_value;
  m_value += clampf(diff, -max_step, max_step);
  return m_value;
}

}  // namespace cq
```

`src/domain/motion/deadband.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Maps a track target in % to PWM, compensating static friction: from rest the first non-zero
// command starts at `start_pwm`, while moving it starts at `keep_pwm`. |pct| < 0.5 → 0.
int16_t deadbandPwm(float pct, bool moving, uint8_t start_pwm, uint8_t keep_pwm);

}  // namespace cq
```

`src/domain/motion/deadband.cpp`:
```cpp
#include "domain/motion/deadband.h"

#include "domain/util/math_util.h"

namespace cq {

int16_t deadbandPwm(float pct, bool moving, uint8_t start_pwm, uint8_t keep_pwm) {
  const float mag = absf(pct);
  if (mag < 0.5f) return 0;
  const float base = moving ? keep_pwm : start_pwm;
  const float pwm = clampf(base + (mag / 100.0f) * (255.0f - base), 0.0f, 255.0f);
  const int16_t v = static_cast<int16_t>(pwm + 0.5f);
  return pct < 0.0f ? static_cast<int16_t>(-v) : v;
}

}  // namespace cq
```

`src/domain/motion/battery_comp.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// PWM scale so the 6 V motors never see more than motor_v_max from a fresh 2S pack. No pack (USB): 1.
inline float batteryScale(uint16_t batt_mv, uint16_t motor_v_max_mv) {
  if (batt_mv < 3000 || batt_mv <= motor_v_max_mv) return 1.0f;
  const float s = static_cast<float>(motor_v_max_mv) / static_cast<float>(batt_mv);
  return s < 0.5f ? 0.5f : s;
}

}  // namespace cq
```

`src/domain/motion/mixer.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

struct MixerConfig {
  uint8_t turn_full_speed_pct;  // turning authority left at full speed (profile setting)
  bool car_reverse;             // reverse_steer == 0: steering inverted when reversing
  bool pivot_fallback;          // setting pivot_fallback: inner track below keep_pct → 0
  float keep_pct;               // track speed % at the deadband keep PWM (≈ keep_pwm × 100 / 255)
};

// Arcade mix v (forward %) + w (CCW %) → track targets in % (LLD §3.4). Turning priority: when a track
// would exceed ±100 %, |v| is reduced so the difference L − R (the turn) is kept. Pivot fallback: a
// non-zero inner target below keep_pct becomes 0 (the tank pivots instead of crawling that track).
void mix(float v_pct, float w_pct, const MixerConfig& cfg, float& l_pct, float& r_pct);

}  // namespace cq
```

`src/domain/motion/mixer.cpp`:
```cpp
#include "domain/motion/mixer.h"

#include "domain/util/math_util.h"

namespace cq {

void mix(float v_pct, float w_pct, const MixerConfig& cfg, float& l_pct, float& r_pct) {
  float v = clampf(v_pct, -100.0f, 100.0f);
  const float authority = 1.0f - (1.0f - cfg.turn_full_speed_pct / 100.0f) * (absf(v) / 100.0f);
  float w = clampf(w_pct * authority, -100.0f, 100.0f);
  if (cfg.car_reverse && v < 0.0f) w = -w;
  const float excess = absf(v) + absf(w) - 100.0f;  // turning priority: give up speed, keep the turn
  if (excess > 0.0f) v = v > 0.0f ? v - excess : v + excess;
  l_pct = v - w;
  r_pct = v + w;
  if (!cfg.pivot_fallback) return;
  float* inner = absf(l_pct) < absf(r_pct) ? &l_pct : (absf(r_pct) < absf(l_pct) ? &r_pct : nullptr);
  if (inner != nullptr && *inner != 0.0f && absf(*inner) < cfg.keep_pct) *inner = 0.0f;
}

}  // namespace cq
```

`src/domain/motion/target_limiter.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

struct Limits {
  uint8_t speed_cap_pct;  // envelope
  uint16_t dist_mm;       // ultrasonic (0xFFFF clear, 0xFFFE fault)
  uint8_t stop_cm;
  uint8_t slow_cm;
  bool brake_on;
  uint8_t degraded_cap_pct;  // 100 none; 50 ultrasonic fault, 30 IMU fault (min of both) — LLD §9.5
};

struct Limited {
  float l;
  float r;
  bool capped;
  bool slowed;
};

// Applied in % BEFORE the speed controllers so a 20 % envelope means 20 % track speed. Cap = min(envelope,
// degraded). Both tracks are scaled by the same factor (arc shape kept). Forward motion fades to 0
// between slow_cm and stop_cm.
Limited limitTargets(float l_pct, float r_pct, const Limits& lim);

}  // namespace cq
```

`src/domain/motion/target_limiter.cpp`:
```cpp
#include "domain/motion/target_limiter.h"

#include "domain/util/math_util.h"

namespace cq {

Limited limitTargets(float l_pct, float r_pct, const Limits& lim) {
  Limited o{l_pct, r_pct, false, false};
  const float m = absf(l_pct) > absf(r_pct) ? absf(l_pct) : absf(r_pct);
  const float cap = lim.speed_cap_pct < lim.degraded_cap_pct ? lim.speed_cap_pct : lim.degraded_cap_pct;
  if (m > cap && m > 0.0f) {
    o.l = l_pct * cap / m;
    o.r = r_pct * cap / m;
    o.capped = true;
  }
  const bool forward = (o.l + o.r) > 0.0f;
  const bool valid = lim.dist_mm < 0xFFFE;
  const float stop_mm = lim.stop_cm * 10.0f;
  const float slow_mm = lim.slow_cm * 10.0f;
  if (lim.brake_on && forward && valid && lim.dist_mm < slow_mm && slow_mm > stop_mm) {
    const float f = clampf((lim.dist_mm - stop_mm) / (slow_mm - stop_mm), 0.0f, 1.0f);
    o.l *= f;
    o.r *= f;
    o.slowed = true;
  }
  return o;
}

}  // namespace cq
```

In `src/domain/calibration/cal_script_runner.cpp` (Task 7) add `#include "domain/motion/deadband.h"` and replace the body of `CalScriptRunner::pctToPwm` with the same map from its one implementation:
```cpp
int16_t CalScriptRunner::pctToPwm(float pct) const { return deadbandPwm(pct, true, m_cfg.keep_pwm, m_cfg.keep_pwm); }
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_motion_blocks`
Expected: `11 Tests 0 Failures`; `pio test -e native -f test_calibration` still `11 Tests 0 Failures` (pctToPwm now calls deadbandPwm).

- [ ] **Step 5: Commit**

```bash
git add firmware/uno
git commit -m "feat(uno): motion blocks - PID, ramp, deadband, battery compensation, mixer, target limiter"
```

---

### Task 9: Speed controller (per track) and heading controller (rate + hold)

**Files:**
- Create: `src/domain/motion/speed_controller.{h,cpp}`, `heading_controller.{h,cpp}`
- Test: `test/test_controllers/test_main.cpp`

**Interfaces:**
- Consumes: `Pid`, `PidGains`, `deadbandPwm` (Task 8), `wrap180`, `clampf`.
- Produces:
  - `cq::SpeedConfig { uint8_t start_pwm; uint8_t keep_pwm; uint16_t max_cps; PidGains gains; bool closed_loop; }`; `cq::SpeedController`: `void configure(const SpeedConfig&)`, `int16_t update(float target_pct, float actual_cps, float dt_s, int16_t pwm_limit, bool moving, bool capped)` (integrator frozen while `capped` — the TargetLimiter's `Limited.capped`, LLD §9.5 — OR while the output sits on `pwm_limit`), `void reset()`, `bool saturated() const`, `float integrator() const`; `cq::pwmLimitForCap(uint8_t cap_pct, uint8_t keep_pwm) -> int16_t`.
  - `cq::HeadingMode : uint8_t { kOff, kRate, kHold }`; `cq::HeadingConfig { PidGains rate; PidGains heading; uint16_t turn_rate_max_dps; }`; `cq::HeadingController`: `void configure(const HeadingConfig&)`, `float update(HeadingMode mode, float w_cmd_pct, float rate_target_dps, float heading_target_deg, float yaw_deg, float yaw_rate_dps, float dt_s, bool freeze)` (returns w % for the mixer), `float teleop(float v_pct, float w_pct, float yaw_deg, float yaw_rate_dps, float dt_s, bool freeze)` (stick w → rate mode; stick centred while driving → holds the captured heading), `void reset()`, `float heldHeading() const`, `bool holding() const`.

- [ ] **Step 1: Write the failing tests**

`test/test_controllers/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/motion/heading_controller.h"
#include "domain/motion/speed_controller.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
SpeedConfig closedCfg() { return SpeedConfig{70, 55, 2500, {0.15f, 0.8f, 0.0f}, true}; }
HeadingConfig headingCfg() { return HeadingConfig{{1.2f, 0.4f, 0.02f}, {3.0f, 0.0f, 0.1f}, 180}; }
}  // namespace

void test_open_loop_is_feedforward_only() {
  SpeedController s;
  SpeedConfig c = closedCfg();
  c.closed_loop = false;
  s.configure(c);
  TEST_ASSERT_EQUAL(155, s.update(50.0f, 0.0f, 0.01f, 255, true, false));
  TEST_ASSERT_EQUAL(0, s.update(0.0f, 0.0f, 0.01f, 255, true, false));
}

void test_closed_loop_pushes_when_slow() {
  SpeedController s;
  s.configure(closedCfg());
  const int16_t first = s.update(50.0f, 1250.0f, 0.01f, 255, true, false);  // on target (50 % of 2500 cps)
  SpeedController t;
  t.configure(closedCfg());
  const int16_t slow = t.update(50.0f, 500.0f, 0.01f, 255, true, false);    // 20 % actual
  TEST_ASSERT_TRUE(slow > first);
}

void test_integrator_frozen_when_capped() {
  SpeedController s;
  s.configure(closedCfg());
  const int16_t limit = pwmLimitForCap(20, 55);
  TEST_ASSERT_EQUAL(115, limit);  // 20 % → 95 PWM + 20 headroom
  int16_t out = 0;
  float i_frozen = 0.0f;
  for (int i = 0; i < 500; ++i) {  // 5 s with the track held: actual stays 0 (output-saturation anti-windup)
    out = s.update(20.0f, 0.0f, 0.01f, limit, true, false);
    if (i == 99) i_frozen = s.integrator();  // the output reached the limit within ~0.5 s
  }
  TEST_ASSERT_TRUE(s.saturated());
  TEST_ASSERT_TRUE(s.integrator() < 15.0f);  // without the freeze it would reach 80
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, i_frozen, s.integrator());  // nothing integrated while on the limit
  out = s.update(20.0f, 500.0f, 0.01f, limit, true, false);  // released and on target
  TEST_ASSERT_TRUE(out < limit);
}

void test_integrator_frozen_while_target_capped() {
  SpeedController s;
  s.configure(closedCfg());
  for (int i = 0; i < 100; ++i) {  // TargetLimiter capped the target: no wind-up even below the PWM limit
    TEST_ASSERT_TRUE(s.update(20.0f, 0.0f, 0.01f, 255, true, true) < 255);
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, s.integrator());
  s.update(20.0f, 0.0f, 0.01f, 255, true, false);  // cap lifted → integrates again
  TEST_ASSERT_TRUE(s.integrator() > 0.0f);
}

void test_rate_mode_feedforward_and_feedback() {
  HeadingController h;
  h.configure(headingCfg());
  const float w = h.update(HeadingMode::kRate, 0.0f, 90.0f, 0.0f, 0.0f, 0.0f, 0.01f, false);
  TEST_ASSERT_TRUE(w > 50.0f);  // 50 % feed-forward (90 of 180 °/s) plus P on the 90 °/s error
  HeadingController g;
  g.configure(headingCfg());
  const float w2 = g.update(HeadingMode::kRate, 0.0f, 90.0f, 0.0f, 0.0f, 90.0f, 0.01f, false);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 50.0f, w2);
}

void test_hold_corrects_drift_with_wraparound() {
  HeadingController h;
  h.configure(headingCfg());
  float w = h.update(HeadingMode::kHold, 0.0f, 0.0f, 359.0f, 4.0f, 0.0f, 0.01f, false);  // 5° left of target
  TEST_ASSERT_TRUE(w < 0.0f);  // turn back right (CW)
  w = h.update(HeadingMode::kHold, 0.0f, 0.0f, 1.0f, 356.0f, 0.0f, 0.01f, false);  // 5° right of target
  TEST_ASSERT_TRUE(w > 0.0f);
}

void test_off_mode_passes_stick_through() {
  HeadingController h;
  h.configure(headingCfg());
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 30.0f, h.update(HeadingMode::kOff, 30.0f, 0, 0, 0, 0, 0.01f, false));
}

void test_teleop_captures_heading_when_stick_centres() {
  HeadingController h;
  h.configure(headingCfg());
  h.teleop(50.0f, 40.0f, 10.0f, 60.0f, 0.01f, false);  // turning
  TEST_ASSERT_FALSE(h.holding());
  h.teleop(50.0f, 0.0f, 20.0f, 30.0f, 0.01f, false);   // centred, still rotating: not yet captured
  TEST_ASSERT_FALSE(h.holding());
  h.teleop(50.0f, 0.0f, 22.0f, 2.0f, 0.01f, false);    // rotation settled → capture 22°
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 22.0f, h.heldHeading());
  const float w = h.teleop(50.0f, 0.0f, 27.0f, 0.0f, 0.01f, false);  // drifted left 5°
  TEST_ASSERT_TRUE(w < 0.0f);
  h.teleop(0.0f, 0.0f, 27.0f, 0.0f, 0.01f, false);  // stopped: hold released
  TEST_ASSERT_FALSE(h.holding());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_open_loop_is_feedforward_only);
  RUN_TEST(test_closed_loop_pushes_when_slow);
  RUN_TEST(test_integrator_frozen_when_capped);
  RUN_TEST(test_integrator_frozen_while_target_capped);
  RUN_TEST(test_rate_mode_feedforward_and_feedback);
  RUN_TEST(test_hold_corrects_drift_with_wraparound);
  RUN_TEST(test_off_mode_passes_stick_through);
  RUN_TEST(test_teleop_captures_heading_when_stick_centres);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_controllers`
Expected: FAIL — `domain/motion/heading_controller.h: No such file or directory`.

- [ ] **Step 3: Implement the controllers**

`src/domain/motion/speed_controller.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/util/pid.h"

namespace cq {

struct SpeedConfig {
  uint8_t start_pwm;
  uint8_t keep_pwm;
  uint16_t max_cps;
  PidGains gains;   // % correction per % speed error
  bool closed_loop; // encoders enabled and healthy
};

// PWM limit while the envelope caps speed: feed-forward PWM of the cap plus 20 headroom.
int16_t pwmLimitForCap(uint8_t cap_pct, uint8_t keep_pwm);

// One track: feed-forward through the deadband map + PID on measured speed (when closed loop).
// The integrator freezes while the TargetLimiter caps the target (`capped`, LLD §9.5) and, as extra
// anti-windup, while the output sits on the PWM limit (envelope cap or 255).
class SpeedController {
 public:
  void configure(const SpeedConfig& cfg);
  int16_t update(float target_pct, float actual_cps, float dt_s, int16_t pwm_limit, bool moving, bool capped);
  void reset() {
    m_pid.reset();
    m_saturated = false;
  }
  bool saturated() const { return m_saturated; }
  float integrator() const { return m_pid.integrator(); }

 private:
  SpeedConfig m_cfg{70, 55, 2500, {0, 0, 0}, false};
  Pid m_pid;
  bool m_saturated = false;
};

}  // namespace cq
```

`src/domain/motion/speed_controller.cpp`:
```cpp
#include "domain/motion/speed_controller.h"

#include "domain/motion/deadband.h"
#include "domain/util/math_util.h"

namespace cq {

int16_t pwmLimitForCap(uint8_t cap_pct, uint8_t keep_pwm) {
  if (cap_pct >= 100) return 255;
  const int16_t v = static_cast<int16_t>(deadbandPwm(cap_pct, true, keep_pwm, keep_pwm) + 20);
  return v > 255 ? 255 : v;
}

void SpeedController::configure(const SpeedConfig& cfg) {
  m_cfg = cfg;
  m_pid.setGains(cfg.gains);
  m_pid.setLimits(-100.0f, 100.0f);
}

int16_t SpeedController::update(float target_pct, float actual_cps, float dt_s, int16_t pwm_limit, bool moving,
                                bool capped) {
  if (absf(target_pct) < 0.5f) {
    reset();
    return 0;
  }
  float cmd_pct = target_pct;
  if (m_cfg.closed_loop && m_cfg.max_cps > 0) {
    const float actual_pct = actual_cps * 100.0f / m_cfg.max_cps;
    cmd_pct = clampf(target_pct + m_pid.update(target_pct - actual_pct, dt_s, m_saturated || capped), -100.0f, 100.0f);
  }
  int16_t pwm = deadbandPwm(cmd_pct, moving, m_cfg.start_pwm, m_cfg.keep_pwm);
  m_saturated = pwm >= pwm_limit || pwm <= -pwm_limit;
  if (pwm > pwm_limit) pwm = pwm_limit;
  if (pwm < -pwm_limit) pwm = static_cast<int16_t>(-pwm_limit);
  return pwm;
}

}  // namespace cq
```

`src/domain/motion/heading_controller.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/util/pid.h"

namespace cq {

enum class HeadingMode : uint8_t { kOff, kRate, kHold };

struct HeadingConfig {
  PidGains rate;     // w % per °/s error
  PidGains heading;  // °/s per ° error
  uint16_t turn_rate_max_dps;
};

// Gyro steering. Rate: w = feed-forward + PID(rate error). Hold: heading PID → rate target → rate loop.
class HeadingController {
 public:
  static constexpr float kCaptureRateDps = 5.0f;
  void configure(const HeadingConfig& cfg);
  float update(HeadingMode mode, float w_cmd_pct, float rate_target_dps, float heading_target_deg, float yaw_deg,
               float yaw_rate_dps, float dt_s, bool freeze);
  float teleop(float v_pct, float w_pct, float yaw_deg, float yaw_rate_dps, float dt_s, bool freeze);
  void reset();
  float heldHeading() const { return m_held_deg; }
  bool holding() const { return m_holding; }

 private:
  float rateLoop(float rate_target_dps, float yaw_rate_dps, float dt_s, bool freeze);
  HeadingConfig m_cfg{{0, 0, 0}, {0, 0, 0}, 180};
  Pid m_rate;
  Pid m_heading;
  bool m_holding = false;
  float m_held_deg = 0.0f;
};

}  // namespace cq
```

`src/domain/motion/heading_controller.cpp`:
```cpp
#include "domain/motion/heading_controller.h"

#include "domain/util/math_util.h"

namespace cq {

void HeadingController::configure(const HeadingConfig& cfg) {
  m_cfg = cfg;
  m_rate.setGains(cfg.rate);
  m_rate.setLimits(-100.0f, 100.0f);
  m_heading.setGains(cfg.heading);
  m_heading.setLimits(-static_cast<float>(cfg.turn_rate_max_dps), static_cast<float>(cfg.turn_rate_max_dps));
}

void HeadingController::reset() {
  m_rate.reset();
  m_heading.reset();
  m_holding = false;
}

float HeadingController::rateLoop(float rate_target_dps, float yaw_rate_dps, float dt_s, bool freeze) {
  const float ff = rate_target_dps * 100.0f / m_cfg.turn_rate_max_dps;
  return clampf(ff + m_rate.update(rate_target_dps - yaw_rate_dps, dt_s, freeze), -100.0f, 100.0f);
}

float HeadingController::update(HeadingMode mode, float w_cmd_pct, float rate_target_dps, float heading_target_deg,
                                 float yaw_deg, float yaw_rate_dps, float dt_s, bool freeze) {
  switch (mode) {
    case HeadingMode::kOff:
      return w_cmd_pct;
    case HeadingMode::kRate:
      return rateLoop(rate_target_dps, yaw_rate_dps, dt_s, freeze);
    case HeadingMode::kHold: {
      const float err = wrap180(heading_target_deg - yaw_deg);
      const float rate_target = m_heading.update(err, dt_s, freeze);
      return rateLoop(rate_target, yaw_rate_dps, dt_s, freeze);
    }
  }
  return 0.0f;
}

float HeadingController::teleop(float v_pct, float w_pct, float yaw_deg, float yaw_rate_dps, float dt_s, bool freeze) {
  if (absf(v_pct) < 0.5f && absf(w_pct) < 0.5f) {
    reset();
    return 0.0f;
  }
  if (absf(w_pct) >= 1.0f) {
    m_holding = false;
    m_heading.reset();
    return update(HeadingMode::kRate, w_pct, w_pct * m_cfg.turn_rate_max_dps / 100.0f, 0.0f, yaw_deg, yaw_rate_dps,
                  dt_s, freeze);
  }
  if (!m_holding) {
    if (absf(yaw_rate_dps) > kCaptureRateDps) {  // let the turn settle before capturing
      return update(HeadingMode::kRate, 0.0f, 0.0f, 0.0f, yaw_deg, yaw_rate_dps, dt_s, freeze);
    }
    m_holding = true;
    m_held_deg = yaw_deg;
    m_heading.reset();
  }
  return update(HeadingMode::kHold, 0.0f, 0.0f, m_held_deg, yaw_deg, yaw_rate_dps, dt_s, freeze);
}

}  // namespace cq
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_controllers`
Expected: `8 Tests 0 Failures`.

- [ ] **Step 5: Commit**

```bash
git add firmware/uno
git commit -m "feat(uno): per-track speed controller with capped-integrator freeze, gyro heading controller"
```

---

### Task 10: Primitives — TURN (spin/arc/pivot, by/to), MOVE (distance + heading hold), cancel semantics

**Files:**
- Create: `src/domain/motion/motion_setpoint.h`, `turn_profile.{h,cpp}`, `move_profile.{h,cpp}`, `primitives.{h,cpp}`
- Test: `test/test_primitives/test_main.cpp`

**Interfaces:**
- Consumes: `HeadingMode` (Task 9), `proto::TurnMsg`, `proto::MoveMsg`, `proto::TurnMode`, `proto::PrimitiveKind`, `proto::AckStatus`, `wrap180`.
- Produces:
  - `cq::MotionSetpoint { HeadingMode mode; float v_pct; float rate_dps; float heading_deg; }`.
  - `cq::ProfileState : uint8_t { kRunning, kDone, kTimeout, kStalled }`.
  - `cq::TurnParams { float angle_deg; float max_rate_dps; float accel_dps2; float arc_v_pct; }`; `cq::TurnProfile`: `void start(const TurnParams&, float yaw_total_deg, uint32_t now_ms)`, `ProfileState tick(float yaw_total_deg, float yaw_rate_dps, float dt_s, uint32_t now_ms, MotionSetpoint& sp)`, `uint8_t progressPct() const`.
  - `cq::MoveParams { float dist_mm; float max_v_pct; float v_full_mm_s; float accel_mm_s2; float heading_deg; float counts_per_mm; }`; `cq::MoveProfile`: `void start(const MoveParams&, int32_t enc_l, int32_t enc_r, uint32_t now_ms)`, `ProfileState tick(int32_t enc_l, int32_t enc_r, bool stalled, float dt_s, uint32_t now_ms, MotionSetpoint& sp)`, `uint8_t progressPct() const`.
  - `cq::PrimitiveConfig { uint16_t turn_rate_max_dps; uint16_t track_width_mm; float v_full_mm_s; float counts_per_mm; bool encoders; bool imu_ok; }`; `cq::PrimitiveInputs { float yaw_deg; float yaw_total_deg; float yaw_rate_dps; int32_t enc_l; int32_t enc_r; bool stalled; }`; `cq::PrimitiveResult { proto::PrimitiveKind kind; bool ok; proto::AckStatus reason; }`.
  - `cq::Primitives`: `proto::AckStatus startTurn(const proto::TurnMsg&, const PrimitiveConfig&, const PrimitiveInputs&, uint32_t now_ms)` (UNAVAILABLE without IMU; INVALID_COMMAND for speed 0/>100, a TurnMode other than BY/TO, or a speed/radius whose max turn rate is below `TurnProfile::kMinRateDps` = 15 °/s), `proto::AckStatus startMove(const proto::MoveMsg&, const PrimitiveConfig&, const PrimitiveInputs&, uint32_t now_ms)` (UNAVAILABLE without encoders or without IMU — MOVE holds the heading), `void cancel(proto::AckStatus reason)`, `bool tick(const PrimitiveInputs&, float dt_s, uint32_t now_ms, MotionSetpoint& sp)` (true while active), `bool takeResult(PrimitiveResult& out)`, `proto::PrimitiveKind active() const`, `uint8_t progressPct() const`.

- [ ] **Step 1: Write the failing tests**

`test/test_primitives/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/motion/primitives.h"
#include "domain/util/math_util.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {

const PrimitiveConfig kCfg{180, 150, 329.0f, 7.6f, true, true};

proto::TurnMsg turn(int16_t angle_dd, uint8_t speed, uint16_t radius_cm, uint8_t mode = 0) {
  proto::TurnMsg m{};
  m.src = 1;
  m.mode = mode;
  m.angle = angle_dd;
  m.speed = speed;
  m.radius_cm = radius_cm;
  return m;
}

proto::MoveMsg move(int32_t dist_mm, uint8_t speed, int16_t heading = 32767) {
  proto::MoveMsg m{};
  m.src = 1;
  m.dist_mm = dist_mm;
  m.speed = speed;
  m.heading = heading;
  return m;
}

// Simple plant: yaw rate follows the commanded rate with a 0.1 s lag.
struct Plant {
  PrimitiveInputs in{0.0f, 0.0f, 0.0f, 0, 0, false};
  float rate = 0.0f;
  float enc = 0.0f;
  void step(const MotionSetpoint& sp, float dt) {
    const float target_rate = sp.mode == HeadingMode::kRate ? sp.rate_dps : 0.0f;
    rate += (target_rate - rate) * dt / 0.1f;
    in.yaw_rate_dps = rate;
    in.yaw_total_deg += rate * dt;
    in.yaw_deg = wrap360(in.yaw_total_deg);
    enc += sp.v_pct / 100.0f * 329.0f * dt * 7.6f;
    in.enc_l = static_cast<int32_t>(enc);
    in.enc_r = static_cast<int32_t>(enc);
  }
};

PrimitiveResult runToEnd(Primitives& p, Plant& plant, uint32_t max_ms) {
  MotionSetpoint sp{};
  for (uint32_t t = 0; t < max_ms; t += 10) {
    if (!p.tick(plant.in, 0.01f, t, sp)) break;
    plant.step(sp, 0.01f);
  }
  PrimitiveResult r{};
  p.takeResult(r);
  return r;
}

}  // namespace

void test_spin_90_ccw_reaches_target() {
  Primitives p;
  Plant plant;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(p.startTurn(turn(900, 50, 0), kCfg, plant.in, 0)));
  const PrimitiveResult r = runToEnd(p, plant, 10000);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kTurn), static_cast<int>(r.kind));
  TEST_ASSERT_FLOAT_WITHIN(1.5f, 90.0f, plant.in.yaw_total_deg);
}

void test_turn_to_takes_shortest_path() {
  Primitives p;
  Plant plant;
  plant.in.yaw_total_deg = 350.0f;
  plant.in.yaw_deg = 350.0f;
  p.startTurn(turn(100, 50, 0, static_cast<uint8_t>(proto::TurnMode::kTo)), kCfg, plant.in, 0);  // to 10°
  runToEnd(p, plant, 10000);
  TEST_ASSERT_FLOAT_WITHIN(1.5f, 370.0f, plant.in.yaw_total_deg);  // +20°, not -340°
}

void test_arc_setpoint_matches_radius() {
  Primitives p;
  Plant plant;
  p.startTurn(turn(900, 60, 50), kCfg, plant.in, 0);  // radius 50 cm
  MotionSetpoint sp{};
  for (uint32_t t = 0; t < 600; t += 10) {
    p.tick(plant.in, 0.01f, t, sp);
    plant.step(sp, 0.01f);
  }
  TEST_ASSERT_TRUE(sp.v_pct > 0.0f);
  TEST_ASSERT_TRUE(sp.rate_dps > 0.0f);
  const float radius_mm = (sp.v_pct / 100.0f * 329.0f) / (sp.rate_dps * 0.0174533f);
  TEST_ASSERT_FLOAT_WITHIN(25.0f, 500.0f, radius_mm);
}

void test_turn_stalls_when_yaw_does_not_move() {
  Primitives p;
  PrimitiveInputs frozen{0.0f, 0.0f, 0.0f, 0, 0, false};
  p.startTurn(turn(900, 50, 0), kCfg, frozen, 0);
  MotionSetpoint sp{};
  uint32_t t = 0;
  for (; t < 5000 && p.tick(frozen, 0.01f, t, sp); t += 10) {
  }
  PrimitiveResult r{};
  TEST_ASSERT_TRUE(p.takeResult(r));
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kStalled), static_cast<int>(r.reason));
  TEST_ASSERT_UINT32_WITHIN(20, 1000, t);  // 1 s of < 3 °/s while commanding ≥ 15 °/s
}

void test_move_500mm_holds_heading_and_stops() {
  Primitives p;
  Plant plant;
  plant.in.yaw_deg = 45.0f;
  plant.in.yaw_total_deg = 45.0f;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(p.startMove(move(500, 40), kCfg, plant.in, 0)));
  MotionSetpoint sp{};
  p.tick(plant.in, 0.01f, 0, sp);
  TEST_ASSERT_EQUAL(static_cast<int>(HeadingMode::kHold), static_cast<int>(sp.mode));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45.0f, sp.heading_deg);
  const PrimitiveResult r = runToEnd(p, plant, 20000);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FLOAT_WITHIN(10.0f, 500.0f, plant.enc / 7.6f);
}

void test_move_needs_encoders_turn_needs_imu() {
  Primitives p;
  PrimitiveConfig no_enc = kCfg;
  no_enc.encoders = false;
  const PrimitiveInputs in{0, 0, 0, 0, 0, false};
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kUnavailable),
                    static_cast<int>(p.startMove(move(500, 40), no_enc, in, 0)));
  PrimitiveConfig no_imu = kCfg;
  no_imu.imu_ok = false;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kUnavailable),
                    static_cast<int>(p.startTurn(turn(900, 50, 0), no_imu, in, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kUnavailable),
                    static_cast<int>(p.startMove(move(500, 40), no_imu, in, 0)));  // heading hold needs the gyro
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(p.startTurn(turn(900, 0, 0), kCfg, in, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(p.startTurn(turn(900, 50, 0, 5), kCfg, in, 0)));  // TurnMode 5 does not exist
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kNone), static_cast<int>(p.active()));
}

void test_turn_too_slow_is_rejected() {
  Primitives p;
  const PrimitiveInputs in{0, 0, 0, 0, 0, false};
  // spin at 5 % → 9 °/s, below the 15 °/s minimum the profile and stall check rely on
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(p.startTurn(turn(900, 5, 0), kCfg, in, 0)));
  // arc 10 % on a 50 cm radius → 32.9 mm/s / 500 mm ≈ 3.8 °/s
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(p.startTurn(turn(900, 10, 50), kCfg, in, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(p.startTurn(turn(900, 10, 0), kCfg, in, 0)));  // spin 10 % = 18 °/s
}

void test_link_loss_cancels_and_does_not_resume() {
  Primitives p;
  Plant plant;
  p.startTurn(turn(1800, 50, 0), kCfg, plant.in, 0);
  MotionSetpoint sp{};
  for (uint32_t t = 0; t < 500; t += 10) {
    p.tick(plant.in, 0.01f, t, sp);
    plant.step(sp, 0.01f);
  }
  p.cancel(proto::AckStatus::kLinkLost);
  PrimitiveResult r{};
  TEST_ASSERT_TRUE(p.takeResult(r));
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kLinkLost), static_cast<int>(r.reason));
  // link comes back: nothing resumes, the setpoint is a stop
  for (uint32_t t = 500; t < 2000; t += 10) {
    TEST_ASSERT_FALSE(p.tick(plant.in, 0.01f, t, sp));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, sp.v_pct);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, sp.rate_dps);
  }
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kNone), static_cast<int>(p.active()));
  TEST_ASSERT_FALSE(p.takeResult(r));
}

void test_new_primitive_replaces_running_one() {
  Primitives p;
  Plant plant;
  p.startTurn(turn(1800, 50, 0), kCfg, plant.in, 0);
  p.startMove(move(300, 40), kCfg, plant.in, 100);
  PrimitiveResult r{};
  TEST_ASSERT_TRUE(p.takeResult(r));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kTurn), static_cast<int>(r.kind));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCancelled), static_cast<int>(r.reason));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kMove), static_cast<int>(p.active()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_spin_90_ccw_reaches_target);
  RUN_TEST(test_turn_to_takes_shortest_path);
  RUN_TEST(test_arc_setpoint_matches_radius);
  RUN_TEST(test_turn_stalls_when_yaw_does_not_move);
  RUN_TEST(test_move_500mm_holds_heading_and_stops);
  RUN_TEST(test_move_needs_encoders_turn_needs_imu);
  RUN_TEST(test_turn_too_slow_is_rejected);
  RUN_TEST(test_link_loss_cancels_and_does_not_resume);
  RUN_TEST(test_new_primitive_replaces_running_one);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_primitives`
Expected: FAIL — `domain/motion/primitives.h: No such file or directory`.

- [ ] **Step 3: Implement profiles and the primitive manager**

`src/domain/motion/motion_setpoint.h`:
```cpp
#pragma once

#include "domain/motion/heading_controller.h"

namespace cq {

// What a motion source asks of the controllers: forward speed % and steering (rate or heading).
struct MotionSetpoint {
  HeadingMode mode;
  float v_pct;
  float rate_dps;
  float heading_deg;
};

enum class ProfileState : uint8_t { kRunning, kDone, kTimeout, kStalled };

}  // namespace cq
```

`src/domain/motion/turn_profile.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/motion/motion_setpoint.h"

namespace cq {

struct TurnParams {
  float angle_deg;     // relative, CCW +
  float max_rate_dps;
  float accel_dps2;
  float arc_v_pct;     // forward speed at max rate (0 = spin in place)
};

// Trapezoidal yaw-rate profile on the unwrapped gyro yaw. Done within 1°.
class TurnProfile {
 public:
  static constexpr float kToleranceDeg = 1.0f;
  static constexpr float kMinRateDps = 15.0f;
  void start(const TurnParams& p, float yaw_total_deg, uint32_t now_ms);
  ProfileState tick(float yaw_total_deg, float yaw_rate_dps, float dt_s, uint32_t now_ms, MotionSetpoint& sp);
  uint8_t progressPct() const { return m_progress; }

 private:
  TurnParams m_p{0, 0, 0, 0};
  float m_target_total = 0.0f;
  float m_rate_cmd = 0.0f;
  uint32_t m_start_ms = 0;
  uint32_t m_timeout_ms = 0;
  uint32_t m_slow_since_ms = 0;
  bool m_slow = false;
  uint8_t m_progress = 0;
};

}  // namespace cq
```

`src/domain/motion/turn_profile.cpp`:
```cpp
#include "domain/motion/turn_profile.h"

#include <math.h>

#include "domain/util/math_util.h"

namespace cq {

void TurnProfile::start(const TurnParams& p, float yaw_total_deg, uint32_t now_ms) {
  m_p = p;
  m_target_total = yaw_total_deg + p.angle_deg;
  m_rate_cmd = 0.0f;
  m_start_ms = now_ms;
  m_timeout_ms = static_cast<uint32_t>(absf(p.angle_deg) / p.max_rate_dps * 2000.0f) + 3000u;
  m_slow = false;
  m_progress = 0;
}

ProfileState TurnProfile::tick(float yaw_total_deg, float yaw_rate_dps, float dt_s, uint32_t now_ms,
                               MotionSetpoint& sp) {
  sp = MotionSetpoint{HeadingMode::kRate, 0.0f, 0.0f, 0.0f};
  const float remaining = m_target_total - yaw_total_deg;
  const float total = absf(m_p.angle_deg);
  m_progress = total > 0.0f ? static_cast<uint8_t>(clampf((1.0f - absf(remaining) / total) * 100.0f, 0.0f, 100.0f)) : 100;
  if (absf(remaining) < kToleranceDeg) {
    m_progress = 100;
    return ProfileState::kDone;
  }
  if (elapsed(now_ms, m_start_ms, m_timeout_ms)) return ProfileState::kTimeout;

  const float decel_limit = sqrtf(2.0f * m_p.accel_dps2 * absf(remaining));
  float rate = m_rate_cmd + m_p.accel_dps2 * dt_s;
  if (rate > m_p.max_rate_dps) rate = m_p.max_rate_dps;
  if (rate > decel_limit) rate = decel_limit;
  if (rate < kMinRateDps) rate = kMinRateDps;
  m_rate_cmd = rate;

  if (absf(yaw_rate_dps) < 3.0f) {  // commanded ≥ 15 °/s but nothing turns
    if (!m_slow) {
      m_slow = true;
      m_slow_since_ms = now_ms;
    } else if (elapsed(now_ms, m_slow_since_ms, 1000)) {
      return ProfileState::kStalled;
    }
  } else {
    m_slow = false;
  }
  sp.rate_dps = remaining > 0.0f ? rate : -rate;
  sp.v_pct = m_p.arc_v_pct * (rate / m_p.max_rate_dps);
  return ProfileState::kRunning;
}

}  // namespace cq
```

`src/domain/motion/move_profile.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/motion/motion_setpoint.h"

namespace cq {

struct MoveParams {
  float dist_mm;       // signed (negative = reverse)
  float max_v_pct;
  float v_full_mm_s;   // track speed at 100 %
  float accel_mm_s2;
  float heading_deg;   // held during the move
  float counts_per_mm;
};

// Trapezoidal speed profile on the mean encoder distance; done within 5 mm or on overshoot.
class MoveProfile {
 public:
  static constexpr float kToleranceMm = 5.0f;
  static constexpr float kMinSpeedMmS = 30.0f;
  void start(const MoveParams& p, int32_t enc_l, int32_t enc_r, uint32_t now_ms);
  ProfileState tick(int32_t enc_l, int32_t enc_r, bool stalled, float dt_s, uint32_t now_ms, MotionSetpoint& sp);
  uint8_t progressPct() const { return m_progress; }

 private:
  MoveParams m_p{0, 0, 1, 1, 0, 1};
  int32_t m_l0 = 0;
  int32_t m_r0 = 0;
  float m_v_cmd = 0.0f;
  uint32_t m_start_ms = 0;
  uint32_t m_timeout_ms = 0;
  uint8_t m_progress = 0;
};

}  // namespace cq
```

`src/domain/motion/move_profile.cpp`:
```cpp
#include "domain/motion/move_profile.h"

#include <math.h>

#include "domain/util/math_util.h"

namespace cq {

void MoveProfile::start(const MoveParams& p, int32_t enc_l, int32_t enc_r, uint32_t now_ms) {
  m_p = p;
  m_l0 = enc_l;
  m_r0 = enc_r;
  m_v_cmd = 0.0f;
  m_start_ms = now_ms;
  const float v_max = p.max_v_pct / 100.0f * p.v_full_mm_s;
  m_timeout_ms = static_cast<uint32_t>(absf(p.dist_mm) / (v_max > 1.0f ? v_max : 1.0f) * 2000.0f) + 3000u;
  m_progress = 0;
}

ProfileState MoveProfile::tick(int32_t enc_l, int32_t enc_r, bool stalled, float dt_s, uint32_t now_ms,
                               MotionSetpoint& sp) {
  sp = MotionSetpoint{HeadingMode::kHold, 0.0f, 0.0f, m_p.heading_deg};
  const float travelled = (static_cast<float>(enc_l - m_l0) + static_cast<float>(enc_r - m_r0)) * 0.5f / m_p.counts_per_mm;
  const float dir = m_p.dist_mm < 0.0f ? -1.0f : 1.0f;
  const float remaining = (m_p.dist_mm - travelled) * dir;  // > 0 while short of the target
  const float total = absf(m_p.dist_mm);
  m_progress = static_cast<uint8_t>(clampf((1.0f - remaining / (total > 0.0f ? total : 1.0f)) * 100.0f, 0.0f, 100.0f));
  if (remaining < kToleranceMm) {
    m_progress = 100;
    return ProfileState::kDone;
  }
  if (stalled) return ProfileState::kStalled;
  if (elapsed(now_ms, m_start_ms, m_timeout_ms)) return ProfileState::kTimeout;

  const float v_max = m_p.max_v_pct / 100.0f * m_p.v_full_mm_s;
  float v = m_v_cmd + m_p.accel_mm_s2 * dt_s;
  if (v > v_max) v = v_max;
  const float decel_limit = sqrtf(2.0f * m_p.accel_mm_s2 * remaining);
  if (v > decel_limit) v = decel_limit;
  if (v < kMinSpeedMmS) v = kMinSpeedMmS;
  m_v_cmd = v;
  sp.v_pct = dir * v / m_p.v_full_mm_s * 100.0f;
  return ProfileState::kRunning;
}

}  // namespace cq
```

`src/domain/motion/primitives.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/motion/motion_setpoint.h"
#include "domain/motion/move_profile.h"
#include "domain/motion/turn_profile.h"
#include "generated/cq_protocol.h"

namespace cq {

struct PrimitiveConfig {
  uint16_t turn_rate_max_dps;
  uint16_t track_width_mm;
  float v_full_mm_s;
  float counts_per_mm;
  bool encoders;
  bool imu_ok;
};

struct PrimitiveInputs {
  float yaw_deg;
  float yaw_total_deg;
  float yaw_rate_dps;
  int32_t enc_l;
  int32_t enc_r;
  bool stalled;
};

struct PrimitiveResult {
  proto::PrimitiveKind kind;
  bool ok;
  proto::AckStatus reason;
};

// One primitive at a time. A cancelled primitive never resumes (link loss, STOP, CANCEL, IR, new command).
class Primitives {
 public:
  static constexpr float kTurnAccelDps2 = 360.0f;
  static constexpr float kMoveAccelMmS2 = 400.0f;

  proto::AckStatus startTurn(const proto::TurnMsg& m, const PrimitiveConfig& cfg, const PrimitiveInputs& in,
                             uint32_t now_ms);
  proto::AckStatus startMove(const proto::MoveMsg& m, const PrimitiveConfig& cfg, const PrimitiveInputs& in,
                             uint32_t now_ms);
  void cancel(proto::AckStatus reason);
  bool tick(const PrimitiveInputs& in, float dt_s, uint32_t now_ms, MotionSetpoint& sp);
  bool takeResult(PrimitiveResult& out);
  proto::PrimitiveKind active() const { return m_active; }
  uint8_t progressPct() const;

 private:
  void end(bool ok, proto::AckStatus reason);
  proto::PrimitiveKind m_active = proto::PrimitiveKind::kNone;
  TurnProfile m_turn;
  MoveProfile m_move;
  PrimitiveResult m_result{proto::PrimitiveKind::kNone, false, proto::AckStatus::kOk};
  bool m_has_result = false;
};

}  // namespace cq
```

`src/domain/motion/primitives.cpp`:
```cpp
#include "domain/motion/primitives.h"

#include "domain/util/math_util.h"

namespace cq {

namespace {
constexpr float kDegToRad = 0.0174533f;
constexpr int16_t kHeadingCurrent = 32767;
}  // namespace

void Primitives::end(bool ok, proto::AckStatus reason) {
  if (m_active == proto::PrimitiveKind::kNone) return;
  m_result = PrimitiveResult{m_active, ok, reason};
  m_has_result = true;
  m_active = proto::PrimitiveKind::kNone;
}

void Primitives::cancel(proto::AckStatus reason) { end(false, reason); }

proto::AckStatus Primitives::startTurn(const proto::TurnMsg& m, const PrimitiveConfig& cfg, const PrimitiveInputs& in,
                                       uint32_t now_ms) {
  if (!cfg.imu_ok) return proto::AckStatus::kUnavailable;
  if (m.speed == 0 || m.speed > 100) return proto::AckStatus::kInvalidCommand;
  if ((m.mode & 0x7F) > static_cast<uint8_t>(proto::TurnMode::kTo)) return proto::AckStatus::kInvalidCommand;
  const bool pivot = (m.mode & 0x80) != 0;
  const proto::TurnMode mode = static_cast<proto::TurnMode>(m.mode & 0x7F);
  float angle = m.angle * 0.1f;
  if (mode == proto::TurnMode::kTo) angle = wrap180(wrap360(angle) - in.yaw_deg);

  float max_rate = m.speed / 100.0f * cfg.turn_rate_max_dps;
  float radius_mm = pivot ? cfg.track_width_mm * 0.5f : m.radius_cm * 10.0f;
  float arc_v_pct = 0.0f;
  if (radius_mm > 0.0f) {
    const float v_allowed = m.speed / 100.0f * cfg.v_full_mm_s;               // forward speed budget
    const float rate_for_v = v_allowed / radius_mm / kDegToRad;               // °/s that budget allows
    if (rate_for_v < max_rate) max_rate = rate_for_v;
    arc_v_pct = max_rate * kDegToRad * radius_mm / cfg.v_full_mm_s * 100.0f;  // keeps v/ω = radius
  }
  // Too slow to profile: the 15 °/s floor would over-speed the arc and the 3 °/s stall check would trip.
  if (max_rate < TurnProfile::kMinRateDps) return proto::AckStatus::kInvalidCommand;
  cancel(proto::AckStatus::kCancelled);
  m_turn.start(TurnParams{angle, max_rate, kTurnAccelDps2, arc_v_pct}, in.yaw_total_deg, now_ms);
  m_active = proto::PrimitiveKind::kTurn;
  return proto::AckStatus::kOk;
}

proto::AckStatus Primitives::startMove(const proto::MoveMsg& m, const PrimitiveConfig& cfg, const PrimitiveInputs& in,
                                       uint32_t now_ms) {
  if (!cfg.encoders || !cfg.imu_ok) return proto::AckStatus::kUnavailable;  // distance + gyro heading hold
  if (m.speed == 0 || m.speed > 100 || m.dist_mm == 0) return proto::AckStatus::kInvalidCommand;
  const float heading = m.heading == kHeadingCurrent ? in.yaw_deg : wrap360(m.heading * 0.1f);
  cancel(proto::AckStatus::kCancelled);
  m_move.start(MoveParams{static_cast<float>(m.dist_mm), static_cast<float>(m.speed), cfg.v_full_mm_s, kMoveAccelMmS2,
                          heading, cfg.counts_per_mm},
               in.enc_l, in.enc_r, now_ms);
  m_active = proto::PrimitiveKind::kMove;
  return proto::AckStatus::kOk;
}

bool Primitives::tick(const PrimitiveInputs& in, float dt_s, uint32_t now_ms, MotionSetpoint& sp) {
  sp = MotionSetpoint{HeadingMode::kRate, 0.0f, 0.0f, 0.0f};
  ProfileState st = ProfileState::kRunning;
  switch (m_active) {
    case proto::PrimitiveKind::kTurn:
      st = m_turn.tick(in.yaw_total_deg, in.yaw_rate_dps, dt_s, now_ms, sp);
      break;
    case proto::PrimitiveKind::kMove:
      st = m_move.tick(in.enc_l, in.enc_r, in.stalled, dt_s, now_ms, sp);
      break;
    default:
      return false;
  }
  switch (st) {
    case ProfileState::kRunning: return true;
    case ProfileState::kDone: end(true, proto::AckStatus::kOk); break;
    case ProfileState::kTimeout: end(false, proto::AckStatus::kTimeout); break;
    case ProfileState::kStalled: end(false, proto::AckStatus::kStalled); break;
  }
  sp = MotionSetpoint{HeadingMode::kRate, 0.0f, 0.0f, 0.0f};
  return false;
}

bool Primitives::takeResult(PrimitiveResult& out) {
  if (!m_has_result) return false;
  out = m_result;
  m_has_result = false;
  return true;
}

uint8_t Primitives::progressPct() const {
  switch (m_active) {
    case proto::PrimitiveKind::kTurn: return m_turn.progressPct();
    case proto::PrimitiveKind::kMove: return m_move.progressPct();
    default: return 0;
  }
}

}  // namespace cq
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_primitives`
Expected: `9 Tests 0 Failures`.

- [ ] **Step 5: Commit**

```bash
git add firmware/uno
git commit -m "feat(uno): TURN/MOVE primitives with trapezoid profiles, stall/timeout, no-resume cancel"
```

---

### Task 11: Closed-loop control pipeline — DRIVE_RATE, TURN, MOVE, CANCEL, PROFILE; floor tests

**Files:**
- Modify (replace whole file): `src/app/control_task.cpp` (v2), `src/app/motion_api.h`
- Modify: `src/app/app.h`, `src/app/routes.cpp`, `src/app/apply_settings.cpp`, `tools/smoke/uno_smoke.py`

**Interfaces:**
- Consumes: everything from Tasks 4–10 (`Arbitrator` incl. `setSynced`/4-argument `tick`, `SafetySupervisor` + `SafetyInputs.stall_mask`, `EncoderTrack/Health`, `HeadingController`, `SpeedController` (`capped` argument), `Ramp`, `mix` + `MixerConfig{…, pivot_fallback, keep_pct}`, `limitTargets` + `Limits.degraded_cap_pct`, `batteryScale`, `Primitives`, `calibrationTick/Abort`, `encodersTick`); `controlInit`/`controlTask` stay declared in `app/tasks.h` (Task 6).
- Produces:
  - Control order per 10 ms tick: encoders → arbitration → source (calibration | primitive | network/IR teleop) → heading → mixer → limiter (envelope %, degraded cap, obstacle slow) → ramps (accel = min(envelope accel cap, `accel_limit`)) → speed controllers (PWM limit from the cap; integrator frozen while capped or the track is stalled) → battery scale → `SafetySupervisor` → motor port.
  - New handlers `onDriveRate`, `onTurn`, `onMove`, `onCancel`, `onProfile`; `configureMotion()` (called from `applySettings()`); `stopTeleop()` (zeroes stick, DRIVE_RATE mode and rate — used by Task 14's IR controller).
  - App `deadman_latched` (set by `DEADMAN_STOP`, cleared by the next accepted DRIVE/DRIVE_RATE; Task 13 reports it as `flags2.deadman`).
  - Events produced here: `CONTROL_CHANGED`, `IR_OVERRIDE`, `DEADMAN_STOP`, `PRIMITIVE_DONE` (`a = kind | ok << 8`, `b = reason`), `BLOCKED`/`UNBLOCKED`, `REJECTED`, `ENVELOPE_CHANGED`.
  - Smoke: `Link.paused` (simulated link loss); commands `driverate V RATE`, `turn ANGLE SPEED [--radius CM] [--to] [--pivot] [--then-to DEG] [--cancel-after S] [--drop-link-after S --restore-after S]`, `move MM SPEED [--heading DEG]`, `cancel`; `drive … --envelope CAP [--accel A] [--lock]`.

- [ ] **Step 1: Replace the motion API header**

`src/app/motion_api.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace app {

// controlInit/controlTask/selfTestTask are declared in app/tasks.h.
void configureMotion();  // re-read settings into controllers
void stopTeleop();       // zero stick, DRIVE_RATE mode and rate (IR stop/takeover, Task 14)

proto::AckStatus onDrive(const uint8_t* p, uint8_t len);
proto::AckStatus onDriveRate(const uint8_t* p, uint8_t len);
proto::AckStatus onStop(const uint8_t* p, uint8_t len);
proto::AckStatus onTurn(const uint8_t* p, uint8_t len);
proto::AckStatus onMove(const uint8_t* p, uint8_t len);
proto::AckStatus onCancel(const uint8_t* p, uint8_t len);
proto::AckStatus onBrake(const uint8_t* p, uint8_t len);
proto::AckStatus onEnvelope(const uint8_t* p, uint8_t len);
proto::AckStatus onOwnerHint(const uint8_t* p, uint8_t len);
proto::AckStatus onProfile(const uint8_t* p, uint8_t len);

proto::AckStatus onCalibrate(const uint8_t* p, uint8_t len);
bool calibrationTick(int16_t& pwm_l, int16_t& pwm_r);  // true while a script drives the motors
void calibrationAbort(proto::AckStatus reason);
void encodersTick();  // 100 Hz, called by the control task before control

}  // namespace app
}  // namespace cq
```

- [ ] **Step 2: App members for the pipeline**

Add to `src/app/app.h` includes:
```cpp
#include "domain/motion/heading_controller.h"
#include "domain/motion/primitives.h"
#include "domain/motion/ramp.h"
#include "domain/motion/speed_controller.h"
```
Add members (next to `teleop_v`/`teleop_w`):
```cpp
  bool teleop_rate_mode = false;  // DRIVE_RATE: closed-loop yaw rate instead of stick w
  float teleop_rate_dps = 0.0f;
  bool deadman_latched = false;   // DEADMAN_STOP fired; cleared by the next accepted DRIVE/DRIVE_RATE
  Primitives primitives;
  HeadingController heading;
  SpeedController speed_l;
  SpeedController speed_r;
  Ramp ramp_l;
  Ramp ramp_r;
```
In `src/app/apply_settings.cpp` add `#include "app/motion_api.h"` and, before `// APPLY-END`, the line `configureMotion();`.

- [ ] **Step 3: Replace the control task with the full pipeline**

`src/app/control_task.cpp` (v2):
```cpp
#include "app/app.h"
#include "app/motion_api.h"
#include "app/tasks.h"
#include "domain/motion/battery_comp.h"
#include "domain/motion/mixer.h"
#include "domain/motion/target_limiter.h"
#include "domain/util/math_util.h"
#include "ports/clock.h"
#include "ports/encoders.h"
#include "ports/motor.h"

namespace cq {
namespace app {
namespace {

constexpr float kDt = 0.01f;

PrimitiveConfig g_prim{180, 150, 329.0f, 7.6f, false, false};
SpeedConfig g_cfg[4] = {};  // l_fwd, l_rev, r_fwd, r_rev
bool g_soft_stop = false;

// Same 3-line helper as in imu_task/sensor_tasks: accepted duplication (file-local, no shared state).
void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_app.fast_flags = on ? static_cast<uint16_t>(g_app.fast_flags | m) : static_cast<uint16_t>(g_app.fast_flags & ~m);
}

void zeroTeleop() {
  g_app.teleop_v = 0;
  g_app.teleop_w = 0;
  g_app.teleop_rate_mode = false;
  g_app.teleop_rate_dps = 0.0f;
}

void rejectEvent(uint8_t frame_type, uint8_t src, proto::AckStatus st) {
  if (g_app.rejects.allow(src, port::clock::nowMs())) {
    g_app.events.push(proto::EventKind::kRejected, frame_type, static_cast<int16_t>(st));
  }
}

bool imuUsable() { return g_app.imu_ok && g_app.imu_fail_streak < 10; }

PrimitiveInputs primInputs() {
  return PrimitiveInputs{g_app.yaw.yawDeg(), g_app.yaw.totalDeg(), g_app.yaw_rate_dps, g_app.enc_l.count(),
                         g_app.enc_r.count(), g_app.enc_health.stalled(0) || g_app.enc_health.stalled(1)};
}

void pushPrimitiveResult() {
  PrimitiveResult r{};
  if (!g_app.primitives.takeResult(r)) return;
  g_app.events.push(proto::EventKind::kPrimitiveDone,
                    static_cast<int16_t>(static_cast<uint8_t>(r.kind) | ((r.ok ? 1 : 0) << 8)),
                    static_cast<int16_t>(r.reason));
}

proto::AckStatus networkMotion(uint8_t frame_type, uint8_t src, bool teleop) {
  const proto::AckStatus st = g_app.arbitrator.requestNetwork(teleop, port::clock::nowMs());
  if (st != proto::AckStatus::kOk) rejectEvent(frame_type, src, st);
  return st;
}

int16_t scaled(int16_t pwm, float s) { return static_cast<int16_t>(static_cast<float>(pwm) * s); }

}  // namespace

void stopTeleop() { zeroTeleop(); }

void configureMotion() {
  const GlobalSettings& gs = g_app.settings.global();
  const ProfileSettings& ps = g_app.settings.profile();
  const PidGains speed = gainsFromMilli(ps.pid_speed_kp, ps.pid_speed_ki, ps.pid_speed_kd);
  const bool enc = g_app.encoders_enabled;
  g_cfg[0] = SpeedConfig{ps.deadband_start[0], ps.deadband_keep[0], ps.max_cps_l, speed, enc};
  g_cfg[1] = SpeedConfig{ps.deadband_start[1], ps.deadband_keep[1], ps.max_cps_l, speed, enc};
  g_cfg[2] = SpeedConfig{ps.deadband_start[2], ps.deadband_keep[2], ps.max_cps_r, speed, enc};
  g_cfg[3] = SpeedConfig{ps.deadband_start[3], ps.deadband_keep[3], ps.max_cps_r, speed, enc};
  g_app.heading.configure(HeadingConfig{gainsFromMilli(ps.pid_rate_kp, ps.pid_rate_ki, ps.pid_rate_kd),
                                        gainsFromMilli(ps.pid_heading_kp, ps.pid_heading_ki, ps.pid_heading_kd),
                                        gs.turn_rate_max});
  const float counts_per_mm = gs.counts_per_cm / 1000.0f;  // setting is counts/cm × 100
  const float v_full = (ps.max_cps_l + ps.max_cps_r) * 0.5f / counts_per_mm;
  g_prim = PrimitiveConfig{gs.turn_rate_max, ps.track_width_eff, v_full, counts_per_mm, enc, imuUsable()};
}

void controlInit() {
  port::motor::begin();
  port::motor::apply(PwmOutput::zero());
  port::encoders::begin();
}

proto::AckStatus onDrive(const uint8_t* p, uint8_t len) {
  proto::DriveMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = networkMotion(static_cast<uint8_t>(proto::FrameType::kDrive), m.src, true);
  if (st != proto::AckStatus::kOk) return st;
  if (g_app.primitives.active() != proto::PrimitiveKind::kNone) g_app.primitives.cancel(proto::AckStatus::kCancelled);
  g_app.deadman_latched = false;
  g_app.teleop_rate_mode = false;
  g_app.teleop_v = m.v;
  g_app.teleop_w = m.w;
  return st;
}

proto::AckStatus onDriveRate(const uint8_t* p, uint8_t len) {
  proto::DriveRateMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (!imuUsable()) return proto::AckStatus::kUnavailable;
  const proto::AckStatus st = networkMotion(static_cast<uint8_t>(proto::FrameType::kDriveRate), m.src, true);
  if (st != proto::AckStatus::kOk) return st;
  if (g_app.primitives.active() != proto::PrimitiveKind::kNone) g_app.primitives.cancel(proto::AckStatus::kCancelled);
  g_app.deadman_latched = false;
  g_app.teleop_rate_mode = true;
  g_app.teleop_v = m.v;
  g_app.teleop_rate_dps = clampf(m.yaw_rate * 0.1f, -static_cast<float>(g_prim.turn_rate_max_dps),
                                 static_cast<float>(g_prim.turn_rate_max_dps));
  return st;
}

proto::AckStatus onStop(const uint8_t* p, uint8_t len) {
  proto::StopMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  zeroTeleop();
  g_app.primitives.cancel(proto::AckStatus::kCancelled);
  calibrationAbort(proto::AckStatus::kCalCancelled);
  g_app.arbitrator.onNetworkStop();
  // Accepted from any source. While IR owns, it only zeroes the current command; IR keeps ownership.
  return proto::AckStatus::kOk;
}

proto::AckStatus onTurn(const uint8_t* p, uint8_t len) {
  proto::TurnMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus own = networkMotion(static_cast<uint8_t>(proto::FrameType::kTurn), m.src, false);
  if (own != proto::AckStatus::kOk) return own;
  zeroTeleop();
  g_prim.imu_ok = imuUsable();
  return g_app.primitives.startTurn(m, g_prim, primInputs(), port::clock::nowMs());
}

proto::AckStatus onMove(const uint8_t* p, uint8_t len) {
  proto::MoveMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus own = networkMotion(static_cast<uint8_t>(proto::FrameType::kMove), m.src, false);
  if (own != proto::AckStatus::kOk) return own;
  zeroTeleop();
  g_prim.imu_ok = imuUsable();  // MOVE holds a gyro heading: UNAVAILABLE without the IMU (R7)
  g_prim.encoders = g_app.encoders_enabled && !g_app.enc_health.faulted(0) && !g_app.enc_health.faulted(1);
  return g_app.primitives.startMove(m, g_prim, primInputs(), port::clock::nowMs());
}

proto::AckStatus onCancel(const uint8_t* p, uint8_t len) {
  proto::CancelMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (g_app.arbitrator.owner() == proto::Owner::kIr) return proto::AckStatus::kNotOwner;
  g_app.primitives.cancel(proto::AckStatus::kCancelled);
  return proto::AckStatus::kOk;
}

proto::AckStatus onBrake(const uint8_t* p, uint8_t len) {
  proto::BrakeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  return g_app.envelope.setBrake(m.on != 0);
}

proto::AckStatus onEnvelope(const uint8_t* p, uint8_t len) {
  proto::EnvelopeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  const proto::AckStatus st = g_app.envelope.set(m.speed_cap, m.accel_cap, m.lock_brake);
  if (st == proto::AckStatus::kOk) {
    const uint16_t packed = static_cast<uint16_t>(static_cast<uint16_t>(m.speed_cap) |
                                                  static_cast<uint16_t>(static_cast<uint16_t>(m.accel_cap) << 8u));
    g_app.events.push(proto::EventKind::kEnvelopeChanged, static_cast<int16_t>(packed), m.lock_brake);
    g_app.slow.env_speed_cap = m.speed_cap;
    g_app.slow.env_accel_cap = m.accel_cap;
    g_app.slow.env_lock_brake = m.lock_brake;
  }
  return st;
}

proto::AckStatus onOwnerHint(const uint8_t* p, uint8_t len) {
  proto::OwnerHintMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.owner_hint = static_cast<proto::OwnerHintValue>(m.owner);
  return proto::AckStatus::kOk;
}

proto::AckStatus onProfile(const uint8_t* p, uint8_t len) {
  proto::ProfileMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  // MVP: a single surface profile (index 0).
  if (m.op == static_cast<uint8_t>(proto::ProfileOp::kSelect) && m.index == 0) return proto::AckStatus::kOk;
  return proto::AckStatus::kUnavailable;
}

void controlTask() {
  const uint32_t now = port::clock::nowMs();
  const GlobalSettings& gs = g_app.settings.global();
  encodersTick();

  // 1. ownership and link/deadman rules. Network motion is BUSY until HELLO_ACK (LLD §9.4); the
  //    Network → None idle release waits while a primitive runs (LLD §9.18).
  g_app.arbitrator.setSynced(g_app.link.synced());
  const bool prim_busy = g_app.primitives.active() != proto::PrimitiveKind::kNone;
  switch (g_app.arbitrator.tick(now, g_app.link.alive(now), prim_busy, Arbitrator::Config{gs.deadman, gs.ir_handback})) {
    case Arbitrator::Action::kDeadmanStop:
      zeroTeleop();
      g_soft_stop = true;
      g_app.deadman_latched = true;
      g_app.events.push(proto::EventKind::kDeadmanStop);
      break;
    case Arbitrator::Action::kLinkLostStop:
      calibrationAbort(proto::AckStatus::kLinkLost);
      zeroTeleop();
      g_soft_stop = true;
      g_app.primitives.cancel(proto::AckStatus::kLinkLost);  // never resumed
      break;
    case Arbitrator::Action::kIrReleased:
      zeroTeleop();
      break;
    case Arbitrator::Action::kNone:
      break;
  }
  if (g_app.arbitrator.takeOwnerChanged()) {
    const proto::Owner owner = g_app.arbitrator.owner();
    g_app.events.push(proto::EventKind::kControlChanged, static_cast<int16_t>(owner));
    // IR takeover: Task 14's input task cancels primitives and stops teleop *before* the IR command is
    // applied (doing it here would cancel a TURN the IR `0` key just started).
    if (owner == proto::Owner::kIr) g_app.events.push(proto::EventKind::kIrOverride);
    // An idle release, STOP or calibration never resumes an old DRIVE/DRIVE_RATE (IR writes teleop while it owns).
    if (owner == proto::Owner::kNone || owner == proto::Owner::kCalibration) zeroTeleop();
  }

  // 2. motion source
  int16_t pwm_l = 0;
  int16_t pwm_r = 0;
  const bool calibrating = calibrationTick(pwm_l, pwm_r);
  bool capped = false;
  float w = 0.0f;
  float tgt_l = 0.0f;
  float tgt_r = 0.0f;
  if (calibrating) {
    g_app.ramp_l.reset();
    g_app.ramp_r.reset();
    g_app.speed_l.reset();
    g_app.speed_r.reset();
    g_app.heading.reset();
  } else {
    const ProfileSettings& ps = g_app.settings.profile();
    const float yaw = g_app.yaw.yawDeg();
    const float rate = g_app.yaw_rate_dps;
    // A TURN cannot finish without the gyro (R7): end it; a running MOVE goes on without heading hold.
    if (!imuUsable() && g_app.primitives.active() == proto::PrimitiveKind::kTurn) {
      g_app.primitives.cancel(proto::AckStatus::kUnavailable);
    }
    float v = 0.0f;
    MotionSetpoint sp{HeadingMode::kRate, 0.0f, 0.0f, 0.0f};
    if (g_app.primitives.tick(primInputs(), kDt, now, sp)) {
      v = sp.v_pct;
      w = imuUsable() ? g_app.heading.update(sp.mode, 0.0f, sp.rate_dps, sp.heading_deg, yaw, rate, kDt, false)
                      : 0.0f;
    } else if (g_app.teleop_rate_mode) {
      v = g_app.teleop_v;
      w = imuUsable() ? g_app.heading.update(HeadingMode::kRate, 0.0f, g_app.teleop_rate_dps, 0.0f, yaw, rate, kDt, false)
                      : g_app.teleop_rate_dps * 100.0f / gs.turn_rate_max;
    } else {
      v = g_app.teleop_v;
      w = imuUsable() ? g_app.heading.teleop(v, g_app.teleop_w, yaw, rate, kDt, false)
                      : static_cast<float>(g_app.teleop_w);
    }

    // 3. mix (turning priority, pivot fallback), envelope/degraded caps and obstacle slow in %, ramps
    const float keep_pct = (ps.deadband_keep[0] + ps.deadband_keep[1] + ps.deadband_keep[2] + ps.deadband_keep[3]) *
                           100.0f / (4.0f * 255.0f);
    float l = 0.0f, r = 0.0f;
    mix(v, w, MixerConfig{ps.turn_rate_full_speed, gs.reverse_steer == 0, gs.pivot_fallback != 0, keep_pct}, l, r);
    const uint8_t degraded = !imuUsable() ? 30 : (g_app.us.fault() ? 50 : 100);  // LLD §9.5/§9.16 (min of both)
    const Limited lim = limitTargets(l, r, Limits{g_app.envelope.speedCap(), g_app.us.distMm(), gs.obstacle_stop,
                                                  gs.obstacle_slow, g_app.envelope.brakeOn(), degraded});
    capped = lim.capped;
    // The envelope accel cap may only tighten the global limit, never loosen it.
    const float accel_lim = static_cast<float>(gs.accel_limit);
    const float accel_env = static_cast<float>(g_app.envelope.accelCap());
    const float accel = (accel_env != 0.0f && accel_env < accel_lim) ? accel_env : accel_lim;
    tgt_l = g_app.ramp_l.step(lim.l, kDt, accel, accel * 2.0f);
    tgt_r = g_app.ramp_r.step(lim.r, kDt, accel, accel * 2.0f);
    if (g_soft_stop && absf(tgt_l) < 0.5f && absf(tgt_r) < 0.5f) g_soft_stop = false;

    // 4. per-track speed control (closed loop only with a healthy encoder). The integrator is frozen while
    //    the target is capped (R4) or the supervisor cuts a stalled track (no wind-up → no lurch after 1 s).
    SpeedConfig cl = g_cfg[tgt_l >= 0.0f ? 0 : 1];
    SpeedConfig cr = g_cfg[tgt_r >= 0.0f ? 2 : 3];
    cl.closed_loop = cl.closed_loop && !g_app.enc_health.faulted(0);
    cr.closed_loop = cr.closed_loop && !g_app.enc_health.faulted(1);
    g_app.speed_l.configure(cl);
    g_app.speed_r.configure(cr);
    const bool moving_l = g_app.encoders_enabled ? g_app.enc_l.recent() : g_app.moving;
    const bool moving_r = g_app.encoders_enabled ? g_app.enc_r.recent() : g_app.moving;
    const uint8_t cap = g_app.envelope.speedCap();
    pwm_l = g_app.speed_l.update(tgt_l, g_app.enc_l.cps(), kDt, pwmLimitForCap(cap, cl.keep_pwm), moving_l,
                                 capped || g_app.enc_health.stalled(0));
    pwm_r = g_app.speed_r.update(tgt_r, g_app.enc_r.cps(), kDt, pwmLimitForCap(cap, cr.keep_pwm), moving_r,
                                 capped || g_app.enc_health.stalled(1));

    // 5. never more than motor_v_max on the 6 V motors
    const float s = batteryScale(g_app.batt.mv(), gs.motor_v_max);
    pwm_l = scaled(pwm_l, s);
    pwm_r = scaled(pwm_r, s);
  }

  // 6. safety gate → motors (req_pwm_* = asked of the supervisor, used by EncoderHealth to clear a stall)
  g_app.req_pwm_l = pwm_l;
  g_app.req_pwm_r = pwm_r;
  const SafetyInputs in{g_app.lift.lifted(), g_app.lift.tilted(),
                        g_app.batt.level() == BatteryMonitor::Level::kCritical, g_app.envelope.brakeOn(),
                        g_app.us.distMm(), gs.obstacle_stop, g_app.enc_health.stalledMask()};
  const bool was_blocked = g_app.safety.blocked;
  const PwmOutput out = g_app.supervisor.apply(pwm_l, pwm_r, in, g_app.safety);
  port::motor::apply(out);

  if ((g_app.safety.blocked || g_app.safety.halted || g_app.safety.coast) &&
      g_app.primitives.active() != proto::PrimitiveKind::kNone) {
    g_app.primitives.cancel(proto::AckStatus::kBlocked);
  }
  pushPrimitiveResult();
  if (g_app.safety.blocked && !was_blocked) g_app.events.push(proto::EventKind::kBlocked, static_cast<int16_t>(g_app.us.distMm()));
  if (!g_app.safety.blocked && was_blocked) g_app.events.push(proto::EventKind::kUnblocked);

  // 7. state + telemetry
  g_app.out_pwm_l = out.left();
  g_app.out_pwm_r = out.right();
  if (out.left() != 0 || out.right() != 0) g_app.last_motion_ms = now;
  g_app.moving = !elapsed(now, g_app.last_motion_ms, 300);
  g_app.stationary = !g_app.moving;

  setFast(proto::FastFlagBit::kBlocked, g_app.safety.blocked);
  setFast(proto::FastFlagBit::kHalted, g_app.safety.halted);
  setFast(proto::FastFlagBit::kCapped, capped || g_app.speed_l.saturated() || g_app.speed_r.saturated());
  setFast(proto::FastFlagBit::kTurnLimited, absf(w) >= 99.9f);
  setFast(proto::FastFlagBit::kSoftStop, g_soft_stop);
  setFast(proto::FastFlagBit::kLocked, g_app.envelope.lockBrake());
  setFast(proto::FastFlagBit::kCalibrating, calibrating);
  setFast(proto::FastFlagBit::kStallL, g_app.enc_health.stalled(0));
  setFast(proto::FastFlagBit::kStallR, g_app.enc_health.stalled(1));
  setFast(proto::FastFlagBit::kEncoderFault, g_app.enc_health.faulted(0) || g_app.enc_health.faulted(1));
  setFast(proto::FastFlagBit::kSynced, g_app.link.synced());

  proto::TelemFastMsg& f = g_app.fast;
  f.pwm_l = out.left();
  f.pwm_r = out.right();
  f.spd_cmd_l = static_cast<int8_t>(tgt_l);
  f.spd_cmd_r = static_cast<int8_t>(tgt_r);
  f.flags = g_app.fast_flags;
  f.prim = static_cast<uint8_t>(g_app.primitives.active());
  f.prim_pct = g_app.primitives.progressPct();
  g_app.slow.owner = static_cast<uint8_t>(g_app.arbitrator.owner());
  g_app.slow.driving = g_app.moving ? 1 : 0;
}

}  // namespace app
}  // namespace cq
```

Note: `proto::AckStatus::kBlocked` is the obstacle/safety cancel reason (ack_status index 2, `BLOCKED`).

- [ ] **Step 4: Route rows**

In `src/app/routes.cpp` add before `// ROUTES-END`:
```cpp
    {static_cast<uint8_t>(proto::FrameType::kDriveRate), onDriveRate, false},
    {static_cast<uint8_t>(proto::FrameType::kTurn), onTurn, true},
    {static_cast<uint8_t>(proto::FrameType::kMove), onMove, true},
    {static_cast<uint8_t>(proto::FrameType::kCancel), onCancel, true},
    {static_cast<uint8_t>(proto::FrameType::kProfile), onProfile, true},
```

- [ ] **Step 5: Smoke commands for primitives**

Every smoke run reopens the serial port, which reboots the Uno (DTR) and clears its RAM state (owner, envelope, a running primitive). So each floor check below runs in **one** process: cancel, link loss, a second turn and the envelope are options of the same command (ruling R13).

In `tools/smoke/uno_smoke.py`, class `Link` (Task 2): in `__init__` add `self.paused = False  # True: send nothing (simulated link loss)` after `self.synced = False`, and make the first line of `send()` (before `with self.lock:`) `if self.paused: return -1`. That stops heartbeats, commands and the HELLO_ACK reply alike, while RX keeps printing.

In `cmd_drive` (Task 6) replace the envelope block with
```python
    if args.envelope is not None:
        lock = 1 if args.lock else 0
        link.send(g.Envelope(speed_cap=args.envelope, accel_cap=args.accel, lock_brake=lock))
        time.sleep(0.1)
```
and add to the `drive` parser:
```python
    d.add_argument("--accel", type=int, default=0, help="envelope accel cap %%/s (with --envelope)")
    d.add_argument("--lock", action="store_true", help="envelope locks the obstacle brake on")
```

Add to `tools/smoke/uno_smoke.py`:
```python
def watch_primitive(link: Link) -> threading.Event:
    """Print ACKs/events; the returned flag is set by PRIMITIVE_DONE or by a refused command."""
    done = threading.Event()

    def on(msg: object) -> None:
        if isinstance(msg, g.Ack):
            print(f"ACK {g.ACK_STATUS[msg.status]}")
            if msg.status != 0:
                done.set()
        elif isinstance(msg, g.Event):
            print(fmt_event(msg))
            if msg.kind == g.EventKind.PRIMITIVE_DONE:
                name = g.PrimitiveKind(msg.a & 0xFF).name
                res = "OK" if (msg.a >> 8) & 1 else "FAILED"
                print(f"  {name} {res} reason={g.ACK_STATUS[msg.b]}")
                done.set()

    link.handlers.append(on)
    return done


def print_pose(link: Link) -> None:
    fast = link.last.get("TelemFast")
    if isinstance(fast, g.TelemFast):
        pose = f"yaw={fast.yaw / 100:.1f} enc={fast.enc_l},{fast.enc_r}"
        print(f"{pose} pwm={fast.pwm_l},{fast.pwm_r}")


def wait_primitive(link: Link, done: threading.Event, timeout: float) -> None:
    done.wait(timeout)
    done.clear()
    print_pose(link)


def send_turn(link: Link, args: argparse.Namespace, angle: float, to: bool) -> None:
    mode = (g.TurnMode.TO if to else g.TurnMode.BY) | (0x80 if args.pivot else 0)
    link.send(g.Turn(src=g.Source.HUMAN, mode=mode, angle=round(angle * 10), speed=args.speed,
                     radius_cm=args.radius))


def cmd_turn(link: Link, args: argparse.Namespace) -> None:
    done = watch_primitive(link)
    wait_synced(link)
    send_turn(link, args, args.angle, args.to)
    if args.cancel_after is not None:
        time.sleep(args.cancel_after)
        link.send(g.Cancel(src=g.Source.HUMAN))
    if args.drop_link_after is not None:
        time.sleep(args.drop_link_after)
        link.paused = True  # no heartbeats: the Uno declares LINK_LOST after 300 ms
        print("link paused")
        time.sleep(args.restore_after)
        link.paused = False
        print("link restored")
        for _ in range(6):  # the turn must not resume: yaw stays put, pwm stays 0
            time.sleep(0.5)
            print_pose(link)
        return
    wait_primitive(link, done, 30)
    if args.then_to is not None:
        send_turn(link, args, args.then_to, True)
        wait_primitive(link, done, 30)


def cmd_move(link: Link, args: argparse.Namespace) -> None:
    done = watch_primitive(link)
    wait_synced(link)
    heading = 32767 if args.heading is None else round(args.heading * 10)
    link.send(g.Move(src=g.Source.HUMAN, dist_mm=args.mm, speed=args.speed, heading=heading))
    wait_primitive(link, done, 60)


def cmd_cancel(link: Link, args: argparse.Namespace) -> None:
    done = watch_primitive(link)
    wait_synced(link)
    link.send(g.Cancel(src=g.Source.HUMAN))
    wait_primitive(link, done, 2)


def cmd_driverate(link: Link, args: argparse.Namespace) -> None:
    link.handlers.append(lambda m: print(fmt_event(m)) if isinstance(m, g.Event) else None)
    wait_synced(link)
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        link.send(g.DriveRate(src=g.Source.HUMAN, v=args.v, yaw_rate=round(args.rate * 10)))
        fast = link.last.get("TelemFast")
        if isinstance(fast, g.TelemFast):
            print(f"rate={fast.yaw_rate / 10:.1f} pwm={fast.pwm_l},{fast.pwm_r}")
        time.sleep(0.05)
```
Register `"turn": cmd_turn, "move": cmd_move, "cancel": cmd_cancel, "driverate": cmd_driverate` and in `main()`:
```python
    t = sub.add_parser("turn")
    t.add_argument("angle", type=float)
    t.add_argument("speed", type=int)
    t.add_argument("--radius", type=int, default=0)
    t.add_argument("--to", action="store_true")
    t.add_argument("--pivot", action="store_true")
    t.add_argument("--then-to", type=float, default=None, help="then TURN to this heading")
    t.add_argument("--cancel-after", type=float, default=None, help="send CANCEL after S s")
    t.add_argument("--drop-link-after", type=float, default=None, help="pause all TX after S s")
    t.add_argument("--restore-after", type=float, default=2.0, help="resume TX after S s")
    mv = sub.add_parser("move")
    mv.add_argument("mm", type=int)
    mv.add_argument("speed", type=int)
    mv.add_argument("--heading", type=float)
    sub.add_parser("cancel")
    dr = sub.add_parser("driverate")
    dr.add_argument("v", type=int)
    dr.add_argument("rate", type=float)
    dr.add_argument("--seconds", type=float, default=3.0)
```
Run (from `tools/smoke/`): `uv run ruff check . && uv run pyright`
Expected: `All checks passed!`, `0 errors, 0 warnings, 0 informations`.

- [ ] **Step 6: Build, flash, floor tests (tethered USB, long cable, clear floor ≥ 2 × 2 m)**

Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py && pio run -e uno -t upload`
Then from `tools/smoke/` (encoders enabled and `bias` done in Task 7). Each line is one self-contained run (the Uno reboots when the script opens the port):
1. Heading hold: `… drive 40 0 --seconds 4` → `yaw` stays within ±2° of its start; without IMU usable the tank would drift.
2. Spin: `… turn 90 50` → `PRIMITIVE_DONE`, `TURN OK`, printed `yaw` within ±2° of 90 (yaw starts at 0 after the reboot); `… turn -360 50` → back within ±2° of 0.
3. Turn-to and arc: `… turn 90 50 --then-to 0` → two `TURN OK`, final `yaw` within ±2° of 0; `… turn 90 50 --radius 50` drives a quarter circle (radius ≈ 50 cm by tape).
4. Move: `… move 1000 40` → `MOVE OK`; tape: 100 cm ± 3 cm. If off, set `counts_per_cm` = 7600 × (enc distance / tape distance) via `… setting counts_per_cm <v> --save`.
5. Cancel and link loss: `… turn 720 30 --cancel-after 2` → `TURN FAILED reason=CANCELLED`, tank stops. `… turn 720 30 --drop-link-after 1 --restore-after 2` → tank stops within ≈0.3 s of `link paused`; `EVENT LINK_LOST` and `TURN FAILED reason=LINK_LOST` print during the pause or right after `link restored`; after `link restored` the six `yaw=… pwm=0,0` lines show the turn does **not** resume.
6. Child-play envelope: `… drive 100 0 --seconds 3 --envelope 20 --accel 50 --lock` → `pwm` ≤ 115, `flags` bit 7 (`CAPPED`, 0x0080) set, bit 10 (`LOCKED`) set, tank crawls and takes ≈0.4 s to reach its 20 % (accel cap 50 %/s instead of `accel_limit` 200 %/s).
7. Obstacle: drive `… drive 60 0 --seconds 6` toward a wall → slows from 50 cm, `EVENT BLOCKED` at ≈ 15 cm, stops; `… drive -40 0` backs away.
8. Deadman: `… deadman` → `EVENT DEADMAN_STOP` ≈300 ms after the last DRIVE, `motors stopped` ≤ ~420 ms after it (300 ms deadman + SoftStop ramp at 2 × `accel_limit` + one 50 ms TelemFast period). `… move 300 30` (no DRIVE stream at all) → `MOVE OK`: the deadman and the 1 s idle release do not apply while a primitive runs.
9. Degraded cap (stand, tracks off the ground): unplug the ultrasonic sensor, `… drive 100 0 --seconds 3` → within ≈1 s `flags` has `US_FAULT` (0x1000) and `CAPPED` (0x0080), tracks at ≈ half speed (target capped to 50 %). Plug it back in.

- [ ] **Step 7: Commit**

```bash
git add firmware/uno tools/smoke
git commit -m "feat(uno): closed-loop control pipeline with gyro heading, primitives, envelope caps, obstacle slow/stop"
```

---

### Task 12: Camera pan/tilt — smooth servo profiles, joystick hold/return-home, lock, CAM_* routes

**Files:**
- Create: `src/domain/camera/servo_profile.{h,cpp}`, `camera_head.{h,cpp}`; `src/ports/servos.h`; `src/drivers/servos_uno.cpp`; `src/app/camera_api.h`, `src/app/camera_task.cpp`
- Modify: `src/app/app.h`, `src/app/tasks.h`, `src/app/routes.cpp`, `src/app/apply_settings.cpp`, `src/main.cpp`, `tools/smoke/uno_smoke.py`
- Test: `test/test_camera/test_main.cpp`

**Interfaces:**
- Consumes: `Axis` (Task 1), `GlobalSettings` servo fields (Task 3), `proto::CamJoyMsg/CamSetMsg/CamNudgeMsg/CamHomeMsg/CamLockMsg`.
- Produces:
  - `cq::ServoProfile`: `void reset(float pos_deg)`, `void setTarget(float deg)`, `void setLimits(float v_max_dps, float a_max_dps2)`, `float step(float dt_s)`, `float position() const`, `float target() const`, `bool moving() const`.
  - `cq::CameraConfig { uint8_t pan_min, pan_max, tilt_min, tilt_max; int16_t pan_trim_dd, tilt_trim_dd; uint16_t speed_dps, accel_dps2, return_delay_ms; bool idle_detach; }`; `cq::CameraHead`: `void configure(const CameraConfig&)`, `void joy(int8_t x, int8_t y, bool active, uint32_t now_ms)`, `void set(float pan_deg, float tilt_deg, float speed_dps)`, `void nudge(float dpan_deg, float dtilt_deg)`, `void home()`, `void lock(bool on)`, `void tick(float dt_s, uint32_t now_ms)`, `float panDeg() const`, `float tiltDeg() const`, `float panOutDeg() const`, `float tiltOutDeg() const`, `bool moving() const`, `bool wantAttached() const`; constants `kHomePan = 90`, `kHomeTilt = 90`, `kJoyTimeoutMs = 500`, `kDetachAfterMs = 2000`.
  - Port `cq::port::servos`: `begin()`, `attach(Axis)`, `detach(Axis)`, `writeDeg(Axis, float deg)`.
  - `app/tasks.h`: `cq::app::cameraInit()`, `cameraTask()` (50 Hz); `app/camera_api.h` (like `motion_api.h`): `configureCamera()`, handlers `onCamJoy/onCamSet/onCamNudge/onCamHome/onCamLock`.
  - Smoke: `cam joy X Y S [--lock] [--drive V W]`, `cam set|nudge|home`.

- [ ] **Step 1: Write the failing camera tests**

`test/test_camera/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/camera/camera_head.h"
#include "domain/camera/servo_profile.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
const CameraConfig kCfg{0, 180, 30, 110, 0, 0, 120, 400, 500, false};

void run(CameraHead& h, uint32_t& t, uint32_t until_ms) {
  for (; t < until_ms; t += 20) h.tick(0.02f, t);
}

// The ESP32 streams CAM_JOY at 20 Hz while the stick is held.
void runJoy(CameraHead& h, uint32_t& t, uint32_t until_ms, int8_t x, int8_t y) {
  for (; t < until_ms; t += 20) {
    if (t % 50 < 20) h.joy(x, y, true, t);
    h.tick(0.02f, t);
  }
}
}  // namespace

void test_profile_respects_speed_and_lands_exactly() {
  ServoProfile p;
  p.reset(90.0f);
  p.setLimits(120.0f, 400.0f);
  p.setTarget(150.0f);
  float max_step = 0.0f;
  float prev = 90.0f;
  int ticks = 0;
  while (p.moving() && ticks < 500) {
    const float pos = p.step(0.02f);
    const float stepd = pos - prev;
    if (stepd > max_step) max_step = stepd;
    prev = pos;
    ++ticks;
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 150.0f, p.position());
  TEST_ASSERT_TRUE(max_step <= 120.0f * 0.02f + 1e-3f);
  TEST_ASSERT_INT_WITHIN(5, 40, ticks);  // 60° at 120 °/s ≈ 0.5 s + 0.3 s accel/decel
}

void test_joy_is_absolute_and_release_returns_home() {
  CameraHead h;
  h.configure(kCfg);
  uint32_t t = 0;
  runJoy(h, t, 2000, 100, 0);  // full right
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f, h.panDeg());
  runJoy(h, t, 3000, 0, 100);  // straight, full up
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f, h.panDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 110.0f, h.tiltDeg());
  h.joy(0, 0, false, t);   // released
  run(h, t, t + 400);      // within the 500 ms return delay: stays
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 110.0f, h.tiltDeg());
  run(h, t, t + 1500);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, CameraHead::kHomeTilt, h.tiltDeg());
}

void test_lock_holds_position_on_release() {
  CameraHead h;
  h.configure(kCfg);
  uint32_t t = 0;
  h.lock(true);
  runJoy(h, t, 1500, -50, 0);
  h.joy(0, 0, false, t);
  run(h, t, t + 2000);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 135.0f, h.panDeg());
}

void test_joy_stream_timeout_counts_as_release() {
  CameraHead h;
  h.configure(kCfg);
  uint32_t t = 0;
  runJoy(h, t, 1500, 0, -100);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 30.0f, h.tiltDeg());
  run(h, t, 4000);  // stream stops: 500 ms timeout + 500 ms delay, then home
  TEST_ASSERT_FLOAT_WITHIN(0.5f, CameraHead::kHomeTilt, h.tiltDeg());
}

void test_set_clamps_and_nudge_is_relative() {
  CameraHead h;
  h.configure(kCfg);
  uint32_t t = 0;
  h.set(200.0f, 10.0f, 0.0f);
  run(h, t, 3000);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 180.0f, h.panDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 30.0f, h.tiltDeg());
  h.nudge(-10.0f, 5.0f);
  run(h, t, 4000);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 170.0f, h.panDeg());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 35.0f, h.tiltDeg());
  h.home();
  run(h, t, 6000);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f, h.panDeg());
}

void test_trim_applies_to_output_only() {
  CameraConfig c = kCfg;
  c.pan_trim_dd = -25;
  CameraHead h;
  h.configure(c);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 90.0f, h.panDeg());
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 87.5f, h.panOutDeg());
}

void test_idle_detach() {
  CameraConfig c = kCfg;
  c.idle_detach = true;
  CameraHead h;
  h.configure(c);
  uint32_t t = 0;
  h.nudge(10.0f, 0.0f);
  run(h, t, 500);
  TEST_ASSERT_TRUE(h.wantAttached());
  run(h, t, 3000);
  TEST_ASSERT_FALSE(h.wantAttached());
  h.nudge(5.0f, 0.0f);
  TEST_ASSERT_TRUE(h.wantAttached());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_profile_respects_speed_and_lands_exactly);
  RUN_TEST(test_joy_is_absolute_and_release_returns_home);
  RUN_TEST(test_lock_holds_position_on_release);
  RUN_TEST(test_joy_stream_timeout_counts_as_release);
  RUN_TEST(test_set_clamps_and_nudge_is_relative);
  RUN_TEST(test_trim_applies_to_output_only);
  RUN_TEST(test_idle_detach);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_camera`
Expected: FAIL — `domain/camera/camera_head.h: No such file or directory`.

- [ ] **Step 3: Implement the servo profile and camera head**

`src/domain/camera/servo_profile.h`:
```cpp
#pragma once

namespace cq {

// Trapezoidal velocity profile for one servo axis (degrees).
class ServoProfile {
 public:
  void reset(float pos_deg) {
    m_pos = pos_deg;
    m_target = pos_deg;
    m_vel = 0.0f;
  }
  void setTarget(float deg) { m_target = deg; }
  void setLimits(float v_max_dps, float a_max_dps2) {
    m_v_max = v_max_dps;
    m_a_max = a_max_dps2;
  }
  float step(float dt_s);
  float position() const { return m_pos; }
  float target() const { return m_target; }
  bool moving() const { return m_pos != m_target || m_vel != 0.0f; }

 private:
  float m_pos = 90.0f;
  float m_target = 90.0f;
  float m_vel = 0.0f;
  float m_v_max = 120.0f;
  float m_a_max = 400.0f;
};

}  // namespace cq
```

`src/domain/camera/servo_profile.cpp`:
```cpp
#include "domain/camera/servo_profile.h"

#include <math.h>

#include "domain/util/math_util.h"

namespace cq {

float ServoProfile::step(float dt_s) {
  const float dist = m_target - m_pos;
  if (absf(dist) < 0.05f && absf(m_vel) < m_a_max * dt_s) {
    m_pos = m_target;
    m_vel = 0.0f;
    return m_pos;
  }
  float v_des = sqrtf(2.0f * m_a_max * absf(dist));
  if (v_des > m_v_max) v_des = m_v_max;
  v_des *= signf(dist);
  const float dv = m_a_max * dt_s;
  m_vel += clampf(v_des - m_vel, -dv, dv);
  const float next = m_pos + m_vel * dt_s;
  if ((m_target - next) * dist <= 0.0f) {  // would pass the target: land on it
    m_pos = m_target;
    m_vel = 0.0f;
  } else {
    m_pos = next;
  }
  return m_pos;
}

}  // namespace cq
```

`src/domain/camera/camera_head.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/camera/servo_profile.h"

namespace cq {

struct CameraConfig {
  uint8_t pan_min;
  uint8_t pan_max;
  uint8_t tilt_min;
  uint8_t tilt_max;
  int16_t pan_trim_dd;   // 0.1°
  int16_t tilt_trim_dd;
  uint16_t speed_dps;
  uint16_t accel_dps2;
  uint16_t return_delay_ms;
  bool idle_detach;
};

// Pan/tilt for the ESP32 camera. CAM_JOY is absolute (stick position = angle); on release (or 500 ms
// without CAM_JOY) the head returns home after return_delay unless locked. Pan: x > 0 = right = lower angle.
class CameraHead {
 public:
  static constexpr float kHomePan = 90.0f;
  static constexpr float kHomeTilt = 90.0f;
  static constexpr uint16_t kJoyTimeoutMs = 500;
  static constexpr uint16_t kDetachAfterMs = 2000;

  void configure(const CameraConfig& cfg);
  void joy(int8_t x, int8_t y, bool active, uint32_t now_ms);
  void set(float pan_deg, float tilt_deg, float speed_dps);
  void nudge(float dpan_deg, float dtilt_deg);
  void home();
  void lock(bool on) { m_locked = on; }
  void tick(float dt_s, uint32_t now_ms);
  float panDeg() const { return m_pan.position(); }
  float tiltDeg() const { return m_tilt.position(); }
  float panOutDeg() const { return m_pan.position() + m_cfg.pan_trim_dd * 0.1f; }
  float tiltOutDeg() const { return m_tilt.position() + m_cfg.tilt_trim_dd * 0.1f; }
  bool moving() const { return m_pan.moving() || m_tilt.moving(); }
  bool wantAttached() const { return !m_cfg.idle_detach || !m_idle; }

 private:
  void target(float pan, float tilt);
  void speed(float dps);
  CameraConfig m_cfg{0, 180, 30, 110, 0, 0, 120, 400, 500, false};
  ServoProfile m_pan;
  ServoProfile m_tilt;
  bool m_joy_active = false;
  uint32_t m_last_joy_ms = 0;
  bool m_return_pending = false;
  uint32_t m_release_ms = 0;
  bool m_locked = false;
  bool m_idle = false;
  uint32_t m_still_since_ms = 0;
  bool m_was_moving = false;
  bool m_configured = false;
};

}  // namespace cq
```

`src/domain/camera/camera_head.cpp`:
```cpp
#include "domain/camera/camera_head.h"

#include "domain/util/math_util.h"

namespace cq {

void CameraHead::configure(const CameraConfig& cfg) {
  m_cfg = cfg;
  speed(cfg.speed_dps);
  if (!m_configured) {  // first call: start at home; later calls (setting changes) keep the position
    m_pan.reset(clampf(kHomePan, cfg.pan_min, cfg.pan_max));
    m_tilt.reset(clampf(kHomeTilt, cfg.tilt_min, cfg.tilt_max));
    m_configured = true;
  } else {
    target(m_pan.target(), m_tilt.target());  // re-clamp to new limits
  }
}

void CameraHead::speed(float dps) {
  m_pan.setLimits(dps, m_cfg.accel_dps2);
  m_tilt.setLimits(dps, m_cfg.accel_dps2);
}

void CameraHead::target(float pan, float tilt) {
  m_pan.setTarget(clampf(pan, m_cfg.pan_min, m_cfg.pan_max));
  m_tilt.setTarget(clampf(tilt, m_cfg.tilt_min, m_cfg.tilt_max));
  m_idle = false;
}

void CameraHead::joy(int8_t x, int8_t y, bool active, uint32_t now_ms) {
  if (!active) {
    if (m_joy_active) {
      m_joy_active = false;
      m_return_pending = !m_locked;
      m_release_ms = now_ms;
    }
    return;
  }
  m_joy_active = true;
  m_return_pending = false;
  m_last_joy_ms = now_ms;
  speed(m_cfg.speed_dps);
  const float home_pan = clampf(kHomePan, m_cfg.pan_min, m_cfg.pan_max);
  const float home_tilt = clampf(kHomeTilt, m_cfg.tilt_min, m_cfg.tilt_max);
  const float pan = x >= 0 ? home_pan - x / 100.0f * (home_pan - m_cfg.pan_min)
                           : home_pan - x / 100.0f * (m_cfg.pan_max - home_pan);
  const float tilt = y >= 0 ? home_tilt + y / 100.0f * (m_cfg.tilt_max - home_tilt)
                            : home_tilt + y / 100.0f * (home_tilt - m_cfg.tilt_min);
  target(pan, tilt);
}

void CameraHead::set(float pan_deg, float tilt_deg, float speed_dps) {
  m_joy_active = false;
  m_return_pending = false;
  speed(speed_dps > 0.0f ? speed_dps : m_cfg.speed_dps);
  target(pan_deg, tilt_deg);
}

void CameraHead::nudge(float dpan_deg, float dtilt_deg) {
  m_return_pending = false;
  speed(m_cfg.speed_dps);
  target(m_pan.target() + dpan_deg, m_tilt.target() + dtilt_deg);
}

void CameraHead::home() {
  m_joy_active = false;
  m_return_pending = false;
  speed(m_cfg.speed_dps);
  target(kHomePan, kHomeTilt);
}

void CameraHead::tick(float dt_s, uint32_t now_ms) {
  if (m_joy_active && elapsed(now_ms, m_last_joy_ms, kJoyTimeoutMs)) joy(0, 0, false, m_last_joy_ms + kJoyTimeoutMs);
  if (m_return_pending && elapsed(now_ms, m_release_ms, m_cfg.return_delay_ms)) {
    m_return_pending = false;
    target(kHomePan, kHomeTilt);
  }
  m_pan.step(dt_s);
  m_tilt.step(dt_s);
  const bool mv = moving();
  if (mv) {
    m_idle = false;
  } else if (m_was_moving) {
    m_still_since_ms = now_ms;
  } else if (elapsed(now_ms, m_still_since_ms, kDetachAfterMs)) {
    m_idle = true;
  }
  m_was_moving = mv;
}

}  // namespace cq
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_camera`
Expected: `7 Tests 0 Failures`.

- [ ] **Step 5: Servo port and driver**

`src/ports/servos.h`:
```cpp
#pragma once
#include "types/hw_types.h"

namespace cq {
namespace port {
namespace servos {
void begin();
void attach(Axis a);
void detach(Axis a);
void writeDeg(Axis a, float deg);  // 0..180 → 544..2400 µs
}  // namespace servos
}  // namespace port
}  // namespace cq
```

`src/drivers/servos_uno.cpp`:
```cpp
#include <Arduino.h>
#include <Servo.h>

#include "drivers/board_config.h"
#include "ports/servos.h"

namespace cq {
namespace port {
namespace servos {
namespace {
Servo g_servo[2];
const uint8_t kPins[2] = {board::kPinServoPan, board::kPinServoTilt};
}  // namespace

void begin() {}

void attach(Axis a) {
  Servo& s = g_servo[static_cast<uint8_t>(a)];
  if (!s.attached()) s.attach(kPins[static_cast<uint8_t>(a)], 544, 2400);
}

void detach(Axis a) {
  Servo& s = g_servo[static_cast<uint8_t>(a)];
  if (s.attached()) s.detach();
}

void writeDeg(Axis a, float deg) {
  if (deg < 0.0f) deg = 0.0f;
  if (deg > 180.0f) deg = 180.0f;
  g_servo[static_cast<uint8_t>(a)].writeMicroseconds(static_cast<int>(544.0f + deg * (1856.0f / 180.0f)));
}

}  // namespace servos
}  // namespace port
}  // namespace cq
```

- [ ] **Step 6: Camera task, routes and settings**

Add to `src/app/app.h`: `#include "domain/camera/camera_head.h"` and member `CameraHead camera;`.

`src/app/camera_api.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace app {

// cameraInit/cameraTask are declared in app/tasks.h.
void configureCamera();  // re-read servo settings (applySettings)

proto::AckStatus onCamJoy(const uint8_t* p, uint8_t len);
proto::AckStatus onCamSet(const uint8_t* p, uint8_t len);
proto::AckStatus onCamNudge(const uint8_t* p, uint8_t len);
proto::AckStatus onCamHome(const uint8_t* p, uint8_t len);
proto::AckStatus onCamLock(const uint8_t* p, uint8_t len);

}  // namespace app
}  // namespace cq
```

In `src/app/tasks.h` add before `// TASK-DECLS-END`:
```cpp
void cameraInit();  // boot: servos to the home pulse, then attach
void cameraTask();  // 50 Hz
```

`src/app/camera_task.cpp`:
```cpp
#include "app/app.h"
#include "app/camera_api.h"
#include "app/tasks.h"
#include "ports/clock.h"
#include "ports/servos.h"

namespace cq {
namespace app {

void configureCamera() {
  const GlobalSettings& gs = g_app.settings.global();
  g_app.camera.configure(CameraConfig{gs.pan_min, gs.pan_max, gs.tilt_min, gs.tilt_max, gs.pan_trim, gs.tilt_trim,
                                      gs.servo_speed, gs.servo_accel, gs.cam_return_delay, gs.servo_idle_detach != 0});
}

void cameraInit() {
  port::servos::begin();
  port::servos::writeDeg(Axis::kPan, g_app.camera.panOutDeg());
  port::servos::writeDeg(Axis::kTilt, g_app.camera.tiltOutDeg());
  port::servos::attach(Axis::kPan);
  port::servos::attach(Axis::kTilt);
}

void cameraTask() {
  g_app.camera.tick(0.02f, port::clock::nowMs());
  if (g_app.camera.wantAttached()) {
    port::servos::attach(Axis::kPan);
    port::servos::attach(Axis::kTilt);
    port::servos::writeDeg(Axis::kPan, g_app.camera.panOutDeg());
    port::servos::writeDeg(Axis::kTilt, g_app.camera.tiltOutDeg());
  } else {
    port::servos::detach(Axis::kPan);
    port::servos::detach(Axis::kTilt);
  }
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(proto::FastFlagBit::kCamMoving));
  g_app.fast_flags = g_app.camera.moving() ? static_cast<uint16_t>(g_app.fast_flags | m)
                                           : static_cast<uint16_t>(g_app.fast_flags & ~m);
  g_app.fast.pan = static_cast<int16_t>(g_app.camera.panDeg() * 10.0f);
  g_app.fast.tilt = static_cast<int16_t>(g_app.camera.tiltDeg() * 10.0f);
}

proto::AckStatus onCamJoy(const uint8_t* p, uint8_t len) {
  proto::CamJoyMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.joy(m.x, m.y, m.active != 0, port::clock::nowMs());
  return proto::AckStatus::kOk;
}

proto::AckStatus onCamSet(const uint8_t* p, uint8_t len) {
  proto::CamSetMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.set(m.pan * 0.1f, m.tilt * 0.1f, m.speed * 2.0f);
  return proto::AckStatus::kOk;
}

proto::AckStatus onCamNudge(const uint8_t* p, uint8_t len) {
  proto::CamNudgeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.nudge(m.dpan * 0.1f, m.dtilt * 0.1f);
  return proto::AckStatus::kOk;
}

proto::AckStatus onCamHome(const uint8_t* p, uint8_t len) {
  proto::CamHomeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.home();
  return proto::AckStatus::kOk;
}

proto::AckStatus onCamLock(const uint8_t* p, uint8_t len) {
  proto::CamLockMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  g_app.camera.lock(m.on != 0);
  return proto::AckStatus::kOk;
}

}  // namespace app
}  // namespace cq
```

In `src/app/routes.cpp` add `#include "app/camera_api.h"` and rows before `// ROUTES-END`:
```cpp
    {static_cast<uint8_t>(proto::FrameType::kCamJoy), onCamJoy, false},
    {static_cast<uint8_t>(proto::FrameType::kCamSet), onCamSet, true},
    {static_cast<uint8_t>(proto::FrameType::kCamNudge), onCamNudge, true},
    {static_cast<uint8_t>(proto::FrameType::kCamHome), onCamHome, true},
    {static_cast<uint8_t>(proto::FrameType::kCamLock), onCamLock, true},
```
In `src/app/apply_settings.cpp` add `#include "app/camera_api.h"` and the line `configureCamera();` before `// APPLY-END`.
In `src/main.cpp` (already includes `app/tasks.h`): call `cq::app::cameraInit();` before `// SETUP-END` (after `settingsLoad()`); task row `{cq::app::cameraTask, 20000UL, 0},` before `// TASKS-END`.

- [ ] **Step 7: Smoke command for the camera**

Add to `tools/smoke/uno_smoke.py`:
```python
def cmd_cam(link: Link, args: argparse.Namespace) -> None:
    """cam joy X Y SECONDS [--lock] [--drive V W] | set PAN TILT | nudge DPAN DTILT | home"""
    link.handlers.append(print_ack)
    wait_synced(link)
    v = args.values
    if args.lock:  # same run: reopening the port reboots the Uno, which clears the lock
        link.send(g.CamLock(on=1))
    if args.action == "joy":
        end = time.monotonic() + v[2]
        while time.monotonic() < end:
            link.send(g.CamJoy(x=int(v[0]), y=int(v[1]), active=1))
            if args.drive is not None:  # drive while panning, same 50 ms loop
                link.send(g.Drive(src=g.Source.HUMAN, v=args.drive[0], w=args.drive[1]))
            time.sleep(0.05)
        link.send(g.CamJoy(x=0, y=0, active=0))
    elif args.action == "set":
        link.send(g.CamSet(pan=round(v[0] * 10), tilt=round(v[1] * 10), speed=0))
    elif args.action == "nudge":
        link.send(g.CamNudge(dpan=round(v[0] * 10), dtilt=round(v[1] * 10)))
    elif args.action == "home":
        link.send(g.CamHome())
    t0 = time.monotonic()
    while time.monotonic() - t0 < 2.0:
        fast = link.last.get("TelemFast")
        if isinstance(fast, g.TelemFast):
            print(f"pan={fast.pan / 10:.1f} tilt={fast.tilt / 10:.1f}")
        time.sleep(0.25)
    slow = link.last.get("TelemSlow")
    if isinstance(slow, g.TelemSlow):
        print(f"SLOW load={slow.loop_load}% max={slow.loop_max_us}us")
```
Register `"cam": cmd_cam` and in `main()`:
```python
    cm = sub.add_parser("cam")
    cm.add_argument("action", choices=["joy", "set", "nudge", "home"])
    cm.add_argument("values", type=float, nargs="*")
    cm.add_argument("--lock", action="store_true", help="send CAM_LOCK on first (same run)")
    cm.add_argument("--drive", type=int, nargs=2, default=None, metavar=("V", "W"))
```
Run (from `tools/smoke/`): `uv run ruff check . && uv run pyright`
Expected: `All checks passed!`, `0 errors, 0 warnings, 0 informations`.

- [ ] **Step 8: Build, flash, check on the stand**

Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py && pio run -e uno -t upload`
Each line is one self-contained run (opening the port reboots the Uno, which clears the lock):
1. Power on: the head snaps to 90/90 once (its start position is unknown, so this first move cannot be smoothed); after that every move is smooth with no jumps.
2. `… cam joy 100 0 2` → pans smoothly fully right (camera looks right), holds while the stick is held, returns home ≈ 0.5 s after release.
3. `… cam joy -50 50 2 --lock` → `ACK OK` (lock), then after the release the head stays at the released position for the 2 s watch (no return home).
4. `… cam set 200 0` → stops at the configured limits (180 / 30); no buzzing at the end stops. If the head is off-centre at 90/90, set `pan_trim`/`tilt_trim` (0.1°) with `… setting pan_trim <v> --save`. (Writing the limits themselves: keep `pan_min ≤ pan_max` / `tilt_min ≤ tilt_max` after every single write, else `SETTING_OUT_OF_RANGE`.)
5. Drive while panning (stand, tracks off the ground): `… cam joy 100 0 3 --drive 40 0` → tracks and head move together with no stutter in either; the final `SLOW load` < 60 %.

- [ ] **Step 9: Commit**

```bash
git add firmware/uno tools/smoke
git commit -m "feat(uno): camera pan/tilt with trapezoid servo profiles, joystick hold/return, lock, CAM routes"
```

---

### Task 13: Sounds and LED — sequencers, annunciator, catalogue, tone + WS2812 drivers, LED/SOUND/TONE/AUDIO_MODE

**Files:**
- Create: `src/domain/ui/sound_sequencer.{h,cpp}`, `led_sequencer.{h,cpp}`, `catalogues.{h,cpp}`, `annunciator.{h,cpp}`; `src/ports/tone.h`, `led.h`; `src/drivers/tone_uno.cpp`, `ws2812_uno.cpp`; `src/app/ui_api.h`, `src/app/ui_task.cpp`
- Modify: `src/app/app.h`, `src/app/tasks.h`, `src/app/imu_task.cpp`, `src/app/routes.cpp`, `src/app/comms.cpp`, `src/main.cpp`, `tools/smoke/uno_smoke.py`
- Test: `test/test_ui/test_main.cpp`

**Interfaces:**
- Consumes: `Rgb` (Task 1), `proto::SoundId`, `proto::LedMode`, `proto::AudioModeValue`, `proto::Owner`, `proto::OwnerHintValue`, `proto::LedMsg`, `proto::SoundMsg`, `proto::ToneMsg` (`notes[i].hz/.ms`), `proto::AudioModeMsg`; `g_app.deadman_latched` (Task 11) → `flags2.deadman`.
- Produces:
  - `cq::Note { uint16_t hz; uint16_t ms; }`; `cq::SoundSequencer`: `bool play(const Note* notes, uint8_t n, uint8_t priority, uint32_t now_ms)`, `void setMode(proto::AudioModeValue m)`, `proto::AudioModeValue mode() const`, `void stop()`, `bool tick(uint32_t now_ms, uint16_t& hz)` (true when the output changed; 0 = silence), `bool playing() const`; priorities `kPrioInfo = 1, kPrioEvent = 2, kPrioAlarm = 3`.
  - `cq::LedLayer : uint8_t { kAlarm, kAttention, kCustom, kStatus, kBase, kCount }` (lower = wins); `cq::LedPattern { proto::LedMode mode; Rgb color; uint16_t period_ms; uint16_t duration_ms; }`; `cq::LedSequencer`: `void set(LedLayer, const LedPattern&, uint32_t now_ms)`, `void clear(LedLayer)`, `Rgb output(uint32_t now_ms)`.
  - `cq::soundNotes(proto::SoundId id, Note* out) -> uint8_t` (count, PROGMEM catalogue).
  - `cq::AnnState { bool synced, calibrating, blocked, lifted, tilted, batt_low, batt_crit, selftest_critical, reversing, forward, parking_beeps; proto::Owner owner; proto::OwnerHintValue hint; uint8_t bump_seq; bool selftest_done; bool selftest_failed; }`; `cq::Annunciator::update(const AnnState&, uint32_t now_ms, LedSequencer&, SoundSequencer&)`.
  - Ports `cq::port::tone::{begin(), play(uint16_t hz), stop()}`, `cq::port::led::{begin(), write(const Rgb&)}`.
  - `app/tasks.h`: `cq::app::uiInit()`, `uiTask()` (50 Hz); `app/ui_api.h` (like `camera_api.h`): handlers `onLed/onSound/onTone/onAudioMode`. The alarm LED layer is re-picked on any change of tilted/lifted/batt_crit/batt_low/selftest_critical.
  - Smoke: `sound NAME [--mode normal|quiet|mute]`, `led MODE R G B`; `cmd_drive` prints the last `SLOW load/errs`.

- [ ] **Step 1: Write the failing UI tests**

`test/test_ui/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/ui/annunciator.h"
#include "domain/ui/catalogues.h"
#include "domain/ui/led_sequencer.h"
#include "domain/ui/sound_sequencer.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_sound_plays_notes_in_order() {
  SoundSequencer s;
  const Note n[] = {{440, 100}, {0, 50}, {880, 100}};
  TEST_ASSERT_TRUE(s.play(n, 3, SoundSequencer::kPrioEvent, 0));
  uint16_t hz = 1;
  TEST_ASSERT_TRUE(s.tick(0, hz));
  TEST_ASSERT_EQUAL(440, hz);
  TEST_ASSERT_FALSE(s.tick(60, hz));
  TEST_ASSERT_TRUE(s.tick(100, hz));
  TEST_ASSERT_EQUAL(0, hz);
  TEST_ASSERT_TRUE(s.tick(150, hz));
  TEST_ASSERT_EQUAL(880, hz);
  TEST_ASSERT_TRUE(s.tick(250, hz));
  TEST_ASSERT_EQUAL(0, hz);
  TEST_ASSERT_FALSE(s.playing());
}

void test_sound_priority_and_modes() {
  SoundSequencer s;
  const Note a[] = {{1000, 500}};
  const Note b[] = {{2000, 100}};
  s.play(a, 1, SoundSequencer::kPrioAlarm, 0);
  TEST_ASSERT_FALSE(s.play(b, 1, SoundSequencer::kPrioEvent, 10));  // lower priority cannot interrupt
  s.stop();
  s.setMode(proto::AudioModeValue::kQuiet);
  TEST_ASSERT_FALSE(s.play(b, 1, SoundSequencer::kPrioEvent, 20));
  TEST_ASSERT_TRUE(s.play(a, 1, SoundSequencer::kPrioAlarm, 20));
  s.setMode(proto::AudioModeValue::kMute);
  uint16_t hz = 1;
  s.tick(30, hz);
  TEST_ASSERT_EQUAL(0, hz);  // mute silences a playing sound immediately
  TEST_ASSERT_FALSE(s.play(a, 1, SoundSequencer::kPrioAlarm, 40));
}

void test_led_layers_blink_and_expiry() {
  LedSequencer l;
  l.set(LedLayer::kBase, {proto::LedMode::kSolid, {0, 40, 0}, 0, 0}, 0);
  l.set(LedLayer::kCustom, {proto::LedMode::kBlink, {0, 0, 200}, 400, 1000}, 0);
  TEST_ASSERT_TRUE((l.output(100) == Rgb{0, 0, 200}));
  TEST_ASSERT_TRUE((l.output(300) == Rgb{0, 0, 0}));     // blink off half
  TEST_ASSERT_TRUE((l.output(1100) == Rgb{0, 40, 0}));   // custom expired → base
  l.set(LedLayer::kAlarm, {proto::LedMode::kSolid, {255, 0, 0}, 0, 0}, 1200);
  l.set(LedLayer::kCustom, {proto::LedMode::kSolid, {0, 0, 200}, 0, 0}, 1200);
  TEST_ASSERT_TRUE((l.output(1300) == Rgb{255, 0, 0}));  // alarm overrides custom
  l.clear(LedLayer::kAlarm);
  TEST_ASSERT_TRUE((l.output(1300) == Rgb{0, 0, 200}));
}

void test_led_pulse_peaks_mid_period() {
  LedSequencer l;
  l.set(LedLayer::kBase, {proto::LedMode::kPulse, {200, 100, 0}, 1000, 0}, 0);
  TEST_ASSERT_TRUE((l.output(500) == Rgb{200, 100, 0}));
  TEST_ASSERT_TRUE(l.output(0).r < 10);
  TEST_ASSERT_UINT8_WITHIN(5, 100, l.output(250).r);
}

void test_catalogue() {
  Note n[SoundSequencer::kMaxNotes];
  TEST_ASSERT_EQUAL(4, soundNotes(proto::SoundId::kStartup, n));
  TEST_ASSERT_EQUAL(523, n[0].hz);
  TEST_ASSERT_EQUAL(0, soundNotes(static_cast<proto::SoundId>(200), n));
}

namespace {
AnnState base() {
  AnnState s{};
  s.synced = true;
  s.owner = proto::Owner::kNone;
  s.hint = proto::OwnerHintValue::kNone;
  return s;
}
}  // namespace

void test_annunciator_tilt_alarm_and_sound() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  a.update(s, 0, l, snd);
  s.tilted = true;
  a.update(s, 20, l, snd);
  TEST_ASSERT_TRUE(snd.playing());
  TEST_ASSERT_TRUE((l.output(20) == Rgb{255, 0, 0}) || (l.output(20) == Rgb{0, 0, 0}));  // red blink
  TEST_ASSERT_TRUE((l.output(20 + 60) == Rgb{255, 0, 0}));
}

void test_annunciator_owner_colours() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  s.owner = proto::Owner::kIr;
  a.update(s, 0, l, snd);
  TEST_ASSERT_TRUE((l.output(0) == Annunciator::kColorIr));
  s.owner = proto::Owner::kNetwork;
  s.hint = proto::OwnerHintValue::kAi;
  a.update(s, 20, l, snd);
  TEST_ASSERT_TRUE((l.output(20) == Annunciator::kColorAi));
}

void test_annunciator_parking_beeps_only_when_enabled() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  s.reversing = true;
  a.update(s, 0, l, snd);       // CHIRP_DOWN when reversing starts
  uint16_t hz = 0;
  for (uint32_t t = 0; t < 400; t += 20) snd.tick(t, hz);
  int beeps = 0;
  for (uint32_t t = 400; t < 3000; t += 20) {
    a.update(s, t, l, snd);
    if (snd.tick(t, hz) && hz != 0) ++beeps;
  }
  TEST_ASSERT_EQUAL(0, beeps);
  s.parking_beeps = true;
  for (uint32_t t = 3000; t < 6000; t += 20) {
    a.update(s, t, l, snd);
    if (snd.tick(t, hz) && hz != 0) ++beeps;
  }
  TEST_ASSERT_INT_WITHIN(1, 5, beeps);  // one TICK every 600 ms
}

void test_annunciator_alarm_follows_remaining_cause() {
  Annunciator a;
  LedSequencer l;
  SoundSequencer snd;
  AnnState s = base();
  s.lifted = true;
  s.batt_crit = true;
  a.update(s, 0, l, snd);
  TEST_ASSERT_TRUE((l.output(200) == Rgb{0, 0, 0}));  // 250 ms "lifted" blink: off half
  s.lifted = false;                                    // put down, battery still critical
  a.update(s, 1000, l, snd);
  TEST_ASSERT_TRUE((l.output(1200) == Annunciator::kColorAlarm));  // 1000 ms batt_crit blink: on half
  TEST_ASSERT_TRUE((l.output(1700) == Rgb{0, 0, 0}));              // off half
  s.selftest_critical = true;                                      // critical self-test failure → solid
  a.update(s, 2000, l, snd);
  TEST_ASSERT_TRUE((l.output(2700) == Annunciator::kColorAlarm));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sound_plays_notes_in_order);
  RUN_TEST(test_sound_priority_and_modes);
  RUN_TEST(test_led_layers_blink_and_expiry);
  RUN_TEST(test_led_pulse_peaks_mid_period);
  RUN_TEST(test_catalogue);
  RUN_TEST(test_annunciator_tilt_alarm_and_sound);
  RUN_TEST(test_annunciator_owner_colours);
  RUN_TEST(test_annunciator_parking_beeps_only_when_enabled);
  RUN_TEST(test_annunciator_alarm_follows_remaining_cause);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_ui`
Expected: FAIL — `domain/ui/annunciator.h: No such file or directory`.

- [ ] **Step 3: Implement the sequencers and catalogue**

`src/domain/ui/sound_sequencer.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

struct Note {
  uint16_t hz;  // 0 = rest
  uint16_t ms;
};

// Non-blocking note player for the passive buzzer. Higher priority interrupts; QUIET plays alarms only.
class SoundSequencer {
 public:
  static constexpr uint8_t kMaxNotes = 10;
  static constexpr uint8_t kPrioInfo = 1;
  static constexpr uint8_t kPrioEvent = 2;
  static constexpr uint8_t kPrioAlarm = 3;

  bool play(const Note* notes, uint8_t n, uint8_t priority, uint32_t now_ms);
  void setMode(proto::AudioModeValue m);
  proto::AudioModeValue mode() const { return m_mode; }
  void stop();
  bool tick(uint32_t now_ms, uint16_t& hz);
  bool playing() const { return m_count > 0; }

 private:
  Note m_notes[kMaxNotes] = {};
  uint8_t m_count = 0;
  uint8_t m_idx = 0;
  uint8_t m_prio = 0;
  uint32_t m_note_start_ms = 0;
  bool m_started = false;
  uint16_t m_out_hz = 0;
  proto::AudioModeValue m_mode = proto::AudioModeValue::kNormal;
};

}  // namespace cq
```

`src/domain/ui/sound_sequencer.cpp`:
```cpp
#include "domain/ui/sound_sequencer.h"

#include "domain/util/math_util.h"

namespace cq {

bool SoundSequencer::play(const Note* notes, uint8_t n, uint8_t priority, uint32_t now_ms) {
  if (n == 0 || m_mode == proto::AudioModeValue::kMute) return false;
  if (m_mode == proto::AudioModeValue::kQuiet && priority < kPrioAlarm) return false;
  if (playing() && priority < m_prio) return false;
  if (n > kMaxNotes) n = kMaxNotes;
  for (uint8_t i = 0; i < n; ++i) m_notes[i] = notes[i];
  m_count = n;
  m_idx = 0;
  m_prio = priority;
  m_note_start_ms = now_ms;
  m_started = false;
  return true;
}

void SoundSequencer::setMode(proto::AudioModeValue m) {
  m_mode = m;
  if (m == proto::AudioModeValue::kMute) stop();
}

void SoundSequencer::stop() {
  m_count = 0;
  m_idx = 0;
  m_prio = 0;
}

bool SoundSequencer::tick(uint32_t now_ms, uint16_t& hz) {
  uint16_t want = 0;
  if (m_count > 0) {
    if (!m_started) {
      m_started = true;
      m_note_start_ms = now_ms;
    }
    while (m_count > 0 && elapsed(now_ms, m_note_start_ms, m_notes[m_idx].ms)) {
      m_note_start_ms += m_notes[m_idx].ms;
      if (++m_idx >= m_count) stop();
    }
    if (m_count > 0) want = m_notes[m_idx].hz;
  }
  hz = want;
  if (want == m_out_hz) return false;
  m_out_hz = want;
  return true;
}

}  // namespace cq
```

`src/domain/ui/led_sequencer.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"
#include "types/hw_types.h"

namespace cq {

// Lower value wins. Custom = LED frame from a client; alarms always override it.
enum class LedLayer : uint8_t { kAlarm = 0, kAttention, kCustom, kStatus, kBase, kCount };

struct LedPattern {
  proto::LedMode mode;
  Rgb color;
  uint16_t period_ms;
  uint16_t duration_ms;  // 0 = until changed
};

class LedSequencer {
 public:
  void set(LedLayer layer, const LedPattern& p, uint32_t now_ms);
  void clear(LedLayer layer) { m_active[static_cast<uint8_t>(layer)] = false; }
  Rgb output(uint32_t now_ms);

 private:
  static constexpr uint8_t kLayers = static_cast<uint8_t>(LedLayer::kCount);
  LedPattern m_p[kLayers] = {};
  uint32_t m_start_ms[kLayers] = {};
  bool m_active[kLayers] = {};
};

}  // namespace cq
```

`src/domain/ui/led_sequencer.cpp`:
```cpp
#include "domain/ui/led_sequencer.h"

#include "domain/util/math_util.h"

namespace cq {

void LedSequencer::set(LedLayer layer, const LedPattern& p, uint32_t now_ms) {
  const uint8_t i = static_cast<uint8_t>(layer);
  m_p[i] = p;
  m_start_ms[i] = now_ms;
  m_active[i] = true;
}

Rgb LedSequencer::output(uint32_t now_ms) {
  for (uint8_t i = 0; i < kLayers; ++i) {
    if (!m_active[i]) continue;
    const LedPattern& p = m_p[i];
    if (p.duration_ms != 0 && elapsed(now_ms, m_start_ms[i], p.duration_ms)) {
      m_active[i] = false;
      continue;
    }
    const uint32_t period = p.period_ms == 0 ? 1000u : p.period_ms;
    const uint32_t phase = (now_ms - m_start_ms[i]) % period;
    switch (p.mode) {
      case proto::LedMode::kOff:
        return Rgb{0, 0, 0};
      case proto::LedMode::kSolid:
        return p.color;
      case proto::LedMode::kBlink:
        return phase < period / 2 ? p.color : Rgb{0, 0, 0};
      case proto::LedMode::kPulse: {
        const uint32_t half = period / 2;
        const uint32_t level = phase < half ? (phase * 255u) / half : ((period - phase) * 255u) / half;
        return Rgb{static_cast<uint8_t>(p.color.r * level / 255u), static_cast<uint8_t>(p.color.g * level / 255u),
                   static_cast<uint8_t>(p.color.b * level / 255u)};
      }
    }
  }
  return Rgb{0, 0, 0};
}

}  // namespace cq
```

`src/domain/ui/catalogues.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/ui/sound_sequencer.h"

namespace cq {

// Copies the notes of a catalogue sound (PROGMEM on the Uno) into `out`; returns the count (0 = unknown).
uint8_t soundNotes(proto::SoundId id, Note* out);

}  // namespace cq
```

`src/domain/ui/catalogues.cpp`:
```cpp
#include "domain/ui/catalogues.h"

#include <string.h>

#ifdef CQ_HOST_TEST
#define PROGMEM
#define CQ_READ_P(dst, src, n) memcpy((dst), (src), (n))
#else
#include <avr/pgmspace.h>
#define CQ_READ_P(dst, src, n) memcpy_P((dst), (src), (n))
#endif

namespace cq {
namespace {

struct SoundDef {
  uint8_t id;
  uint8_t n;
  Note notes[SoundSequencer::kMaxNotes];
};

constexpr uint8_t id(proto::SoundId s) { return static_cast<uint8_t>(s); }

const SoundDef kSounds[] PROGMEM = {
    {id(proto::SoundId::kStartup), 4, {{523, 100}, {659, 100}, {784, 100}, {1047, 200}}},
    {id(proto::SoundId::kReady), 2, {{784, 80}, {1047, 120}}},
    {id(proto::SoundId::kSelftestFail), 3, {{400, 200}, {0, 80}, {300, 400}}},
    {id(proto::SoundId::kChirpUp), 3, {{1000, 40}, {1500, 40}, {2000, 40}}},
    {id(proto::SoundId::kChirpDown), 3, {{2000, 40}, {1500, 40}, {1000, 40}}},
    {id(proto::SoundId::kBlip), 1, {{1800, 30}}},
    {id(proto::SoundId::kTick), 1, {{2500, 15}}},
    {id(proto::SoundId::kJingle), 5, {{784, 120}, {659, 120}, {523, 120}, {659, 120}, {784, 240}}},
    {id(proto::SoundId::kThinking), 5, {{600, 80}, {0, 80}, {700, 80}, {0, 80}, {800, 80}}},
    {id(proto::SoundId::kOuch), 2, {{1200, 60}, {600, 150}}},
    {id(proto::SoundId::kError), 3, {{300, 150}, {0, 60}, {300, 150}}},
};

}  // namespace

uint8_t soundNotes(proto::SoundId sid, Note* out) {
  for (uint8_t i = 0; i < sizeof kSounds / sizeof kSounds[0]; ++i) {
    SoundDef d;
    CQ_READ_P(&d, &kSounds[i], sizeof d);
    if (d.id != static_cast<uint8_t>(sid)) continue;
    for (uint8_t k = 0; k < d.n; ++k) out[k] = d.notes[k];
    return d.n;
  }
  return 0;
}

}  // namespace cq
```

`src/domain/ui/annunciator.h`:
```cpp
#pragma once
#include <stdint.h>

#include "domain/ui/led_sequencer.h"
#include "domain/ui/sound_sequencer.h"

namespace cq {

struct AnnState {
  bool synced;
  bool calibrating;
  bool blocked;
  bool lifted;
  bool tilted;
  bool batt_low;
  bool batt_crit;
  bool selftest_critical;
  bool reversing;
  bool forward;
  bool parking_beeps;
  proto::Owner owner;
  proto::OwnerHintValue hint;
  uint8_t bump_seq;
  bool selftest_done;
  bool selftest_failed;
};

// Turns robot state into LED layers and sounds (edges only; no event hooks needed).
class Annunciator {
 public:
  static constexpr Rgb kColorWaiting{60, 30, 0};   // amber pulse: no ESP32 yet
  // Owner colours per LLD §9.9: IR purple, network human green, network AI cyan.
  static constexpr Rgb kColorReady{20, 20, 20};    // dim white: idle, ESP32 synced
  static constexpr Rgb kColorHuman{0, 120, 0};     // green
  static constexpr Rgb kColorAi{0, 100, 100};      // cyan
  static constexpr Rgb kColorIr{80, 0, 120};       // purple
  static constexpr Rgb kColorCal{120, 100, 0};     // yellow blink
  static constexpr Rgb kColorAlarm{255, 0, 0};
  static constexpr Rgb kColorLow{160, 50, 0};      // orange pulse
  static constexpr uint16_t kParkingBeepMs = 600;

  void update(const AnnState& s, uint32_t now_ms, LedSequencer& led, SoundSequencer& snd);

 private:
  void sound(proto::SoundId id, uint8_t prio, uint32_t now_ms, SoundSequencer& snd);
  AnnState m_prev{};
  bool m_primed = false;
  uint32_t m_last_beep_ms = 0;
  uint32_t m_last_crit_ms = 0;
  bool m_prev_base_set = false;
};

}  // namespace cq
```

`src/domain/ui/annunciator.cpp`:
```cpp
#include "domain/ui/annunciator.h"

#include "domain/ui/catalogues.h"
#include "domain/util/math_util.h"

namespace cq {

void Annunciator::sound(proto::SoundId id, uint8_t prio, uint32_t now_ms, SoundSequencer& snd) {
  Note n[SoundSequencer::kMaxNotes];
  const uint8_t count = soundNotes(id, n);
  snd.play(n, count, prio, now_ms);
}

void Annunciator::update(const AnnState& s, uint32_t now_ms, LedSequencer& led, SoundSequencer& snd) {
  const AnnState p = m_primed ? m_prev : AnnState{};
  m_primed = true;

  // sounds on edges
  if (s.selftest_done && !p.selftest_done) {
    sound(s.selftest_failed ? proto::SoundId::kSelftestFail : proto::SoundId::kStartup, SoundSequencer::kPrioEvent,
          now_ms, snd);
  }
  if (s.synced && !p.synced) sound(proto::SoundId::kReady, SoundSequencer::kPrioInfo, now_ms, snd);
  if ((s.tilted && !p.tilted) || s.bump_seq != p.bump_seq) sound(proto::SoundId::kOuch, SoundSequencer::kPrioAlarm, now_ms, snd);
  if (s.lifted && !p.lifted) sound(proto::SoundId::kBlip, SoundSequencer::kPrioEvent, now_ms, snd);
  if (s.batt_low && !p.batt_low) sound(proto::SoundId::kError, SoundSequencer::kPrioEvent, now_ms, snd);
  if (s.batt_crit && (!p.batt_crit || elapsed(now_ms, m_last_crit_ms, 30000))) {
    m_last_crit_ms = now_ms;
    sound(proto::SoundId::kError, SoundSequencer::kPrioAlarm, now_ms, snd);
  }
  if (s.blocked && !p.blocked) sound(proto::SoundId::kTick, SoundSequencer::kPrioEvent, now_ms, snd);
  if (s.owner == proto::Owner::kIr && p.owner != proto::Owner::kIr) sound(proto::SoundId::kBlip, SoundSequencer::kPrioInfo, now_ms, snd);
  if (s.forward && !p.forward) sound(proto::SoundId::kChirpUp, SoundSequencer::kPrioInfo, now_ms, snd);
  if (s.reversing && !p.reversing) {
    sound(proto::SoundId::kChirpDown, SoundSequencer::kPrioInfo, now_ms, snd);
    m_last_beep_ms = now_ms;
  }
  if (s.reversing && s.parking_beeps && elapsed(now_ms, m_last_beep_ms, kParkingBeepMs)) {
    m_last_beep_ms = now_ms;
    sound(proto::SoundId::kTick, SoundSequencer::kPrioInfo, now_ms, snd);
  }

  // LED layers (only re-set on change so blink phases stay steady). Any change of an alarm input re-picks the
  // pattern: e.g. lifted clears while batt_crit stays → the 1000 ms batt_crit blink takes over.
  if (s.tilted != p.tilted || s.lifted != p.lifted || s.batt_crit != p.batt_crit || s.batt_low != p.batt_low ||
      s.selftest_critical != p.selftest_critical) {
    if (s.selftest_critical) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kSolid, kColorAlarm, 0, 0}, now_ms);
    } else if (s.tilted || s.lifted) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kColorAlarm, 250, 0}, now_ms);
    } else if (s.batt_crit) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kColorAlarm, 1000, 0}, now_ms);
    } else if (s.batt_low) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kPulse, kColorLow, 2000, 0}, now_ms);
    } else {
      led.clear(LedLayer::kAlarm);
    }
  }
  if (s.blocked != p.blocked) {
    if (s.blocked) {
      led.set(LedLayer::kAttention, {proto::LedMode::kBlink, kColorAlarm, 200, 0}, now_ms);
    } else {
      led.clear(LedLayer::kAttention);
    }
  }
  if (s.calibrating != p.calibrating) {
    if (s.calibrating) {
      led.set(LedLayer::kStatus, {proto::LedMode::kBlink, kColorCal, 500, 0}, now_ms);
    } else {
      led.clear(LedLayer::kStatus);
    }
  }
  if (s.owner != p.owner || s.hint != p.hint || s.synced != p.synced || !m_prev_base_set) {
    m_prev_base_set = true;
    LedPattern base{proto::LedMode::kSolid, kColorReady, 0, 0};
    if (s.owner == proto::Owner::kIr) {
      base.color = kColorIr;
    } else if (s.owner == proto::Owner::kNetwork) {
      base.color = s.hint == proto::OwnerHintValue::kAi ? kColorAi : kColorHuman;
    } else if (!s.synced) {
      base = LedPattern{proto::LedMode::kPulse, kColorWaiting, 2000, 0};
    }
    led.set(LedLayer::kBase, base, now_ms);
  }
  m_prev = s;
}

}  // namespace cq
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_ui`
Expected: `9 Tests 0 Failures`.

- [ ] **Step 5: Tone and WS2812 drivers**

`src/ports/tone.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {
namespace port {
namespace tone {
void begin();
void play(uint16_t hz);  // continuous until stop()/next play (Timer2)
void stop();
}  // namespace tone
}  // namespace port
}  // namespace cq
```

`src/ports/led.h`:
```cpp
#pragma once
#include "types/hw_types.h"

namespace cq {
namespace port {
namespace led {
void begin();
void write(const Rgb& c);  // one WS2812; interrupts off ≈ 32 µs
}  // namespace led
}  // namespace port
}  // namespace cq
```

`src/drivers/tone_uno.cpp`:
```cpp
#include <Arduino.h>

#include "drivers/board_config.h"
#include "ports/tone.h"

namespace cq {
namespace port {
namespace tone {
void begin() { pinMode(board::kPinBuzzer, OUTPUT); }
void play(uint16_t hz) { ::tone(board::kPinBuzzer, hz); }
void stop() { ::noTone(board::kPinBuzzer); }
}  // namespace tone
}  // namespace port
}  // namespace cq
```

`src/drivers/ws2812_uno.cpp` (original cycle-counted loop for 16 MHz; one bit ≈ 20–21 cycles ≈ 1.3 µs (spec 1.25 ± 0.6 µs), T0H ≈ 6 cycles = 375 ns, T1H ≈ 13 cycles = 810 ns, T1L ≈ 7 cycles = 440 ns (spec ≥ 300 ns — the second `rjmp` after `dec` keeps it off the minimum), T0L ≈ 15 cycles = 940 ns; 24 bits ≈ 32 µs with interrupts off, shorter than one 250000-baud UART byte (40 µs), so no RX byte is lost):
```cpp
#include <Arduino.h>

#include "drivers/board_config.h"
#include "ports/led.h"

static_assert(F_CPU == 16000000UL, "WS2812 timing is written for 16 MHz");

namespace cq {
namespace port {
namespace led {
namespace {

void sendByte(uint8_t byte) {
  uint8_t bits = 8;
  asm volatile(
      "1:                       \n\t"
      "  sbi  %[port], %[pin]   \n\t"  // 2  rise
      "  rjmp .+0               \n\t"  // 2
      "  sbrs %[byte], 7        \n\t"  // 1 (bit 0) / 2 (bit 1, skips the cbi)
      "  cbi  %[port], %[pin]   \n\t"  // 2  0-bit falls here (~6 cycles high)
      "  rjmp .+0               \n\t"  // 2
      "  rjmp .+0               \n\t"  // 2
      "  lsl  %[byte]           \n\t"  // 1
      "  cbi  %[port], %[pin]   \n\t"  // 2  1-bit falls here (~13 cycles high)
      "  dec  %[bits]           \n\t"  // 1
      "  rjmp .+0               \n\t"  // 2
      "  rjmp .+0               \n\t"  // 2  T1L ≈ 7 cycles (440 ns)
      "  brne 1b                \n\t"  // 2
      : [byte] "+r"(byte), [bits] "+r"(bits)
      : [port] "I"(_SFR_IO_ADDR(PORTD)), [pin] "I"(PORTD4));
}

}  // namespace

void begin() {
  pinMode(board::kPinLed, OUTPUT);
  digitalWrite(board::kPinLed, LOW);
}

void write(const Rgb& c) {
  const uint8_t sreg = SREG;
  cli();
  sendByte(c.g);  // WS2812 order: G, R, B, MSB first
  sendByte(c.r);
  sendByte(c.b);
  SREG = sreg;
}

}  // namespace led
}  // namespace port
}  // namespace cq
```

`board_config.h` already pins the LED to D4 = PD4; the driver asserts that assumption at compile time — add to `ws2812_uno.cpp` after the includes:
```cpp
static_assert(cq::board::kPinLed == 4, "ws2812_uno.cpp drives PORTD4 directly");
```

- [ ] **Step 6: UI task, routes and audio flags**

Add to `src/app/app.h`:
```cpp
#include "domain/ui/annunciator.h"
#include "domain/ui/led_sequencer.h"
#include "domain/ui/sound_sequencer.h"
```
members:
```cpp
  // ui
  SoundSequencer sound;
  LedSequencer led;
  Annunciator annunciator;
  Rgb led_out{1, 1, 1};  // forces the first write
  uint8_t bump_seq = 0;
```
In `src/app/imu_task.cpp`, where a bump is detected, add `++g_app.bump_seq;` next to the `kBump` event push (make the `if` a block).

`src/app/ui_api.h`:
```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace app {

// uiInit/uiTask are declared in app/tasks.h.
proto::AckStatus onLed(const uint8_t* p, uint8_t len);
proto::AckStatus onSound(const uint8_t* p, uint8_t len);
proto::AckStatus onTone(const uint8_t* p, uint8_t len);
proto::AckStatus onAudioMode(const uint8_t* p, uint8_t len);

}  // namespace app
}  // namespace cq
```

`src/app/ui_task.cpp`:
```cpp
#include "app/app.h"
#include "app/tasks.h"
#include "app/ui_api.h"
#include "domain/ui/catalogues.h"
#include "ports/clock.h"
#include "ports/led.h"
#include "ports/tone.h"

namespace cq {
namespace app {

void uiInit() {
  port::tone::begin();
  port::led::begin();
}

void uiTask() {
  const uint32_t now = port::clock::nowMs();
  const GlobalSettings& gs = g_app.settings.global();
  AnnState s{};
  s.synced = g_app.link.synced();
  s.calibrating = g_app.cal.running();
  s.blocked = g_app.safety.blocked;
  s.lifted = g_app.lift.lifted();
  s.tilted = g_app.lift.tilted();
  s.batt_low = g_app.batt.level() == BatteryMonitor::Level::kLow;
  s.batt_crit = g_app.batt.level() == BatteryMonitor::Level::kCritical;
  s.selftest_critical = g_app.self_test_done && g_app.self_test.critical();
  s.reversing = g_app.out_pwm_l < 0 && g_app.out_pwm_r < 0;
  s.forward = g_app.out_pwm_l > 0 && g_app.out_pwm_r > 0;
  s.parking_beeps = gs.parking_beeps != 0;
  s.owner = g_app.arbitrator.owner();
  s.hint = g_app.owner_hint;
  s.bump_seq = g_app.bump_seq;
  s.selftest_done = g_app.self_test_done;
  s.selftest_failed = g_app.self_test.failMask() != 0;
  g_app.annunciator.update(s, now, g_app.led, g_app.sound);

  uint16_t hz = 0;
  if (g_app.sound.tick(now, hz)) {
    if (hz == 0) {
      port::tone::stop();
    } else {
      port::tone::play(hz);
    }
  }
  const Rgb c = g_app.led.output(now);
  if (c != g_app.led_out) {
    port::led::write(c);
    g_app.led_out = c;
  }
}

proto::AckStatus onLed(const uint8_t* p, uint8_t len) {
  proto::LedMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (m.mode > static_cast<uint8_t>(proto::LedMode::kPulse)) return proto::AckStatus::kInvalidCommand;
  g_app.led.set(LedLayer::kCustom, {static_cast<proto::LedMode>(m.mode), {m.r, m.g, m.b}, m.period_ms, m.duration_ms},
                port::clock::nowMs());
  return proto::AckStatus::kOk;
}

proto::AckStatus onSound(const uint8_t* p, uint8_t len) {
  proto::SoundMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  Note n[SoundSequencer::kMaxNotes];
  const uint8_t count = soundNotes(static_cast<proto::SoundId>(m.id), n);
  if (count == 0) return proto::AckStatus::kInvalidCommand;
  return g_app.sound.play(n, count, SoundSequencer::kPrioEvent, port::clock::nowMs()) ? proto::AckStatus::kOk
                                                                                       : proto::AckStatus::kBusy;
}

proto::AckStatus onTone(const uint8_t* p, uint8_t len) {
  proto::ToneMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (m.n == 0) return proto::AckStatus::kInvalidCommand;  // nothing to play (play() would answer BUSY)
  Note n[SoundSequencer::kMaxNotes];
  for (uint8_t i = 0; i < m.n; ++i) n[i] = Note{m.notes[i].hz, m.notes[i].ms};
  return g_app.sound.play(n, m.n, SoundSequencer::kPrioEvent, port::clock::nowMs()) ? proto::AckStatus::kOk
                                                                                     : proto::AckStatus::kBusy;
}

proto::AckStatus onAudioMode(const uint8_t* p, uint8_t len) {
  proto::AudioModeMsg m{};
  if (!proto::decode(p, len, m)) return proto::AckStatus::kInvalidCommand;
  if (m.mode > static_cast<uint8_t>(proto::AudioModeValue::kMute)) return proto::AckStatus::kInvalidCommand;
  g_app.sound.setMode(static_cast<proto::AudioModeValue>(m.mode));
  if (m.mode == static_cast<uint8_t>(proto::AudioModeValue::kMute)) port::tone::stop();
  return proto::AckStatus::kOk;
}

}  // namespace app
}  // namespace cq
```
`ToneMsg::decode` rejects `n > 10` (Plan A), so the copy loop is bounded.

In `src/app/routes.cpp` add `#include "app/ui_api.h"` and rows before `// ROUTES-END`:
```cpp
    {static_cast<uint8_t>(proto::FrameType::kLed), onLed, true},
    {static_cast<uint8_t>(proto::FrameType::kSound), onSound, true},
    {static_cast<uint8_t>(proto::FrameType::kTone), onTone, true},
    {static_cast<uint8_t>(proto::FrameType::kAudioMode), onAudioMode, true},
```
In `src/app/comms.cpp` `telemetrySlowDue()` before `s.flags2 = g_app.slow_flags;` add:
```cpp
  const uint32_t muted = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kMuted);
  const uint32_t quiet = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kQuiet);
  const uint32_t brake_off = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kBrakeOff);
  const uint32_t envelope = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kEnvelope);
  const uint32_t link_lost = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kLinkLost);
  const uint32_t deadman = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kDeadman);
  g_app.slow_flags &= ~(muted | quiet | brake_off | envelope | link_lost | deadman);
  if (g_app.sound.mode() == proto::AudioModeValue::kMute) g_app.slow_flags |= muted;
  if (g_app.sound.mode() == proto::AudioModeValue::kQuiet) g_app.slow_flags |= quiet;
  if (!g_app.envelope.brakeOn()) g_app.slow_flags |= brake_off;
  if (g_app.envelope.active()) g_app.slow_flags |= envelope;
  if (!g_app.link.alive(port::clock::nowMs())) g_app.slow_flags |= link_lost;
  if (g_app.deadman_latched) g_app.slow_flags |= deadman;  // Task 11: DEADMAN_STOP until the next DRIVE
```
In `src/app/tasks.h` add before `// TASK-DECLS-END`:
```cpp
void uiInit();  // boot: buzzer and WS2812 pins
void uiTask();  // 50 Hz
```
In `src/main.cpp` (already includes `app/tasks.h`): call `cq::app::uiInit();` before `// SETUP-END`; task row `{cq::app::uiTask, 20000UL, 0},` before `// TASKS-END`.

- [ ] **Step 7: Smoke commands for LED/sound**

Add to `tools/smoke/uno_smoke.py`:
```python
def cmd_led(link: Link, args: argparse.Namespace) -> None:
    link.handlers.append(print_ack)
    wait_synced(link)
    link.send(g.Led(mode=g.LedMode[args.mode.upper()], r=args.r, g=args.g, b=args.b,
                    period_ms=args.period, duration_ms=args.duration))
    time.sleep(0.3)


def cmd_sound(link: Link, args: argparse.Namespace) -> None:
    link.handlers.append(print_ack)
    wait_synced(link)
    if args.mode is not None:  # same run: a new port open reboots the Uno (mode back to normal)
        link.send(g.AudioMode(mode=int(g.AudioModeValue[args.mode.upper()])))
        time.sleep(0.1)
    if args.name == "tone":
        link.send(g.Tone(n=3, notes=((440, 200), (0, 100), (880, 200))))
    elif args.name in ("normal", "quiet", "mute"):
        link.send(g.AudioMode(mode=int(g.AudioModeValue[args.name.upper()])))
    else:
        link.send(g.Sound(id=g.SoundId[args.name.upper()]))
    time.sleep(1.0)
```
where `g.AudioModeValue` is the generated enum (`NORMAL`, `QUIET`, `MUTE`) and `g.AudioMode` is the AUDIO_MODE frame class (Plan A names the enums `AudioModeValue`/`OwnerHintValue` to avoid clashing with the frame classes); `Tone.notes` is a tuple of `(hz, ms)` tuples (generated type `tuple[tuple[int, ...], ...]`).
At the end of `cmd_drive` (Task 6) add:
```python
    slow = link.last.get("TelemSlow")
    if isinstance(slow, g.TelemSlow):
        print(f"SLOW load={slow.loop_load}% max={slow.loop_max_us}us errs={slow.err_frames}")
```
Register `"led": cmd_led, "sound": cmd_sound` and in `main()`:
```python
    ld = sub.add_parser("led")
    ld.add_argument("mode", choices=["off", "solid", "blink", "pulse"])
    ld.add_argument("r", type=int)
    ld.add_argument("g", type=int)
    ld.add_argument("b", type=int)
    ld.add_argument("--period", type=int, default=500)
    ld.add_argument("--duration", type=int, default=3000)
    sd = sub.add_parser("sound")
    sd.add_argument("name", help="catalogue name (startup, jingle, …), tone, or normal/quiet/mute")
    sd.add_argument("--mode", choices=["normal", "quiet", "mute"], default=None,
                    help="AUDIO_MODE sent first (same run)")
```
Run (from `tools/smoke/`): `uv run ruff check . && uv run pyright`
Expected: `All checks passed!`, `0 errors, 0 warnings, 0 informations`.

- [ ] **Step 8: Build, flash, check**

Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py && pio run -e uno -t upload`
Each numbered check is one run per command (opening the port reboots the Uno; RAM state such as the audio mode does not carry over):
1. Boot: LED pulses amber (no ESP32); ≈ 2 s later the STARTUP jingle (or SELFTEST_FAIL with the ultrasonic unplugged). Start `… watch` → READY chirp, LED dim white.
2. `… led blink 0 0 200 --period 400 --duration 3000` → blue blink for 3 s, then dim white again. While it blinks, tilt the tank past 35° → red fast blink overrides, OUCH sound.
3. `… sound jingle` and `… sound tone` → `ACK OK`, both play; `… sound jingle --mode quiet` → `ACK OK` (mode) then `ACK BUSY` (quiet: catalogue sounds refused, alarms still play).
4. Stand, tracks off the ground: `… setting parking_beeps 1 --save`, then `… drive -30 0 --seconds 3` → CHIRP_DOWN then a tick every 0.6 s; afterwards `… setting parking_beeps 0 --save`.
5. Stand: hold a hand ≈ 10 cm in front of the ultrasonic sensor and run `… drive 30 0 --seconds 5` → `EVENT BLOCKED`, the LED blinks red (a WS2812 write every 100 ms with interrupts off ≈ 32 µs) while DRIVE frames stream; the final `SLOW load` < 60 % and `errs=0` (WS2812 writes do not drop UART bytes).
6. Hardware checklist (R14, needs a scope or logic analyser ≥ 24 MS/s on D4): one bit ≈ 1.3 µs; T0H ≈ 375 ns, T1H ≈ 810 ns, T1L ≥ 300 ns (≈ 440 ns expected). If no scope is available, record "not measured" in the sign-off and rely on check 2 (correct colours, no flicker).

- [ ] **Step 9: Commit**

```bash
git add firmware/uno tools/smoke
git commit -m "feat(uno): sound/LED sequencers, annunciator, catalogue, buzzer + WS2812 drivers, UI routes"
```

---

### Task 14: IR remote control (LLD §9.8 keymap) and the mode button (WiFi/admin reset requests)

**Files:**
- Create: `src/domain/input/ir_codes.h`, `ir_controller.{h,cpp}`, `boot_button.{h,cpp}`; `src/app/input_task.cpp`
- Modify: `src/app/control_task.cpp`, `src/app/motion_api.h`, `src/app/tasks.h`, `src/main.cpp`
- Test: `test/test_input/test_main.cpp`

**Interfaces:**
- Consumes: `g_app.ir_event/ir_pending` (Task 5), `Arbitrator::onIrInput/releaseIr` (Task 6), `g_app.teleop_v/w`, `stopTeleop()` + `Primitives` (Tasks 10–11), `CameraHead::home/nudge` (Task 12), `SoundSequencer`, `LedSequencer`, `soundNotes` (Task 13), `LinkSession::setRequest` (Task 2), `port::button` (Task 5).
- Produces:
  - `cq::ir_codes::kUp … kHash` (corrected NEC codes, MSB-first as received).
  - `cq::IrOutput { bool active; int8_t v; int8_t w; int8_t jog_pan; int8_t jog_tilt; bool stop; bool cam_home; bool speed_changed; bool toggle_mute; bool battery_check; bool turn_180; bool toggle_parking; bool cal_request; }` (`jog_pan` +1 = right, `jog_tilt` +1 = up).
  - `cq::IrController`: `bool onEvent(const IrEvent& e, IrOutput& out)` (true = known key → counts as IR input; one-shot flags only on the first frame, never on repeats), `bool tick(uint32_t now_ms, IrOutput& out)` (true when a held drive/jog key was released), `uint8_t speedPct() const`; `kReleaseMs = 200`, `kCalHoldMs = 3000`.
  - `cq::BootButton`: `enum class Action : uint8_t { kNone, kPress, kWifiThreshold, kAdminThreshold, kWifiReset, kAdminReset }`; `Action update(bool raw_pressed, uint32_t now_ms)`.
  - `cq::app::startLocalTurn(int16_t angle_dd, uint8_t speed_pct) -> proto::AckStatus` (IR `0` key, `motion_api.h`); `cq::app::inputTask()` (50 Hz, `app/tasks.h`).
  - IR takeover (owner changes to IR) is handled here, before the IR command is applied: running primitives are cancelled (`CANCELLED`) and network teleop incl. DRIVE_RATE is stopped. The control task only reports `CONTROL_CHANGED`/`IR_OVERRIDE` (Task 11).

Keymap (LLD §9.8, codes corrected): ▲/▼ drive ±ir_speed with heading hold, ◀/▶ spin (held; stop 200 ms after the last repeat) · OK stop + release to None · 1/3 camera pan left/right, 2/8 tilt up/down (held) · 5 camera home · 4/6 ir_speed −/+10 % (20–100, default 50, beep pitch ∝ speed) · 7 mute toggle · 9 battery check (LED colour + 3/2/1 beeps) · 0 gyro TURN by 180° · * parking-beeps toggle · # held 3 s → `CAL_REQUEST` event (no ESP32 → error beep).

- [ ] **Step 1: Write the failing input tests**

`test/test_input/test_main.cpp`:
```cpp
#include <unity.h>

#include "domain/input/boot_button.h"
#include "domain/input/ir_codes.h"
#include "domain/input/ir_controller.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_ir_hold_up_drives_then_release_stops() {
  IrController c;
  IrOutput o{};
  TEST_ASSERT_TRUE(c.onEvent({ir_codes::kUp, false, 0}, o));
  TEST_ASSERT_TRUE(o.active);
  TEST_ASSERT_EQUAL(50, o.v);
  TEST_ASSERT_EQUAL(0, o.w);
  TEST_ASSERT_FALSE(c.tick(150, o));
  c.onEvent({ir_codes::kUp, true, 108}, o);  // NEC repeat while held
  TEST_ASSERT_FALSE(c.tick(250, o));
  TEST_ASSERT_TRUE(c.tick(310, o));          // 200 ms without a repeat → released
  TEST_ASSERT_FALSE(o.active);
  TEST_ASSERT_EQUAL(0, o.v);
}

void test_ir_directions_speed_and_ok() {
  IrController c;
  IrOutput o{};
  c.onEvent({ir_codes::k6, false, 0}, o);
  TEST_ASSERT_TRUE(o.speed_changed);
  TEST_ASSERT_EQUAL(60, c.speedPct());
  c.onEvent({ir_codes::k4, false, 10}, o);
  c.onEvent({ir_codes::k4, false, 20}, o);
  TEST_ASSERT_EQUAL(40, c.speedPct());
  c.onEvent({ir_codes::kLeft, false, 30}, o);
  TEST_ASSERT_EQUAL(0, o.v);
  TEST_ASSERT_EQUAL(40, o.w);  // CCW
  c.onEvent({ir_codes::kRight, false, 40}, o);
  TEST_ASSERT_EQUAL(-40, o.w);
  c.onEvent({ir_codes::kDown, false, 50}, o);
  TEST_ASSERT_EQUAL(-40, o.v);
  c.onEvent({ir_codes::kOk, false, 60}, o);
  TEST_ASSERT_TRUE(o.stop);
  TEST_ASSERT_FALSE(o.active);
}

void test_ir_speed_limits() {
  IrController c;
  IrOutput o{};
  for (int i = 0; i < 10; ++i) c.onEvent({ir_codes::k6, false, static_cast<uint32_t>(i * 300)}, o);
  TEST_ASSERT_EQUAL(100, c.speedPct());
  for (int i = 0; i < 10; ++i) c.onEvent({ir_codes::k4, false, static_cast<uint32_t>(3000 + i * 300)}, o);
  TEST_ASSERT_EQUAL(20, c.speedPct());
}

void test_ir_camera_jog_while_held() {
  IrController c;
  IrOutput o{};
  c.onEvent({ir_codes::k1, false, 0}, o);
  TEST_ASSERT_EQUAL(-1, o.jog_pan);
  TEST_ASSERT_TRUE(c.tick(250, o));
  TEST_ASSERT_EQUAL(0, o.jog_pan);
  c.onEvent({ir_codes::k2, false, 300}, o);
  TEST_ASSERT_EQUAL(1, o.jog_tilt);
  c.onEvent({ir_codes::k8, false, 400}, o);
  TEST_ASSERT_EQUAL(-1, o.jog_tilt);
}

void test_ir_one_shot_keys_ignore_repeats() {
  IrController c;
  IrOutput o{};
  c.onEvent({ir_codes::k5, false, 0}, o);
  TEST_ASSERT_TRUE(o.cam_home);
  c.onEvent({ir_codes::k5, true, 108}, o);
  TEST_ASSERT_FALSE(o.cam_home);
  c.onEvent({ir_codes::k7, false, 500}, o);
  TEST_ASSERT_TRUE(o.toggle_mute);
  c.onEvent({ir_codes::k9, false, 1000}, o);
  TEST_ASSERT_TRUE(o.battery_check);
  c.onEvent({ir_codes::k0, false, 1500}, o);
  TEST_ASSERT_TRUE(o.turn_180);
  c.onEvent({ir_codes::kStar, false, 2000}, o);
  TEST_ASSERT_TRUE(o.toggle_parking);
  TEST_ASSERT_FALSE(c.onEvent({0x12345678UL, false, 2500}, o));
}

void test_ir_hash_hold_3s_requests_calibration_once() {
  IrController c;
  IrOutput o{};
  int requests = 0;
  c.onEvent({ir_codes::kHash, false, 0}, o);
  requests += o.cal_request ? 1 : 0;
  for (uint32_t t = 108; t < 4000; t += 108) {
    c.onEvent({ir_codes::kHash, true, t}, o);
    requests += o.cal_request ? 1 : 0;
  }
  TEST_ASSERT_EQUAL(1, requests);
}

void test_corrected_codes() {
  TEST_ASSERT_EQUAL_HEX32(0x00FF18E7UL, ir_codes::k5);
  TEST_ASSERT_EQUAL_HEX32(0x00FF38C7UL, ir_codes::k8);
  TEST_ASSERT_EQUAL_HEX32(0x00FF5AA5UL, ir_codes::k9);
  TEST_ASSERT_EQUAL_HEX32(0x00FF4AB5UL, ir_codes::k0);
  TEST_ASSERT_EQUAL_HEX32(0x00FF52ADUL, ir_codes::kHash);
}

namespace {
BootButton::Action holdFor(BootButton& b, uint32_t from_ms, uint32_t ms, uint8_t& thresholds) {
  BootButton::Action last = BootButton::Action::kNone;
  for (uint32_t t = from_ms; t < from_ms + ms; t += 20) {
    const BootButton::Action a = b.update(true, t);
    if (a == BootButton::Action::kWifiThreshold || a == BootButton::Action::kAdminThreshold) ++thresholds;
  }
  for (uint32_t t = from_ms + ms; t < from_ms + ms + 100; t += 20) {
    const BootButton::Action a = b.update(false, t);
    if (a != BootButton::Action::kNone) last = a;
  }
  return last;
}
}  // namespace

void test_short_press_after_boot() {
  BootButton b;
  for (uint32_t t = 0; t < 1000; t += 20) b.update(false, t);
  uint8_t th = 0;
  TEST_ASSERT_EQUAL(static_cast<int>(BootButton::Action::kPress), static_cast<int>(holdFor(b, 1000, 200, th)));
}

void test_boot_hold_5s_requests_wifi_reset() {
  BootButton b;
  uint8_t th = 0;
  TEST_ASSERT_EQUAL(static_cast<int>(BootButton::Action::kWifiReset), static_cast<int>(holdFor(b, 0, 6000, th)));
  TEST_ASSERT_EQUAL(1, th);
}

void test_boot_hold_15s_requests_admin_reset() {
  BootButton b;
  uint8_t th = 0;
  TEST_ASSERT_EQUAL(static_cast<int>(BootButton::Action::kAdminReset), static_cast<int>(holdFor(b, 0, 16000, th)));
  TEST_ASSERT_EQUAL(2, th);
}

void test_long_press_after_boot_is_ignored() {
  BootButton b;
  for (uint32_t t = 0; t < 1000; t += 20) b.update(false, t);
  uint8_t th = 0;
  TEST_ASSERT_EQUAL(static_cast<int>(BootButton::Action::kNone), static_cast<int>(holdFor(b, 1000, 6000, th)));
  TEST_ASSERT_EQUAL(0, th);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ir_hold_up_drives_then_release_stops);
  RUN_TEST(test_ir_directions_speed_and_ok);
  RUN_TEST(test_ir_speed_limits);
  RUN_TEST(test_ir_camera_jog_while_held);
  RUN_TEST(test_ir_one_shot_keys_ignore_repeats);
  RUN_TEST(test_ir_hash_hold_3s_requests_calibration_once);
  RUN_TEST(test_corrected_codes);
  RUN_TEST(test_short_press_after_boot);
  RUN_TEST(test_boot_hold_5s_requests_wifi_reset);
  RUN_TEST(test_boot_hold_15s_requests_admin_reset);
  RUN_TEST(test_long_press_after_boot_is_ignored);
  return UNITY_END();
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `pio test -e native -f test_input`
Expected: FAIL — `domain/input/boot_button.h: No such file or directory`.

- [ ] **Step 3: Implement IR codes, IR controller and the button**

`src/domain/input/ir_codes.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {
namespace ir_codes {
// ELEGOO remote, NEC, MSB-first as received. Values from the original ELEGOO firmware constants;
// LLD §9.8 listed 5, 8 and 9 wrongly (fixed in Task 15).
constexpr uint32_t kUp = 0x00FF629DUL;
constexpr uint32_t kDown = 0x00FFA857UL;
constexpr uint32_t kLeft = 0x00FF22DDUL;
constexpr uint32_t kRight = 0x00FFC23DUL;
constexpr uint32_t kOk = 0x00FF02FDUL;
constexpr uint32_t k1 = 0x00FF6897UL;
constexpr uint32_t k2 = 0x00FF9867UL;
constexpr uint32_t k3 = 0x00FFB04FUL;
constexpr uint32_t k4 = 0x00FF30CFUL;
constexpr uint32_t k5 = 0x00FF18E7UL;
constexpr uint32_t k6 = 0x00FF7A85UL;
constexpr uint32_t k7 = 0x00FF10EFUL;
constexpr uint32_t k8 = 0x00FF38C7UL;
constexpr uint32_t k9 = 0x00FF5AA5UL;
constexpr uint32_t k0 = 0x00FF4AB5UL;
constexpr uint32_t kStar = 0x00FF42BDUL;
constexpr uint32_t kHash = 0x00FF52ADUL;
}  // namespace ir_codes
}  // namespace cq
```

`src/domain/input/ir_controller.h`:
```cpp
#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {

struct IrOutput {
  bool active;     // a drive key is held
  int8_t v;
  int8_t w;
  int8_t jog_pan;  // +1 right, -1 left (while 3 / 1 held)
  int8_t jog_tilt; // +1 up, -1 down (while 2 / 8 held)
  bool stop;
  bool cam_home;
  bool speed_changed;
  bool toggle_mute;
  bool battery_check;
  bool turn_180;
  bool toggle_parking;
  bool cal_request;
};

class IrController {
 public:
  static constexpr uint16_t kReleaseMs = 200;
  static constexpr uint16_t kCalHoldMs = 3000;
  bool onEvent(const IrEvent& e, IrOutput& out);
  bool tick(uint32_t now_ms, IrOutput& out);
  uint8_t speedPct() const { return m_speed_pct; }

 private:
  enum class Hold : uint8_t { kNone, kDrive, kJog, kHash };
  uint8_t m_speed_pct = 50;
  Hold m_hold = Hold::kNone;
  uint32_t m_last_ms = 0;
  uint32_t m_hash_start_ms = 0;
  bool m_hash_fired = false;
};

}  // namespace cq
```

`src/domain/input/ir_controller.cpp`:
```cpp
#include "domain/input/ir_controller.h"

#include "domain/input/ir_codes.h"
#include "domain/util/math_util.h"

namespace cq {

bool IrController::onEvent(const IrEvent& e, IrOutput& out) {
  out.stop = out.cam_home = out.speed_changed = out.toggle_mute = false;
  out.battery_check = out.turn_180 = out.toggle_parking = out.cal_request = false;
  const int8_t s = static_cast<int8_t>(m_speed_pct);

  // held keys: repeats keep them alive
  int8_t v = 0, w = 0, jp = 0, jt = 0;
  Hold hold = Hold::kNone;
  switch (e.code) {
    case ir_codes::kUp: v = s; hold = Hold::kDrive; break;
    case ir_codes::kDown: v = static_cast<int8_t>(-s); hold = Hold::kDrive; break;
    case ir_codes::kLeft: w = s; hold = Hold::kDrive; break;
    case ir_codes::kRight: w = static_cast<int8_t>(-s); hold = Hold::kDrive; break;
    case ir_codes::k1: jp = -1; hold = Hold::kJog; break;
    case ir_codes::k3: jp = 1; hold = Hold::kJog; break;
    case ir_codes::k2: jt = 1; hold = Hold::kJog; break;
    case ir_codes::k8: jt = -1; hold = Hold::kJog; break;
    case ir_codes::kHash: hold = Hold::kHash; break;
    default: break;
  }
  if (hold != Hold::kNone) {
    m_last_ms = e.t_ms;
    if (hold == Hold::kHash) {
      if (!e.repeat || m_hold != Hold::kHash) {
        m_hash_start_ms = e.t_ms;
        m_hash_fired = false;
      } else if (!m_hash_fired && elapsed(e.t_ms, m_hash_start_ms, kCalHoldMs)) {
        m_hash_fired = true;
        out.cal_request = true;
      }
      m_hold = hold;
      return true;
    }
    m_hold = hold;
    out.active = hold == Hold::kDrive;
    out.v = v;
    out.w = w;
    out.jog_pan = jp;
    out.jog_tilt = jt;
    return true;
  }
  if (e.repeat) return false;  // repeats only matter for held keys

  switch (e.code) {
    case ir_codes::kOk:
      m_hold = Hold::kNone;
      out.active = false;
      out.v = out.w = out.jog_pan = out.jog_tilt = 0;
      out.stop = true;
      return true;
    case ir_codes::k4:
    case ir_codes::k6: {
      const int16_t next = static_cast<int16_t>(m_speed_pct + (e.code == ir_codes::k6 ? 10 : -10));
      m_speed_pct = static_cast<uint8_t>(next < 20 ? 20 : (next > 100 ? 100 : next));
      out.speed_changed = true;
      return true;
    }
    case ir_codes::k5: out.cam_home = true; return true;
    case ir_codes::k7: out.toggle_mute = true; return true;
    case ir_codes::k9: out.battery_check = true; return true;
    case ir_codes::k0: out.turn_180 = true; return true;
    case ir_codes::kStar: out.toggle_parking = true; return true;
    default: return false;
  }
}

bool IrController::tick(uint32_t now_ms, IrOutput& out) {
  if (m_hold == Hold::kNone || !elapsed(now_ms, m_last_ms, kReleaseMs)) return false;
  const bool released_motion = m_hold == Hold::kDrive || m_hold == Hold::kJog;
  m_hold = Hold::kNone;
  out.active = false;
  out.v = out.w = out.jog_pan = out.jog_tilt = 0;
  return released_motion;
}

}  // namespace cq
```

`src/domain/input/boot_button.h`:
```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Mode button (D2). Held from boot: 5 s → WiFi reset request, 15 s → admin reset request (on release,
// a tick at each threshold). After boot: a short press (30 ms..2 s) is BUTTON_PRESS; longer is ignored.
class BootButton {
 public:
  enum class Action : uint8_t { kNone, kPress, kWifiThreshold, kAdminThreshold, kWifiReset, kAdminReset };
  static constexpr uint16_t kBootWindowMs = 500;
  static constexpr uint16_t kWifiHoldMs = 5000;
  static constexpr uint16_t kAdminHoldMs = 15000;
  static constexpr uint8_t kDebounceMs = 20;

  Action update(bool raw_pressed, uint32_t now_ms);

 private:
  bool m_stable = false;
  bool m_raw = false;
  uint32_t m_raw_since_ms = 0;
  bool m_first = true;
  bool m_boot_hold = false;
  uint32_t m_press_ms = 0;
  uint8_t m_thresholds = 0;
};

}  // namespace cq
```

`src/domain/input/boot_button.cpp`:
```cpp
#include "domain/input/boot_button.h"

#include "domain/util/math_util.h"

namespace cq {

BootButton::Action BootButton::update(bool raw_pressed, uint32_t now_ms) {
  if (m_first) {
    m_first = false;
    m_raw = raw_pressed;
    m_stable = raw_pressed;
    m_raw_since_ms = now_ms;
    if (raw_pressed && now_ms < kBootWindowMs) {
      m_boot_hold = true;
      m_press_ms = now_ms;
    }
    return Action::kNone;
  }
  if (raw_pressed != m_raw) {
    m_raw = raw_pressed;
    m_raw_since_ms = now_ms;
  }
  if (m_raw != m_stable && elapsed(now_ms, m_raw_since_ms, kDebounceMs)) {
    m_stable = m_raw;
    if (m_stable) {
      m_press_ms = now_ms;
      return Action::kNone;
    }
    const uint32_t held = now_ms - m_press_ms;
    if (m_boot_hold) {  // one-shot per boot
      m_boot_hold = false;
      if (held >= kAdminHoldMs) return Action::kAdminReset;
      if (held >= kWifiHoldMs) return Action::kWifiReset;
      return Action::kNone;
    }
    return (held >= 30 && held < 2000) ? Action::kPress : Action::kNone;
  }
  if (m_boot_hold && m_stable) {
    const uint32_t held = now_ms - m_press_ms;
    if (m_thresholds == 0 && held >= kWifiHoldMs) {
      m_thresholds = 1;
      return Action::kWifiThreshold;
    }
    if (m_thresholds == 1 && held >= kAdminHoldMs) {
      m_thresholds = 2;
      return Action::kAdminThreshold;
    }
  }
  return Action::kNone;
}

}  // namespace cq
```

- [ ] **Step 4: Run the tests to see them pass**

Run: `pio test -e native -f test_input`
Expected: `11 Tests 0 Failures`.

- [ ] **Step 5: Local TURN for the IR `0` key**

Add to `src/app/motion_api.h`: `proto::AckStatus startLocalTurn(int16_t angle_dd, uint8_t speed_pct);`
Add to `src/app/control_task.cpp` (after `onTurn`):
```cpp
proto::AckStatus startLocalTurn(int16_t angle_dd, uint8_t speed_pct) {
  proto::TurnMsg m{};
  m.src = static_cast<uint8_t>(proto::Source::kNone);
  m.mode = static_cast<uint8_t>(proto::TurnMode::kBy);
  m.angle = angle_dd;
  m.speed = speed_pct;
  m.radius_cm = 0;
  zeroTeleop();
  g_prim.imu_ok = imuUsable();
  return g_app.primitives.startTurn(m, g_prim, primInputs(), port::clock::nowMs());
}
```
The input task cancels primitives only when the owner *changes* to IR (before it applies the key), so a TURN started by the IR `0` key keeps running — also when `0` is the first IR key; network motion is refused `NOT_OWNER` meanwhile.

- [ ] **Step 6: Input task (IR → arbitration/teleop/camera/sounds, button → requests)**

`src/app/input_task.cpp`:
```cpp
#include "app/app.h"
#include "app/motion_api.h"
#include "domain/input/boot_button.h"
#include "domain/input/ir_controller.h"
#include "domain/ui/catalogues.h"
#include "ports/button.h"
#include "ports/clock.h"

namespace cq {
namespace app {
namespace {

constexpr float kJogStepDeg = 1.2f;  // per 20 ms tick = 60 °/s

IrController g_ir;
BootButton g_button;
int8_t g_jog_pan = 0;
int8_t g_jog_tilt = 0;

void play(proto::SoundId id, uint8_t prio = SoundSequencer::kPrioEvent) {
  Note n[SoundSequencer::kMaxNotes];
  const uint8_t count = soundNotes(id, n);
  g_app.sound.play(n, count, prio, port::clock::nowMs());
}

void beeps(uint8_t count, uint16_t hz) {
  Note n[SoundSequencer::kMaxNotes];
  uint8_t k = 0;
  for (uint8_t i = 0; i < count && k + 1 < SoundSequencer::kMaxNotes; ++i) {
    n[k++] = Note{hz, 80};
    n[k++] = Note{0, 120};
  }
  g_app.sound.play(n, k, SoundSequencer::kPrioEvent, port::clock::nowMs());
}

void batteryCheck(uint32_t now) {
  Rgb c{40, 40, 40};
  uint8_t count = 0;
  switch (g_app.batt.level()) {
    case BatteryMonitor::Level::kOk: c = Rgb{0, 150, 0}; count = 3; break;
    case BatteryMonitor::Level::kLow: c = Rgb{160, 80, 0}; count = 2; break;
    case BatteryMonitor::Level::kCritical: c = Rgb{255, 0, 0}; count = 1; break;
    case BatteryMonitor::Level::kUsb: break;  // no pack: white, no beeps
  }
  g_app.led.set(LedLayer::kAttention, {proto::LedMode::kSolid, c, 0, 2000}, now);
  if (count > 0) beeps(count, 1500);
}

void applyIr(const IrOutput& o, uint32_t now) {
  const proto::PrimitiveKind prim = g_app.primitives.active();
  if (o.active) {
    if (prim != proto::PrimitiveKind::kNone) g_app.primitives.cancel(proto::AckStatus::kCancelled);
    g_app.teleop_rate_mode = false;
    g_app.teleop_v = o.v;
    g_app.teleop_w = o.w;
  } else {
    stopTeleop();  // also ends DRIVE_RATE mode: no spin may survive under IR ownership
  }
  g_jog_pan = o.jog_pan;
  g_jog_tilt = o.jog_tilt;
  if (o.stop) {
    stopTeleop();
    g_app.primitives.cancel(proto::AckStatus::kCancelled);
    g_app.arbitrator.releaseIr();
  }
  if (o.cam_home) g_app.camera.home();
  if (o.speed_changed) {
    const Note n{static_cast<uint16_t>(400 + g_ir.speedPct() * 10), 60};
    g_app.sound.play(&n, 1, SoundSequencer::kPrioEvent, now);
  }
  if (o.toggle_mute) {
    const bool muted = g_app.sound.mode() == proto::AudioModeValue::kMute;
    g_app.sound.setMode(muted ? proto::AudioModeValue::kNormal : proto::AudioModeValue::kMute);
    if (muted) play(proto::SoundId::kBlip);
  }
  if (o.battery_check) batteryCheck(now);
  if (o.turn_180 && startLocalTurn(1800, g_ir.speedPct()) != proto::AckStatus::kOk) play(proto::SoundId::kError);
  if (o.toggle_parking) {
    int32_t v = 0;
    g_app.settings.get(static_cast<uint8_t>(proto::SettingId::kParkingBeeps), v);
    g_app.settings.set(static_cast<uint8_t>(proto::SettingId::kParkingBeeps), v ? 0 : 1);  // RAM until SETTINGS_SAVE
    play(v ? proto::SoundId::kChirpDown : proto::SoundId::kChirpUp);
  }
  if (o.cal_request) {
    if (g_app.link.alive(now)) {
      g_app.events.push(proto::EventKind::kCalRequest);
      play(proto::SoundId::kBlip);
    } else {
      play(proto::SoundId::kError);
    }
  }
}

}  // namespace

// 50 Hz.
void inputTask() {
  const uint32_t now = port::clock::nowMs();
  IrOutput out{};
  if (g_app.ir_pending) {
    g_app.ir_pending = false;
    if (g_ir.onEvent(g_app.ir_event, out)) {
      // IR always wins and works with no ESP32 at all. On takeover end network primitives and teleop
      // (incl. DRIVE_RATE) first, so the key applied next (e.g. `0` → TURN 180°) is not cancelled.
      if (g_app.arbitrator.onIrInput(now)) {
        g_app.primitives.cancel(proto::AckStatus::kCancelled);
        stopTeleop();
      }
      applyIr(out, now);
    }
  }
  if (g_ir.tick(now, out)) applyIr(out, now);
  if (g_jog_pan != 0 || g_jog_tilt != 0) {
    g_app.camera.nudge(-g_jog_pan * kJogStepDeg, g_jog_tilt * kJogStepDeg);  // pan right = lower servo angle
  }

  switch (g_button.update(port::button::pressed(), now)) {
    case BootButton::Action::kPress:
      g_app.events.push(proto::EventKind::kButtonPress);
      play(proto::SoundId::kBlip);
      break;
    case BootButton::Action::kWifiThreshold:
    case BootButton::Action::kAdminThreshold:
      play(proto::SoundId::kTick, SoundSequencer::kPrioAlarm);
      break;
    case BootButton::Action::kWifiReset:
      g_app.link.setRequest(static_cast<uint8_t>(proto::FrameType::kWifiResetReq));
      play(proto::SoundId::kJingle, SoundSequencer::kPrioAlarm);
      g_app.led.set(LedLayer::kAttention, {proto::LedMode::kBlink, Rgb{0, 0, 255}, 200, 3000}, now);
      break;
    case BootButton::Action::kAdminReset:
      g_app.link.setRequest(static_cast<uint8_t>(proto::FrameType::kAdminResetReq));
      play(proto::SoundId::kJingle, SoundSequencer::kPrioAlarm);
      g_app.led.set(LedLayer::kAttention, {proto::LedMode::kBlink, Rgb{255, 0, 255}, 200, 3000}, now);
      break;
    case BootButton::Action::kNone:
      break;
  }
}

}  // namespace app
}  // namespace cq
```
In `src/app/tasks.h` add before `// TASK-DECLS-END`:
```cpp
void inputTask();  // 50 Hz: IR remote + mode button
```
and add `#include "app/tasks.h"` to `input_task.cpp`. In `src/main.cpp` add the row `{cq::app::inputTask, 20000UL, 0},` before `// TASKS-END`.

- [ ] **Step 7: Build, flash, check with the remote (ESP32 absent)**

Run: `pio test -e native && pio run -e uno && python3 scripts/size_gate.py && pio run -e uno -t upload`
Unplug USB (battery on, tank on the floor, no link at all):
1. Hold ▲ → drives forward holding heading; release → stops within 0.2 s. ◀/▶ spin, ▼ reverses. LED purple while IR owns; 3 s after the last key it returns to the amber "waiting" pulse.
2. `6`/`4` change speed (beep pitch rises/falls); `OK` stops at once and releases control (LED back to amber).
3. Hold `1`/`3` → camera pans left/right; `2`/`8` tilt; `5` homes it.
4. `9` → LED green + 3 beeps on a full pack; `7` mutes/unmutes; `*` toggles parking beeps (chirp up = on); `0` spins 180° (± 2°).
5. Hold `#` 3 s → error beep (no ESP32). With USB: start `… watch --seconds 20`, then hold `#` 3 s → `EVENT CAL_REQUEST`.
Checks 6–8 use USB with the tank **on the stand** (tracks off the ground); each is one smoke run (opening the port reboots the Uno):
6. `… drive 30 0 --seconds 10`, press and hold ▲ at ≈ 2 s for ≈ 2 s, then release → `EVENT IR_OVERRIDE`, `EVENT REJECTED a=16 b=1` (DRIVE refused `NOT_OWNER`) while IR holds control; ≈ 3 s after the release `EVENT CONTROL_CHANGED a=0` then `a=1`, and the printed `pwm` is non-zero again from the script's DRIVE stream — all in the same run.
7. `… driverate 0 45 --seconds 6`, press `5` (camera home, a non-drive key) at ≈ 2 s → `EVENT IR_OVERRIDE`, the spin stops at once (`pwm=0,0`, `EVENT REJECTED a=17 b=1` for the DRIVE_RATE stream) for as long as IR owns: no spin survives under IR ownership. ≈ 3 s after the key (`ir_handback`) `EVENT CONTROL_CHANGED a=0` then `a=1`, and the still-running stream spins the tank again — that restart is the normal hand-back, not a fault.
8. Start `… watch --seconds 40` first; hold the mode button and press the Uno RESET button (the watch keeps the port open, so the board does not reboot again), release the mode button after 6 s → `WifiResetReq` printed every 500 ms (the smoke tool does not ESP_ACK it; Plan C does). Repeat holding 16 s → `AdminResetReq`.

- [ ] **Step 8: Commit**

```bash
git add firmware/uno
git commit -m "feat(uno): IR remote keymap (drive, camera jog, speed, battery check, turn 180) and mode button resets"
```

---

### Task 15: Integration — soak test, budgets, LLD correction, phase 1–2 sign-off

**Files:**
- Modify: `tools/smoke/uno_smoke.py` (soak command), `docs/lld/uno-firmware-lld.md` (§9.8 IR codes)

**Interfaces:**
- Consumes: the complete firmware (Tasks 1–14) and every smoke command.
- Produces: smoke command `soak --minutes N`; the corrected LLD §9.8 table; the phase 1–2 sign-off checklist below.

- [ ] **Step 1: Run the whole host suite**

Run: `pio test -e native`
Expected: 14 suites pass — `test_scheduler` 4, `test_comms` 16, `test_settings` 8, `test_estimation` 9, `test_sensors` 11, `test_safety` 13, `test_encoders` 4, `test_calibration` 11, `test_motion_blocks` 11, `test_controllers` 8, `test_primitives` 9, `test_camera` 7, `test_ui` 9, `test_input` 11 — **131 test cases, 0 failures**.

- [ ] **Step 2: Build gates and memory map**

Run: `pio run -e uno && python3 scripts/size_gate.py`
Expected: `size_gate: flash ≤ 30720 B, static RAM ≤ 1750 B` and exit 0.
Run: `~/.platformio/packages/toolchain-atmelavr/bin/avr-nm --size-sort -S -C .pio/build/uno/firmware.elf | grep -i " [bBdD] " | tail -15`
Expected: the largest RAM symbols are `cq::g_app` (all domain state), the Serial RX/TX rings (64 + 64 B), `g_tx` (53 B) and the Servo table; nothing unexpected (no `String`, no `malloc`). Record the flash/RAM numbers in the commit message.

- [ ] **Step 3: Soak command**

Add to `tools/smoke/uno_smoke.py`:
```python
def cmd_soak(link: Link, args: argparse.Namespace) -> None:
    """Mixed traffic for N minutes on the stand; fails on resets, frame/I2C errors, RAM or load."""
    seen: dict[str, int] = {"min_ram": 65535, "max_loop_us": 0, "max_load": 0, "hello": 0}
    first: dict[str, int] = {}
    last: dict[str, int] = {}

    def on(msg: object) -> None:
        if isinstance(msg, g.TelemSlow):
            if not first:
                first.update(err_frames=msg.err_frames, err_i2c=msg.err_i2c, uptime=msg.uptime_s)
            last.update(err_frames=msg.err_frames, err_i2c=msg.err_i2c, uptime=msg.uptime_s)
            seen["min_ram"] = min(seen["min_ram"], msg.free_ram)
            seen["max_loop_us"] = max(seen["max_loop_us"], msg.loop_max_us)
            seen["max_load"] = max(seen["max_load"], msg.loop_load)
        elif isinstance(msg, g.Hello) and first:
            seen["hello"] += 1  # a HELLO after the start = the Uno rebooted

    link.handlers.append(on)
    wait_synced(link)
    end = time.monotonic() + args.minutes * 60
    i = 0
    while time.monotonic() < end:
        phase = i % 200
        if phase < 60:
            link.send(g.Drive(src=g.Source.HUMAN, v=30, w=(phase % 20) - 10))
        elif phase == 60:
            link.send(g.Stop(src=g.Source.HUMAN))
        elif phase == 80:
            link.send(g.Turn(src=g.Source.HUMAN, mode=0, angle=900, speed=40, radius_cm=0))
        elif phase == 120:
            link.send(g.CamSet(pan=(i % 1800), tilt=900, speed=0))
        elif phase == 140:
            link.send(g.Led(mode=g.LedMode.BLINK, r=0, g=0, b=80, period_ms=300, duration_ms=2000))
        elif phase == 160:
            link.send(g.SettingGet(id=g.SettingId.DEADMAN))
        elif phase == 180:
            link.send(g.Sound(id=g.SoundId.BLIP))
        i += 1
        time.sleep(0.05)
    link.send(g.Stop(src=g.Source.HUMAN))
    time.sleep(1.5)
    errs = last.get("err_frames", 0) - first.get("err_frames", 0)
    i2c = last.get("err_i2c", 0) - first.get("err_i2c", 0)
    # No TELEM_SLOW at all (wrong port, dead link) or uptime going backwards (a reboot) also fails.
    up = last.get("uptime", 0) - first.get("uptime", 0)
    ok = (bool(first) and up >= int(args.minutes * 60) - 5 and seen["hello"] == 0 and errs == 0
          and i2c == 0 and seen["min_ram"] >= 250 and seen["max_load"] <= 70
          and seen["max_loop_us"] <= 4000)
    print(f"uptime_delta={up}s resets={seen['hello']} frame_errs={errs} i2c_errs={i2c} "
          f"min_free_ram={seen['min_ram']} max_load={seen['max_load']}% "
          f"max_loop={seen['max_loop_us']}us")
    print("SOAK:", "PASS" if ok else "FAIL")
```
Register `"soak": cmd_soak` and in `main()`:
```python
    sk = sub.add_parser("soak")
    sk.add_argument("--minutes", type=float, default=10.0)
```
Run (from `tools/smoke/`): `uv run ruff check . && uv run pyright`
Expected: `All checks passed!`, `0 errors, 0 warnings, 0 informations` (strict mode, ruling R15: the smoke tool falls under the CLAUDE.md Python rule).

- [ ] **Step 4: Soak on the stand**

Tank on the stand, battery on, USB connected (S1 on upload/USB).
Run (from `tools/smoke/`): `uv run python uno_smoke.py --port PORT soak --minutes 10`
Expected: `uptime_delta≥595s resets=0 frame_errs=0 i2c_errs=0 min_free_ram≥250 max_load≤70% max_loop≤4000us` and `SOAK: PASS`. (TURN on the stand ends `STALLED` after 1 s because the gyro sees no rotation — expected.)

- [ ] **Step 5: Correct the IR codes in the LLD**

In `docs/lld/uno-firmware-lld.md` §9.8 replace these table rows:
```
| 2 `0x00FF9867` / 8 `0x00FF4AB5` | `jog(tilt ±rate)` | while held |
| 5 `0x00FF38C7` | camera home | — |
```
```
| 9 `0x00FF52AD` | battery check: LED colour + 3/2/1 beeps | — |
| 0 | gyro TURN by 180° | — |
| * | parking beeps toggle | — |
| # | (hold 3 s) → `cal_request` to ESP32 | hold timer |
```
with
```
| 2 `0x00FF9867` / 8 `0x00FF38C7` | `jog(tilt ±rate)` | while held |
| 5 `0x00FF18E7` | camera home | — |
```
```
| 9 `0x00FF5AA5` | battery check: LED colour + 3/2/1 beeps | — |
| 0 `0x00FF4AB5` | gyro TURN by 180° | — |
| * `0x00FF42BD` | parking beeps toggle | — |
| # `0x00FF52AD` | (hold 3 s) → `cal_request` to ESP32 | hold timer |
```
and the whole line below the table
```
Codes are the 32-bit values as shifted in by the decoder in received order (matches the original firmware constants); 0/*/# codes captured during hardware bring-up. `IrKeymap` in PROGMEM; `ir_speed_pct` held by the Arbitrator. New `CameraHead::jog(pan_rate, tilt_rate)`.
```
with
```
Codes are the 32-bit values as shifted in by the decoder in received order. Codes verified against the original ELEGOO firmware constants (5, 8, 9 corrected and 0/*/# filled in on 2026-10-11, Plan B Task 14). Keys are decoded by `IrController` (a switch on the code; `ir_speed_pct` lives in `IrController`); camera jog is repeated `CameraHead::nudge` at 60 °/s (no separate jog method).
```

- [ ] **Step 6: Phase 1–2 sign-off (roadmap)**

Tick each item; every one was already exercised in its task — this is the consolidated run on the final build:
- Phase 1 (stand, USB): `probe` PASS · `mount` PASS · `watch` shows all sensors live (yaw, pitch/roll, dist, batt, enc, pan/tilt) · `EVENT SELFTEST a=0` · `setting … --save` survives a reset · `deadman`: `DEADMAN_STOP` at ≈ 300 ms, tracks stopped ≤ ~420 ms · link pull stops the tracks · unplugged ultrasonic → `US_FAULT` + 50 % cap · WS2812 timing on a scope (or "not measured", Task 13 Step 8 item 6) · `soak` PASS · size gate PASS.
- Phase 2 (floor, USB tether): `drive 40 0` heading within ±2° over 4 s · `turn 90 50` / `turn -360 50` within ±2° · `turn 90 50 --then-to 0` back within ±2° · `turn 90 50 --radius 50` quarter circle · `move 1000 40` = 100 ± 3 cm · obstacle slow + BLOCKED at ≈ 15 cm · `drive 100 0 --envelope 20 --accel 50 --lock` caps PWM ≤ 115 · `turn 720 30 --drop-link-after 1 --restore-after 2`: TURN cancelled by link loss does not resume · IR remote drives with no ESP32 · IR key during a DRIVE_RATE spin stops it.

- [ ] **Step 7: Commit**

```bash
git add tools/smoke docs/lld/uno-firmware-lld.md
git commit -m "test(uno): soak test and phase 1-2 sign-off; fix IR codes 5/8/9 in the Uno LLD"
```

Note: Plan A names these enums `AudioModeValue` and `OwnerHintValue` (C++ `proto::AudioModeValue`, `proto::OwnerHintValue`; Python `g.AudioModeValue`, `g.OwnerHintValue`) so they never clash with the `AUDIO_MODE`/`OWNER_HINT` frame classes.

---

## Done when

- `cd firmware/uno && pio test -e native` passes all 131 host tests; `pio run -e uno && python3 scripts/size_gate.py` passes the flash/RAM budget.
- The phase 1 and phase 2 sign-off lists in Task 15 are fully ticked on the real tank.
- `tools/smoke/uno_smoke.py` covers handshake, telemetry, settings, driving, primitives, camera, LED/sound, calibration scripts, probe, mount check, bias and soak — the same link behaviour Plan C's bridge and Plan D's bench tool build on.
- `docs/lld/uno-firmware-lld.md` §9.8 lists the corrected IR codes.
