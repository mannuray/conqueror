#include "domain/lite/yaw_lite.h"

namespace cq {

void YawLite::update(int16_t g, bool stationary) {
  if (stationary && g > -kStillRaw && g < kStillRaw) g = 0;
  m_rate_ddps = static_cast<int16_t>(static_cast<int32_t>(g) * 20 / 131);  // raw → 0.1 °/s
  m_acc += g;
  if (m_acc >= kTurn) m_acc -= kTurn;
  if (m_acc < 0) m_acc += kTurn;
}

}  // namespace cq
