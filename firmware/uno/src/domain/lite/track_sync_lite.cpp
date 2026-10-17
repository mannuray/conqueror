#include "domain/lite/track_sync_lite.h"

namespace cq {
namespace {
// Moves v's magnitude down by d (d < 0: up), keeping its sign and |v| <= 100: the correction may stop a track,
// never reverse it or overdrive it (final review M1).
int16_t towardZero(int16_t v, int16_t d) {
  const int16_t mag = static_cast<int16_t>((v >= 0 ? v : -v) - d);
  const int16_t m = mag < 0 ? 0 : (mag > 100 ? 100 : mag);
  return v >= 0 ? m : static_cast<int16_t>(-m);
}
}  // namespace

Tracks TrackSyncLite::apply(Tracks t, bool active, int32_t count_l, int32_t count_r, uint16_t cps_l, uint16_t cps_r,
                            int16_t balance_pm) {
  if (!active) {
    reset();
    return t;
  }
  if (!m_active) {
    m_active = true;
    m_base_l = count_l;
    m_base_r = count_r;
  }
  const int32_t dl = count_l - m_base_l, dr = count_r - m_base_r;
  int32_t ahead = (dl - dr) + dl / 1000 * balance_pm;  // > 0: left ahead of its share (no overflow: review M1)
  if (ahead > kSyncMaxCounts) ahead = kSyncMaxCounts;
  if (ahead < -kSyncMaxCounts) ahead = -kSyncMaxCounts;
  const int32_t dcps = (static_cast<int32_t>(cps_l) - cps_r) + static_cast<int32_t>(cps_l) * balance_pm / 1000;
  int32_t c = ahead / kSyncCountsPerPct + dcps / kSyncCpsPerPct;
  if (c > kSyncMaxPct) c = kSyncMaxPct;
  if (c < -kSyncMaxPct) c = -kSyncMaxPct;
  m_c = static_cast<int16_t>(c);
  const int16_t half = static_cast<int16_t>(m_c / 2);    // the faster side gives up half
  const int16_t rest = static_cast<int16_t>(m_c - half);  // the slower side gains the rest
  t.l = towardZero(t.l, half);
  t.r = towardZero(t.r, static_cast<int16_t>(-rest));
  return t;
}

}  // namespace cq
