# Plan X — Camera: the gain limit fix, low light for driving, white balance — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The camera gives usable pictures in dim rooms without losing frame rate, and its colour can be set. The bug that capped the OV3660's gain near zero is fixed.

**Architecture:**
- **A pure `core/cam_tuning`** maps `CamSettings` to the OV3660's values: a raw gain ceiling of 128 or 512, ae_level, night, wb_mode. It also reads a saved block of either size, carrying the old 8-byte one over.
- **`net/camera_service`** applies it.
- **`camera_set` / `tank`** carry `wb` and `night`, and the Camera card shows them.
- **The tank's source archive leaves out the implementation plans**, so the bridge fits its source partition again.

**Tech Stack:** C++17 (ESP32-S3 Arduino `bridge`, `bridge_ota`, `native` Unity host tests), plain JS in `web/settings.html` with a Playwright check.

**Spec:** `docs/lld/esp32-bridge-lld.md` R24 (approved 2026-10-24); design-notes §139, §140; esp32-camera `sensors/ov3660.c` (the raw gain ceiling in 0x3A18 / 0x3A19, night mode in 0x3A00 bit 2).

**Built from a tested draft:** branch `x-draft`. Bridge native 672/672; bridge flash 44.5 %; `/settings` 22.2 KB gzipped; source archive 1.22 MB; the page check passes.

## Global Constraints
- **Bridge:** every function's complexity ≤ 10 (pre-commit `size_check`); core code pure (host-tested).
- **No protocol or Uno change.**
- **OV3660 raw gain:** 16 = 1×; normal **128** (8×), low light **512** (32×). Low light: ae +1, **no night mode**. Night: its own switch.
- **White balance names** (lower case): auto, sunny, cloudy, office, home → modes 0–4; AWB stays on.
- **`CamSettings`** 8 → 10 bytes (`wb`, `night` at the end). **The old 8-byte saved block is carried over** (owner), then saved in the new size.
- **Hardware:**
  - the bridge flashed over WiFi; never print the OTA password;
  - pictures from the tank stay local and are deleted after the numbers are written down;
  - the camera goes back to VGA afterwards.

## Review Focus
1. **The owner's saved camera settings** (VGA, vflip …) survive the first boot of the new firmware. Covered by `test_the_old_8_byte_settings_are_carried_over`; seen on the tank (Task 6 Step 1).
2. **A corrupt saved block** (a bad size or wb value) falls back to the defaults, never to garbage. Covered by `test_a_new_block_round_trips_and_other_sizes_are_refused`.
3. **Low light must not lower the frame rate:** only night does. Covered by `test_low_light_…keeps_the_frame_rate`; measured on the tank (Task 6 Step 3).
4. **Reset to defaults** also resets wb and night. `cameraDefaults()` builds a fresh `CamSettings` (auto, off); covered by `test_camera_set_reset_uses_the_defaults` (unchanged) and the struct defaults.
5. **The source archive** stays well under 0x1E0000 B as docs grow. The build fails loudly if not (`pack_source`); 1.22 MB today.

---

### Task 1: The implementation plans out of the tank's source archive

**Why:** `git archive HEAD | gzip -9` reached 2 025 617 B, over the 1 966 080 B source partition, so **main can't build the bridge** (found while drafting). The plans are 0.80 MB of it (history, with their full diffs). Owner decision: the plans stay in git, not in the tank's archive, like the Stitch images and the datasheets.

**Files:** Modify: `.gitattributes`

- [ ] **Step 1: Apply this diff:**

```diff
diff --git a/.gitattributes b/.gitattributes
index 2f59887..31cfd41 100644
--- a/.gitattributes
+++ b/.gitattributes
@@ -4,3 +4,5 @@ docs/design/stitch/** export-ignore
 *.png export-ignore
 # Third-party datasheets (PDF, available online) stay in git but not in the tank's source archive (design-notes §103).
 docs/hardware/datasheets/** export-ignore
+# The implementation plans are history (with their full diffs): in git, not in the tank's source archive.
+docs/superpowers/plans/** export-ignore
```

- [ ] **Step 2: Commit, then check** (`git archive` reads `.gitattributes` from the commit)

```bash
git add .gitattributes
git commit -m "build: the implementation plans out of the tank's source archive (it no longer fit the partition)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git archive HEAD | gzip -9 | wc -c
```
Expected: about 1 222 968 (< 1 966 080).

### Task 2: The camera's sensor values (pure)

**Files:**
- Create: `firmware/esp32/src/core/cam_tuning.h`, `firmware/esp32/src/core/cam_tuning.cpp`, `firmware/esp32/test/test_cam_tuning/test_main.cpp`
- Modify: `firmware/esp32/src/core/cam_settings.h` (`wb`, `night`, `kCamSettingsV1Bytes`)

