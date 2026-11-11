#pragma once

// Heap readings written to the SD card, not to the serial port.
//
// The reason this exists at all: a conversion that had run many times before
// aborted at 537 s into a session, in operator new, asking for 16,385 bytes
// (crash_report.txt, firmware 1.5.0-dev-main-e14f93e4). The same book converted
// without complaint from a fresh boot. The difference is not how much heap is
// free but how large a single contiguous block is still available -- and this
// device's serial output does not reach its user, so a reading that is only
// logged is a reading nobody will ever see.
//
// getFreeHeap() alone cannot show this. getMaxAllocHeap() is the number that
// decides whether a 16 KB string can grow, so both are always written together.
namespace heapdiag {

// One line appended to /heap.txt:
//   <millis> <tag> free=<bytes> max=<bytes> min=<bytes> <detail>
// `detail` may be nullptr. Costs one open/write/close; call it at points of
// interest, never in a loop.
void sample(const char* tag, const char* detail = nullptr);

}  // namespace heapdiag
