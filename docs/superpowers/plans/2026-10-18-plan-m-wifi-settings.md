# Plan M — Bridge WiFi modes + `/settings` page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The bridge keeps up to 3 home networks in NVS, joins the strongest one, switches home ↔ its own AP on a double press of the tank's button (or a confirmed page command), and serves a `/settings` page (WiFi, Tank info, Calibration, Maintenance) whose every change waits for a press of the tank's button.

**Architecture:** Pure core classes (`WifiStore`, `ConfirmGate`, `DoublePress`, `ButtonRouter`, `AckedOps`, forced-AP `WifiPolicy`) carry the logic and are host-tested. The Arduino loop's `WifiManager` owns the radio and NVS; the link task owns WebSocket I/O, the button and the confirmations; two FreeRTOS queues join them (`wsSend` is link-task only). The page is static HTML on the bridge talking WebSocket JSON.

**Tech Stack:** C++17 ESP32-S3 Arduino (`bridge` / `bridge_ota`), Unity host tests, `scripts/pack_web.py`, HTML/JS (no framework), Playwright for the page check.

**Spec:** `docs/superpowers/specs/2026-10-10-02-tank-firmware-design.md` Part I; LLD `docs/lld/esp32-bridge-lld.md` Revision 9; UX design-notes §72 + `docs/design/stitch/12-bridge-settings-desktop-v2.png`.

## Global Constraints

- Bridge only: no Uno change, no `protocol/defs` change (protocol 3).
- ≤ 3 saved networks; SSID 1…32 bytes; password empty (open) or 8…63; own-AP password 8…63; NVS namespace `cq_wifi`, key `store`; first boot with no store → `secrets.h` network in slot 1 and `secrets.h` AP password.
- Boot: join the **strongest visible saved network**; none in 10 s → own AP `conqueror-bridge`; forced AP (double press / confirmed `wifi_mode`) has no station retries; **not remembered** across restarts.
- Double press = two `button_press` events ≤ 1000 ms apart while no confirmation waits; a press while a confirmation waits only confirms.
- Every client change (`wifi_save`, `wifi_forget`, `wifi_mode`, `ap_password`, `calib` reset, `setting` set) waits ≤ 30 s for a press; replies `confirm` → `confirmed` / `confirm_expired`.
- **Passwords never appear in any JSON reply or log line.**
- `setting`: get ids {1, 11, 12, 66}; set only 66 (`track_balance`, −100…100), then `SETTINGS_SAVE`.
- `wsSend` only from the link task; `WiFi.*` and the WiFi NVS only from the Arduino loop.
- Mode switch feedback: SOUND `kJingle`, LED blink blue (home) / white (own AP), 3 s.
- Safety unchanged; the page keeps an Emergency STOP on every tab (sends `{"t":"stop"}`).
- Commits: Conventional Commits + `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch **`plan-m`**; ask before merging.
- Hardware: owner present; no motion in this plan except none (WiFi only) — still say what will happen before each test.

## Review Focus

1. **The network in use is edited or forgotten** → the bridge must reconnect (or fall back to its AP), never stay "connected" to stale credentials — test in Task 1 (`test_store_edit_in_use_flags_reconnect`) and Task 6 glue.
2. **A corrupt or old NVS blob** (version mismatch, short read) → treated as empty and re-seeded from `secrets.h`, never a crash or garbage SSID — test in Task 1 (`test_store_decode_rejects_corrupt`).
3. **Three presses in a row** → one toggle, the third press starts a new double, never a second toggle — test in Task 2 (`test_double_press_three_presses_one_toggle`).
4. **The confirming client disconnects** while a change waits → the change is dropped, a later press does nothing — test in Task 2 (`test_gate_drop_client`).
5. **An SSID with quotes or backslashes** → escaped in every JSON reply, never breaks the page — test in Task 4 (`test_wifi_status_escapes_ssid`).

---

## File Structure

| File | Responsibility |
|---|---|
| `src/core/wifi_store.h/.cpp` (create) | saved networks + AP password, validation, NVS blob, strongest pick |
| `src/core/confirm.h/.cpp` (create) | `PendingChange`, `ConfirmGate`, `DoublePress`, `ButtonRouter` |
| `src/core/acked_ops.h/.cpp` (create) | one-at-a-time queue of acked Uno ops (SOUND, LED, SETTING_*) |
| `src/core/wifi_policy.h` (modify) | `kForcedAp`, `forceAp`, `forceHome` |
| `src/core/ws_codec.h/.cpp`, `ws_json.h/.cpp` (modify) | new commands and replies |
| `src/core/body_link.h/.cpp` (modify) | `sendLed`, `sendSettingGet/Set`, `sendSettingsSave`, `ackPending`, `kSettingValue` event |
| `src/net/wifi_manager.h/.cpp` (modify) | store in NVS, scan / strongest join, forced AP, request / result queues |
| `src/net/link_task.cpp` (modify) | commands, button, confirmations, results, settings ops, restart |
| `web/settings.html` (create), `web/basic_drive.html` (modify), `scripts/pack_web.py` (modify) | the page, the drive-page link, packing |
| `test/test_wifi_store`, `test/test_confirm` (create); `test_wifi_policy`, `test_ws_codec`, `test_ws_json`, `test_body_link` (modify) | host tests |

All paths under `firmware/esp32/`. Branch: `git checkout main && git pull --ff-only && git checkout -b plan-m`.

---

### Task 1: `WifiStore`

**Files:** Create `src/core/wifi_store.h`, `src/core/wifi_store.cpp`; Test `test/test_wifi_store/test_main.cpp`.

**Interfaces — Produces:**
```cpp
namespace cq::core {
struct ScanEntry { char ssid[33]; int8_t rssi; bool secure; };
enum class StoreResult : uint8_t { kOk, kBadSsid, kBadPassword, kFull, kBadSlot };
class WifiStore {
 public:
  static constexpr uint8_t kSlots = 3;
  static constexpr size_t kBlobSize = 1 + kSlots * (33 + 64) + 64;  // version, slots, ap password
  void clear();
  void seed(const char* ssid, const char* pass, const char* ap_pass);  // first boot
  StoreResult save(int slot, const char* ssid, const char* pass);     // slot 0..2, -1 = same name or first free
  StoreResult forget(int slot);                                        // -1 = all
  StoreResult setApPassword(const char* pass);
  uint8_t count() const;
  const char* ssid(uint8_t slot) const;  // "" when empty
  const char* pass(uint8_t slot) const;
  const char* apPass() const;
  int pickStrongest(const ScanEntry* nets, size_t n) const;  // slot of the strongest visible saved network, -1
  int slotOf(const char* ssid) const;                         // -1
  size_t encode(uint8_t* out) const;                          // kBlobSize bytes
  bool decode(const uint8_t* in, size_t n);                   // false (and cleared) when invalid
};
}
```

- [ ] **Step 1: Failing tests** — `test/test_wifi_store/test_main.cpp`:

```cpp
// Plan M (bridge LLD R9.2): saved networks + AP password.
#include <string.h>
#include <unity.h>

#include "core/wifi_store.h"

using namespace cq::core;

void setUp() {}
void tearDown() {}

namespace {
int S(StoreResult r) { return static_cast<int>(r); }
ScanEntry net(const char* ssid, int8_t rssi) {
  ScanEntry e{};
  strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
  e.rssi = rssi;
  e.secure = true;
  return e;
}
}  // namespace

void test_store_seed_and_read() {
  WifiStore s;
  s.clear();
  s.seed("MyHome", "homepass1", "appass12");
  TEST_ASSERT_EQUAL_UINT8(1, s.count());
  TEST_ASSERT_EQUAL_STRING("MyHome", s.ssid(0));
  TEST_ASSERT_EQUAL_STRING("homepass1", s.pass(0));
  TEST_ASSERT_EQUAL_STRING("appass12", s.apPass());
  TEST_ASSERT_EQUAL_STRING("", s.ssid(1));
}

void test_store_save_new_edit_full() {
  WifiStore s;
  s.clear();
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(-1, "A", "password1")));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(-1, "B", "")));  // open network
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(-1, "A", "password2")));  // same name: edit in place
  TEST_ASSERT_EQUAL_UINT8(2, s.count());
  TEST_ASSERT_EQUAL_STRING("password2", s.pass(0));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(-1, "C", "password3")));
  TEST_ASSERT_EQUAL(S(StoreResult::kFull), S(s.save(-1, "D", "password4")));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(1, "D", "password4")));  // explicit slot replaces B
  TEST_ASSERT_EQUAL_STRING("D", s.ssid(1));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadSlot), S(s.save(3, "E", "password5")));
}

