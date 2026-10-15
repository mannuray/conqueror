#include "domain/lite/encoder_lite.h"

namespace cq {

void EncoderLite::update(int32_t count, uint32_t period_us, uint32_t last_edge_us, uint32_t now_us) {
  m_count = count;
  if (period_us == 0 || static_cast<uint32_t>(now_us - last_edge_us) > kStaleUs) {
    m_cps = 0;
    return;
  }
  const uint32_t cps = 1000000UL / period_us;
  m_cps = static_cast<uint16_t>(cps > 65535UL ? 65535UL : cps);
}

int8_t speedPctLite(uint16_t cps, int16_t pwm_sign_source, uint16_t max_cps, bool enabled) {
  if (!enabled || max_cps == 0) return -128;
  int32_t pct = static_cast<int32_t>(cps) * 100 / max_cps;
  if (pct > 127) pct = 127;
  return static_cast<int8_t>(pwm_sign_source < 0 ? -pct : pct);
}

}  // namespace cq
