#include "core/cam_tuning.h"

#include <string.h>

namespace cq {
namespace core {
namespace {
const char* const kWbNames[kWbModes] = {"auto", "sunny", "cloudy", "office", "home"};
}  // namespace

CamTuning sensorTuning(const CamSettings& s) {
  CamTuning t;
  t.gain_ceiling_raw = s.low_light ? kGainCeilingLowLight : kGainCeilingNormal;
  t.ae_level = s.low_light ? 1 : 0;
  t.night = s.night;
  t.wb_mode = s.wb < kWbModes ? s.wb : 0;
  return t;
}

const char* wbName(uint8_t mode) { return kWbNames[mode < kWbModes ? mode : 0]; }

bool wbOf(const char* name, uint8_t& mode) {
  for (uint8_t i = 0; i < kWbModes; ++i) {
    if (strcmp(name, kWbNames[i]) == 0) {
      mode = i;
      return true;
    }
  }
  return false;
}

bool camFromBlock(const uint8_t* data, size_t len, CamSettings& out) {
  CamSettings c = out;
  if (len == sizeof(CamSettings)) {
    memcpy(&c, data, sizeof(CamSettings));
  } else if (len == kCamSettingsV1Bytes) {
    memcpy(&c, data, kCamSettingsV1Bytes);  // the fields before R24 are the struct's first 8 bytes
    c.wb = 0;
    c.night = false;
  } else {
    return false;
  }
  if (c.size >= 4 || c.quality < 10 || c.quality > 40 || c.wb >= kWbModes) return false;
  out = c;
  return true;
}

}  // namespace core
}  // namespace cq
