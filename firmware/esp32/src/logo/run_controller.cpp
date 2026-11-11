#include "logo/run_controller.h"

#include <stdio.h>

#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

// How a primitive's end reason ends the program (0: it goes on; kBlocked depends on BLOCKED?, LLD §9).
constexpr uint16_t kByEndReason[] = {
    0,     // kDone
    7040,  // kCancelled
    7040,  // kStop
    7045,  // kNotOwner
    7044,  // kStalled
    7044,  // kTimeout (no progress)
    7034,  // kLink
    7034,  // kDeadman
    7044,  // kDiverged
    7024,  // kBlocked
    7021,  // kEncoderFault
};
static_assert(sizeof kByEndReason / sizeof kByEndReason[0] == static_cast<size_t>(core::EndReason::kEncoderFault) + 1,
              "one per EndReason");

constexpr uint16_t kByMotionEvent[] = {
    7040,  // kStop
    7045,  // kRelease
    7046,  // kOwnerClosed (a hidden page closes its socket)
    7034,  // kOta
    7034,  // kBodyOffline
    7045,  // kOwnerCommand
    7040,  // kCancel
    7034,  // kTxFull
    7044,  // kBodyStall
    7021,  // kBodyEncoderFault
};
static_assert(sizeof kByMotionEvent / sizeof kByMotionEvent[0] ==
                  static_cast<size_t>(core::MotionEvent::kBodyEncoderFault) + 1,
              "one per MotionEvent");

// How a skill's end on the laptop ends the program (kBlocked: as a move's block).
constexpr uint16_t kBySkillEnd[] = {
    0,     // kDone
    7050,  // kNotFound
    7051,  // kLost
    7054,  // kOutsideArea
    7055,  // kNoWay
    7024,  // kBlocked
    7053,  // kUnknownThing
    7052,  // kStale (the helper stopped answering)
    7056,  // kLearning (a teach started between the check and the word, Plan Z2b-1)
    7064,  // kRefused (the engine's skill_failed "stale": it couldn't do the word just then, design-notes §236)
    0,     // kCaught: the game caught the tank - the word ends, the program goes on (Tank Logo LLD §27.1)
};
static_assert(sizeof kBySkillEnd / sizeof kBySkillEnd[0] == static_cast<size_t>(SkillEnd::kCaught) + 1, "one per SkillEnd");

constexpr uint16_t kByRunEvent[] = {7040, 0, 7042, 7043};  // kLogoStop, kStep, kBatteryLow, kTilt

// The yaw change from `from` to `to`, -18000..18000 (0.01°, + = left).
int32_t yawDelta(uint16_t from, uint16_t to) {
  int32_t d = static_cast<int32_t>(to) - static_cast<int32_t>(from);
  if (d > 18000) d -= 36000;
  if (d < -18000) d += 36000;
  return d;
}

// The actions the program waits out by the clock (WAIT, the tune of PLAY, one tick of WAIT UNTIL).
bool waits(ActKind k) { return k == ActKind::kWait || k == ActKind::kTone || k == ActKind::kTick; }

}  // namespace

bool RunController::start(const Program& p, const RunConfig& cfg, const RunTelem& t, uint32_t now_ms) {
  if (!p.ok) return false;
  m_p = &p;
  m_cfg = cfg;
  for (Part& pt : m_parts) {
    Vm* vm = pt.vm;
    pt = Part();
    pt.vm = vm;
  }
  m_cur = 0;
  m_first = 1;  // the main part first, whatever the last run's ticks were (final review M3)
  m_launches = 0;
  m_parts[0].vm->load(p, cfg.seed);
  m_parts[0].vm->setStepping(cfg.step);
  m_parts[0].state = RunState::kRunning;
  m_start = now_ms;
  m_start_yaw = t.yaw_cdeg;
  m_speed = 40;
  m_blocked = false;
  m_going = false;
  m_area_m = 0;
  m_area_at = Pose();
  m_thing[0] = '\0';
  m_next_ask = now_ms;
  return true;
}

bool RunController::due(uint32_t now_ms) {
  Part& p = cur();
  if (p.state == RunState::kWaiting) {
    if (static_cast<int32_t>(now_ms - p.wake) < 0) return false;
    p.state = RunState::kRunning;
  } else if (p.state == RunState::kAsking) {
    if (static_cast<int32_t>(now_ms - p.ask_deadline) < 0) return false;
    answer(p.ask_q == AskQ::kSee ? 0 : -1);  // no answer in time: FALSE / -1
  }
  return true;
}

// The value the kResume after the question pushes: SEE? a boolean, HOWFAR a number of cm.
void RunController::answer(int32_t value) {
  constexpr int32_t kMaxCm = 1000000;  // far beyond any room; keeps value * 1000 in range
  Part& p = cur();
  value = value > kMaxCm ? kMaxCm : (value < -kMaxCm ? -kMaxCm : value);
  p.vm->resume(p.ask_q == AskQ::kSee ? Value{VTag::kBool, value != 0 ? 1 : 0} : Value{VTag::kNum, value * 1000});
  p.state = RunState::kRunning;
}

