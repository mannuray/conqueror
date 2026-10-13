// BodyLink: master handshake, heartbeats, STOP resend until ACK, telemetry snapshot, events.
#include <unity.h>

#include <new>

#include "../support/frames.h"
#include "../support/wire.h"
#include "core/body_link.h"

using cq::core::BodyLink;
using cq::core::FrameTx;
using cq::core::HandshakeState;
using cq::core::LinkEvent;
using cq::core::LinkEventKind;
namespace proto = cq::proto;
using FT = cq::proto::FrameType;

namespace {

FrameTx* tx = nullptr;
BodyLink* bl = nullptr;
cqtest::Wire* wire = nullptr;

void unoSends(const uint8_t* d, size_t n, uint32_t now) { bl->onUartBytes(d, n, now); }

void hello(uint8_t proto_version, uint32_t now, uint8_t seq = 1) {
  uint8_t f[64];
  const size_t n =
      cqtest::frame(proto::HelloMsg{0x0100, proto_version, 0, 1, 0x1a2b3c4dUL}, seq, f);
  unoSends(f, n, now);
}

void ack(uint8_t ack_seq, uint8_t status, uint32_t now) {
  uint8_t f[16];
  const size_t n = cqtest::frame(proto::AckMsg{ack_seq, status}, 9, f);
  unoSends(f, n, now);
}

void run(uint32_t from, uint32_t to) {
  for (uint32_t t = from; t <= to; t += 2) {  // link_task: step + drain every 2 ms
    bl->step(t);
    wire->drain(*tx);
  }
}

bool popKind(LinkEventKind k, LinkEvent& out) {
  LinkEvent ev{};
  while (bl->popEvent(ev)) {
    if (ev.kind == k) {
      out = ev;
      return true;
    }
  }
  return false;
}

}  // namespace

void setUp() {
  static FrameTx s_tx;
  static cqtest::Wire s_wire;
  s_tx = FrameTx();
  s_wire = cqtest::Wire();
  tx = &s_tx;
  wire = &s_wire;
  alignas(BodyLink) static unsigned char s_mem[sizeof(BodyLink)];
  bl = new (s_mem) BodyLink(*tx);  // fresh object per test, no heap
}
void tearDown() {}

void test_start_master_sends_heartbeat_and_hello_req() {
  bl->startMaster(1000);
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(1, wire->count(FT::kHeartbeat));
  TEST_ASSERT_EQUAL(1, wire->count(FT::kHelloReq));
  TEST_ASSERT_EQUAL(static_cast<int>(HandshakeState::kWaitHello),
                    static_cast<int>(bl->handshake()));
}

void test_heartbeat_every_100ms() {
  bl->startMaster(0);
  run(2, 1000);
  TEST_ASSERT_EQUAL(11, wire->count(FT::kHeartbeat));  // t = 0, 100, ..., 1000
}

void test_hello_req_repeats_every_second_until_hello() {
  bl->startMaster(0);
  run(2, 2500);
  TEST_ASSERT_EQUAL(3, wire->count(FT::kHelloReq));  // 0, 1000, 2000
}

void test_handshake_acks_matching_proto_and_sends_owner_hint() {
  bl->startMaster(0);
  bl->setOwnerHint(proto::OwnerHintValue::kNone);
  hello(proto::kProtocolVersion, 50);
  wire->drain(*tx);
  const cqtest::Sent* a = wire->last(FT::kHelloAck);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL(proto::kProtocolVersion, a->payload[0]);
  TEST_ASSERT_EQUAL(1, wire->count(FT::kOwnerHint));
  TEST_ASSERT_EQUAL(0, wire->count(FT::kEnvelope));  // R2.14 #5: never an ENVELOPE
  TEST_ASSERT_TRUE(bl->synced());
  TEST_ASSERT_EQUAL(0x1a2b3c4dUL, bl->hello().commit);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kBodyOnline, ev));
}

