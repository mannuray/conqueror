# Plan N5 — `GO` and `HOLD` (Spec 5 Revision 3, Tank Logo LLD §22, bridge LLD R20) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two Tank Logo words. **`GO`** drives straight forward at `SPEED` while the program goes on at once; **`HOLD`** stops the tank while the program goes on. So `FOREVER [ GO  WAIT UNTIL BLOCKED?  BACK 10  RIGHT 90 ]` drives smoothly instead of stopping every 20 cm, and later `GO WAIT UNTIL SEE? "ball HOLD` works with the AI.

**Architecture:**
- **Language:** `words.yaml` gains the two words (regenerated with `logogen`). `EffectKind` and `ActKind` gain `kGo` / `kHold`, appended at the end of each enum.
- **Runner:** `RunController` carries a `GO` without blocking (`m_going`). It handles the GO's background end (`goEnded`) and clears `BLOCKED?` when a GO starts. The simulator draws a `GO` as an uncertain 1 m.
- **Bridge:** `MotionService` gains `PrimKind::kGo` (v = speed, w = 0, refreshed ≤ 300 ms, ends at once on the obstacle brake), and `MotionLink::hold` ends it as done with a STOP. The Uno's stall event ends a GO as it does a MOVE.
- **App:** `LogoController` keeps a GO alive every tick and turns `SPEED` while going into a new `GO`; `MotionController::startLogo` handles `kGo` / `kHold`.
- **Unchanged:** the Uno, the protocol and every safety rule.

**Tech Stack:** C++17 (ESP32-S3 Arduino 3.x, PlatformIO `bridge`, `bridge_ota`, `native`), Unity host tests, `logogen` (Python 3.12 + uv), Node test for the `/logo` page.

**Spec:** `docs/superpowers/specs/2026-10-11-05-tank-logo-design.md` Revision 3; `docs/lld/tank-logo-lld.md` §22; `docs/lld/esp32-bridge-lld.md` Revision 20 (all approved 2026-10-22; design-notes §118).

## Global Constraints
- **`GO`, `HOLD`:** level 2, group `move`, kind `command`, no inputs, no `needs`. `GO 40` → 7004.
- **`GO`** drives forward only, at the current `SPEED`.
  - A second `GO` while going changes nothing; `SPEED` while going applies at once.
  - It ends at the next motion word (no end event, the motion takes over), at `HOLD` (ends done), or when the obstacle brake engages: at once, with `BLOCKED?` TRUE when the program uses `BLOCKED?`, else 7024.
  - The Uno's stall event → 7044. No refresh for more than 300 ms (`kRateDeadmanMs`) → deadman → 7034. The program's end stops it.
- **`HOLD`** with nothing going does nothing (its action is still traced as `HOLD`).
- **`BLOCKED?`** is cleared when a GO starts, and as today when a move ends done.
- **Preview:** a `GO` is 1 m ahead (`kGoPreviewCm` 100); every point after it is uncertain.
- **Generated files are never edited by hand:** change `logo/spec/*`, then run `cd logo && uv run logogen write`.
- No change to the Uno, the protocol or any safety rule; no heap; no `%llu`.
- Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch `plan-n5` from `main`.

## Review Focus
1. **HOLD's own done end arriving after `m_going` is cleared** must be ignored, not end the program with 7040. The order is: `carry` clears `m_going`, the host's hold fires the end synchronously, `goEnded` sees not going and ignores it. Pinned by `test_go_is_kept_alive_every_tick_until_hold` and the run case "GO, then HOLD".
2. **A motion word right after a GO** must not produce a GO end. Otherwise a blocked flag left over from the GO could end the program. Pinned by `test_a_move_takes_over_from_go_without_an_end` and the run case "the motion after a GO counts as the next motion".
3. **A GO whose client loses the drive** (another phone, the IR remote) must end and never send a STOP for the new driver. It uses the existing `MotionLink` owner rules, which keep the drive while the program runs. The reviewer checks this by reading `MotionLink::onTelemetry` for `kGo`.
4. **The keep-alive stops when the program ends or holds,** so the bridge's 300 ms deadman can stop a GO left over by a bug. `keepGoing` runs only while `RunController::going()`. Pinned by the "held: nothing more to keep alive" assertion.
5. **`primitive_done` for a GO says `"prim":"go"`,** and the phone page's stall banner and the Tune log stay sensible. The reviewer checks `primName` users: `ws_json_drive.cpp` tf progress shows `"kind":"go"` with pct 0.

---

### Task 1: The words, the runner and the preview

**Files:**
- Modify: `logo/spec/words.yaml`, `logo/spec/tests/reading.yaml`, `logo/spec/tests/errors.yaml`, `logo/spec/tests/running.yaml`
- Regenerate (never by hand): `firmware/esp32/src/logo/generated/words.h`, `firmware/esp32/web/generated/words.json`, `firmware/esp32/test/test_logo_conformance/cases.h`, `firmware/esp32/test/test_logo_running/cases.h`
- Modify: `firmware/esp32/src/logo/program.{h,cpp}`, `compiler.cpp`, `effects.{h,cpp}`, `effects_text.cpp`, `run_controller.{h,cpp}`, `simulator.{h,cpp}`
- Test: `firmware/esp32/test/test_logo_running/test_main.cpp` (the GO world rule), `firmware/esp32/test/test_logo_sim/test_main.cpp`

**Interfaces:**
- Produces:
  - `EffectKind::kGo`, `EffectKind::kHold` (after `kTick`); `ActKind::kGo`, `ActKind::kHold` (after `kTick`)
  - `RunController::going() const`
  - `Simulator::kGoPreviewCm` = 100
  - action text `"GO"` / `"HOLD"`

- [ ] **Step 1: Write the failing spec cases and tests**

The word table and the shared test cases:

```diff
diff --git a/logo/spec/tests/errors.yaml b/logo/spec/tests/errors.yaml
index b4dad97..96b175e 100644
--- a/logo/spec/tests/errors.yaml
+++ b/logo/spec/tests/errors.yaml
@@ -185,3 +185,8 @@
   source: REPEAT 20000 [ BEEP ]
   expect: {error: 7025, line: 1, col: 8}
   message_has: "from 0 to 10000."
+
+- name: GO takes no input (Spec 5 Revision 3)
+  source: GO 40
+  expect: {error: 7004, line: 1, col: 4}
+  message_has: "40"
diff --git a/logo/spec/tests/reading.yaml b/logo/spec/tests/reading.yaml
index fbfdc52..a479cd0 100644
--- a/logo/spec/tests/reading.yaml
+++ b/logo/spec/tests/reading.yaml
@@ -235,3 +235,8 @@
   reads: >
     (TO SUM (:n :acc) [ (IFELSE (= :n 0) [ (OUTPUT :acc) ] [ (OUTPUT (SUM (- :n 1) (+ :acc :n))) ]) ])
     (FORWARD (SUM 20 0))
+
+- name: GO and HOLD take no inputs (Spec 5 Revision 3)
+  source: GO WAIT UNTIL DISTANCE < 30 HOLD BEEP
+  expect: ok
+  reads: (GO) (WAIT-UNTIL (< (DISTANCE) 30)) (HOLD) (BEEP)
diff --git a/logo/spec/tests/running.yaml b/logo/spec/tests/running.yaml
index 0f10cc9..d9f8cff 100644
--- a/logo/spec/tests/running.yaml
+++ b/logo/spec/tests/running.yaml
@@ -169,3 +169,53 @@
   source: FOREVER [ WAIT 60 ]
   does: [WAIT 60, WAIT 60, WAIT 60, WAIT 60, WAIT 60]
   ends: {error: 7041, line: 1}
+
+# ---- GO and HOLD (Spec 5 Revision 3, Tank Logo LLD §22): GO drives on and the program goes on at once. In the
+# world a GO counts as a motion: cut_short / stuck end it at once, otherwise it drives until HOLD or another motion.
+- name: GO, then HOLD
+  source: GO WAIT 1 HOLD
+  does: [GO, WAIT 1, HOLD]
+  ends: finished
+
+- name: a move takes over from GO
+  source: GO FD 50
+  does: [GO, FORWARD 50]
+  ends: finished
+
+- name: GO until blocked, with BLOCKED?
+  source: |
+    GO WAIT UNTIL BLOCKED?
+    BK 10 RT 90
+  world: {cut_short: [1]}
+  does: [GO, WAIT-UNTIL, BACK 10, RIGHT 90]
+  ends: finished
+
+- name: GO blocked without BLOCKED? ends the program
+  source: |
+    GO
+    WAIT 5
+  world: {cut_short: [1]}
+  does: [GO]
+  ends: {error: 7024, line: 1}
+
+- name: GO with a stuck track ends the program
+  source: GO WAIT 2
+  world: {stuck: [1]}
+  does: [GO]
+  ends: {error: 7044, line: 1}
+
+- name: SPEED while going, and GO again
+  source: GO SPEED 60 GO HOLD
+  does: [GO, SPEED 60, GO, HOLD]
+  ends: finished
+
+- name: HOLD with nothing going
+  source: HOLD BEEP
+  does: [HOLD, BEEP]
+  ends: finished
+
+- name: the motion after a GO counts as the next motion
+  source: GO FD 20 FD 30
+  world: {cut_short: [3]}
+  does: [GO, FORWARD 20, FORWARD 30]
+  ends: {error: 7024, line: 1}
diff --git a/logo/spec/words.yaml b/logo/spec/words.yaml
index 5ebedfc..73a717a 100644
--- a/logo/spec/words.yaml
+++ b/logo/spec/words.yaml
@@ -146,6 +146,20 @@ words:
     does: TURN to the heading the program started with
     help: turn back the way you started
     example: HOME
+  - name: GO
+    level: 2
+    kind: command
+    group: move
+    does: drive straight forward at the current SPEED and go on at once (Spec 5 Revision 3); ends at the next move or turn, HOLD, something in the way, or the program's end
+    help: drive on until something happens
+    example: GO  WAIT UNTIL DISTANCE < 30  HOLD
+  - name: HOLD
+    level: 2
+    kind: command
+    group: move
+    does: stop the tank and go on with the program (STOP ends the whole program)
+    help: stop the tank, keep the program going
+    example: GO  WAIT 2  HOLD
   - name: STOP
     level: 1
     kind: control
```

