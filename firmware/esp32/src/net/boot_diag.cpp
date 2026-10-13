#include "net/boot_diag.h"

#include <Preferences.h>
#include <esp_system.h>
#include <stdio.h>

#include "core/boot_history.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {
core::BootHistory g_hist;
}  // namespace

void bootDiagBegin() {
  Preferences p;
  if (p.begin("cqboot", false)) {
    uint8_t ring[core::BootHistory::kSize] = {};
    p.getBytes("ring", ring, sizeof(ring));
    g_hist.restore(p.getUInt("count", 0), ring, p.getUChar("head", 0));
    g_hist.push(static_cast<uint8_t>(esp_reset_reason()));
    p.putUInt("count", g_hist.count());
    p.putBytes("ring", g_hist.ring(), core::BootHistory::kSize);
    p.putUChar("head", g_hist.head());
    p.end();
  } else {
    g_hist.push(static_cast<uint8_t>(esp_reset_reason()));
  }
  uint8_t r[core::BootHistory::kSize];
  const size_t n = g_hist.recent(r, core::BootHistory::kSize);
  char list[128] = "";
  size_t used = 0;
  for (size_t i = 0; i < n && used < sizeof(list); ++i) {
    used += static_cast<size_t>(snprintf(list + used, sizeof(list) - used, "%s%s", i ? " " : "",
                                         core::BootHistory::reasonName(r[i])));
  }
  logf("boot: #%lu, reset reason %s (recent: %s)", static_cast<unsigned long>(g_hist.count()),
       core::BootHistory::reasonName(r[0]), list);
}

size_t bootDiagJson(char* out, size_t cap) {
  uint8_t r[core::BootHistory::kSize];
  const size_t n = g_hist.recent(r, core::BootHistory::kSize);
  int w = snprintf(out, cap, "{\"count\":%lu,\"reason\":\"%s\",\"recent\":[",
                   static_cast<unsigned long>(g_hist.count()),
                   n ? core::BootHistory::reasonName(r[0]) : "unknown");
  for (size_t i = 0; i < n && w > 0 && static_cast<size_t>(w) < cap; ++i) {
    w += snprintf(out + w, cap - static_cast<size_t>(w), "%s\"%s\"", i ? "," : "",
                  core::BootHistory::reasonName(r[i]));
  }
  if (w > 0 && static_cast<size_t>(w) < cap) w += snprintf(out + w, cap - static_cast<size_t>(w), "]}");
  return (w > 0 && static_cast<size_t>(w) < cap) ? static_cast<size_t>(w) : 0;
}

}  // namespace net
}  // namespace cq
