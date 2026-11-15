#include "HalSystem.h"

#include <cstring>
#include <string>

#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"

#define MAX_PANIC_STACK_DEPTH 32

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];

// Which binary was running when the panic happened. Written at begin() on every
// boot that is NOT a panic reboot, so on a panic reboot it still holds the
// version of the image that faulted -- which is a different image from the one
// now running whenever the panic happened during an update. See
// HalSystem::panickedVersion().
#define RUNNING_VERSION_LEN 48
RTC_NOINIT_ATTR char runningVersion[RUNNING_VERSION_LEN];

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";
void IRAM_ATTR __wrap_panic_abort(const char* message) {
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

#if !__riscv
  __real_panic_print_backtrace(frame, core);
  return;
#else
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }

  __real_panic_print_backtrace(frame, core);
#endif
}
}

namespace HalSystem {

void begin() {
  // This is mostly for the first boot, we need to initialize the panic info and logs to empty state
  // If we reboot from a panic state, we want to keep the panic info until we successfully dump it to the SD card, use
  // `clearPanic()` to clear it after dumping
  if (!isRebootFromPanic()) {
    clearPanic();
    // Record which binary this is, for the next panic to report. Deliberately
    // NOT written on a panic reboot: the value left there is the whole point.
    snprintf(runningVersion, RUNNING_VERSION_LEN, "%s", CROSSPOINT_VERSION);
  } else {
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      LOG_INF("SYS", "Dumped panic info to SD card");
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  clearLastLogs();
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += "CrossPoint version: " CROSSPOINT_VERSION;

    // The version that faulted, when it is not this one. Printed before
    // anything else that could be misread, because every line below it belongs
    // to that binary and not to this one.
    const char* faulted = panickedVersion();
    if (faulted[0] != '\0' && strcmp(faulted, CROSSPOINT_VERSION) != 0) {
      info += "\n\n*** This report was written by a DIFFERENT firmware than the one that crashed.";
      info += "\n*** Crashed in : ";
      info += faulted;
      info += "\n*** Reported by: " CROSSPOINT_VERSION;
      info += "\n*** The logs and stack below belong to the crashed version.";
      info += "\n*** This normally means the panic happened during a firmware update.";
    } else if (faulted[0] == '\0') {
      info += "\n(The crashed version could not be confirmed: RTC memory does not survive power loss.)";
    }

    // ★ The reset reason, because an empty panic reason is not an absence of
    // information -- it says the reboot did not come through panic_abort(), and
    // which non-panic path it came through is the whole question.
    //
    // panicMessage is filled only by __wrap_panic_abort() above. Everything that
    // reboots without calling it leaves the string as clearPanic() left it, i.e.
    // empty: a brownout (esp_brownout_isr goes straight to esp_restart_noos), an
    // RTC watchdog, a supply collapse, or a plain esp_restart(). Those are very
    // different faults and the report could not tell them apart.
    const char* resetName = "?";
    switch (esp_reset_reason()) {
      case ESP_RST_POWERON:
        resetName = "POWERON";
        break;
      case ESP_RST_EXT:
        resetName = "EXT";
        break;
      case ESP_RST_SW:
        resetName = "SW (esp_restart)";
        break;
      case ESP_RST_PANIC:
        resetName = "PANIC";
        break;
      case ESP_RST_INT_WDT:
        resetName = "INT_WDT";
        break;
      case ESP_RST_TASK_WDT:
        resetName = "TASK_WDT";
        break;
      case ESP_RST_WDT:
        resetName = "WDT (other)";
        break;
      case ESP_RST_DEEPSLEEP:
        resetName = "DEEPSLEEP";
        break;
      case ESP_RST_BROWNOUT:
        resetName = "BROWNOUT";
        break;
      case ESP_RST_SDIO:
        resetName = "SDIO";
        break;
      default:
        resetName = "UNKNOWN";
        break;
    }
    info += "\n\nReset reason: " + std::string(resetName);
    info += "\n\nPanic reason: " + std::string(panicMessage);
    if (panicMessage[0] == '\0') {
      info += "  (empty: this reboot did not go through panic_abort(); read the reset reason above)";
    }
    info += "\n\nLast logs:\n" + getLastLogs();
    info += "\n\nStack memory:\n";

    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
    for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
      if (panicStack[i].sp == 0) {
        break;
      }
      info += "0x" + toHex(panicStack[i].sp) + ": ";
      for (size_t j = 0; j < 8; j++) {
        info += "0x" + toHex(panicStack[i].spp[j]) + " ";
      }
      info += "\n";
    }

    return info;
  }
}

const char* panickedVersion() {
  // RTC_NOINIT survives a reset but not a power cut, so after a battery pull
  // this is whatever those cells held. Accept it only if it is a NUL-terminated
  // run of printable ASCII -- a version string always is, and random bytes
  // almost never are. An unverifiable answer is reported as no answer.
  for (size_t i = 0; i < RUNNING_VERSION_LEN; i++) {
    const char c = runningVersion[i];
    if (c == '\0') {
      return i == 0 ? "" : runningVersion;  // empty is also "unknown"
    }
    if (c < 0x20 || c > 0x7E) break;
  }
  return "";
}

void archiveCrashReportForUpdate() {
  if (!Storage.exists("/crash_report.txt")) return;
  // Overwrite any older archive rather than accumulating: the question a stale
  // report creates is "which firmware is this about", and one previous answer
  // is enough to settle it. Remove first because rename onto an existing name
  // fails on FAT.
  if (Storage.exists("/crash_report.prev.txt")) {
    Storage.remove("/crash_report.prev.txt");
  }
  if (Storage.rename("/crash_report.txt", "/crash_report.prev.txt")) {
    LOG_INF("SYS", "Archived crash_report.txt as crash_report.prev.txt before update");
  } else {
    // Diagnostics must never block an install. Say so and continue.
    LOG_ERR("SYS", "Could not archive crash_report.txt -- continuing with the update");
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  return resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP || resetReason == ESP_RST_INT_WDT ||
         resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
}

}  // namespace HalSystem