void test_store_rejects_bad_input() {
  WifiStore s;
  s.clear();
  TEST_ASSERT_EQUAL(S(StoreResult::kBadSsid), S(s.save(-1, "", "password1")));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadSsid), S(s.save(-1, "123456789012345678901234567890123", "password1")));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadPassword), S(s.save(-1, "A", "short")));
  char longpw[70];
  memset(longpw, 'x', 64);
  longpw[64] = '\0';
  TEST_ASSERT_EQUAL(S(StoreResult::kBadPassword), S(s.save(-1, "A", longpw)));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadPassword), S(s.setApPassword("1234567")));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.setApPassword("12345678")));
  TEST_ASSERT_EQUAL_UINT8(0, s.count());
}

void test_store_forget_one_and_all() {
  WifiStore s;
  s.clear();
  s.save(-1, "A", "password1");
  s.save(-1, "B", "password2");
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.forget(0)));
  TEST_ASSERT_EQUAL_UINT8(1, s.count());
  TEST_ASSERT_EQUAL(-1, s.slotOf("A"));
  TEST_ASSERT_EQUAL(1, s.slotOf("B"));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadSlot), S(s.forget(5)));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.forget(-1)));
  TEST_ASSERT_EQUAL_UINT8(0, s.count());
}

void test_store_pick_strongest_visible() {
  WifiStore s;
  s.clear();
  s.save(-1, "Home", "password1");
  s.save(-1, "Office", "password2");
  const ScanEntry nets[] = {net("Neighbour", -40), net("Office", -70), net("Home", -55)};
  TEST_ASSERT_EQUAL(0, s.pickStrongest(nets, 3));  // Home -55 beats Office -70; Neighbour is not saved
  const ScanEntry far[] = {net("Neighbour", -40)};
  TEST_ASSERT_EQUAL(-1, s.pickStrongest(far, 1));
  TEST_ASSERT_EQUAL(-1, s.pickStrongest(nullptr, 0));
}

void test_store_encode_decode_round_trip() {
  WifiStore a;
  a.clear();
  a.seed("Home", "password1", "appass12");
  a.save(-1, "Office", "password2");
  uint8_t blob[WifiStore::kBlobSize];
  TEST_ASSERT_EQUAL(WifiStore::kBlobSize, a.encode(blob));
  WifiStore b;
  TEST_ASSERT_TRUE(b.decode(blob, sizeof(blob)));
  TEST_ASSERT_EQUAL_UINT8(2, b.count());
  TEST_ASSERT_EQUAL_STRING("Office", b.ssid(1));
  TEST_ASSERT_EQUAL_STRING("appass12", b.apPass());
}

void test_store_decode_rejects_corrupt() {  // Review Focus 2
  WifiStore a;
  a.clear();
  a.seed("Home", "password1", "appass12");
  uint8_t blob[WifiStore::kBlobSize];
  a.encode(blob);
  WifiStore b;
  TEST_ASSERT_FALSE(b.decode(blob, sizeof(blob) - 1));  // short
  TEST_ASSERT_EQUAL_UINT8(0, b.count());
  blob[0] = 99;  // unknown version
  TEST_ASSERT_FALSE(b.decode(blob, sizeof(blob)));
  a.encode(blob);
  memset(blob + 1, 'x', 33);  // ssid without a terminator
  TEST_ASSERT_FALSE(b.decode(blob, sizeof(blob)));
  TEST_ASSERT_EQUAL_UINT8(0, b.count());
}

void test_store_edit_in_use_flags_reconnect() {  // Review Focus 1: the caller compares before / after
  WifiStore s;
  s.clear();
  s.save(-1, "Home", "password1");
  const int in_use = s.slotOf("Home");
  s.save(-1, "Home", "password9");
  TEST_ASSERT_EQUAL(in_use, s.slotOf("Home"));
  TEST_ASSERT_EQUAL_STRING("password9", s.pass(in_use));  // changed: WifiManager reconnects (Task 5)
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_store_seed_and_read);
  RUN_TEST(test_store_save_new_edit_full);
  RUN_TEST(test_store_rejects_bad_input);
  RUN_TEST(test_store_forget_one_and_all);
  RUN_TEST(test_store_pick_strongest_visible);
  RUN_TEST(test_store_encode_decode_round_trip);
  RUN_TEST(test_store_decode_rejects_corrupt);
  RUN_TEST(test_store_edit_in_use_flags_reconnect);
  return UNITY_END();
}
```

  Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_wifi_store` → build FAIL (no header).

- [ ] **Step 2: Implement** — `src/core/wifi_store.h`:

```cpp
// Saved home networks + the own-AP password (Plan M, spec 02 §I1, bridge LLD R9.2). Pure C++: the bridge keeps the
// blob in NVS (net/wifi_manager.cpp). Passwords leave this class only to WiFi.begin / softAP - never to JSON or logs.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

struct ScanEntry {
  char ssid[33];
  int8_t rssi;
  bool secure;
};

enum class StoreResult : uint8_t { kOk, kBadSsid, kBadPassword, kFull, kBadSlot };

class WifiStore {
 public:
  static constexpr uint8_t kSlots = 3;
  static constexpr uint8_t kVersion = 1;
  static constexpr size_t kSsidSize = 33;
  static constexpr size_t kPassSize = 64;
  static constexpr size_t kBlobSize = 1 + kSlots * (kSsidSize + kPassSize) + kPassSize;

  void clear();
  void seed(const char* ssid, const char* pass, const char* ap_pass);
  StoreResult save(int slot, const char* ssid, const char* pass);
  StoreResult forget(int slot);
  StoreResult setApPassword(const char* pass);
  uint8_t count() const;
  const char* ssid(uint8_t slot) const { return slot < kSlots ? m_ssid[slot] : ""; }
  const char* pass(uint8_t slot) const { return slot < kSlots ? m_pass[slot] : ""; }
  const char* apPass() const { return m_ap_pass; }
  int pickStrongest(const ScanEntry* nets, size_t n) const;
  int slotOf(const char* ssid) const;
  size_t encode(uint8_t* out) const;
  bool decode(const uint8_t* in, size_t n);

 private:
  char m_ssid[kSlots][kSsidSize] = {};
  char m_pass[kSlots][kPassSize] = {};
  char m_ap_pass[kPassSize] = {};
};

}  // namespace core
}  // namespace cq
```

  `src/core/wifi_store.cpp`:

```cpp
#include "core/wifi_store.h"

#include <string.h>

namespace cq {
namespace core {
namespace {
bool validSsid(const char* s) {
  const size_t n = s ? strlen(s) : 0;
  return n >= 1 && n <= 32;
}
bool validPass(const char* p, bool allow_open) {
  const size_t n = p ? strlen(p) : 0;
  return (allow_open && n == 0) || (n >= 8 && n <= 63);
}
void copy(char* dst, size_t cap, const char* src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = '\0';
}
bool terminated(const uint8_t* p, size_t cap) { return memchr(p, '\0', cap) != nullptr; }
}  // namespace

void WifiStore::clear() {
  memset(m_ssid, 0, sizeof(m_ssid));
  memset(m_pass, 0, sizeof(m_pass));
  memset(m_ap_pass, 0, sizeof(m_ap_pass));
}

void WifiStore::seed(const char* ssid, const char* pass, const char* ap_pass) {
  clear();
  if (validSsid(ssid) && validPass(pass, true)) {
    copy(m_ssid[0], kSsidSize, ssid);
    copy(m_pass[0], kPassSize, pass);
  }
  if (validPass(ap_pass, false)) copy(m_ap_pass, kPassSize, ap_pass);
}

StoreResult WifiStore::save(int slot, const char* ssid, const char* pass) {
  if (!validSsid(ssid)) return StoreResult::kBadSsid;
  if (!validPass(pass, true)) return StoreResult::kBadPassword;
  if (slot < -1 || slot >= kSlots) return StoreResult::kBadSlot;
  if (slot == -1) {
    slot = slotOf(ssid);
    for (int i = 0; slot == -1 && i < kSlots; ++i) {
      if (m_ssid[i][0] == '\0') slot = i;
    }
    if (slot == -1) return StoreResult::kFull;
  }
  copy(m_ssid[slot], kSsidSize, ssid);
  copy(m_pass[slot], kPassSize, pass);
  return StoreResult::kOk;
}

StoreResult WifiStore::forget(int slot) {
  if (slot == -1) {
    memset(m_ssid, 0, sizeof(m_ssid));
    memset(m_pass, 0, sizeof(m_pass));
    return StoreResult::kOk;
  }
  if (slot < 0 || slot >= kSlots) return StoreResult::kBadSlot;
  memset(m_ssid[slot], 0, kSsidSize);
  memset(m_pass[slot], 0, kPassSize);
  return StoreResult::kOk;
}

StoreResult WifiStore::setApPassword(const char* pass) {
  if (!validPass(pass, false)) return StoreResult::kBadPassword;
  copy(m_ap_pass, kPassSize, pass);
  return StoreResult::kOk;
}

uint8_t WifiStore::count() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < kSlots; ++i) n = static_cast<uint8_t>(n + (m_ssid[i][0] != '\0'));
  return n;
}

int WifiStore::slotOf(const char* ssid) const {
  if (!ssid || ssid[0] == '\0') return -1;
  for (int i = 0; i < kSlots; ++i) {
    if (strcmp(m_ssid[i], ssid) == 0) return i;
  }
  return -1;
}

int WifiStore::pickStrongest(const ScanEntry* nets, size_t n) const {
  int best = -1;
  int best_rssi = -1000;
  for (size_t i = 0; nets && i < n; ++i) {
    const int slot = slotOf(nets[i].ssid);
    if (slot >= 0 && nets[i].rssi > best_rssi) {
      best = slot;
      best_rssi = nets[i].rssi;
    }
  }
  return best;
}

size_t WifiStore::encode(uint8_t* out) const {
  uint8_t* p = out;
  *p++ = kVersion;
  for (uint8_t i = 0; i < kSlots; ++i) {
    memcpy(p, m_ssid[i], kSsidSize);
    p += kSsidSize;
    memcpy(p, m_pass[i], kPassSize);
    p += kPassSize;
  }
  memcpy(p, m_ap_pass, kPassSize);
  return kBlobSize;
}

bool WifiStore::decode(const uint8_t* in, size_t n) {
  clear();
  if (!in || n != kBlobSize || in[0] != kVersion) return false;
  const uint8_t* p = in + 1;
  for (uint8_t i = 0; i < kSlots; ++i) {
    if (!terminated(p, kSsidSize) || !terminated(p + kSsidSize, kPassSize)) {
      clear();
      return false;
    }
    p += kSsidSize + kPassSize;
  }
  if (!terminated(p, kPassSize)) return false;
  p = in + 1;
  for (uint8_t i = 0; i < kSlots; ++i) {
    memcpy(m_ssid[i], p, kSsidSize);
    p += kSsidSize;
    memcpy(m_pass[i], p, kPassSize);
    p += kPassSize;
  }
  memcpy(m_ap_pass, p, kPassSize);
  return true;
}

}  // namespace core
}  // namespace cq
```

