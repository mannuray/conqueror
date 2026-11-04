#include "core/ai_pages.h"

#include <stdio.h>
#include <string.h>

namespace cq {
namespace core {

bool AiPages::onHello(int fd) {
  if (contains(fd)) return true;
  for (int& f : m_fds) {
    if (f < 0) {
      f = fd;
      return true;
    }
  }
  return false;
}

void AiPages::onClose(int fd) {
  for (int& f : m_fds) {
    if (f == fd) f = -1;
  }
}

bool AiPages::contains(int fd) const {
  if (fd < 0) return false;
  for (int f : m_fds) {
    if (f == fd) return true;
  }
  return false;
}

uint8_t AiPages::count() const {
  uint8_t n = 0;
  for (int f : m_fds) n += f >= 0 ? 1 : 0;
  return n;
}

size_t AiPages::text(char* buf, size_t cap, const char* url, uint8_t proto) {
  const int n = url[0] == '\0' ? snprintf(buf, cap, "{\"t\":\"ai_at\",\"url\":null}")
                               : snprintf(buf, cap, "{\"t\":\"ai_at\",\"url\":\"%s\",\"proto\":%u}", url,
                                          static_cast<unsigned>(proto));
  return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

bool AiPages::changed(const char* url) {
  if (strcmp(url, m_last) == 0) return false;
  snprintf(m_last, sizeof m_last, "%s", url);
  return true;
}

}  // namespace core
}  // namespace cq
