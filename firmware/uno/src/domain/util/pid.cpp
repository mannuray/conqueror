#include "domain/util/pid.h"

#include "domain/util/math_util.h"

namespace cq {

float Pid::update(float error, float dt_s, bool freeze_integrator) {
  if (dt_s <= 0.0f) dt_s = 1e-3f;
  const float raw_d = m_primed ? (error - m_prev_err) / dt_s : 0.0f;
  m_primed = true;
  m_prev_err = error;
  m_d += 0.5f * (raw_d - m_d);
  const float p = m_g.kp * error;
  const float d = m_g.kd * m_d;
  const float unsat = p + m_i + d;
  const bool pushing_high = unsat >= m_max && error > 0.0f;
  const bool pushing_low = unsat <= m_min && error < 0.0f;
  if (!freeze_integrator && !pushing_high && !pushing_low) {
    m_i = clampf(m_i + m_g.ki * error * dt_s, m_min, m_max);
  }
  return clampf(p + m_i + d, m_min, m_max);
}

}  // namespace cq
