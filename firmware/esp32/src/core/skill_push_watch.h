// A skill's "pushing but not moving" (bridge LLD R26.1 / R26.3, Plan Z2a Task 5 round 4 ruling): over kMoveStallMs of
// pushing time, less than 1 cm of forward travel with something within kMoveObstacleMm ahead -> blocked; then the slip
// watch (R21) on a pushing-time clock. Forward travel is taken conservatively from each track's commanded direction, so
// a spin, however fast and however it alternates with a creep, never counts as progress. Pure, host-tested.
#pragma once

#include <stdint.h>

#include "core/move_controller.h"
#include "core/slip_watch.h"

namespace cq {
namespace core {

class SkillPushWatch {
 public:
  void reset();
  // What the tracks were just told (the frame's v and w, sent at now_ms): the next frame's counts come from it.
  void commanded(int8_t v, int8_t w, uint32_t now_ms);
  // Each TELEM_FAST, dt_ms since the last one: true once the skill is pushing but not moving.
  bool step(const MotionTelem& t, uint32_t dt_ms, uint32_t now_ms, uint16_t cpm);

 private:
  struct Track {
    int8_t dir = 1;  // the direction it was last commanded (0 keeps the last, as logo::Odometry)
    bool flipped = false;
    uint32_t flip_ms = 0;  // its coast after a flip is not counted
    int32_t prev = 0;
    int32_t carry = 0;  // forward counts not yet matched by the other track's
    int32_t off = 0, fed = 0;  // the slip watch sees the counts of the pushing frames only
    void command(int mix, uint32_t now_ms);
    int32_t forward(int32_t enc, uint32_t now_ms, bool& coasting);
  };
  void forget();
  int32_t forward(const MotionTelem& t, uint32_t now_ms, bool pushing);
  void still(const MotionTelem& t, uint32_t now_ms);
  Track m_l, m_r;
  bool m_prev_set = false;
  int8_t m_sent_v = 0;      // the v sent on the last frame
  uint32_t m_push_ms = 0;   // pushing time in the current window ...
  int32_t m_push_fwd = 0;   // ... the net forward travel in counts over it ...
  uint16_t m_min_mm = 0xFFFF;  // ... and the nearest the sonar saw over it
  bool m_still = false;     // the frames since m_still_since were all not pushing
  uint32_t m_still_since = 0;
  uint32_t m_clock = 0;     // the slip watch's clock: pushing time only
  SlipWatch m_slip;
};

}  // namespace core
}  // namespace cq
