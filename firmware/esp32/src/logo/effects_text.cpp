// Actions in Logo terms (logo/spec/tests/running.yaml): what the tank was asked to do, without fd, ids or units.
#include <stdio.h>
#include <string.h>

#include "logo/effects.h"
#include "logo/generated/words.h"

namespace cq {
namespace logo {
namespace {

void deg(int32_t cdeg, char* out, size_t cap) { formatMilli((cdeg < 0 ? -cdeg : cdeg) * 10, out, cap); }
const char* side(int32_t cdeg) { return cdeg < 0 ? "RIGHT" : "LEFT"; }

void tone(const Action& a, char* out, size_t cap) {
  snprintf(out, cap, "PLAY");
  for (uint8_t i = 0; i < a.n_notes; ++i) {
    char one[16];
    snprintf(one, sizeof one, " %u/%u", static_cast<unsigned>(a.notes[i].hz), static_cast<unsigned>(a.notes[i].ms));
    strncat(out, one, cap - strlen(out) - 1);
  }
}

void motion(const Action& a, char* out, size_t cap) {
  char d[16];
  deg(a.delta_cdeg, d, sizeof d);
  if (a.kind == ActKind::kMove) {
    snprintf(out, cap, "%s %d", a.dist_cm < 0 ? "BACK" : "FORWARD", a.dist_cm < 0 ? -a.dist_cm : a.dist_cm);
  } else if (a.kind == ActKind::kTurn) {
    snprintf(out, cap, "%s %s", side(a.delta_cdeg), d);
  } else if (a.kind == ActKind::kArc) {
    snprintf(out, cap, "ARC %s %s %u", side(a.delta_cdeg), d, a.radius_cm);
  } else {
    snprintf(out, cap, "PIVOT %s %s", side(a.delta_cdeg), d);
  }
}

void plain(const char* s, char* out, size_t cap) { snprintf(out, cap, "%s", s); }
void none(const Action&, char* out, size_t cap) { plain("", out, cap); }
void home(const Action&, char* out, size_t cap) { plain("HOME", out, cap); }
void beep(const Action&, char* out, size_t cap) { plain("BEEP", out, cap); }
void lookHome(const Action&, char* out, size_t cap) { plain("LOOK HOME", out, cap); }
void untilStart(const Action&, char* out, size_t cap) { plain("WAIT-UNTIL", out, cap); }
void wait(const Action& a, char* out, size_t cap) {
  char s[16];
  formatMilli(static_cast<int32_t>(a.ms), s, sizeof s);
  snprintf(out, cap, "WAIT %s", s);
}
void speed(const Action& a, char* out, size_t cap) { snprintf(out, cap, "SPEED %u", a.speed_pct); }
void led(const Action& a, char* out, size_t cap) { snprintf(out, cap, "LED %u %u %u", a.r, a.g, a.b); }
void sound(const Action& a, char* out, size_t cap) { snprintf(out, cap, "SOUND %s", gen::kSounds[a.sound_id - 1]); }
void look(const Action& a, char* out, size_t cap) { snprintf(out, cap, "LOOK %d %d", a.pan_deg, a.tilt_deg); }
void say(const Action& a, char* out, size_t cap) { snprintf(out, cap, "SAY %s", a.text); }
void go(const Action&, char* out, size_t cap) { plain("GO", out, cap); }
void hold(const Action&, char* out, size_t cap) { plain("HOLD", out, cap); }

void ask(const Action& a, char* out, size_t cap) {
  snprintf(out, cap, "%s %s", a.ask_q == AskQ::kSee ? "SEE?" : "HOWFAR", a.thing);
}
void area(const Action& a, char* out, size_t cap) { snprintf(out, cap, "AREA %u", a.area_m); }
// FIND ball / WANDER 10 / EXPLORE / SETXY 20 30, then " AREA 2 AT x y" (cm, the run's frame) when an AREA is on.
void skill(const Action& a, char* out, size_t cap) {
  switch (a.skill) {
    case SkillKind::kFind: snprintf(out, cap, "FIND %s", a.thing); break;
    case SkillKind::kWander: snprintf(out, cap, "WANDER %u", a.seconds); break;
    case SkillKind::kExplore: snprintf(out, cap, "EXPLORE"); break;
    case SkillKind::kSetxy: snprintf(out, cap, "SETXY %d %d", a.x_cm, a.y_cm); break;
  }
  if (!a.has_area) return;
  const size_t n = strlen(out);
  snprintf(out + n, cap - n, " AREA %u AT %ld %ld", a.area_m, static_cast<long>(a.area_at.x_mm / 10),
           static_cast<long>(a.area_at.y_mm / 10));
}

using TextFn = void (*)(const Action&, char*, size_t);
constexpr TextFn kText[] = {none, motion, motion, motion, motion, home,     speed, wait,       led,
                            beep, sound,  tone,   look,   lookHome, say,    untilStart, none,  go, hold, ask, skill, area};
static_assert(sizeof kText / sizeof kText[0] == static_cast<size_t>(ActKind::kArea) + 1, "one per ActKind");

}  // namespace

void actionText(const Action& a, char* out, size_t cap) { kText[static_cast<uint8_t>(a.kind)](a, out, cap); }

}  // namespace logo
}  // namespace cq
