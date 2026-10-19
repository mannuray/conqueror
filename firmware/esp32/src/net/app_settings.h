// Plan O (bridge LLD R13): bridge-wide app settings in NVS cq_app - the AI engine's page address.
#pragma once

#include <stddef.h>

namespace cq {
namespace net {

bool appAiUrlLoad(char* out, size_t cap);  // "" when none saved
bool appAiUrlSave(const char* url);        // "" removes it; the caller validates (core::appUrlValid)

}  // namespace net
}  // namespace cq
