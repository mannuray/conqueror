#pragma once

namespace cq {
namespace lite {

bool imuUsable();
void imuTask();
void irTask();
void ultrasonicTask();
void batteryTask();
void encodersTask();
void buttonTask();
void selfTestTask();
void selfTestRecoverTask();
void settingsLoad();
void settingsTask();
void controlTask();  // lite_control.cpp (Task 14)
void uiTask();       // lite_ui.cpp (Task 15)
void cameraTask();   // lite_ui.cpp (Task 15)

}  // namespace lite
}  // namespace cq