Then regenerate: `cd logo && uv run logogen write` (expected: `logogen: wrote 4 files`).

The run-test world rule for GO, and the preview test:

```diff
diff --git a/firmware/esp32/test/test_logo_running/cases.h b/firmware/esp32/test/test_logo_running/cases.h
index f22c027..5ef60d6 100644
--- a/firmware/esp32/test/test_logo_running/cases.h
+++ b/firmware/esp32/test/test_logo_running/cases.h
@@ -31,6 +31,14 @@ static const RunCase kRunCases[] = {
     {"WAIT UNTIL that is already true", "WAIT UNTIL DISTANCE < 20  BEEP", "WAIT-UNTIL | BEEP", 0, 0, "", 10, 0ULL, 0ULL},
     {"WAIT UNTIL gives up after 60 s and the program goes on", "WAIT UNTIL DISTANCE < 5  BEEP", "WAIT-UNTIL | BEEP", 0, 0, "", 0, 0ULL, 0ULL},
     {"the time limit", "FOREVER [ WAIT 60 ]", "WAIT 60 | WAIT 60 | WAIT 60 | WAIT 60 | WAIT 60", 7041, 1, "", 0, 0ULL, 0ULL},
+    {"GO, then HOLD", "GO WAIT 1 HOLD", "GO | WAIT 1 | HOLD", 0, 0, "", 0, 0ULL, 0ULL},
+    {"a move takes over from GO", "GO FD 50", "GO | FORWARD 50", 0, 0, "", 0, 0ULL, 0ULL},
+    {"GO until blocked, with BLOCKED?", "GO WAIT UNTIL BLOCKED?\nBK 10 RT 90", "GO | WAIT-UNTIL | BACK 10 | RIGHT 90", 0, 0, "", 0, 1ULL, 0ULL},
+    {"GO blocked without BLOCKED? ends the program", "GO\nWAIT 5", "GO", 7024, 1, "", 0, 1ULL, 0ULL},
+    {"GO with a stuck track ends the program", "GO WAIT 2", "GO", 7044, 1, "", 0, 0ULL, 1ULL},
+    {"SPEED while going, and GO again", "GO SPEED 60 GO HOLD", "GO | SPEED 60 | GO | HOLD", 0, 0, "", 0, 0ULL, 0ULL},
+    {"HOLD with nothing going", "HOLD BEEP", "HOLD | BEEP", 0, 0, "", 0, 0ULL, 0ULL},
+    {"the motion after a GO counts as the next motion", "GO FD 20 FD 30", "GO | FORWARD 20 | FORWARD 30", 7024, 1, "", 0, 4ULL, 0ULL},
 };
 
-#define LOGO_RUN_CASES(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26)
+#define LOGO_RUN_CASES(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34)
diff --git a/firmware/esp32/test/test_logo_running/test_main.cpp b/firmware/esp32/test/test_logo_running/test_main.cpp
index 33a90b6..57b5eb8 100644
--- a/firmware/esp32/test/test_logo_running/test_main.cpp
+++ b/firmware/esp32/test/test_logo_running/test_main.cpp
@@ -29,6 +29,7 @@ struct World {
   RunTelem tel;
   uint32_t now = 1000;
   uint8_t motion = 0;
+  bool going = false;  // a GO drives on (Tank Logo LLD §22): it counts as one motion
   char trace[2048] = "";
 };
 
@@ -52,13 +53,35 @@ RunOut finishMotion(World& w, RunController& rc, const Action& a) {
   return rc.onPrimitiveEnd(e);
 }
 
+// GO (LLD §22): the first GO of a run of GOs takes a motion number; if the world cuts it short or sticks it,
+// its end comes at once, otherwise it drives on until HOLD or the next motion takes over.
+RunOut goStarted(World& w, RunController& rc) {
+  if (w.going) return RunOut();
+  w.going = true;
+  const unsigned long long bit = 1ULL << w.motion++;
+  if ((w.c.cut_short & bit) == 0 && (w.c.stuck & bit) == 0) return RunOut();
+  PrimitiveEnd e;
+  e.reason = (w.c.cut_short & bit) != 0 ? EndReason::kBlocked : EndReason::kStalled;
+  w.going = false;
+  return rc.onPrimitiveEnd(e);
+}
+
 RunOut runToEnd(World& w, RunController& rc) {
   RunOut out;
   for (int i = 0; i < 500000 && !out.ended; ++i) {
     if (rc.state() == RunState::kWaiting && rc.wakeAt() > w.now) w.now = rc.wakeAt();
     out = rc.tick(w.tel, w.now);
     note(w, out);
-    if (rc.state() == RunState::kMotion) out = finishMotion(w, rc, out.action);
+    if (out.act && out.action.kind == ActKind::kGo) {
+      const RunOut g = goStarted(w, rc);
+      if (g.ended) out = g;
+    } else if (out.act && out.action.kind == ActKind::kHold) {
+      w.going = false;
+    }
+    if (rc.state() == RunState::kMotion) {
+      w.going = false;
+      out = finishMotion(w, rc, out.action);
+    }
     w.now += 2;
   }
   return out;
diff --git a/firmware/esp32/test/test_logo_sim/test_main.cpp b/firmware/esp32/test/test_logo_sim/test_main.cpp
index a1c5ca7..44e597b 100644
--- a/firmware/esp32/test/test_logo_sim/test_main.cpp
+++ b/firmware/esp32/test/test_logo_sim/test_main.cpp
@@ -79,6 +79,19 @@ void test_uncertain_after_a_sensor_read() {
   TEST_ASSERT_EQUAL(2, g_path.pts[2].line);
 }
 
+// Tank Logo LLD §22.4: GO has no length - the preview draws 1 m straight ahead and every point from there on is
+// uncertain (dashed); HOLD draws nothing.
+void test_go_draws_one_uncertain_metre() {
+  plan("FD 10\nGO\nHOLD RT 90 FD 20");
+  TEST_ASSERT_EQUAL(5, g_path.n);  // start, FD 10, GO, RT 90, FD 20
+  TEST_ASSERT_FALSE(g_path.pts[1].uncertain);
+  TEST_ASSERT_EQUAL_INT32(0, g_path.pts[2].x_mm);
+  TEST_ASSERT_EQUAL_INT32(1100, g_path.pts[2].y_mm);  // 10 cm + 1 m ahead
+  TEST_ASSERT_TRUE(g_path.pts[2].uncertain);
+  TEST_ASSERT_EQUAL(2, g_path.pts[2].line);
+  TEST_ASSERT_TRUE(g_path.pts[4].uncertain);  // still a guess after HOLD
+}
+
 void test_limits_cut_the_path_short() {
   plan("FOREVER [ ]");
   TEST_ASSERT_TRUE(g_r.cut_short);
@@ -118,6 +131,7 @@ int main(int, char**) {
   RUN_TEST(test_arc_and_pivot);
   RUN_TEST(test_home_turns_back_to_the_start_heading);
   RUN_TEST(test_uncertain_after_a_sensor_read);
+  RUN_TEST(test_go_draws_one_uncertain_metre);
   RUN_TEST(test_limits_cut_the_path_short);
   RUN_TEST(test_simulated_time);
   RUN_TEST(test_run_time_error_ends_the_path);
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f "test_logo*"`
Expected: FAIL to compile — `no member named 'kGo' in 'cq::logo::ActKind'` (in `test_logo_running`).

