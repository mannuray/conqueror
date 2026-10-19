// Port: the AI engine's page address in flash (bridge LLD R13, R15.3).
#pragma once

#include <stddef.h>

namespace cq {
namespace ports {

bool loadAiUrl(char* out, size_t cap);  // "" when none saved
bool saveAiUrl(const char* url);        // "" removes it; the caller validates

}  // namespace ports
}  // namespace cq
