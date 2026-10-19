# Plan O — The tank app Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the designed tank app except Logo and AI: the ☰ menu on Drive, `/settings` in four tabs (WiFi · Tank · Tune · Log) with live sensors, self-test, camera settings, child-play card, live graphs, the calibration wizard and the event log — plus the Uno Lite and bridge pieces behind them.

**Architecture:** The Uno Lite adds board temperature, integer pitch/roll and a re-runnable self-test (Uno Lite LLD §21). The bridge adds a pure `EventLog` ring, `SelfTestRecord`, run-time `CameraSettings` (NVS `cq_cam`) and `AppSettings` (NVS `cq_app`), five WebSocket commands and log sources in the link task (bridge LLD R13). The pages stay plain HTML/JS on the bridge; graphs and the wizard use the existing `debug` stream (`td`) and `move` / `calib` commands.

**Tech Stack:** C++17 (Uno Lite AVR + host, ESP32-S3 Arduino + host), Unity, PlatformIO, HTML/CSS/JS (no framework), Playwright page checks, `tools/smoke` (uv).

**Spec:** `docs/lld/esp32-bridge-lld.md` Revision 13; `docs/lld/uno-lite-lld.md` §21; UX `docs/design/bridge-ux.md` (screens 17–20); design-notes §80, §85, §89–§91.

## Global Constraints

- Safety unchanged: no new motion paths except the wizard's existing `move`; STOP rules (§72), deadman, link-loss, obstacle brake untouched; Settings shows a STOP only in the wizard step (§85).
- Uno Lite flash: today 31 044 / 32 256 B; Task 1 may add **≤ 400 B** (measured, recorded); no heap, no `virtual`, no floats in the Lite's new code, no recursion.
- Bridge: no heap after start-up in `src/core`; `wsSend` only from the link task; camera setters only on the camera / stream side through a queue; NVS namespaces `cq_cam`, `cq_app` (new).
- **Protocol unchanged** (version 3): TELEM_FAST `pitch`/`roll` (0.01°), TELEM_SLOW `temp` (0.01 °C), CALIBRATE `op`/`script` `SELF_TEST` (9) already exist; flash Uno and bridge together anyway.
- `calib` set needs the tank's button (owner, §91) — also from `tools/smoke/move.py`.
- AI address: `http://` only, ≤ 96 chars, stored on the bridge.
- Camera choices: size QVGA / VGA / SVGA / HD, quality 10–40, fps cap 0 (max) / 15 / 10 / 5, brightness and contrast −2…2, low-light, vflip, hmirror; defaults = VGA, 12, max, 0, 0, off, vflip on (OV3660), hmirror off.
- `/settings` budget 36 KB gzipped; `/` (drive) stays ≤ 24 KB.
- Commits: Conventional Commits + `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch **`plan-o`**; ask before merging.
- Hardware: owner present; ask "stand or floor?" and say what moves before every drive (wizard on the floor, tracks on the stand for the rest).

## Review Focus

1. **The event log wraps** (more than 200 events, a page that last asked for seq 10 when the oldest kept is 150) → the page gets the oldest kept entries and a gap marker, never stale or repeated rows — test in Task 2 (`test_log_since_after_wrap_returns_oldest_kept`).
2. **A camera setting arrives while two viewers stream** (size change mid-stream, HD with the buffers sized at start) → no crash, no corrupt frames; the next frames use the new size — tank check in Task 7 + the "init at the largest size" rule in Task 4.
3. **`selftest` while the tank drives or the body is offline** → `nack BUSY` / `BODY_OFFLINE`, nothing sent to the Uno — test in Task 5 (`handleSelfTest` paths, host part in `test_ws_codec` + glue check on the tank).
4. **A `calib` set from a client that is not the driver, or while a move runs, or while another confirmation waits** → refused as today (`NOT_OWNER` / `BUSY`) before the confirmation starts — test in Task 3 (`test_confirm_calib_set_kind`) + Task 5 glue.
5. **An AI address with quotes, spaces, `javascript:` or 200 characters** → `nack INVALID_COMMAND`, never stored, never echoed unescaped — test in Task 3 (`test_app_url_validation`).

---

## File Structure

| File | Responsibility |
|---|---|
| `firmware/uno/src/domain/lite/int_math.h/.cpp` (create) | `iatan2Cdeg`, `isqrt32` |
| `firmware/uno/src/domain/lite/tilt_lite.h/.cpp` (modify) | `pitchCdeg()`, `rollCdeg()` |
| `firmware/uno/src/lite/lite_sensors.cpp`, `lite_comms.cpp`, `lite_routes.cpp`, `domain/lite/lite_frames.cpp` (modify) | fill pitch/roll/temp; CALIBRATE SELF_TEST |
| `firmware/uno/test/test_lite_int_math`, `test_lite_frames` (create / modify) | host tests |
| `firmware/esp32/src/core/event_log.h/.cpp` (create) | the ring |
| `firmware/esp32/src/core/ws_codec.*`, `ws_json.*`, `confirm.*`, `body_link.*` (modify) | commands, JSON, `kCalibSet`, `sendCalibrate`, `SelfTestRecord` |
| `firmware/esp32/src/net/camera_service.*`, `stream_server.cpp` (modify), `net/app_settings.h/.cpp` (create) | camera settings + stats; AI address |
| `firmware/esp32/src/net/link_task.cpp`, `wifi_manager.*`, `boot_diag.*` (modify) | handlers, log sources |
| `firmware/esp32/web/basic_drive.html`, `settings.html`, `help.html`, `scripts/pack_web.py` (modify) | pages |
| `firmware/esp32/test/test_event_log` (create); `test_ws_codec`, `test_ws_json`, `test_confirm`, `test_body_link` (modify) | host tests |
| `tools/smoke/move.py` (modify) | waits for the button confirmation when saving |

Branch: `git checkout main && git pull --ff-only && git checkout -b plan-o`.

---

### Task 1: Uno Lite — temperature, pitch / roll, self-test on request

**Files:** Create `firmware/uno/src/domain/lite/int_math.h`, `int_math.cpp`, `firmware/uno/test/test_lite_int_math/test_main.cpp`; Modify `domain/lite/tilt_lite.h/.cpp`, `lite/lite_sensors.cpp`, `lite/lite_comms.cpp`, `lite/lite_routes.cpp`, `domain/lite/lite_frames.cpp`, `firmware/uno/platformio.ini` (`env:uno` `build_src_filter` adds `+<domain/lite/int_math.cpp>` if the Lite filter lists files), the Lite frames test.

**Interfaces:**
- Produces: `int16_t iatan2Cdeg(int32_t y, int32_t x)` (0.01°, −18000…18000; `(0,0)` → 0); `uint32_t isqrt32(uint32_t n)`; `int16_t TiltLite::pitchCdeg() const`, `int16_t TiltLite::rollCdeg() const` (pitch = `iatan2(x, isqrt(y²+z²))`, roll = `iatan2(y, z)` on the filtered accel — the full firmware's `attitude.cpp` convention); TELEM_FAST `pitch`/`roll` filled; TELEM_SLOW `temp` = `raw × 100 / 340 + 3653`; CALIBRATE `{op START, script SELF_TEST}` → ACK OK (BUSY while moving) and the self-test runs again.

- [ ] **Step 1: Branch + failing math tests**

```bash
git checkout main && git pull --ff-only && git checkout -b plan-o
```
`firmware/uno/test/test_lite_int_math/test_main.cpp`:
```cpp
// Uno Lite LLD §21: integer atan2 (0.01°) and isqrt32 - no floats on the Lite.
#include <math.h>
#include <unity.h>

