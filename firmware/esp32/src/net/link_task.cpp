#include "core/motion_config.h"
#include "core/acked_ops.h"
#include "core/arc_limits.h"
#include "core/confirm.h"
#include "core/move_controller.h"
#include "core/roll_learner.h"
#include "net/motion_store.h"
#include "net/wifi_manager.h"
#include "core/event_log.h"
#include "net/app_settings.h"
#include "net/camera_service.h"
#include "net/stream_server.h"
#include <esp_system.h>
#include "net/link_task.h"

#include <Arduino.h>
#include <string.h>

#include "board_config.h"
#include "core/frame_tx.h"
#include "core/ws_clients.h"
#include "core/ws_codec.h"
#include "core/motion_router.h"
#include "core/motion_link.h"
#include "core/ws_json.h"
#include "net/http_server.h"
#include "net/log.h"
#include "net/uart_port.h"
#include "version.h"

namespace cq {
namespace net {
namespace {

QueueHandle_t g_queue = nullptr;
portMUX_TYPE g_status_mux = portMUX_INITIALIZER_UNLOCKED;
LinkStatus g_status;
const char* noSensor() { return "none"; }
const char* (*g_sensor)() = noSensor;

UartPort g_uart;
core::FrameTx g_tx;
core::BodyLink g_link(g_tx);
core::LinkOwner g_owner;
core::WsClients g_ws;
bool g_ota_active = false;

// WS bookkeeping (link_task only)
char g_json[core::kWsJsonMax];
char g_big[3584];              // Plan O: tank and log replies (25 log entries do not fit g_json)
core::EventLog g_log;          // Plan O (LLD R13): the Log tab
char g_ai_url[97] = "";        // the AI engine's page (NVS cq_app)
uint32_t g_last_debug_slow = 0;  // LLD R5: TELEM_SLOW count last seen by the debug stream
uint32_t g_ev_seq = 0;
uint32_t g_tf_seq = 0;
uint32_t g_last_fast_count = 0;
uint32_t g_last_slow_count = 0;
uint32_t g_last_ts_ms = 0;
struct Waiter {
  int fd;
  int32_t id;
};
constexpr size_t kMaxStopWaiters = 2 * core::kMaxWsClients;  // review M8
Waiter g_stop_waiters[kMaxStopWaiters];
size_t g_stop_waiter_n = 0;
Waiter g_cam_waiter{-1, -1};
// Plan M (bridge LLD R9): confirmations by the tank's button, the WiFi double press, acked Uno ops for the page.
core::ConfirmGate g_gate;
core::ButtonRouter g_button{g_gate};
core::AckedOps g_ops;
uint32_t g_restart_at = 0;
void askConfirm(const core::PendingChange& c, uint32_t now);
void sendCalib(int fd);
uint8_t g_cam_seq = 0;
core::StopBarrier g_stop_barrier;  // STOP fast path: drop motion posted before a STOP
core::MotionLink g_motion(g_owner);  // Plan H: TURN / DRIVE_RATE run by the bridge for a body that cannot
uint16_t g_cpm = core::motion::kCpmDefault;  // Plan K: counts per metre (NVS)
uint16_t g_roll_mm = core::motion::kRollDefaultMm;  // spec 02 §G6: learned roll-on after a move's STOP (NVS)
core::RollLearner g_roll;
int32_t g_roll_last = -1;  // diagnostics in the calib reply: last reading (mm, -1 none) ...
uint16_t g_roll_begun = 0, g_roll_read = 0;  // ... moves watched / readings taken

void drainTx() {
  const uint8_t* p = nullptr;
  size_t len = 0;
  while ((len = g_tx.peek(p)) != 0) {
    if (g_uart.txFree() < len) return;  // next tick (UART drains 25 B/ms); never a partial chunk
    g_uart.write(p, len);
    g_tx.pop();
  }
}

core::OwnerView ownerView() {
  if (g_owner.irActive()) return {"ir", ""};
  if (g_owner.driverFd() != core::LinkOwner::kNoFd) return {"human", g_ws.name(g_owner.driverFd())};
  return {"none", ""};
}

bool bodyOffline() { return !g_link.synced(); }

uint32_t tankMs() { return g_link.snapshot().fast_valid ? g_link.snapshot().fast.uno_ms : 0; }

void sendTo(int fd, size_t len) {
  if (len) wsSend(fd, g_json, len);
}

void broadcast(size_t len) {
  if (len == 0) return;
  for (size_t i = 0; i < g_ws.count(); ++i) wsSend(g_ws.fdAt(i), g_json, len);
}

void broadcastTs() {
  broadcast(core::jsonTs(g_json, sizeof(g_json), g_link.snapshot(), ownerView(), bodyOffline()));
}

void nack(int fd, int32_t id, core::BridgeReason r);

// Executes LinkOwner actions in the documented order.
void apply(const core::Actions& a, uint32_t now) {
  if (a.has(core::Actions::kStopMaster)) {
    g_link.stopMaster();
    g_tx.clear();  // drop queued ESP32 frames; a pending STOP (urgent slot) is kept
    if (g_cam_waiter.fd != -1) {  // stopMaster() cancelled the CAM_HOME: answer it (review M8)
      if (g_ws.contains(g_cam_waiter.fd)) {
        nack(g_cam_waiter.fd, g_cam_waiter.id, core::BridgeReason::kTimeout);
      }
      g_cam_waiter = Waiter{-1, -1};
    }
  }
  if (a.has(core::Actions::kSendStop)) g_link.sendStop(now);
  if (a.has(core::Actions::kHintNone)) g_link.setOwnerHint(proto::OwnerHintValue::kNone);
  if (a.has(core::Actions::kHintHuman)) g_link.setOwnerHint(proto::OwnerHintValue::kHuman);
  if (a.has(core::Actions::kStartMaster)) g_link.startMaster(now);
  if (a.has(core::Actions::kOwnerChanged)) {
    broadcast(core::jsonControlChanged(g_json, sizeof(g_json), ++g_ev_seq, tankMs(), ownerView()));
    broadcastTs();
  }
}

void nack(int fd, int32_t id, core::BridgeReason r) {
  sendTo(fd, core::jsonNack(g_json, sizeof(g_json), id, r));
}

core::BridgeReason reasonOf(core::Decision d) {
  switch (d) {
    case core::Decision::kNackBusy:
      return core::BridgeReason::kBusy;
    case core::Decision::kNackBodyOffline:
      return core::BridgeReason::kBodyOffline;
    case core::Decision::kNackStopped:
      return core::BridgeReason::kStopped;
    case core::Decision::kNackNotOwner:
    default:
      return core::BridgeReason::kNotOwner;
  }
}

// drive / cam_joy are streaming: a rejection is reported once (nack without id), not per message.
void streamReject(int fd, core::BridgeReason r) {
  if (g_ws.reportStreamReject(fd, static_cast<uint8_t>(r))) nack(fd, -1, r);
}

// Plan H (bridge LLD R4.6): execute a MotionLink decision - DRIVE src AUTO, STOP, primitive_done.
void runMotion(const core::LinkOut& o, uint32_t now) {
  if (o.drive && !g_link.sendDrive(o.v, o.w, proto::Source::kAuto)) {
    // Body gone: no frame can reach it. Queue full while synced: the STOP still goes (urgent slot, review minor 12).
    const core::MotionEvent e = g_link.synced() ? core::MotionEvent::kTxFull : core::MotionEvent::kBodyOffline;
    runMotion(g_motion.onEvent(e, now), now);
    return;
  }
  if (o.stop) g_link.sendStop(now);  // STOP purges queued motion frames: no DRIVE 0/0 needed (review M11)
  if (o.event) broadcast(core::jsonPrimitiveDone(g_json, sizeof(g_json), ++g_ev_seq, tankMs(), o.end));
  if (o.event && o.end.reason != core::EndReason::kDone) {  // Plan O: why a move / turn ended early
    g_log.add(core::LogKind::kPrimitive, static_cast<int32_t>(o.end.reason), static_cast<int32_t>(o.end.kind), "", tankMs(), now);
  }
  if (o.event && o.end.kind == core::PrimKind::kMove && o.end.reason == core::EndReason::kDone) {
    const core::TankSnapshot& s = g_link.snapshot();  // §G6: watch the roll-on from here
    g_roll.begin((s.fast.enc_l + s.fast.enc_r) / 2, now);
    ++g_roll_begun;
  }
}

void endMotion(core::MotionEvent e, uint32_t now) { runMotion(g_motion.onEvent(e, now), now); }

bool motionOwnedBy(int fd) { return g_motion.service().active() && g_motion.service().ownerFd() == fd; }

// Plan K: the TELEM_FAST fields the motion service uses.
core::MotionTelem telemOf(const core::TankSnapshot& s) {
  core::MotionTelem t;
  t.yaw_cdeg = s.fast.yaw;
  t.rate_ddps = s.fast.yaw_rate;
  t.enc_l = s.fast.enc_l;
  t.enc_r = s.fast.enc_r;
  t.flags = s.fast.flags;
  t.dist_mm = s.fast.dist_mm;
  return t;
}

// Streaming commands (one rejection reported per stream) and every motion command (dropped behind a STOP).
bool isStream(core::WsCmdKind k) {
  return k == core::WsCmdKind::kDrive || k == core::WsCmdKind::kCamJoy || k == core::WsCmdKind::kDriveRate;
}
bool isMotion(core::WsCmdKind k) {
  return isStream(k) || k == core::WsCmdKind::kTurn || k == core::WsCmdKind::kMove;
}

// SOUND / AUDIO_MODE / CAM_LOCK on the shared acked slot (Plan F, R3.1); false = not queued (body offline).
bool sendAckedCommand(const core::WsCommand& c, uint32_t now, uint8_t& seq) {
  switch (c.kind) {
    case core::WsCmdKind::kSound: return g_link.sendSound(c.sound_id, now, seq);
    case core::WsCmdKind::kAudioMode: return g_link.sendAudioMode(c.audio_mode, now, seq);
    default: return g_link.sendCamLock(c.on, now, seq);
  }
}

// turn (LLD R4.2 / R4.6 / R8): spin, arc or pivot - Local on a body without TURN / MOVE; Forward / Unavailable
// answered UNAVAILABLE for now.
void handleTurn(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  if (g_ota_active) {
    nack(fd, id, core::BridgeReason::kBusy);
    return;
  }
  if (core::routeTurn(g_link.hello().caps, c.radius_cm, c.pivot) != core::Route::kLocal) {
    nack(fd, id, core::BridgeReason::kUnavailable);  // arc / pivot need encoders; Forward: no full-firmware body yet
    return;
  }
  const core::TankSnapshot& snap = g_link.snapshot();
  if (bodyOffline() || !snap.fast_valid) {
    nack(fd, id, core::BridgeReason::kBodyOffline);
    return;
  }
  if (c.radius_cm > 0) {  // Plan L: arcs need trustworthy counts and a radius / speed the tracks can drive
    if ((snap.fast.flags >> static_cast<unsigned>(proto::FastFlagBit::kEncoderFault)) & 1u) {
      nack(fd, id, core::BridgeReason::kUnavailable);
      return;
    }
    const core::ArcBand b = core::arcBand(c.radius_cm);
    if (!b.possible || c.speed < b.lo || c.speed > b.hi) {
      char why[96];
      core::arcRefusal(why, sizeof(why), c.radius_cm);
      sendTo(fd, core::jsonNackMessage(g_json, sizeof(g_json), id, core::BridgeReason::kInvalidCommand, why));
      return;
    }
  }
  core::Actions a;
  const core::Decision d = g_owner.onDrive(fd, now, a);
  apply(a, now);
  if (d != core::Decision::kSend) {
    nack(fd, id, reasonOf(d));
    return;
  }
  endMotion(core::MotionEvent::kOwnerCommand, now);  // a new turn replaces the old primitive
  g_roll.cancel();  // §G6: a new primitive - the last move's roll-on is no longer measurable
  const int32_t delta = core::turnDelta(snap.fast.yaw, c.turn_has_to, c.turn_by_cdeg, c.turn_to_cdeg);
  if (c.radius_cm > 0) {
    g_motion.service().startArc(fd, telemOf(snap), delta, c.radius_cm, c.speed, g_cpm, id, now);
  } else if (c.pivot) {
    g_motion.service().startPivot(fd, snap.fast.yaw, delta, c.speed, id, now);
  } else {
    g_motion.service().startTurn(fd, snap.fast.yaw, delta, c.speed, id, now);
  }
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
}

// drive_rate (LLD R4.4): a stream like drive; the DRIVE frames come from the service on each TELEM_FAST.
void handleDriveRate(int fd, const core::WsCommand& c, uint32_t now) {
  if (g_ota_active) {
    streamReject(fd, core::BridgeReason::kBusy);
    return;
  }
  if (core::routeDriveRate(g_link.hello().caps) != core::Route::kLocal) {
    streamReject(fd, core::BridgeReason::kUnavailable);  // Forward: no full-firmware body yet (once per stream)
    return;
  }
  if (bodyOffline()) {
    streamReject(fd, core::BridgeReason::kBodyOffline);
    return;
  }
  core::Actions a;
  const core::Decision d = g_owner.onDrive(fd, now, a);
  apply(a, now);
  if (d != core::Decision::kSend) {
    streamReject(fd, reasonOf(d));
    return;
  }
  const core::MotionService& svc = g_motion.service();
  if (svc.active() && (svc.kind() != core::PrimKind::kRate || svc.ownerFd() != fd)) {
    endMotion(core::MotionEvent::kOwnerCommand, now);
  }
  g_roll.cancel();  // §G6: a new primitive - the last move's roll-on is no longer measurable
  g_motion.service().driveRate(fd, c.v, c.rate_ddps, now);
  g_ws.streamAccepted(fd);
}

// move (Plan K, LLD R7.6): Local for an Uno with encoders and no encoder fault; Forward / Unavailable answered
// UNAVAILABLE (no full-firmware body yet).
void handleMove(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  if (g_ota_active) {
    nack(fd, id, core::BridgeReason::kBusy);
    return;
  }
  if (core::routeMove(g_link.hello().caps) != core::Route::kLocal) {
    nack(fd, id, core::BridgeReason::kUnavailable);  // encoders off
    return;
  }
  const core::TankSnapshot& snap = g_link.snapshot();
  if (bodyOffline() || !snap.fast_valid) {
    nack(fd, id, core::BridgeReason::kBodyOffline);
    return;
  }
  if ((snap.fast.flags >> static_cast<unsigned>(proto::FastFlagBit::kEncoderFault)) & 1u) {
    nack(fd, id, core::BridgeReason::kUnavailable);  // an encoder is switched off until the Uno restarts
    return;
  }
  core::Actions a;
  const core::Decision d = g_owner.onDrive(fd, now, a);
  apply(a, now);
  if (d != core::Decision::kSend) {
    nack(fd, id, reasonOf(d));
    return;
  }
  endMotion(core::MotionEvent::kOwnerCommand, now);  // a new move replaces the old primitive
  g_roll.cancel();  // a new move: the last one's roll-on is no longer measurable
  g_motion.service().startMove(fd, telemOf(snap), c.move_cm, c.speed, g_cpm, id, now, g_roll_mm);
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
}

// calib (Plan K, LLD R7.1): read for anyone; set only by the driver, not while a primitive runs.
void handleCalib(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  if (c.calib_reset >= 0) {  // Plan M: reset to the default - confirmed by the tank's button
    core::PendingChange ch;
    ch.kind = core::ChangeKind::kCalibReset;
    ch.fd = fd;
    ch.id = id;
    ch.calib_what = static_cast<uint8_t>(c.calib_reset);
    askConfirm(ch, now);
    return;
  }
  if (c.has_cpm) {  // Plan O (owner, design-notes §91): a save waits for the tank's button
    if (g_owner.driverFd() != fd) {
      nack(fd, id, core::BridgeReason::kNotOwner);
      return;
    }
    if (g_motion.service().active()) {
      nack(fd, id, core::BridgeReason::kBusy);
      return;
    }
    core::PendingChange ch;
    ch.kind = core::ChangeKind::kCalibSet;
    ch.fd = fd;
    ch.id = id;
    ch.cpm = c.cpm;
    askConfirm(ch, now);
    return;
  }
  sendCalib(fd);
}

void sendCalib(int fd) {
  size_t n = core::jsonCalib(g_json, sizeof(g_json) - 64, g_cpm, g_roll_mm);
  if (n > 1) {  // + roll-on learner diagnostics (spec 02 §G6 floor check): replace the closing brace
    const int k = snprintf(g_json + n - 1, sizeof(g_json) - n + 1, ",\"roll_last\":%ld,\"roll_watched\":%u,\"roll_read\":%u}",
                           static_cast<long>(g_roll_last), static_cast<unsigned>(g_roll_begun),
                           static_cast<unsigned>(g_roll_read));
    if (k > 0) n = n - 1 + static_cast<size_t>(k);
  }
  sendTo(fd, n);
}


// ---------- Plan M (spec 02 Part I, bridge LLD R9): WiFi, settings page, confirmations ----------
bool settingReadable(uint8_t id) { return id == 1 || id == 11 || id == 12 || id == 66; }

void pushFeedback(bool to_ap) {  // the tank says which mode it went to: jingle + blue (home) / white (own AP)
  core::AckedOp snd;
  snd.kind = core::AckedOp::kSound;
  snd.a = static_cast<uint8_t>(proto::SoundId::kJingle);
  g_ops.push(snd);
  core::AckedOp led;
  led.kind = core::AckedOp::kLed;
  led.a = static_cast<uint8_t>(proto::LedMode::kBlink);
  led.r = to_ap ? 255 : 0;
  led.g = to_ap ? 255 : 0;
  led.b = 255;
  led.period_ms = 200;
  led.duration_ms = 3000;
  g_ops.push(led);
}

bool wifiIsAp() {
  net::WifiSnapshot s;
  net::wifiSnapshot(s);
  return strcmp(s.mode, "ap") == 0;
}

void sendWifiStatus(int fd) {
  net::WifiSnapshot s;
  net::wifiSnapshot(s);
  char host[40];
  snprintf(host, sizeof(host), "%s.local", board::kHostname);
  core::WifiStatusView v{s.mode, s.ssid, s.rssi, s.ip, host, {s.saved[0], s.saved[1], s.saved[2]}, s.n_saved,
                         board::kFallbackSsid};
  sendTo(fd, core::jsonWifiStatus(g_json, sizeof(g_json), v));
}

// A change from the page: held until the tank's button is pressed (or 30 s pass).
void askConfirm(const core::PendingChange& c, uint32_t now) {
  if (g_gate.busyFor(c.fd)) {  // Plan O final review I2: another device's change waits for the button
    nack(c.fd, c.id, core::BridgeReason::kBusy);
    return;
  }
  g_gate.begin(c, now);
  sendTo(c.fd, core::jsonConfirm(g_json, sizeof(g_json), core::changeName(c.kind),
                                 static_cast<uint8_t>(core::ConfirmGate::kWindowMs / 1000)));
}

void handleSettingsCommand(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  core::PendingChange ch;
  ch.fd = fd;
  ch.id = id;
  switch (c.kind) {
    case core::WsCmdKind::kWifiStatus: sendWifiStatus(fd); return;
    case core::WsCmdKind::kWifiScan: {
      net::WifiRequest r;
      r.kind = net::WifiRequest::kScan;
      r.fd = fd;
      r.id = id;
      if (!net::wifiPost(r)) nack(fd, id, core::BridgeReason::kBusy);
      return;
    }
    case core::WsCmdKind::kWifiSave:
      ch.kind = core::ChangeKind::kWifiSave;
      ch.slot = c.slot;
      snprintf(ch.ssid, sizeof(ch.ssid), "%s", c.ssid);
      snprintf(ch.pass, sizeof(ch.pass), "%s", c.password);
      break;
    case core::WsCmdKind::kWifiForget:
      ch.kind = core::ChangeKind::kWifiForget;
      ch.slot = c.slot;
      break;
    case core::WsCmdKind::kWifiMode:
      ch.kind = core::ChangeKind::kWifiMode;
      ch.ap = c.wifi_ap;
      break;
    case core::WsCmdKind::kApPassword:
      ch.kind = core::ChangeKind::kApPassword;
      snprintf(ch.pass, sizeof(ch.pass), "%s", c.password);
      break;
    case core::WsCmdKind::kSetting:
      if (!settingReadable(c.setting_id)) {
        nack(fd, id, core::BridgeReason::kInvalidCommand);
        return;
      }
      if (!c.has_value) {  // read: SETTING_GET -> SETTING_VALUE
        core::AckedOp op;
        op.kind = core::AckedOp::kSettingGet;
        op.a = c.setting_id;
        op.fd = fd;
        op.id = id;
        if (!g_ops.push(op)) nack(fd, id, core::BridgeReason::kBusy);
        return;
      }
      if (c.setting_id != 66 || c.value < -100 || c.value > 100) {  // v1: only track_balance may change
        nack(fd, id, core::BridgeReason::kInvalidCommand);
        return;
      }
      ch.kind = core::ChangeKind::kSetting;
      ch.setting_id = c.setting_id;
      ch.value = c.value;
      break;
    case core::WsCmdKind::kRestart:
      if (g_ota_active || g_motion.service().active()) {
        nack(fd, id, core::BridgeReason::kBusy);
        return;
      }
      if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
      g_restart_at = now + 500;
      logf("bridge: restart asked by %s", g_ws.name(fd));
      g_log.add(core::LogKind::kRestart, 0, 0, g_ws.name(fd), tankMs(), now);
      return;
    default:
      return;
  }
  askConfirm(ch, now);
}

void applyChange(core::PendingChange& ch, uint32_t now) {
  (void)now;
  const char* what = core::changeName(ch.kind);
  net::WifiRequest r;
  r.fd = ch.fd;
  r.id = ch.id;
  switch (ch.kind) {
    case core::ChangeKind::kWifiSave:
      r.kind = net::WifiRequest::kSave;
      r.slot = ch.slot;
      snprintf(r.ssid, sizeof(r.ssid), "%s", ch.ssid);
      snprintf(r.pass, sizeof(r.pass), "%s", ch.pass);
      break;
    case core::ChangeKind::kWifiForget:
      r.kind = net::WifiRequest::kForget;
      r.slot = ch.slot;
      break;
    case core::ChangeKind::kWifiMode:
      r.kind = net::WifiRequest::kMode;
      r.ap = ch.ap;
      pushFeedback(ch.ap);
      break;
    case core::ChangeKind::kApPassword:
      r.kind = net::WifiRequest::kApPassword;
      snprintf(r.pass, sizeof(r.pass), "%s", ch.pass);
      break;
    case core::ChangeKind::kCalibSet:  // Plan O: checked again now - no primitive may be running (the driver may
                                       // have changed during the 30 s; the save itself is not motion)
      if (g_motion.service().active() || !net::motionStoreSave(ch.cpm)) {
        if (g_ws.contains(ch.fd)) nack(ch.fd, ch.id, core::BridgeReason::kBusy);
        return;
      }
      g_cpm = ch.cpm;
      if (g_ws.contains(ch.fd)) {
        sendTo(ch.fd, core::jsonConfirmed(g_json, sizeof(g_json), what));
        sendCalib(ch.fd);
      }
      return;
    case core::ChangeKind::kCalibReset:
      if (ch.calib_what == 0) {
        g_cpm = core::motion::kCpmDefault;
        net::motionStoreSave(g_cpm);
      } else {
        g_roll_mm = core::motion::kRollDefaultMm;
        net::motionStoreSaveRoll(g_roll_mm);
      }
      sendTo(ch.fd, core::jsonConfirmed(g_json, sizeof(g_json), what));
      sendCalib(ch.fd);
      memset(ch.pass, 0, sizeof(ch.pass));
      return;
    case core::ChangeKind::kSetting: {
      core::AckedOp set;
      set.kind = core::AckedOp::kSettingSet;
      set.a = ch.setting_id;
      set.value = ch.value;
      g_ops.push(set);
      core::AckedOp save;
      save.kind = core::AckedOp::kSettingsSave;
      g_ops.push(save);
      core::AckedOp get;  // the reply shows what the Uno stored
      get.kind = core::AckedOp::kSettingGet;
      get.a = ch.setting_id;
      get.fd = ch.fd;
      get.id = ch.id;
      g_ops.push(get);
      sendTo(ch.fd, core::jsonConfirmed(g_json, sizeof(g_json), what));
      return;
    }
    case core::ChangeKind::kNone:
    default:
      return;
  }
  memset(ch.pass, 0, sizeof(ch.pass));
  if (!net::wifiPost(r)) nack(ch.fd, ch.id, core::BridgeReason::kBusy);
  memset(r.pass, 0, sizeof(r.pass));
}

void onButtonPress(uint32_t now) {
  switch (g_button.onPress(now)) {
    case core::ButtonAction::kConfirm: {
      core::PendingChange ch;
      if (g_gate.take(ch)) {
        logf("settings: %s confirmed on the tank", core::changeName(ch.kind));
        applyChange(ch, now);
      }
      break;
    }
    case core::ButtonAction::kToggleWifi: {
      const bool to_ap = !wifiIsAp();
      net::WifiRequest r;
      r.kind = net::WifiRequest::kMode;
      r.ap = to_ap;
      net::wifiPost(r);
      pushFeedback(to_ap);
      logf("wifi: double press - %s", to_ap ? "own AP" : "home WiFi");
      break;
    }
    case core::ButtonAction::kNone:
    default:
      break;
  }
}

// Each tick: expired confirmations, WiFi results, the next acked op, a pending restart.
void settingsTick(uint32_t now) {
  core::PendingChange old;
  if (g_gate.expired(now, old)) {
    if (g_ws.contains(old.fd)) sendTo(old.fd, core::jsonConfirmExpired(g_json, sizeof(g_json), core::changeName(old.kind)));
    memset(old.pass, 0, sizeof(old.pass));
  }
  net::WifiResult res;
  while (net::wifiPoll(res)) {
    if (res.kind == net::WifiResult::kModeChanged) {  // Plan O: 0 home, 1 own AP (forced), 2 fallback AP
      const auto m = static_cast<core::WifiMode>(res.id);
      g_log.add(core::LogKind::kWifiMode, m == core::WifiMode::kStation ? 0 : (m == core::WifiMode::kForcedAp ? 1 : 2), 0, "",
                tankMs(), now);
      continue;
    }
    if (!g_ws.contains(res.fd)) continue;
    static const char* const kWhat[] = {"wifi_status", "wifi_scan", "wifi_save", "wifi_forget", "wifi_mode",
                                        "ap_password"};
    switch (res.kind) {
      case net::WifiResult::kStatus: sendWifiStatus(res.fd); break;
      case net::WifiResult::kScan: {
        net::WifiSnapshot s;
        net::wifiSnapshot(s);
        sendTo(res.fd, core::jsonWifiScan(g_json, sizeof(g_json), s.scan, s.n_scan));
        break;
      }
      case net::WifiResult::kApplied:
        sendTo(res.fd, core::jsonConfirmed(g_json, sizeof(g_json), res.req < 6 ? kWhat[res.req] : "wifi"));
        sendWifiStatus(res.fd);
        break;
      case net::WifiResult::kFailed:
        sendTo(res.fd, core::jsonNackMessage(g_json, sizeof(g_json), res.id, core::BridgeReason::kInvalidCommand,
                                             res.text));
        break;
    }
  }
  core::AckedOp op;
  if (g_ops.busy() && g_ops.timedOut(now)) {  // review I1: no answer (lost frame) - never stuck
    if (g_ops.front(op) && op.fd >= 0 && g_ws.contains(op.fd)) nack(op.fd, op.id, core::BridgeReason::kTimeout);
    g_ops.done();
  }
  if (!g_ops.busy() && !g_link.ackPending() && g_cam_waiter.fd == -1 && g_ops.front(op)) {
    uint8_t seq = 0;
    bool sent = false;
    switch (op.kind) {
      case core::AckedOp::kSound: sent = g_link.sendSound(op.a, now, seq); break;
      case core::AckedOp::kLed:
        sent = g_link.sendLed(op.a, op.r, op.g, op.b, op.period_ms, op.duration_ms, now, seq);
        break;
      case core::AckedOp::kSettingGet:
        sent = g_link.sendSettingGet(op.a, now, seq);
        break;
      case core::AckedOp::kSettingSet: sent = g_link.sendSettingSet(op.a, op.value, now, seq); break;
      case core::AckedOp::kSettingsSave: sent = g_link.sendSettingsSave(now, seq); break;
      case core::AckedOp::kSelfTest:
        sent = g_link.sendCalibrate(static_cast<uint8_t>(proto::CalOp::kStart),
                                    static_cast<uint8_t>(proto::CalScript::kSelfTest), now, seq);
        break;
    }
    if (sent) {
      g_ops.markSent(seq, now);
    } else if (!g_link.synced()) {  // body offline: answer and drop it
      if (op.fd >= 0 && g_ws.contains(op.fd)) nack(op.fd, op.id, core::BridgeReason::kBodyOffline);
      g_ops.markSent(0, now);
      g_ops.done();
    }
  }
  if (g_restart_at != 0 && static_cast<int32_t>(now - g_restart_at) >= 0) ESP.restart();
}

// cancel: the owner ends its primitive (STOP while still the owner); acked also when nothing runs.
void handleCancel(int fd, int32_t id, uint32_t now) {
  if (g_motion.service().active() && g_motion.service().ownerFd() != fd) {
    nack(fd, id, core::BridgeReason::kNotOwner);
    return;
  }
  endMotion(core::MotionEvent::kCancel, now);
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
}

// STOP fast path: may overtake its own kWsOpen - never lost.
void handleStop(const LinkCommand& cmd, int32_t id, uint32_t now) {
  const int fd = cmd.fd;
  g_stop_barrier.onStop(cmd.post_seq);
  if (g_owner.state() == core::LinkState::kWebDriving && g_owner.driverFd() != fd) {  // Plan O: §72 stop hold
    g_log.add(core::LogKind::kStopHold, 0, 0, g_ws.contains(fd) ? g_ws.name(fd) : "?", tankMs(), now);
  }
  apply(g_owner.onStop(fd), now);  // urgent slot; queued DRIVE/CAM_JOY frames are purged
  endMotion(core::MotionEvent::kStop, now);  // Plan H: a STOP from anyone ends a primitive
  if (!g_ws.contains(fd)) return;
  g_ws.admit(fd, core::WsCmdKind::kStop, now);  // never limited; counts as alive
  if (g_stop_waiter_n < kMaxStopWaiters) {
    g_stop_waiters[g_stop_waiter_n++] = Waiter{fd, id};
  } else {
    nack(fd, id, core::BridgeReason::kBusy);  // review M8: answered; the STOP was still applied
  }
}

// False when the command is dropped (stale behind a STOP) or rate limited (answered).
bool admitted(const LinkCommand& cmd, core::WsCmdKind kind, int32_t id, uint32_t now) {
  const int fd = cmd.fd;
  if (isMotion(kind) && g_stop_barrier.stale(cmd.post_seq)) {
    return false;  // posted before a STOP that was handled first: silently dropped
  }
  if (g_ws.admit(fd, kind, now)) return true;
  if (isStream(kind)) {
    streamReject(fd, core::BridgeReason::kRateLimited);
  } else {
    nack(fd, id, core::BridgeReason::kRateLimited);
  }
  return false;
}

void handleHello(int fd) {
  const core::HelloInfo hello = g_link.hello();
  const core::WelcomeInfo w{CQ_FW_VERSION, &hello, g_sensor(), ownerView(), g_ws.name(fd)};
  sendTo(fd, core::jsonWelcome(g_json, sizeof(g_json), w));
  sendTo(fd, core::jsonTs(g_json, sizeof(g_json), g_link.snapshot(), ownerView(), bodyOffline()));
}

void handleDrive(int fd, const core::WsCommand& c, uint32_t now) {
  if (g_ota_active) {
    streamReject(fd, core::BridgeReason::kBusy);
    return;
  }
  core::Actions a;
  const core::Decision d = g_owner.onDrive(fd, now, a, c.v != 0 || c.w != 0);  // §72: a held stick stays stopped
  apply(a, now);
  if (d == core::Decision::kSend && motionOwnedBy(fd)) endMotion(core::MotionEvent::kOwnerCommand, now);
  if (d == core::Decision::kSend && (c.v != 0 || c.w != 0)) g_roll.cancel();  // §G6: driving again, no roll-on
  if (d != core::Decision::kSend) {
    streamReject(fd, reasonOf(d));
  } else if (g_link.sendDrive(c.v, c.w)) {  // 1:1 - the bridge never repeats a DRIVE
    g_ws.streamAccepted(fd);
  } else {
    streamReject(fd, core::BridgeReason::kBodyOffline);
  }
}

void handleCamJoy(int fd, const core::WsCommand& c) {
  const core::Decision d = g_owner.onCamera(fd);
  if (d != core::Decision::kSend || g_ota_active) {
    streamReject(fd, g_ota_active ? core::BridgeReason::kBusy : reasonOf(d));
  } else if (g_link.sendCamJoy(c.x, c.y, c.active)) {
    g_ws.streamAccepted(fd);
  }
}

void handleCamHome(int fd, int32_t id, uint32_t now) {
  const core::Decision d = g_owner.onCamera(fd);
  if (d != core::Decision::kSend) {
    nack(fd, id, reasonOf(d));
  } else if (g_cam_waiter.fd != -1 || g_link.ackPending()) {
    nack(fd, id, core::BridgeReason::kBusy);  // review M8: one cam_home at a time, always answered
  } else if (g_link.sendCamHome(now, g_cam_seq)) {
    g_cam_waiter = Waiter{fd, id};
  } else {
    nack(fd, id, core::BridgeReason::kBodyOffline);
  }
}

// SOUND / AUDIO_MODE (R3.1) / CAM_LOCK (Plan F): owner or nobody (as the camera); acked by the Uno, one at a time.
void handleAcked(int fd, int32_t id, const core::WsCommand& c, uint32_t now) {
  const core::Decision d = g_owner.onCamera(fd);
  uint8_t seq = 0;
  if (d != core::Decision::kSend || g_ota_active) {
    nack(fd, id, g_ota_active ? core::BridgeReason::kBusy : reasonOf(d));
  } else if (g_cam_waiter.fd != -1 || g_link.ackPending()) {  // Plan M: a settings op may hold the slot
    nack(fd, id, core::BridgeReason::kBusy);
  } else if (sendAckedCommand(c, now, seq)) {
    g_cam_seq = seq;
    g_cam_waiter = Waiter{fd, id};
  } else {
    nack(fd, id, core::BridgeReason::kBodyOffline);
  }
}

// LLD R5: opt-in debug stream for this client only (any role: read-only data).
void handleDebug(int fd, int32_t id, const core::WsCommand& c) {
  g_ws.setDebug(fd, c.on ? static_cast<uint8_t>(c.debug_hz == 50 ? 1 : 5) : 0);
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
}

void handleRelease(int fd, int32_t id, uint32_t now) {
  apply(g_owner.onRelease(fd), now);
  if (motionOwnedBy(fd)) endMotion(core::MotionEvent::kRelease, now);  // review I1: release ends a primitive
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
}

// Plan O (bridge LLD R13): the Tank tab's facts. `posted`: camera settings just posted (not applied yet).
void sendTank(int fd, const net::CamSettings* posted) {
  core::TankView v;
  v.st = g_link.selfTest();
  v.st_age_s = v.st.seen ? (millis() - v.st.at_ms) / 1000u : 0;
  v.hw = g_link.snapshot().slow_valid ? g_link.snapshot().slow.hw : 0;
  const net::CamSettings cs = posted != nullptr ? *posted : net::cameraSettings();
  v.cam_size = cs.size;
  v.cam_quality = cs.quality;
  v.cam_fps = cs.fps_cap;
  v.cam_bright = cs.brightness;
  v.cam_contrast = cs.contrast;
  v.cam_low = cs.low_light;
  v.cam_vflip = cs.vflip;
  v.cam_hmirror = cs.hmirror;
  v.viewers = net::streamViewers();
  const net::CamStats st = net::cameraStats(millis());
  v.fps_x10 = st.fps_x10;
  v.kbps = st.kbps;
  v.ai_url = g_ai_url;
  const size_t n = core::jsonTank(g_big, sizeof(g_big), v);
  if (n) wsSend(fd, g_big, n);
}

void sendLog(int fd, uint32_t since, uint32_t now) {
  core::LogEntry e[25];
  bool gap = false;
  const uint8_t n = g_log.since(since, e, 25, gap);
  const size_t len = core::jsonLog(g_big, sizeof(g_big), e, n, g_log.last(), gap, now);
  if (len) wsSend(fd, g_big, len);
}

void handleCameraSet(int fd, int32_t id, const core::WsCommand& c) {
  net::CamSettings s = c.cam.reset ? net::cameraDefaults() : net::cameraSettings();
  if (c.cam.has_size) s.size = c.cam.size;
  if (c.cam.has_quality) s.quality = c.cam.quality;
  if (c.cam.has_fps) s.fps_cap = c.cam.fps;
  if (c.cam.has_bright) s.brightness = c.cam.bright;
  if (c.cam.has_contrast) s.contrast = c.cam.contrast;
  if (c.cam.has_low) s.low_light = c.cam.low;
  if (c.cam.has_vflip) s.vflip = c.cam.vflip;
  if (c.cam.has_hmirror) s.hmirror = c.cam.hmirror;
  if (!net::cameraPost(s)) {
    nack(fd, id, core::BridgeReason::kUnavailable);  // no camera
    return;
  }
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
  sendTank(fd, &s);
}

void handleSelfTest(int fd, int32_t id) {
  const core::TankSnapshot& s = g_link.snapshot();
  if (bodyOffline()) {
    nack(fd, id, core::BridgeReason::kBodyOffline);
    return;
  }
  if (g_motion.service().active() || g_owner.state() == core::LinkState::kWebDriving ||
      (s.slow_valid && s.slow.driving != 0)) {
    nack(fd, id, core::BridgeReason::kBusy);  // the tank must stand still
    return;
  }
  core::AckedOp op;
  op.kind = core::AckedOp::kSelfTest;
  op.fd = fd;
  op.id = id;
  if (!g_ops.push(op)) nack(fd, id, core::BridgeReason::kBusy);
}

void handleAppSet(int fd, int32_t id, const core::WsCommand& c) {
  if (!net::appAiUrlSave(c.ai_url)) {  // short NVS write (rare, page action)
    nack(fd, id, core::BridgeReason::kBusy);
    return;
  }
  snprintf(g_ai_url, sizeof(g_ai_url), "%s", c.ai_url);
  if (id >= 0) sendTo(fd, core::jsonAck(g_json, sizeof(g_json), id));
}

void handleWsText(const LinkCommand& cmd, uint32_t now) {
  const int fd = cmd.fd;
  const core::WsCommand c = core::parseWsCommand(cmd.text, cmd.len);
  const int32_t id = c.has_id ? c.id : -1;
  if (c.kind == core::WsCmdKind::kStop) {
    handleStop(cmd, id, now);
    return;
  }
  if (!g_ws.contains(fd) || !admitted(cmd, c.kind, id, now)) return;
  switch (c.kind) {
    case core::WsCmdKind::kHello: handleHello(fd); break;
    case core::WsCmdKind::kDrive: handleDrive(fd, c, now); break;
    case core::WsCmdKind::kStop: break;  // handled above (fast path)
    case core::WsCmdKind::kCamJoy: handleCamJoy(fd, c); break;
    case core::WsCmdKind::kCamHome: handleCamHome(fd, id, now); break;
    case core::WsCmdKind::kSound:
    case core::WsCmdKind::kAudioMode:
    case core::WsCmdKind::kCamLock: handleAcked(fd, id, c, now); break;
    case core::WsCmdKind::kTurn: handleTurn(fd, id, c, now); break;
    case core::WsCmdKind::kDriveRate: handleDriveRate(fd, c, now); break;
    case core::WsCmdKind::kCancel: handleCancel(fd, id, now); break;
    case core::WsCmdKind::kMove: handleMove(fd, id, c, now); break;
    case core::WsCmdKind::kCalib: handleCalib(fd, id, c, now); break;
    case core::WsCmdKind::kWifiStatus:
    case core::WsCmdKind::kWifiScan:
    case core::WsCmdKind::kWifiSave:
    case core::WsCmdKind::kWifiForget:
    case core::WsCmdKind::kWifiMode:
    case core::WsCmdKind::kApPassword:
    case core::WsCmdKind::kSetting:
    case core::WsCmdKind::kRestart: handleSettingsCommand(fd, id, c, now); break;
    case core::WsCmdKind::kRelease: handleRelease(fd, id, now); break;
    case core::WsCmdKind::kTank: sendTank(fd, nullptr); break;
    case core::WsCmdKind::kLog: sendLog(fd, c.log_since, now); break;
    case core::WsCmdKind::kCameraSet: handleCameraSet(fd, id, c); break;
    case core::WsCmdKind::kSelfTest: handleSelfTest(fd, id); break;
    case core::WsCmdKind::kAppSet: handleAppSet(fd, id, c); break;
    case core::WsCmdKind::kDebug: handleDebug(fd, id, c); break;
    case core::WsCmdKind::kPing: sendTo(fd, core::jsonPong(g_json, sizeof(g_json), c.t0, now)); break;
    case core::WsCmdKind::kUnsupported:
    case core::WsCmdKind::kInvalid:
    default: nack(fd, id, core::BridgeReason::kInvalidCommand); break;
  }
}

void closeAllClients() {
  for (size_t i = 0; i < g_ws.count(); ++i) wsClose(g_ws.fdAt(i));  // kWsClose follows
}

void handleCommand(const LinkCommand& cmd, uint32_t now) {
  switch (cmd.kind) {
    case LinkCommand::Kind::kWsOpen:
      if (g_ota_active || !g_ws.add(cmd.fd, now)) {
        wsClose(cmd.fd);  // full (4 clients) or updating
        return;
      }
      logf("ws: %s connected (%u clients)", g_ws.name(cmd.fd), static_cast<unsigned>(g_ws.count()));
      apply(g_owner.onWsOpen(cmd.fd), now);
      break;
    case LinkCommand::Kind::kWsClose: {
      if (!g_ws.contains(cmd.fd)) return;  // a plain HTTP socket
      char name[12];
      snprintf(name, sizeof(name), "%s", g_ws.name(cmd.fd));
      if (motionOwnedBy(cmd.fd)) endMotion(core::MotionEvent::kOwnerClosed, now);  // onWsClose STOPs its driver
      if (cmd.fd == g_owner.driverFd()) g_log.add(core::LogKind::kClientLost, 0, 0, name, tankMs(), now);  // Plan O
      const core::Actions a = g_owner.onWsClose(cmd.fd);
      g_gate.dropClient(cmd.fd);  // Plan M: a waiting change of that client is dropped (review focus 4)
      g_ws.remove(cmd.fd);
      logf("ws: %s closed (%u clients)", name, static_cast<unsigned>(g_ws.count()));
      apply(a, now);
      break;
    }
    case LinkCommand::Kind::kWsText:
      handleWsText(cmd, now);
      break;
    case LinkCommand::Kind::kOtaStart:
    default:
      g_ota_active = true;  // from now on: drives nacked BUSY, new phones refused
      g_log.add(core::LogKind::kOta, 0, 0, "", tankMs(), now);
      endMotion(core::MotionEvent::kOta, now);  // the STOP below stops it
      g_link.sendStop(now);  // resent until the Uno's ACK
      g_link.stopMaster();  // heartbeats stop: Uno link-loss stop at standstill is harmless
      closeAllClients();
      break;
  }
}

void handleLinkEvents(uint32_t now) {
  core::LinkEvent ev{};
  while (g_link.popEvent(ev)) {
    switch (ev.kind) {
      case core::LinkEventKind::kBodyOnline: {
        const core::HelloInfo& h = g_link.hello();
        logf("uno: synced fw %u.%u proto %u commit %08lx", h.fw >> 8, h.fw & 0xFF, h.proto,
             static_cast<unsigned long>(h.commit));
        broadcast(core::jsonBody(g_json, sizeof(g_json), ++g_ev_seq, tankMs(), true, -1));
        g_log.add(core::LogKind::kBodyOnline, h.fw, h.proto, "", tankMs(), now);
        apply(g_owner.onSynced(true), now);
        break;
      }
      case core::LinkEventKind::kBodyOffline:
        if (ev.code == 1) {
          logf("uno: incompatible protocol %d (need %u) - flash Plan B firmware", ev.a,
               proto::kProtocolVersion);
        } else {
          logf("uno: body offline (silent)");
        }
        broadcast(core::jsonBody(g_json, sizeof(g_json), ++g_ev_seq, tankMs(), false,
                                 ev.code == 1 ? ev.a : -1));
        endMotion(core::MotionEvent::kBodyOffline, now);
        apply(g_owner.onSynced(false), now);
        g_log.add(core::LogKind::kBodyOffline, ev.code, ev.a, "", tankMs(), now);
        break;
      case core::LinkEventKind::kUnoEvent:
        if (ev.code == static_cast<uint8_t>(proto::EventKind::kControlChanged)) {
          apply(g_owner.onUnoOwner(static_cast<proto::Owner>(ev.a)), now);
        }
        broadcast(core::jsonUnoEvent(g_json, sizeof(g_json), ++g_ev_seq, tankMs(), ev.code, ev.a,
                                     ev.b));
        if (ev.code != static_cast<uint8_t>(proto::EventKind::kButtonPress) &&
            ev.code != static_cast<uint8_t>(proto::EventKind::kUnblocked)) {  // Plan O: not the noisy ones
          g_log.add(core::LogKind::kUno, ev.code, ev.a, "", tankMs(), now);
        }
        if (ev.code == static_cast<uint8_t>(proto::EventKind::kButtonPress)) {
          onButtonPress(now);  // Plan M: confirm a waiting change, or the WiFi double press
        } else if (ev.code == static_cast<uint8_t>(proto::EventKind::kStall)) {
          endMotion(core::MotionEvent::kBodyStall, now);  // Plan K: ends a move (turn / rate unchanged)
        } else if (ev.code == static_cast<uint8_t>(proto::EventKind::kEncoderFault)) {
          endMotion(core::MotionEvent::kBodyEncoderFault, now);
        }
        break;
      case core::LinkEventKind::kStopResult:
        for (size_t i = 0; i < g_stop_waiter_n; ++i) {
          const Waiter& w = g_stop_waiters[i];
          if (!g_ws.contains(w.fd)) continue;
          sendTo(w.fd, ev.status == 0 ? core::jsonAck(g_json, sizeof(g_json), w.id)
                                      : core::jsonNackStatus(g_json, sizeof(g_json), w.id,
                                                             ev.status));
        }
        g_stop_waiter_n = 0;
        break;
      case core::LinkEventKind::kAckResult:
        if (g_ops.isCurrent(ev.seq)) {  // Plan M: a settings / feedback op answered
          core::AckedOp op;
          const bool have = g_ops.front(op);
          if (have && ev.status == 0 && op.kind == core::AckedOp::kSettingGet) break;  // review I5: wait for its VALUE
          if (have && ev.status == 0 && op.kind == core::AckedOp::kSelfTest && op.fd >= 0 && g_ws.contains(op.fd)) {
            sendTo(op.fd, core::jsonAck(g_json, sizeof(g_json), op.id));  // Plan O: the result follows as EVENT SELFTEST
          }
          if (have && ev.status != 0 && op.fd >= 0 && g_ws.contains(op.fd)) {
            sendTo(op.fd, core::jsonNackStatus(g_json, sizeof(g_json), op.id, ev.status));
          }
          g_ops.done();
          break;
        }
        if (ev.seq == g_cam_seq && g_ws.contains(g_cam_waiter.fd)) {
          sendTo(g_cam_waiter.fd,
                 ev.status == 0
                     ? core::jsonAck(g_json, sizeof(g_json), g_cam_waiter.id)
                     : core::jsonNackStatus(g_json, sizeof(g_json), g_cam_waiter.id, ev.status));
        }
        g_cam_waiter = Waiter{-1, -1};
        break;
      case core::LinkEventKind::kSettingValue: {  // Plan M: SETTING_GET answer - completes the GET op (review I5)
        const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint16_t>(ev.b)) << 16) |
                                               static_cast<uint16_t>(ev.a));
        core::AckedOp op;
        if (g_ops.busy() && g_ops.front(op) && op.kind == core::AckedOp::kSettingGet && op.a == ev.code) {
          if (op.fd >= 0 && g_ws.contains(op.fd)) sendTo(op.fd, core::jsonSetting(g_json, sizeof(g_json), ev.code, v));
          g_ops.done();
        }
        break;
      }
      case core::LinkEventKind::kResetRequest:
      default:
        logf("uno: %s reset request ignored (bridge has no stored settings)",
             ev.code == static_cast<uint8_t>(proto::FrameType::kWifiResetReq) ? "WiFi" : "admin");
        break;
    }
  }
}

