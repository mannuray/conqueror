#include "app/command_router.h"

#include <string.h>

#include "ports/client_out.h"

namespace cq {
namespace app {

namespace {
using K = core::WsCmdKind;
using C = CommandRouter;
using W = core::WsCommand;
}  // namespace

// VARIANT POINT (LLD R15.4): one row per command kind.
const CommandRouter::Route CommandRouter::kRoutes[] = {
    {K::kHello, [](C& r, int fd, int32_t, const W& c, uint32_t) { r.onHello(fd, c); }},
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
    {K::kBadField, [](C& r, int fd, int32_t id, const W&, uint32_t) { r.m_out.nack(fd, id, core::BridgeReason::kBadField); }},
    {K::kLogoStep, [](C& r, int fd, int32_t id, const W&, uint32_t) { r.m_logo.onStep(fd, id); }},
    {K::kLogoStop, [](C& r, int fd, int32_t id, const W&, uint32_t now) { r.m_logo.onStop(fd, id, now); }},
    // Plan Z2a (bridge LLD R26.2): the skill server
    {K::kSkillThings, [](C& r, int fd, int32_t id, const W& c, uint32_t) { r.onThings(fd, id, c); }},
    {K::kSkillAnswer, [](C& r, int fd, int32_t, const W& c, uint32_t now) { r.m_logo.onSkillAnswer(fd, c.id, c.value, now); }},
    {K::kSkillDrive, [](C& r, int fd, int32_t, const W& c, uint32_t now) { r.m_motion.onSkillDrive(fd, c.id, c.v, c.rate_ddps, now); }},
    {K::kSkillDone, [](C& r, int fd, int32_t, const W& c, uint32_t now) { r.m_logo.onSkillEnd(fd, c.id, logo::SkillEnd::kDone, now); }},
    {K::kSkillFailed, [](C& r, int fd, int32_t, const W& c, uint32_t now) { r.m_logo.onSkillEnd(fd, c.id, c.skill_end, now); }},
    // Plan Z2b-1 (bridge LLD R27.1): around a teach; the hub ignores it from anyone but the server
    {K::kSkillLearning, [](C& r, int fd, int32_t, const W& c, uint32_t) { if (r.m_hub != nullptr) r.m_hub->onLearning(fd, c.on); }},
};

// hello: "ai-skills" registers the one skill server (another fd's gets BUSY, no welcome); everyone gets a welcome.
void CommandRouter::onHello(int fd, const core::WsCommand& c) {
  if (m_hub != nullptr && strcmp(c.client, "ai-skills") == 0) {
    char ip[16] = "";  // Plan Z2b-1 (R27.1): the engine's address, from its socket - never typed
    if (!ports::clientPeerIp(fd, ip, sizeof ip)) ip[0] = '\0';
    if (m_hub->onHello(fd, ip, c.page_port, c.page_proto) == core::SkillHub::Decision::kBusy) {
      return m_out.nack(fd, c.has_id ? c.id : -1, core::BridgeReason::kBusy);
    }
  }
  m_tel.hello(fd, m_fw, m_sensor ? m_sensor() : "none");
}

// skill_things: one part of the list, parsed from a copy of the message (the views point into it while it is staged:
// ThingNames::add copies the text). Bad -> BAD_FIELD, over a bound -> TOO_BIG, not the server -> NOT_OWNER.
void CommandRouter::onThings(int fd, int32_t id, const core::WsCommand& c) {
  if (m_hub == nullptr || m_len >= sizeof m_copy) return m_out.nack(fd, id, core::BridgeReason::kTooBig);
  memcpy(m_copy, m_text, m_len);
  m_copy[m_len] = '\0';
  const core::ThingParse p = core::parseThingList(m_copy, m_len, m_things);
  if (p != core::ThingParse::kOk) {
    return m_out.nack(fd, id, p == core::ThingParse::kTooBig ? core::BridgeReason::kTooBig : core::BridgeReason::kBadField);
  }
  switch (m_hub->onThingsPart(fd, c.part, c.of, m_things)) {
    case core::SkillHub::Decision::kOk: case core::SkillHub::Decision::kReady: return m_out.ack(fd, id);
    case core::SkillHub::Decision::kTooBig: return m_out.nack(fd, id, core::BridgeReason::kTooBig);
    case core::SkillHub::Decision::kNotServer: return m_out.nack(fd, id, core::BridgeReason::kNotOwner);
    default: return m_out.nack(fd, id, core::BridgeReason::kBadField);
  }
}

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
  m_text = text;
  m_len = len;
  run(*this, fd, id, c, now);
}

}  // namespace app
}  // namespace cq