// The question goes out now: the host carries out the kAsk, the 300 ms start, the next question waits 100 ms.
void RunController::sendAsk(uint32_t now_ms, RunOut& out) {
  Part& p = cur();
  p.ask_held = false;
  out.act = true;
  out.action = p.ask_action;
  p.ask_deadline = now_ms + kAskMs;
  m_next_ask = now_ms + kAskGapMs;
}

RunOut RunController::onSkillEnd(SkillEnd e, uint32_t) {
  Part& m = m_parts[0];  // a skill is always the main part's
  if (m.state != RunState::kSkill) return RunOut();
  m.state = RunState::kRunning;
  if (e == SkillEnd::kDone || e == SkillEnd::kCaught || gameWordGoesOn(e)) {
    m_blocked = false;
    return RunOut();
  }
  if (e == SkillEnd::kBlocked && m_p->handles_obstacles) {
    m_blocked = true;  // BLOCKED? is true until the next move ends, as for a move
    return RunOut();
  }
  return end(kBySkillEnd[static_cast<uint8_t>(e)]);
}

// A game word that lost the person or found no way just ends: the program decides (look, wait - Tank Logo LLD §27.1).
bool RunController::gameWordGoesOn(SkillEnd e) const {
  return m_game_word && (e == SkillEnd::kLost || e == SkillEnd::kNoWay);
}

void RunController::step(const RunTelem& t, uint32_t now_ms, RunOut& out) {
  const Sensors s = sensors(t, now_ms);
  Vm& vm = *cur().vm;
  switch (vm.run(m_cfg.budget, s)) {  // VARIANT POINT: Vm results
    case VmStatus::kYield: return;
    case VmStatus::kLine:
      cur().state = RunState::kPaused;
      out.line = true;
      out.line_no = vm.line();
      out.round = vm.round();
      return;
    case VmStatus::kEffect: return effect(t, now_ms, out);
    case VmStatus::kHalt:  // the main program at its end: done; the launched part's block at its end: it stops
      if (m_cur == 0) {
        out = end(0);
      } else {  // the page's marker for this part goes (line 0, final review M5)
        cur().state = RunState::kIdle;
        out.line = true;
        out.line_no = 0;
      }
      return;
    case VmStatus::kStop: out = end(0); return;  // the STOP word: done, as at the end (Tank Logo LLD §25.6)
    case VmStatus::kError: out = endWith(vm.error().code, vm.error().line, vm.error().msg); return;
  }
}

void RunController::effect(const RunTelem& t, uint32_t now_ms, RunOut& out) {
  Part& p = cur();
  const Effect& e = p.vm->effect();
  p.last = e.kind;
  p.last_sub = e.sub;
  MapState ms;
  ms.speed_pct = m_speed;
  Action a;
  RunError err;
  if (!mapEffect(e, *m_p, ms, a, err)) {
    out = endWith(err.code, err.line, err.msg);
    return;
  }
  if (a.kind != ActKind::kTick && a.kind != ActKind::kAsk) {  // a question is asked again and again: no line event
    out.line = true;
    out.line_no = e.line;
    out.round = p.vm->round();
  }
  if (m_cur == 1 && mainOnly(a.kind)) {  // the checker refuses it first (7057); this is the guard (LLD §26.4)
    out = end(7057);
  } else if (a.kind == ActKind::kLaunch) {  // nothing for the host: the second part starts (7058: one runs)
    if (!launch(a)) out = end(7058);
  } else {
    carry(a, t, now_ms, out);
  }
}

// The new state for an action, and whether the host has to do it.
void RunController::carry(const Action& a, const RunTelem& t, uint32_t now_ms, RunOut& out) {
  out.action = a;
  out.act = a.kind != ActKind::kTick;
  if (carryAi(a, t, now_ms, out)) return;
  if (a.kind == ActKind::kGo) {  // drives on; the program goes on at once (LLD §22.2)
    if (!m_going) m_blocked = false;
    m_going = true;
    return;
  }
  if (a.kind == ActKind::kHold) {
    m_going = false;
    return;
  }
  if (waitsForMotion(a.kind)) {
    m_going = false;  // the motion takes over from a GO (no end comes for the GO)
    if (a.kind == ActKind::kHome) out.action.delta_cdeg = yawDelta(t.yaw_cdeg, m_start_yaw);
    cur().state = RunState::kMotion;
    return;
  }
  if (a.kind == ActKind::kSpeed) m_speed = a.speed_pct;
  if (waits(a.kind)) {
    cur().wake = now_ms + (a.kind == ActKind::kTick ? 1 : a.ms);
    cur().state = RunState::kWaiting;
  }
  // LED, BEEP, SOUND, LOOK, SAY: sent, the program goes on
}

