// The skill server's messages (bridge LLD R26.2).
#include "core/ws_json_skill.h"

#include <stdio.h>

#include "core/ws_json.h"
#include "core/ws_json_util.h"

namespace cq {
namespace core {
using namespace detail;

namespace {

const char* const kSkillNames[] = {"find", "wander", "explore", "setxy"};
const char* const kEndWhy[] = {"stop", "deadman", "page", "owner", "ended"};

// The skill's own argument as `,"key":value` (nothing for explore).
int skillArg(char* out, size_t cap, const logo::Action& a, FwdLeft to) {
  char thing[64];
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

size_t jsonSkillAsk(char* out, size_t cap, int32_t id, logo::AskQ q, const char* thing) {
  char t[64];
  jsonEscape(t, sizeof t, thing);
  return fmt(out, cap, "{\"t\":\"skill_ask\",\"id\":%ld,\"q\":\"%s\",\"thing\":\"%s\"}", static_cast<long>(id),
             q == logo::AskQ::kSee ? "see?" : "howfar", t);
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

size_t jsonSkillEnd(char* out, size_t cap, int32_t id, LeaseEnd why) {
  return fmt(out, cap, "{\"t\":\"skill_end\",\"id\":%ld,\"why\":\"%s\"}", static_cast<long>(id),
             kEndWhy[static_cast<uint8_t>(why)]);
}

}  // namespace core
}  // namespace cq
