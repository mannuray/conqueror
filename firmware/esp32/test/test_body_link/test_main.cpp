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

// Review M11: handing the link to the bench (stopMaster) is not a lost body - no body_offline.
void test_stop_master_is_not_body_offline() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 50);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kBodyOnline, ev));
  bl->stopMaster();
  TEST_ASSERT_FALSE(popKind(LinkEventKind::kBodyOffline, ev));
  TEST_ASSERT_FALSE(bl->master());
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
  hello(proto::kProtocolVersion, 10);  // no web client: the bridge stays silent (idle)
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
  TEST_ASSERT_TRUE(bl->sendDrive(-30, 25, proto::Source::kAuto));  // Plan H: the motion service's frames
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::Source::kAuto), wire->last(FT::kDrive)->payload[0]);
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
  bl->sendStop(0);  // works without being master (idle / not synced)
  run(2, 1000);
  TEST_ASSERT_EQUAL(10, wire->count(FT::kStop));
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kStopResult, ev));
  TEST_ASSERT_EQUAL(cq::core::kStatusTimeout, ev.status);
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

// Plan F: SOUND and CAM_LOCK use the same acked slot as CAM_HOME (one at a time, 200 ms timeout).
void test_sound_and_cam_lock_acked() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  uint8_t seq = 0;
  TEST_ASSERT_TRUE(bl->sendSound(13, 20, seq));
  wire->drain(*tx);
  TEST_ASSERT_NOT_NULL(wire->last(FT::kSound));
  TEST_ASSERT_EQUAL(seq, wire->last(FT::kSound)->seq);
  ack(seq, 0, 30);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kAckResult, ev));
  TEST_ASSERT_EQUAL(0, ev.status);
  TEST_ASSERT_TRUE(bl->sendCamLock(true, 40, seq));
  wire->drain(*tx);
  TEST_ASSERT_NOT_NULL(wire->last(FT::kCamLock));
  run(42, 300);
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kAckResult, ev));
  TEST_ASSERT_EQUAL(cq::core::kStatusTimeout, ev.status);
}

// R3.1: AUDIO_MODE {mode} on the same acked slot.
void test_audio_mode_acked() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  uint8_t seq = 0;
  TEST_ASSERT_TRUE(bl->sendAudioMode(2, 20, seq));
  wire->drain(*tx);
  TEST_ASSERT_NOT_NULL(wire->last(FT::kAudioMode));
  TEST_ASSERT_EQUAL(seq, wire->last(FT::kAudioMode)->seq);
  TEST_ASSERT_EQUAL_UINT8(2, wire->last(FT::kAudioMode)->payload[0]);
  ack(seq, 0, 30);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kAckResult, ev));
  TEST_ASSERT_EQUAL(0, ev.status);
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

// Plan Q1 (AI engine LLD §2): a TELEM_FAST frame keeps the ESP32 time it arrived at (same clock as the camera's
// frame stamp); a TELEM_SLOW frame does not change it.
// R19: the snapshot carries the heading and rate corrected by the gyro scale, and the raw heading; a HELLO (the
// Uno restarted, its heading starts again) re-anchors the corrected heading on the raw one.
void fastYaw(uint16_t yaw, int16_t rate, uint8_t seq, uint32_t now) {
  proto::TelemFastMsg tf{};
  tf.yaw = yaw;
  tf.yaw_rate = rate;
  uint8_t f[64];
  const size_t n = cqtest::frame(tf, seq, f);
  unoSends(f, n, now);
}

void test_snapshot_heading_is_corrected_by_the_gyro_scale() {
  TEST_ASSERT_EQUAL_UINT16(10000, bl->gyroScale());
  TEST_ASSERT_TRUE(bl->setGyroScale(10230));
  TEST_ASSERT_FALSE(bl->setGyroScale(12000));
  TEST_ASSERT_EQUAL_UINT16(10230, bl->gyroScale());
  fastYaw(9000, 0, 1, 5);  // the first frame anchors
  TEST_ASSERT_EQUAL_UINT16(9000, bl->snapshot().fast.yaw);
  fastYaw(18000, 650, 2, 25);  // +90° raw -> +92.07°
  TEST_ASSERT_EQUAL_UINT16(18207, bl->snapshot().fast.yaw);
  TEST_ASSERT_EQUAL_UINT16(18000, bl->snapshot().yaw_raw);
  TEST_ASSERT_EQUAL_INT16(665, bl->snapshot().fast.yaw_rate);
  bl->startMaster(30);
  hello(proto::kProtocolVersion, 40, 3);  // the Uno (re)started
  fastYaw(0, 0, 4, 60);
  TEST_ASSERT_EQUAL_UINT16(0, bl->snapshot().fast.yaw);  // anchored again
}

