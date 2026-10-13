#pragma once
#include <stdint.h>

#include "domain/settings/settings.h"

namespace cq {

// TELEM_SLOW.cal_status: 2 bits per item (0 missing = factory default, 1 calibrated).
// Items: 0 gyro bias, 1 level, 2 deadband, 3 max_cps, 4 counts_per_cm, 5 track width,
//        6 battery scale, 7 servo trims, 8 ultrasonic offset, 9 speed PID.
void calStatus(const GlobalSettings& g, const ProfileSettings& p, uint8_t out[3]);

}  // namespace cq
