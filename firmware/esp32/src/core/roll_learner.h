// Learned early stop (spec 02 §G6, bridge LLD R7.9): after a move ends done, watch the mean encoder count until it
// settles and report how far the tracks rolled on after the STOP (mm). Whole numbers only.
#pragma once

#include <stdint.h>

namespace cq {
namespace core {

class RollLearner {
 public:
  void begin(int32_t counts_at_stop, uint32_t now_ms);
  // One TELEM_FAST while watching: true when a reading is ready in `mm` (then watching stops). Driven again in the
  // window or a reading outside 0..kRollMaxMm -> no reading, watching stops.
  bool observe(int32_t counts, bool driven, uint16_t cpm, uint32_t now_ms, int32_t& mm);
  bool watching() const { return m_on; }
  void cancel() { m_on = false; }

 private:
  bool m_on = false;
  int32_t m_stop = 0;
  int32_t m_last = 0;
  uint32_t m_start_ms = 0;
  uint32_t m_change_ms = 0;
};

uint16_t rollAverage(uint16_t old_mm, int32_t reading_mm);  // (3 × old + reading + 2) / 4

}  // namespace core
}  // namespace cq
