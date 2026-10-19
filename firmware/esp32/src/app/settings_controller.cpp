#include "app/settings_controller.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "core/motion_config.h"
#include "core/wifi_policy.h"
#include "ports/log_out.h"
#include "ports/motion_store.h"
#include "ports/system_control.h"
#include "ports/wifi_control.h"

namespace cq {
namespace app {
namespace {

bool settingReadable(uint8_t id) { return id == 1 || id == 11 || id == 12 || id == 66; }
constexpr uint8_t kTrackBalanceId = 66;  // v1: the only setting a page may change (spec 02 §F5)

// The WiFi request a confirmed change becomes (wifi_save / wifi_forget / wifi_mode / ap_password).
bool wifiRequestOf(const core::PendingChange& ch, core::WifiRequest& r) {
  r.fd = ch.fd;
  r.id = ch.id;
  r.slot = ch.slot;
  r.ap = ch.ap;
  switch (ch.kind) {
    case core::ChangeKind::kWifiSave: r.kind = core::WifiRequest::kSave; break;
    case core::ChangeKind::kWifiForget: r.kind = core::WifiRequest::kForget; break;
    case core::ChangeKind::kWifiMode: r.kind = core::WifiRequest::kMode; break;
    case core::ChangeKind::kApPassword: r.kind = core::WifiRequest::kApPassword; break;
    default: return false;
  }
  snprintf(r.ssid, sizeof(r.ssid), "%s", ch.ssid);
  snprintf(r.pass, sizeof(r.pass), "%s", ch.pass);
  return true;
}

// The change a page command asks for; false: not a change (handled elsewhere).
bool changeOf(const core::WsCommand& c, core::PendingChange& ch) {
  switch (c.kind) {
    case core::WsCmdKind::kWifiSave:
      ch.kind = core::ChangeKind::kWifiSave;
      snprintf(ch.ssid, sizeof(ch.ssid), "%s", c.ssid);
      snprintf(ch.pass, sizeof(ch.pass), "%s", c.password);
      break;
    case core::WsCmdKind::kWifiForget: ch.kind = core::ChangeKind::kWifiForget; break;
    case core::WsCmdKind::kWifiMode: ch.kind = core::ChangeKind::kWifiMode; break;
    case core::WsCmdKind::kApPassword:
      ch.kind = core::ChangeKind::kApPassword;
      snprintf(ch.pass, sizeof(ch.pass), "%s", c.password);
      break;
    default: return false;
  }
  ch.slot = c.slot;
  ch.ap = c.wifi_ap;
  return true;
}

}  // namespace

uint32_t SettingsController::tankMs() const {
  return m_link.snapshot().fast_valid ? m_link.snapshot().fast.uno_ms : 0;
}

// A change from the page: held until the tank's button is pressed (or 30 s pass).
void SettingsController::askConfirm(const core::PendingChange& c, uint32_t now) {
  if (m_gate.busyFor(c.fd)) {  // Plan O final review I2: another device's change waits for the button
    m_out.nack(c.fd, c.id, core::BridgeReason::kBusy);
    return;
  }
  m_gate.begin(c, now);
  m_out.to(c.fd, core::jsonConfirm(m_out.buf(), m_out.cap(), core::changeName(c.kind),
                                   static_cast<uint8_t>(core::ConfirmGate::kWindowMs / 1000)));
}

void SettingsController::sendWifiStatus(int fd) {
  core::WifiSnapshot s;
  ports::wifiSnapshot(s);
  char host[40];
  snprintf(host, sizeof(host), "%s.local", board::kHostname);
  const core::WifiStatusView v{s.mode, s.ssid, s.rssi, s.ip, host, {s.saved[0], s.saved[1], s.saved[2]}, s.n_saved,
                               board::kFallbackSsid};
  m_out.to(fd, core::jsonWifiStatus(m_out.buf(), m_out.cap(), v));
}

bool SettingsController::wifiIsAp() {
  core::WifiSnapshot s;
  ports::wifiSnapshot(s);
  return strcmp(s.mode, "ap") == 0;
}

void SettingsController::pushFeedback(bool to_ap) {  // jingle + blue (home) / white (own AP) blink
  core::AckedOp snd;
  snd.kind = core::AckedOp::kSound;
  snd.a = static_cast<uint8_t>(proto::SoundId::kJingle);
  m_h.queue_op(snd);
  core::AckedOp led;
  led.kind = core::AckedOp::kLed;
  led.a = static_cast<uint8_t>(proto::LedMode::kBlink);
  led.r = to_ap ? 255 : 0;
  led.g = to_ap ? 255 : 0;
  led.b = 255;
  led.period_ms = 200;
  led.duration_ms = 3000;
  m_h.queue_op(led);
}

void SettingsController::onScan(int fd, int32_t id) {
  core::WifiRequest r;
  r.kind = core::WifiRequest::kScan;
  r.fd = fd;
  r.id = id;
  if (!ports::wifiPost(r)) m_out.nack(fd, id, core::BridgeReason::kBusy);
}

void SettingsController::onSetting(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  if (!settingReadable(c.setting_id)) {
    m_out.nack(fd, id, core::BridgeReason::kInvalidCommand);
    return;
  }
  if (!c.has_value) {  // read: SETTING_GET -> SETTING_VALUE
    core::AckedOp op;
    op.kind = core::AckedOp::kSettingGet;
    op.a = c.setting_id;
    op.fd = fd;
    op.id = id;
    if (!m_h.queue_op(op)) m_out.nack(fd, id, core::BridgeReason::kBusy);
    return;
  }
  if (c.setting_id != kTrackBalanceId || c.value < -100 || c.value > 100) {
    m_out.nack(fd, id, core::BridgeReason::kInvalidCommand);
    return;
  }
  core::PendingChange ch;
  ch.kind = core::ChangeKind::kSetting;
  ch.fd = fd;
  ch.id = id;
  ch.setting_id = c.setting_id;
  ch.value = c.value;
  askConfirm(ch, now);
}

void SettingsController::onRestart(int fd, int32_t id, uint32_t now) {
  if (m_h.ota() || m_h.motion_active()) {
    m_out.nack(fd, id, core::BridgeReason::kBusy);
    return;
  }
  m_out.ack(fd, id);
  m_restart_at = now + 500;
  ports::logLine("bridge: restart asked by %s", m_out.clients().name(fd));
  m_log.add(core::LogKind::kRestart, 0, 0, m_out.clients().name(fd), tankMs(), now);
}

void SettingsController::onCommand(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  core::PendingChange ch;
  ch.fd = fd;
  ch.id = id;
  if (c.kind == core::WsCmdKind::kWifiStatus) return sendWifiStatus(fd);
  if (c.kind == core::WsCmdKind::kWifiScan) return onScan(fd, id);
  if (c.kind == core::WsCmdKind::kSetting) return onSetting(fd, id, c, now);
  if (c.kind == core::WsCmdKind::kRestart) return onRestart(fd, id, now);
  if (changeOf(c, ch)) askConfirm(ch, now);
}

void SettingsController::onCalib(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  core::PendingChange ch;
  ch.fd = fd;
  ch.id = id;
  if (c.calib_reset >= 0) {  // Plan M: reset to the default - confirmed by the tank's button
    ch.kind = core::ChangeKind::kCalibReset;
    ch.calib_what = static_cast<uint8_t>(c.calib_reset);
    return askConfirm(ch, now);
  }
  if (!c.has_cpm) return m_h.send_calib(fd);
  if (m_h.driver_fd() != fd) return m_out.nack(fd, id, core::BridgeReason::kNotOwner);  // Plan O (§91)
  if (m_h.motion_active()) return m_out.nack(fd, id, core::BridgeReason::kBusy);
  ch.kind = core::ChangeKind::kCalibSet;
  ch.cpm = c.cpm;
  askConfirm(ch, now);
}

void SettingsController::applyWifi(core::PendingChange& ch) {
  core::WifiRequest r;
  wifiRequestOf(ch, r);
  if (ch.kind == core::ChangeKind::kWifiMode) pushFeedback(ch.ap);
  memset(ch.pass, 0, sizeof(ch.pass));
  if (!ports::wifiPost(r)) m_out.nack(ch.fd, ch.id, core::BridgeReason::kBusy);
  memset(r.pass, 0, sizeof(r.pass));
}

// Plan O: checked again now - no primitive may be running (the save itself is not motion).
void SettingsController::applyCalibSet(const core::PendingChange& ch) {
  if (m_h.motion_active() || !ports::saveCpm(ch.cpm)) {
    m_out.nack(ch.fd, ch.id, core::BridgeReason::kBusy);
    return;
  }
  *m_h.cpm = ch.cpm;
  m_out.to(ch.fd, core::jsonConfirmed(m_out.buf(), m_out.cap(), core::changeName(ch.kind)));
  m_h.send_calib(ch.fd);
}

void SettingsController::applyCalibReset(const core::PendingChange& ch) {
  if (ch.calib_what == 0) {
    *m_h.cpm = core::motion::kCpmDefault;
    if (!ports::saveCpm(*m_h.cpm)) ports::logLine("calib: counts_per_m reset not saved (NVS)");
  } else {
    *m_h.roll_mm = core::motion::kRollDefaultMm;
    if (!ports::saveRoll(*m_h.roll_mm)) ports::logLine("calib: roll-on reset not saved (NVS)");
  }
  m_out.to(ch.fd, core::jsonConfirmed(m_out.buf(), m_out.cap(), core::changeName(ch.kind)));
  m_h.send_calib(ch.fd);
}

void SettingsController::applySetting(const core::PendingChange& ch) {
  core::AckedOp set;
  set.kind = core::AckedOp::kSettingSet;
  set.a = ch.setting_id;
  set.value = ch.value;
  m_h.queue_op(set);
  core::AckedOp save;
  save.kind = core::AckedOp::kSettingsSave;
  m_h.queue_op(save);
  core::AckedOp get;  // the reply shows what the Uno stored
  get.kind = core::AckedOp::kSettingGet;
  get.a = ch.setting_id;
  get.fd = ch.fd;
  get.id = ch.id;
  m_h.queue_op(get);
  m_out.to(ch.fd, core::jsonConfirmed(m_out.buf(), m_out.cap(), core::changeName(ch.kind)));
}

void SettingsController::applyChange(core::PendingChange& ch) {
  switch (ch.kind) {
    case core::ChangeKind::kCalibSet: applyCalibSet(ch); break;
    case core::ChangeKind::kCalibReset: applyCalibReset(ch); break;
    case core::ChangeKind::kSetting: applySetting(ch); break;
    case core::ChangeKind::kNone: break;
    default: applyWifi(ch); break;
  }
  memset(ch.pass, 0, sizeof(ch.pass));
}

void SettingsController::onButtonPress(uint32_t now) {
  core::PendingChange ch;
  switch (m_button.onPress(now)) {
    case core::ButtonAction::kConfirm:
      if (!m_gate.take(ch)) break;
      ports::logLine("settings: %s confirmed on the tank", core::changeName(ch.kind));
      applyChange(ch);
      break;
    case core::ButtonAction::kToggleWifi: {
      const bool to_ap = !wifiIsAp();
      core::WifiRequest r;
      r.kind = core::WifiRequest::kMode;
      r.ap = to_ap;
      ports::wifiPost(r);
      pushFeedback(to_ap);
      ports::logLine("wifi: double press - %s", to_ap ? "own AP" : "home WiFi");
      break;
    }
    default: break;
  }
}

void SettingsController::onWifiResult(const core::WifiResult& res, uint32_t now) {
  static const char* const kWhat[] = {"wifi_status", "wifi_scan", "wifi_save", "wifi_forget", "wifi_mode",
                                      "ap_password"};
  if (res.kind == core::WifiResult::kModeChanged) {  // Plan O: 0 home, 1 own AP (forced), 2 fallback AP
    const auto m = static_cast<core::WifiMode>(res.id);
    const int32_t a = m == core::WifiMode::kStation ? 0 : (m == core::WifiMode::kForcedAp ? 1 : 2);
    m_log.add(core::LogKind::kWifiMode, a, 0, "", tankMs(), now);
    return;
  }
  if (res.kind == core::WifiResult::kStatus || res.kind == core::WifiResult::kApplied) {
    if (res.kind == core::WifiResult::kApplied) {
      m_out.to(res.fd, core::jsonConfirmed(m_out.buf(), m_out.cap(), res.req < 6 ? kWhat[res.req] : "wifi"));
    }
    return sendWifiStatus(res.fd);
  }
  if (res.kind == core::WifiResult::kScan) {
    core::WifiSnapshot s;
    ports::wifiSnapshot(s);
    return m_out.to(res.fd, core::jsonWifiScan(m_out.buf(), m_out.cap(), s.scan, s.n_scan));
  }
  m_out.to(res.fd, core::jsonNackMessage(m_out.buf(), m_out.cap(), res.id, core::BridgeReason::kInvalidCommand, res.text));
}

void SettingsController::tick(uint32_t now) {
  core::PendingChange old;
  if (m_gate.expired(now, old)) {
    m_out.to(old.fd, core::jsonConfirmExpired(m_out.buf(), m_out.cap(), core::changeName(old.kind)));
    memset(old.pass, 0, sizeof(old.pass));
  }
  core::WifiResult res;
  while (ports::wifiPoll(res)) onWifiResult(res, now);
  if (m_restart_at != 0 && static_cast<int32_t>(now - m_restart_at) >= 0) {
    m_restart_at = 0;
    ports::systemRestart();  // does not return on the tank
  }
}

}  // namespace app
}  // namespace cq
