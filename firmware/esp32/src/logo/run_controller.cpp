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
};
static_assert(sizeof kBySkillEnd / sizeof kBySkillEnd[0] == static_cast<size_t>(SkillEnd::kStale) + 1, "one per SkillEnd");

constexpr uint16_t kByRunEvent[] = {7040, 0, 7042, 7043};  // kLogoStop, kStep, kBatteryLow, kTilt

// The yaw change from `from` to `to`, -18000..18000 (0.01°, + = left).
int32_t yawDelta(uint16_t from, uint16_t to) {
  int32_t d = static_cast<int32_t>(to) - static_cast<int32_t>(from);
  if (d > 18000) d -= 36000;
  if (d < -18000) d += 36000;
  return d;
}

// The actions the motion service carries out (the program waits for their end).
bool isMotion(ActKind k) {
  return k == ActKind::kMove || k == ActKind::kTurn || k == ActKind::kArc || k == ActKind::kPivot ||
         k == ActKind::kHome;
}

// The actions the program waits out by the clock (WAIT, the tune of PLAY, one tick of WAIT UNTIL).
bool waits(ActKind k) { return k == ActKind::kWait || k == ActKind::kTone || k == ActKind::kTick; }

}  // namespace

bool RunController::start(const Program& p, const RunConfig& cfg, const RunTelem& t, uint32_t now_ms) {
  if (!p.ok) return false;
  m_p = &p;
  m_cfg = cfg;
  m_vm.load(p, cfg.seed);
  m_vm.setStepping(cfg.step);
  m_state = RunState::kRunning;
  m_start = now_ms;
  m_wake = 0;
  m_start_yaw = t.yaw_cdeg;
  m_speed = 40;
  m_blocked = false;
  m_going = false;
  m_area_m = 0;
  m_area_at = Pose();
  m_thing[0] = '\0';
  m_ask_deadline = 0;
  m_next_ask = now_ms;
  m_ask_held = false;
  return true;
}

RunOut RunController::tick(const RunTelem& t, uint32_t now_ms) {
  RunOut out;
  if (!active()) return out;
  if (now_ms - m_start >= static_cast<uint32_t>(gen::kRunTimeS) * 1000u) return end(7041);
  if (m_state == RunState::kMotion || m_state == RunState::kPaused || m_state == RunState::kSkill) return out;
  if (m_ask_held) {  // a question paced to ~10 Hz goes out now (its 300 ms start here)
    if (static_cast<int32_t>(now_ms - m_next_ask) < 0) return out;
    sendAsk(now_ms, out);
    return out;
  }
  if (!due(now_ms)) return out;
  step(t, now_ms, out);
  return out;
}

bool RunController::due(uint32_t now_ms) {
  if (m_state == RunState::kWaiting) {
    if (static_cast<int32_t>(now_ms - m_wake) < 0) return false;
    m_state = RunState::kRunning;
  } else if (m_state == RunState::kAsking) {
    if (static_cast<int32_t>(now_ms - m_ask_deadline) < 0) return false;
    answer(m_ask_q == AskQ::kSee ? 0 : -1);  // no answer in time: FALSE / -1
  }
  return true;
}

// The value the kResume after the question pushes: SEE? a boolean, HOWFAR a number of cm.
void RunController::answer(int32_t value) {
  constexpr int32_t kMaxCm = 1000000;  // far beyond any room; keeps value * 1000 in range
  value = value > kMaxCm ? kMaxCm : (value < -kMaxCm ? -kMaxCm : value);
  m_vm.resume(m_ask_q == AskQ::kSee ? Value{VTag::kBool, value != 0 ? 1 : 0} : Value{VTag::kNum, value * 1000});
  m_state = RunState::kRunning;
}

// The question goes out now: the host carries out the kAsk, the 300 ms start, the next question waits 100 ms.
void RunController::sendAsk(uint32_t now_ms, RunOut& out) {
  m_ask_held = false;
  out.act = true;
  out.action = m_ask_action;
  m_ask_deadline = now_ms + kAskMs;
  m_next_ask = now_ms + kAskGapMs;
}

RunOut RunController::onAskAnswer(int32_t value, uint32_t now_ms) {
  if (m_state != RunState::kAsking || m_ask_held || static_cast<int32_t>(now_ms - m_ask_deadline) >= 0) return RunOut();
  answer(value);
  return RunOut();
}

