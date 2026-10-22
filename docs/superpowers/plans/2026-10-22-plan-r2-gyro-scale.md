# Plan R2 — Gyro scale on the bridge (bridge LLD R19) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Correct the Uno gyro's ~2.3 % under-reading on the bridge, so that turns, arcs, Logo paths, the phone, recordings and the AI all see the true heading. The fix is one saved scale applied where the heading arrives.

**Architecture:**
- A pure `HeadingScale` (`core/`) adds each frame's heading change times `gyro_scale / 10000`, in whole numbers fine enough that rounding never builds up, and scales the rate.
- `BodyLink` owns one and applies it to every TELEM_FAST before anyone reads the snapshot. The raw heading stays in `snapshot().yaw_raw`; each HELLO re-anchors.
- The scale is saved in NVS (`cq_motion/gyro_scale`), set and reset through `calib` with the tank's button (as counts_per_m), and shown in the `calib` reply. `move.py --gyro-scale N` saves it.
- The Uno is unchanged.

**Tech Stack:** C++17 (ESP32-S3 Arduino 3.x, PlatformIO `bridge`, `bridge_ota`, `native`), Unity host tests, Python 3.12 + uv (smoke tools: pytest, ruff, pyright strict).

**Spec:** `docs/lld/esp32-bridge-lld.md` Revision 19 (approved 2026-10-22); measurements in `docs/design-notes.md` §116.

## Global Constraints
- `gyro_scale` in parts per 10 000; range **9000-11000**; default **10000** (no correction); first value **10230**.
- Corrected heading: `sum += wrap(raw − last_raw) × scale`, in 1/10 000 of 0.01° (`int32`), wrapped at 36000 × 10000; heading out = round(sum / 10000) mod 36000; `wrap` takes the short way (−180..180°).
- Rate out = round(rate × scale / 10000), clamped to ±32767.
- The first frame, and the first after every HELLO, anchors: corrected = raw.
- Setting it: driver only, not while a primitive runs, the tank's button confirms; the reset goes to 10000; both saved in NVS and handed to `BodyLink` at once.
- No change to the Uno, the protocol (no protogen run) or any safety rule; no heap, no `%llu`; generated files untouched.
- Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch `plan-r2-gyro` from `main`.

## Review Focus
1. **The Uno restarts mid-drive.** Its heading jumps to 0. The corrected heading must re-anchor on the HELLO, not add a −N° change × scale. Pinned by `test_snapshot_heading_is_corrected_by_the_gyro_scale` (the HELLO part).
2. **A heading crossing 0°/360° between frames** must be taken the short way, never as a 359° turn. Pinned by `test_a_wrap_is_taken_the_short_way`.
3. **Rounding over a long session** (an hour at 50 Hz = 180 000 frames) must not drift. Pinned by `test_no_rounding_build_up` (100 000 frames exact).
4. **A saved value out of range** (old or corrupt NVS) must give 10000 at boot. `motionStoreLoadGyroScale` checks 9000..11000. It is device-only: the reviewer checks it by reading.
5. **A `calib` with both `counts_per_m` and `gyro_scale`** saves only the gyro scale (one value per message, the gyro scale first). A reasonable sender sends one; the rule is in `onCalib`. The reviewer checks that the other value is not silently half-saved.

## File structure
| File | Change |
|---|---|
| `firmware/esp32/src/core/heading_scale.{h,cpp}` | new: `HeadingScale`, `kGyroScale*` |
| `firmware/esp32/src/core/body_link.{h,cpp}` | owns a `HeadingScale`; corrects TELEM_FAST; `yaw_raw`; `setGyroScale` / `gyroScale`; re-anchors on HELLO |
| `firmware/esp32/src/core/ws_codec*.{h,cpp}`, `confirm.h`, `ws_json.h`, `ws_json_motion.cpp` | `gyro_scale` key and reset; the `calib` reply gains `gyro_scale` |
| `firmware/esp32/src/ports/motion_store.h`, `firmware/esp32/src/net/motion_store.{h,cpp}`, `firmware/esp32/src/net/ports_esp32.cpp`, `firmware/esp32/src/fakes/fake_ports.{h,cpp}` | `loadGyroScale` / `saveGyroScale` |
| `firmware/esp32/src/app/motion_controller.{h,cpp}`, `firmware/esp32/src/app/settings_controller.{h,cpp}`, `firmware/esp32/src/net/link_task.cpp` | load at begin; `setGyroScale`; button-confirmed set / reset (`applyGyroScale`) |
| `tools/smoke/move.py` (+ test) | `--gyro-scale N` |
| tests | `test_heading_scale` (new), `test_body_link`, `test_ws_codec`, `test_ws_json`, `test_settings_controller`, `test_motion_controller` |

---

### Task 1: HeadingScale

**Files:**
- Create: `firmware/esp32/src/core/heading_scale.h`, `firmware/esp32/src/core/heading_scale.cpp`
- Test: `firmware/esp32/test/test_heading_scale/test_main.cpp`

**Interfaces:**
- Produces:
  - `cq::core::HeadingScale` with `bool setScale(uint16_t)`, `uint16_t scale() const`, `void reset()`, `void apply(uint16_t yaw_raw, int16_t rate_raw, uint16_t& yaw, int16_t& rate)`
  - `kGyroScaleDefault` 10000, `kGyroScaleMin` 9000, `kGyroScaleMax` 11000

- [ ] **Step 1: Write the failing test** — create `firmware/esp32/test/test_heading_scale/test_main.cpp`:

