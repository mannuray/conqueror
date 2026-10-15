#include "domain/lite/cam_lite.h"
#include "lite/lite_app.h"
#include "lite/lite_tasks.h"
#include "ports/clock.h"
#include "ports/led.h"
#include "ports/servos.h"
#include "ports/tone.h"

namespace cq {
namespace lite {

namespace {
Rgb g_led_out{1, 1, 1};
uint16_t g_tone_hz = 0;
}  // namespace

void uiTask() {
  const uint32_t now = port::clock::nowMs();
  LiteUiState s{};
  s.synced = g_lite.link.synced();
  s.owner = g_lite.arbitrator.owner();
  s.driving = g_lite.moving;
  const int16_t a = g_lite.fast.spd_cmd_l < 0 ? static_cast<int16_t>(-g_lite.fast.spd_cmd_l) : g_lite.fast.spd_cmd_l;
  s.speed_pct = static_cast<uint8_t>(a > 100 ? 100 : a);
  s.locked = g_lite.self_test.locked();
  s.tilted_or_lifted = g_lite.tilt.tilted() || g_lite.tilt.lifted();
  s.batt_crit = g_lite.batt.level() == BatteryMonitor::Level::kCritical;
  s.batt_low = g_lite.batt.level() == BatteryMonitor::Level::kLow;
  s.blocked = g_lite.safety.blocked;
  g_lite.ann.update(s, now, g_lite.led);
  const Rgb c = g_lite.led.output(now);
  if (c != g_led_out) {
    port::led::write(c);
    g_led_out = c;
  }
  const uint16_t hz = g_lite.beeper.tick(now);
  if (hz != g_tone_hz) {
    if (hz == 0) {
      port::tone::stop();
    } else {
      port::tone::play(hz);
    }
    g_tone_hz = hz;
  }
}

void cameraTask() {
  g_lite.cam.tick(port::clock::nowMs());
  g_lite.cam.step();  // smooth: at most 120 °/s
  port::servos::writeUs(Axis::kPan, CamLite::toUs(g_lite.cam.outPan()));
  port::servos::writeUs(Axis::kTilt, CamLite::toUs(g_lite.cam.outTilt()));
}

}  // namespace lite
}  // namespace cq
