#include "core/body_link.h"

namespace cq {
namespace core {

template <class Msg>
bool BodyLink::emit(const Msg& m, uint8_t seq, Lane lane) {
  uint8_t frame[proto::kMaxPayload + 5];
  const uint8_t n = proto::encode(m, frame + 4);
  if (n == proto::kEncodeError || n > proto::kMaxPayload) return false;
  frame[0] = proto::kSync;
  frame[1] = n;
  frame[2] = static_cast<uint8_t>(Msg::kType);
  frame[3] = seq;
  frame[4 + n] = proto::crc8(frame + 1, static_cast<uint8_t>(n + 3));
  const bool ok = lane == Lane::kUrgent ? m_tx.pushUrgent(frame, n + 5u)
                 : lane == Lane::kMotion ? m_tx.push(frame, n + 5u, ChunkClass::kMotion)
                                         : m_tx.push(frame, n + 5u);
  if (ok) ++m_stats.frames_tx;
  return ok;
}

void BodyLink::startMaster(uint32_t now_ms) {
  m_master = true;
  m_hs = HandshakeState::kWaitHello;
  m_incompatible_reported = false;
  m_last_hb_ms = now_ms;
  m_last_req_ms = now_ms;
  emitNext(proto::HeartbeatMsg{now_ms});
  emitNext(proto::HelloReqMsg{});
}

// A deliberate handover (bench connects, OTA): not a lost body, so no kBodyOffline (review M11).
// While the bench drives, link_task reports body health from the Uno's traffic (bodyOffline()).
void BodyLink::stopMaster() {
  m_master = false;
  m_hs = HandshakeState::kIdle;
  m_cam_pending = false;
}

void BodyLink::step(uint32_t now_ms) {
  if (m_master) {
    if (now_ms - m_last_hb_ms >= kHeartbeatMs) {
      m_last_hb_ms = now_ms;
      emitNext(proto::HeartbeatMsg{now_ms});
    }
    if (m_hs == HandshakeState::kSynced && now_ms - m_last_rx_ms >= kUnoSilentMs) {
      m_hs = HandshakeState::kWaitHello;  // Uno gone or rebooting: start over with HELLO_REQ
      pushEvent({LinkEventKind::kBodyOffline, 0, 0, 0, 0, 0});
      m_last_req_ms = now_ms - kHelloRetryMs;
    }
    if (m_hs != HandshakeState::kSynced && now_ms - m_last_req_ms >= kHelloRetryMs) {
      m_last_req_ms = now_ms;
      emitNext(proto::HelloReqMsg{});
    }
  }
  if (m_stop_sends != 0 && now_ms - m_stop_last_ms >= kStopResendMs) {
    if (m_stop_sends >= kStopMaxSends) {
      m_stop_sends = 0;
      pushEvent({LinkEventKind::kStopResult, 0, m_stop_seq, kStatusTimeout, 0, 0});
    } else {
      m_stop_last_ms = now_ms;
      sendStopFrame();
    }
  }
  if (m_cam_pending && now_ms - m_cam_sent_ms >= kAckTimeoutMs) {
    m_cam_pending = false;
    pushEvent({LinkEventKind::kAckResult, 0, m_cam_seq, kStatusTimeout, 0, 0});
  }
}

void BodyLink::onUartBytes(const uint8_t* data, size_t len, uint32_t now_ms) {
  m_parser.feed(data, len, [this, now_ms](const FrameView& f) { onFrame(f, now_ms); });
}

void BodyLink::onFrame(const FrameView& f, uint32_t now_ms) {
  m_last_rx_ms = now_ms;
  ++m_stats.frames_rx;
  switch (static_cast<proto::FrameType>(f.type)) {
    case proto::FrameType::kHello: {
      proto::HelloMsg h{};
      if (proto::decode(f.payload, f.len, h)) onHello(h);
      break;
    }
    case proto::FrameType::kAck: {
      proto::AckMsg a{};
      if (!proto::decode(f.payload, f.len, a)) break;
      if (m_stop_sends != 0 && a.ack_seq == m_stop_seq) {
        m_stop_sends = 0;
        pushEvent({LinkEventKind::kStopResult, 0, a.ack_seq, a.status, 0, 0});
      } else if (m_cam_pending && a.ack_seq == m_cam_seq) {
        m_cam_pending = false;
        pushEvent({LinkEventKind::kAckResult, 0, a.ack_seq, a.status, 0, 0});
      }
      break;
    }
    case proto::FrameType::kTelemFast:
      if (proto::decode(f.payload, f.len, m_snap.fast)) {
        m_snap.fast_valid = true;
        ++m_snap.fast_count;
      }
      break;
    case proto::FrameType::kTelemSlow:
      if (proto::decode(f.payload, f.len, m_snap.slow)) {
        m_snap.slow_valid = true;
        ++m_snap.slow_count;
      }
      break;
    case proto::FrameType::kEvent: {
      proto::EventMsg e{};
      if (proto::decode(f.payload, f.len, e)) {
        pushEvent({LinkEventKind::kUnoEvent, e.kind, f.seq, 0, e.a, e.b});
      }
      break;
    }
    case proto::FrameType::kWifiResetReq:
    case proto::FrameType::kAdminResetReq:
      if (m_master) {
        const proto::EspAckMsg ack{f.seq, static_cast<uint8_t>(proto::AckStatus::kUnavailable)};
        emitNext(ack);
        pushEvent({LinkEventKind::kResetRequest, f.type, f.seq, 0, 0, 0});
      }
      break;
    default:
      break;  // SETTING_VALUE, CAL_*: only the bench uses them (passthrough)
  }
}

void BodyLink::onHello(const proto::HelloMsg& h) {
  m_hello = HelloInfo{true, h.fw, h.proto, h.caps, h.reset_reason, h.commit};
  if (!m_master) return;  // bench is master: it answers its own HELLO
  if (h.proto != proto::kProtocolVersion) {
    m_hs = HandshakeState::kIncompatible;
    if (!m_incompatible_reported) {
      m_incompatible_reported = true;
      pushEvent({LinkEventKind::kBodyOffline, 1, 0, 0, h.proto, 0});
    }
    return;
  }
  const bool was_synced = m_hs == HandshakeState::kSynced;
  emitNext(proto::HelloAckMsg{proto::kProtocolVersion});
  emitNext(proto::OwnerHintMsg{static_cast<uint8_t>(m_hint)});
  m_hs = HandshakeState::kSynced;
  m_incompatible_reported = false;
  if (!was_synced) pushEvent({LinkEventKind::kBodyOnline, 0, 0, 0, 0, 0});
}

bool BodyLink::sendDrive(int8_t v, int8_t w) {
  if (!synced()) return false;
  return emitNext(proto::DriveMsg{static_cast<uint8_t>(proto::Source::kHuman), v, w}, Lane::kMotion);
}

bool BodyLink::sendCamJoy(int8_t x, int8_t y, bool active) {
  if (!synced()) return false;
  return emitNext(proto::CamJoyMsg{x, y, static_cast<uint8_t>(active ? 1 : 0)}, Lane::kMotion);
}

bool BodyLink::sendCamHome(uint32_t now_ms, uint8_t& seq_out) { return sendAcked(proto::CamHomeMsg{}, now_ms, seq_out); }

bool BodyLink::sendSound(uint8_t sound_id, uint32_t now_ms, uint8_t& seq_out) {
  return sendAcked(proto::SoundMsg{sound_id}, now_ms, seq_out);
}

bool BodyLink::sendCamLock(bool on, uint32_t now_ms, uint8_t& seq_out) {
  return sendAcked(proto::CamLockMsg{static_cast<uint8_t>(on ? 1 : 0)}, now_ms, seq_out);
}

bool BodyLink::sendAudioMode(uint8_t mode, uint32_t now_ms, uint8_t& seq_out) {
  return sendAcked(proto::AudioModeMsg{mode}, now_ms, seq_out);
}

void BodyLink::setOwnerHint(proto::OwnerHintValue hint) {
  m_hint = hint;
  if (synced()) emitNext(proto::OwnerHintMsg{static_cast<uint8_t>(hint)});
}

uint8_t BodyLink::sendStop(uint32_t now_ms) {
  const uint8_t seq = m_next_seq++;
  injectStop(seq, now_ms);
  return seq;
}

void BodyLink::injectStop(uint8_t seq, uint32_t now_ms) {
  m_stop_seq = seq;
  m_stop_sends = 0;
  m_stop_last_ms = now_ms;
  sendStopFrame();
}

void BodyLink::sendStopFrame() {
  // STOP is accepted by the Uno from any source in any state, so it goes out even before sync.
  // A full urgent slot (previous STOP still being written) also counts as an attempt.
  emit(proto::StopMsg{static_cast<uint8_t>(proto::Source::kHuman)}, m_stop_seq, Lane::kUrgent);
  ++m_stop_sends;
}

void BodyLink::pushEvent(const LinkEvent& ev) {
  if (m_ev_count == kEventSlots) {  // full: drop the oldest
    m_ev_head = (m_ev_head + 1) % kEventSlots;
    --m_ev_count;
  }
  m_events[(m_ev_head + m_ev_count) % kEventSlots] = ev;
  ++m_ev_count;
}

bool BodyLink::popEvent(LinkEvent& ev) {
  if (m_ev_count == 0) return false;
  ev = m_events[m_ev_head];
  m_ev_head = (m_ev_head + 1) % kEventSlots;
  --m_ev_count;
  return true;
}

}  // namespace core
}  // namespace cq
