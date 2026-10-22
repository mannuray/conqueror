#include "logo/simulator.h"

#include <math.h>
#include <stdio.h>

#include "core/motion_config.h"
#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kArcStepDeg = 10.0;  // one path point per 10° of an arc

double rad(double deg) { return deg * kPi / 180.0; }

double wrap180(double deg) {
  while (deg > 180.0) deg -= 360.0;
  while (deg <= -180.0) deg += 360.0;
  return deg;
}

}  // namespace

SimResult Simulator::plan(const Program& p, PathBuffer& out, uint32_t seed) {
  m_p = &p;
  m_out = &out;
  out.n = 0;
  m_pose = Pose();
  m_ms = 0;
  m_speed = 40;
  m_line = 0;
  m_full = false;
  m_guess = false;
  SimResult r;
  if (!p.ok) {  // never plan a program that did not compile completely
    r.code = 7010;
    return r;
  }
  m_vm.load(p, seed);
  point(0);
  while (step(r)) {
  }
  r.sim_ms = m_ms;
  return r;
}

bool Simulator::step(SimResult& r) {
  if (m_vm.executed() >= kMaxInstructions || m_ms >= gen::kRunTimeS * 1000u || m_full) {
    r.cut_short = true;
    return false;
  }
  const Sensors s = sensors();
  switch (m_vm.run(5000, s)) {  // VARIANT POINT: Vm results
    case VmStatus::kYield: case VmStatus::kLine: return true;
    case VmStatus::kEffect: return effect(r);
    case VmStatus::kError:
      r.code = m_vm.error().code;
      r.line = m_vm.error().line;
      snprintf(r.msg, sizeof r.msg, "%s", m_vm.error().msg);
      return false;
    default: return false;  // HALT, STOP: the path ends here
  }
}

bool Simulator::effect(SimResult& r) {
  MapState ms;
  ms.speed_pct = m_speed;
  Action a;
  RunError err;
  if (!mapEffect(m_vm.effect(), *m_p, ms, a, err)) {
    r.code = err.code;
    r.line = err.line;
    snprintf(r.msg, sizeof r.msg, "%s", err.msg);
    return false;
  }
  m_line = a.line;
  apply(a);
  return true;
}

void Simulator::apply(const Action& a) {
  if (moves(a)) return;
  if (a.kind == ActKind::kSpeed) m_speed = a.speed_pct;
  if (a.kind == ActKind::kWait || a.kind == ActKind::kTone) m_ms += a.ms;
  if (a.kind == ActKind::kTick) m_ms += kTickMs;
}

// The actions that move the planned tank. False for the others.
bool Simulator::moves(const Action& a) {
  switch (a.kind) {  // VARIANT POINT: motion actions
    case ActKind::kMove: move(a.dist_cm); return true;
    case ActKind::kTurn: turn(-a.delta_cdeg / 100.0); return true;
    case ActKind::kHome: turn(-wrap180(m_pose.h)); return true;
    case ActKind::kArc: around(a.radius_cm * 10.0, -a.delta_cdeg / 100.0); return true;
    case ActKind::kPivot: around(core::motion::kTrackWidthCm * 5.0, -a.delta_cdeg / 100.0); return true;
    case ActKind::kGo:  // no length: 1 m, and everything after it is a guess (LLD §22.4)
      m_guess = true;
      move(kGoPreviewCm);
      return true;
    default: return false;
  }
}

void Simulator::move(int32_t cm) {
  m_pose.x += cm * 10.0 * sin(rad(m_pose.h));
  m_pose.y += cm * 10.0 * cos(rad(m_pose.h));
  m_ms += static_cast<uint32_t>(fabs(cm) / cmPerS() * 1000.0);
  point(m_line);
}

void Simulator::turn(double deg) {
  m_pose.h += deg;
  m_ms += static_cast<uint32_t>(fabs(deg) / kTurnDegPerS * 1000.0);
  point(m_line);
}

// Around a centre `radius_mm` to the right (deg > 0, clockwise) or the left (deg < 0), a point every 10°.
void Simulator::around(double radius_mm, double deg) {
  const double side = deg > 0 ? 1.0 : -1.0;
  const double h0 = rad(m_pose.h);
  const double cx = m_pose.x + side * radius_mm * cos(h0);
  const double cy = m_pose.y - side * radius_mm * sin(h0);
  double done = 0;
  while (fabs(done) < fabs(deg) && !m_full) {
    const double stepd = fabs(deg) - fabs(done) < kArcStepDeg ? deg - done : side * kArcStepDeg;
    const double t = rad(stepd);
    const double vx = m_pose.x - cx, vy = m_pose.y - cy;
    m_pose.x = cx + vx * cos(t) + vy * sin(t);
    m_pose.y = cy - vx * sin(t) + vy * cos(t);
    m_pose.h += stepd;
    done += stepd;
    point(m_line);
  }
  m_ms += static_cast<uint32_t>(rad(fabs(deg)) * radius_mm / 10.0 / cmPerS() * 1000.0);
}

void Simulator::point(uint16_t line) {
  if (m_out->n >= m_out->cap) {
    m_full = true;
    return;
  }
  PathPoint& p = m_out->pts[m_out->n++];
  p.x_mm = static_cast<int32_t>(lround(m_pose.x));
  p.y_mm = static_cast<int32_t>(lround(m_pose.y));
  p.heading_deg = static_cast<int16_t>(lround(wrap180(m_pose.h)));
  p.line = line;
  p.uncertain = m_vm.sensorRead() || m_guess;
}

Sensors Simulator::sensors() const {
  Sensors s;
  s.heading_milli = static_cast<int32_t>(lround(wrap180(m_pose.h) * 1000.0));
  s.timer_milli = static_cast<int32_t>(m_ms);
  return s;
}

// The track speed model of the motion service (motion_config.h): v = 15.5 + 0.18 · SPEED cm/s.
double Simulator::cmPerS() const {
  return core::motion::kTrackSpeedOffsetCmS + core::motion::kTrackSpeedSlopeCmS * m_speed;
}

}  // namespace logo
}  // namespace cq