#include "domain/lite/int_math.h"

using cq::iatan2Cdeg;
using cq::isqrt32;

void setUp() {}
void tearDown() {}

void test_iatan2_matches_atan2_every_degree() {
  for (int deg = -179; deg <= 180; ++deg) {
    const double r = deg * M_PI / 180.0;
    const int32_t y = static_cast<int32_t>(lround(sin(r) * 128)), x = static_cast<int32_t>(lround(cos(r) * 128));
    const double want = atan2(static_cast<double>(y), static_cast<double>(x)) * 18000.0 / M_PI;
    TEST_ASSERT_INT_WITHIN_MESSAGE(50, static_cast<int>(lround(want)), iatan2Cdeg(y, x), "deg");
  }
}

void test_iatan2_axes_and_zero() {
  TEST_ASSERT_EQUAL(0, iatan2Cdeg(0, 0));
  TEST_ASSERT_EQUAL(9000, iatan2Cdeg(100, 0));
  TEST_ASSERT_EQUAL(-9000, iatan2Cdeg(-100, 0));
  TEST_ASSERT_EQUAL(18000, iatan2Cdeg(0, -100));
}

void test_isqrt32() {
  TEST_ASSERT_EQUAL(0u, isqrt32(0));
  TEST_ASSERT_EQUAL(181u, isqrt32(32768));
  TEST_ASSERT_EQUAL(362u, isqrt32(131072));
  TEST_ASSERT_EQUAL(65535u, isqrt32(0xFFFFFFFFu));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_iatan2_matches_atan2_every_degree);
  RUN_TEST(test_iatan2_axes_and_zero);
  RUN_TEST(test_isqrt32);
  return UNITY_END();
}
```
Run: `cd firmware/uno && ~/.platformio/penv/bin/pio test -e native -f test_lite_int_math` → Expected FAIL: `int_math.h: No such file`.

- [ ] **Step 2: Implement `int_math`**

`int_math.h`:
```cpp
// Uno Lite LLD §21: integer maths for tilt angles (no floats are linked on the Lite).
#pragma once

#include <stdint.h>

namespace cq {

int16_t iatan2Cdeg(int32_t y, int32_t x);  // 0.01°, -18000..18000, error <= 0.25°
uint32_t isqrt32(uint32_t n);              // floor(sqrt(n))

}  // namespace cq
```
`int_math.cpp`:
```cpp
#include "domain/lite/int_math.h"