// SEE? / HOWFAR wait for the laptop (300 ms at most); AREA is a setting; a skill is like a motion, but it runs on
// the laptop and carries the AREA that is on. Each of them takes over from a GO (a question does not).
bool RunController::carryAi(const Action& a, const RunTelem& t, uint32_t now_ms, RunOut& out) {
  if (a.kind == ActKind::kAsk) {
    Part& p = cur();
    p.ask_q = a.ask_q;
    p.state = RunState::kAsking;
    p.ask_action = a;
    if (static_cast<int32_t>(now_ms - m_next_ask) >= 0 && !otherAsking()) {
      sendAsk(now_ms, out);
    } else {
      p.ask_held = true;  // too soon after the last question, or the other part's is out: wait to ask
      out.act = false;
    }
    return true;
  }
  if (a.kind == ActKind::kArea) {
    m_area_m = a.area_m;  // 0 clears it
    m_area_at = t.pose;
    return true;
  }
  if (a.kind != ActKind::kSkill) return false;
  m_going = false;
  snprintf(m_thing, sizeof m_thing, "%s", a.thing);
  m_game_word = a.skill >= SkillKind::kFace;
  out.action.has_area = m_area_m != 0;
  out.action.area_m = m_area_m;
  out.action.area_at = m_area_at;
  cur().state = RunState::kSkill;
  return true;
}

Sensors RunController::sensors(const RunTelem& t, uint32_t now_ms) const {
  Sensors s;
  s.distance_milli = t.distance_mm == 0 ? 999000 : static_cast<int32_t>(t.distance_mm) * 100;
  s.obstacle = t.distance_mm != 0 && t.distance_mm < m_cfg.obstacle_mm;
  s.blocked = m_blocked;
  s.heading_milli = -yawDelta(m_start_yaw, t.yaw_cdeg) * 10;  // Logo: right = +
  s.tilted = t.tilted;
  s.battery_milli = static_cast<int32_t>(t.battery_pct) * 1000;
  s.timer_milli = static_cast<int32_t>(now_ms - m_start);
  s.caught = t.caught;
  return s;
}

RunOut RunController::onPrimitiveEnd(const core::PrimitiveEnd& e) {
  if (m_parts[0].state != RunState::kMotion) return goEnded(e);  // a motion is always the main part's
  m_parts[0].state = RunState::kRunning;
  if (e.reason == core::EndReason::kDone) {
    m_blocked = false;
    return RunOut();
  }
  if (e.reason == core::EndReason::kBlocked && m_p->handles_obstacles) {
    m_blocked = true;  // BLOCKED? is true until the next move ends (design-notes §82 h)
    return RunOut();
  }
  return end(kByEndReason[static_cast<uint8_t>(e.reason)]);
}

// A GO's end, arriving while the program runs on (LLD §22.2): HOLD's own end (done) is expected; the obstacle
// carries on with BLOCKED? or ends 7024, like a move; every other end ends the program as a move's would.
RunOut RunController::goEnded(const core::PrimitiveEnd& e) {
  if (!m_going || !active()) return RunOut();
  m_going = false;
  if (e.reason == core::EndReason::kDone) return RunOut();
  if (e.reason == core::EndReason::kBlocked && m_p->handles_obstacles) {
    m_blocked = true;
    return RunOut();
  }
  return end(kByEndReason[static_cast<uint8_t>(e.reason)]);
}

RunOut RunController::onMotionEvent(core::MotionEvent e) {
  if (!active()) return RunOut();
  return end(kByMotionEvent[static_cast<uint8_t>(e)]);
}

RunOut RunController::onEvent(RunEvent e) {
  if (!active()) return RunOut();
  if (e != RunEvent::kStep) return end(kByRunEvent[static_cast<uint8_t>(e)]);
  if (m_parts[0].state == RunState::kPaused) m_parts[0].state = RunState::kRunning;
  return RunOut();
}

// An ending with its message: {line} = the current part's line, {word} = its last effect's word (7021, 7057).
RunOut RunController::end(uint16_t code) {
  char line[8], msg[192] = "";
  const Part& p = cur();
  snprintf(line, sizeof line, "%u", static_cast<unsigned>(p.vm->line()));
  const DiagArg a[] = {{"line", line}, {"word", effectWord(p.last, p.last_sub)}, {"thing", m_thing}};
  if (code != 0) renderMessage(errorTemplate(code), a, 3, msg, sizeof msg);
  return endWith(code, p.vm->line(), msg);
}

RunOut RunController::endWith(uint16_t code, uint16_t line, const char* msg) {
  RunOut out;
  m_parts[0].state = m_parts[1].state = RunState::kEnded;  // an ending ends both parts (LLD §26.3 rule 4)
  out.ended = true;
  out.code = code;
  out.end_line = line;
  snprintf(out.msg, sizeof out.msg, "%s", msg);
  return out;
}

}  // namespace logo
}  // namespace cq
