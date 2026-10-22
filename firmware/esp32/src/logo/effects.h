// Tank Logo effects → what the tank does (LLD §10): units and signs of the bridge's motion service (cm, 0.01°,
// + = left like the tank's yaw), run-time ranges and types (7012 / 7025 / 7029, ARC too tight 7022), colours,
// sounds, LOOK angles and notes from the generated tables. Pure; the RunController carries the Action out.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/notes.h"
#include "logo/program.h"
#include "logo/vm.h"

namespace cq {
namespace logo {

enum class ActKind : uint8_t {
  kNone, kMove, kTurn, kArc, kPivot, kHome, kSpeed, kWait, kLed, kBeep, kSound, kTone, kLook, kLookHome, kSay,
  kUntilStart, kTick, kGo, kHold  // kGo: drive on at speed_pct; kHold: stop it (Tank Logo LLD §22)
};

struct Action {
  ActKind kind = ActKind::kNone;
  uint16_t line = 0;
  int16_t dist_cm = 0;       // kMove: + forward
  int32_t delta_cdeg = 0;    // kTurn / kArc / kPivot: + = left (the tank's yaw convention); Logo's RIGHT is -
  uint16_t radius_cm = 0;    // kArc
  uint8_t speed_pct = 40;    // the motion's speed; kSpeed: the new SPEED
  bool speed_moved = false;  // kArc: SPEED was moved into the radius's band (a note for the page)
  uint32_t ms = 0;           // kWait; kTone: the whole tune
  uint8_t r = 0, g = 0, b = 0;         // kLed
  uint8_t sound_id = 0;                // kSound: protocol SoundId (presets.sound order + 1)
  int16_t pan_deg = 0, tilt_deg = 0;   // kLook: from the centre, right / up = +
  uint8_t n_notes = 0;                 // kTone
  Note notes[NoteReader::kMaxNotes];
  char text[128] = "";                 // kSay
};

struct MapState {
  uint8_t speed_pct = 40;  // SPEED (words.yaml default)
};

// One effect → one Action. False: refused at run time, `err` says why (with the effect's line).
bool mapEffect(const Effect& e, const Program& p, const MapState& st, Action& out, RunError& err);

// The Action in Logo terms, as logo/spec/tests/running.yaml writes it ("FORWARD 50", "RIGHT 90", "LED 0 255 0" …);
// "" for an internal tick.
void actionText(const Action& a, char* out, size_t cap);

}  // namespace logo
}  // namespace cq
