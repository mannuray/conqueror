// The skill server's messages (bridge LLD R26.2).
#include "core/ws_json_skill.h"

#include <stdio.h>

#include "core/ws_json.h"
#include "core/ws_json_util.h"

namespace cq {
namespace core {
using namespace detail;

namespace {

const char* const kSkillNames[] = {"find", "wander", "explore", "setxy", "face", "escape", "follow"};
const char* const kEndWhy[] = {"stop", "deadman", "page", "owner", "ended", "caught"};

// The game words: the person, then ESCAPE's cm or FOLLOW's seconds (bridge LLD R34.3).
int gameArg(char* out, size_t cap, const logo::Action& a) {
  char who[64];
  jsonEscape(who, sizeof who, a.thing);
  if (a.skill == logo::SkillKind::kEscape) return snprintf(out, cap, ",\"thing\":\"%s\",\"cm\":%u", who, a.cm);
  if (a.skill == logo::SkillKind::kFollow) {
    return snprintf(out, cap, ",\"thing\":\"%s\",\"s\":%u", who, static_cast<unsigned>(a.seconds));
  }
  return snprintf(out, cap, ",\"thing\":\"%s\"", who);
}

// The skill's own argument as `,"key":value` (nothing for explore).
int skillArg(char* out, size_t cap, const logo::Action& a, FwdLeft to) {
  char thing[64];
  if (a.skill >= logo::SkillKind::kFace) return gameArg(out, cap, a);
  switch (a.skill) {
    case logo::SkillKind::kFind:
      jsonEscape(thing, sizeof thing, a.thing);
      return snprintf(out, cap, ",\"thing\":\"%s\"", thing);
    case logo::SkillKind::kWander:
      return snprintf(out, cap, ",\"s\":%u", static_cast<unsigned>(a.seconds));
    case logo::SkillKind::kSetxy:
      return snprintf(out, cap, ",\"to\":{\"fwd_cm\":%d,\"left_cm\":%d}", to.fwd_cm, to.left_cm);
    default:
      out[0] = '\0';
      return 0;
  }
}

}  // namespace

size_t jsonSkillAsk(char* out, size_t cap, int32_t id, logo::AskQ q, const char* thing, bool game) {
  char t[64];
  jsonEscape(t, sizeof t, thing);
  return fmt(out, cap, "{\"t\":\"skill_ask\",\"id\":%ld,\"q\":\"%s\",\"thing\":\"%s\"%s}", static_cast<long>(id),
             q == logo::AskQ::kSee ? "see?" : "howfar", t, game ? ",\"game\":true" : "");
}

size_t jsonSkillStart(char* out, size_t cap, int32_t id, const logo::Action& a, FwdLeft to, FwdLeft area) {
  char arg[96], ar[80] = "";
  skillArg(arg, sizeof arg, a, to);
  if (a.has_area) {
    snprintf(ar, sizeof ar, ",\"area\":{\"r_m\":%u,\"fwd_cm\":%d,\"left_cm\":%d}", static_cast<unsigned>(a.area_m),
             area.fwd_cm, area.left_cm);
  }
  return fmt(out, cap, "{\"t\":\"skill_start\",\"id\":%ld,\"skill\":\"%s\"%s%s}", static_cast<long>(id),
             kSkillNames[static_cast<uint8_t>(a.skill)], arg, ar);
}

size_t jsonSkillAskBackOk(char* out, size_t cap, int32_t id, int16_t cm) {
  return fmt(out, cap, "{\"t\":\"skill_ask\",\"id\":%ld,\"q\":\"back_ok\",\"cm\":%d}", static_cast<long>(id),
             static_cast<int>(cm));
}

size_t jsonGame(char* out, size_t cap, bool on) {
  return fmt(out, cap, "{\"t\":\"game\",\"on\":%s}", on ? "true" : "false");
}

size_t jsonGameEvent(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, GameState s, uint16_t caught) {
  static const char* const kStates[] = {"off", "arming", "playing", "caught", "held"};
  return fmt(out, cap, "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"game\",\"state\":\"%s\",\"caught\":%u}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), kStates[static_cast<uint8_t>(s)],
             static_cast<unsigned>(caught));
}

size_t jsonSkillEnd(char* out, size_t cap, int32_t id, LeaseEnd why) {
  return fmt(out, cap, "{\"t\":\"skill_end\",\"id\":%ld,\"why\":\"%s\"}", static_cast<long>(id),
             kEndWhy[static_cast<uint8_t>(why)]);
}

}  // namespace core
}  // namespace cq
