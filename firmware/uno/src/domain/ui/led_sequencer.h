#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"
#include "types/hw_types.h"

namespace cq {

// Lower value wins. Custom = LED frame from a client; alarms always override it.
enum class LedLayer : uint8_t { kAlarm = 0, kAttention, kCustom, kStatus, kBase, kCount };

struct LedPattern {
  proto::LedMode mode;
  Rgb color;
  uint16_t period_ms;
  uint16_t duration_ms;  // 0 = until changed
  // kBlink only (internal, not on the wire): lit for flash_ms at the start of each period, see-through for the
  // rest so the layer below shows - a short flash "over the base colour". 0 = plain blink (half on, half off).
  uint16_t flash_ms = 0;
};

class LedSequencer {
 public:
  static constexpr uint8_t kMinLevelPct = 20;  // breathing floor and level at standstill (LLD §10.6)

  void set(LedLayer layer, const LedPattern& p, uint32_t now_ms);
  void clear(LedLayer layer) { m_active[static_cast<uint8_t>(layer)] = false; }
  // false once cleared or expired (expiry is noticed by output(), i.e. one UI tick late)
  bool active(LedLayer layer) const { return m_active[static_cast<uint8_t>(layer)]; }
  // led_max (setting, default 50): every output is scaled by max / 255.
  void setMax(uint8_t max) { m_max = max; }
  // Style of a SOLID base layer (the owner colour): breathe (triangle 20..100 %, period) when idle, else a steady
  // level in % (20 % + 80 % × speed while driving). Default: steady 100 % (no styling).
  void setBaseStyle(bool breathe, uint16_t period_ms, uint8_t level_pct);
  Rgb output(uint32_t now_ms);

 private:
  Rgb raw(uint32_t now_ms, uint8_t& layer);
  static constexpr uint8_t kLayers = static_cast<uint8_t>(LedLayer::kCount);
  LedPattern m_p[kLayers] = {};
  uint32_t m_start_ms[kLayers] = {};
  bool m_active[kLayers] = {};
  uint8_t m_max = 255;
  bool m_breathe = false;
  uint16_t m_breathe_ms = 2000;
  uint8_t m_level_pct = 100;
};

}  // namespace cq