namespace cq {

uint32_t isqrt32(uint32_t n) {
  uint32_t r = 0, bit = 1UL << 30;
  while (bit > n) bit >>= 2;
  while (bit != 0) {
    if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
    else { r >>= 1; }
    bit >>= 2;
  }
  return r;
}

int16_t iatan2Cdeg(int32_t y, int32_t x) {
  if (x == 0 && y == 0) return 0;
  const uint32_t ax = static_cast<uint32_t>(x < 0 ? -x : x), ay = static_cast<uint32_t>(y < 0 ? -y : y);
  const bool swap = ay > ax;
  const uint32_t num = swap ? ax : ay, den = swap ? ay : ax;
  // r in Q15 (0..32768); atan(r) ≈ 45 r + 15.64 r (1 - r) degrees (max error 0.22°)
  const uint32_t r = (num << 15) / den;
  const uint32_t lin = (4500u * r) >> 15;
  const uint32_t quad = (((r * (32768u - r)) >> 15) * 1564u) >> 15;
  int32_t a = static_cast<int32_t>(lin + quad);  // 0..4500
  if (swap) a = 9000 - a;
  if (x < 0) a = 18000 - a;
  return static_cast<int16_t>(y < 0 ? -a : a);
}

}  // namespace cq
```
(The native `build_src_filter` of the Uno project must include `domain/lite/`; if `env:native` already compiles `+<domain/>`, nothing to add.)
Run the test → Expected `3 test cases: 3 succeeded`.

- [ ] **Step 3: Tilt angles, temperature, CALIBRATE SELF_TEST (host tests first)**

Add to the Lite tilt host test (the existing `test_lite_tilt` or the suite that covers `TiltLite`; create `test_lite_tilt_angles` if none):
```cpp
void test_tilt_angles_from_filtered_accel() {
  cq::TiltLite t;
  for (int i = 0; i < 200; ++i) t.update(0, 0, 16384, 45, static_cast<uint32_t>(i * 20));  // level, 1 g on z
  TEST_ASSERT_INT_WITHIN(50, 0, t.pitchCdeg());
  TEST_ASSERT_INT_WITHIN(50, 0, t.rollCdeg());
  for (int i = 0; i < 400; ++i) t.update(8192, 0, 14189, 45, static_cast<uint32_t>(4000 + i * 20));  // 30° on x
  TEST_ASSERT_INT_WITHIN(60, 3000, t.pitchCdeg());
}
```
(Read `tilt_lite.cpp` for the raw-to-filter scale before choosing the inputs: the filter keeps 128 per g after `/64`; raw 16384 = 1 g at ±2 g range. Adjust the raw values if the driver's range differs and note it.)
And to the Lite frames test:
```cpp
void test_calibrate_is_handled_for_self_test() {
  TEST_ASSERT_EQUAL(static_cast<int>(cq::lite_frames::FrameUse::kHandled),
                    static_cast<int>(cq::lite_frames::frameUse(static_cast<uint8_t>(cq::proto::FrameType::kCalibrate))));
}
```
Run both → Expected FAIL (no `pitchCdeg`; CALIBRATE still unavailable).
Implement:
- `TiltLite`: `int16_t pitchCdeg() const { const int32_t x = m_fx / 64, y = m_fy / 64, z = m_fz / 64; return iatan2Cdeg(x, static_cast<int32_t>(isqrt32(static_cast<uint32_t>(y * y + z * z)))); }` and `int16_t rollCdeg() const { return iatan2Cdeg(m_fy / 64, m_fz / 64); }` (same 128-per-g scale as `over()`).
- `lite_sensors.cpp`: after `g_lite.tilt.update(...)`, set the fast frame's `pitch = g_lite.tilt.pitchCdeg()`, `roll = g_lite.tilt.rollCdeg()`; store `g_lite.imu_temp_raw = r.temp`.
- `lite_comms.cpp` (TELEM_SLOW fill): `slow.temp = static_cast<int16_t>(static_cast<int32_t>(g_lite.imu_temp_raw) * 100 / 340 + 3653);` (add `int16_t imu_temp_raw` to the Lite state struct).
- `lite_frames.cpp`: move `F::kCalibrate` to the handled list.
- `lite_routes.cpp`: route CALIBRATE: decode `proto::CalibrateMsg`; `op == CalOp::kStart && script == CalScript::kSelfTest` → if `g_lite.moving` return `AckStatus::kBusy`; else `g_lite.self_test_pending = true; return kOk;`; any other script → `kUnavailable`. (`selfTestTask` already runs when `self_test_pending` and > 2 s after boot, and sends EVENT SELFTEST.)
Run the Lite native suite → all pass.

- [ ] **Step 4: Flash budget, commit**

Run: `cd firmware/uno && ~/.platformio/penv/bin/pio run -e uno | grep Flash`
Expected: ≤ 31 444 B (≤ +400 B). If over: Ruling in the ledger + drop the cheapest feature in this order: temperature last-value caching → combine pitch/roll into one call → ask the owner.
```bash
git add firmware/uno
git commit -m "feat(uno): Lite board temperature, integer pitch/roll and self-test on request (Lite LLD §21)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Bridge `EventLog`

**Files:** Create `firmware/esp32/src/core/event_log.h`, `event_log.cpp`, `firmware/esp32/test/test_event_log/test_main.cpp`.

