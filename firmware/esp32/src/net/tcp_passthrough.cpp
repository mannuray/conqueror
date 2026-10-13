#include "net/tcp_passthrough.h"

#include "board_config.h"
#include "net/log.h"

namespace cq {
namespace net {

void TcpPassthrough::begin() {
  m_server.begin(board::kTcpPort);
  m_server.setNoDelay(true);
  logf("tcp: bench passthrough on port %u", board::kTcpPort);
}

core::Actions TcpPassthrough::poll(uint32_t now_ms, core::LinkOwner& owner, core::FrameTx& tx,
                                   const char* driver, bool refuse_all) {
  core::Actions out;
  if (m_server.hasClient()) {
    WiFiClient incoming = m_server.accept();
    core::Actions a;
    const bool refuse = refuse_all || owner.onTcpConnect(a) == core::Admit::kRefuseBusy;
    if (refuse) {
      incoming.stop();  // the bench reads EOF: "link closed by bridge"
      ++m_stats.refused;
      snprintf(m_last_refusal, sizeof(m_last_refusal), "%s",
               refuse_all ? "ota in progress" : (m_active ? "bench connected" : driver));
      logf("tcp refused: %s%s", m_last_refusal,
           (refuse_all || m_active) ? "" : " is driving");
    } else {
      m_client = incoming;
      m_client.setNoDelay(true);
      m_active = true;
      m_last_rx_ms = now_ms;
      m_up.reset();
      m_down.reset();
      ++m_stats.sessions;
      logf("tcp: bench connected from %s", m_client.remoteIP().toString().c_str());
      out = a;
    }
  }
  if (!m_active) return out;

  uint8_t buf[256];
  UpSink up{&tx, &m_stats};
  int avail = m_client.available();
  while (avail > 0) {
    const int n = m_client.read(buf, avail < static_cast<int>(sizeof(buf)) ? avail : sizeof(buf));
    if (n <= 0) break;
    m_up.push(buf, static_cast<size_t>(n), now_ms, up);
    m_last_rx_ms = now_ms;
    avail = m_client.available();
  }
  m_up.flushIfStale(now_ms, up);
  DownSink down{&m_client, &m_swallow, &m_stats, now_ms};
  m_down.flushIfStale(now_ms, down);

  const bool silent = now_ms - m_last_rx_ms >= kBenchSilentMs;
  if (!m_client.connected() || silent) {
    logf("tcp: bench %s", silent ? "silent for 1 s - dropped" : "disconnected");
    tx.clear();  // review I3: a DRIVE that came with the disconnect must not follow the STOP below
    close();
    const core::Actions c = owner.onTcpClose();
    out.bits |= c.bits;
  }
  return out;
}

void TcpPassthrough::forwardFromUno(const uint8_t* data, size_t len, uint32_t now_ms) {
  if (!m_active) return;  // no bench: Uno bytes only feed BodyLink's parser
  DownSink down{&m_client, &m_swallow, &m_stats, now_ms};
  m_down.push(data, len, now_ms, down);
}

void TcpPassthrough::close() {
  m_up.discardPartial();  // a half bench frame never reaches the Uno
  m_down.discardPartial();
  m_client.stop();
  m_active = false;
}

}  // namespace net
}  // namespace cq
