// Port: the WiFi manager (runs in the Arduino loop) seen from the link task (bridge LLD R9, R15.3).
#pragma once

#include "core/wifi_types.h"

namespace cq {
namespace ports {

bool wifiPost(const core::WifiRequest& r);  // false: the queue is full
bool wifiPoll(core::WifiResult& out);       // one result at a time
void wifiSnapshot(core::WifiSnapshot& out);

}  // namespace ports
}  // namespace cq