**Interfaces:**
- Produces: `enum class LogKind : uint8_t { kBoot, kUno, kPrimitive, kStopHold, kClientLost, kBodyOnline, kBodyOffline, kWifiMode, kRestart, kOta }`; `struct LogEntry { uint32_t seq; uint32_t t_ms; uint32_t uno_ms; LogKind kind; int32_t a; int32_t b; char client[12]; }`; `class EventLog { static constexpr uint16_t kCap = 200; uint32_t add(LogKind, int32_t a, int32_t b, const char* client, uint32_t uno_ms, uint32_t now_ms); uint8_t since(uint32_t seq, LogEntry* out, uint8_t max, bool& gap) const; uint32_t last() const; uint32_t oldest() const; }`.

- [ ] **Step 1: Failing tests**

```cpp
// Bridge LLD R13: the event log ring (200), paging with since(), wrap and gap.
#include <string.h>
#include <unity.h>

#include "core/event_log.h"

using namespace cq::core;

void setUp() {}
void tearDown() {}

void test_log_add_and_since_pages() {
  EventLog l;
  for (int i = 0; i < 30; ++i) l.add(LogKind::kUno, i, 0, "phone-2", 0, static_cast<uint32_t>(i));
  LogEntry out[25];
  bool gap = false;
  TEST_ASSERT_EQUAL(25, l.since(0, out, 25, gap));
  TEST_ASSERT_EQUAL(1u, out[0].seq);
  TEST_ASSERT_FALSE(gap);
  TEST_ASSERT_EQUAL(5, l.since(25, out, 25, gap));
  TEST_ASSERT_EQUAL(30u, out[4].seq);
  TEST_ASSERT_EQUAL(0, l.since(30, out, 25, gap));
  TEST_ASSERT_EQUAL(30u, l.last());
}

void test_log_since_after_wrap_returns_oldest_kept() {
  EventLog l;
  for (int i = 0; i < 250; ++i) l.add(LogKind::kUno, i, 0, "", 0, 0);
  LogEntry out[25];
  bool gap = false;
  TEST_ASSERT_EQUAL(25, l.since(10, out, 25, gap));
  TEST_ASSERT_TRUE(gap);
  TEST_ASSERT_EQUAL(51u, out[0].seq);  // 250 - 200 + 1
  TEST_ASSERT_EQUAL(51u, l.oldest());
}

void test_log_client_name_truncated_and_terminated() {
  EventLog l;
  l.add(LogKind::kStopHold, 0, 0, "a-very-long-client-name", 0, 0);
  LogEntry out[1];
  bool gap;
  l.since(0, out, 1, gap);
  TEST_ASSERT_EQUAL(11, static_cast<int>(strlen(out[0].client)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_log_add_and_since_pages);
  RUN_TEST(test_log_since_after_wrap_returns_oldest_kept);
  RUN_TEST(test_log_client_name_truncated_and_terminated);
  return UNITY_END();
}
```
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_event_log` → Expected FAIL (no header).

- [ ] **Step 2: Implement**

`event_log.h`:
```cpp
// Bridge LLD R13: the last 200 things that happened (Uno events, stops, WiFi, restarts) for the Log tab. Pure C++.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

enum class LogKind : uint8_t { kBoot, kUno, kPrimitive, kStopHold, kClientLost, kBodyOnline, kBodyOffline, kWifiMode, kRestart, kOta };

struct LogEntry {
  uint32_t seq = 0;
  uint32_t t_ms = 0;   // bridge uptime
  uint32_t uno_ms = 0;
  LogKind kind = LogKind::kBoot;
  int32_t a = 0;       // kUno: event kind; kPrimitive: EndReason; kWifiMode: 0 home / 1 own AP / 2 fallback AP; kBoot: reason
  int32_t b = 0;       // kUno: payload a; kPrimitive: PrimKind
  char client[12] = "";
};

class EventLog {
 public:
  static constexpr uint16_t kCap = 200;
  uint32_t add(LogKind kind, int32_t a, int32_t b, const char* client, uint32_t uno_ms, uint32_t now_ms);
  // Entries with seq > `seq`, oldest first, at most `max`; gap = some asked-for entries were already overwritten.
  uint8_t since(uint32_t seq, LogEntry* out, uint8_t max, bool& gap) const;
  uint32_t last() const { return m_next - 1; }
  uint32_t oldest() const { return m_next > kCap ? m_next - kCap : 1; }

 private:
  LogEntry m_e[kCap];
  uint32_t m_next = 1;  // seq of the next entry
};

}  // namespace core
}  // namespace cq
```
`event_log.cpp`:
```cpp
#include "core/event_log.h"

#include <string.h>

