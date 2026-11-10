// The effect -> Action mapper (internal to effects*.cpp): effects.cpp maps the motion, light and sound words,
// effects_ai.cpp the AI words and the game words (Tank Logo LLD §24.2, §27).
#pragma once

#include <stdint.h>

#include "logo/effects.h"
#include "logo/generated/words.h"
#include "logo/words.h"

namespace cq {
namespace logo {
namespace detail {

inline int32_t whole(int32_t milli) { return (milli < 0 ? milli - 500 : milli + 500) / 1000; }

class Mapper {
 public:
  Mapper(const Effect& e, const Program& p, const MapState& st, Action& out, RunError& err)
      : m_e(e), m_p(p), m_st(st), m_out(out), m_err(err), m_word(wordIndex(effectWord(e.kind, e.sub))) {}
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
  bool ask();     // SEE? / HOWFAR
  bool skill();   // FIND / WANDER / EXPLORE / SETXY
  bool area();    // AREA
  bool launch();  // LAUNCH
  bool thing(uint8_t i, bool ask = false);
  bool person(uint8_t i);
  bool gameSkill();  // FACE / ESCAPE / FOLLOW
  bool word(uint8_t i, const char* type);
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

}  // namespace detail
}  // namespace logo
}  // namespace cq
