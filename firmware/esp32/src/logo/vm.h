// Tank Logo runner (LLD §8): runs a compiled Program in slices. It stops at every effect (the host does it and
// calls run() again), at the end of a slice's budget, at a new line in step mode, at the end, at STOP, or at a
// run-time error. It never knows who its host is: the RunController (on the tank) and the Simulator (planned path)
// both drive it. Fixed stacks; no recursion; no heap.
#pragma once

#include <stdint.h>

#include "logo/diag.h"
#include "logo/program.h"

namespace cq {
namespace logo {

// What the program can read, in Logo units (filled by the host before each run()).
struct Sensors {
  int32_t distance_milli = 999000;  // cm; 999 when nothing is seen
  bool obstacle = false;
  bool blocked = false;             // the last move was cut short by the obstacle brake
  int32_t heading_milli = 0;        // degrees since the start, -180..180, right = +
  bool tilted = false;
  int32_t battery_milli = 100000;   // %
  int32_t timer_milli = 0;          // seconds since the start
  bool caught = false;              // game mode: the person has caught the tank (CAUGHT?, bridge LLD R34)
};

enum class VmStatus : uint8_t { kYield, kLine, kEffect, kHalt, kStop, kError };

struct Effect {
  EffectKind kind = EffectKind::kBeep;
  uint8_t n = 0;
  Value args[3];
  uint16_t line = 0;
  uint8_t sub = 0;  // kAsk: AskQ; kSkill: SkillKind
  uint16_t entry = 0;  // kLaunch: where the launched block's code starts (Tank Logo LLD §26.4)
};

struct RunError {
  uint16_t code = 0;
  uint16_t line = 0;
  char msg[192] = "";
};

class Vm {
 public:
  static constexpr uint8_t kMaxFrames = gen::kNesting + 1;  // the program's own frame + 100 calls (7011)
  static constexpr uint16_t kMaxValues = 1024;  // 100 calls of an 8-input word reach 7011 first (N3)
  static constexpr uint8_t kMaxLoops = 128;

  // From the program's start, or (a launched part, Tank Logo LLD §26.4) from `entry`: its block ends with HALT.
  void load(const Program& p, uint32_t seed, uint16_t entry = 0);
  // A launched part started inside a word of your own sees that word's inputs as they are now (copies; LLD §26.3).
  void inheritInputs(const Vm& from);
  void setStepping(bool on) { m_stepping = on; }
  VmStatus run(uint16_t budget, const Sensors& s);
  const Effect& effect() const { return m_effect; }
  const RunError& error() const { return m_error; }
  uint16_t line() const { return m_line; }
  int32_t round() const;      // REPCOUNT of the innermost running REPEAT / FOREVER, 0 outside any
  bool sensorRead() const { return m_sensor_read; }  // a sensor or RANDOM was read (planned path uncertain)
  uint32_t executed() const { return m_executed; }
  // Run-time errors from the host (an effect refused at run time: ranges, types, arc too tight).
  void fail(uint16_t code, const DiagArg* a, uint8_t n);
  // The result of the last effect (kAsk), which the next kResume pushes. A question is a read of the world: the
  // planned path is uncertain after it.
  void resume(Value v) {
    m_result = v;
    m_sensor_read = true;
  }

 private:
  friend struct VmOps;
  struct Frame {
    uint16_t ret_pc;
    uint16_t args_base;
    int8_t proc;  // -1: the program itself
    uint8_t loop_base;
  };
  struct Loop {
    int32_t count;  // -1: FOREVER
    int32_t round;
  };

  bool push(Value v);
  Value pop();  // 7014 on an empty stack (never reads past it)
  void signal(VmStatus s) {
    if (m_done != VmStatus::kError) m_signal = s;  // an error in the same step wins
  }
  void error(uint16_t code, const DiagArg* a = nullptr, uint8_t n = 0);
  void typeError(const char* word, const char* type, Value got);
  void leaveFrame();

  const Program* m_p = nullptr;
  const Sensors* m_sensors = nullptr;
  uint16_t m_pc = 0;
  uint16_t m_line = 0;
  bool m_stepping = false;
  bool m_sensor_read = false;
  uint32_t m_rng = 1;
  uint32_t m_executed = 0;
  VmStatus m_signal = VmStatus::kYield;
  VmStatus m_done = VmStatus::kYield;  // kHalt / kStop / kError once finished
  Value m_stack[kMaxValues];
  uint16_t m_sp = 0;
  Frame m_frames[kMaxFrames];
  uint8_t m_nf = 0;
  Loop m_loops[kMaxLoops];
  uint8_t m_nl = 0;
  Effect m_effect;
  Value m_result;
  RunError m_error;
};

}  // namespace logo
}  // namespace cq
