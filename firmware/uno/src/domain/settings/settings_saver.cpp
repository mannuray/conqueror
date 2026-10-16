#include "domain/settings/settings_saver.h"

#include "domain/settings/crc16.h"

namespace cq {

bool SettingsSaver::queueBlock(const Settings& s, uint8_t block, uint16_t addr, EepromWriter& w) {
  uint8_t buf[kEeGlobalBlockBytes > kEeProfileBlockBytes ? kEeGlobalBlockBytes : kEeProfileBlockBytes];
  const uint8_t len = s.serialize(block, buf + 1);
  buf[0] = len;
  const uint16_t crc = crc16(buf + 1, len);
  buf[1 + len] = static_cast<uint8_t>(crc);
  buf[2 + len] = static_cast<uint8_t>(crc >> 8);
  return w.enqueue(addr, buf, static_cast<uint8_t>(len + 3));  // one range: data first, CRC last
}

bool SettingsSaver::step(const Settings& s, EepromWriter& w, const EepromIo& io, bool may_start) {
  if (!w.step(io)) return false;  // still writing the current block
  switch (m_stage) {
    case Stage::kIdle: {
      if (!m_requested || !may_start) return false;
      const uint8_t magic[kEeMagicBytes] = {'C', 'Q', kEeLayoutVersion};
      // The writer is empty here and the static_asserts guarantee the fit, so enqueue cannot fail;
      // if it ever did, the request stays pending instead of being lost.
      if (!w.enqueue(kEeMagicAddr, magic, kEeMagicBytes) || !queueBlock(s, Settings::kBlockGlobal, kEeGlobalAddr, w)) {
        return false;
      }
      m_requested = false;
      m_changes_at_start = s.changes();
      m_stage = Stage::kGlobal;
      return false;
    }
    case Stage::kGlobal:
      if (!queueBlock(s, Settings::kBlockProfile, kEeProfileAddr, w)) return false;  // retried next pass
      m_stage = Stage::kProfile;
      return false;
    case Stage::kProfile:
      m_stage = Stage::kIdle;
      return true;
  }
  return false;
}

}  // namespace cq