- [ ] **Step 3: Implement**

```diff
diff --git a/firmware/esp32/src/logo/compiler.cpp b/firmware/esp32/src/logo/compiler.cpp
index 4300fdd..60a7195 100644
--- a/firmware/esp32/src/logo/compiler.cpp
+++ b/firmware/esp32/src/logo/compiler.cpp
@@ -22,6 +22,7 @@ constexpr WordUse kUses[] = {
     fx("LED", EffectKind::kLed), fx("BEEP", EffectKind::kBeep), fx("SOUND", EffectKind::kSound),
     fx("LOOK", EffectKind::kLook), fx("HOME", EffectKind::kHome), fx("ARC", EffectKind::kArc),
     fx("PIVOT", EffectKind::kPivot), fx("PLAY", EffectKind::kPlay), fx("SAY", EffectKind::kSay),
+    fx("GO", EffectKind::kGo), fx("HOLD", EffectKind::kHold),
     {"REPEAT", WordRole::kRepeat, 0}, {"FOREVER", WordRole::kForever, 0}, {"IF", WordRole::kIf, 0},
     {"IFELSE", WordRole::kIfElse, 0}, {"WHILE", WordRole::kWhile, 0}, {"WAIT UNTIL", WordRole::kWaitUntil, 0},
     {"STOP", WordRole::kStop, 0}, {"OUTPUT", WordRole::kOutput, 0},
diff --git a/firmware/esp32/src/logo/effects.cpp b/firmware/esp32/src/logo/effects.cpp
index 9059c9c..56e6008 100644
--- a/firmware/esp32/src/logo/effects.cpp
+++ b/firmware/esp32/src/logo/effects.cpp
@@ -43,6 +43,7 @@ class Mapper {
   bool pivot();
   bool play();
   bool say();
+  bool goHold();  // GO (at the current SPEED) / HOLD
   bool number(uint8_t i, int32_t& milli);
   bool preset(uint8_t i, gen::InType place, const char* const* names, uint8_t n, uint8_t& idx);
   bool numbers(uint8_t i, uint8_t want, const char* type, int32_t* out);
@@ -64,7 +65,7 @@ bool Mapper::run() {
   static constexpr Fn kMap[] = {&Mapper::move,   &Mapper::move,  &Mapper::turn,  &Mapper::turn,   &Mapper::speed,
                                 &Mapper::wait,   &Mapper::led,   &Mapper::simple, &Mapper::sound, &Mapper::look,
                                 &Mapper::simple, &Mapper::arc,   &Mapper::pivot, &Mapper::play,   &Mapper::say,
-                                &Mapper::simple, &Mapper::simple};
+                                &Mapper::simple, &Mapper::simple, &Mapper::goHold, &Mapper::goHold};
   static_assert(sizeof kMap / sizeof kMap[0] == static_cast<size_t>(EffectKind::kCount), "one per EffectKind");
   m_out = Action();
   m_out.line = m_e.line;
@@ -292,6 +293,11 @@ bool Mapper::play() {
   return true;
 }
 
+bool Mapper::goHold() {
+  m_out.kind = m_e.kind == EffectKind::kGo ? ActKind::kGo : ActKind::kHold;  // speed_pct: set by run()
+  return true;
+}
+
 bool Mapper::say() {
   m_out.kind = ActKind::kSay;
   valueText(m_p, m_e.args[0], false, m_out.text, sizeof m_out.text);
diff --git a/firmware/esp32/src/logo/effects.h b/firmware/esp32/src/logo/effects.h
index 2bfedee..92eb87f 100644
--- a/firmware/esp32/src/logo/effects.h
+++ b/firmware/esp32/src/logo/effects.h
@@ -15,7 +15,7 @@ namespace logo {
 
 enum class ActKind : uint8_t {
   kNone, kMove, kTurn, kArc, kPivot, kHome, kSpeed, kWait, kLed, kBeep, kSound, kTone, kLook, kLookHome, kSay,
-  kUntilStart, kTick
+  kUntilStart, kTick, kGo, kHold  // kGo: drive on at speed_pct; kHold: stop it (Tank Logo LLD §22)
 };
 
 struct Action {
diff --git a/firmware/esp32/src/logo/effects_text.cpp b/firmware/esp32/src/logo/effects_text.cpp
index 0ddac8e..4e77c57 100644
--- a/firmware/esp32/src/logo/effects_text.cpp
+++ b/firmware/esp32/src/logo/effects_text.cpp
@@ -51,11 +51,13 @@ void led(const Action& a, char* out, size_t cap) { snprintf(out, cap, "LED %u %u
 void sound(const Action& a, char* out, size_t cap) { snprintf(out, cap, "SOUND %s", gen::kSounds[a.sound_id - 1]); }
 void look(const Action& a, char* out, size_t cap) { snprintf(out, cap, "LOOK %d %d", a.pan_deg, a.tilt_deg); }
 void say(const Action& a, char* out, size_t cap) { snprintf(out, cap, "SAY %s", a.text); }
+void go(const Action&, char* out, size_t cap) { plain("GO", out, cap); }
+void hold(const Action&, char* out, size_t cap) { plain("HOLD", out, cap); }
 
 using TextFn = void (*)(const Action&, char*, size_t);
 constexpr TextFn kText[] = {none, motion, motion, motion, motion, home,     speed, wait,       led,
-                            beep, sound,  tone,   look,   lookHome, say,    untilStart, none};
-static_assert(sizeof kText / sizeof kText[0] == static_cast<size_t>(ActKind::kTick) + 1, "one per ActKind");
+                            beep, sound,  tone,   look,   lookHome, say,    untilStart, none,  go, hold};
+static_assert(sizeof kText / sizeof kText[0] == static_cast<size_t>(ActKind::kHold) + 1, "one per ActKind");
 
 }  // namespace
 
diff --git a/firmware/esp32/src/logo/program.cpp b/firmware/esp32/src/logo/program.cpp
index 48585e6..10a4771 100644
--- a/firmware/esp32/src/logo/program.cpp
+++ b/firmware/esp32/src/logo/program.cpp
@@ -9,7 +9,7 @@ namespace {
 
 constexpr const char* kEffectWords[] = {"FORWARD", "BACK",  "RIGHT", "LEFT", "SPEED", "WAIT",       "LED",       "BEEP",
                                         "SOUND",   "LOOK",  "HOME",  "ARC",  "PIVOT", "PLAY",       "SAY",
-                                        "WAIT UNTIL", "WAIT UNTIL"};
+                                        "WAIT UNTIL", "WAIT UNTIL", "GO", "HOLD"};
 static_assert(sizeof kEffectWords / sizeof kEffectWords[0] == static_cast<size_t>(EffectKind::kCount), "one per kind");
 
 constexpr uint8_t kMaxDepth = 64;
diff --git a/firmware/esp32/src/logo/program.h b/firmware/esp32/src/logo/program.h
index 8adc2e0..ba5f6c6 100644
--- a/firmware/esp32/src/logo/program.h
+++ b/firmware/esp32/src/logo/program.h
@@ -26,7 +26,7 @@ enum class Sensor : uint8_t { kDistance, kObstacle, kBlocked, kHeading, kTilted,
 enum class Builtin : uint8_t { kRandom, kRemainder, kAbs, kRound, kRepcount };
 enum class EffectKind : uint8_t {
   kForward, kBack, kRight, kLeft, kSpeed, kWait, kLed, kBeep, kSound, kLook, kHome,
-  kArc, kPivot, kPlay, kSay, kUntilStart, kTick,
+  kArc, kPivot, kPlay, kSay, kUntilStart, kTick, kGo, kHold,  // GO / HOLD: Spec 5 Revision 3
   kCount
 };
 const char* effectWord(EffectKind k);  // "FORWARD" … ("WAIT UNTIL" for kUntilStart / kTick)
diff --git a/firmware/esp32/src/logo/run_controller.cpp b/firmware/esp32/src/logo/run_controller.cpp
index 2351c60..b028932 100644
--- a/firmware/esp32/src/logo/run_controller.cpp
+++ b/firmware/esp32/src/logo/run_controller.cpp
@@ -74,6 +74,7 @@ bool RunController::start(const Program& p, const RunConfig& cfg, const RunTelem
   m_start_yaw = t.yaw_cdeg;
   m_speed = 40;
   m_blocked = false;
+  m_going = false;
   return true;
 }
 
@@ -130,7 +131,17 @@ void RunController::effect(const RunTelem& t, uint32_t now_ms, RunOut& out) {
 void RunController::carry(const Action& a, const RunTelem& t, uint32_t now_ms, RunOut& out) {
   out.action = a;
   out.act = a.kind != ActKind::kTick;
+  if (a.kind == ActKind::kGo) {  // drives on; the program goes on at once (LLD §22.2)
+    if (!m_going) m_blocked = false;
+    m_going = true;
+    return;
+  }
+  if (a.kind == ActKind::kHold) {
+    m_going = false;
+    return;
+  }
   if (isMotion(a.kind)) {
+    m_going = false;  // the motion takes over from a GO (no end comes for the GO)
     if (a.kind == ActKind::kHome) out.action.delta_cdeg = yawDelta(t.yaw_cdeg, m_start_yaw);
     m_state = RunState::kMotion;
     return;
@@ -156,7 +167,7 @@ Sensors RunController::sensors(const RunTelem& t, uint32_t now_ms) const {
 }
 
 RunOut RunController::onPrimitiveEnd(const core::PrimitiveEnd& e) {
-  if (m_state != RunState::kMotion) return RunOut();
+  if (m_state != RunState::kMotion) return goEnded(e);
   m_state = RunState::kRunning;
   if (e.reason == core::EndReason::kDone) {
     m_blocked = false;
@@ -169,6 +180,19 @@ RunOut RunController::onPrimitiveEnd(const core::PrimitiveEnd& e) {
   return end(kByEndReason[static_cast<uint8_t>(e.reason)]);
 }
 
+// A GO's end, arriving while the program runs on (LLD §22.2): HOLD's own end (done) is expected; the obstacle
+// carries on with BLOCKED? or ends 7024, like a move; every other end ends the program as a move's would.
+RunOut RunController::goEnded(const core::PrimitiveEnd& e) {
+  if (!m_going || !active()) return RunOut();
+  m_going = false;
+  if (e.reason == core::EndReason::kDone) return RunOut();
+  if (e.reason == core::EndReason::kBlocked && m_p->handles_obstacles) {
+    m_blocked = true;
+    return RunOut();
+  }
+  return end(kByEndReason[static_cast<uint8_t>(e.reason)]);
+}
+
 RunOut RunController::onMotionEvent(core::MotionEvent e) {
   if (!active()) return RunOut();
   return end(kByMotionEvent[static_cast<uint8_t>(e)]);
diff --git a/firmware/esp32/src/logo/run_controller.h b/firmware/esp32/src/logo/run_controller.h
index cacffed..c0acf0c 100644
--- a/firmware/esp32/src/logo/run_controller.h
+++ b/firmware/esp32/src/logo/run_controller.h
@@ -55,6 +55,7 @@ class RunController {
   RunOut onMotionEvent(core::MotionEvent e);           // STOP, release, page gone … (also between motions)
   RunOut onEvent(RunEvent e);
   RunState state() const { return m_state; }
+  bool going() const { return m_going; }  // a GO drives on in the background (LLD §22): keep it alive each tick
   uint32_t wakeAt() const { return m_wake; }  // kWaiting: when WAIT / PLAY / a WAIT UNTIL tick is over
 
  private:
@@ -62,6 +63,7 @@ class RunController {
   void effect(const RunTelem& t, uint32_t now_ms, RunOut& out);
   void carry(const Action& a, const RunTelem& t, uint32_t now_ms, RunOut& out);
   Sensors sensors(const RunTelem& t, uint32_t now_ms) const;
+  RunOut goEnded(const core::PrimitiveEnd& e);
   RunOut end(uint16_t code);
   RunOut endWith(uint16_t code, uint16_t line, const char* msg);
   bool active() const { return m_state != RunState::kIdle && m_state != RunState::kEnded; }
@@ -75,6 +77,7 @@ class RunController {
   uint16_t m_start_yaw = 0;
   uint8_t m_speed = 40;
   bool m_blocked = false;
+  bool m_going = false;  // GO (LLD §22)
   EffectKind m_last = EffectKind::kBeep;  // the last effect (its word for 7021)
 };
 
diff --git a/firmware/esp32/src/logo/simulator.cpp b/firmware/esp32/src/logo/simulator.cpp
index 71a439a..4bdeb5c 100644
--- a/firmware/esp32/src/logo/simulator.cpp
+++ b/firmware/esp32/src/logo/simulator.cpp
@@ -32,6 +32,7 @@ SimResult Simulator::plan(const Program& p, PathBuffer& out, uint32_t seed) {
   m_speed = 40;
   m_line = 0;
   m_full = false;
+  m_guess = false;
   SimResult r;
   if (!p.ok) {  // never plan a program that did not compile completely
     r.code = 7010;
@@ -94,6 +95,10 @@ bool Simulator::moves(const Action& a) {
     case ActKind::kHome: turn(-wrap180(m_pose.h)); return true;
     case ActKind::kArc: around(a.radius_cm * 10.0, -a.delta_cdeg / 100.0); return true;
     case ActKind::kPivot: around(core::motion::kTrackWidthCm * 5.0, -a.delta_cdeg / 100.0); return true;
+    case ActKind::kGo:  // no length: 1 m, and everything after it is a guess (LLD §22.4)
+      m_guess = true;
+      move(kGoPreviewCm);
+      return true;
     default: return false;
   }
 }
@@ -141,7 +146,7 @@ void Simulator::point(uint16_t line) {
   p.y_mm = static_cast<int32_t>(lround(m_pose.y));
   p.heading_deg = static_cast<int16_t>(lround(wrap180(m_pose.h)));
   p.line = line;
-  p.uncertain = m_vm.sensorRead();
+  p.uncertain = m_vm.sensorRead() || m_guess;
 }
 
 Sensors Simulator::sensors() const {
diff --git a/firmware/esp32/src/logo/simulator.h b/firmware/esp32/src/logo/simulator.h
index c12cfdd..31e46ac 100644
--- a/firmware/esp32/src/logo/simulator.h
+++ b/firmware/esp32/src/logo/simulator.h
@@ -40,6 +40,7 @@ class Simulator {
  public:
   static constexpr uint32_t kMaxInstructions = 200000;
   static constexpr uint32_t kTickMs = 100;     // one WAIT UNTIL check
+  static constexpr int32_t kGoPreviewCm = 100;  // GO has no length: the preview draws 1 m (LLD §22.4)
   static constexpr double kTurnDegPerS = 90.0;  // spin speed of the model (time only)
 
   explicit Simulator(Vm& vm) : m_vm(vm) {}
@@ -68,6 +69,7 @@ class Simulator {
   uint8_t m_speed = 40;
   uint16_t m_line = 0;
   bool m_full = false;
+  bool m_guess = false;  // after a GO every point is uncertain
 };
 
 }  // namespace logo
```

