// The ESP32 side of the body link when the ESP32 is the master (bridge LLD R2.6-R2.8).
// Master: HEARTBEAT every 100 ms; HELLO_REQ at start and every 1 s until a HELLO with
// proto == kProtocolVersion, then HELLO_ACK + OWNER_HINT (no ENVELOPE - R2.14 #5). STOP is resent
// every 50 ms until the Uno's ACK (max 10 sends). Uno frames are always parsed (also during TCP
// passthrough) to keep the snapshot, HELLO info and events for the web clients.
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

struct TankSnapshot {
  bool fast_valid = false;
  bool slow_valid = false;
  uint32_t fast_count = 0;  // TELEM_FAST frames seen (tf is sent for every 2nd)
  uint32_t slow_count = 0;  // TELEM_SLOW frames seen (owner is applied once per new frame)
  proto::TelemFastMsg fast{};
  proto::TelemSlowMsg slow{};
};

enum class LinkEventKind : uint8_t {
  kUnoEvent,      // EVENT frame: code, a, b
  kStopResult,    // seq, status (ack_status index or kStatusTimeout)
  kAckResult,     // CAM_HOME: seq, status
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
  bool sendDrive(int8_t v, int8_t w);
  bool sendCamJoy(int8_t x, int8_t y, bool active);
  bool sendCamHome(uint32_t now_ms, uint8_t& seq_out);
  // Plan F: share the one acked slot with CAM_HOME (kAckResult on ACK or after kAckTimeoutMs).
  bool sendSound(uint8_t sound_id, uint32_t now_ms, uint8_t& seq_out);
  bool sendCamLock(bool on, uint32_t now_ms, uint8_t& seq_out);
  void setOwnerHint(proto::OwnerHintValue hint);  // remembered; sent now if synced, and after HELLO

  // STOP {src HUMAN}: master -> own seq; passthrough -> injectStop with the caller's seq.
  // Both use the urgent slot (fast path: ahead of queued frames, never mid-frame; queued
  // DRIVE/CAM_JOY are purged) and are resent until the ACK. Returns the seq used.
  uint8_t sendStop(uint32_t now_ms);
  void injectStop(uint8_t seq, uint32_t now_ms);
  bool stopPending() const { return m_stop_sends != 0; }
  uint8_t pendingStopSeq() const { return m_stop_seq; }  // valid while stopPending()

  bool popEvent(LinkEvent& ev);
  const HelloInfo& hello() const { return m_hello; }
  const TankSnapshot& snapshot() const { return m_snap; }
  const ParserStats& parserStats() const { return m_parser.stats(); }
  const LinkStats& stats() const { return m_stats; }
  uint32_t lastRxMs() const { return m_last_rx_ms; }

 private:
  enum class Lane : uint8_t { kQueue, kMotion, kUrgent };  // kMotion: purged by a later STOP
  template <class Msg>
  bool emit(const Msg& m, uint8_t seq, Lane lane);
  // One acked command at a time (CAM_HOME, SOUND, CAM_LOCK): its ACK or a timeout gives kAckResult.
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
  TankSnapshot m_snap;
  LinkStats m_stats;
  static constexpr size_t kEventSlots = 16;
  LinkEvent m_events[kEventSlots] = {};
  size_t m_ev_head = 0;
  size_t m_ev_count = 0;
};

}  // namespace core
}  // namespace cq
