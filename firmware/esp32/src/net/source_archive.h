// The "source" flash partition: 64-byte header + git archive (tar.gz) written by
// scripts/pack_source.py (bridge LLD R2.3 step 6, R2.4). Read-only.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace net {

struct SourceHeader {  // little-endian, 64 bytes
  char magic[8];       // "CQSRC1\0\0"
  char commit[40];     // git commit, 40 hex characters
  uint8_t dirty;
  uint8_t reserved[3];
  uint32_t length;  // payload bytes after the header
  uint32_t crc32;   // CRC-32 (zlib) of the payload
  uint8_t pad[4];
};
static_assert(sizeof(SourceHeader) == 64, "header layout is shared with pack_source.py");

bool sourceBegin();  // reads the header and checks the CRC once at boot (~0.5 MB read)
bool sourceValid();
const SourceHeader& sourceHeader();
size_t sourceRead(size_t offset, uint8_t* buf, size_t len);  // payload offset
void sourceRegister();  // GET /source.tar.gz on port 80 (after httpServerStart)

}  // namespace net
}  // namespace cq