void test_telem_fast_keeps_its_arrival_time() {
  uint8_t f[64];
  size_t n = cqtest::frame(proto::TelemFastMsg{}, 1, f);
  bl->onUartBytes(f, n, 5, 123456789012ULL);
  TEST_ASSERT_TRUE(bl->snapshot().fast_bus_us == 123456789012ULL);
  n = cqtest::frame(proto::TelemSlowMsg{}, 2, f);
  bl->onUartBytes(f, n, 6, 999ULL);
  TEST_ASSERT_TRUE(bl->snapshot().fast_bus_us == 123456789012ULL);
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


// Plan M (bridge LLD R9.2): settings / LED through the acked slot; SETTING_VALUE becomes an event.
void test_setting_frames_and_value_event() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  uint8_t seq = 0;
  TEST_ASSERT_TRUE(bl->sendSettingGet(66, 20, seq));
  TEST_ASSERT_TRUE(bl->ackPending());
  wire->drain(*tx);
  TEST_ASSERT_NOT_NULL(wire->last(FT::kSettingGet));
  ack(seq, 0, 30);
  TEST_ASSERT_FALSE(bl->ackPending());
  uint8_t f[32];
  const size_t n = cqtest::frame(proto::SettingValueMsg{66, -12}, 11, f);
  unoSends(f, n, 40);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kSettingValue, ev));
  TEST_ASSERT_EQUAL_UINT8(66, ev.code);
  const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(static_cast<uint16_t>(ev.b)) << 16) |
                                         static_cast<uint16_t>(ev.a));
  TEST_ASSERT_EQUAL_INT32(-12, v);
  TEST_ASSERT_TRUE(bl->sendSettingSet(66, 19, 50, seq));
  wire->drain(*tx);
  TEST_ASSERT_NOT_NULL(wire->last(FT::kSettingSet));
  ack(seq, 0, 60);
  TEST_ASSERT_TRUE(bl->sendSettingsSave(70, seq));
  wire->drain(*tx);
  TEST_ASSERT_NOT_NULL(wire->last(FT::kSettingsSave));
  ack(seq, 0, 80);
  TEST_ASSERT_TRUE(bl->sendLed(static_cast<uint8_t>(proto::LedMode::kBlink), 0, 0, 255, 200, 3000, 90, seq));
  wire->drain(*tx);
  TEST_ASSERT_NOT_NULL(wire->last(FT::kLed));
}


// Plan O: the last self-test result is kept; CALIBRATE SELF_TEST can be sent.
void test_self_test_record_and_calibrate() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  TEST_ASSERT_FALSE(bl->selfTest().seen);
  uint8_t f[32];
  const size_t n = cqtest::frame(proto::EventMsg{static_cast<uint8_t>(proto::EventKind::kSelftest), 1, 1}, 5, f);
  unoSends(f, n, 20);
  TEST_ASSERT_TRUE(bl->selfTest().seen);
  TEST_ASSERT_EQUAL(1, bl->selfTest().mask);
  TEST_ASSERT_TRUE(bl->selfTest().locked);
  uint8_t seq = 0;
  TEST_ASSERT_TRUE(bl->sendCalibrate(0, 9, 30, seq));
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(1, wire->count(FT::kCalibrate));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_start_master_sends_heartbeat_and_hello_req);
  RUN_TEST(test_heartbeat_every_100ms);
  RUN_TEST(test_hello_req_repeats_every_second_until_hello);
  RUN_TEST(test_handshake_acks_matching_proto_and_sends_owner_hint);
  RUN_TEST(test_stop_master_is_not_body_offline);
  RUN_TEST(test_handshake_rejects_wrong_proto);
  RUN_TEST(test_uno_reboot_hello_is_acked_again);
  RUN_TEST(test_silent_uno_restarts_handshake);
  RUN_TEST(test_not_master_never_sends_heartbeat_or_hello_ack);
  RUN_TEST(test_drive_only_when_synced);
  RUN_TEST(test_bridge_never_invents_drive);
  RUN_TEST(test_stop_resend_until_ack);
  RUN_TEST(test_stop_purges_queued_drive_and_cam_joy);
  RUN_TEST(test_stop_gives_up_after_ten_sends);
  RUN_TEST(test_cam_home_ack_and_timeout);
  RUN_TEST(test_telemetry_snapshot_and_events);
  RUN_TEST(test_telem_fast_keeps_its_arrival_time);
  RUN_TEST(test_snapshot_heading_is_corrected_by_the_gyro_scale);
  RUN_TEST(test_slow_count_counts_slow_frames);
  RUN_TEST(test_wifi_reset_request_answered_unavailable);
  RUN_TEST(test_sound_and_cam_lock_acked);
  RUN_TEST(test_audio_mode_acked);
  RUN_TEST(test_setting_frames_and_value_event);
  RUN_TEST(test_self_test_record_and_calibrate);
  return UNITY_END();
}
