// TelemetryPublisher: td / tds per client, tf every 5th TELEM_FAST, ts at 1 Hz, hello (bridge LLD R2.14, R5, R15.4).
#include <unity.h>

#include "../support/frames.h"
#include "app/telemetry_publisher.h"
#include "fakes/fake_ports.h"

using namespace cq;

namespace {
core::WsClients ws;
app::Replies out(ws);
core::FrameTx tx;
core::BodyLink* link = nullptr;
app::TelemetryPublisher* tel = nullptr;

core::OwnerView owner() { return {"none", ""}; }
core::PrimitiveView prim() { return {false, core::PrimKind::kNone, 0, false}; }
bool offline() { return false; }

void fastFrames(int n, uint32_t now) {
  for (int i = 0; i < n; ++i) {
    uint8_t f[64];
    const size_t len = cqtest::frame(proto::TelemFastMsg{}, static_cast<uint8_t>(i), f);
    link->onUartBytes(f, len, now);
    const app::TelemetryPublisher::News news = tel->poll();
    if (news.fast) tel->afterFast();
  }
}
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  ws.add(3, 0);
  delete tel;
  delete link;
  link = new core::BodyLink(tx);
  tel = new app::TelemetryPublisher(out, *link);
  tel->setViews({owner, prim, offline});
}
void tearDown() {}

void test_tf_every_fifth_fast_frame() {
  fastFrames(10, 100);
  TEST_ASSERT_EQUAL(2, fakes::countTo(3, "\"t\":\"tf\""));
}

void test_ts_once_per_second_even_without_frames() {
  tel->tickTs(1000);
  tel->tickTs(1500);
  tel->tickTs(2000);
  TEST_ASSERT_EQUAL(2, fakes::countTo(3, "\"t\":\"ts\""));
}

void test_td_only_to_clients_that_asked_at_their_divider() {
  ws.add(4, 0);
  ws.setDebug(4, 5);  // 10 Hz: every 5th frame
  fastFrames(10, 100);
  TEST_ASSERT_EQUAL(2, fakes::countTo(4, "\"t\":\"td\""));
  TEST_ASSERT_EQUAL(0, fakes::countTo(3, "\"t\":\"td\""));
}

void test_hello_sends_welcome_then_ts() {
  tel->hello(3, "abc1234", "OV3660");
  TEST_ASSERT_EQUAL(2, fakes::world().sent.size());
  TEST_ASSERT_NOT_EQUAL(std::string::npos, fakes::world().sent[0].json.find("\"t\":\"welcome\""));
  TEST_ASSERT_NOT_EQUAL(std::string::npos, fakes::world().sent[1].json.find("\"t\":\"ts\""));
}

void test_event_seq_counts_up() {
  const uint32_t a = tel->nextEventSeq();
  TEST_ASSERT_EQUAL(a + 1, tel->nextEventSeq());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_tf_every_fifth_fast_frame);
  RUN_TEST(test_ts_once_per_second_even_without_frames);
  RUN_TEST(test_td_only_to_clients_that_asked_at_their_divider);
  RUN_TEST(test_hello_sends_welcome_then_ts);
  RUN_TEST(test_event_seq_counts_up);
  return UNITY_END();
}
