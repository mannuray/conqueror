#include "domain/lite/heading_hold_lite.h"

namespace cq {

HeadingGains headingGainsFromMilli(int32_t heading_kp_milli, int32_t rate_kp_milli) {
  return HeadingGains{static_cast<int16_t>(heading_kp_milli * 9 / 20), static_cast<int16_t>(rate_kp_milli * 256 / 1000)};
}

void HeadingHoldLite::reset() {
  m_state = State::kIdle;
  m_settle = 0;
  m_err = 0;
  m_i = 0;
}

void HeadingHoldLite::teleop(int8_t v, int8_t w, bool gyro_ok, int32_t yaw_cdeg, int16_t rate_ddps, int8_t& w_steer,
                             int8_t& w_ctrl) {
  w_steer = 0;
  w_ctrl = 0;
  if (!gyro_ok) {
    reset();
    w_steer = w;
    return;
  }
  if (v == 0 && w == 0) {
    reset();
    return;
  }
  if (w != 0) {
    m_state = State::kSteering;
    m_err = 0;
    m_i = 0;
    w_steer = w;
    return;
  }
  if (m_state != State::kHolding) {
    if (m_state != State::kSettling) {
      m_state = State::kSettling;
      m_settle = 0;
    }
    const int16_t r = rate_ddps < 0 ? static_cast<int16_t>(-rate_ddps) : rate_ddps;
    if (r > kCaptureDdps && m_settle < kSettleTicks) {
      ++m_settle;
      return;
    }
    m_held_cdeg = yaw_cdeg;
    m_i = 0;
    m_state = State::kHolding;
  }
  int32_t err = m_held_cdeg - yaw_cdeg;  // short way round: −18000..17999
  if (err >= 18000) err -= 36000;
  if (err < -18000) err += 36000;
  m_err = err;
  const int32_t p = static_cast<int32_t>(m_g.kp_q8) * err / 25600;
  if (!m_i_frozen && p < 100 && p > -100) m_i += err;  // review M2: frozen when held, no wind-up while P saturates
  if (m_g.kp_q8 > 0) {
    const int32_t lim = kIMaxPct * 25600L * kIDivTicks / m_g.kp_q8;  // |Ki · m_i| <= 15 %
    if (m_i > lim) m_i = lim;
    if (m_i < -lim) m_i = -lim;
  } else {
    m_i = 0;
  }
  int32_t out = p +
                static_cast<int32_t>(m_g.kp_q8) * m_i / (25600L * kIDivTicks) -
                static_cast<int32_t>(m_g.kd_q8) * rate_ddps / 2560;
  if (out > 100) out = 100;
  if (out < -100) out = -100;
  w_ctrl = static_cast<int8_t>(out);
}

}  // namespace cq
