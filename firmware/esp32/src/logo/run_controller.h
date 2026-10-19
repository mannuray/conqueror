// Tank Logo RunController (LLD §9): one running program on the tank - its states, its slices of the Vm, the
// actions it asks for, and every way it ends. Bound to the bridge's motion layer (core::PrimitiveEnd,
// core::MotionEvent), so Plan N3 only wires it into the link task. Pure; the host carries out each RunOut and stops
// the tank on every ending (motion service STOP, or BodyLink STOP when no primitive runs - LLD §9).
#pragma once

#include <stdint.h>

#include "core/motion_link.h"
#include "core/motion_service.h"
#include "logo/effects.h"
#include "logo/program.h"
#include "logo/vm.h"

namespace cq {
namespace logo {

// What the tank reports, in its own units (from TELEM_FAST / TELEM_SLOW, each tick).
struct RunTelem {
  uint16_t yaw_cdeg = 0;     // 0..35999, + = left
  uint16_t distance_mm = 0;  // ultrasonic ahead; 0 = nothing seen
  bool tilted = false;
  uint8_t battery_pct = 100;
};

struct RunConfig {
  bool step = false;          // step mode: pause at each new line (logo_step goes on)
  uint32_t seed = 1;          // RANDOM
  uint16_t obstacle_mm = 150; // OBSTACLE?: closer than the obstacle-stop distance
  uint16_t budget = 500;      // instructions per tick (LLD §16)
};

enum class RunState : uint8_t { kIdle, kRunning, kMotion, kWaiting, kPaused, kEnded };
enum class RunEvent : uint8_t { kLogoStop, kStep, kBatteryLow, kTilt };

// What the host does now: at most one action, a line event, or the ending.
struct RunOut {
  bool act = false;  // carry out `action` (kSpeed / kUntilStart are for the trace only)
  Action action;
  bool line = false;  // logo_line {line_no, round}
  uint16_t line_no = 0;
  int32_t round = 0;
  bool ended = false;  // logo_ended: stop the tank, release the driver; code 0 = finished
  uint16_t code = 0;
  uint16_t end_line = 0;
  char msg[192] = "";
};

class RunController {
 public:
  explicit RunController(Vm& vm) : m_vm(vm) {}
  bool start(const Program& p, const RunConfig& cfg, const RunTelem& t, uint32_t now_ms);  // false: not compiled OK
  RunOut tick(const RunTelem& t, uint32_t now_ms);
  RunOut onPrimitiveEnd(const core::PrimitiveEnd& e);  // the move / turn / arc / pivot it asked for ended
  RunOut onMotionEvent(core::MotionEvent e);           // STOP, release, page gone … (also between motions)
  RunOut onEvent(RunEvent e);
  RunState state() const { return m_state; }
  uint32_t wakeAt() const { return m_wake; }  // kWaiting: when WAIT / PLAY / a WAIT UNTIL tick is over

 private:
  void step(const RunTelem& t, uint32_t now_ms, RunOut& out);
  void effect(const RunTelem& t, uint32_t now_ms, RunOut& out);
  void carry(const Action& a, const RunTelem& t, uint32_t now_ms, RunOut& out);
  Sensors sensors(const RunTelem& t, uint32_t now_ms) const;
  RunOut end(uint16_t code);
  RunOut endWith(uint16_t code, uint16_t line, const char* msg);
  bool active() const { return m_state != RunState::kIdle && m_state != RunState::kEnded; }

  Vm& m_vm;
  const Program* m_p = nullptr;
  RunConfig m_cfg;
  RunState m_state = RunState::kIdle;
  uint32_t m_start = 0;
  uint32_t m_wake = 0;
  uint16_t m_start_yaw = 0;
  uint8_t m_speed = 40;
  bool m_blocked = false;
  EffectKind m_last = EffectKind::kBeep;  // the last effect (its word for 7021)
};

}  // namespace logo
}  // namespace cq
