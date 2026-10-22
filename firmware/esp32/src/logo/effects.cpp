#include "logo/effects.h"

#include <stdio.h>
#include <string.h>

#include "core/arc_limits.h"
#include "logo/generated/words.h"
#include "logo/words.h"

namespace cq {
namespace logo {
namespace {

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c; }

bool sameText(const char* a, uint16_t alen, const char* b) {
  if (strlen(b) != alen) return false;
  for (uint16_t i = 0; i < alen; ++i) {
    if (upper(a[i]) != upper(b[i])) return false;
  }
  return true;
}

int32_t whole(int32_t milli) { return (milli < 0 ? milli - 500 : milli + 500) / 1000; }

class Mapper {
 public:
  Mapper(const Effect& e, const Program& p, const MapState& st, Action& out, RunError& err)
      : m_e(e), m_p(p), m_st(st), m_out(out), m_err(err), m_word(wordIndex(effectWord(e.kind))) {}
  bool run();

 private:
  bool move();
  bool turn();
  bool speed();
  bool wait();
  bool led();
  bool simple();  // BEEP, HOME, WAIT UNTIL start / tick
  bool sound();
  bool look();
  bool arc();
  bool tooTight(int32_t sign, int32_t angle);
  bool pivot();
  bool play();
  bool say();
  bool goHold();  // GO (at the current SPEED) / HOLD
  bool number(uint8_t i, int32_t& milli);
  bool preset(uint8_t i, gen::InType place, const char* const* names, uint8_t n, uint8_t& idx);
  bool numbers(uint8_t i, uint8_t want, const char* type, int32_t* out);
  bool part(int32_t milli, int32_t lo, int32_t hi, const char* what);
  bool side(uint8_t i, int32_t& sign);
  bool fail(uint16_t code, const DiagArg* a, uint8_t n);
  bool typeError(uint8_t i, const char* type);