- [ ] **Step 3: Run** — `pio test -e native` → all PASS (8 new).
- [ ] **Step 4: Commit** — `feat(bridge): WifiStore - up to 3 saved networks + AP password, NVS blob (plan M)`.

---

### Task 2: Confirmation gate, double press, button router

**Files:** Create `src/core/confirm.h`, `src/core/confirm.cpp`; Test `test/test_confirm/test_main.cpp`.

**Interfaces — Produces:**
```cpp
namespace cq::core {
enum class ChangeKind : uint8_t { kNone, kWifiSave, kWifiForget, kWifiMode, kApPassword, kCalibReset, kSetting };
struct PendingChange { ChangeKind kind = ChangeKind::kNone; int fd = -1; int8_t slot = -1; bool ap = false;
  char ssid[33] = ""; char pass[64] = ""; uint8_t setting_id = 0; int32_t value = 0; uint8_t calib_what = 0; };
const char* changeName(ChangeKind k);  // "wifi_save", "wifi_forget", "wifi_mode", "ap_password", "calib", "setting"
class ConfirmGate { public: static constexpr uint32_t kWindowMs = 30000;
  void begin(const PendingChange& c, uint32_t now_ms); bool waiting() const; const PendingChange& pending() const;
  bool take(PendingChange& out); bool expired(uint32_t now_ms, PendingChange& out); void dropClient(int fd); };
class DoublePress { public: static constexpr uint32_t kGapMs = 1000; bool onPress(uint32_t now_ms); void reset(); };
enum class ButtonAction : uint8_t { kNone, kConfirm, kToggleWifi };
class ButtonRouter { public: explicit ButtonRouter(ConfirmGate& g); ButtonAction onPress(uint32_t now_ms); };
}
```

- [ ] **Step 1: Failing tests** — `test/test_confirm/test_main.cpp`:

```cpp
// Plan M (bridge LLD R9.3): confirmation by the tank's button, double press.
#include <string.h>
#include <unity.h>

#include "core/confirm.h"

using namespace cq::core;

void setUp() {}
void tearDown() {}

namespace {
PendingChange change(ChangeKind k, int fd) {
  PendingChange c;
  c.kind = k;
  c.fd = fd;
  return c;
}
int A(ButtonAction a) { return static_cast<int>(a); }
}  // namespace

void test_gate_confirm_then_empty() {
  ConfirmGate g;
  g.begin(change(ChangeKind::kWifiSave, 7), 0);
  TEST_ASSERT_TRUE(g.waiting());
  PendingChange out;
  TEST_ASSERT_TRUE(g.take(out));
  TEST_ASSERT_EQUAL(static_cast<int>(ChangeKind::kWifiSave), static_cast<int>(out.kind));
  TEST_ASSERT_EQUAL_INT(7, out.fd);
  TEST_ASSERT_FALSE(g.waiting());
  TEST_ASSERT_FALSE(g.take(out));
}

void test_gate_expires_once() {
  ConfirmGate g;
  g.begin(change(ChangeKind::kWifiMode, 3), 1000);
  PendingChange out;
  TEST_ASSERT_FALSE(g.expired(1000 + ConfirmGate::kWindowMs - 1, out));
  TEST_ASSERT_TRUE(g.expired(1000 + ConfirmGate::kWindowMs, out));
  TEST_ASSERT_EQUAL_INT(3, out.fd);
  TEST_ASSERT_FALSE(g.expired(1000 + ConfirmGate::kWindowMs + 5000, out));
  TEST_ASSERT_FALSE(g.waiting());
}

void test_gate_new_change_replaces() {
  ConfirmGate g;
  g.begin(change(ChangeKind::kWifiSave, 3), 0);
  g.begin(change(ChangeKind::kApPassword, 4), 100);
  PendingChange out;
  TEST_ASSERT_TRUE(g.take(out));
  TEST_ASSERT_EQUAL(static_cast<int>(ChangeKind::kApPassword), static_cast<int>(out.kind));
  TEST_ASSERT_EQUAL_INT(4, out.fd);
}

void test_gate_drop_client() {  // Review Focus 4
  ConfirmGate g;
  g.begin(change(ChangeKind::kWifiForget, 9), 0);
  g.dropClient(8);  // someone else
  TEST_ASSERT_TRUE(g.waiting());
  g.dropClient(9);
  TEST_ASSERT_FALSE(g.waiting());
}

void test_double_press_gap() {
  DoublePress d;
  TEST_ASSERT_FALSE(d.onPress(0));
  TEST_ASSERT_TRUE(d.onPress(DoublePress::kGapMs));  // second within 1 s
  TEST_ASSERT_FALSE(d.onPress(5000));
  TEST_ASSERT_FALSE(d.onPress(5000 + DoublePress::kGapMs + 1));  // too slow: this one starts afresh
  TEST_ASSERT_TRUE(d.onPress(5000 + DoublePress::kGapMs + 400));
}

void test_double_press_three_presses_one_toggle() {  // Review Focus 3
  DoublePress d;
  TEST_ASSERT_FALSE(d.onPress(0));
  TEST_ASSERT_TRUE(d.onPress(300));
  TEST_ASSERT_FALSE(d.onPress(600));  // a third press starts a new double, never a second toggle
  TEST_ASSERT_TRUE(d.onPress(900));
}

void test_router_confirms_while_waiting_and_never_doubles() {
  ConfirmGate g;
  ButtonRouter r(g);
  g.begin(change(ChangeKind::kWifiSave, 1), 0);
  TEST_ASSERT_EQUAL(A(ButtonAction::kConfirm), A(r.onPress(100)));  // confirms (the caller takes the change)
  PendingChange out;
  g.take(out);
  TEST_ASSERT_EQUAL(A(ButtonAction::kNone), A(r.onPress(400)));      // the confirming press never counts
  TEST_ASSERT_EQUAL(A(ButtonAction::kToggleWifi), A(r.onPress(700)));  // 400 + 700: a fresh double
}

void test_change_names() {
  TEST_ASSERT_EQUAL_STRING("wifi_save", changeName(ChangeKind::kWifiSave));
  TEST_ASSERT_EQUAL_STRING("wifi_mode", changeName(ChangeKind::kWifiMode));
  TEST_ASSERT_EQUAL_STRING("setting", changeName(ChangeKind::kSetting));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_gate_confirm_then_empty);
  RUN_TEST(test_gate_expires_once);
  RUN_TEST(test_gate_new_change_replaces);
  RUN_TEST(test_gate_drop_client);
  RUN_TEST(test_double_press_gap);
  RUN_TEST(test_double_press_three_presses_one_toggle);
  RUN_TEST(test_router_confirms_while_waiting_and_never_doubles);
  RUN_TEST(test_change_names);
  return UNITY_END();
}
```

  Run → build FAIL.

