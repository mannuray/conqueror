#include "core/ws_json.h"
#include "core/ws_json_util.h"

#include <stdio.h>
#include <string.h>

namespace cq {
namespace core {
using namespace detail;

size_t jsonWifiStatus(char* out, size_t cap, const WifiStatusView& v) {
  char ssid[72], saved[3 * 72 + 8] = "";
  jsonEscape(ssid, sizeof(ssid), v.ssid ? v.ssid : "");
  size_t n = 0;
  for (uint8_t i = 0; i < v.n_saved && i < 3; ++i) {
    char e[72];
    jsonEscape(e, sizeof(e), v.saved[i]);
    const int k = snprintf(saved + n, sizeof(saved) - n, "%s\"%s\"", n ? "," : "", e);
    if (k > 0 && static_cast<size_t>(k) < sizeof(saved) - n) n += static_cast<size_t>(k);
  }
  return fmt(out, cap,
             "{\"t\":\"wifi_status\",\"mode\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"host\":\"%s\","
             "\"saved\":[%s],\"ap_ssid\":\"%s\"}",
             v.mode, ssid, v.rssi, v.ip, v.host, saved, v.ap_ssid);
}

size_t jsonWifiScan(char* out, size_t cap, const ScanEntry* nets, size_t count) {
  char list[320] = "";
  size_t n = 0;
  for (size_t i = 0; i < count && i < 12; ++i) {
    char e[72];
    jsonEscape(e, sizeof(e), nets[i].ssid);
    const int k = snprintf(list + n, sizeof(list) - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}", n ? "," : "",
                           e, nets[i].rssi, nets[i].secure ? "true" : "false");
    if (k <= 0 || static_cast<size_t>(k) >= sizeof(list) - n) break;  // keep what fits (kWsJsonMax)
    n += static_cast<size_t>(k);
  }
  return fmt(out, cap, "{\"t\":\"wifi_scan\",\"nets\":[%s]}", list);
}

size_t jsonConfirm(char* out, size_t cap, const char* what, uint8_t secs) {
  return fmt(out, cap, "{\"t\":\"confirm\",\"what\":\"%s\",\"secs\":%u}", what, static_cast<unsigned>(secs));
}

size_t jsonConfirmed(char* out, size_t cap, const char* what) {
  return fmt(out, cap, "{\"t\":\"confirmed\",\"what\":\"%s\"}", what);
}

size_t jsonConfirmExpired(char* out, size_t cap, const char* what) {
  return fmt(out, cap, "{\"t\":\"confirm_expired\",\"what\":\"%s\"}", what);
}

size_t jsonSetting(char* out, size_t cap, uint8_t id, int32_t value) {
  return fmt(out, cap, "{\"t\":\"setting\",\"id\":%u,\"value\":%ld}", static_cast<unsigned>(id),
             static_cast<long>(value));
}

}  // namespace core
}  // namespace cq
