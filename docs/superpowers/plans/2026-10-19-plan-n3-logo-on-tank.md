# Tank Logo N3 — on the Tank Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tank Logo programs checked, run and saved on the tank: the N2 runner wired into the bridge (Plan P structure), program text over HTTP, control and events over the WebSocket, saved programs on a new `logo` flash area, a laptop smoke tool, then stand and floor checks.

**Architecture:** Pure parts stay in `src/logo/` and `src/core/` (host-tested): one check as a function, the check box, odometry, the event JSON. The link task gets a `LogoController` (`src/app/`, host-tested with fakes) that runs the N2 `RunController` each tick and carries out its actions through MotionController and UnoChannel. A worker task on core 0 does every `/logo/*` HTTP request as an async job, so the WebSocket (STOP included) is never held up.

**Tech Stack:** C++17 / PlatformIO (`env:native` Unity + `env:bridge` ESP32-S3, Arduino 3 / ESP-IDF 5), esp_http_server async requests, LittleFS, FreeRTOS; Python 3.12 smoke tool (uv, pytest, ruff, pyright strict).

**Spec:** `docs/lld/tank-logo-lld.md` §20 (approved), with §8–§13 and §19; design-notes §100–§103; bridge LLD R4.6 (primitives and ownership), R14–R15 (one master, controllers and ports).

**Branch:** `plan-n3` from `main`. Hardware steps (Tasks 12–13) only with the owner present, STOP within reach.

## Global Constraints

- Coding standards §1–§3: no recursion, no heap after start-up (boot-time PSRAM allocations only), no `virtual`, fixed capacities; size check (file ≤ 400 lines, function ≤ 40 NLOC, CCN ≤ 10) runs in the pre-commit hook.
- Safety is unchanged: every move is an ordinary motion-service primitive under all Uno rules; every ending stops the tank — but a STOP frame only while the program still owns the drive (bridge LLD R4.6: never STOP a phone or the IR remote that took over).
- Program text never goes through the link task; the web server task never runs a check (async jobs on the worker).
- Generated files are never edited by hand (`cd logo && uv run logogen write`, `cd protocol && uv run protogen write`); this plan changes none.
- Hardware: tank on the stand (tracks off the ground) unless a step says floor; ask "stand or floor?" before every run and say what will move.
- Never print WiFi SSIDs or passwords or the OTA password; the bridge has no authentication (home network, development only).
- Commits: Conventional Commits, one task each, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Run `pio test` with explicit `-f` lists.

## Rules this plan decides (owner confirms at plan review; recorded in Task 14)

