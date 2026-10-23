// The field readers of the WebSocket command parser, one per JSON key (bridge LLD R15.6).
#include <math.h>
#include <string.h>

#include "core/heading_scale.h"
#include "core/motion_config.h"
#include "core/ws_codec_internal.h"

namespace cq {
namespace core {
namespace codec {
namespace {

int8_t clampPct(double v) {
  if (v > 100.0) return 100;
  if (v < -100.0) return -100;
  return static_cast<int8_t>(v < 0 ? v - 0.5 : v + 0.5);
}

bool isWhole(double n) { return n == floor(n); }

bool readBool(Reader& r, bool& out, bool& seen) {
  if (r.word("true")) {
    out = true;
  } else if (r.word("false")) {
    out = false;  // tank check: the field's default may be true (vflip) - always write what was sent
  } else {
    return false;
  }
  seen = true;
  return true;
}

bool numIn(Reader& r, double lo, double hi, double& n) { return r.num(n, nullptr, 0) && n >= lo && n <= hi; }
bool wholeIn(Reader& r, double lo, double hi, double& n) { return numIn(r, lo, hi, n) && isWhole(n); }

bool readPct(Reader& r, int8_t& out, bool& seen) {
  double n = 0;
  if (!r.num(n, nullptr, 0)) return false;
  out = clampPct(n);
  seen = true;
  return true;
}

using KeyReader = bool (*)(Reader& r, WsCommand& c, Seen& s);

// ---- one reader per key (values and ranges exactly as before the table, review-checked) ----
bool readT(Reader& r, WsCommand&, Seen& s) { return r.str(s.t, sizeof(s.t)); }
bool readId(Reader& r, WsCommand& c, Seen&) {  // review M4: a non-negative integer that fits int32
  double n = 0;
  if (!wholeIn(r, 0, 2147483647.0, n)) return false;
  c.has_id = true;
  c.id = static_cast<int32_t>(n);
  return true;
}
bool readV(Reader& r, WsCommand& c, Seen& s) { return readPct(r, c.v, s.v); }
bool readW(Reader& r, WsCommand& c, Seen& s) { return readPct(r, c.w, s.w); }
bool readX(Reader& r, WsCommand& c, Seen& s) { return readPct(r, c.x, s.x); }
bool readY(Reader& r, WsCommand& c, Seen& s) { return readPct(r, c.y, s.y); }
bool readActive(Reader& r, WsCommand& c, Seen& s) { return readBool(r, c.active, s.active); }
bool readOn(Reader& r, WsCommand& c, Seen& s) { return readBool(r, c.on, s.on); }
bool readPivot(Reader& r, WsCommand& c, Seen& s) { return readBool(r, c.pivot, s.pivot); }
bool readBy(Reader& r, WsCommand& c, Seen& s) {  // ±1..720° (LLD R4.2)
  double n = 0;
  if (!r.num(n, nullptr, 0) || fabs(n) < 1.0 || fabs(n) > 720.0) return false;
  c.turn_by_cdeg = static_cast<int32_t>(lround(n * 100.0));
  s.mf.by = true;
  return true;
}
bool readTo(Reader& r, WsCommand& c, Seen& s) {
  double n = 0;
  if (!r.num(n, nullptr, 0) || n < 0.0 || n >= 360.0) return false;
  c.turn_to_cdeg = static_cast<uint16_t>(lround(n * 100.0) % 36000);
  c.turn_has_to = true;
  s.mf.to = true;
  return true;
}
bool readSpeed(Reader& r, WsCommand& c, Seen&) {  // 1..100 here; per-command minimums in the checks below
  double n = 0;
  if (!wholeIn(r, 1.0, 100.0, n)) return false;
  c.speed = static_cast<uint8_t>(n);
  return true;
}
bool readRadius(Reader& r, WsCommand& c, Seen&) {
  double n = 0;
  if (!wholeIn(r, 0.0, 1000.0, n)) return false;
  c.radius_cm = static_cast<uint16_t>(n);
  return true;
}
bool readYawRate(Reader& r, WsCommand& c, Seen& s) {
  double n = 0;
  if (!numIn(r, -180.0, 180.0, n)) return false;
  c.rate_ddps = static_cast<int16_t>(lround(n * 10.0));
  s.mf.rate = true;
  return true;
}
bool readMode(Reader& r, WsCommand&, Seen& s) { return r.str(s.mode, sizeof(s.mode)); }
bool readName(Reader& r, WsCommand&, Seen& s) { return r.str(s.name, sizeof(s.name)); }
bool readT0(Reader& r, WsCommand& c, Seen&) {
  double n = 0;
  return r.num(n, c.t0, sizeof(c.t0));
}
bool readClient(Reader& r, WsCommand& c, Seen&) { return r.str(c.client, sizeof(c.client)); }
bool readHz(Reader& r, WsCommand& c, Seen&) {  // debug: 10 or 50 only (LLD R5)
  double n = 0;
  if (!r.num(n, nullptr, 0) || (n != 10.0 && n != 50.0)) return false;
  c.debug_hz = static_cast<uint8_t>(n);
  return true;
}
bool readDist(Reader& r, WsCommand& c, Seen& s) {  // move: whole cm, 1..500 in size
  double n = 0;
  if (!wholeIn(r, -motion::kMoveMaxCm, motion::kMoveMaxCm, n) || n == 0.0) return false;
  c.move_cm = static_cast<int16_t>(n);
  s.dist = true;
  return true;
}
bool readHeading(Reader& r, WsCommand&, Seen& s) {  // move heading: not in Plan K (spec 02 §G1) - refused below
  s.heading = true;
  return r.skip();
}
bool readSsid(Reader& r, WsCommand& c, Seen& s) {  // Plan M: 1..32 bytes (checked again by WifiStore)
  s.ssid = r.str(c.ssid, sizeof(c.ssid));
  return s.ssid;
}
bool readPassword(Reader& r, WsCommand& c, Seen& s) {
  s.password = r.str(c.password, sizeof(c.password));
  return s.password;
}
bool readSlot(Reader& r, WsCommand& c, Seen&) {  // JSON 1..3
  double n = 0;
  if (!wholeIn(r, 1, 3, n)) return false;
  c.slot = static_cast<int8_t>(n - 1);
  return true;
}
bool readValue(Reader& r, WsCommand& c, Seen&) {  // setting
  double n = 0;
  if (!wholeIn(r, -2147483648.0, 2147483647.0, n)) return false;
  c.value = static_cast<int32_t>(n);
  c.has_value = true;
  return true;
}
bool readTry(Reader& r, WsCommand& c, Seen&) {  // setting (R23)
  bool seen = false;
  return readBool(r, c.try_only, seen);
}
bool readReset(Reader& r, WsCommand&, Seen& s) {  // calib: a name; camera_set: true / false
  if (r.word("true")) {
    s.reset_true = true;
    return true;
  }
  return r.word("false") || r.str(s.reset, sizeof(s.reset));
}
bool readSince(Reader& r, WsCommand& c, Seen&) {  // log
  double n = 0;
  if (!wholeIn(r, 0, 4294967295.0, n)) return false;
  c.log_since = static_cast<uint32_t>(n);
  return true;
}
bool readSize(Reader& r, WsCommand& c, Seen&) {  // camera_set
  char sz[8];
  if (!r.str(sz, sizeof(sz))) return false;
  const char* const kSizes[] = {"QVGA", "VGA", "SVGA", "HD"};
  for (uint8_t i = 0; i < 4; ++i) {
    if (strcmp(sz, kSizes[i]) != 0) continue;
    c.cam.size = i;
    c.cam.has_size = true;
    return true;
  }
  return false;
}
bool readQuality(Reader& r, WsCommand& c, Seen&) {
  double n = 0;
  if (!wholeIn(r, 10, 40, n)) return false;
  c.cam.quality = static_cast<uint8_t>(n);
  c.cam.has_quality = true;
  return true;
}
bool readFpsCap(Reader& r, WsCommand& c, Seen&) {
  double n = 0;
  if (!r.num(n, nullptr, 0) || !(n == 0 || n == 5 || n == 10 || n == 15)) return false;
  c.cam.fps = static_cast<uint8_t>(n);
  c.cam.has_fps = true;
  return true;
}
bool readBrightness(Reader& r, WsCommand& c, Seen&) {
  double n = 0;
  if (!wholeIn(r, -2, 2, n)) return false;
  c.cam.bright = static_cast<int8_t>(n);
  c.cam.has_bright = true;
  return true;
}
bool readContrast(Reader& r, WsCommand& c, Seen&) {
  double n = 0;
  if (!wholeIn(r, -2, 2, n)) return false;
  c.cam.contrast = static_cast<int8_t>(n);
  c.cam.has_contrast = true;
  return true;
}
bool readLowLight(Reader& r, WsCommand& c, Seen&) { return readBool(r, c.cam.low, c.cam.has_low); }
bool readVflip(Reader& r, WsCommand& c, Seen&) { return readBool(r, c.cam.vflip, c.cam.has_vflip); }
bool readHmirror(Reader& r, WsCommand& c, Seen&) { return readBool(r, c.cam.hmirror, c.cam.has_hmirror); }
bool readAiUrl(Reader& r, WsCommand& c, Seen& s) {  // app_set
  if (!r.str(c.ai_url, sizeof(c.ai_url))) return false;
  c.has_ai_url = true;
  s.url_bad = r.cut() || !appUrlValid(c.ai_url);  // never store an address cut short (final review I3)
  return true;
}
bool readSettingId(Reader& r, WsCommand& c, Seen& s) {  // setting: the Uno setting (the command id stays "id")
  double n = 0;
  if (!wholeIn(r, 0, 255, n)) return false;
  c.setting_id = static_cast<uint8_t>(n);
  s.setting_id = true;
  return true;
}
bool readCpm(Reader& r, WsCommand& c, Seen&) {  // calib set
  double n = 0;
  if (!wholeIn(r, motion::kCpmMin, motion::kCpmMax, n)) return false;
  c.has_cpm = true;
  c.cpm = static_cast<uint16_t>(n);
  return true;
}
bool readGyroScale(Reader& r, WsCommand& c, Seen&) {  // R19 calib set
  double n = 0;
  if (!wholeIn(r, kGyroScaleMin, kGyroScaleMax, n)) return false;
  c.has_gyro_scale = true;
  c.gyro_scale = static_cast<uint16_t>(n);
  return true;
}

// cam_set (R22): degrees from the centre, kept in 0.1 deg; the range is checked with the command (OUT_OF_RANGE).
bool readAngle(Reader& r, int32_t& d10, bool& seen) {
  double n = 0;
  if (!r.num(n, nullptr, 0)) return false;
  if (n < -1000.0 || n > 1000.0) n = n < 0 ? -1000.0 : 1000.0;  // still a number: refused as OUT_OF_RANGE
  d10 = static_cast<int32_t>(lround(n * 10.0));
  seen = true;
  return true;
}
bool readPan(Reader& r, WsCommand&, Seen& s) { return readAngle(r, s.pan_d10, s.pan); }
bool readTilt(Reader& r, WsCommand&, Seen& s) { return readAngle(r, s.tilt_d10, s.tilt); }

bool readProg(Reader& r, WsCommand& c, Seen& s) {  // logo_run (Plan N3): 1..2^32-1
  double n = 0;
  if (!wholeIn(r, 1, 4294967295.0, n)) return false;
  c.prog = static_cast<uint32_t>(n);
  s.prog = true;
  return true;
}
bool readStep(Reader& r, WsCommand& c, Seen&) {
  bool seen = false;
  return readBool(r, c.step, seen);
}

struct KeyRow {
  const char* key;
  KeyReader read;
};
// VARIANT POINT (LLD R15.6): one row per JSON key; WsCommand stays flat (Plan P ruling).
const KeyRow kKeys[] = {
    {"t", readT},           {"id", readId},           {"v", readV},             {"w", readW},
    {"x", readX},           {"y", readY},             {"active", readActive},   {"on", readOn},
    {"pivot", readPivot},   {"by", readBy},           {"to", readTo},           {"speed", readSpeed},
    {"radius_cm", readRadius}, {"yaw_rate", readYawRate}, {"mode", readMode},   {"name", readName},
    {"t0", readT0},         {"client", readClient},   {"hz", readHz},           {"dist_cm", readDist},
    {"heading", readHeading}, {"ssid", readSsid},     {"password", readPassword}, {"slot", readSlot},
    {"value", readValue},   {"reset", readReset},     {"since", readSince},     {"size", readSize},
    {"quality", readQuality}, {"fps_cap", readFpsCap}, {"brightness", readBrightness},
    {"contrast", readContrast}, {"low_light", readLowLight}, {"vflip", readVflip}, {"hmirror", readHmirror},
    {"ai_url", readAiUrl},  {"setting_id", readSettingId}, {"counts_per_m", readCpm}, {"gyro_scale", readGyroScale}, {"pan", readPan}, {"tilt", readTilt}, {"prog", readProg},
    {"step", readStep},     {"try", readTry},
};

}  // namespace

bool readField(const char* key, Reader& r, WsCommand& c, Seen& s) {
  for (const KeyRow& k : kKeys) {
    if (strcmp(key, k.key) == 0) return k.read(r, c, s);
  }
  return r.skip();
}

}  // namespace codec
}  // namespace core
}  // namespace cq
