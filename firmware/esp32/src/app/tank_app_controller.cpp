#include "app/tank_app_controller.h"

#include <stdio.h>

#include "ports/app_store.h"
#include "ports/camera_control.h"

namespace cq {
namespace app {

void TankAppController::begin() { ports::loadAiUrl(m_ai_url, sizeof(m_ai_url)); }

void TankAppController::sendTank(int fd, uint32_t now, const core::CamSettings& cs) {
  core::TankView v;
  v.st = m_link.selfTest();
  v.st_age_s = v.st.seen ? (now - v.st.at_ms) / 1000u : 0;
  v.hw = m_link.snapshot().slow_valid ? m_link.snapshot().slow.hw : 0;
  v.cam_size = cs.size;
  v.cam_quality = cs.quality;
  v.cam_fps = cs.fps_cap;
  v.cam_bright = cs.brightness;
  v.cam_contrast = cs.contrast;
  v.cam_low = cs.low_light;
  v.cam_vflip = cs.vflip;
  v.cam_hmirror = cs.hmirror;
  v.viewers = ports::cameraViewers();
  const core::CamStats st = ports::cameraStats(now);
  v.fps_x10 = st.fps_x10;
  v.kbps = st.kbps;
  v.ai_url = m_ai_url;
  m_out.bigTo(fd, core::jsonTank(m_out.big(), m_out.bigCap(), v));
}

void TankAppController::onTank(int fd, uint32_t now) { sendTank(fd, now, ports::cameraSettings()); }

void TankAppController::onLog(int fd, uint32_t since, uint32_t now) {
  core::LogEntry e[25];
  bool gap = false;
  const uint8_t n = m_log.since(since, e, 25, gap);
  m_out.bigTo(fd, core::jsonLog(m_out.big(), m_out.bigCap(), e, n, m_log.last(), gap, now));
}

core::CamSettings TankAppController::merge(const core::CamFields& f) const {
  core::CamSettings s = f.reset ? ports::cameraDefaults() : ports::cameraSettings();
  if (f.has_size) s.size = f.size;
  if (f.has_quality) s.quality = f.quality;
  if (f.has_fps) s.fps_cap = f.fps;
  if (f.has_bright) s.brightness = f.bright;
  if (f.has_contrast) s.contrast = f.contrast;
  if (f.has_low) s.low_light = f.low;
  if (f.has_vflip) s.vflip = f.vflip;
  if (f.has_hmirror) s.hmirror = f.hmirror;
  return s;
}

void TankAppController::onCameraSet(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  const core::CamSettings s = merge(c.cam);
  if (!ports::cameraPost(s)) {
    m_out.nack(fd, id, core::BridgeReason::kUnavailable);  // no camera
    return;
  }
  m_out.ack(fd, id);
  sendTank(fd, now, s);  // the posted settings: the loop applies them a moment later
}

void TankAppController::onAppSet(int fd, int32_t id, const core::WsCommand& c) {
  if (!ports::saveAiUrl(c.ai_url)) {  // short flash write (rare, page action)
    m_out.nack(fd, id, core::BridgeReason::kBusy);
    return;
  }
  snprintf(m_ai_url, sizeof(m_ai_url), "%s", c.ai_url);
  m_out.ack(fd, id);
}

void TankAppController::onSelfTest(int fd, int32_t id) {
  if (m_hooks.body_offline()) {
    m_out.nack(fd, id, core::BridgeReason::kBodyOffline);
  } else if (m_hooks.moving() || !m_hooks.queue(fd, id)) {
    m_out.nack(fd, id, core::BridgeReason::kBusy);  // the tank must stand still / the queue is full
  }  // queued: answered when the Uno acks (EVENT SELFTEST carries the result)
}

}  // namespace app
}  // namespace cq
