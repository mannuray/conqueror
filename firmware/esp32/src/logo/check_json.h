// The reply to POST /logo/check (Tank Logo LLD §20.4), written in chunks so no buffer has to hold a whole planned
// path (≈ 40 KB). Pure; the web server gives it a sink that sends each chunk.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "logo/check_job.h"

namespace cq {
namespace logo {

class JsonOut {
 public:
  using Flush = bool (*)(void* ctx, const char* data, size_t len);
  JsonOut(Flush flush, void* ctx) : m_flush(flush), m_ctx(ctx) {}
  void put(const char* s);
  void putf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void text(const char* s, size_t n);  // a JSON string, quoted and escaped
  bool finish();                       // the last chunk; false when any chunk could not be sent

 private:
  void raw(const char* s, size_t n);
  Flush m_flush;
  void* m_ctx;
  char m_buf[512];
  size_t m_n = 0;
  bool m_ok = true;
};

// {"ok","prog","diags":[…],"words":[…],"handles_obstacles","path":[[x_mm,y_mm,heading,line,uncertain]…],
//  "cut_short","sim_error"} for the check in `s` of `src`.
bool writeCheckReply(const CheckSpace& s, const char* src, bool runnable, uint32_t prog, JsonOut& out);

}  // namespace logo
}  // namespace cq
