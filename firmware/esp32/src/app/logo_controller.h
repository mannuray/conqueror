// A Tank Logo program running on the tank (Plan N3, Tank Logo LLD §20.3, §20.5): logo_run / logo_step / logo_stop,
// the RunController's slices each tick, its actions carried out (moves through MotionController, light / sound /
// tune / camera through UnoChannel, waiting for the Uno's ACK; SAY to the pages), the driven pose, and every ending
// - which stops the tank and gives the drive back. Plan Z2a (bridge LLD R26.4, Tank Logo LLD §24.2): the AI words -
// questions and skills through the SkillHub, places relative to the tank, and every end of the drive lease.
// Link task only. Hooks are captureless functions (no virtual).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/replies.h"
#include "app/telemetry_publisher.h"
#include "app/uno_channel.h"
#include "core/acked_ops.h"
#include "core/body_link.h"
#include "core/event_log.h"
#include "core/link_owner.h"
#include "core/motion_link.h"
#include "core/skill_hub.h"
#include "core/ws_json_skill.h"
#include "logo/odometry.h"
#include "logo/program.h"
#include "logo/run_controller.h"
#include "logo/vm.h"

namespace cq {
namespace app {

class LogoController {
 public:
  enum class Took : uint8_t { kOk, kCheckAgain, kBusy };
  struct Hooks {
    core::Decision (*claim)(int fd, uint32_t now) = nullptr;  // take the drive like a manual drive
    // Still this page's drive? Also keeps it (the 5 s handback, like a running primitive) - LinkOwner::keepDriving.
    core::Decision (*keep)(int fd, uint32_t now) = nullptr;
    bool (*ota)() = nullptr;
    Took (*take)(uint32_t prog, logo::Program& out, char* name, size_t cap) = nullptr;  // the checked program
    bool (*start_motion)(int fd, const logo::Action& a, uint32_t now, core::BridgeReason& why) = nullptr;
    // Ends a running primitive; a STOP frame only while the program still owns the drive (bridge LLD R4.6).
    void (*stop_tank)(bool owns, uint32_t now) = nullptr;
    void (*release)(int fd, uint32_t now) = nullptr;
    bool (*uno_request)(const core::AckedOp& op) = nullptr;  // false: the queue is full (tried again next tick)
    uint16_t (*cpm)() = nullptr;
    void (*keep_go)(uint32_t now) = nullptr;  // LLD §22: a GO drives on while the program keeps it alive
    // Plan Z2a (bridge LLD R26.4): a skill word's drive primitive under lease `id` (MotionController::startSkill)
    // and the camera for the lease's holder (LinkOwner::setLeaseHolder). False with why as start_motion.
    bool (*start_skill)(int fd, int32_t id, uint32_t now, core::BridgeReason& why) = nullptr;
    // Every end of the lease: the skill's drive ends (a STOP while the program owns the body; kDone when the
    // server finished it) and the holder loses the camera (LinkOwner::clearLeaseHolder).
    void (*end_skill)(core::EndReason why, uint32_t now) = nullptr;
    // Plan G1 (bridge LLD R34): game mode. game_start: the run's checker flag; 0 to go, else the code it ends with
    // (7060 / 7061). game_ready: false while arming (the program waits). game_caught: CAUGHT?. game_end: the run
    // ended. game_backing: a backward move's length, before it starts (back_ok).
    uint16_t (*game_start)(bool game, uint32_t now) = nullptr;
    bool (*game_ready)() = nullptr;
    bool (*game_caught)() = nullptr;
    void (*game_end)(uint32_t now) = nullptr;
    void (*game_backing)(int16_t cm) = nullptr;
  };
  static constexpr uint32_t kSayPauseMs = 200;  // design-notes §102
  static constexpr uint32_t kPoseMs = 100;      // logo_pose at 10 Hz
  static constexpr uint8_t kToneChunk = 10;     // TONE frame: at most 10 notes

