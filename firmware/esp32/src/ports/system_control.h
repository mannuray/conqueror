// Port: restart and reset reason (bridge LLD R15.3).
#pragma once

#include <stdint.h>

namespace cq {
namespace ports {

void systemRestart();
uint8_t resetReason();  // esp_reset_reason_t value

}  // namespace ports
}  // namespace cq
