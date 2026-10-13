#pragma once

namespace cq {
namespace app {

void settingsLoad();   // boot: EEPROM → settings (defaults + SETTINGS_RESET on bad CRC/magic)
void settingsTask();   // 50 Hz: deferred save, one EEPROM byte per pass
void applySettings();  // pushes settings into domain objects (app/apply_settings.cpp)
void imuInit();  // boot: MPU6050 begin(); also the 1 s re-init while !imu_ok
void imuTask();  // 200 Hz
// TASK-DECLS-END

}  // namespace app
}  // namespace cq