**Interfaces:**
- Produces: `CamTuning sensorTuning(const CamSettings&)`; `const char* wbName(uint8_t)`; `bool wbOf(const char*, uint8_t&)`; `bool camFromBlock(const uint8_t*, size_t, CamSettings&)`; `kGainCeilingNormal` 128, `kGainCeilingLowLight` 512, `kWbModes` 5.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/firmware/esp32/test/test_cam_tuning/test_main.cpp b/firmware/esp32/test/test_cam_tuning/test_main.cpp
new file mode 100644
index 0000000..94d5f7e
--- /dev/null
+++ b/firmware/esp32/test/test_cam_tuning/test_main.cpp
@@ -0,0 +1,101 @@
+// The camera's sensor values and the saved settings' carry-over (bridge LLD R24, design-notes §140).
+#include <string.h>
+#include <unity.h>
+
+#include "core/cam_tuning.h"
+
+using namespace cq::core;
+
+void setUp() {}
+void tearDown() {}
+
+void test_normal_light_gains_up_to_8x_without_night_mode() {
+  CamSettings s;
+  const CamTuning t = sensorTuning(s);
+  TEST_ASSERT_EQUAL_UINT16(128, t.gain_ceiling_raw);  // OV3660 raw gain: 16 = 1x
+  TEST_ASSERT_EQUAL_INT8(0, t.ae_level);
+  TEST_ASSERT_FALSE(t.night);
+  TEST_ASSERT_EQUAL_UINT8(0, t.wb_mode);
+}
+
+void test_low_light_gains_up_to_32x_one_step_brighter_and_keeps_the_frame_rate() {
+  CamSettings s;
+  s.low_light = true;
+  const CamTuning t = sensorTuning(s);
+  TEST_ASSERT_EQUAL_UINT16(512, t.gain_ceiling_raw);
+  TEST_ASSERT_EQUAL_INT8(1, t.ae_level);
+  TEST_ASSERT_FALSE(t.night);  // night mode lowers the frame rate: its own switch
+}
+
+void test_night_is_its_own_switch() {
+  CamSettings s;
+  s.night = true;
+  TEST_ASSERT_TRUE(sensorTuning(s).night);
+  TEST_ASSERT_EQUAL_UINT16(128, sensorTuning(s).gain_ceiling_raw);
+}
+
+void test_white_balance_names_and_modes() {
+  const char* const names[] = {"auto", "sunny", "cloudy", "office", "home"};
+  for (uint8_t i = 0; i < 5; ++i) {
+    TEST_ASSERT_EQUAL_STRING(names[i], wbName(i));
+    uint8_t m = 99;
+    TEST_ASSERT_TRUE(wbOf(names[i], m));
+    TEST_ASSERT_EQUAL_UINT8(i, m);
+    CamSettings s;
+    s.wb = i;
+    TEST_ASSERT_EQUAL_UINT8(i, sensorTuning(s).wb_mode);
+  }
+  uint8_t m = 7;
+  TEST_ASSERT_FALSE(wbOf("Auto", m));  // lower case only, as the names are sent
+  TEST_ASSERT_FALSE(wbOf("night", m));
+  TEST_ASSERT_EQUAL_STRING("auto", wbName(9));
+}
+
+void test_the_old_8_byte_settings_are_carried_over() {
+  const uint8_t old[8] = {0, 25, 10, static_cast<uint8_t>(-1), 2, 1, 0, 1};  // QVGA q25 fps10 b-1 c2 low no-vflip mirror
+  CamSettings s;
+  s.wb = 3;
+  s.night = true;
+  TEST_ASSERT_TRUE(camFromBlock(old, sizeof(old), s));
+  TEST_ASSERT_EQUAL_UINT8(0, s.size);
+  TEST_ASSERT_EQUAL_UINT8(25, s.quality);
+  TEST_ASSERT_EQUAL_UINT8(10, s.fps_cap);
+  TEST_ASSERT_EQUAL_INT8(-1, s.brightness);
+  TEST_ASSERT_EQUAL_INT8(2, s.contrast);
+  TEST_ASSERT_TRUE(s.low_light);
+  TEST_ASSERT_FALSE(s.vflip);
+  TEST_ASSERT_TRUE(s.hmirror);
+  TEST_ASSERT_EQUAL_UINT8(0, s.wb);  // the new ones take their defaults
+  TEST_ASSERT_FALSE(s.night);
+}
+
+void test_a_new_block_round_trips_and_other_sizes_are_refused() {
+  CamSettings a;
+  a.size = 2;
+  a.wb = 4;
+  a.night = true;
+  uint8_t buf[sizeof(CamSettings)];
+  memcpy(buf, &a, sizeof(a));
+  CamSettings b;
+  TEST_ASSERT_TRUE(camFromBlock(buf, sizeof(buf), b));
+  TEST_ASSERT_EQUAL_UINT8(2, b.size);
+  TEST_ASSERT_EQUAL_UINT8(4, b.wb);
+  TEST_ASSERT_TRUE(b.night);
+  CamSettings c;
+  TEST_ASSERT_FALSE(camFromBlock(buf, 7, c));
+  uint8_t bad[8] = {9, 12, 0, 0, 0, 0, 1, 0};  // size 9: not a size
+  TEST_ASSERT_FALSE(camFromBlock(bad, sizeof(bad), c));
+  buf[sizeof(CamSettings) - 2] = 7;  // wb 7: not a mode
+  TEST_ASSERT_FALSE(camFromBlock(buf, sizeof(buf), c));
+}
+
+int main() {
+  UNITY_BEGIN();
+  RUN_TEST(test_normal_light_gains_up_to_8x_without_night_mode);
+  RUN_TEST(test_low_light_gains_up_to_32x_one_step_brighter_and_keeps_the_frame_rate);
+  RUN_TEST(test_night_is_its_own_switch);
+  RUN_TEST(test_white_balance_names_and_modes);
+  RUN_TEST(test_the_old_8_byte_settings_are_carried_over);
+  RUN_TEST(test_a_new_block_round_trips_and_other_sizes_are_refused);
+  return UNITY_END();
+}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_cam_tuning`
Expected: `fatal error: 'core/cam_tuning.h' file not found`.

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/firmware/esp32/src/core/cam_settings.h b/firmware/esp32/src/core/cam_settings.h
index f83f83e..542e4b8 100644
--- a/firmware/esp32/src/core/cam_settings.h
+++ b/firmware/esp32/src/core/cam_settings.h
@@ -17,7 +17,10 @@ struct CamSettings {
   bool low_light = false;
   bool vflip = true;
   bool hmirror = false;
+  uint8_t wb = 0;      // R24: white balance 0 auto, 1 sunny, 2 cloudy, 3 office, 4 home
+  bool night = false;  // R24: the sensor's night mode (longer exposures, fewer pictures) - for looking, not driving
 };
+constexpr unsigned kCamSettingsV1Bytes = 8;  // the block saved before R24: up to hmirror
 struct CamStats {
   uint16_t fps_x10 = 0;
   uint16_t kbps = 0;
diff --git a/firmware/esp32/src/core/cam_tuning.cpp b/firmware/esp32/src/core/cam_tuning.cpp
new file mode 100644
index 0000000..8c99998
--- /dev/null
+++ b/firmware/esp32/src/core/cam_tuning.cpp
@@ -0,0 +1,49 @@
+#include "core/cam_tuning.h"
+
+#include <string.h>
+
+namespace cq {
+namespace core {
+namespace {
+const char* const kWbNames[kWbModes] = {"auto", "sunny", "cloudy", "office", "home"};
+}  // namespace
+
+CamTuning sensorTuning(const CamSettings& s) {
+  CamTuning t;
+  t.gain_ceiling_raw = s.low_light ? kGainCeilingLowLight : kGainCeilingNormal;
+  t.ae_level = s.low_light ? 1 : 0;
+  t.night = s.night;
+  t.wb_mode = s.wb < kWbModes ? s.wb : 0;
+  return t;
+}
+
+const char* wbName(uint8_t mode) { return kWbNames[mode < kWbModes ? mode : 0]; }
+
+bool wbOf(const char* name, uint8_t& mode) {
+  for (uint8_t i = 0; i < kWbModes; ++i) {
+    if (strcmp(name, kWbNames[i]) == 0) {
+      mode = i;
+      return true;
+    }
+  }
+  return false;
+}
+
+bool camFromBlock(const uint8_t* data, size_t len, CamSettings& out) {
+  CamSettings c = out;
+  if (len == sizeof(CamSettings)) {
+    memcpy(&c, data, sizeof(CamSettings));
+  } else if (len == kCamSettingsV1Bytes) {
+    memcpy(&c, data, kCamSettingsV1Bytes);  // the fields before R24 are the struct's first 8 bytes
+    c.wb = 0;
+    c.night = false;
+  } else {
+    return false;
+  }
+  if (c.size >= 4 || c.quality < 10 || c.quality > 40 || c.wb >= kWbModes) return false;
+  out = c;
+  return true;
+}
+
+}  // namespace core
+}  // namespace cq
diff --git a/firmware/esp32/src/core/cam_tuning.h b/firmware/esp32/src/core/cam_tuning.h
new file mode 100644
index 0000000..5734060
--- /dev/null
+++ b/firmware/esp32/src/core/cam_tuning.h
@@ -0,0 +1,33 @@
+// The camera's sensor values and the saved settings' carry-over (bridge LLD R24, design-notes §140). Pure.
+#pragma once
+
+#include <stddef.h>
+#include <stdint.h>
+
+#include "core/cam_settings.h"
+
+namespace cq {
+namespace core {
+
+// What applyTo() writes to the OV3660. Its gain ceiling is a RAW 10-bit gain (16 = 1x, 0x3FF ~ 64x; esp32-camera
+// sensors/ov3660.c writes it to 0x3A18 / 0x3A19) - not the library's GAINCEILING_* names (the §96 bug).
+struct CamTuning {
+  uint16_t gain_ceiling_raw = 128;
+  int8_t ae_level = 0;
+  bool night = false;
+  uint8_t wb_mode = 0;
+};
+
+constexpr uint16_t kGainCeilingNormal = 128;  // 8x
+constexpr uint16_t kGainCeilingLowLight = 512;  // 32x
+constexpr uint8_t kWbModes = 5;
+
+CamTuning sensorTuning(const CamSettings& s);
+const char* wbName(uint8_t mode);  // "auto" for anything out of range
+bool wbOf(const char* name, uint8_t& mode);
+// The settings from a saved NVS block: the R24 size, or the 8-byte block before it (the new fields keep the
+// values already in `out`); false for any other size or an invalid value.
+bool camFromBlock(const uint8_t* data, size_t len, CamSettings& out);
+
+}  // namespace core
+}  // namespace cq
```

