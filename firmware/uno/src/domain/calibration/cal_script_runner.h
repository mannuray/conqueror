#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

namespace cq {

struct CalInputs {
  int16_t gx, gy, gz, ax, ay, az;  // raw counts, tank frame (65.5 LSB per °/s, 8192 LSB per g)
  int16_t temp_centi_c;            // 0.01 °C
  float yaw_total_deg;             // unwrapped, CCW +
  float yaw_rate_dps;              // CCW +
  int32_t enc_l, enc_r;            // signed by the commanded direction
  float cps_l, cps_r;              // counts/s, signed
  uint16_t batt_mv;
};

// Profile values the closed-loop scripts need; set by the app before start().
struct CalConfig {
  uint16_t max_cps;            // mean of max_cps_l / max_cps_r
  uint16_t turn_rate_max_dps;  // feed-forward scale for w %
  uint8_t keep_pwm;            // mean deadband keep value (percent → PWM map, like deadbandPwm)
};

struct CalOutputs {
  int16_t pwm_l;
  int16_t pwm_r;
  bool sample;            // `data` holds a new CAL_DATA sample (50 Hz)
  proto::CalDataMsg data;
  bool finished;          // script ended this tick; `status` is the result
  proto::AckStatus status;
  uint8_t progress;       // %
  uint8_t stage;          // = CAL_DATA.phase
};

// Motion scripts for off-board calibration maths (LLD §9.7). Runs at the 100 Hz control rate and
// streams a CAL_DATA sample every second tick (50 Hz). The v[0..7] layouts below are the contract
// with the bench calculators (Plan D); authoritative table: docs/lld/protocol-generator-lld.md
// "CAL_DATA channel layouts". Unused channels are 0.
//   STILL_SAMPLE, IDLE  param ms (0 → 3000)      gx gy gz ax ay az temp_centi_c batt_mv
//   PWM_RAMP            track | rev<<1 | max<<8  |pwm| yaw_rate×10 |enc| |cps|  (selected track; ph 0 up, 1 down)
//   FULL_SPIN           pwm (0 → 255)            pwm_l pwm_r cps_l cps_r yaw_rate×10 yaw×10 enc_l enc_r
//   SPIN_AND_ARC        bit0 = CW                enc_l enc_r yaw×10 yaw_rate×10 pwm_l pwm_r (since phase
//                                                start; ph 0 = 360° spin + settle, ph 1 = 90° arc)
//   RATE_STEPS          speed steps (0 → 4)      speed% w%×10 yaw_rate×10 cps_l cps_r pwm_l pwm_r (ph = step)
//   RELAY_OSCILLATION   loop 0 speed/1 rate/2 hdg setpoint×10 measured×10 relay_output pwm_l pwm_r
//   DRIVE_COUNTS        counts (0 → 7600)        enc_l enc_r yaw×10 pwm_l pwm_r cps_l cps_r
//   SERVO_NUDGE         reserved → UNAVAILABLE (servo trim is done host-side with CAM_NUDGE)
class CalScriptRunner {
 public:
  void configure(const CalConfig& cfg) { m_cfg = cfg; }
  proto::AckStatus start(proto::CalScript script, int32_t param, bool encoders, uint32_t now_ms);
  void cancel() { m_cancel = true; }
  bool running() const { return m_running; }
  proto::CalScript script() const { return m_script; }
  void tick(uint32_t now_ms, const CalInputs& in, CalOutputs& out);

 private:
  void finish(CalOutputs& out, proto::AckStatus status);
  void emit(CalOutputs& out, uint32_t t_ms, const int16_t v[8]);
  void setOrigin(const CalInputs& in);
  int16_t pctToPwm(float pct) const;
  void tickSpinAndArc(uint32_t now_ms, const CalInputs& in, CalOutputs& out, int16_t& l, int16_t& r);
  void tickRelay(const CalInputs& in, float yaw, uint32_t t, CalOutputs& out, int16_t& l, int16_t& r);

  CalConfig m_cfg{2500, 180, 55};
  proto::CalScript m_script = proto::CalScript::kIdle;
  int32_t m_param = 0;
  bool m_running = false;
  bool m_cancel = false;
  bool m_primed = false;
  uint32_t m_start_ms = 0;
  uint32_t m_duration_ms = 0;
  uint8_t m_tick = 0;
  uint8_t m_step = 0;        // SPIN_AND_ARC: 0 spin, 1 settle, 2 arc
  uint32_t m_step_ms = 0;
  float m_yaw0 = 0.0f;       // origin: script start (phase start for SPIN_AND_ARC)
  int32_t m_enc0_l = 0;
  int32_t m_enc0_r = 0;
};

}  // namespace cq
