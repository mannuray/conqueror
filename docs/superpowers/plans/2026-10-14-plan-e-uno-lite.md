# Plan E — Uno Lite Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A small, drivable Uno firmware (≤ 28 KB flash, ≤ 1.5 KB RAM) for the ELEGOO Uno, keeping every safety rule and the same ESP32 protocol, while the full Mega firmware stays untouched.

**Architecture:** New app layer `src/lite/` plus new integer-only pure classes in `src/domain/lite/` (host-tested). The Uno build (`env:uno`, `-DPROFILE_LITE`) compiles only `lite/`, `drivers/`, `domain/lite/` and the reused integer domain modules (protocol, link, IR, safety supervisor, arbitrator, envelope, self-test, battery monitor, EEPROM writer, LED sequencer, scheduler). The Mega build is unchanged.

**Tech Stack:** C++17 (avr-gcc via PlatformIO `atmelavr@^5`, `-Os -flto`), Unity host tests (`env:native`), protocol generator (`protocol/`, `uv run protogen write`).

**Spec:** `docs/superpowers/specs/2026-10-10-02-tank-firmware-design.md` Part D (approved 2026-10-14) and `docs/lld/uno-lite-lld.md` (approved 2026-10-14). Decision history: `docs/design-notes.md` §43.

## Global Constraints
- Budget: **≤ 28 KB flash (28672 B), ≤ 1.5 KB static RAM (1536 B)** for `env:uno`; `scripts/size_gate.py uno` fails the build above that.
- **No `float` or `double`** in `src/lite/` and `src/domain/lite/` (check: `grep -rnE "\bfloat\b|\bdouble\b" src/lite src/domain/lite` returns nothing).
- Uno rules (CLAUDE.md): no heap, no `virtual`, no `delay()`, ISRs capture-only, domain code never includes `ports/` or `Arduino.h`, only `SafetySupervisor` produces `PwmOutput`.
- Safety features are never weakened: deadman (300 ms), link-loss stop, STOP, obstacle brake, child-play envelope caps, battery low/critical, lift/tilt stop, boot self-test lock (LLD §10.9 rules). No Override mode.
- Same protocol to the ESP32: framing, CRC, HELLO/HELLO_ACK/heartbeat; unimplemented frames → `ACK UNAVAILABLE` (`proto::AckStatus::kUnavailable` = 3).
- Generated files are never edited by hand: change `protocol/defs/*.yaml`, run `cd protocol && uv run protogen write`.
- The full firmware must keep building (`pio run -e mega`) and its tests must keep passing (`pio test -e native`).
- Commits: Conventional Commits, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Branch: `uno-lite`.
- Tools: `~/.platformio/penv/bin/pio` (Mac). Firmware dir: `firmware/uno`.

## Review Focus
1. **Yaw wrap during a hold** (heading crosses 0°/360° while holding): the correction must take the short way, not spin the tank. → `test_heading_wraps_short_way` (Task 4).
2. **EEPROM holding someone else's data** (ELEGOO or full-firmware bytes): Lite must load defaults, never garbage gains. → `test_settings_garbage_eeprom_gives_defaults` (Task 9).
3. **IMU fails mid-drive**: steering must keep working open-loop, no stale hold correction. → `test_heading_gyro_lost_is_open_loop` (Task 4).
4. **Obstacle slow while reversing**: the brake/slow zone only limits forward motion. → `test_cap_obstacle_slows_forward_only` (Task 5).
5. **Unknown or unsupported frame types** from the unauthenticated WiFi bridge: TURN/MOVE/LED… answer UNAVAILABLE, unknown types are counted, nothing moves. → `test_frames_unsupported_are_unavailable` (Task 11).

## Rulings made while writing this plan (deviations from the LLD, recorded here and in the LLD)
- **Gyro zero tracks the Z axis only** (Lite has no gyro attitude filter; X/Y gyro are unused). Same rules as LLD §11.8.
- **Motor trim settings are deferred**: the protocol has no trim setting and adding one forces changes to the full settings table. The gyro hold already absorbs the pull (floor test: ~12 % difference handled). Revisit with encoder balance (LLD §8.2).
- **Heading gains reuse existing setting ids:** `pid_heading_kp` (58, milli) → `kp_q8 = milli × 9 / 20` (3500 → 1575 ≈ 6.15 % per degree); `pid_rate_kp` (55, milli) → `kd_q8 = milli × 256 / 1000` (1200 → 307 = 1.2 % per °/s). No integral term (steady error ≈ 1° for the measured pull, like ELEGOO).
- **Lift detection copies the full LiftDetector thresholds** (|az| < 0.6 g for 60 ms → lifted; 0.85–1.15 g for 500 ms → landed), not the LLD's 0.6–1.4 g sketch.
- **Host tests are grouped into five suites** (`test_lite_motion`, `test_lite_sensors`, `test_lite_settings`, `test_lite_ui`, `test_lite_frames`) instead of one per class (build time).
- **Camera:** `CamLite` (instant positioning in deci-degrees, home 90°/90°, limits pan 0–180°, tilt 30–110°, joystick release → home after 500 ms unless locked). Smooth motion is LLD §8 extra 1 (Task 15).

---

### Task 1: Build split, skeleton and first size measurement

**Files:**
- Modify: `firmware/uno/platformio.ini` (env:uno)
- Modify: `firmware/uno/scripts/size_gate.py` (uno limits)
- Modify: `firmware/uno/src/main.cpp` (profile switch)
- Modify: `firmware/uno/src/ports/servos.h`, `firmware/uno/src/drivers/servos_uno.cpp` (integer `writeUs`)
- Create: `firmware/uno/src/lite/lite_app.h`, `firmware/uno/src/lite/lite_app.cpp`, `firmware/uno/src/lite/lite_comms.cpp`

**Interfaces:**
- Produces: `namespace cq::lite { void setup(); void loop(); struct LiteApp; extern LiteApp g_lite; void commsPoll(); void txPump(); }`; `port::servos::writeUs(Axis, uint16_t us)`.

- [ ] **Step 1: Uno env and size gate.** In `platformio.ini` replace the `[env:uno]` section with:

```ini
; Uno = Lite profile (spec 02 Part D, docs/lld/uno-lite-lld.md). Only lite/ + integer domain modules.
[env:uno]
platform = atmelavr@^5
board = uno
framework = arduino
lib_deps = arduino-libraries/Servo@^1.2.2
extra_scripts = pre:scripts/git_commit.py
build_flags = ${env.build_flags} -DBOARD_UNO -DPROFILE_LITE -Os -flto -fno-exceptions -fno-rtti
build_src_filter =
  +<main.cpp> +<lite/> +<drivers/> +<domain/lite/> +<domain/comms/>
  +<domain/input/nec_decoder.cpp> +<domain/input/ir_controller.cpp> +<domain/input/boot_button.cpp>
  +<domain/safety/safety_supervisor.cpp> +<domain/safety/envelope.cpp> +<domain/arbitration/>
  +<domain/estimation/battery_monitor.cpp> +<domain/estimation/ultrasonic_filter.cpp>
  +<domain/settings/eeprom_writer.cpp> +<domain/ui/led_sequencer.cpp> +<domain/util/scheduler.cpp>
build_src_flags = -Wall -Wextra -Werror
upload_speed = 115200
monitor_speed = 250000
```

In `scripts/size_gate.py` set the `uno` entry of `LIMITS` to `(28672, 1536)`.

- [ ] **Step 2: Integer servo write.** In `src/ports/servos.h` add after `writeDeg`:

```cpp
void writeUs(Axis a, uint16_t us);  // integer path for the Lite firmware (544..2400 µs)
```

In `src/drivers/servos_uno.cpp` add after `writeDeg`:

```cpp
void writeUs(Axis a, uint16_t us) {
  if (us < 544) us = 544;
  if (us > 2400) us = 2400;
  g_servo[static_cast<uint8_t>(a)].writeMicroseconds(us);
}
```

- [ ] **Step 3: Profile switch in `main.cpp`.** Wrap the whole existing file body after the includes so the Lite build uses its own app:

```cpp
#if defined(PROFILE_LITE)
#include "lite/lite_app.h"
void setup() { cq::lite::setup(); }
void loop() { cq::lite::loop(); }
#else
// ... the existing includes-dependent content of main.cpp, unchanged ...
#endif
```

(The existing `#include "app/..."` lines move inside the `#else` branch.)

- [ ] **Step 4: Skeleton app.** Create `src/lite/lite_app.h`:

```cpp
#pragma once
#include <stdint.h>

#include "domain/comms/event_queue.h"
#include "domain/comms/frame_parser.h"
#include "domain/comms/frame_writer.h"
#include "domain/comms/link_session.h"
#include "generated/cq_protocol.h"

namespace cq {
namespace lite {

// All Lite state in one object (no heap). Later tasks add members before LITE-STATE-END.
struct LiteApp {
  FrameParser parser;
  FrameWriter writer;
  EventQueue events;
  AckQueue acks;
  RejectLimiter rejects;
  LinkSession link;
  proto::TelemFastMsg fast{};
  proto::TelemSlowMsg slow{};
  bool fast_due = false;
  bool slow_due = false;
  proto::SettingValueMsg setting_value{};
  bool setting_value_pending = false;
  proto::ResetReason reset_reason = proto::ResetReason::kUnknown;
  bool encoders_enabled = false;
  uint16_t fast_flags = 0;
  uint32_t slow_flags = 0;
  // LITE-STATE-END
};
extern LiteApp g_lite;

void setup();
void loop();
void commsPoll();
void txPump();
void telemetryFastDue();
void telemetrySlowDue();

}  // namespace lite
}  // namespace cq
```

Create `src/lite/lite_comms.cpp` (routing is added in Task 11; the skeleton answers HELLO_ACK / HELLO_REQ / ESP_ACK / HEARTBEAT and drops everything else):

```cpp
#include "drivers/board_config.h"
#include "lite/lite_app.h"
#include "ports/clock.h"
#include "ports/link.h"

namespace cq {
namespace lite {

LiteApp g_lite;

namespace {
constexpr uint8_t kPollSteps = 64;  // same bound as the full app (LLD §5: one hostile byte ≤ ~0.7 ms)
uint8_t g_tx[proto::kMaxPayload + 5];

template <typename Msg>
bool sendMsg(const Msg& m) {
  uint8_t payload[proto::kMaxPayload];
  const uint8_t n = proto::encode(m, payload);
  if (n == proto::kEncodeError) return false;
  const uint8_t total = g_lite.writer.build(static_cast<uint8_t>(Msg::kType), payload, n, g_tx);
  if (port::link::txFree() < total) {
    g_lite.writer.setSeq(g_lite.writer.lastSeq());
    ++g_lite.parser.errors().tx_dropped;
    return false;
  }
  return port::link::write(g_tx, total);
}

bool sendRaw(uint8_t type) {
  const uint8_t total = g_lite.writer.build(type, nullptr, 0, g_tx);
  if (port::link::txFree() < total) {
    g_lite.writer.setSeq(g_lite.writer.lastSeq());
    return false;
  }
  return port::link::write(g_tx, total);
}
}  // namespace

// Defined in lite_routes.cpp from Task 11; the skeleton version lives here until then.
bool route(uint8_t type, const uint8_t* p, uint8_t len, proto::AckStatus& st);

void commsPoll() {
  uint8_t budget = kPollSteps;
  uint8_t b = 0;
  while (budget != 0) {
    if (!g_lite.parser.pending()) {
      if (!port::link::readByte(b)) break;
      g_lite.parser.feed(b);
    }
    if (!g_lite.parser.next(budget)) continue;
    g_lite.link.onValidFrame(port::clock::nowMs());
    proto::AckStatus st = proto::AckStatus::kOk;
    if (route(g_lite.parser.type(), g_lite.parser.payload(), g_lite.parser.len(), st)) {
      g_lite.acks.push(g_lite.parser.seq(), st);
    }
  }
}

void txPump() {
  const uint32_t now = port::clock::nowMs();
  uint8_t seq = 0;
  proto::AckStatus st{};
  while (g_lite.acks.peek(seq, st)) {
    proto::AckMsg a{};
    a.ack_seq = seq;
    a.status = static_cast<uint8_t>(st);
    if (!sendMsg(a)) return;
    g_lite.acks.pop(seq, st);
  }
  uint8_t req = 0;
  if (g_lite.link.requestDue(now, req)) {
    if (!sendRaw(req)) return;
    g_lite.link.onRequestSent(g_lite.writer.lastSeq(), now);
  }
  Event e{};
  while (g_lite.events.peek(e)) {
    proto::EventMsg m{};
    m.kind = e.kind;
    m.a = e.a;
    m.b = e.b;
    if (!sendMsg(m)) return;
    g_lite.events.pop(e);
  }
  if (g_lite.link.helloDue(now)) {
    proto::HelloMsg h{};
    h.fw = board::kFwVersion;
    h.proto = proto::kProtocolVersion;
    h.caps = g_lite.encoders_enabled ? 1 : 0;  // Task 11 replaces this with helloCaps()
    h.reset_reason = static_cast<uint8_t>(g_lite.reset_reason);
    h.commit = board::kGitCommit;
    sendMsg(h);
  }
  if (g_lite.setting_value_pending && sendMsg(g_lite.setting_value)) g_lite.setting_value_pending = false;
  if (g_lite.fast_due) {
    g_lite.fast.uno_ms = now;
    if (sendMsg(g_lite.fast)) g_lite.fast_due = false;
  }
  if (g_lite.slow_due && sendMsg(g_lite.slow)) g_lite.slow_due = false;
}

void telemetryFastDue() { g_lite.fast_due = true; }

void telemetrySlowDue() {
  g_lite.slow.uptime_s = port::clock::nowMs() / 1000u;
  g_lite.slow.reset_reason = static_cast<uint8_t>(g_lite.reset_reason);
  g_lite.slow.err_frames = static_cast<uint16_t>(g_lite.parser.errors().bad_crc + g_lite.parser.errors().bad_len +
                                                 g_lite.parser.errors().unknown_type);
  g_lite.slow.flags2 = g_lite.slow_flags;
  g_lite.slow_due = true;
}

}  // namespace lite
}  // namespace cq
```

Create `src/lite/lite_app.cpp` (skeleton: drivers up, link, telemetry; motors held at zero through `PwmOutput::zero()`):

```cpp
#include "drivers/board_config.h"
#include "domain/util/scheduler.h"
#include "lite/lite_app.h"
#include "ports/battery_adc.h"
#include "ports/button.h"
#include "ports/clock.h"
#include "ports/encoders.h"
#include "ports/imu.h"
#include "ports/ir.h"
#include "ports/led.h"
#include "ports/link.h"
#include "ports/motor.h"
#include "ports/servos.h"
#include "ports/tone.h"
#include "ports/ultrasonic.h"
#include "ports/watchdog.h"

namespace cq {
namespace lite {

// Skeleton router (replaced by lite_routes.cpp in Task 11): link frames only; nothing else is acked.
bool route(uint8_t type, const uint8_t* p, uint8_t len, proto::AckStatus& st) {
  (void)st;
  switch (static_cast<proto::FrameType>(type)) {
    case proto::FrameType::kHelloAck: {
      proto::HelloAckMsg m{};
      if (proto::decode(p, len, m)) g_lite.link.onHelloAck();
      return false;
    }
    case proto::FrameType::kHelloReq: g_lite.link.onHelloReq(); return false;
    default: ++g_lite.parser.errors().unknown_type; return false;
  }
}

namespace {
void linkWatchTask() {
  switch (g_lite.link.poll(port::clock::nowMs())) {
    case LinkChange::kLost: g_lite.events.push(proto::EventKind::kLinkLost); break;
    case LinkChange::kRestored: g_lite.events.push(proto::EventKind::kLinkRestored); break;
    case LinkChange::kNone: break;
  }
}
void motorsZeroTask() { port::motor::apply(PwmOutput::zero()); }

// LITE-TASKS-BEGIN
Task g_tasks[] = {
    {linkWatchTask, 10000UL, 2500UL, 0},
    {motorsZeroTask, 10000UL, 2500UL, 0},
    {telemetryFastDue, 50000UL, 0UL, 0},
    {telemetrySlowDue, 1000000UL, 7500UL, 0},
};
// LITE-TASKS-END
Scheduler g_scheduler(g_tasks, static_cast<uint8_t>(sizeof g_tasks / sizeof g_tasks[0]));
}  // namespace

void setup() {
  g_lite.reset_reason = port::watchdog::resetReason();
  port::link::begin(board::kBaud);
  port::motor::begin();
  port::imu::begin();
  port::encoders::begin();
  port::ultrasonic::begin();
  port::ir::begin();
  port::battery_adc::begin();
  port::button::begin();
  port::led::begin();
  port::tone::begin();
  port::servos::begin();
  port::watchdog::enable();
  g_scheduler.start(port::clock::nowUs());
}

void loop() {
  const uint32_t t0 = port::clock::nowUs();
  commsPoll();
  g_scheduler.runDue(t0);
  txPump();
  port::watchdog::kick();
}

}  // namespace lite
}  // namespace cq
```

