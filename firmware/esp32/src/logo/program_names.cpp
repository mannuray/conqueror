#include "logo/program_names.h"

#include <string.h>

namespace cq {
namespace logo {
namespace {

constexpr Example kExamples[] = {
    {"back_to_start",
     "; Lesson 1 - back to the start\n"
     "; The tank drives out, then back to where the program started.\n"
     "; Watch: it never lands exactly on the spot - every turn adds a little error.\n"
     "; Try this: put tape where it starts, then change 60 to 120.\n"
     "FORWARD 60\nRIGHT 90\nFORWARD 40\nSAY [ going back now ]\nSETXY 0 0\nSOUND SUCCESS\n",
     true},
    {"where_is_my_ball",
     "; Lesson 2 - where is my ball?\n"
     "; The tank looks round, searches and checks each maybe until it finds the ball.\n"
     "; Watch: it says how many seconds it took.\n"
     "; Try this: run it again - if the tank has a home, it remembers where the ball was.\n"
     "AREA 2\nFIND \"ball\nSAY [ found it in ]\nSAY ROUND TIMER\nSAY [ seconds ]\n",
     true},
    {"treasure_hunt",
     "; Lesson 3 - treasure hunt\n"
     "; The tank finds the car, then the ball, then goes back to the start.\n"
     "; Watch: after 5 minutes any program stops by itself.\n"
     "; Try this: swap car and ball, or add a third thing to find.\n"
     "AREA 2\nFIND \"car\nSAY [ found the car ]\nFIND \"ball\nSAY [ found the ball ]\nSETXY 0 0\n"
     "PLAY [ C4 E4 G4 C5 2 ]\n",
     true},
    {"hot_and_cold",
     "; Lesson 4 - hot and cold\n"
     "; The tank stands still. Roll the ball slowly along the floor towards it.\n"
     "; The nearer the ball, the faster it beeps.\n"
     "; Try this: change 40 to 60 - it finds you from further away.\n"
     "TO HOTCOLD :cm\n"
     "  IFELSE :cm < 0 [ WAIT 0.5 ] [\n"
     "    IFELSE :cm < 40 [ PLAY [ C4 E4 G4 C5 2 ] SAY [ you found me ] STOP ] [ BEEP WAIT :cm / 100 ]\n"
     "  ]\n"
     "END\n"
     "FOREVER [ HOTCOLD HOWFAR \"ball ]\n",
     true},
    {"hot_and_cold_hunt",
     "; Lesson 5 - hot and cold hunt\n"
     "; Hide the ball. The tank wanders while it listens: the nearer the ball, the faster it beeps.\n"
     "; LAUNCH starts a second part that runs alongside - it may beep and ask,\n"
     "; but only the main program moves the tank.\n"
     "; Try this: change 50 to 80 - it says found from further away.\n"
     "TO LISTEN :cm\n"
     "  IFELSE :cm < 0 [ WAIT 0.5 ] [\n"
     "    IFELSE :cm < 50 [ SOUND SUCCESS SAY [ found you ] STOP ] [ BEEP WAIT :cm / 200 ]\n"
     "  ]\n"
     "END\n"
     "LAUNCH [ FOREVER [ LISTEN HOWFAR \"ball ] ]\n"
     "AREA 2\n"
     "WANDER 120\n",
     true},
    {"first_square", "; Your first square\nREPEAT 4 [ FORWARD 30 RIGHT 90 ]\nBEEP\n", false},
    {"avoid_walls",
     "; Drive around and turn away from walls\nFOREVER [\n  FORWARD 20\n  IF BLOCKED? [ BACK 10 RIGHT 90 ]\n]\n",
     false},
    {"look_around", "; Look around, then say so\nLOOK LEFT WAIT 1\nLOOK RIGHT WAIT 1\nLOOK HOME\nSAY [ all clear ]\n",
     false},
};
static_assert(sizeof kExamples / sizeof kExamples[0] == kBuiltInCount, "kBuiltInCount must match the table");

bool nameChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }

}  // namespace

bool programName(const char* in, char* out, size_t cap) {
  const size_t n = strlen(in);
  if (n == 0 || n > kNameMax || cap <= n) return false;
  for (size_t i = 0; i < n; ++i) {
    const char c = (in[i] >= 'A' && in[i] <= 'Z') ? static_cast<char>(in[i] + 32) : in[i];
    if (!nameChar(c)) return false;
    out[i] = c;
  }
  out[n] = '\0';
  return true;
}

uint8_t exampleCount() { return static_cast<uint8_t>(sizeof kExamples / sizeof kExamples[0]); }
const Example& example(uint8_t i) { return kExamples[i]; }

const Example* findExample(const char* lower_name) {
  for (const Example& e : kExamples) {
    if (strcmp(e.name, lower_name) == 0) return &e;
  }
  return nullptr;
}

}  // namespace logo
}  // namespace cq
