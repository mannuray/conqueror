// The camera's sensor values and the saved settings' carry-over (bridge LLD R24, design-notes §140). Pure.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/cam_settings.h"

namespace cq {
namespace core {

// What applyTo() writes to the OV3660. Its gain ceiling is a RAW 10-bit gain (16 = 1x, 0x3FF ~ 64x; esp32-camera
// sensors/ov3660.c writes it to 0x3A18 / 0x3A19) - not the library's GAINCEILING_* names (the §96 bug).
struct CamTuning {
  uint16_t gain_ceiling_raw = 248;
  int8_t ae_level = 0;
  bool night = false;
  uint8_t wb_mode = 0;
};

constexpr uint16_t kGainCeilingNormal = 248;  // 15.5x: the OV3660's own default (0x3A19 = 0xF8; Plan X review)
constexpr uint16_t kGainCeilingLowLight = 512;  // 32x
constexpr uint8_t kWbModes = 5;

CamTuning sensorTuning(const CamSettings& s);
const char* wbName(uint8_t mode);  // "auto" for anything out of range
bool wbOf(const char* name, uint8_t& mode);
// The settings from a saved NVS block: the R24 size, or the 8-byte block before it (the new fields keep the
// values already in `out`); false for any other size or an invalid value.
bool camFromBlock(const uint8_t* data, size_t len, CamSettings& out);

}  // namespace core
}  // namespace cq
