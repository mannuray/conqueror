#include <unity.h>

#include "domain/motion/primitives.h"
#include "domain/util/math_util.h"

using namespace cq;

void setUp() {}
void tearDown() {}

namespace {

const PrimitiveConfig kCfg{180, 150, 329.0f, 7.6f, true, true};

proto::TurnMsg turn(int16_t angle_dd, uint8_t speed, uint16_t radius_cm, uint8_t mode = 0) {
  proto::TurnMsg m{};
  m.src = 1;
  m.mode = mode;
  m.angle = angle_dd;
  m.speed = speed;
  m.radius_cm = radius_cm;
  return m;
}

proto::MoveMsg move(int32_t dist_mm, uint8_t speed, int16_t heading = 32767) {
  proto::MoveMsg m{};
  m.src = 1;
  m.dist_mm = dist_mm;
  m.speed = speed;
  m.heading = heading;
  return m;
}

// Simple plant: yaw rate follows the commanded rate with a 0.1 s lag.
struct Plant {
  PrimitiveInputs in{0.0f, 0.0f, 0.0f, 0, 0, false};
  float rate = 0.0f;
  float enc = 0.0f;
  void step(const MotionSetpoint& sp, float dt) {
    const float target_rate = sp.mode == HeadingMode::kRate ? sp.rate_dps : 0.0f;
    rate += (target_rate - rate) * dt / 0.1f;
    in.yaw_rate_dps = rate;
    in.yaw_total_deg += rate * dt;
    in.yaw_deg = wrap360(in.yaw_total_deg);
    enc += sp.v_pct / 100.0f * 329.0f * dt * 7.6f;
    in.enc_l = static_cast<int32_t>(enc);
    in.enc_r = static_cast<int32_t>(enc);
  }
};

PrimitiveResult runToEnd(Primitives& p, Plant& plant, uint32_t max_ms) {
  MotionSetpoint sp{};
  for (uint32_t t = 0; t < max_ms; t += 10) {
    if (!p.tick(plant.in, 0.01f, t, sp)) break;
    plant.step(sp, 0.01f);
  }
  PrimitiveResult r{};
  p.takeResult(r);
  return r;
}

}  // namespace

void test_spin_90_ccw_reaches_target() {
  Primitives p;
  Plant plant;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(p.startTurn(turn(900, 50, 0), kCfg, plant.in, 0)));
  const PrimitiveResult r = runToEnd(p, plant, 10000);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kTurn), static_cast<int>(r.kind));
  TEST_ASSERT_FLOAT_WITHIN(1.5f, 90.0f, plant.in.yaw_total_deg);
}

void test_turn_to_takes_shortest_path() {
  Primitives p;
  Plant plant;
  plant.in.yaw_total_deg = 350.0f;
  plant.in.yaw_deg = 350.0f;
  p.startTurn(turn(100, 50, 0, static_cast<uint8_t>(proto::TurnMode::kTo)), kCfg, plant.in, 0);  // to 10°
  runToEnd(p, plant, 10000);
  TEST_ASSERT_FLOAT_WITHIN(1.5f, 370.0f, plant.in.yaw_total_deg);  // +20°, not -340°
}

void test_arc_setpoint_matches_radius() {
  Primitives p;
  Plant plant;
  p.startTurn(turn(900, 60, 50), kCfg, plant.in, 0);  // radius 50 cm
  MotionSetpoint sp{};
  for (uint32_t t = 0; t < 600; t += 10) {
    p.tick(plant.in, 0.01f, t, sp);
    plant.step(sp, 0.01f);
  }
  TEST_ASSERT_TRUE(sp.v_pct > 0.0f);
  TEST_ASSERT_TRUE(sp.rate_dps > 0.0f);
  const float radius_mm = (sp.v_pct / 100.0f * 329.0f) / (sp.rate_dps * 0.0174533f);
  TEST_ASSERT_FLOAT_WITHIN(25.0f, 500.0f, radius_mm);
}

void test_turn_stalls_when_yaw_does_not_move() {
  Primitives p;
  PrimitiveInputs frozen{0.0f, 0.0f, 0.0f, 0, 0, false};
  p.startTurn(turn(900, 50, 0), kCfg, frozen, 0);
  MotionSetpoint sp{};
  uint32_t t = 0;
  for (; t < 5000 && p.tick(frozen, 0.01f, t, sp); t += 10) {
  }
  PrimitiveResult r{};
  TEST_ASSERT_TRUE(p.takeResult(r));
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kStalled), static_cast<int>(r.reason));
  TEST_ASSERT_UINT32_WITHIN(20, 1000, t);  // 1 s of < 3 °/s while commanding ≥ 15 °/s
}