RunOut RunController::onSkillEnd(SkillEnd e, uint32_t) {
  if (m_state != RunState::kSkill) return RunOut();
  m_state = RunState::kRunning;
  if (e == SkillEnd::kDone) {
    m_blocked = false;
    return RunOut();
  }
  if (e == SkillEnd::kBlocked && m_p->handles_obstacles) {
    m_blocked = true;  // BLOCKED? is true until the next move ends, as for a move
    return RunOut();
  }
  return end(kBySkillEnd[static_cast<uint8_t>(e)]);
}

void RunController::step(const RunTelem& t, uint32_t now_ms, RunOut& out) {
  const Sensors s = sensors(t, now_ms);
  switch (m_vm.run(m_cfg.budget, s)) {  // VARIANT POINT: Vm results
    case VmStatus::kYield: return;
    case VmStatus::kLine:
      m_state = RunState::kPaused;
      out.line = true;
      out.line_no = m_vm.line();
      out.round = m_vm.round();
      return;
    case VmStatus::kEffect: return effect(t, now_ms, out);
    case VmStatus::kHalt: out = end(0); return;
    case VmStatus::kStop: out = end(7040); return;
    case VmStatus::kError: out = endWith(m_vm.error().code, m_vm.error().line, m_vm.error().msg); return;
  }
}

void RunController::effect(const RunTelem& t, uint32_t now_ms, RunOut& out) {
  const Effect& e = m_vm.effect();
  m_last = e.kind;
  m_last_sub = e.sub;
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
    out.round = m_vm.round();
  }
  carry(a, t, now_ms, out);
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
  if (isMotion(a.kind)) {
    m_going = false;  // the motion takes over from a GO (no end comes for the GO)
    if (a.kind == ActKind::kHome) out.action.delta_cdeg = yawDelta(t.yaw_cdeg, m_start_yaw);
    m_state = RunState::kMotion;
    return;
  }
  if (a.kind == ActKind::kSpeed) m_speed = a.speed_pct;
  if (waits(a.kind)) {
    m_wake = now_ms + (a.kind == ActKind::kTick ? 1 : a.ms);
    m_state = RunState::kWaiting;
  }
  // LED, BEEP, SOUND, LOOK, SAY: sent, the program goes on
}

// SEE? / HOWFAR wait for the laptop (300 ms at most); AREA is a setting; a skill is like a motion, but it runs on
// the laptop and carries the AREA that is on. Each of them takes over from a GO (a question does not).
bool RunController::carryAi(const Action& a, const RunTelem& t, uint32_t now_ms, RunOut& out) {
  if (a.kind == ActKind::kAsk) {
    m_ask_q = a.ask_q;
    m_state = RunState::kAsking;
    m_ask_action = a;
    if (static_cast<int32_t>(now_ms - m_next_ask) >= 0) {
      sendAsk(now_ms, out);
    } else {
      m_ask_held = true;  // too soon after the last question: the runner waits until it may ask
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
  out.action.has_area = m_area_m != 0;
  out.action.area_m = m_area_m;
  out.action.area_at = m_area_at;
  m_state = RunState::kSkill;
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
  return s;
}

RunOut RunController::onPrimitiveEnd(const core::PrimitiveEnd& e) {
  if (m_state != RunState::kMotion) return goEnded(e);
  m_state = RunState::kRunning;
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
  if (m_state == RunState::kPaused) m_state = RunState::kRunning;
  return RunOut();
}

// An ending with its message: {line} = the program's line, {word} = the last effect's word (7021).
RunOut RunController::end(uint16_t code) {
  char line[8], msg[192] = "";
  snprintf(line, sizeof line, "%u", static_cast<unsigned>(m_vm.line()));
  const DiagArg a[] = {{"line", line}, {"word", effectWord(m_last, m_last_sub)}, {"thing", m_thing}};
  if (code != 0) renderMessage(errorTemplate(code), a, 3, msg, sizeof msg);
  return endWith(code, m_vm.line(), msg);
}

RunOut RunController::endWith(uint16_t code, uint16_t line, const char* msg) {
  RunOut out;
  m_state = RunState::kEnded;
  out.ended = true;
  out.code = code;
  out.end_line = line;
  snprintf(out.msg, sizeof out.msg, "%s", msg);
  return out;
}

}  // namespace logo
}  // namespace cq
