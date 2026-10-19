// Saved Tank Logo programs (Plan N3, Tank Logo LLD §12, §20.4): one file per program, /p/<name>.logo, on the
// LittleFS "logo" partition; the built-in examples are read-only. Web server task only.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace net {

enum class StoreStatus : uint8_t { kOk, kName, kFull, kTooBig, kExample, kNotFound, kFailed };
const char* storeStatusName(StoreStatus s);  // "NAME", "FULL" …

struct ProgramInfo {
  char name[25];
  uint16_t bytes;
  bool example;
};

bool programStoreBegin();  // mounts (formats on the first boot)
uint8_t programList(ProgramInfo* out, uint8_t cap);  // the examples first
int programLoad(const char* name, char* buf, size_t cap);  // its length, or -1 when there is none
StoreStatus programSave(const char* name, const char* src, size_t len);
StoreStatus programRemove(const char* name);

}  // namespace net
}  // namespace cq
