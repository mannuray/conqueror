#include <Arduino.h>
#include <util/atomic.h>

#include "drivers/board_config.h"
#include "drivers/pcint_hub.h"
#include "ports/encoders.h"

// The pin-change ISR lives in drivers/pcint_hub.cpp (Task 6b; Uno PCINT1, Mega PCINT2 shared with
// IR); it calls encoderEdges() with the rising edges. Channel A only, rising edges (LLD §9.6).
namespace {
volatile uint32_t g_edges_l = 0, g_edges_r = 0;
volatile uint32_t g_last_l = 0, g_last_r = 0;
volatile uint32_t g_period_l = 0, g_period_r = 0;
}  // namespace

void cq::drv::encoderEdges(bool left_rising, bool right_rising, uint32_t now) {
  if (left_rising) {
    g_period_l = now - g_last_l;
    g_last_l = now;
    ++g_edges_l;
  }
  if (right_rising) {
    g_period_r = now - g_last_r;
    g_last_r = now;
    ++g_edges_r;
  }
}

namespace cq {
namespace port {
namespace encoders {

void begin() { drv::encodersEnablePcint(); }

void snapshot(EncoderSnapshot& out) {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
    out.count_l = static_cast<int32_t>(g_edges_l);
    out.count_r = static_cast<int32_t>(g_edges_r);
    out.period_l_us = g_period_l;
    out.period_r_us = g_period_r;
    out.last_edge_l_us = g_last_l;
    out.last_edge_r_us = g_last_r;
  }
  out.t_us = micros();
}

}  // namespace encoders
}  // namespace port
}  // namespace cq
