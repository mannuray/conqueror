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

void RunController::start(const Program& p, const RunConfig& cfg, const RunTelem& t, uint32_t now_ms) {
  m_p = &p;
  m_cfg = cfg;
  m_vm.load(p, cfg.seed);
  m_vm.setStepping(cfg.step);
  m_state = RunState::kRunning;
  m_start = now_ms;
  m_wake = m_until = 0;
  m_start_yaw = t.yaw_cdeg;
  m_speed = 40;
  m_blocked = false;
}

RunOut RunController::tick(const RunTelem& t, uint32_t now_ms) {
  RunOut out;
  if (!active()) return out;
  if (now_ms - m_start >= static_cast<uint32_t>(gen::kRunTimeS) * 1000u) return end(7041);
  if (m_state == RunState::kMotion || m_state == RunState::kPaused) return out;
  if (m_state == RunState::kWaiting) {
    if (static_cast<int32_t>(now_ms - m_wake) < 0) return out;
    m_state = RunState::kRunning;
  }
  step(t, now_ms, out);
  return out;
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
  MapState ms;
  ms.speed_pct = m_speed;
  Action a;
  RunError err;
  if (!mapEffect(e, *m_p, ms, a, err)) {
    out = endWith(err.code, err.line, err.msg);
    return;
  }
  if (a.kind != ActKind::kTick) {
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
  if (isMotion(a.kind)) {
    if (a.kind == ActKind::kHome) out.action.delta_cdeg = yawDelta(t.yaw_cdeg, m_start_yaw);
    m_state = RunState::kMotion;
    return;
  }
  if (a.kind == ActKind::kSpeed) m_speed = a.speed_pct;
  if (a.kind == ActKind::kUntilStart) m_until = now_ms + static_cast<uint32_t>(gen::kWaitS) * 1000u;
  if (waits(a.kind)) {
    m_wake = now_ms + (a.kind == ActKind::kTick ? 1 : a.ms);
    m_state = RunState::kWaiting;
  }
  // LED, BEEP, SOUND, LOOK, SAY: sent, the program goes on
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
  s.until_over = static_cast<int32_t>(now_ms - m_until) >= 0;
  return s;
}

RunOut RunController::onPrimitiveEnd(const core::PrimitiveEnd& e) {
  if (m_state != RunState::kMotion) return RunOut();
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
  const DiagArg a[] = {{"line", line}, {"word", effectWord(m_last)}};
  if (code != 0) renderMessage(errorTemplate(code), a, 2, msg, sizeof msg);
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
