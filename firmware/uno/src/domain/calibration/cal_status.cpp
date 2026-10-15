#include "domain/calibration/cal_status.h"

namespace cq {

void calStatus(const GlobalSettings& g, const ProfileSettings& p, uint8_t out[3]) {
  // "Calibrated" = differs from the generated defaults (never hard-code them: §39 changed them).
  Settings defaults;
  defaults.loadDefaults();
  const ProfileSettings& d = defaults.profile();
  bool deadband = false;
  for (uint8_t i = 0; i < 4; ++i) {
    deadband = deadband || p.deadband_start[i] != d.deadband_start[i] || p.deadband_keep[i] != d.deadband_keep[i];
  }
  const bool items[10] = {
      g.gyro_bias_x != 0 || g.gyro_bias_y != 0 || g.gyro_bias_z != 0,
      g.level_pitch != 0 || g.level_roll != 0,
      deadband,
      p.max_cps_l != 2500 || p.max_cps_r != 2500,
      g.counts_per_cm != 7600,
      p.track_width_eff != 150,
      g.batt_mv_per_count != 4050,
      g.pan_trim != 0 || g.tilt_trim != 0,
      g.ultrasonic_offset != 0,
      p.pid_speed_kp != 150 || p.pid_speed_ki != 800,
  };
  out[0] = out[1] = out[2] = 0;
  for (uint8_t i = 0; i < 10; ++i) {
    if (items[i]) out[i / 4] = static_cast<uint8_t>(out[i / 4] | (1u << ((i % 4) * 2)));
  }
}

}  // namespace cq
