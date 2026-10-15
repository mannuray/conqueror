# Plan F — Cockpit-lite (ESP32 phone page) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (owner: native) task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** The bridge's phone page looks and works like the phone Cockpit (hub-ux.md Screen 7b) within what the bridge can do, plus four small bridge additions from protocol spec 01.

**Architecture:** Core (host-tested) gets the `sound` / `cam_lock` WebSocket commands and the new `welcome.caps` / `tf.yaw_rate` JSON fields; `LinkTask` routes the two commands to body-link frames; the stream server gets `/capture`; `web/basic_drive.html` gets the Screen 7b layout. Branch `plan-c-exec` (the bridge is not merged yet).

**Tech Stack:** ESP32-S3 Arduino/ESP-IDF (PlatformIO `bridge`, host tests `native`), vanilla HTML/CSS/JS packed by `scripts/pack_web.py`.

**Spec:** `docs/superpowers/specs/2026-10-10-01-protocol-design.md` §2, §4, §6 (gateway names); UX `docs/design/hub-ux.md` Screen 7b; LLD `docs/lld/esp32-bridge-lld.md` R3.

## Global Constraints
- Gateway names and fields exactly as spec 01 (`sound{name}`, `cam_lock{on}`, `tf.yaw_rate`, `welcome.tank.caps`).
- Owner rules for `sound`/`cam_lock` = those of `drive` (owner or nobody; NOT_OWNER / BUSY as today).
- Web budget: page + help + manifest ≤ 25 + 12 + 6 KB gzipped (`pack_web.py` reports sizes).
- No authentication on the bridge (dev only) — unchanged; Cockpit-lite has no child-play toggle.
- ESP32 host tests (`pio test -e native`) and the `bridge` build must pass; updates go over OTA (`scripts/ota.sh`), USB only for recovery.
- Commits: Conventional Commits + `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. A `sound` name that is not in the catalogue, or a `name` field that is missing / too long → nack, never an out-of-range SoundId to the Uno. → `test_sound_unknown_name_invalid` (Task 1).
2. `sound` / `cam_lock` from a phone that does not own the link while another phone drives → NOT_OWNER, nothing sent. → Task 2 owner-rule test.
3. `/capture` while OTA has stopped the camera → 503, no crash. → Task 3 (hardware check, camera stopped path reviewed).
4. Uno Lite (caps = AUDIO_LED only): MOVES hidden, SOUNDS shown; full firmware: both shown. → `test_welcome_caps_bits` (Task 1) + Task 4 page check.
5. Phone in portrait / small screen → "rotate" hint, nothing overlaps STOP. → Task 4 manual check list.

---

### Task 0: Bring the protocol CapBit change into the bridge branch

`proto::CapBit` (HELLO capability bits) was added on branch `uno-lite`; `plan-c-exec` does not have it. Owner approved the merge (2026-10-15).

- [ ] **Step 1:** `git merge uno-lite` into `plan-c-exec` (both are feature branches; `main` is not touched). Resolve conflicts (expected only in `docs/design-notes.md`: keep both sides).
- [ ] **Step 2:** `cd protocol && uv run protogen check` (generated files consistent), `cd firmware/esp32 && pio test -e native && pio run -e bridge`, `cd firmware/uno && pio test -e native`.
- [ ] **Step 3:** commit the merge.

### Task 1: Core — `sound` / `cam_lock` commands, caps and yaw_rate JSON (host-tested)

**Files:** Modify `firmware/esp32/src/core/ws_codec.h/.cpp`, `firmware/esp32/src/core/ws_json.cpp`; Test: `firmware/esp32/test/test_ws_codec/test_main.cpp`, `firmware/esp32/test/test_ws_json/test_main.cpp`.

**Interfaces — Produces:** `WsCmdKind::kSound`, `WsCmdKind::kCamLock`; `WsCommand::sound_id` (uint8_t, 1..23 = `proto::SoundId`), `WsCommand::on` (bool); `jsonWelcome` writes `caps:{encoders, turn, move, drive_rate, calibrate, audio_led, sensor}`; `jsonTf` writes `yaw_rate` (°/s, 1 decimal).

- [ ] **Step 1: Failing tests** (add to `test_ws_codec`):

```cpp
void test_sound_by_name() {
  const WsCommand c = P("{\"t\":\"sound\",\"name\":\"fanfare\",\"id\":7}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSound), K(c.kind));
  TEST_ASSERT_EQUAL_UINT8(13, c.sound_id);  // proto::SoundId::kFanfare
  TEST_ASSERT_EQUAL(K(WsCmdKind::kSound), K(P("{\"t\":\"sound\",\"name\":\"goodbye\"}").kind));
}

void test_sound_unknown_name_invalid() {
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"sound\",\"name\":\"bogus\"}").kind));
  TEST_ASSERT_EQUAL(K(WsCmdKind::kInvalid), K(P("{\"t\":\"sound\"}").kind));
}

