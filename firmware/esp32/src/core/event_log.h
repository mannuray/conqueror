// Bridge LLD R13: the last 200 things that happened (Uno events, stops, WiFi, restarts) for the Log tab. Pure C++.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

enum class LogKind : uint8_t {
  kBoot, kUno, kPrimitive, kStopHold, kClientLost, kBodyOnline, kBodyOffline, kWifiMode, kRestart, kOta,
  kLogo,  // Plan N3: a program ended (a = code, 0 finished; b = line; client = its name) or a request refused (a = -1)
};

struct LogEntry {
  uint32_t seq = 0;
  uint32_t t_ms = 0;  // bridge uptime
  uint32_t uno_ms = 0;
  LogKind kind = LogKind::kBoot;
  int32_t a = 0;  // kUno: event kind; kPrimitive: EndReason; kWifiMode: 0 home / 1 own AP / 2 fallback AP; kBoot: reason
  int32_t b = 0;  // kUno: payload a; kPrimitive: PrimKind
  char client[12] = "";
};

class EventLog {
 public:
  static constexpr uint16_t kCap = 200;
  uint32_t add(LogKind kind, int32_t a, int32_t b, const char* client, uint32_t uno_ms, uint32_t now_ms);
  // Entries with seq > `seq`, oldest first, at most `max`; gap = some asked-for entries were already overwritten.
  uint8_t since(uint32_t seq, LogEntry* out, uint8_t max, bool& gap) const;
  uint32_t last() const { return m_next - 1; }
  uint32_t oldest() const { return m_next > kCap ? m_next - kCap : 1; }

 private:
  LogEntry m_e[kCap];
  uint32_t m_next = 1;  // seq of the next entry
};

}  // namespace core
}  // namespace cq
