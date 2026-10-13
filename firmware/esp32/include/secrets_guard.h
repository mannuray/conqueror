// Compile-time checks on include/secrets.h (included by main.cpp only).
#pragma once

#if !__has_include("secrets.h")
#error "include/secrets.h missing: cp include/secrets.example.h include/secrets.h and fill it in"
#endif
#include "secrets.h"

#if !defined(CQ_WIFI_SSID) || !defined(CQ_WIFI_PASSWORD) || !defined(CQ_AP_PASSWORD) || \
    !defined(CQ_OTA_PASSWORD)
#error "secrets.h must define CQ_WIFI_SSID, CQ_WIFI_PASSWORD, CQ_AP_PASSWORD, CQ_OTA_PASSWORD"
#endif

namespace cq {
namespace secrets_guard {
constexpr bool streq(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || streq(a + 1, b + 1));
}
static_assert(!streq(CQ_OTA_PASSWORD, "change-me"), "set CQ_OTA_PASSWORD in secrets.h");
static_assert(sizeof(CQ_OTA_PASSWORD) >= 9, "CQ_OTA_PASSWORD: at least 8 characters");
static_assert(sizeof(CQ_AP_PASSWORD) >= 9, "CQ_AP_PASSWORD: WPA2 needs at least 8 characters");
}  // namespace secrets_guard
}  // namespace cq
