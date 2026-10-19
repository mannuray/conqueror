// The ESP32 side of the body link when the ESP32 is the master (bridge LLD R2.6-R2.8).
// Master: HEARTBEAT every 100 ms; HELLO_REQ at start and every 1 s until a HELLO with
// proto == kProtocolVersion, then HELLO_ACK + OWNER_HINT (no ENVELOPE - R2.14 #5). STOP is resent
// every 50 ms until the Uno's ACK (max 10 sends). Uno frames are always parsed (also while not
// master) to keep the snapshot, HELLO info and events for the web clients.
// It never originates DRIVE/CAM_JOY: those are sent 1:1 for client messages. Pure C++.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/frame_parser.h"
#include "core/frame_tx.h"
#include "cq_protocol.h"

namespace cq {
namespace core {

constexpr uint32_t kHeartbeatMs = 100;
constexpr uint32_t kHelloRetryMs = 1000;
constexpr uint32_t kUnoSilentMs = 1000;  // synced Uno silent this long -> re-handshake
constexpr uint32_t kStopResendMs = 50;
constexpr uint8_t kStopMaxSends = 10;
constexpr uint32_t kAckTimeoutMs = 200;
constexpr uint8_t kStatusTimeout = 0xFF;  // LinkEvent.status: no ACK in time

enum class HandshakeState : uint8_t { kIdle, kWaitHello, kSynced, kIncompatible };

struct HelloInfo {
  bool seen = false;
  uint16_t fw = 0;
  uint8_t proto = 0;
  uint8_t caps = 0;
  uint8_t reset_reason = 0;
  uint32_t commit = 0;
};

// Plan O (bridge LLD R13): the last EVENT SELFTEST (a = failed-check bitmask, b = critical lock 0 / 1).
struct SelfTestRecord {
  bool seen = false;
  uint8_t mask = 0;
  bool locked = false;
  uint32_t at_ms = 0;  // bridge time it arrived
};

struct TankSnapshot {
  bool fast_valid = false;
  bool slow_valid = false;
  uint32_t fast_count = 0;  // TELEM_FAST frames seen (50 Hz; tf is sent for every 5th)
  uint32_t slow_count = 0;  // TELEM_SLOW frames seen (owner is applied once per new frame)
  proto::TelemFastMsg fast{};
  proto::TelemSlowMsg slow{};
};

enum class LinkEventKind : uint8_t {
  kUnoEvent,      // EVENT frame: code, a, b
  kStopResult,    // seq, status (ack_status index or kStatusTimeout)
  kAckResult,     // CAM_HOME / SOUND / AUDIO_MODE / CAM_LOCK: seq, status
  kSettingValue,  // Plan M: SETTING_VALUE - code = id, a / b = value low / high 16 bits
  kBodyOnline,    // HELLO accepted
  kBodyOffline,   // code 0 = silent, 1 = incompatible protocol (a = Uno proto)
  kResetRequest,  // code = frame type (WIFI/ADMIN_RESET_REQ), answered UNAVAILABLE
};

struct LinkEvent {
  LinkEventKind kind;
  uint8_t code;
  uint8_t seq;
  uint8_t status;
  int16_t a;
  int16_t b;
};

struct LinkStats {
  uint32_t frames_tx = 0;
  uint32_t frames_rx = 0;
};

class BodyLink {
 public:
  explicit BodyLink(FrameTx& tx) : m_tx(tx) {}

  void startMaster(uint32_t now_ms);  // HEARTBEAT + HELLO_REQ now
  void stopMaster();                  // no more heartbeats; snapshot/events keep flowing
  bool master() const { return m_master; }
  bool synced() const { return m_master && m_hs == HandshakeState::kSynced; }
  HandshakeState handshake() const { return m_hs; }

  void step(uint32_t now_ms);
  void onUartBytes(const uint8_t* data, size_t len, uint32_t now_ms);