- [ ] **Step 4: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f "test_logo*"`
Expected: PASS (all Logo suites; the 8 new run cases, 2 new reading / error cases, the preview test).
Run: `cd logo && uv run pytest -q && uv run logogen check`
Expected: 14 passed; check exit 0.
Run: `cd firmware/esp32/web && node --test test/logo.test.js`
Expected: pass 46.

- [ ] **Step 5: Commit**

```bash
git add logo/spec firmware/esp32/src/logo firmware/esp32/web/generated firmware/esp32/test/test_logo_conformance firmware/esp32/test/test_logo_running firmware/esp32/test/test_logo_sim
git commit -m "feat(logo): GO and HOLD - drive on until something happens (Spec 5 Revision 3)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The GO primitive on the bridge

**Files:**
- Modify: `firmware/esp32/src/core/motion_service.{h,cpp}`, `firmware/esp32/src/core/motion_link.{h,cpp}`, `firmware/esp32/src/core/ws_json_util.cpp`
- Test: `firmware/esp32/test/test_motion/test_main.cpp`

**Interfaces:**
- Produces:
  - `PrimKind::kGo`
  - `MotionService::startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms)`, `keepGo(uint32_t now_ms)`, `MotionOut hold()`
  - `MotionLink::hold(uint32_t now_ms) -> LinkOut`
  - `primName(kGo)` = `"go"`

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/firmware/esp32/test/test_motion/test_main.cpp b/firmware/esp32/test/test_motion/test_main.cpp
index 0804517..a52f428 100644
--- a/firmware/esp32/test/test_motion/test_main.cpp
+++ b/firmware/esp32/test/test_motion/test_main.cpp
@@ -1050,6 +1050,115 @@ void test_service_pivot_correction_keeps_the_same_track() {
   }
 }
 
