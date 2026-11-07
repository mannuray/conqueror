// Plan Z2b-1 Task 4: AiPages - which /ws clients are /ai pages, and the ai_at text for them (bridge LLD R27.1-R27.3).
#include <string.h>
#include <unity.h>

#include "core/ai_pages.h"

using cq::core::AiPages;

void setUp() {}
void tearDown() {}

void test_text_exact_with_url() {
  char b[96];
  const size_t n = AiPages::text(b, sizeof b, "ws://192.168.1.23:8770/feed", 1);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ai_at\",\"url\":\"ws://192.168.1.23:8770/feed\",\"proto\":1}", b);
  TEST_ASSERT_EQUAL(strlen(b), n);
}

void test_text_exact_null() {
  char b[96];
  const size_t n = AiPages::text(b, sizeof b, "", 1);
  TEST_ASSERT_EQUAL_STRING("{\"t\":\"ai_at\",\"url\":null}", b);
  TEST_ASSERT_EQUAL(strlen(b), n);
}

void test_text_too_small_is_nothing() {
  char b[10];
  TEST_ASSERT_EQUAL(0, AiPages::text(b, sizeof b, "ws://192.168.1.23:8770/feed", 1));
}

void test_hello_registers_up_to_four() {
  AiPages p;
  for (int fd = 3; fd < 7; ++fd) TEST_ASSERT_TRUE(p.onHello(fd));
  TEST_ASSERT_TRUE(p.onHello(4));   // again: still one place
  TEST_ASSERT_FALSE(p.onHello(9));  // full
  TEST_ASSERT_EQUAL(4, p.count());
  TEST_ASSERT_TRUE(p.contains(6));
  TEST_ASSERT_FALSE(p.contains(9));
}

void test_close_forgets() {
  AiPages p;
  p.onHello(3);
  p.onHello(5);
  p.onClose(3);
  p.onClose(8);  // not a page: nothing
  TEST_ASSERT_FALSE(p.contains(3));
  TEST_ASSERT_TRUE(p.contains(5));
  TEST_ASSERT_EQUAL(1, p.count());
  TEST_ASSERT_TRUE(p.onHello(7));
}

void test_changed_only_on_a_new_url() {
  AiPages p;
  TEST_ASSERT_FALSE(p.changed(""));  // nothing announced yet and no feed: no change
  TEST_ASSERT_TRUE(p.changed("ws://192.168.1.23:8770/feed"));
  TEST_ASSERT_FALSE(p.changed("ws://192.168.1.23:8770/feed"));
  TEST_ASSERT_TRUE(p.changed("ws://192.168.1.24:8770/feed"));
}

// The compare runs on the hub's state once a loop, so any way the server's socket ends (a close, a ghost, the 3 s
// silent rule) shows up as the URL gone.
void test_url_gone_is_a_change() {
  AiPages p;
  p.changed("ws://192.168.1.23:8770/feed");
  TEST_ASSERT_TRUE(p.changed(""));
  TEST_ASSERT_FALSE(p.changed(""));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_text_exact_with_url);
  RUN_TEST(test_text_exact_null);
  RUN_TEST(test_text_too_small_is_nothing);
  RUN_TEST(test_hello_registers_up_to_four);
  RUN_TEST(test_close_forgets);
  RUN_TEST(test_changed_only_on_a_new_url);
  RUN_TEST(test_url_gone_is_a_change);
  return UNITY_END();
}
