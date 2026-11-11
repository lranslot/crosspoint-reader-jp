#include "HeapDiag.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <cstdio>

namespace heapdiag {

void sample(const char* tag, const char* detail) {
  char buf[192];
  const int n =
      snprintf(buf, sizeof(buf), "%lu %s free=%u max=%u min=%u%s%.80s\n", static_cast<unsigned long>(millis()),
               tag != nullptr ? tag : "-", static_cast<unsigned>(ESP.getFreeHeap()),
               static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(ESP.getMinFreeHeap()),
               detail != nullptr ? " " : "", detail != nullptr ? detail : "");
  if (n <= 0) return;
  // Appended rather than rewritten: the point is the sequence, and the sample
  // taken just before a crash is the one that must already be on the card.
  HalFile log = Storage.open("/heap.txt", O_WRITE | O_CREAT | O_AT_END);
  if (!log) return;
  log.write(buf, static_cast<size_t>(n));
  log.flush();
  log.close();
}

}  // namespace heapdiag
