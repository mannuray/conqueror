// ArduinoOTA with a password (bridge LLD R2.5, R2.14 #3). Polled only while nobody drives.
#pragma once

#include <stdint.h>

namespace cq {
namespace net {

class OtaService {
 public:
  using Hook = void (*)();
  void begin();
  void addStartHook(Hook hook);  // e.g. stop the camera (Task 8)
  void poll(bool allowed);       // ArduinoOTA.handle() only when allowed

 private:
  static void onStart();
  static constexpr int kMaxHooks = 4;
  static Hook s_hooks[kMaxHooks];
  static int s_hook_count;
  bool m_started = false;
};

}  // namespace net
}  // namespace cq
