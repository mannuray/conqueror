#include "domain/lite/lite_telemetry.h"

#include "generated/cq_protocol.h"

namespace cq {
namespace lite_telemetry {

namespace {
uint32_t bit(proto::SlowFlagBit b) { return 1UL << static_cast<uint8_t>(b); }
}  // namespace

uint32_t slowFlags(uint32_t base, bool brake_on, bool envelope_active, bool link_alive, bool deadman_latched,
                   proto::AudioModeValue mode) {
  const uint32_t brake_off = bit(proto::SlowFlagBit::kBrakeOff), envelope = bit(proto::SlowFlagBit::kEnvelope);
  const uint32_t link_lost = bit(proto::SlowFlagBit::kLinkLost), deadman = bit(proto::SlowFlagBit::kDeadman);
  const uint32_t muted = bit(proto::SlowFlagBit::kMuted), quiet = bit(proto::SlowFlagBit::kQuiet);
  base &= ~(brake_off | envelope | link_lost | deadman | muted | quiet);
  if (mode == proto::AudioModeValue::kMute) base |= muted;
  if (mode == proto::AudioModeValue::kQuiet) base |= quiet;
  if (!brake_on) base |= brake_off;
  if (envelope_active) base |= envelope;
  if (!link_alive) base |= link_lost;
  if (deadman_latched) base |= deadman;
  return base;
}

uint8_t hwByte(bool encoders_enabled, bool imu_ok, bool us_ok) {
  uint8_t hw = 0;
  if (encoders_enabled) hw |= 1u << static_cast<uint8_t>(proto::HwBit::kEncodersEnabled);
  if (imu_ok) hw |= 1u << static_cast<uint8_t>(proto::HwBit::kImuOk);
  if (us_ok) hw |= 1u << static_cast<uint8_t>(proto::HwBit::kUsOk);
  return hw;
}

}  // namespace lite_telemetry
}  // namespace cq
