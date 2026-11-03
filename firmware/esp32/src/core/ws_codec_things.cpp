// The things list of a hello / skill_things frame (bridge LLD R26.2), parsed in place into views.
#include <string.h>

#include "core/json_reader.h"
#include "core/thing_list.h"

namespace cq {
namespace core {
namespace {

struct Cursor {
  char* p;
  char* end;
  bool ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p < end;
  }
  bool eat(char c) {
    if (!ws() || *p != c) return false;
    ++p;
    return true;
  }
  bool peek(char c) { return ws() && *p == c; }
  // A plain string (no escapes, no control characters) as a C string in place.
  bool str(const char*& out) {
    if (!eat('"')) return false;
    char* q = p;
    while (q < end && *q != '"') {
      if (*q == '\\' || static_cast<unsigned char>(*q) < 0x20) return false;
      ++q;
    }
    if (q >= end) return false;
    *q = '\0';
    out = p;
    p = q + 1;
    return true;
  }
  bool skipValue() {
    ws();
    Reader r(p, static_cast<size_t>(end - p));
    if (!r.skip()) return false;
    p = const_cast<char*>(r.pos());
    return true;
  }
};

// "names": ["car", ...] -> the word's slots in the flat array
ThingParse readNames(Cursor& c, ThingList& out, uint8_t w) {
  if (!c.eat('[')) return ThingParse::kBad;
  if (c.eat(']')) return ThingParse::kOk;
  do {
    if (out.n_names[w] >= ThingList::kNameStride) return ThingParse::kTooBig;
    const char* s = nullptr;
    if (!c.str(s)) return ThingParse::kBad;
    out.names[w * ThingList::kNameStride + out.n_names[w]++] = s;
  } while (c.eat(','));
  return c.eat(']') ? ThingParse::kOk : ThingParse::kBad;
}

// One "key": value of a thing; unknown keys are skipped.
ThingParse readThingKey(Cursor& c, ThingList& out, uint8_t w) {
  const char* key = nullptr;
  if (!c.str(key) || !c.eat(':')) return ThingParse::kBad;
  if (strcmp(key, "word") == 0) return c.str(out.word[w]) ? ThingParse::kOk : ThingParse::kBad;
  if (strcmp(key, "names") == 0) return readNames(c, out, w);
  return c.skipValue() ? ThingParse::kOk : ThingParse::kBad;
}

// One {"word": "...", "names": [...]}.
ThingParse readThing(Cursor& c, ThingList& out) {
  if (out.n >= ThingList::kMaxWords) return ThingParse::kTooBig;
  const uint8_t w = out.n;
  if (!c.eat('{')) return ThingParse::kBad;
  do {
    const ThingParse r = readThingKey(c, out, w);
    if (r != ThingParse::kOk) return r;
  } while (c.eat(','));
  if (!c.eat('}') || out.word[w] == nullptr) return ThingParse::kBad;
  ++out.n;
  return ThingParse::kOk;
}

ThingParse readList(Cursor& c, ThingList& out) {
  if (!c.eat('[')) return ThingParse::kBad;
  if (c.eat(']')) return ThingParse::kOk;
  do {
    const ThingParse r = readThing(c, out);
    if (r != ThingParse::kOk) return r;
  } while (c.eat(','));
  return c.eat(']') ? ThingParse::kOk : ThingParse::kBad;
}

}  // namespace

ThingParse parseThingList(char* text, size_t len, ThingList& out) {
  out.clear();
  Reader r(text, len);
  if (!r.eat('{')) return ThingParse::kBad;
  if (r.peek('}')) return ThingParse::kOk;
  do {
    char key[16];
    if (!r.str(key, sizeof(key)) || !r.eat(':')) return ThingParse::kBad;
    if (strcmp(key, "things") == 0) {
      r.ws();
      Cursor c{const_cast<char*>(r.pos()), text + len};
      const ThingParse t = readList(c, out);
      if (t != ThingParse::kOk) out.clear();
      return t;
    }
    if (!r.skip()) return ThingParse::kBad;
  } while (r.eat(','));
  return ThingParse::kOk;
}

}  // namespace core
}  // namespace cq
