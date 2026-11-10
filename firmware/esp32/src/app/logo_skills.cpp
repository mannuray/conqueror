// LogoController: the AI words (Plan Z2a, bridge LLD R26.4, Tank Logo LLD §24.2) - questions and skills through the
// SkillHub, places relative to the tank, and every end of the drive lease. Every end of a live lease goes through
// endLease() once: the hub forgets it, skill_end tells the server why (unless it ended the skill itself or is gone),
// the skill's drive stops and the holder loses the camera. Stale answers / ends are the hub's to spot (by id + fd).
#include "app/logo_controller.h"
#include "logo/relative.h"
#include "logo/words.h"

namespace cq {
namespace app {
namespace {

// skill_end's why for an event that ends the program (bridge LLD R26.1).
core::LeaseEnd leaseEndOf(core::MotionEvent e) {
  switch (e) {
    case core::MotionEvent::kStop: case core::MotionEvent::kCancel: return core::LeaseEnd::kStop;
    case core::MotionEvent::kOwnerClosed: return core::LeaseEnd::kPage;
    case core::MotionEvent::kRelease: case core::MotionEvent::kOwnerCommand: return core::LeaseEnd::kOwner;
    default: return core::LeaseEnd::kEnded;
  }
}

// A skill's drive ended on the tank's side (the motion layer): skill_end's why and how the program ends - as a move
// would (kByEndReason's codes), except blocked (BLOCKED? may go on) and the deadman: no skill_drive for 300 ms is the
// helper not answering (7052, Review Focus 3: a WiFi drop is only ever seen by the deadman).
struct SkillDriveEnd {
  core::LeaseEnd why;
  bool by_skill_end;  // true: RunController::onSkillEnd(skill); false: onMotionEvent(event)
  logo::SkillEnd skill;
  core::MotionEvent event;
};
constexpr SkillDriveEnd kBySkillDrive[] = {
    {core::LeaseEnd::kEnded, true, logo::SkillEnd::kDone, core::MotionEvent::kStop},      // kDone (not seen)
    {core::LeaseEnd::kStop, false, logo::SkillEnd::kDone, core::MotionEvent::kCancel},    // kCancelled (waits)
    {core::LeaseEnd::kStop, false, logo::SkillEnd::kDone, core::MotionEvent::kStop},      // kStop (waits)
    {core::LeaseEnd::kOwner, false, logo::SkillEnd::kDone, core::MotionEvent::kRelease},  // kNotOwner 7045
    {core::LeaseEnd::kEnded, false, logo::SkillEnd::kDone, core::MotionEvent::kBodyStall},     // kStalled 7044
    {core::LeaseEnd::kEnded, false, logo::SkillEnd::kDone, core::MotionEvent::kBodyStall},     // kTimeout 7044
    {core::LeaseEnd::kEnded, false, logo::SkillEnd::kDone, core::MotionEvent::kBodyOffline},   // kLink 7034
    {core::LeaseEnd::kDeadman, true, logo::SkillEnd::kStale, core::MotionEvent::kStop},        // kDeadman 7052
    {core::LeaseEnd::kEnded, false, logo::SkillEnd::kDone, core::MotionEvent::kBodyStall},     // kDiverged 7044
    {core::LeaseEnd::kEnded, true, logo::SkillEnd::kBlocked, core::MotionEvent::kStop},        // kBlocked 7024
    {core::LeaseEnd::kEnded, false, logo::SkillEnd::kDone, core::MotionEvent::kBodyEncoderFault},  // 7021
};
static_assert(sizeof kBySkillDrive / sizeof kBySkillDrive[0] == static_cast<size_t>(core::EndReason::kEncoderFault) + 1,
              "one per EndReason");

}  // namespace

void LogoController::carryAi(const logo::Action& a, uint32_t now) {
  if (a.kind == logo::ActKind::kAsk) return ask(a, now);
  if (a.kind == logo::ActKind::kSkill) return startSkill(a, now);
  speedWhileGoing(a, now);  // SPEED while a GO drives on; WAIT / WAIT UNTIL / AREA: nothing to do here
}

// SEE? / HOWFAR: the question goes to the server (none: the runner's 300 ms give FALSE / -1).
void LogoController::ask(const logo::Action& a, uint32_t now) {
  if (m_hub == nullptr) return;
  const int32_t id = m_hub->ask(a.ask_q, a.thing, now);
  const bool game = m_prog.game && logo::isPersonWord(a.thing);  // the engine answers from PersonWatch (§27)
  if (id >= 0) m_out.to(m_hub->server(), core::jsonSkillAsk(m_out.buf(), m_out.cap(), id, a.ask_q, a.thing, game));
}

// FIND / WANDER / EXPLORE / SETXY: a GO is held first (startSkill replaces a primitive without its end; HOLD does
// nothing when no GO drives), the drive primitive starts, then the lease goes to the server with skill_start - its
// places (SETXY's point, the AREA's anchor) turned from the run's frame into the tank's now.
void LogoController::startSkill(const logo::Action& a, uint32_t now) {
  if (m_hub == nullptr || m_hub->server() < 0) return handle(m_rc.onSkillEnd(logo::SkillEnd::kStale, now), now);
  logo::Action hold;
  hold.kind = logo::ActKind::kHold;
  core::BridgeReason why = core::BridgeReason::kBodyOffline;
  m_h.start_motion(m_fd, hold, now, why);  // never refused
  const int32_t id = m_hub->newId();
  if (!m_h.start_skill(m_fd, id, now, why)) return refused(why, now);
  int32_t replaced = -1;
  m_hub->start(id, now, &replaced);
  if (replaced >= 0) {  // never while every end goes through endLease - but a lease is never replaced silently
    m_out.to(m_hub->server(), core::jsonSkillEnd(m_out.buf(), m_out.cap(), replaced, core::LeaseEnd::kEnded));
  }
  m_skill_id = id;
  const logo::Pose p = m_odo.pose();
  const logo::FwdLeft to = logo::relative(p, a.x_cm * 10, a.y_cm * 10);
  const logo::FwdLeft area = a.has_area ? logo::relative(p, a.area_at.x_mm, a.area_at.y_mm) : logo::FwdLeft();
  m_out.to(m_hub->server(), core::jsonSkillStart(m_out.buf(), m_out.cap(), id, a, to, area));
}

// The one way a live lease ends. tell: send skill_end (false when the server ended the skill or is gone).
void LogoController::endLease(bool tell, core::LeaseEnd why, core::EndReason motion, uint32_t now) {
  if (m_skill_id < 0) return;
  const int32_t id = m_skill_id;
  m_skill_id = -1;  // first: the skill's own end calls back into onPrimitiveEnd
  if (m_hub != nullptr) {
    if (m_hub->leaseId() == id) m_hub->endLease();
    if (tell) m_out.to(m_hub->server(), core::jsonSkillEnd(m_out.buf(), m_out.cap(), id, why));
  }
  if (m_h.end_skill != nullptr) m_h.end_skill(motion, now);
}

void LogoController::onSkillAnswer(int fd, int32_t id, int32_t value, uint32_t now) {
  if (!m_running || m_hub == nullptr || !m_hub->answerFor(fd, id, now)) return;  // late, old, or not the server's
  m_hub->clearAsk();
  handle(m_rc.onAskAnswer(value, now), now);
}

void LogoController::onSkillEnd(int fd, int32_t id, logo::SkillEnd e, uint32_t now) {
  if (!m_running || m_hub == nullptr || id != m_skill_id || !m_hub->endFor(fd, id)) return;
  endLease(false, core::LeaseEnd::kEnded,
           e == logo::SkillEnd::kDone ? core::EndReason::kDone : core::EndReason::kCancelled, now);
  handle(m_rc.onSkillEnd(e, now), now);
}

void LogoController::onClientClose(int fd, uint32_t now) {
  if (m_hub == nullptr || fd < 0 || fd != m_hub->server()) return;
  const int32_t lease = m_hub->onClose(fd);  // also drops its question; its things empty at the next commit
  if (lease < 0 || lease != m_skill_id || !m_running) return;
  endLease(false, core::LeaseEnd::kEnded, core::EndReason::kCancelled, now);
  handle(m_rc.onSkillEnd(logo::SkillEnd::kStale, now), now);  // 7052
}

void LogoController::onMotionEvent(core::MotionEvent e, uint32_t now) {
  if (!m_running) return;
  endLease(true, leaseEndOf(e), core::EndReason::kCancelled, now);  // nothing without a live lease
  handle(m_rc.onMotionEvent(e), now);
}

// A skill's drive primitive ended. Only the live lease's: our own endLease() cleared m_skill_id before it. STOP /
// cancel wait for the MotionEvent behind them (MotionController::end sends it next: page / owner / stop); if none
// came, the server's frames are refused from now on and the hub's deadman ends the lease.
void LogoController::skillPrimitiveEnded(const core::PrimitiveEnd& e, uint32_t now) {
  if (!m_running || m_skill_id < 0 || e.id != m_skill_id) return;
  if (e.reason == core::EndReason::kDone || e.reason == core::EndReason::kCancelled ||
      e.reason == core::EndReason::kStop) {
    return;
  }
  const SkillDriveEnd& row = kBySkillDrive[static_cast<uint8_t>(e.reason)];
  endLease(true, row.why, e.reason, now);
  handle(row.by_skill_end ? m_rc.onSkillEnd(row.skill, now) : m_rc.onMotionEvent(row.event), now);
}

// Each loop: the lease's deadman (7052 with skill_end deadman) and a server reaped without a close (ghost path).
void LogoController::tickHub(uint32_t now) {
  if (m_hub == nullptr) return;
  const int32_t lapsed = m_hub->tick(now);
  if (lapsed >= 0 && lapsed == m_skill_id && m_running) {
    endLease(true, core::LeaseEnd::kDeadman, core::EndReason::kDeadman, now);
    handle(m_rc.onSkillEnd(logo::SkillEnd::kStale, now), now);
  }
  const int server = m_hub->server();
  if (server >= 0 && !m_out.clients().contains(server)) onClientClose(server, now);
}

}  // namespace app
}  // namespace cq
