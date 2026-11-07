// Bridge-wide app settings in NVS cq_app (Plan O, bridge LLD R13). The only one, the AI engine's page address, is
// retired (design-notes §200): /ai and ai_at replaced it. What is left removes it from the flash once.
#pragma once

namespace cq {
namespace net {

void appForgetAiUrl();  // removes the saved address, if any (boot; nothing happens when none is saved)

}  // namespace net
}  // namespace cq
