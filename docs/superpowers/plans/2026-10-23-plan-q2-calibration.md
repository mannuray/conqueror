# Plan Q2 — Camera calibration and the camera model (Spec 7 phase 2, with bridge LLD R22 `cam_set`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The AI engine knows exactly how its camera sees the floor: a calibrated lens, the mount (height, position, real tilt) and the servo maps. With them, `CameraModel` turns any pixel into a floor point and back. A `cam_set` command on the bridge sets the camera to exact angles for the servo calibration, and later for the driving tilt.

**Architecture:**
- **Bridge (R22):** `cam_set` in degrees from the centre, through the existing `CAM_SET` frame. It uses the camera stick's permission rule and refuses angles outside pan ±90 / tilt −20…+60 with 3018 `OUT_OF_RANGE`. `BridgeLink.cam_set` awaits the ack.
- **Engine, pure (`domain/`):**
  - `camera.py`: `Calibration`, `CameraModel` (pinhole + OpenCV distortion, mount, pan / tilt maps), `from_servo`;
  - `calib_math.py`: laser lines, the horizon tilt, pan from a vertical line, the servo fits, the floor-mark error, the tilt study;
  - `Pose` in `types.py`.
- **Adapters:** `calibration_store.py` (YAML) and `lens_opencv.py` (the checkerboard, corners, `calibrateCamera`).
- **App:** `cq-calibrate lens | mount | servos | study`, whose steps take their inputs as functions.

**Tech Stack:**
- **Bridge:** C++17, PlatformIO, Unity.
- **Engine:** Python 3.12 + uv, numpy < 2.3, OpenCV (`opencv-python` 4.11), PyYAML, pytest + pytest-asyncio, ruff, pyright strict.

**Spec:**
- `docs/superpowers/specs/2026-10-20-07-ai-module1-map-design.md` §5 and phase 2;
- `docs/lld/ai-engine-lld.md` §5.3 and §8 (amended 2026-10-23);
- `docs/lld/esp32-bridge-lld.md` Revision 22 (approved 2026-10-23);
- design-notes §123 (the calibration decisions) and §124 (cam_set).

## Global Constraints
- **`cam_set`:**
  - `{"t":"cam_set","pan":deg,"tilt":deg,"id":n}`: degrees from the centre, right / up = +, 0.1° steps; servo value = 900 − deg × 10.
  - pan −90…90, tilt −20…60; outside → 3018 `OUT_OF_RANGE` "Out of range - pan -90..90, tilt -20..60 degrees"; a missing or non-number input → `INVALID_COMMAND`.
  - The camera rule (`LinkOwner::onCamera`): `BUSY` in OTA. Never takes ownership. Acked after the Uno's ACK.
- **The engine never sends motion;** `cam_set` is its only command.
- **Frames and conventions:**
  - The room: x forward at the start, y left, z up.
  - The camera: image x right, y down.
  - Pan + = right, tilt + = up. Telemetry's servo angles: from-centre = 90 − servo.
- **QVGA** (320 × 240) calibration; the file is `ai-engine/config/calibration.yaml`, refused for another size.
- **Lens accepted below 0.5 px RMS. Floor marks at 50 / 100 cm from the tracks' front: within 2 cm.**
- **Tilt study tilts:** 0, −5, −10, −15, −20; servo points: tilt −15, −5, 5, 15, 30; pan −30, −15, 0, 15, 30 (camera level).
- **No change to the Uno, the protocol, generated files or any safety rule.**
- Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch `plan-q2` from `main`.

## Review Focus
1. **Sign conventions across the whole chain:** a `cam_set` pan of +15 must turn the camera **right**, and the model's `real_pan_deg()` + must look right. Telemetry's 75° servo pan means +15. Pinned by `test_cam_set` (750), `test_pan_right_looks_to_the_right`, `test_servo_angles_from_telemetry_become_degrees_from_the_centre`; the stand check confirms the direction physically.
2. **Pixels near the picture's edge with real distortion** must round-trip, without NaN, for any lens the calibration can produce (RMS < 0.5 px, k1 about −0.1…−0.3 at QVGA). Pinned by `test_pixel_floor_round_trips_with_distortion_pan_and_tilt` (k1 −0.1). The reviewer checks the undistortion's behaviour for k1 −0.3 at the corners.
3. **The laser line finder on real pictures:** ambient red objects, auto-exposure, a line too faint. It returns None, never a wrong row, when nothing stands out (`MIN_LINE_SCORE`). Pinned by `test_no_line_no_answer`; the tank session is the real test.
4. **`cam_set` while a phone drives:** refused `NOT_OWNER` for others, and never steals or holds the drive. Pinned by `test_cam_set_follows_the_camera_rule`.
5. **The tool blocks the event loop during clicks or input,** and the bridge drops a client silent for 3 s. `_click` and `_ask` keep the loop running (`waitKey` + `asyncio.sleep(0)`, `to_thread(input)`); the reviewer checks no other blocking call is in `_run`.

---

### Task 1: `cam_set` on the bridge (bridge LLD R22)

**Files:**
- Modify: `firmware/esp32/src/core/ws_codec.h`, `ws_codec.cpp`, `ws_codec_keys.cpp`, `ws_codec_internal.h`, `ws_json.h`, `ws_json_reply.cpp`
- Modify: `firmware/esp32/src/app/drive_controller.{h,cpp}`, `firmware/esp32/src/app/command_router.cpp`
- Test: `firmware/esp32/test/test_ws_codec`, `test_ws_json`, `test_drive_controller`

**Interfaces:**
- Produces:
  - `WsCmdKind::kCamSet`; `WsCommand::cam_pan_dd`, `cam_tilt_dd`, `cam_in_range`
  - `BridgeReason::kOutOfRange` (3018)
  - `DriveController::onCamSet(int fd, int32_t id, const WsCommand&)`

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/firmware/esp32/test/test_drive_controller/test_main.cpp b/firmware/esp32/test/test_drive_controller/test_main.cpp
index 6f4dcfd..a7613f6 100644
--- a/firmware/esp32/test/test_drive_controller/test_main.cpp
+++ b/firmware/esp32/test/test_drive_controller/test_main.cpp
@@ -231,6 +231,58 @@ void test_cam_home_waits_in_the_queue() {
   TEST_ASSERT_FALSE(uno->empty());
 }
 
+// Bridge LLD R22: cam_set queues CAM_SET with the servo values and is answered after the Uno's ACK; the camera
+// rule (the driver while someone drives, anyone otherwise; never ownership); refused during OTA and out of range.
+namespace {
+const cqtest::Sent* sendQueued(uint32_t now) {
+  uno->tick(now);
+  wire.drain(*tx);
+  return wire.last(FT::kCamSet);
+}
+}  // namespace
+
+void test_cam_set_reaches_the_uno_and_is_acked() {
+  sync();
+  drive->onCamSet(3, 7, P("{\"t\":\"cam_set\",\"pan\":15,\"tilt\":-15,\"id\":7}"));
+  TEST_ASSERT_FALSE(has(3, "nack"));
+  const cqtest::Sent* s = sendQueued(30);
+  TEST_ASSERT_NOT_NULL(s);
+  proto::CamSetMsg m{};
+  TEST_ASSERT_TRUE(proto::decode(s->payload, s->len, m));
+  TEST_ASSERT_EQUAL_INT16(750, m.pan);
+  TEST_ASSERT_EQUAL_INT16(1050, m.tilt);
+  uint8_t f[16];
+  const size_t n = cqtest::frame(proto::AckMsg{s->seq, 0}, 9, f);
+  unoSends(f, n, 40);
+  pump(40);
+  TEST_ASSERT_TRUE(has(3, "\"t\":\"ack\",\"id\":7"));
+  TEST_ASSERT_EQUAL_INT(-1, owner->driverFd());  // a camera command never takes ownership
+}
+
+void test_cam_set_follows_the_camera_rule() {
+  sync();
+  driveFrom(4, 30, 30);  // phone 4 drives
+  drive->onCamSet(3, 8, P("{\"t\":\"cam_set\",\"pan\":0,\"tilt\":0,\"id\":8}"));
+  TEST_ASSERT_TRUE(has(3, "NOT_OWNER"));
+  drive->onCamSet(4, 9, P("{\"t\":\"cam_set\",\"pan\":0,\"tilt\":0,\"id\":9}"));
+  TEST_ASSERT_FALSE(has(4, "nack"));
+}
+
+void test_cam_set_out_of_range_and_during_ota() {
+  sync();
+  drive->onCamSet(3, 10, P("{\"t\":\"cam_set\",\"pan\":0,\"tilt\":-30,\"id\":10}"));
+  TEST_ASSERT_TRUE(has(3, "OUT_OF_RANGE"));
+  TEST_ASSERT_TRUE(uno->empty());
+  drive->startOta(100);
+  drive->onCamSet(3, 11, P("{\"t\":\"cam_set\",\"pan\":0,\"tilt\":0,\"id\":11}"));
+  TEST_ASSERT_TRUE(has(3, "BUSY"));
+}
+
+void test_cam_set_before_sync_is_body_offline() {
+  drive->onCamSet(3, 12, P("{\"t\":\"cam_set\",\"pan\":0,\"tilt\":0,\"id\":12}"));
+  TEST_ASSERT_TRUE(has(3, "BODY_OFFLINE"));
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_drive_before_sync_is_body_offline);
@@ -247,5 +299,9 @@ int main() {
   RUN_TEST(test_ir_takeover_drops_the_driver);
   RUN_TEST(test_ota_refuses_drive_busy_and_closes_clients);
   RUN_TEST(test_cam_home_waits_in_the_queue);
+  RUN_TEST(test_cam_set_reaches_the_uno_and_is_acked);
+  RUN_TEST(test_cam_set_follows_the_camera_rule);
+  RUN_TEST(test_cam_set_out_of_range_and_during_ota);
+  RUN_TEST(test_cam_set_before_sync_is_body_offline);
   return UNITY_END();
 }
diff --git a/firmware/esp32/test/test_ws_codec/test_main.cpp b/firmware/esp32/test/test_ws_codec/test_main.cpp
index da24270..3e6f503 100644
--- a/firmware/esp32/test/test_ws_codec/test_main.cpp
+++ b/firmware/esp32/test/test_ws_codec/test_main.cpp
@@ -339,6 +339,32 @@ void test_calib_gyro_scale() {
   TEST_ASSERT_EQUAL_INT8(2, P("{\"t\":\"calib\",\"reset\":\"gyro_scale\"}").calib_reset);
 }
 
