// Tank Logo planned path (LLD §11): runs the same Vm on a compiled program with ideal motion - moves and turns
// happen exactly, sensors are fixed (DISTANCE 999, OBSTACLE? / BLOCKED? / TILTED? false, BATTERY 100), TIMER is
// the simulated time. Every point after a sensor or RANDOM was read is `uncertain` (drawn dashed). Stops at 300 s
// simulated, 200 000 instructions or a full path buffer ("path cut short"). Pure; the caller owns the buffer.
#pragma once

#include <stdint.h>

#include "logo/effects.h"
#include "logo/program.h"
#include "logo/vm.h"

namespace cq {
namespace logo {

// Start at (0, 0) facing +y; x to the right; heading in degrees, right = +.
struct PathPoint {
  int32_t x_mm = 0;
  int32_t y_mm = 0;
  int16_t heading_deg = 0;
  uint16_t line = 0;
  bool uncertain = false;
};

struct PathBuffer {
  PathPoint* pts;
  uint16_t cap;
  uint16_t n = 0;
};

struct SimResult {
  bool cut_short = false;  // a limit was reached before the program ended
  uint16_t code = 0;       // a run-time error (0: none; STOP ends the path like the end of the program)
  uint16_t line = 0;
  char msg[192] = "";
  uint32_t sim_ms = 0;
};

class Simulator {
 public:
  static constexpr uint32_t kMaxInstructions = 200000;
  static constexpr uint32_t kTickMs = 100;     // one WAIT UNTIL check
  static constexpr double kTurnDegPerS = 90.0;  // spin speed of the model (time only)

  explicit Simulator(Vm& vm) : m_vm(vm) {}
  SimResult plan(const Program& p, PathBuffer& out, uint32_t seed = 1);

 private:
  struct Pose {
    double x = 0, y = 0, h = 0;
  };
  bool step(SimResult& r);  // false when the path is over
  bool effect(SimResult& r);
  void apply(const Action& a);
  bool moves(const Action& a);
  void move(int32_t cm);
  void turn(double deg);
  void around(double radius_mm, double deg);  // along a circle; deg > 0 = right (clockwise)
  void point(uint16_t line);
  Sensors sensors() const;
  double cmPerS() const;

  Vm& m_vm;
  const Program* m_p = nullptr;
  PathBuffer* m_out = nullptr;
  Pose m_pose;
  uint32_t m_ms = 0;
  uint32_t m_until = 0;
  uint8_t m_speed = 40;
  uint16_t m_line = 0;
  bool m_full = false;
};

}  // namespace logo
}  // namespace cq