- [ ] **Step 2: Implement** — `src/core/confirm.h`:

```cpp
// Physical confirmation and the WiFi double press (Plan M, spec 02 §I1 / §I2, bridge LLD R9.3). Pure C++.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

enum class ChangeKind : uint8_t { kNone, kWifiSave, kWifiForget, kWifiMode, kApPassword, kCalibReset, kSetting };

struct PendingChange {
  ChangeKind kind = ChangeKind::kNone;
  int fd = -1;          // the client that asked
  int32_t id = -1;      // its command id (ack / confirmed)
  int8_t slot = -1;     // wifi_save / wifi_forget (-1 = first free / all)
  bool ap = false;      // wifi_mode: true = own AP
  char ssid[33] = "";
  char pass[64] = "";   // wifi_save / ap_password - never echoed
  uint8_t setting_id = 0;
  int32_t value = 0;
  uint8_t calib_what = 0;  // 0 counts_per_m, 1 roll
};

const char* changeName(ChangeKind k);

class ConfirmGate {
 public:
  static constexpr uint32_t kWindowMs = 30000;
  void begin(const PendingChange& c, uint32_t now_ms) {
    m_change = c;
    m_since = now_ms;
    m_waiting = true;
  }
  bool waiting() const { return m_waiting; }
  const PendingChange& pending() const { return m_change; }
  bool take(PendingChange& out);
  bool expired(uint32_t now_ms, PendingChange& out);
  void dropClient(int fd) {
    if (m_waiting && m_change.fd == fd) m_waiting = false;
  }

 private:
  PendingChange m_change;
  uint32_t m_since = 0;
  bool m_waiting = false;
};

class DoublePress {
 public:
  static constexpr uint32_t kGapMs = 1000;
  bool onPress(uint32_t now_ms);  // true on the second press within kGapMs (then starts afresh)
  void reset() { m_armed = false; }

 private:
  bool m_armed = false;
  uint32_t m_first_ms = 0;
};

enum class ButtonAction : uint8_t { kNone, kConfirm, kToggleWifi };

class ButtonRouter {
 public:
  explicit ButtonRouter(ConfirmGate& gate) : m_gate(gate) {}
  ButtonAction onPress(uint32_t now_ms);

 private:
  ConfirmGate& m_gate;
  DoublePress m_double;
};

}  // namespace core
}  // namespace cq
```

  `src/core/confirm.cpp`:

```cpp
#include "core/confirm.h"

namespace cq {
namespace core {

const char* changeName(ChangeKind k) {
  switch (k) {
    case ChangeKind::kWifiSave: return "wifi_save";
    case ChangeKind::kWifiForget: return "wifi_forget";
    case ChangeKind::kWifiMode: return "wifi_mode";
    case ChangeKind::kApPassword: return "ap_password";
    case ChangeKind::kCalibReset: return "calib";
    case ChangeKind::kSetting: return "setting";
    case ChangeKind::kNone:
    default: return "none";
  }
}

bool ConfirmGate::take(PendingChange& out) {
  if (!m_waiting) return false;
  out = m_change;
  m_waiting = false;
  return true;
}

bool ConfirmGate::expired(uint32_t now_ms, PendingChange& out) {
  if (!m_waiting || now_ms - m_since < kWindowMs) return false;
  out = m_change;
  m_waiting = false;
  return true;
}

bool DoublePress::onPress(uint32_t now_ms) {
  if (m_armed && now_ms - m_first_ms <= kGapMs) {
    m_armed = false;
    return true;
  }
  m_armed = true;
  m_first_ms = now_ms;
  return false;
}

ButtonAction ButtonRouter::onPress(uint32_t now_ms) {
  if (m_gate.waiting()) {
    m_double.reset();  // a confirming press never counts towards a double press
    return ButtonAction::kConfirm;
  }
  return m_double.onPress(now_ms) ? ButtonAction::kToggleWifi : ButtonAction::kNone;
}

}  // namespace core
}  // namespace cq
```

  Note: in `test_router_confirms_while_waiting_and_never_doubles` the press at 400 arms the double press (returns kNone) and 700 completes it — as the test says.

- [ ] **Step 3: Run** — `pio test -e native` → all PASS.
- [ ] **Step 4: Commit** — `feat(bridge): button confirmation gate and WiFi double press (plan M)`.

---

### Task 3: Forced AP in `WifiPolicy`; `AckedOps` queue

**Files:** Modify `src/core/wifi_policy.h`; Create `src/core/acked_ops.h`, `src/core/acked_ops.cpp`; Test `test/test_wifi_policy/test_main.cpp`, `test/test_confirm/test_main.cpp` (AckedOps tests live there to avoid a new suite).

**Interfaces — Produces:** `WifiMode::kForcedAp`; `WifiAction WifiPolicy::forceAp(uint32_t now_ms)` (→ `kStartAp`, mode kForcedAp, no retries, the station stays off), `WifiAction WifiPolicy::forceHome(uint32_t now_ms)` (→ `kStartStation` via begin, `kApOff` semantics handled by the caller). `struct AckedOp { enum Kind : uint8_t { kSound, kLed, kSettingGet, kSettingSet, kSettingsSave } kind; uint8_t a; int32_t value; uint8_t r, g, b; uint16_t period_ms, duration_ms; int fd; int32_t id; };` `class AckedOps { bool push(const AckedOp&); bool front(AckedOp&) const; void pop(); bool empty() const; bool busy() const; void markSent(uint8_t seq, uint32_t now); bool isCurrent(uint8_t seq) const; void done(); }` — capacity 8.

- [ ] **Step 1: Failing tests** — `test_wifi_policy` gains:

```cpp
// Plan M (bridge LLD R9.3): forced AP by double press / confirmed wifi_mode.
void test_forced_ap_has_no_retries_and_home_restarts() {
  WifiPolicy p;
  p.begin(0);
  p.step(2000, true, false);  // station up
  TEST_ASSERT_EQUAL(static_cast<int>(WifiAction::kStartAp), static_cast<int>(p.forceAp(3000)));
  TEST_ASSERT_EQUAL(static_cast<int>(WifiMode::kForcedAp), static_cast<int>(p.mode()));
  for (uint32_t t = 3000; t < 300000; t += 1000) {
    TEST_ASSERT_EQUAL(static_cast<int>(WifiAction::kNone), static_cast<int>(p.step(t, false, false)));
  }
  TEST_ASSERT_EQUAL(static_cast<int>(WifiAction::kStartStation), static_cast<int>(p.forceHome(300000)));
  TEST_ASSERT_EQUAL(static_cast<int>(WifiMode::kConnecting), static_cast<int>(p.mode()));
  TEST_ASSERT_EQUAL(static_cast<int>(WifiAction::kStationUp), static_cast<int>(p.step(302000, true, false)));
}
```

  `test_confirm` gains:

```cpp
#include "core/acked_ops.h"

void test_acked_ops_one_at_a_time() {
  AckedOps q;
  AckedOp a{};
  a.kind = AckedOp::kSound;
  AckedOp b{};
  b.kind = AckedOp::kLed;
  TEST_ASSERT_TRUE(q.push(a));
  TEST_ASSERT_TRUE(q.push(b));
  AckedOp f{};
  TEST_ASSERT_TRUE(q.front(f));
  TEST_ASSERT_EQUAL(AckedOp::kSound, f.kind);
  TEST_ASSERT_FALSE(q.busy());
  q.markSent(42, 0);
  TEST_ASSERT_TRUE(q.busy());
  TEST_ASSERT_TRUE(q.isCurrent(42));
  TEST_ASSERT_FALSE(q.isCurrent(43));
  q.done();  // ACK (or timeout) for 42: next op
  TEST_ASSERT_FALSE(q.busy());
  TEST_ASSERT_TRUE(q.front(f));
  TEST_ASSERT_EQUAL(AckedOp::kLed, f.kind);
  q.markSent(43, 10);
  q.done();
  TEST_ASSERT_TRUE(q.empty());
  for (int i = 0; i < 8; ++i) TEST_ASSERT_TRUE(q.push(a));
  TEST_ASSERT_FALSE(q.push(a));  // full
}
```

  (+ `RUN_TEST`s). Run → build FAIL.

