// Tank Logo compiled program (LLD §7): instructions, constants (words and lists), your words. Filled by the
// Compiler, run by the Vm. Fixed capacities; the caller owns the storage (N3 puts it in PSRAM).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/ast.h"
#include "logo/generated/words.h"

namespace cq {
namespace logo {

enum class VTag : uint8_t { kNum, kBool, kWord, kList };
struct Value {
  VTag tag = VTag::kNum;
  int32_t n = 0;  // kNum: 1/1000; kBool: 0 / 1; kWord / kList: constant index
};

enum class OpCode : uint8_t {
  kHalt, kLine, kPushN, kPushB, kPushC, kArg, kOp, kSensor, kCallB, kJmp, kJz, kJnz,
  kLoopInit, kLoopNext, kCall, kTCall, kRet, kRetV, kEffect, kStop, kFail,
  kCount
};
enum class Sensor : uint8_t { kDistance, kObstacle, kBlocked, kHeading, kTilted, kBattery, kTimer, kUntilOver };
enum class Builtin : uint8_t { kRandom, kRemainder, kAbs, kRound, kRepcount };
enum class EffectKind : uint8_t {
  kForward, kBack, kRight, kLeft, kSpeed, kWait, kLed, kBeep, kSound, kLook, kHome,
  kArc, kPivot, kPlay, kSay, kUntilStart, kTick,
  kCount
};
const char* effectWord(EffectKind k);  // "FORWARD" … ("WAIT UNTIL" for kUntilStart / kTick)

// a: Op / Sensor / Builtin / EffectKind / argument count / 1 = FOREVER; n: number, constant, jump target, line,
// procedure or error code (see compiler.cpp for each instruction's use).
struct Instr {
  OpCode op = OpCode::kHalt;
  uint8_t a = 0;
  int32_t n = 0;
};

struct Const {
  VTag tag = VTag::kWord;
  uint16_t start = 0;  // kWord: text[start..+len); kList: items[start..+len)
  uint16_t len = 0;
};

struct ProcEntry {
  uint16_t entry = 0;
  uint8_t n_inputs = 0;
  bool gives_value = false;
  uint16_t name = 0;  // word constant with the name (messages)
};

struct Program {
  static constexpr uint16_t kMaxCode = 2048;
  static constexpr uint16_t kMaxConsts = 256;
  static constexpr uint16_t kMaxItems = 512;
  static constexpr uint16_t kTextBytes = 4096;

  Instr code[kMaxCode];
  uint16_t n_code = 0;
  Const consts[kMaxConsts];
  uint16_t n_consts = 0;
  Value items[kMaxItems];
  uint16_t n_items = 0;
  char text[kTextBytes];
  uint16_t n_text = 0;
  ProcEntry procs[gen::kMaxProcs];
  uint8_t n_procs = 0;
  bool handles_obstacles = false;

  void clear();
  int16_t addWord(const char* s, uint16_t len);  // -1 when full
  int16_t addList(const Value* items, uint16_t n);  // -1 when full
  const char* wordText(uint16_t c, uint16_t& len) const;
};

// Values as text: numbers like the reads printer (2.5, -30), words as written, lists in [ ] (the outer brackets
// only when `brackets`). Bounded by cap; nested lists deeper than 8 are shown as [...].
void formatMilli(int32_t milli, char* out, size_t cap);
void valueText(const Program& p, Value v, bool brackets, char* out, size_t cap);
bool sameValue(const Program& p, Value a, Value b);  // = (words case-insensitive, lists by contents)

}  // namespace logo
}  // namespace cq