- [ ] **Step 4: Run the tests**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_cam_tuning`
Expected: `6 test cases: 6 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/cam_tuning.h firmware/esp32/src/core/cam_tuning.cpp firmware/esp32/src/core/cam_settings.h firmware/esp32/test/test_cam_tuning
git commit -m "feat(bridge): camera tuning - the OV3660's raw gain ceiling, low light, night, white balance (bridge LLD R24)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: Through the bridge — message, reply, controller, sensor

**Files:**
- Modify: `firmware/esp32/src/core/ws_codec.h` (`CamFields`: `wb`, `night`; `any()` split for complexity), `firmware/esp32/src/core/ws_codec_keys.cpp` (`"wb"`, `"night"`), `firmware/esp32/src/core/ws_json.h` / `ws_json_tank.cpp` (`"wb"`, `"night"` in the reply), `firmware/esp32/src/app/tank_app_controller.cpp` (`merge` in two halves), `firmware/esp32/src/net/camera_service.cpp` (`applyTo` with `sensorTuning`; `loadSettings` with `camFromBlock`, saving a carried-over block anew)
- Test: `firmware/esp32/test/test_ws_codec/test_main.cpp`, `firmware/esp32/test/test_tank_app_controller/test_main.cpp`

**Interfaces:**
- Consumes: Task 2's `sensorTuning`, `wbName`, `wbOf`, `camFromBlock`.
- Produces: `camera_set` `"wb"` / `"night"`; the `tank` reply's `camera.wb` / `camera.night`.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/firmware/esp32/test/test_tank_app_controller/test_main.cpp b/firmware/esp32/test/test_tank_app_controller/test_main.cpp
index 3d4d43f..783ecc6 100644
--- a/firmware/esp32/test/test_tank_app_controller/test_main.cpp
+++ b/firmware/esp32/test/test_tank_app_controller/test_main.cpp
@@ -73,6 +73,16 @@ void test_camera_set_merges_only_sent_fields_then_acks_and_sends_tank() {
   TEST_ASSERT_NOT_EQUAL(std::string::npos, fakes::lastTo(3).find("\"vflip\":false"));  // the posted settings
 }
 
