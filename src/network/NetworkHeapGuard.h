#pragma once

#include <cstdint>

// The gate every Wi-Fi path passes through before the radio comes up.
//
// Why this exists, from the device's own numbers. Bringing station mode up and
// starting the web server costs about 80 KB of free heap, measured end to end
// on this hardware (net-sta-before -> net-web-begun: 81,480 B on one run,
// 79,828 B on another). From a fresh boot that is affordable -- 129,504 free
// going in, roughly 48,000 left over. After a book has been read it is not:
// 81,456 free going in, 1,628 left, and the screen that has to be drawn next
// cannot get the 189-203 bytes it needs per glyph bitmap. Eleven of those
// failures in a row and the device aborts.
//
// Two things were learned the hard way and both are encoded here:
//
//   Releasing caches is not enough. The measurement build freed everything it
//   knew how to free when a book closed -- page arrays, index, glyph bitmaps,
//   advance tables -- and still only reached 81,456 against a fresh boot's
//   129,504. The missing ~48 KB is held by owners we do not have a handle on
//   (library internals, allocator state). Only a restart returns it.
//
//   Releasing caches belongs here, not at book close. The release costs a
//   re-read from the card on the next page drawn, and doing it every time a
//   book is closed pays that price for everyone, including readers who never
//   touch the network. Moving it to the moment the network is entered pays it
//   only when it buys something. (This placement was the user's correction,
//   and it was the better one.)
//
// So: release, measure, and either carry on or restart into the screen the
// reader asked for. A reader who has not opened a book never restarts; one who
// has waits a few seconds instead of losing the session to an abort.

class GfxRenderer;

namespace netheap {

// Where to land if a restart turns out to be necessary.
enum class Target : uint8_t {
  FileTransfer,  // the file-transfer / Calibre screen
  Home,          // everything else that uses Wi-Fi (OTA, fonts, clock, OPDS, sync)
};

// Release what can be released, then decide. Returns true when the caller may
// bring Wi-Fi up. Does NOT return when a restart is needed -- it reboots.
//
// `tag` names the call site in /heap.txt so a failure can be traced to the
// route that triggered it.
bool ensureHeadroom(const GfxRenderer& renderer, const char* tag, Target target);

// Called from setup() when the boot came from a restart this guard asked for.
// Every ensureHeadroom() for the rest of this boot then proceeds without
// rebooting, however low the heap is.
//
// Without this, a device that cannot reach the threshold even from a fresh boot
// would reboot every time the user opened a network screen, forever. One
// restart is a fix; an endless series of them is a worse fault than the crash.
// It is boot-scoped rather than one-shot because a single route can pass two
// guards -- the file-transfer screen, then the scan inside it.
void suppressForThisBoot();

}  // namespace netheap
