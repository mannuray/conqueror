#include <unity.h>

#include "domain/lite/lite_frames.h"

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
                                   proto::FrameType::kSafetyOverride, proto::FrameType::kLed, proto::FrameType::kSound,
                                   proto::FrameType::kTone, proto::FrameType::kAudioMode};
  for (const proto::FrameType f : gone) {
    TEST_ASSERT_EQUAL(FrameUse::kUnavailable, lite_frames::frameUse(t(f)));
    TEST_ASSERT_TRUE(lite_frames::needsAck(t(f)));  // the sender learns why nothing happened
  }
  TEST_ASSERT_EQUAL(FrameUse::kUnknown, lite_frames::frameUse(0x7F));
}

void test_frames_hello_caps() {
  TEST_ASSERT_EQUAL_UINT8(0, lite_frames::helloCaps(false));
  TEST_ASSERT_EQUAL_UINT8(1, lite_frames::helloCaps(true));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_frames_driving_is_handled);
  RUN_TEST(test_frames_unsupported_are_unavailable);
  RUN_TEST(test_frames_hello_caps);
  return UNITY_END();
}
