// The AI words and the game words into Actions (Tank Logo LLD §24.2, §27): a thing, a person, the questions, the
// skills and AREA.
#include <stdio.h>

#include "logo/effects_mapper.h"
#include "logo/words.h"

namespace cq {
namespace logo {
namespace detail {

// A word input into m_out.thing: lower case, at most 24 bytes (longer: 7053, no thing or person is that long).
bool Mapper::word(uint8_t i, const char* type) {
  if (m_e.args[i].tag != VTag::kWord) return typeError(i, type);
  uint16_t len = 0;
  const char* s = m_p.wordText(static_cast<uint16_t>(m_e.args[i].n), len);
  if (len == 0) {  // an empty word is not a thing (an empty {value} would drop the whole message)
    const DiagArg a[] = {{"word", gen::kWords[m_word].name}, {"type", "a thing"}, {"value", "an empty word"}};
    return fail(7012, a, 3);
  }
  if (len >= sizeof m_out.thing) {
    char given[40];
    snprintf(given, sizeof given, "%.*s", static_cast<int>(len < 36 ? len : 36), s);
    const DiagArg a[] = {{"thing", given}};
    return fail(7053, a, 1);
  }
  for (uint16_t k = 0; k < len; ++k) {
    m_out.thing[k] = (s[k] >= 'A' && s[k] <= 'Z') ? static_cast<char>(s[k] + 32) : s[k];
  }
  m_out.thing[len] = '\0';
  return true;
}

// A thing (a quoted one was checked; a computed one is here). The person is never one, but SEE? / HOWFAR in a game.
bool Mapper::thing(uint8_t i, bool ask) {
  if (!word(i, "a thing")) return false;
  if (!isPersonWord(m_out.thing) || (ask && m_p.game)) return true;
  const DiagArg a[] = {{"thing", m_out.thing}};
  return fail(7053, a, 1);
}

// The game's person (FACE / ESCAPE / FOLLOW): one of words.yaml's person_words, else 7062.
bool Mapper::person(uint8_t i) {
  if (!word(i, "a person")) return false;
  return isPersonWord(m_out.thing) || fail(7062, nullptr, 0);
}

bool Mapper::ask() {
  m_out.kind = ActKind::kAsk;
  m_out.ask_q = static_cast<AskQ>(m_e.sub);
  return thing(0, true);
}

bool Mapper::skill() {
  int32_t a = 0, b = 0;
  m_out.kind = ActKind::kSkill;
  m_out.skill = static_cast<SkillKind>(m_e.sub);
  if (m_out.skill >= SkillKind::kFace) return gameSkill();
  switch (m_out.skill) {  // VARIANT POINT: skills
    case SkillKind::kFind: return thing(0);
    case SkillKind::kWander:
      if (!number(0, a)) return false;
      m_out.seconds = static_cast<uint16_t>(whole(a));
      return true;
    case SkillKind::kSetxy:
      if (!number(0, a) || !number(1, b)) return false;
      m_out.x_cm = static_cast<int16_t>(whole(a));
      m_out.y_cm = static_cast<int16_t>(whole(b));
      return true;
    default: return true;  // EXPLORE
  }
}

// The person, then ESCAPE's cm or FOLLOW's seconds (Tank Logo LLD §27.1).
bool Mapper::gameSkill() {
  int32_t n = 0;
  if (!person(0) || (m_out.skill != SkillKind::kFace && !number(1, n))) return false;
  if (m_out.skill == SkillKind::kEscape) m_out.cm = static_cast<uint8_t>(whole(n));
  if (m_out.skill == SkillKind::kFollow) m_out.seconds = static_cast<uint16_t>(whole(n));
  return true;
}

bool Mapper::area() {
  int32_t m = 0;
  if (!number(0, m)) return false;
  m_out.kind = ActKind::kArea;
  m_out.area_m = static_cast<uint8_t>(m > 0 && whole(m) == 0 ? 1 : whole(m));  // never 0 by rounding
  return true;
}

}  // namespace detail
}  // namespace logo
}  // namespace cq
