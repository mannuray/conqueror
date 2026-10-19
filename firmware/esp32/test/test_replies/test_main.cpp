// Replies: the JSON buffers + the ClientOut port (bridge LLD R15.3).
#include <stdio.h>
#include <unity.h>

#include "app/replies.h"
#include "fakes/fake_ports.h"

using namespace cq;

namespace {
core::WsClients ws;
app::Replies out(ws);
}  // namespace

void setUp() {
  fakes::reset();
  ws = core::WsClients();
  ws.add(3, 0);
}
void tearDown() {}

void test_ack_only_with_an_id() {
  out.ack(3, -1);
  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());
  out.ack(3, 7);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ack\",\"id\":7}", fakes::lastTo(3).c_str());
}

void test_reply_to_unknown_fd_is_dropped() {  // Review Focus 4
  out.nack(9, 1, core::BridgeReason::kBusy);
  TEST_ASSERT_EQUAL(0, fakes::world().sent.size());
}

void test_all_reaches_every_client() {
  ws.add(4, 0);
  const size_t n = static_cast<size_t>(snprintf(out.buf(), out.cap(), "{\"t\":\"x\"}"));
  out.all(n);
  TEST_ASSERT_EQUAL(2, fakes::world().sent.size());
}

void test_big_reply_failure_is_logged() {
  fakes::world().send_ok = false;
  const size_t n = static_cast<size_t>(snprintf(out.big(), out.bigCap(), "{\"t\":\"log\"}"));
  out.bigTo(3, n);
  TEST_ASSERT_EQUAL(1, fakes::world().logs.size());
}

void test_stream_reject_once_per_stream() {
  out.streamReject(3, core::BridgeReason::kNotOwner);
  out.streamReject(3, core::BridgeReason::kNotOwner);
  TEST_ASSERT_EQUAL(1, fakes::world().sent.size());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_ack_only_with_an_id);
  RUN_TEST(test_reply_to_unknown_fd_is_dropped);
  RUN_TEST(test_all_reaches_every_client);
  RUN_TEST(test_big_reply_failure_is_logged);
  RUN_TEST(test_stream_reject_once_per_stream);
  return UNITY_END();
}
