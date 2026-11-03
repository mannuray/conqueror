#include "core/skill_push_watch.h"

#include "core/motion_config.h"

namespace cq {
namespace core {
namespace {
constexpr int32_t kProgressCounts = motion::kCpmDefault / 100;  // 1 cm (as the GO's, R20)
constexpr uint32_t kCoastMs = 100;  // a track's counts are not used this long after its direction flips
}  // namespace

void SkillPushWatch::reset() {
  m_l = Track{};
  m_r = Track{};
  m_prev_set = false;
  m_sent_v = 0;
  m_still = false;
  m_clock = 0;
  m_slip.reset();
  forget();
}

// A new pushing window: no pushing time, travel, nearest sonar or unmatched counts yet.
void SkillPushWatch::forget() {
  m_push_ms = 0;
  m_push_fwd = 0;
  m_min_mm = 0xFFFF;
  m_l.carry = m_r.carry = 0;
}

void SkillPushWatch::Track::command(int mix, uint32_t now_ms) {
  const int8_t d = mix > 0 ? 1 : -1;
  if (mix == 0 || d == dir) return;
  dir = d;
  flipped = true;
  flip_ms = now_ms;
}

// The plain mix L = v - w, R = v + w; a change of direction starts the track's coast time.
void SkillPushWatch::commanded(int8_t v, int8_t w, uint32_t now_ms) {
  m_l.command(v - w, now_ms);
  m_r.command(v + w, now_ms);
  m_sent_v = v;
}

// The track's forward counts this frame: its edges signed by its commanded direction (the Uno counts rising edges both
// ways), 0 while it may still coast the old way after a flip (`coasting` then says so).
int32_t SkillPushWatch::Track::forward(int32_t enc, uint32_t now_ms, bool& coasting) {
  const int32_t d = enc - prev;
  prev = enc;
  coasting = flipped && now_ms - flip_ms < kCoastMs;
  return coasting ? 0 : dir * d;
}

// This frame's forward travel, taken conservatively: the smaller of the two tracks' forward counts - a spin (one track
// backward) gives <= 0. A track's counts are summed until the other's match them (a slow creep's edges arrive one track
// at a time: no per-frame truncation); a backward track, a track still coasting after a flip or a frame that does not
// push drops the unmatched counts (else one spin's forward track could be matched with the opposite spin's).
int32_t SkillPushWatch::forward(const MotionTelem& t, uint32_t now_ms, bool pushing) {
  if (!m_prev_set) {
    m_prev_set = true;
    m_l.prev = t.enc_l;
    m_r.prev = t.enc_r;
  }
  bool cl = false, cr = false;
  const int32_t sl = m_l.forward(t.enc_l, now_ms, cl);
  const int32_t sr = m_r.forward(t.enc_r, now_ms, cr);
  if (!pushing || cl || cr || sl < 0 || sr < 0) {
    m_l.carry = m_r.carry = 0;
    return sl < sr ? sl : sr;
  }
  const int32_t al = m_l.carry + sl;
  const int32_t ar = m_r.carry + sr;
  const int32_t m = al < ar ? al : ar;
  m_l.carry = al - m;
  m_r.carry = ar - m;
  return m;
}

// Not pushing (the last frame sent v <= 0: standing, a spin, backing): nothing of it counts as pushing or slipping.
// The slip watch's clock and counts stand still; only a stretch of kMoveStallMs forgets the window and the slip watch.
void SkillPushWatch::still(const MotionTelem& t, uint32_t now_ms) {
  if (!m_still) {
    m_still = true;
    m_still_since = now_ms;
  }
  m_l.off = t.enc_l - m_l.fed;
  m_r.off = t.enc_r - m_r.fed;
  if (now_ms - m_still_since >= motion::kMoveStallMs) {
    forget();
    m_slip.reset();
  }
}

// A frame pushes when the frame before it sent v > 0 (its counts come from that command). The sonar's own change is
// not used (turning changes it), only the nearest it saw over the window.
bool SkillPushWatch::step(const MotionTelem& t, uint32_t dt_ms, uint32_t now_ms, uint16_t cpm) {
  const bool pushing = m_sent_v > 0;
  const int32_t fwd = forward(t, now_ms, pushing);
  if (!pushing) {
    still(t, now_ms);
    return false;
  }
  m_still = false;
  m_push_ms += dt_ms;
  m_push_fwd += fwd;
  if (t.dist_mm < m_min_mm) m_min_mm = t.dist_mm;
  if (m_push_ms > motion::kMoveStallMs) {
    const bool stuck = m_push_fwd < kProgressCounts && m_min_mm < motion::kMoveObstacleMm;
    forget();
    if (stuck) return true;
  }
  m_clock += dt_ms;
  m_l.fed = t.enc_l - m_l.off;
  m_r.fed = t.enc_r - m_r.off;
  return m_slip.step(m_l.fed, m_r.fed, t.rate_ddps, cpm, m_clock);  // R21
}

}  // namespace core
}  // namespace cq
