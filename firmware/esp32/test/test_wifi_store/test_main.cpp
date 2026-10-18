// Plan M (bridge LLD R9.2): saved networks + AP password.
#include <string.h>
#include <unity.h>

#include "core/wifi_store.h"

using namespace cq::core;

void setUp() {}
void tearDown() {}

namespace {
int S(StoreResult r) { return static_cast<int>(r); }
ScanEntry net(const char* ssid, int8_t rssi) {
  ScanEntry e{};
  strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
  e.rssi = rssi;
  e.secure = true;
  return e;
}
}  // namespace

void test_store_seed_and_read() {
  WifiStore s;
  s.clear();
  s.seed("MyHome", "homepass1", "appass12");
  TEST_ASSERT_EQUAL_UINT8(1, s.count());
  TEST_ASSERT_EQUAL_STRING("MyHome", s.ssid(0));
  TEST_ASSERT_EQUAL_STRING("homepass1", s.pass(0));
  TEST_ASSERT_EQUAL_STRING("appass12", s.apPass());
  TEST_ASSERT_EQUAL_STRING("", s.ssid(1));
}

void test_store_save_new_edit_full() {
  WifiStore s;
  s.clear();
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(-1, "A", "password1")));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(-1, "B", "")));  // open network
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(-1, "A", "password2")));  // same name: edit in place
  TEST_ASSERT_EQUAL_UINT8(2, s.count());
  TEST_ASSERT_EQUAL_STRING("password2", s.pass(0));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(-1, "C", "password3")));
  TEST_ASSERT_EQUAL(S(StoreResult::kFull), S(s.save(-1, "D", "password4")));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.save(1, "D", "password4")));  // explicit slot replaces B
  TEST_ASSERT_EQUAL_STRING("D", s.ssid(1));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadSlot), S(s.save(3, "E", "password5")));
}

void test_store_rejects_bad_input() {
  WifiStore s;
  s.clear();
  TEST_ASSERT_EQUAL(S(StoreResult::kBadSsid), S(s.save(-1, "", "password1")));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadSsid), S(s.save(-1, "123456789012345678901234567890123", "password1")));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadPassword), S(s.save(-1, "A", "short")));
  char longpw[70];
  memset(longpw, 'x', 64);
  longpw[64] = '\0';
  TEST_ASSERT_EQUAL(S(StoreResult::kBadPassword), S(s.save(-1, "A", longpw)));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadPassword), S(s.setApPassword("1234567")));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.setApPassword("12345678")));
  TEST_ASSERT_EQUAL_UINT8(0, s.count());
}

void test_store_forget_one_and_all() {
  WifiStore s;
  s.clear();
  s.save(-1, "A", "password1");
  s.save(-1, "B", "password2");
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.forget(0)));
  TEST_ASSERT_EQUAL_UINT8(1, s.count());
  TEST_ASSERT_EQUAL(-1, s.slotOf("A"));
  TEST_ASSERT_EQUAL(1, s.slotOf("B"));
  TEST_ASSERT_EQUAL(S(StoreResult::kBadSlot), S(s.forget(5)));
  TEST_ASSERT_EQUAL(S(StoreResult::kOk), S(s.forget(-1)));
  TEST_ASSERT_EQUAL_UINT8(0, s.count());
}

void test_store_pick_strongest_visible() {
  WifiStore s;
  s.clear();
  s.save(-1, "Home", "password1");
  s.save(-1, "Office", "password2");
  const ScanEntry nets[] = {net("Neighbour", -40), net("Office", -70), net("Home", -55)};
  TEST_ASSERT_EQUAL(0, s.pickStrongest(nets, 3));  // Home -55 beats Office -70; Neighbour is not saved
  const ScanEntry far[] = {net("Neighbour", -40)};
  TEST_ASSERT_EQUAL(-1, s.pickStrongest(far, 1));
  TEST_ASSERT_EQUAL(-1, s.pickStrongest(nullptr, 0));
}

void test_store_encode_decode_round_trip() {
  WifiStore a;
  a.clear();
  a.seed("Home", "password1", "appass12");
  a.save(-1, "Office", "password2");
  uint8_t blob[WifiStore::kBlobSize];
  TEST_ASSERT_EQUAL(WifiStore::kBlobSize, a.encode(blob));
  WifiStore b;
  TEST_ASSERT_TRUE(b.decode(blob, sizeof(blob)));
  TEST_ASSERT_EQUAL_UINT8(2, b.count());
  TEST_ASSERT_EQUAL_STRING("Office", b.ssid(1));
  TEST_ASSERT_EQUAL_STRING("appass12", b.apPass());
}

void test_store_decode_rejects_corrupt() {  // Review Focus 2
  WifiStore a;
  a.clear();
  a.seed("Home", "password1", "appass12");
  uint8_t blob[WifiStore::kBlobSize];
  a.encode(blob);
  WifiStore b;
  TEST_ASSERT_FALSE(b.decode(blob, sizeof(blob) - 1));  // short
  TEST_ASSERT_EQUAL_UINT8(0, b.count());
  blob[0] = 99;  // unknown version
  TEST_ASSERT_FALSE(b.decode(blob, sizeof(blob)));
  a.encode(blob);
  memset(blob + 1, 'x', 33);  // ssid without a terminator
  TEST_ASSERT_FALSE(b.decode(blob, sizeof(blob)));
  TEST_ASSERT_EQUAL_UINT8(0, b.count());
}

void test_store_edit_in_use_flags_reconnect() {  // Review Focus 1: the caller compares before / after
  WifiStore s;
  s.clear();
  s.save(-1, "Home", "password1");
  const int in_use = s.slotOf("Home");
  s.save(-1, "Home", "password9");
  TEST_ASSERT_EQUAL(in_use, s.slotOf("Home"));
  TEST_ASSERT_EQUAL_STRING("password9", s.pass(in_use));  // changed: WifiManager reconnects (Task 5)
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_store_seed_and_read);
  RUN_TEST(test_store_save_new_edit_full);
  RUN_TEST(test_store_rejects_bad_input);
  RUN_TEST(test_store_forget_one_and_all);
  RUN_TEST(test_store_pick_strongest_visible);
  RUN_TEST(test_store_encode_decode_round_trip);
  RUN_TEST(test_store_decode_rejects_corrupt);
  RUN_TEST(test_store_edit_in_use_flags_reconnect);
  return UNITY_END();
}
