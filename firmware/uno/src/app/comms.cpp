#include <avr/pgmspace.h>
#include <string.h>

#include "app/app.h"
#include "app/comms.h"
#include "drivers/board_config.h"
#include "ports/clock.h"
#include "ports/link.h"

// avr-libc heap symbols. Declared at global scope with C linkage: a block-scope `extern` inside
// cq::app would name cq::app::__heap_start and fail to link.
extern "C" {
extern int __heap_start;
extern char* __brkval;
}

namespace cq {

App g_app;

namespace app {

// Parser work per loop pass is bounded in parser steps, not received bytes: one hostile byte
// can need ~700 steps and ~26 CRC passes of replay (input reachable through the unauthenticated
// WiFi bridge), so the rest of a replay stays pending in the parser for the next pass.
// Worst case per pass at 16 MHz (host model: ~40 cycles/step, ~70 cycles/CRC byte): CRC bytes in
// one pass <= steps + 51 carried-over stored bytes, so 64 x 40 + (64 + 51) x 70 ~= 10.6 k cycles
// ~= 0.66 ms (adversarial search incl. resync scan/memmove: 10.8 k cycles ~= 0.67 ms) <= ~1 ms.
// Normal traffic costs one step per byte: commands + 10 Hz heartbeats are well under 1 kB/s
// (< 10 bytes per 10 ms tick) against >= 64 bytes per pass; at line rate (25 kB/s) the 64-byte
// RX buffer fills in ~2.5 ms, i.e. one full buffer per pass. Telemetry out (~20 Hz) does not use
// the parser. A flood of crafted bytes overflows the RX buffer instead of stalling the loop;
// the link-loss stop still applies.
constexpr uint8_t kPollSteps = 64;

void commsPoll() {
  uint8_t budget = kPollSteps;
  uint8_t b = 0;
  while (budget != 0) {
    // Finish pending replay work first; read a new byte only when none is left.
    if (!g_app.parser.pending()) {
      if (!port::link::readByte(b)) break;
      g_app.parser.feed(b);  // always accepted: nothing is pending
    }
    if (!g_app.parser.next(budget)) continue;  // next() consumed >= 1 step; the budget bounds the loop
    g_app.link.onValidFrame(port::clock::nowMs());
    const uint8_t type = g_app.parser.type();
    bool routed = false;
    for (uint8_t i = 0; i < kRouteCount; ++i) {
      Route r;
      memcpy_P(&r, &kRoutes[i], sizeof r);
      if (r.type != type) continue;
      routed = true;
      const proto::AckStatus st = r.handler(g_app.parser.payload(), g_app.parser.len());
      if (r.needs_ack) g_app.acks.push(g_app.parser.seq(), st);
      break;
    }
    if (!routed) ++g_app.parser.errors().unknown_type;
  }
}

namespace {

uint8_t g_tx[proto::kMaxPayload + 5];

template <typename Msg>
bool sendMsg(const Msg& m) {
  uint8_t payload[proto::kMaxPayload];
  const uint8_t n = proto::encode(m, payload);
  if (n == proto::kEncodeError) return false;
  const uint8_t total =
      g_app.writer.build(static_cast<uint8_t>(Msg::kType), payload, n, g_tx);
  if (port::link::txFree() < total) {
    g_app.writer.setSeq(g_app.writer.lastSeq());  // reuse the seq next time
    ++g_app.parser.errors().tx_dropped;
    return false;
  }
  return port::link::write(g_tx, total);
}

bool sendRaw(uint8_t type) {
  const uint8_t total = g_app.writer.build(type, nullptr, 0, g_tx);
  if (port::link::txFree() < total) {
    g_app.writer.setSeq(g_app.writer.lastSeq());
    return false;
  }
  return port::link::write(g_tx, total);
}

}  // namespace

void txPump() {
  const uint32_t now = port::clock::nowMs();
  uint8_t seq = 0;
  proto::AckStatus st{};
  // Peek, send, then pop: a full TX buffer leaves the item at the head, so order is kept.
  while (g_app.acks.peek(seq, st)) {
    proto::AckMsg a{};
    a.ack_seq = seq;
    a.status = static_cast<uint8_t>(st);
    if (!sendMsg(a)) return;
    g_app.acks.pop(seq, st);
  }
  uint8_t req = 0;
  if (g_app.link.requestDue(now, req)) {
    if (!sendRaw(req)) return;
    g_app.link.onRequestSent(g_app.writer.lastSeq(), now);
  }
  Event e{};
  while (g_app.events.peek(e)) {
    proto::EventMsg m{};
    m.kind = e.kind;
    m.a = e.a;
    m.b = e.b;
    if (!sendMsg(m)) return;
    g_app.events.pop(e);
  }
  if (g_app.link.helloDue(now)) {
    proto::HelloMsg h{};
    h.fw = board::kFwVersion;
    h.proto = proto::kProtocolVersion;
    h.caps = g_app.encoders_enabled ? 1 : 0;
    h.reset_reason = static_cast<uint8_t>(g_app.reset_reason);
    h.commit = board::kGitCommit;
    sendMsg(h);
  }
  if (g_app.setting_value_pending && sendMsg(g_app.setting_value)) g_app.setting_value_pending = false;
  if (g_app.cal_result_pending && sendMsg(g_app.cal_result)) g_app.cal_result_pending = false;
  if (g_app.cal_data_pending && sendMsg(g_app.cal_data)) g_app.cal_data_pending = false;
  if (g_app.fast_due) {
    g_app.fast.uno_ms = now;
    if (sendMsg(g_app.fast)) g_app.fast_due = false;
  }
  if (g_app.slow_due && sendMsg(g_app.slow)) g_app.slow_due = false;
}

void telemetryFastDue() { g_app.fast_due = true; }

void telemetrySlowDue() {
  proto::TelemSlowMsg& s = g_app.slow;
  s.uptime_s = port::clock::nowMs() / 1000u;
  s.reset_reason = static_cast<uint8_t>(g_app.reset_reason);
  s.loop_load = g_app.load.loadPct();
  s.loop_max_us = g_app.load.maxUs();
  s.free_ram = freeRam();
  // bad_crc / bad_len count rejection events; a resync replay can count the same bytes again.
  s.err_frames = static_cast<uint16_t>(g_app.parser.errors().bad_crc + g_app.parser.errors().bad_len +
                                       g_app.parser.errors().unknown_type);
  s.settings_crc = g_app.settings.crc();
  const uint32_t muted = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kMuted);
  const uint32_t quiet = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kQuiet);
  const uint32_t brake_off = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kBrakeOff);
  const uint32_t envelope = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kEnvelope);
  const uint32_t link_lost = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kLinkLost);
  const uint32_t deadman = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kDeadman);
  g_app.slow_flags &= ~(muted | quiet | brake_off | envelope | link_lost | deadman);
  if (g_app.sound.mode() == proto::AudioModeValue::kMute) g_app.slow_flags |= muted;
  if (g_app.sound.mode() == proto::AudioModeValue::kQuiet) g_app.slow_flags |= quiet;
  if (!g_app.safety_override.brakeOn(g_app.envelope.brakeOn())) g_app.slow_flags |= brake_off;  // effective brake
  if (g_app.envelope.active()) g_app.slow_flags |= envelope;
  if (!g_app.link.alive(port::clock::nowMs())) g_app.slow_flags |= link_lost;
  if (g_app.deadman_latched) g_app.slow_flags |= deadman;  // Task 11: DEADMAN_STOP until the next DRIVE
  s.flags2 = g_app.slow_flags;
  s.profile_idx = 0;
  g_app.slow_due = true;
}

uint16_t freeRam() {
  int top = 0;
  const int free_bytes = ::__brkval == nullptr
                             ? reinterpret_cast<int>(&top) - reinterpret_cast<int>(&::__heap_start)
                             : reinterpret_cast<int>(&top) - reinterpret_cast<int>(::__brkval);
  return free_bytes < 0 ? 0 : static_cast<uint16_t>(free_bytes);
}

}  // namespace app
}  // namespace cq
