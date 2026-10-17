#include "net/motion_store.h"

#include <Preferences.h>

#include "core/motion_config.h"
#include "net/log.h"

namespace cq {
namespace net {

uint16_t motionStoreLoad() {
  uint16_t v = core::motion::kCpmDefault;
  Preferences p;
  if (p.begin("cq_motion", true)) {  // read-only; fails when the namespace was never written -> default
    const uint16_t saved = p.getUShort("cpm", 0);
    if (core::motion::validCpm(saved)) v = saved;
    p.end();
  }
  logf("motion: counts_per_m %u", static_cast<unsigned>(v));
  return v;
}

bool motionStoreSave(uint16_t cpm) {
  if (!core::motion::validCpm(cpm)) return false;
  Preferences p;
  if (!p.begin("cq_motion", false)) return false;
  const bool ok = p.putUShort("cpm", cpm) == sizeof(uint16_t);
  p.end();
  if (ok) logf("motion: counts_per_m saved %u", static_cast<unsigned>(cpm));
  return ok;
}

}  // namespace net
}  // namespace cq