namespace cq {
namespace core {

uint32_t EventLog::add(LogKind kind, int32_t a, int32_t b, const char* client, uint32_t uno_ms, uint32_t now_ms) {
  LogEntry& e = m_e[(m_next - 1) % kCap];
  e.seq = m_next;
  e.t_ms = now_ms;
  e.uno_ms = uno_ms;
  e.kind = kind;
  e.a = a;
  e.b = b;
  strncpy(e.client, client != nullptr ? client : "", sizeof(e.client) - 1);
  e.client[sizeof(e.client) - 1] = '\0';
  return m_next++;
}

uint8_t EventLog::since(uint32_t seq, LogEntry* out, uint8_t max, bool& gap) const {
  const uint32_t first_kept = oldest();
  uint32_t s = seq + 1;
  gap = s < first_kept && m_next > 1;
  if (s < first_kept) s = first_kept;
  uint8_t n = 0;
  for (; s < m_next && n < max; ++s) out[n++] = m_e[(s - 1) % kCap];
  return n;
}

}  // namespace core
}  // namespace cq
```
Run → Expected `3 test cases: 3 succeeded`.

- [ ] **Step 3: Commit**

```bash
git add firmware/esp32/src/core/event_log.* firmware/esp32/test/test_event_log
git commit -m "feat(bridge): event log ring (200) with since() paging (LLD R13)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Bridge commands, JSON, confirmation and body link

**Files:** Modify `firmware/esp32/src/core/ws_codec.h/.cpp`, `ws_json.h/.cpp`, `confirm.h/.cpp`, `body_link.h/.cpp`; tests in `test_ws_codec`, `test_ws_json`, `test_confirm`, `test_body_link`.

**Interfaces:**
- Consumes: `EventLog`, `LogEntry`.
- Produces:
  - `WsCmdKind::kTank`, `kLog`, `kCameraSet`, `kSelfTest`, `kAppSet`; `WsCommand` fields `log_since` (uint32), `cam` (`CamFields { bool has_size, has_quality, has_fps, has_bright, has_contrast, has_low, has_vflip, has_hmirror, reset; uint8_t size /*0 QVGA,1 VGA,2 SVGA,3 HD*/, quality /*10-40*/, fps /*0,5,10,15*/; int8_t bright, contrast /*-2..2*/; bool low, vflip, hmirror; }`), `ai_url[97]`; JSON names `size` ∈ {"QVGA","VGA","SVGA","HD"}.
  - `bool appUrlValid(const char* url)` (`http://` prefix, 8–96 printable ASCII, no space, quote, backslash or `<>`).
  - `ChangeKind::kCalibSet` + `PendingChange::cpm` (uint16); `changeName(kCalibSet)` = `"calib"`.
  - `struct SelfTestRecord { bool seen; uint8_t mask; bool locked; uint32_t uno_ms; }` in `BodyLink` (`selfTest()`), filled from EVENT SELFTEST (a = mask, b = locked); `bool BodyLink::sendCalibrate(uint8_t op, uint8_t script, uint32_t now_ms, uint8_t& seq_out)` (acked slot).
  - `const char* unoEventName(uint8_t kind)` — every EventKind the Lite sends (incl. `selftest`, `imu_fault`, `us_fault`, `envelope_changed`, `settings_saved`, `settings_reset`, `rejected`, `heading_fault`).
  - `size_t jsonTank(char*, size_t, const TankView&)` with `struct TankView { SelfTestRecord st; uint8_t hw; uint8_t cam_size, cam_quality, cam_fps; int8_t cam_bright, cam_contrast; bool cam_low, cam_vflip, cam_hmirror; uint8_t viewers; uint16_t fps_x10; uint16_t kbps; const char* ai_url; uint32_t uptime_s; }`.
  - `size_t jsonLog(char*, size_t, const LogEntry* e, uint8_t n, uint32_t last, bool gap, uint32_t now_ms)` → `{"t":"log","entries":[{"seq","age_s","kind","ev"?,"a","b","client"}],"last","gap"?}` (`kind` names: boot, uno, primitive, stop_hold, client_lost, body_online, body_offline, wifi_mode, restart, ota; `ev` = `unoEventName(a)` for `uno`).
  - `jsonTs` adds `"temp_c"` (1 decimal; `null` before the first slow frame).

- [ ] **Step 1: Failing tests** (add to the existing suites; register with `RUN_TEST`)