- [ ] **Step 2: Implement**
  - `wifi_policy.h`: `enum class WifiMode : uint8_t { kConnecting, kStation, kFallbackAp, kForcedAp };` and public

```cpp
  WifiAction forceAp(uint32_t now_ms) {  // Plan M: double press / confirmed wifi_mode ap - no station retries
    m_mode = WifiMode::kForcedAp;
    m_since_ms = now_ms;
    return WifiAction::kStartAp;
  }
  WifiAction forceHome(uint32_t now_ms) { return begin(now_ms); }  // as boot: scan, strongest saved, 10 s
```

    and in `step`, before the `kFallbackAp` default: `case WifiMode::kForcedAp: return WifiAction::kNone;`. Update the header comment ("… forced AP (Plan M): stays until switched back").
  - `src/core/acked_ops.h`:

```cpp
// One acked Uno command at a time (Plan M, bridge LLD R9.2): SOUND / LED feedback and SETTING_* for the settings page
// share BodyLink's single acked slot with the drive page's commands. Pure C++.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

struct AckedOp {
  enum Kind : uint8_t { kSound, kLed, kSettingGet, kSettingSet, kSettingsSave };
  Kind kind = kSound;
  uint8_t a = 0;      // sound id / setting id / led mode
  int32_t value = 0;  // setting value
  uint8_t r = 0, g = 0, b = 0;
  uint16_t period_ms = 0, duration_ms = 0;
  int fd = -1;        // client to answer (settings), -1 = none
  int32_t id = -1;
};

class AckedOps {
 public:
  static constexpr uint8_t kCap = 8;
  bool push(const AckedOp& op);
  bool front(AckedOp& out) const;
  bool empty() const { return m_n == 0; }
  bool busy() const { return m_sent; }
  void markSent(uint8_t seq, uint32_t now_ms) {
    m_seq = seq;
    m_sent = true;
    m_sent_ms = now_ms;
  }
  bool isCurrent(uint8_t seq) const { return m_sent && seq == m_seq; }
  void done();  // the current op answered (ACK / timeout): drop it, next may go

 private:
  AckedOp m_ops[kCap];
  uint8_t m_head = 0;
  uint8_t m_n = 0;
  bool m_sent = false;
  uint8_t m_seq = 0;
  uint32_t m_sent_ms = 0;
};

}  // namespace core
}  // namespace cq
```

    `src/core/acked_ops.cpp`:

```cpp
#include "core/acked_ops.h"

namespace cq {
namespace core {

bool AckedOps::push(const AckedOp& op) {
  if (m_n >= kCap) return false;
  m_ops[(m_head + m_n) % kCap] = op;
  ++m_n;
  return true;
}

bool AckedOps::front(AckedOp& out) const {
  if (m_n == 0) return false;
  out = m_ops[m_head];
  return true;
}

void AckedOps::done() {
  m_sent = false;
  if (m_n == 0) return;
  m_head = static_cast<uint8_t>((m_head + 1) % kCap);
  --m_n;
}

}  // namespace core
}  // namespace cq
```

- [ ] **Step 3: Run** — `pio test -e native` → all PASS (the existing WiFi policy tests unchanged).
- [ ] **Step 4: Commit** — `feat(bridge): forced AP mode and a one-at-a-time acked-op queue (plan M)`.

---

### Task 4: WebSocket commands and replies

**Files:** Modify `src/core/ws_codec.h/.cpp`, `src/core/ws_json.h/.cpp`; Test `test/test_ws_codec`, `test/test_ws_json`.

**Interfaces — Produces:**
- `WsCmdKind`: `kWifiStatus`, `kWifiScan`, `kWifiSave`, `kWifiForget`, `kWifiMode`, `kApPassword`, `kSetting`, `kRestart` (before `kUnsupported`); `calib` gains `"reset":"counts_per_m"|"roll"`.
- `WsCommand` fields: `char ssid[33]`, `char password[64]`, `int8_t slot = -1` (JSON 1…3 → 0…2), `bool has_slot`, `bool wifi_ap` (mode "ap"), `uint8_t setting_id`, `bool has_value`, `int32_t value`, `int8_t calib_reset = -1` (0 counts_per_m, 1 roll).
- JSON: `struct WifiStatusView { const char* mode; const char* ssid; int rssi; const char* ip; const char* host; const char* saved[3]; uint8_t n_saved; const char* ap_ssid; };` `size_t jsonWifiStatus(char*, size_t, const WifiStatusView&)`; `size_t jsonWifiScan(char*, size_t, const ScanEntry*, size_t n)`; `size_t jsonConfirm(char*, size_t, const char* what, uint8_t secs)`; `size_t jsonConfirmed(char*, size_t, const char* what)`; `size_t jsonConfirmExpired(char*, size_t, const char* what)`; `size_t jsonSetting(char*, size_t, uint8_t id, int32_t value)`.

- [ ] **Step 1: Failing tests** — `test_ws_codec`:

```cpp
// Plan M (bridge LLD R9 / spec 02 §I3).
void test_wifi_commands() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiStatus), K(P("{\"t\":\"wifi_status\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiScan), K(P("{\"t\":\"wifi_scan\"}").kind));
  WsCommand c = P("{\"t\":\"wifi_save\",\"slot\":2,\"ssid\":\"Office\",\"password\":\"secret123\",\"id\":4}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiSave), K(c.kind));
  TEST_ASSERT_EQUAL_INT8(1, c.slot);  // JSON 1..3 -> 0..2
  TEST_ASSERT_EQUAL_STRING("Office", c.ssid);
  TEST_ASSERT_EQUAL_STRING("secret123", c.password);
  c = P("{\"t\":\"wifi_save\",\"ssid\":\"Open\",\"password\":\"\"}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiSave), K(c.kind));
  TEST_ASSERT_EQUAL_INT8(-1, c.slot);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"wifi_save\",\"password\":\"x\"}").kind));  // no ssid
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"wifi_save\",\"ssid\":\"A\",\"slot\":4,\"password\":\"\"}").kind));
  c = P("{\"t\":\"wifi_forget\",\"slot\":1}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kWifiForget), K(c.kind));
  TEST_ASSERT_EQUAL_INT8(0, c.slot);
  TEST_ASSERT_EQUAL_INT8(-1, P("{\"t\":\"wifi_forget\"}").slot);  // all
  TEST_ASSERT_TRUE(P("{\"t\":\"wifi_mode\",\"mode\":\"ap\"}").wifi_ap);
  TEST_ASSERT_FALSE(P("{\"t\":\"wifi_mode\",\"mode\":\"home\"}").wifi_ap);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"wifi_mode\",\"mode\":\"x\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kApPassword), K(P("{\"t\":\"ap_password\",\"password\":\"abcdefgh\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kRestart), K(P("{\"t\":\"restart\"}").kind));
}

void test_setting_and_calib_reset() {
  WsCommand c = P("{\"t\":\"setting\",\"setting_id\":66}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSetting), K(c.kind));
  TEST_ASSERT_EQUAL_UINT8(66, c.setting_id);
  TEST_ASSERT_FALSE(c.has_value);
  c = P("{\"t\":\"setting\",\"setting_id\":66,\"value\":-12}");
  TEST_ASSERT_TRUE(c.has_value);
  TEST_ASSERT_EQUAL_INT32(-12, c.value);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"setting\"}").kind));
  TEST_ASSERT_EQUAL_INT8(0, P("{\"t\":\"calib\",\"reset\":\"counts_per_m\"}").calib_reset);
  TEST_ASSERT_EQUAL_INT8(1, P("{\"t\":\"calib\",\"reset\":\"roll\"}").calib_reset);
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"calib\",\"reset\":\"x\"}").kind));
}
```

  `test_ws_json`:

