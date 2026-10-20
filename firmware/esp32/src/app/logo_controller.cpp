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
  const logo::Pose p = m_odo.update(s.fast.enc_l, s.fast.enc_r, s.fast.yaw, s.fast.spd_cmd_l, s.fast.spd_cmd_r);
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
