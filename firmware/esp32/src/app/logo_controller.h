// A Tank Logo program running on the tank (Plan N3, Tank Logo LLD §20.3, §20.5): logo_run / logo_step / logo_stop,
// the RunController's slices each tick, its actions carried out (moves through MotionController, light / sound /
// tune / camera through UnoChannel, waiting for the Uno's ACK; SAY to the pages), the driven pose, and every ending
// - which stops the tank and gives the drive back. Link task only. Hooks are captureless functions (no virtual).
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
  };
  static constexpr uint32_t kSayPauseMs = 200;  // design-notes §102
  static constexpr uint32_t kPoseMs = 100;      // logo_pose at 10 Hz
  static constexpr uint8_t kToneChunk = 10;     // TONE frame: at most 10 notes

  LogoController(Replies& out, core::EventLog& log, core::BodyLink& link, TelemetryPublisher& tel,
                 logo::Program& running, logo::Vm& vm)
      : m_out(out), m_log(log), m_link(link), m_tel(tel), m_prog(running), m_rc(vm) {}
  void setHooks(const Hooks& h) { m_h = h; }
  void onRun(int fd, int32_t id, uint32_t prog, bool step, uint32_t now);  // logo_run
  void onStep(int fd, int32_t id);
  void onStop(int fd, int32_t id, uint32_t now);
  void tick(uint32_t now);
  void onPrimitiveEnd(const core::PrimitiveEnd& e, uint32_t now);
  void onMotionEvent(core::MotionEvent e, uint32_t now);
  void onUnoAlarm(logo::RunEvent e, uint32_t now);  // battery critical, tilted / lifted
  void onUnoDone(uint8_t status, uint32_t now);      // the answer to the program's request (UnoChannel)
  int fd() const { return m_running ? m_fd : -1; }
  bool running() const { return m_running; }

 private:
  void keepGoing(uint32_t now);
  void speedWhileGoing(const logo::Action& a, uint32_t now);
  // logo_controller.cpp
  bool runRefused(int fd, int32_t id, uint32_t prog, uint32_t now);
  void handle(const logo::RunOut& o, uint32_t now);
  void finish(const logo::RunOut& o, uint32_t now);
  void pose(uint32_t now);
  logo::RunTelem telem() const;
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
};

}  // namespace app
}  // namespace cq
