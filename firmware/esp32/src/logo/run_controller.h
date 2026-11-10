// Tank Logo RunController (LLD §9): one running program on the tank - its states, its slices of the Vm, the
// actions it asks for, and every way it ends. Bound to the bridge's motion layer (core::PrimitiveEnd,
// core::MotionEvent), so Plan N3 only wires it into the link task. Pure; the host carries out each RunOut and stops
// the tank on every ending (motion service STOP, or BodyLink STOP when no primitive runs - LLD §9).
#pragma once

#include <stdint.h>

#include "core/motion_link.h"
#include "core/motion_service.h"
#include "logo/effects.h"
#include "logo/odometry.h"
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
  Pose pose;                 // the driven path so far (Odometry, the run's frame: x right, y ahead, right = +)
  bool caught = false;       // game mode (bridge LLD R34): the person has caught the tank
};

struct RunConfig {
  bool step = false;          // step mode: pause at each new line (logo_step goes on)
  uint32_t seed = 1;          // RANDOM
  uint16_t obstacle_mm = 150; // OBSTACLE?: closer than the obstacle-stop distance
  uint16_t budget = 500;      // instructions per tick (LLD §16)
};

// kAsking: a question (SEE? / HOWFAR) is out to the laptop; kSkill: a FIND / WANDER / EXPLORE / SETXY runs there.
enum class RunState : uint8_t { kIdle, kRunning, kMotion, kWaiting, kAsking, kSkill, kPaused, kEnded };
// How a skill ended on the laptop (Tank Logo LLD §24.2); tank-side endings come through onMotionEvent.
enum class SkillEnd : uint8_t { kDone, kNotFound, kLost, kOutsideArea, kNoWay, kBlocked, kUnknownThing, kStale, kLearning, kCaught };
enum class RunEvent : uint8_t { kLogoStop, kStep, kBatteryLow, kTilt };

// What the host does now: at most one action, a line event, or the ending.
struct RunOut {
  bool act = false;  // carry out `action` (kSpeed / kUntilStart are for the trace only)
  Action action;
  bool line = false;  // logo_line {line_no, round, part}
  uint8_t part = 0;   // 0 the main program, 1 the launched part (Tank Logo LLD §26.4)
  uint16_t line_no = 0;
  int32_t round = 0;
  bool ended = false;  // logo_ended: stop the tank, release the driver; code 0 = finished
  uint16_t code = 0;
  uint16_t end_line = 0;
  char msg[192] = "";
};

class RunController {
 public:
  // Two Vms over one Program: the main program and the LAUNCH part (Tank Logo LLD §26.4).
  RunController(Vm& main, Vm& launched) {
    m_parts[0].vm = &main;
    m_parts[1].vm = &launched;
  }
  bool start(const Program& p, const RunConfig& cfg, const RunTelem& t, uint32_t now_ms);  // false: not compiled OK
  RunOut tick(const RunTelem& t, uint32_t now_ms);
  RunOut onPrimitiveEnd(const core::PrimitiveEnd& e);  // the move / turn / arc / pivot it asked for ended
  RunOut onMotionEvent(core::MotionEvent e);           // STOP, release, page gone … (also between motions)
  RunOut onEvent(RunEvent e);
  RunOut abort(uint16_t code) { return end(code); }  // bridge LLD R34: game mode ends the run (7052 / 7060 / 7061)
  // The laptop's answer to the question just asked: SEE? 1 / 0, HOWFAR cm (-1 = not seen). Ignored when no question
  // is waiting or its 300 ms are over (the runner then goes on with FALSE / -1).
  RunOut onAskAnswer(int32_t value, uint32_t now_ms);
  // A skill ended on the laptop. Ignored unless a skill is running (an old end never touches the next word).
  RunOut onSkillEnd(SkillEnd e, uint32_t now_ms);
  static constexpr uint32_t kAskMs = 300;
  static constexpr uint32_t kAskGapMs = 100;  // a new question goes out at least this long after the last (~10 Hz)
  RunState state() const { return m_parts[0].state; }  // the main program's
  bool going() const { return m_going; }  // a GO drives on in the background (LLD §22): keep it alive each tick
  // The first moment a waiting part goes on (kWaiting: WAIT / PLAY / a WAIT UNTIL tick); 0 when none waits.
  uint32_t wakeAt() const;
  bool ready() const;  // a part can run now (not waiting for a clock, the laptop or the motion service)

 private:
  bool gameWordGoesOn(SkillEnd e) const;
  // One part's own state (Tank Logo LLD §26.4); moving, skills, GO, SPEED and AREA belong to the main part alone.
  struct Part {
    Vm* vm = nullptr;
    RunState state = RunState::kIdle;
    uint32_t wake = 0;
    AskQ ask_q = AskQ::kSee;
    uint32_t ask_deadline = 0;
    bool ask_held = false;  // the question waits for m_next_ask, or for the other part's question to be answered
    Action ask_action;
    EffectKind last = EffectKind::kBeep;  // the last effect (its word for 7021 / 7057)
    uint8_t last_sub = 0;
  };
  Part& cur() { return m_parts[m_cur]; }
  const Part& cur() const { return m_parts[m_cur]; }
  bool runs(uint8_t i) const;    // the part can take a slice now (not waiting on the host)
  void slice(const RunTelem& t, uint32_t now_ms, RunOut& out);  // the current part's turn
  bool otherAsking() const;      // the other part's question is out (one question at a time)
  bool launch(const Action& a);  // LAUNCH: false = one already runs (7058)
  bool mainOnly(ActKind k) const;
  void step(const RunTelem& t, uint32_t now_ms, RunOut& out);
  void effect(const RunTelem& t, uint32_t now_ms, RunOut& out);
  void carry(const Action& a, const RunTelem& t, uint32_t now_ms, RunOut& out);
  Sensors sensors(const RunTelem& t, uint32_t now_ms) const;
  uint16_t timeUp(uint32_t now_ms) const;
  RunOut goEnded(const core::PrimitiveEnd& e);
  RunOut end(uint16_t code);
  RunOut endWith(uint16_t code, uint16_t line, const char* msg);
  bool carryAi(const Action& a, const RunTelem& t, uint32_t now_ms, RunOut& out);  // kAsk / kArea / kSkill
  bool due(uint32_t now_ms);  // a waiting or asking state is over (an unanswered question gets FALSE / -1)
  void answer(int32_t value);
  void sendAsk(uint32_t now_ms, RunOut& out);  // the kAsk goes out now (see kAskGapMs)
  bool active() const { return m_parts[0].state != RunState::kIdle && m_parts[0].state != RunState::kEnded; }

  Part m_parts[2];
  uint8_t m_cur = 0;  // the part whose turn it is (0 outside a slice: events are the main program's)
  uint8_t m_first = 1;  // the part that went first last tick
  uint8_t m_launches = 0;  // LAUNCHes this run: each has its own RANDOM numbers
  const Program* m_p = nullptr;
  RunConfig m_cfg;
  uint32_t m_start = 0;
  uint16_t m_start_yaw = 0;
  uint8_t m_speed = 40;
  bool m_blocked = false;
  bool m_going = false;  // GO (LLD §22)
  uint32_t m_next_ask = 0;  // the earliest time the next question may go out (both parts)
  uint8_t m_area_m = 0;   // AREA (0 = none) and where it was set, in the run's frame
  Pose m_area_at;
  char m_thing[25] = "";  // the running skill's thing, for 7050 / 7051 / 7053 / 7054
  bool m_game_word = false;  // the running skill is FACE / ESCAPE / FOLLOW: lost / no_way end it, not the program
};

}  // namespace logo
}  // namespace cq
