#include "net/source_archive.h"

#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <stdio.h>
#include <string.h>

#include "net/http_server.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {

const esp_partition_t* g_part = nullptr;
SourceHeader g_header{};
bool g_valid = false;
uint8_t g_buf[4096];  // httpd task only (and once at boot)

esp_err_t serveSource(httpd_req_t* req) {
  if (!g_valid) {
    httpd_resp_send_404(req);
    return ESP_OK;
  }
  char disposition[80];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"conqueror-%.8s.tar.gz\"",
           g_header.commit);
  httpd_resp_set_type(req, "application/gzip");
  httpd_resp_set_hdr(req, "Content-Disposition", disposition);
  for (size_t off = 0; off < g_header.length;) {
    const size_t n = sourceRead(off, g_buf, sizeof(g_buf));
    if (n == 0 || httpd_resp_send_chunk(req, reinterpret_cast<const char*>(g_buf),
                                        static_cast<ssize_t>(n)) != ESP_OK) {
      return ESP_FAIL;
    }
    off += n;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

}  // namespace

bool sourceBegin() {
  g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "source");
  if (g_part == nullptr) {
    logf("source: no \"source\" partition - wrong partition table?");
    return false;
  }
  if (esp_partition_read(g_part, 0, &g_header, sizeof(g_header)) != ESP_OK ||
      memcmp(g_header.magic, "CQSRC1\0\0", 8) != 0 ||
      g_header.length > g_part->size - sizeof(SourceHeader)) {
    logf("source: no archive (run: pio run -e bridge -t upload_source)");
    return false;
  }
  uint32_t crc = 0;
  for (size_t off = 0; off < g_header.length;) {
    const size_t n = sourceRead(off, g_buf, sizeof(g_buf));
    if (n == 0) return false;
    crc = esp_rom_crc32_le(crc, g_buf, n);
    off += n;
  }
  g_valid = crc == g_header.crc32;
  logf("source: %lu bytes, commit %.8s%s, %s", static_cast<unsigned long>(g_header.length),
       g_header.commit, g_header.dirty ? " (dirty)" : "", g_valid ? "CRC ok" : "CRC BAD");
  return g_valid;
}

bool sourceValid() { return g_valid; }

const SourceHeader& sourceHeader() { return g_header; }

size_t sourceRead(size_t offset, uint8_t* buf, size_t len) {
  if (g_part == nullptr || offset >= g_header.length) return 0;
  const size_t n = (g_header.length - offset < len) ? g_header.length - offset : len;
  if (esp_partition_read(g_part, sizeof(SourceHeader) + offset, buf, n) != ESP_OK) return 0;
  return n;
}

void sourceRegister() {
  httpd_uri_t u{};
  u.uri = "/source.tar.gz";
  u.method = HTTP_GET;
  u.handler = serveSource;
  httpd_register_uri_handler(httpServer(), &u);
}

}  // namespace net
}  // namespace cq