```cpp
// HeadingScale (bridge LLD R19): the Uno's gyro heading and rate corrected by one scale.
#include <unity.h>

#include "core/heading_scale.h"

using cq::core::HeadingScale;

void setUp() {}
void tearDown() {}

namespace {
struct Out {
  uint16_t yaw;
  int16_t rate;
};
Out feed(HeadingScale& h, uint16_t raw, int16_t rate = 0) {
  Out o{0, 0};
  h.apply(raw, rate, o.yaw, o.rate);
  return o;
}
// Spin `turns` full turns from raw 0 in 3° frames (+ = left), returns the last corrected heading.
uint16_t spin(HeadingScale& h, int turns) {
  int32_t raw = 0;
  uint16_t yaw = feed(h, 0).yaw;
  const int frames = (turns < 0 ? -turns : turns) * 120;
  for (int i = 0; i < frames; ++i) {
    raw += turns < 0 ? -300 : 300;
    yaw = feed(h, static_cast<uint16_t>(((raw % 36000) + 36000) % 36000)).yaw;
  }
  return yaw;
}
}  // namespace

void test_default_scale_passes_the_heading_through() {
  HeadingScale h;
  TEST_ASSERT_EQUAL_UINT16(10000, h.scale());
  TEST_ASSERT_EQUAL_UINT16(12345, feed(h, 12345).yaw);
  TEST_ASSERT_EQUAL_UINT16(12645, feed(h, 12645, 900).yaw);
  TEST_ASSERT_EQUAL_INT16(900, feed(h, 12700, 900).rate);
  TEST_ASSERT_EQUAL_UINT16(0, spin(h, 4));
}

void test_scale_corrects_four_turns_both_ways() {
  HeadingScale left;
  left.setScale(10230);
  TEST_ASSERT_EQUAL_UINT16(3312, spin(left, 4));  // 1440 x 1.023 = 1473.12° -> 33.12°
  HeadingScale right;
  right.setScale(10230);
  TEST_ASSERT_EQUAL_UINT16(32688, spin(right, -4));  // -1473.12° -> 326.88°
}

void test_no_rounding_build_up() {
  HeadingScale h;
  h.setScale(10230);
  uint16_t yaw = feed(h, 0).yaw;
  for (int32_t i = 1; i <= 100000; ++i) yaw = feed(h, static_cast<uint16_t>(i % 36000)).yaw;
  TEST_ASSERT_EQUAL_UINT16(30300, yaw);  // 100 000 cdeg x 1.023 = 102 300 -> wrapped 303.00°
}

void test_first_frame_and_reset_anchor_to_the_raw_heading() {
  HeadingScale h;
  h.setScale(10230);
  TEST_ASSERT_EQUAL_UINT16(9000, feed(h, 9000).yaw);  // first frame: no change
  TEST_ASSERT_EQUAL_UINT16(10023, feed(h, 10000).yaw);  // +10° -> +10.23°
  h.reset();  // the Uno restarted: its heading starts again
  TEST_ASSERT_EQUAL_UINT16(0, feed(h, 0).yaw);
  TEST_ASSERT_EQUAL_UINT16(1023, feed(h, 1000).yaw);
}

void test_a_wrap_is_taken_the_short_way() {
  HeadingScale h;
  h.setScale(10230);
  feed(h, 35900);
  TEST_ASSERT_EQUAL_UINT16(105, feed(h, 100).yaw);  // +2° across 0 -> +2.046°: 35900 + 204.6 -> 104.6 -> 105
  TEST_ASSERT_EQUAL_UINT16(35900, feed(h, 35900).yaw);  // back -2° -> -2.046°: where it was
}

void test_rate_is_scaled_and_rounded() {
  HeadingScale h;
  h.setScale(10230);
  TEST_ASSERT_EQUAL_INT16(665, feed(h, 0, 650).rate);    // 650 x 1.023 = 664.95
  TEST_ASSERT_EQUAL_INT16(-665, feed(h, 0, -650).rate);
  TEST_ASSERT_EQUAL_INT16(0, feed(h, 0, 0).rate);
  h.setScale(11000);
  TEST_ASSERT_EQUAL_INT16(32767, feed(h, 0, 32000).rate);  // kept inside int16
  TEST_ASSERT_EQUAL_INT16(-32767, feed(h, 0, -32000).rate);
}

void test_out_of_range_scale_is_refused() {
  HeadingScale h;
  TEST_ASSERT_FALSE(h.setScale(8999));
  TEST_ASSERT_FALSE(h.setScale(11001));
  TEST_ASSERT_EQUAL_UINT16(10000, h.scale());
  TEST_ASSERT_TRUE(h.setScale(9000));
  TEST_ASSERT_TRUE(h.setScale(11000));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_default_scale_passes_the_heading_through);
  RUN_TEST(test_scale_corrects_four_turns_both_ways);
  RUN_TEST(test_no_rounding_build_up);
  RUN_TEST(test_first_frame_and_reset_anchor_to_the_raw_heading);
  RUN_TEST(test_a_wrap_is_taken_the_short_way);
  RUN_TEST(test_rate_is_scaled_and_rounded);
  RUN_TEST(test_out_of_range_scale_is_refused);
  return UNITY_END();
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_heading_scale`
Expected: FAIL — `'core/heading_scale.h' file not found`.

- [ ] **Step 3: Implement** — create `firmware/esp32/src/core/heading_scale.h`:

```cpp
// The Uno's gyro heading corrected by one scale (bridge LLD R19): each frame's change times gyro_scale / 10000,
// summed in whole numbers fine enough that the rounding never builds up. Pure, host-tested.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

constexpr uint16_t kGyroScaleDefault = 10000;  // x1.0000: no correction
constexpr uint16_t kGyroScaleMin = 9000;
constexpr uint16_t kGyroScaleMax = 11000;

class HeadingScale {
 public:
  bool setScale(uint16_t scale_e4);  // false (unchanged) outside kGyroScaleMin..kGyroScaleMax
  uint16_t scale() const { return m_scale; }
  void reset() { m_anchored = false; }  // the next frame anchors: corrected = raw
  // yaw_raw 0.01° (0..35999), rate_raw 0.1 °/s in; the corrected pair out.
  void apply(uint16_t yaw_raw, int16_t rate_raw, uint16_t& yaw, int16_t& rate);

 private:
  uint16_t m_scale = kGyroScaleDefault;
  bool m_anchored = false;
  uint16_t m_last_raw = 0;
  int32_t m_sum_e4 = 0;  // corrected heading in 1/10000 of 0.01°, 0 .. 36000 x 10000 - 1
};

}  // namespace core
}  // namespace cq
```

and `firmware/esp32/src/core/heading_scale.cpp`:

```cpp
#include "core/heading_scale.h"

namespace cq {
namespace core {
namespace {
constexpr int32_t kTurnE4 = 36000 * 10000;  // one full turn, 3.6e8 < 2^31
}  // namespace

bool HeadingScale::setScale(uint16_t scale_e4) {
  if (scale_e4 < kGyroScaleMin || scale_e4 > kGyroScaleMax) return false;
  m_scale = scale_e4;
  return true;
}

void HeadingScale::apply(uint16_t yaw_raw, int16_t rate_raw, uint16_t& yaw, int16_t& rate) {
  if (!m_anchored) {
    m_anchored = true;
    m_sum_e4 = static_cast<int32_t>(yaw_raw % 36000) * 10000;
  } else {
    int32_t d = static_cast<int32_t>(yaw_raw) - m_last_raw;  // the short way: -180..180°
    if (d > 18000) d -= 36000;
    if (d < -18000) d += 36000;
    m_sum_e4 += d * m_scale;  // |d| <= 18000, scale <= 11000: < 2e8
    if (m_sum_e4 >= kTurnE4) m_sum_e4 -= kTurnE4;
    if (m_sum_e4 < 0) m_sum_e4 += kTurnE4;
  }
  m_last_raw = yaw_raw;
  yaw = static_cast<uint16_t>(((m_sum_e4 + 5000) / 10000) % 36000);
  const int32_t r = static_cast<int32_t>(rate_raw) * m_scale;
  int32_t v = r >= 0 ? (r + 5000) / 10000 : -((-r + 5000) / 10000);
  if (v > 32767) v = 32767;
  if (v < -32767) v = -32767;
  rate = static_cast<int16_t>(v);
}

}  // namespace core
}  // namespace cq
```

