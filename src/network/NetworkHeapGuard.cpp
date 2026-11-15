#include "NetworkHeapGuard.h"

#include <Arduino.h>
#include <HeapDiag.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "GfxRenderer.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "components/UITheme.h"

namespace netheap {
namespace {

// --- The thresholds, and where each number comes from -----------------------
//
// Free heap. Bringing the network up and starting the web server consumed
// 81,480 B and 79,828 B on the two complete runs recorded in /heap.txt. The
// screen drawn immediately afterwards then needs its glyph bitmaps, which the
// crash log prices at 189-203 B each; a screen of Japanese is on the order of a
// hundred of them, and the advance table and kern data have just been dropped
// by the release above, so they are re-read too. Call that 25 KB. 82 + 25 is
// 107 KB, so the bar is 110,000.
//
// This is deliberately a high bar. A fresh boot clears it with 129,504 and a
// post-reading state does not with 81,456, which is exactly the split we want;
// in between, the choice is between making someone wait a few seconds and
// letting the device abort, and waiting is the lesser harm.
constexpr uint32_t MIN_FREE = 110000;

// Largest contiguous block. Switching to station mode took the largest block
// from 114,676 to 69,620 on a fresh boot -- it wants 45,056 B in one piece.
// 50,000 leaves a little over that.
//
// Both tests are needed, and one recorded run proves it: that run entered with
// max = 61,428, which clears this bar comfortably, and still ended at 2,048 B
// free. A large block says nothing about the total, and the total says nothing
// about the largest block.
constexpr uint32_t MIN_MAX_BLOCK = 50000;

bool suppressed = false;

}  // namespace

void suppressForThisBoot() { suppressed = true; }

bool ensureHeadroom(const GfxRenderer& renderer, const char* tag, const Target target) {
  // Plan 1, at the moment it earns its cost: hand back the loaded fonts'
  // caches -- the on-demand glyph bitmaps and the per-style advance tables.
  // Nothing is unloaded; what goes is re-read from the card when next needed.
  const int fonts = sdFontSystem.releaseFontCaches(true);

  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxBlock = ESP.getMaxAllocHeap();
  const bool enough = freeHeap >= MIN_FREE && maxBlock >= MIN_MAX_BLOCK;

  char detail[96];
  snprintf(detail, sizeof(detail), "%s fonts=%d need=%u/%u %s", tag, fonts, (unsigned)MIN_FREE,
           (unsigned)MIN_MAX_BLOCK, enough ? "ok" : (suppressed ? "low-suppressed" : "low-restart"));
  heapdiag::sample("net-guard", detail);

  if (enough) return true;

  if (suppressed) {
    // This boot already came from a restart this guard asked for, and the heap
    // still does not reach the bar. Proceeding may abort; rebooting again
    // certainly achieves nothing and would leave the reader unable to open the
    // screen at all. The flag stays set for the rest of the boot, so a route
    // that passes two guards (file transfer, then the scan) cannot turn one
    // restart into a series of them.
    LOG_ERR("NETHEAP", "Still low after a restart (free=%u max=%u) -- proceeding anyway", (unsigned)freeHeap,
            (unsigned)maxBlock);
    return true;
  }

  LOG_INF("NETHEAP", "Low heap before Wi-Fi (free=%u max=%u) -- restarting", (unsigned)freeHeap, (unsigned)maxBlock);

  // Say why before the reboot. silentRestart*() draws its own "loading" popup,
  // but on its own that reads as the device simply going back to the shelf,
  // which is the one outcome this must not look like.
  GUI.drawPopup(renderer, tr(STR_NETWORK_PREPARING));
  delay(900);

  if (target == Target::FileTransfer) {
    silentRestartToFileTransfer();  // does not return
  } else {
    silentRestart();  // does not return
  }
  return false;  // unreachable
}

}  // namespace netheap
