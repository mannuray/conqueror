#pragma once
#include <stdint.h>

#include "domain/safety/safety_override.h"
#include "generated/cq_protocol.h"

namespace cq {
namespace app {

// controlInit/controlTask/selfTestTask are declared in app/tasks.h.
void configureMotion();  // re-read settings into controllers
void stopTeleop();       // zero stick, DRIVE_RATE mode and rate (IR stop/takeover, Task 14)

proto::AckStatus onDrive(const uint8_t* p, uint8_t len);
proto::AckStatus onDriveRate(const uint8_t* p, uint8_t len);
proto::AckStatus onStop(const uint8_t* p, uint8_t len);
proto::AckStatus onTurn(const uint8_t* p, uint8_t len);
proto::AckStatus onMove(const uint8_t* p, uint8_t len);
proto::AckStatus onCancel(const uint8_t* p, uint8_t len);
proto::AckStatus onBrake(const uint8_t* p, uint8_t len);
proto::AckStatus onEnvelope(const uint8_t* p, uint8_t len);
proto::AckStatus onOwnerHint(const uint8_t* p, uint8_t len);
proto::AckStatus onProfile(const uint8_t* p, uint8_t len);
proto::AckStatus onSafetyOverride(const uint8_t* p, uint8_t len);
SafetyOverride::Gate overrideGate();  // Safety Override glue (Task 6, LLD §10)
void drainOverride();

proto::AckStatus onCalibrate(const uint8_t* p, uint8_t len);
bool calibrationTick(int16_t& pwm_l, int16_t& pwm_r);  // true while a script drives the motors
void calibrationAbort(proto::AckStatus reason);
void encodersTick();  // 100 Hz, called by the control task before control

}  // namespace app
}  // namespace cq