// LLD R5: td (every div-th TELEM_FAST) / tds (each new TELEM_SLOW) to the clients that asked for them.
void sendDebug(const core::TankSnapshot& s, bool fast, bool slow) {
  for (size_t i = 0; i < g_ws.count(); ++i) {
    const int fd = g_ws.fdAt(i);
    const uint8_t div = g_ws.debugDiv(fd);
    if (div == 0) continue;
    if (fast && s.fast_count % div == 0) sendTo(fd, core::jsonTd(g_json, sizeof(g_json), g_ws.nextDebugSeq(fd), s));
    if (slow) sendTo(fd, core::jsonTds(g_json, sizeof(g_json), s));  // g_json is shared: built per client
  }
}

void telemetry(uint32_t now) {
  const core::TankSnapshot& s = g_link.snapshot();
  const bool new_slow = s.slow_count != g_last_debug_slow;
  g_last_debug_slow = s.slow_count;
  const bool new_fast = s.fast_count != g_last_fast_count;
  if (new_fast || new_slow) sendDebug(s, new_fast, new_slow);
  if (new_fast) {
    g_last_fast_count = s.fast_count;
    int32_t roll = 0;  // §G6: learn the roll-on of the last move from the frames after its STOP
    if (g_roll.watching() &&
        g_roll.observe((s.fast.enc_l + s.fast.enc_r) / 2, s.fast.pwm_l != 0 || s.fast.pwm_r != 0, g_cpm, now, roll)) {
      const uint16_t next = core::rollAverage(g_roll_mm, roll);
      g_roll_last = roll;
      ++g_roll_read;
      logf("motion: roll-on %ld mm -> average %u mm", static_cast<long>(roll), static_cast<unsigned>(next));
      if (next != g_roll_mm && net::motionStoreSaveRoll(next)) g_roll_mm = next;
    }
    runMotion(g_motion.onTelemetry(telemOf(s), now), now);  // 50 Hz
    if (s.fast_count % 5u == 0) {  // Uno 50 Hz -> page 10 Hz (R2.14 #7, spec 02 §E4)
      const core::MotionService& svc = g_motion.service();
      const core::PrimitiveView pv{svc.active(), svc.kind(), svc.progressPct(), svc.limited()};
      broadcast(core::jsonTf(g_json, sizeof(g_json), ++g_tf_seq, s, pv));
    }
  }
  runMotion(g_motion.tick(now), now);  // telemetry gap / drive_rate deadman
  if (now - g_last_ts_ms >= 1000) {  // 1 Hz, also while the Uno is silent (body_offline)
    g_last_ts_ms = now;
    broadcastTs();
  }
}