+// ---------- GO (bridge LLD R20, Tank Logo LLD §22): drive on until told otherwise ----------
+namespace {
+MotionTelem blockedAt(int32_t counts) {
+  MotionTelem t = encAt(counts);
+  t.flags = 1u << static_cast<unsigned>(proto::FastFlagBit::kBlocked);
+  return t;
+}
+}  // namespace
+
+void test_go_drives_straight_at_its_speed_until_held() {
+  MotionService svc;
+  svc.startGo(3, 40, 7, 0);
+  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kGo), static_cast<int>(svc.kind()));
+  MotionOut o = svc.onTelemetry(encAt(0), 20);
+  TEST_ASSERT_TRUE(o.send);
+  TEST_ASSERT_EQUAL_INT8(40, o.v);
+  TEST_ASSERT_EQUAL_INT8(0, o.w);  // the Uno's heading hold keeps it straight, as a MOVE
+  for (uint32_t t = 40; t <= 2000; t += 20) {  // drives on while kept alive
+    svc.keepGo(t);
+    o = svc.onTelemetry(encAt(static_cast<int32_t>(t)), t);
+    TEST_ASSERT_FALSE(o.ended);
+  }
+  o = svc.hold();
+  TEST_ASSERT_TRUE(o.ended);
+  TEST_ASSERT_TRUE(o.end.ok);
+  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(o.end.reason));
+  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kGo), static_cast<int>(o.end.kind));
+  TEST_ASSERT_EQUAL_INT32(7, o.end.id);
+  TEST_ASSERT_EQUAL_INT8(0, o.v);
+  TEST_ASSERT_FALSE(svc.active());
+  TEST_ASSERT_FALSE(svc.hold().ended);  // nothing going: nothing to hold
+}
+
+void test_go_ends_blocked_as_soon_as_the_brake_engages() {
+  MotionService svc;
+  svc.startGo(3, 40, 7, 0);
+  svc.onTelemetry(encAt(0), 20);
+  const MotionOut o = svc.onTelemetry(blockedAt(10), 40);  // no 2 s wait: the tank already stopped
+  TEST_ASSERT_TRUE(o.ended);
+  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kBlocked), static_cast<int>(o.end.reason));
+  TEST_ASSERT_EQUAL_INT8(0, o.v);
+}
+
+void test_go_stops_without_a_refresh_within_300_ms() {
+  MotionService svc;
+  svc.startGo(3, 40, 7, 0);
+  svc.keepGo(100);
+  TEST_ASSERT_FALSE(svc.onTelemetry(encAt(0), 380).ended);  // 280 ms since the refresh
+  TEST_ASSERT_FALSE(svc.tick(400).ended);                    // 300 ms: still on
+  const MotionOut o = svc.tick(401);
+  TEST_ASSERT_TRUE(o.ended);
+  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDeadman), static_cast<int>(o.end.reason));
+}
+
+void test_go_again_changes_the_speed_at_once() {
+  MotionService svc;
+  svc.startGo(3, 40, 7, 0);
+  svc.onTelemetry(encAt(0), 20);
+  svc.startGo(3, 60, 7, 30);  // GO / SPEED while going: the same GO, a new speed
+  const MotionOut o = svc.onTelemetry(encAt(5), 40);
+  TEST_ASSERT_EQUAL_INT8(60, o.v);
+  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kGo), static_cast<int>(svc.kind()));
+}
+
+void test_a_move_takes_over_from_go_without_an_end() {
+  MotionService svc;
+  svc.startGo(3, 40, 7, 0);
+  svc.onTelemetry(encAt(0), 20);
+  svc.startMove(3, encAt(100), 20, 40, motion::kCpmDefault, 7, 30);
+  TEST_ASSERT_EQUAL(static_cast<int>(PrimKind::kMove), static_cast<int>(svc.kind()));
+  const MotionOut o = svc.onTelemetry(encAt(110), 40);
+  TEST_ASSERT_FALSE(o.ended);
+  TEST_ASSERT_EQUAL_INT8(40, o.v);
+}
+
+void test_link_hold_sends_stop_and_ends_done() {
+  LinkOwner owner;
+  MotionLink ml{owner};
+  owner.begin();
+  owner.onWsOpen(kFd);
+  owner.onSynced(true);
+  Actions a;
+  owner.onDrive(kFd, 0, a);
+  TEST_ASSERT_FALSE(ml.hold(10).event);  // nothing going
+  ml.service().startGo(kFd, 40, 5, 0);
+  ml.onTelemetry(encAt(0), 20);
+  const LinkOut o = ml.hold(30);
+  TEST_ASSERT_TRUE(o.event);
+  TEST_ASSERT_TRUE(o.stop);
+  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kDone), static_cast<int>(o.end.reason));
+  TEST_ASSERT_FALSE(ml.service().active());
+}
+
+void test_link_uno_stall_ends_a_go() {
+  LinkOwner owner;
+  MotionLink ml{owner};
+  owner.begin();
+  owner.onWsOpen(kFd);
+  owner.onSynced(true);
+  Actions a;
+  owner.onDrive(kFd, 0, a);
+  ml.service().startGo(kFd, 40, 5, 0);
+  ml.onTelemetry(encAt(0), 20);
+  const LinkOut o = ml.onEvent(MotionEvent::kBodyStall, 30);  // the Uno's own stuck-track check
+  TEST_ASSERT_TRUE(o.event);
+  TEST_ASSERT_TRUE(o.stop);
+  TEST_ASSERT_EQUAL(static_cast<int>(EndReason::kStalled), static_cast<int>(o.end.reason));
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_router_lite_runs_turn_and_rate_locally);
@@ -1118,5 +1227,12 @@ int main() {
   RUN_TEST(test_link_uno_stall_ends_an_arc_not_a_pivot);
   RUN_TEST(test_rate_clamp_to_minimum_spin);
   RUN_TEST(test_service_pivot_correction_keeps_the_same_track);
+  RUN_TEST(test_go_drives_straight_at_its_speed_until_held);
+  RUN_TEST(test_go_ends_blocked_as_soon_as_the_brake_engages);
+  RUN_TEST(test_go_stops_without_a_refresh_within_300_ms);
+  RUN_TEST(test_go_again_changes_the_speed_at_once);
+  RUN_TEST(test_a_move_takes_over_from_go_without_an_end);
+  RUN_TEST(test_link_hold_sends_stop_and_ends_done);
+  RUN_TEST(test_link_uno_stall_ends_a_go);
   return UNITY_END();
 }
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion`
Expected: FAIL to compile — `no member named 'startGo' in 'cq::core::MotionService'`.

- [ ] **Step 3: Implement**

```diff
diff --git a/firmware/esp32/src/core/motion_link.cpp b/firmware/esp32/src/core/motion_link.cpp
index e90ba69..dd51849 100644
--- a/firmware/esp32/src/core/motion_link.cpp
+++ b/firmware/esp32/src/core/motion_link.cpp
@@ -52,7 +52,8 @@ LinkOut MotionLink::onEvent(MotionEvent e, uint32_t now_ms) {
   if (!m_svc.active()) return LinkOut{};
   const int fd = m_svc.ownerFd();
   if (e == MotionEvent::kBodyStall || e == MotionEvent::kBodyEncoderFault) {
-    if (m_svc.kind() != PrimKind::kMove && m_svc.kind() != PrimKind::kArc) return LinkOut{};  // spin / pivot / rate: own checks
+    const PrimKind k = m_svc.kind();
+    if (k != PrimKind::kMove && k != PrimKind::kArc && k != PrimKind::kGo) return LinkOut{};  // spin / pivot / rate: own checks
     const EndReason r = e == MotionEvent::kBodyStall ? EndReason::kStalled : EndReason::kEncoderFault;
     return finished(m_svc.cancel(r), fd, now_ms);  // the Uno already stopped both tracks; STOP while owner
   }
@@ -61,5 +62,11 @@ LinkOut MotionLink::onEvent(MotionEvent e, uint32_t now_ms) {
   return eventOnly(m_svc.cancel(reasonOf(e)));  // the STOP is already on its way, or the owner's own frames follow
 }
 
+LinkOut MotionLink::hold(uint32_t now_ms) {
+  if (m_svc.kind() != PrimKind::kGo) return LinkOut{};
+  const int fd = m_svc.ownerFd();
+  return finished(m_svc.hold(), fd, now_ms);
+}
+
 }  // namespace core
 }  // namespace cq
diff --git a/firmware/esp32/src/core/motion_link.h b/firmware/esp32/src/core/motion_link.h
index 08984d2..bbde6d2 100644
--- a/firmware/esp32/src/core/motion_link.h
+++ b/firmware/esp32/src/core/motion_link.h
@@ -50,6 +50,7 @@ class MotionLink {
   }
   LinkOut tick(uint32_t now_ms);
   LinkOut onEvent(MotionEvent e, uint32_t now_ms);
+  LinkOut hold(uint32_t now_ms);  // R20: a GO ends as done, STOP while its client still owns the body
 
  private:
   LinkOut finished(const MotionOut& o, int fd, uint32_t now_ms);
diff --git a/firmware/esp32/src/core/motion_service.cpp b/firmware/esp32/src/core/motion_service.cpp
index 0681691..7768735 100644
--- a/firmware/esp32/src/core/motion_service.cpp
+++ b/firmware/esp32/src/core/motion_service.cpp
@@ -1,6 +1,7 @@
 #include "core/motion_service.h"
 
 #include "core/motion_config.h"
