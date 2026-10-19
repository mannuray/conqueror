#include "net/app_settings.h"

#include <Preferences.h>
#include <string.h>

namespace cq {
namespace net {

bool appAiUrlLoad(char* out, size_t cap) {
  out[0] = '\0';
  Preferences p;
  if (!p.begin("cq_app", true)) return false;
  const size_t n = p.getString("ai", out, cap);
  p.end();
  if (n == 0) out[0] = '\0';
  return true;
}

bool appAiUrlSave(const char* url) {
  Preferences p;
  if (!p.begin("cq_app", false)) return false;
  // an empty address clears it: remove() is false when nothing was saved, which is fine
  const bool ok = url[0] == '\0' ? (p.remove("ai") || true) : p.putString("ai", url) == strlen(url);
  p.end();
  return ok;
}

}  // namespace net
}  // namespace cq
