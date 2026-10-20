// The camera stream's part header with the frame's capture time (Plan Q1, AI engine LLD §2).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

// A 64-bit count as decimal text (the bridge's printf has no %llu). Returns its length, or 0 when it does not fit
// (out is then empty).
size_t u64Str(char* out, size_t cap, uint64_t v);

// "\r\n--<boundary>\r\nContent-Type: image/jpeg\r\nContent-Length: <len>\r\nX-Frame-Us: <cap_us>\r\n
// X-Frame-Seq: <seq>\r\n\r\n". cap_us = the frame's capture time on the ESP32 timer (µs since boot, the same clock
// as the telemetry's bus_us). Returns the length, or 0 when it does not fit.
size_t mjpegPartHeader(char* out, size_t cap, const char* boundary, size_t jpeg_len, uint64_t cap_us, uint32_t seq);

}  // namespace core
}  // namespace cq
