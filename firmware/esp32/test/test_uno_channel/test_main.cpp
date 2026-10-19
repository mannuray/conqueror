// UnoChannel: one queue for every acked Uno request; STOP keeps its urgent slot (bridge LLD R15.5).
#include <unity.h>

#include "../support/frames.h"
#include "../support/wire.h"
#include "app/uno_channel.h"
#include "fakes/fake_ports.h"

using namespace cq;
using FT = proto::FrameType;

namespace {
core::WsClients ws;
app::Replies out(ws);
core::FrameTx* tx = nullptr;
core::BodyLink* link = nullptr;
app::UnoChannel* uno = nullptr;
cqtest::Wire wire;

void unoSends(const uint8_t* f, size_t n, uint32_t now) { link->onUartBytes(f, n, now); }

void pump() {  // the root: link events -> the channel
  core::LinkEvent ev{};
  while (link->popEvent(ev)) {
    if (ev.kind == core::LinkEventKind::kAckResult) uno->onAck(ev.seq, ev.status);
    if (ev.kind == core::LinkEventKind::kStopResult) uno->onStopResult(ev.status);
    if (ev.kind == core::LinkEventKind::kSettingValue) {
      uno->onSettingValue(ev.code, static_cast<int32_t>(static_cast<uint16_t>(ev.a)));
    }
  }
}

void ackLast(FT type, uint8_t status, uint32_t now) {
  wire.drain(*tx);
  const cqtest::Sent* s = wire.last(type);
  TEST_ASSERT_NOT_NULL(s);
  uint8_t f[16];
  const size_t n = cqtest::frame(proto::AckMsg{s->seq, status}, 9, f);
  unoSends(f, n, now);
  pump();
}

core::AckedOp op(core::AckedOp::Kind k, int fd = -1, int32_t id = -1) {
  core::AckedOp o;
  o.kind = k;
  o.fd = fd;
  o.id = id;
  return o;
}

void synced() {
  link->startMaster(0);
  uint8_t f[64];
  const size_t n = cqtest::frame(proto::HelloMsg{0x0100, proto::kProtocolVersion, 1, 1, 0x1UL}, 1, f);
  unoSends(f, n, 10);
  pump();
  wire.drain(*tx);
  wire.clear();
}
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  ws.add(3, 0);
  delete uno;
  delete link;
  delete tx;
  tx = new core::FrameTx();
  link = new core::BodyLink(*tx);
  uno = new app::UnoChannel(*link, *tx, out);
  wire.clear();
  synced();
}
void tearDown() {}

void test_requests_go_one_at_a_time_in_order() {
  TEST_ASSERT_TRUE(uno->request(op(core::AckedOp::kSound)));
  TEST_ASSERT_TRUE(uno->request(op(core::AckedOp::kLed)));
  uno->tick(100);
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kSound));
  TEST_ASSERT_EQUAL(0, wire.count(FT::kLed));
  ackLast(FT::kSound, 0, 110);
  uno->tick(112);
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kLed));
}

void test_drive_page_request_waits_instead_of_busy() {  // R15.5: the one behaviour change
  uno->request(op(core::AckedOp::kLed));
  uno->tick(100);
  TEST_ASSERT_TRUE(uno->request(op(core::AckedOp::kCamHome, 3, 9)));
  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());  // no BUSY
  ackLast(FT::kLed, 0, 110);
  uno->tick(112);
  ackLast(FT::kCamHome, 0, 120);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ack\",\"id\":9}", fakes::lastTo(3).c_str());
}

void test_ninth_request_is_refused() {
  for (int i = 0; i < 8; ++i) TEST_ASSERT_TRUE(uno->request(op(core::AckedOp::kSound)));
  TEST_ASSERT_FALSE(uno->request(op(core::AckedOp::kSound)));
}

void test_setting_get_answered_by_its_value() {
  core::AckedOp g = op(core::AckedOp::kSettingGet, 3, 4);
  g.a = 66;
  uno->request(g);
  uno->tick(100);
  ackLast(FT::kSettingGet, 0, 110);
  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());  // waits for SETTING_VALUE
  uno->onSettingValue(66, 19);
  TEST_ASSERT_NOT_EQUAL(std::string::npos, fakes::lastTo(3).find("\"value\":19"));
}

void test_no_answer_times_out_and_the_next_goes() {
  uno->request(op(core::AckedOp::kCamLock, 3, 5));
  uno->request(op(core::AckedOp::kSound));
  uno->tick(100);
  for (uint32_t t = 102; t <= 320; t += 2) {
    link->step(t);
    pump();
    uno->tick(t);
  }
  TEST_ASSERT_TRUE(fakes::countTo(3, "nack") == 1);
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kSound));
}

void test_stop_overtakes_queued_requests() {  // Review Focus 1
  uno->request(op(core::AckedOp::kSound));
  uno->request(op(core::AckedOp::kLed));
  uno->request(op(core::AckedOp::kCamHome, 3, 7));
  uno->tick(100);  // SOUND in flight
  link->sendStop(105);
  uno->waitStop(3, 9);
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kStop));
  ackLast(FT::kStop, 0, 110);
  TEST_ASSERT_EQUAL(1, fakes::countTo(3, "\"t\":\"ack\",\"id\":9"));
  ackLast(FT::kSound, 0, 115);
  uno->tick(116);
  wire.drain(*tx);
  TEST_ASSERT_EQUAL(1, wire.count(FT::kLed));  // the queue carries on after the STOP
}

