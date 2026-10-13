// Bench tool on TCP 3333 (bridge LLD R2.7/R2.8): one client, frame-aligned both ways, refused
// (accept + close) while a phone drives or a bench is already connected, dropped after 1 s
// without a byte. Owned by link_task only.
#pragma once

#include <WiFi.h>
#include <stdint.h>

#include "core/frame_aligner.h"
#include "core/frame_tx.h"
#include "core/link_owner.h"

namespace cq {
namespace net {

constexpr uint32_t kBenchSilentMs = 1000;
constexpr uint32_t kSwallowMs = 500;

struct TcpStats {
  uint32_t up = 0;       // bench -> Uno bytes
  uint32_t down = 0;     // Uno -> bench bytes
  uint32_t refused = 0;  // connections accepted and closed at once
  uint32_t sessions = 0;
};

class TcpPassthrough {
 public:
  void begin();
  // Accepts/refuses, pumps bench bytes into tx, drops a closed or silent bench.
  // Returns the LinkOwner actions to apply (connect / close).
  core::Actions poll(uint32_t now_ms, core::LinkOwner& owner, core::FrameTx& tx,
                     const char* driver, bool refuse_all);
  void forwardFromUno(const uint8_t* data, size_t len, uint32_t now_ms);
  bool active() { return m_active; }
  uint8_t injectSeq() const { return static_cast<uint8_t>(m_up.lastSeq() + 128); }
  void swallowAck(uint8_t seq, uint32_t now_ms) { m_swallow.arm(seq, now_ms + kSwallowMs); }
  void close();  // drop the client now (OTA start); caller applies owner.onTcpClose()
  const TcpStats& stats() const { return m_stats; }
  const char* lastRefusal() const { return m_last_refusal; }

 private:
  struct UpSink {
    core::FrameTx* tx;
    TcpStats* stats;
    void chunk(const uint8_t* d, size_t len, core::ChunkKind) {
      if (tx->push(d, len)) stats->up += len;
    }
  };
  struct DownSink {
    WiFiClient* client;
    core::AckSwallow* swallow;
    TcpStats* stats;
    uint32_t now_ms;
    void chunk(const uint8_t* d, size_t len, core::ChunkKind kind) {
      if (kind == core::ChunkKind::kFrame && swallow->shouldDrop(d, len, now_ms)) return;
      stats->down += client->write(d, len);
    }
  };

  WiFiServer m_server{3333};
  WiFiClient m_client;
  bool m_active = false;
  uint32_t m_last_rx_ms = 0;
  core::FrameAligner m_up;
  core::FrameAligner m_down;
  core::AckSwallow m_swallow;
  TcpStats m_stats;
  char m_last_refusal[48] = "";
};

}  // namespace net
}  // namespace cq
