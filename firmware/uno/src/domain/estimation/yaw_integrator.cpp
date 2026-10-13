#include "domain/estimation/yaw_integrator.h"

#include "domain/util/math_util.h"

namespace cq {

void YawIntegrator::update(float rate_dps, float dt_s, bool stationary) {
  if (stationary && absf(rate_dps) < kStationaryClampDps) rate_dps = 0.0f;
  m_rate_dps = rate_dps;
  const float d = rate_dps * dt_s;
  m_total_deg += d;
  m_yaw_deg = wrap360(m_yaw_deg + d);
}

}  // namespace cq