+#include "cq_protocol.h"
 
 namespace cq {
 namespace core {
@@ -116,6 +117,31 @@ MotionOut MotionService::finish(EndReason why) {
 
 MotionOut MotionService::cancel(EndReason why) { return active() ? finish(why) : MotionOut{}; }
 
+void MotionService::startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms) {
+  if (m_kind != PrimKind::kGo || m_fd != fd) {
+    m_kind = PrimKind::kGo;
+    m_fd = fd;
+    m_id = id;
+    m_last_telem_ms = now_ms;
+  }
+  m_go_v = static_cast<int8_t>(speed_pct > 100 ? 100 : speed_pct);
+  m_go_update_ms = now_ms;
+}
+
+MotionOut MotionService::hold() { return m_kind == PrimKind::kGo ? finish(EndReason::kDone) : MotionOut{}; }
+
+// R20: no target - ends at once when the Uno's obstacle brake engages (the tank has already stopped).
+MotionOut MotionService::stepGo(const MotionTelem& t, uint32_t now_ms) {
+  if (now_ms - m_go_update_ms > motion::kRateDeadmanMs) return finish(EndReason::kDeadman);
+  if (((t.flags >> static_cast<unsigned>(proto::FastFlagBit::kBlocked)) & 1u) != 0) {
+    return finish(EndReason::kBlocked);
+  }
+  MotionOut o;
+  o.send = true;
+  o.v = m_go_v;  // w = 0: the Uno's heading hold keeps it straight
+  return o;
+}
+
 MotionOut MotionService::stepTurn(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms) {
   const TurnStep s = m_turn.step(yaw_cdeg, rate_ddps, now_ms);
   if (s.coast_ms != 0) m_coast_reading = s.coast_ms;
@@ -189,6 +215,7 @@ MotionOut MotionService::onTelemetry(const MotionTelem& t, uint32_t now_ms) {
     case PrimKind::kMove: o = stepMove(t, now_ms); break;
     case PrimKind::kArc: o = stepArc(t, now_ms); break;
     case PrimKind::kPivot: o = stepPivot(t.yaw_cdeg, t.rate_ddps, now_ms); break;
+    case PrimKind::kGo: o = stepGo(t, now_ms); break;
     default: o = stepRate(t.rate_ddps, now_ms); break;
   }
   m_last_telem_ms = now_ms;
@@ -201,6 +228,7 @@ MotionOut MotionService::tick(uint32_t now_ms) {
   if (m_kind == PrimKind::kRate && now_ms - m_rate_update_ms > motion::kRateDeadmanMs) {
     return finish(EndReason::kDeadman);
   }
+  if (m_kind == PrimKind::kGo && now_ms - m_go_update_ms > motion::kRateDeadmanMs) return finish(EndReason::kDeadman);
   return MotionOut{};
 }
 
diff --git a/firmware/esp32/src/core/motion_service.h b/firmware/esp32/src/core/motion_service.h
index 4af24f0..edaa4b5 100644
--- a/firmware/esp32/src/core/motion_service.h
+++ b/firmware/esp32/src/core/motion_service.h
@@ -14,7 +14,7 @@
 namespace cq {
 namespace core {
 
-enum class PrimKind : uint8_t { kNone, kTurn, kRate, kMove, kArc, kPivot };
+enum class PrimKind : uint8_t { kNone, kTurn, kRate, kMove, kArc, kPivot, kGo };  // kGo: bridge LLD R20
 enum class EndReason : uint8_t {
   kDone,
   kCancelled,
@@ -54,6 +54,13 @@ class MotionService {
                  uint16_t coast_ms = motion::kCoastDefaultMs);
   void driveRate(int fd, int8_t v, int16_t rate_ddps, uint32_t now_ms);  // starts or updates the stream
   MotionOut cancel(EndReason why);
+  // R20: GO - drive straight on at speed_pct (w = 0, the Uno's heading hold) until held, replaced or blocked; the
+  // owner refreshes it (keepGo) at least every kRateDeadmanMs. startGo while going: a new speed, refreshed.
+  void startGo(int fd, uint8_t speed_pct, int32_t id, uint32_t now_ms);
+  void keepGo(uint32_t now_ms) {
+    if (m_kind == PrimKind::kGo) m_go_update_ms = now_ms;
+  }
+  MotionOut hold();  // ends a GO as done (v = 0); nothing going: nothing
   void startMove(int fd, const MotionTelem& t, int16_t dist_cm, uint8_t speed_pct, uint16_t cpm, int32_t id,
                  uint32_t now_ms, uint16_t roll_mm = 0);
   void startArc(int fd, const MotionTelem& t, int32_t delta_cdeg, uint16_t radius_cm, uint8_t speed_pct, uint16_t cpm,
@@ -78,6 +85,7 @@ class MotionService {
  private:
   MotionOut finish(EndReason why);
   MotionOut stepTurn(uint16_t yaw_cdeg, int16_t rate_ddps, uint32_t now_ms);
+  MotionOut stepGo(const MotionTelem& t, uint32_t now_ms);
   MotionOut stepRate(int16_t rate_ddps, uint32_t now_ms);
   bool rateDiverged(bool saturated, int16_t rate_ddps, uint32_t now_ms);
   bool rateStalled(bool saturated, int16_t rate_ddps, uint32_t now_ms);
@@ -95,6 +103,8 @@ class MotionService {
   int8_t m_pivot_dir = 1;  // +1 left pivot (left track still), -1 right pivot
   uint16_t m_coast_reading = 0;  // R18: kept until the turn / pivot ends
   int8_t m_rate_v = 0;
+  int8_t m_go_v = 0;            // R20
+  uint32_t m_go_update_ms = 0;  // R20: the last startGo / keepGo
   int16_t m_rate_target = 0;
   bool m_rate_limited = false;
   uint32_t m_rate_update_ms = 0;
diff --git a/firmware/esp32/src/core/ws_json_util.cpp b/firmware/esp32/src/core/ws_json_util.cpp
index aa28dda..3caa894 100644
--- a/firmware/esp32/src/core/ws_json_util.cpp
+++ b/firmware/esp32/src/core/ws_json_util.cpp
@@ -18,7 +18,10 @@ size_t fmt(char* out, size_t cap, const char* f, ...) {
   return (n < 0 || static_cast<size_t>(n) >= cap) ? 0 : static_cast<size_t>(n);
 }
 
-const char* primName(PrimKind k) { return k == PrimKind::kRate ? "rate" : k == PrimKind::kMove ? "move" : "turn"; }
+const char* primName(PrimKind k) {
+  if (k == PrimKind::kGo) return "go";  // R20
+  return k == PrimKind::kRate ? "rate" : k == PrimKind::kMove ? "move" : "turn";
+}
 
 }  // namespace detail
 
```

- [ ] **Step 4: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion`
Expected: PASS, 73 test cases.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core/motion_service.h firmware/esp32/src/core/motion_service.cpp firmware/esp32/src/core/motion_link.h firmware/esp32/src/core/motion_link.cpp firmware/esp32/src/core/ws_json_util.cpp firmware/esp32/test/test_motion/test_main.cpp
git commit -m "feat(bridge): R20 the GO primitive - drive on, kept alive, held, blocked at once

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Logo drives the GO (app wiring)

**Files:**
- Modify: `firmware/esp32/src/app/motion_controller.{h,cpp}`, `firmware/esp32/src/app/logo_controller.{h,cpp}`, `firmware/esp32/src/app/logo_actions.cpp`, `firmware/esp32/src/net/link_task.cpp`
- Test: `firmware/esp32/test/test_logo_controller/test_main.cpp`, `firmware/esp32/test/test_motion_controller/test_main.cpp`

**Interfaces:**
- Consumes: (Task 1) `ActKind::kGo` / `kHold`, `RunController::going()`; (Task 2) `startGo`, `keepGo`, `MotionLink::hold`.
- Produces:
  - `MotionController::keepLogoGo(uint32_t now)`
  - `LogoController::Hooks::keep_go` (`void (*)(uint32_t)`, last member)
  - `LogoController::keepGoing`, `speedWhileGoing` (private)

- [ ] **Step 1: Write the failing tests**

```diff
diff --git a/firmware/esp32/test/test_logo_controller/test_main.cpp b/firmware/esp32/test/test_logo_controller/test_main.cpp
index de29548..2e13f36 100644
--- a/firmware/esp32/test/test_logo_controller/test_main.cpp
+++ b/firmware/esp32/test/test_logo_controller/test_main.cpp
@@ -38,6 +38,7 @@ bool g_queue_ok = true;
 std::vector<logo::Action> g_motions;
 std::vector<core::AckedOp> g_ops;
 int g_stops = 0, g_releases = 0;
+int g_go_keeps = 0;  // Tank Logo LLD §22: keep_go calls
 uint32_t g_now = 1000;
 
 void unoSends(const uint8_t* f, size_t n, uint32_t now) { link->onUartBytes(f, n, now); }
@@ -83,6 +84,7 @@ void hooks() {
     return g_queue_ok;
   };
   h.cpm = [] { return static_cast<uint16_t>(7514); };
+  h.keep_go = [](uint32_t) { ++g_go_keeps; };
   lc->setHooks(h);
 }
 
@@ -130,6 +132,7 @@ void setUp() {
   g_motions.clear();
   g_ops.clear();
   g_stops = g_releases = 0;
+  g_go_keeps = 0;
 }
 void tearDown() {}
 
@@ -342,6 +345,39 @@ void test_pose_ten_times_a_second() {
   TEST_ASSERT_TRUE(sent(4, "\"x_cm\":0.0,\"y_cm\":0.0,\"heading\":0"));
 }
 
+// Tank Logo LLD §22: GO goes to the motion layer and the program runs on; every tick keeps the GO alive; HOLD is
+// sent and ends the keeping; SPEED while going sends GO again at the new speed.
+void test_go_is_kept_alive_every_tick_until_hold() {
+  run("GO WAIT 1 HOLD BEEP");
+  ticks(1);
+  TEST_ASSERT_EQUAL(1, g_motions.size());
+  TEST_ASSERT_EQUAL(logo::ActKind::kGo, g_motions[0].kind);
+  TEST_ASSERT_EQUAL(40, g_motions[0].speed_pct);
+  ticks(100);  // the program waits; the GO drives on
+  TEST_ASSERT_TRUE(g_go_keeps >= 99);
+  TEST_ASSERT_EQUAL(1, g_motions.size());
+  ticks(500);  // the WAIT is over: HOLD
+  TEST_ASSERT_EQUAL(2, g_motions.size());
+  TEST_ASSERT_EQUAL(logo::ActKind::kHold, g_motions[1].kind);
+  const int kept = g_go_keeps;
+  ticks(10);
+  TEST_ASSERT_EQUAL(kept, g_go_keeps);  // held: nothing more to keep alive
+}
+
+void test_speed_while_going_sends_go_again() {
+  run("GO SPEED 60 WAIT 1");
+  ticks(3);
+  TEST_ASSERT_EQUAL(2, g_motions.size());
+  TEST_ASSERT_EQUAL(logo::ActKind::kGo, g_motions[1].kind);
+  TEST_ASSERT_EQUAL(60, g_motions[1].speed_pct);
+}
+
+void test_speed_without_go_sends_nothing() {
+  run("SPEED 60 WAIT 1");
+  ticks(3);
+  TEST_ASSERT_EQUAL(0, g_motions.size());
+}
+
 void test_arc_speed_note() {
   run("SPEED 100 ARC RIGHT 90 40");
   ticks(1);
@@ -372,5 +408,8 @@ int main() {
   RUN_TEST(test_step_mode);
   RUN_TEST(test_pose_ten_times_a_second);
   RUN_TEST(test_arc_speed_note);
+  RUN_TEST(test_go_is_kept_alive_every_tick_until_hold);
+  RUN_TEST(test_speed_while_going_sends_go_again);
+  RUN_TEST(test_speed_without_go_sends_nothing);
   return UNITY_END();
 }
diff --git a/firmware/esp32/test/test_motion_controller/test_main.cpp b/firmware/esp32/test/test_motion_controller/test_main.cpp
index b396802..c8a6f45 100644
--- a/firmware/esp32/test/test_motion_controller/test_main.cpp
+++ b/firmware/esp32/test/test_motion_controller/test_main.cpp
@@ -206,6 +206,32 @@ void test_begin_loads_the_gyro_scale_and_calib_shows_it() {
   TEST_ASSERT_FALSE(mc->setGyroScale(20000));
 }
 
+// Tank Logo LLD §22 / bridge LLD R20: a Logo GO starts the GO primitive (DRIVE frames at its speed), the program
+// keeps it alive, HOLD ends it with a STOP; GO needs no encoders.
+void test_logo_go_and_hold() {
+  syncedWithEncoders();
+  core::Actions took;
+  owner.onDrive(3, 50, took);  // logo_run takes the drive first (Tank Logo LLD §20.5)
+  logo::Action go;
+  go.kind = logo::ActKind::kGo;
+  go.speed_pct = 40;
+  core::BridgeReason why = core::BridgeReason::kBusy;
+  TEST_ASSERT_TRUE(mc->startLogo(3, go, 100, why));
+  TEST_ASSERT_EQUAL(static_cast<int>(core::PrimKind::kGo), static_cast<int>(mc->view().kind));
+  mc->keepLogoGo(350);
+  mc->onTelemetry(360);  // 260 ms since the start, 10 since the keep: still going
+  TEST_ASSERT_TRUE(mc->active());
+  wire.drain(*tx);
+  wire.clear();
+  logo::Action hold;
+  hold.kind = logo::ActKind::kHold;
+  TEST_ASSERT_TRUE(mc->startLogo(3, hold, 400, why));
+  TEST_ASSERT_FALSE(mc->active());
+  wire.drain(*tx);
+  TEST_ASSERT_EQUAL(1, wire.count(FT::kStop));
+  TEST_ASSERT_TRUE(has(4, "\"prim\":\"go\",\"ok\":true,\"reason\":\"done\""));  // to every page
+}
+
 void test_calib_reply_has_counts_and_roll_diagnostics() {
   mc->sendCalib(3);
   const std::string j = fakes::lastTo(3);
@@ -313,6 +339,7 @@ int main() {
   RUN_TEST(test_a_done_turn_saves_the_learned_coast);
   RUN_TEST(test_a_cancelled_turn_learns_nothing);
   RUN_TEST(test_begin_loads_the_gyro_scale_and_calib_shows_it);
+  RUN_TEST(test_logo_go_and_hold);
   RUN_TEST(test_calib_reply_has_counts_and_roll_diagnostics);
   RUN_TEST(test_logo_move_starts_without_a_reply_and_its_end_reaches_the_program);
   RUN_TEST(test_a_page_s_own_primitive_end_does_not_reach_the_program);
```

- [ ] **Step 2: Run to verify they fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_controller -f test_motion_controller`
Expected: FAIL to compile — `no member named 'keep_go' in 'cq::app::LogoController::Hooks'`, `no member named 'keepLogoGo' in 'cq::app::MotionController'`.

- [ ] **Step 3: Implement**

```diff
diff --git a/firmware/esp32/src/app/logo_actions.cpp b/firmware/esp32/src/app/logo_actions.cpp
index a054577..97d629f 100644
--- a/firmware/esp32/src/app/logo_actions.cpp
+++ b/firmware/esp32/src/app/logo_actions.cpp
@@ -42,9 +42,10 @@ core::AckedOp lookOp(const logo::Action& a) {
   return o;
 }
 
-bool isMotion(logo::ActKind k) {
+bool isMotion(logo::ActKind k) {  // + GO / HOLD (LLD §22): the motion layer carries them out too
   return k == logo::ActKind::kMove || k == logo::ActKind::kTurn || k == logo::ActKind::kArc ||
-         k == logo::ActKind::kPivot || k == logo::ActKind::kHome;
+         k == logo::ActKind::kPivot || k == logo::ActKind::kHome || k == logo::ActKind::kGo ||
+         k == logo::ActKind::kHold;
 }
 
 }  // namespace
@@ -60,10 +61,18 @@ void LogoController::carry(const logo::Action& a, uint32_t now) {
       m_out.all(core::jsonLogoSay(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), a.text));
       m_hold_until = now + kSayPauseMs;
       return;
-    default: return;  // SPEED, WAIT, WAIT UNTIL: the RunController's own
+    default: return speedWhileGoing(a, now);  // SPEED, WAIT, WAIT UNTIL: the RunController's own
   }
 }
 
+// LLD §22: SPEED while a GO drives on - GO again at the new speed (the bridge changes it at once).
+void LogoController::speedWhileGoing(const logo::Action& a, uint32_t now) {
+  if (a.kind != logo::ActKind::kSpeed || !m_rc.going()) return;
+  logo::Action go = a;
+  go.kind = logo::ActKind::kGo;
+  motion(go, now);
+}
+
 // A move / turn / arc / pivot / HOME as the program's primitive; refused → the program ends (7021 / 7034).
 void LogoController::motion(const logo::Action& a, uint32_t now) {
   note(a);
diff --git a/firmware/esp32/src/app/logo_controller.cpp b/firmware/esp32/src/app/logo_controller.cpp
index 838715e..e7472ae 100644
--- a/firmware/esp32/src/app/logo_controller.cpp
+++ b/firmware/esp32/src/app/logo_controller.cpp
@@ -76,12 +76,18 @@ void LogoController::tick(uint32_t now) {
                                                                           : core::MotionEvent::kRelease), now);
   }
   pose(now);
+  keepGoing(now);
   if (!pendingSent() || m_wait_ack) return;  // the program goes on at the Uno's ACK (§102)
   if (m_tone_i < m_tone_n && static_cast<int32_t>(now - m_tone_next) >= 0) return toneChunk(now);
   if (static_cast<int32_t>(now - m_hold_until) < 0) return;  // SAY's pause
   handle(m_rc.tick(telem(), now), now);
 }
 
+// LLD §22: a GO drives on only while the program keeps it alive (the bridge stops it after 300 ms without).
+void LogoController::keepGoing(uint32_t now) {
+  if (m_rc.going() && m_h.keep_go != nullptr) m_h.keep_go(now);
+}
+
 void LogoController::onPrimitiveEnd(const core::PrimitiveEnd& e, uint32_t now) {
   if (m_running) handle(m_rc.onPrimitiveEnd(e), now);
 }
diff --git a/firmware/esp32/src/app/logo_controller.h b/firmware/esp32/src/app/logo_controller.h
index 7bbd16d..ff03d02 100644
--- a/firmware/esp32/src/app/logo_controller.h
+++ b/firmware/esp32/src/app/logo_controller.h
@@ -38,6 +38,7 @@ class LogoController {
     void (*release)(int fd, uint32_t now) = nullptr;
     bool (*uno_request)(const core::AckedOp& op) = nullptr;  // false: the queue is full (tried again next tick)
     uint16_t (*cpm)() = nullptr;
+    void (*keep_go)(uint32_t now) = nullptr;  // LLD §22: a GO drives on while the program keeps it alive
   };
   static constexpr uint32_t kSayPauseMs = 200;  // design-notes §102
   static constexpr uint32_t kPoseMs = 100;      // logo_pose at 10 Hz
@@ -59,6 +60,8 @@ class LogoController {
   bool running() const { return m_running; }
 
  private:
+  void keepGoing(uint32_t now);
+  void speedWhileGoing(const logo::Action& a, uint32_t now);
   // logo_controller.cpp
   bool runRefused(int fd, int32_t id, uint32_t prog, uint32_t now);
   void handle(const logo::RunOut& o, uint32_t now);
diff --git a/firmware/esp32/src/app/motion_controller.cpp b/firmware/esp32/src/app/motion_controller.cpp
index 40fd5e7..4a3ab29 100644
--- a/firmware/esp32/src/app/motion_controller.cpp
+++ b/firmware/esp32/src/app/motion_controller.cpp
@@ -105,11 +105,13 @@ bool MotionController::logoRefused(const logo::Action& a, core::BridgeReason& wh
 
 bool MotionController::startLogo(int fd, const logo::Action& a, uint32_t now, core::BridgeReason& why) {
   if (logoRefused(a, why)) return false;
-  m_roll.cancel();  // §G6: a new primitive
+  if (a.kind != logo::ActKind::kHold) m_roll.cancel();  // §G6: a new primitive
   const core::TankSnapshot& snap = m_link.snapshot();
   core::MotionService& svc = m_motion.service();
   const uint8_t spin = a.speed_pct < core::motion::kTurnWMin ? core::motion::kTurnWMin : a.speed_pct;
-  switch (a.kind) {  // VARIANT POINT: Logo motions (Tank Logo LLD §10)
+  switch (a.kind) {  // VARIANT POINT: Logo motions (Tank Logo LLD §10, §22)
+    case logo::ActKind::kGo: svc.startGo(fd, a.speed_pct, kLogoPrimitive, now); return true;
+    case logo::ActKind::kHold: run(m_motion.hold(now), now); return true;  // a GO ends as done, with a STOP
     case logo::ActKind::kMove:
       svc.startMove(fd, telemOf(snap), a.dist_cm, a.speed_pct, m_cpm, kLogoPrimitive, now, m_roll_mm);
       return true;
diff --git a/firmware/esp32/src/app/motion_controller.h b/firmware/esp32/src/app/motion_controller.h
index 98c01fb..a15e91a 100644
--- a/firmware/esp32/src/app/motion_controller.h
+++ b/firmware/esp32/src/app/motion_controller.h
@@ -46,6 +46,7 @@ class MotionController {
   // why (kUnavailable: no encoders or an encoder fault; kBodyOffline; kBusy: OTA) when it cannot start.
   bool startLogo(int fd, const logo::Action& a, uint32_t now, core::BridgeReason& why);
   bool ownedBy(int fd) const;
+  void keepLogoGo(uint32_t now) { m_motion.service().keepGo(now); }  // Tank Logo LLD §22: each tick while going
   bool active() const { return m_motion.service().active(); }
   void cancelRollOn() { m_roll.cancel(); }  // driving again / a new primitive
   uint16_t& cpm() { return m_cpm; }
diff --git a/firmware/esp32/src/net/link_task.cpp b/firmware/esp32/src/net/link_task.cpp
index f6e78c5..aeb3197 100644
--- a/firmware/esp32/src/net/link_task.cpp
+++ b/firmware/esp32/src/net/link_task.cpp
@@ -222,6 +222,7 @@ void wireLogo() {
   h.release = [](int fd, uint32_t now) { g_drive.onRelease(fd, -1, now); };
   h.uno_request = [](const core::AckedOp& op) { return g_uno.request(op); };
   h.cpm = [] { return g_motion.cpm(); };
+  h.keep_go = [](uint32_t now) { g_motion.keepLogoGo(now); };
   g_logo->setHooks(h);
   g_uno.setLogoDone([](uint8_t status) { g_logo->onUnoDone(status, millis()); });
 }
```

- [ ] **Step 4: Run to verify they pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native`
Expected: PASS, 636 test cases.
Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge`
Expected: SUCCESS (flash ~44.3 %).
Run: `uv run --with lizard==1.24.1 python scripts/size_check.py`
Expected: no output, exit 0. (`carry` and `tick` stay ≤ 10 through `speedWhileGoing` / `keepGoing`.)

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/app firmware/esp32/src/net/link_task.cpp firmware/esp32/test/test_logo_controller/test_main.cpp firmware/esp32/test/test_motion_controller/test_main.cpp
git commit -m "feat(bridge): Logo GO and HOLD - kept alive every tick, SPEED while going, HOLD with a STOP

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: On the tank — flash and floor check (owner present)

**Safety:** a **floor** check, so the tank drives. Ask "stand or floor?" before each drive and say what moves. Keep STOP within reach. Only the bridge is flashed.

- [ ] **Step 1: Flash the bridge over the air**

```bash
cd firmware/esp32
export CQ_OTA_PASSWORD="$(sed -n 's/.*CQ_OTA_PASSWORD[^"]*"\([^"]*\)".*/\1/p' include/secrets.h)"
~/.platformio/penv/bin/pio run -e bridge_ota -t upload --upload-port 192.168.31.167 2>&1 | grep -vF -- "$CQ_OTA_PASSWORD" | tail -3
unset CQ_OTA_PASSWORD
```
Expected: `SUCCESS`.

- [ ] **Step 2: Drive until something is close**

`GO  WAIT UNTIL DISTANCE < 30  HOLD  BEEP` (`tools/smoke/logo.py run`), with a box about 1 m ahead.
Expected: the tank drives smoothly and stops about 30 cm (plus a few cm of glide) before the box, then beeps; it finishes.

- [ ] **Step 3: Avoid walls without stopping**

`FOREVER [ GO  WAIT UNTIL BLOCKED?  BACK 10  RIGHT 90 ]` in the same room as `avoid_walls` (design-notes §118). Stopped after about 40 s with Ctrl-C (STOP), and recorded with `td_record.py`.
Expected: no stop-start cycle between walls; at each wall it stops, backs off and turns; no bumps.

- [ ] **Step 4: The `/logo` page**

The owner opens `/logo`. The word help lists `GO` and `HOLD` (Explorer). The preview of `GO WAIT 1 HOLD RIGHT 90` draws a dashed 1 m line.

---

### Task 5: Docs as built

**Files:** `docs/lld/tank-logo-lld.md` (§22.7 as built + floor results), `docs/lld/esp32-bridge-lld.md` (R20 as built), `docs/design-notes.md` (the floor results with how they were read), `CLAUDE.md` (plan list, Next).

- [ ] **Step 1: Write the as-built notes and the floor results;** decide with the owner whether `avoid_walls` is rewritten with `GO` (Tank Logo LLD §22.6).
- [ ] **Step 2: Commit**

```bash
git add docs CLAUDE.md
git commit -m "docs: Plan N5 GO and HOLD - as built and floor results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