+// Bridge LLD R22: cam_set - degrees from the centre (right / up = +), 0.1 deg steps -> the Uno's servo values
+// (900 - deg x 10); outside pan -90..90 / tilt -20..60 it is still a cam_set, marked out of range (its own nack).
+void test_cam_set() {
+  WsCommand c = P("{\"t\":\"cam_set\",\"pan\":15,\"tilt\":-15,\"id\":7}");
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kCamSet), K(c.kind));
+  TEST_ASSERT_TRUE(c.cam_in_range);
+  TEST_ASSERT_EQUAL_INT16(750, c.cam_pan_dd);
+  TEST_ASSERT_EQUAL_INT16(1050, c.cam_tilt_dd);
+  c = P("{\"t\":\"cam_set\",\"pan\":-90,\"tilt\":60}");
+  TEST_ASSERT_TRUE(c.cam_in_range);
+  TEST_ASSERT_EQUAL_INT16(1800, c.cam_pan_dd);
+  TEST_ASSERT_EQUAL_INT16(300, c.cam_tilt_dd);
+  c = P("{\"t\":\"cam_set\",\"pan\":0.2,\"tilt\":-20}");
+  TEST_ASSERT_EQUAL_INT16(898, c.cam_pan_dd);  // 0.1 deg steps
+  TEST_ASSERT_EQUAL_INT16(1100, c.cam_tilt_dd);
+  const char* outside[] = {"{\"t\":\"cam_set\",\"pan\":91,\"tilt\":0}", "{\"t\":\"cam_set\",\"pan\":0,\"tilt\":61}",
+                           "{\"t\":\"cam_set\",\"pan\":0,\"tilt\":-21}", "{\"t\":\"cam_set\",\"pan\":-90.1,\"tilt\":0}"};
+  for (const char* s : outside) {
+    c = P(s);
+    TEST_ASSERT_EQUAL_MESSAGE(K(WsCmdKind::kCamSet), K(c.kind), s);
+    TEST_ASSERT_FALSE_MESSAGE(c.cam_in_range, s);
+  }
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"cam_set\",\"pan\":10}").kind));
+  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"cam_set\",\"pan\":\"x\",\"tilt\":0}").kind));
+}
+
 void test_setting_and_calib_reset() {
   WsCommand c = P("{\"t\":\"setting\",\"setting_id\":66}");
   TEST_ASSERT_EQUAL(K(WsCmdKind::kSetting), K(c.kind));
@@ -472,6 +498,7 @@ int main() {
   RUN_TEST(test_turn_pivot_field);
   RUN_TEST(test_wifi_commands);
   RUN_TEST(test_setting_and_calib_reset);
+  RUN_TEST(test_cam_set);
   RUN_TEST(test_calib_gyro_scale);
   RUN_TEST(test_tank_log_selftest);
   RUN_TEST(test_camera_set_fields_and_ranges);
diff --git a/firmware/esp32/test/test_ws_json/test_main.cpp b/firmware/esp32/test/test_ws_json/test_main.cpp
index 5546dc6..a02e2dd 100644
--- a/firmware/esp32/test/test_ws_json/test_main.cpp
+++ b/firmware/esp32/test/test_ws_json/test_main.cpp
@@ -556,8 +556,18 @@ void test_logo_refusals() {
   TEST_ASSERT_NOT_NULL(strstr(buf, "\"code\":\"COMMAND_3017\",\"reason\":\"NOT_RUNNING\""));
 }
 
+// Bridge LLD R22: the out-of-range refusal says the limits.
+void test_out_of_range_nack_says_the_limits() {
+  jsonNack(buf, sizeof(buf), 7, BridgeReason::kOutOfRange);
+  TEST_ASSERT_EQUAL_STRING(
+      "{\"t\":\"nack\",\"id\":7,\"code\":\"COMMAND_3018\",\"reason\":\"OUT_OF_RANGE\","
+      "\"message\":\"Out of range - pan -90..90, tilt -20..60 degrees\"}",
+      buf);
+}
+
 int main() {
   UNITY_BEGIN();
+  RUN_TEST(test_out_of_range_nack_says_the_limits);
   RUN_TEST(test_ack_reason_table_matches_generated_codes);
   RUN_TEST(test_tf_scaling_and_sentinels);
   RUN_TEST(test_ts_flags_owner_and_battery);
```

- [ ] **Step 2: Run them to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_codec -f test_ws_json -f test_drive_controller`
Expected: FAIL to compile — `no member named 'onCamSet' in 'cq::app::DriveController'` (and `kCamSet`, `kOutOfRange`).

- [ ] **Step 3: Implement**

```diff
diff --git a/firmware/esp32/src/app/command_router.cpp b/firmware/esp32/src/app/command_router.cpp
index bfb6755..3fa1fad 100644
--- a/firmware/esp32/src/app/command_router.cpp
+++ b/firmware/esp32/src/app/command_router.cpp
@@ -15,6 +15,7 @@ const CommandRouter::Route CommandRouter::kRoutes[] = {
     {K::kDrive, [](C& r, int fd, int32_t, const W& c, uint32_t now) { r.m_drive.onDrive(fd, c, now); }},
     {K::kCamJoy, [](C& r, int fd, int32_t, const W& c, uint32_t) { r.m_drive.onCamJoy(fd, c); }},
     {K::kCamHome, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onUnoRequest(fd, id, c); }},
+    {K::kCamSet, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onCamSet(fd, id, c); }},
     {K::kSound, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onUnoRequest(fd, id, c); }},
     {K::kAudioMode, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onUnoRequest(fd, id, c); }},
     {K::kCamLock, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onUnoRequest(fd, id, c); }},
diff --git a/firmware/esp32/src/app/drive_controller.cpp b/firmware/esp32/src/app/drive_controller.cpp
index f4af9bb..f36e9e0 100644
--- a/firmware/esp32/src/app/drive_controller.cpp
+++ b/firmware/esp32/src/app/drive_controller.cpp
@@ -130,6 +130,20 @@ void DriveController::onUnoRequest(int fd, int32_t id, const core::WsCommand& c)
   if (!m_uno.request(requestOf(fd, id, c))) m_out.nack(fd, id, core::BridgeReason::kBusy);  // R15.5: BUSY only when full
 }
 
+// R22: the camera rule (as cam_home), never ownership; OUT_OF_RANGE first, so the sender learns the limits.
+void DriveController::onCamSet(int fd, int32_t id, const core::WsCommand& c) {
+  if (!c.cam_in_range) return m_out.nack(fd, id, core::BridgeReason::kOutOfRange);
+  core::BridgeReason why = core::BridgeReason::kBusy;
+  if (requestRefused(c, m_owner.onCamera(fd), why)) return m_out.nack(fd, id, why);
+  core::AckedOp op;
+  op.kind = core::AckedOp::kCamSet;
+  op.fd = fd;
+  op.id = id;
+  op.pan_dd = c.cam_pan_dd;
+  op.tilt_dd = c.cam_tilt_dd;
+  if (!m_uno.request(op)) m_out.nack(fd, id, core::BridgeReason::kBusy);
+}
+
 void DriveController::onRelease(int fd, int32_t id, uint32_t now) {
   applyOwner(m_owner.onRelease(fd), now);
   if (m_motion.ownedBy(fd)) m_motion.end(core::MotionEvent::kRelease, now);  // review I1: release ends a primitive
diff --git a/firmware/esp32/src/app/drive_controller.h b/firmware/esp32/src/app/drive_controller.h
index ce05fed..4e9a69b 100644
--- a/firmware/esp32/src/app/drive_controller.h
+++ b/firmware/esp32/src/app/drive_controller.h
@@ -34,6 +34,7 @@ class DriveController {
   void onDrive(int fd, const core::WsCommand& c, uint32_t now);
   void onCamJoy(int fd, const core::WsCommand& c);
   void onUnoRequest(int fd, int32_t id, const core::WsCommand& c);  // cam_home / cam_lock / sound / audio_mode
+  void onCamSet(int fd, int32_t id, const core::WsCommand& c);      // R22: the camera to exact angles
   void onRelease(int fd, int32_t id, uint32_t now);
   void onDebug(int fd, int32_t id, const core::WsCommand& c);
   void onClientOpen(int fd, uint32_t now);
diff --git a/firmware/esp32/src/core/ws_codec.cpp b/firmware/esp32/src/core/ws_codec.cpp
index e85e845..f19dfce 100644
--- a/firmware/esp32/src/core/ws_codec.cpp
+++ b/firmware/esp32/src/core/ws_codec.cpp
@@ -33,7 +33,7 @@ const KindName kKinds[] = {
     {"setting", WsCmdKind::kSetting},      {"restart", WsCmdKind::kRestart},
     {"tank", WsCmdKind::kTank},            {"log", WsCmdKind::kLog},
     {"camera_set", WsCmdKind::kCameraSet}, {"selftest", WsCmdKind::kSelfTest},
-    {"app_set", WsCmdKind::kAppSet},       {"logo_run", WsCmdKind::kLogoRun},
+    {"app_set", WsCmdKind::kAppSet},       {"logo_run", WsCmdKind::kLogoRun}, {"cam_set", WsCmdKind::kCamSet},
     {"logo_step", WsCmdKind::kLogoStep},   {"logo_stop", WsCmdKind::kLogoStop},
 };
 
@@ -103,6 +103,17 @@ bool checkCalib(WsCommand& c, const Seen& s) {
                                                          : -1;
   return c.calib_reset >= 0;
 }
+// R22: both angles; the servo values as LOOK makes them (right / up are the smaller angles); the range is the
+// servos' (pan -90..90, tilt -20..60 - CamLite 300..1100): outside it stays a cam_set, refused with OUT_OF_RANGE.
+bool checkCamSet(WsCommand& c, const Seen& s) {
+  if (!s.pan || !s.tilt) return false;
+  c.cam_in_range = s.pan_d10 >= -900 && s.pan_d10 <= 900 && s.tilt_d10 >= -200 && s.tilt_d10 <= 600;
+  if (c.cam_in_range) {
+    c.cam_pan_dd = static_cast<int16_t>(900 - s.pan_d10);
+    c.cam_tilt_dd = static_cast<int16_t>(900 - s.tilt_d10);
+  }
+  return true;
+}
 bool checkDriveRate(WsCommand&, const Seen& s) { return s.v && s.mf.rate; }  // review M20
 bool checkCameraSet(WsCommand& c, const Seen& s) {
   c.cam.reset = s.reset_true;
@@ -126,6 +137,7 @@ const CheckRow kChecks[] = {
     {WsCmdKind::kCalib, checkCalib},         {WsCmdKind::kDriveRate, checkDriveRate},
     {WsCmdKind::kDebug, checkOn},            {WsCmdKind::kCameraSet, checkCameraSet},
     {WsCmdKind::kAppSet, checkAppSet},       {WsCmdKind::kLogoRun, checkLogoRun},
+    {WsCmdKind::kCamSet, checkCamSet},
 };
 
 bool complete(WsCommand& c, const codec::Seen& s) {
diff --git a/firmware/esp32/src/core/ws_codec.h b/firmware/esp32/src/core/ws_codec.h
index db81d14..34adb49 100644
--- a/firmware/esp32/src/core/ws_codec.h
+++ b/firmware/esp32/src/core/ws_codec.h
@@ -42,6 +42,7 @@ enum class WsCmdKind : uint8_t {
   kLogoRun,     // Plan N3 (Tank Logo LLD §20.4): {"t":"logo_run","prog":n,"step"?:bool} -> run a checked program
   kLogoStep,    // {"t":"logo_step"} -> the next line in step mode
   kLogoStop,    // {"t":"logo_stop"} -> end the running program (7040)
+  kCamSet,      // bridge LLD R22: {"t":"cam_set","pan":deg,"tilt":deg} -> CAM_SET (degrees from the centre)
   kUnsupported,  // valid JSON, "t" not in the Basic Drive slice -> nack INVALID_COMMAND
   kInvalid,      // not JSON / no "t" / bad field -> nack INVALID_COMMAND
 };
@@ -93,6 +94,9 @@ struct WsCommand {
   uint16_t gyro_scale = 0;      // R19: parts per 10 000 (10230 = x1.0230)
   uint32_t log_since = 0;     // log (Plan O)
   CamFields cam;              // camera_set
+  int16_t cam_pan_dd = 900;   // cam_set (R22): the Uno's servo values, 900 - deg x 10 ...
+  int16_t cam_tilt_dd = 900;
+  bool cam_in_range = false;  // ... and pan -90..90 / tilt -20..60 (else nack OUT_OF_RANGE)
   char ai_url[97] = "";       // app_set ("" clears it)
   bool has_ai_url = false;
   uint32_t prog = 0;          // logo_run: the number the check returned
diff --git a/firmware/esp32/src/core/ws_codec_internal.h b/firmware/esp32/src/core/ws_codec_internal.h
index d9ad675..06b5f04 100644
--- a/firmware/esp32/src/core/ws_codec_internal.h
+++ b/firmware/esp32/src/core/ws_codec_internal.h
@@ -28,6 +28,8 @@ struct Seen {
   char reset[16] = "";                                      // calib reset
   bool reset_true = false;                                  // camera_set reset (Plan O)
   bool url_bad = false;                                     // app_set
+  bool pan = false, tilt = false;                           // cam_set (R22)
+  int32_t pan_d10 = 0, tilt_d10 = 0;                        // ... their 0.1 deg
   bool prog = false;                                        // logo_run (Plan N3)
 };
 
diff --git a/firmware/esp32/src/core/ws_codec_keys.cpp b/firmware/esp32/src/core/ws_codec_keys.cpp
index ab1de00..baaaf7f 100644
--- a/firmware/esp32/src/core/ws_codec_keys.cpp
+++ b/firmware/esp32/src/core/ws_codec_keys.cpp
@@ -223,6 +223,17 @@ bool readGyroScale(Reader& r, WsCommand& c, Seen&) {  // R19 calib set
   return true;
 }
 
+// cam_set (R22): degrees from the centre, kept in 0.1 deg; the range is checked with the command (OUT_OF_RANGE).
+bool readAngle(Reader& r, int32_t& d10, bool& seen) {
+  double n = 0;
+  if (!r.num(n, nullptr, 0) || n < -1000.0 || n > 1000.0) return false;
+  d10 = static_cast<int32_t>(lround(n * 10.0));
+  seen = true;
+  return true;
+}
+bool readPan(Reader& r, WsCommand&, Seen& s) { return readAngle(r, s.pan_d10, s.pan); }
+bool readTilt(Reader& r, WsCommand&, Seen& s) { return readAngle(r, s.tilt_d10, s.tilt); }
+
 bool readProg(Reader& r, WsCommand& c, Seen& s) {  // logo_run (Plan N3): 1..2^32-1
   double n = 0;
   if (!wholeIn(r, 1, 4294967295.0, n)) return false;
@@ -250,7 +261,7 @@ const KeyRow kKeys[] = {
     {"value", readValue},   {"reset", readReset},     {"since", readSince},     {"size", readSize},
     {"quality", readQuality}, {"fps_cap", readFpsCap}, {"brightness", readBrightness},
     {"contrast", readContrast}, {"low_light", readLowLight}, {"vflip", readVflip}, {"hmirror", readHmirror},
-    {"ai_url", readAiUrl},  {"setting_id", readSettingId}, {"counts_per_m", readCpm}, {"gyro_scale", readGyroScale}, {"prog", readProg},
+    {"ai_url", readAiUrl},  {"setting_id", readSettingId}, {"counts_per_m", readCpm}, {"gyro_scale", readGyroScale}, {"pan", readPan}, {"tilt", readTilt}, {"prog", readProg},
     {"step", readStep},
 };
 
diff --git a/firmware/esp32/src/core/ws_json.h b/firmware/esp32/src/core/ws_json.h
index d43ccd3..94b045c 100644
--- a/firmware/esp32/src/core/ws_json.h
+++ b/firmware/esp32/src/core/ws_json.h
@@ -21,7 +21,8 @@ constexpr size_t kWsBigMax = 3584;  // Plan O: tank / log replies - sent through
 // Reasons the bridge itself produces (not on the Uno's ack_status list).
 enum class BridgeReason : uint8_t { kNotOwner, kBusy, kBodyOffline, kRateLimited, kInvalidCommand, kTimeout, kUnavailable,
                                     kStopped,  // kStopped: design-notes §72 STOP hold
-                                    kCheckAgain, kNotRunning };  // Plan N3: logo_run / logo_step / logo_stop
+                                    kCheckAgain, kNotRunning,  // Plan N3: logo_run / logo_step / logo_stop
+                                    kOutOfRange };             // R22: cam_set outside the servos' range
 
 struct OwnerView {
   const char* owner;   // "none" | "human" | "ir"
diff --git a/firmware/esp32/src/core/ws_json_reply.cpp b/firmware/esp32/src/core/ws_json_reply.cpp
index daa61f4..f76e919 100644
--- a/firmware/esp32/src/core/ws_json_reply.cpp
+++ b/firmware/esp32/src/core/ws_json_reply.cpp
@@ -56,8 +56,9 @@ constexpr ReasonInfo kBridgeReasons[] = {
     {3011, "STOPPED", "Stopped by another device - release the stick to drive again"},  // design-notes §72
     {3016, "CHECK_AGAIN", "The program changed - check it again"},                       // Plan N3 (bridge only)
     {3017, "NOT_RUNNING", "No program is running"},
+    {3018, "OUT_OF_RANGE", "Out of range - pan -90..90, tilt -20..60 degrees"},  // R22 (bridge only)
 };
-static_assert(sizeof(kBridgeReasons) / sizeof(kBridgeReasons[0]) == static_cast<size_t>(BridgeReason::kNotRunning) + 1,
+static_assert(sizeof(kBridgeReasons) / sizeof(kBridgeReasons[0]) == static_cast<size_t>(BridgeReason::kOutOfRange) + 1,
               "one row per BridgeReason");
 
 const char* domain(uint16_t code) {
```

- [ ] **Step 4: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS, 660 test cases.
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge` → SUCCESS; `uv run --with lizard==1.24.1 python scripts/size_check.py` → no output.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src firmware/esp32/test/test_ws_codec firmware/esp32/test/test_ws_json firmware/esp32/test/test_drive_controller
git commit -m "feat(bridge): R22 cam_set - the camera to exact angles, refused outside pan -90..90 / tilt -20..60

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `BridgeLink.cam_set` (the engine's only command)

**Files:**
- Modify: `ai-engine/src/conqueror_ai/adapters/bridge_ws.py`, `ai-engine/tests/fake_bridge.py`
- Test: `ai-engine/tests/test_cam_set.py`

**Interfaces:**
- Consumes (Task 1): the `cam_set` message and its ack / nack.
- Produces:
  - `BridgeLink.connected` (property)
  - `async BridgeLink.cam_set(pan_deg: float, tilt_deg: float, timeout_s: float = 3.0) -> None`
  - `CommandRefused(reason, message)` (`.reason`)

- [ ] **Step 1: Write the failing test** — the fake bridge answers `cam_set`:

```diff
diff --git a/ai-engine/tests/fake_bridge.py b/ai-engine/tests/fake_bridge.py
index c44b04b..8492c48 100644
--- a/ai-engine/tests/fake_bridge.py
+++ b/ai-engine/tests/fake_bridge.py
@@ -70,6 +70,20 @@ class FakeBridge:
                     )
                 elif t == "calib":
                     await ws.send(json.dumps({"t": "calib", "counts_per_m": 7514, "roll_mm": 27}))
+                elif t == "cam_set":  # bridge LLD R22: pan -90..90, tilt -20..60 degrees
+                    ok = -90 <= m["pan"] <= 90 and -20 <= m["tilt"] <= 60
+                    reply = (
+                        {"t": "ack", "id": m["id"]}
+                        if ok
+                        else {
+                            "t": "nack",
+                            "id": m["id"],
+                            "code": "COMMAND_3018",
+                            "reason": "OUT_OF_RANGE",
+                            "message": "Out of range - pan -90..90, tilt -20..60 degrees",
+                        }
+                    )
+                    await ws.send(json.dumps(reply))
         finally:
             if sender is not None:
                 sender.cancel()
```

and create `ai-engine/tests/test_cam_set.py`:

```python
"""BridgeLink.cam_set (bridge LLD R22): the camera to exact angles - the engine's only command."""

from __future__ import annotations

import asyncio

import pytest

from conqueror_ai.adapters.bridge_ws import BridgeLink, CommandRefused
from tests.fake_bridge import FakeBridge


async def _connected(link: BridgeLink) -> None:
    for _ in range(200):
        if link.connected:
            return
        await asyncio.sleep(0.01)
    raise AssertionError("never connected")


async def test_cam_set_sends_degrees_and_returns_on_the_ack() -> None:
    bridge = FakeBridge()
    await bridge.start()
    link = BridgeLink("127.0.0.1", bridge.ws_port)
    stop = asyncio.Event()
    task = asyncio.create_task(link.run(stop))
    try:
        await _connected(link)
        await link.cam_set(10.0, -15.0)
        sent = [m for m in bridge.received if m.get("t") == "cam_set"]
        assert len(sent) == 1
        assert sent[0]["pan"] == 10.0 and sent[0]["tilt"] == -15.0 and isinstance(sent[0]["id"], int)
    finally:
        stop.set()
        await link.close()
        await asyncio.wait_for(task, 3)
        await bridge.stop()


async def test_cam_set_out_of_range_raises_with_the_reason() -> None:
    bridge = FakeBridge()
    await bridge.start()
    link = BridgeLink("127.0.0.1", bridge.ws_port)
    stop = asyncio.Event()
    task = asyncio.create_task(link.run(stop))
    try:
        await _connected(link)
        with pytest.raises(CommandRefused) as e:
            await link.cam_set(0.0, -30.0)
        assert e.value.reason == "OUT_OF_RANGE"
        assert "-20..60" in str(e.value)
    finally:
        stop.set()
        await link.close()
        await asyncio.wait_for(task, 3)
        await bridge.stop()


async def test_cam_set_without_a_connection_raises() -> None:
    link = BridgeLink("127.0.0.1", 9)
    with pytest.raises(ConnectionError):
        await link.cam_set(0.0, 0.0)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd ai-engine && uv run pytest -q tests/test_cam_set.py`
Expected: FAIL — `cannot import name 'CommandRefused'`.

- [ ] **Step 3: Implement**

```diff
diff --git a/ai-engine/src/conqueror_ai/adapters/bridge_ws.py b/ai-engine/src/conqueror_ai/adapters/bridge_ws.py
index c0f6eaf..65fa5c1 100644
--- a/ai-engine/src/conqueror_ai/adapters/bridge_ws.py
+++ b/ai-engine/src/conqueror_ai/adapters/bridge_ws.py
@@ -1,4 +1,5 @@
-"""The bridge's WebSocket, read only (AI engine LLD §6): `hello`, the 50 Hz debug stream, pings for the clock.
+"""The bridge's WebSocket (AI engine LLD §6): `hello`, the 50 Hz debug stream, pings for the clock, and one
+command - `cam_set`, the camera to exact angles (bridge LLD R22).
 
 It never sends anything that moves the tank. Raw messages go to an optional tap (the recorder).
 """
@@ -20,6 +21,14 @@ from conqueror_ai.domain.types import TelemSample
 RawTap = Callable[[float, str], None]  # (laptop receive time, raw JSON text)
 
 
+class CommandRefused(Exception):
+    """The bridge answered a command with a nack: `reason` (OUT_OF_RANGE, NOT_OWNER, BUSY, ...) + message."""
+
+    def __init__(self, reason: str, message: str) -> None:
+        super().__init__(f"{reason}: {message}")
+        self.reason = reason
+
+
 class BridgeLink:
     def __init__(self, host: str, port: int = 80, client: str = "ai-map", silent_s: float = 3.0) -> None:
         self.url = f"ws://{host}:{port}/ws"
@@ -31,6 +40,35 @@ class BridgeLink:
         self.counts_per_m: int | None = None
         self.last_sample_s: float | None = None
         self._ws: ClientConnection | None = None
+        self._next_id = 0
+        self._waiting: dict[int, asyncio.Future[dict[str, Any]]] = {}
+
+    @property
+    def connected(self) -> bool:
+        return self._ws is not None
+
+    async def cam_set(self, pan_deg: float, tilt_deg: float, timeout_s: float = 3.0) -> None:
+        """The camera to (pan, tilt) degrees from the centre, right / up = + (bridge LLD R22).
+
+        Returns on the ack. Raises CommandRefused on a nack (OUT_OF_RANGE: pan -90..90, tilt -20..60),
+        ConnectionError when not connected, TimeoutError without an answer. A camera angle is not motion:
+        the engine still never moves the tank."""
+        ws = self._ws
+        if ws is None:
+            raise ConnectionError("not connected to the bridge")
+        self._next_id += 1
+        mid = self._next_id
+        reply: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()
+        self._waiting[mid] = reply
+        try:
+            await self._send(
+                ws, {"t": "cam_set", "pan": round(pan_deg, 1), "tilt": round(tilt_deg, 1), "id": mid}
+            )
+            m = await asyncio.wait_for(reply, timeout_s)
+        finally:
+            self._waiting.pop(mid, None)
+        if m.get("t") == "nack":
+            raise CommandRefused(str(m.get("reason", "")), str(m.get("message", "")))
 
     async def run(self, stop: asyncio.Event) -> None:
         """Connects (again after a drop, with back-off) until `stop` is set."""
@@ -114,6 +152,11 @@ class BridgeLink:
             t0, us = m.get("t0"), m.get("bridge_us")
             if isinstance(t0, int | float) and isinstance(us, int):
                 self.clock.add(float(t0) / 1000.0, now, us)
+        elif kind in ("ack", "nack"):
+            mid = m.get("id")
+            fut = self._waiting.get(mid) if isinstance(mid, int) else None
+            if fut is not None and not fut.done():
+                fut.set_result(m)
         elif kind == "calib":
             cpm = m.get("counts_per_m")
             if isinstance(cpm, int):
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd ai-engine && uv run pytest -q && uv run ruff check . && uv run pyright`
Expected: all pass; `All checks passed!`; `0 errors`.

- [ ] **Step 5: Commit**

```bash
git add ai-engine/src/conqueror_ai/adapters/bridge_ws.py ai-engine/tests/fake_bridge.py ai-engine/tests/test_cam_set.py
git commit -m "feat(ai): BridgeLink.cam_set - the camera to exact angles, awaits the ack

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: `CameraModel` and `Pose`

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/camera.py`
- Modify: `ai-engine/src/conqueror_ai/domain/types.py`
- Test: `ai-engine/tests/test_camera.py`

**Interfaces:**
- Produces:
  - `Pose(t_us, x_m, y_m, th_rad, v_mps, w_rps)`
  - `Calibration(k, dist, size, cam_height_m, cam_x_m, cam_y_m, track_front_m, tilt0_deg, pan_map=(1, 0), tilt_map=(1, 0), readout_us=0, date="")`
  - `CameraModel(cal)` with `.for_frame(pose, pan_deg, tilt_deg)`, `.pixel_to_floor(u, v)`, `.floor_to_pixel(x, y)`, `.floor_distance(u, v)`, `.expected_floor_depth()`, `.floor_mask()`, `.sees(x, y)`, `.distort(xn, yn)`, `.undistort(xd, yd)`, `.axes()`, `.position()`
  - `from_servo(pan_servo_deg, tilt_servo_deg)`

- [ ] **Step 1: Write the failing test** — create `ai-engine/tests/test_camera.py`:

```python
"""CameraModel (AI engine LLD §5.3): pixel <-> floor through the lens, the mount and the pan / tilt servos."""

from __future__ import annotations

import math

import numpy as np

from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.types import Pose


def calib(tilt0: float = 0.0, k1: float = 0.0) -> Calibration:
    return Calibration(
        k=np.array([[200.0, 0.0, 160.0], [0.0, 200.0, 120.0], [0.0, 0.0, 1.0]]),
        dist=np.array([k1, 0.0, 0.0, 0.0, 0.0]),
        size=(320, 240),
        cam_height_m=0.178,
        cam_x_m=0.05,
        cam_y_m=0.0,
        track_front_m=0.10,
        tilt0_deg=tilt0,
    )


def at_start(c: Calibration, pan: float = 0.0, tilt: float = 0.0) -> CameraModel:
    return CameraModel(c).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), pan, tilt)


def test_a_level_camera_never_sees_the_floor_above_the_centre_row() -> None:
    cam = at_start(calib())
    assert cam.pixel_to_floor(160, 120) is None  # the centre ray is level
    assert cam.pixel_to_floor(160, 60) is None


def test_the_floor_under_a_downward_ray_is_where_trigonometry_says() -> None:
    cam = at_start(calib(tilt0=-30.0))  # looking 30 deg down
    p = cam.pixel_to_floor(160, 120)  # the centre ray hits the floor at h / tan 30 ahead of the lens
    assert p is not None
    assert math.isclose(p[0], 0.05 + 0.178 / math.tan(math.radians(30)), abs_tol=1e-6)
    assert math.isclose(p[1], 0.0, abs_tol=1e-9)


def test_pixel_floor_round_trips_with_distortion_pan_and_tilt() -> None:
    cam = at_start(calib(tilt0=-20.0, k1=-0.1), pan=15.0, tilt=-5.0)  # a lens like the OV3660 (barrel)
    for u, v in [(40, 200), (160, 180), (300, 230), (100, 150)]:
        p = cam.pixel_to_floor(u, v)
        assert p is not None
        q = cam.floor_to_pixel(*p)
        assert q is not None
        assert math.isclose(q[0], u, abs_tol=0.05) and math.isclose(q[1], v, abs_tol=0.05)


def test_pan_right_looks_to_the_right() -> None:
    cam = at_start(calib(tilt0=-30.0), pan=20.0)  # pan + = right; the room's y is left
    p = cam.pixel_to_floor(160, 120)
    assert p is not None and p[1] < 0
    assert math.isclose(math.degrees(math.atan2(-p[1], p[0] - 0.05)), 20.0, abs_tol=1e-6)


def test_the_tank_pose_moves_the_floor_point() -> None:
    c = calib(tilt0=-30.0)
    ahead = CameraModel(c).for_frame(Pose(0, 1.0, 2.0, math.pi / 2, 0.0, 0.0), 0.0, 0.0)  # facing +y
    p = ahead.pixel_to_floor(160, 120)
    assert p is not None
    d = 0.05 + 0.178 / math.tan(math.radians(30))
    assert math.isclose(p[0], 1.0, abs_tol=1e-6) and math.isclose(p[1], 2.0 + d, abs_tol=1e-6)


def test_expected_floor_depth_and_the_trusted_mask() -> None:
    cam = at_start(calib(tilt0=-30.0))
    depth = cam.expected_floor_depth()
    assert depth.shape == (240, 320)
    assert math.isinf(depth[0, 160])  # the top row looks above the horizon? (30 deg down, 31 deg half view)
    centre = 0.178 / math.sin(math.radians(30))
    assert math.isclose(float(depth[120, 160]), centre, rel_tol=1e-6)
    mask = cam.floor_mask()
    assert mask[120, 160] and not mask[0, 160]


def test_sees_only_floor_points_in_the_picture_and_near_enough() -> None:
    cam = at_start(calib(tilt0=-30.0))
    assert cam.sees(0.5, 0.0)
    assert not cam.sees(-0.5, 0.0)  # behind
    assert not cam.sees(0.5, 2.0)  # far to the side
    assert not cam.sees(4.0, 0.0)  # beyond 2.5 m


def test_servo_angles_from_telemetry_become_degrees_from_the_centre() -> None:
    from conqueror_ai.domain.camera import from_servo

    assert from_servo(90.0, 90.0) == (0.0, 0.0)
    assert from_servo(75.0, 105.0) == (15.0, -15.0)  # right / up are the smaller servo angles (LOOK, cam_set)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd ai-engine && uv run pytest -q tests/test_camera.py`
Expected: FAIL — `No module named 'conqueror_ai.domain.camera'`.

- [ ] **Step 3: Implement** — `Pose`:

```diff
diff --git a/ai-engine/src/conqueror_ai/domain/types.py b/ai-engine/src/conqueror_ai/domain/types.py
index d191b91..b267114 100644
--- a/ai-engine/src/conqueror_ai/domain/types.py
+++ b/ai-engine/src/conqueror_ai/domain/types.py
@@ -31,3 +31,16 @@ class Frame:
     cap_us: int
     recv_s: float  # laptop monotonic time when the whole picture had arrived
     jpeg: bytes
+
+
+@dataclass(frozen=True)
+class Pose:
+    """The tank in the room (LLD §3): start = 0, 0, 0; x forward at the start, y left, th counter-clockwise
+    (ROS REP 103, as the bridge's arcs)."""
+
+    t_us: int
+    x_m: float
+    y_m: float
+    th_rad: float
+    v_mps: float
+    w_rps: float
```

and create `ai-engine/src/conqueror_ai/domain/camera.py`:

```python
"""The camera model (AI engine LLD §5.3): pixel <-> floor point through the lens (pinhole + distortion),
the mount on the tank and the pan / tilt servos. Pure numpy: no OpenCV, no I/O.

Frames: the room (x forward at the start, y left, z up); the camera looks along its forward axis, image x to
right and image y down (OpenCV). Angles from the centre: pan + = right, tilt + = up (cam_set, Logo's LOOK)."""

from __future__ import annotations

import math
from dataclasses import dataclass, field, replace

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.types import Pose

FloatArray = NDArray[np.float64]
NEAR_M, FAR_M = 0.25, 2.5  # the floor fit is trusted between these (LLD §5.3)


def from_servo(pan_servo_deg: float, tilt_servo_deg: float) -> tuple[float, float]:
    """Telemetry's servo angles (90 = centre) -> degrees from the centre (right / up = +)."""
    return 90.0 - pan_servo_deg, 90.0 - tilt_servo_deg


@dataclass(frozen=True)
class Calibration:
    """What the calibration tool measured (LLD §8); stored in config/calibration.yaml."""

    k: FloatArray  # 3 x 3: fx, fy, cx, cy
    dist: FloatArray  # k1 k2 p1 p2 k3 (OpenCV)
    size: tuple[int, int]  # width, height in pixels (QVGA 320 x 240)
    cam_height_m: float  # the lens above the floor
    cam_x_m: float  # the lens ahead of the tank's centre
    cam_y_m: float  # the lens left of the tank's centre
    track_front_m: (
        float  # the tracks' front ahead of the tank's centre (the blind zone is measured from here)
    )
    tilt0_deg: float  # the camera's real tilt with the servo at its centre (+ = up)
    pan_map: tuple[float, float] = (1.0, 0.0)  # real pan = gain x commanded + offset
    tilt_map: tuple[float, float] = (1.0, 0.0)  # real tilt change = gain x commanded + offset
    readout_us: int = 0
    date: str = ""


@dataclass(frozen=True)
class CameraModel:
    """One camera pose: the tank pose and the commanded pan / tilt (for_frame), over a Calibration."""

    cal: Calibration
    pose: Pose = field(default_factory=lambda: Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0))
    pan_deg: float = 0.0
    tilt_deg: float = 0.0

    def for_frame(self, pose: Pose, pan_deg: float, tilt_deg: float) -> CameraModel:
        return replace(self, pose=pose, pan_deg=pan_deg, tilt_deg=tilt_deg)

    # ---- the camera in the room
    def real_pan_deg(self) -> float:
        g, o = self.cal.pan_map
        return g * self.pan_deg + o

    def real_tilt_deg(self) -> float:
        g, o = self.cal.tilt_map
        return self.cal.tilt0_deg + g * self.tilt_deg + o

    def position(self) -> FloatArray:
        c, s = math.cos(self.pose.th_rad), math.sin(self.pose.th_rad)
        x = self.pose.x_m + c * self.cal.cam_x_m - s * self.cal.cam_y_m
        y = self.pose.y_m + s * self.cal.cam_x_m + c * self.cal.cam_y_m
        return np.array([x, y, self.cal.cam_height_m])

    def axes(self) -> tuple[FloatArray, FloatArray, FloatArray]:
        """The camera's right, down and forward directions in the room."""
        yaw = self.pose.th_rad - math.radians(self.real_pan_deg())  # pan right turns clockwise
        pitch = math.radians(self.real_tilt_deg())
        fwd = np.array([math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch)])
        right = np.array([math.sin(yaw), -math.cos(yaw), 0.0])
        down = np.cross(fwd, right)
        return right, down, fwd

    # ---- the lens
    def distort(self, xn: float, yn: float) -> tuple[float, float]:
        k1, k2, p1, p2, k3 = (float(v) for v in self.cal.dist[:5])
        r2 = xn * xn + yn * yn
        radial = 1 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2
        xd = xn * radial + 2 * p1 * xn * yn + p2 * (r2 + 2 * xn * xn)
        yd = yn * radial + p1 * (r2 + 2 * yn * yn) + 2 * p2 * xn * yn
        return xd, yd

    def undistort(self, xd: float, yd: float) -> tuple[float, float]:
        """Inverts distort as OpenCV's undistortPoints does: divide out the radial factor, less tangential."""
        k1, k2, p1, p2, k3 = (float(v) for v in self.cal.dist[:5])
        xn, yn = xd, yd
        for _ in range(50):
            r2 = xn * xn + yn * yn
            radial = 1 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2
            dx = 2 * p1 * xn * yn + p2 * (r2 + 2 * xn * xn)
            dy = p1 * (r2 + 2 * yn * yn) + 2 * p2 * xn * yn
            xn, yn = (xd - dx) / radial, (yd - dy) / radial
        return xn, yn

    def pixel_ray(self, u: float, v: float) -> FloatArray:
        k = self.cal.k
        xn, yn = self.undistort((u - k[0, 2]) / k[0, 0], (v - k[1, 2]) / k[1, 1])
        right, down, fwd = self.axes()
        return xn * right + yn * down + fwd

    # ---- pixel <-> floor
    def pixel_to_floor(self, u: float, v: float) -> tuple[float, float] | None:
        """The floor point (room x, y) the pixel sees, or None at or above the horizon."""
        ray = self.pixel_ray(u, v)
        if ray[2] >= -1e-9:
            return None
        c = self.position()
        t = -c[2] / ray[2]
        p = c + t * ray
        return float(p[0]), float(p[1])

    def floor_to_pixel(self, x: float, y: float) -> tuple[float, float] | None:
        """Where the floor point (room x, y) is in the picture, or None behind the camera."""
        d = np.array([x, y, 0.0]) - self.position()
        right, down, fwd = self.axes()
        z = float(d @ fwd)
        if z <= 1e-9:
            return None
        xd, yd = self.distort(float(d @ right) / z, float(d @ down) / z)
        k = self.cal.k
        return float(k[0, 0] * xd + k[0, 2]), float(k[1, 1] * yd + k[1, 2])

    def floor_distance(self, u: float, v: float) -> float:
        """Along the pixel's ray to the floor (m); inf at or above the horizon."""
        ray = self.pixel_ray(u, v)
        if ray[2] >= -1e-9:
            return math.inf
        return float(-self.cal.cam_height_m / ray[2] * np.linalg.norm(ray))

    def expected_floor_depth(self) -> FloatArray:
        """For every pixel, the distance along its ray to the floor (inf above the horizon) - all at once."""
        w, h = self.cal.size
        k = self.cal.k
        uu, vv = np.meshgrid(np.arange(w, dtype=np.float64), np.arange(h, dtype=np.float64))
        xd, yd = (uu - k[0, 2]) / k[0, 0], (vv - k[1, 2]) / k[1, 1]
        k1, k2, p1, p2, k3 = (float(v) for v in self.cal.dist[:5])
        xn, yn = xd.copy(), yd.copy()
        for _ in range(50):  # the same iteration as undistort, on whole arrays
            r2 = xn * xn + yn * yn
            radial = 1 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2
            dx = 2 * p1 * xn * yn + p2 * (r2 + 2 * xn * xn)
            dy = p1 * (r2 + 2 * yn * yn) + 2 * p2 * xn * yn
            xn, yn = (xd - dx) / radial, (yd - dy) / radial
        right, down, fwd = self.axes()
        rz = xn * right[2] + yn * down[2] + fwd[2]
        norm = np.sqrt(
            (xn * right[0] + yn * down[0] + fwd[0]) ** 2
            + (xn * right[1] + yn * down[1] + fwd[1]) ** 2
            + rz**2
        )
        with np.errstate(divide="ignore", invalid="ignore"):
            out = np.where(rz < -1e-9, -self.cal.cam_height_m / rz * norm, np.inf)
        return out.astype(np.float64)

    def floor_mask(self) -> NDArray[np.bool_]:
        d = self.expected_floor_depth()
        return (d >= NEAR_M) & (d <= FAR_M)

    def sees(self, x: float, y: float) -> bool:
        """The floor point is inside the picture and within FAR_M of the camera."""
        q = self.floor_to_pixel(x, y)
        if q is None:
            return False
        w, h = self.cal.size
        if not (0 <= q[0] < w and 0 <= q[1] < h):
            return False
        c = self.position()
        return math.dist((x, y, 0.0), (float(c[0]), float(c[1]), float(c[2]))) <= FAR_M
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd ai-engine && uv run pytest -q tests/test_camera.py`
Expected: PASS, 8 tests (in well under a second: `expected_floor_depth` works on whole arrays).

- [ ] **Step 5: Commit**

```bash
git add ai-engine/src/conqueror_ai/domain/camera.py ai-engine/src/conqueror_ai/domain/types.py ai-engine/tests/test_camera.py
git commit -m "feat(ai): CameraModel - pixel <-> floor through the lens, the mount and the pan / tilt servos

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The calibration maths

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/calib_math.py`
- Test: `ai-engine/tests/test_calib_math.py`

**Interfaces:**
- Consumes (Task 3): `Calibration`, `CameraModel`, `Pose`.
- Produces:
  - `laser_row(rgb) -> float | None`, `laser_col(rgb) -> float | None`
  - `horizon_tilt_deg(cal, row)`, `pan_from_col_deg(cal, col)`
  - `fit_linear(commanded, real) -> (gain, offset)`
  - `floor_mark_error_m(cam, u, v, from_tracks_m) -> float | None`
  - `TiltRow(tilt_deg, blind_m, far_m, horizon_in_view)`, `tilt_study(cal, tilts) -> list[TiltRow]`

- [ ] **Step 1: Write the failing test** — create `ai-engine/tests/test_calib_math.py`:

```python
"""The calibration maths (AI engine LLD §8, design-notes §123): the laser horizon, finding lines, servo map,
the floor-mark check and the tilt study - on synthetic pictures and a synthetic camera."""

from __future__ import annotations

import math

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain import calib_math as cm
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Pose
from tests.test_camera import calib


def red_line_picture(row: float | None = None, col: float | None = None) -> NDArray[np.uint8]:
    """A grey 320 x 240 RGB picture with a 3-pixel-wide red laser line (blurred edges) at row or col."""
    img = np.full((240, 320, 3), 90, dtype=np.uint8)
    rng = np.random.default_rng(1)
    img = np.clip(img + rng.normal(0, 8, img.shape), 0, 255).astype(np.uint8)
    if row is not None:
        for r in range(240):
            w = max(0.0, 1.0 - abs(r - row) / 2.0)
            img[r, :, 0] = np.clip(img[r, :, 0] + 160 * w, 0, 255)
    if col is not None:
        for c in range(320):
            w = max(0.0, 1.0 - abs(c - col) / 2.0)
            img[:, c, 0] = np.clip(img[:, c, 0] + 160 * w, 0, 255)
    return img


def test_the_laser_line_is_found_to_a_fraction_of_a_pixel() -> None:
    r = cm.laser_row(red_line_picture(row=87.4))
    assert r is not None and abs(r - 87.4) < 0.3
    c = cm.laser_col(red_line_picture(col=201.6))
    assert c is not None and abs(c - 201.6) < 0.3


def test_no_line_no_answer() -> None:
    assert cm.laser_row(red_line_picture()) is None
    assert cm.laser_col(red_line_picture()) is None


def test_the_horizon_row_gives_the_tilt() -> None:
    c = calib()  # fy 200, cy 120, no distortion
    assert math.isclose(cm.horizon_tilt_deg(c, 120.0), 0.0, abs_tol=1e-9)
    assert math.isclose(cm.horizon_tilt_deg(c, 120.0 - 200.0 * math.tan(math.radians(7))), -7.0, abs_tol=1e-6)


def test_the_horizon_row_matches_the_camera_model() -> None:  # a level line at lens height through the model
    c = calib(tilt0=-9.0, k1=-0.1)
    cam = CameraModel(c).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    right, down, fwd = cam.axes()
    level = np.array([1.0, 0.0, 0.0])  # straight ahead, level
    _xd, yd = cam.distort(float(level @ right) / float(level @ fwd), float(level @ down) / float(level @ fwd))
    row = c.k[1, 1] * yd + c.k[1, 2]
    assert math.isclose(cm.horizon_tilt_deg(c, row), -9.0, abs_tol=1e-6)


def test_a_vertical_line_straight_ahead_gives_the_pan() -> None:
    c = calib()
    col = 160.0 - 200.0 * math.tan(math.radians(12))  # pan 12 deg right: the line moves left
    assert math.isclose(cm.pan_from_col_deg(c, col), 12.0, abs_tol=1e-6)


def test_the_servo_map_is_a_straight_line_fit() -> None:
    gain, offset = cm.fit_linear([-15.0, 0.0, 15.0, 30.0], [-14.0, 0.4, 14.8, 29.2])
    assert math.isclose(gain, 0.968, abs_tol=0.01) and math.isclose(offset, 0.36, abs_tol=0.05)


def test_the_floor_mark_check() -> None:
    c = calib(tilt0=-30.0)
    cam = CameraModel(c).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    q = cam.floor_to_pixel(c.track_front_m + 0.50, 0.0)
    assert q is not None
    err = cm.floor_mark_error_m(cam, q[0], q[1], 0.50)
    assert err is not None and err < 1e-6
    q2 = cam.floor_to_pixel(c.track_front_m + 0.53, 0.0)  # a mark 3 cm further than its label
    assert q2 is not None
    err2 = cm.floor_mark_error_m(cam, q2[0], q2[1], 0.50)
    assert err2 is not None and math.isclose(err2, 0.03, abs_tol=1e-6)


def test_the_tilt_study_shrinks_the_blind_zone_as_the_camera_looks_down() -> None:
    rows = cm.tilt_study(calib(tilt0=-7.0), [0.0, -8.0, -18.0])
    assert [r.tilt_deg for r in rows] == [0.0, -8.0, -18.0]
    assert rows[0].blind_m > rows[1].blind_m > rows[2].blind_m > 0
    assert rows[0].horizon_in_view and rows[1].horizon_in_view  # walls still in view
    for r in rows:
        assert r.far_m > r.blind_m
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd ai-engine && uv run pytest -q tests/test_calib_math.py`
Expected: FAIL — `cannot import name 'calib_math'`.

- [ ] **Step 3: Implement** — create `ai-engine/src/conqueror_ai/domain/calib_math.py`:

```python
"""The calibration tool's maths (AI engine LLD §8, design-notes §123). Pure numpy: pictures in, numbers out.

- The laser level's horizontal line on a wall at exactly the lens height is the horizon: its row gives the
  camera's real tilt. Its vertical line straight ahead gives the real pan (measured with the camera level).
- The servo map: commanded angle -> real angle, a straight-line fit.
- The floor-mark check, and the tilt study (blind zone and view for a few tilts)."""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.types import Pose

MIN_LINE_SCORE = 20.0  # mean red excess on the line's row / column; below it there is no line


def _redness(rgb: NDArray[np.uint8]) -> NDArray[np.float64]:
    """How much red stands out: R - max(G, B), never below 0."""
    f = rgb.astype(np.float64)
    return np.clip(f[:, :, 0] - np.maximum(f[:, :, 1], f[:, :, 2]), 0.0, None)


def _peak(profile: NDArray[np.float64]) -> float | None:
    """The sub-pixel centre of the strongest peak (weighted over +-3), or None when nothing stands out."""
    background = float(np.median(profile))
    p = profile - background
    i = int(np.argmax(p))
    if p[i] < MIN_LINE_SCORE:
        return None
    lo, hi = max(0, i - 3), min(len(p), i + 4)
    w = np.clip(p[lo:hi], 0.0, None)
    return float((np.arange(lo, hi) * w).sum() / w.sum())


def laser_row(rgb: NDArray[np.uint8]) -> float | None:
    """The row of a horizontal red laser line (mean over the middle half of the columns)."""
    w = rgb.shape[1]
    return _peak(_redness(rgb)[:, w // 4 : 3 * w // 4].mean(axis=1))


def laser_col(rgb: NDArray[np.uint8]) -> float | None:
    """The column of a vertical red laser line (mean over the middle half of the rows)."""
    h = rgb.shape[0]
    return _peak(_redness(rgb)[h // 4 : 3 * h // 4, :].mean(axis=0))


def _normalised(cal: Calibration, u: float, v: float) -> tuple[float, float]:
    return CameraModel(cal).undistort((u - cal.k[0, 2]) / cal.k[0, 0], (v - cal.k[1, 2]) / cal.k[1, 1])


def horizon_tilt_deg(cal: Calibration, row: float) -> float:
    """The camera's real tilt (+ = up) from the row where a level line at lens height appears."""
    _, yn = _normalised(cal, float(cal.k[0, 2]), row)
    return math.degrees(math.atan(yn))


def pan_from_col_deg(cal: Calibration, col: float) -> float:
    """The real pan (+ = right) from the column of a vertical line straight ahead, the camera level."""
    xn, _ = _normalised(cal, col, float(cal.k[1, 2]))
    return -math.degrees(math.atan(xn))


def fit_linear(commanded: Sequence[float], real: Sequence[float]) -> tuple[float, float]:
    """Least squares: real = gain x commanded + offset."""
    gain, offset = np.polyfit(np.asarray(commanded, dtype=np.float64), np.asarray(real, dtype=np.float64), 1)
    return float(gain), float(offset)


def floor_mark_error_m(cam: CameraModel, u: float, v: float, from_tracks_m: float) -> float | None:
    """How far the model puts a tape mark (clicked at u, v) from where it is - `from_tracks_m` ahead of
    the tracks' front, the tank at the start pose. None when the click is above the horizon."""
    p = cam.pixel_to_floor(u, v)
    if p is None:
        return None
    return math.dist(p, (cam.cal.track_front_m + from_tracks_m, 0.0))


@dataclass(frozen=True)
class TiltRow:
    tilt_deg: float  # commanded, from the centre (+ = up)
    blind_m: float  # nearest floor point in view, from the tracks' front (straight ahead)
    far_m: float  # farthest floor point in view (inf: the horizon is in the picture)
    horizon_in_view: bool  # walls and the room's far side are in view


def tilt_study(cal: Calibration, tilts: Sequence[float]) -> list[TiltRow]:
    """For each tilt: what the camera sees straight ahead (the bottom and top rows, centre column)."""
    w, h = cal.size
    rows: list[TiltRow] = []
    for t in tilts:
        cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, t)
        near = cam.pixel_to_floor(w / 2, h - 1)
        top = cam.pixel_to_floor(w / 2, 0)
        blind = (near[0] - cal.track_front_m) if near is not None else math.inf
        far = (top[0] - cal.track_front_m) if top is not None else math.inf
        rows.append(TiltRow(t, blind, far, top is None))
    return rows
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd ai-engine && uv run pytest -q tests/test_calib_math.py`
Expected: PASS, 8 tests.

- [ ] **Step 5: Commit**

```bash
git add ai-engine/src/conqueror_ai/domain/calib_math.py ai-engine/tests/test_calib_math.py
git commit -m "feat(ai): the calibration maths - laser horizon, servo fits, floor-mark check, tilt study

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: The calibration file and the lens step (OpenCV, PyYAML)

**Files:**
- Modify: `ai-engine/pyproject.toml`, `ai-engine/uv.lock` (by `uv add`)
- Create: `ai-engine/src/conqueror_ai/adapters/calibration_store.py`, `ai-engine/src/conqueror_ai/adapters/lens_opencv.py`
- Test: `ai-engine/tests/test_calibration_files.py`

**Interfaces:**
- Consumes (Task 3): `Calibration`.
- Produces:
  - `calibration_store.save(cal, path)`, `calibration_store.load(path, size)` (ValueError for another size)
  - `lens_opencv.PATTERN` (9, 6), `checkerboard_png(path, square_px=120)`, `board_points(square_m)`, `find_corners(gray) -> (54, 2) | None`, `calibrate(corner_sets, square_m, size) -> (K, dist, rms)`

- [ ] **Step 1: Add the packages**

Run: `cd ai-engine && uv add "opencv-python>=4.10,<4.12" "pyyaml>=6,<7" && uv add --dev "types-pyyaml>=6"`
Expected: `cv2.__version__` 4.11.x with numpy 2.2.x (`uv run python -c "import cv2, numpy; print(cv2.__version__, numpy.__version__)"`).

- [ ] **Step 2: Write the failing test** — create `ai-engine/tests/test_calibration_files.py`:

```python
"""The calibration file (config/calibration.yaml) and the lens step with OpenCV (AI engine LLD §8)."""

from __future__ import annotations

import math
from pathlib import Path

import cv2
import numpy as np
import pytest
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store, lens_opencv
from tests.test_camera import calib


def test_the_calibration_file_round_trips(tmp_path: Path) -> None:
    c = calib(tilt0=-8.5, k1=-0.12)
    path = tmp_path / "calibration.yaml"
    calibration_store.save(c, path)
    back = calibration_store.load(path, size=(320, 240))
    assert np.allclose(back.k, c.k) and np.allclose(back.dist, c.dist)
    assert back.size == c.size and back.tilt0_deg == c.tilt0_deg and back.cam_height_m == c.cam_height_m
    assert back.track_front_m == c.track_front_m and back.pan_map == c.pan_map


def test_a_file_for_another_picture_size_is_refused(tmp_path: Path) -> None:
    path = tmp_path / "calibration.yaml"
    calibration_store.save(calib(), path)
    with pytest.raises(ValueError, match="320 x 240"):
        calibration_store.load(path, size=(640, 480))


def test_the_checkerboard_picture_and_its_corners_are_found_in_perspective(tmp_path: Path) -> None:
    png = tmp_path / "board.png"
    lens_opencv.checkerboard_png(png, square_px=40)
    board = cv2.imread(str(png), cv2.IMREAD_GRAYSCALE)
    assert board is not None
    h, w = board.shape
    src = np.array([[0, 0], [w, 0], [w, h], [0, h]], dtype=np.float32)
    dst = np.array([[30, 20], [290, 40], [300, 220], [20, 200]], dtype=np.float32)  # seen at a slant, QVGA
    seen: NDArray[np.uint8] = cv2.warpPerspective(  # pyright: ignore[reportAssignmentType]
        board, cv2.getPerspectiveTransform(src, dst), (320, 240), borderValue=(255.0,)
    )
    corners = lens_opencv.find_corners(seen)
    assert corners is not None and corners.shape == (54, 2)


def test_calibration_recovers_a_known_lens() -> None:
    k = np.array([[260.0, 0, 158.0], [0, 260.0, 121.0], [0, 0, 1]])
    dist = np.array([-0.15, 0.05, 0.0, 0.0, 0.0])
    obj = lens_opencv.board_points(0.025)
    sets: list[NDArray[np.float64]] = []
    rng = np.random.default_rng(3)
    for _ in range(15):  # the board held at different angles 30-45 cm away
        rvec = rng.normal(0, 0.35, 3)
        tvec = np.array([rng.normal(-0.1, 0.03), rng.normal(-0.06, 0.03), rng.uniform(0.30, 0.45)])
        img, _ = cv2.projectPoints(obj, rvec, tvec, k, dist)
        sets.append(np.asarray(img, dtype=np.float64).reshape(-1, 2))
    k2, dist2, rms = lens_opencv.calibrate(sets, 0.025, (320, 240))
    assert rms < 0.1
    assert math.isclose(k2[0, 0], 260.0, rel_tol=0.01) and math.isclose(k2[1, 2], 121.0, abs_tol=2.0)
    assert math.isclose(dist2[0], -0.15, abs_tol=0.03)
```

- [ ] **Step 3: Run it to verify it fails**

Run: `cd ai-engine && uv run pytest -q tests/test_calibration_files.py`
Expected: FAIL — `cannot import name 'calibration_store'`.

- [ ] **Step 4: Implement** — create `ai-engine/src/conqueror_ai/adapters/calibration_store.py`:

```python
"""config/calibration.yaml (AI engine LLD §8): what the calibration tool measured. The engine refuses to map
without it, and refuses a file made for another picture size."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np
import yaml

from conqueror_ai.domain.camera import Calibration


def save(cal: Calibration, path: Path) -> None:
    doc: dict[str, Any] = {
        "date": cal.date,
        "size": list(cal.size),
        "lens": {"k": cal.k.tolist(), "dist": cal.dist.tolist()},
        "mount": {
            "cam_height_m": cal.cam_height_m,
            "cam_x_m": cal.cam_x_m,
            "cam_y_m": cal.cam_y_m,
            "track_front_m": cal.track_front_m,
            "tilt0_deg": cal.tilt0_deg,
        },
        "servos": {"pan_map": list(cal.pan_map), "tilt_map": list(cal.tilt_map)},
        "readout_us": cal.readout_us,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(yaml.safe_dump(doc, sort_keys=False))


def load(path: Path, size: tuple[int, int]) -> Calibration:
    """The calibration for pictures of `size`; ValueError when the file is for another size."""
    d: dict[str, Any] = yaml.safe_load(path.read_text())
    got = (int(d["size"][0]), int(d["size"][1]))
    if got != size:
        raise ValueError(f"{path} is for {got[0]} x {got[1]} pictures, not {size[0]} x {size[1]}")
    m, s = d["mount"], d["servos"]
    return Calibration(
        k=np.asarray(d["lens"]["k"], dtype=np.float64),
        dist=np.asarray(d["lens"]["dist"], dtype=np.float64),
        size=got,
        cam_height_m=float(m["cam_height_m"]),
        cam_x_m=float(m["cam_x_m"]),
        cam_y_m=float(m["cam_y_m"]),
        track_front_m=float(m["track_front_m"]),
        tilt0_deg=float(m["tilt0_deg"]),
        pan_map=(float(s["pan_map"][0]), float(s["pan_map"][1])),
        tilt_map=(float(s["tilt_map"][0]), float(s["tilt_map"][1])),
        readout_us=int(d.get("readout_us", 0)),
        date=str(d.get("date", "")),
    )
```

and `ai-engine/src/conqueror_ai/adapters/lens_opencv.py`:

```python
"""The lens step with OpenCV (AI engine LLD §8, design-notes §123): the checkerboard picture for the laptop
screen, its corners in a camera picture, and the lens fit (focal length, centre, distortion)."""

from __future__ import annotations

from collections.abc import Sequence
from pathlib import Path
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

PATTERN = (9, 6)  # inner corners (10 x 7 squares)


def checkerboard_png(path: Path, square_px: int = 120) -> None:
    """A 10 x 7 black-and-white board with a white margin of one square, for showing full-screen."""
    cols, rows = PATTERN[0] + 1, PATTERN[1] + 1
    img = np.full(((rows + 2) * square_px, (cols + 2) * square_px), 255, dtype=np.uint8)
    for r in range(rows):
        for c in range(cols):
            if (r + c) % 2 == 0:
                y, x = (r + 1) * square_px, (c + 1) * square_px
                img[y : y + square_px, x : x + square_px] = 0
    cv2.imwrite(str(path), img)


def board_points(square_m: float) -> NDArray[np.float64]:
    """The inner corners on the board, in metres (z = 0)."""
    pts = np.zeros((PATTERN[0] * PATTERN[1], 3), dtype=np.float64)
    pts[:, :2] = np.mgrid[0 : PATTERN[0], 0 : PATTERN[1]].T.reshape(-1, 2) * square_m
    return pts


def find_corners(gray: NDArray[np.uint8]) -> NDArray[np.float64] | None:
    """The 54 inner corners (sub-pixel), or None when the whole board is not in the picture."""
    found, corners = cv2.findChessboardCorners(gray, PATTERN, flags=cv2.CALIB_CB_ADAPTIVE_THRESH)
    if not found:
        return None
    crit = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 30, 0.01)
    fine = cv2.cornerSubPix(gray, corners, (5, 5), (-1, -1), crit)
    return np.asarray(fine, dtype=np.float64).reshape(-1, 2)


def calibrate(
    corner_sets: Sequence[NDArray[np.float64]], square_m: float, size: tuple[int, int]
) -> tuple[NDArray[np.float64], NDArray[np.float64], float]:
    """K (3 x 3), dist (k1 k2 p1 p2 k3) and the RMS reprojection error in pixels."""
    obj = board_points(square_m).astype(np.float32)
    objs = [obj for _ in corner_sets]
    imgs = [np.asarray(c, dtype=np.float32).reshape(-1, 1, 2) for c in corner_sets]
    k0 = np.zeros((3, 3), dtype=np.float64)
    d0 = np.zeros(5, dtype=np.float64)
    out = cast(
        "tuple[float, NDArray[np.float64], NDArray[np.float64], object, object]",
        cv2.calibrateCamera(objs, imgs, size, k0, d0),  # pyright: ignore[reportCallIssue, reportArgumentType]
    )
    rms, k, dist = out[0], out[1], out[2]
    return np.asarray(k, dtype=np.float64), np.asarray(dist, dtype=np.float64).reshape(-1)[:5], float(rms)
```

- [ ] **Step 5: Run to verify it passes**

Run: `cd ai-engine && uv run pytest -q tests/test_calibration_files.py`
Expected: PASS, 4 tests.

- [ ] **Step 6: Commit**

```bash
git add ai-engine/pyproject.toml ai-engine/uv.lock ai-engine/src/conqueror_ai/adapters/calibration_store.py ai-engine/src/conqueror_ai/adapters/lens_opencv.py ai-engine/tests/test_calibration_files.py
git commit -m "feat(ai): the calibration file and the lens step (OpenCV checkerboard, calibrateCamera)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: `cq-calibrate`

**Files:**
- Create: `ai-engine/src/conqueror_ai/app/calibrate.py`
- Modify: `ai-engine/pyproject.toml` (the script)
- Test: `ai-engine/tests/test_calibrate_tool.py`

**Interfaces:**
- Consumes: Tasks 2-5.
- Produces:
  - `cq-calibrate lens | mount | servos | study [--host] [--out config/calibration.yaml]`
  - the steps `lens`, `mount_numbers`, `mount_tilt`, `floor_marks`, `servo_tilt`, `servo_pan`, `study_table`, with the types `Grab`, `Ask`, `CamSet`, `Click`

- [ ] **Step 1: Write the failing test** — create `ai-engine/tests/test_calibrate_tool.py`:

```python
"""The calibration tool's steps (AI engine LLD §8, design-notes §123), driven by a synthetic tank: pictures
from a known camera, scripted answers and clicks, a recorded cam_set."""

from __future__ import annotations

import math
from collections.abc import Iterator
from pathlib import Path

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import lens_opencv
from conqueror_ai.app import calibrate as tool
from conqueror_ai.domain import calib_math as cm
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Pose
from tests.test_calib_math import red_line_picture
from tests.test_camera import calib

TRUE_TILT0 = -8.0  # the camera's real tilt at the servo centre in the synthetic tank
TILT_GAIN, TILT_OFFSET = 0.97, 0.5  # the real servo: real change = 0.97 x commanded + 0.5
PAN_GAIN = 1.03


class FakeTank:
    """Answers cam_set by remembering the angles; makes the picture the camera would see."""

    def __init__(self, scene: str) -> None:
        self.scene = scene  # "horizon" (laser line at lens height) or "vertical" (line straight ahead)
        self.pan = 0.0
        self.tilt = 0.0
        self.sets: list[tuple[float, float]] = []
        self.cal = calib(tilt0=TRUE_TILT0)

    async def cam_set(self, pan: float, tilt: float) -> None:
        self.pan, self.tilt = pan, tilt
        self.sets.append((pan, tilt))

    async def grab(self) -> NDArray[np.uint8]:
        k = self.cal.k
        if self.scene == "horizon":
            real = TRUE_TILT0 + TILT_GAIN * self.tilt + TILT_OFFSET
            return red_line_picture(row=float(k[1, 2] + k[1, 1] * math.tan(math.radians(real))))
        real_pan = PAN_GAIN * self.pan
        return red_line_picture(col=float(k[0, 2] - k[0, 0] * math.tan(math.radians(real_pan))))


def answers(*lines: str) -> tool.Ask:
    it: Iterator[str] = iter(lines)

    async def ask(_prompt: str) -> str:
        return next(it)

    return ask


async def test_the_mount_step_reads_the_tilt_from_the_laser_horizon() -> None:
    tank = FakeTank("horizon")
    base = calib(tilt0=0.0)  # what the lens step left: no tilt known yet
    cal = await tool.mount_tilt(base, tank.grab, tank.cam_set, answers(""))
    assert tank.sets[-1] == (0.0, 0.0)  # the camera at its centre first
    assert math.isclose(cal.tilt0_deg, TRUE_TILT0 + TILT_OFFSET, abs_tol=0.2)


async def test_the_floor_marks_are_checked_by_clicks() -> None:
    cal = calib(tilt0=-30.0)
    cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
    marks = [cam.floor_to_pixel(cal.track_front_m + d, 0.0) for d in (0.50, 1.00)]
    clicks = iter([m for m in marks if m is not None])

    async def click(_img: NDArray[np.uint8], _what: str) -> tuple[float, float]:
        return next(clicks)

    async def grab() -> NDArray[np.uint8]:
        return np.zeros((240, 320, 3), dtype=np.uint8)

    errors = await tool.floor_marks(cal, grab, click, answers(""))
    assert len(errors) == 2 and all(e < 0.005 for e in errors)


async def test_the_servo_step_fits_the_real_servo() -> None:
    tank = FakeTank("horizon")
    cal = calib(tilt0=TRUE_TILT0 + TILT_OFFSET)  # the mount step's result
    cal = await tool.servo_tilt(cal, tank.grab, tank.cam_set)
    gain, offset = cal.tilt_map
    assert math.isclose(gain, TILT_GAIN, abs_tol=0.02) and math.isclose(offset, 0.0, abs_tol=0.3)
    assert tank.sets[-1] == (0.0, 0.0)  # back to the centre
    tank.scene = "vertical"
    cal = await tool.servo_pan(cal, tank.grab, tank.cam_set, answers(""))
    assert math.isclose(cal.pan_map[0], PAN_GAIN, abs_tol=0.02)


async def test_the_lens_step_calibrates_from_board_pictures(tmp_path: Path) -> None:
    png = tmp_path / "board.png"
    lens_opencv.checkerboard_png(png, square_px=40)
    board = cv2.imread(str(png), cv2.IMREAD_GRAYSCALE)
    assert board is not None
    h, w = board.shape
    src = np.array([[0, 0], [w, 0], [w, h], [0, h]], dtype=np.float32)
    rng = np.random.default_rng(5)
    pics: list[NDArray[np.uint8]] = []
    for _ in range(14):  # the board seen from different angles
        dst = np.array([[30, 20], [290, 30], [295, 220], [25, 210]], dtype=np.float32) + rng.normal(
            0, 12, (4, 2)
        )
        g: NDArray[np.uint8] = cv2.warpPerspective(  # pyright: ignore[reportAssignmentType]
            board, cv2.getPerspectiveTransform(src, dst.astype(np.float32)), (320, 240), borderValue=(255.0,)
        )
        pics.append(np.asarray(cv2.cvtColor(g, cv2.COLOR_GRAY2RGB), dtype=np.uint8))
    it = iter(pics)

    async def grab() -> NDArray[np.uint8]:
        return next(it)

    script = ["25"] + [""] * 14 + ["q"]
    k, dist, rms, n = await tool.lens(grab, answers(*script), tmp_path, show=lambda _p: None)
    # random warps are not one real camera, so only the flow is checked here; the lens accuracy is checked by
    # test_calibration_recovers_a_known_lens (rms < 0.1 px)
    assert n == 14 and math.isfinite(rms) and k[0, 0] > 0 and dist.shape == (5,)


def test_the_tilt_study_table_has_one_line_per_tilt() -> None:
    text = tool.study_table(cm.tilt_study(calib(tilt0=-7.0), [0.0, -10.0, -20.0]))
    assert text.count("\n") >= 3 and "-20" in text and "blind" in text
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd ai-engine && uv run pytest -q tests/test_calibrate_tool.py`
Expected: FAIL — `cannot import name 'calibrate'`.

- [ ] **Step 3: Implement** — in `ai-engine/pyproject.toml` under `[project.scripts]` add `cq-calibrate = "conqueror_ai.app.calibrate:main"`, and create `ai-engine/src/conqueror_ai/app/calibrate.py`:

```python
"""`cq-calibrate` - the calibration tool (AI engine LLD §8, design-notes §123). Steps, in order:

  lens    the checkerboard on the laptop screen, ~20 pictures -> focal length, centre, distortion
  mount   the measured mount, the laser level's horizon line -> the real tilt, then two tape marks to check
  servos  commanded -> real tilt (the horizon line) and pan (the level's vertical line, camera level)
  study   the blind zone and the view at a few tilts, for the owner's driving-tilt decision

Each step saves config/calibration.yaml. The tool's only command to the tank is cam_set (bridge LLD R22): it
never drives. The steps take their inputs as functions, so tests drive them with a synthetic tank."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import datetime
import subprocess
from collections.abc import Awaitable, Callable
from dataclasses import replace
from pathlib import Path

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store, lens_opencv
from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.domain import calib_math as cm
from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.types import Pose

Grab = Callable[[], Awaitable[NDArray[np.uint8]]]  # the newest picture, RGB
Ask = Callable[[str], Awaitable[str]]  # a question to the owner, the typed answer
CamSet = Callable[
    [float, float], Awaitable[None]
]  # pan, tilt from the centre; returns once the camera settled
Click = Callable[[NDArray[np.uint8], str], Awaitable[tuple[float, float]]]  # the owner clicks a point

QVGA = (320, 240)
MIN_LENS_PICTURES = 10
TILT_POINTS = [-15.0, -5.0, 5.0, 15.0, 30.0]
PAN_POINTS = [-30.0, -15.0, 0.0, 15.0, 30.0]
STUDY_TILTS = [0.0, -5.0, -10.0, -15.0, -20.0]
START = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


# ---------------------------------------------------------------- the steps
async def lens(
    grab: Grab, ask: Ask, out_dir: Path, show: Callable[[Path], None]
) -> tuple[NDArray[np.float64], NDArray[np.float64], float, int]:
    """The board on the screen, then pictures until the owner types q: K, dist, RMS (px), pictures used."""
    png = out_dir / "checkerboard.png"
    lens_opencv.checkerboard_png(png)
    show(png)
    square_mm = float(await ask("Measure one square of the board on the screen (mm): "))
    sets: list[NDArray[np.float64]] = []
    while True:
        a = (
            await ask(
                f"[{len(sets)} pictures] Enter = take one (tilt the tank a little each time), q = done: "
            )
        ).strip()
        if a == "q":
            if len(sets) >= MIN_LENS_PICTURES:
                break
            print(f"at least {MIN_LENS_PICTURES} pictures with the whole board, please")
            continue
        gray = np.asarray(cv2.cvtColor(await grab(), cv2.COLOR_RGB2GRAY), dtype=np.uint8)
        corners = lens_opencv.find_corners(gray)
        if corners is None:
            print("  the whole board was not found - is all of it in the picture?")
        else:
            sets.append(corners)
    k, dist, rms = lens_opencv.calibrate(sets, square_mm / 1000.0, QVGA)
    return k, dist, rms, len(sets)


async def mount_numbers(cal: Calibration, ask: Ask) -> Calibration:
    """The mount, measured with a ruler (cm), typed in."""

    async def cm_(q: str, now: float) -> float:
        a = (await ask(f"{q} [{now * 100:.1f} cm]: ")).strip()
        return float(a) / 100.0 if a else now

    return replace(
        cal,
        cam_height_m=await cm_("Lens height above the floor", cal.cam_height_m),
        cam_x_m=await cm_("Lens ahead of the tank's centre", cal.cam_x_m),
        cam_y_m=await cm_("Lens left of the tank's centre", cal.cam_y_m),
        track_front_m=await cm_("Tracks' front ahead of the tank's centre", cal.track_front_m),
    )


async def mount_tilt(cal: Calibration, grab: Grab, cam_set: CamSet, ask: Ask) -> Calibration:
    """The real tilt at the servo's centre, from the laser level's line on a wall at the lens height."""
    await cam_set(0.0, 0.0)
    while True:
        await ask("Laser level: a horizontal line on the wall ahead at exactly the lens height. Enter: ")
        row = cm.laser_row(await grab())
        if row is not None:
            tilt0 = cm.horizon_tilt_deg(cal, row)
            print(
                f"  horizon at row {row:.1f} -> the camera looks {abs(tilt0):.1f} deg "
                f"{'up' if tilt0 > 0 else 'down'}"
            )
            return replace(cal, tilt0_deg=tilt0)
        print("  no red line found - is it in the picture and bright enough?")


async def floor_marks(cal: Calibration, grab: Grab, click: Click, ask: Ask) -> list[float]:
    """Tape marks 50 and 100 cm ahead of the tracks' front: the model's error for each (m)."""
    await ask("Tape marks on the floor 50 and 100 cm straight ahead of the tracks' front. Enter: ")
    img = await grab()
    cam = CameraModel(cal).for_frame(START, 0.0, 0.0)
    errors: list[float] = []
    for d in (0.50, 1.00):
        u, v = await click(img, f"click the {d * 100:.0f} cm mark")
        e = cm.floor_mark_error_m(cam, u, v, d)
        errors.append(e if e is not None else float("inf"))
        print(f"  {d * 100:.0f} cm mark: off by {errors[-1] * 100:.1f} cm")
    return errors


async def servo_tilt(cal: Calibration, grab: Grab, cam_set: CamSet) -> Calibration:
    """Commanded tilt -> real tilt change, from the horizon line at each tilt (laser at lens height)."""
    real: list[float] = []
    for t in TILT_POINTS:
        await cam_set(0.0, t)
        row = cm.laser_row(await grab())
        if row is None:
            raise RuntimeError(f"no horizon line at tilt {t:+.0f} - keep the laser line at lens height")
        real.append(cm.horizon_tilt_deg(cal, row) - cal.tilt0_deg)
    await cam_set(0.0, 0.0)
    return replace(cal, tilt_map=cm.fit_linear(TILT_POINTS, real))


async def servo_pan(cal: Calibration, grab: Grab, cam_set: CamSet, ask: Ask) -> Calibration:
    """Commanded pan -> real pan, from the laser level's vertical line straight ahead, the camera level."""
    g, o = cal.tilt_map
    level = max(-20.0, min(60.0, (-cal.tilt0_deg - o) / g))
    await ask("Laser level: a vertical line straight ahead of the tank, on the wall. Enter: ")
    real: list[float] = []
    for p in PAN_POINTS:
        await cam_set(p, level)
        col = cm.laser_col(await grab())
        if col is None:
            raise RuntimeError(f"no vertical line at pan {p:+.0f} - is it straight ahead and bright enough?")
        real.append(cm.pan_from_col_deg(cal, col))
    await cam_set(0.0, 0.0)
    return replace(cal, pan_map=cm.fit_linear(PAN_POINTS, real))


def study_table(rows: list[cm.TiltRow]) -> str:
    lines = ["tilt   blind (from the tracks)   farthest floor   walls in view"]
    for r in rows:
        far = "-" if r.far_m == float("inf") else f"{r.far_m * 100:.0f} cm"
        lines.append(
            f"{r.tilt_deg:+5.0f}  {r.blind_m * 100:9.1f} cm {far:>23}   "
            f"{'yes' if r.horizon_in_view else 'no'}"
        )
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------- the real tank
def _defaults() -> Calibration:
    """Before the lens step: Q0's measured mount (design-notes §109), a nominal QVGA lens."""
    k = np.array([[260.0, 0.0, 160.0], [0.0, 260.0, 120.0], [0.0, 0.0, 1.0]])
    return Calibration(
        k=k,
        dist=np.zeros(5),
        size=QVGA,
        cam_height_m=0.178,
        cam_x_m=0.05,
        cam_y_m=0.0,
        track_front_m=0.10,
        tilt0_deg=0.0,
    )


async def _ask(prompt: str) -> str:
    return await asyncio.to_thread(input, prompt)


def _show(path: Path) -> None:
    subprocess.run(["open", str(path)], check=False)  # macOS: Preview; the owner makes it full-screen


async def _click(img: NDArray[np.uint8], what: str) -> tuple[float, float]:
    """An OpenCV window; the event loop keeps running (the bridge drops a client silent for 3 s)."""
    got: list[tuple[float, float]] = []

    def on_mouse(event: int, x: int, y: int, _flags: int, _param: object) -> None:
        if event == cv2.EVENT_LBUTTONDOWN:
            got.append((float(x), float(y)))

    name = f"cq-calibrate: {what}"
    cv2.namedWindow(name, cv2.WINDOW_NORMAL)
    cv2.imshow(name, cv2.cvtColor(img, cv2.COLOR_RGB2BGR))
    cv2.setMouseCallback(name, on_mouse)
    while not got:
        cv2.waitKey(30)
        await asyncio.sleep(0)
    cv2.destroyWindow(name)
    cv2.waitKey(1)
    return got[0]


async def _run(step: str, host: str, out: Path) -> None:
    link = BridgeLink(host)
    cam = MjpegSource(host)
    stop = asyncio.Event()
    tasks = [asyncio.create_task(link.run(stop)), asyncio.create_task(cam.run(stop))]

    async def grab() -> NDArray[np.uint8]:
        f = await cam.next_frame()
        f = await cam.next_frame()  # a picture taken after the last move
        bgr = cv2.imdecode(np.frombuffer(f.jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
        return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB).astype(np.uint8)

    async def cam_set(pan: float, tilt: float) -> None:
        while not link.connected:
            await asyncio.sleep(0.1)
        await link.cam_set(pan, tilt)
        await asyncio.sleep(1.0)  # the servos move at 120 deg/s; then a new picture

    try:
        cal = calibration_store.load(out, QVGA) if out.exists() else _defaults()
        if step == "lens":
            k, dist, rms, n = await lens(grab, _ask, out.parent, _show)
            print(f"lens: {n} pictures, error {rms:.2f} px ({'good' if rms < 0.5 else 'high - try again'})")
            cal = replace(cal, k=k, dist=dist)
        elif step == "mount":
            cal = await mount_numbers(cal, _ask)
            cal = await mount_tilt(cal, grab, cam_set, _ask)
            errors = await floor_marks(cal, grab, _click, _ask)
            print("mount: " + ("pass" if all(e <= 0.02 for e in errors) else "a mark is more than 2 cm off"))
        elif step == "servos":
            cal = await servo_tilt(cal, grab, cam_set)
            cal = await servo_pan(cal, grab, cam_set, _ask)
            print(
                f"servos: tilt {cal.tilt_map[0]:.3f} x + {cal.tilt_map[1]:+.2f}, "
                f"pan {cal.pan_map[0]:.3f} x + {cal.pan_map[1]:+.2f}"
            )
        elif step == "study":
            print(study_table(cm.tilt_study(cal, STUDY_TILTS)))
            return
        calibration_store.save(replace(cal, date=datetime.date.today().isoformat()), out)
        print(f"saved {out}")
    finally:
        stop.set()
        with contextlib.suppress(Exception):
            await link.close()
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


def main() -> None:
    ap = argparse.ArgumentParser(description="Calibrate the tank's camera (AI engine LLD §8)")
    ap.add_argument("step", choices=["lens", "mount", "servos", "study"])
    ap.add_argument("--host", default="192.168.31.167")
    ap.add_argument("--out", type=Path, default=Path("config/calibration.yaml"))
    a = ap.parse_args()
    asyncio.run(_run(a.step, a.host, a.out))
```

- [ ] **Step 4: Run to verify it passes**

Run: `cd ai-engine && uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: 54 passed; `All checks passed!`; all files formatted; `0 errors`.

- [ ] **Step 5: Commit**

```bash
git add ai-engine/pyproject.toml ai-engine/src/conqueror_ai/app/calibrate.py ai-engine/tests/test_calibrate_tool.py
git commit -m "feat(ai): cq-calibrate - lens, mount (laser horizon, floor marks), servos, tilt study

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: On the tank (owner present)

**Safety:** nothing drives; only the camera moves (stand or floor, the tank still). Say what moves before each step. Lasers: low power, never at eyes or faces (design-notes §111).

- [ ] **Step 1: Flash the bridge over the air** (as earlier plans: `CQ_OTA_PASSWORD` from `secrets.h` into the environment, never printed) → `SUCCESS`.
- [ ] **Step 2: `cam_set` on the stand** — from the laptop, `BridgeLink.cam_set` to (0, 0), (30, 0), (0, −15), (0, 50): the camera moves there and stays (pan +30 = to the tank's right). `tf` / `td` show the servo angles (90 − deg). (0, −30) → `OUT_OF_RANGE`.
- [ ] **Step 3: `cq-calibrate lens`** — the owner shows the checkerboard full-screen, measures a square and takes ~20 pictures. Expected: RMS < 0.5 px, saved.
- [ ] **Step 4: `cq-calibrate mount`**:
  - the owner types the lens height and position;
  - the laser level makes a horizontal line on a wall at exactly the lens height → the tilt at the servo's centre;
  - tape marks at 50 / 100 cm → each within 2 cm.
- [ ] **Step 5: `cq-calibrate servos`** — the horizon line for the tilt points, then the level's vertical line straight ahead for the pan points → `tilt_map` / `pan_map` (gain near 1).
- [ ] **Step 6: `cq-calibrate study`** — the table of blind zone / farthest floor / walls in view per tilt. **The owner chooses the driving tilt** (the §109 open decision).

---

### Task 8: Docs as built

**Files:**
- `docs/lld/ai-engine-lld.md` (§13 as built: files, the calibration values, the tilt decision);
- `docs/lld/esp32-bridge-lld.md` (R22 as built);
- `docs/design-notes.md` (the calibration results with how each was measured);
- `CLAUDE.md` (plan list, Next).

- [ ] **Step 1: Write them** (the results with how each number was worked out, design-notes rule).
- [ ] **Step 2: Commit**

```bash
git add docs CLAUDE.md ai-engine/config/calibration.yaml
git commit -m "docs: Plan Q2 camera calibration - as built and results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