void test_body_offline_answers_and_clears_the_queue() {  // Review Focus 2
  uno->request(op(core::AckedOp::kCamHome, 3, 1));
  uno->request(op(core::AckedOp::kCamLock, 3, 2));
  uno->tick(100);
  uno->onBodyOffline();
  TEST_ASSERT_EQUAL(2, fakes::countTo(3, "BODY_OFFLINE"));
  TEST_ASSERT_TRUE(uno->request(op(core::AckedOp::kSound)));  // empty again
}

void test_ota_answers_everything_and_stops_master() {
  uno->request(op(core::AckedOp::kCamHome, 3, 1));
  uno->request(op(core::AckedOp::kSettingGet, 3, 2));
  uno->tick(100);
  uno->waitStop(3, 3);
  uno->stopForOta();
  TEST_ASSERT_FALSE(link->synced());
  TEST_ASSERT_EQUAL(3, fakes::countTo(3, "nack"));
}

// ---- Plan N3: the running Logo program's requests (TONE / CAM_SET frames; answers through its sink) ----
uint8_t g_logo_status = 0xAA;
int g_logo_answers = 0;
void logoDone(uint8_t status) {
  g_logo_status = status;
  ++g_logo_answers;
}

core::AckedOp logoOp(core::AckedOp::Kind k) {
  core::AckedOp o = op(k, core::kLogoClient);
  uno->setLogoDone(logoDone);
  g_logo_status = 0xAA;
  g_logo_answers = 0;
  return o;
}

void test_logo_tone_and_cam_set_frames() {
  core::AckedOp t = logoOp(core::AckedOp::kTone);
  t.n_notes = 2;
  t.hz[0] = 262;
  t.ms[0] = 250;
  t.hz[1] = 0;
  t.ms[1] = 500;
  uno->request(t);
  uno->tick(100);
  wire.drain(*tx);
  const cqtest::Sent* s = wire.last(FT::kTone);
  TEST_ASSERT_NOT_NULL(s);
  proto::ToneMsg m{};
  TEST_ASSERT_TRUE(proto::decode(s->payload, s->len, m));
  TEST_ASSERT_EQUAL(2, m.n);
  TEST_ASSERT_EQUAL(262, m.notes[0].hz);
  TEST_ASSERT_EQUAL(500, m.notes[1].ms);
  ackLast(FT::kTone, 0, 110);
  core::AckedOp c = logoOp(core::AckedOp::kCamSet);
  c.pan_dd = 600;
  c.tilt_dd = 1000;
  uno->request(c);
  uno->tick(120);
  wire.drain(*tx);
  proto::CamSetMsg cm{};
  TEST_ASSERT_TRUE(proto::decode(wire.last(FT::kCamSet)->payload, wire.last(FT::kCamSet)->len, cm));
  TEST_ASSERT_EQUAL(600, cm.pan);
  TEST_ASSERT_EQUAL(1000, cm.tilt);
}

void test_logo_answers_go_to_its_sink_never_to_a_client() {
  uno->request(logoOp(core::AckedOp::kLed));
  uno->tick(100);
  ackLast(FT::kLed, 0, 110);
  TEST_ASSERT_EQUAL(1, g_logo_answers);
  TEST_ASSERT_EQUAL(0, g_logo_status);
  uno->request(logoOp(core::AckedOp::kSound));
  uno->tick(120);
  ackLast(FT::kSound, static_cast<uint8_t>(proto::AckStatus::kBusy), 130);
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::AckStatus::kBusy), g_logo_status);
  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());
}

void test_logo_no_answer_and_body_offline() {
  uno->request(logoOp(core::AckedOp::kLed));
  uno->tick(100);
  for (uint32_t t = 102; t <= 800; t += 2) {
    link->step(t);
    pump();
    uno->tick(t);
  }
  TEST_ASSERT_EQUAL(1, g_logo_answers);
  TEST_ASSERT_EQUAL(app::UnoChannel::kLogoTimeout, g_logo_status);  // BodyLink's own 200 ms timeout, as one value
  uno->request(logoOp(core::AckedOp::kLed));
  uno->onBodyOffline();
  TEST_ASSERT_EQUAL(app::UnoChannel::kLogoOffline, g_logo_status);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_requests_go_one_at_a_time_in_order);
  RUN_TEST(test_drive_page_request_waits_instead_of_busy);
  RUN_TEST(test_ninth_request_is_refused);
  RUN_TEST(test_setting_get_answered_by_its_value);
  RUN_TEST(test_no_answer_times_out_and_the_next_goes);
  RUN_TEST(test_stop_overtakes_queued_requests);
  RUN_TEST(test_body_offline_answers_and_clears_the_queue);
  RUN_TEST(test_ota_answers_everything_and_stops_master);
  RUN_TEST(test_logo_tone_and_cam_set_frames);
  RUN_TEST(test_logo_answers_go_to_its_sink_never_to_a_client);
  RUN_TEST(test_logo_no_answer_and_body_offline);
  return UNITY_END();
}