void test_cam_lock() {
  const WsCommand c = P("{\"t\":\"cam_lock\",\"on\":true}");
  TEST_ASSERT_EQUAL(K(WsCmdKind::kCamLock), K(c.kind));
  TEST_ASSERT_TRUE(c.on);
  TEST_ASSERT_FALSE(P("{\"t\":\"cam_lock\",\"on\":false}").on);
}
```

and to `test_ws_json` (welcome caps from HELLO caps = AUDIO_LED only; tf yaw_rate from TELEM_FAST 0.1 °/s):

```cpp
void test_welcome_caps_bits() {
  HelloInfo h{};
  h.valid = true;
  h.caps = 1u << static_cast<uint8_t>(proto::CapBit::kAudioLed);
  char buf[512];
  WelcomeInfo w{"bridge-test", &h, "OV3660", OwnerView{}, "phone-1"};
  jsonWelcome(buf, sizeof buf, w);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"audio_led\":true"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"turn\":false"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"move\":false"));
}

void test_tf_yaw_rate() {
  TankSnapshot s{};
  s.fast_valid = true;
  s.fast.yaw_rate = -125;  // 0.1 °/s
  char buf[256];
  jsonTf(buf, sizeof buf, 1, s);
  TEST_ASSERT_NOT_NULL(strstr(buf, "\"yaw_rate\":-12.5"));
}
```
(Adapt the `HelloInfo` / `OwnerView` initialisers to their real fields when writing the test; record a ruling if they differ.)

- [ ] **Step 2: Run, expect FAIL** (unknown `kSound`/`kCamLock`, fields absent). `cd firmware/esp32 && pio test -e native -f test_ws_codec -f test_ws_json`
- [ ] **Step 3: Implement.** In `ws_codec.h` add `kSound, kCamLock` before `kUnsupported`, and fields `uint8_t sound_id = 0; bool on = false;`. In `ws_codec.cpp`: a `const char* const kSoundNames[23]` table in `proto::SoundId` order (`startup, ready, selftest_fail, chirp_up, chirp_down, blip, tick, jingle, thinking, ouch, error, success, fanfare, sad, safety_chirp, ack_blip, fail, confirm, question, ok_chirp, grumble, scan, goodbye`), `sound_id = index + 1`; missing/unknown name → `kInvalid`. `cam_lock`: boolean `on` (missing → false). In `ws_json.cpp`: welcome caps from `hello->caps` CapBits; tf adds `"yaw_rate":%d.%d` from `fast.yaw_rate` (sign-correct one decimal).
- [ ] **Step 4: Run, expect PASS**; then the whole `native` suite.
- [ ] **Step 5: Commit** `feat(esp32): sound and cam_lock commands, caps and yaw_rate in the gateway JSON`.

### Task 2: Route `sound` / `cam_lock` to the Uno with the drive owner rules

**Files:** Modify `firmware/esp32/src/net/link_task.cpp` (and `core/link_owner` only if the owner check needs a new entry point); Test: `firmware/esp32/test/test_link_owner/test_main.cpp` (owner rule for the new commands, if the check lives in core).

- [ ] **Step 1: Failing test** (if the owner check is a core function): a phone that does not own the link while another phone drives gets NOT_OWNER for `sound` and `cam_lock`; the owner and an idle link are accepted.
- [ ] **Step 2: Implement:** `kSound` → `proto::SoundMsg{sound_id}` frame, `kCamLock` → `proto::CamLockMsg{on}`, both acked from the Uno ACK like `stop` (ack/nack by id).
- [ ] **Step 3:** `pio test -e native` and `pio run -e bridge` pass. **Commit** `feat(esp32): route sound and cam_lock to the Uno`.

### Task 3: `/capture` snapshot

**Files:** Modify `firmware/esp32/src/net/stream_server.cpp`.

- [ ] **Step 1: Implement:** URI `/capture` on port 81: `cameraGrab()` one frame → `image/jpeg`, `Content-Disposition: attachment; filename="tank-<uptime_s>.jpg"`, release the frame; camera stopped or no frame → 503 `camera unavailable`.
- [ ] **Step 2:** `pio run -e bridge`; after OTA (Task 5): `curl -s -o /tmp/snap.jpg http://<ip>:81/capture && file /tmp/snap.jpg` → JPEG image data.
- [ ] **Step 3: Commit** `feat(esp32): /capture snapshot for the phone page`.

