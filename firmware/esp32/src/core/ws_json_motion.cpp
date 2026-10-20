#include "core/ws_json.h"

#include "core/stream_header.h"
#include "core/ws_json_util.h"

#include <stdio.h>
#include <string.h>

namespace cq {
namespace core {
using namespace detail;

size_t jsonTd(char* out, size_t cap, uint32_t seq, const TankSnapshot& s) {
  if (!s.fast_valid) return 0;
  const proto::TelemFastMsg& f = s.fast;
  char act_l[6] = "null", act_r[6] = "null";  // -128 = unknown (encoders off)
  if (f.spd_act_l != -128) snprintf(act_l, sizeof(act_l), "%d", f.spd_act_l);
  if (f.spd_act_r != -128) snprintf(act_r, sizeof(act_r), "%d", f.spd_act_r);
  char dist[8] = "null";  // 65535 no echo, 65534 sensor fault
  if (f.dist_mm < 65534) snprintf(dist, sizeof(dist), "%u", static_cast<unsigned>(f.dist_mm));
  char us[21];  // Plan Q1: when it arrived (ESP32 timer; no %llu in the bridge's printf)
  u64Str(us, sizeof(us), s.fast_bus_us);
  return fmt(out, cap,
             "{\"t\":\"td\",\"seq\":%lu,\"uno_ms\":%lu,\"bus_us\":%s,\"enc\":[%ld,%ld],\"pwm\":[%d,%d],\"cmd\":[%d,%d],"
             "\"act\":[%s,%s],\"yaw\":%.2f,\"rate\":%.1f,\"pitch\":%.2f,\"roll\":%.2f,\"dist_mm\":%s%s,"
             "\"closing\":%d,\"pan\":%.1f,\"tilt\":%.1f,\"flags\":%u,\"prim\":%u,\"pct\":%u}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(f.uno_ms), us, static_cast<long>(f.enc_l),
             static_cast<long>(f.enc_r), f.pwm_l, f.pwm_r, f.spd_cmd_l, f.spd_cmd_r, act_l, act_r, f.yaw / 100.0,
             f.yaw_rate / 10.0, f.pitch / 100.0, f.roll / 100.0, dist, f.dist_mm == 65534 ? ",\"dist_fault\":true" : "",
             f.closing_cm_s, f.pan / 10.0, f.tilt / 10.0, static_cast<unsigned>(f.flags),
             static_cast<unsigned>(f.prim), static_cast<unsigned>(f.prim_pct));
}

size_t jsonTds(char* out, size_t cap, const TankSnapshot& s) {
  if (!s.slow_valid) return 0;
  const proto::TelemSlowMsg& m = s.slow;
  return fmt(out, cap,
             "{\"t\":\"tds\",\"uptime_s\":%lu,\"reset_reason\":%u,\"batt_mv\":%u,\"driving\":%u,\"temp\":%d,"
             "\"owner\":%u,\"flags2\":%lu,\"accel_var\":%u,\"loop_load\":%u,\"loop_max_us\":%u,\"free_ram\":%u,"
             "\"err_frames\":%u,\"err_i2c\":%u,\"settings_crc\":%u,\"env\":[%u,%u,%u],\"profile\":%u,"
             "\"cal\":[%u,%u,%u],\"hw\":%u}",
             static_cast<unsigned long>(m.uptime_s), static_cast<unsigned>(m.reset_reason),
             static_cast<unsigned>(m.batt_mv), static_cast<unsigned>(m.driving), m.temp,
             static_cast<unsigned>(m.owner), static_cast<unsigned long>(m.flags2), static_cast<unsigned>(m.accel_var),
             static_cast<unsigned>(m.loop_load), static_cast<unsigned>(m.loop_max_us),
             static_cast<unsigned>(m.free_ram), static_cast<unsigned>(m.err_frames), static_cast<unsigned>(m.err_i2c),
             static_cast<unsigned>(m.settings_crc), static_cast<unsigned>(m.env_speed_cap),
             static_cast<unsigned>(m.env_accel_cap), static_cast<unsigned>(m.env_lock_brake),
             static_cast<unsigned>(m.profile_idx), static_cast<unsigned>(m.cal_status[0]),
             static_cast<unsigned>(m.cal_status[1]), static_cast<unsigned>(m.cal_status[2]),
             static_cast<unsigned>(m.hw));
}

size_t jsonPrimitiveDone(char* out, size_t cap, uint32_t seq, uint32_t tank_ms, const PrimitiveEnd& e) {
  static const char* const kReasons[] = {"done", "cancelled", "stop", "not_owner", "stalled", "timeout", "link",
                                         "deadman", "diverged", "blocked", "encoder_fault"};
  const uint8_t r = static_cast<uint8_t>(e.reason);
  char id[20] = "";
  if (e.id >= 0) snprintf(id, sizeof(id), ",\"id\":%ld", static_cast<long>(e.id));
  char moved[24] = "";
  if (e.has_moved) {
    const long mm = static_cast<long>(e.moved_mm);
    const long a = mm < 0 ? -mm : mm;
    snprintf(moved, sizeof(moved), ",\"moved_cm\":%s%ld.%ld", mm < 0 ? "-" : "", a / 10, a % 10);
  }
  return fmt(out, cap,
             "{\"t\":\"ev\",\"seq\":%lu,\"tank_ms\":%lu,\"kind\":\"primitive_done\",\"prim\":\"%s\","
             "\"ok\":%s,\"reason\":\"%s\"%s%s}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(tank_ms), primName(e.kind),
             e.ok ? "true" : "false", r < sizeof(kReasons) / sizeof(kReasons[0]) ? kReasons[r] : "done", moved, id);
}

size_t jsonCalib(char* out, size_t cap, uint16_t counts_per_m, uint16_t roll_mm) {
  return fmt(out, cap, "{\"t\":\"calib\",\"counts_per_m\":%u,\"roll_mm\":%u}", static_cast<unsigned>(counts_per_m),
             static_cast<unsigned>(roll_mm));
}

}  // namespace core
}  // namespace cq