`test_ws_codec`:
```cpp
void test_tank_log_selftest() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kTank), K(P("{\"t\":\"tank\"}").kind));
  const WsCommand l = P("{\"t\":\"log\",\"since\":57}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kLog), K(l.kind));
  TEST_ASSERT_EQUAL_UINT32(57, l.log_since);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSelfTest), K(P("{\"t\":\"selftest\",\"id\":3}").kind));
}

void test_camera_set_fields_and_ranges() {
  const WsCommand c = P("{\"t\":\"camera_set\",\"size\":\"SVGA\",\"quality\":20,\"fps_cap\":10,\"brightness\":-1,"
                        "\"low_light\":true,\"hmirror\":false,\"id\":4}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCameraSet), K(c.kind));
  TEST_ASSERT_TRUE(c.cam.has_size);
  TEST_ASSERT_EQUAL(2, c.cam.size);
  TEST_ASSERT_EQUAL(20, c.cam.quality);
  TEST_ASSERT_EQUAL(10, c.cam.fps);
  TEST_ASSERT_EQUAL(-1, c.cam.bright);
  TEST_ASSERT_TRUE(c.cam.low);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"camera_set\",\"quality\":5}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"camera_set\",\"size\":\"UXGA\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"camera_set\",\"fps_cap\":7}").kind));
  TEST_ASSERT_TRUE(P("{\"t\":\"camera_set\",\"reset\":true}").cam.reset);
}

void test_app_url_validation() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kAppSet), K(P("{\"t\":\"app_set\",\"ai_url\":\"http://192.168.31.8:8000/\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kAppSet), K(P("{\"t\":\"app_set\",\"ai_url\":\"\"}").kind));  // clears it
  TEST_ASSERT_FALSE(cq::core::appUrlValid("javascript:alert(1)"));
  TEST_ASSERT_FALSE(cq::core::appUrlValid("http://a b"));
  TEST_ASSERT_FALSE(cq::core::appUrlValid("http://x\"y"));
  char longu[220] = "http://";
  for (int i = 7; i < 210; ++i) longu[i] = 'a';
  longu[210] = '\0';
  TEST_ASSERT_FALSE(cq::core::appUrlValid(longu));
  TEST_ASSERT_TRUE(cq::core::appUrlValid("http://laptop.local:8000/ai"));
}
```
`test_ws_json`:
```cpp
void test_json_log_entries_and_gap() {
  cq::core::LogEntry e[2];
  e[0].seq = 51; e[0].t_ms = 1000; e[0].kind = cq::core::LogKind::kUno; e[0].a = 6; e[0].b = 1;
  e[1].seq = 52; e[1].t_ms = 5000; e[1].kind = cq::core::LogKind::kStopHold; strcpy(e[1].client, "phone-3");
  char out[512];
  cq::core::jsonLog(out, sizeof out, e, 2, 52, true, 9000);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"kind\":\"uno\",\"ev\":\"stall\""));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"age_s\":8"));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"client\":\"phone-3\""));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"gap\":true"));
}

void test_json_tank_and_ts_temp() {
  cq::core::TankView v{};
  v.st.seen = true; v.st.mask = 0; v.cam_size = 1; v.cam_quality = 12; v.viewers = 1; v.fps_x10 = 125;
  v.ai_url = "http://x\"y";  // escaped even if it ever got in
  char out[512];
  cq::core::jsonTank(out, sizeof out, v);
  TEST_ASSERT_NOT_NULL(strstr(out, "\"size\":\"VGA\""));
  TEST_ASSERT_NOT_NULL(strstr(out, "\"fps\":12.5"));
  TEST_ASSERT_NOT_NULL(strstr(out, "x\\\"y"));
}
```
(`jsonTs` temp: extend its existing test with a snapshot whose `slow.temp = 3125` and expect `"temp_c":31.3`.)
`test_confirm`: `test_confirm_calib_set_kind` — `changeName(ChangeKind::kCalibSet)` is `"calib"` and a gate holding a `kCalibSet` with `cpm = 7295` returns it from `take()`.
`test_body_link`: an EVENT SELFTEST frame (a = 0b0001, b = 1) → `bl->selfTest().seen && mask == 1 && locked`; `sendCalibrate(0, 9, …)` emits one CALIBRATE frame with `script == 9`.
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native > /tmp/o3.log 2>&1; grep -E "FAIL|error" /tmp/o3.log | head` → Expected: compile errors for the missing names.

- [ ] **Step 2: Implement**
- `ws_codec`: names in `kindOf` (`"tank"`, `"log"`, `"camera_set"`, `"selftest"`, `"app_set"`); keys `since` (whole ≥ 0), `size` (string → 0..3 else invalid), `quality` (whole 10..40), `fps_cap` (0/5/10/15), `brightness` / `contrast` (whole −2..2), `low_light` / `vflip` / `hmirror` (bool), `reset` (bool for camera_set — keep the existing string `reset` for calib: read the value once and interpret per command in `complete()`), `ai_url` (string ≤ 96 → `c.ai_url`; `complete()` requires `appUrlValid` or empty). `complete()`: `kCameraSet` needs at least one field or `reset`; `kAppSet` needs `ai_url` present.
- `appUrlValid` in `ws_codec.cpp` (declared in the header).
- `ws_json`: `unoEventName()` covering every `proto::EventKind` (extend the switch already used by `jsonUnoEvent`, which now calls it); `jsonLog`, `jsonTank` (all strings through `jsonEscape`), `temp_c` in `jsonTs`.
- `confirm`: `kCalibSet` + `cpm`.
- `body_link`: `SelfTestRecord m_self_test` set in the EVENT handler for `EventKind::kSelftest` (the event is still queued as before); `sendCalibrate` through the acked slot like `sendSettingGet`.
Run the whole native suite → Expected: all pass.

- [ ] **Step 3: Commit**

```bash
git add firmware/esp32/src/core firmware/esp32/test
git commit -m "feat(bridge): tank / log / camera_set / selftest / app_set commands, JSON, calib-set confirmation, self-test record (LLD R13)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Camera settings at run time + AI address store

**Files:** Modify `firmware/esp32/src/net/camera_service.h/.cpp`, `net/stream_server.cpp`; Create `firmware/esp32/src/net/app_settings.h/.cpp`.

