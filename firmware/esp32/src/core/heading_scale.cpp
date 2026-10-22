#include "core/heading_scale.h"

namespace cq {
namespace core {
namespace {
constexpr int32_t kTurnE4 = 36000 * 10000;  // one full turn, 3.6e8 < 2^31
}  // namespace

bool HeadingScale::setScale(uint16_t scale_e4) {
  if (scale_e4 < kGyroScaleMin || scale_e4 > kGyroScaleMax) return false;
  m_scale = scale_e4;
  return true;
}

void HeadingScale::apply(uint16_t yaw_raw, int16_t rate_raw, uint16_t& yaw, int16_t& rate) {
  if (!m_anchored) {
    m_anchored = true;
    m_sum_e4 = static_cast<int32_t>(yaw_raw % 36000) * 10000;
  } else {
    int32_t d = static_cast<int32_t>(yaw_raw) - m_last_raw;  // the short way: -180..180°
    if (d > 18000) d -= 36000;
    if (d < -18000) d += 36000;
    m_sum_e4 += d * m_scale;  // |d| <= 18000, scale <= 11000: < 2e8
    if (m_sum_e4 >= kTurnE4) m_sum_e4 -= kTurnE4;
    if (m_sum_e4 < 0) m_sum_e4 += kTurnE4;
  }
  m_last_raw = yaw_raw;
  yaw = static_cast<uint16_t>(((m_sum_e4 + 5000) / 10000) % 36000);
  const int32_t r = static_cast<int32_t>(rate_raw) * m_scale;
  int32_t v = r >= 0 ? (r + 5000) / 10000 : -((-r + 5000) / 10000);
  if (v > 32767) v = 32767;
  if (v < -32767) v = -32767;
  rate = static_cast<int16_t>(v);
}

}  // namespace core
}  // namespace cq
