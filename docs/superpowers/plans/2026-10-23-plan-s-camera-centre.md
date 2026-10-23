# Plan S — Camera centre Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One saved "camera centre" (pan and tilt trim) on the Uno Lite, so HOME, the stick's return, the IR HOME key, Logo `LOOK` and `cam_set` all look truly straight ahead; set by eye on the Settings page.

**Architecture:**
- **Uno Lite:** gains the protocol's existing settings 27 `pan_trim` and 28 `tilt_trim`, saved in its EEPROM block. `CamLite` keeps logical angles (telemetry) and adds the trim only at the servo write.
- **Bridge:** lets the Settings page read both trims, try a trim live (`"try":true` → SETTING_SET only, the camera rule, no button) and save it (the tank's button → SET + SAVE).
- **Settings page:** the Camera card gets a "Camera centre" row.
- **Afterwards:** the AI calibration's `pan_map` goes back to (1, 0).

**Tech Stack:**
- C++ (Uno: AVR `uno` env, `-Os`, no heap; bridge: ESP32-S3 Arduino `bridge` / `bridge_ota`), PlatformIO `native` Unity host tests;
- plain JS in `web/settings.html` with a Playwright check;
- the AI engine YAML config.

**Spec:**
- `docs/lld/uno-lite-lld.md` §22 (approved 2026-10-23);
- `docs/lld/esp32-bridge-lld.md` R23 (approved 2026-10-23);
- design-notes §125 (b), §128.

**Built from a tested draft:** branch `s-draft`. Uno suite 297/297; bridge suite 664/664; Uno flash 31 344 B (+200 B); the page check passes.

## Global Constraints
- **Uno:**
  - no heap, no `virtual`, no `delay()`; domain code never includes `ports/` or `Arduino.h`;
  - Uno Lite flash ≤ 200 B more than 31 144 B (before);
  - every function's cyclomatic complexity ≤ 10 (the pre-commit `size_check`).
- **Protocol:** unchanged (version 3). Settings 27 / 28 are i16, 0.1° steps, −300…300, default 0. No generated file is edited.
- **Trims are in servo terms** (added to the servo angle): looking right or up is a smaller servo angle on this tank (design-notes §45).
- **Telemetry `pan` / `tilt` stay logical** (`CamLite::outPan()` / `outTilt()`).
- **A try changes RAM only and needs no button.** Only ids 27 / 28 can be tried, and only under the camera rule (only the driver while someone drives; BUSY during OTA or with a full queue).
- **Saving uses the tank's button,** as `track_balance` does.
- **The safety features are untouched.**
- **Hardware:**
  - no drive in this plan, only the camera moves (owner: "stand or floor?" still asked before flashing);
  - pictures of the home stay local and are deleted after the check;
  - never print the OTA password.

## Review Focus
1. **The tank's current EEPROM block** (the size from plan J, without trims) must load with `track_balance` 19 kept and the trims 0. Covered by `test_settings_block_before_the_trims_still_loads` (Task 1).
2. **A saved trim must apply from the first servo write after a restart.** `settingsLoad()` → `applySettings()` → `cam.setTrim` runs in `setup()` before the first `cameraTask`. Checked on the tank: restart, then HOME looks dead ahead (Task 4 Step 4).
3. **A refused step** (someone else drives) must not leave the page showing a trim the Uno never got. A refused or expired Save must not mark the trims saved when another change is confirmed later. Covered by `out.refused` / `out.resave` in `camera_centre_check.js` (Task 3).
4. **The servo's own 0…180° stop:** a logical angle near an end plus the trim must clamp, never wrap. Covered by `test_cam_trim_moves_only_the_servo` (Task 1).
5. **The AI calibration must not correct twice.** Once the trim is saved, telemetry 90° means dead ahead, so `pan_map` must go back to (1, 0) in the same session (Task 4 Step 6).

**Known and accepted:** `applySettings()` runs after every SETTING_SET, so any setting change wakes the camera servos for 2 s. It is harmless: they hold their position.

---

### Task 1: Uno Lite — the trims (settings, camera, servo write)

**Files:**
- Modify: `firmware/uno/src/domain/lite/settings_lite.h`, `firmware/uno/src/domain/lite/settings_lite.cpp`, `firmware/uno/src/domain/lite/cam_lite.h`, `firmware/uno/src/lite/lite_routes.cpp` (`applySettings`), `firmware/uno/src/lite/lite_ui.cpp` (`cameraTask`)
- Test: `firmware/uno/test/test_lite_settings/test_main.cpp`, `firmware/uno/test/test_lite_ui/test_main.cpp`

**Interfaces:**
- Consumes: `proto::SettingId::kPanTrim` (27) / `kTiltTrim` (28) from `generated/cq_protocol.h`.
- Produces:
  - `LiteSettings::pan_trim`, `tilt_trim` (int16, ×0.1°, at the end of the struct);
  - `SettingsLite::get` / `set` for 27 / 28 (−300…300, else `kSettingOutOfRange`);
  - `CamLite::setTrim(int16_t pan_dd, int16_t tilt_dd)`, `CamLite::servoPan()` / `servoTilt()` → int16 0…1800.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/firmware/uno/test/test_lite_settings/test_main.cpp b/firmware/uno/test/test_lite_settings/test_main.cpp
index 109c83f..02822e8 100644
--- a/firmware/uno/test/test_lite_settings/test_main.cpp
+++ b/firmware/uno/test/test_lite_settings/test_main.cpp
@@ -118,6 +118,48 @@ void test_settings_previous_block_still_loads() {
   TEST_ASSERT_EQUAL_INT16(0, r.values().track_balance);
 }
 
+// Uno Lite LLD §22: the camera centre trims (ids 27 / 28), ±30.0° in 0.1° steps, default 0.
+void test_settings_camera_trims() {
+  SettingsLite s;
+  s.defaults();
+  int32_t v = 1;
+  TEST_ASSERT_TRUE(s.get(id(proto::SettingId::kPanTrim), v));
+  TEST_ASSERT_EQUAL_INT32(0, v);
+  TEST_ASSERT_EQUAL(proto::AckStatus::kOk, s.set(id(proto::SettingId::kPanTrim), -50));
+  TEST_ASSERT_EQUAL(proto::AckStatus::kOk, s.set(id(proto::SettingId::kTiltTrim), 300));
+  TEST_ASSERT_TRUE(s.get(id(proto::SettingId::kPanTrim), v));
+  TEST_ASSERT_EQUAL_INT32(-50, v);
+  TEST_ASSERT_EQUAL_INT16(300, s.values().tilt_trim);
+  TEST_ASSERT_EQUAL(proto::AckStatus::kSettingOutOfRange, s.set(id(proto::SettingId::kPanTrim), -301));
+  TEST_ASSERT_EQUAL(proto::AckStatus::kSettingOutOfRange, s.set(id(proto::SettingId::kTiltTrim), 301));
+  uint8_t n = s.encode(g_mem);
+  TEST_ASSERT_EQUAL_UINT8(SettingsLite::kBlockSize, n);
+  SettingsLite r;
+  TEST_ASSERT_TRUE(r.load(rd));
+  TEST_ASSERT_EQUAL_INT16(-50, r.values().pan_trim);
+  TEST_ASSERT_EQUAL_INT16(300, r.values().tilt_trim);
+}
+
+// A block saved before the trims (plan J's size) still loads; the trims take their default 0.
+void test_settings_block_before_the_trims_still_loads() {
+  SettingsLite s;
+  s.defaults();
+  s.set(id(proto::SettingId::kTrackBalance), 19);
+  const uint8_t old_size = static_cast<uint8_t>(offsetof(LiteSettings, pan_trim));
+  uint8_t full[SettingsLite::kBlockSize];
+  s.encode(full);
+  g_mem[0] = SettingsLite::kMagic0;
+  g_mem[1] = SettingsLite::kMagic1;
+  g_mem[2] = old_size;
+  memcpy(g_mem + 3, full + 3, old_size);
+  g_mem[3 + old_size] = proto::crc8(g_mem + 2, static_cast<uint8_t>(old_size + 1));
+  SettingsLite r;
+  TEST_ASSERT_TRUE(r.load(rd));
+  TEST_ASSERT_EQUAL_INT16(19, r.values().track_balance);
+  TEST_ASSERT_EQUAL_INT16(0, r.values().pan_trim);
+  TEST_ASSERT_EQUAL_INT16(0, r.values().tilt_trim);
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_settings_defaults);
@@ -128,5 +170,7 @@ int main() {
   RUN_TEST(test_settings_out_of_range_block_gives_defaults);
   RUN_TEST(test_settings_track_balance_range);
   RUN_TEST(test_settings_previous_block_still_loads);
+  RUN_TEST(test_settings_camera_trims);
+  RUN_TEST(test_settings_block_before_the_trims_still_loads);
   return UNITY_END();
 }
diff --git a/firmware/uno/test/test_lite_ui/test_main.cpp b/firmware/uno/test/test_lite_ui/test_main.cpp
index 506ac06..3416fd8 100644
--- a/firmware/uno/test/test_lite_ui/test_main.cpp
+++ b/firmware/uno/test/test_lite_ui/test_main.cpp
@@ -131,6 +131,37 @@ void test_cam_small_jog_wakes_servos() {
   TEST_ASSERT_TRUE(c.wantAttached());
 }
 
+// Uno Lite LLD §22: the trim is added only at the servo; the logical angles (telemetry) stay as they are.
+void test_cam_trim_moves_only_the_servo() {
+  CamLite c;
+  c.setTrim(-50, 20);
+  for (int i = 0; i < 10; ++i) c.step();
+  TEST_ASSERT_EQUAL_INT16(900, c.outPan());
+  TEST_ASSERT_EQUAL_INT16(900, c.outTilt());
+  TEST_ASSERT_EQUAL_INT16(850, c.servoPan());
+  TEST_ASSERT_EQUAL_INT16(920, c.servoTilt());
+  c.set(30, 1100);
+  for (int i = 0; i < 60; ++i) c.step();
+  TEST_ASSERT_EQUAL_INT16(30, c.outPan());
+  TEST_ASSERT_EQUAL_INT16(0, c.servoPan());  // the servo's own 0..180° stop
+  TEST_ASSERT_EQUAL_INT16(1120, c.servoTilt());
+  c.setTrim(50, 0);
+  c.set(1800, 900);
+  for (int i = 0; i < 100; ++i) c.step();  // 1770 dd at 24 per step
+  TEST_ASSERT_EQUAL_INT16(1800, c.servoPan());
+}
+
+// A new trim wakes released servos, so the camera moves to the new centre at once.
+void test_cam_trim_wakes_servos() {
+  CamLite c;
+  c.tick(0);
+  c.tick(2000);
+  TEST_ASSERT_FALSE(c.wantAttached());
+  c.setTrim(-50, 0);
+  c.tick(2020);
+  TEST_ASSERT_TRUE(c.wantAttached());
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_cam_limits_and_us);
@@ -143,5 +174,7 @@ int main() {
   RUN_TEST(test_cam_joy_timeout_returns_home);
   RUN_TEST(test_cam_idle_detach_after_2s);
   RUN_TEST(test_cam_small_jog_wakes_servos);
+  RUN_TEST(test_cam_trim_moves_only_the_servo);
+  RUN_TEST(test_cam_trim_wakes_servos);
   return UNITY_END();
 }
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/uno && ~/.platformio/penv/bin/pio test -e native -f test_lite_settings -f test_lite_ui`
Expected: compile errors `no member named 'setTrim' in 'cq::CamLite'`, `no member named 'servoPan'`, and `kPanTrim` / `pan_trim` unknown.

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/firmware/uno/src/domain/lite/cam_lite.h b/firmware/uno/src/domain/lite/cam_lite.h
index 7510305..1cedf63 100644
--- a/firmware/uno/src/domain/lite/cam_lite.h
+++ b/firmware/uno/src/domain/lite/cam_lite.h
@@ -37,13 +37,27 @@ class CamLite {
   void step();
   int16_t outPan() const { return m_out_pan; }
   int16_t outTilt() const { return m_out_tilt; }
+  // The camera centre (Uno Lite LLD §22): added only at the servo, so pan()/outPan() (and telemetry) stay logical.
+  void setTrim(int16_t pan_dd, int16_t tilt_dd) {
+    m_pan_trim = pan_dd;
+    m_tilt_trim = tilt_dd;
+    m_active = true;  // wakes released servos: the camera moves to the new centre at once
+  }
+  int16_t servoPan() const { return servoOf(m_out_pan, m_pan_trim); }
+  int16_t servoTilt() const { return servoOf(m_out_tilt, m_tilt_trim); }
   static uint16_t toUs(int16_t dd) { return static_cast<uint16_t>(544 + static_cast<int32_t>(dd) * 1856 / 1800); }
 
  private:
+  static int16_t servoOf(int16_t out, int16_t trim) {
+    const int16_t v = static_cast<int16_t>(out + trim);
+    return v < 0 ? 0 : (v > 1800 ? 1800 : v);
+  }
   int16_t m_pan = kHome;
   int16_t m_tilt = kHome;
   int16_t m_out_pan = kHome;
   int16_t m_out_tilt = kHome;
+  int16_t m_pan_trim = 0;
+  int16_t m_tilt_trim = 0;
   int8_t m_jog_pan = 0;
   int8_t m_jog_tilt = 0;
   bool m_locked = false;
diff --git a/firmware/uno/src/domain/lite/settings_lite.cpp b/firmware/uno/src/domain/lite/settings_lite.cpp
index 3841e47..6c58065 100644
--- a/firmware/uno/src/domain/lite/settings_lite.cpp
+++ b/firmware/uno/src/domain/lite/settings_lite.cpp
@@ -8,6 +8,10 @@ namespace cq {
 namespace {
 using Id = proto::SettingId;
 bool inRange(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }
+// The block sizes saved so far: before plan J, before the camera trims, now. Older blocks end early.
+bool knownSize(uint8_t n) {
+  return n == sizeof(LiteSettings) || n == offsetof(LiteSettings, track_balance) || n == offsetof(LiteSettings, pan_trim);
+}
 }  // namespace
 
 void SettingsLite::defaults() {
@@ -25,13 +29,14 @@ void SettingsLite::defaults() {
   m_s.reverse_steer = 0;
   m_s.encoders_enabled = 0;
   m_s.track_balance = 0;
+  m_s.pan_trim = 0;
+  m_s.tilt_trim = 0;
 }
 
 bool SettingsLite::load(uint8_t (*read)(uint16_t)) {
   defaults();
   const uint8_t stored = read(kAddr + 2);
-  constexpr uint8_t kPrevSize = static_cast<uint8_t>(offsetof(LiteSettings, track_balance));  // before plan J
-  if (read(kAddr) != kMagic0 || read(kAddr + 1) != kMagic1 || (stored != sizeof(LiteSettings) && stored != kPrevSize)) {
+  if (read(kAddr) != kMagic0 || read(kAddr + 1) != kMagic1 || !knownSize(stored)) {
     return false;
   }
   uint8_t buf[sizeof(LiteSettings) + 1];
@@ -79,6 +84,8 @@ bool SettingsLite::get(uint8_t id, int32_t& v) const {
     case Id::kReverseSteer: v = m_s.reverse_steer; return true;
     case Id::kEncodersEnabled: v = m_s.encoders_enabled; return true;
     case Id::kTrackBalance: v = m_s.track_balance; return true;
+    case Id::kPanTrim: v = m_s.pan_trim; return true;
+    case Id::kTiltTrim: v = m_s.tilt_trim; return true;
     default: return false;
   }
 }
@@ -109,6 +116,8 @@ proto::AckStatus SettingsLite::set(uint8_t id, int32_t v) {
     case Id::kReverseSteer: if (!inRange(v, 0, 1)) return kRange; m_s.reverse_steer = static_cast<uint8_t>(v); return kOk;
     case Id::kEncodersEnabled: if (!inRange(v, 0, 1)) return kRange; m_s.encoders_enabled = static_cast<uint8_t>(v); return kOk;
     case Id::kTrackBalance: if (!inRange(v, -100, 100)) return kRange; m_s.track_balance = static_cast<int16_t>(v); return kOk;
+    case Id::kPanTrim: if (!inRange(v, -300, 300)) return kRange; m_s.pan_trim = static_cast<int16_t>(v); return kOk;
+    case Id::kTiltTrim: if (!inRange(v, -300, 300)) return kRange; m_s.tilt_trim = static_cast<int16_t>(v); return kOk;
     default: return proto::AckStatus::kUnavailable;
   }
 }
diff --git a/firmware/uno/src/domain/lite/settings_lite.h b/firmware/uno/src/domain/lite/settings_lite.h
index 97d6057..d307053 100644
--- a/firmware/uno/src/domain/lite/settings_lite.h
+++ b/firmware/uno/src/domain/lite/settings_lite.h
@@ -18,6 +18,8 @@ struct LiteSettings {
   uint8_t reverse_steer;
   uint8_t encoders_enabled;
   int16_t track_balance;  // ‰, spec 02 §F5.1 (plan J; older EEPROM blocks end before it)
+  int16_t pan_trim;       // ×0.1°, added at the servo (Uno Lite LLD §22; older blocks end before it)
+  int16_t tilt_trim;
 };
 
 // The Lite settings (spec 02 Part D): existing SettingId numbers, so the bridge and bench tools work unchanged.
diff --git a/firmware/uno/src/lite/lite_routes.cpp b/firmware/uno/src/lite/lite_routes.cpp
index 11e133a..6f091f4 100644
--- a/firmware/uno/src/lite/lite_routes.cpp
+++ b/firmware/uno/src/lite/lite_routes.cpp
@@ -204,6 +204,7 @@ void applySettings() {
   g_lite.encoders_enabled = s.encoders_enabled != 0;
   g_lite.gyro_zero.seed(s.gyro_bias_z);
   g_lite.heading.configure(headingGainsFromMilli(s.heading_kp_milli, s.rate_kp_milli));
+  g_lite.cam.setTrim(s.pan_trim, s.tilt_trim);
 }
 
 }  // namespace lite
diff --git a/firmware/uno/src/lite/lite_ui.cpp b/firmware/uno/src/lite/lite_ui.cpp
index 45c9f8b..460aedd 100644
--- a/firmware/uno/src/lite/lite_ui.cpp
+++ b/firmware/uno/src/lite/lite_ui.cpp
@@ -85,8 +85,8 @@ void cameraTask() {
     g_servos_attached = want;
   }
   if (!want) return;
-  port::servos::writeUs(Axis::kPan, CamLite::toUs(g_lite.cam.outPan()));
-  port::servos::writeUs(Axis::kTilt, CamLite::toUs(g_lite.cam.outTilt()));
+  port::servos::writeUs(Axis::kPan, CamLite::toUs(g_lite.cam.servoPan()));
+  port::servos::writeUs(Axis::kTilt, CamLite::toUs(g_lite.cam.servoTilt()));
 }
 
 }  // namespace lite
```

- [ ] **Step 4: Run the tests, the whole Uno suite and the Uno build**

Run: `cd firmware/uno && ~/.platformio/penv/bin/pio test -e native -f test_lite_settings -f test_lite_ui && ~/.platformio/penv/bin/pio test -e native 2>&1 | tail -2 && ~/.platformio/penv/bin/pio run -e uno | grep -E "Flash:|RAM:"`
Expected:
- the two test groups PASS; the whole suite `297 test cases: 297 succeeded`;
- `Flash: … (used 31344 bytes from 32256 bytes)` (≤ 31 344), RAM 1540 B.

- [ ] **Step 5: Commit** (the pre-commit hook runs `protogen check` and `size_check`; `SettingsLite::load` stays ≤ 10 because the size test is the helper `knownSize`)

```bash
git add firmware/uno
git commit -m "feat(uno): camera centre - pan and tilt trims on the Uno Lite (Uno Lite LLD §22)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2: Bridge — read, try and save the trims

**Files:**
- Modify: `firmware/esp32/src/core/ws_codec.h` (`WsCommand::try_only`), `firmware/esp32/src/core/ws_codec_keys.cpp` (`"try"`), `firmware/esp32/src/app/settings_controller.h` / `.cpp` (`settingReadable`, `settingWritable`, `onTry`)
- Test: `firmware/esp32/test/test_ws_codec/test_main.cpp`, `firmware/esp32/test/test_settings_controller/test_main.cpp`

**Interfaces:**
- Consumes: the Uno's settings 27 / 28 (Task 1); `core::AckedOp::kSettingSet` (acked to `op.fd` / `op.id` by `UnoChannel::onAck`); `Hooks::driver_fd`, `ota`, `queue_op`.
- Produces:
  - WebSocket `{"t":"setting","setting_id":27|28,"value":v,"try":true,"id":n}` → `ack` after the Uno's ACK, or `nack` `INVALID_COMMAND` / `NOT_OWNER` / `BUSY`;
  - the plain `setting` set for 27 / 28 (−300…300) → confirm → SET + SAVE + GET;
  - reads of 27 / 28.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/firmware/esp32/test/test_settings_controller/test_main.cpp b/firmware/esp32/test/test_settings_controller/test_main.cpp
index 80d93ea..04e4f52 100644
--- a/firmware/esp32/test/test_settings_controller/test_main.cpp
+++ b/firmware/esp32/test/test_settings_controller/test_main.cpp
@@ -204,6 +204,58 @@ void test_restart_refused_while_moving() {
   TEST_ASSERT_TRUE(has(3, "BUSY"));
 }
 
+// R23: the camera centre. A try is SETTING_SET alone (no button), acked after the Uno's ACK; the camera rule.
+void test_camera_trim_try_sets_without_the_button() {
+  sc->onCommand(3, 20, P("{\"t\":\"setting\",\"setting_id\":27,\"value\":-50,\"try\":true,\"id\":20}"), 1000);
+  TEST_ASSERT_EQUAL(1, g_ops.size());
+  TEST_ASSERT_EQUAL(core::AckedOp::kSettingSet, g_ops[0].kind);
+  TEST_ASSERT_EQUAL_UINT8(27, g_ops[0].a);
+  TEST_ASSERT_EQUAL_INT32(-50, g_ops[0].value);
+  TEST_ASSERT_EQUAL(3, g_ops[0].fd);
+  TEST_ASSERT_EQUAL(20, g_ops[0].id);
+  TEST_ASSERT_FALSE(has(3, "\"t\":\"confirm\""));
+  sc->onCommand(3, 21, P("{\"t\":\"setting\",\"setting_id\":28,\"value\":300,\"try\":true,\"id\":21}"), 1000);
+  TEST_ASSERT_EQUAL(2, g_ops.size());
+}
+
+void test_camera_trim_try_only_for_the_trims_in_range() {
+  sc->onCommand(3, 22, P("{\"t\":\"setting\",\"setting_id\":66,\"value\":5,\"try\":true,\"id\":22}"), 1000);
+  sc->onCommand(3, 23, P("{\"t\":\"setting\",\"setting_id\":27,\"value\":301,\"try\":true,\"id\":23}"), 1000);
+  sc->onCommand(3, 24, P("{\"t\":\"setting\",\"setting_id\":28,\"value\":-301,\"try\":true,\"id\":24}"), 1000);
+  TEST_ASSERT_EQUAL(3, fakes::countTo(3, "INVALID_COMMAND"));
+  TEST_ASSERT_EQUAL(0, g_ops.size());
+}
+
+void test_camera_trim_try_follows_the_camera_rule() {
+  g_driver = 4;  // another client drives: only the driver may move the camera
+  sc->onCommand(3, 25, P("{\"t\":\"setting\",\"setting_id\":27,\"value\":-50,\"try\":true,\"id\":25}"), 1000);
+  TEST_ASSERT_TRUE(has(3, "NOT_OWNER"));
+  sc->onCommand(4, 26, P("{\"t\":\"setting\",\"setting_id\":27,\"value\":-50,\"try\":true,\"id\":26}"), 1000);
+  TEST_ASSERT_EQUAL(1, g_ops.size());
+  g_ota = true;
+  sc->onCommand(4, 27, P("{\"t\":\"setting\",\"setting_id\":27,\"value\":-45,\"try\":true,\"id\":27}"), 1000);
+  TEST_ASSERT_TRUE(has(4, "BUSY"));
+  g_ota = false;
+  g_free = 0;
+  sc->onCommand(4, 28, P("{\"t\":\"setting\",\"setting_id\":27,\"value\":-45,\"try\":true,\"id\":28}"), 1000);
+  TEST_ASSERT_EQUAL(2, fakes::countTo(4, "BUSY"));
+}
+
+void test_camera_trim_save_waits_for_the_button_and_reads() {
+  sc->onCommand(3, 29, P("{\"t\":\"setting\",\"setting_id\":27,\"id\":29}"), 1000);
+  TEST_ASSERT_EQUAL(1, g_ops.size());
+  TEST_ASSERT_EQUAL(core::AckedOp::kSettingGet, g_ops[0].kind);
+  sc->onCommand(3, 30, P("{\"t\":\"setting\",\"setting_id\":27,\"value\":-50,\"id\":30}"), 1000);
+  TEST_ASSERT_TRUE(has(3, "\"t\":\"confirm\""));
+  TEST_ASSERT_EQUAL(1, g_ops.size());
+  sc->onButtonPress(2000);
+  TEST_ASSERT_EQUAL(4, g_ops.size());
+  TEST_ASSERT_EQUAL(core::AckedOp::kSettingSet, g_ops[1].kind);
+  TEST_ASSERT_EQUAL(core::AckedOp::kSettingsSave, g_ops[2].kind);
+  sc->onCommand(3, 31, P("{\"t\":\"setting\",\"setting_id\":28,\"value\":-301,\"id\":31}"), 1000);
+  TEST_ASSERT_TRUE(has(3, "INVALID_COMMAND"));
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_change_waits_for_the_button_then_posts_wifi_with_feedback);
@@ -221,5 +273,9 @@ int main() {
   RUN_TEST(test_wifi_mode_result_is_logged);
   RUN_TEST(test_restart_is_acked_logged_and_due_after_500_ms);
   RUN_TEST(test_restart_refused_while_moving);
+  RUN_TEST(test_camera_trim_try_sets_without_the_button);
+  RUN_TEST(test_camera_trim_try_only_for_the_trims_in_range);
+  RUN_TEST(test_camera_trim_try_follows_the_camera_rule);
+  RUN_TEST(test_camera_trim_save_waits_for_the_button_and_reads);
   return UNITY_END();
 }
diff --git a/firmware/esp32/test/test_ws_codec/test_main.cpp b/firmware/esp32/test/test_ws_codec/test_main.cpp
index a85b14c..dd340c1 100644
--- a/firmware/esp32/test/test_ws_codec/test_main.cpp
+++ b/firmware/esp32/test/test_ws_codec/test_main.cpp
@@ -377,6 +377,8 @@ void test_setting_and_calib_reset() {
   TEST_ASSERT_TRUE(c.has_value);
   TEST_ASSERT_EQUAL_INT32(-12, c.value);
   TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"setting\"}").kind));
+  TEST_ASSERT_FALSE(c.try_only);  // R23: a try changes the Uno's RAM only
+  TEST_ASSERT_TRUE(P("{\"t\":\"setting\",\"setting_id\":27,\"value\":-50,\"try\":true}").try_only);
   TEST_ASSERT_EQUAL_INT8(0, P("{\"t\":\"calib\",\"reset\":\"counts_per_m\"}").calib_reset);
   TEST_ASSERT_EQUAL_INT8(1, P("{\"t\":\"calib\",\"reset\":\"roll\"}").calib_reset);
   TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"reset\":\"x\"}").kind));
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_codec -f test_settings_controller`
Expected: `no member named 'try_only' in 'cq::core::WsCommand'` (test_ws_codec); four `test_camera_trim_*` FAILED.

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/firmware/esp32/src/app/settings_controller.cpp b/firmware/esp32/src/app/settings_controller.cpp
index d2e842d..80874d4 100644
--- a/firmware/esp32/src/app/settings_controller.cpp
+++ b/firmware/esp32/src/app/settings_controller.cpp
@@ -16,8 +16,14 @@ namespace cq {
 namespace app {
 namespace {
 
-bool settingReadable(uint8_t id) { return id == 1 || id == 11 || id == 12 || id == 66; }
-constexpr uint8_t kTrackBalanceId = 66;  // v1: the only setting a page may change (spec 02 §F5)
+bool settingReadable(uint8_t id) { return id == 1 || id == 11 || id == 12 || id == 66 || id == 27 || id == 28; }
+constexpr uint8_t kTrackBalanceId = 66;  // spec 02 §F5
+bool isCameraTrim(uint8_t id) { return id == 27 || id == 28; }  // pan_trim / tilt_trim (R23)
+// The settings a page may change (with the tank's button), each in its range.
+bool settingWritable(uint8_t id, int32_t v) {
+  if (id == kTrackBalanceId) return v >= -100 && v <= 100;
+  return isCameraTrim(id) && v >= -300 && v <= 300;
+}
 
 // The WiFi request a confirmed change becomes (wifi_save / wifi_forget / wifi_mode / ap_password).
 bool wifiRequestOf(const core::PendingChange& ch, core::WifiRequest& r) {
@@ -129,7 +135,8 @@ void SettingsController::onSetting(int fd, int32_t id, const core::WsCommand& c,
     if (!m_h.queue_op(op)) m_out.nack(fd, id, core::BridgeReason::kBusy);
     return;
   }
-  if (c.setting_id != kTrackBalanceId || c.value < -100 || c.value > 100) {
+  if (c.try_only) return onTry(fd, id, c);
+  if (!settingWritable(c.setting_id, c.value)) {
     m_out.nack(fd, id, core::BridgeReason::kInvalidCommand);
     return;
   }
@@ -142,6 +149,31 @@ void SettingsController::onSetting(int fd, int32_t id, const core::WsCommand& c,
   askConfirm(ch, now);
 }
 
+// R23: a camera trim tried live - SETTING_SET alone (the Uno's RAM, a restart forgets it), no button; the camera
+// stick's rule: only the driver while someone drives. Acked after the Uno's ACK.
+void SettingsController::onTry(int fd, int32_t id, const core::WsCommand& c) {
+  if (!isCameraTrim(c.setting_id) || !settingWritable(c.setting_id, c.value)) {
+    m_out.nack(fd, id, core::BridgeReason::kInvalidCommand);
+    return;
+  }
+  if (m_h.ota()) {
+    m_out.nack(fd, id, core::BridgeReason::kBusy);
+    return;
+  }
+  const int driver = m_h.driver_fd();
+  if (driver >= 0 && driver != fd) {
+    m_out.nack(fd, id, core::BridgeReason::kNotOwner);
+    return;
+  }
+  core::AckedOp op;
+  op.kind = core::AckedOp::kSettingSet;
+  op.a = c.setting_id;
+  op.value = c.value;
+  op.fd = fd;
+  op.id = id;
+  if (!m_h.queue_op(op)) m_out.nack(fd, id, core::BridgeReason::kBusy);
+}
+
 void SettingsController::onRestart(int fd, int32_t id, uint32_t now) {
   if (m_h.ota() || m_h.motion_active()) {
     m_out.nack(fd, id, core::BridgeReason::kBusy);
diff --git a/firmware/esp32/src/app/settings_controller.h b/firmware/esp32/src/app/settings_controller.h
index f1f4b0f..2e8f5b9 100644
--- a/firmware/esp32/src/app/settings_controller.h
+++ b/firmware/esp32/src/app/settings_controller.h
@@ -46,6 +46,7 @@ class SettingsController {
   bool wifiIsAp();
   void pushFeedback(bool to_ap);
   void onSetting(int fd, int32_t id, const core::WsCommand& c, uint32_t now);
+  void onTry(int fd, int32_t id, const core::WsCommand& c);  // R23: a camera trim, RAM only
   void onRestart(int fd, int32_t id, uint32_t now);
   void onScan(int fd, int32_t id);
   void applyChange(core::PendingChange& ch);
diff --git a/firmware/esp32/src/core/ws_codec.h b/firmware/esp32/src/core/ws_codec.h
index 34adb49..bf21965 100644
--- a/firmware/esp32/src/core/ws_codec.h
+++ b/firmware/esp32/src/core/ws_codec.h
@@ -86,6 +86,7 @@ struct WsCommand {
   uint8_t setting_id = 0;  // setting
   bool has_value = false;
   int32_t value = 0;
+  bool try_only = false;  // setting (R23): SETTING_SET alone, not saved - the camera trims only
   int8_t calib_reset = -1;  // calib: 0 counts_per_m, 1 roll, 2 gyro_scale (R19)
   int16_t move_cm = 0;        // move: ±1..500 (+ forward)
   bool has_cpm = false;       // calib: set (else read)
diff --git a/firmware/esp32/src/core/ws_codec_keys.cpp b/firmware/esp32/src/core/ws_codec_keys.cpp
index 41faa7c..2e2ad8d 100644
--- a/firmware/esp32/src/core/ws_codec_keys.cpp
+++ b/firmware/esp32/src/core/ws_codec_keys.cpp
@@ -139,6 +139,10 @@ bool readValue(Reader& r, WsCommand& c, Seen&) {  // setting
   c.has_value = true;
   return true;
 }
+bool readTry(Reader& r, WsCommand& c, Seen&) {  // setting (R23)
+  bool seen = false;
+  return readBool(r, c.try_only, seen);
+}
 bool readReset(Reader& r, WsCommand&, Seen& s) {  // calib: a name; camera_set: true / false
   if (r.word("true")) {
     s.reset_true = true;
@@ -263,7 +267,7 @@ const KeyRow kKeys[] = {
     {"quality", readQuality}, {"fps_cap", readFpsCap}, {"brightness", readBrightness},
     {"contrast", readContrast}, {"low_light", readLowLight}, {"vflip", readVflip}, {"hmirror", readHmirror},
     {"ai_url", readAiUrl},  {"setting_id", readSettingId}, {"counts_per_m", readCpm}, {"gyro_scale", readGyroScale}, {"pan", readPan}, {"tilt", readTilt}, {"prog", readProg},
-    {"step", readStep},
+    {"step", readStep},     {"try", readTry},
 };
 
 }  // namespace
```

- [ ] **Step 4: Run the tests and the whole bridge suite**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_codec -f test_settings_controller && ~/.platformio/penv/bin/pio test -e native 2>&1 | tail -2`
Expected: `56 test cases: 56 succeeded`; the whole suite `664 test cases: 664 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src firmware/esp32/test
git commit -m "feat(bridge): camera trims - read, try live, save with the button (bridge LLD R23)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: The Settings page — the Camera centre row

**Files:**
- Modify: `firmware/esp32/web/settings.html`
- Create: `firmware/esp32/web/test/camera_centre_check.js` (Playwright `run_code`)

**Interfaces:**
- Consumes: Task 2's messages. The `confirmed` reply's `what` is `"setting"` for a setting change (`core::changeName`).
- Produces: buttons `#trimL` `#trimR` `#trimU` `#trimD` `#trimUndo` `#trimSave`, and the readout `#trimV` ("pan −5.0° · tilt 0.0°").

- [ ] **Step 1: Write the failing check** (apply this diff):

```diff
diff --git a/firmware/esp32/web/test/camera_centre_check.js b/firmware/esp32/web/test/camera_centre_check.js
new file mode 100644
index 0000000..53bf91b
--- /dev/null
+++ b/firmware/esp32/web/test/camera_centre_check.js
@@ -0,0 +1,64 @@
+// Bridge LLD R23 browser check (Playwright run_code): the Settings page's "Camera centre" row.
+// Serve firmware/esp32/web on :8766 (python3 -m http.server 8766) and open /settings.html.
+async (page) => {
+  await page.setViewportSize({ width: 390, height: 844 });
+  await page.addInitScript(() => {  // a stand-in WebSocket: records what the page sends, answers the setting reads
+    window.__sent = [];
+    class FakeWS {
+      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
+        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
+      send(s) { const m = JSON.parse(s); window.__sent.push(m);
+        if (m.t === "hello") this.emit({ t: "welcome", you: "settings-1", tank: { fw: { uno: "1.0" } } });
+        if (m.t === "setting" && m.value === undefined) this.emit({ t: "setting", id: m.setting_id, value: m.setting_id === 27 ? -50 : 0 });
+        if (m.t === "setting" && m.try && !window.__noAck) this.emit({ t: "ack", id: m.id }); }
+      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
+      close() { this.readyState = 3; this.onclose && this.onclose(); }
+    }
+    window.WebSocket = FakeWS;
+    window.__fakeEmit = (o) => window.__ws.emit(o);
+  });
+  const q = (f, a) => page.evaluate(f, a);
+  const sent = (t) => q((t) => window.__sent.filter((m) => m.t === t), t);
+  const out = {};
+  await page.goto("http://127.0.0.1:8766/settings.html?v=" + Date.now()); await page.waitForTimeout(800);
+  out.reads = (await sent("setting")).map((m) => m.setting_id).filter((id) => id === 27 || id === 28);  // [27, 28]
+  out.shown = await q(() => document.getElementById("trimV").textContent);                              // pan -5.0° · tilt 0.0°
+  await page.click('#tabs button[data-tab="tank"]'); await page.waitForTimeout(100);  // the Camera card is on TANK
+  await q(() => window.__sent.length = 0);
+  await page.click("#trimR");  // look further right: pan_trim - 5, after one cam_home
+  await page.click("#trimU");  // look further up: tilt_trim - 5
+  await page.click("#trimR");
+  await page.waitForTimeout(100);
+  out.home = (await sent("cam_home")).length;                                                       // 1
+  out.tries = (await sent("setting")).map((m) => [m.setting_id, m.value, m.try === true]);         // [27,-55,t] [28,-5,t] [27,-60,t]
+  out.shownAfter = await q(() => document.getElementById("trimV").textContent);                    // pan -6.0° · tilt -0.5° (unsaved)
+  await q(() => window.__sent.length = 0);
+  await page.click("#trimSave");
+  await page.waitForTimeout(100);
+  out.save = (await sent("setting")).map((m) => [m.setting_id, m.value, m.try === true]);          // [27,-60,false]
+  await q(() => window.__fakeEmit({ t: "confirmed", what: "setting" })); await page.waitForTimeout(50);
+  await q(() => window.__sent.length = 0);
+  await page.click("#trimL");  // a step after the save
+  await page.click("#trimUndo");  // back to what was saved
+  await page.waitForTimeout(100);
+  out.undo = (await sent("setting")).map((m) => [m.setting_id, m.value, m.try === true]);          // [27,-55,t] [27,-60,t]
+  await q(() => window.__sent.length = 0);
+  await page.click("#trimL"); await page.click("#trimL");
+  for (let i = 0; i < 70; i++) await page.click("#trimL");  // never past +30.0°
+  out.max = await q(() => document.getElementById("trimV").textContent);                            // pan +30.0° ...
+  await q(() => { window.__sent.length = 0; window.__noAck = true; });
+  await page.click("#trimD");  // refused by the bridge (someone else drives): the shown trim goes back
+  const tr = (await sent("setting"))[0];
+  await q((id) => window.__fakeEmit({ t: "nack", id: id, reason: "NOT_OWNER", message: "Someone else is driving" }), tr.id);
+  await page.waitForTimeout(50);
+  out.refused = await q(() => document.getElementById("trimV").textContent);                        // pan +30.0° · tilt -0.5°
+  await page.click("#trimSave");
+  const sv = (await sent("setting")).slice(-1)[0];
+  await q((id) => window.__fakeEmit({ t: "nack", id: id, reason: "BUSY" }), sv.id); await page.waitForTimeout(50);
+  await q(() => window.__fakeEmit({ t: "confirmed", what: "wifi" })); await page.waitForTimeout(50);  // another change
+  await q(() => window.__sent.length = 0);
+  await page.click("#trimSave");  // still unsaved: Save sends again
+  out.resave = (await sent("setting")).map((m) => [m.setting_id, m.value]);                          // [[27,300]]
+  out.wide = await q(() => document.documentElement.scrollWidth <= 390);                             // true
+  return out;
+}
```

- [ ] **Step 2: Run it to see it fail**

Run:
- `cd firmware/esp32/web && python3 -m http.server 8766 --bind 127.0.0.1` (in the background);
- copy the check to `.playwright-mcp/` in the repo (the Playwright tool reads files only there) and run it with `browser_run_code_unsafe` `filename`.

Expected: `TypeError: Cannot read properties of null (reading 'textContent')` (no `#trimV`).

- [ ] **Step 3: Write the page code** (apply this diff):

```diff
diff --git a/firmware/esp32/web/settings.html b/firmware/esp32/web/settings.html
index b3bc92a..bd1b3fd 100644
--- a/firmware/esp32/web/settings.html
+++ b/firmware/esp32/web/settings.html
@@ -114,6 +114,7 @@ footer { text-align: center; color: var(--dim); font-size: 10px; letter-spacing:
 .tog { min-width: 56px; } .tog.on { background: var(--ui); color: #022; }
 .mini { display: inline-grid; grid-template-columns: 30px 34px 30px; align-items: center; text-align: center; }
 .mini .btn { padding: 4px 0; } .mini b { text-align: center; }
+.trim { display: inline-flex; gap: 4px; } .trim .btn { min-width: 34px; padding: 4px 6px; }
 input[type=range] { padding: 0; border: 0; accent-color: var(--ui); background: none; }
 #gCan { display: block; width: 100%; height: 190px; margin-top: 8px; border: 1px solid var(--line); background: #020502; }
 .gfoot { display: flex; align-items: center; gap: 8px; margin-top: 8px; }
@@ -237,6 +238,9 @@ input[type=range] { padding: 0; border: 0; accent-color: var(--ui); background:
   </div>
   <div class="card"><h2><svg viewBox="0 0 24 24"><path d="M14.5 4h-5L7 7H4a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3l-2.5-3z"/><circle cx="12" cy="13" r="3"/></svg>[ 05 // CAMERA ]</h2>
     <div class="prev"><img id="cPrev" alt=""><span id="cPrevMsg">[ NO VIDEO ]</span></div>
+    <div class="row"><span>Camera centre</span><b id="trimV">--</b></div>
+    <div class="row"><span class="trim"><button class="btn" type="button" id="trimL" aria-label="Look further left">◀</button><button class="btn" type="button" id="trimR" aria-label="Look further right">▶</button><button class="btn" type="button" id="trimU" aria-label="Look further up">▲</button><button class="btn" type="button" id="trimD" aria-label="Look further down">▼</button></span><span class="trim"><button class="btn" type="button" id="trimUndo">Undo</button><button class="btn" type="button" id="trimSave">Save</button></span></div>
+    <div class="hint">// Line the picture up with something straight ahead, then Save (press the tank's button).</div>
     <div class="row"><span>Viewers</span><b id="cViewers">--</b></div>
     <div class="row"><span>Now</span><b id="cNow">--</b></div>
     <label>Picture size</label>
@@ -702,8 +706,54 @@ input[type=range] { padding: 0; border: 0; accent-color: var(--ui); background:
     if (isNaN(v) || v < -100 || v > 100) { toast("Track balance: -100 … 100"); return; }
     send({ t: "setting", setting_id: 66, value: v, id: nextId++ });
   });
+  // ---------- Camera centre (bridge LLD R23): a step is tried live (no button), Save asks for the tank's button ----------
+  // The trims are in servo terms (0.1°): looking further right or up is a smaller servo angle on this tank.
+  var trim = { saved: { 27: 0, 28: 0 }, cur: { 27: 0, 28: 0 }, homed: false, tries: {}, saveId: -1 };
+  function trimText(v) { return (v > 0 ? "+" : "") + (v / 10).toFixed(1) + "°"; }
+  function trimUi() { $("trimV").textContent = "pan " + trimText(trim.cur[27]) + " · tilt " + trimText(trim.cur[28]); }
+  function trimTry(id, v, prev) {  // a refused try puts the shown trim back (the Uno did not change)
+    trim.tries[nextId] = { sid: id, prev: prev };
+    send({ t: "setting", setting_id: id, value: v, try: true, id: nextId++ });
+  }
+  function trimStep(id, d) {
+    var v = Math.max(-300, Math.min(300, trim.cur[id] + d));
+    if (v === trim.cur[id]) return;
+    if (!trim.homed) { send({ t: "cam_home", id: nextId++ }); trim.homed = true; }  // trim with the camera at its centre
+    var prev = trim.cur[id];
+    trim.cur[id] = v; trimUi(); trimTry(id, v, prev);
+  }
+  $("trimL").addEventListener("click", function () { trimStep(27, 5); });
+  $("trimR").addEventListener("click", function () { trimStep(27, -5); });
+  $("trimU").addEventListener("click", function () { trimStep(28, -5); });
+  $("trimD").addEventListener("click", function () { trimStep(28, 5); });
+  $("trimUndo").addEventListener("click", function () {
+    [27, 28].forEach(function (id) {
+      if (trim.cur[id] === trim.saved[id]) return;
+      var prev = trim.cur[id];
+      trim.cur[id] = trim.saved[id]; trimTry(id, trim.cur[id], prev);
+    });
+    trimUi();
+  });
+  $("trimSave").addEventListener("click", function () {  // the Uno saves its whole block: one Save keeps both trims
+    var id = trim.cur[27] !== trim.saved[27] ? 27 : (trim.cur[28] !== trim.saved[28] ? 28 : 0);
+    if (!id) { toast("Camera centre already saved", true); return; }
+    trim.saveId = nextId;
+    send({ t: "setting", setting_id: id, value: trim.cur[id], id: nextId++ });
+  });
+  function trimSaved(what) {  // the Uno saved its whole block: both trims
+    if (what !== "setting" || trim.saveId < 0) return;
+    trim.saveId = -1; trim.saved[27] = trim.cur[27]; trim.saved[28] = trim.cur[28];
+  }
+  function trimReply(m) {  // ack / nack of a try or of the Save
+    var t = trim.tries[m.id];
+    if (t) { delete trim.tries[m.id]; if (m.t === "nack") { trim.cur[t.sid] = t.prev; trimUi(); } }
+    if (m.t === "nack" && m.id === trim.saveId) trim.saveId = -1;
+  }
+  function onTrim(m) { trim.saved[m.id] = m.value; trim.cur[m.id] = m.value; trimUi(); }
+
   var SETTING_EL = { 66: ["kBal", ""], 11: ["kStop", " cm"], 12: ["kSlow", " cm"], 1: ["kEncOn", ""] };
   function onSetting(m) {
+    if (m.id === 27 || m.id === 28) return onTrim(m);
     var e = SETTING_EL[m.id]; if (!e) return;
     $(e[0]).textContent = m.id === 1 ? (m.value ? "✓ YES" : "NO") : m.value + e[1];
     if (m.id === 66 && document.activeElement !== $("kBalIn")) $("kBalIn").value = m.value;
@@ -911,13 +961,15 @@ input[type=range] { padding: 0; border: 0; accent-color: var(--ui); background:
       case "calib": cpm = m.counts_per_m; $("kCpm").textContent = m.counts_per_m; $("kRoll").textContent = m.roll_mm; break;
       case "setting": onSetting(m); break;
       case "confirm": showConfirm(m.what, m.secs || 30); break;
-      case "confirmed": hideConfirm("Done ✓", true); editSlot = -1; $("nSlot").textContent = "NEW"; break;
-      case "confirm_expired": hideConfirm("Not confirmed - nothing changed"); break;
+      case "confirmed": trimSaved(m.what); hideConfirm("Done ✓", true); editSlot = -1; $("nSlot").textContent = "NEW"; break;
+      case "confirm_expired": trim.saveId = -1; hideConfirm("Not confirmed - nothing changed"); break;
       case "nack":
+        trimReply(m);
         if (m.id === wiz.id && (wiz.st === "sent" || wiz.st === "driving")) wizState("idle", "// Refused: " + (m.message || m.reason || "?"));
         hideConfirm(""); scanDone(); toast(m.message || m.reason || "Refused");
         break;
       case "ack":
+        trimReply(m);
         if (wizAck(m.id)) break;
         if (m.id === restartId) {  // owner: the restart needs feedback
           restarting = true; restartId = -1; toast("Restarting the bridge …", true);
@@ -938,7 +990,7 @@ input[type=range] { padding: 0; border: 0; accent-color: var(--ui); background:
       if (restarting) { restarting = false; toast("Bridge back online ✓", true); }
       send({ t: "hello", client: "settings", proto: 1 });
       send({ t: "wifi_status" }); send({ t: "calib" });
-      [66, 11, 12, 1].forEach(function (id) { send({ t: "setting", setting_id: id }); });
+      [66, 11, 12, 1, 27, 28].forEach(function (id) { send({ t: "setting", setting_id: id }); });
       loadInfo(); showTab(tab); wizUi();
     };
     ws.onmessage = function (e) { var m; try { m = JSON.parse(e.data); } catch (x) { return; } onMessage(m); };
```

- [ ] **Step 4: Run the check and the bridge build**

Run: the check again.
Expected: `{"reads":[27,28],"shown":"pan -5.0° · tilt 0.0°","home":1,"tries":[[27,-55,true],[28,-5,true],[27,-60,true]],"shownAfter":"pan -6.0° · tilt -0.5°","save":[[27,-60,false]],"undo":[[27,-55,true],[27,-60,true]],"max":"pan +30.0° · tilt -0.5°","refused":"pan +30.0° · tilt -0.5°","resave":[[27,300]],"wide":true}`.
- The console shows only the test server's 404s (`/info`, the icon) and the refused `:81/stream`; no script error.

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge | grep -E "pack_web|Flash:"`
Expected: `/settings` about 21.6 KB gzipped (budget 36 KB); SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/web/settings.html firmware/esp32/web/test/camera_centre_check.js
git commit -m "feat(bridge): Settings page - Camera centre row (bridge LLD R23.2)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 4: On the tank — flash, set the centre, check (owner present)

No drive. Only the camera moves. Ask the owner "stand or floor?" before flashing, and say that only the camera servos move.

- [ ] **Step 1: Flash the Uno over USB.**
  - The owner sets S1 to upload and plugs the Uno's USB into this Mac.
  - `cd firmware/uno && ~/.platformio/penv/bin/pio run -e uno -t upload`
  - Expected: `SUCCESS`. Then S1 goes back and the USB comes out.
- [ ] **Step 2: Flash the bridge over the air**

```bash
cd firmware/esp32
export CQ_OTA_PASSWORD="$(sed -n 's/.*CQ_OTA_PASSWORD[^"]*"\([^"]*\)".*/\1/p' include/secrets.h)"
~/.platformio/penv/bin/pio run -e bridge_ota -t upload --upload-port 192.168.31.167 2>&1 | grep -vF -- "$CQ_OTA_PASSWORD" | tail -3
unset CQ_OTA_PASSWORD
```
Expected: `SUCCESS`.
- [ ] **Step 3: Set the centre.**
  - The owner opens Settings → TANK → Camera on the phone, steps ▶ / ◀ (and ▲ / ▼ if needed) until the picture looks dead ahead, then presses Save and the tank's button.
  - Expected: about pan −5.0° (§125), and "Done ✓".
- [ ] **Step 4: Check after a restart.** Power the tank off and on, then check that it looks dead ahead and telemetry pan reads 90° (`tf` / `td`) in each case:
  - HOME (phone);
  - the camera stick let go (it returns home after 500 ms);
  - `{"t":"cam_set","pan":0,"tilt":0}` from the laptop;
  - Logo `LOOK 0 0`.
- [ ] **Step 5: Check a refusal:** while the phone drives, a try from the laptop → `NOT_OWNER`.
- [ ] **Step 6: Set the AI calibration back.** In `ai-engine/config/calibration.yaml`, set `pan_map` to `[1.0, 0.0]` (the trim now carries the 5°). `tilt_map` stays (1, 0). If a tilt trim t (×0.1°) was saved, the mount tilt changes with it: `tilt0_deg` = −0.15 − t / 10 (a smaller servo angle looks up). With no tilt trim (expected, §127), leave `tilt0_deg`. Write in the ledger: "pan_map (1, 0): the Uno trim carries the offset".
  - Run: `cd ai-engine && uv run pytest -q`
  - Expected: all pass (66).
- [ ] **Step 7: Commit**

```bash
git add ai-engine/config/calibration.yaml
git commit -m "chore(ai): pan_map back to (1, 0) - the Uno camera centre carries the 5 degrees

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 5: Docs as built

**Files:** `docs/lld/uno-lite-lld.md` (§22.1 as built: flash, tests), `docs/lld/esp32-bridge-lld.md` (R23.5 as built: the refused-step / refused-Save handling on the page), `docs/design-notes.md` (the tank results: the saved trims and what was checked), `CLAUDE.md` (plan list: `2026-10-23-plan-s-camera-centre.md`; Next).

- [ ] **Step 1: Write them** (each result with how it was checked).
- [ ] **Step 2: Commit**

```bash
git add docs CLAUDE.md
git commit -m "docs: Plan S camera centre - as built and tank results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