+void test_camera_set_white_balance_and_night_are_merged_and_reported() {  // R24
+  tank.onCameraSet(3, 9, P("{\"t\":\"camera_set\",\"wb\":\"office\",\"night\":true,\"id\":9}"), 1000);
+  TEST_ASSERT_EQUAL(1, fakes::world().cam_posted.size());
+  TEST_ASSERT_EQUAL(3, fakes::world().cam_posted[0].wb);
+  TEST_ASSERT_TRUE(fakes::world().cam_posted[0].night);
+  const std::string j = fakes::lastTo(3);
+  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"wb\":\"office\""));
+  TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"night\":true"));
+}
+
 void test_camera_set_reset_uses_the_defaults() {
   fakes::world().cam.quality = 30;
   fakes::world().cam_defaults.quality = 12;
@@ -126,6 +136,7 @@ int main() {
   RUN_TEST(test_log_pages_twenty_five_at_a_time);
   RUN_TEST(test_camera_set_merges_only_sent_fields_then_acks_and_sends_tank);
   RUN_TEST(test_camera_set_reset_uses_the_defaults);
+  RUN_TEST(test_camera_set_white_balance_and_night_are_merged_and_reported);
   RUN_TEST(test_camera_set_without_camera_is_unavailable);
   RUN_TEST(test_app_set_saves_and_acks);
   RUN_TEST(test_app_set_save_failure_is_busy);
diff --git a/firmware/esp32/test/test_ws_codec/test_main.cpp b/firmware/esp32/test/test_ws_codec/test_main.cpp
index dd340c1..ffd341b 100644
--- a/firmware/esp32/test/test_ws_codec/test_main.cpp
+++ b/firmware/esp32/test/test_ws_codec/test_main.cpp
@@ -404,6 +404,13 @@ void test_camera_set_false_turns_a_switch_off() {  // tank check: "flip upside d
   TEST_ASSERT_TRUE(d.cam.has_low && !d.cam.low && d.cam.has_hmirror && !d.cam.hmirror);
 }
 
+void test_camera_set_white_balance_and_night() {  // R24
+  const WsCommand c = P("{\"t\":\"camera_set\",\"wb\":\"home\",\"night\":true,\"id\":4}");
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kCameraSet), K(c.kind));
+  TEST_ASSERT_TRUE(c.cam.has_wb && c.cam.wb == 4 && c.cam.has_night && c.cam.night);
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"camera_set\",\"wb\":\"disco\"}").kind));
+}
+
 void test_camera_set_fields_and_ranges() {
   const WsCommand c = P("{\"t\":\"camera_set\",\"size\":\"SVGA\",\"quality\":20,\"fps_cap\":10,\"brightness\":-1,"
                         "\"low_light\":true,\"hmirror\":false,\"id\":4}");
@@ -507,6 +514,7 @@ int main() {
   RUN_TEST(test_calib_gyro_scale);
   RUN_TEST(test_tank_log_selftest);
   RUN_TEST(test_camera_set_fields_and_ranges);
+  RUN_TEST(test_camera_set_white_balance_and_night);
   RUN_TEST(test_camera_set_false_turns_a_switch_off);
   RUN_TEST(test_app_url_validation);
   RUN_TEST(test_app_url_too_long_is_refused_by_the_parser);
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_codec -f test_tank_app_controller`
Expected: `no member named 'has_wb' in 'cq::core::CamFields'`, `no member named 'wb' in 'cq::core::CamSettings'` (the latter if Task 2 is not in).

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/firmware/esp32/src/app/tank_app_controller.cpp b/firmware/esp32/src/app/tank_app_controller.cpp
index 0cef68e..4a7fd5e 100644
--- a/firmware/esp32/src/app/tank_app_controller.cpp
+++ b/firmware/esp32/src/app/tank_app_controller.cpp
@@ -23,6 +23,8 @@ void TankAppController::sendTank(int fd, uint32_t now, const core::CamSettings&
   v.cam_low = cs.low_light;
   v.cam_vflip = cs.vflip;
   v.cam_hmirror = cs.hmirror;
+  v.cam_wb = cs.wb;
+  v.cam_night = cs.night;
   v.viewers = ports::cameraViewers();
   const core::CamStats st = ports::cameraStats(now);
   v.fps_x10 = st.fps_x10;
@@ -40,16 +42,28 @@ void TankAppController::onLog(int fd, uint32_t since, uint32_t now) {
   m_out.bigTo(fd, core::jsonLog(m_out.big(), m_out.bigCap(), e, n, m_log.last(), gap, now));
 }
 
-core::CamSettings TankAppController::merge(const core::CamFields& f) const {
-  core::CamSettings s = f.reset ? ports::cameraDefaults() : ports::cameraSettings();
+namespace {
+// Two halves of TankAppController::merge (complexity <= 10 each): what is sent and how the picture looks.
+void mergePicture(core::CamSettings& s, const core::CamFields& f) {
   if (f.has_size) s.size = f.size;
   if (f.has_quality) s.quality = f.quality;
   if (f.has_fps) s.fps_cap = f.fps;
+  if (f.has_vflip) s.vflip = f.vflip;
+  if (f.has_hmirror) s.hmirror = f.hmirror;
+}
+void mergeLook(core::CamSettings& s, const core::CamFields& f) {
   if (f.has_bright) s.brightness = f.bright;
   if (f.has_contrast) s.contrast = f.contrast;
   if (f.has_low) s.low_light = f.low;
-  if (f.has_vflip) s.vflip = f.vflip;
-  if (f.has_hmirror) s.hmirror = f.hmirror;
+  if (f.has_wb) s.wb = f.wb;  // R24
+  if (f.has_night) s.night = f.night;
+}
+}  // namespace
+
+core::CamSettings TankAppController::merge(const core::CamFields& f) const {
+  core::CamSettings s = f.reset ? ports::cameraDefaults() : ports::cameraSettings();
+  mergePicture(s, f);
+  mergeLook(s, f);
   return s;
 }
 
diff --git a/firmware/esp32/src/core/ws_codec.h b/firmware/esp32/src/core/ws_codec.h
index bf21965..ed5363e 100644
--- a/firmware/esp32/src/core/ws_codec.h
+++ b/firmware/esp32/src/core/ws_codec.h
@@ -51,11 +51,16 @@ enum class WsCmdKind : uint8_t {
 struct CamFields {
   bool has_size = false, has_quality = false, has_fps = false, has_bright = false, has_contrast = false;
   bool has_low = false, has_vflip = false, has_hmirror = false, reset = false;
+  bool has_wb = false, has_night = false;  // R24
+  uint8_t wb = 0;
+  bool night = false;
   uint8_t size = 1, quality = 12, fps = 0;
   int8_t bright = 0, contrast = 0;
   bool low = false, vflip = true, hmirror = false;
-  bool any() const {
-    return has_size || has_quality || has_fps || has_bright || has_contrast || has_low || has_vflip || has_hmirror || reset;
+  bool any() const { return anyPicture() || anyLook() || reset; }
+  bool anyPicture() const { return has_size || has_quality || has_fps; }
+  bool anyLook() const {
+    return has_bright || has_contrast || has_low || has_vflip || has_hmirror || has_wb || has_night;
   }
 };
 
diff --git a/firmware/esp32/src/core/ws_codec_keys.cpp b/firmware/esp32/src/core/ws_codec_keys.cpp
index 2e2ad8d..32db75e 100644
--- a/firmware/esp32/src/core/ws_codec_keys.cpp
+++ b/firmware/esp32/src/core/ws_codec_keys.cpp
@@ -4,6 +4,7 @@
 
 #include "core/heading_scale.h"
 #include "core/motion_config.h"
+#include "core/cam_tuning.h"
 #include "core/ws_codec_internal.h"
 
 namespace cq {
@@ -199,6 +200,13 @@ bool readContrast(Reader& r, WsCommand& c, Seen&) {
 bool readLowLight(Reader& r, WsCommand& c, Seen&) { return readBool(r, c.cam.low, c.cam.has_low); }
 bool readVflip(Reader& r, WsCommand& c, Seen&) { return readBool(r, c.cam.vflip, c.cam.has_vflip); }
 bool readHmirror(Reader& r, WsCommand& c, Seen&) { return readBool(r, c.cam.hmirror, c.cam.has_hmirror); }
+bool readNight(Reader& r, WsCommand& c, Seen&) { return readBool(r, c.cam.night, c.cam.has_night); }  // R24
+bool readWb(Reader& r, WsCommand& c, Seen&) {  // R24: camera_set white balance, by name
+  char name[8];
+  if (!r.str(name, sizeof(name)) || !wbOf(name, c.cam.wb)) return false;
+  c.cam.has_wb = true;
+  return true;
+}
 bool readAiUrl(Reader& r, WsCommand& c, Seen& s) {  // app_set
   if (!r.str(c.ai_url, sizeof(c.ai_url))) return false;
   c.has_ai_url = true;
@@ -267,7 +275,7 @@ const KeyRow kKeys[] = {
     {"quality", readQuality}, {"fps_cap", readFpsCap}, {"brightness", readBrightness},
     {"contrast", readContrast}, {"low_light", readLowLight}, {"vflip", readVflip}, {"hmirror", readHmirror},
     {"ai_url", readAiUrl},  {"setting_id", readSettingId}, {"counts_per_m", readCpm}, {"gyro_scale", readGyroScale}, {"pan", readPan}, {"tilt", readTilt}, {"prog", readProg},
-    {"step", readStep},     {"try", readTry},
+    {"step", readStep},     {"try", readTry},         {"wb", readWb},           {"night", readNight},
 };
 
 }  // namespace
diff --git a/firmware/esp32/src/core/ws_json.h b/firmware/esp32/src/core/ws_json.h
index 94b045c..8a6bebd 100644
--- a/firmware/esp32/src/core/ws_json.h
+++ b/firmware/esp32/src/core/ws_json.h
@@ -97,6 +97,8 @@ struct TankView {
   uint8_t cam_size = 1, cam_quality = 12, cam_fps = 0;
   int8_t cam_bright = 0, cam_contrast = 0;
   bool cam_low = false, cam_vflip = true, cam_hmirror = false;
+  uint8_t cam_wb = 0;  // R24
+  bool cam_night = false;
   uint8_t viewers = 0;
   uint16_t fps_x10 = 0;
   uint16_t kbps = 0;
diff --git a/firmware/esp32/src/core/ws_json_tank.cpp b/firmware/esp32/src/core/ws_json_tank.cpp
index a9ea099..e5c0d26 100644
--- a/firmware/esp32/src/core/ws_json_tank.cpp
+++ b/firmware/esp32/src/core/ws_json_tank.cpp
@@ -1,4 +1,5 @@
 #include "core/ws_json.h"
+#include "core/cam_tuning.h"
 #include "core/ws_json_util.h"
 #include "generated/cq_protocol_names.h"
 
@@ -19,11 +20,13 @@ size_t jsonTank(char* out, size_t cap, const TankView& v) {
              "{\"t\":\"tank\",\"selftest\":{\"seen\":%s,\"mask\":%u,\"locked\":%s,\"age_s\":%lu},"
              "\"hw\":{\"encoders\":%s,\"imu\":%s,\"us\":%s},"
              "\"camera\":{\"size\":\"%s\",\"quality\":%u,\"fps_cap\":%u,\"brightness\":%d,\"contrast\":%d,"
-             "\"low_light\":%s,\"vflip\":%s,\"hmirror\":%s,\"viewers\":%u,\"fps\":%.1f,\"kbps\":%u},"
+             "\"low_light\":%s,\"vflip\":%s,\"hmirror\":%s,\"wb\":\"%s\",\"night\":%s,\"viewers\":%u,\"fps\":%.1f,"
+             "\"kbps\":%u},"
              "\"ai_url\":\"%s\"}",
              tf(v.st.seen), v.st.mask, tf(v.st.locked), static_cast<unsigned long>(v.st_age_s), tf(v.hw & 1u),
              tf(v.hw & 2u), tf(v.hw & 4u), kSizes[v.cam_size < 4 ? v.cam_size : 1], v.cam_quality, v.cam_fps,
-             v.cam_bright, v.cam_contrast, tf(v.cam_low), tf(v.cam_vflip), tf(v.cam_hmirror), v.viewers,
+             v.cam_bright, v.cam_contrast, tf(v.cam_low), tf(v.cam_vflip), tf(v.cam_hmirror), wbName(v.cam_wb),
+             tf(v.cam_night), v.viewers,
              v.fps_x10 / 10.0, v.kbps, url);
 }
 
diff --git a/firmware/esp32/src/net/camera_service.cpp b/firmware/esp32/src/net/camera_service.cpp
index 838b837..96984e1 100644
--- a/firmware/esp32/src/net/camera_service.cpp
+++ b/firmware/esp32/src/net/camera_service.cpp
@@ -10,6 +10,7 @@
 #include <driver/gpio.h>
 
 #include "board_config.h"
+#include "core/cam_tuning.h"
 #include "net/log.h"
 
 namespace cq {
@@ -33,22 +34,19 @@ void applyTo(sensor_t* s, const CamSettings& c) {
   s->set_quality(s, c.quality);
   s->set_brightness(s, c.brightness);
   s->set_contrast(s, c.contrast);
-  // design-notes §96: low light = night mode (longer exposures in the dark) + 32x gain ceiling + one step brighter
-  s->set_aec2(s, c.low_light ? 1 : 0);
-  s->set_gainceiling(s, c.low_light ? GAINCEILING_32X : GAINCEILING_4X);
-  s->set_ae_level(s, c.low_light ? 1 : 0);
+  // R24: the OV3660 takes a RAW gain ceiling (16 = 1x), not the GAINCEILING_* names (§96 sent 1 / 4: almost no
+  // gain); low light = 32x + one step brighter, keeping the frame rate; night mode is its own switch.
+  const core::CamTuning t = core::sensorTuning(c);
+  s->set_gainceiling(s, static_cast<gainceiling_t>(t.gain_ceiling_raw));
+  s->set_ae_level(s, t.ae_level);
+  s->set_aec2(s, t.night ? 1 : 0);
+  s->set_whitebal(s, 1);
+  s->set_awb_gain(s, 1);
+  s->set_wb_mode(s, t.wb_mode);
   s->set_vflip(s, c.vflip ? 1 : 0);
   s->set_hmirror(s, c.hmirror ? 1 : 0);
 }
 
-bool loadSettings(CamSettings& c) {
-  Preferences p;
-  if (!p.begin("cq_cam", true)) return false;
-  const bool ok = p.getBytesLength("s") == sizeof(CamSettings) && p.getBytes("s", &c, sizeof(c)) == sizeof(c);
-  p.end();
-  return ok && c.size < 4 && c.quality >= 10 && c.quality <= 40;
-}
-
 void saveSettings(const CamSettings& c) {
   Preferences p;
   if (!p.begin("cq_cam", false)) {
@@ -58,6 +56,20 @@ void saveSettings(const CamSettings& c) {
   if (p.putBytes("s", &c, sizeof(c)) != sizeof(c)) logf("camera: settings not saved (NVS write failed)");
   p.end();
 }
+
+// The saved block in either format (R24: the 8-byte one from before is carried over, then saved anew).
+bool loadSettings(CamSettings& c) {
+  Preferences p;
+  if (!p.begin("cq_cam", true)) return false;
+  uint8_t buf[sizeof(CamSettings)];
+  const size_t len = p.getBytesLength("s");
+  const bool read = len <= sizeof(buf) && p.getBytes("s", buf, len) == len;
+  p.end();
+  if (!read || !core::camFromBlock(buf, len, c)) return false;
+  if (len != sizeof(CamSettings)) saveSettings(c);
+  return true;
+}
+
 const char* g_name = "none";
 std::atomic<bool> g_stopped{false};
 
@@ -169,8 +181,8 @@ void cameraApplyPending() {
   g_cur = c;
   portEXIT_CRITICAL(&g_cam_mux);
   saveSettings(c);
-  logf("camera: settings size %u q%u fps %u b%d c%d low %u", c.size, c.quality, c.fps_cap, c.brightness, c.contrast,
-       c.low_light ? 1u : 0u);
+  logf("camera: settings size %u q%u fps %u b%d c%d low %u wb %s night %u", c.size, c.quality, c.fps_cap, c.brightness,
+       c.contrast, c.low_light ? 1u : 0u, core::wbName(c.wb), c.night ? 1u : 0u);
 }
 
 CamSettings cameraSettings() {
```

- [ ] **Step 4: Run the tests, the whole suite and the build**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_cam_tuning -f test_ws_codec -f test_tank_app_controller && ~/.platformio/penv/bin/pio test -e native 2>&1 | tail -1 && ~/.platformio/penv/bin/pio run -e bridge | grep -E "pack_source|Flash:"`
Expected: 54 succeeded in the three groups; `672 test cases: 672 succeeded`; SUCCESS; `pack_source: ~1222968 B`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src firmware/esp32/test
git commit -m "feat(bridge): camera_set wb / night, the gain limit fixed in the sensor, old settings carried over (bridge LLD R24)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 4: The Camera card

**Files:**
- Modify: `firmware/esp32/web/settings.html` (a White balance segmented row, a Night (slow) toggle, the two hints)
- Create: `firmware/esp32/web/test/camera_tuning_check.js` (Playwright `run_code`)

- [ ] **Step 1: Write the failing check** (apply this diff):

```diff
diff --git a/firmware/esp32/web/test/camera_tuning_check.js b/firmware/esp32/web/test/camera_tuning_check.js
new file mode 100644
index 0000000..0511caa
--- /dev/null
+++ b/firmware/esp32/web/test/camera_tuning_check.js
@@ -0,0 +1,38 @@
+// Bridge LLD R24 browser check (Playwright run_code): the Camera card's white balance and night switch.
+// Serve firmware/esp32/web on :8766 (python3 -m http.server 8766) and open /settings.html.
+async (page) => {
+  await page.setViewportSize({ width: 390, height: 844 });
+  await page.addInitScript(() => {
+    window.__sent = [];
+    const cam = { size: "QVGA", quality: 12, fps_cap: 0, brightness: 0, contrast: 0, low_light: false, vflip: true,
+                  hmirror: false, wb: "auto", night: false, viewers: 0, fps: 0, kbps: 0 };
+    class FakeWS {
+      constructor(url) { this.url = url; this.readyState = 0; window.__ws = this;
+        setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
+      send(s) { const m = JSON.parse(s); window.__sent.push(m);
+        const tank = () => this.emit({ t: "tank", selftest: { seen: false, mask: 0, locked: false, age_s: 0 },
+                                       hw: { encoders: true, imu: true, us: true }, camera: cam, ai_url: "" });
+        if (m.t === "hello") this.emit({ t: "welcome", you: "settings-1", tank: { fw: { uno: "1.0" } } });
+        if (m.t === "tank") tank();
+        if (m.t === "camera_set") { Object.assign(cam, m); delete cam.t; delete cam.id; this.emit({ t: "ack", id: m.id }); tank(); } }
+      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
+      close() { this.readyState = 3; this.onclose && this.onclose(); }
+    }
+    window.WebSocket = FakeWS;
+  });
+  const q = (f, a) => page.evaluate(f, a);
+  const out = {};
+  await page.goto("http://127.0.0.1:8766/settings.html?v=" + Date.now()); await page.waitForTimeout(500);
+  await page.click('#tabs button[data-tab="tank"]'); await page.waitForTimeout(300);
+  out.wbShown = await q(() => [...document.querySelectorAll("#cWb button.on")].map((b) => b.dataset.v));   // ["auto"]
+  out.nightShown = await q(() => document.getElementById("cNight").textContent);                        // OFF
+  await q(() => window.__sent.length = 0);
+  await page.click('#cWb button[data-v="home"]'); await page.waitForTimeout(100);
+  await page.click("#cNight"); await page.waitForTimeout(100);
+  out.sent = await q(() => window.__sent.filter((m) => m.t === "camera_set").map((m) => [m.wb, m.night]));  // [["home",undefined],[undefined,true]]
+  out.wbAfter = await q(() => [...document.querySelectorAll("#cWb button.on")].map((b) => b.dataset.v));   // ["home"]
+  out.nightAfter = await q(() => document.getElementById("cNight").textContent);                        // ON
+  out.hints = await q(() => document.getElementById("t-tank").innerText.includes("for looking, not for driving"));  // true
+  out.wide = await q(() => document.documentElement.scrollWidth <= 390);
+  return out;
+}
```

- [ ] **Step 2: Run it to see it fail**

Serve `firmware/esp32/web` on :8766 (`python3 -m http.server 8766 --bind 127.0.0.1`). Copy the check to `.playwright-mcp/` in the repo and run it (`browser_run_code_unsafe` `filename`).
Expected: `TypeError: Cannot read properties of null (reading 'textContent')` (no `#cNight`).

- [ ] **Step 3: Write the page code** (apply this diff):

```diff
diff --git a/firmware/esp32/web/settings.html b/firmware/esp32/web/settings.html
index a6616fc..61f2bb3 100644
--- a/firmware/esp32/web/settings.html
+++ b/firmware/esp32/web/settings.html
@@ -252,6 +252,11 @@ input[type=range] { padding: 0; border: 0; accent-color: var(--ui); background:
     <div class="row" style="margin-top:8px"><span>Brightness</span><span class="mini"><button class="btn" type="button" data-k="brightness" data-d="-1">−</button><b id="cBright">--</b><button class="btn" type="button" data-k="brightness" data-d="1">+</button></span></div>
     <div class="row"><span>Contrast</span><span class="mini"><button class="btn" type="button" data-k="contrast" data-d="-1">−</button><b id="cContrast">--</b><button class="btn" type="button" data-k="contrast" data-d="1">+</button></span></div>
     <div class="row"><span>Low-light mode</span><button class="btn tog" type="button" id="cLow" data-k="low_light">--</button></div>
+    <div class="hint">// More gain for dim rooms; keeps the frame rate.</div>
+    <div class="row"><span>Night (slow)</span><button class="btn tog" type="button" id="cNight" data-k="night">--</button></div>
+    <div class="hint">// Longer exposures, fewer pictures - for looking, not for driving.</div>
+    <label>White balance</label>
+    <div class="seg" id="cWb"><button type="button" data-v="auto">AUTO</button><button type="button" data-v="sunny">SUN</button><button type="button" data-v="cloudy">CLOUD</button><button type="button" data-v="office">OFFICE</button><button type="button" data-v="home">HOME</button></div>
     <div class="row"><span>Flip upside down</span><button class="btn tog" type="button" id="cFlip" data-k="vflip">--</button></div>
     <div class="row"><span>Mirror</span><button class="btn tog" type="button" id="cMirror" data-k="hmirror">--</button></div>
     <button class="btn warn wide" id="cReset" type="button">Reset to defaults</button>
@@ -618,12 +623,12 @@ input[type=range] { padding: 0; border: 0; accent-color: var(--ui); background:
     if (cam) {
       $("cViewers").textContent = cam.viewers + " / 2";
       $("cNow").textContent = cam.viewers ? (cam.fps / cam.viewers).toFixed(1) + " fps" + (cam.viewers > 1 ? " each" : "") + " · " + cam.kbps + " kbit/s" : "nobody watching";  // frames are counted for all viewers
-      segOn("cSize", cam.size); segOn("cFps", cam.fps_cap);
+      segOn("cSize", cam.size); segOn("cFps", cam.fps_cap); segOn("cWb", cam.wb || "auto");
       if (document.activeElement !== $("cQual")) $("cQual").value = cam.quality;
       $("cQualV").textContent = cam.quality;
       $("cBright").textContent = (cam.brightness > 0 ? "+" : "") + cam.brightness;
       $("cContrast").textContent = (cam.contrast > 0 ? "+" : "") + cam.contrast;
-      [["cLow", cam.low_light], ["cFlip", cam.vflip], ["cMirror", cam.hmirror]].forEach(function (x) {
+      [["cLow", cam.low_light], ["cNight", cam.night], ["cFlip", cam.vflip], ["cMirror", cam.hmirror]].forEach(function (x) {
         $(x[0]).textContent = x[1] ? "ON" : "OFF"; $(x[0]).classList.toggle("on", !!x[1]);
       });
     }
@@ -663,7 +668,8 @@ input[type=range] { padding: 0; border: 0; accent-color: var(--ui); background:
       if (o[b.dataset.k] !== cam[b.dataset.k]) camSet(o);
     });
   });
-  ["cLow", "cFlip", "cMirror"].forEach(function (id) {
+  $("cWb").addEventListener("click", function (e) { var b = e.target.closest("button"); if (b) camSet({ wb: b.dataset.v }); });
+  ["cLow", "cNight", "cFlip", "cMirror"].forEach(function (id) {
     $(id).addEventListener("click", function () { if (!cam) return; var o = {}; o[$(id).dataset.k] = !cam[$(id).dataset.k]; camSet(o); });
   });
   $("cReset").addEventListener("click", function () { camSet({ reset: true }); });
```

- [ ] **Step 4: Run the check and the bridge build**

Run: the check again.
Expected: `{"wbShown":["auto"],"nightShown":"OFF","sent":[["home",null],[null,true]],"wbAfter":["home"],"nightAfter":"ON","hints":true,"wide":true}`; the console shows only the test server's 404s.

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge | grep pack_web`
Expected: `/settings` about 22.2 KB (budget 36 KB).

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/web/settings.html firmware/esp32/web/test/camera_tuning_check.js
git commit -m "feat(bridge): Camera card - white balance and night (bridge LLD R24)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 5: Final review (fresh reviewer), then its fixes test-first

### Task 6: On the tank (owner present; the tank stands, nothing moves)

- [ ] **Step 0: Before flashing, measure the old firmware.** QVGA (`camera_set` `"QVGA"`), low light off. Run a session script that grabs 20 pictures and prints their median brightness (0–255) and the frame rate; the room as it is. Then low light on, the same. Write both down (the "before").
- [ ] **Step 1: Flash the bridge over the air:**

```bash
cd firmware/esp32
export CQ_OTA_PASSWORD="$(sed -n 's/.*CQ_OTA_PASSWORD[^"]*"\([^"]*\)".*/\1/p' include/secrets.h)"
~/.platformio/penv/bin/pio run -e bridge_ota -t upload --upload-port 192.168.31.167 2>&1 | grep -vF -- "$CQ_OTA_PASSWORD" | tail -3
unset CQ_OTA_PASSWORD
```
Then the `tank` reply shows the **same** size, quality, vflip … as before (carried over), with `wb` "auto" and `night` false.
- [ ] **Step 2: After.** The same brightness and frame-rate measurement, low light off, then on. Expected: clearly brighter in the dim room, with low light more so, and the frame rate unchanged by low light (≥ 10 / s at QVGA).
- [ ] **Step 3: YOLO in the dim room:** the owner stands at the 1 m mark; a 10 s `cq-record`; the person score as in §139 (median; how many ≥ 0.35). Expected: much higher than §139's median 0.0–0.08.
- [ ] **Step 4: Night** on for a moment: brighter still, fewer pictures a second (the reason it's for looking only). Then off.
- [ ] **Step 5: White balance:** the owner picks Auto / Office / Home by eye on the Camera card.
- [ ] **Step 6:** Write the numbers in the ledger, delete the pictures and recordings, and set the camera back to VGA.

### Task 7: Docs as built

**Files:**
- `docs/lld/esp32-bridge-lld.md` (R24.3 as built, with the numbers);
- `docs/design-notes.md` (before / after, the source-archive change);
- `CLAUDE.md` (plan list: `2026-10-24-plan-x-camera.md`).

- [ ] **Step 1: Write them.**
- [ ] **Step 2: Commit**

```bash
git add docs CLAUDE.md
git commit -m "docs: Plan X camera - as built and tank results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
