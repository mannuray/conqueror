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

using TextFn = void (*)(const Action&, char*, size_t);
constexpr TextFn kText[] = {none, motion, motion, motion, motion, home,     speed, wait,       led,
                            beep, sound,  tone,   look,   lookHome, say,    untilStart, none,  go, hold};
static_assert(sizeof kText / sizeof kText[0] == static_cast<size_t>(ActKind::kHold) + 1, "one per ActKind");

}  // namespace

void actionText(const Action& a, char* out, size_t cap) { kText[static_cast<uint8_t>(a.kind)](a, out, cap); }

}  // namespace logo
}  // namespace cq
