#include "net/program_store.h"

#include <LittleFS.h>
#include <stdio.h>
#include <string.h>

#include "logo/generated/words.h"
#include "logo/program_names.h"
#include "net/log.h"

namespace cq {
namespace net {
namespace {

bool g_mounted = false;

void pathOf(const char* lower, char* out, size_t cap) { snprintf(out, cap, "/p/%s.logo", lower); }

uint8_t savedCount() {
  uint8_t n = 0;
  File dir = LittleFS.open("/p");
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) ++n;
  return n;
}

// The name of a saved file without its folder and ".logo"; false for anything else.
bool fileName(const char* path, char* out, size_t cap) {
  const char* base = strrchr(path, '/');
  base = base != nullptr ? base + 1 : path;
  const size_t n = strlen(base);
  if (n <= 5 || strcmp(base + n - 5, ".logo") != 0 || n - 5 >= cap) return false;
  memcpy(out, base, n - 5);
  out[n - 5] = '\0';
  return true;
}

}  // namespace

const char* storeStatusName(StoreStatus s) {
  static constexpr const char* kNames[] = {"OK", "NAME", "FULL", "TOO_BIG", "EXAMPLE", "NOT_FOUND", "FAILED"};
  return kNames[static_cast<uint8_t>(s)];
}

bool programStoreBegin() {
  g_mounted = LittleFS.begin(true, "/logo", 4, "logo");
  if (g_mounted && !LittleFS.exists("/p")) LittleFS.mkdir("/p");
  logf("logo: program store %s", g_mounted ? "mounted" : "NOT mounted (partition 'logo' missing?)");
  return g_mounted;
}

uint8_t programList(ProgramInfo* out, uint8_t cap) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < logo::exampleCount() && n < cap; ++i, ++n) {
    snprintf(out[n].name, sizeof out[n].name, "%s", logo::example(i).name);
    out[n].bytes = static_cast<uint16_t>(strlen(logo::example(i).src));
    out[n].example = true;
  }
  if (!g_mounted) return n;
  File dir = LittleFS.open("/p");
  for (File f = dir.openNextFile(); f && n < cap; f = dir.openNextFile()) {
    if (!fileName(f.name(), out[n].name, sizeof out[n].name)) continue;
    out[n].bytes = static_cast<uint16_t>(f.size());
    out[n++].example = false;
  }
  return n;
}

int programLoad(const char* name, char* buf, size_t cap) {
  char lower[25], path[40];
  if (!logo::programName(name, lower, sizeof lower)) return -1;
  if (const logo::Example* e = logo::findExample(lower)) return snprintf(buf, cap, "%s", e->src);
  pathOf(lower, path, sizeof path);
  if (!g_mounted || !LittleFS.exists(path)) return -1;
  File f = LittleFS.open(path, "r");
  const size_t n = f.read(reinterpret_cast<uint8_t*>(buf), cap - 1);
  buf[n] = '\0';
  return static_cast<int>(n);
}

StoreStatus programSave(const char* name, const char* src, size_t len) {
  char lower[25], path[40];
  if (!logo::programName(name, lower, sizeof lower)) return StoreStatus::kName;
  if (logo::findExample(lower) != nullptr) return StoreStatus::kExample;
  if (len > logo::gen::kSourceBytes) return StoreStatus::kTooBig;
  pathOf(lower, path, sizeof path);
  if (!g_mounted) return StoreStatus::kFailed;
  if (!LittleFS.exists(path) && savedCount() >= logo::kMaxPrograms) return StoreStatus::kFull;
  File f = LittleFS.open(path, "w");
  if (!f) return StoreStatus::kFull;
  const size_t n = f.write(reinterpret_cast<const uint8_t*>(src), len);
  f.close();
  return n == len ? StoreStatus::kOk : StoreStatus::kFull;
}

StoreStatus programRemove(const char* name) {
  char lower[25], path[40];
  if (!logo::programName(name, lower, sizeof lower)) return StoreStatus::kName;
  if (logo::findExample(lower) != nullptr) return StoreStatus::kExample;
  pathOf(lower, path, sizeof path);
  if (!g_mounted || !LittleFS.exists(path)) return StoreStatus::kNotFound;
  return LittleFS.remove(path) ? StoreStatus::kOk : StoreStatus::kFailed;
}

}  // namespace net
}  // namespace cq