- [ ] **Step 5: Build both profiles and measure.**

Run: `cd firmware/uno && ~/.platformio/penv/bin/pio run -e uno && python3 scripts/size_gate.py uno && ~/.platformio/penv/bin/pio run -e mega && python3 scripts/size_gate.py mega`
Expected: both build. Record the Uno skeleton flash/RAM in LLD §7 as "measured skeleton". **Stop rule:** the LLD estimates core + drivers + protocol + link at ≈ 12.0 KB / 0.66 KB; if the skeleton is above 13.8 KB flash or 0.76 KB RAM (+15 %), stop and review with the owner before Task 2.

- [ ] **Step 6: Float check and full tests.**

Run: `grep -rnE "\bfloat\b|\bdouble\b" src/lite src/domain/lite; ~/.platformio/penv/bin/pio test -e native 2>&1 | tail -1`
Expected: grep prints nothing; `182 test cases: 182 succeeded`.

- [ ] **Step 7: Commit.**

```bash
git add firmware/uno/platformio.ini firmware/uno/scripts/size_gate.py firmware/uno/src/main.cpp firmware/uno/src/ports/servos.h firmware/uno/src/drivers/servos_uno.cpp firmware/uno/src/lite docs/lld/uno-lite-lld.md
git commit -m "feat(uno-lite): Uno build runs the Lite skeleton (link, telemetry, drivers); size measured

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: GyroZeroLite (Z axis) — `test_lite_motion`

**Files:**
- Create: `firmware/uno/src/domain/lite/gyro_zero_lite.h`, `.cpp`
- Test: `firmware/uno/test/test_lite_motion/test_main.cpp` (new suite; later tasks add to it)

**Interfaces:**
- Produces: `class GyroZeroLite { void seed(int16_t stored_raw); void update(int16_t raw, bool quiet, uint32_t now_ms); int16_t zero() const; uint16_t accepted() const; }`

- [ ] **Step 1: Failing tests.** Create `test/test_lite_motion/test_main.cpp`:

```cpp
#include <unity.h>

#include "domain/lite/gyro_zero_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
// n samples of z with ±3 alternating noise every 5 ms; returns the next sample time.
uint32_t feedZ(GyroZeroLite& g, int16_t z, int n, uint32_t t, bool quiet = true) {
  for (int i = 0; i < n; ++i, t += 5) g.update(static_cast<int16_t>(z + ((i & 1) ? 3 : -3)), quiet, t);
  return t;
}
}  // namespace