void reapSilent(uint32_t now) {
  int fds[core::kMaxWsClients];
  const size_t n = g_ws.silent(now, fds, core::kMaxWsClients);
  for (size_t i = 0; i < n; ++i) {
    logf("ws: %s silent for 3 s - closing", g_ws.name(fds[i]));
    if (!wsClose(fds[i])) {  // kWsClose follows -> LinkOwner STOP if it was the driver
      LinkCommand gone{};    // review M6: a ghost (session already gone) - remove it here
      gone.kind = LinkCommand::Kind::kWsClose;
      gone.fd = fds[i];
      handleCommand(gone, now);
    }
  }
}

void publishStatus() {
  LinkStatus s;
  s.state = g_owner.state();
  s.ota_active = g_ota_active;
  s.ota_allowed = g_owner.otaAllowed() && !g_ota_active;
  s.synced = g_link.synced();
  s.hello = g_link.hello();
  snprintf(s.driver, sizeof(s.driver), "%s", ownerView().client);
  s.ws_clients = static_cast<uint8_t>(g_ws.count());
  s.frames_tx = g_link.stats().frames_tx;
  s.frames_rx = g_link.stats().frames_rx;
  s.bad_crc = g_link.parserStats().bad_crc;
  s.tx_dropped = g_tx.dropped();
  portENTER_CRITICAL(&g_status_mux);
  g_status = s;
  portEXIT_CRITICAL(&g_status_mux);
}

