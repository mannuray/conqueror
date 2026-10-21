// Plan K (spec 02 §G4, bridge LLD R7.2): counts_per_m in NVS (namespace "cq_motion", key "cpm").
#pragma once

#include <stdint.h>

namespace cq {
namespace net {

uint16_t motionStoreLoad();             // the saved value, or motion::kCpmDefault when missing / out of range
bool motionStoreSave(uint16_t cpm);     // false when out of range or the flash write failed
uint16_t motionStoreLoadRoll();         // spec 02 §G6: learned roll-on (mm), default 25, 0..80
bool motionStoreSaveRoll(uint16_t mm);
uint16_t motionStoreLoadCoast();        // bridge LLD R18.2: learned turn coast (ms), default 60, 20..400
bool motionStoreSaveCoast(uint16_t ms);

}  // namespace net
}  // namespace cq
