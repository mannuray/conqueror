// Tank -> skill server JSON (bridge LLD R26.2): skill_ask, skill_start, skill_end. Same rules as ws_json.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/effects.h"
#include "logo/program.h"
#include "logo/relative.h"

namespace cq {
namespace core {

// A place relative to the tank now (cm; forward +, left +): one type with logo::relative()'s result.
using FwdLeft = logo::FwdLeft;

// Why the lease ended from the tank's side (skill_end "why").
enum class LeaseEnd : uint8_t { kStop, kDeadman, kPage, kOwner, kEnded };

// {"t":"skill_ask","id":n,"q":"see?"|"howfar","thing":"toy"}
size_t jsonSkillAsk(char* out, size_t cap, int32_t id, logo::AskQ q, const char* thing);
// {"t":"skill_start","id":n,"skill":"find"|"wander"|"explore"|"setxy", find: "thing", wander: "s", setxy: "to" {...},
//  and, when the action has an AREA, "area":{"r_m","fwd_cm","left_cm"}}. `to` and `area` are relative to the tank now.
size_t jsonSkillStart(char* out, size_t cap, int32_t id, const logo::Action& a, FwdLeft to, FwdLeft area);
// {"t":"skill_end","id":n,"why":"stop"|"deadman"|"page"|"owner"|"ended"}
size_t jsonSkillEnd(char* out, size_t cap, int32_t id, LeaseEnd why);

}  // namespace core
}  // namespace cq
