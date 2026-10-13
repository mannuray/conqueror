#include <string.h>
#include <unity.h>

#include "domain/settings/crc16.h"
#include "domain/settings/eeprom_writer.h"
#include "domain/settings/settings.h"
#include "domain/settings/settings_saver.h"

using namespace cq;

void setUp() {}
void tearDown() {}

void test_defaults_match_descriptors() {
  Settings s;
  s.loadDefaults();
  int32_t v = 0;
  TEST_ASSERT_TRUE(s.get(static_cast<uint8_t>(proto::SettingId::kDeadman), v));
  TEST_ASSERT_EQUAL(300, v);
  TEST_ASSERT_TRUE(s.get(static_cast<uint8_t>(proto::SettingId::kCountsPerRev), v));
  TEST_ASSERT_EQUAL(960, v);
  TEST_ASSERT_TRUE(s.get(static_cast<uint8_t>(proto::SettingId::kPidHeadingKp), v));
  TEST_ASSERT_EQUAL(3000, v);
  TEST_ASSERT_EQUAL(300, s.global().deadman);
}

void test_every_descriptor_has_a_slot() {
  Settings s;
  s.loadDefaults();
  for (uint8_t i = 0; i < proto::kSettingCount; ++i) {
    int32_t v = 0;
    TEST_ASSERT_TRUE_MESSAGE(s.get(proto::kSettingDescriptors[i].id, v), "missing slot");
    TEST_ASSERT_EQUAL(proto::kSettingDescriptors[i].def, v);
  }
}

void test_set_validates_range_and_unknown() {
  Settings s;
  s.loadDefaults();
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(s.set(static_cast<uint8_t>(proto::SettingId::kTiltLimit), 40)));
  TEST_ASSERT_EQUAL(40, s.global().tilt_limit);
  TEST_ASSERT_TRUE(s.dirty());
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kSettingOutOfRange),
                    static_cast<int>(s.set(static_cast<uint8_t>(proto::SettingId::kTiltLimit), 99)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kSettingUnknown),
                    static_cast<int>(s.set(200, 1)));
}

// Ordering invariants are checked against the current values; the hub orders its writes.
void test_set_rejects_cross_field_inversions() {
  Settings s;
  s.loadDefaults();  // pan 0..180, tilt 30..110, obstacle stop 15 / slow 50, batt crit 6600 / low 7000
  const auto set = [&s](proto::SettingId id, int32_t v) {
    return static_cast<int>(s.set(static_cast<uint8_t>(id), v));
  };
  const int ok = static_cast<int>(proto::AckStatus::kOk);
  const int range = static_cast<int>(proto::AckStatus::kSettingOutOfRange);
  TEST_ASSERT_EQUAL(ok, set(proto::SettingId::kPanMax, 90));
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kPanMin, 91));   // min above max
  TEST_ASSERT_EQUAL(ok, set(proto::SettingId::kPanMin, 90));      // equal is allowed
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kPanMax, 89));   // max below min
  TEST_ASSERT_EQUAL(90, s.global().pan_max);                      // rejected value not stored
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kTiltMax, 29));
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kTiltMin, 111));
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kObstacleSlow, 15));  // slow must be > stop
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kObstacleStop, 50));
  TEST_ASSERT_EQUAL(ok, set(proto::SettingId::kObstacleStop, 49));
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kBattCrit, 7200));    // crit must be < low (7200)
  TEST_ASSERT_EQUAL(range, set(proto::SettingId::kBattLow, 6600));
  TEST_ASSERT_EQUAL(ok, set(proto::SettingId::kBattLow, 6601));
}

void test_serialize_roundtrip_and_bad_crc_loads_defaults() {
  Settings a;
  a.loadDefaults();
  a.set(static_cast<uint8_t>(proto::SettingId::kPanTrim), -25);
  uint8_t buf[96];
  const uint8_t n = a.serialize(Settings::kBlockGlobal, buf);
  const uint16_t crc = crc16(buf, n);
  Settings b;
  b.loadDefaults();
  TEST_ASSERT_TRUE(b.deserialize(Settings::kBlockGlobal, buf, n, crc));
  TEST_ASSERT_EQUAL(-25, b.global().pan_trim);
  Settings c;
  c.loadDefaults();
  TEST_ASSERT_FALSE(c.deserialize(Settings::kBlockGlobal, buf, n, static_cast<uint16_t>(crc ^ 1)));
  TEST_ASSERT_EQUAL(0, c.global().pan_trim);  // defaults kept
}

void test_crc16_check_value() {
  const uint8_t msg[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16(msg, 9));  // CRC-16/CCITT-FALSE
}

