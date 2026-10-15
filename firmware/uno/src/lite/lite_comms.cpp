#include "domain/lite/lite_frames.h"
#include "domain/lite/lite_telemetry.h"
#include "lite/lite_tasks.h"
#include "drivers/board_config.h"
#include "lite/lite_app.h"
#include "ports/clock.h"
#include "ports/imu.h"
#include "ports/link.h"

// avr-libc heap start, global scope with C linkage (as app/comms.cpp).
extern "C" {
extern int __heap_start;
}

namespace cq {
namespace lite {

LiteApp g_lite;

namespace {
constexpr uint8_t kPollSteps = 64;  // same bound as the full app (LLD §5: one hostile byte ≤ ~0.7 ms)
uint8_t g_tx[proto::kMaxPayload + 5];

template <typename Msg>
bool sendMsg(const Msg& m) {
  uint8_t payload[proto::kMaxPayload];
  const uint8_t n = proto::encode(m, payload);
  if (n == proto::kEncodeError) return false;
  const uint8_t total = g_lite.writer.build(static_cast<uint8_t>(Msg::kType), payload, n, g_tx);
  if (port::link::txFree() < total) {
    g_lite.writer.setSeq(g_lite.writer.lastSeq());
    ++g_lite.parser.errors().tx_dropped;
    return false;
  }
  return port::link::write(g_tx, total);
}

bool sendRaw(uint8_t type) {
  const uint8_t total = g_lite.writer.build(type, nullptr, 0, g_tx);
  if (port::link::txFree() < total) {
    g_lite.writer.setSeq(g_lite.writer.lastSeq());
    return false;
  }
  return port::link::write(g_tx, total);
}
}  // namespace

// lite_routes.cpp: true when an ACK must be sent.
bool route(uint8_t type, const uint8_t* p, uint8_t len, proto::AckStatus& st);

void commsPoll() {
  uint8_t budget = kPollSteps;
  uint8_t b = 0;
  while (budget != 0) {
    if (!g_lite.parser.pending()) {
      if (!port::link::readByte(b)) break;
      g_lite.parser.feed(b);
    }
    if (!g_lite.parser.next(budget)) continue;
    g_lite.link.onValidFrame(port::clock::nowMs());
    proto::AckStatus st = proto::AckStatus::kOk;
    if (route(g_lite.parser.type(), g_lite.parser.payload(), g_lite.parser.len(), st)) {
      g_lite.acks.push(g_lite.parser.seq(), st);
    }
  }
}

namespace {
// Each returns false when the TX buffer is full: the item stays queued and order is kept (peek, send, pop).
bool sendAcks() {
  uint8_t seq = 0;
  proto::AckStatus st{};
  while (g_lite.acks.peek(seq, st)) {
    proto::AckMsg a{};
    a.ack_seq = seq;
    a.status = static_cast<uint8_t>(st);
    if (!sendMsg(a)) return false;
    g_lite.acks.pop(seq, st);
  }
  return true;
}

bool sendEvents() {
  Event e{};
  while (g_lite.events.peek(e)) {
    proto::EventMsg m{};
    m.kind = e.kind;
    m.a = e.a;
    m.b = e.b;
    if (!sendMsg(m)) return false;
    g_lite.events.pop(e);
  }
  return true;
}

void sendHello() {
  proto::HelloMsg h{};
  h.fw = board::kFwVersion;
  h.proto = proto::kProtocolVersion;
  h.caps = lite_frames::helloCaps(g_lite.encoders_enabled);
  h.reset_reason = static_cast<uint8_t>(g_lite.reset_reason);
  h.commit = board::kGitCommit;
  sendMsg(h);
}
}  // namespace

void txPump() {
  const uint32_t now = port::clock::nowMs();
  if (!sendAcks()) return;
  uint8_t req = 0;
  if (g_lite.link.requestDue(now, req)) {
    if (!sendRaw(req)) return;
    g_lite.link.onRequestSent(g_lite.writer.lastSeq(), now);
  }
  if (!sendEvents()) return;
  if (g_lite.link.helloDue(now)) sendHello();
  if (g_lite.setting_value_pending && sendMsg(g_lite.setting_value)) g_lite.setting_value_pending = false;
  if (g_lite.fast_due) {
    g_lite.fast.uno_ms = now;
    if (sendMsg(g_lite.fast)) g_lite.fast_due = false;
  }
  if (g_lite.slow_due && sendMsg(g_lite.slow)) g_lite.slow_due = false;
}

void telemetryFastDue() { g_lite.fast_due = true; }

namespace {
constexpr uint8_t kPaint = 0xAA;
uint8_t* heapStart() { return reinterpret_cast<uint8_t*>(&::__heap_start); }
}  // namespace

// Stack high-water mark (spec 02 §D5.1): at boot the free gap between the heap start and the stack is painted;
// telemetry counts the bytes the stack has never reached since. No heap is used, so __brkval stays 0.
void paintStack() {
  uint8_t marker = 0;
  for (uint8_t* p = heapStart(); p < &marker - 32; ++p) *p = kPaint;
}

uint16_t stackHeadroom() {
  uint8_t marker = 0;
  uint16_t n = 0;
  for (const uint8_t* p = heapStart(); p < &marker && *p == kPaint; ++p) ++n;
  return n;
}

void telemetrySlowDue() {
  g_lite.slow.uptime_s = port::clock::nowMs() / 1000u;
  g_lite.slow.reset_reason = static_cast<uint8_t>(g_lite.reset_reason);
  g_lite.slow.err_frames = static_cast<uint16_t>(g_lite.parser.errors().bad_crc + g_lite.parser.errors().bad_len +
                                                 g_lite.parser.errors().unknown_type);
  g_lite.slow_flags = lite_telemetry::slowFlags(g_lite.slow_flags, g_lite.envelope.brakeOn(), g_lite.envelope.active(),
                                               g_lite.link.alive(port::clock::nowMs()), g_lite.deadman_latched,
                                               g_lite.sound.mode());
  g_lite.slow.flags2 = g_lite.slow_flags;
  g_lite.slow.free_ram = stackHeadroom();
  g_lite.slow.err_i2c = port::imu::i2cErrors();
  g_lite.slow.hw = lite_telemetry::hwByte(g_lite.encoders_enabled, imuUsable(), !g_lite.range.fault());
  g_lite.slow_due = true;
}

}  // namespace lite
}  // namespace cq
