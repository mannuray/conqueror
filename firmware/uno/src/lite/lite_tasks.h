#pragma once
#include <stdint.h>

#include "generated/cq_protocol.h"

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
void playSound(proto::SoundId id, uint8_t prio, uint32_t now_ms);  // lite_ui.cpp: one melody from the catalogue

}  // namespace lite
}  // namespace cq
