// The game's keys (bridge LLD R34.3): the engine's person estimate {"t":"person","seen","d_cm","bearing","coming",
// "sure","age_ms"} and its hello's "people" (the seeing test passed). Kept apart from ws_codec_keys.cpp (its size).
#include <math.h>
#include <string.h>

#include "core/ws_codec_internal.h"

namespace cq {
namespace core {
namespace codec {
namespace {

bool word(Reader& r, bool& out) {
  if (r.word("true")) {
    out = true;
    return true;
  }
  if (r.word("false")) {
    out = false;
    return true;
  }
  return false;
}

bool whole(Reader& r, double lo, double hi, double& n) {
  return r.num(n, nullptr, 0) && n >= lo && n <= hi && n == floor(n);
}

bool readSeen(Reader& r, WsCommand& c, Seen& s) { return word(r, c.person.seen) && (s.person_seen = true); }
bool readDcm(Reader& r, WsCommand& c, Seen& s) {
  double n = 0;
  if (!whole(r, -1, 1000, n)) return false;  // −1: no distance; up to 10 m
  c.person.d_cm = static_cast<int16_t>(n);
  return s.person_d = true;
}
bool readBearing(Reader& r, WsCommand& c, Seen& s) {
  double n = 0;
  if (!whole(r, -180, 180, n)) return false;
  c.person.bearing_deg = static_cast<int16_t>(n);
  return s.person_bearing = true;
}
bool readComing(Reader& r, WsCommand& c, Seen&) {
  double n = 0;
  if (!whole(r, -1, 1, n)) return false;
  c.person.coming = static_cast<int8_t>(n);
  return true;
}
bool readSure(Reader& r, WsCommand& c, Seen&) {  // 0..1 on the wire, 0..255 kept
  double n = 0;
  if (!r.num(n, nullptr, 0) || n < 0.0 || n > 1.0) return false;
  c.person.sure = static_cast<uint8_t>(lround(n * 255.0));
  return true;
}
bool readAge(Reader& r, WsCommand& c, Seen&) {
  double n = 0;
  if (!whole(r, 0, 60000, n)) return false;
  c.person_age_ms = static_cast<uint16_t>(n);
  return true;
}
bool readPeople(Reader& r, WsCommand& c, Seen&) { return word(r, c.people); }
bool readPress(Reader& r, WsCommand& c, Seen&) { return word(r, c.press); }
bool readGameCap(Reader& r, WsCommand& c, Seen&) {  // calib: the game's speed cap, a track % (1..100)
  double n = 0;
  if (!whole(r, 1, 100, n)) return false;
  c.game_cap = static_cast<uint8_t>(n);
  return c.has_game_cap = true;
}

struct Row {
  const char* key;
  bool (*read)(Reader&, WsCommand&, Seen&);
};
const Row kRows[] = {
    {"seen", readSeen}, {"d_cm", readDcm}, {"bearing", readBearing}, {"coming", readComing},
    {"sure", readSure}, {"age_ms", readAge}, {"people", readPeople}, {"game_cap_pct", readGameCap},
    {"press", readPress},
};

}  // namespace

bool readGameKey(const char* key, Reader& r, WsCommand& c, Seen& s, bool& known) {
  for (const Row& row : kRows) {
    if (strcmp(key, row.key) != 0) continue;
    known = true;
    return row.read(r, c, s);
  }
  known = false;
  return true;
}

bool checkPerson(const Seen& s) { return s.person_seen && s.person_d && s.person_bearing; }

}  // namespace codec
}  // namespace core
}  // namespace cq