  LogoController(Replies& out, core::EventLog& log, core::BodyLink& link, TelemetryPublisher& tel,
                 logo::Program& running, logo::Vm& vm, logo::Vm& launched)
      : m_out(out), m_log(log), m_link(link), m_tel(tel), m_prog(running), m_rc(vm, launched) {}
  void setHooks(const Hooks& h) { m_h = h; }
  void onRun(int fd, int32_t id, uint32_t prog, bool step, uint32_t now);  // logo_run
  void onStep(int fd, int32_t id);
  void onStop(int fd, int32_t id, uint32_t now);
  void tick(uint32_t now);
  void onPrimitiveEnd(const core::PrimitiveEnd& e, uint32_t now);
  void onMotionEvent(core::MotionEvent e, uint32_t now);
  void onUnoAlarm(logo::RunEvent e, uint32_t now);  // battery critical, tilted / lifted
  void onUnoDone(uint8_t status, uint32_t now);      // the answer to the program's request (UnoChannel)
  // Plan Z2a: the link task's SkillHub (its questions, the lease, its deadman - tick() runs hub->tick each loop).
  void setSkillHub(core::SkillHub* hub) { m_hub = hub; }
  // The server's skill_answer / skill_done / skill_failed: anything the hub says is not the current question or
  // lease (an old id, another fd, too late) is dropped - it never answers or ends the next word.
  void onSkillAnswer(int fd, int32_t id, int32_t value, uint32_t now);
  void onSkillEnd(int fd, int32_t id, logo::SkillEnd e, uint32_t now);
  // A WebSocket closed (before DriveController's close): the skill server's close ends a running skill 7052.
  void onClientClose(int fd, uint32_t now);
  int fd() const { return m_running ? m_fd : -1; }
  bool running() const { return m_running; }
  void endWith(uint16_t code, uint32_t now);  // game mode ends the running program (held 7052, arming 7060)

 private:
  void keepGoing(uint32_t now);
  bool holding(uint32_t now) const;
  void pauseGo(uint32_t now);
  void speedWhileGoing(const logo::Action& a, uint32_t now);
  // logo_controller.cpp
  bool runRefused(int fd, int32_t id, uint32_t prog, uint32_t now);
  void handle(const logo::RunOut& o, uint32_t now);
  void finish(const logo::RunOut& o, uint32_t now);
  void pose(uint32_t now);
  logo::RunTelem telem() const;
  // logo_skills.cpp (Plan Z2a)
  void carryAi(const logo::Action& a, uint32_t now);
  void ask(const logo::Action& a, uint32_t now);
  void startSkill(const logo::Action& a, uint32_t now);
  void refused(core::BridgeReason why, uint32_t now);
  void endLease(bool tell, core::LeaseEnd why, core::EndReason motion, uint32_t now);
  void skillPrimitiveEnded(const core::PrimitiveEnd& e, uint32_t now);
  void tickHub(uint32_t now);
  // logo_actions.cpp
  void carry(const logo::Action& a, uint32_t now);
  void motion(const logo::Action& a, uint32_t now);
  void request(core::AckedOp op);
  bool pendingSent();
  void startTone(const logo::Action& a, uint32_t now);
  void toneChunk(uint32_t now);
  void note(const logo::Action& a);

  Replies& m_out;
  core::EventLog& m_log;
  core::BodyLink& m_link;
  TelemetryPublisher& m_tel;
  logo::Program& m_prog;
  logo::RunController m_rc;
  logo::Odometry m_odo;
  Hooks m_h;
  bool m_running = false;
  int m_fd = -1;
  char m_name[25] = "";
  bool m_wait_ack = false;
  bool m_has_pending = false;
  core::AckedOp m_pending;
  uint32_t m_hold_until = 0;  // SAY's pause
  uint32_t m_last_pose = 0;
  logo::Note m_tone[logo::NoteReader::kMaxNotes];
  uint8_t m_tone_n = 0, m_tone_i = 0;
  uint32_t m_tone_next = 0;
  bool m_paused_go = false;  // step mode: a GO held while paused on a line (owner 2026-10-22)
  bool m_hold_end = false;   // ... and its HOLD's own end, which is not the program's
  uint8_t m_go_speed = 40;
  core::SkillHub* m_hub = nullptr;
  int32_t m_skill_id = -1;  // the live lease (-1: none); cleared before anything that can call back in
};

}  // namespace app
}  // namespace cq