  // Client commands (master + synced only; false = not sent).
  bool sendDrive(int8_t v, int8_t w, proto::Source src = proto::Source::kHuman);  // AUTO = motion service
  bool sendCamJoy(int8_t x, int8_t y, bool active);
  bool sendCamHome(uint32_t now_ms, uint8_t& seq_out);
  // Plan F: share the one acked slot with CAM_HOME (kAckResult on ACK or after kAckTimeoutMs).
  bool sendSound(uint8_t sound_id, uint32_t now_ms, uint8_t& seq_out);
  bool sendCamLock(bool on, uint32_t now_ms, uint8_t& seq_out);
  bool sendAudioMode(uint8_t mode, uint32_t now_ms, uint8_t& seq_out);  // R3.1: proto::AudioModeValue
  // Plan M: the settings page and the WiFi switch feedback (same acked slot, one at a time).
  bool sendLed(uint8_t mode, uint8_t r, uint8_t g, uint8_t b, uint16_t period_ms, uint16_t duration_ms, uint32_t now_ms,
               uint8_t& seq_out);
  bool sendSettingGet(uint8_t id, uint32_t now_ms, uint8_t& seq_out);
  bool sendSettingSet(uint8_t id, int32_t value, uint32_t now_ms, uint8_t& seq_out);
  bool sendSettingsSave(uint32_t now_ms, uint8_t& seq_out);
  bool sendCalibrate(uint8_t op, uint8_t script, uint32_t now_ms, uint8_t& seq_out);  // Plan O: SELF_TEST only
  // Plan N3 (Tank Logo): PLAY (≤ 10 notes per frame; a new TONE replaces the tune playing) and LOOK.
  bool sendTone(const uint16_t* hz, const uint16_t* ms, uint8_t n, uint32_t now_ms, uint8_t& seq_out);
  bool sendCamSet(int16_t pan_dd, int16_t tilt_dd, uint32_t now_ms, uint8_t& seq_out);  // servo angles, 0.1°
  bool ackPending() const { return m_cam_pending; }
  void setOwnerHint(proto::OwnerHintValue hint);  // remembered; sent now if synced, and after HELLO

  // STOP {src HUMAN} with the next own seq, master or not. Uses the urgent slot (fast path: ahead of queued frames, never mid-frame; queued
  // DRIVE/CAM_JOY are purged) and are resent until the ACK. Returns the seq used.
  uint8_t sendStop(uint32_t now_ms);

  bool popEvent(LinkEvent& ev);
  const HelloInfo& hello() const { return m_hello; }
  const SelfTestRecord& selfTest() const { return m_self_test; }
  const TankSnapshot& snapshot() const { return m_snap; }
  const ParserStats& parserStats() const { return m_parser.stats(); }
  const LinkStats& stats() const { return m_stats; }
  uint32_t lastRxMs() const { return m_last_rx_ms; }

 private:
  enum class Lane : uint8_t { kQueue, kMotion, kUrgent };  // kMotion: purged by a later STOP
  template <class Msg>
  bool emit(const Msg& m, uint8_t seq, Lane lane);
  // One acked command at a time (CAM_HOME, SOUND, AUDIO_MODE, CAM_LOCK): its ACK or a timeout gives kAckResult.
  template <typename Msg>
  bool sendAcked(const Msg& m, uint32_t now_ms, uint8_t& seq_out) {
    if (!synced()) return false;
    seq_out = m_next_seq;
    if (!emitNext(m)) return false;
    m_cam_seq = seq_out;
    m_cam_pending = true;
    m_cam_sent_ms = now_ms;
    return true;
  }
  template <class Msg>
  bool emitNext(const Msg& m, Lane lane = Lane::kQueue) {
    return emit(m, m_next_seq++, lane);
  }
  void onFrame(const FrameView& f, uint32_t now_ms);
  void onHello(const proto::HelloMsg& h);
  void masterStep(uint32_t now_ms);
  void stopResend(uint32_t now_ms);
  void onAckFrame(const FrameView& f);
  void onTelemFrame(const FrameView& f);
  void onEventFrame(const FrameView& f, uint32_t now_ms);
  void onSettingValueFrame(const FrameView& f);
  void onResetRequest(const FrameView& f);
  void pushEvent(const LinkEvent& ev);
  void sendStopFrame();

  FrameTx& m_tx;
  FrameParser m_parser;
  bool m_master = false;
  HandshakeState m_hs = HandshakeState::kIdle;
  uint8_t m_next_seq = 0;
  uint32_t m_last_hb_ms = 0;
  uint32_t m_last_req_ms = 0;
  uint32_t m_last_rx_ms = 0;
  bool m_incompatible_reported = false;
  proto::OwnerHintValue m_hint = proto::OwnerHintValue::kNone;
  uint8_t m_stop_seq = 0;
  uint8_t m_stop_sends = 0;
  uint32_t m_stop_last_ms = 0;
  uint8_t m_cam_seq = 0;
  bool m_cam_pending = false;
  uint32_t m_cam_sent_ms = 0;
  HelloInfo m_hello;
  SelfTestRecord m_self_test;
  TankSnapshot m_snap;
  LinkStats m_stats;
  static constexpr size_t kEventSlots = 16;
  LinkEvent m_events[kEventSlots] = {};
  size_t m_ev_head = 0;
  size_t m_ev_count = 0;
};

}  // namespace core
}  // namespace cq
