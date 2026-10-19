#include "app/telemetry_publisher.h"

namespace cq {
namespace app {

uint32_t TelemetryPublisher::tankMs() const {
  return m_link.snapshot().fast_valid ? m_link.snapshot().fast.uno_ms : 0;
}

// LLD R5: td (every div-th TELEM_FAST) / tds (each new TELEM_SLOW) to the clients that asked for them.
void TelemetryPublisher::sendDebug(bool fast, bool slow) {
  const core::TankSnapshot& s = m_link.snapshot();
  core::WsClients& ws = m_out.clients();
  for (size_t i = 0; i < ws.count(); ++i) {
    const int fd = ws.fdAt(i);
    const uint8_t div = ws.debugDiv(fd);
    if (div == 0) continue;
    if (fast && s.fast_count % div == 0) m_out.to(fd, core::jsonTd(m_out.buf(), m_out.cap(), ws.nextDebugSeq(fd), s));
    if (slow) m_out.to(fd, core::jsonTds(m_out.buf(), m_out.cap(), s));  // the buffer is shared: built per client
  }
}

TelemetryPublisher::News TelemetryPublisher::poll() {
  const core::TankSnapshot& s = m_link.snapshot();
  const News n{s.fast_count != m_last_fast, s.slow_count != m_last_slow};
  m_last_fast = s.fast_count;
  m_last_slow = s.slow_count;
  if (n.fast || n.slow) sendDebug(n.fast, n.slow);
  return n;
}

void TelemetryPublisher::afterFast() {
  const core::TankSnapshot& s = m_link.snapshot();
  if (s.fast_count % 5u != 0) return;  // Uno 50 Hz -> page 10 Hz (R2.14 #7, spec 02 §E4)
  m_out.all(core::jsonTf(m_out.buf(), m_out.cap(), ++m_tf_seq, s, m_views.primitive()));
}

void TelemetryPublisher::sendTs() {
  m_out.all(core::jsonTs(m_out.buf(), m_out.cap(), m_link.snapshot(), m_views.owner(), m_views.body_offline()));
}

void TelemetryPublisher::tickTs(uint32_t now) {
  if (now - m_last_ts_ms < 1000) return;
  m_last_ts_ms = now;
  sendTs();
}

void TelemetryPublisher::hello(int fd, const char* fw, const char* sensor) {
  const core::HelloInfo h = m_link.hello();
  const core::WelcomeInfo w{fw, &h, sensor, m_views.owner(), m_out.clients().name(fd)};
  m_out.to(fd, core::jsonWelcome(m_out.buf(), m_out.cap(), w));
  m_out.to(fd, core::jsonTs(m_out.buf(), m_out.cap(), m_link.snapshot(), m_views.owner(), m_views.body_offline()));
}

}  // namespace app
}  // namespace cq
