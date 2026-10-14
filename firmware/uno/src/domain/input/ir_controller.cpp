#include "domain/input/ir_controller.h"

#include "domain/input/ir_codes.h"
#include "domain/util/math_util.h"

namespace cq {

bool IrController::onEvent(const IrEvent& e, IrOutput& out) {
  out.stop = out.cam_home = out.speed_changed = out.toggle_mute = false;
  out.battery_check = out.turn_180 = out.toggle_parking = out.cal_request = out.toggle_override = false;
  const int8_t s = static_cast<int8_t>(m_speed_pct);

  // held keys: repeats keep them alive
  int8_t v = 0, w = 0, jp = 0, jt = 0;
  Hold hold = Hold::kNone;
  switch (e.code) {
    case ir_codes::kUp: v = s; hold = Hold::kDrive; break;
    case ir_codes::kDown: v = static_cast<int8_t>(-s); hold = Hold::kDrive; break;
    case ir_codes::kLeft: w = s; hold = Hold::kDrive; break;
    case ir_codes::kRight: w = static_cast<int8_t>(-s); hold = Hold::kDrive; break;
    case ir_codes::k1: jp = -1; hold = Hold::kJog; break;
    case ir_codes::k3: jp = 1; hold = Hold::kJog; break;
    case ir_codes::k2: jt = 1; hold = Hold::kJog; break;
    case ir_codes::k8: jt = -1; hold = Hold::kJog; break;
    case ir_codes::kHash: hold = Hold::kHash; break;
    case ir_codes::kStar: hold = Hold::kStar; break;
    default: break;
  }
  if (hold != Hold::kNone) {
    if (hold == Hold::kDrive) {
      if (e.repeat) {
        m_seen_repeat = true;  // also a repeat arriving after a nudge ended: the key is held
      } else {
        m_code_ms = e.t_ms;
        m_seen_repeat = false;
      }
    }
    m_last_ms = e.t_ms;
    if (hold == Hold::kHash) {
      if (!e.repeat || m_hold != Hold::kHash) {
        m_hash_start_ms = e.t_ms;
        m_hash_fired = false;
      } else if (!m_hash_fired && elapsed(e.t_ms, m_hash_start_ms, kCalHoldMs)) {
        m_hash_fired = true;
        out.cal_request = true;
      }
      m_hold = hold;
      return true;
    }
    if (hold == Hold::kStar) {  // tap → parking beeps (on release, tick); held 2 s → Safety Override
      if (!e.repeat || m_hold != Hold::kStar) {
        m_star_start_ms = e.t_ms;
        m_star_fired = false;
      } else if (!m_star_fired && elapsed(e.t_ms, m_star_start_ms, kOverrideHoldMs)) {
        m_star_fired = true;
        out.toggle_override = true;
      }
      m_hold = hold;
      return true;
    }
    m_hold = hold;
    out.active = hold == Hold::kDrive;
    out.v = v;
    out.w = w;
    out.jog_pan = jp;
    out.jog_tilt = jt;
    return true;
  }
  if (e.repeat) return false;  // repeats only matter for held keys

  switch (e.code) {
    case ir_codes::kOk:
      m_hold = Hold::kNone;
      out.active = false;
      out.v = out.w = out.jog_pan = out.jog_tilt = 0;
      out.stop = true;
      return true;
    case ir_codes::k4:
    case ir_codes::k6: {
      const int16_t next = static_cast<int16_t>(m_speed_pct + (e.code == ir_codes::k6 ? 10 : -10));
      m_speed_pct = static_cast<uint8_t>(next < 20 ? 20 : (next > 100 ? 100 : next));
      out.speed_changed = true;
      return true;
    }
    case ir_codes::k5: out.cam_home = true; return true;
    case ir_codes::k7: out.toggle_mute = true; return true;
    case ir_codes::k9: out.battery_check = true; return true;
    case ir_codes::k0: out.turn_180 = true; return true;
    default: return false;
  }
}

bool IrController::tick(uint32_t now_ms, IrOutput& out) {
  if (m_hold == Hold::kNone) return false;
  const bool tap_done = m_hold == Hold::kDrive && !m_seen_repeat && elapsed(now_ms, m_code_ms, kNudgeMs);
  if (!tap_done && !elapsed(now_ms, m_last_ms, kReleaseMs)) return false;
  const Hold released = m_hold;
  m_hold = Hold::kNone;
  out = IrOutput{};  // not active, no jog, no one-shot flag left over from an earlier key
  if (released == Hold::kStar) {
    out.toggle_parking = !m_star_fired;  // released before 2 s = a tap
    return out.toggle_parking;
  }
  return released == Hold::kDrive || released == Hold::kJog;
}

}  // namespace cq
