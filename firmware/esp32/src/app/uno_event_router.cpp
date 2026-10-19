#include "app/uno_event_router.h"

#include "ports/log_out.h"

namespace cq {
namespace app {
namespace {
using EK = proto::EventKind;
bool is(uint8_t code, EK k) { return code == static_cast<uint8_t>(k); }
// Plan N3: the Uno's alarms that end a running Logo program.
bool alarm(uint8_t code) {
  return code == static_cast<uint8_t>(proto::EventKind::kBattCritical) ||
         code == static_cast<uint8_t>(proto::EventKind::kTilted) || code == static_cast<uint8_t>(proto::EventKind::kLifted);
}

}  // namespace

void UnoEventRouter::bodyOnline(uint32_t now) {
  const core::HelloInfo& h = m_link.hello();
  ports::logLine("uno: synced fw %u.%u proto %u commit %08lx", h.fw >> 8, h.fw & 0xFF, h.proto,
                 static_cast<unsigned long>(h.commit));
  m_out.all(core::jsonBody(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), true, -1));
  m_log.add(core::LogKind::kBodyOnline, h.fw, h.proto, "", m_tel.tankMs(), now);
  m_sinks.body_online(now);
}

void UnoEventRouter::bodyOffline(const core::LinkEvent& ev, uint32_t now) {
  if (ev.code == 1) {
    ports::logLine("uno: incompatible protocol %d (need %u) - flash Plan B firmware", ev.a, proto::kProtocolVersion);
  } else {
    ports::logLine("uno: body offline (silent)");
  }
  m_out.all(core::jsonBody(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), false, ev.code == 1 ? ev.a : -1));
  m_sinks.body_offline(ev.code, ev.a, now);
  m_log.add(core::LogKind::kBodyOffline, ev.code, ev.a, "", m_tel.tankMs(), now);
}

void UnoEventRouter::unoEvent(const core::LinkEvent& ev, uint32_t now) {
  if (is(ev.code, EK::kControlChanged)) m_sinks.uno_owner(static_cast<uint8_t>(ev.a), now);
  m_out.all(core::jsonUnoEvent(m_out.buf(), m_out.cap(), m_tel.nextEventSeq(), m_tel.tankMs(), ev.code, ev.a, ev.b));
  if (!is(ev.code, EK::kButtonPress) && !is(ev.code, EK::kUnblocked)) {  // Plan O: not the noisy ones
    m_log.add(core::LogKind::kUno, ev.code, ev.a, "", m_tel.tankMs(), now);
  }
  if (is(ev.code, EK::kButtonPress)) {
    m_sinks.button_press(now);  // Plan M: confirm a waiting change, or the WiFi double press
  } else if (is(ev.code, EK::kStall) || is(ev.code, EK::kEncoderFault)) {
    m_sinks.motion_event(ev.code, now);  // Plan K: ends a move (turn / rate unchanged)
  } else if (alarm(ev.code) && m_sinks.alarm != nullptr) {
    m_sinks.alarm(ev.code, now);  // Plan N3: a running Logo program ends (7042 / 7043)
  }
}

void UnoEventRouter::route(const core::LinkEvent& ev, uint32_t now) {
  switch (ev.kind) {
    case core::LinkEventKind::kBodyOnline: bodyOnline(now); break;
    case core::LinkEventKind::kBodyOffline: bodyOffline(ev, now); break;
    case core::LinkEventKind::kUnoEvent: unoEvent(ev, now); break;
    case core::LinkEventKind::kStopResult: m_sinks.stop_result(ev.status); break;
    case core::LinkEventKind::kAckResult: m_sinks.ack_result(ev.seq, ev.status); break;
    case core::LinkEventKind::kSettingValue: {  // Plan M: SETTING_GET answer - value in a (low) / b (high)
      const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint16_t>(ev.b)) << 16) |
                                             static_cast<uint16_t>(ev.a));
      m_sinks.setting_value(ev.code, v);
      break;
    }
    default:
      ports::logLine("uno: %s reset request ignored (bridge has no stored settings)",
                     ev.code == static_cast<uint8_t>(proto::FrameType::kWifiResetReq) ? "WiFi" : "admin");
      break;
  }
}

}  // namespace app
}  // namespace cq
