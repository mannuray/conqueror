#include "core/wifi_store.h"

#include <string.h>

namespace cq {
namespace core {
namespace {
bool validSsid(const char* s) {
  const size_t n = s ? strlen(s) : 0;
  return n >= 1 && n <= 32;
}
bool validPass(const char* p, bool allow_open) {
  const size_t n = p ? strlen(p) : 0;
  return (allow_open && n == 0) || (n >= 8 && n <= 63);
}
void copy(char* dst, size_t cap, const char* src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = '\0';
}
bool terminated(const uint8_t* p, size_t cap) { return memchr(p, '\0', cap) != nullptr; }
}  // namespace

void WifiStore::clear() {
  memset(m_ssid, 0, sizeof(m_ssid));
  memset(m_pass, 0, sizeof(m_pass));
  memset(m_ap_pass, 0, sizeof(m_ap_pass));
}

void WifiStore::seed(const char* ssid, const char* pass, const char* ap_pass) {
  clear();
  if (validSsid(ssid) && validPass(pass, true)) {
    copy(m_ssid[0], kSsidSize, ssid);
    copy(m_pass[0], kPassSize, pass);
  }
  if (validPass(ap_pass, false)) copy(m_ap_pass, kPassSize, ap_pass);
}

StoreResult WifiStore::save(int slot, const char* ssid, const char* pass) {
  if (!validSsid(ssid)) return StoreResult::kBadSsid;
  if (!validPass(pass, true)) return StoreResult::kBadPassword;
  if (slot < -1 || slot >= kSlots) return StoreResult::kBadSlot;
  if (slot == -1) {
    slot = slotOf(ssid);
    for (int i = 0; slot == -1 && i < kSlots; ++i) {
      if (m_ssid[i][0] == '\0') slot = i;
    }
    if (slot == -1) return StoreResult::kFull;
  }
  copy(m_ssid[slot], kSsidSize, ssid);
  copy(m_pass[slot], kPassSize, pass);
  return StoreResult::kOk;
}

StoreResult WifiStore::forget(int slot) {
  if (slot == -1) {
    memset(m_ssid, 0, sizeof(m_ssid));
    memset(m_pass, 0, sizeof(m_pass));
    return StoreResult::kOk;
  }
  if (slot < 0 || slot >= kSlots) return StoreResult::kBadSlot;
  memset(m_ssid[slot], 0, kSsidSize);
  memset(m_pass[slot], 0, kPassSize);
  return StoreResult::kOk;
}

StoreResult WifiStore::setApPassword(const char* pass) {
  if (!validPass(pass, false)) return StoreResult::kBadPassword;
  copy(m_ap_pass, kPassSize, pass);
  return StoreResult::kOk;
}

uint8_t WifiStore::count() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < kSlots; ++i) n = static_cast<uint8_t>(n + (m_ssid[i][0] != '\0'));
  return n;
}

int WifiStore::slotOf(const char* ssid) const {
  if (!ssid || ssid[0] == '\0') return -1;
  for (int i = 0; i < kSlots; ++i) {
    if (strcmp(m_ssid[i], ssid) == 0) return i;
  }
  return -1;
}

int WifiStore::pickStrongest(const ScanEntry* nets, size_t n) const {
  int best = -1;
  int best_rssi = -1000;
  for (size_t i = 0; nets && i < n; ++i) {
    const int slot = slotOf(nets[i].ssid);
    if (slot >= 0 && nets[i].rssi > best_rssi) {
      best = slot;
      best_rssi = nets[i].rssi;
    }
  }
  return best;
}

size_t WifiStore::encode(uint8_t* out) const {
  uint8_t* p = out;
  *p++ = kVersion;
  for (uint8_t i = 0; i < kSlots; ++i) {
    memcpy(p, m_ssid[i], kSsidSize);
    p += kSsidSize;
    memcpy(p, m_pass[i], kPassSize);
    p += kPassSize;
  }
  memcpy(p, m_ap_pass, kPassSize);
  return kBlobSize;
}

bool WifiStore::decode(const uint8_t* in, size_t n) {
  clear();
  if (!in || n != kBlobSize || in[0] != kVersion) return false;
  const uint8_t* p = in + 1;
  for (uint8_t i = 0; i < kSlots; ++i) {
    if (!terminated(p, kSsidSize) || !terminated(p + kSsidSize, kPassSize)) {
      clear();
      return false;
    }
    p += kSsidSize + kPassSize;
  }
  if (!terminated(p, kPassSize)) return false;
  p = in + 1;
  for (uint8_t i = 0; i < kSlots; ++i) {
    memcpy(m_ssid[i], p, kSsidSize);
    p += kSsidSize;
    memcpy(m_pass[i], p, kPassSize);
    p += kPassSize;
  }
  memcpy(m_ap_pass, p, kPassSize);
  return true;
}

}  // namespace core
}  // namespace cq
