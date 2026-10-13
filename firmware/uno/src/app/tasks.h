#pragma once

namespace cq {
namespace app {

void settingsLoad();   // boot: EEPROM → settings (defaults + SETTINGS_RESET on bad CRC/magic)
void settingsTask();   // 50 Hz: deferred save, one EEPROM byte per pass
void applySettings();  // pushes settings into domain objects (app/apply_settings.cpp)
void imuInit();  // boot: MPU6050 begin(); also the 1 s re-init while !imu_ok
void imuTask();  // 200 Hz
void sensorsInit();     // boot: ultrasonic, IR, battery ADC, button ports
void ultrasonicTask();  // 20 Hz
void batteryTask();     // 10 Hz
void irTask();          // 200 Hz
void controlInit();  // boot: motor port in standby, outputs zero
void controlTask();  // 100 Hz
void selfTestTask();  // 1 Hz: hw bits, cal status, boot self-test + SELF_TEST re-runs
void selfTestRecoverTask();  // 10 Hz: battery-only lock auto-clears after 1 s valid (LLD §10.9)
void cameraInit();  // boot: servos to the home pulse, then attach
void cameraTask();  // 50 Hz
void uiInit();  // boot: buzzer and WS2812 pins
void uiTask();  // 50 Hz
// TASK-DECLS-END

}  // namespace app
}  // namespace cq
