#include <unity.h>

#include "domain/lite/lite_frames.h"
#include "domain/lite/lite_telemetry.h"

using namespace cq;
using lite_frames::FrameUse;

void setUp() {}
void tearDown() {}

namespace {
uint8_t t(proto::FrameType f) { return static_cast<uint8_t>(f); }
}  // namespace

void test_frames_driving_is_handled() {
  TEST_ASSERT_EQUAL(FrameUse::kHandled, lite_frames::frameUse(t(proto::FrameType::kDrive)));
  TEST_ASSERT_EQUAL(FrameUse::kHandled, lite_frames::frameUse(t(proto::FrameType::kStop)));
  TEST_ASSERT_EQUAL(FrameUse::kHandled, lite_frames::frameUse(t(proto::FrameType::kEnvelope)));
  TEST_ASSERT_EQUAL(FrameUse::kHandled, lite_frames::frameUse(t(proto::FrameType::kCamSet)));
  TEST_ASSERT_TRUE(lite_frames::needsAck(t(proto::FrameType::kStop)));
  TEST_ASSERT_FALSE(lite_frames::needsAck(t(proto::FrameType::kDrive)));
}

void test_frames_unsupported_are_unavailable() {
  const proto::FrameType gone[] = {proto::FrameType::kTurn, proto::FrameType::kMove, proto::FrameType::kDriveRate,
                                   proto::FrameType::kCancel, proto::FrameType::kCalibrate, proto::FrameType::kProfile,
                                   proto::FrameType::kSafetyOverride};
  for (const proto::FrameType f : gone) {
    TEST_ASSERT_EQUAL(FrameUse::kUnavailable, lite_frames::frameUse(t(f)));
    TEST_ASSERT_TRUE(lite_frames::needsAck(t(f)));  // the sender learns why nothing happened
  }
  TEST_ASSERT_EQUAL(FrameUse::kUnknown, lite_frames::frameUse(0x7F));
}

void test_frames_hello_caps() {
  const uint8_t audio = 1u << static_cast<uint8_t>(proto::CapBit::kAudioLed);
  TEST_ASSERT_EQUAL_UINT8(audio, lite_frames::helloCaps(false));
  TEST_ASSERT_EQUAL_UINT8(audio | 1u, lite_frames::helloCaps(true));
}

// Spec 02 §D4 (owner, option A): the phone/AI sound and LED commands of §A9 are handled.
void test_frames_sound_and_led_handled() {
  const proto::FrameType on[] = {proto::FrameType::kSound, proto::FrameType::kTone, proto::FrameType::kAudioMode,
                                 proto::FrameType::kLed};
  for (const proto::FrameType f : on) {
    TEST_ASSERT_EQUAL(FrameUse::kHandled, lite_frames::frameUse(t(f)));
    TEST_ASSERT_TRUE(lite_frames::needsAck(t(f)));
  }
}

// Final review I1: TELEM_SLOW carries the same safety-state bits as the full firmware, and the hw byte.
void test_slow_flags_safety_state() {
  const uint32_t f = lite_telemetry::slowFlags(0, false, true, false, true);
  TEST_ASSERT_TRUE(f & (1UL << static_cast<uint8_t>(proto::SlowFlagBit::kBrakeOff)));
  TEST_ASSERT_TRUE(f & (1UL << static_cast<uint8_t>(proto::SlowFlagBit::kEnvelope)));
  TEST_ASSERT_TRUE(f & (1UL << static_cast<uint8_t>(proto::SlowFlagBit::kLinkLost)));
  TEST_ASSERT_TRUE(f & (1UL << static_cast<uint8_t>(proto::SlowFlagBit::kDeadman)));
  const uint32_t battlow = 1UL << static_cast<uint8_t>(proto::SlowFlagBit::kBattLow);
  // bits 6/7 (envelope, deadman) cleared when false; battery and mute/quiet (bits 4/5) are kept as they are
  TEST_ASSERT_EQUAL_UINT32(battlow | 0x30, lite_telemetry::slowFlags(battlow | 0xF0, true, false, true, false) & 0xFF);
}

void test_hw_byte() {
  TEST_ASSERT_EQUAL_UINT8(0x06, lite_telemetry::hwByte(false, true, true));
  TEST_ASSERT_EQUAL_UINT8(0x01, lite_telemetry::hwByte(true, false, false));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_frames_driving_is_handled);
  RUN_TEST(test_frames_unsupported_are_unavailable);
  RUN_TEST(test_frames_hello_caps);
  RUN_TEST(test_frames_sound_and_led_handled);
  RUN_TEST(test_slow_flags_safety_state);
  RUN_TEST(test_hw_byte);
  return UNITY_END();
}
