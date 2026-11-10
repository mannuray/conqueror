// Page commands -> controllers (bridge LLD R15.4): parse, STOP fast path, admission, one table of routes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/drive_controller.h"
#include "app/logo_controller.h"
#include "app/motion_controller.h"
#include "app/replies.h"
#include "app/settings_controller.h"
#include "app/tank_app_controller.h"
#include "app/telemetry_publisher.h"
#include "core/ai_pages.h"
#include "core/skill_hub.h"
#include "core/thing_list.h"
#include "core/ws_codec.h"

namespace cq {
namespace app {

class CommandRouter {
 public:
  struct GameHooks {  // Plan G1 (bridge LLD R34): a person's STOP and the engine's answers reach game mode
    void (*on_stop)(uint32_t now) = nullptr;
    void (*on_answer)(int32_t id, int32_t value, uint32_t now) = nullptr;
    void (*on_person)(const core::PersonEst& e, uint32_t now) = nullptr;  // at receipt (review I6)
    void (*on_page)(int fd) = nullptr;  // an /ai page said hello: the game's state now (G3 final review I1)
  };
  void setGameHooks(const GameHooks& g) { m_game = g; }
  CommandRouter(DriveController& d, MotionController& m, SettingsController& s, TankAppController& t, LogoController& l,
                TelemetryPublisher& tel, Replies& out)
      : m_drive(d), m_motion(m), m_settings(s), m_tank(t), m_logo(l), m_tel(tel), m_out(out) {}
  // Plan Q1: the ESP32 timer (µs) for `pong` (the AI engine's clock offset); unset -> 0.
  void setClockUs(uint64_t (*now_us)()) { m_clock_us = now_us; }
  uint64_t clockUs() const { return m_clock_us != nullptr ? m_clock_us() : 0; }
  void setHello(const char* fw, const char* (*sensor)()) {
    m_fw = fw;
    m_sensor = sensor;
  }
  // Plan Z2a (bridge LLD R26.2): the skill server's hello and its things list go to the link task's SkillHub.
  void setSkillHub(core::SkillHub* hub) { m_hub = hub; }
  void setAiPages(core::AiPages* pages) { m_pages = pages; }  // Plan Z2b-1 (R27.1): "ai-page" hellos
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
  void onHello(int fd, const core::WsCommand& c);
  bool registerServer(int fd, const core::WsCommand& c);
  void sendAiAt(int fd);
  void onThings(int fd, int32_t id, const core::WsCommand& c);

  DriveController& m_drive;
  MotionController& m_motion;
  SettingsController& m_settings;
  TankAppController& m_tank;
  LogoController& m_logo;
  TelemetryPublisher& m_tel;
  Replies& m_out;
  const char* m_fw = "";
  const char* (*m_sensor)() = nullptr;
  uint64_t (*m_clock_us)() = nullptr;
  core::SkillHub* m_hub = nullptr;
  core::AiPages* m_pages = nullptr;
  const char* m_text = "";  // the message being routed (skill_things reads its list from a copy)
  size_t m_len = 0;
  char m_copy[201];          // skill_things: parsed in place (one LinkCommand holds at most 200 bytes)
  core::ThingList m_things;  // ~1.7 KB: a member, never on the link task's stack
  GameHooks m_game;
};

}  // namespace app
}  // namespace cq
