// One acked Uno command at a time (Plan M, bridge LLD R9.2, R15.5): every request that waits for the Uno's ACK -
// feedback, settings, self-test and the drive page's camera / sound / audio - in one queue. Pure C++.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

constexpr int kLogoClient = -2;  // Plan N3: the running Logo program waits for this answer (UnoChannel's logo sink)

struct AckedOp {
  // kSelfTest: Plan O; kCamHome / kCamLock / kAudioMode: the drive page's requests (Plan P, LLD R15.5)
  enum Kind : uint8_t { kSound, kLed, kSettingGet, kSettingSet, kSettingsSave, kSelfTest, kCamHome, kCamLock, kAudioMode,
                        kTone, kCamSet };  // Plan N3: PLAY / LOOK of a Logo program
  Kind kind = kSound;
  uint8_t a = 0;      // sound id / setting id / led mode / camera lock on / audio mode
  int32_t value = 0;  // setting value
  uint8_t r = 0, g = 0, b = 0;
  uint16_t period_ms = 0, duration_ms = 0;
  int16_t pan_dd = 0, tilt_dd = 0;  // kCamSet
  uint8_t n_notes = 0;              // kTone
  uint16_t hz[10] = {};
  uint16_t ms[10] = {};
  int fd = -1;        // client to answer, -1 = none (feedback)
  int32_t id = -1;
};

class AckedOps {
 public:
  static constexpr uint8_t kCap = 8;
  static constexpr uint32_t kTimeoutMs = 600;  // review I1: the answer never came (lost frame)
  bool push(const AckedOp& op);
  bool front(AckedOp& out) const;
  bool empty() const { return m_n == 0; }
  uint8_t freeSlots() const { return static_cast<uint8_t>(kCap - m_n); }
  bool busy() const { return m_sent; }
  void markSent(uint8_t seq, uint32_t now_ms) {
    m_seq = seq;
    m_sent = true;
    m_sent_ms = now_ms;
  }
  bool isCurrent(uint8_t seq) const { return m_sent && seq == m_seq; }
  bool timedOut(uint32_t now_ms) const { return m_sent && now_ms - m_sent_ms >= kTimeoutMs; }
  void done();  // the current op answered (ACK / timeout): drop it, next may go

 private:
  AckedOp m_ops[kCap];
  uint8_t m_head = 0;
  uint8_t m_n = 0;
  bool m_sent = false;
  uint8_t m_seq = 0;
  uint32_t m_sent_ms = 0;
};

}  // namespace core
}  // namespace cq
