#include "domain/camera/servo_profile.h"

#include <math.h>

#include "domain/util/math_util.h"

namespace cq {

float ServoProfile::step(float dt_s) {
  const float dist = m_target - m_pos;
  if (absf(dist) < 0.05f && absf(m_vel) < m_a_max * dt_s) {
    m_pos = m_target;
    m_vel = 0.0f;
    return m_pos;
  }
  float v_des = sqrtf(2.0f * m_a_max * absf(dist));
  if (v_des > m_v_max) v_des = m_v_max;
  v_des *= signf(dist);
  const float dv = m_a_max * dt_s;
  m_vel += clampf(v_des - m_vel, -dv, dv);
  const float next = m_pos + m_vel * dt_s;
  if ((m_target - next) * dist <= 0.0f) {  // would pass the target: land on it
    m_pos = m_target;
    m_vel = 0.0f;
  } else {
    m_pos = next;
  }
  return m_pos;
}

}  // namespace cq
