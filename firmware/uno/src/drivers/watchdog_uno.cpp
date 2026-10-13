#include <avr/io.h>
#include <avr/wdt.h>

#include "ports/watchdog.h"

namespace {
// Survive resets; filled before C++ init.
uint8_t g_mcusr __attribute__((section(".noinit")));
uint8_t g_r2 __attribute__((section(".noinit")));
}  // namespace

// Optiboot >= 6 passes MCUSR in r2. The stock Uno bootloader clears MCUSR and leaves r2 as garbage,
// so r2 is trusted only when it holds nothing but reset-flag bits; otherwise the reason is kUnknown.
extern "C" void cqCaptureR2() __attribute__((naked, used, section(".init0")));
extern "C" void cqCaptureR2() { asm volatile("sts %0, r2" : "=m"(g_r2)); }

extern "C" void cqCaptureMcusr() __attribute__((naked, used, section(".init3")));
extern "C" void cqCaptureMcusr() {
  g_mcusr = MCUSR;
  MCUSR = 0;
  wdt_disable();
}

namespace cq {
namespace port {
namespace watchdog {

void enable() { wdt_enable(WDTO_1S); }
void kick() { wdt_reset(); }

proto::ResetReason resetReason() {
  constexpr uint8_t kResetFlagBits = _BV(WDRF) | _BV(BORF) | _BV(EXTRF) | _BV(PORF);  // 0x0F
  const bool r2_plausible = g_r2 != 0 && (g_r2 & static_cast<uint8_t>(~kResetFlagBits)) == 0;
  const uint8_t flags = g_mcusr != 0 ? g_mcusr : (r2_plausible ? g_r2 : 0);
  if (flags & _BV(WDRF)) return proto::ResetReason::kWatchdog;
  if (flags & _BV(BORF)) return proto::ResetReason::kBrownOut;
  if (flags & _BV(EXTRF)) return proto::ResetReason::kExternal;
  if (flags & _BV(PORF)) return proto::ResetReason::kPowerOn;
  return proto::ResetReason::kUnknown;
}

}  // namespace watchdog
}  // namespace port
}  // namespace cq
