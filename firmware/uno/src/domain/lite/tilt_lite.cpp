#include "domain/lite/tilt_lite.h"

#include "domain/lite/int_math.h"

#ifdef CQ_HOST_TEST
#define PROGMEM
#define CQ_READ_WORD_P(p) (*(p))
#else
#include <avr/pgmspace.h>
#define CQ_READ_WORD_P(p) pgm_read_word(p)
#endif

namespace cq {

namespace {
// tan²(5°·i) × 256 for i = 0..13 (0°..65°); in flash (coding standard §2.2), not RAM.
const uint16_t kTan2Q8[14] PROGMEM = {0, 2, 8, 18, 34, 56, 85, 126, 180, 256, 364, 522, 768, 1177};
uint16_t tan2Q8(uint8_t deg) {
  uint8_t i = static_cast<uint8_t>(deg / 5);
  if (i > 13) i = 13;
  return CQ_READ_WORD_P(&kTan2Q8[i]);
}
}  // namespace

int16_t TiltLite::pitchCdeg() const {  // 128 per g after / 64
  return iatan2Cdeg(static_cast<int16_t>(m_fx / 64),
                    static_cast<int16_t>(ihypot16(static_cast<int16_t>(m_fy / 64), static_cast<int16_t>(m_fz / 64))));
}

int16_t TiltLite::rollCdeg() const { return iatan2Cdeg(static_cast<int16_t>(m_fy / 64), static_cast<int16_t>(m_fz / 64)); }

bool TiltLite::over(uint8_t deg) const {
  if (m_fz <= 0) return true;  // past 90° or upside down
  const int32_t x = m_fx / 64, y = m_fy / 64, z = m_fz / 64;  // 128 per g
  const uint32_t h2 = static_cast<uint32_t>(x * x + y * y);
  const uint32_t v2 = static_cast<uint32_t>(z * z);
  return h2 * 256u > v2 * tan2Q8(deg);
}

TiltLite::Change TiltLite::update(int16_t ax, int16_t ay, int16_t az, uint8_t limit_deg, uint32_t now_ms) {
  // int32: on the AVR int is 16-bit and a saturated sample minus an opposite-sign filter value would wrap.
  m_fx = static_cast<int16_t>(m_fx + (static_cast<int32_t>(ax) - m_fx) / 8);
  m_fy = static_cast<int16_t>(m_fy + (static_cast<int32_t>(ay) - m_fy) / 8);
  m_fz = static_cast<int16_t>(m_fz + (static_cast<int32_t>(az) - m_fz) / 8);
  if (!m_tilted) {
    if (over(limit_deg)) {
      if (!m_over) {
        m_over = true;
        m_over_since_ms = now_ms;
      }
      if (static_cast<uint32_t>(now_ms - m_over_since_ms) >= kTiltHoldMs) {
        m_tilted = true;
        m_over = false;
        return Change::kTilted;
      }
    } else {
      m_over = false;
    }
  } else if (!over(static_cast<uint8_t>(limit_deg > 5 ? limit_deg - 5 : 0))) {
    m_tilted = false;
    return Change::kLevel;
  }
  const int16_t a = m_fz < 0 ? static_cast<int16_t>(-m_fz) : m_fz;
  const bool cond = m_lifted ? (m_fz > 6963 && m_fz < 9421) : (a < 4915);
  if (cond != m_cond) {
    m_cond = cond;
    m_cond_since_ms = now_ms;
  }
  if (!cond) return Change::kNone;
  const uint32_t hold_ms = m_lifted ? 500u : 60u;
  if (static_cast<uint32_t>(now_ms - m_cond_since_ms) < hold_ms) return Change::kNone;
  m_lifted = !m_lifted;
  m_cond = false;
  return m_lifted ? Change::kLifted : Change::kLanded;
}

}  // namespace cq
