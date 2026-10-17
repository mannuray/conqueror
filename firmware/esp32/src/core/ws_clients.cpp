#include "core/ws_clients.h"

#include <stdio.h>

namespace cq {
namespace core {

int WsClients::find(int fd) const {
  for (size_t i = 0; i < m_count; ++i) {
    if (m_c[i].fd == fd) return static_cast<int>(i);
  }
  return -1;
}

bool WsClients::add(int fd, uint32_t now_ms) {
  if (m_count == kMaxWsClients || contains(fd)) return false;
  Client& c = m_c[m_count++];
  c = Client{};
  c.fd = fd;
  snprintf(c.name, sizeof(c.name), "phone-%lu", static_cast<unsigned long>(m_next_number++));
  c.last_rx_ms = now_ms;
  c.window_ms = now_ms;
  c.last_reject = 0xFF;
  return true;
}

bool WsClients::remove(int fd) {
  const int i = find(fd);
  if (i < 0) return false;
  m_c[i] = m_c[--m_count];
  return true;
}

bool WsClients::setDebug(int fd, uint8_t div) {
  const int i = find(fd);
  if (i < 0) return false;
  m_c[i].debug_div = div;
  return true;
}

uint8_t WsClients::debugDiv(int fd) const {
  const int i = find(fd);
  return i < 0 ? 0 : m_c[i].debug_div;
}

uint32_t WsClients::nextDebugSeq(int fd) {
  const int i = find(fd);
  return i < 0 ? 0 : ++m_c[i].debug_seq;
}

const char* WsClients::name(int fd) const {
  const int i = find(fd);
  return i < 0 ? "" : m_c[i].name;
}

bool WsClients::admit(int fd, WsCmdKind kind, uint32_t now_ms) {
  const int i = find(fd);
  if (i < 0) return false;
  Client& c = m_c[i];
  c.last_rx_ms = now_ms;  // any message counts as alive
  if (now_ms - c.window_ms >= 1000) {
    c.window_ms = now_ms;
    c.stream_n = c.cam_n = c.total_n = 0;
  }
  if (kind == WsCmdKind::kStop) return true;
  if (c.total_n < 255) ++c.total_n;
  if (c.total_n > kTotalLimitPerS) return false;
  if (kind == WsCmdKind::kDrive || kind == WsCmdKind::kDriveRate) return ++c.stream_n <= kStreamLimitPerS;
  if (kind == WsCmdKind::kCamJoy) return ++c.cam_n <= kStreamLimitPerS;
  return true;
}

size_t WsClients::silent(uint32_t now_ms, int* out, size_t max) {
  size_t n = 0;
  for (size_t i = 0; i < m_count && n < max; ++i) {
    if (!m_c[i].closing && now_ms - m_c[i].last_rx_ms >= kWsSilentMs) {
      m_c[i].closing = true;
      out[n++] = m_c[i].fd;
    }
  }
  return n;
}

bool WsClients::reportStreamReject(int fd, uint8_t reason) {
  const int i = find(fd);
  if (i < 0 || m_c[i].last_reject == reason) return false;
  m_c[i].last_reject = reason;
  return true;
}

void WsClients::streamAccepted(int fd) {
  const int i = find(fd);
  if (i >= 0) m_c[i].last_reject = 0xFF;
}

}  // namespace core
}  // namespace cq
