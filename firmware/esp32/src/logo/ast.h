// Tank Logo syntax tree (LLD §5): nodes from a fixed pool, children as a singly linked list.
#pragma once

#include <stdint.h>

namespace cq {
namespace logo {

enum class NodeKind : uint8_t {
  kProgram, kDef, kParams, kBlock, kTemplate, kList, kCall, kProcCall,
  kNumber, kWord, kQuoted, kInput, kPlaceholder, kBool, kPreset, kUnary, kBinary,
};
enum class Op : uint8_t { kNone, kAdd, kSub, kMul, kDiv, kEq, kNe, kLt, kGt, kLe, kGe, kAnd, kOr, kNot, kNeg };

struct Node {
  NodeKind kind = NodeKind::kProgram;
  Op op = Op::kNone;
  bool tail = false;      // set by the checker (grammar.md §8)
  int16_t ref = -1;       // kCall: gen::kWords index; kProcCall / kDef: proc index; kPreset: gen::InType
  uint16_t tok = 0;       // the token the node starts at
  uint8_t ph = 0;         // kPlaceholder
  int32_t milli = 0;      // kNumber; kBool 0 / 1
  Node* first = nullptr;
  Node* last = nullptr;
  Node* next = nullptr;
  uint16_t count = 0;  // a block can hold more than 255 statements
  void add(Node* c) {
    if (first == nullptr) first = c; else last->next = c;
    last = c;
    ++count;
  }
};

class NodePool {
 public:
  NodePool(Node* buf, uint16_t cap) : m_buf(buf), m_cap(cap) {}
  Node* make(NodeKind k, uint16_t tok);  // nullptr when full
  uint16_t used() const { return m_used; }
  bool full() const { return m_used >= m_cap; }
  void reset() { m_used = 0; }

 private:
  Node* m_buf;
  uint16_t m_cap;
  uint16_t m_used = 0;
};

}  // namespace logo
}  // namespace cq