  const Effect& m_e;
  const Program& m_p;
  const MapState& m_st;
  Action& m_out;
  RunError& m_err;
  int16_t m_word;
};

bool Mapper::run() {
  using Fn = bool (Mapper::*)();
  static constexpr Fn kMap[] = {&Mapper::move,   &Mapper::move,  &Mapper::turn,  &Mapper::turn,   &Mapper::speed,
                                &Mapper::wait,   &Mapper::led,   &Mapper::simple, &Mapper::sound, &Mapper::look,
                                &Mapper::simple, &Mapper::arc,   &Mapper::pivot, &Mapper::play,   &Mapper::say,
                                &Mapper::simple, &Mapper::simple, &Mapper::goHold, &Mapper::goHold};
  static_assert(sizeof kMap / sizeof kMap[0] == static_cast<size_t>(EffectKind::kCount), "one per EffectKind");
  m_out = Action();
  m_out.line = m_e.line;
  m_out.speed_pct = m_st.speed_pct;
  return (this->*kMap[static_cast<uint8_t>(m_e.kind)])();
}

bool Mapper::fail(uint16_t code, const DiagArg* a, uint8_t n) {
  m_err.code = code;
  m_err.line = m_e.line;
  renderMessage(errorTemplate(code), a, n, m_err.msg, sizeof m_err.msg);
  return false;
}

// 7012: "{word} needs {type}, but got {value}."
bool Mapper::typeError(uint8_t i, const char* type) {
  char v[48];
  valueText(m_p, m_e.args[i], true, v, sizeof v);
  const DiagArg a[] = {{"word", gen::kWords[m_word].name}, {"type", type}, {"value", v}};
  return fail(7012, a, 3);
}

// A number input: its type, then the word's range (words.yaml) - 7012 / 7025.
bool Mapper::number(uint8_t i, int32_t& milli) {
  if (m_e.args[i].tag != VTag::kNum) return typeError(i, "a number");
  milli = m_e.args[i].n;
  const gen::InputSpec& in = gen::kWords[m_word].inputs[i];
  if (!in.has_range || (milli >= in.min_milli && milli <= in.max_milli)) return true;
  char mn[16], mx[16];
  formatMilli(in.min_milli, mn, sizeof mn);
  formatMilli(in.max_milli, mx, sizeof mx);
  const DiagArg a[] = {{"word", gen::kWords[m_word].name}, {"what", in.what}, {"min", mn}, {"max", mx}, {"unit", in.unit}};
  return fail(7025, a, 5);
}

// A preset place given a word: its index in the preset list (7029 when it is not one of them).
bool Mapper::preset(uint8_t i, gen::InType place, const char* const* names, uint8_t n, uint8_t& idx) {
  uint16_t len = 0;
  const char* s = m_p.wordText(static_cast<uint16_t>(m_e.args[i].n), len);
  for (idx = 0; idx < n; ++idx) {
    if (sameText(s, len, names[idx])) return true;
  }
  char given[40], choices[96];
  snprintf(given, sizeof given, "%.*s", static_cast<int>(len), s);
  presetChoices(place, choices, sizeof choices);
  const DiagArg a[] = {{"word", gen::kWords[m_word].name}, {"choices", choices}, {"given", given}};
  return fail(7029, a, 3);
}

// A list of `want` numbers ([r g b], [pan tilt]); 7012 otherwise.
bool Mapper::numbers(uint8_t i, uint8_t want, const char* type, int32_t* out) {
  const Value v = m_e.args[i];
  if (v.tag != VTag::kList || m_p.consts[v.n].len != want) return typeError(i, type);
  const Const& c = m_p.consts[v.n];
  for (uint8_t k = 0; k < want; ++k) {
    const Value item = m_p.items[c.start + k];
    if (item.tag != VTag::kNum) return typeError(i, type);
    out[k] = item.n;
  }
  return true;
}

bool Mapper::part(int32_t milli, int32_t lo, int32_t hi, const char* what) {
  if (milli >= lo * 1000 && milli <= hi * 1000) return true;
  char mn[8], mx[8];
  snprintf(mn, sizeof mn, "%ld", static_cast<long>(lo));
  snprintf(mx, sizeof mx, "%ld", static_cast<long>(hi));
  const DiagArg a[] = {{"word", gen::kWords[m_word].name}, {"what", what}, {"min", mn}, {"max", mx}, {"unit", ""}};
  return fail(7025, a, 5);
}

// LEFT → +1, RIGHT → -1 (the tank's yaw: left = +).
bool Mapper::side(uint8_t i, int32_t& sign) {
  uint8_t idx = 0;
  if (m_e.args[i].tag != VTag::kWord) return typeError(i, "LEFT or RIGHT");
  if (!preset(i, gen::InType::kSide, gen::kSides, gen::kSidesCount, idx)) return false;
  sign = idx == 0 ? 1 : -1;
  return true;
}

bool Mapper::move() {
  int32_t d = 0;
  if (!number(0, d)) return false;
  m_out.kind = ActKind::kMove;
  m_out.dist_cm = static_cast<int16_t>(m_e.kind == EffectKind::kForward ? whole(d) : -whole(d));
  return true;
}

bool Mapper::turn() {
  int32_t a = 0;
  if (!number(0, a)) return false;
  m_out.kind = ActKind::kTurn;
  m_out.delta_cdeg = m_e.kind == EffectKind::kRight ? -a / 10 : a / 10;
  return true;
}

bool Mapper::speed() {
  int32_t p = 0;
  if (!number(0, p)) return false;
  m_out.kind = ActKind::kSpeed;
  m_out.speed_pct = static_cast<uint8_t>(whole(p));
  return true;
}

bool Mapper::wait() {
  int32_t s = 0;
  if (!number(0, s)) return false;
  m_out.kind = ActKind::kWait;
  m_out.ms = static_cast<uint32_t>(s);
  return true;
}

bool Mapper::simple() {
  static constexpr ActKind kSimple[] = {ActKind::kBeep, ActKind::kHome, ActKind::kUntilStart, ActKind::kTick};
  const EffectKind k = m_e.kind;
  const uint8_t i = k == EffectKind::kBeep ? 0 : (k == EffectKind::kHome ? 1 : (k == EffectKind::kUntilStart ? 2 : 3));
  m_out.kind = kSimple[i];
  return true;
}

// LED GREEN (a colour word) or LED [255 128 0].
bool Mapper::led() {
  m_out.kind = ActKind::kLed;
  if (m_e.args[0].tag == VTag::kWord) {
    uint8_t i = 0;
    if (!preset(0, gen::InType::kColour, gen::kColours, gen::kColoursCount, i)) return false;
    m_out.r = gen::kColourRgb[i].r;
    m_out.g = gen::kColourRgb[i].g;
    m_out.b = gen::kColourRgb[i].b;
    return true;
  }
  int32_t c[3];
  if (!numbers(0, 3, "a colour", c)) return false;
  for (int32_t x : c) {
    if (!part(x, 0, 255, "a colour part")) return false;
  }
  m_out.r = static_cast<uint8_t>(whole(c[0]));
  m_out.g = static_cast<uint8_t>(whole(c[1]));
  m_out.b = static_cast<uint8_t>(whole(c[2]));
  return true;
}

bool Mapper::sound() {
  uint8_t i = 0;
  if (m_e.args[0].tag != VTag::kWord) return typeError(0, "a sound");
  if (!preset(0, gen::InType::kSound, gen::kSounds, gen::kSoundsCount, i)) return false;
  m_out.kind = ActKind::kSound;
  m_out.sound_id = static_cast<uint8_t>(i + 1);
  return true;
}

// LOOK LEFT (a preset; HOME = the camera's home) or LOOK [pan tilt] in degrees from the centre.
bool Mapper::look() {
  if (m_e.args[0].tag == VTag::kWord) {
    uint8_t i = 0;
    if (!preset(0, gen::InType::kLook, gen::kLooks, gen::kLooksCount, i)) return false;
    m_out.kind = gen::kLookDeg[i].home ? ActKind::kLookHome : ActKind::kLook;
    m_out.pan_deg = gen::kLookDeg[i].pan;
    m_out.tilt_deg = gen::kLookDeg[i].tilt;
    return true;
  }
  int32_t d[2];
  if (!numbers(0, 2, "where to look", d)) return false;
  m_out.kind = ActKind::kLook;
  m_out.pan_deg = static_cast<int16_t>(whole(d[0]));
  m_out.tilt_deg = static_cast<int16_t>(whole(d[1]));
  return true;
}

// ARC side angle radius: a radius below the smallest circle is 7022; SPEED moves into the radius's band.
bool Mapper::arc() {
  int32_t sign = 0, angle = 0, radius = 0;
  if (!side(0, sign) || !number(1, angle)) return false;
  if (m_e.args[2].tag != VTag::kNum) return typeError(2, "a number");
  const int32_t r = whole(m_e.args[2].n);
  const core::ArcBand band = core::arcBand(static_cast<uint16_t>(r > 0 ? r : 0));
  if (r < core::arcMinRadiusCm() || !band.possible) return tooTight(sign, angle);
  if (!number(2, radius)) return false;
  m_out.kind = ActKind::kArc;
  m_out.delta_cdeg = sign * angle / 10;
  m_out.radius_cm = static_cast<uint16_t>(r);
  const uint8_t sp = m_st.speed_pct;
  m_out.speed_pct = sp < band.lo ? band.lo : (sp > band.hi ? band.hi : sp);
  m_out.speed_moved = m_out.speed_pct != sp;
  return true;
}

// 7022: "ARC RIGHT 90 20 is too tight — the smallest circle is 31 cm. Try PIVOT."
bool Mapper::tooTight(int32_t sign, int32_t angle) {
  char word[16], ang[16], rad[16];
  snprintf(word, sizeof word, "ARC %s", sign > 0 ? "LEFT" : "RIGHT");
  formatMilli(angle, ang, sizeof ang);
  formatMilli(m_e.args[2].n, rad, sizeof rad);
  const DiagArg a[] = {{"word", word}, {"angle", ang}, {"radius", rad}};
  return fail(7022, a, 3);
}

bool Mapper::pivot() {
  int32_t sign = 0, angle = 0;
  if (!side(0, sign) || !number(1, angle)) return false;
  m_out.kind = ActKind::kPivot;
  m_out.delta_cdeg = sign * angle / 10;
  return true;
}

// PLAY [ C4 E4 2 G4 ]: the notes and their lengths (the checker already read the list: errors here are rare).
bool Mapper::play() {
  const Value v = m_e.args[0];
  if (v.tag != VTag::kList) return typeError(0, "notes in [ ]");
  NoteReader rd;
  const Const& c = m_p.consts[v.n];
  for (uint16_t k = 0; k < c.len; ++k) {
    const Value item = m_p.items[c.start + k];
    uint16_t len = 0;
    const char* s = item.tag == VTag::kWord ? m_p.wordText(static_cast<uint16_t>(item.n), len) : "";
    const NoteReader::Err e = item.tag == VTag::kNum ? rd.number(item.n) : rd.word(s, len);
    if (e != NoteReader::Err::kNone) return typeError(0, "notes like C4 E4 2 G4");
  }
  m_out.kind = ActKind::kTone;
  m_out.n_notes = rd.count();
  for (uint8_t k = 0; k < rd.count(); ++k) {
    m_out.notes[k] = rd.notes()[k];
    m_out.ms += rd.notes()[k].ms;
  }
  return true;
}

bool Mapper::goHold() {
  m_out.kind = m_e.kind == EffectKind::kGo ? ActKind::kGo : ActKind::kHold;  // speed_pct: set by run()
  return true;
}

bool Mapper::say() {
  m_out.kind = ActKind::kSay;
  valueText(m_p, m_e.args[0], false, m_out.text, sizeof m_out.text);
  return true;
}

}  // namespace

bool mapEffect(const Effect& e, const Program& p, const MapState& st, Action& out, RunError& err) {
  return Mapper(e, p, st, out, err).run();
}

}  // namespace logo
}  // namespace cq
