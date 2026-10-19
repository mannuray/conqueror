#include "app/uno_channel.h"

namespace cq {
namespace app {

void UnoChannel::waitStop(int fd, int32_t id) {
  if (!m_out.clients().contains(fd)) return;
  if (m_stop_n < kMaxStopWaiters) {
    m_stop_waiters[m_stop_n++] = Waiter{fd, id};
  } else {
    m_out.nack(fd, id, core::BridgeReason::kBusy);  // review M8: answered; the STOP was still applied
  }
}

void UnoChannel::onStopResult(uint8_t status) {
  for (size_t i = 0; i < m_stop_n; ++i) {
    const Waiter& w = m_stop_waiters[i];
    if (status == 0) {
      m_out.ack(w.fd, w.id);
    } else {
      m_out.nackStatus(w.fd, w.id, status);
    }
  }
  m_stop_n = 0;
}

void UnoChannel::onAck(uint8_t seq, uint8_t status) {
  core::AckedOp op;
  if (!m_ops.isCurrent(seq) || !m_ops.front(op)) return;
  if (status == 0 && op.kind == core::AckedOp::kSettingGet) return;  // review I5: answered by its VALUE
  if (op.fd >= 0 && status == 0) m_out.ack(op.fd, op.id);  // the self-test result follows as EVENT SELFTEST
  if (op.fd >= 0 && status != 0) m_out.nackStatus(op.fd, op.id, status);
  m_ops.done();
}

void UnoChannel::onSettingValue(uint8_t id, int32_t value) {
  core::AckedOp op;
  if (!m_ops.busy() || !m_ops.front(op) || op.kind != core::AckedOp::kSettingGet || op.a != id) return;
  if (op.fd >= 0) m_out.to(op.fd, core::jsonSetting(m_out.buf(), m_out.cap(), id, value));
  m_ops.done();
}

void UnoChannel::answerAll(core::BridgeReason r) {
  core::AckedOp op;
  while (m_ops.front(op)) {
    if (op.fd >= 0) m_out.nack(op.fd, op.id, r);
    m_ops.done();
  }
}

void UnoChannel::onBodyOffline() { answerAll(core::BridgeReason::kBodyOffline); }

bool UnoChannel::send(const core::AckedOp& op, uint32_t now, uint8_t& seq) {
  switch (op.kind) {
    case core::AckedOp::kSound: return m_link.sendSound(op.a, now, seq);
    case core::AckedOp::kLed: return m_link.sendLed(op.a, op.r, op.g, op.b, op.period_ms, op.duration_ms, now, seq);
    case core::AckedOp::kSettingGet: return m_link.sendSettingGet(op.a, now, seq);
    case core::AckedOp::kSettingSet: return m_link.sendSettingSet(op.a, op.value, now, seq);
    case core::AckedOp::kSettingsSave: return m_link.sendSettingsSave(now, seq);
    case core::AckedOp::kSelfTest:
      return m_link.sendCalibrate(static_cast<uint8_t>(proto::CalOp::kStart),
                                  static_cast<uint8_t>(proto::CalScript::kSelfTest), now, seq);
    case core::AckedOp::kCamHome: return m_link.sendCamHome(now, seq);
    case core::AckedOp::kCamLock: return m_link.sendCamLock(op.a != 0, now, seq);
    case core::AckedOp::kAudioMode: return m_link.sendAudioMode(op.a, now, seq);
  }
  return false;
}

void UnoChannel::backstop(uint32_t now) {  // the slot was reset without an answer: never stuck
  core::AckedOp op;
  if (!m_ops.busy() || !m_ops.timedOut(now)) return;
  if (m_ops.front(op) && op.fd >= 0) m_out.nack(op.fd, op.id, core::BridgeReason::kTimeout);
  m_ops.done();
}

void UnoChannel::tick(uint32_t now) {
  backstop(now);
  core::AckedOp op;
  if (m_ops.busy() || m_link.ackPending() || !m_ops.front(op)) return;
  uint8_t seq = 0;
  if (send(op, now, seq)) {
    m_ops.markSent(seq, now);
  } else if (!m_link.synced()) {  // body offline: answer and drop it
    if (op.fd >= 0) m_out.nack(op.fd, op.id, core::BridgeReason::kBodyOffline);
    m_ops.done();
  }  // synced but the frame queue is full: next tick
}

void UnoChannel::stopForOta() {
  m_link.stopMaster();
  m_tx.clear();  // drop queued ESP32 frames; a pending STOP (urgent slot) is kept
  answerAll(core::BridgeReason::kBusy);
  for (size_t i = 0; i < m_stop_n; ++i) m_out.nack(m_stop_waiters[i].fd, m_stop_waiters[i].id, core::BridgeReason::kTimeout);
  m_stop_n = 0;
}

}  // namespace app
}  // namespace cq