void test_handshake_rejects_wrong_proto() {
  bl->startMaster(0);
  hello(1, 50);
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(0, wire->count(FT::kHelloAck));
  TEST_ASSERT_FALSE(bl->synced());
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kBodyOffline, ev));
  TEST_ASSERT_EQUAL(1, ev.code);
  TEST_ASSERT_EQUAL(1, ev.a);
  TEST_ASSERT_FALSE(bl->sendDrive(10, 0));  // motion refused while incompatible
}

void test_uno_reboot_hello_is_acked_again() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  hello(proto::kProtocolVersion, 500, 2);  // Uno rebooted: HELLO again
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(2, wire->count(FT::kHelloAck));
}

void test_silent_uno_restarts_handshake() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  wire->drain(*tx);
  wire->clear();
  run(12, 1100);  // no Uno bytes since t = 10
  TEST_ASSERT_FALSE(bl->synced());
  TEST_ASSERT_GREATER_THAN(0, static_cast<int>(wire->count(FT::kHelloReq)));
}

void test_not_master_never_sends_heartbeat_or_hello_ack() {
  hello(proto::kProtocolVersion, 10);  // bench is master (passthrough)
  run(0, 1000);
  TEST_ASSERT_EQUAL(0, wire->count(FT::kHeartbeat));
  TEST_ASSERT_EQUAL(0, wire->count(FT::kHelloAck));
  TEST_ASSERT_TRUE(bl->hello().seen);  // HELLO info still recorded for /info
}

void test_drive_only_when_synced() {
  bl->startMaster(0);
  TEST_ASSERT_FALSE(bl->sendDrive(40, 0));
  hello(proto::kProtocolVersion, 10);
  TEST_ASSERT_TRUE(bl->sendDrive(40, -20));
  wire->drain(*tx);
  const cqtest::Sent* d = wire->last(FT::kDrive);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::Source::kHuman), d->payload[0]);
  TEST_ASSERT_EQUAL(40, static_cast<int8_t>(d->payload[1]));
  TEST_ASSERT_EQUAL(-20, static_cast<int8_t>(d->payload[2]));
}

void test_bridge_never_invents_drive() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  bl->sendDrive(50, 0);
  for (uint32_t t = 12; t <= 5000; t += 2) {
    if (t % 40 == 0) {
      uint8_t f[64];
      const size_t n = cqtest::frame(proto::TelemFastMsg{}, 3, f);  // keep the link alive
      unoSends(f, n, t);
    }
    bl->step(t);
  }
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(1, wire->count(FT::kDrive));  // exactly the one client message
  TEST_ASSERT_EQUAL(0, wire->count(FT::kCamJoy));
}

void test_stop_resend_until_ack() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  const uint8_t seq = bl->sendStop(20);
  run(22, 130);  // sends at 20, 70, 120
  TEST_ASSERT_EQUAL(3, wire->count(FT::kStop));
  TEST_ASSERT_EQUAL(seq, wire->last(FT::kStop)->seq);
  ack(seq, 0, 131);
  run(132, 400);
  TEST_ASSERT_EQUAL(3, wire->count(FT::kStop));
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kStopResult, ev));
  TEST_ASSERT_EQUAL(seq, ev.seq);
  TEST_ASSERT_EQUAL(0, ev.status);
}

void test_stop_purges_queued_drive_and_cam_joy() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  wire->drain(*tx);
  wire->clear();
  bl->sendDrive(50, 0);  // queued, not yet on the UART
  bl->sendCamJoy(10, 0, true);
  bl->step(100);  // HEARTBEAT queued behind them
  bl->sendStop(100);
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(FT::kStop), wire->sent[0].type);  // STOP first
  TEST_ASSERT_EQUAL(0, wire->count(FT::kDrive));  // the older drive never follows the STOP
  TEST_ASSERT_EQUAL(0, wire->count(FT::kCamJoy));
  TEST_ASSERT_EQUAL(1, wire->count(FT::kHeartbeat));
}

void test_stop_gives_up_after_ten_sends() {
  bl->sendStop(0);  // works without being master (passthrough / not synced)
  run(2, 1000);
  TEST_ASSERT_EQUAL(10, wire->count(FT::kStop));
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kStopResult, ev));
  TEST_ASSERT_EQUAL(cq::core::kStatusTimeout, ev.status);
}

