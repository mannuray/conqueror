// The Tank tab and the AI address: tank, log, camera_set, selftest, app_set (bridge LLD R13, R15.4).
#pragma once

#include <stdint.h>

#include "app/replies.h"
#include "core/body_link.h"
#include "core/cam_settings.h"
#include "core/event_log.h"
#include "core/ws_codec.h"

namespace cq {
namespace app {

class TankAppController {
 public:
  struct SelfTestHooks {  // until UnoChannel / DriveController own these (Plan P Tasks 10-11)
    bool (*body_offline)();
    bool (*moving)();                  // driving or a primitive running: the tank must stand still
    bool (*queue)(int fd, int32_t id);  // false: the Uno request queue is full
  };

  TankAppController(Replies& out, core::EventLog& log, const core::BodyLink& link)
      : m_out(out), m_log(log), m_link(link) {}
  void begin();  // loads the AI address
  void setSelfTestHooks(const SelfTestHooks& h) { m_hooks = h; }
  void onTank(int fd, uint32_t now);
  void onLog(int fd, uint32_t since, uint32_t now);
  void onCameraSet(int fd, int32_t id, const core::WsCommand& c, uint32_t now);
  void onAppSet(int fd, int32_t id, const core::WsCommand& c);
  void onSelfTest(int fd, int32_t id);
  const char* aiUrl() const { return m_ai_url; }
  core::CamSettings merge(const core::CamFields& f) const;  // the settings in force (or the defaults) + the sent fields

 private:
  void sendTank(int fd, uint32_t now, const core::CamSettings& cs);

  Replies& m_out;
  core::EventLog& m_log;
  const core::BodyLink& m_link;
  SelfTestHooks m_hooks{nullptr, nullptr, nullptr};
  char m_ai_url[97] = "";
};

}  // namespace app
}  // namespace cq