### Task 4: Cockpit-lite page (Screen 7b)

**Files:** Modify `firmware/esp32/web/basic_drive.html`; `scripts/pack_web.py` regenerates the gzipped asset.

- [ ] **Step 1: Layout and widgets** as hub-ux.md Screen 7b: chip row (latency from ping RTT, battery %, WiFi bars from `/info` every 10 s, CTRL owner), heading tape (canvas or CSS strip, ±30°), STOP top-right, distance bar right edge (0–2 m, green/amber < 50 cm/red < 15 cm), drive joystick + TILT STEER, camera joystick + LOCK (`cam_lock` toggle) + HOME, **S1 hint**: when `ts.flags` has `link_lost` (the Uno hears nothing) while telemetry keeps arriving, show the banner "Uno can't hear the ESP32 — check switch S1" instead of only "command timeout" (owner, 2026-10-15: S1 left in the upload position caused exactly this); pills SNAP (`<a download href=":81/capture">`), SOUNDS (sheet listing the 23 names → `sound`), MOVES (hidden unless `caps.turn || caps.move`; placeholder sheet, real moves come with the full firmware/hub), ⋯ sheet with the speed slider; vitals line (speed % from `spd_cmd`, heading, `yaw_rate`, `batt_v`); portrait → rotate hint.
- [ ] **Step 2: Size check:** `python3 scripts/pack_web.py` → page ≤ 25 KB gzipped.
- [ ] **Step 3: Desktop check** in a browser against the live bridge (layout at 844×390 phone landscape and 390×844 portrait; STOP never covered).
- [ ] **Step 4: Commit** `feat(esp32): Cockpit-lite phone page (hub-ux Screen 7b)`.

### Task 5: OTA and owner check on the tank

- [ ] **Step 1:** `scripts/ota.sh` (password from `secrets.h`, never printed); `/info` shows the new build, `matches_fw` true.
- [ ] **Step 2 (owner, tank on the stand):** with S1 in the upload position the page shows the S1 hint; back in the ESP32 position it clears; chips update; heading tape follows turning; distance bar follows a hand; SNAP downloads a photo; SOUNDS plays a melody; LOCK keeps the camera after release; MOVES hidden on the Uno Lite; STOP works; drive/deadman as before.
- [ ] **Step 3:** record results in design-notes; final review of the branch range.