**Interfaces:**
- Produces: `struct CamSettings { uint8_t size = 1; uint8_t quality = 12; uint8_t fps_cap = 0; int8_t brightness = 0; int8_t contrast = 0; bool low_light = false; bool vflip = true; bool hmirror = false; }`; `bool cameraPost(const CamSettings&)` (queue → applied by the camera side; saves NVS `cq_cam` key `s`); `CamSettings cameraSettings()`; `struct CamStats { uint16_t fps_x10; uint16_t kbps; }` `CamStats cameraStats()` (last 2 s); `bool appSettingsLoad(char* url, size_t cap)`, `bool appSettingsSave(const char* url)` (NVS `cq_app` key `ai`).
- Rules: `esp_camera_init` with `FRAMESIZE_HD` and `fb_count = 2` in PSRAM (buffers big enough for every allowed size), then apply the stored settings (`set_framesize`, `set_quality`, `set_brightness`, `set_contrast`, `set_gainceiling(low_light ? GAINCEILING_16X : GAINCEILING_4X)`, `set_vflip`, `set_hmirror`); the stream worker applies a queued change between frames and sleeps to honour `fps_cap`; frame / byte counters are atomic.

- [ ] **Step 1: Implement (device code; tested on the tank in Task 7)**
`camera_service.cpp`: init at HD; `static QueueHandle_t g_cam_q` (depth 2); `cameraPost` copies into the queue; `void cameraApplyPending()` (called by the stream worker before each grab and by `cameraBegin` after init) pops and applies via `sensor_t*` setters, then saves NVS; `cameraSettings()` returns the current copy (spinlock). Map size 0..3 → `FRAMESIZE_QVGA, VGA, SVGA, HD`. Keep the weak XCLK drive (R12) after init.
`stream_server.cpp`: in `serveStream` loop: `cameraApplyPending()`; enforce `fps_cap` (`vTaskDelay` until `1000 / fps_cap` ms since the previous frame); add `fb->len` to `g_bytes` and 1 to `g_frames`; `cameraStats()` computes from counters sampled every 2 s.
`app_settings.cpp`: `Preferences` namespace `cq_app`, key `ai`, string ≤ 96.
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge | tail -3` → Expected `[SUCCESS]`.

- [ ] **Step 2: Commit**

```bash
git add firmware/esp32/src/net
git commit -m "feat(bridge): camera settings at run time (NVS cq_cam, fps cap, stats) and the AI address store (LLD R13)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Link task — handlers and log sources

**Files:** Modify `firmware/esp32/src/net/link_task.cpp`, `net/wifi_manager.h/.cpp` (mode-change result), `net/boot_diag.h/.cpp` (`uint8_t bootReasonCode()` if missing), `tools/smoke/move.py`.

**Interfaces:**
- Consumes: Tasks 2–4.
- Produces: `g_log` (`EventLog`); handlers `tank`, `log` (25 per reply), `camera_set` (→ `cameraPost`, then `tank` to the sender), `selftest` (driving / primitive running → `BUSY`; body offline → `BODY_OFFLINE`; else `sendCalibrate(kStart, kSelfTest)` through `AckedOps` with ack to the sender), `app_set`; `calib` set → `ConfirmGate` (`kCalibSet`), applied in `applyChange` with today's checks repeated at apply time (still the driver? no primitive?) → `confirmed` + `calib`; `WifiResult::kModeChanged` posted by `WifiManager` whenever its mode changes (home / own AP / fallback AP).
- Log sources (each `g_log.add(...)` next to the existing code): boot reason at `linkTaskStart`; every Uno EVENT (kind, payload a) in `handleLinkEvents`; `runMotion` when `o.event && reason != done`; `LinkOwner` STOP hold set (stopper + driver names); a silent / closed client that was the driver; body online / offline; `kModeChanged`; restart asked; OTA start.

- [ ] **Step 1: Implement**, then `pio test -e native` (all suites) and `pio run -e bridge`.
`move.py`: after `{"t":"calib","counts_per_m":n,"id"}`, wait for `confirmed` (print "press the tank's button to save") or `confirm_expired` (print "not saved").
Run: `cd tools/smoke && uv run ruff check . && uv run pyright && uv run pytest -q` → clean.
Expected: native suite all pass; bridge builds.

- [ ] **Step 2: Commit**