1. **`/logo/*` requests are async jobs on the worker task** (esp_http_server `httpd_req_async_handler_begin`): the web server only hands the request over and goes back to the WebSocket; the worker reads the body, checks / saves / loads, replies and completes the request. One job at a time; a second gets `503 busy` at once. (§20.2's "the web server waits up to 2 s" is replaced: nothing waits on the web server task.)
2. **A running program keeps the drive** with `LinkOwner::keepDriving` every tick (as a running primitive does): no 5 s handback during `WAIT`, lights or sounds; `keepDriving` failing = a takeover (7045) or the body gone (7034).
3. **The ending's STOP goes only while the program still owns the drive**; after a takeover the program just ends (no STOP for the new driver).
4. Bridge-only refusal codes **3016 CHECK_AGAIN** and **3017 NOT_RUNNING** (like 3011 STOPPED; the protocol YAML is unchanged, so the Uno build is untouched).
5. **LOOK** degrees → the Uno's servo angles: 900 = centre, right and up are the *smaller* angles (Uno Lite `CamLite`). `BEEP` = `SOUND BLIP`.
6. **PLAY**: the LogoController sends each TONE frame (≤ 10 notes) when the previous one has played (a new TONE replaces the tune playing); the program waits for the whole tune.
7. Uno alarms: `batt_critical` → 7042; `tilted` / `lifted` → 7043 (`batt_low` is a warning only).
8. **Memory as built:** check workspace ≈ 330 KB PSRAM (the token list and node pool are bigger than §20.6 estimated), worker stack 16 KB (the checker keeps ≈ 6 KB on its stack); running program + Vm ≈ 36 KB in PSRAM, falling back to internal RAM so the LogoController always exists.
9. `upload_source.py` reads the `source` offset from the partition table (it was hard-coded); **flash order** for the new layout: `-t upload` (partitions + app) first, then `-t upload_source`.
10. The Log gets a `logo` kind (a program ended: code, line, name; a request refused). The smoke tool pings once a second (the bridge closes a client silent for 3 s).

## Review Focus

1. **STOP while a check or a big reply is going** (an 8 KB `FOREVER` program, a 40 KB path) → the STOP is not delayed: `/logo/*` never runs on the web server task — rule 1 in `net/logo_http.cpp` (Task 10); measured on the stand (Task 12 step 5).
2. **Another phone or the IR remote takes over while a program runs** → the program ends (7045) and sends no STOP to the new driver — `test_another_device_taking_the_drive_ends_it_without_stopping_them` (Task 8).
3. **A program that only waits or blinks for more than 5 s** keeps the drive — `test_a_waiting_program_keeps_the_drive_every_tick` (Task 8).
4. **The Uno never answers** a light / sound / camera request → the program ends with 7034 (never hangs); a full request queue is tried again — `test_logo_no_answer_and_body_offline` (Task 6), `test_no_answer_ends_with_7034_a_refusal_goes_on`, `test_a_full_queue_is_tried_again` (Task 8).
5. **A heavy check** (8 KB, a `FOREVER` hitting the 200 000-instruction cap, 2000 path points) on core 0 never trips the 3 s core-0 idle watchdog — the worker logs each check's time; measured on the stand (Task 12 step 4).

## File map

| Task | Creates | Changes |
|---|---|---|
| 1 N2 follow-ups | — | `src/logo/{program,vm,vm_ops,compiler,compiler_stmt,run_controller,simulator}`, tests of compiler / run / sim |
| 2 One check + its reply | `src/logo/check_job.*`, `check_json.*`, `test/test_logo_check` | — |
| 3 Check box, names, examples | `src/logo/check_box.*`, `program_names.*`, `test/test_logo_box` | — |
| 4 Odometry | `src/logo/odometry.*`, `test/test_logo_odometry` | — |
| 5 Events + refusals + Log kind | `src/core/ws_json_logo.cpp` | `src/core/{ws_json.h, ws_json_reply.cpp, event_log.h, ws_json_tank.cpp}`, `test/test_ws_json` |
| 6 Uno requests | — | `src/core/{body_link.*, acked_ops.h}`, `src/app/uno_channel.*`, `test/test_uno_channel` |
| 7 Motion hooks | — | `src/app/motion_controller.*`, `test/test_motion_controller` |
| 8 LogoController | `src/app/logo_controller.h`, `logo_controller.cpp`, `logo_actions.cpp`, `test/test_logo_controller` | — |
| 9 Commands + routes + alarms | — | `src/core/ws_codec*`, `src/app/{drive_controller.cpp, command_router.*, uno_event_router.*}`, their tests |
| 10 On the tank (ESP32) | `src/net/{logo_worker,logo_http,program_store}.*` | `partitions_cq8m.csv`, `src/net/{http_server.cpp, link_task.*}`, `src/main.cpp`, `scripts/{upload_source.py, make_release.sh}`, `web/help.html` |
| 11 Smoke tool | `tools/smoke/logo.py`, `tools/smoke/tests/test_logo_tool.py` | — |
| 12 Stand checks | — | design-notes §104 |
| 13 Floor checks (after the road-wheel fix) | — | design-notes §104 |
| 14 Docs | — | Tank Logo LLD §20.9, bridge LLD R16, design-notes, CLAUDE.md |

(`src/…`, `test/…`, `scripts/…`, `web/…` are under `firmware/esp32/`.)


---

### Task 1: N2 follow-ups (a WAIT UNTIL deadline per wait, the `ok` flag, a deeper value stack)

**Files:** `src/logo/{program.h, program.cpp, vm.h, vm_ops.h, vm_ops.cpp, compiler.cpp, compiler_stmt.cpp, run_controller.h, run_controller.cpp, simulator.h, simulator.cpp}`; tests `test_logo_compiler`, `test_logo_run`, `test_logo_sim`.

**Interfaces:**
- Consumes: the N2 runner.
- Produces: `Program::ok` (set by `Compiler::compile`); `bool RunController::start(...)` (false: not compiled OK); `Simulator::plan` answers 7010 for a not-OK program; new instructions `kUntilOver` / `kPop` (a WAIT UNTIL keeps its start time on the value stack); `Sensor::kUntilOver` and `Sensors::until_over` are gone; `Vm::kMaxValues = 1024`.

- [ ] **Step 1: Write the failing tests**


Change `firmware/esp32/test/test_logo_compiler/test_main.cpp` (unified diff against the previous task):
````diff
@@ -20,7 +20,6 @@ Vm g_vm;
 char g_trace[2048];
 VmStatus g_end;
 int g_effects;
-int g_until_ticks = 3;  // WAIT UNTIL gives up after this many ticks (the host's 60 s)
 
 void record(const Effect& e) {
   if (e.kind == EffectKind::kTick) return;
@@ -35,11 +34,13 @@ void record(const Effect& e) {
   ++g_effects;
 }
 
-// The host's side of an effect: FORWARD brings the wall 10 cm closer; WAIT UNTIL ticks count towards its limit.
+// The host's side of an effect: FORWARD brings the wall 10 cm closer; each WAIT UNTIL tick lets 20 s pass.
 void host(const Effect& e, Sensors& s, int& ticks) {
   if (e.kind == EffectKind::kForward) s.distance_milli -= 10000;
-  if (e.kind == EffectKind::kUntilStart) ticks = 0;
-  if (e.kind == EffectKind::kTick) s.until_over = ++ticks >= g_until_ticks;
+  if (e.kind == EffectKind::kTick) {
+    s.timer_milli += 20000;
+    ++ticks;
+  }
 }
 
 // False when the program was refused before running (Diagnostics has why).
@@ -209,6 +210,30 @@ void test_reporter_ending_with_a_command_call_is_7031() {
   TEST_ASSERT_EQUAL(7031, g_vm.error().code);
 }
 
+// Plan N3 Task 1: every WAIT UNTIL has its own 60 s - also one inside a word that another WAIT UNTIL's test calls.
+void test_wait_until_inside_a_reporter_has_its_own_deadline() {
+  TEST_ASSERT_TRUE(run("TO F WAIT UNTIL DISTANCE < 0 OUTPUT FALSE END\nWAIT UNTIL F BEEP"));
+  TEST_ASSERT_EQUAL_STRING("WAIT UNTIL | WAIT UNTIL | BEEP", g_trace);
+  TEST_ASSERT_EQUAL(VmStatus::kHalt, g_end);
+}
+
+// Plan N3 Task 1: 100 calls of a 3-input word reach the 100-calls limit (7011) before the value stack runs out.
+void test_deep_calls_with_inputs_are_7011_not_7014() {
+  TEST_ASSERT_TRUE(run("TO D :a :b :c D :a :b :c BEEP END D 1 2 3"));
+  TEST_ASSERT_EQUAL(7011, g_vm.error().code);
+}
+
+// Plan N3 Task 1: a compiled program says so; a failed compile does not.
+void test_program_ok_flag() {
+  TEST_ASSERT_TRUE(run("FD 10"));
+  TEST_ASSERT_TRUE(g_prog.ok);
+  static char src[6000];
+  src[0] = '\0';
+  for (int i = 0; i < 1100; ++i) strcat(src, "BEEP ");
+  TEST_ASSERT_FALSE(run(src));
+  TEST_ASSERT_FALSE(g_prog.ok);
+}
+
 int main(int, char**) {
   UNITY_BEGIN();
   RUN_TEST(test_effects_in_order_with_their_values);
@@ -230,5 +255,8 @@ int main(int, char**) {
   RUN_TEST(test_reporter_without_output_and_output_inside_loops);
   RUN_TEST(test_stop_inside_a_word_ends_the_program);
   RUN_TEST(test_reporter_ending_with_a_command_call_is_7031);
+  RUN_TEST(test_wait_until_inside_a_reporter_has_its_own_deadline);
+  RUN_TEST(test_deep_calls_with_inputs_are_7011_not_7014);
+  RUN_TEST(test_program_ok_flag);
   return UNITY_END();
 }
````

Change `firmware/esp32/test/test_logo_run/test_main.cpp` (unified diff against the previous task):
````diff
@@ -212,6 +212,13 @@ void test_run_time_refusal_ends_with_its_line_and_speed_applies() {
   TEST_ASSERT_EQUAL(1, g_last.end_line);
 }
 
+// Plan N3 Task 1: a program that did not compile completely never starts.
+void test_a_failed_compile_never_starts() {
+  TEST_ASSERT_TRUE(load("FD 10"));
+  g_prog.ok = false;
+  TEST_ASSERT_FALSE(g_rc.start(g_prog, RunConfig(), g_tel, g_now));
+}
+
 int main(int, char**) {
   UNITY_BEGIN();
   RUN_TEST(test_square_finishes);
@@ -226,5 +233,6 @@ int main(int, char**) {
   RUN_TEST(test_step_mode);
   RUN_TEST(test_home_heading_distance_obstacle);
   RUN_TEST(test_run_time_refusal_ends_with_its_line_and_speed_applies);
+  RUN_TEST(test_a_failed_compile_never_starts);
   return UNITY_END();
 }
````

Change `firmware/esp32/test/test_logo_sim/test_main.cpp` (unified diff against the previous task):
````diff
@@ -103,6 +103,15 @@ void test_run_time_error_ends_the_path() {
   TEST_ASSERT_EQUAL(2, g_path.n);
 }
 
+// Plan N3 Task 1: a program that did not compile completely is never planned.
+void test_a_failed_compile_is_not_planned() {
+  plan("FD 10");
+  g_prog.ok = false;
+  g_r = Simulator(g_vm).plan(g_prog, g_path);
+  TEST_ASSERT_EQUAL(7010, g_r.code);
+  TEST_ASSERT_EQUAL(0, g_path.n);
+}
+
 int main(int, char**) {
   UNITY_BEGIN();
   RUN_TEST(test_square_corners_within_1_mm);
@@ -112,5 +121,6 @@ int main(int, char**) {
   RUN_TEST(test_limits_cut_the_path_short);
   RUN_TEST(test_simulated_time);
   RUN_TEST(test_run_time_error_ends_the_path);
+  RUN_TEST(test_a_failed_compile_is_not_planned);
   return UNITY_END();
 }
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_compiler -f test_logo_vm -f test_logo_run -f test_logo_sim -f test_logo_running -f test_logo_effects`
Expected: FAIL — compile error `no member named 'ok' in 'cq::logo::Program'`.

- [ ] **Step 3: Implement**


Change `firmware/esp32/src/logo/compiler.cpp` (unified diff against the previous task):
````diff
@@ -50,7 +50,8 @@ bool Compiler::compile(const Node* program, bool handles_obstacles, Program& out
   while (m_ns > 0 && !m_failed) {
     if (stepNode(m_st[m_ns - 1])) --m_ns;
   }
-  return !m_failed;
+  out.ok = !m_failed;
+  return out.ok;
 }
 
 bool Compiler::stepNode(Work& w) {
````

Change `firmware/esp32/src/logo/compiler_stmt.cpp` (unified diff against the previous task):
````diff
@@ -126,20 +126,24 @@ bool Compiler::stepWhile(Work& w) {
   return true;
 }
 
-// LINE EFFECT(start, 60 s); L: test JNZ end; EFFECT(tick); SENSOR(over); JZ L; end:
+// LINE EFFECT(start) SENSOR(timer); L: test JNZ end; EFFECT(tick); UNTIL_OVER; JZ L; end: POP
+// The start time stays on the value stack, so every WAIT UNTIL has its own 60 s (also one inside a word that a
+// WAIT UNTIL test calls).
 bool Compiler::stepWaitUntil(Work& w) {
   if (w.step == 0) {
     line(w.n);
     emit(OpCode::kEffect, static_cast<uint8_t>(EffectKind::kUntilStart), 0);
+    emit(OpCode::kSensor, static_cast<uint8_t>(Sensor::kTimer));
     w.p2 = m_out->n_code;
     w.step = 1;
     return !child(w);
   }
   w.p1 = emit(OpCode::kJnz, kCondUntil);
   emit(OpCode::kEffect, static_cast<uint8_t>(EffectKind::kTick), 0);
-  emit(OpCode::kSensor, static_cast<uint8_t>(Sensor::kUntilOver));
+  emit(OpCode::kUntilOver);
   emit(OpCode::kJz, kCondUntil, w.p2);
   patch(w.p1);
+  emit(OpCode::kPop);
   return true;
 }
````

Change `firmware/esp32/src/logo/program.cpp` (unified diff against the previous task):
````diff
@@ -74,6 +74,7 @@ void Program::clear() {
   n_code = n_consts = n_items = n_text = 0;
   n_procs = 0;
   handles_obstacles = false;
+  ok = false;
 }
 
 int16_t Program::addWord(const char* s, uint16_t len) {
````

Change `firmware/esp32/src/logo/program.h` (unified diff against the previous task):
````diff
@@ -19,10 +19,10 @@ struct Value {
 
 enum class OpCode : uint8_t {
   kHalt, kLine, kPushN, kPushB, kPushC, kArg, kOp, kSensor, kCallB, kJmp, kJz, kJnz,
-  kLoopInit, kLoopNext, kCall, kTCall, kRet, kRetV, kEffect, kStop, kFail,
+  kLoopInit, kLoopNext, kCall, kTCall, kRet, kRetV, kEffect, kStop, kFail, kUntilOver, kPop,
   kCount
 };
-enum class Sensor : uint8_t { kDistance, kObstacle, kBlocked, kHeading, kTilted, kBattery, kTimer, kUntilOver };
+enum class Sensor : uint8_t { kDistance, kObstacle, kBlocked, kHeading, kTilted, kBattery, kTimer };
 enum class Builtin : uint8_t { kRandom, kRemainder, kAbs, kRound, kRepcount };
 enum class EffectKind : uint8_t {
   kForward, kBack, kRight, kLeft, kSpeed, kWait, kLed, kBeep, kSound, kLook, kHome,
@@ -69,6 +69,7 @@ struct Program {
   ProcEntry procs[gen::kMaxProcs];
   uint8_t n_procs = 0;
   bool handles_obstacles = false;
+  bool ok = false;  // compiled completely (Plan N3: a failed compile is never run)
 
   void clear();
   int16_t addWord(const char* s, uint16_t len);  // -1 when full
````

Change `firmware/esp32/src/logo/run_controller.cpp` (unified diff against the previous task):
````diff
@@ -62,17 +62,19 @@ bool waits(ActKind k) { return k == ActKind::kWait || k == ActKind::kTone || k =
 
 }  // namespace
 
-void RunController::start(const Program& p, const RunConfig& cfg, const RunTelem& t, uint32_t now_ms) {
+bool RunController::start(const Program& p, const RunConfig& cfg, const RunTelem& t, uint32_t now_ms) {
+  if (!p.ok) return false;
   m_p = &p;
   m_cfg = cfg;
   m_vm.load(p, cfg.seed);
   m_vm.setStepping(cfg.step);
   m_state = RunState::kRunning;
   m_start = now_ms;
-  m_wake = m_until = 0;
+  m_wake = 0;
   m_start_yaw = t.yaw_cdeg;
   m_speed = 40;
   m_blocked = false;
+  return true;
 }
 
 RunOut RunController::tick(const RunTelem& t, uint32_t now_ms) {
@@ -134,7 +136,6 @@ void RunController::carry(const Action& a, const RunTelem& t, uint32_t now_ms, R
     return;
   }
   if (a.kind == ActKind::kSpeed) m_speed = a.speed_pct;
-  if (a.kind == ActKind::kUntilStart) m_until = now_ms + static_cast<uint32_t>(gen::kWaitS) * 1000u;
   if (waits(a.kind)) {
     m_wake = now_ms + (a.kind == ActKind::kTick ? 1 : a.ms);
     m_state = RunState::kWaiting;
@@ -151,7 +152,6 @@ Sensors RunController::sensors(const RunTelem& t, uint32_t now_ms) const {
   s.tilted = t.tilted;
   s.battery_milli = static_cast<int32_t>(t.battery_pct) * 1000;
   s.timer_milli = static_cast<int32_t>(now_ms - m_start);
-  s.until_over = static_cast<int32_t>(now_ms - m_until) >= 0;
   return s;
 }
````

Change `firmware/esp32/src/logo/run_controller.h` (unified diff against the previous task):
````diff
@@ -49,7 +49,7 @@ struct RunOut {
 class RunController {
  public:
   explicit RunController(Vm& vm) : m_vm(vm) {}
-  void start(const Program& p, const RunConfig& cfg, const RunTelem& t, uint32_t now_ms);
+  bool start(const Program& p, const RunConfig& cfg, const RunTelem& t, uint32_t now_ms);  // false: not compiled OK
   RunOut tick(const RunTelem& t, uint32_t now_ms);
   RunOut onPrimitiveEnd(const core::PrimitiveEnd& e);  // the move / turn / arc / pivot it asked for ended
   RunOut onMotionEvent(core::MotionEvent e);           // STOP, release, page gone … (also between motions)
@@ -72,7 +72,6 @@ class RunController {
   RunState m_state = RunState::kIdle;
   uint32_t m_start = 0;
   uint32_t m_wake = 0;
-  uint32_t m_until = 0;
   uint16_t m_start_yaw = 0;
   uint8_t m_speed = 40;
   bool m_blocked = false;
````

Change `firmware/esp32/src/logo/simulator.cpp` (unified diff against the previous task):
````diff
@@ -28,13 +28,17 @@ SimResult Simulator::plan(const Program& p, PathBuffer& out, uint32_t seed) {
   m_out = &out;
   out.n = 0;
   m_pose = Pose();
-  m_ms = m_until = 0;
+  m_ms = 0;
   m_speed = 40;
   m_line = 0;
   m_full = false;
+  SimResult r;
+  if (!p.ok) {  // never plan a program that did not compile completely
+    r.code = 7010;
+    return r;
+  }
   m_vm.load(p, seed);
   point(0);
-  SimResult r;
   while (step(r)) {
   }
   r.sim_ms = m_ms;
@@ -78,7 +82,6 @@ bool Simulator::effect(SimResult& r) {
 void Simulator::apply(const Action& a) {
   if (moves(a)) return;
   if (a.kind == ActKind::kSpeed) m_speed = a.speed_pct;
-  if (a.kind == ActKind::kUntilStart) m_until = m_ms + static_cast<uint32_t>(gen::kWaitS) * 1000u;
   if (a.kind == ActKind::kWait || a.kind == ActKind::kTone) m_ms += a.ms;
   if (a.kind == ActKind::kTick) m_ms += kTickMs;
 }
@@ -145,7 +148,6 @@ Sensors Simulator::sensors() const {
   Sensors s;
   s.heading_milli = static_cast<int32_t>(lround(wrap180(m_pose.h) * 1000.0));
   s.timer_milli = static_cast<int32_t>(m_ms);
-  s.until_over = m_ms >= m_until;
   return s;
 }
````

Change `firmware/esp32/src/logo/simulator.h` (unified diff against the previous task):
````diff
@@ -65,7 +65,6 @@ class Simulator {
   PathBuffer* m_out = nullptr;
   Pose m_pose;
   uint32_t m_ms = 0;
-  uint32_t m_until = 0;
   uint8_t m_speed = 40;
   uint16_t m_line = 0;
   bool m_full = false;
````

Change `firmware/esp32/src/logo/vm.h` (unified diff against the previous task):
````diff
@@ -21,7 +21,6 @@ struct Sensors {
   bool tilted = false;
   int32_t battery_milli = 100000;   // %
   int32_t timer_milli = 0;          // seconds since the start
-  bool until_over = false;          // WAIT UNTIL has waited its 60 s
 };
 
 enum class VmStatus : uint8_t { kYield, kLine, kEffect, kHalt, kStop, kError };
@@ -42,7 +41,7 @@ struct RunError {
 class Vm {
  public:
   static constexpr uint8_t kMaxFrames = gen::kNesting + 1;  // the program's own frame + 100 calls (7011)
-  static constexpr uint16_t kMaxValues = 256;
+  static constexpr uint16_t kMaxValues = 1024;  // 100 calls of an 8-input word reach 7011 first (N3)
   static constexpr uint8_t kMaxLoops = 128;
 
   void load(const Program& p, uint32_t seed);
````

Change `firmware/esp32/src/logo/vm_ops.cpp` (unified diff against the previous task):
````diff
@@ -12,7 +12,7 @@ using OpFn = void (*)(Vm&, const Instr&);
 constexpr OpFn kOps[] = {VmOps::halt,     VmOps::line,     VmOps::pushN, VmOps::pushB, VmOps::pushC, VmOps::arg,
                          VmOps::op,       VmOps::sensor,   VmOps::callB, VmOps::jmp,   VmOps::jz,    VmOps::jnz,
                          VmOps::loopInit, VmOps::loopNext, VmOps::call,  VmOps::tcall, VmOps::ret,   VmOps::retV,
-                         VmOps::effect,   VmOps::stop,     VmOps::fail};
+                         VmOps::effect,   VmOps::stop,     VmOps::fail,  VmOps::untilOver, VmOps::pop};
 static_assert(sizeof kOps / sizeof kOps[0] == static_cast<size_t>(OpCode::kCount), "one handler per OpCode");
 
 // The word a condition belongs to (Instr::a of kJz / kJnz), for 7012.
@@ -114,7 +114,7 @@ void VmOps::op(Vm& vm, const Instr& in) {
 void VmOps::sensor(Vm& vm, const Instr& in) {
   const Sensors& s = *vm.m_sensors;
   const Sensor k = static_cast<Sensor>(in.a);
-  if (k != Sensor::kUntilOver) vm.m_sensor_read = true;
+  vm.m_sensor_read = true;
   switch (k) {  // VARIANT POINT: sensors (words.yaml level 2 reporters)
     case Sensor::kDistance: vm.push(num(s.distance_milli)); return;
     case Sensor::kObstacle: vm.push(boolean(s.obstacle)); return;
@@ -123,7 +123,6 @@ void VmOps::sensor(Vm& vm, const Instr& in) {
     case Sensor::kTilted: vm.push(boolean(s.tilted)); return;
     case Sensor::kBattery: vm.push(num(s.battery_milli)); return;
     case Sensor::kTimer: vm.push(num(s.timer_milli)); return;
-    case Sensor::kUntilOver: vm.push(boolean(s.until_over)); return;
   }
 }
 
@@ -256,5 +255,13 @@ void VmOps::fail(Vm& vm, const Instr& in) {
   vm.error(static_cast<uint16_t>(in.n), a, 1);
 }
 
+// UNTIL_OVER: the WAIT UNTIL whose start time (TIMER) is on top of the stack has waited its 60 s (it stays there).
+void VmOps::untilOver(Vm& vm, const Instr&) {
+  const int32_t start = vm.m_stack[vm.m_sp - 1].n;
+  vm.push(boolean(vm.m_sensors->timer_milli - start >= static_cast<int32_t>(gen::kWaitS) * 1000));
+}
+
+void VmOps::pop(Vm& vm, const Instr&) { vm.pop(); }
+
 }  // namespace logo
 }  // namespace cq
````

Change `firmware/esp32/src/logo/vm_ops.h` (unified diff against the previous task):
````diff
@@ -30,6 +30,8 @@ struct VmOps {
   static void effect(Vm& vm, const Instr& in);
   static void stop(Vm& vm, const Instr& in);
   static void fail(Vm& vm, const Instr& in);
+  static void untilOver(Vm& vm, const Instr& in);
+  static void pop(Vm& vm, const Instr& in);
 
   // helpers
   static bool popNumber(Vm& vm, const char* word, int32_t& out);
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_compiler -f test_logo_vm -f test_logo_run -f test_logo_sim -f test_logo_running -f test_logo_effects`
Expected: `95 test cases: 95 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/logo firmware/esp32/test
git commit -m "feat(logo): N2 follow-ups - a WAIT UNTIL deadline per wait, Program ok flag, value stack 1024

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 2: One check and its reply

**Files:** `src/logo/check_job.h/.cpp`, `src/logo/check_json.h/.cpp`; `test/test_logo_check`.

**Interfaces:**
- Consumes: lexer, Parser, Checker, Compiler, Simulator.
- Produces: `struct CheckSpace` (one check's whole workspace; the caller owns it); `bool checkProgram(const char* src, uint16_t len, const Caps&, CheckSpace&)`; `constexpr uint16_t kSourceInbox = 8193`; `class JsonOut { JsonOut(Flush, void* ctx); put; putf; text; finish; }` (chunks of ≤ 512 B); `bool writeCheckReply(const CheckSpace&, const char* src, bool runnable, uint32_t prog, JsonOut&)`.

- [ ] **Step 1: Write the failing tests**


Create `firmware/esp32/test/test_logo_check/test_main.cpp`:
````cpp
// Plan N3 Task 2: one check (read → check → compile → plan the path) and its JSON reply in chunks (LLD §20.2, §20.4).
#include <string.h>
#include <unity.h>

#include <string>

#include "logo/check_json.h"

using namespace cq::logo;

namespace {
CheckSpace g_s;
std::string g_json;
int g_chunks = 0;

bool sink(void*, const char* data, size_t len) {
  g_json.append(data, len);
  ++g_chunks;
  return true;
}

bool check(const char* src, bool encoders = true) {
  Caps caps;
  caps.encoders = encoders;
  const bool ok = checkProgram(src, static_cast<uint16_t>(strlen(src)), caps, g_s);
  g_json.clear();
  g_chunks = 0;
  JsonOut out(sink, nullptr);
  TEST_ASSERT_TRUE(writeCheckReply(g_s, src, ok, 17, out));
  return ok;
}
bool has(const char* needle) { return g_json.find(needle) != std::string::npos; }
}  // namespace

void setUp() {}
void tearDown() {}

void test_a_good_program_has_its_path_and_number() {
  TEST_ASSERT_TRUE(check("REPEAT 4 [ FD 50 RT 90 ]"));
  TEST_ASSERT_TRUE(g_s.program.ok);
  TEST_ASSERT_EQUAL(9, g_s.path.n);
  TEST_ASSERT_TRUE(has("{\"ok\":true,\"prog\":17,\"diags\":[],\"words\":[],\"handles_obstacles\":false,"));
  TEST_ASSERT_TRUE(has("\"path\":[[0,0,0,0,0],[0,500,0,1,0],[0,500,90,1,0],"));
  TEST_ASSERT_TRUE(has("\"cut_short\":false,\"sim_error\":null}"));
}

void test_errors_are_listed_and_nothing_runs() {
  TEST_ASSERT_FALSE(check("FD 600\nFOO"));
  TEST_ASSERT_FALSE(g_s.compiled);
  TEST_ASSERT_TRUE(has("{\"ok\":false,\"prog\":17,\"diags\":[{"));  // the reader's errors, then the checker's
  TEST_ASSERT_TRUE(has("\"code\":7025,\"line\":1,\"col\":4,\"msg\":"));
  TEST_ASSERT_TRUE(has("\"code\":7001,\"line\":2,\"col\":1"));
  TEST_ASSERT_TRUE(has("\"path\":[]"));
}

void test_your_words_and_obstacles() {
  TEST_ASSERT_TRUE(check("to square :s repeat 4 [ fd :s rt 90 ] end\nTO SIDE OUTPUT 5 END\nsquare side IF BLOCKED? [ BEEP ]"));
  TEST_ASSERT_TRUE(has("\"words\":[{\"name\":\"SQUARE\",\"inputs\":1,\"gives\":false},{\"name\":\"SIDE\",\"inputs\":0,\"gives\":true}],"));
  TEST_ASSERT_TRUE(has("\"handles_obstacles\":true"));
}

void test_no_encoders_refuses_moves_before_running() {
  TEST_ASSERT_FALSE(check("FD 10", false));
  TEST_ASSERT_TRUE(has("\"code\":7021"));
  TEST_ASSERT_TRUE(check("RT 90 BEEP", false));
}

void test_too_long_is_7010() {
  static char big[8300];
  memset(big, ' ', sizeof big - 1);
  big[sizeof big - 1] = '\0';
  TEST_ASSERT_FALSE(check(big));
  TEST_ASSERT_TRUE(has("\"code\":7010"));
}

void test_a_long_path_goes_out_in_chunks_and_is_cut_short() {
  TEST_ASSERT_TRUE(check("FOREVER [ FD 10 RT 1 ]"));
  TEST_ASSERT_TRUE(g_chunks > 20);
  TEST_ASSERT_TRUE(has("\"cut_short\":true"));
  TEST_ASSERT_EQUAL('{', g_json.front());
  TEST_ASSERT_EQUAL('}', g_json.back());
}

void test_a_run_time_error_in_the_plan() {
  TEST_ASSERT_TRUE(check("FD 10\nFD 5 / 0"));
  TEST_ASSERT_TRUE(has("\"sim_error\":{\"code\":7013,\"line\":2,\"msg\":\"Can't divide by zero on line 2.\"}"));
}

void test_text_is_escaped() {
  g_json.clear();
  JsonOut out(sink, nullptr);
  out.text("a\"b\\c\n", 6);
  out.finish();
  TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\u000a\"", g_json.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_good_program_has_its_path_and_number);
  RUN_TEST(test_errors_are_listed_and_nothing_runs);
  RUN_TEST(test_your_words_and_obstacles);
  RUN_TEST(test_no_encoders_refuses_moves_before_running);
  RUN_TEST(test_too_long_is_7010);
  RUN_TEST(test_a_long_path_goes_out_in_chunks_and_is_cut_short);
  RUN_TEST(test_a_run_time_error_in_the_plan);
  RUN_TEST(test_text_is_escaped);
  return UNITY_END();
}
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_check`
Expected: FAIL — `fatal error: 'logo/check_json.h' file not found`.

- [ ] **Step 3: Implement**


Create `firmware/esp32/src/logo/check_job.cpp`:
````cpp
#include "logo/check_job.h"

#include "logo/generated/words.h"

namespace cq {
namespace logo {

bool checkProgram(const char* src, uint16_t len, const Caps& caps, CheckSpace& s) {
  s.diags.clear();
  s.pool.reset();
  s.program.clear();
  s.path.n = 0;
  s.result = CheckResult();
  s.sim = SimResult();
  s.compiled = false;
  if (len > gen::kSourceBytes) {
    const DiagArg a[] = {{"what", "more than 8192 letters"}};
    s.diags.add(7010, 1, 1, a, 1);
    return false;
  }
  lex(src, len, s.toks, s.diags);
  Node* prog = s.parser.parse(src, s.toks);
  s.result = Checker(s.parser, src, s.toks, s.diags).check(prog, caps);
  if (!s.result.ok) return false;
  Compiler* c = new (s.compiler_mem) Compiler(s.parser, src, s.toks, s.diags);
  s.compiled = c->compile(prog, s.result.handles_obstacles, s.program);
  c->~Compiler();
  if (!s.compiled) return false;
  s.sim = Simulator(s.vm).plan(s.program, s.path);
  return true;
}

}  // namespace logo
}  // namespace cq
````

Create `firmware/esp32/src/logo/check_job.h`:
````cpp
// One check of a program (Tank Logo LLD §20.2): lex → parse → check → compile → plan the path, in one workspace
// the caller owns (N3: the logo worker task's, in PSRAM, allocated once). Pure; host-tested.
#pragma once

#include <new>
#include <stdint.h>

#include "logo/checker.h"
#include "logo/compiler.h"
#include "logo/lexer.h"
#include "logo/parser.h"
#include "logo/program.h"
#include "logo/simulator.h"
#include "logo/vm.h"

namespace cq {
namespace logo {

constexpr uint16_t kSourceInbox = 8193;  // words.yaml source_bytes + 1: a longer text is refused (7010) unread

struct CheckSpace {
  static constexpr uint16_t kNodes = 4096;
  static constexpr uint16_t kPathPoints = 2000;

  CheckSpace() : pool(nodes, kNodes), parser(pool, diags) {}
  TokenList toks;
  Node nodes[kNodes];
  NodePool pool;
  Diagnostics diags;
  Parser parser;
  alignas(Compiler) unsigned char compiler_mem[sizeof(Compiler)];  // built per check (it keeps the source)
  Program program;
  Vm vm;
  PathPoint points[kPathPoints];
  PathBuffer path{points, kPathPoints, 0};
  CheckResult result;
  SimResult sim;
  bool compiled = false;
};

// Reads, checks, compiles and (when it compiled) plans the path. `src` must stay valid while the reply is written
// (words and messages point into it). True when the program can run (no error, compiled).
bool checkProgram(const char* src, uint16_t len, const Caps& caps, CheckSpace& s);

}  // namespace logo
}  // namespace cq
````

Create `firmware/esp32/src/logo/check_json.cpp`:
````cpp
#include "logo/check_json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace cq {
namespace logo {
namespace {

void diags(const Diagnostics& d, JsonOut& out) {
  out.put("\"diags\":[");
  for (uint8_t i = 0; i < d.count(); ++i) {
    const Diag& x = d.at(i);
    out.putf("%s{\"code\":%u,\"line\":%u,\"col\":%u,\"msg\":", i > 0 ? "," : "", x.code, x.line, x.col);
    out.text(x.msg, strlen(x.msg));
    out.putf(",\"warning\":%s}", x.warning ? "true" : "false");
  }
  out.put("],");
}

// Your words: name (as written, upper case), inputs, gives a value.
void words(const CheckSpace& s, const char* src, JsonOut& out) {
  out.put("\"words\":[");
  bool first = true;
  for (uint8_t i = 0; i < s.parser.procCount(); ++i) {
    const ProcInfo& p = s.parser.proc(i);
    if (!p.valid) continue;
    char name[40];
    tokenText(src, s.toks.t[p.name_tok], name, sizeof name);
    for (char* c = name; *c != '\0'; ++c) *c = (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 32) : *c;
    out.put(first ? "{\"name\":" : ",{\"name\":");
    out.text(name, strlen(name));
    out.putf(",\"inputs\":%u,\"gives\":%s}", p.n_inputs, p.gives_value ? "true" : "false");
    first = false;
  }
  out.put("],");
}

void path(const CheckSpace& s, JsonOut& out) {
  out.put("\"path\":[");
  for (uint16_t i = 0; i < s.path.n; ++i) {
    const PathPoint& p = s.path.pts[i];
    out.putf("%s[%ld,%ld,%d,%u,%u]", i > 0 ? "," : "", static_cast<long>(p.x_mm), static_cast<long>(p.y_mm),
             p.heading_deg, p.line, p.uncertain ? 1u : 0u);
  }
  out.put("],");
}

void simError(const SimResult& r, JsonOut& out) {
  if (r.code == 0) {
    out.put("\"sim_error\":null");
    return;
  }
  out.putf("\"sim_error\":{\"code\":%u,\"line\":%u,\"msg\":", r.code, r.line);
  out.text(r.msg, strlen(r.msg));
  out.put("}");
}

}  // namespace

void JsonOut::raw(const char* s, size_t n) {
  while (n > 0) {
    if (m_n == sizeof m_buf) {
      m_ok = m_flush(m_ctx, m_buf, m_n) && m_ok;
      m_n = 0;
    }
    const size_t k = n < sizeof m_buf - m_n ? n : sizeof m_buf - m_n;
    memcpy(m_buf + m_n, s, k);
    m_n += k;
    s += k;
    n -= k;
  }
}

void JsonOut::put(const char* s) { raw(s, strlen(s)); }

void JsonOut::putf(const char* fmt, ...) {
  char tmp[96];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n > 0) raw(tmp, static_cast<size_t>(n) < sizeof tmp ? static_cast<size_t>(n) : sizeof tmp - 1);
}

void JsonOut::text(const char* s, size_t n) {
  raw("\"", 1);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    char esc[8];
    if (c == '"' || c == '\\') {
      esc[0] = '\\';
      esc[1] = static_cast<char>(c);
      raw(esc, 2);
    } else if (c < 0x20) {
      snprintf(esc, sizeof esc, "\\u%04x", c);
      raw(esc, 6);
    } else {
      raw(s + i, 1);
    }
  }
  raw("\"", 1);
}

bool JsonOut::finish() {
  if (m_n > 0) m_ok = m_flush(m_ctx, m_buf, m_n) && m_ok;
  m_n = 0;
  return m_ok;
}

bool writeCheckReply(const CheckSpace& s, const char* src, bool runnable, uint32_t prog, JsonOut& out) {
  out.putf("{\"ok\":%s,\"prog\":%lu,", runnable ? "true" : "false", static_cast<unsigned long>(prog));
  diags(s.diags, out);
  words(s, src, out);
  out.putf("\"handles_obstacles\":%s,", s.result.handles_obstacles ? "true" : "false");
  path(s, out);
  out.putf("\"cut_short\":%s,", s.sim.cut_short ? "true" : "false");
  simError(s.sim, out);
  out.put("}");
  return out.finish();
}

}  // namespace logo
}  // namespace cq
````

Create `firmware/esp32/src/logo/check_json.h`:
````cpp
// The reply to POST /logo/check (Tank Logo LLD §20.4), written in chunks so no buffer has to hold a whole planned
// path (≈ 40 KB). Pure; the web server gives it a sink that sends each chunk.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/check_job.h"

namespace cq {
namespace logo {

class JsonOut {
 public:
  using Flush = bool (*)(void* ctx, const char* data, size_t len);
  JsonOut(Flush flush, void* ctx) : m_flush(flush), m_ctx(ctx) {}
  void put(const char* s);
  void putf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void text(const char* s, size_t n);  // a JSON string, quoted and escaped
  bool finish();                       // the last chunk; false when any chunk could not be sent

 private:
  void raw(const char* s, size_t n);
  Flush m_flush;
  void* m_ctx;
  char m_buf[512];
  size_t m_n = 0;
  bool m_ok = true;
};

// {"ok","prog","diags":[…],"words":[…],"handles_obstacles","path":[[x_mm,y_mm,heading,line,uncertain]…],
//  "cut_short","sim_error"} for the check in `s` of `src`.
bool writeCheckReply(const CheckSpace& s, const char* src, bool runnable, uint32_t prog, JsonOut& out);

}  // namespace logo
}  // namespace cq
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_check`
Expected: `8 test cases: 8 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/logo firmware/esp32/test/test_logo_check
git commit -m "feat(logo): one check (read, check, compile, plan) and its chunked JSON reply

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 3: The check box, program names and the built-in examples

**Files:** `src/logo/check_box.h/.cpp`, `src/logo/program_names.h/.cpp`; `test/test_logo_box`.

**Interfaces:**
- Consumes: `Program`, `checkProgram` (examples test).
- Produces: `class CheckBox { explicit CheckBox(Program& checked); uint32_t put(const Program* compiled, const char* name); Take take(uint32_t prog, Program& out, char* name, size_t cap) const; current(); }` with `enum class Take { kOk, kCheckAgain }`; `bool programName(const char* in, char* out, size_t cap)`; `kNameMax = 24`, `kMaxPrograms = 32`; `exampleCount()`, `example(i)`, `findExample(lower_name)` (first_square, avoid_walls, look_around).

- [ ] **Step 1: Write the failing tests**


Create `firmware/esp32/test/test_logo_box/test_main.cpp`:
````cpp
// Plan N3 Task 3: the checked program box (numbers, take refusals), program names, the built-in examples.
#include <string.h>
#include <unity.h>

#include "logo/check_box.h"
#include "logo/check_job.h"
#include "logo/program_names.h"

using namespace cq::logo;

namespace {
Program g_checked, g_running, g_compiled;
CheckBox g_box(g_checked);
CheckSpace g_space;
}  // namespace

void setUp() {}
void tearDown() {}

void test_numbers_go_up_and_only_the_current_ok_program_runs() {
  g_compiled.clear();
  g_compiled.ok = true;
  g_compiled.n_code = 3;
  const uint32_t a = g_box.put(&g_compiled, "square");
  char name[CheckBox::kNameCap];
  TEST_ASSERT_EQUAL(CheckBox::Take::kOk, g_box.take(a, g_running, name, sizeof name));
  TEST_ASSERT_EQUAL(3, g_running.n_code);
  TEST_ASSERT_EQUAL_STRING("square", name);
  const uint32_t b = g_box.put(&g_compiled, "");
  TEST_ASSERT_EQUAL(a + 1, b);
  TEST_ASSERT_EQUAL(CheckBox::Take::kCheckAgain, g_box.take(a, g_running, name, sizeof name));  // replaced
  TEST_ASSERT_EQUAL(CheckBox::Take::kCheckAgain, g_box.take(0, g_running, name, sizeof name));
}

void test_a_check_with_errors_gets_a_number_but_never_runs() {
  const uint32_t n = g_box.put(nullptr, "");
  char name[CheckBox::kNameCap];
  TEST_ASSERT_EQUAL(CheckBox::Take::kCheckAgain, g_box.take(n, g_running, name, sizeof name));
  g_compiled.ok = false;
  const uint32_t m = g_box.put(&g_compiled, "");
  TEST_ASSERT_EQUAL(CheckBox::Take::kCheckAgain, g_box.take(m, g_running, name, sizeof name));
}

void test_program_names() {
  char out[25];
  TEST_ASSERT_TRUE(programName("My_Square-2", out, sizeof out));
  TEST_ASSERT_EQUAL_STRING("my_square-2", out);
  TEST_ASSERT_FALSE(programName("", out, sizeof out));
  TEST_ASSERT_FALSE(programName("has space", out, sizeof out));
  TEST_ASSERT_FALSE(programName("../etc", out, sizeof out));
  TEST_ASSERT_FALSE(programName("abcdefghijklmnopqrstuvwxy", out, sizeof out));  // 25
  TEST_ASSERT_TRUE(programName("abcdefghijklmnopqrstuvwx", out, sizeof out));    // 24
}

void test_every_example_checks_ok() {
  TEST_ASSERT_EQUAL(3, exampleCount());
  for (uint8_t i = 0; i < exampleCount(); ++i) {
    const Example& e = example(i);
    char lower[25];
    TEST_ASSERT_TRUE_MESSAGE(programName(e.name, lower, sizeof lower), e.name);
    Caps caps;
    TEST_ASSERT_TRUE_MESSAGE(checkProgram(e.src, static_cast<uint16_t>(strlen(e.src)), caps, g_space), e.name);
  }
  TEST_ASSERT_NOT_NULL(findExample("avoid_walls"));
  TEST_ASSERT_NULL(findExample("nothing"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_numbers_go_up_and_only_the_current_ok_program_runs);
  RUN_TEST(test_a_check_with_errors_gets_a_number_but_never_runs);
  RUN_TEST(test_program_names);
  RUN_TEST(test_every_example_checks_ok);
  return UNITY_END();
}
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_box`
Expected: FAIL — `fatal error: 'logo/check_box.h' file not found`.

- [ ] **Step 3: Implement**


Create `firmware/esp32/src/logo/check_box.cpp`:
````cpp
#include "logo/check_box.h"

#include <stdio.h>

namespace cq {
namespace logo {

uint32_t CheckBox::put(const Program* compiled, const char* name) {
  m_ok = compiled != nullptr && compiled->ok;
  if (m_ok) m_p = *compiled;
  snprintf(m_name, sizeof m_name, "%s", name != nullptr ? name : "");
  return ++m_prog;
}

CheckBox::Take CheckBox::take(uint32_t prog, Program& out, char* name, size_t cap) const {
  if (prog == 0 || prog != m_prog || !m_ok) return Take::kCheckAgain;
  out = m_p;
  snprintf(name, cap, "%s", m_name);
  return Take::kOk;
}

}  // namespace logo
}  // namespace cq
````

Create `firmware/esp32/src/logo/check_box.h`:
````cpp
// The checked program waiting to be run (Tank Logo LLD §20.1): the last check's compiled program, its number and
// name. The worker puts every check here; logo_run takes a copy only when the number is the current one and the
// program compiled OK - so the tank runs exactly what the page saw. Pure; the mutex around it lives in net/.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/program.h"

namespace cq {
namespace logo {

class CheckBox {
 public:
  static constexpr size_t kNameCap = 25;
  enum class Take : uint8_t { kOk, kCheckAgain };

  explicit CheckBox(Program& checked) : m_p(checked) {}
  // A new check: its program (copied when it compiled), a saved program's name or "". Returns its number (1, 2 …).
  uint32_t put(const Program* compiled, const char* name);
  Take take(uint32_t prog, Program& out, char* name, size_t cap) const;
  uint32_t current() const { return m_prog; }

 private:
  Program& m_p;
  uint32_t m_prog = 0;
  bool m_ok = false;
  char m_name[kNameCap] = "";
};

}  // namespace logo
}  // namespace cq
````

Create `firmware/esp32/src/logo/program_names.cpp`:
````cpp
#include "logo/program_names.h"

#include <string.h>

namespace cq {
namespace logo {
namespace {

constexpr Example kExamples[] = {
    {"first_square", "; Your first square\nREPEAT 4 [ FORWARD 30 RIGHT 90 ]\nBEEP\n"},
    {"avoid_walls",
     "; Drive around and turn away from walls\nFOREVER [\n  FORWARD 20\n  IF BLOCKED? [ BACK 10 RIGHT 90 ]\n]\n"},
    {"look_around", "; Look around, then say so\nLOOK LEFT WAIT 1\nLOOK RIGHT WAIT 1\nLOOK HOME\nSAY [ all clear ]\n"},
};

bool nameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

}  // namespace

bool programName(const char* in, char* out, size_t cap) {
  const size_t n = strlen(in);
  if (n == 0 || n > kNameMax || cap <= n) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = (in[i] >= 'A' && in[i] <= 'Z') ? static_cast<char>(in[i] + 32) : in[i];
    if (!nameChar(c)) return false;
    out[i] = c;
  }
  out[n] = '\0';
  return true;
}

uint8_t exampleCount() { return static_cast<uint8_t>(sizeof kExamples / sizeof kExamples[0]); }
const Example& example(uint8_t i) { return kExamples[i]; }

const Example* findExample(const char* lower_name) {
  for (const Example& e : kExamples) {
    if (strcmp(e.name, lower_name) == 0) return &e;
  }
  return nullptr;
}

}  // namespace logo
}  // namespace cq
````

Create `firmware/esp32/src/logo/program_names.h`:
````cpp
// Saved program names and the built-in examples (Tank Logo LLD §12, §20.4): 1-24 characters a-z 0-9 _ -, without
// regard to case (kept in lower case); examples are read-only. Pure.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace logo {

constexpr size_t kNameMax = 24;
constexpr uint8_t kMaxPrograms = 32;

// The name in lower case into out (cap ≥ 25); false when it is not a valid name.
bool programName(const char* in, char* out, size_t cap);

struct Example {
  const char* name;
  const char* src;
};
uint8_t exampleCount();
const Example& example(uint8_t i);
const Example* findExample(const char* lower_name);

}  // namespace logo
}  // namespace cq
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_box`
Expected: `4 test cases: 4 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/logo firmware/esp32/test/test_logo_box
git commit -m "feat(logo): check box (numbered checked program), program names, built-in examples

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 4: Odometry (the driven path)

**Files:** `src/logo/odometry.h/.cpp`; `test/test_logo_odometry`.

**Interfaces:**
- Consumes: nothing.
- Produces: `struct Pose { x_mm; y_mm; heading_deg; }`; `class Odometry { start(enc_l, enc_r, yaw_cdeg, counts_per_m); Pose update(enc_l, enc_r, yaw_cdeg); Pose pose() const; }` (axes of the planned path: start (0,0) facing +y, right = +).

- [ ] **Step 1: Write the failing tests**


Create `firmware/esp32/test/test_logo_odometry/test_main.cpp`:
````cpp
// Plan N3 Task 4: the driven path from encoder counts and yaw - straight, back, after a turn, an arc (LLD §11).
#include <math.h>
#include <unity.h>

#include "logo/odometry.h"

using namespace cq::logo;

namespace {
constexpr uint16_t kCpm = 7514;
Odometry g_o;
}  // namespace

void setUp() { g_o.start(1000, 2000, 500, kCpm); }
void tearDown() {}

void test_straight_ahead_and_back() {
  Pose p = g_o.update(1000 + kCpm, 2000 + kCpm, 500);
  TEST_ASSERT_INT_WITHIN(1, 0, p.x_mm);
  TEST_ASSERT_INT_WITHIN(1, 1000, p.y_mm);
  p = g_o.update(1000 + kCpm / 2, 2000 + kCpm / 2, 500);  // back 50 cm (counts go down)
  TEST_ASSERT_INT_WITHIN(1, 500, p.y_mm);
  TEST_ASSERT_EQUAL(0, p.heading_deg);
}

void test_after_a_right_turn_x_grows() {
  g_o.update(1000, 2000, static_cast<uint16_t>(500 + 36000 - 9000));  // spin right 90° on the spot
  const Pose p = g_o.update(1000 + kCpm / 2, 2000 + kCpm / 2, static_cast<uint16_t>(500 + 36000 - 9000));
  TEST_ASSERT_EQUAL(90, p.heading_deg);
  TEST_ASSERT_INT_WITHIN(1, 500, p.x_mm);
  TEST_ASSERT_INT_WITHIN(1, 0, p.y_mm);
}

void test_a_quarter_arc_ends_within_1_cm() {
  // ARC RIGHT 90 50: 90 steps of 1° along a 500 mm circle, centre to the right.
  const double step_mm = 500.0 * 3.14159265358979 / 180.0;
  int32_t l = 1000, r = 2000;
  Pose p;
  for (int i = 1; i <= 90; ++i) {
    l += static_cast<int32_t>(lround(step_mm * kCpm / 1000.0));
    r += static_cast<int32_t>(lround(step_mm * kCpm / 1000.0));
    p = g_o.update(l, r, static_cast<uint16_t>((500 + 36000 - i * 100) % 36000));
  }
  TEST_ASSERT_INT_WITHIN(10, 500, p.x_mm);
  TEST_ASSERT_INT_WITHIN(10, 500, p.y_mm);
  TEST_ASSERT_EQUAL(90, p.heading_deg);
}

void test_yaw_wraps_through_zero() {
  g_o.start(0, 0, 100, kCpm);
  const Pose p = g_o.update(0, 0, 35900);  // 2° right of the start through 0
  TEST_ASSERT_EQUAL(2, p.heading_deg);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_straight_ahead_and_back);
  RUN_TEST(test_after_a_right_turn_x_grows);
  RUN_TEST(test_a_quarter_arc_ends_within_1_cm);
  RUN_TEST(test_yaw_wraps_through_zero);
  return UNITY_END();
}
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_odometry`
Expected: FAIL — `fatal error: 'logo/odometry.h' file not found`.

- [ ] **Step 3: Implement**


Create `firmware/esp32/src/logo/odometry.cpp`:
````cpp
#include "logo/odometry.h"

#include <math.h>

namespace cq {
namespace logo {
namespace {

constexpr double kPi = 3.14159265358979323846;

double wrap180(double d) {
  while (d > 180.0) d -= 360.0;
  while (d <= -180.0) d += 360.0;
  return d;
}

}  // namespace

void Odometry::start(int32_t enc_l, int32_t enc_r, uint16_t yaw_cdeg, uint16_t counts_per_m) {
  m_l = enc_l;
  m_r = enc_r;
  m_yaw0 = yaw_cdeg;
  m_cpm = counts_per_m != 0 ? counts_per_m : 7514;
  m_x = m_y = m_h = 0;
}

double Odometry::heading(uint16_t yaw_cdeg) const {
  return wrap180(-(static_cast<int32_t>(yaw_cdeg) - static_cast<int32_t>(m_yaw0)) / 100.0);  // tank: left = +
}

// The distance since the last update along the heading half-way between the two readings (good on arcs).
Pose Odometry::update(int32_t enc_l, int32_t enc_r, uint16_t yaw_cdeg) {
  const double counts = ((enc_l - m_l) + (enc_r - m_r)) / 2.0;
  m_l = enc_l;
  m_r = enc_r;
  const double h = heading(yaw_cdeg);
  const double mid = m_h + wrap180(h - m_h) / 2.0;
  const double d_mm = counts * 1000.0 / m_cpm;
  m_x += d_mm * sin(mid * kPi / 180.0);
  m_y += d_mm * cos(mid * kPi / 180.0);
  m_h = h;
  return pose();
}

Pose Odometry::pose() const {
  Pose p;
  p.x_mm = static_cast<int32_t>(lround(m_x));
  p.y_mm = static_cast<int32_t>(lround(m_y));
  p.heading_deg = static_cast<int16_t>(lround(m_h));
  return p;
}

}  // namespace logo
}  // namespace cq
````

Create `firmware/esp32/src/logo/odometry.h`:
````cpp
// The driven path while a program runs (Tank Logo LLD §11, §20.1): dead reckoning from the encoder counts (signed
// by drive direction, counts per metre) and the gyro yaw. Same axes as the planned path: start (0, 0) facing +y,
// x to the right, heading in degrees, right = +. Pure.
#pragma once

#include <stdint.h>

namespace cq {
namespace logo {

struct Pose {
  int32_t x_mm = 0;
  int32_t y_mm = 0;
  int16_t heading_deg = 0;
};

class Odometry {
 public:
  void start(int32_t enc_l, int32_t enc_r, uint16_t yaw_cdeg, uint16_t counts_per_m);
  Pose update(int32_t enc_l, int32_t enc_r, uint16_t yaw_cdeg);
  Pose pose() const;

 private:
  double heading(uint16_t yaw_cdeg) const;  // since the start, -180..180, right = +
  int32_t m_l = 0, m_r = 0;
  uint16_t m_yaw0 = 0;
  uint16_t m_cpm = 7514;
  double m_x = 0, m_y = 0, m_h = 0;
};

}  // namespace logo
}  // namespace cq
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_odometry`
Expected: `4 test cases: 4 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/logo firmware/esp32/test/test_logo_odometry
git commit -m "feat(logo): odometry - driven pose from encoder counts and yaw

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 5: The logo_* events, two refusals and a Log kind

**Files:** `src/core/ws_json.h`, `ws_json_logo.cpp` (new), `ws_json_reply.cpp`, `event_log.h`, `ws_json_tank.cpp`; `test/test_ws_json`.

**Interfaces:**
- Consumes: `detail::fmt`, `jsonEscape`.
- Produces: `jsonLogoStarted / jsonLogoLine / jsonLogoPose / jsonLogoSay / jsonLogoNote / jsonLogoEnded` (`{"t":"ev","seq","tank_ms","kind":"logo_…",…}`); `BridgeReason::kCheckAgain` (3016 CHECK_AGAIN), `kNotRunning` (3017 NOT_RUNNING); `LogKind::kLogo` ("logo").

- [ ] **Step 1: Write the failing tests**


Change `firmware/esp32/test/test_ws_json/test_main.cpp` (unified diff against the previous task):
````diff
@@ -528,6 +528,31 @@ void test_uno_event_names_cover_lite_events() {
   TEST_ASSERT_EQUAL_STRING("unknown", unoEventName(250));
 }
 
+// Plan N3: the logo_* events and the two new refusals (Tank Logo LLD §20.4).
+void test_logo_events() {
+  jsonLogoStarted(buf, sizeof buf, 1, 2, "square", "phone-1");
+  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ev\",\"seq\":1,\"tank_ms\":2,\"kind\":\"logo_started\",\"name\":\"square\",\"by\":\"phone-1\"}", buf);
+  jsonLogoLine(buf, sizeof buf, 3, 4, 7, 2);
+  TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"logo_line\",\"line\":7,\"round\":2}"));
+  jsonLogoPose(buf, sizeof buf, 5, 6, 1234, -56, -90);
+  TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"logo_pose\",\"x_cm\":123.4,\"y_cm\":-5.6,\"heading\":-90}"));
+  jsonLogoSay(buf, sizeof buf, 7, 8, "say \"hi\"");
+  TEST_ASSERT_NOT_NULL(strstr(buf, "\"text\":\"say \\\"hi\\\"\"}"));
+  jsonLogoNote(buf, sizeof buf, 9, 10, 3, "ARC RIGHT 90 40: speed set to 43 %");
+  TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"logo_note\",\"line\":3,\"msg\":\"ARC RIGHT 90 40: speed set to 43 %\"}"));
+  jsonLogoEnded(buf, sizeof buf, 11, 12, 0, 4, "");
+  TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"logo_ended\",\"reason\":\"finished\",\"line\":4,\"msg\":\"\"}"));
+  jsonLogoEnded(buf, sizeof buf, 13, 14, 7024, 2, "Something is in the way on line 2.");
+  TEST_ASSERT_NOT_NULL(strstr(buf, "\"reason\":7024,\"line\":2,\"msg\":\"Something is in the way on line 2.\"}"));
+}
+
+void test_logo_refusals() {
+  jsonNack(buf, sizeof buf, 5, BridgeReason::kCheckAgain);
+  TEST_ASSERT_NOT_NULL(strstr(buf, "\"code\":\"COMMAND_3016\",\"reason\":\"CHECK_AGAIN\""));
+  jsonNack(buf, sizeof buf, 6, BridgeReason::kNotRunning);
+  TEST_ASSERT_NOT_NULL(strstr(buf, "\"code\":\"COMMAND_3017\",\"reason\":\"NOT_RUNNING\""));
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_ack_reason_table_matches_generated_codes);
@@ -561,5 +586,7 @@ int main() {
   RUN_TEST(test_json_log_that_does_not_fit_stays_valid);
   RUN_TEST(test_json_tank_info);
   RUN_TEST(test_uno_event_names_cover_lite_events);
+  RUN_TEST(test_logo_events);
+  RUN_TEST(test_logo_refusals);
   return UNITY_END();
 }
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_json -f test_tank_app_controller`
Expected: FAIL — `use of undeclared identifier 'jsonLogoStarted'`.

- [ ] **Step 3: Implement**


Change `firmware/esp32/src/core/event_log.h` (unified diff against the previous task):
````diff
@@ -8,6 +8,7 @@ namespace core {
 
 enum class LogKind : uint8_t {
   kBoot, kUno, kPrimitive, kStopHold, kClientLost, kBodyOnline, kBodyOffline, kWifiMode, kRestart, kOta,
+  kLogo,  // Plan N3: a program ended (a = code, 0 finished; b = line; client = its name) or a request refused (a = -1)
 };
 
 struct LogEntry {
````

Change `firmware/esp32/src/core/ws_json.h` (unified diff against the previous task):
````diff
@@ -20,7 +20,8 @@ constexpr size_t kWsBigMax = 3584;  // Plan O: tank / log replies - sent through
 
 // Reasons the bridge itself produces (not on the Uno's ack_status list).
 enum class BridgeReason : uint8_t { kNotOwner, kBusy, kBodyOffline, kRateLimited, kInvalidCommand, kTimeout, kUnavailable,
-                                    kStopped };  // kStopped: design-notes §72 STOP hold
+                                    kStopped,  // kStopped: design-notes §72 STOP hold
+                                    kCheckAgain, kNotRunning };  // Plan N3: logo_run / logo_step / logo_stop
 
 struct OwnerView {
   const char* owner;   // "none" | "human" | "ir"
@@ -105,6 +106,16 @@ size_t jsonTank(char* out, size_t cap, const TankView& v);
 size_t jsonLog(char* out, size_t cap, const LogEntry* e, uint8_t n, uint32_t last, bool gap, uint32_t now_ms);
 size_t jsonPong(char* out, size_t cap, const char* t0, uint32_t tank_ms);
 
+// Plan N3 (Tank Logo LLD §20.4): the logo_* events, {"t":"ev","seq","tank_ms","kind":"logo_…",…} to every client.
+size_t jsonLogoStarted(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const char* name, const char* by);
+size_t jsonLogoLine(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint16_t line, int32_t round);
+size_t jsonLogoPose(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, int32_t x_mm, int32_t y_mm, int16_t heading);
+size_t jsonLogoSay(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const char* text);
+size_t jsonLogoNote(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint16_t line, const char* msg);
+// reason "finished" when code is 0, else the error code number
+size_t jsonLogoEnded(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint16_t code, uint16_t line,
+                     const char* msg);
+
 // Error code number behind an ack_status index (checked against kAckStatusCode in tests).
 uint16_t ackReasonCode(uint8_t status);
````

Create `firmware/esp32/src/core/ws_json_logo.cpp`:
````cpp
// The Tank Logo events (Plan N3, Tank Logo LLD §20.4).
#include "core/ws_json.h"
#include "core/ws_json_util.h"

#include <stdio.h>

namespace cq {
namespace core {
using namespace detail;

size_t jsonLogoStarted(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const char* name, const char* by) {
  char n[64], b[32];
  jsonEscape(n, sizeof n, name);
  jsonEscape(b, sizeof b, by);
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_started\",\"name\":\"%s\",\"by\":\"%s\"}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), n, b);
}

size_t jsonLogoLine(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint16_t line, int32_t round) {
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_line\",\"line\":%u,\"round\":%ld}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), line, static_cast<long>(round));
}

size_t jsonLogoPose(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, int32_t x_mm, int32_t y_mm, int16_t heading) {
  return fmt(out, cap,
             "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_pose\",\"x_cm\":%.1f,\"y_cm\":%.1f,\"heading\":%d}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), x_mm / 10.0, y_mm / 10.0, heading);
}

size_t jsonLogoSay(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const char* text) {
  char t[256];
  jsonEscape(t, sizeof t, text);
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_say\",\"text\":\"%s\"}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), t);
}

size_t jsonLogoNote(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint16_t line, const char* msg) {
  char m[200];
  jsonEscape(m, sizeof m, msg);
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_note\",\"line\":%u,\"msg\":\"%s\"}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), line, m);
}

size_t jsonLogoEnded(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, uint16_t code, uint16_t line,
                     const char* msg) {
  char m[200], reason[12];
  jsonEscape(m, sizeof m, msg);
  if (code == 0) {
    snprintf(reason, sizeof reason, "\"finished\"");
  } else {
    snprintf(reason, sizeof reason, "%u", code);
  }
  return fmt(out, cap,
             "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"logo_ended\",\"reason\":%s,\"line\":%u,\"msg\":\"%s\"}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), reason, line, m);
}

}  // namespace core
}  // namespace cq
````

Change `firmware/esp32/src/core/ws_json_reply.cpp` (unified diff against the previous task):
````diff
@@ -52,7 +52,11 @@ constexpr ReasonInfo kBridgeReasons[] = {
     {3009, "COMMAND_TIMEOUT", "No acknowledgement in time"},
     {3003, "UNAVAILABLE", "Capability not available"},  // Plan H: e.g. turn with radius / move without encoders
     {3011, "STOPPED", "Stopped by another device - release the stick to drive again"},  // design-notes §72
+    {3016, "CHECK_AGAIN", "The program changed - check it again"},                       // Plan N3 (bridge only)
+    {3017, "NOT_RUNNING", "No program is running"},
 };
+static_assert(sizeof(kBridgeReasons) / sizeof(kBridgeReasons[0]) == static_cast<size_t>(BridgeReason::kNotRunning) + 1,
+              "one row per BridgeReason");
 
 const char* domain(uint16_t code) {
   switch (code / 1000) {
````

Change `firmware/esp32/src/core/ws_json_tank.cpp` (unified diff against the previous task):
````diff
@@ -30,8 +30,8 @@ size_t jsonTank(char* out, size_t cap, const TankView& v) {
 namespace {
 
 const char* const kLogKinds[] = {"boot", "uno", "primitive", "stop_hold", "client_lost", "body_online",
-                                 "body_offline", "wifi_mode", "restart", "ota"};
-static_assert(sizeof(kLogKinds) / sizeof(kLogKinds[0]) == static_cast<size_t>(LogKind::kOta) + 1, "a LogKind without a name");
+                                 "body_offline", "wifi_mode", "restart", "ota", "logo"};
+static_assert(sizeof(kLogKinds) / sizeof(kLogKinds[0]) == static_cast<size_t>(LogKind::kLogo) + 1, "a LogKind without a name");
 
 // One entry as JSON (with a leading comma unless first); its length, or 0 when it does not fit `cap`.
 size_t logItem(char* out, size_t cap, const LogEntry& x, bool first, uint32_t now_ms) {
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_json -f test_tank_app_controller`
Expected: `42 test cases: 42 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core firmware/esp32/test/test_ws_json
git commit -m "feat(bridge): logo_* events, CHECK_AGAIN / NOT_RUNNING, Log kind logo

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 6: Uno requests: TONE, CAM_SET and the program's waiter

**Files:** `src/core/body_link.h/.cpp`, `src/core/acked_ops.h`, `src/app/uno_channel.h/.cpp`; `test/test_uno_channel`.

**Interfaces:**
- Consumes: generated `ToneMsg`, `CamSetMsg`.
- Produces: `BodyLink::sendTone(hz*, ms*, n, now, seq&)`, `sendCamSet(pan_dd, tilt_dd, now, seq&)`; `AckedOp::kTone / kCamSet` with `pan_dd, tilt_dd, n_notes, hz[10], ms[10]`; `core::kLogoClient = -2`; `UnoChannel::setLogoDone(void (*)(uint8_t status))`, `kLogoTimeout = 0xFE`, `kLogoOffline = 0xFD` (BodyLink's own timeout arrives as kLogoTimeout).

- [ ] **Step 1: Write the failing tests**


Change `firmware/esp32/test/test_uno_channel/test_main.cpp` (unified diff against the previous task):
````diff
@@ -164,6 +164,80 @@ void test_ota_answers_everything_and_stops_master() {
   TEST_ASSERT_EQUAL(3, fakes::countTo(3, "nack"));
 }
 
+// ---- Plan N3: the running Logo program's requests (TONE / CAM_SET frames; answers through its sink) ----
+uint8_t g_logo_status = 0xAA;
+int g_logo_answers = 0;
+void logoDone(uint8_t status) {
+  g_logo_status = status;
+  ++g_logo_answers;
+}
+
+core::AckedOp logoOp(core::AckedOp::Kind k) {
+  core::AckedOp o = op(k, core::kLogoClient);
+  uno->setLogoDone(logoDone);
+  g_logo_status = 0xAA;
+  g_logo_answers = 0;
+  return o;
+}
+
+void test_logo_tone_and_cam_set_frames() {
+  core::AckedOp t = logoOp(core::AckedOp::kTone);
+  t.n_notes = 2;
+  t.hz[0] = 262;
+  t.ms[0] = 250;
+  t.hz[1] = 0;
+  t.ms[1] = 500;
+  uno->request(t);
+  uno->tick(100);
+  wire.drain(*tx);
+  const cqtest::Sent* s = wire.last(FT::kTone);
+  TEST_ASSERT_NOT_NULL(s);
+  proto::ToneMsg m{};
+  TEST_ASSERT_TRUE(proto::decode(s->payload, s->len, m));
+  TEST_ASSERT_EQUAL(2, m.n);
+  TEST_ASSERT_EQUAL(262, m.notes[0].hz);
+  TEST_ASSERT_EQUAL(500, m.notes[1].ms);
+  ackLast(FT::kTone, 0, 110);
+  core::AckedOp c = logoOp(core::AckedOp::kCamSet);
+  c.pan_dd = 600;
+  c.tilt_dd = 1000;
+  uno->request(c);
+  uno->tick(120);
+  wire.drain(*tx);
+  proto::CamSetMsg cm{};
+  TEST_ASSERT_TRUE(proto::decode(wire.last(FT::kCamSet)->payload, wire.last(FT::kCamSet)->len, cm));
+  TEST_ASSERT_EQUAL(600, cm.pan);
+  TEST_ASSERT_EQUAL(1000, cm.tilt);
+}
+
+void test_logo_answers_go_to_its_sink_never_to_a_client() {
+  uno->request(logoOp(core::AckedOp::kLed));
+  uno->tick(100);
+  ackLast(FT::kLed, 0, 110);
+  TEST_ASSERT_EQUAL(1, g_logo_answers);
+  TEST_ASSERT_EQUAL(0, g_logo_status);
+  uno->request(logoOp(core::AckedOp::kSound));
+  uno->tick(120);
+  ackLast(FT::kSound, static_cast<uint8_t>(proto::AckStatus::kBusy), 130);
+  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::AckStatus::kBusy), g_logo_status);
+  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());
+}
+
+void test_logo_no_answer_and_body_offline() {
+  uno->request(logoOp(core::AckedOp::kLed));
+  uno->tick(100);
+  for (uint32_t t = 102; t <= 800; t += 2) {
+    link->step(t);
+    pump();
+    uno->tick(t);
+  }
+  TEST_ASSERT_EQUAL(1, g_logo_answers);
+  TEST_ASSERT_EQUAL(app::UnoChannel::kLogoTimeout, g_logo_status);  // BodyLink's own 200 ms timeout, as one value
+  uno->request(logoOp(core::AckedOp::kLed));
+  uno->onBodyOffline();
+  TEST_ASSERT_EQUAL(app::UnoChannel::kLogoOffline, g_logo_status);
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_requests_go_one_at_a_time_in_order);
@@ -174,5 +248,8 @@ int main() {
   RUN_TEST(test_stop_overtakes_queued_requests);
   RUN_TEST(test_body_offline_answers_and_clears_the_queue);
   RUN_TEST(test_ota_answers_everything_and_stops_master);
+  RUN_TEST(test_logo_tone_and_cam_set_frames);
+  RUN_TEST(test_logo_answers_go_to_its_sink_never_to_a_client);
+  RUN_TEST(test_logo_no_answer_and_body_offline);
   return UNITY_END();
 }
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_uno_channel -f test_body_link`
Expected: FAIL — `no member named 'kLogoClient' in namespace 'cq::core'`.