void linkTask(void*) {
  uint8_t rx[256];
  uint32_t last_publish = 0;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    size_t n = 0;
    while ((n = g_uart.read(rx, sizeof(rx))) != 0) {
      g_link.onUartBytes(rx, n, now);  // snapshot, events, STOP ACKs
    }
    LinkCommand cmd;
    while (xQueueReceive(g_queue, &cmd, 0) == pdTRUE) handleCommand(cmd, now);
    // Only a NEW slow frame updates the owner (review I1): re-applying the 1 Hz frame's older owner
    // every tick would undo an IR takeover reported by EVENT control_changed.
    if (g_link.snapshot().slow_count != g_last_slow_count) {
      g_last_slow_count = g_link.snapshot().slow_count;
      apply(g_owner.onUnoOwner(static_cast<proto::Owner>(g_link.snapshot().slow.owner)), now);
    }
    apply(g_owner.tick(now), now);
    reapSilent(now);
    g_link.step(now);
    handleLinkEvents(now);
    telemetry(now);
    settingsTick(now);  // Plan M
    drainTx();
    if (now - last_publish >= 100) {
      last_publish = now;
      publishStatus();
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(board::kLinkTickMs));
  }
}

}  // namespace

void linkTaskStart(const char* (*camera_sensor)()) {
  g_sensor = camera_sensor;
  g_cpm = net::motionStoreLoad();  // Plan K: before the link task runs
  g_roll_mm = net::motionStoreLoadRoll();
  net::appAiUrlLoad(g_ai_url, sizeof(g_ai_url));  // Plan O
  g_log.add(core::LogKind::kBoot, static_cast<int32_t>(esp_reset_reason()), 0, "", 0, millis());
  g_uart.begin();
  g_queue = xQueueCreate(16, sizeof(LinkCommand));
  xTaskCreatePinnedToCore(linkTask, "link", 8192, nullptr, 5, nullptr, 1);
  logf("uno link: Serial1 RX %d TX %d %lu baud", board::kUnoRxPin, board::kUnoTxPin,
       static_cast<unsigned long>(board::kUnoBaud));
}

bool linkPost(const LinkCommand& cmd, bool to_front) {
  if (g_queue == nullptr) return false;
  const TickType_t wait = pdMS_TO_TICKS(100);  // drained every 2 ms: waits only if 16 are queued
  return (to_front ? xQueueSendToFront(g_queue, &cmd, wait)
                   : xQueueSendToBack(g_queue, &cmd, wait)) == pdTRUE;
}

LinkStatus linkStatus() {
  portENTER_CRITICAL(&g_status_mux);
  const LinkStatus s = g_status;
  portEXIT_CRITICAL(&g_status_mux);
  return s;
}

}  // namespace net
}  // namespace cq
