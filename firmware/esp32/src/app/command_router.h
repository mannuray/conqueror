// Page commands -> controllers (bridge LLD R15.4): parse, STOP fast path, admission, one table of routes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/drive_controller.h"
#include "app/motion_controller.h"
#include "app/replies.h"
#include "app/settings_controller.h"
#include "app/tank_app_controller.h"
#include "app/telemetry_publisher.h"
#include "core/ws_codec.h"

namespace cq {
namespace app {

class CommandRouter {
 public:
  CommandRouter(DriveController& d, MotionController& m, SettingsController& s, TankAppController& t,
                TelemetryPublisher& tel, Replies& out)
      : m_drive(d), m_motion(m), m_settings(s), m_tank(t), m_tel(tel), m_out(out) {}
  void setHello(const char* fw, const char* (*sensor)()) {
    m_fw = fw;
    m_sensor = sensor;
  }
  void onText(int fd, const char* text, size_t len, uint32_t post_seq, uint32_t now);
  static bool routes(core::WsCmdKind kind);  // a handler exists (STOP is the fast path, before the table)

 private:
  using Handler = void (*)(CommandRouter& r, int fd, int32_t id, const core::WsCommand& c, uint32_t now);
  struct Route {
    core::WsCmdKind kind;
    Handler run;
  };
  static Handler find(core::WsCmdKind kind);
  static const Route kRoutes[];

  DriveController& m_drive;
  MotionController& m_motion;
  SettingsController& m_settings;
  TankAppController& m_tank;
  TelemetryPublisher& m_tel;
  Replies& m_out;
  const char* m_fw = "";
  const char* (*m_sensor)() = nullptr;
};

}  // namespace app
}  // namespace cq
