// Port: counts per metre and the learned roll-on in flash (bridge LLD R15.3, spec 02 §G6).
#pragma once

#include <stdint.h>

namespace cq {
namespace ports {

uint16_t loadCpm();         // the saved value, or the default when missing / out of range
bool saveCpm(uint16_t v);   // false: out of range or the flash write failed
uint16_t loadRoll();        // mm
bool saveRoll(uint16_t v);

}  // namespace ports
}  // namespace cq
