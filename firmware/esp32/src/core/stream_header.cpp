#include "core/stream_header.h"

#include <stdio.h>

namespace cq {
namespace core {

size_t u64Str(char* out, size_t cap, uint64_t v) {
  char tmp[21];
  size_t n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v != 0);
  if (cap == 0) return 0;
  if (n + 1 > cap) {
    out[0] = '\0';
    return 0;
  }
  for (size_t i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
  out[n] = '\0';
  return n;
}

size_t mjpegPartHeader(char* out, size_t cap, const char* boundary, size_t jpeg_len, uint64_t cap_us, uint32_t seq) {
  char us[21];
  u64Str(us, sizeof(us), cap_us);
  const int n = snprintf(out, cap,
                         "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Frame-Us: %s\r\n"
                         "X-Frame-Seq: %lu\r\n\r\n",
                         boundary, static_cast<unsigned>(jpeg_len), us, static_cast<unsigned long>(seq));
  if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
  return static_cast<size_t>(n);
}

}  // namespace core
}  // namespace cq
