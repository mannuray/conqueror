// Tank -> skill server JSON (bridge LLD R26.2): skill_ask, skill_start, skill_end. Same rules as ws_json.h.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/game_mode.h"
#include "logo/effects.h"
#include "logo/program.h"
#include "logo/relative.h"

namespace cq {
namespace core {

// A place relative to the tank now (cm; forward +, left +): one type with logo::relative()'s result.
using FwdLeft = logo::FwdLeft;

// Why the lease ended from the tank's side (skill_end "why").
enum class LeaseEnd : uint8_t { kStop, kDeadman, kPage, kOwner, kEnded, kCaught };  // kCaught: game mode (R34)

// {"t":"skill_ask","id":n,"q":"see?"|"howfar","thing":"toy"}
// game: a person question in a game run - `"game":true` (bridge LLD R34.3, the engine answers from PersonWatch)
size_t jsonSkillAsk(char* out, size_t cap, int32_t id, logo::AskQ q, const char* thing, bool game = false);
// {"t":"skill_start","id":n,"skill":"find"|"wander"|"explore"|"setxy", find: "thing", wander: "s", setxy: "to" {...},
//  and, when the action has an AREA, "area":{"r_m","fwd_cm","left_cm"}}. `to` and `area` are relative to the tank now.
size_t jsonSkillStart(char* out, size_t cap, int32_t id, const logo::Action& a, FwdLeft to, FwdLeft area);
// R34.3: {"t":"skill_ask","id":n,"q":"back_ok","cm":n} - may a backward move this long run over trusted floor?
size_t jsonSkillAskBackOk(char* out, size_t cap, int32_t id, int16_t cm);
// R34.3: {"t":"game","on":true|false} - start / stop the engine's person stream
size_t jsonGame(char* out, size_t cap, bool on);
// R34: to the pages {"t":"ev","seq","tank_ms","kind":"game","state":"arming"|"playing"|"caught"|"held"|"off","caught":n}
size_t jsonGameEvent(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, GameState s, uint16_t caught);
// {"t":"skill_end","id":n,"why":"stop"|"deadman"|"page"|"owner"|"ended"|"caught"}
size_t jsonSkillEnd(char* out, size_t cap, int32_t id, LeaseEnd why);

}  // namespace core
}  // namespace cq