```cpp
// Plan M: replies (no password ever).
void test_wifi_status_and_scan() {
  WifiStatusView v{"home", "MyHome", -54, "192.168.31.167", "conqueror-bridge.local", {"MyHome", "Office", ""}, 2,
                   "conqueror-bridge"};
  jsonWifiStatus(buf, sizeof(buf), v);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"wifi_status\",\"mode\":\"home\",\"ssid\":\"MyHome\",\"rssi\":-54,\"ip\":\"192.168.31.167\","
      "\"host\":\"conqueror-bridge.local\",\"saved\":[\"MyHome\",\"Office\"],\"ap_ssid\":\"conqueror-bridge\"}",
      buf);
  ScanEntry nets[2] = {};
  strcpy(nets[0].ssid, "Office-5G");
  nets[0].rssi = -58;
  nets[0].secure = true;
  strcpy(nets[1].ssid, "Cafe");
  nets[1].rssi = -80;
  nets[1].secure = false;
  jsonWifiScan(buf, sizeof(buf), nets, 2);
  TEST_ASSERT_EQUAL_STRING(
      "{\"t\":\"wifi_scan\",\"nets\":[{\"ssid\":\"Office-5G\",\"rssi\":-58,\"secure\":true},"
      "{\"ssid\":\"Cafe\",\"rssi\":-80,\"secure\":false}]}",
      buf);
}

void test_wifi_status_escapes_ssid() {  // Review Focus 5
  WifiStatusView v{"home", "Bob's \"Net\"\\", -60, "1.2.3.4", "h", {"Bob's \"Net\"\\", "", ""}, 1, "ap"};
  jsonWifiStatus(buf, sizeof(buf), v);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"ssid\":\"Bob's \\\"Net\\\"\\\\\""));
}

void test_confirm_replies_and_setting() {
  jsonConfirm(buf, sizeof(buf), "wifi_save", 30);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"confirm\",\"what\":\"wifi_save\",\"secs\":30}", buf);
  jsonConfirmed(buf, sizeof(buf), "wifi_mode");
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"confirmed\",\"what\":\"wifi_mode\"}", buf);
  jsonConfirmExpired(buf, sizeof(buf), "ap_password");
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"confirm_expired\",\"what\":\"ap_password\"}", buf);
  jsonSetting(buf, sizeof(buf), 66, -12);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"setting\",\"id\":66,\"value\":-12}", buf);
}
```

  (+ RUN_TESTs; `#include "core/wifi_store.h"` in `test_ws_json`). Run → build FAIL.

- [ ] **Step 2: Implement**
  - `ws_codec.h`: the 8 kinds before `kUnsupported` (one-line comments `// Plan M (spec 02 §I3)`), and in `WsCommand`:

```cpp
  char ssid[33] = "";      // wifi_save (Plan M)
  char password[64] = "";  // wifi_save / ap_password - never logged or echoed
  int8_t slot = -1;        // wifi_save / wifi_forget: 0..2, -1 = first free / all
  bool wifi_ap = false;    // wifi_mode: true = own AP
  uint8_t setting_id = 0;  // setting
  bool has_value = false;
  int32_t value = 0;
  int8_t calib_reset = -1;  // calib: 0 counts_per_m, 1 roll
```

  - `ws_codec.cpp`: `kindOf` adds `"wifi_status"`, `"wifi_scan"`, `"wifi_save"`, `"wifi_forget"`, `"wifi_mode"`, `"ap_password"`, `"setting"`, `"restart"`. `Seen` gains `bool ssid = false, password = false, setting_id = false; char reset[16] = "";` (mode already exists: `char mode[8]` — `"home"` / `"ap"` fit). `readField`, before `return r.skip();`:

```cpp
  if (strcmp(key, "ssid") == 0) {  // Plan M: 1..32 bytes (checked again by WifiStore)
    s.ssid = r.str(c.ssid, sizeof(c.ssid));
    return s.ssid;
  }
  if (strcmp(key, "password") == 0) {
    s.password = r.str(c.password, sizeof(c.password));
    return s.password;
  }
  if (strcmp(key, "slot") == 0) {  // JSON 1..3
    if (!r.num(num, nullptr, 0) || !isWhole(num) || num < 1 || num > 3) return false;
    c.slot = static_cast<int8_t>(num - 1);
    return true;
  }
  if (strcmp(key, "value") == 0) {  // setting
    if (!r.num(num, nullptr, 0) || !isWhole(num) || num < -2147483648.0 || num > 2147483647.0) return false;
    c.value = static_cast<int32_t>(num);
    c.has_value = true;
    return true;
  }
  if (strcmp(key, "reset") == 0) return r.str(s.reset, sizeof(s.reset));
```

    The `"id"` key is already the command id, so the Uno setting is named by **`"setting_id"`** (spec §I3 / LLD R9.3 say so; the reply `{"t":"setting","id":66,"value":v}` uses `id` for the setting). `readField`: `if (strcmp(key, "setting_id") == 0) { if (!r.num(num,nullptr,0) || !isWhole(num) || num < 0 || num > 255) return false; c.setting_id = static_cast<uint8_t>(num); s.setting_id = true; return true; }`.

    `complete`:

```cpp
    case WsCmdKind::kWifiSave: return s.ssid && s.password;
    case WsCmdKind::kWifiForget: return true;
    case WsCmdKind::kWifiMode:
      c.wifi_ap = strcmp(s.mode, "ap") == 0;
      return c.wifi_ap || strcmp(s.mode, "home") == 0;
    case WsCmdKind::kApPassword: return s.password;
    case WsCmdKind::kSetting: return s.setting_id;
    case WsCmdKind::kCalib:
      if (s.reset[0] == '\0') return true;
      c.calib_reset = strcmp(s.reset, "counts_per_m") == 0 ? 0 : (strcmp(s.reset, "roll") == 0 ? 1 : -1);
      return c.calib_reset >= 0;
    case WsCmdKind::kWifiStatus:
    case WsCmdKind::kWifiScan:
    case WsCmdKind::kRestart: return true;
```

    (replace the existing `case WsCmdKind::kCalib: return true;`). Check `key` buffer size (`char key[16]` in `parseWsCommand`): `"setting_id"` (10) and `"password"` fit.
  - `ws_json.h` / `.cpp`: `#include "core/wifi_store.h"`; a local `escaped(const char* s)` helper writing into a static `char esc[2][80]` ring (two strings per call) via `jsonEscape`; implement:

```cpp
size_t jsonWifiStatus(char* out, size_t cap, const WifiStatusView& v) {
  char ssid[72], saved[3 * 72 + 8] = "";
  jsonEscape(ssid, sizeof(ssid), v.ssid ? v.ssid : "");
  size_t n = 0;
  for (uint8_t i = 0; i < v.n_saved && i < 3; ++i) {
    char e[72];
    jsonEscape(e, sizeof(e), v.saved[i]);
    const int k = snprintf(saved + n, sizeof(saved) - n, "%s\"%s\"", n ? "," : "", e);
    if (k > 0 && static_cast<size_t>(k) < sizeof(saved) - n) n += static_cast<size_t>(k);
  }
  return fmt(out, cap,
             "{\"t\":\"wifi_status\",\"mode\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"host\":\"%s\","
             "\"saved\":[%s],\"ap_ssid\":\"%s\"}",
             v.mode, ssid, v.rssi, v.ip, v.host, saved, v.ap_ssid);
}

size_t jsonWifiScan(char* out, size_t cap, const ScanEntry* nets, size_t count) {
  char list[320] = "";
  size_t n = 0;
  for (size_t i = 0; i < count && i < 12; ++i) {
    char e[72];
    jsonEscape(e, sizeof(e), nets[i].ssid);
    const int k = snprintf(list + n, sizeof(list) - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}", n ? "," : "",
                           e, nets[i].rssi, nets[i].secure ? "true" : "false");
    if (k <= 0 || static_cast<size_t>(k) >= sizeof(list) - n) break;  // keep what fits (kWsJsonMax)
    n += static_cast<size_t>(k);
  }
  return fmt(out, cap, "{\"t\":\"wifi_scan\",\"nets\":[%s]}", list);
}

size_t jsonConfirm(char* out, size_t cap, const char* what, uint8_t secs) {
  return fmt(out, cap, "{\"t\":\"confirm\",\"what\":\"%s\",\"secs\":%u}", what, static_cast<unsigned>(secs));
}
size_t jsonConfirmed(char* out, size_t cap, const char* what) {
  return fmt(out, cap, "{\"t\":\"confirmed\",\"what\":\"%s\"}", what);
}
size_t jsonConfirmExpired(char* out, size_t cap, const char* what) {
  return fmt(out, cap, "{\"t\":\"confirm_expired\",\"what\":\"%s\"}", what);
}
size_t jsonSetting(char* out, size_t cap, uint8_t id, int32_t value) {
  return fmt(out, cap, "{\"t\":\"setting\",\"id\":%u,\"value\":%ld}", static_cast<unsigned>(id),
             static_cast<long>(value));
}
```

    (the `escaped` helper is not needed with these local buffers — skip it). Check `kWsJsonMax` (384): a scan list beyond it is trimmed by the `list` buffer (320) — fine.
- [ ] **Step 3: Run** — `pio test -e native` → all PASS.
- [ ] **Step 4: Commit** — `feat(bridge): WebSocket commands and replies for WiFi, settings, confirmation (plan M)`.

---

### Task 5: `BodyLink` settings / LED senders and the `SETTING_VALUE` event

**Files:** Modify `src/core/body_link.h/.cpp`; Test `test/test_body_link/test_main.cpp`.

