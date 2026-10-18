// Saved home networks + the own-AP password (Plan M, spec 02 §I1, bridge LLD R9.2). Pure C++: the bridge keeps the
// blob in NVS (net/wifi_manager.cpp). Passwords leave this class only to WiFi.begin / softAP - never to JSON or logs.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

struct ScanEntry {
  char ssid[33];
  int8_t rssi;
  bool secure;
};

enum class StoreResult : uint8_t { kOk, kBadSsid, kBadPassword, kFull, kBadSlot };

class WifiStore {
 public:
  static constexpr uint8_t kSlots = 3;
  static constexpr uint8_t kVersion = 1;
  static constexpr size_t kSsidSize = 33;
  static constexpr size_t kPassSize = 64;
  static constexpr size_t kBlobSize = 1 + kSlots * (kSsidSize + kPassSize) + kPassSize;

  void clear();
  void seed(const char* ssid, const char* pass, const char* ap_pass);
  StoreResult save(int slot, const char* ssid, const char* pass);
  StoreResult forget(int slot);
  StoreResult setApPassword(const char* pass);
  uint8_t count() const;
  const char* ssid(uint8_t slot) const { return slot < kSlots ? m_ssid[slot] : ""; }
  const char* pass(uint8_t slot) const { return slot < kSlots ? m_pass[slot] : ""; }
  const char* apPass() const { return m_ap_pass; }
  int pickStrongest(const ScanEntry* nets, size_t n) const;
  int slotOf(const char* ssid) const;
  size_t encode(uint8_t* out) const;
  bool decode(const uint8_t* in, size_t n);

 private:
  char m_ssid[kSlots][kSsidSize] = {};
  char m_pass[kSlots][kPassSize] = {};
  char m_ap_pass[kPassSize] = {};
};

}  // namespace core
}  // namespace cq
