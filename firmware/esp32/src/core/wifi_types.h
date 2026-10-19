// WiFi request / result / snapshot passed between the link task and the WiFi manager (bridge LLD R9, R15.3).
#pragma once

#include <stdint.h>

#include "core/wifi_store.h"

namespace cq {
namespace core {

struct WifiRequest {
  enum Kind : uint8_t { kStatus, kScan, kSave, kForget, kMode, kApPassword };
  Kind kind = kStatus;
  int fd = -1;
  int32_t id = -1;
  int8_t slot = -1;
  bool ap = false;
  char ssid[33] = "";
  char pass[64] = "";  // never logged
};

struct WifiResult {
  enum Kind : uint8_t { kStatus, kScan, kApplied, kFailed, kModeChanged };  // kModeChanged: id = core::WifiMode (Plan O log)
  Kind kind = kStatus;
  int fd = -1;
  int32_t id = -1;
  uint8_t req = 0;     // WifiRequest::Kind
  char text[48] = "";  // kFailed reason (no secrets)
};

struct WifiSnapshot {
  char mode[12] = "connecting";
  char ssid[33] = "";
  int rssi = 0;
  char ip[16] = "";
  char saved[core::WifiStore::kSlots][33] = {};
  uint8_t n_saved = 0;
  core::ScanEntry scan[12] = {};
  uint8_t n_scan = 0;
};

}  // namespace core
}  // namespace cq
