#pragma once
#include <stdint.h>

#include "types/hw_types.h"

namespace cq {

struct IrOutput {
  bool active;     // a drive key is held
  int8_t v;
  int8_t w;
  int8_t jog_pan;  // +1 right, -1 left (while 3 / 1 held)
  int8_t jog_tilt; // +1 up, -1 down (while 2 / 8 held)
  bool stop;
  bool cam_home;
  bool speed_changed;
  bool toggle_mute;
  bool battery_check;
  bool turn_180;
  bool toggle_parking;   // `*` tap: reported on release (LLD §10.5)
  bool cal_request;
  bool toggle_override;  // `*` held 2 s: Safety Override on/off (once per hold)
};

class IrController {
 public:
  // Held key ends 350 ms after the last repeat: tolerates two lost NEC repeats (~108 ms apart);
  // the decoder uses the same window (design-notes §39).
  static constexpr uint16_t kReleaseMs = 350;
  static constexpr uint16_t kCalHoldMs = 3000;
  static constexpr uint16_t kOverrideHoldMs = 2000;
  bool onEvent(const IrEvent& e, IrOutput& out);
  // true when a held drive/jog key was released or a `*` tap ended (out.toggle_parking); a release
  // clears every one-shot flag of `out`, so an earlier key is never applied twice.
  bool tick(uint32_t now_ms, IrOutput& out);
  uint8_t speedPct() const { return m_speed_pct; }

 private:
  enum class Hold : uint8_t { kNone, kDrive, kJog, kHash, kStar };
  uint8_t m_speed_pct = 50;
  Hold m_hold = Hold::kNone;
  uint32_t m_last_ms = 0;
  uint32_t m_hash_start_ms = 0;
  bool m_hash_fired = false;
  uint32_t m_star_start_ms = 0;
  bool m_star_fired = false;
};

}  // namespace cq
