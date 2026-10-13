#pragma once

#include "domain/motion/heading_controller.h"

namespace cq {

// What a motion source asks of the controllers: forward speed % and steering (rate or heading).
struct MotionSetpoint {
  HeadingMode mode;
  float v_pct;
  float rate_dps;
  float heading_deg;
};

enum class ProfileState : uint8_t { kRunning, kDone, kTimeout, kStalled };

}  // namespace cq
