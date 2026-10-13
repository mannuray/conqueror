#pragma once

namespace cq {
namespace app {

void settingsLoad();   // boot: EEPROM → settings (defaults + SETTINGS_RESET on bad CRC/magic)
void settingsTask();   // 50 Hz: deferred save, one EEPROM byte per pass
void applySettings();  // pushes settings into domain objects (app/apply_settings.cpp)
// TASK-DECLS-END

}  // namespace app
}  // namespace cq
