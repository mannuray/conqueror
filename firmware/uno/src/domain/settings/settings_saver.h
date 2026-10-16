#pragma once
#include <stdint.h>

#include "domain/settings/eeprom_writer.h"
#include "domain/settings/settings.h"

namespace cq {

// EEPROM layout (LLD §7): magic 'C','Q',version at 0; global block at 8; profile block at 80.
// A block is [len][data…][crc lo][crc hi] (CRC-16/CCITT-FALSE over data).
constexpr uint16_t kEeMagicAddr = 0;
constexpr uint8_t kEeLayoutVersion = 1;
constexpr uint16_t kEeGlobalAddr = 8;
constexpr uint16_t kEeProfileAddr = 80;
constexpr uint8_t kEeMagicBytes = 3;
constexpr uint8_t kEeGlobalBlockBytes = static_cast<uint8_t>(1 + sizeof(GlobalSettings) + 2);
constexpr uint8_t kEeProfileBlockBytes = static_cast<uint8_t>(1 + sizeof(ProfileSettings) + 2);
static_assert(kEeGlobalAddr + kEeGlobalBlockBytes <= kEeProfileAddr, "global block overlaps the profile block");
// One block at a time (LLD §10.10): the writer queue only ever holds magic + global, or profile.
static_assert(kEeMagicBytes + kEeGlobalBlockBytes <= EepromWriter::kMaxBytes, "writer queue too small");
static_assert(kEeProfileBlockBytes <= EepromWriter::kMaxBytes, "writer queue too small");

// Saves magic + global, waits for the writer to drain, then saves profile. However many settings
// changed, the EepromWriter never holds more than one block, so it cannot overflow or drop bytes.
class SettingsSaver {
 public:
  void request() { m_requested = true; }
  bool busy() const { return m_stage != Stage::kIdle; }  // SETTINGS_SAVE → SETTINGS_SAVE_BUSY
  // One scheduler pass (50 Hz): writes ≤ 1 byte. may_start = the tank is stopped.
  // Returns true on the pass that finished a whole save.
  bool step(const Settings& s, EepromWriter& w, const EepromIo& io, bool may_start);
  uint8_t savedChanges() const { return m_changes_at_start; }  // Settings::changes() when the save began

 private:
  enum class Stage : uint8_t { kIdle, kGlobal, kProfile };
  static bool queueBlock(const Settings& s, uint8_t block, uint16_t addr, EepromWriter& w);
  Stage m_stage = Stage::kIdle;
  bool m_requested = false;
  uint8_t m_changes_at_start = 0;
};

}  // namespace cq