void test_gzl_starts_from_stored() {
  GyroZeroLite g;
  g.seed(-17);
  TEST_ASSERT_EQUAL_INT16(-17, g.zero());
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_learns_after_two_agreeing_windows() {
  GyroZeroLite g;
  g.seed(0);
  uint32_t t = feedZ(g, -13, 200 + 400, 0);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
  feedZ(g, -13, 400, t);
  TEST_ASSERT_EQUAL_UINT16(1, g.accepted());
  TEST_ASSERT_EQUAL_INT16(-13, g.zero());
}

void test_gzl_ignores_a_bump() {
  GyroZeroLite g;
  g.seed(0);
  uint32_t t = feedZ(g, -13, 200 + 200, 0);
  t = feedZ(g, 200, 1, t);
  t = feedZ(g, -13, 199, t);
  feedZ(g, -13, 400, t);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_motion_restarts_settle() {
  GyroZeroLite g;
  g.seed(0);
  uint32_t t = feedZ(g, -13, 200 + 200, 0);
  t = feedZ(g, -13, 1, t, false);
  feedZ(g, -13, 800, t);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_disagreeing_windows_not_learned() {
  GyroZeroLite g;
  g.seed(0);
  uint32_t t = feedZ(g, 100, 200 + 400, 0);
  t = feedZ(g, 140, 400, t);
  feedZ(g, 180, 400, t);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_rejects_out_of_range() {
  GyroZeroLite g;
  g.seed(0);
  feedZ(g, 1400, 200 + 800, 0);
  TEST_ASSERT_EQUAL_UINT16(0, g.accepted());
}

void test_gzl_reseed_only_on_change() {
  GyroZeroLite g;
  g.seed(0);
  feedZ(g, -13, 200 + 800, 0);
  g.seed(0);
  TEST_ASSERT_EQUAL_INT16(-13, g.zero());
  g.seed(-15);
  TEST_ASSERT_EQUAL_INT16(-15, g.zero());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_gzl_starts_from_stored);
  RUN_TEST(test_gzl_learns_after_two_agreeing_windows);
  RUN_TEST(test_gzl_ignores_a_bump);
  RUN_TEST(test_gzl_motion_restarts_settle);
  RUN_TEST(test_gzl_disagreeing_windows_not_learned);
  RUN_TEST(test_gzl_rejects_out_of_range);
  RUN_TEST(test_gzl_reseed_only_on_change);
  // LITE-MOTION-TESTS-END
  return UNITY_END();
}
```

- [ ] **Step 2: Run, expect a compile failure** (`gyro_zero_lite.h` not found).

Run: `cd firmware/uno && ~/.platformio/penv/bin/pio test -e native -f test_lite_motion`

- [ ] **Step 3: Implement.** `src/domain/lite/gyro_zero_lite.h`:

```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Learns the Z gyro zero while the tank stands still (LLD §11.8 rules, Lite: Z only, integers).
// Quiet 1 s, then 2 s windows (400 samples at 200 Hz). Valid window: spread ≤ 98 raw (1.5 °/s), |mean| ≤ 1310 raw
// (20 °/s). Two valid windows in a row within 20 raw (0.3 °/s) → zero = their average. RAM only.
class GyroZeroLite {
 public:
  static constexpr uint16_t kSettleMs = 1000;
  static constexpr uint16_t kWindowSamples = 400;
  static constexpr int16_t kMaxSpreadRaw = 98;
  static constexpr int16_t kMaxZeroRaw = 1310;
  static constexpr int16_t kAgreeRaw = 20;
  void seed(int16_t stored_raw);  // restarts only when the stored value changed (applySettings runs often)
  void update(int16_t raw, bool quiet, uint32_t now_ms);
  int16_t zero() const { return m_zero; }
  uint16_t accepted() const { return m_accepted; }

 private:
  enum class State : uint8_t { kWaitQuiet, kSettle, kCollect };
  void startWindow();
  void judge();
  State m_state = State::kWaitQuiet;
  uint32_t m_quiet_since_ms = 0;
  uint16_t m_n = 0;
  int32_t m_sum = 0;
  int16_t m_min = 0;
  int16_t m_max = 0;
  bool m_have_prev = false;
  int16_t m_prev = 0;
  int16_t m_zero = 0;
  int16_t m_seeded = 0;
  bool m_seeded_valid = false;
  uint16_t m_accepted = 0;
};

}  // namespace cq
```

`src/domain/lite/gyro_zero_lite.cpp`:

```cpp
#include "domain/lite/gyro_zero_lite.h"

namespace cq {

namespace {
int16_t absI(int16_t v) { return v < 0 ? static_cast<int16_t>(-v) : v; }
}  // namespace

void GyroZeroLite::seed(int16_t stored_raw) {
  if (m_seeded_valid && m_seeded == stored_raw) return;
  m_seeded = stored_raw;
  m_seeded_valid = true;
  m_zero = stored_raw;
  m_state = State::kWaitQuiet;
  m_have_prev = false;
  m_accepted = 0;
}

void GyroZeroLite::startWindow() {
  m_n = 0;
  m_sum = 0;
  m_min = INT16_MAX;
  m_max = INT16_MIN;
}

void GyroZeroLite::update(int16_t raw, bool quiet, uint32_t now_ms) {
  if (!quiet) {
    m_state = State::kWaitQuiet;
    return;
  }
  if (m_state == State::kWaitQuiet) {
    m_quiet_since_ms = now_ms;
    m_state = State::kSettle;
  }
  if (m_state == State::kSettle) {
    if (static_cast<uint32_t>(now_ms - m_quiet_since_ms) < kSettleMs) return;
    startWindow();
    m_state = State::kCollect;
  }
  m_sum += raw;
  if (raw < m_min) m_min = raw;
  if (raw > m_max) m_max = raw;
  if (++m_n < kWindowSamples) return;
  judge();
  startWindow();
}

void GyroZeroLite::judge() {
  const int32_t half = m_n / 2;
  const int16_t mean = static_cast<int16_t>((m_sum >= 0 ? m_sum + half : m_sum - half) / m_n);
  const bool valid = static_cast<int32_t>(m_max) - m_min <= kMaxSpreadRaw && absI(mean) <= kMaxZeroRaw;
  if (!valid) {
    m_have_prev = false;
    return;
  }
  if (m_have_prev && absI(static_cast<int16_t>(mean - m_prev)) <= kAgreeRaw) {
    m_zero = static_cast<int16_t>((static_cast<int32_t>(mean) + m_prev) / 2);
    ++m_accepted;
  }
  m_prev = mean;
  m_have_prev = true;
}

}  // namespace cq
```

- [ ] **Step 4: Run, expect PASS (7 tests).** Same command as Step 2.

- [ ] **Step 5: Commit.**

```bash
git add firmware/uno/src/domain/lite/gyro_zero_lite.* firmware/uno/test/test_lite_motion/test_main.cpp
git commit -m "feat(uno-lite): integer gyro zero tracker (Z axis)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: YawLite — `test_lite_motion`

**Files:** Create `firmware/uno/src/domain/lite/yaw_lite.h`, `.cpp`; modify `test/test_lite_motion/test_main.cpp`.

**Interfaces:**
- Produces: `class YawLite { static constexpr int32_t kTurn = 4716000; void update(int16_t gz_corrected, bool stationary); int32_t yawCdeg() const /*0..35999*/; int16_t rateDdps() const /*0.1 °/s, CCW +*/; void reset(); }`

- [ ] **Step 1: Failing tests** (add before `int main`, register before `// LITE-MOTION-TESTS-END`):

```cpp
#include "domain/lite/yaw_lite.h"

// 65.5 raw = 1 °/s; 200 samples = 1 s → 655 raw for 200 samples = 10° = 1000 cdeg.
void test_yaw_integrates_ccw() {
  YawLite y;
  for (int i = 0; i < 200; ++i) y.update(655, false);
  TEST_ASSERT_INT32_WITHIN(2, 1000, y.yawCdeg());
  TEST_ASSERT_EQUAL_INT16(100, y.rateDdps());  // 10.0 °/s
}

void test_yaw_wraps_below_zero() {
  YawLite y;
  for (int i = 0; i < 200; ++i) y.update(-655, false);  // 10° clockwise from 0
  TEST_ASSERT_INT32_WITHIN(2, 35000, y.yawCdeg());
}

void test_yaw_still_clamp_only_when_stationary() {
  YawLite y;
  for (int i = 0; i < 200; ++i) y.update(20, true);  // 0.3 °/s noise at rest: ignored
  TEST_ASSERT_EQUAL_INT32(0, y.yawCdeg());
  for (int i = 0; i < 2000; ++i) y.update(20, false);  // same rate while moving: counted (≈ 3.05°)
  TEST_ASSERT_INT32_WITHIN(5, 305, y.yawCdeg());
}
```

Register: `RUN_TEST(test_yaw_integrates_ccw); RUN_TEST(test_yaw_wraps_below_zero); RUN_TEST(test_yaw_still_clamp_only_when_stationary);`

- [ ] **Step 2: Run, expect compile failure.** `~/.platformio/penv/bin/pio test -e native -f test_lite_motion`

- [ ] **Step 3: Implement.** `yaw_lite.h`:

```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Heading from the Z gyro, integers only. IMU at 200 Hz, ±500 °/s = 65.5 raw per °/s, so one degree is
// 65.5 × 200 = 13100 raw·samples and a full turn kTurn = 4716000. yaw [cdeg] = acc / 131. CCW positive.
class YawLite {
 public:
  static constexpr int32_t kTurn = 4716000;
  static constexpr int16_t kStillRaw = 33;  // 0.5 °/s: ignored while the tank is not driven (full: same clamp)
  void update(int16_t gz_corrected, bool stationary);
  int32_t yawCdeg() const { return m_acc / 131; }
  int16_t rateDdps() const { return m_rate_ddps; }
  void reset() { m_acc = 0; }

 private:
  int32_t m_acc = 0;  // 0 .. kTurn-1
  int16_t m_rate_ddps = 0;
};

}  // namespace cq
```

`yaw_lite.cpp`:

```cpp
#include "domain/lite/yaw_lite.h"

namespace cq {

void YawLite::update(int16_t g, bool stationary) {
  if (stationary && g > -kStillRaw && g < kStillRaw) g = 0;
  m_rate_ddps = static_cast<int16_t>(static_cast<int32_t>(g) * 20 / 131);  // raw → 0.1 °/s
  m_acc += g;
  if (m_acc >= kTurn) m_acc -= kTurn;
  if (m_acc < 0) m_acc += kTurn;
}

}  // namespace cq
```

- [ ] **Step 4: Run, expect PASS (10 tests).**
- [ ] **Step 5: Commit** `feat(uno-lite): integer yaw integrator` with the two new files and the test file.

---

### Task 4: HeadingHoldLite — `test_lite_motion`

**Files:** Create `firmware/uno/src/domain/lite/heading_hold_lite.h`, `.cpp`; modify the motion test.

**Interfaces:**
- Consumes: yaw in cdeg (0..35999) and rate in 0.1 °/s from `YawLite`.
- Produces: `struct HeadingGains { int16_t kp_q8; int16_t kd_q8; }; class HeadingHoldLite { void configure(const HeadingGains&); void teleop(int8_t v, int8_t w, bool gyro_ok, int32_t yaw_cdeg, int16_t rate_ddps, int8_t& w_steer, int8_t& w_ctrl); void reset(); bool holding() const; }`; `HeadingGains headingGainsFromMilli(int32_t heading_kp_milli, int32_t rate_kp_milli)`.

- [ ] **Step 1: Failing tests:**

```cpp
#include "domain/lite/heading_hold_lite.h"

namespace {
HeadingHoldLite hh() {
  HeadingHoldLite h;
  h.configure(headingGainsFromMilli(3500, 1200));
  return h;
}
}  // namespace

void test_heading_gains_from_settings() {
  const HeadingGains g = headingGainsFromMilli(3500, 1200);
  TEST_ASSERT_EQUAL_INT16(1575, g.kp_q8);
  TEST_ASSERT_EQUAL_INT16(307, g.kd_q8);
}

void test_heading_steering_is_open_loop() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(0, 40, true, 1000, 2000, s, c);
  TEST_ASSERT_EQUAL_INT8(40, s);
  TEST_ASSERT_EQUAL_INT8(0, c);
  TEST_ASSERT_FALSE(h.holding());
}

void test_heading_captures_then_corrects_drift() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 1000, 0, s, c);  // straight, not rotating → capture 10.00°
  TEST_ASSERT_TRUE(h.holding());
  h.teleop(80, 0, true, 900, 0, s, c);   // drifted 1° clockwise → turn CCW (positive), ≈ 6 %
  TEST_ASSERT_EQUAL_INT8(0, s);
  TEST_ASSERT_INT8_WITHIN(1, 6, c);
}

void test_heading_rate_damping() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 1000, 0, s, c);
  h.teleop(80, 0, true, 1000, 100, s, c);  // on heading but turning CCW at 10 °/s → push CW (≈ −12 %)
  TEST_ASSERT_INT8_WITHIN(1, -12, c);
}

void test_heading_waits_for_turn_to_settle_max_300ms() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 50, true, 0, 900, s, c);  // steering
  for (int i = 0; i < 29; ++i) h.teleop(80, 0, true, 0, 200, s, c);  // still turning 20 °/s
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL_INT8(0, c);  // no braking while settling
  h.teleop(80, 0, true, 0, 200, s, c);
  h.teleop(80, 0, true, 0, 200, s, c);
  TEST_ASSERT_TRUE(h.holding());  // 0.3 s passed: captured despite the pull
}

void test_heading_wraps_short_way() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 50, 0, s, c);      // hold 0.50°
  h.teleop(80, 0, true, 35950, 0, s, c);   // now 359.50° = 1° clockwise of the hold → small CCW push
  TEST_ASSERT_INT8_WITHIN(1, 6, c);
}

void test_heading_gyro_lost_is_open_loop() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 1000, 0, s, c);
  h.teleop(80, 0, false, 500, 0, s, c);  // gyro gone: no stale correction
  TEST_ASSERT_EQUAL_INT8(0, c);
  TEST_ASSERT_EQUAL_INT8(0, s);
  TEST_ASSERT_FALSE(h.holding());
  h.teleop(80, 30, false, 500, 0, s, c);
  TEST_ASSERT_EQUAL_INT8(30, s);
}

void test_heading_stop_resets() {
  HeadingHoldLite h = hh();
  int8_t s = 0, c = 0;
  h.teleop(80, 0, true, 1000, 0, s, c);
  h.teleop(0, 0, true, 1000, 0, s, c);
  TEST_ASSERT_FALSE(h.holding());
}
```

Register all eight.

- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement.** `heading_hold_lite.h`:

```cpp
#pragma once
#include <stdint.h>

namespace cq {

// kp: % turn per degree × 256; kd: % per °/s × 256.
struct HeadingGains {
  int16_t kp_q8;
  int16_t kd_q8;
};
// Settings are in milli units (full firmware ids 58 pid_heading_kp, 55 pid_rate_kp). 3500 → 1575 (≈ 6.15 %/°,
// ±10 PWM per track per degree at 80 %); 1200 → 307 (1.2 % per °/s).
HeadingGains headingGainsFromMilli(int32_t heading_kp_milli, int32_t rate_kp_milli);

// Floor-tested behaviour (design-notes §41): steering is open-loop; when it stops, wait for |rate| < 5 °/s or
// 0.3 s, capture the heading, then hold it with P on the error and D on the rate. Not scaled by turn authority.
class HeadingHoldLite {
 public:
  static constexpr int16_t kCaptureDdps = 50;
  static constexpr uint8_t kSettleTicks = 30;  // 10 ms control ticks
  void configure(const HeadingGains& g) { m_g = g; }
  // One of w_steer (driver, open loop) and w_ctrl (hold) is set; the other is 0.
  void teleop(int8_t v_pct, int8_t w_pct, bool gyro_ok, int32_t yaw_cdeg, int16_t rate_ddps, int8_t& w_steer,
              int8_t& w_ctrl);
  void reset();
  bool holding() const { return m_state == State::kHolding; }

 private:
  enum class State : uint8_t { kIdle, kSteering, kSettling, kHolding };
  HeadingGains m_g{1575, 307};
  State m_state = State::kIdle;
  int32_t m_held_cdeg = 0;
  uint8_t m_settle = 0;
};

}  // namespace cq
```

`heading_hold_lite.cpp`:

```cpp
#include "domain/lite/heading_hold_lite.h"

namespace cq {

HeadingGains headingGainsFromMilli(int32_t heading_kp_milli, int32_t rate_kp_milli) {
  return HeadingGains{static_cast<int16_t>(heading_kp_milli * 9 / 20), static_cast<int16_t>(rate_kp_milli * 256 / 1000)};
}

void HeadingHoldLite::reset() {
  m_state = State::kIdle;
  m_settle = 0;
}

void HeadingHoldLite::teleop(int8_t v, int8_t w, bool gyro_ok, int32_t yaw_cdeg, int16_t rate_ddps, int8_t& w_steer,
                             int8_t& w_ctrl) {
  w_steer = 0;
  w_ctrl = 0;
  if (!gyro_ok) {
    reset();
    w_steer = w;
    return;
  }
  if (v == 0 && w == 0) {
    reset();
    return;
  }
  if (w != 0) {
    m_state = State::kSteering;
    w_steer = w;
    return;
  }
  if (m_state != State::kHolding) {
    if (m_state != State::kSettling) {
      m_state = State::kSettling;
      m_settle = 0;
    }
    const int16_t r = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
    if (r > kCaptureDdps && m_settle < kSettleTicks) {
      ++m_settle;
      return;
    }
    m_held_cdeg = yaw_cdeg;
    m_state = State::kHolding;
  }
  int32_t err = m_held_cdeg - yaw_cdeg;  // short way round: −18000..17999
  if (err >= 18000) err -= 36000;
  if (err < -18000) err += 36000;
  int32_t out = static_cast<int32_t>(m_g.kp_q8) * err / 25600 - static_cast<int32_t>(m_g.kd_q8) * rate_ddps / 2560;
  if (out > 100) out = 100;
  if (out < -100) out = -100;
  w_ctrl = static_cast<int8_t>(out);
}

}  // namespace cq
```

- [ ] **Step 4: Run, expect PASS (18 tests).**
- [ ] **Step 5: Commit** `feat(uno-lite): integer heading hold (open-loop steering, P+D hold)`.

---

### Task 5: DriveMixLite — `test_lite_motion`

**Files:** Create `firmware/uno/src/domain/lite/drive_mix_lite.h`, `.cpp`; modify the motion test.

**Interfaces:**
- Produces: `struct Tracks { int16_t l; int16_t r; };` (percent) · `Tracks mixLite(int8_t v, int8_t w_steer, int8_t w_ctrl, uint8_t turn_full_pct)` · `Tracks capLite(Tracks t, uint8_t cap_pct, uint16_t dist_mm, uint8_t stop_cm, uint8_t slow_cm, bool brake_on, bool& capped)` · `class RampLite { int16_t step(int16_t target_pct, uint16_t accel_pct_s); void reset(); int16_t value() const /* 0.1 % */; }` · `int16_t toPwmLite(int16_t permille, bool moving, uint8_t start_pwm, uint8_t keep_pwm)`.

- [ ] **Step 1: Failing tests:**

```cpp
#include "domain/lite/drive_mix_lite.h"

void test_mix_steering_scaled_gyro_not() {
  Tracks t = mixLite(80, 0, 10, 40);  // gyro 10 % at 80 %: full 10
  TEST_ASSERT_EQUAL_INT16(70, t.l);
  TEST_ASSERT_EQUAL_INT16(90, t.r);
  t = mixLite(80, 10, 0, 40);         // driver 10 % at 80 %: authority 52 % → 5
  TEST_ASSERT_EQUAL_INT16(75, t.l);
  TEST_ASSERT_EQUAL_INT16(85, t.r);
}

void test_mix_turning_priority() {
  const Tracks t = mixLite(100, 50, 0, 40);  // w = 20 → v reduced to 80, difference 40 kept
  TEST_ASSERT_EQUAL_INT16(60, t.l);
  TEST_ASSERT_EQUAL_INT16(100, t.r);
}

void test_cap_keeps_ratio() {
  bool capped = false;
  const Tracks t = capLite(Tracks{40, 80}, 40, 0xFFFF, 15, 50, true, capped);
  TEST_ASSERT_TRUE(capped);
  TEST_ASSERT_EQUAL_INT16(20, t.l);
  TEST_ASSERT_EQUAL_INT16(40, t.r);
}

void test_cap_obstacle_slows_forward_only() {
  bool capped = false;
  Tracks t = capLite(Tracks{60, 60}, 100, 325, 15, 50, true, capped);  // halfway between 150 and 500 mm
  TEST_ASSERT_EQUAL_INT16(30, t.l);
  t = capLite(Tracks{-60, -60}, 100, 325, 15, 50, true, capped);       // reversing: not slowed
  TEST_ASSERT_EQUAL_INT16(-60, t.l);
  t = capLite(Tracks{60, 60}, 100, 325, 15, 50, false, capped);        // brake off: not slowed
  TEST_ASSERT_EQUAL_INT16(60, t.l);
  t = capLite(Tracks{60, 60}, 100, 0xFFFE, 15, 50, true, capped);      // sensor fault: not slowed
  TEST_ASSERT_EQUAL_INT16(60, t.l);
}

void test_ramp_accel_and_faster_decel() {
  RampLite r;
  r.step(100, 200);                         // 200 %/s → 2 % per 10 ms tick
  TEST_ASSERT_EQUAL_INT16(20, r.value());   // 0.1 % units
  for (int i = 0; i < 9; ++i) r.step(100, 200);
  TEST_ASSERT_EQUAL_INT16(200, r.value());
  r.step(0, 200);                           // decel twice as fast: −4 %
  TEST_ASSERT_EQUAL_INT16(160, r.value());
}

void test_to_pwm_deadband() {
  TEST_ASSERT_EQUAL_INT16(0, toPwmLite(4, false, 110, 90));
  TEST_ASSERT_EQUAL_INT16(110, toPwmLite(5, false, 110, 90));   // start PWM at the first 0.5 %
  TEST_ASSERT_EQUAL_INT16(222, toPwmLite(800, true, 110, 90));  // 90 + 0.8 × 165
  TEST_ASSERT_EQUAL_INT16(-255, toPwmLite(-1000, true, 110, 90));
}
```

- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement.** `drive_mix_lite.h`:

```cpp
#pragma once
#include <stdint.h>

namespace cq {

struct Tracks {
  int16_t l;  // %
  int16_t r;
};

// Arcade mix (CCW +): the turn authority (less turning at speed) applies to the driver's steering only;
// the gyro's w_ctrl goes in unscaled (design-notes §41). Turning priority keeps L − R when a track would exceed 100 %.
Tracks mixLite(int8_t v, int8_t w_steer, int8_t w_ctrl, uint8_t turn_full_pct);
// Envelope / degraded speed cap (ratio kept) and the obstacle slow zone (forward only, brake on, valid range).
Tracks capLite(Tracks t, uint8_t cap_pct, uint16_t dist_mm, uint8_t stop_cm, uint8_t slow_cm, bool brake_on,
               bool& capped);

// Acceleration limit per 10 ms tick, value in 0.1 % (decel twice as fast, as the full firmware).
class RampLite {
 public:
  int16_t step(int16_t target_pct, uint16_t accel_pct_s);
  void reset() { m_pm = 0; }
  int16_t value() const { return m_pm; }

 private:
  int16_t m_pm = 0;
};

// Deadband compensation: |0.1 %| < 5 → 0; else base + |pm| × (255 − base) / 1000, base = keep when moving.
int16_t toPwmLite(int16_t permille, bool moving, uint8_t start_pwm, uint8_t keep_pwm);

}  // namespace cq
```

`drive_mix_lite.cpp`:

```cpp
#include "domain/lite/drive_mix_lite.h"

namespace cq {

namespace {
int16_t absI(int16_t v) { return v < 0 ? static_cast<int16_t>(-v) : v; }
int16_t clampI(int16_t v, int16_t lo, int16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

Tracks mixLite(int8_t v8, int8_t w_steer, int8_t w_ctrl, uint8_t turn_full_pct) {
  int16_t v = v8;
  const int16_t authority = static_cast<int16_t>(100 - (100 - turn_full_pct) * absI(v) / 100);
  int16_t w = clampI(static_cast<int16_t>(w_steer * authority / 100 + w_ctrl), -100, 100);
  const int16_t excess = static_cast<int16_t>(absI(v) + absI(w) - 100);
  if (excess > 0) v = static_cast<int16_t>(v > 0 ? v - excess : v + excess);
  return Tracks{static_cast<int16_t>(v - w), static_cast<int16_t>(v + w)};
}

Tracks capLite(Tracks t, uint8_t cap_pct, uint16_t dist_mm, uint8_t stop_cm, uint8_t slow_cm, bool brake_on,
               bool& capped) {
  capped = false;
  const int16_t m = absI(t.l) > absI(t.r) ? absI(t.l) : absI(t.r);
  if (m > cap_pct && m > 0) {
    t.l = static_cast<int16_t>(static_cast<int32_t>(t.l) * cap_pct / m);
    t.r = static_cast<int16_t>(static_cast<int32_t>(t.r) * cap_pct / m);
    capped = true;
  }
  const uint16_t stop_mm = static_cast<uint16_t>(stop_cm) * 10u;
  const uint16_t slow_mm = static_cast<uint16_t>(slow_cm) * 10u;
  if (brake_on && t.l + t.r > 0 && dist_mm < 0xFFFE && dist_mm < slow_mm && slow_mm > stop_mm) {
    const int32_t f = dist_mm <= stop_mm ? 0 : static_cast<int32_t>(dist_mm - stop_mm) * 1000 / (slow_mm - stop_mm);
    t.l = static_cast<int16_t>(t.l * f / 1000);
    t.r = static_cast<int16_t>(t.r * f / 1000);
  }
  return t;
}

int16_t RampLite::step(int16_t target_pct, uint16_t accel_pct_s) {
  const int16_t target = static_cast<int16_t>(target_pct * 10);
  const bool same_sign = m_pm == 0 || (target >= 0) == (m_pm > 0);
  const bool accelerating = same_sign && absI(target) > absI(m_pm);
  int16_t max_step = static_cast<int16_t>((accelerating ? accel_pct_s : accel_pct_s * 2u) / 10u);
  if (max_step < 1) max_step = 1;
  const int16_t diff = static_cast<int16_t>(target - m_pm);
  m_pm = static_cast<int16_t>(m_pm + clampI(diff, static_cast<int16_t>(-max_step), max_step));
  return m_pm;
}

int16_t toPwmLite(int16_t pm, bool moving, uint8_t start_pwm, uint8_t keep_pwm) {
  const int16_t mag = absI(pm);
  if (mag < 5) return 0;
  const int16_t base = moving ? keep_pwm : start_pwm;
  int32_t pwm = base + static_cast<int32_t>(mag > 1000 ? 1000 : mag) * (255 - base) / 1000;
  if (pwm > 255) pwm = 255;
  return static_cast<int16_t>(pm < 0 ? -pwm : pwm);
}

}  // namespace cq
```

- [ ] **Step 4: Run, expect PASS (24 tests).**
- [ ] **Step 5: Commit** `feat(uno-lite): integer drive mixing, caps, ramp and deadband`.

---

### Task 6: TiltLite — `test_lite_sensors`

**Files:** Create `firmware/uno/src/domain/lite/tilt_lite.h`, `.cpp`; Test: `firmware/uno/test/test_lite_sensors/test_main.cpp` (new suite).

**Interfaces:**
- Produces: `class TiltLite { enum class Change : uint8_t { kNone, kLifted, kLanded, kTilted, kLevel }; Change update(int16_t ax, int16_t ay, int16_t az, uint8_t limit_deg, uint32_t now_ms); bool lifted() const; bool tilted() const; }` — raw accel, 8192 per g.

- [ ] **Step 1: Failing tests.** Create `test/test_lite_sensors/test_main.cpp`:

```cpp
#include <unity.h>

#include "domain/lite/tilt_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
// Feeds n samples every 5 ms of a constant accel vector; returns the last Change seen that was not kNone.
TiltLite::Change feedA(TiltLite& t, int16_t ax, int16_t ay, int16_t az, int n, uint32_t& now) {
  TiltLite::Change last = TiltLite::Change::kNone;
  for (int i = 0; i < n; ++i, now += 5) {
    const TiltLite::Change c = t.update(ax, ay, az, 45, now);
    if (c != TiltLite::Change::kNone) last = c;
  }
  return last;
}
}  // namespace

void test_tilt_level_stays_level() {
  TiltLite t;
  uint32_t now = 0;
  feedA(t, 0, 0, 8192, 200, now);
  TEST_ASSERT_FALSE(t.tilted());
  TEST_ASSERT_FALSE(t.lifted());
}

void test_tilt_over_limit_needs_300ms() {
  TiltLite t;
  uint32_t now = 0;
  feedA(t, 0, 0, 8192, 100, now);
  feedA(t, 6554, 0, 4588, 40, now);  // ≈ 55° nose up, 200 ms (filter settles in ~40 ms)
  TEST_ASSERT_FALSE(t.tilted());
  TEST_ASSERT_EQUAL(TiltLite::Change::kTilted, feedA(t, 6554, 0, 4588, 40, now));
  TEST_ASSERT_TRUE(t.tilted());
  TEST_ASSERT_EQUAL(TiltLite::Change::kLevel, feedA(t, 0, 0, 8192, 40, now));  // back level (< 40°)
}

void test_tilt_upside_down_is_tilted() {
  TiltLite t;
  uint32_t now = 0;
  feedA(t, 0, 0, -8192, 200, now);
  TEST_ASSERT_TRUE(t.tilted());
}

void test_lift_and_land() {
  TiltLite t;
  uint32_t now = 0;
  feedA(t, 0, 0, 8192, 100, now);
  TEST_ASSERT_EQUAL(TiltLite::Change::kLifted, feedA(t, 0, 0, 2000, 40, now));  // |az| < 0.6 g for 60 ms
  TEST_ASSERT_TRUE(t.lifted());
  TEST_ASSERT_EQUAL(TiltLite::Change::kLanded, feedA(t, 0, 0, 8192, 200, now));  // 1 g for 500 ms
  TEST_ASSERT_FALSE(t.lifted());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tilt_level_stays_level);
  RUN_TEST(test_tilt_over_limit_needs_300ms);
  RUN_TEST(test_tilt_upside_down_is_tilted);
  RUN_TEST(test_lift_and_land);
  // LITE-SENSORS-TESTS-END
  return UNITY_END();
}
```

- [ ] **Step 2: Run, expect compile failure.** `~/.platformio/penv/bin/pio test -e native -f test_lite_sensors`
- [ ] **Step 3: Implement.** `tilt_lite.h`:

```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Lift and tilt from the accelerometer only (Lite has no attitude filter). Accel is low-passed (≈ 40 ms) and
// scaled to 128 per g. Tilted: ax² + ay² > az² × tan²(limit) (or az ≤ 0) for 300 ms; level again below limit − 5°.
// Lifted: |az| < 0.6 g for 60 ms; landed: 0.85..1.15 g for 500 ms (full LiftDetector thresholds).
class TiltLite {
 public:
  enum class Change : uint8_t { kNone, kLifted, kLanded, kTilted, kLevel };
  static constexpr uint32_t kTiltHoldMs = 300;
  Change update(int16_t ax, int16_t ay, int16_t az, uint8_t limit_deg, uint32_t now_ms);
  bool lifted() const { return m_lifted; }
  bool tilted() const { return m_tilted; }

 private:
  bool over(uint8_t deg) const;
  int16_t m_fx = 0, m_fy = 0, m_fz = 8192;  // filtered raw accel
  bool m_lifted = false;
  bool m_tilted = false;
  bool m_over = false;
  uint32_t m_over_since_ms = 0;
  bool m_cond = false;
  uint32_t m_cond_since_ms = 0;
};

}  // namespace cq
```

`tilt_lite.cpp`:

```cpp
#include "domain/lite/tilt_lite.h"

namespace cq {

namespace {
// tan²(5°·i) × 256 for i = 0..13 (0°..65°).
const uint16_t kTan2Q8[14] = {0, 2, 8, 18, 34, 56, 85, 126, 180, 256, 364, 522, 768, 1177};
uint16_t tan2Q8(uint8_t deg) {
  uint8_t i = static_cast<uint8_t>(deg / 5);
  if (i > 13) i = 13;
  return kTan2Q8[i];
}
}  // namespace

bool TiltLite::over(uint8_t deg) const {
  if (m_fz <= 0) return true;  // past 90° or upside down
  const int32_t x = m_fx / 64, y = m_fy / 64, z = m_fz / 64;  // 128 per g
  const uint32_t h2 = static_cast<uint32_t>(x * x + y * y);
  const uint32_t v2 = static_cast<uint32_t>(z * z);
  return h2 * 256u > v2 * tan2Q8(deg);
}

TiltLite::Change TiltLite::update(int16_t ax, int16_t ay, int16_t az, uint8_t limit_deg, uint32_t now_ms) {
  m_fx = static_cast<int16_t>(m_fx + (ax - m_fx) / 8);
  m_fy = static_cast<int16_t>(m_fy + (ay - m_fy) / 8);
  m_fz = static_cast<int16_t>(m_fz + (az - m_fz) / 8);
  if (!m_tilted) {
    if (over(limit_deg)) {
      if (!m_over) {
        m_over = true;
        m_over_since_ms = now_ms;
      }
      if (static_cast<uint32_t>(now_ms - m_over_since_ms) >= kTiltHoldMs) {
        m_tilted = true;
        m_over = false;
        return Change::kTilted;
      }
    } else {
      m_over = false;
    }
  } else if (!over(static_cast<uint8_t>(limit_deg > 5 ? limit_deg - 5 : 0))) {
    m_tilted = false;
    return Change::kLevel;
  }
  const int16_t a = m_fz < 0 ? static_cast<int16_t>(-m_fz) : m_fz;
  const bool cond = m_lifted ? (m_fz > 6963 && m_fz < 9421) : (a < 4915);
  if (cond != m_cond) {
    m_cond = cond;
    m_cond_since_ms = now_ms;
  }
  if (!cond) return Change::kNone;
  const uint32_t hold_ms = m_lifted ? 500u : 60u;
  if (static_cast<uint32_t>(now_ms - m_cond_since_ms) < hold_ms) return Change::kNone;
  m_lifted = !m_lifted;
  m_cond = false;
  return m_lifted ? Change::kLifted : Change::kLanded;
}

}  // namespace cq
```

- [ ] **Step 4: Run, expect PASS (4 tests).** If `test_tilt_over_limit_needs_300ms` fails only on the filter settling time, adjust the first feed count in the test (the 300 ms hold is the requirement, not the exact sample count) and record a `Ruling:` line in the ledger.
- [ ] **Step 5: Commit** `feat(uno-lite): accelerometer lift and tilt detection (integer)`.

---

### Task 7: RangeLite — `test_lite_sensors`

**Files:** Create `firmware/uno/src/domain/lite/range_lite.h`, `.cpp`; modify the sensors test.

**Interfaces:**
- Consumes: `EchoState` from `types/hw_types.h`.
- Produces: `class RangeLite { static constexpr uint16_t kClear = 0xFFFF, kFault = 0xFFFE; enum class Change : uint8_t { kNone, kFault, kRecovered }; Change onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm); uint16_t distMm() const; bool fault() const; }`

- [ ] **Step 1: Failing tests:**

```cpp
#include "domain/lite/range_lite.h"

void test_range_median_of_three() {
  RangeLite r;
  r.onReading(EchoState::kReady, 1166, 0);  // 200 mm
  r.onReading(EchoState::kReady, 5831, 0);  // 1000 mm (spike)
  r.onReading(EchoState::kReady, 1166, 0);
  TEST_ASSERT_UINT16_WITHIN(1, 200, r.distMm());
}

void test_range_fault_after_three_failures_and_recovers() {
  RangeLite r;
  r.onReading(EchoState::kNoEcho, 0, 0);
  r.onReading(EchoState::kNoEcho, 0, 0);
  TEST_ASSERT_EQUAL(RangeLite::Change::kFault, r.onReading(EchoState::kNoEcho, 0, 0));
  TEST_ASSERT_EQUAL_UINT16(RangeLite::kFault, r.distMm());
  TEST_ASSERT_EQUAL(RangeLite::Change::kRecovered, r.onReading(EchoState::kClear, 0, 0));
}

void test_range_clear_never_faults() {
  RangeLite r;
  for (int i = 0; i < 10; ++i) r.onReading(EchoState::kClear, 0, 0);
  TEST_ASSERT_FALSE(r.fault());
  TEST_ASSERT_EQUAL_UINT16(RangeLite::kClear, r.distMm());
}
```

- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement.** `range_lite.h`:

```cpp
#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {

// Integer copy of the full UltrasonicFilter without the closing speed: median of 3, offset, clear (> 4 m),
// fault after 3 failed measurements or > 5 skipped pings (echo stuck high); kClear never faults (LLD §9.16).
class RangeLite {
 public:
  static constexpr uint16_t kClear = 0xFFFF;
  static constexpr uint16_t kFault = 0xFFFE;
  static constexpr uint16_t kMaxMm = 4000;
  enum class Change : uint8_t { kNone, kFault, kRecovered };
  Change onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm);
  uint16_t distMm() const { return m_fault ? kFault : m_dist_mm; }
  bool fault() const { return m_fault; }

 private:
  Change countFailure();
  uint16_t m_hist[3] = {kClear, kClear, kClear};
  uint8_t m_idx = 0;
  uint16_t m_dist_mm = kClear;
  uint8_t m_failures = 0;
  uint8_t m_pending = 0;
  bool m_fault = false;
};

}  // namespace cq
```

`range_lite.cpp`:

```cpp
#include "domain/lite/range_lite.h"

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

RangeLite::Change RangeLite::countFailure() {
  if (m_failures < 255) ++m_failures;
  if (m_failures == 3 && !m_fault) {
    m_fault = true;
    return Change::kFault;
  }
  return Change::kNone;
}

RangeLite::Change RangeLite::onReading(EchoState st, uint16_t echo_us, uint8_t offset_mm) {
  if (st == EchoState::kPending) {
    if (m_pending < 255) ++m_pending;
    return m_pending > 5 ? countFailure() : Change::kNone;
  }
  m_pending = 0;
  if (st == EchoState::kNoEcho) return countFailure();
  m_failures = 0;
  Change change = Change::kNone;
  if (m_fault) {
    m_fault = false;
    change = Change::kRecovered;
  }
  uint16_t mm = kClear;
  if (st == EchoState::kReady) {
    const uint32_t raw = static_cast<uint32_t>(echo_us) * 343u / 2000u;
    if (raw <= kMaxMm) mm = raw > offset_mm ? static_cast<uint16_t>(raw - offset_mm) : 0;
  }
  m_hist[m_idx] = mm;
  m_idx = static_cast<uint8_t>((m_idx + 1) % 3);
  m_dist_mm = median3(m_hist[0], m_hist[1], m_hist[2]);
  return change;
}

}  // namespace cq
```

- [ ] **Step 4: Run, expect PASS (7 tests).**
- [ ] **Step 5: Commit** `feat(uno-lite): integer ultrasonic range filter`.

---

### Task 8: EncoderLite — `test_lite_sensors`

**Files:** Create `firmware/uno/src/domain/lite/encoder_lite.h`, `.cpp`; modify the sensors test.

**Interfaces:**
- Produces: `class EncoderLite { static constexpr uint32_t kStaleUs = 100000; void update(int32_t count, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us); int32_t count() const; uint16_t cps() const; bool moving() const; }` and `int8_t speedPctLite(uint16_t cps, int16_t pwm_sign_source, uint16_t max_cps, bool enabled)`.

- [ ] **Step 1: Failing tests:**

```cpp
#include "domain/lite/encoder_lite.h"

void test_encoder_cps_from_period() {
  EncoderLite e;
  e.update(100, 1000, 50000, 60000);  // 1 ms between edges → 1000 counts/s, last edge 10 ms ago
  TEST_ASSERT_EQUAL_UINT16(1000, e.cps());
  TEST_ASSERT_TRUE(e.moving());
  TEST_ASSERT_EQUAL_INT32(100, e.count());
}

void test_encoder_stale_is_stopped() {
  EncoderLite e;
  e.update(100, 1000, 0, 150000);  // no edge for 150 ms
  TEST_ASSERT_EQUAL_UINT16(0, e.cps());
  TEST_ASSERT_FALSE(e.moving());
}

void test_encoder_speed_pct_sign_and_disabled() {
  TEST_ASSERT_EQUAL_INT8(40, speedPctLite(1000, 120, 2500, true));
  TEST_ASSERT_EQUAL_INT8(-40, speedPctLite(1000, -120, 2500, true));
  TEST_ASSERT_EQUAL_INT8(-128, speedPctLite(1000, 120, 2500, false));  // "unknown", as the full firmware
}
```

- [ ] **Step 2: Run, expect compile failure.**
- [ ] **Step 3: Implement.** `encoder_lite.h`:

```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Single-channel encoder (rising edges only): speed from the period between the last two edges; stopped when
// no edge for 100 ms. Direction comes from the motor command (no quadrature yet).
class EncoderLite {
 public:
  static constexpr uint32_t kStaleUs = 100000;
  void update(int32_t count, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us);
  int32_t count() const { return m_count; }
  uint16_t cps() const { return m_cps; }
  bool moving() const { return m_cps != 0; }

 private:
  int32_t m_count = 0;
  uint16_t m_cps = 0;
};

// Telemetry spd_act: cps as % of max_cps with the command's sign; −128 = unknown (encoders off).
int8_t speedPctLite(uint16_t cps, int16_t pwm_sign_source, uint16_t max_cps, bool enabled);

}  // namespace cq
```

`encoder_lite.cpp`:

```cpp
#include "domain/lite/encoder_lite.h"

namespace cq {

void EncoderLite::update(int32_t count, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us) {
  m_count = count;
  if (period_us == 0 || static_cast<uint32_t>(now_us - last_edge_us) > kStaleUs) {
    m_cps = 0;
    return;
  }
  const uint32_t cps = 1000000UL / period_us;
  m_cps = static_cast<uint16_t>(cps > 65535UL ? 65535UL : cps);
}

int8_t speedPctLite(uint16_t cps, int16_t pwm_sign_source, uint16_t max_cps, bool enabled) {
  if (!enabled || max_cps == 0) return -128;
  int32_t pct = static_cast<int32_t>(cps) * 100 / max_cps;
  if (pct > 127) pct = 127;
  return static_cast<int8_t>(pwm_sign_source < 0 ? -pct : pct);
}

}  // namespace cq
```

- [ ] **Step 4: Run, expect PASS (10 tests).**
- [ ] **Step 5: Commit** `feat(uno-lite): single-channel encoder speed (integer)`.

---

### Task 9: SettingsLite — `test_lite_settings`

**Files:** Create `firmware/uno/src/domain/lite/settings_lite.h`, `.cpp`; Test: `firmware/uno/test/test_lite_settings/test_main.cpp`.

**Interfaces:**
- Consumes: `proto::SettingId`, `proto::AckStatus`, `proto::crc8` (generated).
- Produces: `struct LiteSettings { int16_t gyro_bias_z; uint8_t db_start[4]; uint8_t db_keep[4]; uint16_t heading_kp_milli; uint16_t rate_kp_milli; uint16_t accel_limit; uint8_t turn_full; uint8_t obstacle_stop; uint8_t obstacle_slow; uint8_t reverse_steer; uint8_t encoders_enabled; };` (deadband index: 0 l_fwd, 1 l_rev, 2 r_fwd, 3 r_rev) and `class SettingsLite { static constexpr uint16_t kAddr = 0; static constexpr uint8_t kBlockSize; void defaults(); bool load(uint8_t (*read)(uint16_t)); bool get(uint8_t id, int32_t& v) const; proto::AckStatus set(uint8_t id, int32_t v); uint8_t encode(uint8_t* out) const; const LiteSettings& values() const; }`

- [ ] **Step 1: Failing tests.** Create `test/test_lite_settings/test_main.cpp`:

```cpp
#include <string.h>
#include <unity.h>

#include "domain/lite/settings_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
uint8_t g_mem[64];
uint8_t rd(uint16_t a) { return g_mem[a]; }
uint8_t id(proto::SettingId s) { return static_cast<uint8_t>(s); }
}  // namespace

void test_settings_defaults() {
  SettingsLite s;
  s.defaults();
  TEST_ASSERT_EQUAL_UINT8(110, s.values().db_start[0]);
  TEST_ASSERT_EQUAL_UINT8(90, s.values().db_keep[3]);
  TEST_ASSERT_EQUAL_UINT16(3500, s.values().heading_kp_milli);
  TEST_ASSERT_EQUAL_UINT16(1200, s.values().rate_kp_milli);
  TEST_ASSERT_EQUAL_UINT16(200, s.values().accel_limit);
  TEST_ASSERT_EQUAL_UINT8(40, s.values().turn_full);
  TEST_ASSERT_EQUAL_UINT8(15, s.values().obstacle_stop);
  TEST_ASSERT_EQUAL_UINT8(50, s.values().obstacle_slow);
}

void test_settings_set_get_and_range() {
  SettingsLite s;
  s.defaults();
  TEST_ASSERT_EQUAL(proto::AckStatus::kOk, s.set(id(proto::SettingId::kGyroBiasZ), -17));
  int32_t v = 0;
  TEST_ASSERT_TRUE(s.get(id(proto::SettingId::kGyroBiasZ), v));
  TEST_ASSERT_EQUAL_INT32(-17, v);
  TEST_ASSERT_EQUAL(proto::AckStatus::kSettingOutOfRange, s.set(id(proto::SettingId::kObstacleStop), 4));
  TEST_ASSERT_EQUAL(proto::AckStatus::kOk, s.set(id(proto::SettingId::kDeadbandStartRRev), 120));
  TEST_ASSERT_EQUAL_UINT8(120, s.values().db_start[3]);
}

void test_settings_unknown_id_is_unavailable() {
  SettingsLite s;
  s.defaults();
  TEST_ASSERT_EQUAL(proto::AckStatus::kUnavailable, s.set(id(proto::SettingId::kServoSpeed), 100));
  int32_t v = 0;
  TEST_ASSERT_FALSE(s.get(id(proto::SettingId::kServoSpeed), v));
}

void test_settings_round_trip_through_eeprom_block() {
  SettingsLite a;
  a.defaults();
  a.set(id(proto::SettingId::kPidHeadingKp), 4000);
  memset(g_mem, 0xFF, sizeof g_mem);
  const uint8_t n = a.encode(g_mem + SettingsLite::kAddr);
  TEST_ASSERT_EQUAL_UINT8(SettingsLite::kBlockSize, n);
  SettingsLite b;
  TEST_ASSERT_TRUE(b.load(rd));
  TEST_ASSERT_EQUAL_UINT16(4000, b.values().heading_kp_milli);
}

void test_settings_garbage_eeprom_gives_defaults() {
  SettingsLite s;
  memset(g_mem, 0xA5, sizeof g_mem);  // someone else's bytes (ELEGOO / full firmware layout)
  TEST_ASSERT_FALSE(s.load(rd));
  TEST_ASSERT_EQUAL_UINT16(3500, s.values().heading_kp_milli);
  SettingsLite good;
  good.defaults();
  good.encode(g_mem);
  g_mem[6] ^= 0x01;  // one flipped bit in the data → CRC fails → defaults
  TEST_ASSERT_FALSE(s.load(rd));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_settings_defaults);
  RUN_TEST(test_settings_set_get_and_range);
  RUN_TEST(test_settings_unknown_id_is_unavailable);
  RUN_TEST(test_settings_round_trip_through_eeprom_block);
  RUN_TEST(test_settings_garbage_eeprom_gives_defaults);
  return UNITY_END();
}
```

- [ ] **Step 2: Run, expect compile failure.** `~/.platformio/penv/bin/pio test -e native -f test_lite_settings`
- [ ] **Step 3: Implement.** `settings_lite.h`:

```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

struct LiteSettings {
  int16_t gyro_bias_z;
  uint8_t db_start[4];  // l_fwd, l_rev, r_fwd, r_rev
  uint8_t db_keep[4];
  uint16_t heading_kp_milli;
  uint16_t rate_kp_milli;
  uint16_t accel_limit;
  uint8_t turn_full;
  uint8_t obstacle_stop;
  uint8_t obstacle_slow;
  uint8_t reverse_steer;
  uint8_t encoders_enabled;
};

// The Lite settings (spec 02 Part D): existing SettingId numbers, so the bridge and bench tools work unchanged.
// EEPROM block at kAddr: magic "L1" (2 B), payload size (1 B), payload, CRC-8 of size + payload.
// Any other id → UNAVAILABLE.
class SettingsLite {
 public:
  static constexpr uint16_t kAddr = 0;
  static constexpr uint8_t kMagic0 = 'L';
  static constexpr uint8_t kMagic1 = '1';
  static constexpr uint8_t kBlockSize = static_cast<uint8_t>(4 + sizeof(LiteSettings));
  void defaults();
  bool load(uint8_t (*read)(uint16_t addr));  // false → defaults in use
  bool get(uint8_t id, int32_t& v) const;
  proto::AckStatus set(uint8_t id, int32_t v);
  uint8_t encode(uint8_t* out) const;
  const LiteSettings& values() const { return m_s; }

 private:
  LiteSettings m_s{};
};

}  // namespace cq
```

`settings_lite.cpp`:

```cpp
#include "domain/lite/settings_lite.h"

#include <string.h>

namespace cq {

namespace {
using Id = proto::SettingId;
bool inRange(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }
}  // namespace

void SettingsLite::defaults() {
  m_s.gyro_bias_z = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    m_s.db_start[i] = 110;
    m_s.db_keep[i] = 90;
  }
  m_s.heading_kp_milli = 3500;
  m_s.rate_kp_milli = 1200;
  m_s.accel_limit = 200;
  m_s.turn_full = 40;
  m_s.obstacle_stop = 15;
  m_s.obstacle_slow = 50;
  m_s.reverse_steer = 0;
  m_s.encoders_enabled = 0;
}

bool SettingsLite::load(uint8_t (*read)(uint16_t)) {
  defaults();
  if (read(kAddr) != kMagic0 || read(kAddr + 1) != kMagic1 || read(kAddr + 2) != sizeof(LiteSettings)) return false;
  uint8_t buf[sizeof(LiteSettings) + 1];
  buf[0] = sizeof(LiteSettings);
  for (uint8_t i = 0; i < sizeof(LiteSettings); ++i) buf[i + 1] = read(static_cast<uint16_t>(kAddr + 3 + i));
  if (proto::crc8(buf, sizeof buf) != read(static_cast<uint16_t>(kAddr + 3 + sizeof(LiteSettings)))) return false;
  memcpy(&m_s, buf + 1, sizeof(LiteSettings));
  return true;
}

uint8_t SettingsLite::encode(uint8_t* out) const {
  out[0] = kMagic0;
  out[1] = kMagic1;
  out[2] = sizeof(LiteSettings);
  memcpy(out + 3, &m_s, sizeof(LiteSettings));
  out[3 + sizeof(LiteSettings)] = proto::crc8(out + 2, static_cast<uint8_t>(sizeof(LiteSettings) + 1));
  return kBlockSize;
}

bool SettingsLite::get(uint8_t id, int32_t& v) const {
  const uint8_t db = static_cast<uint8_t>(Id::kDeadbandStartLFwd);
  const uint8_t dk = static_cast<uint8_t>(Id::kDeadbandKeepLFwd);
  if (id >= db && id < db + 4) { v = m_s.db_start[id - db]; return true; }
  if (id >= dk && id < dk + 4) { v = m_s.db_keep[id - dk]; return true; }
  switch (static_cast<Id>(id)) {
    case Id::kGyroBiasZ: v = m_s.gyro_bias_z; return true;
    case Id::kPidHeadingKp: v = m_s.heading_kp_milli; return true;
    case Id::kPidRateKp: v = m_s.rate_kp_milli; return true;
    case Id::kAccelLimit: v = m_s.accel_limit; return true;
    case Id::kTurnRateFullSpeed: v = m_s.turn_full; return true;
    case Id::kObstacleStop: v = m_s.obstacle_stop; return true;
    case Id::kObstacleSlow: v = m_s.obstacle_slow; return true;
    case Id::kReverseSteer: v = m_s.reverse_steer; return true;
    case Id::kEncodersEnabled: v = m_s.encoders_enabled; return true;
    default: return false;
  }
}

proto::AckStatus SettingsLite::set(uint8_t id, int32_t v) {
  constexpr proto::AckStatus kOk = proto::AckStatus::kOk;
  constexpr proto::AckStatus kRange = proto::AckStatus::kSettingOutOfRange;
  const uint8_t db = static_cast<uint8_t>(Id::kDeadbandStartLFwd);
  const uint8_t dk = static_cast<uint8_t>(Id::kDeadbandKeepLFwd);
  if (id >= db && id < db + 4) {
    if (!inRange(v, 0, 255)) return kRange;
    m_s.db_start[id - db] = static_cast<uint8_t>(v);
    return kOk;
  }
  if (id >= dk && id < dk + 4) {
    if (!inRange(v, 0, 255)) return kRange;
    m_s.db_keep[id - dk] = static_cast<uint8_t>(v);
    return kOk;
  }
  switch (static_cast<Id>(id)) {
    case Id::kGyroBiasZ: if (!inRange(v, -2000, 2000)) return kRange; m_s.gyro_bias_z = static_cast<int16_t>(v); return kOk;
    case Id::kPidHeadingKp: if (!inRange(v, 0, 60000)) return kRange; m_s.heading_kp_milli = static_cast<uint16_t>(v); return kOk;
    case Id::kPidRateKp: if (!inRange(v, 0, 60000)) return kRange; m_s.rate_kp_milli = static_cast<uint16_t>(v); return kOk;
    case Id::kAccelLimit: if (!inRange(v, 10, 1000)) return kRange; m_s.accel_limit = static_cast<uint16_t>(v); return kOk;
    case Id::kTurnRateFullSpeed: if (!inRange(v, 10, 100)) return kRange; m_s.turn_full = static_cast<uint8_t>(v); return kOk;
    case Id::kObstacleStop: if (!inRange(v, 5, 100)) return kRange; m_s.obstacle_stop = static_cast<uint8_t>(v); return kOk;
    case Id::kObstacleSlow: if (!inRange(v, 10, 200)) return kRange; m_s.obstacle_slow = static_cast<uint8_t>(v); return kOk;
    case Id::kReverseSteer: if (!inRange(v, 0, 1)) return kRange; m_s.reverse_steer = static_cast<uint8_t>(v); return kOk;
    case Id::kEncodersEnabled: if (!inRange(v, 0, 1)) return kRange; m_s.encoders_enabled = static_cast<uint8_t>(v); return kOk;
    default: return proto::AckStatus::kUnavailable;
  }
}

}  // namespace cq
```

(If a `SettingId` name differs from the generated header — check with `grep -n "kDeadbandStartLFwd\|kPidRateKp\|kTurnRateFullSpeed" src/generated/cq_protocol.h` — use the generated name and record a `Ruling:`.)

- [ ] **Step 4: Run, expect PASS (5 tests).**
- [ ] **Step 5: Commit** `feat(uno-lite): small settings set with a CRC-checked EEPROM block`.

---

### Task 10: AnnunciatorLite, Beeper and CamLite — `test_lite_ui`

**Files:** Create `firmware/uno/src/domain/lite/annunciator_lite.h/.cpp`, `beeper.h/.cpp`, `cam_lite.h/.cpp`; Test: `firmware/uno/test/test_lite_ui/test_main.cpp`.

**Interfaces:**
- Consumes: `LedSequencer`, `LedPattern`, `LedLayer`, `Rgb`, `proto::Owner`.
- Produces:
  - `struct LiteUiState { bool synced; proto::Owner owner; bool driving; uint8_t speed_pct; bool locked; bool tilted_or_lifted; bool batt_crit; bool batt_low; bool blocked; };`
  - `class AnnunciatorLite { static constexpr Rgb kNoLink{0,0,120}, kReady{20,20,20}, kHuman{0,120,0}, kIr{80,0,120}, kAlarm{255,0,0}, kLow{200,110,0}; void update(const LiteUiState&, uint32_t now, LedSequencer&); }`
  - `class Beeper { void beep(uint16_t hz, uint16_t ms, uint32_t now); uint16_t tick(uint32_t now); /* hz to play, 0 = silent */ }`
  - `class CamLite { static constexpr int16_t kHome = 900; void set(int16_t pan_dd, int16_t tilt_dd); void nudge(int16_t dpan_dd, int16_t dtilt_dd); void joy(int8_t x, int8_t y, bool active, uint32_t now); void home(); void lock(bool on); void tick(uint32_t now); int16_t pan() const; int16_t tilt() const; static uint16_t toUs(int16_t dd); }` (deci-degrees; pan 0–1800, tilt 300–1100)

- [ ] **Step 1: Failing tests.** Create `test/test_lite_ui/test_main.cpp`:

```cpp
#include <unity.h>

#include "domain/lite/annunciator_lite.h"
#include "domain/lite/beeper.h"
#include "domain/lite/cam_lite.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {
LiteUiState ready() {
  LiteUiState s{};
  s.synced = true;
  s.owner = proto::Owner::kNone;
  s.driving = true;  // steady base at full level (no breathing) for exact colours
  s.speed_pct = 100;
  return s;
}
}  // namespace

void test_ann_base_colours() {
  AnnunciatorLite a;
  LedSequencer l;
  LiteUiState s = ready();
  a.update(s, 0, l);
  TEST_ASSERT_TRUE((l.output(0) == AnnunciatorLite::kReady));
  s.synced = false;
  a.update(s, 10, l);
  TEST_ASSERT_TRUE((l.output(10) == AnnunciatorLite::kNoLink));
  s.owner = proto::Owner::kIr;
  a.update(s, 20, l);
  TEST_ASSERT_TRUE((l.output(20) == AnnunciatorLite::kIr));
}

void test_ann_alarm_priority() {
  AnnunciatorLite a;
  LedSequencer l;
  LiteUiState s = ready();
  s.locked = true;
  a.update(s, 0, l);
  TEST_ASSERT_TRUE((l.output(500) == AnnunciatorLite::kAlarm));  // solid
  s.locked = false;
  s.batt_low = true;
  a.update(s, 1000, l);
  TEST_ASSERT_TRUE((l.output(1050) == AnnunciatorLite::kLow));    // flash
  TEST_ASSERT_TRUE((l.output(1500) == AnnunciatorLite::kReady));  // base between flashes
}

void test_beeper_plays_then_stops() {
  Beeper b;
  b.beep(1500, 80, 0);
  TEST_ASSERT_EQUAL_UINT16(1500, b.tick(10));
  TEST_ASSERT_EQUAL_UINT16(0, b.tick(80));
}

void test_cam_limits_and_us() {
  CamLite c;
  c.set(2000, 0);
  TEST_ASSERT_EQUAL_INT16(1800, c.pan());
  TEST_ASSERT_EQUAL_INT16(300, c.tilt());
  TEST_ASSERT_EQUAL_UINT16(544, CamLite::toUs(0));
  TEST_ASSERT_EQUAL_UINT16(2400, CamLite::toUs(1800));
  TEST_ASSERT_EQUAL_UINT16(1472, CamLite::toUs(900));
}

void test_cam_joy_release_returns_home_unless_locked() {
  CamLite c;
  c.joy(100, 0, true, 0);             // full right = 90° lower
  TEST_ASSERT_EQUAL_INT16(0, c.pan());
  c.joy(0, 0, false, 100);
  c.tick(599);
  TEST_ASSERT_EQUAL_INT16(0, c.pan());
  c.tick(600);                         // 500 ms after release
  TEST_ASSERT_EQUAL_INT16(900, c.pan());
  c.lock(true);
  c.joy(100, 0, true, 700);
  c.joy(0, 0, false, 800);
  c.tick(2000);
  TEST_ASSERT_EQUAL_INT16(0, c.pan());  // locked: stays
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ann_base_colours);
  RUN_TEST(test_ann_alarm_priority);
  RUN_TEST(test_beeper_plays_then_stops);
  RUN_TEST(test_cam_limits_and_us);
  RUN_TEST(test_cam_joy_release_returns_home_unless_locked);
  return UNITY_END();
}
```

- [ ] **Step 2: Run, expect compile failure.** `~/.platformio/penv/bin/pio test -e native -f test_lite_ui`
- [ ] **Step 3: Implement.** `annunciator_lite.h`:

```cpp
#pragma once
#include <stdint.h>

#include "domain/ui/led_sequencer.h"
#include "generated/cq_protocol.h"

namespace cq {

struct LiteUiState {
  bool synced;
  proto::Owner owner;
  bool driving;
  uint8_t speed_pct;
  bool locked;            // self-test lock: solid red
  bool tilted_or_lifted;  // red blink 250 ms
  bool batt_crit;         // red blink 1000 ms
  bool batt_low;          // amber 150 ms flash every 2 s over the base (spec 02 LED table)
  bool blocked;           // obstacle stop: fast red blink 100/100 (attention layer)
};

// Spec 02 LED table, Lite subset. Base: no ESP32 link = blue, ready = dim white, network = green, IR = purple;
// breathes when idle, brighter with speed when driving.
class AnnunciatorLite {
 public:
  static constexpr Rgb kNoLink{0, 0, 120};
  static constexpr Rgb kReady{20, 20, 20};
  static constexpr Rgb kHuman{0, 120, 0};
  static constexpr Rgb kIr{80, 0, 120};
  static constexpr Rgb kAlarm{255, 0, 0};
  static constexpr Rgb kLow{200, 110, 0};
  void update(const LiteUiState& s, uint32_t now_ms, LedSequencer& led);

 private:
  LiteUiState m_prev{};
  bool m_first = true;
};

}  // namespace cq
```

`annunciator_lite.cpp`:

```cpp
#include "domain/lite/annunciator_lite.h"

namespace cq {

void AnnunciatorLite::update(const LiteUiState& s, uint32_t now, LedSequencer& led) {
  const LiteUiState& p = m_prev;
  if (m_first || s.locked != p.locked || s.tilted_or_lifted != p.tilted_or_lifted || s.batt_crit != p.batt_crit ||
      s.batt_low != p.batt_low) {
    if (s.locked) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kSolid, kAlarm, 0, 0}, now);
    } else if (s.tilted_or_lifted) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kAlarm, 250, 0}, now);
    } else if (s.batt_crit) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kAlarm, 1000, 0}, now);
    } else if (s.batt_low) {
      led.set(LedLayer::kAlarm, {proto::LedMode::kBlink, kLow, 2000, 0, 150}, now);
    } else {
      led.clear(LedLayer::kAlarm);
    }
  }
  if (m_first || s.blocked != p.blocked) {
    if (s.blocked) {
      led.set(LedLayer::kAttention, {proto::LedMode::kBlink, kAlarm, 200, 0}, now);
    } else {
      led.clear(LedLayer::kAttention);
    }
  }
  if (m_first || s.owner != p.owner || s.synced != p.synced) {
    Rgb c = kReady;
    if (s.owner == proto::Owner::kIr) {
      c = kIr;
    } else if (s.owner == proto::Owner::kNetwork) {
      c = kHuman;
    } else if (!s.synced) {
      c = kNoLink;
    }
    led.set(LedLayer::kBase, {proto::LedMode::kSolid, c, 0, 0}, now);
  }
  const uint8_t speed = s.speed_pct > 100 ? 100 : s.speed_pct;
  led.setBaseStyle(!s.driving, s.batt_crit ? 1000 : 2000,
                   static_cast<uint8_t>(LedSequencer::kMinLevelPct + (100u - LedSequencer::kMinLevelPct) * speed / 100u));
  m_prev = s;
  m_first = false;
}

}  // namespace cq
```

(`LedPattern::flash_ms` and the see-through flash come from branch `led-spec-colours`. **Precondition:** merge `led-spec-colours` into `uno-lite` before this task — `git merge led-spec-colours` — and record it in the ledger.)

`beeper.h`:

```cpp
#pragma once
#include <stdint.h>

namespace cq {

// One beep at a time (spec 02 Part D: no melodies in Lite). tick() returns the tone to play, 0 = silent.
class Beeper {
 public:
  void beep(uint16_t hz, uint16_t ms, uint32_t now_ms) {
    m_hz = hz;
    m_until_ms = now_ms + ms;
  }
  uint16_t tick(uint32_t now_ms) {
    if (m_hz != 0 && static_cast<int32_t>(now_ms - m_until_ms) >= 0) m_hz = 0;
    return m_hz;
  }

 private:
  uint16_t m_hz = 0;
  uint32_t m_until_ms = 0;
};

}  // namespace cq
```

(`beeper.cpp` is not needed — header only; drop it from the Files list when executing and note it.)

`cam_lite.h`:

```cpp
#pragma once
#include <stdint.h>

namespace cq {

// Camera pan/tilt in deci-degrees, instant positioning (smooth motion is an optional extra, LLD §8).
// Pan 0..1800 (x > 0 = right = lower angle), tilt 300..1100, home 900/900. Joystick release → home after 500 ms
// unless locked.
class CamLite {
 public:
  static constexpr int16_t kHome = 900;
  static constexpr int16_t kPanMin = 0, kPanMax = 1800, kTiltMin = 300, kTiltMax = 1100;
  static constexpr uint16_t kReturnMs = 500;
  void set(int16_t pan_dd, int16_t tilt_dd);
  void nudge(int16_t dpan_dd, int16_t dtilt_dd) { set(static_cast<int16_t>(m_pan + dpan_dd), static_cast<int16_t>(m_tilt + dtilt_dd)); }
  void joy(int8_t x, int8_t y, bool active, uint32_t now_ms);
  void home() { set(kHome, kHome); }
  void lock(bool on) { m_locked = on; }
  void tick(uint32_t now_ms);
  int16_t pan() const { return m_pan; }
  int16_t tilt() const { return m_tilt; }
  static uint16_t toUs(int16_t dd) { return static_cast<uint16_t>(544 + static_cast<int32_t>(dd) * 1856 / 1800); }

 private:
  int16_t m_pan = kHome;
  int16_t m_tilt = kHome;
  bool m_locked = false;
  bool m_return_pending = false;
  uint32_t m_released_ms = 0;
};

}  // namespace cq
```

`cam_lite.cpp`:

```cpp
#include "domain/lite/cam_lite.h"

namespace cq {

namespace {
int16_t clampI(int16_t v, int16_t lo, int16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

void CamLite::set(int16_t pan_dd, int16_t tilt_dd) {
  m_pan = clampI(pan_dd, kPanMin, kPanMax);
  m_tilt = clampI(tilt_dd, kTiltMin, kTiltMax);
}

void CamLite::joy(int8_t x, int8_t y, bool active, uint32_t now_ms) {
  if (active) {
    m_return_pending = false;
    set(static_cast<int16_t>(kHome - x * 9), static_cast<int16_t>(kHome + y * 9));
    return;
  }
  if (!m_locked) {
    m_return_pending = true;
    m_released_ms = now_ms;
  }
}

void CamLite::tick(uint32_t now_ms) {
  if (m_return_pending && static_cast<uint32_t>(now_ms - m_released_ms) >= kReturnMs) {
    m_return_pending = false;
    home();
  }
}

}  // namespace cq
```

- [ ] **Step 4: Run, expect PASS (5 tests).**
- [ ] **Step 5: Commit** `feat(uno-lite): LED states, single beep and camera positioning`.

---

### Task 11: HELLO capability bits and the Lite frame table — `test_lite_frames`

**Files:**
- Modify: `protocol/defs/enums.yaml` (new `CapBit`), `protocol/defs/body_frames.yaml` (HELLO `caps` doc)
- Regenerate: `cd protocol && uv run protogen write`
- Modify: `firmware/uno/src/app/comms.cpp` (full app: all feature bits)
- Create: `firmware/uno/src/domain/lite/lite_frames.h`, `.cpp`
- Test: `firmware/uno/test/test_lite_frames/test_main.cpp`

**Interfaces:**
- Produces: `proto::CapBit { kEncoders = 0, kTurn = 1, kMove = 2, kDriveRate = 3, kCalibrate = 4, kOverride = 5, kAudioLed = 6, kSettingsFull = 7 }`; `namespace cq::lite_frames { enum class FrameUse : uint8_t { kHandled, kUnavailable, kUnknown }; FrameUse frameUse(uint8_t type); bool needsAck(uint8_t type); uint8_t helloCaps(bool encoders_enabled); }`

- [ ] **Step 1: Protocol change.** In `protocol/defs/enums.yaml` add after `HwBit`:

```yaml
- name: CapBit
  doc: Bit positions in HELLO.caps - features this body firmware has (the Lite Uno firmware sets only ENCODERS when on).
  values: { ENCODERS: 0, TURN: 1, MOVE: 2, DRIVE_RATE: 3, CALIBRATE: 4, OVERRIDE: 5, AUDIO_LED: 6, SETTINGS_FULL: 7 }
```

In `protocol/defs/body_frames.yaml` change the HELLO caps line to:

```yaml
    - { name: caps, type: u8, doc: "CapBit bits: features this firmware has (bit0 = encoders enabled)" }
```

Run: `cd protocol && uv run protogen write && uv run protogen check && uv run pytest -q 2>&1 | tail -1`
Expected: files regenerated, check OK, pytest passes.

- [ ] **Step 2: Full app sets every feature bit.** In `firmware/uno/src/app/comms.cpp` replace `h.caps = g_app.encoders_enabled ? 1 : 0;` with:

```cpp
    h.caps = static_cast<uint8_t>(0xFEu | (g_app.encoders_enabled ? 1u : 0u));  // full firmware: CapBit 1..7 always
```

- [ ] **Step 3: Failing tests.** Create `test/test_lite_frames/test_main.cpp`:

```cpp
#include <unity.h>

#include "domain/lite/lite_frames.h"

using namespace cq;
using lite_frames::FrameUse;

void setUp() {}
void tearDown() {}

namespace {
uint8_t t(proto::FrameType f) { return static_cast<uint8_t>(f); }
}  // namespace

void test_frames_driving_is_handled() {
  TEST_ASSERT_EQUAL(FrameUse::kHandled, lite_frames::frameUse(t(proto::FrameType::kDrive)));
  TEST_ASSERT_EQUAL(FrameUse::kHandled, lite_frames::frameUse(t(proto::FrameType::kStop)));
  TEST_ASSERT_EQUAL(FrameUse::kHandled, lite_frames::frameUse(t(proto::FrameType::kEnvelope)));
  TEST_ASSERT_EQUAL(FrameUse::kHandled, lite_frames::frameUse(t(proto::FrameType::kCamSet)));
  TEST_ASSERT_TRUE(lite_frames::needsAck(t(proto::FrameType::kStop)));
  TEST_ASSERT_FALSE(lite_frames::needsAck(t(proto::FrameType::kDrive)));
}

void test_frames_unsupported_are_unavailable() {
  const proto::FrameType gone[] = {proto::FrameType::kTurn, proto::FrameType::kMove, proto::FrameType::kDriveRate,
                                   proto::FrameType::kCancel, proto::FrameType::kCalibrate, proto::FrameType::kProfile,
                                   proto::FrameType::kSafetyOverride, proto::FrameType::kLed, proto::FrameType::kSound,
                                   proto::FrameType::kTone, proto::FrameType::kAudioMode};
  for (const proto::FrameType f : gone) {
    TEST_ASSERT_EQUAL(FrameUse::kUnavailable, lite_frames::frameUse(t(f)));
    TEST_ASSERT_TRUE(lite_frames::needsAck(t(f)));  // the sender learns why nothing happened
  }
  TEST_ASSERT_EQUAL(FrameUse::kUnknown, lite_frames::frameUse(0x7F));
}

void test_frames_hello_caps() {
  TEST_ASSERT_EQUAL_UINT8(0, lite_frames::helloCaps(false));
  TEST_ASSERT_EQUAL_UINT8(1, lite_frames::helloCaps(true));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_frames_driving_is_handled);
  RUN_TEST(test_frames_unsupported_are_unavailable);
  RUN_TEST(test_frames_hello_caps);
  return UNITY_END();
}
```

- [ ] **Step 4: Run, expect compile failure.** `~/.platformio/penv/bin/pio test -e native -f test_lite_frames`
- [ ] **Step 5: Implement.** `lite_frames.h`:

```cpp
#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {
namespace lite_frames {

enum class FrameUse : uint8_t { kHandled, kUnavailable, kUnknown };
// Spec 02 Part D / Uno Lite LLD §5: which ESP32 → Uno frames the Lite firmware implements.
FrameUse frameUse(uint8_t type);
// ACK rules as the full firmware (DRIVE, OWNER_HINT, CAM_JOY, link frames: no ACK); every unavailable frame is acked.
bool needsAck(uint8_t type);
uint8_t helloCaps(bool encoders_enabled);

}  // namespace lite_frames
}  // namespace cq
```

`lite_frames.cpp`:

```cpp
#include "domain/lite/lite_frames.h"

namespace cq {
namespace lite_frames {

using F = proto::FrameType;

FrameUse frameUse(uint8_t type) {
  switch (static_cast<F>(type)) {
    case F::kHeartbeat: case F::kHelloAck: case F::kHelloReq: case F::kEspAck:
    case F::kDrive: case F::kStop: case F::kBrake: case F::kEnvelope: case F::kOwnerHint:
    case F::kCamJoy: case F::kCamSet: case F::kCamNudge: case F::kCamHome: case F::kCamLock:
    case F::kSettingSet: case F::kSettingGet: case F::kSettingsSave:
      return FrameUse::kHandled;
    case F::kDriveRate: case F::kTurn: case F::kMove: case F::kCancel: case F::kCalibrate: case F::kProfile:
    case F::kSafetyOverride: case F::kLed: case F::kSound: case F::kTone: case F::kAudioMode:
      return FrameUse::kUnavailable;
    default:
      return FrameUse::kUnknown;
  }
}

bool needsAck(uint8_t type) {
  switch (static_cast<F>(type)) {
    case F::kHeartbeat: case F::kHelloAck: case F::kHelloReq: case F::kEspAck:
    case F::kDrive: case F::kOwnerHint: case F::kCamJoy:
      return false;
    default:
      return frameUse(type) != FrameUse::kUnknown;
  }
}

uint8_t helloCaps(bool encoders_enabled) {
  return encoders_enabled ? static_cast<uint8_t>(1u << static_cast<uint8_t>(proto::CapBit::kEncoders)) : 0;
}

}  // namespace lite_frames
}  // namespace cq
```

- [ ] **Step 6: Run the new suite (3 tests PASS) and the whole host suite.**

Run: `~/.platformio/penv/bin/pio test -e native 2>&1 | tail -1 && ~/.platformio/penv/bin/pio run -e mega | tail -1`
Expected: all suites pass; mega builds.

- [ ] **Step 7: Commit** `feat(protocol): HELLO capability bits; Lite frame table` (yaml, generated files, comms.cpp, lite_frames, test).

---

### Task 12: Lite app — routes and network motion state

**Files:**
- Create: `firmware/uno/src/lite/lite_routes.cpp`
- Modify: `firmware/uno/src/lite/lite_app.h` (state), `firmware/uno/src/lite/lite_app.cpp` (remove the skeleton `route()`), `firmware/uno/src/lite/lite_comms.cpp` (HELLO caps via `lite_frames::helloCaps`)

**Interfaces:**
- Consumes: `lite_frames::frameUse/needsAck/helloCaps`, `Arbitrator`, `Envelope`, `SettingsLite`, `CamLite`, `HeadingHoldLite`.
- Produces: `bool route(uint8_t type, const uint8_t* p, uint8_t len, proto::AckStatus& st)` (returns true when an ACK must be sent); `void applySettings()`; LiteApp members listed below.

- [ ] **Step 1: State.** In `lite_app.h` add includes for `domain/lite/*.h`, `domain/arbitration/arbitrator.h`, `domain/safety/envelope.h`, `domain/safety/safety_supervisor.h`, `domain/selftest/self_test.h`, `domain/estimation/battery_monitor.h`, `domain/settings/eeprom_writer.h`, `domain/input/nec_decoder.h`, `domain/ui/led_sequencer.h`, `domain/util/motion_timer.h`, `types/hw_types.h`, and add before `// LITE-STATE-END`:

```cpp
  SettingsLite settings;
  bool settings_loaded = false;
  EepromWriter eewriter;
  bool save_requested = false;
  GyroZeroLite gyro_zero;
  YawLite yaw;
  TiltLite tilt;
  RangeLite range;
  BatteryMonitor batt;
  EncoderLite enc_l;
  EncoderLite enc_r;
  ImuRaw imu_raw{};
  bool imu_ok = false;
  uint8_t imu_fail_streak = 0;
  NecDecoder nec;
  IrEvent ir_event{};
  bool ir_pending = false;
  Envelope envelope;
  Arbitrator arbitrator;
  SafetySupervisor supervisor;
  SafetyStatus safety{};
  SelfTest self_test;
  bool self_test_pending = true;
  HeadingHoldLite heading;
  RampLite ramp_l;
  RampLite ramp_r;
  MotionTimer motion;
  bool moving = false;
  int8_t teleop_v = 0;
  int8_t teleop_w = 0;
  bool deadman_latched = false;
  bool soft_stop = false;
  proto::OwnerHintValue owner_hint = proto::OwnerHintValue::kNone;
  CamLite cam;
  LedSequencer led;
  AnnunciatorLite ann;
  Beeper beeper;
```

and declare `void applySettings();`.

- [ ] **Step 2: Routes.** Create `src/lite/lite_routes.cpp`:

```cpp
#include "domain/lite/lite_frames.h"
#include "lite/lite_app.h"
#include "ports/clock.h"

namespace cq {
namespace lite {

namespace {

void rejectEvent(uint8_t frame_type, uint8_t src, proto::AckStatus st) {
  if (g_lite.rejects.allow(src, port::clock::nowMs())) {
    g_lite.events.push(proto::EventKind::kRejected, frame_type, static_cast<int16_t>(st));
  }
}

proto::AckStatus networkMotion(uint8_t frame_type, uint8_t src) {
  if (g_lite.self_test.locked()) {  // LLD §10.9: critical self-test → every motion command refused
    rejectEvent(frame_type, src, proto::AckStatus::kBlocked);
    return proto::AckStatus::kBlocked;
  }
  const proto::AckStatus st = g_lite.arbitrator.requestNetwork(true, port::clock::nowMs());
  if (st != proto::AckStatus::kOk) rejectEvent(frame_type, src, st);
  return st;
}

template <typename Msg>
bool dec(const uint8_t* p, uint8_t len, Msg& m) { return proto::decode(p, len, m); }

proto::AckStatus handle(uint8_t type, const uint8_t* p, uint8_t len) {
  using F = proto::FrameType;
  constexpr proto::AckStatus kBad = proto::AckStatus::kInvalidCommand;
  const uint32_t now = port::clock::nowMs();
  switch (static_cast<F>(type)) {
    case F::kHeartbeat: { proto::HeartbeatMsg m{}; return dec(p, len, m) ? proto::AckStatus::kOk : kBad; }
    case F::kHelloAck: { proto::HelloAckMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.link.onHelloAck(); return proto::AckStatus::kOk; }
    case F::kHelloReq: { proto::HelloReqMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.link.onHelloReq(); return proto::AckStatus::kOk; }
    case F::kEspAck: { proto::EspAckMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.link.onEspAck(m.ack_seq); return proto::AckStatus::kOk; }
    case F::kDrive: {
      proto::DriveMsg m{};
      if (!dec(p, len, m)) return kBad;
      const proto::AckStatus st = networkMotion(type, m.src);
      if (st != proto::AckStatus::kOk) return st;
      g_lite.deadman_latched = false;
      g_lite.teleop_v = m.v;
      g_lite.teleop_w = m.w;
      return st;
    }
    case F::kStop: {
      proto::StopMsg m{};
      if (!dec(p, len, m)) return kBad;
      g_lite.teleop_v = 0;
      g_lite.teleop_w = 0;
      g_lite.arbitrator.onNetworkStop();
      return proto::AckStatus::kOk;
    }
    case F::kBrake: { proto::BrakeMsg m{}; if (!dec(p, len, m)) return kBad; return g_lite.envelope.setBrake(m.on != 0); }
    case F::kEnvelope: {
      proto::EnvelopeMsg m{};
      if (!dec(p, len, m)) return kBad;
      const proto::AckStatus st = g_lite.envelope.set(m.speed_cap, m.accel_cap, m.lock_brake);
      if (st == proto::AckStatus::kOk) {
        const uint16_t packed = static_cast<uint16_t>(m.speed_cap | (static_cast<uint16_t>(m.accel_cap) << 8u));
        g_lite.events.push(proto::EventKind::kEnvelopeChanged, static_cast<int16_t>(packed), g_lite.envelope.flags());
        g_lite.slow.env_speed_cap = m.speed_cap;
        g_lite.slow.env_accel_cap = m.accel_cap;
        g_lite.slow.env_lock_brake = g_lite.envelope.flags();
      }
      return st;
    }
    case F::kOwnerHint: { proto::OwnerHintMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.owner_hint = static_cast<proto::OwnerHintValue>(m.owner); return proto::AckStatus::kOk; }
    case F::kCamJoy: { proto::CamJoyMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.joy(m.x, m.y, m.active != 0, now); return proto::AckStatus::kOk; }
    case F::kCamSet: { proto::CamSetMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.set(m.pan, m.tilt); return proto::AckStatus::kOk; }
    case F::kCamNudge: { proto::CamNudgeMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.nudge(m.dpan, m.dtilt); return proto::AckStatus::kOk; }
    case F::kCamHome: { proto::CamHomeMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.home(); return proto::AckStatus::kOk; }
    case F::kCamLock: { proto::CamLockMsg m{}; if (!dec(p, len, m)) return kBad; g_lite.cam.lock(m.on != 0); return proto::AckStatus::kOk; }
    case F::kSettingSet: {
      proto::SettingSetMsg m{};
      if (!dec(p, len, m)) return kBad;
      const proto::AckStatus st = g_lite.settings.set(m.id, m.value);
      if (st == proto::AckStatus::kOk) applySettings();
      return st;
    }
    case F::kSettingGet: {
      proto::SettingGetMsg m{};
      if (!dec(p, len, m)) return kBad;
      int32_t v = 0;
      if (!g_lite.settings.get(m.id, v)) return proto::AckStatus::kUnavailable;
      g_lite.setting_value.id = m.id;
      g_lite.setting_value.value = v;
      g_lite.setting_value_pending = true;
      return proto::AckStatus::kOk;
    }
    case F::kSettingsSave: {
      proto::SettingsSaveMsg m{};
      if (!dec(p, len, m)) return kBad;
      if (g_lite.save_requested || !g_lite.eewriter.idle()) return proto::AckStatus::kSettingsSaveBusy;
      g_lite.save_requested = true;
      return proto::AckStatus::kOk;
    }
    default:
      return proto::AckStatus::kUnavailable;
  }
}

}  // namespace

bool route(uint8_t type, const uint8_t* p, uint8_t len, proto::AckStatus& st) {
  switch (lite_frames::frameUse(type)) {
    case lite_frames::FrameUse::kUnknown: ++g_lite.parser.errors().unknown_type; return false;
    case lite_frames::FrameUse::kUnavailable: st = proto::AckStatus::kUnavailable; return true;
    case lite_frames::FrameUse::kHandled: st = handle(type, p, len); return lite_frames::needsAck(type);
  }
  return false;
}

void applySettings() {
  const LiteSettings& s = g_lite.settings.values();
  g_lite.encoders_enabled = s.encoders_enabled != 0;
  g_lite.gyro_zero.seed(s.gyro_bias_z);
  g_lite.heading.configure(headingGainsFromMilli(s.heading_kp_milli, s.rate_kp_milli));
}

}  // namespace lite
}  // namespace cq
```

(Message struct names `CamJoyMsg`, `CamNudgeMsg`, `CamHomeMsg`, `CamLockMsg`, `BrakeMsg`, `OwnerHintMsg`, `SettingGetMsg`, `SettingsSaveMsg`, `EspAckMsg` follow the generator's `<FrameName>Msg` pattern; verify with `grep -n "struct Cam.*Msg\|struct BrakeMsg" src/generated/cq_protocol.h`.)

- [ ] **Step 3: Remove the skeleton `route()` from `lite_app.cpp`**, and in `lite_comms.cpp` replace the HELLO caps line with `h.caps = lite_frames::helloCaps(g_lite.encoders_enabled);` (add `#include "domain/lite/lite_frames.h"`).

- [ ] **Step 4: Build and gate.** Run: `~/.platformio/penv/bin/pio run -e uno && python3 scripts/size_gate.py uno`. Expected: builds, within budget.
- [ ] **Step 5: Commit** `feat(uno-lite): frame routes (Lite subset, UNAVAILABLE for the rest), settings apply`.

---

### Task 13: Lite app — sensors, IR, button, self-test

**Files:** Create `firmware/uno/src/lite/lite_sensors.cpp`, `firmware/uno/src/lite/lite_tasks.h`; modify `lite_app.cpp` (task table, setup).

**Interfaces:**
- Produces (in `lite_tasks.h`): `void imuTask(); void irTask(); void ultrasonicTask(); void batteryTask(); void encodersTask(); void buttonTask(); void selfTestTask(); void selfTestRecoverTask(); void settingsLoad(); void settingsTask();` and `bool imuUsable();`.

- [ ] **Step 1: Sensors.** Create `src/lite/lite_tasks.h` with the declarations above (namespace `cq::lite`) plus `void controlTask(); void uiTask(); void cameraTask();` (Tasks 14–15). Create `src/lite/lite_sensors.cpp`:

```cpp
#include "domain/input/boot_button.h"
#include "drivers/board_config.h"
#include "lite/lite_app.h"
#include "lite/lite_tasks.h"
#include "ports/battery_adc.h"
#include "ports/button.h"
#include "ports/clock.h"
#include "ports/eeprom.h"
#include "ports/encoders.h"
#include "ports/imu.h"
#include "ports/ir.h"
#include "ports/ultrasonic.h"

namespace cq {
namespace lite {

namespace {
constexpr uint8_t kFaultStreak = 10;
constexpr uint32_t kReinitMs = 1000;
constexpr uint8_t kTiltLimitDeg = 45;    // full default (setting 13)
constexpr uint16_t kBattMvPerCountX100 = 4050, kBattLow = 7200, kBattCrit = 6600, kBattHyst = 200;
uint32_t g_last_init_ms = 0;
BootButton g_button;

void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_lite.fast_flags = on ? static_cast<uint16_t>(g_lite.fast_flags | m) : static_cast<uint16_t>(g_lite.fast_flags & ~m);
}
void setSlow(proto::SlowFlagBit bit, bool on) {
  const uint32_t m = 1UL << static_cast<uint8_t>(bit);
  g_lite.slow_flags = on ? (g_lite.slow_flags | m) : (g_lite.slow_flags & ~m);
}
uint8_t eeRead(uint16_t a) { return port::eeprom::read(a); }
}  // namespace

bool imuUsable() { return g_lite.imu_ok && g_lite.imu_fail_streak < kFaultStreak; }

void imuTask() {
  const uint32_t now = port::clock::nowMs();
  if (!g_lite.imu_ok) {
    if (static_cast<uint32_t>(now - g_last_init_ms) >= kReinitMs) {
      g_last_init_ms = now;
      g_lite.imu_ok = port::imu::begin();
      if (g_lite.imu_ok) g_lite.imu_fail_streak = 0;
    }
    setFast(proto::FastFlagBit::kImuFault, !g_lite.imu_ok);
    return;
  }
  ImuRaw& r = g_lite.imu_raw;
  if (!port::imu::read(r)) {
    if (g_lite.imu_fail_streak < 255) ++g_lite.imu_fail_streak;
    if (g_lite.imu_fail_streak == kFaultStreak) {
      g_lite.events.push(proto::EventKind::kImuFault);
      g_lite.imu_ok = false;
      g_last_init_ms = now;
      setFast(proto::FastFlagBit::kImuFault, true);
    }
    return;
  }
  g_lite.imu_fail_streak = 0;
  const bool quiet = !g_lite.moving && !g_lite.tilt.lifted() && !g_lite.tilt.tilted() && !g_lite.self_test_pending;
  g_lite.gyro_zero.update(r.gz, quiet, now);
  g_lite.yaw.update(static_cast<int16_t>(r.gz - g_lite.gyro_zero.zero()), !g_lite.moving);
  switch (g_lite.tilt.update(r.ax, r.ay, r.az, kTiltLimitDeg, now)) {
    case TiltLite::Change::kLifted: g_lite.events.push(proto::EventKind::kLifted); break;
    case TiltLite::Change::kLanded: g_lite.events.push(proto::EventKind::kLanded); break;
    case TiltLite::Change::kTilted: g_lite.events.push(proto::EventKind::kTilted); break;
    case TiltLite::Change::kLevel: g_lite.events.push(proto::EventKind::kLevel); break;
    case TiltLite::Change::kNone: break;
  }
  setFast(proto::FastFlagBit::kLifted, g_lite.tilt.lifted());
  setFast(proto::FastFlagBit::kTilted, g_lite.tilt.tilted());
  proto::TelemFastMsg& f = g_lite.fast;
  f.yaw = static_cast<uint16_t>(g_lite.yaw.yawCdeg());
  f.yaw_rate = g_lite.yaw.rateDdps();
}

void irTask() {
  uint32_t t_us = 0;
  IrEvent e{};
  while (port::ir::popEdge(t_us)) {
    if (g_lite.nec.feed(t_us, port::clock::nowMs(), e)) {
      g_lite.ir_event = e;
      g_lite.ir_pending = true;
    }
  }
}

void ultrasonicTask() {
  uint16_t echo_us = 0;
  const EchoState st = port::ultrasonic::poll(echo_us);
  if (g_lite.range.onReading(st, echo_us, 0) == RangeLite::Change::kFault) g_lite.events.push(proto::EventKind::kUsFault);
  setFast(proto::FastFlagBit::kUsFault, g_lite.range.fault());
  g_lite.fast.dist_mm = g_lite.range.distMm();
  port::ultrasonic::trigger();
}

void batteryTask() {
  uint16_t adc = 0;
  if (!port::battery_adc::ready(adc)) return;
  const BatteryMonitor::Level lvl =
      g_lite.batt.update(adc, kBattMvPerCountX100, kBattLow, kBattCrit, kBattHyst, port::clock::nowMs());
  if (g_lite.batt.changed()) {
    if (lvl == BatteryMonitor::Level::kLow) g_lite.events.push(proto::EventKind::kBattLow, g_lite.batt.mv());
    if (lvl == BatteryMonitor::Level::kCritical) g_lite.events.push(proto::EventKind::kBattCritical, g_lite.batt.mv());
  }
  setSlow(proto::SlowFlagBit::kBattLow, lvl == BatteryMonitor::Level::kLow);
  setSlow(proto::SlowFlagBit::kBattCritical, lvl == BatteryMonitor::Level::kCritical);
  g_lite.slow.batt_mv = g_lite.batt.mv();
  port::battery_adc::start();
}

void encodersTask() {
  EncoderSnapshot s{};
  port::encoders::snapshot(s);
  g_lite.enc_l.update(s.count_l, s.period_l_us, s.last_edge_l_us, s.t_us);
  g_lite.enc_r.update(s.count_r, s.period_r_us, s.last_edge_r_us, s.t_us);
  g_lite.fast.enc_l = g_lite.enc_l.count();
  g_lite.fast.enc_r = g_lite.enc_r.count();
}

void buttonTask() {
  switch (g_button.update(port::button::pressed(), port::clock::nowMs())) {
    case BootButton::Action::kPress: g_lite.events.push(proto::EventKind::kButtonPress); break;
    case BootButton::Action::kWifiReset: g_lite.link.setRequest(static_cast<uint8_t>(proto::FrameType::kWifiResetReq)); break;
    case BootButton::Action::kAdminReset: g_lite.link.setRequest(static_cast<uint8_t>(proto::FrameType::kAdminResetReq)); break;
    default: break;
  }
}

// Boot self-test 2 s after reset (LLD §10.9 rules; reused SelfTest).
void selfTestTask() {
  if (!g_lite.self_test_pending || port::clock::nowMs() < 2000) return;
  g_lite.self_test_pending = false;
  g_lite.self_test.begin();
  g_lite.self_test.report(SelfTest::Check::kImu, g_lite.imu_ok);
  g_lite.self_test.report(SelfTest::Check::kUltrasonic, !g_lite.range.fault());
  g_lite.self_test.report(SelfTest::Check::kBattery, SelfTest::batteryPlausible(g_lite.batt.mv()));
  g_lite.self_test.report(SelfTest::Check::kSettings, g_lite.settings_loaded);
  g_lite.self_test.complete();
  g_lite.events.push(proto::EventKind::kSelftest, g_lite.self_test.failMask(), g_lite.self_test.locked() ? 1 : 0);
  setSlow(proto::SlowFlagBit::kSelftestLocked, g_lite.self_test.locked());
}

void selfTestRecoverTask() {
  if (g_lite.self_test.tickBattery(SelfTest::batteryPlausible(g_lite.batt.mv()), port::clock::nowMs())) {
    g_lite.events.push(proto::EventKind::kSelftest, g_lite.self_test.failMask(), g_lite.self_test.locked() ? 1 : 0);
    setSlow(proto::SlowFlagBit::kSelftestLocked, g_lite.self_test.locked());
  }
}

void settingsLoad() {
  g_lite.settings_loaded = g_lite.settings.load(eeRead);
  if (!g_lite.settings_loaded) g_lite.events.push(proto::EventKind::kSettingsReset);
  applySettings();
}

void settingsTask() {
  static const EepromIo kIo{port::eeprom::read, port::eeprom::writeByteAsync, port::eeprom::busy};
  if (g_lite.save_requested && !g_lite.moving && g_lite.eewriter.idle()) {
    uint8_t block[SettingsLite::kBlockSize];
    g_lite.settings.encode(block);
    if (g_lite.eewriter.enqueue(SettingsLite::kAddr, block, SettingsLite::kBlockSize)) {
      g_lite.save_requested = false;
      g_lite.events.push(proto::EventKind::kSettingsSaved);
    }
  }
  g_lite.eewriter.step(kIo);
}

}  // namespace lite
}  // namespace cq
```

- [ ] **Step 2: Wire into `lite_app.cpp`.** Replace the task table with (motorsZeroTask removed; `controlTask`, `uiTask`, `cameraTask` are added in Tasks 14–15):

```cpp
Task g_tasks[] = {
    {imuTask, 5000UL, 0UL, 0},
    {irTask, 5000UL, 0UL, 0},
    {linkWatchTask, 10000UL, 2500UL, 0},
    {encodersTask, 10000UL, 5000UL, 0},
    {settingsTask, 20000UL, 7500UL, 0},
    {ultrasonicTask, 50000UL, 2500UL, 0},
    {telemetryFastDue, 50000UL, 0UL, 0},
    {buttonTask, 20000UL, 17500UL, 0},
    {batteryTask, 100000UL, 7500UL, 0},
    {selfTestRecoverTask, 100000UL, 7500UL, 0},
    {selfTestTask, 100000UL, 7500UL, 0},
    {telemetrySlowDue, 1000000UL, 7500UL, 0},
};
```

In `setup()` after the driver `begin()` calls add: `settingsLoad(); g_lite.imu_ok = port::imu::begin(); port::ultrasonic::trigger(); port::battery_adc::start();` (replacing the bare `port::imu::begin();`), and keep motors at zero until Task 14 adds the control task (add `{motorsZeroTask, 10000UL, 2500UL, 0}` back to the table for now).

- [ ] **Step 3: Build and gate.** `~/.platformio/penv/bin/pio run -e uno && python3 scripts/size_gate.py uno`
- [ ] **Step 4: Commit** `feat(uno-lite): sensors, IR capture, button, boot self-test, settings load/save`.

---

### Task 14: Lite app — control task (ownership, IR, drive, safety gate) and telemetry

**Files:** Create `firmware/uno/src/lite/lite_control.cpp`; modify `lite_app.cpp` (task table: replace `motorsZeroTask` with `controlTask` at 10 ms / 2.5 ms phase).

**Interfaces:**
- Consumes: everything in `LiteApp`; `IrController` (file-local instance), `mixLite`, `capLite`, `RampLite`, `toPwmLite`, `SafetySupervisor::apply`, `port::motor::apply`.
- Produces: `void controlTask();`

- [ ] **Step 1: Control.** Create `src/lite/lite_control.cpp`:

```cpp
#include "domain/input/ir_controller.h"
#include "domain/lite/drive_mix_lite.h"
#include "domain/lite/encoder_lite.h"
#include "lite/lite_app.h"
#include "lite/lite_tasks.h"
#include "ports/clock.h"
#include "ports/motor.h"

namespace cq {
namespace lite {

namespace {
constexpr uint16_t kDeadmanMs = 300;      // full default (setting 15)
constexpr uint16_t kIrHandbackMs = 3000;  // full default (setting 19)
constexpr uint16_t kMaxCps = 2500;        // full default (settings 48/49)
IrController g_ir;

void setFast(proto::FastFlagBit bit, bool on) {
  const uint16_t m = static_cast<uint16_t>(1u << static_cast<uint8_t>(bit));
  g_lite.fast_flags = on ? static_cast<uint16_t>(g_lite.fast_flags | m) : static_cast<uint16_t>(g_lite.fast_flags & ~m);
}
void zeroTeleop() {
  g_lite.teleop_v = 0;
  g_lite.teleop_w = 0;
}

void irInput(uint32_t now) {
  IrOutput o{};
  bool have = false;
  if (g_lite.ir_pending) {
    g_lite.ir_pending = false;
    if (g_ir.onEvent(g_lite.ir_event, o)) {
      if (g_lite.arbitrator.onIrInput(now)) zeroTeleop();  // IR takeover: network command dropped first
      have = true;
    }
  }
  if (!have && g_ir.tick(now, o)) have = true;
  if (!have) return;
  if (o.active) {
    g_lite.teleop_v = o.v;
    g_lite.teleop_w = o.w;
  } else {
    zeroTeleop();
  }
  if (o.jog_pan != 0 || o.jog_tilt != 0) g_lite.cam.nudge(static_cast<int16_t>(-o.jog_pan * 12), static_cast<int16_t>(o.jog_tilt * 12));
  if (o.stop) {
    zeroTeleop();
    g_lite.arbitrator.releaseIr();
  }
  if (o.cam_home) g_lite.cam.home();
  if (o.speed_changed) g_lite.beeper.beep(static_cast<uint16_t>(400 + g_ir.speedPct() * 10), 60, now);
}
}  // namespace

void controlTask() {
  const uint32_t now = port::clock::nowMs();
  const LiteSettings& s = g_lite.settings.values();

  // 1. ownership, deadman and link loss (reused Arbitrator: same rules as the full firmware)
  g_lite.arbitrator.setSynced(g_lite.link.synced());
  switch (g_lite.arbitrator.tick(now, g_lite.link.alive(now), false, Arbitrator::Config{kDeadmanMs, kIrHandbackMs})) {
    case Arbitrator::Action::kDeadmanStop:
      zeroTeleop();
      g_lite.soft_stop = true;
      g_lite.deadman_latched = true;
      g_lite.events.push(proto::EventKind::kDeadmanStop);
      break;
    case Arbitrator::Action::kLinkLostStop:
      zeroTeleop();
      g_lite.soft_stop = true;
      break;
    case Arbitrator::Action::kIrReleased: zeroTeleop(); break;
    case Arbitrator::Action::kNone: break;
  }
  if (g_lite.arbitrator.takeOwnerChanged()) {
    const proto::Owner owner = g_lite.arbitrator.owner();
    g_lite.events.push(proto::EventKind::kControlChanged, static_cast<int16_t>(owner));
    if (owner == proto::Owner::kIr) g_lite.events.push(proto::EventKind::kIrOverride);
    if (owner == proto::Owner::kNone) zeroTeleop();
  }
  irInput(now);

  // 2. heading hold → mix → caps → ramp → PWM (whole numbers only)
  const int8_t v = g_lite.teleop_v;
  const int8_t steer = (s.reverse_steer == 0 && v < 0) ? static_cast<int8_t>(-g_lite.teleop_w) : g_lite.teleop_w;  // car-style
  int8_t w_steer = 0, w_ctrl = 0;
  g_lite.heading.teleop(v, steer, imuUsable(), g_lite.yaw.yawCdeg(), g_lite.yaw.rateDdps(), w_steer, w_ctrl);
  Tracks t = mixLite(v, w_steer, w_ctrl, s.turn_full);
  const uint8_t degraded = !imuUsable() ? 30 : (g_lite.range.fault() ? 50 : 100);
  const uint8_t cap = g_lite.envelope.speedCap() < degraded ? g_lite.envelope.speedCap() : degraded;
  bool capped = false;
  t = capLite(t, cap, g_lite.range.distMm(), s.obstacle_stop, s.obstacle_slow, g_lite.envelope.brakeOn(), capped);
  const uint16_t accel_env = g_lite.envelope.accelCap();
  const uint16_t accel = (accel_env != 0 && accel_env < s.accel_limit) ? accel_env : s.accel_limit;
  const int16_t pm_l = g_lite.ramp_l.step(t.l, accel);
  const int16_t pm_r = g_lite.ramp_r.step(t.r, accel);
  if (g_lite.soft_stop && pm_l == 0 && pm_r == 0) g_lite.soft_stop = false;
  const bool moving_l = g_lite.encoders_enabled ? g_lite.enc_l.moving() : g_lite.moving;
  const bool moving_r = g_lite.encoders_enabled ? g_lite.enc_r.moving() : g_lite.moving;
  const int16_t pwm_l = toPwmLite(pm_l, moving_l, s.db_start[pm_l >= 0 ? 0 : 1], s.db_keep[pm_l >= 0 ? 0 : 1]);
  const int16_t pwm_r = toPwmLite(pm_r, moving_r, s.db_start[pm_r >= 0 ? 2 : 3], s.db_keep[pm_r >= 0 ? 2 : 3]);

  // 3. the safety gate: only the supervisor makes PwmOutput (hard rule)
  const SafetyInputs in{g_lite.tilt.lifted(), g_lite.tilt.tilted(),
                        g_lite.batt.level() == BatteryMonitor::Level::kCritical, g_lite.envelope.brakeOn(),
                        g_lite.range.distMm(), s.obstacle_stop, 0, g_lite.self_test.locked()};
  const bool was_blocked = g_lite.safety.blocked;
  const PwmOutput out = g_lite.supervisor.apply(pwm_l, pwm_r, in, g_lite.safety);
  port::motor::apply(out);
  if (g_lite.safety.blocked && !was_blocked) g_lite.events.push(proto::EventKind::kBlocked, static_cast<int16_t>(g_lite.range.distMm()));
  if (!g_lite.safety.blocked && was_blocked) g_lite.events.push(proto::EventKind::kUnblocked);

  // 4. state + telemetry
  if (out.left() != 0 || out.right() != 0) g_lite.motion.mark(now);
  g_lite.moving = g_lite.motion.moving(now);
  setFast(proto::FastFlagBit::kBlocked, g_lite.safety.blocked);
  setFast(proto::FastFlagBit::kHalted, g_lite.safety.halted);
  setFast(proto::FastFlagBit::kCapped, capped);
  setFast(proto::FastFlagBit::kSoftStop, g_lite.soft_stop);
  setFast(proto::FastFlagBit::kLocked, g_lite.envelope.lockBrake());
  setFast(proto::FastFlagBit::kSynced, g_lite.link.synced());
  proto::TelemFastMsg& f = g_lite.fast;
  f.pwm_l = out.left();
  f.pwm_r = out.right();
  f.spd_cmd_l = static_cast<int8_t>(pm_l / 10);
  f.spd_cmd_r = static_cast<int8_t>(pm_r / 10);
  f.spd_act_l = speedPctLite(g_lite.enc_l.cps(), out.left(), kMaxCps, g_lite.encoders_enabled);
  f.spd_act_r = speedPctLite(g_lite.enc_r.cps(), out.right(), kMaxCps, g_lite.encoders_enabled);
  f.pan = g_lite.cam.pan();
  f.tilt = g_lite.cam.tilt();
  f.flags = g_lite.fast_flags;
  g_lite.slow.owner = static_cast<uint8_t>(g_lite.arbitrator.owner());
  g_lite.slow.driving = g_lite.moving ? 1 : 0;
}

}  // namespace lite
}  // namespace cq
```

- [ ] **Step 2: Task table:** replace `{motorsZeroTask, 10000UL, 2500UL, 0}` with `{controlTask, 10000UL, 2500UL, 0}` and delete `motorsZeroTask`.
- [ ] **Step 3: Build, gate, float check.**

Run: `~/.platformio/penv/bin/pio run -e uno && python3 scripts/size_gate.py uno && grep -rnE "\bfloat\b|\bdouble\b" src/lite src/domain/lite`
Expected: builds within budget; grep prints nothing.
- [ ] **Step 4: Commit** `feat(uno-lite): control task - ownership, IR, heading hold, caps, safety gate`.

---

### Task 15: Lite app — LED, beep, camera output; extras by space

**Files:** Create `firmware/uno/src/lite/lite_ui.cpp`; modify `lite_app.cpp` (tasks `uiTask` 50 ms / 17.5 ms, `cameraTask` 20 ms / 7.5 ms; `port::servos::attach` both axes in setup).

- [ ] **Step 1: UI and camera.** Create `src/lite/lite_ui.cpp`:

```cpp
#include "domain/lite/cam_lite.h"
#include "lite/lite_app.h"
#include "lite/lite_tasks.h"
#include "ports/clock.h"
#include "ports/led.h"
#include "ports/servos.h"
#include "ports/tone.h"

namespace cq {
namespace lite {

namespace {
Rgb g_led_out{1, 1, 1};
uint16_t g_tone_hz = 0;
}  // namespace

void uiTask() {
  const uint32_t now = port::clock::nowMs();
  LiteUiState s{};
  s.synced = g_lite.link.synced();
  s.owner = g_lite.arbitrator.owner();
  s.driving = g_lite.moving;
  const int16_t a = g_lite.fast.spd_cmd_l < 0 ? static_cast<int16_t>(-g_lite.fast.spd_cmd_l) : g_lite.fast.spd_cmd_l;
  s.speed_pct = static_cast<uint8_t>(a > 100 ? 100 : a);
  s.locked = g_lite.self_test.locked();
  s.tilted_or_lifted = g_lite.tilt.tilted() || g_lite.tilt.lifted();
  s.batt_crit = g_lite.batt.level() == BatteryMonitor::Level::kCritical;
  s.batt_low = g_lite.batt.level() == BatteryMonitor::Level::kLow;
  s.blocked = g_lite.safety.blocked;
  g_lite.ann.update(s, now, g_lite.led);
  const Rgb c = g_lite.led.output(now);
  if (c != g_led_out) {
    port::led::write(c);
    g_led_out = c;
  }
  const uint16_t hz = g_lite.beeper.tick(now);
  if (hz != g_tone_hz) {
    if (hz == 0) {
      port::tone::stop();
    } else {
      port::tone::play(hz);
    }
    g_tone_hz = hz;
  }
}

void cameraTask() {
  g_lite.cam.tick(port::clock::nowMs());
  port::servos::writeUs(Axis::kPan, CamLite::toUs(g_lite.cam.pan()));
  port::servos::writeUs(Axis::kTilt, CamLite::toUs(g_lite.cam.tilt()));
}

}  // namespace lite
}  // namespace cq
```

- [ ] **Step 2: Wire tasks and setup** (`port::servos::attach(Axis::kPan); port::servos::attach(Axis::kTilt);` after `port::servos::begin()`; add `{cameraTask, 20000UL, 7500UL, 0}` and `{uiTask, 50000UL, 17500UL, 0}` to the table).
- [ ] **Step 3: Build, gate and measure.** `~/.platformio/penv/bin/pio run -e uno && python3 scripts/size_gate.py uno`. Record flash/RAM in LLD §7 ("measured, all core features").
- [ ] **Step 4: Extras by space (LLD §8, owner order).** If flash ≤ 26.5 KB (≥ 1.5 KB margin): implement smooth camera movement (`CamLite` moves at most `servo_speed` deg/s = 120 °/s → 24 deci-deg per 20 ms tick toward the target; test `test_cam_smooth_steps` added first in `test_lite_ui`: after `set(1800, 900)` from home, one `step()` moves pan by 24). Then, if still ≥ 1.5 KB margin, propose encoder balance to the owner as a separate small design note (not built in this plan). Otherwise record "no space for extras" in the LLD.
- [ ] **Step 5: Full checks.** `~/.platformio/penv/bin/pio test -e native 2>&1 | tail -1; ~/.platformio/penv/bin/pio run -e mega | tail -1; python3 scripts/size_gate.py mega`
Expected: all host suites pass; mega builds within its gate.
- [ ] **Step 6: Commit** `feat(uno-lite): LED, beep and camera output; measured sizes`.

---

### Task 16: Hardware bring-up on the ELEGOO Uno (owner present)

**Files:** Modify `docs/design-notes.md` (results), `docs/lld/uno-lite-lld.md` (measured numbers, status DONE).

- [ ] **Step 1: Ask the owner before flashing.** Flashing replaces ELEGOO's firmware on the Uno (a full copy is kept at `~/tank-dump/uno-flash.hex` on the Linux machine and in the scratchpad, so it can be restored). Proceed only on a yes.
- [ ] **Step 2: Flash over SSH** (Uno on the Linux machine; S1 in the upload position):

```bash
scp firmware/uno/.pio/build/uno/firmware.hex 192.168.31.8:cq/fw/lite.hex
ssh 192.168.31.8 'AD=~/.platformio/packages/tool-avrdude; $AD/avrdude -C $AD/avrdude.conf -p m328p -c arduino -P /dev/ttyACM0 -b 115200 -D -U flash:w:$HOME/cq/fw/lite.hex:i'
```
Expected: `bytes of flash verified`. (Port may be `/dev/ttyUSB0` for a CH340 board — check `ls /dev/ttyACM* /dev/ttyUSB*`.)

- [ ] **Step 3: Stand checks (tracks off the ground, STOP within reach), record each result:**
  1. HELLO over USB (`_rec.py`): `caps=0`, commit matches; LED breathes blue with no ESP32.
  2. S1 to ESP32, battery only, no USB: ESP32 `/info` shows `uno.synced: true`, boot history `power_on` (no brownout); LED dim white.
  3. IR: tap ◀/▶ (≈ 0.15 s), hold ▲, speed keys 6/4 beep up/down, OK stops.
  4. Phone page: drive, release (deadman stop ≤ 0.3 s), close the page (link-loss stop).
  5. Obstacle: hand in front at < 15 cm while driving forward → stops; reverse still works.
  6. Lift the tank while driving → stops; tilt past 45° → stops.
  7. `TURN` from the bench/phone → `ACK UNAVAILABLE`.
- [ ] **Step 4: Floor check (owner):** 3 m straight run with ▲ held; drift ≤ ~4 cm as with the Mega (gyro zero learned after 5 s at rest).
- [ ] **Step 5: Record and commit** results in design-notes §43 and the LLD (status DONE, measured sizes): `docs(uno-lite): hardware bring-up results`.

---

## Self-review (done while writing)
- **Spec coverage:** D1.1 drivable (Tasks 12–15); D1.2 safety (Arbitrator deadman/link loss, SafetySupervisor obstacle/lift/tilt/battery/self-test lock, Envelope caps — Tasks 12–14, checked on hardware in Task 16); D1.3 protocol (Tasks 1, 11, 12); D1.4 budget (gates in Tasks 1, 12–15); D1.5 integers (float grep in Tasks 1, 14); D1.6 full kept (mega build + 182 tests in Tasks 1, 11, 15); D2 rows: IR (Task 14), phone (Task 12), straight line (Tasks 2–4, 14), encoders (Tasks 8, 13, 14), camera (Tasks 10, 15), LED (Tasks 10, 15), beep (Tasks 10, 14, 15), settings (Tasks 9, 12, 13), telemetry (Tasks 1, 13, 14), drops (Task 11 frame table).
- **Placeholders:** none; two verification notes (SettingId and message struct names) tell the executor exactly what to grep and to record a ruling.
- **Types:** `Tracks`, `RampLite::step(int16_t pct, uint16_t accel)`, `toPwmLite(int16_t permille, …)`, `HeadingHoldLite::teleop(…, int8_t& w_steer, int8_t& w_ctrl)`, `GyroZeroLite::seed(int16_t)`, `SettingsLite::values()` are used with the same signatures in Tasks 12–15.
- **Review Focus:** each of the five lines has its test in Tasks 4, 9, 4, 5, 11.
