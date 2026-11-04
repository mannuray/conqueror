// Which /ws clients are /ai pages, and the ai_at text that tells them where the engine's AI feed is
// (bridge LLD R27.1-R27.3). Pure and host-tested; link task only.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

class AiPages {
 public:
  static constexpr uint8_t kMax = 4;  // the /ws client table holds four
  bool onHello(int fd);               // false: already four pages
  void onClose(int fd);
  bool contains(int fd) const;
  uint8_t count() const;
  int fdAt(uint8_t i) const { return m_fds[i]; }  // -1 = a free place
  // {"t":"ai_at","url":"<url>","proto":n}, or {"t":"ai_at","url":null} for "" - its length, 0 when cap is too small.
  static size_t text(char* buf, size_t cap, const char* url, uint8_t proto);
  // True when `url` differs from the last one announced (then remembered): the link loop sends it to every page.
  bool changed(const char* url);

 private:
  int m_fds[kMax] = {-1, -1, -1, -1};
  char m_last[40] = "";
};

}  // namespace core
}  // namespace cq
