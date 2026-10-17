// Plan K (spec 02 §G4, bridge LLD R7.2): counts_per_m in NVS (namespace "cq_motion", key "cpm").
#pragma once

#include <stdint.h>

namespace cq {
namespace net {

uint16_t motionStoreLoad();             // the saved value, or motion::kCpmDefault when missing / out of range
bool motionStoreSave(uint16_t cpm);     // false when out of range or the flash write failed

}  // namespace net
}  // namespace cq