**Interfaces — Produces:** `bool sendLed(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms, uint16_t duration_ms, uint32_t now_ms, uint8_t& seq_out)`; `bool sendSettingGet(uint8_t id, uint32_t now_ms, uint8_t& seq_out)`; `bool sendSettingSet(uint8_t id, int32_t value, uint32_t now_ms, uint8_t& seq_out)`; `bool sendSettingsSave(uint32_t now_ms, uint8_t& seq_out)`; `bool ackPending() const`; `LinkEventKind::kSettingValue` (code = id, `a`/`b` = value low / high 16 bits).

- [ ] **Step 1: Failing test** — in `test_body_link` (use the file's existing rig: look at `test_handshake_acks_matching_proto_and_sends_owner_hint` for how frames are fed and the TX captured; copy its helpers):

```cpp
// Plan M: settings / LED through the acked slot; SETTING_VALUE becomes an event.
void test_setting_frames_and_value_event() {
  Rig r;            // the file's synced rig (same as the SOUND test) - adapt the name to the file
  r.sync();
  uint8_t seq = 0;
  TEST_ASSERT_TRUE(r.link.sendSettingGet(66, 0, seq));
  TEST_ASSERT_TRUE(r.link.ackPending());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(proto::FrameType::kSettingGet), r.lastTxType());
  r.ack(seq, proto::AckStatus::kOk);
  TEST_ASSERT_FALSE(r.link.ackPending());
  r.feed(proto::SettingValueMsg{66, -12});
  LinkEvent ev{};
  bool found = false;
  while (r.link.popEvent(ev)) {
    if (ev.kind == LinkEventKind::kSettingValue) {
      found = true;
      TEST_ASSERT_EQUAL_UINT8(66, ev.code);
      const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint16_t>(ev.b)) << 16) |
                                             static_cast<uint16_t>(ev.a));
      TEST_ASSERT_EQUAL_INT32(-12, v);
    }
  }
  TEST_ASSERT_TRUE(found);
  TEST_ASSERT_TRUE(r.link.sendLed(static_cast<uint8_t>(proto::LedMode::kBlink), 0, 0, 255, 200, 3000, 10, seq));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(proto::FrameType::kLed), r.lastTxType());
}
```

  The executor reads the existing `test_body_link` helpers first and maps `Rig`, `sync`, `lastTxType`, `ack`, `feed` onto them (add small helpers if missing, in the test file only). Run → build FAIL.

- [ ] **Step 2: Implement** — `body_link.h`: `kSettingValue` in `LinkEventKind` (after `kAckResult`, comment `// Plan M: SETTING_VALUE: code = id, a / b = value low / high 16 bits`); the four senders declared next to `sendSound`; `bool ackPending() const { return m_cam_pending; }`. `body_link.cpp`:

```cpp
bool BodyLink::sendLed(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms, uint16_t duration_ms,
                       uint32_t now_ms, uint8_t& seq_out) {
  return sendAcked(proto::LedMsg{mode, r, g, b, period_ms, duration_ms}, now_ms, seq_out);
}
bool BodyLink::sendSettingGet(uint8_t id, uint32_t now_ms, uint8_t& seq_out) {
  return sendAcked(proto::SettingGetMsg{id}, now_ms, seq_out);
}
bool BodyLink::sendSettingSet(uint8_t id, int32_t value, uint32_t now_ms, uint8_t& seq_out) {
  return sendAcked(proto::SettingSetMsg{id, value}, now_ms, seq_out);
}
bool BodyLink::sendSettingsSave(uint32_t now_ms, uint8_t& seq_out) {
  return sendAcked(proto::SettingsSaveMsg{}, now_ms, seq_out);
}
```

  and in `onFrame` a case:

```cpp
    case proto::FrameType::kSettingValue: {  // Plan M: the settings page asked (SETTING_GET)
      proto::SettingValueMsg m{};
      if (proto::decode(f.payload, f.len, m)) {
        const uint32_t u = static_cast<uint32_t>(m.value);
        pushEvent({LinkEventKind::kSettingValue, m.id, f.seq, 0, static_cast<int16_t>(u & 0xFFFF),
                   static_cast<int16_t>(u >> 16)});
      }
      break;
    }
```

  (match the `LinkEvent` field order of the struct: check `struct LinkEvent` — kind, code, seq, status, a, b — and adapt). Remove `SETTING_VALUE` from the "only the bench uses them" comment in the default case.
- [ ] **Step 3: Run** — `pio test -e native` → all PASS.
- [ ] **Step 4: Commit** — `feat(bridge): BodyLink LED and SETTING_* senders, SETTING_VALUE event (plan M)`.

---

### Task 6: `WifiManager` — NVS store, strongest join, forced AP, queues

**Files:** Modify `src/net/wifi_manager.h/.cpp`, `src/main.cpp`, `src/net/info_service.cpp` (mode names).

**Interfaces — Produces** (namespace `cq::net`):

```cpp
struct WifiRequest {
  enum Kind : uint8_t { kStatus, kScan, kSave, kForget, kMode, kApPassword } kind;
  int fd; int32_t id; int8_t slot; bool ap; char ssid[33]; char pass[64];
};
struct WifiResult {
  enum Kind : uint8_t { kStatus, kScan, kApplied, kFailed } kind;
  int fd; int32_t id; uint8_t req;  // WifiRequest::Kind
  char text[48];                    // kFailed reason (no secrets)
};
bool wifiPost(const WifiRequest& r);   // link task -> loop (queue 4)
bool wifiPoll(WifiResult& r);           // loop -> link task (queue 4)
// Snapshots read by the link task for JSON (filled by the loop, copied under a spinlock):
struct WifiSnapshot { char mode[12]; char ssid[33]; int rssi; char ip[16]; char saved[3][33]; uint8_t n_saved;
                      core::ScanEntry scan[12]; uint8_t n_scan; };
void wifiSnapshot(WifiSnapshot& out);
```

Glue (Arduino WiFi + Preferences + FreeRTOS): no host test runs it; checked by the build and the tank session (Task 8).

- [ ] **Step 1: Implement**
  - `WifiManager::begin`: load `cq_wifi/store` (`Preferences::getBytes`, size `WifiStore::kBlobSize`); `decode` false → `seed(CQ_WIFI_SSID, CQ_WIFI_PASSWORD, CQ_AP_PASSWORD)` and save; create the two queues (`xQueueCreate(4, sizeof(...))`) and the spinlock; then `startScanJoin(now)`: `WiFi.scanNetworks(true)` (async); `policy.begin(now)`.
  - `step`: if a scan completed (`WiFi.scanComplete() >= 0`), copy ≤ 12 entries (strongest first) into the snapshot; if it was the join scan, `slot = store.pickStrongest(...)` → `WiFi.begin(ssid(slot), pass(slot))`, log the **SSID only**; no visible saved network → keep waiting (the policy's 10 s timeout starts the AP). Policy actions as today, but `kStartAp` uses `store.apPass()`, `kRetryStation` re-runs the scan-join, and `kForcedAp` never retries.
  - Requests (`xQueueReceive(…, 0)` each step): `kStatus` → refresh the snapshot, result kStatus; `kScan` → `WiFi.scanNetworks(true)`, result kScan when complete; `kSave` / `kForget` / `kApPassword` → store op → `Preferences::putBytes` → result kApplied (or kFailed with `"bad ssid"` / `"bad password"` / `"full"` / `"bad slot"`); if the edited / forgotten slot is the network in use (compare `WiFi.SSID()`), `WiFi.disconnect()` and `policy.forceHome(now)` (reconnect to the strongest remaining, else AP after 10 s — Review Focus 1); `kApPassword` while an AP is up → restart the AP with the new password; `kMode` → `policy.forceAp(now)` (`WiFi.disconnect(); WiFi.mode(WIFI_AP); softAP(...)`) or `policy.forceHome(now)` (`softAPdisconnect(true)`, `WiFi.mode(WIFI_STA)`, scan-join) → result kApplied.
  - Snapshot mode names: `"home"` (station), `"connecting"`, `"ap_fallback"`, `"ap"` (forced). `modeName()` and `/info` use the same names.
  - `main.cpp` unchanged except for nothing (it already calls `g_wifi.begin` / `step`); `info_service.cpp`: the station check also treats kForcedAp as AP for the IP.
  - Never print a password: `grep -n "pass" src/net/wifi_manager.cpp` must show no `logf` with a password argument.
- [ ] **Step 2: Build** — `pio run -e bridge && pio test -e native` → SUCCESS / all PASS.
- [ ] **Step 3: Commit** — `feat(bridge): WifiManager - saved networks in NVS, strongest join, forced AP, request queues (plan M)`.

---

### Task 7: Link task wiring — commands, button, confirmations, settings ops

**Files:** Modify `src/net/link_task.cpp`.

- [ ] **Step 1: Implement**
  - Globals: `core::ConfirmGate g_gate; core::ButtonRouter g_button{g_gate}; core::AckedOps g_ops;`.
  - Dispatch: `kWifiStatus` → `wifiPost({kStatus, fd, id})`; `kWifiScan` → `kScan`; `kWifiSave` / `kWifiForget` / `kWifiMode` / `kApPassword` / `kSetting` with a value (id must be 66, value −100…100 → else `nack INVALID_COMMAND`) / `kCalib` with `calib_reset >= 0` → build a `PendingChange` (copy the fields; `fd`, `id`), `g_gate.begin(change, now)`, `sendTo(fd, jsonConfirm(..., changeName(kind), 30))`; `kSetting` without a value (id ∈ {1, 11, 12, 66} else nack) → `g_ops.push({kSettingGet, id, fd, cmd id})`; `kRestart` → refused BUSY while a primitive runs / OTA, else ack and set `g_restart_at = now + 500`.
  - Button: in `handleLinkEvents` for `EventKind::kButtonPress`: `switch (g_button.onPress(now))` — `kConfirm`: `g_gate.take(change)` → `applyChange(change, now)`; `kToggleWifi`: `wifiPost({kMode, -1, -1, ap = current mode != "ap"})` and push feedback ops: SOUND `kJingle`, LED `kBlink` blue (0,0,255) when going home / white (255,255,255) when going AP, period 200, duration 3000.
  - `applyChange`: WiFi kinds → `wifiPost` with the change's fields (copy `pass`, then `memset` the change's `pass` to 0), and remember `{fd, id, what}` for the result; `kWifiMode` also pushes the feedback ops; `kCalibReset` → `g_cpm = kCpmDefault` + `motionStoreSave`, or `g_roll_mm = kRollDefaultMm` + `motionStoreSaveRoll`; then `sendTo(fd, jsonConfirmed(..., "calib"))` + the `calib` reply; `kSetting` → `g_ops.push({kSettingSet, 66, value})` then `g_ops.push({kSettingsSave})` then `g_ops.push({kSettingGet, 66, fd, id})` (the reply shows the stored value).
  - Each link tick: `PendingChange old; if (g_gate.expired(now, old)) sendTo(old.fd, jsonConfirmExpired(..., changeName(old.kind)));` and on a client close `g_gate.dropClient(fd)`.
  - Ops runner each tick: `if (!g_ops.busy() && !g_link.ackPending() && g_ops.front(op))` send by kind (`sendSound`, `sendLed`, `sendSettingGet`, `sendSettingSet`, `sendSettingsSave`) → `g_ops.markSent(seq, now)`; in `kAckResult`: `if (g_ops.isCurrent(ev.seq)) { if (failed && op.fd >= 0) nack; g_ops.done(); }` before the existing waiter handling. `kSettingValue` → `sendTo(fd of the front get op or the last asker, jsonSetting(...))` (keep a `g_setting_asker {fd, id}` set when a get op is sent).
  - Results: each tick `while (wifiPoll(r))`: `kStatus` → `wifiSnapshot(snap)` → `jsonWifiStatus` to `r.fd`; `kScan` → `jsonWifiScan`; `kApplied` → `jsonConfirmed(what)` then a fresh status; `kFailed` → `jsonNackMessage(..., kInvalidCommand, r.text)`.
  - Restart: `if (g_restart_at && now >= g_restart_at) ESP.restart();` (in the link task loop; the ack went out 0.5 s earlier).
  - `isMotion` unchanged (none of these are motion).
- [ ] **Step 2: Build** — `pio run -e bridge && pio test -e native` → SUCCESS / all PASS.
- [ ] **Step 3: Commit** — `feat(bridge): settings commands, button confirmations and WiFi double press in the link task (plan M)`.

---

### Task 8: `/settings` page, drive-page link, packing, browser check

**Files:** Create `firmware/esp32/web/settings.html`; Modify `firmware/esp32/web/basic_drive.html`, `firmware/esp32/scripts/pack_web.py`.

- [ ] **Step 1: The page** — `web/settings.html`, one file, no framework, the drive page's CSS tokens (copy `:root` colours and the font from `basic_drive.html`), layout per `docs/design/stitch/12-bridge-settings-desktop-v2.png`: top bar (‹ DRIVE link to `/`, title, battery / WiFi / Uno chips, red **EMERGENCY STOP** sending `{"t":"stop","id":n}` on pointerdown), tabs `WiFi · Tank · Calib · Maint` (phone: tabs row; ≥ 900 px: left navigation), and per tab the cards of spec §I4:
  - **WiFi:** Connection (from `wifi_status`), Saved networks (Edit fills the form with the name, Remove → `wifi_forget slot`), Add network (name input with a `<datalist>` from `wifi_scan`, password input, Save & connect → `wifi_save`), Own WiFi (Change → `ap_password`), "Switch to own WiFi / home" → `wifi_mode`.
  - **Tank:** fetch `/info` (versions, uptime, boots) + `welcome` / `ts` (battery, encoder note).
  - **Calib:** `calib` (counts_per_m, roll_mm) with Reset buttons (`calib reset`), `{"t":"setting","setting_id":66}` (track_balance, edit → `setting` with value), read-only `setting` 11 / 12 / 1; arc model values as static text (B 19 cm, v(p) = 15.5 + 0.18 p, v_min 17.7, v_max 33.5).
  - **Maint:** Restart bridge (`restart`), Forget all networks (`wifi_forget`), Record 10 s CSV: `{"t":"debug","on":true,"hz":50}`, collect `td` for 10 s, `debug off`, build a CSV with `td_record.py`'s TD columns and download via a Blob link.
  - Confirmation: on `confirm` show the amber box "Press the tank's button to confirm — Ns" with a countdown; hide on `confirmed` / `confirm_expired` (show "expired"); show `nack` messages in a toast.
  - WebSocket like the drive page (`ws://host/ws`, hello `client:"settings"`, ping every 1 s, reconnect with back-off); a WiFi change that drops the socket shows "reconnecting — join the tank's WiFi `conqueror-bridge` if it switched".
  - Never put a password into the DOM after sending (clear the input).
- [ ] **Step 2: Drive-page link** — in `basic_drive.html`'s menu / top bar add a "SETTINGS" link to `/settings` (match the existing chip style).
- [ ] **Step 3: Packing** — `pack_web.py`: add `("/settings", "text/html; charset=utf-8", gz((web / "settings.html").read_bytes()), True)` with its own budget check (`SETTINGS_BUDGET = 24 * 1024` gzipped); the HTTP route table picks it up like `/` (check `web_routes.cpp` registers every packed asset; add the route if it is a fixed list).
- [ ] **Step 4: Browser check (local, fake WebSocket, Playwright as in Plan K's banner test)** — serve `web/` with `python3 -m http.server`, inject a fake WebSocket that answers `wifi_status` / `wifi_scan` / `calib` / `setting` and records sends; check: tabs switch; Save & connect sends `wifi_save` with slot / ssid / password and shows the confirm countdown on a `confirm` reply; `confirmed` hides it; the STOP button sends `stop`; the CSV download produces a file with the header row; phone (390 × 844) and desktop (1280 × 900) screenshots look like the mock-ups.
- [ ] **Step 5: Build + commit** — `pio run -e bridge` (pack_web runs as a pre-script) → SUCCESS; commit `feat(bridge): /settings page - WiFi, tank info, calibration, maintenance (plan M)`.

---

### Task 9: Tank acceptance, docs, review (owner present)

- [ ] **Step 1:** OTA (password from `secrets.h`, never printed; log filtered and deleted); `/info` shows the commit; the bridge joins the home network from the seeded store (no behaviour change).
- [ ] **Step 2:** Double press on the tank → jingle + white LED, the bridge's AP appears, the phone joins `conqueror-bridge`, `/settings` shows mode `ap`; double press again → blue LED, back on home WiFi.
- [ ] **Step 3:** From the page: add a second network (e.g. a phone hotspot) → confirm countdown → press the tank's button → saved; restart the bridge with the home router off → it joins the hotspot. A change left unconfirmed shows "expired" after 30 s and nothing changed.
- [ ] **Step 4:** `track_balance` edit (e.g. 19 → 18 → 19) with the button; the value reads back after a Uno restart. `calib` reset roll → 25 mm (then let a move re-learn it).
- [ ] **Step 5:** Record 10 s CSV downloads on the phone; Restart bridge works; the STOP button on the settings page stops a drive started from another phone.
- [ ] **Step 6:** design-notes §73 (results), spec Part I "as run", LLD R9 as built, CLAUDE.md plan list; whole-branch review (fresh reviewer); fix Critical / Important with a failing test first; ask the owner before merging `plan-m`.
