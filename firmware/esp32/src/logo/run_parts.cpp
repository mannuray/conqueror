// RunController: the two parts (Tank Logo LLD §26.4) - turns, the questions' one-at-a-time rule, LAUNCH and what
// only the main program may do. The rest of the runner is run_controller.cpp.
#include "logo/generated/words.h"
#include "logo/run_controller.h"

namespace cq {
namespace logo {
// One part's turn, then (when it had nothing for the host) the other's: at most one RunOut a tick. The parts take
// turns going first, so neither waits for the other's every step (LLD §26.3 rule 5).
RunOut RunController::tick(const RunTelem& t, uint32_t now_ms) {
  RunOut out;
  if (!active()) return out;
  if (now_ms - m_start >= static_cast<uint32_t>(gen::kRunTimeS) * 1000u) return end(7041);
  m_first = static_cast<uint8_t>(1 - m_first);
  for (uint8_t k = 0; k < 2 && !(out.act || out.line || out.ended); ++k) {
    m_cur = static_cast<uint8_t>((m_first + k) % 2);
    const bool paused = m_cur == 1 && m_parts[0].state == RunState::kPaused;  // STEP pauses both (rule 7)
    if (!paused && runs(m_cur)) slice(t, now_ms, out);
  }
  m_cur = 0;
  return out;
}

bool RunController::ready() const {
  for (const Part& p : m_parts) {
    if (p.state == RunState::kRunning) return true;
  }
  return false;
}

bool RunController::runs(uint8_t i) const {
  const RunState s = m_parts[i].state;
  return s == RunState::kRunning || s == RunState::kWaiting || s == RunState::kAsking;
}

void RunController::slice(const RunTelem& t, uint32_t now_ms, RunOut& out) {
  Part& p = cur();
  if (p.ask_held) {  // a question paced to ~10 Hz (and one at a time) goes out now (its 300 ms start here)
    if (static_cast<int32_t>(now_ms - m_next_ask) < 0 || otherAsking()) return;
    sendAsk(now_ms, out);
    return;
  }
  if (!due(now_ms)) return;
  step(t, now_ms, out);
  out.part = m_cur;
}

bool RunController::otherAsking() const {
  const Part& o = m_parts[1 - m_cur];
  return o.state == RunState::kAsking && !o.ask_held;
}

uint32_t RunController::wakeAt() const {
  uint32_t w = 0;
  for (const Part& p : m_parts) {
    if (p.state == RunState::kWaiting && (w == 0 || static_cast<int32_t>(p.wake - w) < 0)) w = p.wake;
  }
  return w;
}

// The answer is the asking part's: the one whose question is out (one at a time, LLD §26.4).
RunOut RunController::onAskAnswer(int32_t value, uint32_t now_ms) {
  for (m_cur = 0; m_cur < 2; ++m_cur) {
    const Part& p = cur();
    const bool out = p.state == RunState::kAsking && !p.ask_held && static_cast<int32_t>(now_ms - p.ask_deadline) < 0;
    if (out) {
      answer(value);
      break;
    }
  }
  m_cur = 0;
  return RunOut();
}

// What only the main program may do: move the tank or its camera, set where it may go, a skill (LLD §26.3).
bool RunController::mainOnly(ActKind k) const {
  return waitsForMotion(k) || k == ActKind::kGo || k == ActKind::kHold || k == ActKind::kSpeed || k == ActKind::kLook ||
         k == ActKind::kLookHome || k == ActKind::kSkill || k == ActKind::kArea;
}

// LAUNCH: the second part starts at the block's entry; one at a time (7058).
bool RunController::launch(const Action& a) {
  Part& l = m_parts[1];
  if (m_cur == 1 || (l.state != RunState::kIdle && l.state != RunState::kEnded)) return false;
  Vm* vm = l.vm;
  l = Part();
  l.vm = vm;
  vm->load(*m_p, m_cfg.seed + 1 + ++m_launches, a.entry);
  vm->inheritInputs(*m_parts[0].vm);  // inside a word of your own: its inputs as they are now
  vm->setStepping(false);  // STEP is the main part's; this part pauses while the main does
  l.state = RunState::kRunning;
  return true;
}

}  // namespace logo
}  // namespace cq
