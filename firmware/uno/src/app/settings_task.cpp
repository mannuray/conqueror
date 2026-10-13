#include "app/app.h"
#include "app/tasks.h"
#include "domain/settings/crc16.h"
#include "ports/eeprom.h"

namespace cq {
namespace app {
namespace {

const EepromIo kIo{port::eeprom::read, port::eeprom::writeByteAsync, port::eeprom::busy};

bool loadBlock(uint8_t block, uint16_t addr) {
  uint8_t buf[64];
  const uint8_t len = port::eeprom::read(addr);
  if (len == 0 || len > sizeof buf) return false;
  for (uint8_t i = 0; i < len; ++i) buf[i] = port::eeprom::read(static_cast<uint16_t>(addr + 1 + i));
  const uint16_t crc = static_cast<uint16_t>(port::eeprom::read(static_cast<uint16_t>(addr + 1 + len)) |
                                             (port::eeprom::read(static_cast<uint16_t>(addr + 2 + len)) << 8));
  return g_app.settings.deserialize(block, buf, len, crc);
}

}  // namespace

void settingsLoad() {
  g_app.settings.loadDefaults();
  uint8_t reset_mask = 0;
  const bool magic_ok = port::eeprom::read(kEeMagicAddr) == 'C' && port::eeprom::read(kEeMagicAddr + 1) == 'Q' &&
                        port::eeprom::read(kEeMagicAddr + 2) == kEeLayoutVersion;
  if (!magic_ok || !loadBlock(Settings::kBlockGlobal, kEeGlobalAddr)) reset_mask |= 0x01;
  if (!magic_ok || !loadBlock(Settings::kBlockProfile, kEeProfileAddr)) reset_mask |= 0x02;
  if (reset_mask != 0) {
    g_app.events.push(proto::EventKind::kSettingsReset, reset_mask);
    g_app.saver.request();  // persisted lazily by the writer, never synchronously at boot
  }
  g_app.settings.clearDirty();
  applySettings();
}

// 50 Hz: deferred save when stopped, one block at a time; one EEPROM byte per pass.
void settingsTask() {
  if (g_app.saver.step(g_app.settings, g_app.eewriter, kIo, !g_app.moving)) {
    g_app.settings.clearDirty();
    g_app.events.push(proto::EventKind::kSettingsSaved);
  }
}

}  // namespace app
}  // namespace cq
