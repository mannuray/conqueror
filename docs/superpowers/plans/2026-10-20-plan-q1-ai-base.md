# AI Engine Phase 1 — One Clock, Recording and the Live View Implementation Plan (Plan Q1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The bridge stamps camera pictures and telemetry with one clock, and the new AI engine (`ai-engine/`) watches the tank — telemetry, camera, clock — records sessions, replays them and shows them live in Rerun; read only, nothing moves the tank.

**Architecture:** Bridge revision R17: `BodyLink` keeps the ESP32 timer of each `TELEM_FAST` arrival (`bus_us` in `td` / `tf`), the MJPEG parts carry `X-Frame-Us` (the camera's VSYNC stamp, same timer) and `X-Frame-Seq`, and `pong` carries `bridge_us`. The engine (Python 3.12, uv) has pure `domain/` parts (telemetry parsing, `BridgeClock`, the MJPEG part parser) and `adapters/` (`BridgeLink`, `MjpegSource`, session recorder / replay, `RerunView`) driven by two commands, `cq-record` and `cq-replay`.

**Tech Stack:** C++17 / PlatformIO (`env:native` Unity, `env:bridge` ESP32-S3); Python 3.12 / uv, `websockets` 15+ (asyncio client and server), `rerun-sdk` 0.39, numpy < 2.3, pytest + pytest-asyncio, ruff, pyright strict.

**Spec:** `docs/superpowers/specs/2026-10-20-07-ai-module1-map-design.md` (Spec 7) §3 and §9 phase 1, with `docs/lld/ai-engine-lld.md` §1-§3, §5.1, §6, §9 (both approved).

**Branch:** `plan-q1` from `main`. Task 4 with the owner present; **flash only after Plan N4 is merged and `main` is merged into `plan-q1`** (otherwise the flash removes the `/logo` page from the tank).

## Global Constraints

- **Read only:** the engine sends `hello`, `debug`, `calib` (no value), `ping` — never `drive`, `turn`, `move`, `drive_rate`, `stop`, `logo_*`. The Uno, its protocol and every safety rule are unchanged.
- Bridge coding standards: no heap after start, no `virtual`, pure `core/` (host-tested), size check (function ≤ 40 NLOC, CCN ≤ 10, file ≤ 400 lines) in the pre-commit hook; the bridge's printf has no `%llu`, so 64-bit numbers are printed with `core::u64Str`.
- Python: type hints, ruff + pyright strict clean, `uv` only; asyncio without blocking the event loop.
- Recordings show the home: `ai-engine/recordings/` and `*.rrd` are git-ignored; never copy them off the laptop.
- Never print WiFi SSIDs or passwords or the OTA password; the bridge has no authentication.
- Hardware: ask "stand or floor?" before Task 4 and say what will move (only what the phone drives).
- Commits: Conventional Commits, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Run `pio test` with explicit `-f` lists while iterating; the whole native suite at the end of Task 1.

## Rules this plan decides (owner confirms at plan review)

1. The telemetry stamp is taken when the link task reads a UART chunk (every 2 ms): `onUartBytes(..., now_us)`; a `TELEM_FAST` frame keeps the stamp of the chunk that completed it. Good to ~2 ms (LLD §2).
2. `bus_us` goes into `td` (after `uno_ms`) **and** `tf` (after `tank_ms`); `uno_ms` / `tank_ms` stay.
3. `X-Frame-Seq` counts per viewer connection from 1; `/capture` gets `X-Frame-Us` too.
4. The router's clock for `pong` is a function pointer (`setClockUs`), like `setHello`'s sensor name; unset → `bridge_us` 0 (the engine then ignores the pong).
5. The engine ignores `td` without `bus_us` and stream parts without `X-Frame-Us` (an older bridge): it needs the stamps.
6. **A new WebSocket connection starts a fresh `BridgeClock`** (a restarted bridge counts from zero again).
7. Recording format as LLD §9: `telemetry.jsonl` (raw messages with the laptop receive time), `frames.bin` (JSON header line + JPEG per picture), `session.json`, plus the Rerun `view.rrd`; a session folder is never written over.
8. No config file yet — command-line options; `engine.yaml` comes with the map plans. The engine's WebSocket client name is `ai-map`.
9. Pins: `rerun-sdk` 0.39.x (its logging API changes between versions), numpy < 2.3 (as Plan Q0).

## Review Focus

1. **The phone's live picture** (Drive page, Settings → Tank → Camera) with the extra part headers → still shows (browsers ignore unknown part headers) — Task 4 step 3.
2. **µs since boot beyond 32 bits** (after 71 minutes) → printed and parsed whole — `test_u64_prints_beyond_32_bits`, the `td` test with 4294967296123 (Task 1), the parser tests (Task 2).
3. **The bridge restarting during a recording** → a fresh clock on reconnect, no wrong ages — `test_a_new_connection_starts_a_fresh_clock` (Task 3).
4. **The engine as the second camera viewer** (the stream allows two: phone + engine) → a third viewer is refused by the bridge as today; `cq-record` retries with back-off and says so.
5. **A recording stopped mid-write** (Ctrl-C) → the cut-off last picture is dropped on replay — `test_a_cut_off_picture_at_the_end_is_dropped` (Task 3).

## File map

| Task | Creates | Changes |
|---|---|---|
| 1 Bridge: one clock | `firmware/esp32/src/core/stream_header.{h,cpp}`, `firmware/esp32/test/test_stream_header/test_main.cpp` | `firmware/esp32/src/core/{body_link.h,body_link.cpp,ws_json.h,ws_json_drive.cpp,ws_json_motion.cpp,ws_json_reply.cpp}`, `firmware/esp32/src/app/command_router.{h,cpp}`, `firmware/esp32/src/net/{link_task.cpp,stream_server.cpp}`, tests `test_body_link`, `test_ws_json`, `test_command_router` |
| 2 Engine: project + domain | `ai-engine/{pyproject.toml,uv.lock,.gitignore}`, `ai-engine/src/conqueror_ai/{__init__,domain/__init__,domain/types,domain/telemetry,domain/clock,domain/mjpeg}.py`, `ai-engine/tests/{__init__,test_domain}.py` | — |
| 3 Engine: link, camera, sessions, view, commands | `ai-engine/src/conqueror_ai/{adapters/__init__,adapters/bridge_ws,adapters/mjpeg_source,adapters/session,adapters/rerun_view,app/__init__,app/record,app/replay}.py`, `ai-engine/tests/{fake_bridge,test_record}.py` | — |
| 4 On the tank (owner) | — | — |
| 5 Docs | — | `docs/lld/esp32-bridge-lld.md` (R17), `docs/lld/ai-engine-lld.md` (§12 as built), `docs/design-notes.md`, `docs/dev-setup.md`, `CLAUDE.md` |

---

### Task 1: The bridge — one clock for pictures and telemetry

**Files:** see the file map.

**Interfaces:**
- Produces: `core::u64Str(char*, size_t, uint64_t) -> size_t`; `core::mjpegPartHeader(char*, size_t, const char* boundary, size_t jpeg_len, uint64_t cap_us, uint32_t seq) -> size_t`; `TankSnapshot::fast_bus_us`; `BodyLink::onUartBytes(data, len, now_ms, uint64_t now_us = 0)`; `jsonPong(out, cap, t0, tank_ms, uint64_t bridge_us)`; `CommandRouter::setClockUs(uint64_t (*)())`, `clockUs()`. Wire: `td` / `tf` `"bus_us":<n>`, `pong` `"bridge_us":<n>`, MJPEG part headers `X-Frame-Us: <n>` / `X-Frame-Seq: <n>`, `/capture` header `X-Frame-Us`.

- [ ] **Step 1: The failing tests** — create `firmware/esp32/test/test_stream_header/test_main.cpp`:

```cpp
// The MJPEG part header with the frame's capture time (Plan Q1, AI engine LLD §2).
#include <string.h>
#include <unity.h>

#include "core/stream_header.h"

using cq::core::mjpegPartHeader;
using cq::core::u64Str;

void setUp() {}
void tearDown() {}

void test_u64_prints_beyond_32_bits() {
  char b[24];
  TEST_ASSERT_EQUAL(1, u64Str(b, sizeof(b), 0));
  TEST_ASSERT_EQUAL_STRING("0", b);
  TEST_ASSERT_EQUAL(20, u64Str(b, sizeof(b), 18446744073709551615ULL));
  TEST_ASSERT_EQUAL_STRING("18446744073709551615", b);
  TEST_ASSERT_EQUAL(13, u64Str(b, sizeof(b), 4294967296123ULL));
  TEST_ASSERT_EQUAL_STRING("4294967296123", b);
}

void test_u64_refuses_a_small_buffer() {
  char b[4];
  TEST_ASSERT_EQUAL(0, u64Str(b, sizeof(b), 12345));
  TEST_ASSERT_EQUAL_STRING("", b);
}

void test_part_header_carries_capture_time_and_seq() {
  char b[160];
  const size_t n = mjpegPartHeader(b, sizeof(b), "cqframe", 5120, 123456789012ULL, 7);
  TEST_ASSERT_EQUAL_STRING(
      "\r\n--cqframe\r\nContent-Type: image/jpeg\r\nContent-Length: 5120\r\n"
      "X-Frame-Us: 123456789012\r\nX-Frame-Seq: 7\r\n\r\n",
      b);
  TEST_ASSERT_EQUAL(strlen(b), n);
}

void test_part_header_too_small_buffer_returns_zero() {
  char b[40];
  TEST_ASSERT_EQUAL(0, mjpegPartHeader(b, sizeof(b), "cqframe", 5120, 1, 1));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_u64_prints_beyond_32_bits);
  RUN_TEST(test_u64_refuses_a_small_buffer);
  RUN_TEST(test_part_header_carries_capture_time_and_seq);
  RUN_TEST(test_part_header_too_small_buffer_returns_zero);
  return UNITY_END();
}
```

and apply to the existing tests:

```diff
diff --git a/firmware/esp32/test/test_body_link/test_main.cpp b/firmware/esp32/test/test_body_link/test_main.cpp
index 610d85e..819c173 100644
--- a/firmware/esp32/test/test_body_link/test_main.cpp
+++ b/firmware/esp32/test/test_body_link/test_main.cpp
@@ -298,6 +298,18 @@ void test_telemetry_snapshot_and_events() {
   TEST_ASSERT_EQUAL(static_cast<int>(proto::EventKind::kDeadmanStop), ev.code);
 }
 
+// Plan Q1 (AI engine LLD §2): a TELEM_FAST frame keeps the ESP32 time it arrived at (same clock as the camera's
+// frame stamp); a TELEM_SLOW frame does not change it.
+void test_telem_fast_keeps_its_arrival_time() {
+  uint8_t f[64];
+  size_t n = cqtest::frame(proto::TelemFastMsg{}, 1, f);
+  bl->onUartBytes(f, n, 5, 123456789012ULL);
+  TEST_ASSERT_TRUE(bl->snapshot().fast_bus_us == 123456789012ULL);
+  n = cqtest::frame(proto::TelemSlowMsg{}, 2, f);
+  bl->onUartBytes(f, n, 6, 999ULL);
+  TEST_ASSERT_TRUE(bl->snapshot().fast_bus_us == 123456789012ULL);
+}
+
 // Review I1: link_task follows TELEM_SLOW.owner only when a new slow frame arrived (an IR takeover
 // from EVENT control_changed must not be undone by the 1 Hz frame's older owner on the next tick).
 void test_slow_count_counts_slow_frames() {
@@ -393,6 +405,7 @@ int main() {
   RUN_TEST(test_stop_gives_up_after_ten_sends);
   RUN_TEST(test_cam_home_ack_and_timeout);
   RUN_TEST(test_telemetry_snapshot_and_events);
+  RUN_TEST(test_telem_fast_keeps_its_arrival_time);
   RUN_TEST(test_slow_count_counts_slow_frames);
   RUN_TEST(test_wifi_reset_request_answered_unavailable);
   RUN_TEST(test_sound_and_cam_lock_acked);
diff --git a/firmware/esp32/test/test_command_router/test_main.cpp b/firmware/esp32/test/test_command_router/test_main.cpp
index 0043104..e8a0da1 100644
--- a/firmware/esp32/test/test_command_router/test_main.cpp
+++ b/firmware/esp32/test/test_command_router/test_main.cpp
@@ -90,9 +90,15 @@ void test_unknown_command_is_invalid() {
   TEST_ASSERT_TRUE(has(3, "INVALID_COMMAND"));
 }
 
+uint64_t fakeClockUs() { return 42000000123ULL; }
+
 void test_ping_answers_pong() {
   text(3, "{\"t\":\"ping\",\"t0\":12}", 100);
   TEST_ASSERT_TRUE(has(3, "\"t\":\"pong\""));
+  TEST_ASSERT_TRUE(has(3, "\"bridge_us\":0"));  // no clock set
+  router->setClockUs(fakeClockUs);  // Plan Q1: the ESP32 timer, for the AI engine's clock offset
+  text(3, "{\"t\":\"ping\",\"t0\":13}", 101);
+  TEST_ASSERT_TRUE(has(3, "\"bridge_us\":42000000123"));
 }
 
 void test_hello_answers_welcome_with_the_sensor() {
diff --git a/firmware/esp32/test/test_ws_json/test_main.cpp b/firmware/esp32/test/test_ws_json/test_main.cpp
index 0a26147..4249c5a 100644
--- a/firmware/esp32/test/test_ws_json/test_main.cpp
+++ b/firmware/esp32/test/test_ws_json/test_main.cpp
@@ -41,7 +41,7 @@ void test_tf_scaling_and_sentinels() {
   s.fast.flags = 1u << static_cast<unsigned>(proto::FastFlagBit::kCamMoving);
   jsonTf(buf, sizeof(buf), 7, s);
   TEST_ASSERT_EQUAL_STRING(
-      "{\"t\":\"tf\",\"seq\":7,\"tank_ms\":1234,\"yaw\":90.50,\"yaw_rate\":-12.5,\"pitch\":-1.25,\"roll\":3.00,"
+      "{\"t\":\"tf\",\"seq\":7,\"tank_ms\":1234,\"bus_us\":0,\"yaw\":90.50,\"yaw_rate\":-12.5,\"pitch\":-1.25,\"roll\":3.00,"
       "\"dist_cm\":42,\"pan\":-15.5,\"tilt\":20.0,\"spd_cmd\":[40,-40],\"cam_moving\":true}",
       buf);
   s.fast.dist_mm = 65535;  // clear
@@ -155,8 +155,8 @@ void test_welcome_and_pong() {
   HelloInfo none;
   jsonWelcome(buf, sizeof(buf), WelcomeInfo{"x", &none, "none", kNone, "phone-2"});
   TEST_ASSERT_NOT_NULL(strstr(buf, "\"uno\":null"));
-  jsonPong(buf, sizeof(buf), "1728734400123.5", 99);
-  TEST_ASSERT_EQUAL_STRING("{\"t\":\"pong\",\"t0\":1728734400123.5,\"tank_ms\":99}", buf);
+  jsonPong(buf, sizeof(buf), "1728734400123.5", 99, 5000000000ULL);
+  TEST_ASSERT_EQUAL_STRING("{\"t\":\"pong\",\"t0\":1728734400123.5,\"tank_ms\":99,\"bridge_us\":5000000000}", buf);
 }
 
 void test_too_small_buffer_returns_zero() {
@@ -260,7 +260,9 @@ void test_primitive_done_and_tf_progress() {
   s.fast_valid = true;
   jsonTf(buf, sizeof(buf), 1, s);
   TEST_ASSERT_NULL(strstr(buf, "primitive"));
+  s.fast_bus_us = 7000001ULL;  // Plan Q1
   jsonTf(buf, sizeof(buf), 2, s, PrimitiveView{true, PrimKind::kTurn, 40});
+  TEST_ASSERT_NOT_NULL(strstr(buf, "\"bus_us\":7000001,"));
   TEST_ASSERT_NOT_NULL(strstr(buf, "\"cam_moving\":false,\"primitive\":{\"kind\":\"turn\",\"pct\":40}}"));
 }
 
@@ -289,9 +291,10 @@ void test_td_raw_fast_values() {
   s.fast.flags = 5;
   s.fast.prim = 1;
   s.fast.prim_pct = 40;
+  s.fast_bus_us = 4294967296123ULL;  // Plan Q1: the ESP32 time it arrived (over 32 bits on purpose)
   jsonTd(buf, sizeof(buf), 3, s);
   TEST_ASSERT_EQUAL_STRING(
-      "{\"t\":\"td\",\"seq\":3,\"uno_ms\":1234,\"enc\":[100,-5],\"pwm\":[130,-130],\"cmd\":[25,-25],"
+      "{\"t\":\"td\",\"seq\":3,\"uno_ms\":1234,\"bus_us\":4294967296123,\"enc\":[100,-5],\"pwm\":[130,-130],\"cmd\":[25,-25],"
       "\"act\":[null,12],\"yaw\":90.50,\"rate\":-12.5,\"pitch\":-1.25,\"roll\":3.00,\"dist_mm\":424,"
       "\"closing\":-7,\"pan\":-15.5,\"tilt\":20.0,\"flags\":5,\"prim\":1,\"pct\":40}",
       buf);
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && pio test -e native -f test_stream_header -f test_body_link -f test_ws_json -f test_command_router`
Expected: ERRORED — `'core/stream_header.h' file not found`, `no matching function for call to 'jsonPong'`, `no member named 'fast_bus_us'`, `too many arguments to function call, expected 3, have 4`.

- [ ] **Step 3: The pure parts** — create `firmware/esp32/src/core/stream_header.h`:

```cpp
// The camera stream's part header with the frame's capture time (Plan Q1, AI engine LLD §2).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

// A 64-bit count as decimal text (the bridge's printf has no %llu). Returns its length, or 0 when it does not fit
// (out is then empty).
size_t u64Str(char* out, size_t cap, uint64_t v);

// "\r\n--<boundary>\r\nContent-Type: image/jpeg\r\nContent-Length: <len>\r\nX-Frame-Us: <cap_us>\r\n
// X-Frame-Seq: <seq>\r\n\r\n". cap_us = the frame's capture time on the ESP32 timer (µs since boot, the same clock
// as the telemetry's bus_us). Returns the length, or 0 when it does not fit.
size_t mjpegPartHeader(char* out, size_t cap, const char* boundary, size_t jpeg_len, uint64_t cap_us, uint32_t seq);

}  // namespace core
}  // namespace cq
```

`firmware/esp32/src/core/stream_header.cpp`:

```cpp
#include "core/stream_header.h"

#include <stdio.h>

namespace cq {
namespace core {

size_t u64Str(char* out, size_t cap, uint64_t v) {
  char tmp[21];
  size_t n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  if (cap == 0) return 0;
  if (n + 1 > cap) {
    out[0] = '\0';
    return 0;
  }
  for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  out[n] = '\0';
  return n;
}

size_t mjpegPartHeader(char* out, size_t cap, const char* boundary, size_t jpeg_len, uint64_t cap_us, uint32_t seq) {
  char us[21];
  u64Str(us, sizeof(us), cap_us);
  const int n = snprintf(out, cap,
                         "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Frame-Us: %s\r\n"
                         "X-Frame-Seq: %lu\r\n\r\n",
                         boundary, static_cast<unsigned>(jpeg_len), us, static_cast<unsigned long>(seq));
  if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
  return static_cast<size_t>(n);
}

}  // namespace core
}  // namespace cq
```

and apply:

```diff
diff --git a/firmware/esp32/src/app/command_router.cpp b/firmware/esp32/src/app/command_router.cpp
index 52a5b40..bfb6755 100644
--- a/firmware/esp32/src/app/command_router.cpp
+++ b/firmware/esp32/src/app/command_router.cpp
@@ -21,7 +21,7 @@ const CommandRouter::Route CommandRouter::kRoutes[] = {
     {K::kRelease, [](C& r, int fd, int32_t id, const W&, uint32_t now) { r.m_drive.onRelease(fd, id, now); }},
     {K::kDebug, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onDebug(fd, id, c); }},
     {K::kPing, [](C& r, int fd, int32_t, const W& c, uint32_t now) {
-       r.m_out.to(fd, core::jsonPong(r.m_out.buf(), r.m_out.cap(), c.t0, now));
+       r.m_out.to(fd, core::jsonPong(r.m_out.buf(), r.m_out.cap(), c.t0, now, r.clockUs()));
      }},
     {K::kTurn, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_motion.onTurn(fd, id, c, now); }},
     {K::kDriveRate, [](C& r, int fd, int32_t, const W& c, uint32_t now) { r.m_motion.onDriveRate(fd, c, now); }},
diff --git a/firmware/esp32/src/app/command_router.h b/firmware/esp32/src/app/command_router.h
index fee9116..015c583 100644
--- a/firmware/esp32/src/app/command_router.h
+++ b/firmware/esp32/src/app/command_router.h
@@ -21,6 +21,9 @@ class CommandRouter {
   CommandRouter(DriveController& d, MotionController& m, SettingsController& s, TankAppController& t, LogoController& l,
                 TelemetryPublisher& tel, Replies& out)
       : m_drive(d), m_motion(m), m_settings(s), m_tank(t), m_logo(l), m_tel(tel), m_out(out) {}
+  // Plan Q1: the ESP32 timer (µs) for `pong` (the AI engine's clock offset); unset -> 0.
+  void setClockUs(uint64_t (*now_us)()) { m_clock_us = now_us; }
+  uint64_t clockUs() const { return m_clock_us != nullptr ? m_clock_us() : 0; }
   void setHello(const char* fw, const char* (*sensor)()) {
     m_fw = fw;
     m_sensor = sensor;
@@ -46,6 +49,7 @@ class CommandRouter {
   Replies& m_out;
   const char* m_fw = "";
   const char* (*m_sensor)() = nullptr;
+  uint64_t (*m_clock_us)() = nullptr;
 };
 
 }  // namespace app
diff --git a/firmware/esp32/src/core/body_link.cpp b/firmware/esp32/src/core/body_link.cpp
index 0a545d0..a1d7240 100644
--- a/firmware/esp32/src/core/body_link.cpp
+++ b/firmware/esp32/src/core/body_link.cpp
@@ -75,7 +75,8 @@ void BodyLink::step(uint32_t now_ms) {
   }
 }
 
-void BodyLink::onUartBytes(const uint8_t* data, size_t len, uint32_t now_ms) {
+void BodyLink::onUartBytes(const uint8_t* data, size_t len, uint32_t now_ms, uint64_t now_us) {
+  m_rx_us = now_us;
   m_parser.feed(data, len, [this, now_ms](const FrameView& f) { onFrame(f, now_ms); });
 }
 
@@ -95,6 +96,7 @@ void BodyLink::onTelemFrame(const FrameView& f) {
   if (f.type == static_cast<uint8_t>(proto::FrameType::kTelemFast)) {
     if (!proto::decode(f.payload, f.len, m_snap.fast)) return;
     m_snap.fast_valid = true;
+    m_snap.fast_bus_us = m_rx_us;
     ++m_snap.fast_count;
     return;
   }
diff --git a/firmware/esp32/src/core/body_link.h b/firmware/esp32/src/core/body_link.h
index 0799b17..bd4687f 100644
--- a/firmware/esp32/src/core/body_link.h
+++ b/firmware/esp32/src/core/body_link.h
@@ -47,6 +47,7 @@ struct TankSnapshot {
   bool fast_valid = false;
   bool slow_valid = false;
   uint32_t fast_count = 0;  // TELEM_FAST frames seen (50 Hz; tf is sent for every 5th)
+  uint64_t fast_bus_us = 0;  // Plan Q1: ESP32 timer (µs since boot) when the newest TELEM_FAST arrived
   uint32_t slow_count = 0;  // TELEM_SLOW frames seen (owner is applied once per new frame)
   proto::TelemFastMsg fast{};
   proto::TelemSlowMsg slow{};
@@ -87,7 +88,8 @@ class BodyLink {
   HandshakeState handshake() const { return m_hs; }
 
   void step(uint32_t now_ms);
-  void onUartBytes(const uint8_t* data, size_t len, uint32_t now_ms);
+  // now_us: the ESP32 timer when these bytes were read (Plan Q1: TELEM_FAST keeps it as fast_bus_us).
+  void onUartBytes(const uint8_t* data, size_t len, uint32_t now_ms, uint64_t now_us = 0);
 
   // Client commands (master + synced only; false = not sent).
   bool sendDrive(int8_t v, int8_t w, proto::Source src = proto::Source::kHuman);  // AUTO = motion service
@@ -161,6 +163,7 @@ class BodyLink {
   uint32_t m_last_hb_ms = 0;
   uint32_t m_last_req_ms = 0;
   uint32_t m_last_rx_ms = 0;
+  uint64_t m_rx_us = 0;  // the ESP32 timer of the bytes being parsed (Plan Q1)
   bool m_incompatible_reported = false;
   proto::OwnerHintValue m_hint = proto::OwnerHintValue::kNone;
   uint8_t m_stop_seq = 0;
diff --git a/firmware/esp32/src/core/ws_json.h b/firmware/esp32/src/core/ws_json.h
index ec471ac..16e6a2c 100644
--- a/firmware/esp32/src/core/ws_json.h
+++ b/firmware/esp32/src/core/ws_json.h
@@ -104,7 +104,8 @@ struct TankView {
 const char* unoEventName(uint8_t kind);  // proto::EventKind in lower snake case, "unknown" otherwise
 size_t jsonTank(char* out, size_t cap, const TankView& v);
 size_t jsonLog(char* out, size_t cap, const LogEntry* e, uint8_t n, uint32_t last, bool gap, uint32_t now_ms);
-size_t jsonPong(char* out, size_t cap, const char* t0, uint32_t tank_ms);
+// bridge_us: the ESP32 timer now (Plan Q1: the AI engine's clock offset from the lowest-round-trip pings).
+size_t jsonPong(char* out, size_t cap, const char* t0, uint32_t tank_ms, uint64_t bridge_us);
 
 // Plan N3 (Tank Logo LLD §20.4): the logo_* events, {"t":"ev","seq","tank_ms","kind":"logo_…",…} to every client.
 size_t jsonLogoStarted(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const char* name, const char* by);
diff --git a/firmware/esp32/src/core/ws_json_drive.cpp b/firmware/esp32/src/core/ws_json_drive.cpp
index b17de2f..2f5cc45 100644
--- a/firmware/esp32/src/core/ws_json_drive.cpp
+++ b/firmware/esp32/src/core/ws_json_drive.cpp
@@ -1,4 +1,5 @@
 #include "core/ws_json.h"
+#include "core/stream_header.h"
 #include "core/ws_json_util.h"
 #include "core/motion_router.h"
 
@@ -78,10 +79,13 @@ size_t jsonTf(char* out, size_t cap, uint32_t seq, const TankSnapshot& s, Primit
     snprintf(progress, sizeof(progress), ",\"primitive\":{\"kind\":\"%s\",\"pct\":%u%s}", primName(prim.kind),
              static_cast<unsigned>(prim.pct), prim.limited ? ",\"limited\":true" : "");
   }
+  char us[21];  // Plan Q1: when the frame arrived (ESP32 timer)
+  u64Str(us, sizeof(us), s.fast_bus_us);
   return fmt(out, cap,
-             "{\"t\":\"tf\",\"seq\":%lu,\"tank_ms\":%lu,\"yaw\":%.2f,\"yaw_rate\":%.1f,\"pitch\":%.2f,\"roll\":%.2f,"
+             "{\"t\":\"tf\",\"seq\":%lu,\"tank_ms\":%lu,\"bus_us\":%s,\"yaw\":%.2f,\"yaw_rate\":%.1f,"
+             "\"pitch\":%.2f,\"roll\":%.2f,"
              "\"dist_cm\":%s%s,\"pan\":%.1f,\"tilt\":%.1f,\"spd_cmd\":[%d,%d],\"cam_moving\":%s%s}",
-             static_cast<unsigned long>(seq), static_cast<unsigned long>(f.uno_ms), f.yaw / 100.0, f.yaw_rate / 10.0,
+             static_cast<unsigned long>(seq), static_cast<unsigned long>(f.uno_ms), us, f.yaw / 100.0, f.yaw_rate / 10.0,
              f.pitch / 100.0, f.roll / 100.0, dist, f.dist_mm == 65534 ? ",\"dist_fault\":true" : "", f.pan / 10.0,
              f.tilt / 10.0, f.spd_cmd_l,
              f.spd_cmd_r, cam_moving ? "true" : "false", progress);
diff --git a/firmware/esp32/src/core/ws_json_motion.cpp b/firmware/esp32/src/core/ws_json_motion.cpp
index bb8129f..0fd1432 100644
--- a/firmware/esp32/src/core/ws_json_motion.cpp
+++ b/firmware/esp32/src/core/ws_json_motion.cpp
@@ -1,4 +1,6 @@
 #include "core/ws_json.h"
+
+#include "core/stream_header.h"
 #include "core/ws_json_util.h"
 
 #include <stdio.h>
@@ -16,11 +18,13 @@ size_t jsonTd(char* out, size_t cap, uint32_t seq, const TankSnapshot& s) {
   if (f.spd_act_r != -128) snprintf(act_r, sizeof(act_r), "%d", f.spd_act_r);
   char dist[8] = "null";  // 65535 no echo, 65534 sensor fault
   if (f.dist_mm < 65534) snprintf(dist, sizeof(dist), "%u", static_cast<unsigned>(f.dist_mm));
+  char us[21];  // Plan Q1: when it arrived (ESP32 timer; no %llu in the bridge's printf)
+  u64Str(us, sizeof(us), s.fast_bus_us);
   return fmt(out, cap,
-             "{\"t\":\"td\",\"seq\":%lu,\"uno_ms\":%lu,\"enc\":[%ld,%ld],\"pwm\":[%d,%d],\"cmd\":[%d,%d],"
+             "{\"t\":\"td\",\"seq\":%lu,\"uno_ms\":%lu,\"bus_us\":%s,\"enc\":[%ld,%ld],\"pwm\":[%d,%d],\"cmd\":[%d,%d],"
              "\"act\":[%s,%s],\"yaw\":%.2f,\"rate\":%.1f,\"pitch\":%.2f,\"roll\":%.2f,\"dist_mm\":%s%s,"
              "\"closing\":%d,\"pan\":%.1f,\"tilt\":%.1f,\"flags\":%u,\"prim\":%u,\"pct\":%u}",
-             static_cast<unsigned long>(seq), static_cast<unsigned long>(f.uno_ms), static_cast<long>(f.enc_l),
+             static_cast<unsigned long>(seq), static_cast<unsigned long>(f.uno_ms), us, static_cast<long>(f.enc_l),
              static_cast<long>(f.enc_r), f.pwm_l, f.pwm_r, f.spd_cmd_l, f.spd_cmd_r, act_l, act_r, f.yaw / 100.0,
              f.yaw_rate / 10.0, f.pitch / 100.0, f.roll / 100.0, dist, f.dist_mm == 65534 ? ",\"dist_fault\":true" : "",
              f.closing_cm_s, f.pan / 10.0, f.tilt / 10.0, static_cast<unsigned>(f.flags),
diff --git a/firmware/esp32/src/core/ws_json_reply.cpp b/firmware/esp32/src/core/ws_json_reply.cpp
index 32547b6..daa61f4 100644
--- a/firmware/esp32/src/core/ws_json_reply.cpp
+++ b/firmware/esp32/src/core/ws_json_reply.cpp
@@ -1,4 +1,6 @@
 #include "core/ws_json.h"
+
+#include "core/stream_header.h"
 #include "core/ws_json_util.h"
 
 #include <stdio.h>
@@ -107,9 +109,11 @@ size_t jsonNackMessage(char* out, size_t cap, int32_t id, BridgeReason reason, c
   return nack(out, cap, id, r);
 }
 
-size_t jsonPong(char* out, size_t cap, const char* t0, uint32_t tank_ms) {
-  return fmt(out, cap, "{\"t\":\"pong\",\"t0\":%s,\"tank_ms\":%lu}", t0,
-             static_cast<unsigned long>(tank_ms));
+size_t jsonPong(char* out, size_t cap, const char* t0, uint32_t tank_ms, uint64_t bridge_us) {
+  char us[21];
+  u64Str(us, sizeof(us), bridge_us);
+  return fmt(out, cap, "{\"t\":\"pong\",\"t0\":%s,\"tank_ms\":%lu,\"bridge_us\":%s}", t0,
+             static_cast<unsigned long>(tank_ms), us);
 }
 
 uint16_t ackReasonCode(uint8_t status) {
```

- [ ] **Step 4: Run the tests, then the whole native suite**

Run: `cd firmware/esp32 && pio test -e native -f test_stream_header -f test_body_link -f test_ws_json -f test_command_router`
Expected: all PASSED (`test_stream_header` 4 cases).
Run: `cd firmware/esp32 && pio test -e native`
Expected: `584 test cases: 584 succeeded`.

- [ ] **Step 5: The ESP32 side** — apply:

```diff
diff --git a/firmware/esp32/src/net/link_task.cpp b/firmware/esp32/src/net/link_task.cpp
index a857950..5be4f32 100644
--- a/firmware/esp32/src/net/link_task.cpp
+++ b/firmware/esp32/src/net/link_task.cpp
@@ -3,6 +3,7 @@
 
 #include <Arduino.h>
 #include <esp_system.h>
+#include <esp_timer.h>
 #include <esp_task_wdt.h>
 
 #include <new>
@@ -113,7 +114,9 @@ void publishStatus() {
 void step(uint32_t now) {
   uint8_t rx[256];
   size_t n = 0;
-  while ((n = g_uart.read(rx, sizeof(rx))) != 0) g_link.onUartBytes(rx, n, now);  // snapshot, events, STOP ACKs
+  while ((n = g_uart.read(rx, sizeof(rx))) != 0) {  // snapshot, events, STOP ACKs
+    g_link.onUartBytes(rx, n, now, static_cast<uint64_t>(esp_timer_get_time()));  // Plan Q1: bus_us
+  }
   LinkCommand cmd;
   while (xQueueReceive(g_queue, &cmd, 0) == pdTRUE) handleCommand(cmd, now);
   g_drive.tick(now);  // owner from a new slow frame, the 5 s handback, silent clients
@@ -231,6 +234,7 @@ void linkTaskStart(const char* (*camera_sensor)()) {
   g_logo = new (g_logo_mem) app::LogoController(g_out, g_log, g_link, g_tel, logoRunningProgram(), logoVm());
   g_router = new (g_router_mem) app::CommandRouter(g_drive, g_motion, g_settings, g_tank, *g_logo, g_tel, g_out);
   g_router->setHello(CQ_FW_VERSION, g_sensor);
+  g_router->setClockUs([]() { return static_cast<uint64_t>(esp_timer_get_time()); });  // Plan Q1: pong
   wireLogo();
   wireViews();
   wireEvents();
diff --git a/firmware/esp32/src/net/stream_server.cpp b/firmware/esp32/src/net/stream_server.cpp
index 40b8507..5597d2b 100644
--- a/firmware/esp32/src/net/stream_server.cpp
+++ b/firmware/esp32/src/net/stream_server.cpp
@@ -11,6 +11,7 @@
 #include "freertos/task.h"
 
 #include "board_config.h"
+#include "core/stream_header.h"
 #include "net/camera_service.h"
 #include "net/log.h"
 
@@ -27,6 +28,11 @@ constexpr BaseType_t kViewerCore = 0;
 std::atomic<uint8_t> g_viewers{0};
 QueueHandle_t g_jobs = nullptr;  // async request copies waiting for a free worker
 
+// The frame's capture time in µs: esp32-camera stamps it at VSYNC with esp_timer_get_time().
+uint64_t frameUs(const camera_fb_t* fb) {
+  return static_cast<uint64_t>(fb->timestamp.tv_sec) * 1000000ULL + static_cast<uint64_t>(fb->timestamp.tv_usec);
+}
+
 void releaseViewer() {
   const uint8_t left = static_cast<uint8_t>(--g_viewers);
   logf("stream: viewer left (%u)", static_cast<unsigned>(left));
@@ -38,9 +44,9 @@ void serveStream(httpd_req_t* req) {
   httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=cqframe");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Cache-Control", "no-store");
-  char part[96];
+  char part[160];
   esp_err_t err = ESP_OK;
-  uint32_t last_ms = 0;
+  uint32_t last_ms = 0, seq = 0;
   while (err == ESP_OK) {
     const uint8_t cap = cameraSettings().fps_cap;  // Plan O: frame-rate limit per viewer
     if (cap > 0) {
@@ -50,10 +56,9 @@ void serveStream(httpd_req_t* req) {
     last_ms = millis();
     camera_fb_t* fb = cameraGrab();
     if (fb == nullptr) break;  // camera stopped (OTA) or failed
-    const int len = snprintf(part, sizeof(part),
-                             "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
-                             kBoundary, static_cast<unsigned>(fb->len));
-    err = httpd_resp_send_chunk(req, part, len);
+    // Plan Q1 (AI engine LLD §2): the capture time (VSYNC, ESP32 timer - the clock of the telemetry's bus_us)
+    const size_t len = core::mjpegPartHeader(part, sizeof(part), kBoundary, fb->len, frameUs(fb), ++seq);
+    err = httpd_resp_send_chunk(req, part, static_cast<ssize_t>(len));
     if (err == ESP_OK) {
       err = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(fb->buf),
                                   static_cast<ssize_t>(fb->len));
@@ -121,7 +126,10 @@ esp_err_t captureHandler(httpd_req_t* req) {
   char disposition[64];
   snprintf(disposition, sizeof(disposition), "attachment; filename=\"tank-%lu.jpg\"",
            static_cast<unsigned long>(millis() / 1000UL));
+  char cap_us[21];  // Plan Q1: the capture time, as on the stream's parts
+  core::u64Str(cap_us, sizeof(cap_us), frameUs(fb));
   httpd_resp_set_type(req, "image/jpeg");
+  httpd_resp_set_hdr(req, "X-Frame-Us", cap_us);
   httpd_resp_set_hdr(req, "Content-Disposition", disposition);
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Cache-Control", "no-store");
```

Run: `cd firmware/esp32 && pio run -e bridge && cd ../.. && python3 scripts/size_check.py`
Expected: `[SUCCESS]`, flash ≈ 43.5 %; size check silent. (A `-Wformat-truncation` warning in `ws_json_motion.cpp` on the `moved_cm` line is older code that recompiles here — not from this change.)

- [ ] **Step 6: Commit**

```bash
git add firmware/esp32/src firmware/esp32/test
git commit -m "feat(bridge): one clock for camera frames and telemetry (bus_us, X-Frame-Us, bridge_us)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The engine project and its pure parts

**Files:** see the file map.

**Interfaces:**
- Produces: `TelemSample(bus_us, uno_ms, enc_l, enc_r, yaw_deg, rate_dps, dist_mm|None, pan_deg, tilt_deg, cmd_l, cmd_r, flags)`, `Frame(seq, cap_us, recv_s, jpeg)`; `parse_td(dict) -> TelemSample | None`; `BridgeClock(keep=8, window_s=60)`: `add(t0_s, t1_s, bridge_us)`, `ready()`, `offset_s()`, `to_local(bridge_us)`, `age_s(bridge_us, now_s)`; `MjpegParser.feed(bytes) -> Iterator[Part(seq, cap_us, jpeg)]`.

- [ ] **Step 1: The project** — `ai-engine/pyproject.toml`:

```toml
[project]
name = "conqueror-ai"
version = "0.1.0"
description = "Conqueror AI engine - module 1: sensor fusion and the space map (Spec 7, AI engine LLD)"
requires-python = ">=3.12,<3.13"
dependencies = [
  "numpy>=1.26,<2.3",
  "websockets>=13",
  "rerun-sdk>=0.39,<0.40",  # its logging API changes between versions
]

[project.scripts]
cq-record = "conqueror_ai.app.record:main"
cq-replay = "conqueror_ai.app.replay:main"

[build-system]
requires = ["hatchling"]
build-backend = "hatchling.build"

[tool.hatch.build.targets.wheel]
packages = ["src/conqueror_ai"]

[dependency-groups]
dev = ["ruff>=0.7", "pyright>=1.1", "pytest>=8", "pytest-asyncio>=0.24"]

[tool.pytest.ini_options]
testpaths = ["tests"]
pythonpath = ["."]
asyncio_mode = "auto"

[tool.ruff]
line-length = 110
target-version = "py312"

[tool.ruff.lint]
select = ["E", "F", "I", "B", "UP", "SIM", "RUF"]

[tool.pyright]
pythonVersion = "3.12"
typeCheckingMode = "strict"
include = ["src", "tests"]
reportMissingTypeStubs = false
```

`ai-engine/.gitignore`:

```text
.venv/
recordings/
__pycache__/
*.rrd
```

Create empty `ai-engine/src/conqueror_ai/__init__.py`, `ai-engine/src/conqueror_ai/domain/__init__.py`, `ai-engine/src/conqueror_ai/adapters/__init__.py`, `ai-engine/src/conqueror_ai/app/__init__.py`, `ai-engine/tests/__init__.py`.
Run: `cd ai-engine && uv sync`
Expected: installs; `uv run python -c "import rerun, websockets; print(rerun.__version__)"` → `0.39.x`.

- [ ] **Step 2: The failing tests** — `ai-engine/tests/test_domain.py`:

```python
from __future__ import annotations

from typing import Any

import pytest

from conqueror_ai.domain.clock import BridgeClock
from conqueror_ai.domain.mjpeg import MjpegParser, Part
from conqueror_ai.domain.telemetry import parse_td

TD: dict[str, Any] = {
    "t": "td",
    "seq": 3,
    "uno_ms": 1234,
    "bus_us": 4294967296123,
    "enc": [100, -5],
    "pwm": [130, -130],
    "cmd": [25, -25],
    "act": [None, 12],
    "yaw": 90.5,
    "rate": -12.5,
    "pitch": -1.25,
    "roll": 3.0,
    "dist_mm": 424,
    "closing": -7,
    "pan": -15.5,
    "tilt": 20.0,
    "flags": 5,
    "prim": 1,
    "pct": 40,
}


def test_td_becomes_a_sample() -> None:
    s = parse_td(TD)
    assert s is not None
    assert (s.bus_us, s.uno_ms, s.enc_l, s.enc_r) == (4294967296123, 1234, 100, -5)
    assert (s.yaw_deg, s.rate_dps, s.dist_mm, s.pan_deg, s.tilt_deg) == (90.5, -12.5, 424, -15.5, 20.0)
    assert (s.cmd_l, s.cmd_r, s.flags) == (25, -25, 5)


def test_no_echo_is_none() -> None:
    s = parse_td({**TD, "dist_mm": None, "dist_fault": True})
    assert s is not None and s.dist_mm is None


def test_other_or_broken_messages_are_skipped() -> None:
    assert parse_td({"t": "tf"}) is None
    assert parse_td({k: v for k, v in TD.items() if k != "bus_us"}) is None  # an older bridge: no stamp
    assert parse_td({**TD, "enc": [1]}) is None
    assert parse_td({**TD, "yaw": "x"}) is None


def test_clock_offset_from_the_fastest_pings() -> None:
    c = BridgeClock(keep=2)
    assert not c.ready()
    # bridge = laptop + 100 s; the slow ping (0.5 s) is misleading, the two fast ones are right
    c.add(10.0, 10.010, int((10.005 + 100) * 1e6))
    c.add(11.0, 11.5, int((11.0 + 100) * 1e6))
    c.add(12.0, 12.002, int((12.001 + 100) * 1e6))
    assert c.offset_s() == pytest.approx(100.0, abs=1e-3)
    assert c.to_local(int(105e6)) == pytest.approx(5.0, abs=1e-3)
    assert c.age_s(int(105e6), 5.25) == pytest.approx(0.25, abs=1e-3)


def test_clock_ignores_pongs_without_bridge_time_and_old_samples() -> None:
    c = BridgeClock(window_s=60)
    c.add(1.0, 1.01, 0)
    assert not c.ready()
    c.add(1.0, 1.01, int(50e6))
    c.add(100.0, 100.01, int(250e6))  # 60+ s later: the first sample leaves the window
    assert c.offset_s() == pytest.approx(150.0 - 0.005, abs=1e-3)
    with pytest.raises(ValueError):
        BridgeClock().offset_s()


def part(jpeg: bytes, us: int | None, seq: int) -> bytes:
    stamp = b"" if us is None else b"X-Frame-Us: %d\r\nX-Frame-Seq: %d\r\n" % (us, seq)
    return (
        (b"\r\n--cqframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n" % len(jpeg))
        + stamp
        + b"\r\n"
        + jpeg
    )


def test_mjpeg_parts_with_their_capture_time_across_chunks() -> None:
    a, b = b"\xff\xd8" + b"A" * 50 + b"\xff\xd9", b"\xff\xd8BB\xff\xd9"
    stream = part(a, 123456789012, 1) + part(b, 123456822345, 2)
    p = MjpegParser()
    got: list[Part] = []
    for i in range(0, len(stream), 9):
        got += list(p.feed(stream[i : i + 9]))
    assert got == [Part(1, 123456789012, a), Part(2, 123456822345, b)]


def test_mjpeg_part_without_a_stamp_is_skipped() -> None:
    p = MjpegParser()
    assert list(p.feed(part(b"\xff\xd8X\xff\xd9", None, 0))) == []
    assert list(p.feed(part(b"\xff\xd8Y\xff\xd9", 5, 9))) == [Part(9, 5, b"\xff\xd8Y\xff\xd9")]
```

Run: `cd ai-engine && uv run pytest -q tests/test_domain.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'conqueror_ai.domain.clock'`.

- [ ] **Step 3: The pure parts** — `ai-engine/src/conqueror_ai/domain/types.py`:

```python
"""Data the engine passes around (AI engine LLD §3). Pure: no I/O."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class TelemSample:
    """One `td` message (50 Hz): what the Uno measured, stamped by the bridge on arrival (`bus_us`)."""

    bus_us: int  # ESP32 timer (µs since boot) when the Uno's frame arrived - the camera frames' clock too
    uno_ms: int
    enc_l: int
    enc_r: int
    yaw_deg: float
    rate_dps: float
    dist_mm: int | None  # None: no echo or sensor fault
    pan_deg: float
    tilt_deg: float
    cmd_l: int
    cmd_r: int
    flags: int


@dataclass(frozen=True)
class Frame:
    """One camera picture with its capture time (VSYNC) on the bridge's clock."""

    seq: int
    cap_us: int
    recv_s: float  # laptop monotonic time when the whole picture had arrived
    jpeg: bytes
```

`ai-engine/src/conqueror_ai/domain/telemetry.py`:

```python
"""The bridge's `td` message -> TelemSample (bridge LLD R5 + Plan Q1 `bus_us`). Pure."""

from __future__ import annotations

from typing import Any

from conqueror_ai.domain.types import TelemSample


def _num(m: dict[str, Any], key: str) -> float:
    v = m[key]
    if isinstance(v, bool) or not isinstance(v, int | float):
        raise ValueError(f"{key} is not a number")
    return float(v)


def _pair(m: dict[str, Any], key: str) -> tuple[int, int]:
    v = m[key]
    if not isinstance(v, list) or len(v) != 2:  # pyright: ignore[reportUnknownArgumentType]
        raise ValueError(f"{key} is not a pair")
    a, b = v[0], v[1]  # pyright: ignore[reportUnknownVariableType]
    if not isinstance(a, int) or not isinstance(b, int):
        raise ValueError(f"{key} is not a pair of integers")
    return a, b


def parse_td(m: dict[str, Any]) -> TelemSample | None:
    """A `td` message, or None when it is something else or broken (a bridge without `bus_us` included:
    the engine needs the stamp)."""
    if m.get("t") != "td":
        return None
    try:
        enc_l, enc_r = _pair(m, "enc")
        cmd_l, cmd_r = _pair(m, "cmd")
        dist = m.get("dist_mm")
        return TelemSample(
            bus_us=int(_num(m, "bus_us")),
            uno_ms=int(_num(m, "uno_ms")),
            enc_l=enc_l,
            enc_r=enc_r,
            yaw_deg=_num(m, "yaw"),
            rate_dps=_num(m, "rate"),
            dist_mm=int(dist) if isinstance(dist, int | float) and not isinstance(dist, bool) else None,
            pan_deg=_num(m, "pan"),
            tilt_deg=_num(m, "tilt"),
            cmd_l=cmd_l,
            cmd_r=cmd_r,
            flags=int(_num(m, "flags")),
        )
    except (KeyError, ValueError):
        return None
```

`ai-engine/src/conqueror_ai/domain/clock.py`:

```python
"""The bridge's clock (ESP32 timer, µs) against the laptop's monotonic clock (AI engine LLD §5.1). Pure.

Only for ages and latency: pairing a picture with telemetry uses the bridge's clock on both sides.
"""

from __future__ import annotations

import statistics


class BridgeClock:
    """Each ping: the laptop's send time t0 and receive time t1 (seconds, monotonic) and the pong's
    `bridge_us`. The offset comes from the `keep` lowest-round-trip samples of the last `window_s`."""

    def __init__(self, keep: int = 8, window_s: float = 60.0) -> None:
        self._keep = keep
        self._window = window_s
        self._samples: list[tuple[float, float, float]] = []  # (t1, round trip, offset)

    def add(self, t0_s: float, t1_s: float, bridge_us: int) -> None:
        if t1_s < t0_s or bridge_us <= 0:
            return  # a pong from a bridge without the clock (bridge_us 0) is ignored
        rtt = t1_s - t0_s
        self._samples.append((t1_s, rtt, bridge_us / 1e6 - (t0_s + t1_s) / 2))
        self._samples = [s for s in self._samples if s[0] >= t1_s - self._window]

    def ready(self) -> bool:
        return bool(self._samples)

    def offset_s(self) -> float:
        """bridge seconds - laptop seconds."""
        if not self._samples:
            raise ValueError("no pong with bridge_us yet")
        best = sorted(self._samples, key=lambda s: s[1])[: self._keep]
        return statistics.median(s[2] for s in best)

    def to_local(self, bridge_us: int) -> float:
        """A bridge time as laptop monotonic seconds."""
        return bridge_us / 1e6 - self.offset_s()

    def age_s(self, bridge_us: int, now_s: float) -> float:
        return now_s - self.to_local(bridge_us)
```

`ai-engine/src/conqueror_ai/domain/mjpeg.py`:

```python
"""The bridge's camera stream (multipart MJPEG, Plan Q1 headers) -> pictures with their capture time. Pure."""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass

BOUNDARY = b"--cqframe"


@dataclass(frozen=True)
class Part:
    seq: int
    cap_us: int
    jpeg: bytes


class MjpegParser:
    """Feed bytes as they come; yields every complete part. Parts without `X-Frame-Us` (an older bridge)
    are skipped: the engine cannot place them in time."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> Iterator[Part]:
        self._buf += chunk
        while True:
            start = self._buf.find(BOUNDARY)
            if start < 0:
                del self._buf[: max(0, len(self._buf) - len(BOUNDARY))]  # keep a possible half boundary
                return
            head_end = self._buf.find(b"\r\n\r\n", start)
            if head_end < 0:
                del self._buf[:start]
                return
            headers = _headers(bytes(self._buf[start + len(BOUNDARY) : head_end]))
            length = headers.get("content-length")
            if length is None or not length.isdigit():
                del self._buf[: head_end + 4]  # a broken header: skip it
                continue
            body_start = head_end + 4
            body_end = body_start + int(length)
            if len(self._buf) < body_end:
                del self._buf[:start]
                return
            jpeg = bytes(self._buf[body_start:body_end])
            del self._buf[:body_end]
            us, seq = headers.get("x-frame-us"), headers.get("x-frame-seq", "0")
            if us is not None and us.isdigit() and seq.isdigit():
                yield Part(int(seq), int(us), jpeg)


def _headers(block: bytes) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in block.decode("latin-1").split("\r\n"):
        name, sep, value = line.partition(":")
        if sep:
            out[name.strip().lower()] = value.strip()
    return out
```

- [ ] **Step 4: Run the tests and the checks**

Run: `cd ai-engine && uv run pytest -q tests/test_domain.py && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: `7 passed`, clean, `0 errors`.

- [ ] **Step 5: Commit**

```bash
git add ai-engine
git commit -m "feat(ai): engine project and its pure parts - telemetry, bridge clock, MJPEG parts

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Link, camera, sessions, live view and the two commands

**Files:** see the file map.

**Interfaces:**
- Consumes: Task 2's domain parts; the bridge wire of Task 1.
- Produces: `BridgeLink(host, port=80, client="ai-map")`: `run(stop)`, `new_session()`, `on_message(dict, now)`, `close()`, `.clock`, `.samples: asyncio.Queue[TelemSample]`, `.tap`, `.counts_per_m`; `MjpegSource(host, port=81)`: `run(stop)`, `take(Frame)`, `next_frame()`, `.raw_tap`; `SessionRecorder(folder, about)`: `telemetry(recv_s, raw)`, `frame(Frame)`, `close()`; `read_frames`, `read_telemetry`, `replay(folder) -> Iterator[Event(recv_s, text|frame)]`; `RerunView(spawn, save)`; `record(link, cam, rec, view, seconds) -> Summary`; `counts(folder)`; commands `cq-record`, `cq-replay`; the test stand-in `tests/fake_bridge.FakeBridge`.

- [ ] **Step 1: The stand-in bridge and the failing tests** — `ai-engine/tests/fake_bridge.py`:

```python
"""A stand-in bridge for tests: the WebSocket (`welcome`, `td` at 50 Hz after `debug`,
`pong` with `bridge_us`, `calib`) and the camera stream (parts with `X-Frame-Us` / `X-Frame-Seq`),
on local ports, with the bridge clock running 1000 s ahead of the laptop's monotonic clock."""

from __future__ import annotations

import asyncio
import contextlib
import json
import time
from typing import Any

from websockets.asyncio.server import Server, ServerConnection, serve

OFFSET_S = 1000.0
JPEG = b"\xff\xd8" + b"\x00" * 200 + b"\xff\xd9"


def bridge_us() -> int:
    return int((time.monotonic() + OFFSET_S) * 1e6)


class FakeBridge:
    def __init__(self, frame_age_s: float = 0.1, fps: float = 20.0) -> None:
        self.frame_age_s = frame_age_s
        self.fps = fps
        self.received: list[dict[str, Any]] = []
        self.ws_port = 0
        self.stream_port = 0
        self._ws_server: Server | None = None
        self._stream_server: asyncio.Server | None = None

    async def start(self) -> None:
        self._ws_server = await serve(self._ws, "127.0.0.1", 0)
        self.ws_port = next(iter(self._ws_server.sockets)).getsockname()[1]
        self._stream_server = await asyncio.start_server(self._stream, "127.0.0.1", 0)
        self.stream_port = self._stream_server.sockets[0].getsockname()[1]

    async def stop(self) -> None:
        if self._ws_server is not None:
            self._ws_server.close()
            await self._ws_server.wait_closed()
        if self._stream_server is not None:
            self._stream_server.close()
            with contextlib.suppress(Exception):
                await asyncio.wait_for(self._stream_server.wait_closed(), 1)

    async def _ws(self, ws: ServerConnection) -> None:
        sender: asyncio.Task[None] | None = None
        try:
            async for raw in ws:
                m = json.loads(raw)
                self.received.append(m)
                t = m.get("t")
                if t == "hello":
                    await ws.send(json.dumps({"t": "welcome", "you": "ai-1"}))
                elif t == "debug" and m.get("on") and sender is None:
                    sender = asyncio.create_task(self._td(ws))
                elif t == "ping":
                    await ws.send(
                        json.dumps({"t": "pong", "t0": m["t0"], "tank_ms": 1, "bridge_us": bridge_us()})
                    )
                elif t == "calib":
                    await ws.send(json.dumps({"t": "calib", "counts_per_m": 7514, "roll_mm": 27}))
        finally:
            if sender is not None:
                sender.cancel()

    async def _td(self, ws: ServerConnection) -> None:
        seq = 0
        while True:
            seq += 1
            await ws.send(
                json.dumps(
                    {
                        "t": "td",
                        "seq": seq,
                        "uno_ms": seq * 20,
                        "bus_us": bridge_us(),
                        "enc": [seq, seq],
                        "pwm": [0, 0],
                        "cmd": [0, 0],
                        "act": [0, 0],
                        "yaw": 1.5,
                        "rate": 0.1,
                        "pitch": 0.0,
                        "roll": 0.0,
                        "dist_mm": 500,
                        "closing": 0,
                        "pan": 0.0,
                        "tilt": 0.0,
                        "flags": 0,
                        "prim": 0,
                        "pct": 0,
                    }
                )
            )
            await asyncio.sleep(0.02)

    async def _stream(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        await reader.readuntil(b"\r\n\r\n")
        writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace;boundary=cqframe\r\n\r\n")
        seq = 0
        try:
            while True:
                seq += 1
                cap = bridge_us() - int(self.frame_age_s * 1e6)  # taken frame_age_s ago
                head = (
                    b"\r\n--cqframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n"
                    b"X-Frame-Us: %d\r\nX-Frame-Seq: %d\r\n\r\n" % (len(JPEG), cap, seq)
                )
                writer.write(head + JPEG)
                await writer.drain()
                await asyncio.sleep(1.0 / self.fps)
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            writer.close()
```

`ai-engine/tests/test_record.py`:

```python
from __future__ import annotations

import json
from pathlib import Path

import pytest

from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.adapters.session import SessionRecorder, read_frames, replay
from conqueror_ai.app.record import NoView, Summary, record
from conqueror_ai.app.replay import counts
from conqueror_ai.domain.types import Frame
from tests.fake_bridge import FakeBridge


async def test_record_a_session_from_the_bridge_and_replay_it(tmp_path: Path) -> None:
    bridge = FakeBridge(frame_age_s=0.1, fps=20)
    await bridge.start()
    try:
        link = BridgeLink("127.0.0.1", bridge.ws_port)
        cam = MjpegSource("127.0.0.1", bridge.stream_port)
        rec = SessionRecorder(tmp_path / "s1", {"what": "test"})
        out = await record(link, cam, rec, NoView(), seconds=1.5)
    finally:
        await bridge.stop()
    sent = [m["t"] for m in bridge.received]
    assert sent[:3] == ["hello", "debug", "calib"]
    assert "ping" in sent
    assert not set(sent) & {"drive", "turn", "move", "drive_rate", "logo_run", "stop"}  # read only
    assert link.counts_per_m == 7514
    assert out.pictures >= 15 and out.samples >= 40
    assert out.ages_ms and 50 < sorted(out.ages_ms)[len(out.ages_ms) // 2] < 250  # ~100 ms + local delay
    pictures, samples = counts(tmp_path / "s1")
    assert (pictures, samples) == (rec.frame_count, out.samples)
    times = [e.recv_s for e in replay(tmp_path / "s1")]
    assert times == sorted(times)
    assert "pictures/s" in out.text()


def test_a_session_is_never_written_over(tmp_path: Path) -> None:
    SessionRecorder(tmp_path / "s", {}).close()
    with pytest.raises(FileExistsError):
        SessionRecorder(tmp_path / "s", {})


def test_a_cut_off_picture_at_the_end_is_dropped(tmp_path: Path) -> None:
    rec = SessionRecorder(tmp_path / "s", {})
    rec.frame(Frame(1, 10, 1.0, b"\xff\xd8AA\xff\xd9"))
    rec.frame(Frame(2, 20, 2.0, b"\xff\xd8BBBB\xff\xd9"))
    rec.close()
    data = (tmp_path / "s" / "frames.bin").read_bytes()
    (tmp_path / "s" / "frames.bin").write_bytes(data[:-3])
    assert [f.seq for f in read_frames(tmp_path / "s" / "frames.bin")] == [1]


def test_replay_order_and_telemetry_lines(tmp_path: Path) -> None:
    rec = SessionRecorder(tmp_path / "s", {})
    rec.telemetry(2.0, json.dumps({"t": "pong"}))
    rec.frame(Frame(1, 10, 1.5, b"\xff\xd8\xff\xd9"))
    rec.telemetry(1.0, json.dumps({"t": "welcome"}))
    rec.close()
    events = list(replay(tmp_path / "s"))
    assert [e.recv_s for e in events] == [1.0, 1.5, 2.0]
    assert events[1].frame is not None and events[0].text is not None


def test_summary_without_clock() -> None:
    s = Summary(seconds=2.0, pictures=30, samples=100)
    assert "15.0 pictures/s" in s.text() and "unknown" in s.text()
    assert Summary().text() == "nothing recorded"


def test_the_live_view_logs_pictures_and_telemetry(tmp_path: Path) -> None:
    from conqueror_ai.adapters.rerun_view import RerunView
    from conqueror_ai.domain.types import TelemSample

    view = RerunView(spawn=False, save=tmp_path / "v.rrd")
    view.telemetry(TelemSample(1_000_000, 1, 2, 3, 4.5, 0.1, 500, 0.0, 0.0, 0, 0, 0))
    view.telemetry(TelemSample(1_020_000, 2, 3, 4, 4.6, 0.1, None, 0.0, 0.0, 0, 0, 0))
    view.frame(Frame(1, 1_010_000, 1.0, b"\xff\xd8\xff\xd9"), 0.12)
    import rerun as rr

    rr.disconnect()  # flush the file
    assert (tmp_path / "v.rrd").stat().st_size > 0


def test_a_new_connection_starts_a_fresh_clock() -> None:
    # a restarted bridge counts from zero again: old clock samples would give wrong ages for up to 60 s
    link = BridgeLink("127.0.0.1")
    link.on_message({"t": "pong", "t0": 1000.0, "bridge_us": 5_000_000_000}, 1.001)
    assert link.clock.ready()
    link.new_session()
    assert not link.clock.ready()
```

Run: `cd ai-engine && uv run pytest -q tests/test_record.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'conqueror_ai.adapters.bridge_ws'`.

- [ ] **Step 2: The WebSocket link** — `ai-engine/src/conqueror_ai/adapters/bridge_ws.py`:

```python
"""The bridge's WebSocket, read only (AI engine LLD §6): `hello`, the 50 Hz debug stream, pings for the clock.

It never sends anything that moves the tank. Raw messages go to an optional tap (the recorder).
"""

from __future__ import annotations

import asyncio
import json
import time
from collections.abc import Callable
from typing import Any

from websockets.asyncio.client import ClientConnection, connect

from conqueror_ai.domain.clock import BridgeClock
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import TelemSample

RawTap = Callable[[float, str], None]  # (laptop receive time, raw JSON text)


class BridgeLink:
    def __init__(self, host: str, port: int = 80, client: str = "ai-map") -> None:
        self.url = f"ws://{host}:{port}/ws"
        self.client = client
        self.clock = BridgeClock()
        self.samples: asyncio.Queue[TelemSample] = asyncio.Queue(maxsize=500)
        self.tap: RawTap | None = None
        self.counts_per_m: int | None = None
        self.last_sample_s: float | None = None
        self._ws: ClientConnection | None = None

    async def run(self, stop: asyncio.Event) -> None:
        """Connects (again after a drop, with back-off) until `stop` is set."""
        delay = 0.5
        while not stop.is_set():
            try:
                async with connect(self.url, open_timeout=5, ping_interval=None) as ws:
                    self._ws = ws
                    delay = 0.5
                    await self._session(ws, stop)
            except (OSError, TimeoutError, ConnectionError) as e:
                print(f"bridge link: {type(e).__name__} - trying again in {delay:.1f} s")
            except Exception as e:  # websockets' ConnectionClosed and friends
                print(f"bridge link closed: {type(e).__name__} - trying again in {delay:.1f} s")
            finally:
                self._ws = None
            if not stop.is_set():
                await asyncio.sleep(delay)
                delay = min(delay * 2, 5.0)

    def new_session(self) -> None:
        """A new connection may be a restarted bridge, whose clock starts from zero again."""
        self.clock = BridgeClock()

    async def _session(self, ws: ClientConnection, stop: asyncio.Event) -> None:
        self.new_session()
        await self._send(ws, {"t": "hello", "client": self.client, "proto": 1})
        await self._send(ws, {"t": "debug", "on": True, "hz": 50})
        await self._send(ws, {"t": "calib"})
        pinger = asyncio.create_task(self._pings(ws, stop))
        try:
            async for raw in ws:
                if stop.is_set():
                    break
                self._on_text(raw if isinstance(raw, str) else raw.decode("utf-8", "replace"))
        finally:
            pinger.cancel()

    async def _pings(self, ws: ClientConnection, stop: asyncio.Event) -> None:
        while not stop.is_set():  # also keeps the link alive: the bridge drops a client silent for 3 s
            await self._send(ws, {"t": "ping", "t0": round(time.monotonic() * 1000, 3)})
            await asyncio.sleep(1.0)

    def _on_text(self, raw: str) -> None:
        now = time.monotonic()
        if self.tap is not None:
            self.tap(now, raw)
        try:
            m: Any = json.loads(raw)
        except ValueError:
            return
        if not isinstance(m, dict):
            return
        self.on_message(m, now)  # pyright: ignore[reportUnknownArgumentType]

    def on_message(self, m: dict[str, Any], now: float) -> None:
        kind = m.get("t")
        if kind == "td":
            s = parse_td(m)
            if s is not None:
                self.last_sample_s = now
                if self.samples.full():
                    self.samples.get_nowait()  # the newest matter most
                self.samples.put_nowait(s)
        elif kind == "pong":
            t0, us = m.get("t0"), m.get("bridge_us")
            if isinstance(t0, int | float) and isinstance(us, int):
                self.clock.add(float(t0) / 1000.0, now, us)
        elif kind == "calib":
            cpm = m.get("counts_per_m")
            if isinstance(cpm, int):
                self.counts_per_m = cpm

    async def _send(self, ws: ClientConnection, msg: dict[str, Any]) -> None:
        await ws.send(json.dumps(msg))

    async def close(self) -> None:
        ws = self._ws
        if ws is not None:
            try:
                await self._send(ws, {"t": "debug", "on": False})
            finally:
                await ws.close()
```

- [ ] **Step 3: The camera stream** — `ai-engine/src/conqueror_ai/adapters/mjpeg_source.py`:

```python
"""The bridge's camera stream (`:81/stream`) as Frames, newest first (AI engine LLD §4: latest frame wins).

The stream allows two viewers (the phone + the engine). A picture that arrives while the last one is not yet
taken replaces it; nothing queues.
"""

from __future__ import annotations

import asyncio
import time

from conqueror_ai.domain.mjpeg import MjpegParser
from conqueror_ai.domain.types import Frame


class MjpegSource:
    def __init__(self, host: str, port: int = 81) -> None:
        self.host, self.port = host, port
        self.newest: Frame | None = None
        self.arrived = asyncio.Event()
        self.frames_in = 0
        self.raw_tap: list[Frame] | None = None  # the recorder takes every picture, not only the newest

    async def run(self, stop: asyncio.Event) -> None:
        delay = 0.5
        while not stop.is_set():
            try:
                await self._session(stop)
                delay = 0.5
            except (OSError, TimeoutError, ConnectionError, asyncio.IncompleteReadError) as e:
                print(f"camera stream: {type(e).__name__} - trying again in {delay:.1f} s")
            if not stop.is_set():
                await asyncio.sleep(delay)
                delay = min(delay * 2, 5.0)

    async def _session(self, stop: asyncio.Event) -> None:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(self.host, self.port), 5)
        try:
            writer.write(f"GET /stream HTTP/1.1\r\nHost: {self.host}\r\nConnection: close\r\n\r\n".encode())
            await writer.drain()
            status = await asyncio.wait_for(reader.readline(), 5)
            if b" 200 " not in status:
                raise ConnectionError(f"stream answered {status.decode('latin-1').strip()}")
            parser = MjpegParser()
            while not stop.is_set():
                chunk = await asyncio.wait_for(reader.read(65536), 5)
                if not chunk:
                    return
                now = time.monotonic()
                for part in parser.feed(chunk):
                    self.take(Frame(part.seq, part.cap_us, now, part.jpeg))
        finally:
            writer.close()

    def take(self, f: Frame) -> None:
        self.frames_in += 1
        self.newest = f
        if self.raw_tap is not None:
            self.raw_tap.append(f)
        self.arrived.set()

    async def next_frame(self) -> Frame:
        """Waits for a picture newer than the last one taken."""
        await self.arrived.wait()
        self.arrived.clear()
        f = self.newest
        assert f is not None
        return f
```

- [ ] **Step 4: Sessions** — `ai-engine/src/conqueror_ai/adapters/session.py`:

```python
"""Recording and replaying a session (AI engine LLD §9).

A session folder holds `telemetry.jsonl` (one line per WebSocket message: {"recv_s": t, "msg": "<raw JSON>"}),
`frames.bin` (per picture: a JSON header line {"seq", "cap_us", "recv_s", "len"} then the JPEG bytes) and
`session.json` (what was recorded and from where). Recordings show the home: keep them on the laptop
(`recordings/` is git-ignored).
"""

from __future__ import annotations

import json
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any

from conqueror_ai.domain.types import Frame


class SessionRecorder:
    def __init__(self, folder: Path, about: dict[str, Any]) -> None:
        folder.mkdir(parents=True, exist_ok=False)  # never write over an earlier session
        self.folder = folder
        self._telem: IO[str] = (folder / "telemetry.jsonl").open("w", encoding="utf-8")
        self._frames: IO[bytes] = (folder / "frames.bin").open("wb")
        self.telem_count = 0
        self.frame_count = 0
        (folder / "session.json").write_text(json.dumps({**about, "started": time.time()}, indent=1))

    def telemetry(self, recv_s: float, raw: str) -> None:
        self._telem.write(json.dumps({"recv_s": recv_s, "msg": raw}) + "\n")
        self.telem_count += 1

    def frame(self, f: Frame) -> None:
        head = {"seq": f.seq, "cap_us": f.cap_us, "recv_s": f.recv_s, "len": len(f.jpeg)}
        self._frames.write(json.dumps(head).encode() + b"\n" + f.jpeg)
        self.frame_count += 1

    def close(self) -> None:
        self._telem.close()
        self._frames.close()


@dataclass(frozen=True)
class Event:
    """One recorded input in time order: a WebSocket text or a picture."""

    recv_s: float
    text: str | None = None
    frame: Frame | None = None


def read_frames(path: Path) -> Iterator[Frame]:
    with path.open("rb") as fh:
        while True:
            line = fh.readline()
            if not line:
                return
            h = json.loads(line)
            jpeg = fh.read(int(h["len"]))
            if len(jpeg) != int(h["len"]):
                return  # cut off at the end (recording stopped mid-write)
            yield Frame(int(h["seq"]), int(h["cap_us"]), float(h["recv_s"]), jpeg)


def read_telemetry(path: Path) -> Iterator[tuple[float, str]]:
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            if line.strip():
                row = json.loads(line)
                yield float(row["recv_s"]), str(row["msg"])


def replay(folder: Path) -> Iterator[Event]:
    """Every recorded input in the order it arrived (deterministic: same folder, same order)."""
    texts = [Event(t, text=m) for t, m in read_telemetry(folder / "telemetry.jsonl")]
    frames = [Event(f.recv_s, frame=f) for f in read_frames(folder / "frames.bin")]
    yield from sorted(texts + frames, key=lambda e: (e.recv_s, e.frame is not None))
```

- [ ] **Step 5: The live view** — `ai-engine/src/conqueror_ai/adapters/rerun_view.py`:

```python
"""The live view (AI engine LLD §4 LiveView): the camera picture and the telemetry in the Rerun viewer, on the
bridge's clock. Also writes a `.rrd` when given a path (open it later with `rerun file.rrd`)."""

from __future__ import annotations

from pathlib import Path

import rerun as rr

from conqueror_ai.domain.types import Frame, TelemSample


class RerunView:
    def __init__(self, spawn: bool = True, save: Path | None = None) -> None:
        rr.init("conqueror-ai", spawn=spawn)
        if save is not None:
            rr.save(str(save))

    def telemetry(self, s: TelemSample) -> None:
        rr.set_time("bridge", duration=s.bus_us / 1e6)
        rr.log("tank/heading_deg", rr.Scalars(s.yaw_deg))
        rr.log("tank/turn_rate_dps", rr.Scalars(s.rate_dps))
        rr.log("tank/encoders", rr.Scalars([float(s.enc_l), float(s.enc_r)]))
        if s.dist_mm is not None:
            rr.log("tank/ultrasonic_cm", rr.Scalars(s.dist_mm / 10.0))

    def frame(self, f: Frame, age_s: float | None) -> None:
        rr.set_time("bridge", duration=f.cap_us / 1e6)
        rr.log("camera/image", rr.EncodedImage(contents=f.jpeg, media_type="image/jpeg"))
        if age_s is not None:
            rr.log("camera/age_on_arrival_ms", rr.Scalars(age_s * 1000.0))
```

- [ ] **Step 6: The commands** — `ai-engine/src/conqueror_ai/app/record.py`:

```python
"""`cq-record`: watch the tank - camera and telemetry - record the session and show it live (Spec 7 phase 1).

Read only: it never moves the tank (drive the tank with the phone meanwhile). Ctrl-C ends it and prints a
summary: pictures and telemetry per second, and the pictures' age on arrival.

    uv run cq-record --host 192.168.31.167 [--seconds 60] [--no-view] [--out recordings/<now>]
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import statistics
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol

from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.domain.types import Frame, TelemSample

DEFAULT_HOST = "192.168.31.167"  # the tank on the home network (mDNS does not resolve from Python here)


class View(Protocol):
    """What the recorder shows: RerunView in real use, NoView in tests and with --no-view."""

    def telemetry(self, s: TelemSample) -> None: ...
    def frame(self, f: Frame, age_s: float | None) -> None: ...


class NoView:
    def telemetry(self, s: TelemSample) -> None:
        pass

    def frame(self, f: Frame, age_s: float | None) -> None:
        pass


@dataclass
class Summary:
    seconds: float = 0.0
    pictures: int = 0
    samples: int = 0
    ages_ms: list[float] = field(default_factory=list[float])

    def text(self) -> str:
        if self.seconds <= 0:
            return "nothing recorded"
        age = (
            f"age on arrival median {statistics.median(self.ages_ms):.0f} ms "
            f"(min {min(self.ages_ms):.0f}, max {max(self.ages_ms):.0f})"
            if self.ages_ms
            else "age on arrival unknown (no pong with bridge_us)"
        )
        return (
            f"{self.seconds:.1f} s: {self.pictures / self.seconds:.1f} pictures/s, "
            f"{self.samples / self.seconds:.1f} telemetry/s; {age}"
        )


async def record(
    link: BridgeLink, cam: MjpegSource, rec: SessionRecorder, view: View, seconds: float | None
) -> Summary:
    stop = asyncio.Event()
    link.tap = rec.telemetry
    cam.raw_tap = []
    tasks = [asyncio.create_task(link.run(stop)), asyncio.create_task(cam.run(stop))]
    out = Summary()
    t0 = time.monotonic()
    try:
        while seconds is None or time.monotonic() - t0 < seconds:
            await asyncio.sleep(0.05)
            for f in cam.raw_tap:
                rec.frame(f)
                age = link.clock.age_s(f.cap_us, f.recv_s) if link.clock.ready() else None
                if age is not None:
                    out.ages_ms.append(age * 1000.0)
                view.frame(f, age)
                out.pictures += 1
            cam.raw_tap.clear()
            while not link.samples.empty():
                view.telemetry(link.samples.get_nowait())
                out.samples += 1
    finally:
        stop.set()
        with contextlib.suppress(Exception):
            await link.close()
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        rec.close()
        out.seconds = time.monotonic() - t0
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--ws-port", type=int, default=80)
    ap.add_argument("--stream-port", type=int, default=81)
    ap.add_argument("--seconds", type=float, default=None)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--no-view", action="store_true")
    a = ap.parse_args()
    folder = a.out or Path("recordings") / time.strftime("%Y%m%d-%H%M%S")
    rec = SessionRecorder(folder, {"host": a.host, "what": "cq-record (Spec 7 phase 1)"})
    view: View = NoView()
    if not a.no_view:
        from conqueror_ai.adapters.rerun_view import RerunView

        view = RerunView(spawn=True, save=folder / "view.rrd")
    link = BridgeLink(a.host, a.ws_port)
    cam = MjpegSource(a.host, a.stream_port)
    print(f"recording to {folder} - Ctrl-C ends it (read only: drive with the phone)")
    try:
        summary = asyncio.run(record(link, cam, rec, view, a.seconds))
    except KeyboardInterrupt:
        summary = Summary()
        print("stopped")
    print(summary.text())
    print(f"{rec.frame_count} pictures and {rec.telem_count} telemetry messages in {folder}")


if __name__ == "__main__":
    main()
```

`ai-engine/src/conqueror_ai/app/replay.py`:

```python
"""`cq-replay`: show a recorded session in the Rerun viewer, as fast as it can (Spec 7 phase 1; later the map
pipeline replays through the same reader).

    uv run cq-replay recordings/20261021-101500
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from conqueror_ai.adapters.session import replay
from conqueror_ai.domain.telemetry import parse_td


def counts(folder: Path) -> tuple[int, int]:
    """(pictures, td samples) of a session - also a quick check that it reads back."""
    pictures = samples = 0
    for e in replay(folder):
        if e.frame is not None:
            pictures += 1
        elif e.text is not None and parse_td(json.loads(e.text)) is not None:
            samples += 1
    return pictures, samples


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path)
    a = ap.parse_args()
    from conqueror_ai.adapters.rerun_view import RerunView

    view = RerunView(spawn=True)
    for e in replay(a.folder):
        if e.frame is not None:
            view.frame(e.frame, None)
        elif e.text is not None:
            s = parse_td(json.loads(e.text))
            if s is not None:
                view.telemetry(s)
    pictures, samples = counts(a.folder)
    print(f"{pictures} pictures, {samples} telemetry samples replayed from {a.folder}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 7: Run all tests (four times: the end-to-end test uses real sockets) and the checks**

Run: `cd ai-engine && for i in 1 2 3 4; do uv run pytest -q | tail -1; done && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: `14 passed` four times; clean; `0 errors`.

- [ ] **Step 8: `cq-record` against the stand-in bridge**

```bash
cd ai-engine && uv run python - <<'EOF'
import asyncio, os, shutil, tempfile
from tests.fake_bridge import FakeBridge
async def main():
    b = FakeBridge(); await b.start()
    out = tempfile.mkdtemp() + "/rec"
    p = await asyncio.create_subprocess_exec("uv", "run", "cq-record", "--host", "127.0.0.1", "--ws-port", str(b.ws_port),
        "--stream-port", str(b.stream_port), "--seconds", "2", "--no-view", "--out", out,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    print((await p.communicate())[0].decode())
    await b.stop(); shutil.rmtree(os.path.dirname(out))
asyncio.run(main())
EOF
```

Expected: `2.0 s: ~20 pictures/s, ~47 telemetry/s; age on arrival median 100 ms (min 100, max 101)` and `~40 pictures and ~100 telemetry messages in …`.

- [ ] **Step 9: Commit**

```bash
git add ai-engine
git commit -m "feat(ai): link, camera, record and replay, live view - cq-record and cq-replay

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: On the tank (owner present)

**Files:** none (results recorded in Task 5).

**Before:** Plan N4 merged into `main` (owner approval), then `git merge main` into `plan-q1`, the native suite and the bridge build again (Task 1 steps 4–5).

Ask **"stand or floor?"** — the engine never drives; only what the phone drives moves. Say so.

- [ ] **Step 1: Flash the bridge** (OTA, no partition change): read the OTA password into `CQ_OTA_PASSWORD` without printing it, `export` it, `cd firmware/esp32 && pio run -e bridge_ota -t upload 2>&1 | grep -vF -- "$CQ_OTA_PASSWORD" > /tmp/ota.log; tail -3 /tmp/ota.log; rm /tmp/ota.log`.
Expected: `[SUCCESS]`; the bridge back on WiFi.

- [ ] **Step 2: The stamps on the wire**
Run: `curl -s -D - -o /dev/null http://192.168.31.167:81/capture | grep -i x-frame-us`
Expected: `X-Frame-Us: <a number>`. Then `cd tools/smoke && uv run python td_record.py --host 192.168.31.167 --hz 50 --seconds 2 --out /tmp/td.csv` still works (it ignores the new field); delete `/tmp/td.csv`.

- [ ] **Step 3: The phone still sees the camera** — Drive page and Settings → Tank → Camera show the live picture (with the extra part headers).

- [ ] **Step 4: Record with the live view** — the phone on the Drive page (one viewer), then on the laptop: `cd ai-engine && uv run cq-record --host 192.168.31.167 --seconds 60`. Meanwhile drive slowly with the phone (stand: tracks turn; floor: a slow loop).
Expected: Rerun opens with the camera picture, heading, turn rate, encoders and the ultrasonic on one time line; the summary prints pictures / s (as Plan Q0's `latency.py`), telemetry ≈ 50 / s, and the age on arrival (compare with Q0's flash method).

- [ ] **Step 5: Replay** — `uv run cq-replay recordings/<the folder>`.
Expected: the same pictures and plots; the counts printed.

- [ ] **Step 6: A bridge restart during a recording** — start `cq-record`, restart the bridge (Settings → Tank → Maintenance → restart), wait until it is back.
Expected: "bridge link closed … trying again", then the recording goes on; the ages after the restart are sensible (fresh clock).

---

### Task 5: Docs

**Files:** see the file map.

- [ ] **Step 1: Bridge LLD R17** — append to `docs/lld/esp32-bridge-lld.md` a section "R17 — one clock for camera frames and telemetry (Plan Q1)": the rules 1–4 of this plan, the wire (`td` / `tf` `bus_us`, `pong` `bridge_us`, `X-Frame-Us` / `X-Frame-Seq`, `/capture` `X-Frame-Us`), `core/stream_header`, the tests, the Task 4 results.
- [ ] **Step 2: AI engine LLD §12 "As built (Plan Q1)"** — the files, rules 5–9, the stand-in bridge for tests, the recording format as built, the Task 4 numbers.
- [ ] **Step 3: design-notes** — a section "Plan Q1 — one clock, recording and the live view (<date>)" (the next free number): what was built, the owner's tank results, the picture age by the engine's clock against Q0's flash method.
- [ ] **Step 4: dev-setup** — `docs/dev-setup.md`: the AI engine (`cd ai-engine && uv sync`; `uv run cq-record --host …`; Rerun opens a viewer window; recordings stay on the laptop).
- [ ] **Step 5: CLAUDE.md** — Plan Q1 in the plan list and the plans row; `ai-engine/` in "Where things are".
- [ ] **Step 6: Commit**

```bash
git add docs/lld/esp32-bridge-lld.md docs/lld/ai-engine-lld.md docs/design-notes.md docs/dev-setup.md CLAUDE.md
git commit -m "docs: Plan Q1 as built - bridge R17, AI engine LLD §12, design-notes, dev-setup

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

