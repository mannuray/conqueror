#include "core/slip_watch.h"

#include "core/motion_config.h"

namespace cq {
namespace core {
namespace {
constexpr float kDegPerRad = 57.29578f;
float absF(float v) { return v < 0.0f ? -v : v; }
}  // namespace

void SlipWatch::startWindow(int32_t enc_l, int32_t enc_r, uint32_t now_ms) {
  m_started = true;
  m_win_ms = now_ms;
  m_l0 = enc_l;
  m_r0 = enc_r;
  m_rate_sum = 0;
  m_rate_n = 0;
}

bool SlipWatch::step(int32_t enc_l, int32_t enc_r, int16_t rate_ddps, uint16_t cpm, uint32_t now_ms) {
  if (!m_started || cpm == 0) {
    startWindow(enc_l, enc_r, now_ms);
    return false;
  }
  m_rate_sum += rate_ddps;
  ++m_rate_n;
  const uint32_t dt = now_ms - m_win_ms;
  if (dt < motion::kSlipWindowMs) return false;
  if (dt > 2 * motion::kSlipWindowMs) {  // frames were missing: start again, never judge one long window
    m_slipping = false;
    startWindow(enc_l, enc_r, now_ms);
    return false;
  }
  // The turn the tracks say (0.1 deg/s): the track speed difference in cm/s over the track width.
  const float s = 100.0f * 1000.0f / static_cast<float>(cpm) / static_cast<float>(dt);
  const float diff_cm_s = static_cast<float>((enc_r - m_r0) - (enc_l - m_l0)) * s;
  const float expected = diff_cm_s / static_cast<float>(motion::kTrackWidthCm) * kDegPerRad * 10.0f;
  const float measured = static_cast<float>(m_rate_sum) / static_cast<float>(m_rate_n);
  const bool slip = absF(expected) >= motion::kSlipExpectedDdps && absF(measured) * 3.0f < absF(expected);
  if (!slip) {
    m_slipping = false;
  } else if (!m_slipping) {
    m_slipping = true;
    m_slip_since = m_win_ms;  // the window it began in
  }
  startWindow(enc_l, enc_r, now_ms);
  return m_slipping && now_ms - m_slip_since >= motion::kSlipHoldMs;
}

}  // namespace core
}  // namespace cq
