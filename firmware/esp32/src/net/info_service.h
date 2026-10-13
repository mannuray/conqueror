// GET /info: read-only JSON self-description (bridge LLD R2.4, design-notes §27). No secrets.
#pragma once

#include "net/wifi_manager.h"

namespace cq {
namespace net {

void infoRegister(const WifiManager& wifi);  // after httpServerStart()

}  // namespace net
}  // namespace cq