void test_move_500mm_holds_heading_and_stops() {
  Primitives p;
  Plant plant;
  plant.in.yaw_deg = 45.0f;
  plant.in.yaw_total_deg = 45.0f;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(p.startMove(move(500, 40), kCfg, plant.in, 0)));
  MotionSetpoint sp{};
  p.tick(plant.in, 0.01f, 0, sp);
  TEST_ASSERT_EQUAL(static_cast<int>(HeadingMode::kHold), static_cast<int>(sp.mode));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 45.0f, sp.heading_deg);
  const PrimitiveResult r = runToEnd(p, plant, 20000);
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FLOAT_WITHIN(10.0f, 500.0f, plant.enc / 7.6f);
}

void test_move_needs_encoders_turn_needs_imu() {
  Primitives p;
  PrimitiveConfig no_enc = kCfg;
  no_enc.encoders = false;
  const PrimitiveInputs in{0, 0, 0, 0, 0, false};
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kUnavailable),
                    static_cast<int>(p.startMove(move(500, 40), no_enc, in, 0)));
  PrimitiveConfig no_imu = kCfg;
  no_imu.imu_ok = false;
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kUnavailable),
                    static_cast<int>(p.startTurn(turn(900, 50, 0), no_imu, in, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kUnavailable),
                    static_cast<int>(p.startMove(move(500, 40), no_imu, in, 0)));  // heading hold needs the gyro
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(p.startTurn(turn(900, 0, 0), kCfg, in, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(p.startTurn(turn(900, 50, 0, 5), kCfg, in, 0)));  // TurnMode 5 does not exist
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kNone), static_cast<int>(p.active()));
}

void test_turn_too_slow_is_rejected() {
  Primitives p;
  const PrimitiveInputs in{0, 0, 0, 0, 0, false};
  // spin at 5 % → 9 °/s, below the 15 °/s minimum the profile and stall check rely on
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(p.startTurn(turn(900, 5, 0), kCfg, in, 0)));
  // arc 10 % on a 50 cm radius → 32.9 mm/s / 500 mm ≈ 3.8 °/s
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kInvalidCommand),
                    static_cast<int>(p.startTurn(turn(900, 10, 50), kCfg, in, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kOk),
                    static_cast<int>(p.startTurn(turn(900, 10, 0), kCfg, in, 0)));  // spin 10 % = 18 °/s
}

void test_link_loss_cancels_and_does_not_resume() {
  Primitives p;
  Plant plant;
  p.startTurn(turn(1800, 50, 0), kCfg, plant.in, 0);
  MotionSetpoint sp{};
  for (uint32_t t = 0; t < 500; t += 10) {
    p.tick(plant.in, 0.01f, t, sp);
    plant.step(sp, 0.01f);
  }
  p.cancel(proto::AckStatus::kLinkLost);
  PrimitiveResult r{};
  TEST_ASSERT_TRUE(p.takeResult(r));
  TEST_ASSERT_FALSE(r.ok);
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kLinkLost), static_cast<int>(r.reason));
  // link comes back: nothing resumes, the setpoint is a stop
  for (uint32_t t = 500; t < 2000; t += 10) {
    TEST_ASSERT_FALSE(p.tick(plant.in, 0.01f, t, sp));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, sp.v_pct);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, sp.rate_dps);
  }
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kNone), static_cast<int>(p.active()));
  TEST_ASSERT_FALSE(p.takeResult(r));
}

void test_new_primitive_replaces_running_one() {
  Primitives p;
  Plant plant;
  p.startTurn(turn(1800, 50, 0), kCfg, plant.in, 0);
  p.startMove(move(300, 40), kCfg, plant.in, 100);
  PrimitiveResult r{};
  TEST_ASSERT_TRUE(p.takeResult(r));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kTurn), static_cast<int>(r.kind));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::AckStatus::kCancelled), static_cast<int>(r.reason));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::PrimitiveKind::kMove), static_cast<int>(p.active()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_spin_90_ccw_reaches_target);
  RUN_TEST(test_turn_to_takes_shortest_path);
  RUN_TEST(test_arc_setpoint_matches_radius);
  RUN_TEST(test_turn_stalls_when_yaw_does_not_move);
  RUN_TEST(test_move_500mm_holds_heading_and_stops);
  RUN_TEST(test_move_needs_encoders_turn_needs_imu);
  RUN_TEST(test_turn_too_slow_is_rejected);
  RUN_TEST(test_link_loss_cancels_and_does_not_resume);
  RUN_TEST(test_new_primitive_replaces_running_one);
  return UNITY_END();
}
