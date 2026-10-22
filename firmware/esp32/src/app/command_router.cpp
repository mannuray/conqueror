#include "app/command_router.h"

namespace cq {
namespace app {

namespace {
using K = core::WsCmdKind;
using C = CommandRouter;
using W = core::WsCommand;
}  // namespace

// VARIANT POINT (LLD R15.4): one row per command kind.
const CommandRouter::Route CommandRouter::kRoutes[] = {
    {K::kHello, [](C& r, int fd, int32_t, const W&, uint32_t) { r.m_tel.hello(fd, r.m_fw, r.m_sensor ? r.m_sensor() : "none"); }},
    {K::kDrive, [](C& r, int fd, int32_t, const W& c, uint32_t now) { r.m_drive.onDrive(fd, c, now); }},
    {K::kCamJoy, [](C& r, int fd, int32_t, const W& c, uint32_t) { r.m_drive.onCamJoy(fd, c); }},
    {K::kCamHome, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onUnoRequest(fd, id, c); }},
    {K::kCamSet, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onCamSet(fd, id, c); }},
    {K::kSound, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onUnoRequest(fd, id, c); }},
    {K::kAudioMode, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onUnoRequest(fd, id, c); }},
    {K::kCamLock, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onUnoRequest(fd, id, c); }},
    {K::kRelease, [](C& r, int fd, int32_t id, const W&, uint32_t now) { r.m_drive.onRelease(fd, id, now); }},
    {K::kDebug, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_drive.onDebug(fd, id, c); }},
    {K::kPing, [](C& r, int fd, int32_t, const W& c, uint32_t now) {
       r.m_out.to(fd, core::jsonPong(r.m_out.buf(), r.m_out.cap(), c.t0, now, r.clockUs()));
     }},
    {K::kTurn, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_motion.onTurn(fd, id, c, now); }},
    {K::kDriveRate, [](C& r, int fd, int32_t, const W& c, uint32_t now) { r.m_motion.onDriveRate(fd, c, now); }},
    {K::kCancel, [](C& r, int fd, int32_t id, const W&, uint32_t now) { r.m_motion.onCancel(fd, id, now); }},
    {K::kMove, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_motion.onMove(fd, id, c, now); }},
    {K::kCalib, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_settings.onCalib(fd, id, c, now); }},
    {K::kWifiStatus, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_settings.onCommand(fd, id, c, now); }},
    {K::kWifiScan, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_settings.onCommand(fd, id, c, now); }},
    {K::kWifiSave, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_settings.onCommand(fd, id, c, now); }},
    {K::kWifiForget, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_settings.onCommand(fd, id, c, now); }},
    {K::kWifiMode, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_settings.onCommand(fd, id, c, now); }},
    {K::kApPassword, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_settings.onCommand(fd, id, c, now); }},
    {K::kSetting, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_settings.onCommand(fd, id, c, now); }},
    {K::kRestart, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_settings.onCommand(fd, id, c, now); }},
    {K::kTank, [](C& r, int fd, int32_t, const W&, uint32_t now) { r.m_tank.onTank(fd, now); }},
    {K::kLog, [](C& r, int fd, int32_t, const W& c, uint32_t now) { r.m_tank.onLog(fd, c.log_since, now); }},
    {K::kCameraSet, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_tank.onCameraSet(fd, id, c, now); }},
    {K::kSelfTest, [](C& r, int fd, int32_t id, const W&, uint32_t) { r.m_tank.onSelfTest(fd, id); }},
    {K::kAppSet, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.m_tank.onAppSet(fd, id, c); }},
    {K::kLogoRun, [](C& r, int fd, int32_t id, const W& c, uint32_t now) { r.m_logo.onRun(fd, id, c.prog, c.step, now); }},
    {K::kLogoStep, [](C& r, int fd, int32_t id, const W&, uint32_t) { r.m_logo.onStep(fd, id); }},
    {K::kLogoStop, [](C& r, int fd, int32_t id, const W&, uint32_t now) { r.m_logo.onStop(fd, id, now); }},
};

CommandRouter::Handler CommandRouter::find(core::WsCmdKind kind) {
  for (const Route& route : kRoutes) {
    if (route.kind == kind) return route.run;
  }
  return nullptr;
}

bool CommandRouter::routes(core::WsCmdKind kind) { return find(kind) != nullptr; }

void CommandRouter::onText(int fd, const char* text, size_t len, uint32_t post_seq, uint32_t now) {
  const core::WsCommand c = core::parseWsCommand(text, len);
  const int32_t id = c.has_id ? c.id : -1;
  if (c.kind == core::WsCmdKind::kStop) {
    m_drive.onStop(fd, id, post_seq, now);  // fast path: may overtake its own kWsOpen - never lost
    return;
  }
  if (!m_out.clients().contains(fd) || !m_drive.admitted(fd, c.kind, id, post_seq, now)) return;
  const Handler run = find(c.kind);
  if (run == nullptr) {
    m_out.nack(fd, id, core::BridgeReason::kInvalidCommand);
    return;
  }
  run(*this, fd, id, c, now);
}

}  // namespace app
}  // namespace cq
