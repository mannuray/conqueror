// The one exit every DRIVE takes (bridge LLD R34.2): BodyLink::sendDrive asks the game's gate; a refused DRIVE goes
// out as zeros. And the ENVELOPE frame, acked through UnoChannel's game sink.
#include <unity.h>

#include <new>

#include "../support/frames.h"
#include "../support/wire.h"
#include "app/uno_channel.h"
#include "core/body_link.h"
#include "fakes/fake_ports.h"

using namespace cq;
using FT = proto::FrameType;

namespace {
core::FrameTx* tx = nullptr;
core::BodyLink* bl = nullptr;
cqtest::Wire* wire = nullptr;
core::WsClients ws;
app::Replies out(ws);

void unoSends(const uint8_t* d, size_t n, uint32_t now) { bl->onUartBytes(d, n, now); }

void synced() {
  bl->startMaster(0);
  uint8_t f[64];
  const size_t n = cqtest::frame(proto::HelloMsg{0x0100, proto::kProtocolVersion, 0, 1, 0x1a2b3c4dUL}, 1, f);
  unoSends(f, n, 10);
  wire->drain(*tx);
}

struct GateSpy {
  bool allow = true;
  int calls = 0;
  int8_t clamp = 0;
};
bool spyGate(void* ctx, int8_t& v, int8_t&, proto::Source) {
  auto* s = static_cast<GateSpy*>(ctx);
  ++s->calls;
  if (s->clamp != 0 && v > s->clamp) v = s->clamp;
  return s->allow;
}

const cqtest::Sent* lastDrive() {
  wire->drain(*tx);
  return wire->last(FT::kDrive);
}

int g_game_status = -1;
int g_game_on = -1;
void gameDone(uint8_t status, bool on) {
  g_game_status = status;
  g_game_on = on ? 1 : 0;
}
}  // namespace

void setUp() {
  static core::FrameTx s_tx;
  static cqtest::Wire s_wire;
  s_tx = core::FrameTx();
  s_wire = cqtest::Wire();
  tx = &s_tx;
  wire = &s_wire;
  alignas(core::BodyLink) static unsigned char s_mem[sizeof(core::BodyLink)];
  bl = new (s_mem) core::BodyLink(*tx);
  g_game_status = -1;
}
void tearDown() {}

void test_sendDrive_sends_zeros_when_the_gate_refuses() {
  synced();
  GateSpy spy;
  spy.allow = false;
  bl->setDriveGate(spyGate, &spy);
  TEST_ASSERT_TRUE(bl->sendDrive(40, -20));  // the stick
  const cqtest::Sent* d = lastDrive();
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL(0, static_cast<int8_t>(d->payload[1]));
  TEST_ASSERT_EQUAL(0, static_cast<int8_t>(d->payload[2]));
  TEST_ASSERT_TRUE(bl->sendDrive(-30, 25, proto::Source::kAuto));  // the motion service
  d = lastDrive();
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::Source::kAuto), d->payload[0]);
  TEST_ASSERT_EQUAL(0, static_cast<int8_t>(d->payload[1]));
  TEST_ASSERT_EQUAL(0, static_cast<int8_t>(d->payload[2]));
  TEST_ASSERT_EQUAL(2, spy.calls);
}

void test_an_allowed_drive_goes_as_asked() {
  synced();
  GateSpy spy;
  bl->setDriveGate(spyGate, &spy);
  TEST_ASSERT_TRUE(bl->sendDrive(40, -20));
  const cqtest::Sent* d = lastDrive();
  TEST_ASSERT_EQUAL(40, static_cast<int8_t>(d->payload[1]));
  TEST_ASSERT_EQUAL(-20, static_cast<int8_t>(d->payload[2]));
}

void test_no_gate_sends_as_today() {
  synced();
  TEST_ASSERT_TRUE(bl->sendDrive(40, -20));
  const cqtest::Sent* d = lastDrive();
  TEST_ASSERT_EQUAL(40, static_cast<int8_t>(d->payload[1]));
}

void test_stop_is_never_gated() {
  synced();
  GateSpy spy;
  spy.allow = false;
  bl->setDriveGate(spyGate, &spy);
  bl->sendStop(20);
  wire->drain(*tx);
  TEST_ASSERT_NOT_NULL(wire->last(FT::kStop));
  TEST_ASSERT_EQUAL(0, spy.calls);
}

void test_envelope_is_an_acked_frame_answered_to_the_game() {
  synced();
  app::UnoChannel uno(*bl, *tx, out);
  uno.setGameDone(gameDone);
  core::AckedOp op;
  op.kind = core::AckedOp::kEnvelope;
  op.a = 75;  // speed cap %
  op.r = 0;   // accel cap
  op.g = 2;   // CHILD_PLAY
  op.fd = core::kGameClient;
  TEST_ASSERT_TRUE(uno.request(op));
  uno.tick(20);
  wire->drain(*tx);
  const cqtest::Sent* e = wire->last(FT::kEnvelope);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_EQUAL(75, e->payload[0]);
  TEST_ASSERT_EQUAL(0, e->payload[1]);
  TEST_ASSERT_EQUAL(2, e->payload[2]);
  uint8_t f[16];
  const size_t n = cqtest::frame(proto::AckMsg{e->seq, 0}, 9, f);
  unoSends(f, n, 30);
  core::LinkEvent ev{};
  while (bl->popEvent(ev)) {
    if (ev.kind == core::LinkEventKind::kAckResult) uno.onAck(ev.seq, ev.status);
  }
  TEST_ASSERT_EQUAL(0, g_game_status);
  TEST_ASSERT_EQUAL(1, g_game_on);  // review C2: the sink knows it was the game's own envelope
}

void test_the_gate_may_clamp_a_drive() {  // review I7: the game's speed cap
  synced();
  GateSpy spy;
  spy.clamp = 60;
  bl->setDriveGate(spyGate, &spy);
  TEST_ASSERT_TRUE(bl->sendDrive(90, 0));
  TEST_ASSERT_EQUAL(60, static_cast<int8_t>(lastDrive()->payload[1]));
}

void test_a_hello_while_synced_is_a_restart() {  // review C1: the Uno restarted faster than the silent rule
  synced();
  core::LinkEvent ev{};
  while (bl->popEvent(ev)) {
  }
  uint8_t f[64];
  const size_t n = cqtest::frame(proto::HelloMsg{0x0100, proto::kProtocolVersion, 0, 1, 0x1a2b3c4dUL}, 2, f);
  unoSends(f, n, 500);
  bool restarted = false;
  while (bl->popEvent(ev)) restarted = restarted || ev.kind == core::LinkEventKind::kBodyRestarted;
  TEST_ASSERT_TRUE(restarted);
  TEST_ASSERT_TRUE(bl->synced());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sendDrive_sends_zeros_when_the_gate_refuses);
  RUN_TEST(test_an_allowed_drive_goes_as_asked);
  RUN_TEST(test_no_gate_sends_as_today);
  RUN_TEST(test_stop_is_never_gated);
  RUN_TEST(test_envelope_is_an_acked_frame_answered_to_the_game);
  RUN_TEST(test_the_gate_may_clamp_a_drive);
  RUN_TEST(test_a_hello_while_synced_is_a_restart);
  return UNITY_END();
}
