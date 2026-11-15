#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {
struct StackFrame {
  uint32_t sp;
  uint32_t spp[8];
};

void begin();

// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();

// The version string of the firmware that was running when the panic happened,
// as opposed to CROSSPOINT_VERSION, which is the one reading the report.
//
// They are normally identical and there is nothing to show. They differ in the
// one case that matters: a panic during an OTA or SD update, where the reboot
// lands in the NEW image, so everything written afterwards -- the report, the
// crash screen, the bug report the user sends -- names a binary that was not
// running when the fault occurred.
//
// Returns an empty string when it cannot be trusted: RTC_NOINIT does not
// survive power loss, so after a battery pull this holds whatever was in those
// cells. Callers show nothing rather than a guess.
const char* panickedVersion();

// Keep the crash report of the firmware being replaced, under
// /crash_report.prev.txt, so the next panic's report cannot be mistaken for it
// and the old one is still readable. Called from the flash path once otadata
// has been switched, i.e. once the next boot is certain to be a different
// binary. Never fails the update: a report is diagnostics, and losing one must
// not stop a firmware from being installed.
void archiveCrashReportForUpdate();
}  // namespace HalSystem