- [ ] **Step 4: Run it to verify it passes**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_heading_scale`
Expected: PASS, 7 test cases.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/heading_scale.h firmware/esp32/src/core/heading_scale.cpp firmware/esp32/test/test_heading_scale/test_main.cpp
git commit -m "feat(bridge): R19 HeadingScale - the gyro heading corrected by one scale

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: BodyLink applies the scale

**Files:**
- Modify: `firmware/esp32/src/core/body_link.h`, `firmware/esp32/src/core/body_link.cpp`
- Test: `firmware/esp32/test/test_body_link/test_main.cpp`

**Interfaces:**
- Consumes (Task 1): `HeadingScale`.
- Produces:
  - `TankSnapshot::yaw_raw` (uint16_t); `snapshot().fast.yaw` / `.yaw_rate` hold the corrected values
  - `bool BodyLink::setGyroScale(uint16_t)`, `uint16_t BodyLink::gyroScale() const`

- [ ] **Step 1: Write the failing test**

```diff
diff --git a/firmware/esp32/test/test_body_link/test_main.cpp b/firmware/esp32/test/test_body_link/test_main.cpp
index 819c173..b72d1c7 100644
--- a/firmware/esp32/test/test_body_link/test_main.cpp
+++ b/firmware/esp32/test/test_body_link/test_main.cpp
@@ -300,6 +300,34 @@ void test_telemetry_snapshot_and_events() {
 
 // Plan Q1 (AI engine LLD §2): a TELEM_FAST frame keeps the ESP32 time it arrived at (same clock as the camera's
 // frame stamp); a TELEM_SLOW frame does not change it.
+// R19: the snapshot carries the heading and rate corrected by the gyro scale, and the raw heading; a HELLO (the
+// Uno restarted, its heading starts again) re-anchors the corrected heading on the raw one.
+void fastYaw(uint16_t yaw, int16_t rate, uint8_t seq, uint32_t now) {
+  proto::TelemFastMsg tf{};
+  tf.yaw = yaw;
+  tf.yaw_rate = rate;
+  uint8_t f[64];
+  const size_t n = cqtest::frame(tf, seq, f);
+  unoSends(f, n, now);
+}
+
+void test_snapshot_heading_is_corrected_by_the_gyro_scale() {
+  TEST_ASSERT_EQUAL_UINT16(10000, bl->gyroScale());
+  TEST_ASSERT_TRUE(bl->setGyroScale(10230));
+  TEST_ASSERT_FALSE(bl->setGyroScale(12000));
+  TEST_ASSERT_EQUAL_UINT16(10230, bl->gyroScale());
+  fastYaw(9000, 0, 1, 5);  // the first frame anchors
+  TEST_ASSERT_EQUAL_UINT16(9000, bl->snapshot().fast.yaw);
+  fastYaw(18000, 650, 2, 25);  // +90° raw -> +92.07°
+  TEST_ASSERT_EQUAL_UINT16(18207, bl->snapshot().fast.yaw);
+  TEST_ASSERT_EQUAL_UINT16(18000, bl->snapshot().yaw_raw);
+  TEST_ASSERT_EQUAL_INT16(665, bl->snapshot().fast.yaw_rate);
+  bl->startMaster(30);
+  hello(proto::kProtocolVersion, 40, 3);  // the Uno (re)started
+  fastYaw(0, 0, 4, 60);
+  TEST_ASSERT_EQUAL_UINT16(0, bl->snapshot().fast.yaw);  // anchored again
+}
+
 void test_telem_fast_keeps_its_arrival_time() {
   uint8_t f[64];
   size_t n = cqtest::frame(proto::TelemFastMsg{}, 1, f);
@@ -406,6 +434,7 @@ int main() {
   RUN_TEST(test_cam_home_ack_and_timeout);
   RUN_TEST(test_telemetry_snapshot_and_events);
   RUN_TEST(test_telem_fast_keeps_its_arrival_time);
+  RUN_TEST(test_snapshot_heading_is_corrected_by_the_gyro_scale);
   RUN_TEST(test_slow_count_counts_slow_frames);
   RUN_TEST(test_wifi_reset_request_answered_unavailable);
   RUN_TEST(test_sound_and_cam_lock_acked);
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_body_link`
Expected: FAIL to compile — `no member named 'gyroScale' in 'cq::core::BodyLink'`.

- [ ] **Step 3: Implement**

```diff
diff --git a/firmware/esp32/src/core/body_link.cpp b/firmware/esp32/src/core/body_link.cpp
index a1d7240..759882f 100644
--- a/firmware/esp32/src/core/body_link.cpp
+++ b/firmware/esp32/src/core/body_link.cpp
@@ -95,6 +95,8 @@ void BodyLink::onAckFrame(const FrameView& f) {
 void BodyLink::onTelemFrame(const FrameView& f) {
   if (f.type == static_cast<uint8_t>(proto::FrameType::kTelemFast)) {
     if (!proto::decode(f.payload, f.len, m_snap.fast)) return;
+    m_snap.yaw_raw = m_snap.fast.yaw;  // R19: everyone downstream reads the corrected heading
+    m_heading.apply(m_snap.yaw_raw, m_snap.fast.yaw_rate, m_snap.fast.yaw, m_snap.fast.yaw_rate);
     m_snap.fast_valid = true;
     m_snap.fast_bus_us = m_rx_us;
     ++m_snap.fast_count;
@@ -152,6 +154,7 @@ void BodyLink::onFrame(const FrameView& f, uint32_t now_ms) {
 
 void BodyLink::onHello(const proto::HelloMsg& h) {
   m_hello = HelloInfo{true, h.fw, h.proto, h.caps, h.reset_reason, h.commit};
+  m_heading.reset();  // R19: the Uno (re)started - its heading starts again; anchor on its next frame
   if (!m_master) return;  // idle (no web client): the bridge stays silent
   if (h.proto != proto::kProtocolVersion) {
     m_hs = HandshakeState::kIncompatible;
diff --git a/firmware/esp32/src/core/body_link.h b/firmware/esp32/src/core/body_link.h
index bd4687f..840ced0 100644
--- a/firmware/esp32/src/core/body_link.h
+++ b/firmware/esp32/src/core/body_link.h
@@ -11,6 +11,7 @@
 
 #include "core/frame_parser.h"
 #include "core/frame_tx.h"
+#include "core/heading_scale.h"
 #include "cq_protocol.h"
 
 namespace cq {
@@ -49,8 +50,9 @@ struct TankSnapshot {
   uint32_t fast_count = 0;  // TELEM_FAST frames seen (50 Hz; tf is sent for every 5th)
   uint64_t fast_bus_us = 0;  // Plan Q1: ESP32 timer (µs since boot) when the newest TELEM_FAST arrived
   uint32_t slow_count = 0;  // TELEM_SLOW frames seen (owner is applied once per new frame)
-  proto::TelemFastMsg fast{};
+  proto::TelemFastMsg fast{};  // yaw / yaw_rate corrected by the gyro scale (R19)
   proto::TelemSlowMsg slow{};
+  uint16_t yaw_raw = 0;  // R19: the Uno's own heading (0.01°), for diagnostics
 };
 
 enum class LinkEventKind : uint8_t {
@@ -120,6 +122,8 @@ class BodyLink {
   const HelloInfo& hello() const { return m_hello; }
   const SelfTestRecord& selfTest() const { return m_self_test; }
   const TankSnapshot& snapshot() const { return m_snap; }
+  bool setGyroScale(uint16_t scale_e4) { return m_heading.setScale(scale_e4); }  // R19; false: out of range
+  uint16_t gyroScale() const { return m_heading.scale(); }
   const ParserStats& parserStats() const { return m_parser.stats(); }
   const LinkStats& stats() const { return m_stats; }
   uint32_t lastRxMs() const { return m_last_rx_ms; }
@@ -175,6 +179,7 @@ class BodyLink {
   HelloInfo m_hello;
   SelfTestRecord m_self_test;
   TankSnapshot m_snap;
+  HeadingScale m_heading;  // R19
   LinkStats m_stats;
   static constexpr size_t kEventSlots = 16;
   LinkEvent m_events[kEventSlots] = {};
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_body_link`
Expected: PASS, 24 test cases.
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS (all suites; the scale defaults to 10000, so nothing else changes).

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/body_link.h firmware/esp32/src/core/body_link.cpp firmware/esp32/test/test_body_link/test_main.cpp
git commit -m "feat(bridge): R19 BodyLink corrects the heading by the gyro scale, keeps the raw one

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Setting the scale (calib, NVS, button)

**Files:**
- Modify: `firmware/esp32/src/core/ws_codec.h`, `ws_codec.cpp`, `ws_codec_keys.cpp`, `ws_json.h`, `ws_json_motion.cpp`, `confirm.h`
- Modify: `firmware/esp32/src/ports/motion_store.h`, `firmware/esp32/src/net/motion_store.{h,cpp}`, `firmware/esp32/src/net/ports_esp32.cpp`, `firmware/esp32/src/net/link_task.cpp`, `firmware/esp32/src/fakes/fake_ports.{h,cpp}`
- Modify: `firmware/esp32/src/app/motion_controller.{h,cpp}`, `firmware/esp32/src/app/settings_controller.{h,cpp}`
- Test: `firmware/esp32/test/test_ws_codec`, `test_ws_json`, `test_settings_controller`, `test_motion_controller`

**Interfaces:**
- Consumes (Task 2): `BodyLink::setGyroScale` / `gyroScale`; (Task 1) `kGyroScale*`.
- Produces:
  - `WsCommand::has_gyro_scale` / `gyro_scale`; `calib_reset` 2 = `"gyro_scale"`
  - `PendingChange::gyro` (`calib_what` 2)
  - `jsonCalib(out, cap, cpm, roll_mm, gyro_scale)`
  - `ports::loadGyroScale` / `saveGyroScale`; `fakes::World::gyro` (10000) / `saved_gyro`
  - `MotionController::setGyroScale(uint16_t)`
  - `SettingsController::Hooks::set_gyro_scale` (last member, `nullptr` default)

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/firmware/esp32/test/test_motion_controller/test_main.cpp b/firmware/esp32/test/test_motion_controller/test_main.cpp
index 5e14fd6..b396802 100644
--- a/firmware/esp32/test/test_motion_controller/test_main.cpp
+++ b/firmware/esp32/test/test_motion_controller/test_main.cpp
@@ -194,6 +194,18 @@ void test_a_cancelled_turn_learns_nothing() {
   TEST_ASSERT_EQUAL(60, mc->coastMs());
 }
 
+// R19: the saved gyro scale reaches the heading correction at begin, and the calib reply shows it.
+void test_begin_loads_the_gyro_scale_and_calib_shows_it() {
+  fakes::world().gyro = 10230;
+  mc->begin();
+  TEST_ASSERT_EQUAL(10230, link->gyroScale());
+  mc->sendCalib(3);
+  TEST_ASSERT_TRUE(has(3, "\"gyro_scale\":10230"));
+  TEST_ASSERT_TRUE(mc->setGyroScale(10100));
+  TEST_ASSERT_EQUAL(10100, link->gyroScale());
+  TEST_ASSERT_FALSE(mc->setGyroScale(20000));
+}
+
 void test_calib_reply_has_counts_and_roll_diagnostics() {
   mc->sendCalib(3);
   const std::string j = fakes::lastTo(3);
@@ -300,6 +312,7 @@ int main() {
   RUN_TEST(test_cancel_by_another_client_is_not_owner);
   RUN_TEST(test_a_done_turn_saves_the_learned_coast);
   RUN_TEST(test_a_cancelled_turn_learns_nothing);
+  RUN_TEST(test_begin_loads_the_gyro_scale_and_calib_shows_it);
   RUN_TEST(test_calib_reply_has_counts_and_roll_diagnostics);
   RUN_TEST(test_logo_move_starts_without_a_reply_and_its_end_reaches_the_program);
   RUN_TEST(test_a_page_s_own_primitive_end_does_not_reach_the_program);
diff --git a/firmware/esp32/test/test_settings_controller/test_main.cpp b/firmware/esp32/test/test_settings_controller/test_main.cpp
index 1ce1107..80d93ea 100644
--- a/firmware/esp32/test/test_settings_controller/test_main.cpp
+++ b/firmware/esp32/test/test_settings_controller/test_main.cpp
@@ -25,7 +25,7 @@ int g_driver = -1;
 int g_calib_sent = 0;
 std::vector<core::AckedOp> g_ops;
 size_t g_free = 8;  // free request slots the fake queue reports
-uint16_t g_cpm = 7514, g_roll = 27, g_coast = 80;
+uint16_t g_cpm = 7514, g_roll = 27, g_coast = 80, g_gyro = 0;
 
 core::WsCommand P(const char* s) { return core::parseWsCommand(s, strlen(s)); }
 bool has(int fd, const char* needle) { return fakes::countTo(fd, needle) > 0; }
@@ -47,6 +47,7 @@ void setUp() {
   g_cpm = 7514;
   g_roll = 27;
   g_coast = 80;
+  g_gyro = 0;
   sc->setHooks({[] { return g_active; }, [] { return g_ota; }, [] { return g_driver; },
                      [](int) { ++g_calib_sent; },
                      [](const core::AckedOp& op) {
@@ -55,7 +56,8 @@ void setUp() {
                        g_ops.push_back(op);
                        return true;
                      },
-                     &g_cpm, &g_roll, [] { return g_free; }, &g_coast});
+                     &g_cpm, &g_roll, [] { return g_free; }, &g_coast,
+                     [](uint16_t v) { g_gyro = v; return true; }});
 }
 void tearDown() {}
 
@@ -152,6 +154,27 @@ void test_calib_reset_roll_also_resets_the_coast() {
   TEST_ASSERT_EQUAL(core::motion::kCoastDefaultMs, fakes::world().saved_coast[0]);
 }
 
+// R19: the gyro scale is set like counts_per_m (driver only, not while moving, the tank's button) and saved;
+// the reset goes back to no correction; both hand the value on to the heading correction.
+void test_calib_gyro_scale_set_and_reset() {
+  sc->onCalib(3, 1, P("{\"t\":\"calib\",\"gyro_scale\":10230,\"id\":1}"), 1000);
+  TEST_ASSERT_TRUE(has(3, "NOT_OWNER"));
+  g_driver = 3;
+  sc->onCalib(3, 2, P("{\"t\":\"calib\",\"gyro_scale\":10230,\"id\":2}"), 1000);
+  TEST_ASSERT_TRUE(has(3, "\"t\":\"confirm\""));
+  TEST_ASSERT_EQUAL(0, g_gyro);  // nothing before the button
+  sc->onButtonPress(2000);
+  TEST_ASSERT_EQUAL(10230, g_gyro);
+  TEST_ASSERT_EQUAL(1, static_cast<int>(fakes::world().saved_gyro.size()));
+  TEST_ASSERT_EQUAL(10230, fakes::world().saved_gyro[0]);
+  TEST_ASSERT_EQUAL(7514, g_cpm);  // counts_per_m untouched
+  sc->onCalib(3, 3, P("{\"t\":\"calib\",\"reset\":\"gyro_scale\",\"id\":3}"), 3000);
+  sc->onButtonPress(4000);
+  TEST_ASSERT_EQUAL(10000, g_gyro);
+  TEST_ASSERT_EQUAL(10000, fakes::world().saved_gyro[1]);
+  TEST_ASSERT_EQUAL(27, g_roll);  // the roll-on untouched
+}
+
 void test_calib_read_sends_calib() {
   sc->onCalib(3, -1, P("{\"t\":\"calib\"}"), 1000);
   TEST_ASSERT_EQUAL(1, g_calib_sent);
@@ -193,6 +216,7 @@ int main() {
   RUN_TEST(test_calib_set_only_for_the_driver_and_not_while_moving);
   RUN_TEST(test_calib_save_failure_is_busy);
   RUN_TEST(test_calib_read_sends_calib);
+  RUN_TEST(test_calib_gyro_scale_set_and_reset);
   RUN_TEST(test_calib_reset_roll_also_resets_the_coast);
   RUN_TEST(test_wifi_mode_result_is_logged);
   RUN_TEST(test_restart_is_acked_logged_and_due_after_500_ms);
diff --git a/firmware/esp32/test/test_ws_codec/test_main.cpp b/firmware/esp32/test/test_ws_codec/test_main.cpp
index f2d5a40..da24270 100644
--- a/firmware/esp32/test/test_ws_codec/test_main.cpp
+++ b/firmware/esp32/test/test_ws_codec/test_main.cpp
@@ -324,6 +324,21 @@ void test_wifi_commands() {
   TEST_ASSERT_EQUAL(K(WsCmdKind::kRestart), K(P("{\"t\":\"restart\"}").kind));
 }
 
+// R19: the gyro scale - set (9000..11000, whole numbers) and reset.
+void test_calib_gyro_scale() {
+  WsCommand c = P("{\"t\":\"calib\",\"gyro_scale\":10230,\"id\":4}");
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(c.kind));
+  TEST_ASSERT_TRUE(c.has_gyro_scale);
+  TEST_ASSERT_FALSE(c.has_cpm);
+  TEST_ASSERT_EQUAL_UINT16(10230, c.gyro_scale);
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(P("{\"t\":\"calib\",\"gyro_scale\":9000}").kind));
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kCalib), K(P("{\"t\":\"calib\",\"gyro_scale\":11000}").kind));
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"gyro_scale\":8999}").kind));
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"gyro_scale\":11001}").kind));
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"gyro_scale\":10230.5}").kind));
+  TEST_ASSERT_EQUAL_INT8(2, P("{\"t\":\"calib\",\"reset\":\"gyro_scale\"}").calib_reset);
+}
+
 void test_setting_and_calib_reset() {
   WsCommand c = P("{\"t\":\"setting\",\"setting_id\":66}");
   TEST_ASSERT_EQUAL(K(WsCmdKind::kSetting), K(c.kind));
@@ -457,6 +472,7 @@ int main() {
   RUN_TEST(test_turn_pivot_field);
   RUN_TEST(test_wifi_commands);
   RUN_TEST(test_setting_and_calib_reset);
+  RUN_TEST(test_calib_gyro_scale);
   RUN_TEST(test_tank_log_selftest);
   RUN_TEST(test_camera_set_fields_and_ranges);
   RUN_TEST(test_camera_set_false_turns_a_switch_off);
diff --git a/firmware/esp32/test/test_ws_json/test_main.cpp b/firmware/esp32/test/test_ws_json/test_main.cpp
index 4249c5a..5546dc6 100644
--- a/firmware/esp32/test/test_ws_json/test_main.cpp
+++ b/firmware/esp32/test/test_ws_json/test_main.cpp
@@ -394,8 +394,8 @@ void test_tf_move_progress_and_calib_reply() {
   s.fast_valid = true;
   jsonTf(buf, sizeof(buf), 1, s, PrimitiveView{true, PrimKind::kMove, 42});
   TEST_ASSERT_NOT_NULL(strstr(buf, "\"primitive\":{\"kind\":\"move\",\"pct\":42}"));
-  jsonCalib(buf, sizeof(buf), 7480, 25);
-  TEST_ASSERT_EQUAL_STRING("{\"t\":\"calib\",\"counts_per_m\":7480,\"roll_mm\":25}", buf);
+  jsonCalib(buf, sizeof(buf), 7480, 25, 10230);
+  TEST_ASSERT_EQUAL_STRING("{\"t\":\"calib\",\"counts_per_m\":7480,\"roll_mm\":25,\"gyro_scale\":10230}", buf);
 }
 
 void test_tf_primitive_limited() {
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_codec -f test_ws_json -f test_settings_controller -f test_motion_controller`
Expected: FAIL to compile — `no member named 'has_gyro_scale' in 'cq::core::WsCommand'`, `no member named 'gyro' / 'saved_gyro' in 'cq::fakes::World'`, `no member named 'setGyroScale' in 'cq::app::MotionController'`, `excess elements in struct initializer`.

- [ ] **Step 3: Implement**

```diff
diff --git a/firmware/esp32/src/app/motion_controller.cpp b/firmware/esp32/src/app/motion_controller.cpp
index 716924e..40fd5e7 100644
--- a/firmware/esp32/src/app/motion_controller.cpp
+++ b/firmware/esp32/src/app/motion_controller.cpp
@@ -31,6 +31,7 @@ void MotionController::begin() {
   m_cpm = ports::loadCpm();  // Plan K
   m_roll_mm = ports::loadRoll();  // spec 02 §G6
   m_coast_ms = ports::loadCoast();  // bridge LLD R18.2
+  m_link.setGyroScale(ports::loadGyroScale());  // R19: corrects the heading from the next frame
 }
 
 bool MotionController::encoderFault() const {
@@ -272,7 +273,7 @@ void MotionController::tick(uint32_t now) { run(m_motion.tick(now), now); }
 
 void MotionController::sendCalib(int fd) {
   char* b = m_out.buf();
-  size_t n = core::jsonCalib(b, m_out.cap() - 64, m_cpm, m_roll_mm);
+  size_t n = core::jsonCalib(b, m_out.cap() - 64, m_cpm, m_roll_mm, m_link.gyroScale());
   if (n > 1) {  // + roll-on learner diagnostics (spec 02 §G6 floor check): replace the closing brace
     const int k = snprintf(b + n - 1, m_out.cap() - n + 1, ",\"roll_last\":%ld,\"roll_watched\":%u,\"roll_read\":%u}",
                            static_cast<long>(m_roll_last), static_cast<unsigned>(m_roll_begun),
diff --git a/firmware/esp32/src/app/motion_controller.h b/firmware/esp32/src/app/motion_controller.h
index 1fc692e..98c01fb 100644
--- a/firmware/esp32/src/app/motion_controller.h
+++ b/firmware/esp32/src/app/motion_controller.h
@@ -51,6 +51,7 @@ class MotionController {
   uint16_t& cpm() { return m_cpm; }
   uint16_t& rollMm() { return m_roll_mm; }
   uint16_t& coastMs() { return m_coast_ms; }  // R18.2
+  bool setGyroScale(uint16_t scale_e4) { return m_link.setGyroScale(scale_e4); }  // R19
   void sendCalib(int fd);
   core::PrimitiveView view() const;
 
diff --git a/firmware/esp32/src/app/settings_controller.cpp b/firmware/esp32/src/app/settings_controller.cpp
index 68e84d6..d2e842d 100644
--- a/firmware/esp32/src/app/settings_controller.cpp
+++ b/firmware/esp32/src/app/settings_controller.cpp
@@ -4,6 +4,7 @@
 #include <string.h>
 
 #include "board_config.h"
+#include "core/heading_scale.h"
 #include "core/motion_config.h"
 #include "core/wifi_policy.h"
 #include "ports/log_out.h"
@@ -172,11 +173,13 @@ void SettingsController::onCalib(int fd, int32_t id, const core::WsCommand& c, u
     ch.calib_what = static_cast<uint8_t>(c.calib_reset);
     return askConfirm(ch, now);
   }
-  if (!c.has_cpm) return m_h.send_calib(fd);
+  if (!c.has_cpm && !c.has_gyro_scale) return m_h.send_calib(fd);
   if (m_h.driver_fd() != fd) return m_out.nack(fd, id, core::BridgeReason::kNotOwner);  // Plan O (§91)
   if (m_h.motion_active()) return m_out.nack(fd, id, core::BridgeReason::kBusy);
   ch.kind = core::ChangeKind::kCalibSet;
+  ch.calib_what = c.has_gyro_scale ? 2 : 0;  // R19: one value per message, the gyro scale first
   ch.cpm = c.cpm;
+  ch.gyro = c.gyro_scale;
   askConfirm(ch, now);
 }
 
@@ -191,6 +194,7 @@ void SettingsController::applyWifi(core::PendingChange& ch) {
 
 // Plan O: checked again now - no primitive may be running (the save itself is not motion).
 void SettingsController::applyCalibSet(const core::PendingChange& ch) {
+  if (ch.calib_what == 2) return applyGyroScale(ch, ch.gyro);
   if (m_h.motion_active() || !ports::saveCpm(ch.cpm)) {
     m_out.nack(ch.fd, ch.id, core::BridgeReason::kBusy);
     return;
@@ -200,7 +204,19 @@ void SettingsController::applyCalibSet(const core::PendingChange& ch) {
   m_h.send_calib(ch.fd);
 }
 
+// R19: save the gyro scale and hand it to the heading correction (set, or reset to no correction).
+void SettingsController::applyGyroScale(const core::PendingChange& ch, uint16_t scale_e4) {
+  if (m_h.motion_active() || !ports::saveGyroScale(scale_e4)) {
+    m_out.nack(ch.fd, ch.id, core::BridgeReason::kBusy);
+    return;
+  }
+  if (m_h.set_gyro_scale != nullptr) m_h.set_gyro_scale(scale_e4);
+  m_out.to(ch.fd, core::jsonConfirmed(m_out.buf(), m_out.cap(), core::changeName(ch.kind)));
+  m_h.send_calib(ch.fd);
+}
+
 void SettingsController::applyCalibReset(const core::PendingChange& ch) {
+  if (ch.calib_what == 2) return applyGyroScale(ch, core::kGyroScaleDefault);
   if (ch.calib_what == 0) {
     *m_h.cpm = core::motion::kCpmDefault;
     if (!ports::saveCpm(*m_h.cpm)) ports::logLine("calib: counts_per_m reset not saved (NVS)");
diff --git a/firmware/esp32/src/app/settings_controller.h b/firmware/esp32/src/app/settings_controller.h
index 70ec970..f1f4b0f 100644
--- a/firmware/esp32/src/app/settings_controller.h
+++ b/firmware/esp32/src/app/settings_controller.h
@@ -27,6 +27,7 @@ class SettingsController {
     uint16_t* roll_mm;  // learned roll-on in use
     size_t (*free_slots)();  // free Uno request slots (shared with the phones' requests, R15.5)
     uint16_t* coast_ms = nullptr;  // R18.2: learned turn coast in use (reset with the roll-on)
+    bool (*set_gyro_scale)(uint16_t) = nullptr;  // R19: hand a saved gyro scale to the heading correction
   };
 
   SettingsController(Replies& out, core::EventLog& log, const core::BodyLink& link)
@@ -51,6 +52,7 @@ class SettingsController {
   void applyWifi(core::PendingChange& ch);
   void applyCalibSet(const core::PendingChange& ch);
   void applyCalibReset(const core::PendingChange& ch);
+  void applyGyroScale(const core::PendingChange& ch, uint16_t scale_e4);
   void applySetting(const core::PendingChange& ch);
   void onWifiResult(const core::WifiResult& res, uint32_t now);
 
diff --git a/firmware/esp32/src/core/confirm.h b/firmware/esp32/src/core/confirm.h
index 06bdc33..748b7af 100644
--- a/firmware/esp32/src/core/confirm.h
+++ b/firmware/esp32/src/core/confirm.h
@@ -18,8 +18,9 @@ struct PendingChange {
   char pass[64] = "";   // wifi_save / ap_password - never echoed
   uint8_t setting_id = 0;
   int32_t value = 0;
-  uint8_t calib_what = 0;  // 0 counts_per_m, 1 roll
+  uint8_t calib_what = 0;  // 0 counts_per_m, 1 roll, 2 gyro_scale (R19)
   uint16_t cpm = 0;        // kCalibSet (Plan O): the counts per metre to save
+  uint16_t gyro = 0;       // kCalibSet with calib_what 2 (R19): the gyro scale to save
 };
 
 const char* changeName(ChangeKind k);
diff --git a/firmware/esp32/src/core/ws_codec.cpp b/firmware/esp32/src/core/ws_codec.cpp
index 8607f21..e85e845 100644
--- a/firmware/esp32/src/core/ws_codec.cpp
+++ b/firmware/esp32/src/core/ws_codec.cpp
@@ -97,7 +97,10 @@ bool checkApPassword(WsCommand&, const Seen& s) { return s.password; }
 bool checkSetting(WsCommand&, const Seen& s) { return s.setting_id; }
 bool checkCalib(WsCommand& c, const Seen& s) {
   if (s.reset[0] == '\0') return true;
-  c.calib_reset = strcmp(s.reset, "counts_per_m") == 0 ? 0 : (strcmp(s.reset, "roll") == 0 ? 1 : -1);
+  c.calib_reset = strcmp(s.reset, "counts_per_m") == 0 ? 0
+                  : strcmp(s.reset, "roll") == 0         ? 1
+                  : strcmp(s.reset, "gyro_scale") == 0   ? 2
+                                                         : -1;
   return c.calib_reset >= 0;
 }
 bool checkDriveRate(WsCommand&, const Seen& s) { return s.v && s.mf.rate; }  // review M20
diff --git a/firmware/esp32/src/core/ws_codec.h b/firmware/esp32/src/core/ws_codec.h
index ac32458..db81d14 100644
--- a/firmware/esp32/src/core/ws_codec.h
+++ b/firmware/esp32/src/core/ws_codec.h
@@ -85,10 +85,12 @@ struct WsCommand {
   uint8_t setting_id = 0;  // setting
   bool has_value = false;
   int32_t value = 0;
-  int8_t calib_reset = -1;  // calib: 0 counts_per_m, 1 roll
+  int8_t calib_reset = -1;  // calib: 0 counts_per_m, 1 roll, 2 gyro_scale (R19)
   int16_t move_cm = 0;        // move: ±1..500 (+ forward)
   bool has_cpm = false;       // calib: set (else read)
   uint16_t cpm = 0;           // calib: counts per metre
+  bool has_gyro_scale = false;  // R19 calib: set the gyro scale
+  uint16_t gyro_scale = 0;      // R19: parts per 10 000 (10230 = x1.0230)
   uint32_t log_since = 0;     // log (Plan O)
   CamFields cam;              // camera_set
   char ai_url[97] = "";       // app_set ("" clears it)
diff --git a/firmware/esp32/src/core/ws_codec_keys.cpp b/firmware/esp32/src/core/ws_codec_keys.cpp
index 641c14e..ab1de00 100644
--- a/firmware/esp32/src/core/ws_codec_keys.cpp
+++ b/firmware/esp32/src/core/ws_codec_keys.cpp
@@ -2,6 +2,7 @@
 #include <math.h>
 #include <string.h>
 
+#include "core/heading_scale.h"
 #include "core/motion_config.h"
 #include "core/ws_codec_internal.h"
 
@@ -214,6 +215,13 @@ bool readCpm(Reader& r, WsCommand& c, Seen&) {  // calib set
   c.cpm = static_cast<uint16_t>(n);
   return true;
 }
+bool readGyroScale(Reader& r, WsCommand& c, Seen&) {  // R19 calib set
+  double n = 0;
+  if (!wholeIn(r, kGyroScaleMin, kGyroScaleMax, n)) return false;
+  c.has_gyro_scale = true;
+  c.gyro_scale = static_cast<uint16_t>(n);
+  return true;
+}
 
 bool readProg(Reader& r, WsCommand& c, Seen& s) {  // logo_run (Plan N3): 1..2^32-1
   double n = 0;
@@ -242,7 +250,7 @@ const KeyRow kKeys[] = {
     {"value", readValue},   {"reset", readReset},     {"since", readSince},     {"size", readSize},
     {"quality", readQuality}, {"fps_cap", readFpsCap}, {"brightness", readBrightness},
     {"contrast", readContrast}, {"low_light", readLowLight}, {"vflip", readVflip}, {"hmirror", readHmirror},
-    {"ai_url", readAiUrl},  {"setting_id", readSettingId}, {"counts_per_m", readCpm}, {"prog", readProg},
+    {"ai_url", readAiUrl},  {"setting_id", readSettingId}, {"counts_per_m", readCpm}, {"gyro_scale", readGyroScale}, {"prog", readProg},
     {"step", readStep},
 };
 
diff --git a/firmware/esp32/src/core/ws_json.h b/firmware/esp32/src/core/ws_json.h
index 16e6a2c..d43ccd3 100644
--- a/firmware/esp32/src/core/ws_json.h
+++ b/firmware/esp32/src/core/ws_json.h
@@ -51,7 +51,7 @@ size_t jsonTf(char* out, size_t cap, uint32_t seq, const TankSnapshot& s, Primit
 size_t jsonPrimitiveDone(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const PrimitiveEnd& e);
 size_t jsonTs(char* out, size_t cap, const TankSnapshot& s, OwnerView owner, bool body_offline);
 // Plan K (LLD R7.1 / R7.9): {"t":"calib","counts_per_m":n,"roll_mm":m} - reply to calib (read or after a save).
-size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m, uint16_t roll_mm);
+size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m, uint16_t roll_mm, uint16_t gyro_scale);  // + R19
 // Plan M (spec 02 §I3): WiFi status / scan, the confirmation replies, Uno settings. Never a password.
 struct WifiStatusView {
   const char* mode;  // "home" / "connecting" / "ap_fallback" / "ap"
diff --git a/firmware/esp32/src/core/ws_json_motion.cpp b/firmware/esp32/src/core/ws_json_motion.cpp
index 0fd1432..178601e 100644
--- a/firmware/esp32/src/core/ws_json_motion.cpp
+++ b/firmware/esp32/src/core/ws_json_motion.cpp
@@ -70,9 +70,9 @@ size_t jsonPrimitiveDone(char* out, size_t cap, uint32_t seq, uint32_t tank_ms,
              e.ok ? "true" : "false", r < sizeof(kReasons) / sizeof(kReasons[0]) ? kReasons[r] : "done", moved, id);
 }
 
-size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m, uint16_t roll_mm) {
-  return fmt(out, cap, "{\"t\":\"calib\",\"counts_per_m\":%u,\"roll_mm\":%u}", static_cast<unsigned>(counts_per_m),
-             static_cast<unsigned>(roll_mm));
+size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m, uint16_t roll_mm, uint16_t gyro_scale) {
+  return fmt(out, cap, "{\"t\":\"calib\",\"counts_per_m\":%u,\"roll_mm\":%u,\"gyro_scale\":%u}",
+             static_cast<unsigned>(counts_per_m), static_cast<unsigned>(roll_mm), static_cast<unsigned>(gyro_scale));
 }
 
 }  // namespace core
diff --git a/firmware/esp32/src/fakes/fake_ports.cpp b/firmware/esp32/src/fakes/fake_ports.cpp
index dcc2a58..a909e68 100644
--- a/firmware/esp32/src/fakes/fake_ports.cpp
+++ b/firmware/esp32/src/fakes/fake_ports.cpp
@@ -74,6 +74,13 @@ bool saveCoast(uint16_t v) {
   world().coast = v;
   return true;
 }
+uint16_t loadGyroScale() { return world().gyro; }
+bool saveGyroScale(uint16_t v) {
+  if (!world().save_ok) return false;
+  world().saved_gyro.push_back(v);
+  world().gyro = v;
+  return true;
+}
 bool loadAiUrl(char* out, size_t cap) {
   snprintf(out, cap, "%s", world().ai_url.c_str());
   return true;
diff --git a/firmware/esp32/src/fakes/fake_ports.h b/firmware/esp32/src/fakes/fake_ports.h
index 53c4525..d832a36 100644
--- a/firmware/esp32/src/fakes/fake_ports.h
+++ b/firmware/esp32/src/fakes/fake_ports.h
@@ -25,10 +25,12 @@ struct World {
   uint16_t cpm = 7514;
   uint16_t roll = 27;
   uint16_t coast = 60;
+  uint16_t gyro = 10000;
   bool save_ok = true;
   std::vector<uint16_t> saved_cpm;
   std::vector<uint16_t> saved_roll;
   std::vector<uint16_t> saved_coast;
+  std::vector<uint16_t> saved_gyro;
   std::string ai_url;
   std::vector<core::WifiRequest> wifi_posted;
   bool wifi_post_ok = true;
diff --git a/firmware/esp32/src/net/link_task.cpp b/firmware/esp32/src/net/link_task.cpp
index 3c5690c..f6e78c5 100644
--- a/firmware/esp32/src/net/link_task.cpp
+++ b/firmware/esp32/src/net/link_task.cpp
@@ -200,7 +200,8 @@ void wireSettingsAndTank() {
   g_settings.setHooks({[] { return g_motion.active(); }, [] { return g_drive.otaActive(); },
                        [] { return g_owner.driverFd(); }, [](int fd) { g_motion.sendCalib(fd); },
                        [](const core::AckedOp& op) { return g_uno.request(op); }, &g_motion.cpm(), &g_motion.rollMm(),
-                       [] { return g_uno.freeSlots(); }, &g_motion.coastMs()});
+                       [] { return g_uno.freeSlots(); }, &g_motion.coastMs(),
+                       [](uint16_t v) { return g_motion.setGyroScale(v); }});
   g_tank.setSelfTestHooks({[] { return !g_link.synced(); }, [] { return !standingStill(); }, queueSelfTest});
 }
 
diff --git a/firmware/esp32/src/net/motion_store.cpp b/firmware/esp32/src/net/motion_store.cpp
index a6ba225..8ff6c50 100644
--- a/firmware/esp32/src/net/motion_store.cpp
+++ b/firmware/esp32/src/net/motion_store.cpp
@@ -2,6 +2,7 @@
 
 #include <Preferences.h>
 
+#include "core/heading_scale.h"
 #include "core/motion_config.h"
 #include "net/log.h"
 
@@ -72,5 +73,26 @@ bool motionStoreSaveCoast(uint16_t ms) {
   return ok;
 }
 
+uint16_t motionStoreLoadGyroScale() {
+  uint16_t v = core::kGyroScaleDefault;
+  Preferences p;
+  if (p.begin("cq_motion", true)) {
+    const uint16_t saved = p.getUShort("gyro_scale", 0xFFFF);
+    if (saved >= core::kGyroScaleMin && saved <= core::kGyroScaleMax) v = saved;
+    p.end();
+  }
+  logf("motion: gyro scale %u", static_cast<unsigned>(v));
+  return v;
+}
+
+bool motionStoreSaveGyroScale(uint16_t scale_e4) {
+  if (scale_e4 < core::kGyroScaleMin || scale_e4 > core::kGyroScaleMax) return false;
+  Preferences p;
+  if (!p.begin("cq_motion", false)) return false;
+  const bool ok = p.putUShort("gyro_scale", scale_e4) == sizeof(uint16_t);
+  p.end();
+  return ok;
+}
+
 }  // namespace net
 }  // namespace cq
diff --git a/firmware/esp32/src/net/motion_store.h b/firmware/esp32/src/net/motion_store.h
index 732e31d..e90cb7b 100644
--- a/firmware/esp32/src/net/motion_store.h
+++ b/firmware/esp32/src/net/motion_store.h
@@ -12,6 +12,8 @@ uint16_t motionStoreLoadRoll();         // spec 02 §G6: learned roll-on (mm), d
 bool motionStoreSaveRoll(uint16_t mm);
 uint16_t motionStoreLoadCoast();        // bridge LLD R18.2: learned turn coast (ms), default 150, 20..400
 bool motionStoreSaveCoast(uint16_t ms);
+uint16_t motionStoreLoadGyroScale();    // bridge LLD R19: gyro scale (1e-4), default 10000, 9000..11000
+bool motionStoreSaveGyroScale(uint16_t scale_e4);
 
 }  // namespace net
 }  // namespace cq
diff --git a/firmware/esp32/src/net/ports_esp32.cpp b/firmware/esp32/src/net/ports_esp32.cpp
index c692844..7388d8f 100644
--- a/firmware/esp32/src/net/ports_esp32.cpp
+++ b/firmware/esp32/src/net/ports_esp32.cpp
@@ -37,6 +37,8 @@ uint16_t loadRoll() { return net::motionStoreLoadRoll(); }
 bool saveRoll(uint16_t v) { return net::motionStoreSaveRoll(v); }
 uint16_t loadCoast() { return net::motionStoreLoadCoast(); }
 bool saveCoast(uint16_t v) { return net::motionStoreSaveCoast(v); }
+uint16_t loadGyroScale() { return net::motionStoreLoadGyroScale(); }
+bool saveGyroScale(uint16_t v) { return net::motionStoreSaveGyroScale(v); }
 
 bool loadAiUrl(char* out, size_t cap) { return net::appAiUrlLoad(out, cap); }
 bool saveAiUrl(const char* url) { return net::appAiUrlSave(url); }
diff --git a/firmware/esp32/src/ports/motion_store.h b/firmware/esp32/src/ports/motion_store.h
index dcb8de3..44f61f8 100644
--- a/firmware/esp32/src/ports/motion_store.h
+++ b/firmware/esp32/src/ports/motion_store.h
@@ -12,6 +12,8 @@ uint16_t loadRoll();        // mm
 bool saveRoll(uint16_t v);
 uint16_t loadCoast();       // R18.2: learned turn coast (ms)
 bool saveCoast(uint16_t v);
+uint16_t loadGyroScale();   // R19: parts per 10 000, 10000 when missing / out of range
+bool saveGyroScale(uint16_t v);
 
 }  // namespace ports
 }  // namespace cq
```

- [ ] **Step 4: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS, 614 test cases.
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge`
Expected: SUCCESS (flash ~44.3 %).
Run: `uv run --with lizard==1.24.1 python scripts/size_check.py`
Expected: no output, exit 0.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src firmware/esp32/test/test_ws_codec firmware/esp32/test/test_ws_json firmware/esp32/test/test_settings_controller firmware/esp32/test/test_motion_controller
git commit -m "feat(bridge): R19 gyro scale in NVS, set and reset through calib with the tank's button

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: `move.py --gyro-scale`

**Files:**
- Modify: `tools/smoke/move.py`
- Test: `tools/smoke/tests/test_move_math.py`

**Interfaces:**
- Consumes (Task 3): `{"t":"calib","gyro_scale":n,"id":m}`, answered by `confirm`, then `confirmed` / `confirm_expired`.
- Produces: `Session.save_calib(key, value)`, `Session.save_gyro_scale(scale_e4)`; the CLI `--gyro-scale N`.

- [ ] **Step 1: Write the failing test**

```diff
diff --git a/tools/smoke/tests/test_move_math.py b/tools/smoke/tests/test_move_math.py
index e6818bc..c533cf7 100644
--- a/tools/smoke/tests/test_move_math.py
+++ b/tools/smoke/tests/test_move_math.py
@@ -55,7 +55,8 @@ class FakeWs:
             if self.pending:
                 return self.pending.pop(0)
             for m in self.sent:
-                if m.get("t") == "calib" and "counts_per_m" in m and not m.get("_answered"):
+                saving = "counts_per_m" in m or "gyro_scale" in m
+                if m.get("t") == "calib" and saving and not m.get("_answered"):
                     m["_answered"] = True
                     end = "confirm_expired" if self.expire else "confirmed"
                     self.pending.append({"t": end, "what": "calib"})
@@ -114,3 +115,16 @@ def test_save_waits_for_the_tanks_button() -> None:  # Plan O (design-notes §91
 def test_save_not_confirmed_is_not_saved() -> None:
     s = Session(ws=FakeWs(expire=True))
     assert not s.save_counts_per_m(7675)
+
+
+def test_gyro_scale_save_sends_its_key_and_waits_for_the_button() -> None:  # bridge LLD R19
+    ws = FakeWs()
+    s = Session(ws=ws)
+    assert s.save_gyro_scale(10230)
+    calib = [m for m in ws.sent if m.get("t") == "calib"]
+    assert calib[-1]["gyro_scale"] == 10230
+    assert "counts_per_m" not in calib[-1]
+
+
+def test_gyro_scale_not_confirmed_is_not_saved() -> None:
+    assert not Session(ws=FakeWs(expire=True)).save_gyro_scale(10230)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd tools/smoke && uv run pytest -q tests/test_move_math.py`
Expected: FAIL — 2 failed (`'Session' object has no attribute 'save_gyro_scale'`), 9 passed.

- [ ] **Step 3: Implement**

```diff
diff --git a/tools/smoke/move.py b/tools/smoke/move.py
index a1f6404..b3d5cd0 100644
--- a/tools/smoke/move.py
+++ b/tools/smoke/move.py
@@ -3,6 +3,7 @@
     uv run python move.py 100 --speed 40      # forward 100 cm
     uv run python move.py -50                 # reverse 50 cm
     uv run python move.py --calibrate         # move 100 cm, type the tape distance, average, save
+    uv run python move.py --gyro-scale 10230  # save the gyro scale (LLD R19, the tank's button)
 
 Takes control (the move command does), so release the phone page first.
 STOP from the phone or the IR remote ends a move.
@@ -111,11 +112,18 @@ class Session:
         return int(msg["counts_per_m"])
 
     def save_counts_per_m(self, cpm: int) -> bool:
+        return self.save_calib("counts_per_m", cpm)
+
+    def save_gyro_scale(self, scale_e4: int) -> bool:
+        """Bridge LLD R19: the gyro scale in parts per 10 000 (10230 = x1.023)."""
+        return self.save_calib("gyro_scale", scale_e4)
+
+    def save_calib(self, key: str, value: int) -> bool:
         self.next_id += 1
         mid = self.next_id
         # Control is handed back 5 s after a move: take it again (drive 0/0 never moves the tank).
         self.ws.send({"t": "drive", "v": 0, "w": 0})
-        self.ws.send({"t": "calib", "counts_per_m": cpm, "id": mid})
+        self.ws.send({"t": "calib", key: value, "id": mid})
         # Plan O (design-notes §91): a save waits for a press of the tank's button (30 s).
         msg = self.pump_until(
             lambda m: (
@@ -206,15 +214,23 @@ def main() -> int:
     ap.add_argument("cm", type=int, nargs="?", help="distance, + forward, - reverse (1..500)")
     ap.add_argument("--speed", type=int, default=40)
     ap.add_argument("--calibrate", action="store_true")
+    ap.add_argument(
+        "--gyro-scale", type=int, default=None, help="save the gyro scale, 9000..11000 (R19)"
+    )
     ap.add_argument("--host", default="conqueror-bridge.local")
     args = ap.parse_args()
-    if not args.calibrate and args.cm is None:
-        ap.error("give a distance in cm, or --calibrate")
+    if not args.calibrate and args.cm is None and args.gyro_scale is None:
+        ap.error("give a distance in cm, --calibrate or --gyro-scale")
     s = Session(args.host)
     try:
+        if args.gyro_scale is not None:
+            return 0 if s.save_gyro_scale(args.gyro_scale) else 1
         if args.calibrate:
             return calibrate(s, args.speed)
-        done, counts, heading = s.move(args.cm, args.speed)
+        cm = args.cm
+        if cm is None:  # checked above; kept for the type checker
+            return 2
+        done, counts, heading = s.move(int(cm), args.speed)
         report(done, counts, heading, s.counts_per_m())
         return 0 if done.get("ok") else 1
     finally:
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd tools/smoke && uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: 21 passed; `All checks passed!`; `10 files already formatted`; `0 errors`.

- [ ] **Step 5: Commit**

```bash
git add tools/smoke/move.py tools/smoke/tests/test_move_math.py
git commit -m "feat(tools): move.py --gyro-scale saves the bridge's gyro scale (R19)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: On the tank — flash, set 10230, floor check (owner present)

**Safety:** a **floor** check, so the tank spins and drives. Ask "stand or floor?" before each drive and say what moves. Keep STOP within reach. Only the bridge is flashed; the Uno is not.

- [ ] **Step 1: Flash the bridge over the air**

```bash
cd firmware/esp32
export CQ_OTA_PASSWORD="$(sed -n 's/.*CQ_OTA_PASSWORD[^"]*"\([^"]*\)".*/\1/p' include/secrets.h)"
~/.platformio/penv/bin/pio run -e bridge_ota -t upload --upload-port 192.168.31.167 2>&1 | grep -vF -- "$CQ_OTA_PASSWORD" | tail -3
unset CQ_OTA_PASSWORD
```
Expected: `SUCCESS`.

- [ ] **Step 2: Save the scale**

Run: `cd tools/smoke && uv run python move.py --gyro-scale 10230 --host 192.168.31.167`, then the owner presses the tank's button.
Expected: `press the tank's button`, then exit 0; a `calib` read shows `"gyro_scale":10230`.

- [ ] **Step 3: The 4-turn spins against the strip (as design-notes §116)**

The owner lines the tank up and sends a start photo. Run `turn.py 720 --speed 40` twice; the owner sends the end photo. Repeat with `-720`.
Expected: each ends within ~5° of the strip, where before it was 39° (left) / 28° (right). The gyro totals now read ~1440° in true degrees.

- [ ] **Step 4: A Logo square**

`REPEAT 4 [FORWARD 50 RIGHT 90]`, with the owner's tape marks at the start.
Expected: back at its marks facing the start heading by eye (the corners now turn the true 90°). The turn coast re-learns over the first few turns.

---

### Task 6: Docs as built

**Files:** `docs/lld/esp32-bridge-lld.md` (R19.5 as built + floor results), `docs/design-notes.md` (the floor results with how they were read), `CLAUDE.md` (plan list, the Next line).

- [ ] **Step 1: Write R19.5 and the design-notes section;** update CLAUDE.md.
- [ ] **Step 2: Commit**

```bash
git add docs/lld/esp32-bridge-lld.md docs/design-notes.md CLAUDE.md
git commit -m "docs: Plan R2 gyro scale - as built and floor results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

