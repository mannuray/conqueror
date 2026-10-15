#include "domain/lite/cam_lite.h"
#include "domain/ui/catalogues.h"
#include "lite/lite_app.h"
#include "lite/lite_tasks.h"
#include "ports/clock.h"
#include "ports/led.h"
#include "ports/servos.h"
#include "ports/tone.h"

namespace cq {
namespace lite {

namespace {
constexpr uint8_t kLedMax = 50;  // brightness cap, as the old ELEGOO firmware and the full default (owner, §D4)
Rgb g_led_out{1, 1, 1};

int16_t absI(int16_t v) { return v < 0 ? static_cast<int16_t>(-v) : v; }

// Same inputs as the full uiTask (app/ui_task.cpp); Lite has no calibration, Override, bump or parking beeps.
AnnState annState() {
  AnnState s{};
  s.synced = g_lite.link.synced();
  s.blocked = g_lite.safety.blocked;
  s.lifted = g_lite.tilt.lifted();
  s.tilted = g_lite.tilt.tilted();
  s.batt_low = g_lite.batt.level() == BatteryMonitor::Level::kLow;
  s.batt_crit = g_lite.batt.level() == BatteryMonitor::Level::kCritical;
  s.usb_power = g_lite.batt.level() == BatteryMonitor::Level::kUsb;
  const uint8_t shown = SelfTest::displayMask(g_lite.self_test.failMask(), s.usb_power);  // spec 02 §D6.3
  s.selftest_critical = g_lite.self_test.locked() && (shown & SelfTest::kCriticalMask) != 0;
  s.reversing = g_lite.fast.pwm_l < 0 && g_lite.fast.pwm_r < 0;
  s.forward = g_lite.fast.pwm_l > 0 && g_lite.fast.pwm_r > 0;
  s.owner = g_lite.arbitrator.owner();
  s.hint = g_lite.owner_hint;
  s.selftest_runs = g_lite.self_test.runs();
  s.selftest_failed = shown != 0;
  s.toggle_seq = g_lite.toggle_seq;
  s.toggle_on = g_lite.toggle_on;
  s.driving = g_lite.moving;
  const int16_t pl = absI(g_lite.fast.pwm_l), pr = absI(g_lite.fast.pwm_r);
  s.speed_pct = static_cast<uint8_t>(static_cast<int32_t>(pl > pr ? pl : pr) * 100 / 255);
  return s;
}
bool g_servos_attached = true;
}  // namespace

void playSound(proto::SoundId id, uint8_t prio, uint32_t now_ms) {
  Note n[SoundSequencer::kMaxNotes];
  const uint8_t count = soundNotes(id, n);
  g_lite.sound.play(n, count, prio, now_ms);
}

void uiTask() {
  const uint32_t now = port::clock::nowMs();
  g_lite.led.setMax(kLedMax);
  g_lite.annunciator.update(annState(), now, g_lite.led, g_lite.sound);
  uint16_t hz = 0;
  if (g_lite.sound.tick(now, hz)) {
    if (hz == 0) {
      port::tone::stop();
    } else {
      port::tone::play(hz);
    }
  }
  const Rgb c = g_lite.led.output(now);
  if (c != g_led_out) {
    port::led::write(c);
    g_led_out = c;
  }
}

void cameraTask() {
  g_lite.cam.tick(port::clock::nowMs());
  g_lite.cam.applyJog();
  g_lite.cam.step();  // smooth: at most 120 °/s
  const bool want = g_lite.cam.wantAttached();  // released after 2 s still (spec 02 §D5.4)
  if (want != g_servos_attached) {
    if (want) {
      port::servos::attach(Axis::kPan);
      port::servos::attach(Axis::kTilt);
    } else {
      port::servos::detach(Axis::kPan);
      port::servos::detach(Axis::kTilt);
    }
    g_servos_attached = want;
  }
  if (!want) return;
  port::servos::writeUs(Axis::kPan, CamLite::toUs(g_lite.cam.outPan()));
  port::servos::writeUs(Axis::kTilt, CamLite::toUs(g_lite.cam.outTilt()));
}

}  // namespace lite
}  // namespace cq