- [ ] **Step 3: Implement**


Change `firmware/esp32/src/app/uno_channel.cpp` (unified diff against the previous task):
````diff
@@ -24,9 +24,16 @@ void UnoChannel::onStopResult(uint8_t status) {
   m_stop_n = 0;
 }
 
+bool UnoChannel::logo(const core::AckedOp& op, uint8_t status) {
+  if (op.fd != core::kLogoClient) return false;
+  if (m_logo_done != nullptr) m_logo_done(status == core::kStatusTimeout ? kLogoTimeout : status);
+  return true;
+}
+
 void UnoChannel::onAck(uint8_t seq, uint8_t status) {
   core::AckedOp op;
   if (!m_ops.isCurrent(seq) || !m_ops.front(op)) return;
+  if (logo(op, status)) return m_ops.done();
   if (status == 0 && op.kind == core::AckedOp::kSettingGet) return;  // review I5: answered by its VALUE
   if (op.fd >= 0 && status == 0) m_out.ack(op.fd, op.id);  // the self-test result follows as EVENT SELFTEST
   if (op.fd >= 0 && status != 0) m_out.nackStatus(op.fd, op.id, status);
@@ -43,7 +50,7 @@ void UnoChannel::onSettingValue(uint8_t id, int32_t value) {
 void UnoChannel::answerAll(core::BridgeReason r) {
   core::AckedOp op;
   while (m_ops.front(op)) {
-    if (op.fd >= 0) m_out.nack(op.fd, op.id, r);
+    if (!logo(op, kLogoOffline) && op.fd >= 0) m_out.nack(op.fd, op.id, r);
     m_ops.done();
   }
 }
@@ -51,26 +58,34 @@ void UnoChannel::answerAll(core::BridgeReason r) {
 void UnoChannel::onBodyOffline() { answerAll(core::BridgeReason::kBodyOffline); }
 
 bool UnoChannel::send(const core::AckedOp& op, uint32_t now, uint8_t& seq) {
-  switch (op.kind) {
-    case core::AckedOp::kSound: return m_link.sendSound(op.a, now, seq);
-    case core::AckedOp::kLed: return m_link.sendLed(op.a, op.r, op.g, op.b, op.period_ms, op.duration_ms, now, seq);
+  switch (op.kind) {  // VARIANT POINT: settings requests; the page's / program's in sendUi
     case core::AckedOp::kSettingGet: return m_link.sendSettingGet(op.a, now, seq);
     case core::AckedOp::kSettingSet: return m_link.sendSettingSet(op.a, op.value, now, seq);
     case core::AckedOp::kSettingsSave: return m_link.sendSettingsSave(now, seq);
     case core::AckedOp::kSelfTest:
       return m_link.sendCalibrate(static_cast<uint8_t>(proto::CalOp::kStart),
                                   static_cast<uint8_t>(proto::CalScript::kSelfTest), now, seq);
+    default: return sendUi(op, now, seq);
+  }
+}
+
+bool UnoChannel::sendUi(const core::AckedOp& op, uint32_t now, uint8_t& seq) {
+  switch (op.kind) {  // VARIANT POINT: light, sound, camera (the drive page's and a Logo program's)
+    case core::AckedOp::kSound: return m_link.sendSound(op.a, now, seq);
+    case core::AckedOp::kLed: return m_link.sendLed(op.a, op.r, op.g, op.b, op.period_ms, op.duration_ms, now, seq);
     case core::AckedOp::kCamHome: return m_link.sendCamHome(now, seq);
     case core::AckedOp::kCamLock: return m_link.sendCamLock(op.a != 0, now, seq);
     case core::AckedOp::kAudioMode: return m_link.sendAudioMode(op.a, now, seq);
+    case core::AckedOp::kTone: return m_link.sendTone(op.hz, op.ms, op.n_notes, now, seq);
+    case core::AckedOp::kCamSet: return m_link.sendCamSet(op.pan_dd, op.tilt_dd, now, seq);
+    default: return false;
   }
-  return false;
 }
 
 void UnoChannel::backstop(uint32_t now) {  // the slot was reset without an answer: never stuck
   core::AckedOp op;
   if (!m_ops.busy() || !m_ops.timedOut(now)) return;
-  if (m_ops.front(op) && op.fd >= 0) m_out.nack(op.fd, op.id, core::BridgeReason::kTimeout);
+  if (m_ops.front(op) && !logo(op, kLogoTimeout) && op.fd >= 0) m_out.nack(op.fd, op.id, core::BridgeReason::kTimeout);
   m_ops.done();
 }
 
@@ -82,7 +97,7 @@ void UnoChannel::tick(uint32_t now) {
   if (send(op, now, seq)) {
     m_ops.markSent(seq, now);
   } else if (!m_link.synced()) {  // body offline: answer and drop it
-    if (op.fd >= 0) m_out.nack(op.fd, op.id, core::BridgeReason::kBodyOffline);
+    if (!logo(op, kLogoOffline) && op.fd >= 0) m_out.nack(op.fd, op.id, core::BridgeReason::kBodyOffline);
     m_ops.done();
   }  // synced but the frame queue is full: next tick
 }
````

Change `firmware/esp32/src/app/uno_channel.h` (unified diff against the previous task):
````diff
@@ -24,6 +24,10 @@ class UnoChannel {
   void onBodyOffline();  // every queued / in-flight request with a client: BODY_OFFLINE, dropped
   void tick(uint32_t now);  // send the next request; the 600 ms backstop (BodyLink's 200 ms timeout comes as onAck)
   void stopForOta();        // R14: the link stops only for OTA; every waiter answered
+  // Plan N3: the answer to a request of the running Logo program (fd core::kLogoClient): 0 = OK, else the Uno's
+  // ack_status, or kLogoTimeout / kLogoOffline when no answer came.
+  static constexpr uint8_t kLogoTimeout = 0xFE, kLogoOffline = 0xFD;
+  void setLogoDone(void (*done)(uint8_t status)) { m_logo_done = done; }
   core::BodyLink& link() { return m_link; }
   const core::BodyLink& link() const { return m_link; }
   bool empty() const { return m_ops.empty(); }
@@ -37,8 +41,10 @@ class UnoChannel {
   static constexpr size_t kMaxStopWaiters = 2 * core::kMaxWsClients;  // review M8
 
   bool send(const core::AckedOp& op, uint32_t now, uint8_t& seq);
+  bool sendUi(const core::AckedOp& op, uint32_t now, uint8_t& seq);
   void backstop(uint32_t now);
   void answerAll(core::BridgeReason r);  // the in-flight request and every queued one, then empty
+  bool logo(const core::AckedOp& op, uint8_t status);  // true: the program's request, answered through its sink
 
   core::BodyLink& m_link;
   core::FrameTx& m_tx;
@@ -46,6 +52,7 @@ class UnoChannel {
   core::AckedOps m_ops;
   Waiter m_stop_waiters[kMaxStopWaiters] = {};
   size_t m_stop_n = 0;
+  void (*m_logo_done)(uint8_t status) = nullptr;
 };
 
 }  // namespace app
````

Change `firmware/esp32/src/core/acked_ops.h` (unified diff against the previous task):
````diff
@@ -7,14 +7,21 @@
 namespace cq {
 namespace core {
 
+constexpr int kLogoClient = -2;  // Plan N3: the running Logo program waits for this answer (UnoChannel's logo sink)
+
 struct AckedOp {
   // kSelfTest: Plan O; kCamHome / kCamLock / kAudioMode: the drive page's requests (Plan P, LLD R15.5)
-  enum Kind : uint8_t { kSound, kLed, kSettingGet, kSettingSet, kSettingsSave, kSelfTest, kCamHome, kCamLock, kAudioMode };
+  enum Kind : uint8_t { kSound, kLed, kSettingGet, kSettingSet, kSettingsSave, kSelfTest, kCamHome, kCamLock, kAudioMode,
+                        kTone, kCamSet };  // Plan N3: PLAY / LOOK of a Logo program
   Kind kind = kSound;
   uint8_t a = 0;      // sound id / setting id / led mode / camera lock on / audio mode
   int32_t value = 0;  // setting value
   uint8_t r = 0, g = 0, b = 0;
   uint16_t period_ms = 0, duration_ms = 0;
+  int16_t pan_dd = 0, tilt_dd = 0;  // kCamSet
+  uint8_t n_notes = 0;              // kTone
+  uint16_t hz[10] = {};
+  uint16_t ms[10] = {};
   int fd = -1;        // client to answer, -1 = none (feedback)
   int32_t id = -1;
 };
````

Change `firmware/esp32/src/core/body_link.cpp` (unified diff against the previous task):
````diff
@@ -196,6 +196,17 @@ bool BodyLink::sendLed(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t p
   return sendAcked(proto::LedMsg{mode, r, g, b, period_ms, duration_ms}, now_ms, seq_out);
 }
 
+bool BodyLink::sendTone(const uint16_t* hz, const uint16_t* ms, uint8_t n, uint32_t now_ms, uint8_t& seq_out) {
+  proto::ToneMsg m{};
+  m.n = n > 10 ? 10 : n;
+  for (uint8_t i = 0; i < m.n; ++i) m.notes[i] = proto::ToneNotesItem{hz[i], ms[i]};
+  return sendAcked(m, now_ms, seq_out);
+}
+
+bool BodyLink::sendCamSet(int16_t pan_dd, int16_t tilt_dd, uint32_t now_ms, uint8_t& seq_out) {
+  return sendAcked(proto::CamSetMsg{pan_dd, tilt_dd, 60}, now_ms, seq_out);  // speed 60 = 120 °/s (Lite: its own)
+}
+
 bool BodyLink::sendSettingGet(uint8_t id, uint32_t now_ms, uint8_t& seq_out) {
   return sendAcked(proto::SettingGetMsg{id}, now_ms, seq_out);
 }
````

Change `firmware/esp32/src/core/body_link.h` (unified diff against the previous task):
````diff
@@ -104,6 +104,9 @@ class BodyLink {
   bool sendSettingSet(uint8_t id, int32_t value, uint32_t now_ms, uint8_t& seq_out);
   bool sendSettingsSave(uint32_t now_ms, uint8_t& seq_out);
   bool sendCalibrate(uint8_t op, uint8_t script, uint32_t now_ms, uint8_t& seq_out);  // Plan O: SELF_TEST only
+  // Plan N3 (Tank Logo): PLAY (≤ 10 notes per frame; a new TONE replaces the tune playing) and LOOK.
+  bool sendTone(const uint16_t* hz, const uint16_t* ms, uint8_t n, uint32_t now_ms, uint8_t& seq_out);
+  bool sendCamSet(int16_t pan_dd, int16_t tilt_dd, uint32_t now_ms, uint8_t& seq_out);  // servo angles, 0.1°
   bool ackPending() const { return m_cam_pending; }
   void setOwnerHint(proto::OwnerHintValue hint);  // remembered; sent now if synced, and after HELLO
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_uno_channel -f test_body_link`
Expected: `33 test cases: 33 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/core firmware/esp32/src/app firmware/esp32/test/test_uno_channel
git commit -m "feat(bridge): TONE and CAM_SET requests, answers for a running Logo program

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 7: MotionController hooks for a running program

**Files:** `src/app/motion_controller.h/.cpp`; `test/test_motion_controller`.

**Interfaces:**
- Consumes: `logo::Action` (N2).
- Produces: `MotionController::kLogoPrimitive = -2`; `Hooks::logo_fd / logo_end / logo_event` (defaulted to nullptr); `bool startLogo(int fd, const logo::Action&, uint32_t now, core::BridgeReason& why)` (move / turn / arc / pivot / HOME; refused: kBusy OTA, kBodyOffline, kUnavailable no encoders or encoder fault); `ownedBy(fd)` is also true for the running program's page; `end()` also tells `logo_event`.

- [ ] **Step 1: Write the failing tests**


Change `firmware/esp32/test/test_motion_controller/test_main.cpp` (unified diff against the previous task):
````diff
@@ -164,6 +164,92 @@ void test_calib_reply_has_counts_and_roll_diagnostics() {
   TEST_ASSERT_NOT_EQUAL(std::string::npos, j.find("\"roll_watched\":0"));
 }
 
+// ---- Plan N3: a running Logo program's motions and the events that end it ----
+int g_logo_fd = -1;
+int g_logo_ends = 0;
+cq::core::EndReason g_logo_reason = cq::core::EndReason::kDone;
+int g_logo_events = 0;
+cq::core::MotionEvent g_logo_event = cq::core::MotionEvent::kStop;
+
+void logoHooks() {
+  app::MotionController::Hooks h;
+  h.claim = [](int fd, uint32_t now) {
+    core::Actions a;
+    return owner.onDrive(fd, now, a);
+  };
+  h.ota = [] { return g_ota; };
+  h.logo_fd = [] { return g_logo_fd; };
+  h.logo_end = [](const core::PrimitiveEnd& e, uint32_t) {
+    ++g_logo_ends;
+    g_logo_reason = e.reason;
+  };
+  h.logo_event = [](core::MotionEvent e, uint32_t) {
+    ++g_logo_events;
+    g_logo_event = e;
+  };
+  mc->setHooks(h);
+  g_logo_fd = -1;
+  g_logo_ends = g_logo_events = 0;
+}
+
+logo::Action logoMove(int16_t cm) {
+  logo::Action a;
+  a.kind = logo::ActKind::kMove;
+  a.dist_cm = cm;
+  a.speed_pct = 40;
+  return a;
+}
+
+void test_logo_move_starts_without_a_reply_and_its_end_reaches_the_program() {
+  syncedWithEncoders();
+  logoHooks();
+  core::BridgeReason why = core::BridgeReason::kBusy;
+  TEST_ASSERT_TRUE(mc->startLogo(3, logoMove(50), 100, why));
+  TEST_ASSERT_TRUE(mc->active());
+  TEST_ASSERT_EQUAL(0, fakes::countTo(3, "ack"));
+  mc->end(core::MotionEvent::kCancel, 200);
+  TEST_ASSERT_EQUAL(1, g_logo_ends);
+  TEST_ASSERT_EQUAL(core::EndReason::kCancelled, g_logo_reason);
+  TEST_ASSERT_EQUAL(1, g_logo_events);
+  TEST_ASSERT_EQUAL(core::MotionEvent::kCancel, g_logo_event);
+}
+
+void test_a_page_s_own_primitive_end_does_not_reach_the_program() {
+  syncedWithEncoders();
+  logoHooks();
+  mc->onMove(3, 1, P("{\"t\":\"move\",\"dist_cm\":100,\"speed\":40,\"id\":1}"), 100);
+  mc->end(core::MotionEvent::kCancel, 200);
+  TEST_ASSERT_EQUAL(0, g_logo_ends);
+}
+
+void test_logo_motions_are_refused_like_the_page_s() {
+  logoHooks();
+  core::BridgeReason why = core::BridgeReason::kBusy;
+  TEST_ASSERT_FALSE(mc->startLogo(3, logoMove(50), 100, why));
+  TEST_ASSERT_EQUAL(core::BridgeReason::kBodyOffline, why);
+  syncedWithEncoders(1u << static_cast<unsigned>(proto::FastFlagBit::kEncoderFault));
+  TEST_ASSERT_FALSE(mc->startLogo(3, logoMove(50), 100, why));
+  TEST_ASSERT_EQUAL(core::BridgeReason::kUnavailable, why);
+  logo::Action turn;
+  turn.kind = logo::ActKind::kTurn;
+  turn.delta_cdeg = -9000;
+  turn.speed_pct = 10;  // below the slowest spin: started at 12 %
+  TEST_ASSERT_TRUE(mc->startLogo(3, turn, 100, why));  // a spin needs no encoders
+  g_ota = true;
+  TEST_ASSERT_FALSE(mc->startLogo(3, turn, 100, why));
+  TEST_ASSERT_EQUAL(core::BridgeReason::kBusy, why);
+}
+
+void test_a_running_program_counts_as_its_page_s_primitive() {
+  logoHooks();
+  TEST_ASSERT_FALSE(mc->ownedBy(3));
+  g_logo_fd = 3;
+  TEST_ASSERT_TRUE(mc->ownedBy(3));  // so release / page closed / owner command reach it (end -> logo_event)
+  TEST_ASSERT_FALSE(mc->ownedBy(4));
+  mc->end(core::MotionEvent::kOwnerClosed, 100);
+  TEST_ASSERT_EQUAL(core::MotionEvent::kOwnerClosed, g_logo_event);
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_begin_loads_counts_and_roll_on);
@@ -176,5 +262,9 @@ int main() {
   RUN_TEST(test_cancelled_move_sends_stop);
   RUN_TEST(test_cancel_by_another_client_is_not_owner);
   RUN_TEST(test_calib_reply_has_counts_and_roll_diagnostics);
+  RUN_TEST(test_logo_move_starts_without_a_reply_and_its_end_reaches_the_program);
+  RUN_TEST(test_a_page_s_own_primitive_end_does_not_reach_the_program);
+  RUN_TEST(test_logo_motions_are_refused_like_the_page_s);
+  RUN_TEST(test_a_running_program_counts_as_its_page_s_primitive);
   return UNITY_END();
 }
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion_controller -f test_drive_controller`
Expected: FAIL — `no member named 'logo_fd' in 'cq::app::MotionController::Hooks'`.

- [ ] **Step 3: Implement**


Change `firmware/esp32/src/app/motion_controller.cpp` (unified diff against the previous task):
````diff
@@ -3,6 +3,7 @@
 #include <stdio.h>
 
 #include "core/arc_limits.h"
+#include "core/motion_config.h"
 #include "core/motion_router.h"
 #include "core/turn_controller.h"
 #include "ports/log_out.h"
@@ -35,7 +36,10 @@ bool MotionController::encoderFault() const {
   return ((m_link.snapshot().fast.flags >> static_cast<unsigned>(proto::FastFlagBit::kEncoderFault)) & 1u) != 0;
 }
 
-bool MotionController::ownedBy(int fd) const { return active() && m_motion.service().ownerFd() == fd; }
+bool MotionController::ownedBy(int fd) const {  // Plan N3: a running Logo program counts as its page's primitive
+  if (m_h.logo_fd != nullptr && m_h.logo_fd() == fd && fd >= 0) return true;
+  return active() && m_motion.service().ownerFd() == fd;
+}
 
 core::PrimitiveView MotionController::view() const {
   const core::MotionService& svc = m_motion.service();
@@ -53,6 +57,7 @@ void MotionController::run(const core::LinkOut& o, uint32_t now) {
   if (o.stop) m_link.sendStop(now);  // STOP purges queued motion frames: no DRIVE 0/0 needed (review M11)
   if (!o.event) return;
   m_out.all(core::jsonPrimitiveDone(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), o.end));
+  if (o.end.id == kLogoPrimitive && m_h.logo_end != nullptr) m_h.logo_end(o.end, now);
   if (o.end.reason != core::EndReason::kDone) {  // Plan O: why a move / turn ended early
     m_log.add(core::LogKind::kPrimitive, static_cast<int32_t>(o.end.reason), static_cast<int32_t>(o.end.kind), "",
               m_tel.tankMs(), now);
@@ -63,7 +68,43 @@ void MotionController::run(const core::LinkOut& o, uint32_t now) {
   }
 }
 
-void MotionController::end(core::MotionEvent e, uint32_t now) { run(m_motion.onEvent(e, now), now); }
+void MotionController::end(core::MotionEvent e, uint32_t now) {
+  run(m_motion.onEvent(e, now), now);
+  if (m_h.logo_event != nullptr) m_h.logo_event(e, now);  // Plan N3: also between the program's motions
+}
+
+// Why a Logo motion cannot start now (true + why), or false.
+bool MotionController::logoRefused(const logo::Action& a, core::BridgeReason& why) const {
+  const bool counted = a.kind == logo::ActKind::kMove || a.kind == logo::ActKind::kArc || a.kind == logo::ActKind::kPivot;
+  if (m_h.ota()) {
+    why = core::BridgeReason::kBusy;
+  } else if (!m_link.synced() || !m_link.snapshot().fast_valid) {
+    why = core::BridgeReason::kBodyOffline;
+  } else if (counted && (core::routeMove(m_link.hello().caps) != core::Route::kLocal || encoderFault())) {
+    why = core::BridgeReason::kUnavailable;
+  } else {
+    return false;
+  }
+  return true;
+}
+
+bool MotionController::startLogo(int fd, const logo::Action& a, uint32_t now, core::BridgeReason& why) {
+  if (logoRefused(a, why)) return false;
+  m_roll.cancel();  // §G6: a new primitive
+  const core::TankSnapshot& snap = m_link.snapshot();
+  core::MotionService& svc = m_motion.service();
+  const uint8_t spin = a.speed_pct < core::motion::kTurnWMin ? core::motion::kTurnWMin : a.speed_pct;
+  switch (a.kind) {  // VARIANT POINT: Logo motions (Tank Logo LLD §10)
+    case logo::ActKind::kMove:
+      svc.startMove(fd, telemOf(snap), a.dist_cm, a.speed_pct, m_cpm, kLogoPrimitive, now, m_roll_mm);
+      return true;
+    case logo::ActKind::kArc:
+      svc.startArc(fd, telemOf(snap), a.delta_cdeg, a.radius_cm, a.speed_pct, m_cpm, kLogoPrimitive, now);
+      return true;
+    case logo::ActKind::kPivot: svc.startPivot(fd, snap.fast.yaw, a.delta_cdeg, spin, kLogoPrimitive, now); return true;
+    default: svc.startTurn(fd, snap.fast.yaw, a.delta_cdeg, spin, kLogoPrimitive, now); return true;  // turn, HOME
+  }
+}
 
 bool MotionController::claimed(int fd, int32_t id, uint32_t now) {
   const core::Decision d = m_h.claim(fd, now);
````

Change `firmware/esp32/src/app/motion_controller.h` (unified diff against the previous task):
````diff
@@ -12,15 +12,22 @@
 #include "core/motion_link.h"
 #include "core/roll_learner.h"
 #include "core/ws_codec.h"
+#include "logo/effects.h"
 
 namespace cq {
 namespace app {
 
 class MotionController {
  public:
+  static constexpr int32_t kLogoPrimitive = -2;  // Plan N3: the id of a running Logo program's primitives
   struct Hooks {  // until DriveController owns these (Plan P Task 11)
     core::Decision (*claim)(int fd, uint32_t now);  // take / keep ownership (applies the owner's actions)
     bool (*ota)();
+    // Plan N3 (Tank Logo LLD §20.5): the running program's fd (-1 none), the end of its primitive, and every event
+    // that ends a drive (STOP, release, page closed, OTA, body offline, stall, the owner's own command).
+    int (*logo_fd)() = nullptr;
+    void (*logo_end)(const core::PrimitiveEnd& e, uint32_t now) = nullptr;
+    void (*logo_event)(core::MotionEvent e, uint32_t now) = nullptr;
   };
 
   MotionController(Replies& out, core::EventLog& log, core::BodyLink& link, core::LinkOwner& owner,
@@ -35,6 +42,9 @@ class MotionController {
   void onTelemetry(uint32_t now);  // a new TELEM_FAST: roll-on observation + the service step (50 Hz)
   void tick(uint32_t now);         // telemetry gap / drive_rate deadman
   void end(core::MotionEvent e, uint32_t now);
+  // Plan N3: a Logo program's move / turn / arc / pivot / HOME (ownership is the program's; no reply). False with
+  // why (kUnavailable: no encoders or an encoder fault; kBodyOffline; kBusy: OTA) when it cannot start.
+  bool startLogo(int fd, const logo::Action& a, uint32_t now, core::BridgeReason& why);
   bool ownedBy(int fd) const;
   bool active() const { return m_motion.service().active(); }
   void cancelRollOn() { m_roll.cancel(); }  // driving again / a new primitive
@@ -53,6 +63,7 @@ class MotionController {
   bool claimed(int fd, int32_t id, uint32_t now);
   void observeRoll(uint32_t now);
   bool encoderFault() const;
+  bool logoRefused(const logo::Action& a, core::BridgeReason& why) const;
 
   Replies& m_out;
   core::EventLog& m_log;
@@ -60,7 +71,7 @@ class MotionController {
   TelemetryPublisher& m_tel;
   core::MotionLink m_motion;
   core::RollLearner m_roll;
-  Hooks m_h{nullptr, nullptr};
+  Hooks m_h{nullptr, nullptr, nullptr, nullptr, nullptr};
   uint16_t m_cpm = 0;
   uint16_t m_roll_mm = 0;
   int32_t m_roll_last = -1;  // diagnostics in the calib reply: last reading (mm, -1 none) ...
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_motion_controller -f test_drive_controller`
Expected: `28 test cases: 28 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/app firmware/esp32/test/test_motion_controller
git commit -m "feat(bridge): MotionController starts a Logo program's motions and reports its ends

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 8: LogoController

**Files:** `src/app/logo_controller.h`, `logo_controller.cpp`, `logo_actions.cpp`; `test/test_logo_controller`.

**Interfaces:**
- Consumes: Tasks 1, 4, 5, 6 (RunController, Odometry, events, UnoChannel constants), `core::Decision`.
- Produces: `class LogoController { LogoController(Replies&, EventLog&, BodyLink&, TelemetryPublisher&, logo::Program& running, logo::Vm&); setHooks(Hooks); onRun(fd, id, prog, step, now); onStep(fd, id); onStop(fd, id, now); tick(now); onPrimitiveEnd(e, now); onMotionEvent(e, now); onUnoAlarm(RunEvent, now); onUnoDone(status, now); fd(); running(); }`; `Hooks { claim, keep, ota, take, start_motion, stop_tank(bool owns, now), release, uno_request, cpm }`; `enum class Took { kOk, kCheckAgain, kBusy }`.

- [ ] **Step 1: Write the failing tests**


Create `firmware/esp32/test/test_logo_controller/test_main.cpp`:
````cpp
// Plan N3: LogoController with fake hooks - run and its refusals, waiting for the Uno's ACK, SAY's pause, PLAY in
// chunks, LOOK angles, every ending stops the tank and gives the drive back, a takeover, step mode, the pose.
#include <string.h>
#include <unity.h>

#include <vector>

#include "../support/frames.h"
#include "../support/wire.h"
#include "app/logo_controller.h"
#include "fakes/fake_ports.h"
#include "logo/check_job.h"

using namespace cq;
using app::LogoController;

namespace {
core::WsClients ws;
app::Replies out(ws);
core::EventLog logbook;
core::FrameTx tx;
core::BodyLink* link = nullptr;
app::TelemetryPublisher* tel = nullptr;
logo::Program g_running, g_checked;
logo::Vm g_vm;
logo::CheckSpace g_space;
LogoController* lc = nullptr;

core::Decision g_claim = core::Decision::kSend;
core::Decision g_keep = core::Decision::kSend;
int g_keeps = 0;
bool g_stop_owned = false;
bool g_ota = false;
LogoController::Took g_took = LogoController::Took::kOk;
bool g_motion_ok = true;
core::BridgeReason g_motion_why = core::BridgeReason::kBodyOffline;
bool g_queue_ok = true;
std::vector<logo::Action> g_motions;
std::vector<core::AckedOp> g_ops;
int g_stops = 0, g_releases = 0;
uint32_t g_now = 1000;

void unoSends(const uint8_t* f, size_t n, uint32_t now) { link->onUartBytes(f, n, now); }

void synced() {
  link->startMaster(0);
  uint8_t f[64];
  size_t n = cqtest::frame(proto::HelloMsg{0x0100, proto::kProtocolVersion, 1, 1, 0x1UL}, 1, f);
  unoSends(f, n, 10);
  proto::TelemFastMsg t{};
  t.dist_mm = 65535;
  n = cqtest::frame(t, 2, f);
  unoSends(f, n, 20);
}

void hooks() {
  LogoController::Hooks h;
  h.claim = [](int, uint32_t) { return g_claim; };
  h.keep = [](int, uint32_t) {
    ++g_keeps;
    return g_keep;
  };
  h.ota = [] { return g_ota; };
  h.take = [](uint32_t, logo::Program& p, char* name, size_t cap) {
    if (g_took == LogoController::Took::kOk) {
      p = g_checked;
      snprintf(name, cap, "square");
    }
    return g_took;
  };
  h.start_motion = [](int, const logo::Action& a, uint32_t, core::BridgeReason& why) {
    g_motions.push_back(a);
    why = g_motion_why;
    return g_motion_ok;
  };
  h.stop_tank = [](bool owns, uint32_t) {
    ++g_stops;
    g_stop_owned = owns;
  };
  h.release = [](int, uint32_t) { ++g_releases; };
  h.uno_request = [](const core::AckedOp& op) {
    if (g_queue_ok) g_ops.push_back(op);
    return g_queue_ok;
  };
  h.cpm = [] { return static_cast<uint16_t>(7514); };
  lc->setHooks(h);
}

// Checks the program into the box and runs it as phone 3.
void run(const char* src, bool step = false) {
  TEST_ASSERT_TRUE_MESSAGE(logo::checkProgram(src, static_cast<uint16_t>(strlen(src)), logo::Caps(), g_space), src);
  g_checked = g_space.program;
  lc->onRun(3, 5, 1, step, g_now);
}

void ticks(int n) {
  for (int i = 0; i < n; ++i) lc->tick(g_now += 2);
}
void done() {  // the motion the program waits for has ended normally
  core::PrimitiveEnd e;
  e.ok = true;
  lc->onPrimitiveEnd(e, g_now);
}
bool sent(int fd, const char* needle) { return fakes::countTo(fd, needle) > 0; }
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  ws.add(3, 0);
  ws.add(4, 0);
  logbook = core::EventLog();
  tx = core::FrameTx();
  delete lc;
  delete tel;
  delete link;
  link = new core::BodyLink(tx);
  tel = new app::TelemetryPublisher(out, *link);
  lc = new LogoController(out, logbook, *link, *tel, g_running, g_vm);
  synced();
  hooks();
  g_claim = core::Decision::kSend;
  g_keep = core::Decision::kSend;
  g_keeps = 0;
  g_stop_owned = false;
  g_ota = false;
  g_took = LogoController::Took::kOk;
  g_motion_ok = true;
  g_queue_ok = true;
  g_motions.clear();
  g_ops.clear();
  g_stops = g_releases = 0;
}
void tearDown() {}

void test_run_starts_and_carries_out_moves() {
  run("FD 50 RT 90");
  TEST_ASSERT_TRUE(sent(3, "\"t\":\"ack\",\"id\":5"));
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_started\",\"name\":\"square\",\"by\":\"phone-"));
  TEST_ASSERT_TRUE(lc->running());
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_motions.size());
  TEST_ASSERT_EQUAL(logo::ActKind::kMove, g_motions[0].kind);
  TEST_ASSERT_EQUAL(50, g_motions[0].dist_cm);
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_line\",\"line\":1"));
  ticks(5);
  TEST_ASSERT_EQUAL(1, g_motions.size());  // waits for the move's end
  done();
  ticks(1);
  TEST_ASSERT_EQUAL(2, g_motions.size());
  TEST_ASSERT_EQUAL(-9000, g_motions[1].delta_cdeg);
  done();
  ticks(1);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_ended\",\"reason\":\"finished\""));
  TEST_ASSERT_EQUAL(1, g_stops);
  TEST_ASSERT_TRUE(g_stop_owned);  // still its drive: the STOP goes
  TEST_ASSERT_EQUAL(1, g_releases);
}

void test_run_refusals() {
  g_took = LogoController::Took::kCheckAgain;
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "CHECK_AGAIN"));
  g_took = LogoController::Took::kBusy;
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "BUSY"));
  g_took = LogoController::Took::kOk;
  g_claim = core::Decision::kNackNotOwner;
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "NOT_OWNER"));
  TEST_ASSERT_FALSE(lc->running());
  g_claim = core::Decision::kSend;
  g_ota = true;
  fakes::reset();
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "BUSY"));
  g_ota = false;
  run("FOREVER [ WAIT 1 ]");
  fakes::reset();
  run("BEEP");
  TEST_ASSERT_TRUE(sent(3, "BUSY"));  // one program at a time
}

void test_light_waits_for_the_uno_s_ack() {
  run("LED GREEN FD 10");
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());
  TEST_ASSERT_EQUAL(core::AckedOp::kLed, g_ops[0].kind);
  TEST_ASSERT_EQUAL(core::kLogoClient, g_ops[0].fd);
  TEST_ASSERT_EQUAL(255, g_ops[0].g);
  ticks(20);
  TEST_ASSERT_EQUAL(0, g_motions.size());  // still waiting
  lc->onUnoDone(0, g_now);
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_motions.size());
}

void test_no_answer_ends_with_7034_a_refusal_goes_on() {
  run("SOUND HORN BEEP FD 10");
  ticks(1);
  TEST_ASSERT_EQUAL(24, g_ops[0].a);
  lc->onUnoDone(static_cast<uint8_t>(proto::AckStatus::kBusy), g_now);  // refused: logged, goes on
  ticks(1);
  TEST_ASSERT_EQUAL(2, g_ops.size());
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::SoundId::kBlip), g_ops[1].a);  // BEEP
  lc->onUnoDone(app::UnoChannel::kLogoTimeout, g_now);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7034"));
  TEST_ASSERT_EQUAL(1, g_stops);
}

void test_a_full_queue_is_tried_again() {
  g_queue_ok = false;
  run("BEEP");
  ticks(3);
  TEST_ASSERT_EQUAL(0, g_ops.size());
  g_queue_ok = true;
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());
}

void test_say_then_a_pause() {
  run("SAY [ hello there ] FD 10");
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_say\",\"text\":\"hello there\""));
  ticks(50);  // 100 ms
  TEST_ASSERT_EQUAL(0, g_motions.size());
  ticks(60);  // past 200 ms
  TEST_ASSERT_EQUAL(1, g_motions.size());
}

void test_play_goes_in_chunks_of_ten_after_each_other() {
  run("PLAY [ C4 D4 E4 F4 G4 A4 B4 C5 D5 E5 F5 G5 ]");  // 12 notes of 250 ms
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());
  TEST_ASSERT_EQUAL(core::AckedOp::kTone, g_ops[0].kind);
  TEST_ASSERT_EQUAL(10, g_ops[0].n_notes);
  lc->onUnoDone(0, g_now);
  ticks(100);  // 200 ms: the first 10 notes play for 2.5 s
  TEST_ASSERT_EQUAL(1, g_ops.size());
  ticks(1200);  // 2.4 s more
  TEST_ASSERT_EQUAL(2, g_ops.size());
  TEST_ASSERT_EQUAL(2, g_ops[1].n_notes);
  TEST_ASSERT_EQUAL(784, g_ops[1].hz[1]);  // G5
}

void test_look_angles_and_home() {
  run("LOOK [ 30 -10 ] LOOK HOME");
  ticks(1);
  TEST_ASSERT_EQUAL(core::AckedOp::kCamSet, g_ops[0].kind);
  TEST_ASSERT_EQUAL(600, g_ops[0].pan_dd);   // right 30° = the smaller servo angle
  TEST_ASSERT_EQUAL(1000, g_ops[0].tilt_dd);  // down 10°
  lc->onUnoDone(0, g_now);
  ticks(1);
  TEST_ASSERT_EQUAL(core::AckedOp::kCamHome, g_ops[1].kind);
}

void test_stop_while_waiting_ends_once() {
  run("WAIT 10 FD 10");
  ticks(1);
  lc->onMotionEvent(core::MotionEvent::kStop, g_now);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7040"));
  lc->onMotionEvent(core::MotionEvent::kOwnerClosed, g_now);
  lc->onPrimitiveEnd(core::PrimitiveEnd(), g_now);
  TEST_ASSERT_EQUAL(1, fakes::countTo(4, "logo_ended"));
  TEST_ASSERT_EQUAL(1, g_stops);
  TEST_ASSERT_EQUAL(1, g_releases);
}

void test_another_device_taking_the_drive_ends_it_without_stopping_them() {
  run("FOREVER [ WAIT 1 ]");
  ticks(2);
  g_keep = core::Decision::kNackNotOwner;  // a phone or the IR remote took over
  ticks(1);
  TEST_ASSERT_FALSE(lc->running());
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7045"));
  TEST_ASSERT_EQUAL(1, g_stops);
  TEST_ASSERT_FALSE(g_stop_owned);  // bridge LLD R4.6: no STOP for the new driver
}

void test_a_waiting_program_keeps_the_drive_every_tick() {
  run("WAIT 10");
  ticks(3000);  // 6 s: longer than the 5 s handback
  TEST_ASSERT_TRUE(lc->running());
  TEST_ASSERT_TRUE(g_keeps >= 3000);
}

void test_the_body_gone_ends_it_with_7034() {
  run("FOREVER [ WAIT 1 ]");
  g_keep = core::Decision::kNackBodyOffline;
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7034"));
}

void test_logo_stop_from_any_page_and_not_running() {
  run("FOREVER [ WAIT 1 ]");
  lc->onStop(4, 9, g_now);
  TEST_ASSERT_TRUE(sent(4, "\"t\":\"ack\",\"id\":9"));
  TEST_ASSERT_TRUE(sent(3, "\"reason\":7040"));
  lc->onStop(4, 10, g_now);
  TEST_ASSERT_TRUE(sent(4, "NOT_RUNNING"));
  lc->onStep(4, 11);
  TEST_ASSERT_EQUAL(2, fakes::countTo(4, "NOT_RUNNING"));
}

void test_a_motion_that_cannot_start_ends_it() {
  g_motion_ok = false;
  g_motion_why = core::BridgeReason::kUnavailable;
  run("FD 10");
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7021"));
  TEST_ASSERT_FALSE(lc->running());
}

void test_battery_and_tilt_alarms() {
  run("FOREVER [ WAIT 1 ]");
  lc->onUnoAlarm(logo::RunEvent::kBatteryLow, g_now);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7042"));
  run("FOREVER [ WAIT 1 ]");
  lc->onUnoAlarm(logo::RunEvent::kTilt, g_now);
  TEST_ASSERT_TRUE(sent(4, "\"reason\":7043"));
}

void test_step_mode() {
  run("BEEP\nBEEP", true);
  ticks(1);
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_line\",\"line\":1"));
  ticks(5);
  TEST_ASSERT_EQUAL(0, g_ops.size());  // paused before line 1
  lc->onStep(3, 12);
  ticks(1);
  TEST_ASSERT_EQUAL(1, g_ops.size());
}

void test_pose_ten_times_a_second() {
  run("FOREVER [ WAIT 1 ]");
  ticks(250);  // 0.5 s
  const size_t n = fakes::countTo(4, "logo_pose");
  TEST_ASSERT_TRUE(n >= 4 && n <= 6);
  TEST_ASSERT_TRUE(sent(4, "\"x_cm\":0.0,\"y_cm\":0.0,\"heading\":0"));
}

void test_arc_speed_note() {
  run("SPEED 100 ARC RIGHT 90 40");
  ticks(1);
  lc->onUnoDone(0, g_now);
  ticks(2);
  TEST_ASSERT_EQUAL(logo::ActKind::kArc, g_motions.back().kind);
  TEST_ASSERT_TRUE(sent(4, "\"kind\":\"logo_note\""));
  TEST_ASSERT_TRUE(sent(4, "for this radius"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_run_starts_and_carries_out_moves);
  RUN_TEST(test_run_refusals);
  RUN_TEST(test_light_waits_for_the_uno_s_ack);
  RUN_TEST(test_no_answer_ends_with_7034_a_refusal_goes_on);
  RUN_TEST(test_a_full_queue_is_tried_again);
  RUN_TEST(test_say_then_a_pause);
  RUN_TEST(test_play_goes_in_chunks_of_ten_after_each_other);
  RUN_TEST(test_look_angles_and_home);
  RUN_TEST(test_stop_while_waiting_ends_once);
  RUN_TEST(test_another_device_taking_the_drive_ends_it_without_stopping_them);
  RUN_TEST(test_a_waiting_program_keeps_the_drive_every_tick);
  RUN_TEST(test_the_body_gone_ends_it_with_7034);
  RUN_TEST(test_logo_stop_from_any_page_and_not_running);
  RUN_TEST(test_a_motion_that_cannot_start_ends_it);
  RUN_TEST(test_battery_and_tilt_alarms);
  RUN_TEST(test_step_mode);
  RUN_TEST(test_pose_ten_times_a_second);
  RUN_TEST(test_arc_speed_note);
  return UNITY_END();
}
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_controller`
Expected: FAIL — `fatal error: 'app/logo_controller.h' file not found`.

- [ ] **Step 3: Implement**


Create `firmware/esp32/src/app/logo_actions.cpp`:
````cpp
// LogoController: carrying out the program's actions (Plan N3, Tank Logo LLD §20.5).
#include <stdio.h>

#include "app/logo_controller.h"
#include "cq_protocol.h"

namespace cq {
namespace app {
namespace {

int16_t servoDd(int16_t deg_from_centre) {  // LOOK degrees (right / up = +) → the Uno's servo angle (0.1°, 900 centre)
  const int32_t dd = 900 - static_cast<int32_t>(deg_from_centre) * 10;  // right and up are the smaller angles
  return static_cast<int16_t>(dd < 0 ? 0 : (dd > 1800 ? 1800 : dd));     // the Uno clamps to its own limits too
}

core::AckedOp op(core::AckedOp::Kind k) {
  core::AckedOp o;
  o.kind = k;
  o.fd = core::kLogoClient;
  return o;
}

core::AckedOp ledOp(const logo::Action& a) {
  core::AckedOp o = op(core::AckedOp::kLed);
  o.a = static_cast<uint8_t>(proto::LedMode::kSolid);
  o.r = a.r;
  o.g = a.g;
  o.b = a.b;
  return o;
}

core::AckedOp soundOp(const logo::Action& a) {  // BEEP = SOUND BLIP (design-notes §102)
  core::AckedOp o = op(core::AckedOp::kSound);
  o.a = a.kind == logo::ActKind::kBeep ? static_cast<uint8_t>(proto::SoundId::kBlip) : a.sound_id;
  return o;
}

core::AckedOp lookOp(const logo::Action& a) {
  core::AckedOp o = op(a.kind == logo::ActKind::kLook ? core::AckedOp::kCamSet : core::AckedOp::kCamHome);
  o.pan_dd = servoDd(a.pan_deg);
  o.tilt_dd = servoDd(a.tilt_deg);
  return o;
}

bool isMotion(logo::ActKind k) {
  return k == logo::ActKind::kMove || k == logo::ActKind::kTurn || k == logo::ActKind::kArc ||
         k == logo::ActKind::kPivot || k == logo::ActKind::kHome;
}

}  // namespace

void LogoController::carry(const logo::Action& a, uint32_t now) {
  if (isMotion(a.kind)) return motion(a, now);
  switch (a.kind) {  // VARIANT POINT: actions (Tank Logo LLD §10, §20.5)
    case logo::ActKind::kLed: return request(ledOp(a));
    case logo::ActKind::kBeep: case logo::ActKind::kSound: return request(soundOp(a));
    case logo::ActKind::kLook: case logo::ActKind::kLookHome: return request(lookOp(a));
    case logo::ActKind::kTone: return startTone(a, now);
    case logo::ActKind::kSay:
      m_out.all(core::jsonLogoSay(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), a.text));
      m_hold_until = now + kSayPauseMs;
      return;
    default: return;  // SPEED, WAIT, WAIT UNTIL: the RunController's own
  }
}

// A move / turn / arc / pivot / HOME as the program's primitive; refused → the program ends (7021 / 7034).
void LogoController::motion(const logo::Action& a, uint32_t now) {
  note(a);
  core::BridgeReason why = core::BridgeReason::kBodyOffline;
  if (m_h.start_motion(m_fd, a, now, why)) return;
  handle(m_rc.onMotionEvent(why == core::BridgeReason::kUnavailable ? core::MotionEvent::kBodyEncoderFault
                                                                    : core::MotionEvent::kBodyOffline), now);
}

// logo_note: an arc's speed moved into its radius's band.
void LogoController::note(const logo::Action& a) {
  if (a.kind != logo::ActKind::kArc || !a.speed_moved) return;
  char text[96], what[32];
  logo::actionText(a, what, sizeof what);
  snprintf(text, sizeof text, "%s: speed set to %u %% for this radius", what, a.speed_pct);
  m_out.all(core::jsonLogoNote(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), a.line, text));
}

// The program goes on at the Uno's ACK (§102); a full queue is tried again next tick.
void LogoController::request(core::AckedOp o) {
  m_pending = o;
  m_has_pending = true;
  pendingSent();
}

bool LogoController::pendingSent() {
  if (!m_has_pending) return true;
  if (!m_h.uno_request(m_pending)) return false;
  m_has_pending = false;
  m_wait_ack = true;
  return false;
}

// PLAY: TONE frames of at most 10 notes, each sent when the one before has played (a new TONE replaces the tune).
void LogoController::startTone(const logo::Action& a, uint32_t now) {
  m_tone_n = a.n_notes;
  m_tone_i = 0;
  for (uint8_t i = 0; i < a.n_notes; ++i) m_tone[i] = a.notes[i];
  toneChunk(now);
}

void LogoController::toneChunk(uint32_t now) {
  core::AckedOp o = op(core::AckedOp::kTone);
  uint32_t ms = 0;
  while (o.n_notes < kToneChunk && m_tone_i < m_tone_n) {
    const logo::Note& n = m_tone[m_tone_i++];
    o.hz[o.n_notes] = n.hz;
    o.ms[o.n_notes++] = n.ms;
    ms += n.ms;
  }
  m_tone_next = now + ms;
  request(o);
}

}  // namespace app
}  // namespace cq
````

Create `firmware/esp32/src/app/logo_controller.cpp`:
````cpp
#include "app/logo_controller.h"

#include <stdio.h>

#include "cq_protocol.h"

namespace cq {
namespace app {
namespace {

bool flag(uint32_t flags, proto::FastFlagBit b) { return ((flags >> static_cast<unsigned>(b)) & 1u) != 0; }

uint8_t batteryPct(uint16_t mv) {  // 2S pack: 6.6 V = 0 %, 8.4 V = 100 % (words.yaml BATTERY)
  if (mv <= 6600) return 0;
  if (mv >= 8400) return 100;
  return static_cast<uint8_t>((mv - 6600) * 100u / 1800u);
}

}  // namespace

// Why logo_run is refused now (answered), or false: then the program is in m_prog and the drive is ours.
bool LogoController::runRefused(int fd, int32_t id, uint32_t prog, uint32_t now) {
  core::BridgeReason why = core::BridgeReason::kBusy;
  if (m_running || m_h.ota()) {
    m_out.nack(fd, id, why);
    return true;
  }
  const Took t = m_h.take(prog, m_prog, m_name, sizeof m_name);
  if (t != Took::kOk) {
    m_out.nack(fd, id, t == Took::kBusy ? core::BridgeReason::kBusy : core::BridgeReason::kCheckAgain);
    return true;
  }
  const core::Decision d = m_h.claim(fd, now);
  if (d != core::Decision::kSend) {
    m_out.nack(fd, id, reasonOf(d));
    return true;
  }
  return false;
}

void LogoController::onRun(int fd, int32_t id, uint32_t prog, bool step, uint32_t now) {
  if (runRefused(fd, id, prog, now)) return;
  logo::RunConfig cfg;
  cfg.step = step;
  cfg.seed = now | 1u;
  if (!m_rc.start(m_prog, cfg, telem(), now)) return m_out.nack(fd, id, core::BridgeReason::kCheckAgain);
  const core::TankSnapshot& s = m_link.snapshot();
  m_odo.start(s.fast.enc_l, s.fast.enc_r, s.fast.yaw, m_h.cpm());
  m_running = true;
  m_fd = fd;
  m_wait_ack = m_has_pending = false;
  m_hold_until = m_last_pose = now;
  m_tone_n = m_tone_i = 0;
  m_out.ack(fd, id);
  m_out.all(core::jsonLogoStarted(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), m_name,
                                  m_out.clients().name(fd)));
}

void LogoController::onStep(int fd, int32_t id) {
  if (!m_running) return m_out.nack(fd, id, core::BridgeReason::kNotRunning);
  handle(m_rc.onEvent(logo::RunEvent::kStep), 0);
  m_out.ack(fd, id);
}

void LogoController::onStop(int fd, int32_t id, uint32_t now) {  // from any page (LLD §20.4)
  if (!m_running) return m_out.nack(fd, id, core::BridgeReason::kNotRunning);
  handle(m_rc.onEvent(logo::RunEvent::kLogoStop), now);
  m_out.ack(fd, id);
}

void LogoController::tick(uint32_t now) {
  if (!m_running) return;
  const core::Decision d = m_h.keep(m_fd, now);  // keeps the drive while it runs (no 5 s handback)
  if (d != core::Decision::kSend) {             // another phone or the IR remote took over, or the body went
    return handle(m_rc.onMotionEvent(d == core::Decision::kNackBodyOffline ? core::MotionEvent::kBodyOffline
                                                                          : core::MotionEvent::kRelease), now);
  }
  pose(now);
  if (!pendingSent() || m_wait_ack) return;  // the program goes on at the Uno's ACK (§102)
  if (m_tone_i < m_tone_n && static_cast<int32_t>(now - m_tone_next) >= 0) return toneChunk(now);
  if (static_cast<int32_t>(now - m_hold_until) < 0) return;  // SAY's pause
  handle(m_rc.tick(telem(), now), now);
}

void LogoController::onPrimitiveEnd(const core::PrimitiveEnd& e, uint32_t now) {
  if (m_running) handle(m_rc.onPrimitiveEnd(e), now);
}

void LogoController::onMotionEvent(core::MotionEvent e, uint32_t now) {
  if (m_running) handle(m_rc.onMotionEvent(e), now);
}

void LogoController::onUnoAlarm(logo::RunEvent e, uint32_t now) {
  if (m_running) handle(m_rc.onEvent(e), now);
}

void LogoController::onUnoDone(uint8_t status, uint32_t now) {
  if (!m_running || !m_wait_ack) return;
  m_wait_ack = false;
  if (status == UnoChannel::kLogoTimeout || status == UnoChannel::kLogoOffline) {
    return handle(m_rc.onMotionEvent(core::MotionEvent::kBodyOffline), now);  // 7034
  }
  if (status != 0) m_log.add(core::LogKind::kLogo, -1, status, "refused", m_tel.tankMs(), now);  // goes on
}

void LogoController::handle(const logo::RunOut& o, uint32_t now) {
  if (o.line) {
    m_out.all(core::jsonLogoLine(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), o.line_no, o.round));
  }
  if (o.act) carry(o.action, now);
  if (o.ended) finish(o, now);
}

// Every ending (LLD §9 / §20.5): stop the tank, give the drive back, tell every page.
void LogoController::finish(const logo::RunOut& o, uint32_t now) {
  if (!m_running) return;
  m_running = false;
  m_h.stop_tank(m_h.keep(m_fd, now) == core::Decision::kSend, now);  // never STOP a new driver
  m_h.release(m_fd, now);
  m_out.all(core::jsonLogoEnded(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), o.code, o.end_line,
                                o.msg));
  m_log.add(core::LogKind::kLogo, o.code, o.end_line, m_name, m_tel.tankMs(), now);
  m_fd = -1;
  m_wait_ack = m_has_pending = false;
  m_tone_n = m_tone_i = 0;
}

void LogoController::pose(uint32_t now) {
  if (now - m_last_pose < kPoseMs) return;
  m_last_pose = now;
  const core::TankSnapshot& s = m_link.snapshot();
  const logo::Pose p = m_odo.update(s.fast.enc_l, s.fast.enc_r, s.fast.yaw);
  m_out.all(core::jsonLogoPose(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), p.x_mm, p.y_mm,
                               p.heading_deg));
}

logo::RunTelem LogoController::telem() const {
  const core::TankSnapshot& s = m_link.snapshot();
  logo::RunTelem t;
  t.yaw_cdeg = s.fast.yaw;
  t.distance_mm = s.fast.dist_mm >= 65534 ? 0 : s.fast.dist_mm;  // 65535 no echo, 65534 sensor fault
  t.tilted = flag(s.fast.flags, proto::FastFlagBit::kTilted) || flag(s.fast.flags, proto::FastFlagBit::kLifted);
  t.battery_pct = s.slow_valid ? batteryPct(s.slow.batt_mv) : 100;
  return t;
}

}  // namespace app
}  // namespace cq
````

Create `firmware/esp32/src/app/logo_controller.h`:
````cpp
// A Tank Logo program running on the tank (Plan N3, Tank Logo LLD §20.3, §20.5): logo_run / logo_step / logo_stop,
// the RunController's slices each tick, its actions carried out (moves through MotionController, light / sound /
// tune / camera through UnoChannel, waiting for the Uno's ACK; SAY to the pages), the driven pose, and every ending
// - which stops the tank and gives the drive back. Link task only. Hooks are captureless functions (no virtual).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/replies.h"
#include "app/telemetry_publisher.h"
#include "app/uno_channel.h"
#include "core/acked_ops.h"
#include "core/body_link.h"
#include "core/event_log.h"
#include "core/link_owner.h"
#include "core/motion_link.h"
#include "logo/odometry.h"
#include "logo/program.h"
#include "logo/run_controller.h"
#include "logo/vm.h"

namespace cq {
namespace app {

class LogoController {
 public:
  enum class Took : uint8_t { kOk, kCheckAgain, kBusy };
  struct Hooks {
    core::Decision (*claim)(int fd, uint32_t now) = nullptr;  // take the drive like a manual drive
    // Still this page's drive? Also keeps it (the 5 s handback, like a running primitive) - LinkOwner::keepDriving.
    core::Decision (*keep)(int fd, uint32_t now) = nullptr;
    bool (*ota)() = nullptr;
    Took (*take)(uint32_t prog, logo::Program& out, char* name, size_t cap) = nullptr;  // the checked program
    bool (*start_motion)(int fd, const logo::Action& a, uint32_t now, core::BridgeReason& why) = nullptr;
    // Ends a running primitive; a STOP frame only while the program still owns the drive (bridge LLD R4.6).
    void (*stop_tank)(bool owns, uint32_t now) = nullptr;
    void (*release)(int fd, uint32_t now) = nullptr;
    bool (*uno_request)(const core::AckedOp& op) = nullptr;  // false: the queue is full (tried again next tick)
    uint16_t (*cpm)() = nullptr;
  };
  static constexpr uint32_t kSayPauseMs = 200;  // design-notes §102
  static constexpr uint32_t kPoseMs = 100;      // logo_pose at 10 Hz
  static constexpr uint8_t kToneChunk = 10;     // TONE frame: at most 10 notes

  LogoController(Replies& out, core::EventLog& log, core::BodyLink& link, TelemetryPublisher& tel,
                 logo::Program& running, logo::Vm& vm)
      : m_out(out), m_log(log), m_link(link), m_tel(tel), m_prog(running), m_rc(vm) {}
  void setHooks(const Hooks& h) { m_h = h; }
  void onRun(int fd, int32_t id, uint32_t prog, bool step, uint32_t now);  // logo_run
  void onStep(int fd, int32_t id);
  void onStop(int fd, int32_t id, uint32_t now);
  void tick(uint32_t now);
  void onPrimitiveEnd(const core::PrimitiveEnd& e, uint32_t now);
  void onMotionEvent(core::MotionEvent e, uint32_t now);
  void onUnoAlarm(logo::RunEvent e, uint32_t now);  // battery critical, tilted / lifted
  void onUnoDone(uint8_t status, uint32_t now);      // the answer to the program's request (UnoChannel)
  int fd() const { return m_running ? m_fd : -1; }
  bool running() const { return m_running; }

 private:
  // logo_controller.cpp
  bool runRefused(int fd, int32_t id, uint32_t prog, uint32_t now);
  void handle(const logo::RunOut& o, uint32_t now);
  void finish(const logo::RunOut& o, uint32_t now);
  void pose(uint32_t now);
  logo::RunTelem telem() const;
  // logo_actions.cpp
  void carry(const logo::Action& a, uint32_t now);
  void motion(const logo::Action& a, uint32_t now);
  void request(core::AckedOp op);
  bool pendingSent();
  void startTone(const logo::Action& a, uint32_t now);
  void toneChunk(uint32_t now);
  void note(const logo::Action& a);

  Replies& m_out;
  core::EventLog& m_log;
  core::BodyLink& m_link;
  TelemetryPublisher& m_tel;
  logo::Program& m_prog;
  logo::RunController m_rc;
  logo::Odometry m_odo;
  Hooks m_h;
  bool m_running = false;
  int m_fd = -1;
  char m_name[25] = "";
  bool m_wait_ack = false;
  bool m_has_pending = false;
  core::AckedOp m_pending;
  uint32_t m_hold_until = 0;  // SAY's pause
  uint32_t m_last_pose = 0;
  logo::Note m_tone[logo::NoteReader::kMaxNotes];
  uint8_t m_tone_n = 0, m_tone_i = 0;
  uint32_t m_tone_next = 0;
};

}  // namespace app
}  // namespace cq
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_logo_controller`
Expected: `18 test cases: 18 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src/app firmware/esp32/test/test_logo_controller
git commit -m "feat(bridge): LogoController - run, ACK pacing, PLAY chunks, SAY pause, endings, takeovers

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 9: The logo_* commands, their routes and the Uno's alarms

**Files:** `src/core/ws_codec.h`, `ws_codec.cpp`, `ws_codec_internal.h`, `ws_codec_keys.cpp`; `src/app/drive_controller.cpp`, `command_router.h/.cpp`, `uno_event_router.h/.cpp`; tests `test_ws_codec`, `test_command_router`, `test_uno_event_router`.

**Interfaces:**
- Consumes: `LogoController` (Task 8).
- Produces: `WsCmdKind::kLogoRun / kLogoStep / kLogoStop`, `WsCommand::prog`, `WsCommand::step`; `CommandRouter(DriveController&, MotionController&, SettingsController&, TankAppController&, LogoController&, TelemetryPublisher&, Replies&)`; `UnoEventRouter::Sinks::alarm` (batt_critical, tilted, lifted); `logo_run` is a motion command for the STOP barrier.

- [ ] **Step 1: Write the failing tests**


Change `firmware/esp32/test/test_command_router/test_main.cpp` (unified diff against the previous task):
````diff
@@ -24,6 +24,9 @@ app::SettingsController* settings = nullptr;
 app::TankAppController* tank = nullptr;
 app::DriveController* drive = nullptr;
 app::CommandRouter* router = nullptr;
+logo::Program g_logo_program;
+logo::Vm g_logo_vm;
+app::LogoController* logoc = nullptr;
 cqtest::Wire wire;
 uint32_t g_post = 0;
 
@@ -48,7 +51,7 @@ void setUp() {
   fakes::reset();
   ws = core::WsClients();
   logbook = core::EventLog();
-  delete router; delete drive; delete tank; delete settings; delete motion; delete uno; delete tel; delete owner;
+  delete router; delete logoc; delete drive; delete tank; delete settings; delete motion; delete uno; delete tel; delete owner;
   delete link; delete tx;
   tx = new core::FrameTx();
   link = new core::BodyLink(*tx);
@@ -59,7 +62,8 @@ void setUp() {
   settings = new app::SettingsController(out, logbook, *link);
   tank = new app::TankAppController(out, logbook, *link);
   drive = new app::DriveController(out, logbook, *owner, *uno, *motion, *settings, *tel);
-  router = new app::CommandRouter(*drive, *motion, *settings, *tank, *tel, out);
+  logoc = new app::LogoController(out, logbook, *link, *tel, g_logo_program, g_logo_vm);
+  router = new app::CommandRouter(*drive, *motion, *settings, *tank, *logoc, *tel, out);
   router->setHello("abc1234", sensor);
   motion->setHooks({[](int fd, uint32_t now) { return drive->claim(fd, now); }, [] { return drive->otaActive(); }});
   tel->setViews({[] { return drive->ownerView(); }, [] { return motion->view(); }, [] { return !link->synced(); }});
@@ -123,6 +127,14 @@ void test_ota_answers_every_waiter() {  // Review Focus 5
   TEST_ASSERT_TRUE(wire.count(FT::kStop) >= 1);
 }
 
+// Plan N3: the logo_* commands reach the LogoController (nothing runs: NOT_RUNNING).
+void test_logo_commands_are_routed() {
+  text(3, "{\"t\":\"logo_stop\",\"id\":7}", 100);
+  TEST_ASSERT_TRUE(fakes::countTo(3, "NOT_RUNNING") == 1);
+  text(3, "{\"t\":\"logo_step\",\"id\":8}", 102);
+  TEST_ASSERT_TRUE(fakes::countTo(3, "NOT_RUNNING") == 2);
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_every_kind_has_a_route_except_the_invalid_ones);
@@ -132,5 +144,6 @@ int main() {
   RUN_TEST(test_stop_bypasses_admission_even_for_an_unknown_client);
   RUN_TEST(test_commands_from_an_unknown_client_are_dropped);
   RUN_TEST(test_ota_answers_every_waiter);
+  RUN_TEST(test_logo_commands_are_routed);
   return UNITY_END();
 }
````

Change `firmware/esp32/test/test_uno_event_router/test_main.cpp` (unified diff against the previous task):
````diff
@@ -13,6 +13,7 @@ core::EventLog logbook;
 core::FrameTx tx;
 core::BodyLink link(tx);
 app::TelemetryPublisher tel(out, link);
+int g_alarm = -1;
 int g_online = 0, g_offline = 0, g_owner = -1, g_button = 0, g_motion = -1, g_ack = -1, g_stop = -1, g_value = -1;
 
 app::UnoEventRouter::Sinks sinks() {
@@ -25,6 +26,7 @@ app::UnoEventRouter::Sinks sinks() {
   s.ack_result = [](uint8_t seq, uint8_t) { g_ack = seq; };
   s.stop_result = [](uint8_t st) { g_stop = st; };
   s.setting_value = [](uint8_t, int32_t v) { g_value = v; };
+  s.alarm = [](uint8_t e, uint32_t) { g_alarm = e; };
   return s;
 }
 app::UnoEventRouter router(out, logbook, tel, link);
@@ -40,7 +42,7 @@ void setUp() {
   ws.add(3, 0);
   logbook = core::EventLog();
   g_online = g_offline = g_button = 0;
-  g_owner = g_motion = g_ack = g_stop = g_value = -1;
+  g_owner = g_motion = g_ack = g_stop = g_value = g_alarm = -1;
   router.setSinks(sinks());
 }
 void tearDown() {}
@@ -94,6 +96,18 @@ void test_answers_reach_their_sinks() {
   TEST_ASSERT_EQUAL(19, g_value);
 }
 
+// Plan N3: battery critical, tilted and lifted go to the alarm sink (a running Logo program ends).
+void test_alarms_go_to_the_alarm_sink() {
+  router.route(unoEvent(proto::EventKind::kBattLow), 0);
+  TEST_ASSERT_EQUAL(-1, g_alarm);  // a warning, not an alarm
+  router.route(unoEvent(proto::EventKind::kBattCritical), 0);
+  TEST_ASSERT_EQUAL(static_cast<int>(proto::EventKind::kBattCritical), g_alarm);
+  router.route(unoEvent(proto::EventKind::kLifted), 0);
+  TEST_ASSERT_EQUAL(static_cast<int>(proto::EventKind::kLifted), g_alarm);
+  router.route(unoEvent(proto::EventKind::kTilted), 0);
+  TEST_ASSERT_EQUAL(static_cast<int>(proto::EventKind::kTilted), g_alarm);
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_uno_event_broadcast_and_logged_except_button_and_unblocked);
@@ -103,5 +117,6 @@ int main() {
   RUN_TEST(test_body_offline_broadcast_logged_and_sunk);
   RUN_TEST(test_body_online_broadcast_logged_and_sunk);
   RUN_TEST(test_answers_reach_their_sinks);
+  RUN_TEST(test_alarms_go_to_the_alarm_sink);
   return UNITY_END();
 }
````

Change `firmware/esp32/test/test_ws_codec/test_main.cpp` (unified diff against the previous task):
````diff
@@ -409,6 +409,23 @@ void test_app_url_validation() {
   TEST_ASSERT_TRUE(cq::core::appUrlValid(""));
 }
 
+// Plan N3: the logo_* commands (Tank Logo LLD §20.4).
+void test_logo_commands() {
+  WsCommand c = P("{\"t\":\"logo_run\",\"prog\":17,\"step\":true,\"id\":5}");
+  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kLogoRun, c.kind);
+  TEST_ASSERT_EQUAL(17, c.prog);
+  TEST_ASSERT_TRUE(c.step);
+  TEST_ASSERT_EQUAL(5, c.id);
+  c = P("{\"t\":\"logo_run\",\"prog\":4294967295}");
+  TEST_ASSERT_EQUAL(4294967295u, c.prog);
+  TEST_ASSERT_FALSE(c.step);
+  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kInvalid, P("{\"t\":\"logo_run\"}").kind);  // no program number
+  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kInvalid, P("{\"t\":\"logo_run\",\"prog\":0}").kind);
+  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kInvalid, P("{\"t\":\"logo_run\",\"prog\":1.5}").kind);
+  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kLogoStep, P("{\"t\":\"logo_step\",\"id\":6}").kind);
+  TEST_ASSERT_EQUAL(cq::core::WsCmdKind::kLogoStop, P("{\"t\":\"logo_stop\"}").kind);
+}
+
 int main() {
   UNITY_BEGIN();
   RUN_TEST(test_drive);
@@ -445,5 +462,6 @@ int main() {
   RUN_TEST(test_camera_set_false_turns_a_switch_off);
   RUN_TEST(test_app_url_validation);
   RUN_TEST(test_app_url_too_long_is_refused_by_the_parser);
+  RUN_TEST(test_logo_commands);
   return UNITY_END();
 }
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_codec -f test_command_router -f test_uno_event_router -f test_drive_controller`
Expected: FAIL — `no type named 'LogoController' in namespace 'cq::app'` (the router test builds a LogoController).

- [ ] **Step 3: Implement**


Change `firmware/esp32/src/app/command_router.cpp` (unified diff against the previous task):
````diff
@@ -41,6 +41,9 @@ const CommandRouter::Route CommandRouter::kRoutes[] = {
     {K::kCameraSet, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_tank.onCameraSet(fd, id, c, now); }},
     {K::kSelfTest, [](C& r, int fd, int32_t id, const W&, uint32_t) { r.m_tank.onSelfTest(fd, id); }},
     {K::kAppSet, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_tank.onAppSet(fd, id, c); }},
+    {K::kLogoRun, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_logo.onRun(fd, id, c.prog, c.step, now); }},
+    {K::kLogoStep, [](C& r, int fd, int32_t id, const W&, uint32_t) { r.m_logo.onStep(fd, id); }},
+    {K::kLogoStop, [](C& r, int fd, int32_t id, const W&, uint32_t now) { r.m_logo.onStop(fd, id, now); }},
 };
 
 CommandRouter::Handler CommandRouter::find(core::WsCmdKind kind) {
````

Change `firmware/esp32/src/app/command_router.h` (unified diff against the previous task):
````diff
@@ -5,6 +5,7 @@
 #include <stdint.h>
 
 #include "app/drive_controller.h"
+#include "app/logo_controller.h"
 #include "app/motion_controller.h"
 #include "app/replies.h"
 #include "app/settings_controller.h"
@@ -17,9 +18,9 @@ namespace app {
 
 class CommandRouter {
  public:
-  CommandRouter(DriveController& d, MotionController& m, SettingsController& s, TankAppController& t,
+  CommandRouter(DriveController& d, MotionController& m, SettingsController& s, TankAppController& t, LogoController& l,
                 TelemetryPublisher& tel, Replies& out)
-      : m_drive(d), m_motion(m), m_settings(s), m_tank(t), m_tel(tel), m_out(out) {}
+      : m_drive(d), m_motion(m), m_settings(s), m_tank(t), m_logo(l), m_tel(tel), m_out(out) {}
   void setHello(const char* fw, const char* (*sensor)()) {
     m_fw = fw;
     m_sensor = sensor;
@@ -40,6 +41,7 @@ class CommandRouter {
   MotionController& m_motion;
   SettingsController& m_settings;
   TankAppController& m_tank;
+  LogoController& m_logo;
   TelemetryPublisher& m_tel;
   Replies& m_out;
   const char* m_fw = "";
````

Change `firmware/esp32/src/app/drive_controller.cpp` (unified diff against the previous task):
````diff
@@ -13,7 +13,9 @@ namespace {
 bool isStream(core::WsCmdKind k) {
   return k == core::WsCmdKind::kDrive || k == core::WsCmdKind::kCamJoy || k == core::WsCmdKind::kDriveRate;
 }
-bool isMotion(core::WsCmdKind k) { return isStream(k) || k == core::WsCmdKind::kTurn || k == core::WsCmdKind::kMove; }
+bool isMotion(core::WsCmdKind k) {  // posted before a STOP that was handled first: dropped (logo_run too, Plan N3)
+  return isStream(k) || k == core::WsCmdKind::kTurn || k == core::WsCmdKind::kMove || k == core::WsCmdKind::kLogoRun;
+}
 
 core::AckedOp requestOf(int fd, int32_t id, const core::WsCommand& c) {
   core::AckedOp op;
````

Change `firmware/esp32/src/app/uno_event_router.cpp` (unified diff against the previous task):
````diff
@@ -7,6 +7,12 @@ namespace app {
 namespace {
 using EK = proto::EventKind;
 bool is(uint8_t code, EK k) { return code == static_cast<uint8_t>(k); }
+// Plan N3: the Uno's alarms that end a running Logo program.
+bool alarm(uint8_t code) {
+  return code == static_cast<uint8_t>(proto::EventKind::kBattCritical) ||
+         code == static_cast<uint8_t>(proto::EventKind::kTilted) || code == static_cast<uint8_t>(proto::EventKind::kLifted);
+}
+
 }  // namespace
 
 void UnoEventRouter::bodyOnline(uint32_t now) {
@@ -39,6 +45,8 @@ void UnoEventRouter::unoEvent(const core::LinkEvent& ev, uint32_t now) {
     m_sinks.button_press(now);  // Plan M: confirm a waiting change, or the WiFi double press
   } else if (is(ev.code, EK::kStall) || is(ev.code, EK::kEncoderFault)) {
     m_sinks.motion_event(ev.code, now);  // Plan K: ends a move (turn / rate unchanged)
+  } else if (alarm(ev.code) && m_sinks.alarm != nullptr) {
+    m_sinks.alarm(ev.code, now);  // Plan N3: a running Logo program ends (7042 / 7043)
   }
 }
````

Change `firmware/esp32/src/app/uno_event_router.h` (unified diff against the previous task):
````diff
@@ -22,6 +22,7 @@ class UnoEventRouter {
     void (*ack_result)(uint8_t seq, uint8_t status);
     void (*stop_result)(uint8_t status);
     void (*setting_value)(uint8_t id, int32_t value);
+    void (*alarm)(uint8_t uno_event, uint32_t now) = nullptr;  // Plan N3: battery critical, tilted, lifted
   };
 
   UnoEventRouter(Replies& out, core::EventLog& log, TelemetryPublisher& tel, const core::BodyLink& link)
````

Change `firmware/esp32/src/core/ws_codec.cpp` (unified diff against the previous task):
````diff
@@ -33,7 +33,8 @@ const KindName kKinds[] = {
     {"setting", WsCmdKind::kSetting},      {"restart", WsCmdKind::kRestart},
     {"tank", WsCmdKind::kTank},            {"log", WsCmdKind::kLog},
     {"camera_set", WsCmdKind::kCameraSet}, {"selftest", WsCmdKind::kSelfTest},
-    {"app_set", WsCmdKind::kAppSet},
+    {"app_set", WsCmdKind::kAppSet},       {"logo_run", WsCmdKind::kLogoRun},
+    {"logo_step", WsCmdKind::kLogoStep},   {"logo_stop", WsCmdKind::kLogoStop},
 };
 
 WsCmdKind kindOf(const char* t) {
@@ -105,6 +106,7 @@ bool checkCameraSet(WsCommand& c, const Seen& s) {
   return c.cam.any();
 }
 bool checkAppSet(WsCommand& c, const Seen& s) { return c.has_ai_url && !s.url_bad; }
+bool checkLogoRun(WsCommand&, const Seen& s) { return s.prog; }
 
 struct CheckRow {
   WsCmdKind kind;
@@ -120,7 +122,7 @@ const CheckRow kChecks[] = {
     {WsCmdKind::kApPassword, checkApPassword}, {WsCmdKind::kSetting, checkSetting},
     {WsCmdKind::kCalib, checkCalib},         {WsCmdKind::kDriveRate, checkDriveRate},
     {WsCmdKind::kDebug, checkOn},            {WsCmdKind::kCameraSet, checkCameraSet},
-    {WsCmdKind::kAppSet, checkAppSet},
+    {WsCmdKind::kAppSet, checkAppSet},       {WsCmdKind::kLogoRun, checkLogoRun},
 };
 
 bool complete(WsCommand& c, const codec::Seen& s) {
````

Change `firmware/esp32/src/core/ws_codec.h` (unified diff against the previous task):
````diff
@@ -39,6 +39,9 @@ enum class WsCmdKind : uint8_t {
   kCameraSet,   // {"t":"camera_set","size"?,"quality"?,"fps_cap"?,"brightness"?,"contrast"?,"low_light"?,"vflip"?,"hmirror"?,"reset"?}
   kSelfTest,    // {"t":"selftest"} -> CALIBRATE SELF_TEST on the Uno
   kAppSet,      // {"t":"app_set","ai_url":"http://..."|""}
+  kLogoRun,     // Plan N3 (Tank Logo LLD §20.4): {"t":"logo_run","prog":n,"step"?:bool} -> run a checked program
+  kLogoStep,    // {"t":"logo_step"} -> the next line in step mode
+  kLogoStop,    // {"t":"logo_stop"} -> end the running program (7040)
   kUnsupported,  // valid JSON, "t" not in the Basic Drive slice -> nack INVALID_COMMAND
   kInvalid,      // not JSON / no "t" / bad field -> nack INVALID_COMMAND
 };
@@ -90,6 +93,8 @@ struct WsCommand {
   CamFields cam;              // camera_set
   char ai_url[97] = "";       // app_set ("" clears it)
   bool has_ai_url = false;
+  uint32_t prog = 0;          // logo_run: the number the check returned
+  bool step = false;          // logo_run: step mode
   char t0[24] = "";  // ping: number echoed verbatim in pong
   char client[24] = "";
 };
````

Change `firmware/esp32/src/core/ws_codec_internal.h` (unified diff against the previous task):
````diff
@@ -28,6 +28,7 @@ struct Seen {
   char reset[16] = "";                                      // calib reset
   bool reset_true = false;                                  // camera_set reset (Plan O)
   bool url_bad = false;                                     // app_set
+  bool prog = false;                                        // logo_run (Plan N3)
 };
 
 // One "key": value pair; false = malformed (the whole command is rejected). Unknown keys are skipped.
````

Change `firmware/esp32/src/core/ws_codec_keys.cpp` (unified diff against the previous task):
````diff
@@ -215,6 +215,18 @@ bool readCpm(Reader& r, WsCommand& c, Seen&) {  // calib set
   return true;
 }
 
+bool readProg(Reader& r, WsCommand& c, Seen& s) {  // logo_run (Plan N3): 1..2^32-1
+  double n = 0;
+  if (!wholeIn(r, 1, 4294967295.0, n)) return false;
+  c.prog = static_cast<uint32_t>(n);
+  s.prog = true;
+  return true;
+}
+bool readStep(Reader& r, WsCommand& c, Seen&) {
+  bool seen = false;
+  return readBool(r, c.step, seen);
+}
+
 struct KeyRow {
   const char* key;
   KeyReader read;
@@ -230,7 +242,8 @@ const KeyRow kKeys[] = {
     {"value", readValue},   {"reset", readReset},     {"since", readSince},     {"size", readSize},
     {"quality", readQuality}, {"fps_cap", readFpsCap}, {"brightness", readBrightness},
     {"contrast", readContrast}, {"low_light", readLowLight}, {"vflip", readVflip}, {"hmirror", readHmirror},
-    {"ai_url", readAiUrl},  {"setting_id", readSettingId}, {"counts_per_m", readCpm},
+    {"ai_url", readAiUrl},  {"setting_id", readSettingId}, {"counts_per_m", readCpm}, {"prog", readProg},
+    {"step", readStep},
 };
 
 }  // namespace
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native -f test_ws_codec -f test_command_router -f test_uno_event_router -f test_drive_controller`
Expected: `65 test cases: 65 succeeded`.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src firmware/esp32/test
git commit -m "feat(bridge): logo_run / logo_step / logo_stop commands, routes, Uno alarms

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 10: On the tank: partitions, the worker, HTTP, saved programs, wiring

**Files:** `partitions_cq8m.csv`, `src/net/logo_worker.h/.cpp`, `logo_http.h/.cpp`, `program_store.h/.cpp` (new), `src/net/http_server.cpp`, `link_task.h/.cpp`, `src/main.cpp`, `scripts/upload_source.py`, `scripts/make_release.sh`, `web/help.html`.

**Interfaces:**
- Consumes: everything above.
- Produces: `logoWorkerBegin()`, `logoJobTake / logoJobPost / logoJobGiveUp`, `logoCheck`, `logoTake`, `logoRunningProgram()`, `logoVm()`; `logoHttpRegister()` (POST /logo/check, GET /logo/programs, GET/PUT/DELETE /logo/programs/<name>); `programStoreBegin / programList / programLoad / programSave / programRemove`; `LinkStatus::logo_running`; the `logo` LittleFS partition at 0x7B0000.

- [ ] **Step 1: No host test**

This task is the ESP32-only wiring (FreeRTOS, esp_http_server, LittleFS): no host test can run it. Its pure parts were tested in Tasks 1–9; the bridge build here and the stand checks (Task 12) test the rest.

- [ ] **Step 2: Write the code**

Change `firmware/esp32/partitions_cq8m.csv` (unified diff against the previous task):
````diff
@@ -1,8 +1,9 @@
-# ESP32-S3 8 MB layout (bridge LLD R2.2). Changing it needs a USB flash.
+# ESP32-S3 8 MB layout (bridge LLD R2.2; Tank Logo LLD §12 / §20.7: + the 256 KB "logo" area). Changing it needs a USB flash.
 # Name,   Type, SubType,  Offset,   Size,     Flags
 nvs,      data, nvs,      0x9000,   0x5000,
 otadata,  data, ota,      0xE000,   0x2000,
-app0,     app,  ota_0,    0x10000,  0x300000,
-app1,     app,  ota_1,    0x310000, 0x300000,
-source,   data, spiffs,   0x610000, 0x1E0000,
+app0,     app,  ota_0,    0x10000,  0x2E0000,
+app1,     app,  ota_1,    0x2F0000, 0x2E0000,
+source,   data, spiffs,   0x5D0000, 0x1E0000,
+logo,     data, littlefs, 0x7B0000, 0x40000,
 coredump, data, coredump, 0x7F0000, 0x10000,
````

Change `firmware/esp32/scripts/make_release.sh` (unified diff against the previous task):
````diff
@@ -20,7 +20,7 @@ PORT="${1:?usage: ./flash.sh /dev/ttyACM0}"
 cd "$(dirname "$0")"
 esptool --chip esp32s3 --port "$PORT" flash_id | grep -Eq "Chip (is|type:) *ESP32-S3" || { echo "not an ESP32-S3 - refusing"; exit 1; }
 esptool --chip esp32s3 --port "$PORT" --baud 921600 write_flash --flash_mode dio --flash_size 8MB \
-  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin 0x610000 source.bin
+  0x0 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin 0x5D0000 source.bin
 FLASH
 chmod +x "$OUT/flash.sh"
 gh release create "$TAG" "$OUT"/* --title "$TAG" \
````

Change `firmware/esp32/scripts/upload_source.py` (unified diff against the previous task):
````diff
@@ -16,7 +16,17 @@ sys.path.insert(0, str(PROJECT / "scripts"))
 import check_chip  # noqa: E402
 
 SOURCE_BIN = PROJECT / "build" / "source.bin"
-SOURCE_OFFSET = "0x610000"
+
+
+def _source_offset() -> str:  # from the partition table, so a new layout can never write the wrong place (Plan N3)
+    for line in (PROJECT / "partitions_cq8m.csv").read_text().splitlines():
+        cols = [c.strip() for c in line.split(",")]
+        if cols and cols[0] == "source":
+            return cols[3]
+    raise RuntimeError("partitions_cq8m.csv has no 'source' partition")
+
+
+SOURCE_OFFSET = _source_offset()
 
 
 def _upload_usb(target, source, env):  # type: ignore[no-untyped-def]
````

Change `firmware/esp32/src/main.cpp` (unified diff against the previous task):
````diff
@@ -6,6 +6,9 @@
 #include <esp_task_wdt.h>
 
 #include "board_config.h"
+#include "net/logo_http.h"
+#include "net/logo_worker.h"
+#include "net/program_store.h"
 #include "net/boot_diag.h"
 #include "net/camera_service.h"
 #include "net/http_server.h"
@@ -110,6 +113,9 @@ void setup() {
   cq::net::sourceBegin();
   cq::net::sourceRedirectRegister();  // port 80 -> 81 (R3.11)
   cq::net::infoRegister(g_wifi);
+  cq::net::logoWorkerBegin();  // Plan N3: Tank Logo (PSRAM), before the link task builds its LogoController
+  cq::net::programStoreBegin();
+  cq::net::logoHttpRegister();
   watchdogBegin();
   cq::net::linkTaskStart(cq::net::cameraSensorName);
   // The camera starts in loop() once WiFi is up (design-notes §38: no simultaneous current peaks).
````

Change `firmware/esp32/src/net/http_server.cpp` (unified diff against the previous task):
````diff
@@ -169,7 +169,8 @@ bool httpServerStart() {
   cfg.server_port = board::kHttpPort;
   cfg.ctrl_port = 32768;
   cfg.max_open_sockets = 6;  // 4 WS + 2 page loads; lwIP budget: Task 1 Step 6
-  cfg.max_uri_handlers = 12;
+  cfg.max_uri_handlers = 20;  // Plan N3: + 5 Tank Logo handlers
+  cfg.uri_match_fn = httpd_uri_match_wildcard;  // /logo/programs/<name>
   cfg.lru_purge_enable = true;
   cfg.stack_size = 6144;
   cfg.core_id = 0;
````

Change `firmware/esp32/src/net/link_task.cpp` (unified diff against the previous task):
````diff
@@ -5,8 +5,11 @@
 #include <esp_system.h>
 #include <esp_task_wdt.h>
 
+#include <new>
+
 #include "app/command_router.h"
 #include "app/drive_controller.h"
+#include "app/logo_controller.h"
 #include "app/motion_controller.h"
 #include "app/replies.h"
 #include "app/settings_controller.h"
@@ -23,6 +26,7 @@
 #include "core/ws_codec.h"
 #include "core/ws_json.h"
 #include "net/log.h"
+#include "net/logo_worker.h"
 #include "net/uart_port.h"
 #include "version.h"
 
@@ -51,7 +55,11 @@ app::SettingsController g_settings(g_out, g_log, g_link);
 app::TankAppController g_tank(g_out, g_log, g_link);
 app::DriveController g_drive(g_out, g_log, g_owner, g_uno, g_motion, g_settings, g_tel);
 app::UnoEventRouter g_events(g_out, g_log, g_tel, g_link);
-app::CommandRouter g_router(g_drive, g_motion, g_settings, g_tank, g_tel, g_out);
+// Plan N3: built in linkTaskStart - the running program and its Vm live in the logo worker's memory (PSRAM).
+alignas(app::LogoController) unsigned char g_logo_mem[sizeof(app::LogoController)];
+alignas(app::CommandRouter) unsigned char g_router_mem[sizeof(app::CommandRouter)];
+app::LogoController* g_logo = nullptr;
+app::CommandRouter* g_router = nullptr;
 
 void drainTx() {
   const uint8_t* p = nullptr;
@@ -67,7 +75,7 @@ void handleCommand(const LinkCommand& cmd, uint32_t now) {
   switch (cmd.kind) {
     case LinkCommand::Kind::kWsOpen: g_drive.onClientOpen(cmd.fd, now); break;
     case LinkCommand::Kind::kWsClose: g_drive.onClientClose(cmd.fd, now); break;
-    case LinkCommand::Kind::kWsText: g_router.onText(cmd.fd, cmd.text, cmd.len, cmd.post_seq, now); break;
+    case LinkCommand::Kind::kWsText: g_router->onText(cmd.fd, cmd.text, cmd.len, cmd.post_seq, now); break;
     case LinkCommand::Kind::kOtaStart:
     default: g_drive.startOta(now); break;
   }
@@ -96,6 +104,7 @@ void publishStatus() {
   s.frames_rx = g_link.stats().frames_rx;
   s.bad_crc = g_link.parserStats().bad_crc;
   s.tx_dropped = g_tx.dropped();
+  s.logo_running = g_logo->running();
   portENTER_CRITICAL(&g_status_mux);
   g_status = s;
   portEXIT_CRITICAL(&g_status_mux);
@@ -112,6 +121,7 @@ void step(uint32_t now) {
   core::LinkEvent ev{};
   while (g_link.popEvent(ev)) g_events.route(ev, now);
   telemetry(now);
+  g_logo->tick(now);     // Plan N3: a running Tank Logo program's slice
   g_settings.tick(now);  // confirmations, WiFi results, restart
   g_uno.tick(now);       // the next acked Uno request
   drainTx();
@@ -137,7 +147,13 @@ void linkTask(void*) {
 // ---- wiring: the hooks between the controllers (captureless functions; no virtual, standard §1.1) ----
 void wireViews() {
   g_tel.setViews({[] { return g_drive.ownerView(); }, [] { return g_motion.view(); }, [] { return !g_link.synced(); }});
-  g_motion.setHooks({[](int fd, uint32_t now) { return g_drive.claim(fd, now); }, [] { return g_drive.otaActive(); }});
+  app::MotionController::Hooks m;
+  m.claim = [](int fd, uint32_t now) { return g_drive.claim(fd, now); };
+  m.ota = [] { return g_drive.otaActive(); };
+  m.logo_fd = [] { return g_logo->fd(); };  // Plan N3: a running program is its page's primitive
+  m.logo_end = [](const core::PrimitiveEnd& e, uint32_t now) { g_logo->onPrimitiveEnd(e, now); };
+  m.logo_event = [](core::MotionEvent e, uint32_t now) { g_logo->onMotionEvent(e, now); };
+  g_motion.setHooks(m);
 }
 
 void wireEvents() {
@@ -157,6 +173,10 @@ void wireEvents() {
   sinks.ack_result = [](uint8_t seq, uint8_t status) { g_uno.onAck(seq, status); };
   sinks.stop_result = [](uint8_t status) { g_uno.onStopResult(status); };
   sinks.setting_value = [](uint8_t id, int32_t v) { g_uno.onSettingValue(id, v); };
+  sinks.alarm = [](uint8_t e, uint32_t now) {  // Plan N3: ends a running Tank Logo program
+    g_logo->onUnoAlarm(e == static_cast<uint8_t>(proto::EventKind::kBattCritical) ? logo::RunEvent::kBatteryLow
+                                                                                  : logo::RunEvent::kTilt, now);
+  };
   g_events.setSinks(sinks);
 }
 
@@ -181,13 +201,37 @@ void wireSettingsAndTank() {
   g_tank.setSelfTestHooks({[] { return !g_link.synced(); }, [] { return !standingStill(); }, queueSelfTest});
 }
 
+// Plan N3 (Tank Logo LLD §20.5): the running program's hooks into the drive, the motion service and the Uno.
+void wireLogo() {
+  app::LogoController::Hooks h;
+  h.claim = [](int fd, uint32_t now) { return g_drive.claim(fd, now); };
+  h.keep = [](int fd, uint32_t now) { return g_owner.keepDriving(fd, now); };
+  h.ota = [] { return g_drive.otaActive(); };
+  h.take = logoTake;
+  h.start_motion = [](int fd, const logo::Action& a, uint32_t now, core::BridgeReason& why) {
+    return g_motion.startLogo(fd, a, now, why);
+  };
+  h.stop_tank = [](bool owns, uint32_t now) {
+    if (g_motion.active()) g_motion.end(core::MotionEvent::kCancel, now);  // STOPs only while its client owns it
+    if (owns) g_link.sendStop(now);  // R4.6: never a STOP for a phone or the IR remote that took over
+  };
+  h.release = [](int fd, uint32_t now) { g_drive.onRelease(fd, -1, now); };
+  h.uno_request = [](const core::AckedOp& op) { return g_uno.request(op); };
+  h.cpm = [] { return g_motion.cpm(); };
+  g_logo->setHooks(h);
+  g_uno.setLogoDone([](uint8_t status) { g_logo->onUnoDone(status, millis()); });
+}
+
 }  // namespace
 
 void linkTaskStart(const char* (*camera_sensor)()) {
   g_sensor = camera_sensor;
   g_motion.begin();  // Plan K: counts per metre and roll-on, before the link task runs
   g_tank.begin();    // Plan O: the AI address
-  g_router.setHello(CQ_FW_VERSION, g_sensor);
+  g_logo = new (g_logo_mem) app::LogoController(g_out, g_log, g_link, g_tel, logoRunningProgram(), logoVm());
+  g_router = new (g_router_mem) app::CommandRouter(g_drive, g_motion, g_settings, g_tank, *g_logo, g_tel, g_out);
+  g_router->setHello(CQ_FW_VERSION, g_sensor);
+  wireLogo();
   wireViews();
   wireEvents();
   wireSettingsAndTank();
````

Change `firmware/esp32/src/net/link_task.h` (unified diff against the previous task):
````diff
@@ -33,6 +33,7 @@ struct LinkStatus {
   uint32_t frames_rx = 0;
   uint32_t bad_crc = 0;
   uint32_t tx_dropped = 0;  // chunks refused by a full FrameTx - review M12
+  bool logo_running = false;  // Plan N3: a Tank Logo program runs (no saving / deleting programs)
 };
 
 void linkTaskStart(const char* (*camera_sensor)());  // sensor name for welcome, read when needed
````

Create `firmware/esp32/src/net/logo_http.cpp`:
````cpp
#include "net/logo_http.h"

#include <esp_http_server.h>
#include <stdio.h>
#include <string.h>

#include "logo/check_json.h"
#include "logo/program_names.h"
#include "net/http_server.h"
#include "net/link_task.h"
#include "net/logo_worker.h"
#include "net/program_store.h"

namespace cq {
namespace net {
namespace {

esp_err_t sendJson(httpd_req_t* req, const char* status, const char* json) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, json);
}

esp_err_t busy(httpd_req_t* req) { return sendJson(req, "503 Service Unavailable", "{\"ok\":false,\"why\":\"BUSY\"}"); }

// The body into buf (cap includes the NUL); false when the client went away.
bool readBody(httpd_req_t* req, char* buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    const int n = httpd_req_recv(req, buf + got, len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  buf[len] = '\0';
  return true;
}

// "/logo/programs/<name>[?…]" → name (≤ 24 + NUL).
void nameOf(const char* uri, char* out, size_t cap) {
  const char* p = uri + strlen("/logo/programs/");
  size_t n = 0;
  while (p[n] != '\0' && p[n] != '?' && n + 1 < cap) {
    out[n] = p[n];
    ++n;
  }
  out[n] = '\0';
}

bool chunk(void* ctx, const char* data, size_t len) {
  return httpd_resp_send_chunk(static_cast<httpd_req_t*>(ctx), data, static_cast<ssize_t>(len)) == ESP_OK;
}

// ---- the jobs: on the worker task, the request is an async copy (the web server is free meanwhile) ----

void doCheck(httpd_req_t* req, const char* name) {
  const size_t len = req->content_len;
  if (len < logo::kSourceInbox && !readBody(req, logoInbox(), len)) return;  // the client went away
  bool runnable = false;
  const uint32_t prog = logoCheck(len < logo::kSourceInbox ? static_cast<uint16_t>(len) : logo::kSourceInbox, name,
                                  runnable);
  httpd_resp_set_type(req, "application/json");
  logo::JsonOut out(chunk, req);
  logo::writeCheckReply(logoSpace(), logoInbox(), runnable, prog, out);
  httpd_resp_send_chunk(req, nullptr, 0);
}

void doList(httpd_req_t* req, const char*) {
  static ProgramInfo list[logo::kMaxPrograms + 3];
  const uint8_t n = programList(list, sizeof list / sizeof list[0]);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send_chunk(req, "{\"programs\":[", HTTPD_RESP_USE_STRLEN);
  for (uint8_t i = 0; i < n; ++i) {
    char item[112];
    snprintf(item, sizeof item, "%s{\"name\":\"%.24s\",\"bytes\":%u,\"example\":%s}", i > 0 ? "," : "", list[i].name,
             list[i].bytes, list[i].example ? "true" : "false");
    httpd_resp_send_chunk(req, item, HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, nullptr, 0);
}

void doLoad(httpd_req_t* req, const char* name) {
  const int n = programLoad(name, logoInbox(), logo::kSourceInbox);
  if (n < 0) {
    sendJson(req, "404 Not Found", "{\"ok\":false,\"why\":\"NOT_FOUND\"}");
    return;
  }
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_send(req, logoInbox(), n);
}

void storeReply(httpd_req_t* req, StoreStatus s) {
  if (s == StoreStatus::kOk) {
    sendJson(req, "200 OK", "{\"ok\":true}");
    return;
  }
  char json[48];
  snprintf(json, sizeof json, "{\"ok\":false,\"why\":\"%s\"}", storeStatusName(s));
  sendJson(req, s == StoreStatus::kNotFound ? "404 Not Found" : "409 Conflict", json);
}

bool running(httpd_req_t* req) {  // no saving / deleting while a program runs (flash writes)
  if (!linkStatus().logo_running) return false;
  sendJson(req, "409 Conflict", "{\"ok\":false,\"why\":\"RUNNING\"}");
  return true;
}

void doSave(httpd_req_t* req, const char* name) {
  if (running(req)) return;
  if (req->content_len >= logo::kSourceInbox) return storeReply(req, StoreStatus::kTooBig);
  if (!readBody(req, logoInbox(), req->content_len)) return;
  storeReply(req, programSave(name, logoInbox(), req->content_len));
}

void doDelete(httpd_req_t* req, const char* name) {
  if (running(req)) return;
  storeReply(req, programRemove(name));
}

// ---- the handlers: on the web server task, they only hand the request over ----

esp_err_t accept(httpd_req_t* req, LogoJob job, const char* name) {
  if (!logoJobTake()) return busy(req);
  httpd_req_t* copy = nullptr;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    logoJobGiveUp();
    return busy(req);
  }
  if (!logoJobPost(job, copy, name)) {
    busy(copy);
    httpd_req_async_handler_complete(copy);
    logoJobGiveUp();
  }
  return ESP_OK;
}

esp_err_t onCheck(httpd_req_t* req) {
  char query[48] = "", name[25] = "";
  if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
    httpd_query_key_value(query, "name", name, sizeof name);
  }
  return accept(req, doCheck, name);
}

esp_err_t onList(httpd_req_t* req) { return accept(req, doList, ""); }

esp_err_t onProgram(httpd_req_t* req, LogoJob job) {
  char name[25];
  nameOf(req->uri, name, sizeof name);
  return accept(req, job, name);
}

esp_err_t onLoad(httpd_req_t* req) { return onProgram(req, doLoad); }
esp_err_t onSave(httpd_req_t* req) { return onProgram(req, doSave); }
esp_err_t onDelete(httpd_req_t* req) { return onProgram(req, doDelete); }

void add(const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
  httpd_uri_t u{};
  u.uri = uri;
  u.method = method;
  u.handler = handler;
  httpd_register_uri_handler(httpServer(), &u);
}

}  // namespace

void logoHttpRegister() {
  add("/logo/check", HTTP_POST, onCheck);
  add("/logo/programs", HTTP_GET, onList);
  add("/logo/programs/*", HTTP_GET, onLoad);
  add("/logo/programs/*", HTTP_PUT, onSave);
  add("/logo/programs/*", HTTP_DELETE, onDelete);
}

}  // namespace net
}  // namespace cq
````

Create `firmware/esp32/src/net/logo_http.h`:
````cpp
// Tank Logo over HTTP (Plan N3, Tank Logo LLD §20.4): POST /logo/check, GET / PUT / DELETE /logo/programs[/<name>].
// The web server task only hands each request to the logo worker (async), so /ws and STOP are never held up.
// Program text never goes through the link task.
#pragma once

namespace cq {
namespace net {

void logoHttpRegister();  // after httpServerStart(), logoWorkerBegin() and programStoreBegin()

}  // namespace net
}  // namespace cq
````

Create `firmware/esp32/src/net/logo_worker.cpp`:
````cpp
#include "net/logo_worker.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <new>

#include "cq_protocol.h"
#include "logo/check_box.h"
#include "net/link_task.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {

struct Shared {  // PSRAM only, allocated once at boot (≈ 330 KB): without it every job answers busy
  logo::CheckSpace space;
  logo::Program checked;
  char inbox[logo::kSourceInbox];
};
struct Run {  // the link task's: PSRAM, else internal RAM (≈ 36 KB) - the LogoController always exists
  logo::Program running;
  logo::Vm vm;
};
struct Job {
  LogoJob run;
  httpd_req_t* req;
  char name[25];
};

Shared* g = nullptr;
Run* g_run = nullptr;
logo::CheckBox* g_box = nullptr;
SemaphoreHandle_t g_busy = nullptr;    // one job at a time
SemaphoreHandle_t g_box_mx = nullptr;  // the CheckBox: worker (put) and link task (take, never waits)
QueueHandle_t g_jobs = nullptr;

template <class T>
T* make(uint32_t caps) {
  void* mem = heap_caps_malloc(sizeof(T), caps);
  return mem != nullptr ? new (mem) T() : nullptr;
}

logo::Caps caps() {
  logo::Caps c;
  const LinkStatus s = linkStatus();
  c.encoders = ((s.hello.caps >> static_cast<unsigned>(proto::CapBit::kEncoders)) & 1u) != 0;
  return c;
}

void workerTask(void*) {
  Job job;
  for (;;) {
    xQueueReceive(g_jobs, &job, portMAX_DELAY);
    job.run(job.req, job.name);
    httpd_req_async_handler_complete(job.req);
    xSemaphoreGive(g_busy);
  }
}

}  // namespace

bool logoWorkerBegin() {
  g_run = make<Run>(MALLOC_CAP_SPIRAM);
  if (g_run == nullptr) g_run = make<Run>(MALLOC_CAP_8BIT);
  g = make<Shared>(MALLOC_CAP_SPIRAM);
  if (g == nullptr) {
    logf("logo: no PSRAM for checks (%u bytes) - /logo answers busy", static_cast<unsigned>(sizeof(Shared)));
    return false;
  }
  static logo::CheckBox box(g->checked);
  g_box = &box;
  g_busy = xSemaphoreCreateBinary();
  xSemaphoreGive(g_busy);
  g_box_mx = xSemaphoreCreateMutex();
  g_jobs = xQueueCreate(1, sizeof(Job));
  xTaskCreatePinnedToCore(workerTask, "logo", 16384, nullptr, 2, nullptr, 0);
  logf("logo: worker ready (%u KB PSRAM)", static_cast<unsigned>(sizeof(Shared) / 1024));
  return true;
}

bool logoJobTake() { return g != nullptr && xSemaphoreTake(g_busy, 0) == pdTRUE; }
void logoJobGiveUp() { xSemaphoreGive(g_busy); }

bool logoJobPost(LogoJob job, httpd_req_t* async_req, const char* name) {
  Job j{job, async_req, ""};
  snprintf(j.name, sizeof j.name, "%s", name);
  return xQueueSend(g_jobs, &j, 0) == pdTRUE;
}

char* logoInbox() { return g->inbox; }
const logo::CheckSpace& logoSpace() { return g->space; }

uint32_t logoCheck(uint16_t len, const char* name, bool& runnable) {
  const uint32_t t0 = millis();
  runnable = logo::checkProgram(g->inbox, len, caps(), g->space);
  xSemaphoreTake(g_box_mx, portMAX_DELAY);
  const uint32_t prog = g_box->put(runnable ? &g->space.program : nullptr, name);
  xSemaphoreGive(g_box_mx);
  logf("logo: check %u: %u bytes in %lu ms%s", static_cast<unsigned>(prog), static_cast<unsigned>(len),
       static_cast<unsigned long>(millis() - t0), runnable ? "" : " (not runnable)");
  return prog;
}

app::LogoController::Took logoTake(uint32_t prog, logo::Program& out, char* name, size_t cap) {
  if (g_box == nullptr || xSemaphoreTake(g_box_mx, 0) != pdTRUE) return app::LogoController::Took::kBusy;
  const logo::CheckBox::Take t = g_box->take(prog, out, name, cap);
  xSemaphoreGive(g_box_mx);
  return t == logo::CheckBox::Take::kOk ? app::LogoController::Took::kOk : app::LogoController::Took::kCheckAgain;
}

bool logoRunReady() { return g_run != nullptr; }
logo::Program& logoRunningProgram() { return g_run->running; }
logo::Vm& logoVm() { return g_run->vm; }

}  // namespace net
}  // namespace cq
````

Create `firmware/esp32/src/net/logo_worker.h`:
````cpp
// The Tank Logo worker (Plan N3, Tank Logo LLD §20.1-§20.2): its own task on core 0 below the web server. The web
// server only accepts a /logo/* request and hands it over (esp_http_server's async requests), so the WebSocket -
// STOP included - is never held up by a check, a 40 KB reply or a flash write. One job at a time. The worker also
// owns the running program and the Vm that the link task's LogoController uses.
#pragma once

#include <esp_http_server.h>
#include <stddef.h>
#include <stdint.h>

#include "app/logo_controller.h"
#include "logo/check_job.h"

namespace cq {
namespace net {

using LogoJob = void (*)(httpd_req_t* req, const char* name);  // runs on the worker task

bool logoWorkerBegin();  // at boot, before linkTaskStart: memory, the task

// Web server task.
bool logoJobTake();  // false: a job is running (answer 503 busy)
// After httpd_req_async_handler_begin: the worker runs job(req, name), then completes the request and lets the
// next job in. False: not posted (the caller answers, completes and calls logoJobGiveUp).
bool logoJobPost(LogoJob job, httpd_req_t* async_req, const char* name);
void logoJobGiveUp();

// Worker task (inside a job).
char* logoInbox();  // ≤ 8192 bytes of program text
uint32_t logoCheck(uint16_t len, const char* name, bool& runnable);  // check + CheckBox; its number
const logo::CheckSpace& logoSpace();

// Link task: the checked program for logo_run, and the running program's storage.
app::LogoController::Took logoTake(uint32_t prog, logo::Program& out, char* name, size_t cap);
bool logoRunReady();  // false only when not even 36 KB were free at boot
logo::Program& logoRunningProgram();
logo::Vm& logoVm();

}  // namespace net
}  // namespace cq
````

Create `firmware/esp32/src/net/program_store.cpp`:
````cpp
#include "net/program_store.h"

#include <LittleFS.h>
#include <stdio.h>
#include <string.h>

#include "logo/generated/words.h"
#include "logo/program_names.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {

bool g_mounted = false;

void pathOf(const char* lower, char* out, size_t cap) { snprintf(out, cap, "/p/%s.logo", lower); }

uint8_t savedCount() {
  uint8_t n = 0;
  File dir = LittleFS.open("/p");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) ++n;
  return n;
}

// The name of a saved file without its folder and ".logo"; false for anything else.
bool fileName(const char* path, char* out, size_t cap) {
  const char* base = strrchr(path, '/');
  base = base != nullptr ? base + 1 : path;
  const size_t n = strlen(base);
  if (n <= 5 || strcmp(base + n - 5, ".logo") != 0 || n - 5 >= cap) return false;
  memcpy(out, base, n - 5);
  out[n - 5] = '\0';
  return true;
}

}  // namespace

const char* storeStatusName(StoreStatus s) {
  static constexpr const char* kNames[] = {"OK", "NAME", "FULL", "TOO_BIG", "EXAMPLE", "NOT_FOUND", "FAILED"};
  return kNames[static_cast<uint8_t>(s)];
}

bool programStoreBegin() {
  g_mounted = LittleFS.begin(true, "/logo", 4, "logo");
  if (g_mounted && !LittleFS.exists("/p")) LittleFS.mkdir("/p");
  logf("logo: program store %s", g_mounted ? "mounted" : "NOT mounted (partition 'logo' missing?)");
  return g_mounted;
}

uint8_t programList(ProgramInfo* out, uint8_t cap) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < logo::exampleCount() && n < cap; ++i, ++n) {
    snprintf(out[n].name, sizeof out[n].name, "%s", logo::example(i).name);
    out[n].bytes = static_cast<uint16_t>(strlen(logo::example(i).src));
    out[n].example = true;
  }
  if (!g_mounted) return n;
  File dir = LittleFS.open("/p");
  for (File f = dir.openNextFile(); f && n < cap; f = dir.openNextFile()) {
    if (!fileName(f.name(), out[n].name, sizeof out[n].name)) continue;
    out[n].bytes = static_cast<uint16_t>(f.size());
    out[n++].example = false;
  }
  return n;
}

int programLoad(const char* name, char* buf, size_t cap) {
  char lower[25], path[40];
  if (!logo::programName(name, lower, sizeof lower)) return -1;
  if (const logo::Example* e = logo::findExample(lower)) return snprintf(buf, cap, "%s", e->src);
  pathOf(lower, path, sizeof path);
  if (!g_mounted || !LittleFS.exists(path)) return -1;
  File f = LittleFS.open(path, "r");
  const size_t n = f.read(reinterpret_cast<uint8_t*>(buf), cap - 1);
  buf[n] = '\0';
  return static_cast<int>(n);
}

StoreStatus programSave(const char* name, const char* src, size_t len) {
  char lower[25], path[40];
  if (!logo::programName(name, lower, sizeof lower)) return StoreStatus::kName;
  if (logo::findExample(lower) != nullptr) return StoreStatus::kExample;
  if (len > logo::gen::kSourceBytes) return StoreStatus::kTooBig;
  pathOf(lower, path, sizeof path);
  if (!g_mounted) return StoreStatus::kFailed;
  if (!LittleFS.exists(path) && savedCount() >= logo::kMaxPrograms) return StoreStatus::kFull;
  File f = LittleFS.open(path, "w");
  if (!f) return StoreStatus::kFull;
  const size_t n = f.write(reinterpret_cast<const uint8_t*>(src), len);
  f.close();
  return n == len ? StoreStatus::kOk : StoreStatus::kFull;
}

StoreStatus programRemove(const char* name) {
  char lower[25], path[40];
  if (!logo::programName(name, lower, sizeof lower)) return StoreStatus::kName;
  if (logo::findExample(lower) != nullptr) return StoreStatus::kExample;
  pathOf(lower, path, sizeof path);
  if (!g_mounted || !LittleFS.exists(path)) return StoreStatus::kNotFound;
  return LittleFS.remove(path) ? StoreStatus::kOk : StoreStatus::kFailed;
}

}  // namespace net
}  // namespace cq
````

Create `firmware/esp32/src/net/program_store.h`:
````cpp
// Saved Tank Logo programs (Plan N3, Tank Logo LLD §12, §20.4): one file per program, /p/<name>.logo, on the
// LittleFS "logo" partition; the built-in examples are read-only. Web server task only.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace net {

enum class StoreStatus : uint8_t { kOk, kName, kFull, kTooBig, kExample, kNotFound, kFailed };
const char* storeStatusName(StoreStatus s);  // "NAME", "FULL" …

struct ProgramInfo {
  char name[25];
  uint16_t bytes;
  bool example;
};

bool programStoreBegin();  // mounts (formats on the first boot)
uint8_t programList(ProgramInfo* out, uint8_t cap);  // the examples first
int programLoad(const char* name, char* buf, size_t cap);  // its length, or -1 when there is none
StoreStatus programSave(const char* name, const char* src, size_t len);
StoreStatus programRemove(const char* name);

}  // namespace net
}  // namespace cq
````

Change `firmware/esp32/web/help.html` (unified diff against the previous task):
````diff
@@ -125,8 +125,8 @@ tank app:
 <tr><th>Camera</th><td><span id="camera">?</span>, ESP32-S3-EYE map: XCLK 15, SDA 4, SCL 5, D7..D0 = 16 17 18 12 10 8 9 11,
 VSYNC 6, HREF 7, PCLK 13, PWDN/RESET not connected</td></tr>
 <tr><th>USB</th><td>native USB (GPIO19/20), <code>303a:1001</code> &rarr; <code>/dev/ttyACM0</code> (Linux), <code>/dev/cu.usbmodem*</code> (macOS)</td></tr>
-<tr><th>Partitions</th><td>nvs 0x9000 &middot; otadata 0xE000 &middot; app0 0x10000 (3 MB) &middot; app1 0x310000 (3 MB) &middot;
-source 0x610000 (1.875 MB) &middot; coredump 0x7F0000</td></tr>
+<tr><th>Partitions</th><td>nvs 0x9000 &middot; otadata 0xE000 &middot; app0 0x10000 (2.875 MB) &middot; app1 0x2F0000 (2.875 MB) &middot;
+source 0x5D0000 (1.875 MB) &middot; logo 0x7B0000 (256 KB, saved Tank Logo programs) &middot; coredump 0x7F0000</td></tr>
 </table>
 
 <h2>[ REFLASH ]</h2>
@@ -134,7 +134,7 @@ source 0x610000 (1.875 MB) &middot; coredump 0x7F0000</td></tr>
 <pre>cd firmware/esp32
 python3 scripts/check_chip.py --port /dev/ttyACM0      # refuses anything but ESP32-S3 / 8 MB / 8 MB PSRAM
 pio run -e bridge -t upload --upload-port /dev/ttyACM0  # bootloader + partitions + app (chip check runs first)
-pio run -e bridge -t upload_source --upload-port /dev/ttyACM0   # source archive at 0x610000
+pio run -e bridge -t upload_source --upload-port /dev/ttyACM0   # source archive at 0x5D0000
 CQ_OTA_PASSWORD=... scripts/ota.sh                      # over WiFi: app + source (only while nobody drives)</pre>
 <p>Release binaries: GitHub releases <code>bridge-vX.Y.Z</code> with <code>flash.sh</code>. Hold BOOT while plugging in if no port appears.</p>
 <h3>Uno (body)</h3>
````

- [ ] **Step 3: Build**

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge | tail -3`
Expected: `[SUCCESS]`, Flash ≈ 43.5 % of **3014656** bytes (the new 2.875 MB app slot), no warnings from `src/net/logo_*`, `src/net/program_store.cpp` or `src/app/logo_*`. Then the whole native suite: `cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native > /tmp/n3.log 2>&1; tail -1 /tmp/n3.log` → `579 test cases: 579 succeeded`.

- [ ] **Step 4: Commit**

```bash
git add firmware/esp32
git commit -m "feat(bridge): Tank Logo on the tank - logo partition, worker (async /logo jobs), saved programs, wiring

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 11: The smoke tool

**Files:** `tools/smoke/logo.py`, `tools/smoke/tests/test_logo_tool.py`.

**Interfaces:**
- Consumes: `bridge_echo.WsClient`; the HTTP and WebSocket messages of LLD §20.4.
- Produces: `uv run python logo.py check|run|list|load|save|delete`; `check_lines(reply)`, `event_line(ev)`, `run_program(ws, prog, step, show, stepper, clock)` (pings once a second).

- [ ] **Step 1: Write the failing tests**


Create `tools/smoke/tests/test_logo_tool.py`:
````python
"""Plan N3: the text logo.py prints and its run loop (Tank Logo LLD §20.4)."""

from __future__ import annotations

from typing import Any

from logo import check_lines, event_line, run_program


def test_check_lines_ok_with_words_and_path() -> None:
    reply: dict[str, Any] = {
        "ok": True,
        "prog": 17,
        "diags": [],
        "words": [{"name": "SQUARE", "inputs": 1, "gives": False}],
        "path": [[0, 0, 0, 0, 0], [0, 500, 0, 1, 0], [0, 500, 90, 1, 1]],
        "cut_short": False,
        "sim_error": None,
    }
    lines = check_lines(reply)
    assert lines[0] == "your word SQUARE (1 inputs)"
    assert lines[1] == "path: 3 points, ends at x 0.0 cm, y 50.0 cm, heading 90° (1 uncertain)"
    assert lines[-1] == "OK - can run (program 17)"


def test_check_lines_errors() -> None:
    reply: dict[str, Any] = {
        "ok": False,
        "prog": 3,
        "diags": [
            {"code": 7001, "line": 2, "col": 1, "msg": "I don't know how to FOO.", "warning": False}
        ],
        "path": [],
    }
    assert check_lines(reply) == [
        "line 2, col 1: error 7001: I don't know how to FOO.",
        "NOT runnable (program 3)",
    ]


def test_event_lines() -> None:
    assert event_line({"kind": "logo_line", "line": 3, "round": 2}) == "line 3 (round 2)"
    finished = {"kind": "logo_ended", "reason": "finished", "line": 4, "msg": ""}
    assert event_line(finished) == "finished"
    blocked = {"kind": "logo_ended", "reason": 7024, "line": 2, "msg": "x"}
    assert event_line(blocked) == "ended 7024 on line 2: x"
    assert event_line({"kind": "primitive_done"}) is None


class FakeWs:
    def __init__(self, replies: list[dict[str, Any]]) -> None:
        self.sent: list[dict[str, Any]] = []
        self.replies = replies

    def send(self, obj: dict[str, Any]) -> None:
        self.sent.append(obj)

    def recv(self, timeout: float) -> dict[str, Any] | None:
        return self.replies.pop(0) if self.replies else None

    def close(self) -> None:
        pass


def test_run_until_ended_and_step() -> None:
    ws = FakeWs(
        [
            {"t": "ack", "id": 1},
            {"t": "ev", "kind": "logo_line", "line": 1, "round": 0},
            {"t": "ev", "kind": "logo_pose", "x_cm": 0, "y_cm": 0, "heading": 0},
            {"t": "ev", "kind": "logo_ended", "reason": "finished", "line": 1, "msg": ""},
        ]
    )
    shown: list[str] = []
    ended = run_program(ws, 17, True, shown.append, lambda: None, clock=lambda: 0.0)
    assert ended is not None and ended["reason"] == "finished"
    assert ws.sent[0] == {"t": "logo_run", "prog": 17, "step": True, "id": 1}
    assert ws.sent[1] == {"t": "logo_step", "id": 2}
    assert shown == ["line 1", "finished"]


def test_run_refused() -> None:
    ws = FakeWs([{"t": "nack", "id": 1, "reason": "CHECK_AGAIN", "message": "check it again"}])
    shown: list[str] = []
    assert run_program(ws, 9, False, shown.append) is None
    assert shown == ["refused: CHECK_AGAIN - check it again"]


def test_pings_once_a_second_while_events_keep_coming() -> None:
    events: list[dict[str, Any]] = [{"t": "ev", "kind": "logo_pose"} for _ in range(30)]
    events.append({"t": "ev", "kind": "logo_ended", "reason": "finished", "line": 1, "msg": ""})
    ws = FakeWs(events)
    t = [0.0]

    def clock() -> float:
        t[0] += 0.1  # an event every 0.1 s (logo_pose at 10 Hz)
        return t[0]

    run_program(ws, 5, False, lambda _: None, clock=clock)
    pings = [m for m in ws.sent if m.get("t") == "ping"]
    assert len(pings) >= 2  # the bridge reaps a client silent for 3 s
````

- [ ] **Step 2: Run them to see them fail**

Run: `cd tools/smoke && uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: FAIL — `ImportError while importing test module … test_logo_tool.py` (no logo.py).

- [ ] **Step 3: Implement**


Create `tools/smoke/logo.py`:
````python
"""Tank Logo from the laptop (Plan N3, Tank Logo LLD §20.4) until the /logo page (Plan N4) exists.

    uv run python logo.py check square.logo            # errors, your words, the planned path
    uv run python logo.py run square.logo [--step]     # check, run (takes control); Ctrl-C = STOP
    uv run python logo.py run --example avoid_walls
    uv run python logo.py list | load NAME | save NAME FILE | delete NAME

Runs move the tank: on the stand first (tracks off the ground). STOP from the phone or the IR
remote ends a run.
"""

from __future__ import annotations

import argparse
import http.client
import json
import sys
import time
import urllib.parse
from collections.abc import Callable
from typing import Any, Protocol

from bridge_echo import WsClient

Message = dict[str, Any]


class WsLike(Protocol):
    def send(self, obj: dict[str, Any]) -> None: ...
    def recv(self, timeout: float) -> dict[str, Any] | None: ...
    def close(self) -> None: ...


def http_call(host: str, method: str, path: str, body: str | None = None) -> tuple[int, str]:
    conn = http.client.HTTPConnection(host, 80, timeout=10)
    try:
        data = body.encode() if body is not None else None
        conn.request(method, path, body=data, headers={"Content-Type": "text/plain; charset=utf-8"})
        r = conn.getresponse()
        return r.status, r.read().decode("utf-8", "replace")
    finally:
        conn.close()


def check_lines(reply: Message) -> list[str]:
    """The check reply in plain lines: OK / every error with its place, your words, the path."""
    out: list[str] = []
    for d in reply.get("diags", []):
        kind = "hint" if d.get("warning") else f"error {d['code']}"
        out.append(f"line {d['line']}, col {d['col']}: {kind}: {d['msg']}")
    for w in reply.get("words", []):
        gives = ", gives a value" if w["gives"] else ""
        out.append(f"your word {w['name']} ({w['inputs']} inputs{gives})")
    path: list[list[int]] = reply.get("path", [])
    if path:
        x, y, h = path[-1][0], path[-1][1], path[-1][2]
        uncertain = sum(1 for p in path if p[4])
        more = f" ({uncertain} uncertain)" if uncertain else ""
        out.append(
            f"path: {len(path)} points, ends at x {x / 10:.1f} cm, y {y / 10:.1f} cm, "
            f"heading {h}°{more}"
        )
    if reply.get("cut_short"):
        out.append("path cut short (a limit was reached)")
    if reply.get("sim_error"):
        e = reply["sim_error"]
        out.append(f"while planning: error {e['code']} on line {e['line']}: {e['msg']}")
    verdict = "OK - can run" if reply.get("ok") else "NOT runnable"
    out.append(f"{verdict} (program {reply.get('prog')})")
    return out


def event_line(ev: Message) -> str | None:
    """One logo_* event as text; None for anything else."""
    kind = ev.get("kind", "")
    if kind == "logo_started":
        return f"started {ev.get('name') or '(not saved)'} by {ev.get('by')}"
    if kind == "logo_line":
        return f"line {ev['line']}" + (f" (round {ev['round']})" if ev.get("round") else "")
    if kind == "logo_say":
        return f"SAY: {ev['text']}"
    if kind == "logo_note":
        return f"note on line {ev['line']}: {ev['msg']}"
    if kind == "logo_ended":
        reason = ev.get("reason")
        if reason == "finished":
            return "finished"
        return f"ended {reason} on line {ev['line']}: {ev['msg']}"
    return None


def run_program(
    ws: WsLike,
    prog: int,
    step: bool,
    show: Callable[[str], None],
    stepper: Callable[[], None] | None = None,
    clock: Callable[[], float] = time.monotonic,
) -> Message | None:
    """logo_run, then the events until logo_ended (None: refused, or nothing for 330 s).

    Pings once a second whatever arrives: the bridge closes a client that sends nothing for 3 s
    (which would end the run with 7046)."""
    ws.send({"t": "logo_run", "prog": prog, "step": step, "id": 1})
    last_ping = last_heard = clock()
    while True:
        if clock() - last_ping >= 1.0:
            ws.send({"t": "ping", "t0": 0})
            last_ping = clock()
        m = ws.recv(1.0)
        if m is None:
            if clock() - last_heard > 330.0:
                return None
            continue
        last_heard = clock()
        if m.get("t") == "nack" and m.get("id") == 1:
            show(f"refused: {m.get('reason')} - {m.get('message')}")
            return None
        if m.get("t") != "ev":
            continue
        text = event_line(m)
        if text is not None:
            show(text)
        if m.get("kind") == "logo_ended":
            return m
        if step and m.get("kind") == "logo_line" and stepper is not None:
            stepper()
            ws.send({"t": "logo_step", "id": 2})


def _wait_enter() -> None:
    input("Enter = next line ")


def _check(host: str, text: str, name: str = "") -> Message:
    query = f"?name={urllib.parse.quote(name)}" if name else ""
    status, body = http_call(host, "POST", f"/logo/check{query}", text)
    if status != 200:
        raise SystemExit(f"check: HTTP {status} {body}")
    reply: Message = json.loads(body)
    return reply


def _source(args: argparse.Namespace) -> tuple[str, str]:
    if args.example:
        status, body = http_call(args.host, "GET", f"/logo/programs/{args.example}")
        if status != 200:
            raise SystemExit(f"load {args.example}: HTTP {status} {body}")
        return body, args.example
    if not args.file:
        raise SystemExit("give a program file or --example NAME")
    with open(args.file, encoding="utf-8") as f:
        return f.read(), ""


def _run(args: argparse.Namespace) -> int:
    text, name = _source(args)
    reply = _check(args.host, text, name)
    for line in check_lines(reply):
        print(line)
    if args.cmd == "check" or not reply.get("ok"):
        return 0 if reply.get("ok") else 1
    ws = WsClient(args.host)
    try:
        prog = int(reply["prog"])
        ended = run_program(ws, prog, args.step, print, _wait_enter)
    except KeyboardInterrupt:
        ws.send({"t": "stop"})
        ws.send({"t": "logo_stop", "id": 3})
        print("STOP sent")
        return 1
    finally:
        ws.close()
    return 0 if ended is not None and ended.get("reason") == "finished" else 1


def _store(args: argparse.Namespace) -> int:
    if args.cmd == "list":
        status, body = http_call(args.host, "GET", "/logo/programs")
        programs: list[dict[str, Any]] = (
            json.loads(body).get("programs", []) if status == 200 else []
        )
        for p in programs:
            print(f"{p['name']:24} {p['bytes']:5} B{'  (example)' if p['example'] else ''}")
        return 0 if status == 200 else 1
    if args.cmd == "load":
        status, body = http_call(args.host, "GET", f"/logo/programs/{args.name}")
    elif args.cmd == "save":
        with open(args.file, encoding="utf-8") as f:
            status, body = http_call(args.host, "PUT", f"/logo/programs/{args.name}", f.read())
    else:
        status, body = http_call(args.host, "DELETE", f"/logo/programs/{args.name}")
    print(body)
    return 0 if status == 200 else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Tank Logo from the laptop (Plan N3)")
    ap.add_argument("cmd", choices=["check", "run", "list", "load", "save", "delete"])
    ap.add_argument(
        "name", nargs="?", help="check / run: a file; load / save / delete: a saved name"
    )
    ap.add_argument("file", nargs="?", help="save: the program file")
    ap.add_argument("--example", default="", help="check / run a saved program or example by name")
    ap.add_argument("--step", action="store_true", help="run: stop at each line (Enter = next)")
    ap.add_argument("--host", default="conqueror-bridge.local")
    args = ap.parse_args(argv)
    if args.cmd in ("check", "run"):
        args.file = args.name
        return _run(args)
    if args.cmd != "list" and not args.name:
        ap.error("give a program name")
    return _store(args)


if __name__ == "__main__":
    sys.exit(main())
````

- [ ] **Step 4: Run them to see them pass**

Run: `cd tools/smoke && uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: `18 passed`; ruff and pyright clean.

- [ ] **Step 5: Commit**

```bash
git add tools/smoke
git commit -m "feat(smoke): logo.py - check, run, step, stop and saved programs from the laptop

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```


---

### Task 12: Stand checks (owner present, tracks off the ground)

**Files:** `docs/design-notes.md` (§104, the results). **Interfaces:** the HTTP and WebSocket messages of LLD §20.4 through `tools/smoke/logo.py`.

- [ ] **Step 1: Ask first.** Tell the owner: "Stand checks for Tank Logo: a USB flash of the ESP32 (new partitions), then programs that turn the tracks and the camera, beep and light the LED. Is the tank on the stand with the tracks off the ground, battery on, STOP within reach?" Wait for a yes.

- [ ] **Step 2: USB flash in this order** (the new layout moves the source area to 0x5D0000, which is inside the old app1: the partitions must go first). On the Linux machine with the bridge on USB (`docs/dev-setup.md`):
```bash
cd ~/NewConquerorTank && git fetch && git checkout plan-n3 && git pull
cd firmware/esp32
~/.platformio/penv/bin/pio run -e bridge -t upload --upload-port /dev/ttyACM0         # bootloader, partitions, app
~/.platformio/penv/bin/pio run -e bridge -t upload_source --upload-port /dev/ttyACM0  # source archive at 0x5D0000
```
Expected on the serial log after the reset (print WiFi lines only as their first 3 words): `logo: worker ready (… KB PSRAM)`, `logo: program store mounted`, the usual WiFi / link lines; the WiFi settings and counts per metre are kept (NVS did not move).

- [ ] **Step 3: Nothing moves yet.** From the laptop (`cd tools/smoke`):
```bash
uv run python logo.py list
printf 'REPEAT 4 [ FD 30 RT 90 ]\n' > /tmp/square.logo && uv run python logo.py check /tmp/square.logo
printf 'FD 600\nFOO\n' > /tmp/bad.logo && uv run python logo.py check /tmp/bad.logo
```
Expected: the three examples; `path: 9 points …`, `OK - can run`; the two errors (7001 line 2, 7025 line 1), `NOT runnable`.

- [ ] **Step 4: A heavy check stays well below the watchdog** (Review Focus 5). Make an ~8 KB program (`python3 -c "print('FOREVER [ FD 10 RT 1 ]\n' + '; padding padding padding\n' * 300)" > /tmp/heavy.logo`), check it 3 times, and read the bridge log line `logo: check N: … bytes in T ms`. Expected: T < 1000 ms each time, no restart. If T ≥ 1000 ms: stop and record it (the worker then needs to yield between simulator slices).

- [ ] **Step 5: STOP is never held up by a check** (Review Focus 1). On the phone's drive page hold the stick at a slow speed (tracks turn), and in a loop on the laptop `for i in $(seq 20); do uv run python logo.py check /tmp/heavy.logo >/dev/null; done`; while it runs press STOP on the phone several times. Expected: the tracks stop at once every time (as without the checks).

- [ ] **Step 6: Runs on the stand.** Before each run say what will move. Expected for each:
  - `uv run python logo.py run /tmp/square.logo` → the tracks turn (forward, spin, four times), events `line 1 (round 1)…`, `finished`; the tank stops; the phone page shows the drive released.
  - `printf 'LED GREEN BEEP SOUND HORN PLAY [ C4 E4 G4 2 ] LOOK LEFT WAIT 1 LOOK HOME SAY [ all done ]\n' > /tmp/ui.logo && uv run python logo.py run /tmp/ui.logo` → light, beep, horn, the tune, camera left and back, `SAY: all done`, `finished`.
  - `printf 'FOREVER [ WAIT 1 ]\n' > /tmp/wait.logo`: run it, wait 10 s (it keeps the drive: no 7045), then Ctrl-C → `STOP sent`, the bridge log / Log tab shows the program ended 7040.
  - STOP from the phone during the square → `ended 7040`, tracks stop.
  - A second phone takes control (drives) during `wait.logo` → `ended 7045`; the second phone keeps driving (no STOP from the program).
  - Kill the smoke tool without STOP during `wait.logo` (`kill -9` its process) → within ~3 s the bridge closes the client and the program ends 7046 (Log tab).
  - `uv run python logo.py run --example look_around` → camera moves, `finished`.

- [ ] **Step 7: Saved programs.** `uv run python logo.py save my_square /tmp/square.logo` → `{"ok":true}`; `list` shows it; `load my_square` prints it; `run --example my_square` runs it; during `wait.logo` a `save` answers `RUNNING`; `delete my_square` → ok; `delete first_square` → `EXAMPLE`.

- [ ] **Step 8: Record and commit.** Append to `docs/design-notes.md` a `## 104. Plan N3 stand checks (date)` with each step's result (check times, the STOP check, every run's ending), anything that did not match, and what was changed. Commit:
```bash
git add docs/design-notes.md
git commit -m "docs: Tank Logo N3 stand checks (§104)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 13: Floor checks (after the road-wheel fix and the motor-power adjustment — owner's order)

**Files:** `docs/design-notes.md` §104. Only when the owner says the road wheel is fixed; ask "stand or floor?" — these are **floor** runs; clear 2 m × 2 m, STOP within reach.

- [ ] **Step 1:** `printf 'REPEAT 4 [ FD 50 RT 90 ]\n' > /tmp/sq50.logo && uv run python logo.py run /tmp/sq50.logo` — Expected: the tank ends within ~5 cm of its start mark and facing the start heading; the last `logo_pose` (bridge log or a second `logo.py` session) within 5 cm of (0, 0).
- [ ] **Step 2:** `run --example avoid_walls` with a box 60 cm ahead → it backs and turns at the box (`BLOCKED?`), keeps going; Ctrl-C ends it.
- [ ] **Step 3:** `printf 'FD 200\nBEEP\n'` with a box 60 cm ahead → `ended 7024 on line 1` (no `BLOCKED?`), the tank stops at the box.
- [ ] **Step 4:** Record in §104 (square error in cm, the two obstacle runs) and commit as in Task 12 Step 8.

---

### Task 14: Docs

**Files:** `docs/lld/tank-logo-lld.md` (§20.9), `docs/lld/esp32-bridge-lld.md` (R16 + the partition table), `docs/design-notes.md`, `CLAUDE.md`.

- [ ] **Step 1: Tank Logo LLD §20.9 "As built (Plan N3)".** Append after §20.8: the ten "Rules this plan decides" (as the owner confirmed them at plan review), the files of Tasks 1–11, and these corrections: §20.2 — the web server hands each `/logo/*` request to the worker (async) and never waits; §20.5 — the program keeps the drive with `keepDriving` each tick and sends the ending's STOP only while it still owns the drive; §20.6 — check workspace ≈ 330 KB PSRAM, worker stack 16 KB, running program + Vm ≈ 36 KB.
- [ ] **Step 2: Bridge LLD.** In the partition table (R2.2, lines with `app0` / `app1` / `source`) put the new layout: `app0 0x10000 0x2E0000 (2.875 MB)`, `app1 0x2F0000 0x2E0000`, `source 0x5D0000 0x1E0000`, a new row `logo | data / littlefs | 0x7B0000 | 0x40000 (256 KB) | saved Tank Logo programs (Plan N3)`, and in step 9 `writes source.bin at 0x5D0000 (read from the partition table)`. Add a short `R16 — Tank Logo on the bridge (Plan N3)` revision: the new controller and its hooks, the async `/logo/*` jobs, `LinkStatus::logo_running`, the codes 3016 / 3017, pointing to Tank Logo LLD §20.
- [ ] **Step 3: CLAUDE.md.** Plan list: after the N2 line add `**Plan N3 — Tank Logo on the tank** (worker, HTTP + WebSocket, saved programs, odometry; Tank Logo LLD §20) — stand-checked (the date of design-notes §104); floor checks after the road-wheel fix.` and make "Next" read `Tank Logo Plan N4 (the /logo page)`; add `2026-10-19-plan-n3-logo-on-tank.md` to the plans row; in the hardware facts add the new partition layout (one USB flash done).
- [ ] **Step 4: Whole check, commit, push.**
```bash
cd firmware/esp32 && ~/.platformio/penv/bin/pio test -e native > /tmp/n3.log 2>&1; tail -1 /tmp/n3.log
~/.platformio/penv/bin/pio run -e bridge | tail -3
cd ../.. && python3 scripts/size_check.py && cd tools/smoke && uv run pytest -q && cd ../..
git add docs CLAUDE.md
git commit -m "docs: Tank Logo LLD §20.9 and bridge LLD R16 (N3 as built), CLAUDE.md

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git push -u origin plan-n3
```
Expected: `579 test cases: 579 succeeded`, `[SUCCESS]`, size check clean, `18 passed`.

---

## Self-review notes (writing-plans)

- **Spec coverage (LLD §20):** 20.1 parts → Tasks 2–10 (`CheckBox` T3, `LogoController` T8, `Odometry` T4, `ProgramStore` + `LogoHttp` + `LogoWorker` T10, JSON T2/T5, additions T6/T7/T9, N2 follow-ups T1, smoke tool T11); 20.2 checking → T2 + T10; 20.3 running → T8 + T9 + T10; 20.4 messages → T2, T5, T9, T10; 20.5 rules → T6–T8 (pacing, who drives, endings, stopping, capabilities); 20.6 memory → T10 (as built: rule 8); 20.7 partitions → T10 + Task 12 flash; 20.8 tests → Tasks 1–12, floor in Task 13.
- **Verified before hand-over:** Tasks 1–11 were replayed in order on a clean branch from `main`: each task's tests fail as its Step 2 says and pass as its Step 4 says; the size check passes after each task; the bridge builds after Task 10 (flash 43.5 % of the 2.875 MB slot); the whole native suite is 579 / 579; the smoke tool's 18 tests, ruff and pyright are clean.
- **Types:** `LogoController::Hooks`, `Took`, `Action`, `RunOut`, `AckedOp` fields, `CheckSpace` are used with the same names across Tasks 2–10 (each task's Interfaces block).
- **Review Focus:** lines 2–4 have host tests; lines 1 and 5 depend on the ESP32's tasks and are measured on the stand (Task 12 steps 4–5).