void test_inject_stop_uses_callers_seq() {
  bl->injectStop(200, 0);
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(200, wire->last(FT::kStop)->seq);
}

void test_cam_home_ack_and_timeout() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  uint8_t seq = 0;
  TEST_ASSERT_TRUE(bl->sendCamHome(20, seq));
  ack(seq, 0, 30);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kAckResult, ev));
  TEST_ASSERT_EQUAL(0, ev.status);
  TEST_ASSERT_TRUE(bl->sendCamHome(40, seq));
  run(42, 300);
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kAckResult, ev));
  TEST_ASSERT_EQUAL(cq::core::kStatusTimeout, ev.status);
}

void test_telemetry_snapshot_and_events() {
  proto::TelemFastMsg tf{};
  tf.yaw = 9000;
  tf.dist_mm = 420;
  uint8_t f[64];
  size_t n = cqtest::frame(tf, 1, f);
  unoSends(f, n, 5);
  n = cqtest::frame(proto::EventMsg{static_cast<uint8_t>(proto::EventKind::kDeadmanStop), 0, 0}, 2,
                    f);
  unoSends(f, n, 6);
  TEST_ASSERT_TRUE(bl->snapshot().fast_valid);
  TEST_ASSERT_EQUAL(9000, bl->snapshot().fast.yaw);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kUnoEvent, ev));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::EventKind::kDeadmanStop), ev.code);
}

// Review I1: link_task follows TELEM_SLOW.owner only when a new slow frame arrived (an IR takeover
// from EVENT control_changed must not be undone by the 1 Hz frame's older owner on the next tick).
void test_slow_count_counts_slow_frames() {
  TEST_ASSERT_EQUAL_UINT32(0, bl->snapshot().slow_count);
  uint8_t f[64];
  size_t n = cqtest::frame(proto::TelemSlowMsg{}, 1, f);
  unoSends(f, n, 5);
  TEST_ASSERT_EQUAL_UINT32(1, bl->snapshot().slow_count);
  n = cqtest::frame(proto::TelemSlowMsg{}, 2, f);
  unoSends(f, n, 1005);
  TEST_ASSERT_EQUAL_UINT32(2, bl->snapshot().slow_count);
}

void test_wifi_reset_request_answered_unavailable() {
  bl->startMaster(0);
  uint8_t f[16];
  const size_t n = cqtest::frame(proto::WifiResetReqMsg{}, 77, f);
  unoSends(f, n, 5);
  wire->drain(*tx);
  const cqtest::Sent* a = wire->last(FT::kEspAck);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL(77, a->payload[0]);
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::AckStatus::kUnavailable), a->payload[1]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_start_master_sends_heartbeat_and_hello_req);
  RUN_TEST(test_heartbeat_every_100ms);
  RUN_TEST(test_hello_req_repeats_every_second_until_hello);
  RUN_TEST(test_handshake_acks_matching_proto_and_sends_owner_hint);
  RUN_TEST(test_handshake_rejects_wrong_proto);
  RUN_TEST(test_uno_reboot_hello_is_acked_again);
  RUN_TEST(test_silent_uno_restarts_handshake);
  RUN_TEST(test_not_master_never_sends_heartbeat_or_hello_ack);
  RUN_TEST(test_drive_only_when_synced);
  RUN_TEST(test_bridge_never_invents_drive);
  RUN_TEST(test_stop_resend_until_ack);
  RUN_TEST(test_stop_purges_queued_drive_and_cam_joy);
  RUN_TEST(test_stop_gives_up_after_ten_sends);
  RUN_TEST(test_inject_stop_uses_callers_seq);
  RUN_TEST(test_cam_home_ack_and_timeout);
  RUN_TEST(test_telemetry_snapshot_and_events);
  RUN_TEST(test_slow_count_counts_slow_frames);
  RUN_TEST(test_wifi_reset_request_answered_unavailable);
  return UNITY_END();
}
