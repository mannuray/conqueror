#include "net/app_settings.h"

#include <Preferences.h>

namespace cq {
namespace net {

void appForgetAiUrl() {
  Preferences p;
  if (!p.begin("cq_app", false)) return;
  if (p.isKey("ai")) p.remove("ai");
  p.end();
}

}  // namespace net
}  // namespace cq