namespace {
uint8_t g_mem[1024];
bool g_busy = false;
int g_writes = 0;
uint8_t fakeRead(uint16_t a) { return g_mem[a]; }
bool fakeWrite(uint16_t a, uint8_t v) {
  if (g_busy) return false;
  g_mem[a] = v;
  ++g_writes;
  g_busy = true;
  return true;
}
bool fakeBusy() { return g_busy; }
}  // namespace

void test_writer_one_byte_per_step() {
  memset(g_mem, 0xFF, sizeof g_mem);
  g_writes = 0;
  EepromWriter w;
  const uint8_t data[] = {1, 2, 3, 0xFF};
  TEST_ASSERT_TRUE(w.enqueue(10, data, 4));
  const EepromIo io{fakeRead, fakeWrite, fakeBusy};
  int steps = 0;
  while (!w.idle() && steps < 50) {
    w.step(io);
    g_busy = false;  // write completes between scheduler passes
    ++steps;
    TEST_ASSERT_TRUE(g_writes <= steps);
  }
  TEST_ASSERT_EQUAL(3, g_writes);  // unchanged 0xFF byte skipped
  TEST_ASSERT_EQUAL(1, g_mem[10]);
  TEST_ASSERT_EQUAL(3, g_mem[12]);
}

void test_writer_waits_while_busy() {
  memset(g_mem, 0, sizeof g_mem);
  g_writes = 0;
  EepromWriter w;
  const uint8_t data[] = {7};
  w.enqueue(0, data, 1);
  g_busy = true;
  const EepromIo io{fakeRead, fakeWrite, fakeBusy};
  w.step(io);
  TEST_ASSERT_EQUAL(0, g_writes);
  g_busy = false;
  w.step(io);
  TEST_ASSERT_EQUAL(1, g_writes);
}

// LLD §10.10: every setting changed at once still saves completely (staged per block, no dropped bytes).
void test_save_every_setting_at_once_never_overflows() {
  memset(g_mem, 0xFF, sizeof g_mem);
  g_busy = false;
  Settings s;
  s.loadDefaults();
  for (uint8_t i = 0; i < proto::kSettingCount; ++i) {  // max where the ordering invariants allow, else min
    const proto::SettingDescriptor& d = proto::kSettingDescriptors[i];
    if (s.set(d.id, d.max) != proto::AckStatus::kOk) s.set(d.id, d.min);
  }
  EepromWriter w;
  SettingsSaver saver;
  const EepromIo io{fakeRead, fakeWrite, fakeBusy};
  saver.request();
  TEST_ASSERT_FALSE(saver.step(s, w, io, false));  // moving: the save waits
  TEST_ASSERT_FALSE(saver.busy());
  bool done = false;
  for (int pass = 0; pass < 1000 && !done; ++pass) {
    done = saver.step(s, w, io, true);
    TEST_ASSERT_TRUE(w.idle() || saver.busy());
    g_busy = false;  // the EEPROM write completes between scheduler passes
  }
  TEST_ASSERT_TRUE(done);
  TEST_ASSERT_FALSE(saver.busy());
  TEST_ASSERT_EQUAL('C', g_mem[kEeMagicAddr]);
  TEST_ASSERT_EQUAL(kEeLayoutVersion, g_mem[kEeMagicAddr + 2]);
  Settings r;
  r.loadDefaults();
  const uint16_t addrs[2] = {kEeGlobalAddr, kEeProfileAddr};
  for (uint8_t b = 0; b < 2; ++b) {
    const uint16_t a = addrs[b];
    const uint8_t len = g_mem[a];
    const uint16_t crc = static_cast<uint16_t>(g_mem[a + 1 + len] | (g_mem[a + 2 + len] << 8));
    TEST_ASSERT_TRUE(r.deserialize(b == 0 ? Settings::kBlockGlobal : Settings::kBlockProfile, &g_mem[a + 1], len, crc));
  }
  for (uint8_t i = 0; i < proto::kSettingCount; ++i) {  // every value survived the round trip
    int32_t want = 0;
    int32_t got = 0;
    s.get(proto::kSettingDescriptors[i].id, want);
    r.get(proto::kSettingDescriptors[i].id, got);
    TEST_ASSERT_EQUAL(want, got);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_defaults_match_descriptors);
  RUN_TEST(test_every_descriptor_has_a_slot);
  RUN_TEST(test_set_validates_range_and_unknown);
  RUN_TEST(test_set_rejects_cross_field_inversions);
  RUN_TEST(test_serialize_roundtrip_and_bad_crc_loads_defaults);
  RUN_TEST(test_crc16_check_value);
  RUN_TEST(test_writer_one_byte_per_step);
  RUN_TEST(test_save_every_setting_at_once_never_overflows);
  RUN_TEST(test_writer_waits_while_busy);
  return UNITY_END();
}
