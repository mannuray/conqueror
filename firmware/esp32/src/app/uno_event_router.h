// Uno events and link changes -> the pages (ev), the Log and the controller that acts on them (bridge LLD R15.4).
#pragma once

#include <stdint.h>

#include "app/replies.h"
#include "app/telemetry_publisher.h"
#include "core/body_link.h"
#include "core/event_log.h"

namespace cq {
namespace app {

class UnoEventRouter {
 public:
  struct Sinks {  // filled by the root; captureless functions
    void (*body_online)(uint32_t now);
    void (*body_offline)(uint8_t code, int16_t a, uint32_t now);
    void (*uno_owner)(uint8_t owner, uint32_t now);
    void (*button_press)(uint32_t now);
    void (*motion_event)(uint8_t uno_event, uint32_t now);  // stall / encoder fault
    void (*ack_result)(uint8_t seq, uint8_t status);
    void (*stop_result)(uint8_t status);
    void (*setting_value)(uint8_t id, int32_t value);
    void (*alarm)(uint8_t uno_event, uint32_t now) = nullptr;  // Plan N3: battery critical, tilted, lifted
  };

  UnoEventRouter(Replies& out, core::EventLog& log, TelemetryPublisher& tel, const core::BodyLink& link)
      : m_out(out), m_log(log), m_tel(tel), m_link(link) {}
  void setSinks(const Sinks& s) { m_sinks = s; }
  void route(const core::LinkEvent& ev, uint32_t now);

 private:
  void bodyOnline(uint32_t now);
  void bodyOffline(const core::LinkEvent& ev, uint32_t now);
  void unoEvent(const core::LinkEvent& ev, uint32_t now);

  Replies& m_out;
  core::EventLog& m_log;
  TelemetryPublisher& m_tel;
  const core::BodyLink& m_link;
  Sinks m_sinks{};
};

}  // namespace app
}  // namespace cq