```bash
git add firmware/esp32/src tools/smoke/move.py
git commit -m "feat(bridge): tank app handlers and event-log sources; calib saves wait for the tank's button (LLD R13)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: Pages — menu, four-tab Settings, Tune, Log

**Files:** Modify `firmware/esp32/web/basic_drive.html`, `firmware/esp32/web/settings.html`, `firmware/esp32/web/help.html`, `firmware/esp32/scripts/pack_web.py` (settings budget 36 KB).

**Interfaces:**
- Consumes: `tank`, `log`, `camera_set`, `selftest`, `app_set`, `ts.temp_c`, `tf.pitch/roll`, `td.enc`, `calib`, `move`, `primitive_done`, `confirm`/`confirmed`/`confirm_expired`.

- [ ] **Step 1: Drive menu** — replace the `⚙ SET` link with a `☰ MENU` chip opening a sheet (screen 17): LOGO (disabled, badge "COMING SOON"), SETTINGS (`/settings`), AI (disabled "NOT SET UP" unless `tank.ai_url` — the page asks `{"t":"tank"}` once after `welcome` and opens the URL in a new tab), HELP (`/help`). The sheet closes on outside tap; STOP stays above the dim layer (z-index).

- [ ] **Step 2: Settings tabs** — four tabs, the active one in `location.hash` (`#wifi`, `#tank`, `#tune`, `#log`); WiFi unchanged; **Tank** cards as screen 20 with these sources: Live sensors (`tf.dist_cm`, `tf.yaw`, `tf.yaw_rate`, `tf.pitch`, `tf.roll`, `td.enc` via `{"t":"debug","on":true,"hz":10}` while the tab is visible — off when leaving it, `ts.temp_c` labelled "BOARD TEMPERATURE", tilted / lifted from `ts.flags`); Self-test (`tank.selftest` → per check from `mask` bits 0 IMU, 1 distance sensor, 2 battery, 3 settings; LOCKED badge; RUN THE CHECKS AGAIN → `selftest` + "the tank must stand still"); Camera (viewers, `fps`, `kbps`; size segmented, quality slider, fps segmented, brightness / contrast steppers, low-light / flip / mirror toggles, RESET → `camera_set`); Child play (read only: `ts.envelope`, badge ON/OFF, note "turning it on comes with the child-play design"); Safety; Track model; Maintenance (+ AI address input → `app_set`).

- [ ] **Step 3: Tune** — Live graphs (screen 18): canvas 60 s ring at 10 Hz; TRACK SPEED = Δenc / 0.1 s × 100 / counts_per_m cm/s (from `calib`), HEADING from `tf.yaw` (unwrapped), BALANCE = (right − left) / ((right + left) / 2) × 1000 ‰ when both > 2 cm/s; HOLD freezes; CSV downloads the ring. Distance + Straight driving cards moved from Tank (same ids and logic). Wizard: step 1 text + DRIVE 1 m (`move` 100 cm at 40 %; disabled unless the page is the driver or nobody drives) + STOP (`stop`) + measured input → shows `round(Δcounts × 100 / measured)` → SAVE (`calib` set → confirm box) — Δcounts from `td.enc` mean of both tracks between `ack` and `primitive_done`.

- [ ] **Step 4: Log** — on opening: `log` since 0 until `last`, then every 2 s while visible; filters ALL / STOPS (uno: stall, blocked, deadman_stop, ir_override, link_lost, batt_critical, tilted, lifted, heading_fault; stop_hold; client_lost; primitive not done) / WIFI (wifi_mode) / SYSTEM (the rest); rows: age, icon, title, plain detail line (a small table maps each kind/ev to text — e.g. stall "right track stuck — no speed for 0.5 s"); a "some older events were dropped" row when `gap`; CSV.

- [ ] **Step 5: Page checks** — Playwright with a stand-in WebSocket (as in Plan M): menu opens, items enabled/disabled; tab hash restores; live sensors render from fake `tf`/`td`/`ts`; camera change sends one `camera_set`; selftest button sends `selftest`; graphs draw from 30 fake `td`; wizard: fake ack + `td` + `primitive_done` → value shown → SAVE sends `calib` with the expected number; log paging requests `since` correctly and shows the gap row. Screenshots phone + desktop for the owner.
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge | grep pack_web` → Expected `/settings` ≤ 36 KB, `/` ≤ 24 KB.

- [ ] **Step 6: Help + commit** — help.html: menu, the four tabs, the new WebSocket commands. 
```bash
git add firmware/esp32/web firmware/esp32/scripts/pack_web.py
git commit -m "feat(bridge): tank app pages - menu, four-tab settings with live sensors, self-test, camera, graphs, wizard, log

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 7: On the tank, docs, review

**Files:** `docs/design-notes.md` (§92 results), `docs/lld/esp32-bridge-lld.md` (R13 "as built" line), `CLAUDE.md` (plan list), `docs/design/bridge-ux.md` (state column).

- [ ] **Step 1: Flash** — Uno over USB (Linux machine, S1 to upload, avrdude as before), bridge over USB (partition table unchanged in this plan) or OTA. Before anything moves: "stand or floor?".
- [ ] **Step 2: Stand checks (tracks off the ground)** — menu items; Tank tab: live values change when the tank is tilted by hand (pitch / roll sign: nose up = positive), board temperature plausible (25–45 °C), self-test RUN → event in the log, locked badge only when a check fails; camera: each size QVGA → HD, quality, fps cap 5 → the page shows ~5 fps; brightness visible; **WiFi check at HD and at max fps: 40 pings, loss and delay recorded**; Log: STOP from a second device appears as "stopped by another device", a WiFi double press appears; AI address saved and the menu item opens it.
- [ ] **Step 3: Floor check** — wizard: DRIVE 1 m (say: forward 1 m at 40 %, STOP within reach), tape measure, value shown, SAVE with the tank's button, `calib` shows it; then `move.py` saving also asks for the button.
- [ ] **Step 4: Docs** — design-notes §92 with the measured Uno flash, camera fps/kbps per size, WiFi ping results, floor result; R13 marked as built; CLAUDE.md plan list adds Plan O.
- [ ] **Step 5: Final review** — whole-branch review package; fix Critical/Important; ask the owner before merging `plan-o`.
