#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ProgressFile.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
// Increment when the cache format changes — and equally when the meaning of what
// is stored changes. The validation fields describe the viewport, not the wrapping
// algorithm, so any change to how lines are measured or broken must bump this or
// stale pageOffsets are silently accepted.
// v5: F-7 replaced the wrapping algorithm, which can move a break by one character.
// v8 cuts pages by accumulated height instead of by a line count, so a page
// carrying ruby holds fewer lines and every boundary after it moves. The header
// changed shape with it: the fields are the settings the index was built from,
// and no longer anything derived from them (f8-ruby.md section 5.5).
// 9: the ruby strip is reserved from the ruby font's measured em rather than
// from an estimate, which moves rubyPitch and so moves where pages break.
// 10: an indent is a line attribute and spends cells out of the line's budget.
// 11: a line carrying emphasis dots reserves a band for them (A7).
constexpr uint8_t CACHE_VERSION = 11;

// --- A7: the emphasis marks, cut by hand ---
//
// U+FE45 ﹅ and U+FE46 ﹆, the proper sesame dots, are in none of the six
// installed .cpfont sizes, and the substitutes that are there (U+30FB, U+25E6)
// advance a full em with a 10-11 px left bearing, so the mark would sit in the
// middle of a cell-wide box we would then have to reserve. Drawing the mark
// ourselves costs a table and buys the right to choose its size, which is what
// makes the band fit inside the ordinary line spacing.
//
// Cut by hand rather than generated: a midpoint circle at these diameters comes
// out lopsided, one row 4 px wide and the next 2, and these are read at about
// 157 dpi. Bit 0 is the leftmost pixel of a row. Generated-from and checked
// against the mock in docs-jp/f8-ruby.md section 6.2.
constexpr int BOUTEN_MIN_SIZE = 3;
constexpr int BOUTEN_MAX_SIZE = 9;
constexpr uint16_t BOUTEN_FILLED[][9] = {
    {0x002, 0x007, 0x002, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000},  // 3 px
    {0x006, 0x00F, 0x00F, 0x006, 0x000, 0x000, 0x000, 0x000, 0x000},  // 4 px
    {0x00E, 0x01F, 0x01F, 0x01F, 0x00E, 0x000, 0x000, 0x000, 0x000},  // 5 px
    {0x01E, 0x03F, 0x03F, 0x03F, 0x03F, 0x01E, 0x000, 0x000, 0x000},  // 6 px
    {0x01C, 0x03E, 0x07F, 0x07F, 0x07F, 0x03E, 0x01C, 0x000, 0x000},  // 7 px
    {0x03C, 0x07E, 0x0FF, 0x0FF, 0x0FF, 0x0FF, 0x07E, 0x03C, 0x000},  // 8 px
    {0x07C, 0x0FE, 0x1FF, 0x1FF, 0x1FF, 0x1FF, 0x1FF, 0x0FE, 0x07C},  // 9 px
};
constexpr uint16_t BOUTEN_HOLLOW[][9] = {
    {0x002, 0x005, 0x002, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000},  // 3 px
    {0x006, 0x009, 0x009, 0x006, 0x000, 0x000, 0x000, 0x000, 0x000},  // 4 px
    {0x00E, 0x011, 0x011, 0x011, 0x00E, 0x000, 0x000, 0x000, 0x000},  // 5 px
    {0x01E, 0x021, 0x021, 0x021, 0x021, 0x01E, 0x000, 0x000, 0x000},  // 6 px
    {0x01C, 0x022, 0x041, 0x041, 0x041, 0x022, 0x01C, 0x000, 0x000},  // 7 px
    {0x03C, 0x042, 0x081, 0x081, 0x081, 0x081, 0x042, 0x03C, 0x000},  // 8 px
    {0x07C, 0x082, 0x101, 0x101, 0x101, 0x101, 0x101, 0x082, 0x07C},  // 9 px
};

// progress.bin. Not part of the index cache and not validated against it, so
// CACHE_VERSION does not cover this — hence its own magic and version.
constexpr uint32_t PROGRESS_MAGIC = 0x50585450;  // "PTXP"
constexpr uint8_t PROGRESS_VERSION = 1;
constexpr size_t PROGRESS_HEADER_SIZE = 8;  // magic + version + count + 2 reserved
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();

  if (!txt) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Checkbox toggling is .md only: rewriting a .txt the user only meant to read
  // would be too surprising. It is also off for anything read through a converted
  // copy — writeCheckbox() writes into the source file, but the offsets belong to
  // the converted bytes, so writing them back would corrupt it.
  isMarkdown = FsHelpers::hasMarkdownExtension(txt->getPath()) && !txt->isConverted();

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(filePath, fileName, "", "");

  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  Activity::onExit();

#if READER_OPEN_DIAG
  // Before anything is torn down, and only when the open was announced: replaces
  // that line with the same fields, now carrying the counters this reading
  // produced. The open-time line has no bt= worth reading -- at the point it is
  // written nothing has been laid out yet.
  if (diagLineWritten && txt) {
    writeOpenDiagnostic();
  }
#endif

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  offsetHistory.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

void TxtReaderActivity::loop() {
  if (ReaderUtils::handleBackNavigation(mappedInput, activityManager, txt ? txt->getPath().c_str() : "",
                                        {this, [](void* ctx) { static_cast<TxtReaderActivity*>(ctx)->onGoHome(); }})) {
    return;
  }

  // --- Markdown checkbox toggle (short power press) ---
  // Mirrors the footnote binding in EpubReaderActivity: Down is excluded because
  // POWER+DOWN is the screenshot chord (caught earlier in main.cpp, guarded twice).
  //
  // Peeked, not consumed: this only asks whether Down also went up, to decide
  // whether to skip the toggle. With sideButtonLayout NEXT_PREV, Down and PageBack
  // resolve to the same pin, so consuming the edge here would swallow the page turn
  // the same frame would otherwise perform — behaviour that held before latching
  // was introduced and should keep holding.
  // BY_SCREEN lands here too: a .md is a checklist, so toggling the checkbox is
  // what its confirm means. Returns either way, so the page-turn route below never
  // also fires for the same press.
  //
  // A page with no checkbox on it has nothing to toggle, so the press falls
  // through instead of being swallowed: under BY_SCREEN it reaches the page-turn
  // route below, which is what "whatever this screen's confirm means" amounts to
  // on a page that is just text. Tested before wasReleased() so the edge is not
  // consumed on the way past. Under CHECKBOX the fall-through turns no page —
  // ReaderUtils only turns on PAGE_TURN and BY_SCREEN — so that setting keeps
  // doing exactly nothing there, as it did before.
  if (isMarkdown &&
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::CHECKBOX ||
       SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::BY_SCREEN) &&
      !pageCheckboxes.empty() && mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleasedPeek(MappedInputManager::Button::Down)) {
    if (selectedCheckbox >= 0 && selectedCheckbox < static_cast<int>(pageCheckboxes.size())) {
      const unsigned long t0 = millis();
      const bool ok = writeCheckbox(pageCheckboxes[selectedCheckbox]);
      LOG_INF("TRS", "checkbox toggle %s in %lu ms", ok ? "ok" : "FAILED", millis() - t0);
      // Only redraw on success: a silent "it looked like it worked" is worse than
      // no feedback. render() re-reads the page, which proves the byte landed.
      if (ok) {
        requestUpdate();
      }
    }
    return;
  }

  // --- Checkbox cursor movement ---
  // Bound to PageBack/PageForward rather than Up/Down so the side-button layout
  // setting (including SIDE_BUTTONS_DISABLED and the NEXT_PREV swap) is honoured.
  //
  // Sample the side buttons under the same rule as ReaderUtils::detectPageTurn:
  // with longPressButtonBehavior OFF (the default) page turns fire on press, so
  // reading wasReleased() here would let the page turn on the way down and this
  // block would never be reached. Touch and tilt are deliberately not consulted —
  // cursor movement is side-button only.
  const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  const bool sideNext = usePress ? mappedInput.wasPressed(MappedInputManager::Button::PageForward)
                                 : mappedInput.wasReleased(MappedInputManager::Button::PageForward);
  const bool sidePrev = usePress ? mappedInput.wasPressed(MappedInputManager::Button::PageBack)
                                 : mappedInput.wasReleased(MappedInputManager::Button::PageBack);

  // The event is only consumed when the cursor actually moves; otherwise it falls
  // through to page turning below. Neither wasPressed() nor wasReleased() clears
  // state, so the same edge is still visible to detectPageTurn() in this frame.
  if (isMarkdown && selectedCheckbox >= 0) {
    const int n = static_cast<int>(pageCheckboxes.size());
    if (sideNext) {
      if (selectedCheckbox < n - 1) {
        const unsigned long t0 = millis();
        selectedCheckbox++;
        requestUpdate();
        LOG_INF("TRS", "checkbox cursor fwd in %lu ms", millis() - t0);
        return;
      }
      // Last checkbox of the last page: swallow it. TxtReaderActivity has no
      // end-of-book screen, so the next press would call onGoHome() and close the
      // file with no confirmation — too costly in the middle of editing.
      if (currentPage >= totalPages - 1) {
        return;
      }
      // Otherwise fall through to the page turn below.
    } else if (sidePrev) {
      if (selectedCheckbox > 0) {
        const unsigned long t0 = millis();
        selectedCheckbox--;
        requestUpdate();
        LOG_INF("TRS", "checkbox cursor back in %lu ms", millis() - t0);
        return;
      }
      // At the first checkbox: fall through to the page turn below.
    }
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;

  // Extend the index one page per pass, after the buttons have been read and
  // before the early return, so it runs every pass rather than only on a turn.
  // Placed here rather than at the top of loop() because a tick ahead of the
  // button read makes every press wait a tick; ~80 ms per page keeps the pass
  // short enough that a press is neither lost (it is latched) nor felt.
  //
  // Held under the RenderLock: this runs on the main task while render() runs on
  // the render task, and it both grows pageOffsets (a reallocation would move the
  // array out from under a concurrent read) and seeks the shared sequential read
  // handle. Skipped rather than waited on when a render already holds the lock —
  // the next pass will pick it up, and blocking here would stall input.
  //
  // `initialized` is the other half of that: initializeReader() is called from
  // render(), so it runs on the render task, and until it has run the viewport is
  // still zero. onEnter() only sets requestedUpdate, so the notify to the render
  // task does not go out until the end of ActivityManager::loop(); that leaves a
  // window where this pass reaches the index extension before the render task has
  // taken the lock, and lays a page out against a budget of nothing. That is what
  // crashed the reader on open -- see PageBudget.h.
  //
  // Read unsynchronised here on purpose, exactly as indexComplete is: it is a
  // hint, and a stale one costs a skipped tick. The decision is the re-check
  // below, which the lock orders -- initializeReader() runs under this same
  // RenderLock, so acquiring it happens after the write of every field it sets.
  bool indexJustCompleted = false;
  if (initialized && !indexComplete && !RenderLock::peek()) {
    RenderLock lock;
    // Re-check under the lock: peek() and acquisition are not atomic, and render()
    // can finish the index itself between the two.
    if (initialized && !indexComplete) {
      indexJustCompleted = advanceIndex(1);
    }
  }
  if (indexJustCompleted) {
    // Safe without the lock: indexComplete is set, so pageOffsets no longer changes.
    savePageIndexCache();
    // Rule 2: recompute the page number, but leave the offset where the reader
    // put it. Snapping here would move the text under them mid-read.
    currentPage = pageForOffset(currentOffset);
    requestUpdate();
  }

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  const size_t fileSize = txt->getFileSize();

  if (prevTriggered) {
    // Rule 4: the walked route first — those offsets are exact even off-grid.
    if (offsetHistory.size() > 1) {
      const size_t previous = offsetHistory[offsetHistory.size() - 2];
      goToOffset(previous, false);
      enteredFromForward = true;
      requestUpdate();
    } else if (currentOffset <= pageOffsets.back() && currentPage > 0) {
      // No history, but the index reaches back this far.
      goToOffset(pageOffsets[currentPage - 1], false);
      enteredFromForward = true;
      requestUpdate();
    } else if (currentPage > 0 && !loadingPopupShown) {
      // Behind the indexed range with nothing walked to fall back on. Refusing is
      // the correct answer: building synchronously to satisfy it is exactly the
      // 48-second stall the EPUB reader hit. Said once — a full-screen refresh per
      // press would be worse than the refusal itself, and the status bar keeps
      // showing the count as estimated.
      loadingPopupShown = true;
      GUI.drawPopup(renderer, tr(STR_LOADING));
      pagesUntilFullRefresh = 1;
      requestUpdate();
    }
  } else if (nextTriggered) {
    // Rule 3: walk forward by where the drawn page ended, so the reader is never
    // held up by the index. Falls back to the index if render() has not run yet —
    // it is on another task, so the very first turn can arrive first.
    const size_t nextOffset = currentPageNextOffset > currentOffset
                                  ? currentPageNextOffset
                                  : (currentPage + 1 < totalPages ? pageOffsets[currentPage + 1] : fileSize);
    if (nextOffset < fileSize) {
      goToOffset(nextOffset, true);
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

#if READER_OPEN_DIAG
// One appended line per book opened, written to the SD card because serial is
// not reachable on the test device. Diagnoses the intermittent report that a
// book sometimes starts horizontal with tofu in the corner: isVertical depends
// on nothing but the setting and the .md test, so a horizontal open means one of
// those two is not what we think it is at that moment. Both are printed next to
// the path they were derived from, and the font state is printed beside them
// because the tofu appears at the same time.
//
// Accumulated, never truncated: the failure is intermittent, so a good open and
// a bad one have to sit next to each other to be compared. Guarded by
// READER_OPEN_DIAG, which only [env:default] defines.
void TxtReaderActivity::writeOpenDiagnostic() {
  // Format first: nothing already on the card is disturbed until there is a new
  // line worth keeping.
  const std::string& path = txt->getPath();
  const char* wanted = SETTINGS.sdFontFamilyName;
  const char* loaded = sdFontSystem.loadedFamilyName();

  // Stack budget: one line, well under the 256-byte local limit. The path is
  // truncated rather than given its own allocation — what matters is the
  // extension and whether the string has anything odd on its tail.
  // gr/gf are running totals for the session, not for this open: glyphs are read
  // while the page is drawn, which is after this line is written. The difference
  // between two consecutive lines is what happened between those two opens.
  //
  // bt= is the opposite: it counts this book only, and it is zero until pages have
  // been laid out. That is why the line is written twice -- once here, where the
  // open is announced and bt= is necessarily 0/0/0/0, and again from onExit(),
  // which replaces it with the totals. A line still reading 0/0/0/0 next to a
  // timestamp near the boot time therefore means the session never reached
  // onExit(): it crashed, or the device is still in that book.
  const bool bootOpen = diagLineWritten ? diagBootOpen : APP_STATE.bootOpenedReader;
  char line[224];
  const int n =
      snprintf(line, sizeof(line),
               "%lu open=%s md=%d vw=%d vert=%d len=%u font=%s/%s id=%d vp=%dx%d n=%d gr=%u gf=%u bt=%u/%u/%u/%u "
               "path=[%.60s]\n",
               static_cast<unsigned long>(millis()), bootOpen ? "boot" : "library", isMarkdown ? 1 : 0,
               static_cast<int>(SETTINGS.verticalWriting), isVertical ? 1 : 0, static_cast<unsigned>(path.size()),
               wanted[0] ? wanted : "-", loaded[0] ? loaded : "-", cachedFontId, viewportWidth, viewportHeight,
               pageExtent / normalPitch, SdCardFont::glyphRetries(), SdCardFont::glyphFailures(), boutenDrawn,
               boutenVariant, boutenCollide, boutenNoParent, path.c_str());
  if (n <= 0) {
    return;
  }
  const size_t lineLen = std::min(static_cast<size_t>(n), sizeof(line) - 1);

  // There is no append-mode open. SDCardManager::openFileForWrite() passes
  // O_TRUNC, so size() is 0 the moment the file is open and seeking to it lands
  // at the start: every open overwrote the previous line, and the file only ever
  // held the last book opened. Read what is there, then write it back ahead of
  // the new line. A handful of lines fits in one buffer; past the cap the oldest
  // lines are dropped, because the newest open is the one being diagnosed.
  static constexpr size_t KEEP_BYTES = 3072;
  const auto kept = makeUniqueNoThrow<char[]>(KEEP_BYTES);
  size_t keptLen = 0;
  size_t keptFrom = 0;
  if (kept) {
    HalFile in;
    if (Storage.openFileForRead("TRS", "/reader-open.txt", in)) {
      const size_t existing = in.size();
      const size_t from = existing > KEEP_BYTES ? existing - KEEP_BYTES : 0;
      if (from == 0 || in.seekSet(from)) {
        const int got = in.read(kept.get(), existing - from);
        if (got > 0) {
          keptLen = static_cast<size_t>(got);
        }
      }
      if (from > 0) {
        // The cut landed mid-line; drop the fragment so the file stays readable
        // line by line.
        while (keptFrom < keptLen && kept[keptFrom] != '\n') {
          keptFrom++;
        }
        if (keptFrom < keptLen) {
          keptFrom++;
        }
      }
      // This open already announced itself, so the line about to be written is a
      // replacement, not a second entry: drop the trailing one. Trimming the kept
      // buffer rather than seeking keeps the file at one line per open, which is
      // the only thing anything reading it relies on.
      if (diagLineWritten && keptLen > keptFrom) {
        size_t cut = keptLen - 1;  // the newline the last line ends with
        while (cut > keptFrom && kept[cut - 1] != '\n') {
          cut--;
        }
        keptLen = cut;
      }
      in.close();  // must close before reopening the same path for write
    }
  }

  HalFile file;
  if (!Storage.openFileForWrite("TRS", "/reader-open.txt", file)) {
    return;  // a diagnostic that cannot be written is not worth a failed open
  }
  if (keptLen > keptFrom) {
    file.write(kept.get() + keptFrom, keptLen - keptFrom);
  }
  file.write(line, lineLen);
  // Consume the flag: only the book the boot opened by itself gets "boot". Latched
  // first, because the replacement written on close has to report the same thing.
  diagBootOpen = bootOpen;
  diagLineWritten = true;
  APP_STATE.bootOpenedReader = false;
}
#endif

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedScreenMargin = SETTINGS.screenMargin;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginTop += cachedScreenMargin;
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  cachedOrientedMarginBottom +=
      std::max(cachedScreenMargin, static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight()));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  // Reserve a fixed gutter on .md files for the checkbox drawn by renderPage().
  // Taken out of viewportWidth before wrapping so the box never shifts the text,
  // and before loadPageIndexCache() reads it — viewportWidth is a cache validation
  // field, so an existing .md index rebuilds exactly once and stays consistent
  // afterwards. Its width is still measured from "> ": the box is sized to fit
  // whatever gutter that yields, which keeps viewportWidth unchanged from the
  // text-cursor era and so costs one cache generation less.
  // Only an Aozora conversion leaves the notation in content.u8. Every other text
  // file means its 《》 literally. Decided before the pitches below, which ask
  // whether a line on this book can carry ruby at all.
  aozoraMarkup = txt && txt->hasAozoraMarkup();

  markerWidth = isMarkdown ? renderer.getTextAdvanceX(cachedFontId, "> ", EpdFontFamily::REGULAR) : 0;
  viewportWidth -= markerWidth;
  viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  cachedLineSpacing = SETTINGS.lineSpacing;
  cachedVerticalWriting = SETTINGS.verticalWriting;
  isVertical = SETTINGS.verticalWriting != 0 && !isMarkdown;
  // The em box, which is what a ruby strip is measured against. Taking it from
  // the full-width advance rather than from the font's ascender is deliberate:
  // the ascender is the rising box and runs about half again as tall, and
  // reserving from it overshoots by more than a line (f8-ruby.md section 0.5).
  cellSize = renderer.getFullWidthAdvance(cachedFontId);
  if (cellSize < 1) cellSize = lineHeight;
  if (isVertical) {
    const size_t ratioIdx = (SETTINGS.lineSpacing < std::size(COLUMN_RATIO_PERCENT)) ? SETTINGS.lineSpacing : 1;
    columnWidth = cellSize * COLUMN_RATIO_PERCENT[ratioIdx] / 100;
    if (columnWidth < 1) columnWidth = 1;
    charsPerUnit = viewportHeight / cellSize;
    if (charsPerUnit < 1) charsPerUnit = 1;
    // Columns are laid out across the width, so that is the budget a page has
    // to spend; a column costs its pitch.
    pageExtent = viewportWidth;
    normalPitch = columnWidth;
  } else {
    columnWidth = charsPerUnit = 0;
    pageExtent = viewportHeight;
    normalPitch = lineHeight;
  }
  if (normalPitch < 1) normalPitch = 1;

  // A ruby glyph's own advance is half the em, rounded up: measured 13/15/17/19
  // against em 25/29/33/37 across the four point sizes the fonts ship. Stage 2d
  // replaces this with the ruby font's measured advance once there is one; if
  // that disagrees, the page boundaries move and CACHE_VERSION moves with them.
  // Ruby is drawn in the installed size nearest half the reader size, because a
  // family ships whole point sizes and nothing on the card is half of 12 or 14.
  // The reserved strip is that font's own em, measured, not the (cell+1)/2 the
  // design memo assumed: on the measured family the smallest file is 8 pt with a
  // 17 px em, so a 14 pt reader reserves 47 px per ruby line, not 45. Reserving
  // the assumed figure would have let every ruby strip overflow by two pixels.
  //
  // 17 and not 16: the 8 pt advance is 16.6875, which toPixel rounds up. The
  // figure was briefly recorded as 16 because the reader used to check it read
  // U+4E00 out of a 12-byte glyph record, and the on-disk record is 16 bytes
  // (EpdGlyph is not packed). See docs-jp/f8-ruby.md section 2.5-2.
  //
  // With no ruby face there is no band. The estimate that used to stand in here
  // reserved (cell + 1) / 2 -- 45 px at 14 pt against the 47 px a real face asks
  // for -- for glyphs that cannot be drawn, because every draw path is guarded on
  // rubyFontId. That spent a page's height on white space and moved the page
  // boundaries of a reader whose card has no SD family, which is exactly the
  // reader that can least afford to lose lines.
  rubyFontId = sdFontSystem.loadRubyFont(renderer, SETTINGS.fontPointSize);
  rubyEm = rubyFontId ? renderer.getFullWidthAdvance(rubyFontId) : 0;
  if (rubyEm < 1) rubyEm = 0;
  rubyPitch = rubyEm > 0 ? std::max(normalPitch, cellSize + rubyEm + 1) : normalPitch;

  // A7. A quarter of the em, which is the largest mark that still disappears
  // into the ordinary line spacing: cellSize + cellSize/4 + 1 comes to 32/37/42/48
  // against a normal pitch of 32/37/42/49 at 12/14/16/18 pt, so a page of
  // emphasised text holds exactly as many lines as a page without it. Only the
  // tight spacing pays, 4-5 px per emphasised line. Nothing is read from a font
  // here — the marks are drawn from a table, so this works on a card with no
  // Japanese family at all, unlike ruby.
  boutenDot = std::max(BOUTEN_MIN_SIZE, std::min(BOUTEN_MAX_SIZE, cellSize / 4));
  boutenPitch = std::max(normalPitch, cellSize + boutenDot + 1);
  // Only an Aozora .txt can carry ruby. Everything else keeps a constant pitch,
  // which is the same arithmetic the line count used to be.
  if (!aozoraMarkup || isMarkdown) {
    rubyPitch = normalPitch;
    boutenPitch = normalPitch;
  }

  LOG_DBG("TRS", "Viewport: %dx%d, %s per page: %d (pitch %d, ruby %d)%s", viewportWidth, viewportHeight,
          isVertical ? "columns" : "lines", pageExtent / normalPitch, normalPitch, rubyPitch,
          isVertical ? "" : " (horizontal)");

#if READER_OPEN_DIAG
  writeOpenDiagnostic();
#endif

  // Try to load cached page index first
  if (loadPageIndexCache()) {
    indexComplete = true;
    indexBuildOffset = txt->getFileSize();
  } else {
    // No usable cache: lay out only enough to open the book. loop() extends the
    // index behind the reader, and savePageIndexCache() runs when it finishes —
    // writing a partial index would cache a lie.
    buildPageIndex();
    if (indexComplete) {
      savePageIndexCache();
    }
  }

  // Load saved progress
  loadProgress();

  // Snap the restored position to the start of the page that contains it. A saved
  // offset was a page boundary under the layout in force when it was written; if
  // the index has since been rebuilt (font size, margins, orientation) it now
  // points into the middle of a page. Rendering from there and then paging forward
  // to pageOffsets[n+1] would step backwards, repeating the lines already shown.
  // A no-op for a normal resume, where the offset is already a boundary. The cost
  // is re-reading up to a page, which is what STR_REINDEXING warns about.
  // Four rules govern how the position relates to the index, together keeping the
  // reader on the page grid without ever moving the text under them:
  //   1. On open, snap only if the index already covers currentOffset. If it does
  //      not (a deep resume the warm-up did not reach), leave the offset alone.
  //   2. When the index completes, do NOT snap. The reader has been reading by
  //      then, and snapping would jump them back up to a page — the same visible
  //      defect fixed in d5e0ed91. Only currentPage is recomputed.
  //   3. Paging forward always uses currentPageNextOffset, never the index, so it
  //      works past the indexed range and cannot be dragged onto the grid.
  //   4. Paging backward prefers offsetHistory, the route actually walked, which
  //      is exact whether or not those offsets sit on the grid.
  // Off-grid reading is therefore harmless, and the next open starts from a valid
  // cache that covers the position, where rule 1 quietly puts it back on the grid.
  if (!pageOffsets.empty() && currentOffset <= pageOffsets.back()) {
    currentPage = pageForOffset(currentOffset);
    currentOffset = pageOffsets[currentPage];
    // Everything behind the tail was a page start under the previous layout and
    // now points into the middle of a page. Snapping the tail alone would leave
    // those to be written back, reproducing the same duplicated lines as soon as
    // paging backwards starts reading the history. Drop them.
    if (indexCacheRejected && offsetHistory.size() > 1) {
      offsetHistory.erase(offsetHistory.begin(), offsetHistory.end() - 1);
    }
    // The history tail is what saveProgress() writes back as the current position,
    // so leave it agreeing with the snapped offset rather than the stale one.
    if (!offsetHistory.empty()) {
      offsetHistory.back() = currentOffset;
    }
  }

  initialized = true;
}

// Lay out the opening pages so the book can be shown, then stop. The rest is
// built a page at a time from loop().
void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0
  indexBuildOffset = 0;
  indexComplete = false;

  LOG_DBG("TRS", "Starting page index for %zu bytes...", txt->getFileSize());

  // A rebuild means the reader changed a setting under a book they had already
  // indexed; say so, because it also costs them a little re-reading (see the
  // snap-to-page-start in initializeReader()). Only the warm-up is announced —
  // everything after it happens quietly behind the reader.
  GUI.drawPopup(renderer, indexCacheRejected ? tr(STR_REINDEXING) : tr(STR_INDEXING));

  advanceIndex(INDEX_WARMUP_PAGES);

  // Size the index once, here, from the page size the warm-up just measured.
  // It cannot be done any earlier: an average needs real page boundaries, and
  // nothing but laying pages out produces them -- before the warm-up there is
  // only a file size, which says nothing about how many pages it wraps to at the
  // current font and margins. Doing it now means the rest of the index lands in
  // one block rather than forcing a geometric regrowth (211 pages = 8
  // reallocations) a page at a time while the reader is reading. Same
  // extrapolation the status bar uses for its estimated page count.
  //
  // Skipped when the warm-up already finished the index: a file that small has
  // its final size, and reserving past it would only waste the slack.
  if (!indexComplete && !pageOffsets.empty() && indexBuildOffset > 0) {
    const size_t avgBytesPerPage = indexBuildOffset / pageOffsets.size();
    if (avgBytesPerPage >= MIN_AVG_BYTES_PER_PAGE) {
      size_t estimate = txt->getFileSize() / avgBytesPerPage;
      // An eighth of headroom against the warm-up landing on denser-than-average
      // text. Sixteen pages is a small sample: if they happen to be unusually
      // full, the measured page size comes out too large and the page count too
      // small, which costs exactly the one reallocation this is here to avoid.
      // The opposite case needs no protection -- short opening pages measure a
      // small average and so overestimate the count, which merely over-reserves.
      estimate += estimate / 8 + 1;
      // std::clamp is undefined when lo > hi. It holds here only because this runs
      // immediately after the warm-up, where pageOffsets.size() is around 17 and
      // so far below INDEX_RESERVE_MAX_PAGES that the two cannot cross. Moving
      // this call later, to a point where the index may already hold more than
      // INDEX_RESERVE_MAX_PAGES pages, would break that and has to be guarded.
      estimate = std::clamp(estimate, pageOffsets.size(), INDEX_RESERVE_MAX_PAGES);
      if (estimate > pageOffsets.capacity()) {
        pageOffsets.reserve(estimate);
      }
    }
  }

  LOG_DBG("TRS", "Warm-up index: %d pages%s, reserved %zu", totalPages, indexComplete ? " (complete)" : "",
          pageOffsets.capacity());
}

bool TxtReaderActivity::advanceIndex(const int maxPages) {
  if (indexComplete) {
    return true;
  }
  const size_t fileSize = txt->getFileSize();

  // Reuse one handle across the pages laid out here. beginSequentialRead() is a
  // no-op when already open, so the handle survives from tick to tick; the
  // checkbox write-back closes it and the next tick simply reopens. Reopening per
  // page instead cost ~130 ms, well over the ~80 ms a page takes to lay out.
  txt->beginSequentialRead();

  for (int i = 0; i < maxPages && indexBuildOffset < fileSize; i++) {
    std::vector<std::string> tempLines;
    size_t nextOffset = indexBuildOffset;

    if (!loadPageAtOffset(indexBuildOffset, tempLines, nullptr, nextOffset) || nextOffset <= indexBuildOffset) {
      // Cannot read or cannot make progress: treat the index as finished here
      // rather than retrying forever.
      LOG_ERR("TRS", "Index build stopped at %zu of %zu", indexBuildOffset, fileSize);
      indexComplete = true;
      break;
    }

    indexBuildOffset = nextOffset;
    if (indexBuildOffset < fileSize) {
      pageOffsets.push_back(indexBuildOffset);
    }

    // Yield during the synchronous warm-up. The per-tick path returns to loop()
    // after a single page and needs no yield of its own.
    if (maxPages > 1 && pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  if (indexBuildOffset >= fileSize) {
    indexComplete = true;
  }
  totalPages = static_cast<int>(pageOffsets.size());

  if (indexComplete) {
    txt->endSequentialRead();
  }
  return indexComplete;
}

namespace {
// Match a Markdown task list item at the start of a source line:
//   [ \t]* ('-' | '*' | '+') ' ' '[' (' ' | 'x' | 'X') ']'
// On success writes the offset of the byte inside the brackets (relative to the
// start of the line) to markPos and its state to checked. Hand-written rather
// than a regex to keep it off the heap.
bool matchCheckbox(const char* line, size_t len, size_t& markPos, char& mark) {
  size_t i = 0;
  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
  if (i >= len || (line[i] != '-' && line[i] != '*' && line[i] != '+')) return false;
  i++;
  if (i >= len || line[i] != ' ') return false;
  i++;
  if (i >= len || line[i] != '[') return false;
  i++;
  if (i >= len) return false;
  const char c = line[i];
  if (c != ' ' && c != 'x' && c != 'X') return false;
  if (i + 1 >= len || line[i + 1] != ']') return false;
  markPos = i;
  mark = c;
  return true;
}
}  // namespace

// Draw one task list checkbox into the marker gutter.
//
// The box keeps the same shape, size and position in all four states; selection
// is expressed by inverting the gutter cell behind it and drawing the box in
// white. Filling the box itself would read as "checked" and mix the two axes.
void TxtReaderActivity::drawCheckbox(const int gutterX, const int y, const int lineHeight, const bool checked,
                                     const bool selected) const {
  // Sized entirely from the gutter and the line, with no nominal cap: a 12 px box
  // beside an 18 pt line reads as an afterthought, and the gutter already scales
  // with the reading font because it is measured from "> ". The margins left over
  // are 2 px at the sides and 4 px above and below.
  const int side = std::min(markerWidth - 4, lineHeight - 8);
  if (side < CHECKBOX_MIN_SIDE) return;  // no room: leave the gutter blank

  const int bx = gutterX + (markerWidth - side) / 2;
  const int by = y + (lineHeight - side) / 2;

  if (selected) renderer.fillRect(gutterX, y, markerWidth, lineHeight, true);
  const bool ink = !selected;  // white box over an inverted cell

  renderer.drawRect(bx, by, side, side, ink);
  if (checked) {
    // A cross, not a tick: it is symmetric, so inverting the cell cannot change
    // its shape, and it puts ink in all four corners of the box, which a tick
    // leaves empty. See docs-jp/f2-checkbox-glyph.md for the four-way comparison.
    const int inner = side - 2;  // inside the 1 px border
    // Weight tracks the box so the cross looks the same at 8 pt and at 18 pt.
    const int stroke = std::max(2, side / 6);
    // drawLine() thickens downwards only (y + i), so the plain line has to end
    // stroke-1 px high for the thickened band to land on the last inner row.
    // Nothing then spills onto the border and no clipping is needed.
    const int x0 = bx + 1;
    const int y0 = by + 1;
    const int x1 = x0 + inner - 1;
    const int y1 = y0 + inner - stroke;
    renderer.drawLine(x0, y0, x1, y1, stroke, ink);
    renderer.drawLine(x1, y0, x0, y1, stroke, ink);
  }
}

// Walks `count` UTF-8 codepoints, stopping at `len`. Returns the byte offset
// reached, which is what the wrap loop wants.
size_t TxtReaderActivity::advanceCodepoints(const char* text, const size_t len, const int count) {
  size_t i = 0;
  for (int taken = 0; taken < count && i < len; taken++) {
    i++;
    while (i < len && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) i++;
  }
  return i;
}

namespace {
// The classes Aozora's implicit parent rule runs over. `Other` never forms a
// run, which is what stops a scan at a comma, a space, or `※`.
enum class RubyClass : uint8_t { Other, Kanji, Hiragana, Katakana, Alnum };

RubyClass rubyClassOf(const uint32_t cp) {
  if ((cp >= 0x4E00 && cp <= 0x9FFF) || cp == 0x3005 || cp == 0x3006 || cp == 0x30F6 ||
      (cp >= 0xF900 && cp <= 0xFAFF)) {
    return RubyClass::Kanji;
  }
  if (cp >= 0x3041 && cp <= 0x309F) return RubyClass::Hiragana;
  if ((cp >= 0x30A1 && cp <= 0x30FA) || cp == 0x30FC || (cp >= 0xFF66 && cp <= 0xFF9F)) return RubyClass::Katakana;
  if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return RubyClass::Alnum;
  if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) || (cp >= 0xFF41 && cp <= 0xFF5A)) {
    return RubyClass::Alnum;
  }
  return RubyClass::Other;
}

// Start of the UTF-8 character ending at `end`, i.e. step back one character.
size_t utf8StepBack(const std::string& s, size_t end) {
  if (end == 0) return 0;
  size_t i = end - 1;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i--;
  return i;
}

// Characters, not bytes: a vertical column advances one cell per character.
size_t countCodepoints(const std::string& s) {
  size_t n = 0;
  for (const char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) n++;
  }
  return n;
}

uint32_t codepointAt(const std::string& s, const size_t at) {
  const auto* p = reinterpret_cast<const unsigned char*>(s.data() + at);
  return utf8NextCodepoint(&p);
}
}  // namespace

size_t TxtReaderActivity::rubyParentStart(const std::string& stripped, const size_t end, const size_t markerAt) {
  // An explicit `｜` wins outright: it exists precisely for the runs the class
  // scan cannot find, and honouring the scan over it would silently shorten
  // them. npos means there is no marker open for this annotation.
  if (markerAt != std::string::npos && markerAt <= end) return markerAt;

  if (end == 0) return end;
  const size_t last = utf8StepBack(stripped, end);
  const RubyClass want = rubyClassOf(codepointAt(stripped, last));
  if (want == RubyClass::Other) return end;  // no run; the annotation has no parent

  size_t start = last;
  while (start > 0) {
    const size_t prev = utf8StepBack(stripped, start);
    if (rubyClassOf(codepointAt(stripped, prev)) != want) break;
    start = prev;
  }
  return start;
}

// Does `kind` name one of the two shapes the table actually cuts? 丸 and ゴマ
// are drawn as themselves; 三角, 二重丸, 蛇の目 and ばつ come out as the disc or
// the ring on their own side of the black/white split, and are counted so a book
// using one is heard from rather than silently approximated.
namespace {
constexpr char BOUTEN_MARU[] = "\xE4\xB8\xB8";              // 丸
constexpr char BOUTEN_GOMA[] = "\xE3\x82\xB4\xE3\x83\x9E";  // ゴマ

bool boutenKindIsExact(const char* kind, const size_t len) {
  size_t at = 0;
  if (len >= 3 && memcmp(kind, "\xE7\x99\xBD", 3) == 0)
    at = 3;  // 白
  else if (len >= 3 && memcmp(kind, "\xE9\xBB\x92", 3) == 0)
    at = 3;  // 黒
  const size_t rest = len - at;
  return (rest == 3 && memcmp(kind + at, BOUTEN_MARU, 3) == 0) || (rest == 6 && memcmp(kind + at, BOUTEN_GOMA, 6) == 0);
}

// ［＃「X」に<kind>傍点］. Returns false for every other annotation, which is what
// leaves the indent marker — also an annotation — in the line to be read as a
// prefix further down.
struct BoutenNote {
  size_t quotedAt = 0;
  size_t quotedLen = 0;
  bool hollow = false;
  bool approximated = false;
};

bool parseBoutenNote(const char* body, const size_t len, BoutenNote& out) {
  static constexpr char KAGI_OPEN[] = "\xE3\x80\x8C";           // 「
  static constexpr char KAGI_CLOSE[] = "\xE3\x80\x8D";          // 」
  static constexpr char NI[] = "\xE3\x81\xAB";                  // に
  static constexpr char BOUTEN[] = "\xE5\x82\x8D\xE7\x82\xB9";  // 傍点
  static constexpr char SHIRO[] = "\xE7\x99\xBD";               // 白
  static constexpr size_t CH = 3;
  static constexpr size_t BOUTEN_LEN = 6;

  if (len < CH * 3 + BOUTEN_LEN) return false;
  if (memcmp(body, KAGI_OPEN, CH) != 0) return false;
  // The first 」 ends the quoted run, which is the rule the notation's own
  // grammar uses; a quoted run containing one is a malformed file.
  size_t closeAt = CH;
  while (closeAt + CH <= len && memcmp(body + closeAt, KAGI_CLOSE, CH) != 0) closeAt++;
  if (closeAt + CH > len) return false;
  const size_t quotedLen = closeAt - CH;
  size_t at = closeAt + CH;
  if (at + CH > len || memcmp(body + at, NI, CH) != 0) return false;
  at += CH;
  const size_t kindAt = at;
  if (len < BOUTEN_LEN || len - BOUTEN_LEN < kindAt) return false;
  if (memcmp(body + len - BOUTEN_LEN, BOUTEN, BOUTEN_LEN) != 0) {
    return false;  // ［＃「…」に傍線］ and the rest: not ours, and not removed
  }
  const size_t kindLen = len - BOUTEN_LEN - kindAt;
  out.quotedAt = CH;
  out.quotedLen = quotedLen;
  out.hollow = false;
  for (size_t k = 0; k + CH <= kindLen; k++) {
    if (memcmp(body + kindAt + k, SHIRO, CH) == 0) {
      out.hollow = true;
      break;
    }
  }
  out.approximated = kindLen > 0 && !boutenKindIsExact(body + kindAt, kindLen);
  return true;
}
}  // namespace

void TxtReaderActivity::stripRubyNotation(std::string& line, std::vector<RubySpan>& spans, std::string& rubyText,
                                          std::vector<BoutenSpan>& bouten) {
  // 《 U+300A, 》 U+300B, ｜ U+FF5C — written as bytes so the encoding of this
  // file cannot change what they mean.
  static constexpr char RUBY_OPEN[] = "\xE3\x80\x8A";
  static constexpr char RUBY_CLOSE[] = "\xE3\x80\x8B";
  static constexpr char RANGE_MARK[] = "\xEF\xBD\x9C";
  static constexpr char NOTE_OPEN[] = "\xEF\xBC\xBB\xEF\xBC\x83";  // ［＃
  static constexpr char NOTE_CLOSE[] = "\xEF\xBC\xBD";             // ］
  static constexpr size_t MARK_LEN = 3;
  static constexpr size_t NOTE_OPEN_LEN = 6;

  if (line.find(RUBY_OPEN) == std::string::npos && line.find(RANGE_MARK) == std::string::npos &&
      line.find(NOTE_OPEN) == std::string::npos) {
    return;  // the common line: nothing to do and nothing copied
  }

  std::string out;
  out.reserve(line.size());
  // Where the run named by an open `｜` starts, in stripped coordinates. Reset
  // once an annotation has consumed it, so a stray marker earlier in the line
  // cannot claim a later annotation's parent.
  size_t markerAt = std::string::npos;
  size_t i = 0;
  while (i < line.size()) {
    const size_t remain = line.size() - i;
    if (remain >= MARK_LEN && memcmp(line.data() + i, RUBY_OPEN, MARK_LEN) == 0) {
      const size_t close = line.find(RUBY_CLOSE, i + MARK_LEN);
      // An unclosed 《 is not a ruby annotation. Left alone rather than swallowing
      // the rest of the line, which is what a file using 《》 as quotation marks
      // would otherwise lose.
      if (close != std::string::npos) {
        const size_t parentAt = rubyParentStart(out, out.size(), markerAt);
        markerAt = std::string::npos;
        const size_t textLen = close - (i + MARK_LEN);
        RubySpan span{};
        span.displayAt = static_cast<uint16_t>(out.size());
        span.srcLen = static_cast<uint16_t>(close + MARK_LEN - i);
        span.parentAt = static_cast<uint16_t>(parentAt);
        // Clamped rather than widened: the longest parent measured in the real
        // corpus is 4 characters and the longest ruby 10, so a byte holds both
        // with room to spare, and a file that exceeds it is malformed enough
        // that drawing a shortened annotation beats refusing the line.
        span.parentLen = static_cast<uint8_t>(std::min<size_t>(out.size() - parentAt, 255));
        span.textAt = static_cast<uint16_t>(rubyText.size());
        span.textLen = static_cast<uint8_t>(std::min<size_t>(textLen, 255));
        rubyText.append(line, i + MARK_LEN, span.textLen);
        spans.push_back(span);
        i = close + MARK_LEN;
        continue;
      }
    } else if (remain >= NOTE_OPEN_LEN && memcmp(line.data() + i, NOTE_OPEN, NOTE_OPEN_LEN) == 0) {
      const size_t close = line.find(NOTE_CLOSE, i + NOTE_OPEN_LEN);
      BoutenNote note;
      if (close != std::string::npos &&
          parseBoutenNote(line.data() + i + NOTE_OPEN_LEN, close - (i + NOTE_OPEN_LEN), note)) {
        const size_t noteLen = close + MARK_LEN - i;
        // Recorded in the offset map as well, with no text and no parent, so the
        // bytes it occupies are counted there and the page index keeps naming
        // real file offsets.
        RubySpan skip{};
        skip.displayAt = static_cast<uint16_t>(out.size());
        skip.srcLen = static_cast<uint16_t>(noteLen);
        skip.parentAt = skip.displayAt;
        spans.push_back(skip);

        BoutenSpan span{};
        span.displayAt = static_cast<uint16_t>(out.size());
        span.hollow = note.hollow;
        // The note names its own parent, so the run is matched rather than
        // scanned for by character class: it is those bytes, immediately before
        // the note. All 126 notes in the corpus and the test file matched; one
        // that does not is left undrawn rather than guessed at, because a guess
        // would emphasise the wrong word.
        const char* quoted = line.data() + i + NOTE_OPEN_LEN + note.quotedAt;
        if (note.quotedLen > 0 && note.quotedLen <= 255 && out.size() >= note.quotedLen &&
            memcmp(out.data() + out.size() - note.quotedLen, quoted, note.quotedLen) == 0) {
          span.parentAt = static_cast<uint16_t>(out.size() - note.quotedLen);
          span.parentLen = static_cast<uint8_t>(note.quotedLen);
        } else {
          boutenNoParent++;
        }
        if (note.approximated) boutenVariant++;
        bouten.push_back(span);
        i += noteLen;
        continue;
      }
    } else if (remain >= MARK_LEN && memcmp(line.data() + i, RANGE_MARK, MARK_LEN) == 0) {
      // ｜ only marks where the base run starts; it is never shown. On a line
      // with no 《 it is an ordinary vertical bar, so it is only dropped when a
      // ruby annotation follows it.
      if (line.find(RUBY_OPEN, i + MARK_LEN) != std::string::npos) {
        RubySpan span{};
        span.displayAt = static_cast<uint16_t>(out.size());
        span.srcLen = static_cast<uint16_t>(MARK_LEN);
        span.parentAt = span.displayAt;  // nothing to draw; kept for the offset map
        spans.push_back(span);
        markerAt = out.size();
        i += MARK_LEN;
        continue;
      }
    }
    out.push_back(line[i]);
    i++;
  }
  line.swap(out);
}

size_t TxtReaderActivity::toSourceOffset(const size_t displayOffset, const std::vector<RubySpan>& spans) {
  size_t src = displayOffset;
  for (const auto& span : spans) {
    // Recorded in order, so the first one at or past the break ends the walk.
    if (span.displayAt >= displayOffset) break;
    src += span.srcLen;
  }
  return src;
}

void TxtReaderActivity::collectRubyForLine(const size_t start, const size_t end, const bool isLastOfSourceLine,
                                           LineAttr& attr) {
  attr.rubyFirst = static_cast<uint16_t>(pageRuby.size());
  attr.rubyCount = 0;
  for (const auto& span : rubySpans) {
    if (span.textLen == 0 || span.parentLen == 0) {
      continue;  // a bare `｜`, or an annotation with nothing to annotate
    }
    const bool mine =
        (span.displayAt >= start && span.displayAt < end) || (isLastOfSourceLine && span.displayAt == end);
    if (!mine) continue;

    // A parent run can begin on the previous visual line when the wrap fell
    // inside it. Only the part on this line is drawn over; the ruby still
    // centres on what is visible, which is the best a line break allows.
    const size_t from = std::max<size_t>(span.parentAt, start);
    RubyDraw draw{};
    draw.at = static_cast<uint16_t>(from - start);
    draw.parentLen = static_cast<uint8_t>(span.displayAt - from);
    draw.textAt = static_cast<uint16_t>(pageRubyText.size());
    draw.textLen = span.textLen;
    pageRubyText.append(rubyText, span.textAt, span.textLen);
    pageRuby.push_back(draw);
    attr.rubyCount++;
  }
}

void TxtReaderActivity::drawRubyHorizontal(const std::string& line, const LineAttr& attr, const int lineX,
                                           const int textTop) {
  for (uint8_t k = 0; k < attr.rubyCount; k++) {
    const RubyDraw& rd = pageRuby[attr.rubyFirst + k];
    if (static_cast<size_t>(rd.at) + rd.parentLen > line.size()) continue;
    const std::string parent(line, rd.at, rd.parentLen);
    const std::string text(pageRubyText, rd.textAt, rd.textLen);
    // getTextAdvanceX needs a null-terminated string, so the prefix is copied
    // rather than viewed (CLAUDE.md, string_view at a C boundary).
    const std::string prefix(line, 0, rd.at);
    const int prefixW = rd.at ? renderer.getTextAdvanceX(cachedFontId, prefix.c_str(), EpdFontFamily::REGULAR) : 0;
    const int parentW = renderer.getTextAdvanceX(cachedFontId, parent.c_str(), EpdFontFamily::REGULAR);
    const int rubyW = renderer.getTextAdvanceX(rubyFontId, text.c_str(), EpdFontFamily::REGULAR);
    // Same overhang, along the line this time: an annotation on the first or
    // last word would otherwise start left of the margin or run past it.
    const int left = cachedOrientedMarginLeft + markerWidth;
    int x = lineX + prefixW + (parentW - rubyW) / 2;
    const int rightmost = left + viewportWidth - rubyW;
    if (x > rightmost) x = rightmost;
    if (x < left) x = left;
    renderer.drawText(rubyFontId, x, textTop - rubyEm - 1, text.c_str());
  }
}

void TxtReaderActivity::drawRubyVertical(const std::string& column, const LineAttr& attr, const int cellX,
                                         const int columnTop) const {
  for (uint8_t k = 0; k < attr.rubyCount; k++) {
    const RubyDraw& rd = pageRuby[attr.rubyFirst + k];
    if (static_cast<size_t>(rd.at) + rd.parentLen > column.size()) continue;
    const std::string parent(column, rd.at, rd.parentLen);
    const std::string text(pageRubyText, rd.textAt, rd.textLen);
    const std::string prefix(column, 0, rd.at);
    // Vertical advance is a count of cells, not a sum of advances, so the
    // position is a count of characters (same rule as advanceCodepoints).
    const int before = static_cast<int>(countCodepoints(prefix));
    const int parentChars = static_cast<int>(countCodepoints(parent));
    const int rubyChars = static_cast<int>(countCodepoints(text));
    int y = columnTop + before * cellSize + (parentChars * cellSize - rubyChars * rubyEm) / 2;
    // A ruby run longer than its parent sticks out at both ends, and at the top
    // or bottom of a column there is nothing to stick out into. On the boundary
    // test file up to 68 px of a strip fell off the page. Pulled back inside
    // rather than clipped: half a strip reads as a different word.
    const int extent = rubyChars * rubyEm;
    const int lowest = columnTop + viewportHeight - extent;
    if (y > lowest) y = lowest;
    if (y < columnTop) y = columnTop;
    renderer.drawTextVertical(rubyFontId, cellX + cellSize + 1, y, rubyEm, text.c_str());
  }
}

bool TxtReaderActivity::lineCarriesBouten(const std::vector<BoutenSpan>& spans, const size_t start, const size_t end,
                                          const bool isLastOfSourceLine) {
  for (const auto& span : spans) {
    if (span.parentLen == 0) continue;
    // The run, not the note, is what has to be on this line: unlike ruby the
    // note sits after its parent, so a wrap between them would otherwise leave
    // the dots on the line that has none of the characters they belong to.
    const size_t from = span.parentAt;
    const size_t to = span.displayAt;
    if (from < end && to > start) return true;
    if (isLastOfSourceLine && from == end) return true;
  }
  return false;
}

void TxtReaderActivity::collectBoutenForLine(const size_t start, const size_t end, const bool isLastOfSourceLine,
                                             LineAttr& attr) {
  attr.boutenFirst = static_cast<uint16_t>(pageBouten.size());
  attr.boutenCount = 0;
  for (const auto& span : boutenSpans) {
    if (span.parentLen == 0) continue;
    const size_t from = std::max<size_t>(span.parentAt, start);
    const size_t to = std::min<size_t>(span.displayAt, end);
    const bool mine = (span.parentAt < end && span.displayAt > start) || (isLastOfSourceLine && span.parentAt == end);
    if (!mine || to <= from) continue;

    // Ruby wins the band. jlreq-d 7.6.2 would put the dots outside the ruby, on
    // the same side; that is a wider band than either and a page model change on
    // top of this one, so for now the dots are dropped and counted instead
    // (f8-ruby.md 6.1, option 2). Measured over the corpus and the test file no
    // character carries both, so this costs nothing that is written today.
    bool underRuby = false;
    for (const auto& ruby : rubySpans) {
      if (ruby.textLen == 0 || ruby.parentLen == 0) continue;
      if (ruby.parentAt<to&& static_cast<size_t>(ruby.displayAt)> from) {
        underRuby = true;
        break;
      }
    }
    if (underRuby) {
      boutenCollide++;
      continue;
    }

    BoutenDraw draw{};
    draw.at = static_cast<uint16_t>(from - start);
    draw.parentLen = static_cast<uint8_t>(to - from);
    draw.hollow = span.hollow;
    pageBouten.push_back(draw);
    attr.boutenCount++;
    boutenDrawn++;
  }
}

void TxtReaderActivity::drawBoutenMark(const int left, const int top, const bool hollow) const {
  // The page is rendered twice, once to collect the glyphs it needs and once to
  // draw. A mark needs no glyph, so it has nothing to contribute to the first
  // pass, and drawPixel is a raw primitive that the scan does not hold back the
  // way it holds back text — it would paint for real, twice. Guarded here rather
  // than at each call so both writing directions behave the same way. Same
  // reasoning as GfxRenderer::drawLeaderDots.
  const auto* fcm = renderer.getFontCacheManager();
  if (fcm != nullptr && fcm->isScanning()) return;
  if (boutenDot < BOUTEN_MIN_SIZE || boutenDot > BOUTEN_MAX_SIZE) return;
  const uint16_t* rows = (hollow ? BOUTEN_HOLLOW : BOUTEN_FILLED)[boutenDot - BOUTEN_MIN_SIZE];
  for (int y = 0; y < boutenDot; y++) {
    const uint16_t bits = rows[y];
    if (bits == 0) continue;
    for (int x = 0; x < boutenDot; x++) {
      if (bits & (1u << x)) renderer.drawPixel(left + x, top + y);
    }
  }
}

void TxtReaderActivity::drawBoutenHorizontal(const std::string& line, const LineAttr& attr, const int lineX,
                                             const int textTop) const {
  for (uint8_t k = 0; k < attr.boutenCount; k++) {
    const BoutenDraw& bd = pageBouten[attr.boutenFirst + k];
    if (static_cast<size_t>(bd.at) + bd.parentLen > line.size()) continue;
    const int top = textTop - boutenDot - 1;
    // One mark per character, which is what an emphasis mark means: a run of
    // four characters is four dots, not one dot over the middle of the word.
    size_t at = bd.at;
    const size_t runEnd = static_cast<size_t>(bd.at) + bd.parentLen;
    while (at < runEnd) {
      size_t next = at + 1;
      while (next < runEnd && (static_cast<unsigned char>(line[next]) & 0xC0) == 0x80) next++;
      const std::string prefix(line, 0, at);
      const std::string glyph(line, at, next - at);
      const int prefixW = at ? renderer.getTextAdvanceX(cachedFontId, prefix.c_str(), EpdFontFamily::REGULAR) : 0;
      const int glyphW = renderer.getTextAdvanceX(cachedFontId, glyph.c_str(), EpdFontFamily::REGULAR);
      drawBoutenMark(lineX + prefixW + (glyphW - boutenDot) / 2, top, bd.hollow);
      at = next;
    }
  }
}

void TxtReaderActivity::drawBoutenVertical(const std::string& column, const LineAttr& attr, const int cellX,
                                           const int columnTop) const {
  for (uint8_t k = 0; k < attr.boutenCount; k++) {
    const BoutenDraw& bd = pageBouten[attr.boutenFirst + k];
    if (static_cast<size_t>(bd.at) + bd.parentLen > column.size()) continue;
    const std::string prefix(column, 0, bd.at);
    const std::string run(column, bd.at, bd.parentLen);
    // Cells, not advances: a vertical line steps one cell per character
    // whatever the glyph measures (same rule as advanceCodepoints).
    const int before = static_cast<int>(countCodepoints(prefix));
    const int chars = static_cast<int>(countCodepoints(run));
    const int left = cellX + cellSize + 1;
    for (int c = 0; c < chars; c++) {
      drawBoutenMark(left, columnTop + (before + c) * cellSize + (cellSize - boutenDot) / 2, bd.hollow);
    }
  }
}

size_t TxtReaderActivity::parseIndentNotation(const std::string& line, uint8_t& indent, uint8_t& hanging) {
  // ［＃ U+FF3B U+FF03, ］ U+FF3D, 字下げ, 、折り返して — bytes, not literals, so the
  // encoding of this file cannot change what they mean. Must stay the spelling
  // AozoraStrip::beginContent() writes.
  static constexpr char NOTE_OPEN[] = "\xEF\xBC\xBB\xEF\xBC\x83";
  static constexpr char NOTE_CLOSE[] = "\xEF\xBC\xBD";
  static constexpr char JISAGE[] = "\xE5\xAD\x97\xE4\xB8\x8B\xE3\x81\x92";
  static constexpr char ORIKAESHI[] = "\xE3\x80\x81\xE6\x8A\x98\xE3\x82\x8A\xE8\xBF\x94\xE3\x81\x97\xE3\x81\xA6";
  static constexpr size_t NOTE_OPEN_LEN = 6;
  static constexpr size_t NOTE_CLOSE_LEN = 3;
  static constexpr size_t JISAGE_LEN = 9;
  static constexpr size_t ORIKAESHI_LEN = 18;

  indent = 0;
  hanging = 0;
  const size_t len = line.size();
  if (len < NOTE_OPEN_LEN || memcmp(line.data(), NOTE_OPEN, NOTE_OPEN_LEN) != 0) return 0;

  // One full-width digit run, then 字下げ. Reading digits only where the grammar
  // puts them is what keeps ［＃ここから１字下げ、折り返して２字下げ］ from being read
  // as a single twelve-cell indent (the mistake the F-8 survey script made).
  const auto readNumber = [&](size_t& at, unsigned& value) {
    const size_t from = at;
    value = 0;
    while (at + 3 <= len && static_cast<uint8_t>(line[at]) == 0xEF && static_cast<uint8_t>(line[at + 1]) == 0xBC &&
           static_cast<uint8_t>(line[at + 2]) >= 0x90 && static_cast<uint8_t>(line[at + 2]) <= 0x99) {
      value = value * 10 + static_cast<unsigned>(static_cast<uint8_t>(line[at + 2]) - 0x90);
      at += 3;
    }
    return at > from;
  };
  const auto expect = [&](size_t& at, const char* want, const size_t wantLen) {
    if (len - at < wantLen || memcmp(line.data() + at, want, wantLen) != 0) return false;
    at += wantLen;
    return true;
  };

  size_t at = NOTE_OPEN_LEN;
  unsigned n = 0;
  if (!readNumber(at, n) || !expect(at, JISAGE, JISAGE_LEN)) return 0;
  unsigned m = 0;
  if (expect(at, ORIKAESHI, ORIKAESHI_LEN)) {
    if (!readNumber(at, m) || !expect(at, JISAGE, JISAGE_LEN)) return 0;
  }
  if (!expect(at, NOTE_CLOSE, NOTE_CLOSE_LEN)) return 0;

  indent = static_cast<uint8_t>(std::min(n, 255u));
  hanging = static_cast<uint8_t>(std::min(m, 255u));
  return at;
}

int TxtReaderActivity::clampIndent(const int cells) const {
  if (cells <= 0) return 0;
  // Vertically a column holds charsPerUnit cells outright; horizontally the line
  // holds as many full-width cells as its width divides into. Both are the live
  // viewport, which is the whole reason this clamp cannot live in the converter.
  const int available = isVertical ? charsPerUnit : (cellSize > 0 ? viewportWidth / cellSize : 0);
  const int room = available - MIN_TEXT_CELLS;
  if (room <= 0) return 0;
  return std::min(cells, room);
}

bool TxtReaderActivity::lineCarriesRuby(const std::vector<RubySpan>& spans, const size_t start, const size_t end,
                                        const bool isLastOfSourceLine) {
  for (const auto& span : spans) {
    if (span.displayAt >= start && span.displayAt < end) return true;
    if (isLastOfSourceLine && span.displayAt == end) return true;
  }
  return false;
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines,
                                         std::vector<LineAttr>* outAttrs, size_t& nextOffset,
                                         std::vector<CheckboxRef>* outCheckboxes) {
  outLines.clear();
  if (outAttrs) outAttrs->clear();
  if (outCheckboxes) outCheckboxes->clear();
  // Attributes are kept here whether or not the caller wants them: they decide
  // what the page holds, so buildPageIndex() has to spend the same budget
  // render() does or the two disagree on where a page starts.
  std::vector<LineAttr> attrs;
  // Only the display load builds ruby placement; the index pass asks for no
  // attributes and would pay for strings it never draws.
  const bool wantRuby = outAttrs != nullptr && rubyFontId != 0;
  // No font is needed to draw a dot, so unlike ruby this does not depend on the
  // card carrying a Japanese family.
  const bool wantBouten = outAttrs != nullptr;
  if (wantRuby) {
    pageRuby.clear();
    pageRubyText.clear();
    pageBouten.clear();
  }
  // How much of the page's budget has been spent. Replaces the line count: with
  // every pitch equal it reaches exactly the old pageExtent / normalPitch lines,
  // which is why keeping ruby out of a book changes nothing about its pages.
  int usedExtent = 0;
  bool pageFull = false;

  // Admit one line of the given height, or report that the page is full. The
  // first line of a page is always admitted: a pitch taller than the whole
  // viewport must still make progress, or the reader stops at that line forever.
  const auto admit = [&](const char* text, const size_t len, const LineAttr attr) {
    const int pitch = pitchOf(attr);
    if (!outLines.empty() && usedExtent + pitch > pageExtent) {
      pageFull = true;
      return false;
    }
    if (len == 0) {
      outLines.emplace_back();
    } else {
      outLines.emplace_back(text, len);
    }
    attrs.push_back(attr);
    usedExtent += pitch;
    return true;
  };
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table with this chunk's codepoints.
  // Without this, every getTextAdvanceX() call in the wrap loop below triggers
  // on-demand glyph loads through the 8-slot overflow ring buffer, which
  // thrashes for any text with more than 8 unique chars (i.e. all English),
  // floods the heap with short-lived bitmap allocations, and eventually
  // corrupts FreeRTOS state. The advance table persists across calls per
  // font, so the cost amortizes to ~ASCII-size after the first chunk.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }

  // Parse lines from buffer
  size_t pos = 0;

  while (pos < chunkSize && !pageFull) {
    // Find end of line
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') {
      lineEnd++;
    }

    // Check if we have a complete line
    bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= fileSize);

    if (!lineComplete && static_cast<int>(outLines.size()) > 0) {
      // Incomplete line and we already have some lines, stop here
      break;
    }

    // Calculate the actual length of line content in the buffer (excluding newline)
    size_t lineContentLen = lineEnd - pos;

    // Check for carriage return
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;

    // Extract line content for display (without CR/LF)
    std::string line(reinterpret_cast<char*>(buffer + pos), displayLen);

    // Take the ruby notation out of what gets measured and drawn. Done before
    // anything reads `line`, so the wrap loop never sees it and the page breaks
    // land exactly where they did before the notation was kept (stage 2b's whole
    // test). `displayLen` follows the stripped string from here on; the bytes
    // removed are remembered so `pos` can be put back into file coordinates.
    //
    // .md is excluded with the same reasoning as everywhere else: it is a
    // checklist, not something typeset, and its lines are never Aozora.
    rubySpans.clear();
    rubyText.clear();
    boutenSpans.clear();
    size_t prefixLen = 0;
    int firstIndent = 0;
    int hangIndent = 0;
    if (aozoraMarkup && !isMarkdown) {
      stripRubyNotation(line, rubySpans, rubyText, boutenSpans);
      displayLen = line.size();

      // Stage 2g: the indent arrives as the notation at the head of the line, and
      // is skipped rather than removed — prefixLen already carries the bytes a
      // line spends on markup that is not laid out, so `pos` stays in file
      // coordinates with nothing new to reconcile. Read after the ruby strip, so
      // the offsets here and the wrap below are in the same (stripped) space.
      uint8_t askedIndent = 0;
      uint8_t askedHanging = 0;
      prefixLen = parseIndentNotation(line, askedIndent, askedHanging);
      if (prefixLen > 0) {
        firstIndent = clampIndent(askedIndent);
        // No hanging indent in the notation means the continuations line up under
        // the first line's text, not back at the margin: that is what 青空文庫
        // writes ［＃ここからN字下げ］ for a block quote to mean.
        hangIndent = askedHanging > 0 ? clampIndent(askedHanging) : firstIndent;
      }
    }

    // Detect a task list marker before wrapping: the notation only ever appears
    // at the head of a source line, which is the first visual line it produces.
    // The match itself runs for every .md line, not just when outCheckboxes was
    // asked for: buildPageIndex() must wrap exactly as render() does, or the two
    // disagree on where pages start.
    //
    // The notation itself is not laid out: prefixLen skips it so wrapping starts
    // at the item's text, and renderPage() draws a box in the marker gutter
    // instead. Because none of "[ ]", "[x]", "[X]" is ever measured, toggling a
    // line can no longer move a wrap point — which is what the width
    // normalisation that used to live here existed to prevent.
    if (isMarkdown) {
      size_t markPos = 0;
      char mark = 0;
      if (matchCheckbox(line.c_str(), displayLen, markPos, mark)) {
        if (outCheckboxes != nullptr) {
          outCheckboxes->push_back({static_cast<int>(outLines.size()), offset + pos + markPos, mark != ' '});
        }
        prefixLen = markPos + 2;  // past the closing ']'
        if (prefixLen < displayLen && line[prefixLen] == ' ') prefixLen++;
      }
    }

    // Track position within this source line (in bytes from pos). Starts past the
    // task list notation when there is one: `pos` is advanced from this, so the
    // skipped bytes stay accounted for and the offsets in outCheckboxes — which
    // toggleSelectedCheckbox() writes through — keep naming the real file bytes.
    size_t lineBytePos = prefixLen;

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed. The remainder is carried as an
    // offset into `line` rather than by rebuilding the string each time, which
    // used to make a long paragraph quadratic in memcpy alone.
    do {
      if (displayLen == 0) {
        // A blank line spends a line's worth of the budget like any other.
        admit(nullptr, 0, LineAttr{});
        break;
      }

      const char* segment = line.c_str() + lineBytePos;
      const size_t segmentLen = displayLen - lineBytePos;

      // The indent is spent out of this visual line's own budget: the paragraph's
      // on its first line, the hanging one on every continuation. Measured here
      // and stored on the line, so what the wrap was computed against and what
      // the renderer draws at are the same number by construction.
      const int indentCells = (lineBytePos == prefixLen) ? firstIndent : hangIndent;
      const int indentPx = indentCells * cellSize;

      // Horizontally the break is a measured width; vertically it is a count,
      // because every codepoint occupies one cell and there is nothing to
      // measure. Both produce the same thing: how many bytes go in this unit.
      size_t breakPos =
          isVertical ? advanceCodepoints(segment, segmentLen, std::max(1, charsPerUnit - indentCells))
                     : renderer.findWrapOffset(cachedFontId, segment, viewportWidth - indentPx, EpdFontFamily::REGULAR);

      // The ruby strip is reserved before the line is admitted, because what it
      // costs is what decides whether it fits. Where the ruby was taken out is
      // known from the removals recorded for this source line, so this is a walk
      // over a handful of entries, not a second pass over the text.
      const bool lastOfLine = breakPos >= segmentLen;
      const size_t emitLen = lastOfLine ? segmentLen : (breakPos == 0 ? 1 : breakPos);
      LineAttr attr;
      attr.hasRuby = lineCarriesRuby(rubySpans, lineBytePos, lineBytePos + emitLen, lastOfLine);
      // Only when there is no ruby: a line with both reserves the ruby band, and
      // the dots on it are dropped rather than drawn into it.
      attr.hasBouten = !attr.hasRuby && lineCarriesBouten(boutenSpans, lineBytePos, lineBytePos + emitLen, lastOfLine);
      attr.indent = static_cast<uint8_t>(indentCells);

      if (lastOfLine) {
        if (!admit(segment, segmentLen, attr)) break;
        if (wantRuby && attr.hasRuby) collectRubyForLine(lineBytePos, lineBytePos + segmentLen, true, attrs.back());
        if (wantBouten) collectBoutenForLine(lineBytePos, lineBytePos + segmentLen, true, attrs.back());
        lineBytePos = displayLen;  // Consumed entire display content
        break;
      }

      breakPos = emitLen;  // never 0: the line has to make progress
      if (!admit(segment, breakPos, attr)) break;
      if (wantRuby && attr.hasRuby) collectRubyForLine(lineBytePos, lineBytePos + breakPos, false, attrs.back());
      if (wantBouten) collectBoutenForLine(lineBytePos, lineBytePos + breakPos, false, attrs.back());

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < segmentLen && segment[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
    } while (lineBytePos < displayLen && !pageFull);

    // Determine how much of the source buffer we consumed
    if (pageFull) {
      // Nothing from lineBytePos on was laid out, so that is where the next page
      // starts. lineBytePos counts stripped bytes and the file does not know
      // about that, hence the conversion back.
      //
      // Unless nothing of this line was laid out at all, in which case it goes to
      // the next page whole and pos must not move: lineBytePos still sits at
      // prefixLen, and walking pos past a task list notation would drop the
      // checkbox from the line and leave toggleSelectedCheckbox() naming a byte
      // that is no longer inside the brackets. A blank line that did not fit is
      // the same case with prefixLen 0.
      if (lineBytePos > prefixLen) pos = pos + toSourceOffset(lineBytePos, rubySpans);
      break;
    }
    // Fully consumed this source line, move past the newline
    pos = lineEnd + 1;
  }

  if (outAttrs) *outAttrs = std::move(attrs);

  // Ensure we make progress even if calculations go wrong
  if (pos == 0 && !outLines.empty()) {
    // Fallback: at minimum, consume something to avoid infinite loop
    pos = 1;
  }

  nextOffset = offset + pos;

  // Make sure we don't go past the file
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  free(buffer);

  return !outLines.empty();
}

// Flip the single byte inside "[ ]" / "[x]" in place. The notation is the same
// length either way, so the file size never changes and neither the page index
// nor its cache (validated on file size, viewport, lines, font) is invalidated.
bool TxtReaderActivity::writeCheckbox(const CheckboxRef& cb) const {
  return txt->writeByteAt(cb.markOffset, cb.checked ? ' ' : 'x');
}

void TxtReaderActivity::render(RenderLock&&) {
  if (!txt) {
    return;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
  }

  if (pageOffsets.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // currentOffset is the position of record; seed it from the page on the first
  // render (before any progress has been loaded) and keep the two in step after.
  if (offsetHistory.empty()) {
    currentOffset = pageOffsets[currentPage];
    offsetHistory.assign(1, currentOffset);
  }
  currentPage = pageForOffset(currentOffset);

  // Load current page content
  size_t nextOffset = currentOffset;
  currentPageLines.clear();
  currentPageAttrs.clear();
  loadPageAtOffset(currentOffset, currentPageLines, &currentPageAttrs, nextOffset,
                   isMarkdown ? &pageCheckboxes : nullptr);
  // Upholds the invariant on currentPageNextOffset: this is the only path that
  // draws a page, and paging forward reads this instead of the index.
  currentPageNextOffset = nextOffset;

  // Only (re)seat the cursor when the page actually changed. render() also runs
  // for a plain redraw — moving the cursor calls requestUpdate(), so resetting
  // unconditionally here would undo the move before it ever reached the screen.
  // Entering a page backwards lands on its last checkbox, so a reader walking
  // towards the start is not thrown back to the top each time.
  const int checkboxCount = static_cast<int>(pageCheckboxes.size());
  if (checkboxCount == 0) {
    selectedCheckbox = -1;
  } else if (checkboxPageStamp != currentPage) {
    selectedCheckbox = enteredFromForward ? checkboxCount - 1 : 0;
  } else if (selectedCheckbox >= checkboxCount) {
    selectedCheckbox = checkboxCount - 1;  // defensive clamp
  }
  checkboxPageStamp = currentPage;
  enteredFromForward = false;

  renderer.clearScreen();
  renderPage();

  // Save progress
  saveProgress();
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  // Vertical takes a separate path and returns. Nothing below this line runs for
  // it, so the horizontal rendering is the same code it was — which is what
  // "horizontal must not change by a pixel" means in practice.
  if (isVertical) {
    renderPageVertical(lineHeight);
    return;
  }
  const int contentWidth = viewportWidth;

  // Text starts after the marker gutter (zero-width unless this is a .md file),
  // so a checkbox never overlaps the text at any alignment.
  const int textLeft = cachedOrientedMarginLeft + markerWidth;

  // Render text lines with alignment. drawBoxes is false for the prewarm scan
  // pass, which accumulates text without drawing — rects are not text and would
  // be painted for real there, to no effect but the cost.
  auto renderLines = [&](const bool drawBoxes) {
    int y = cachedOrientedMarginTop;
    int lineIndex = 0;
    for (const auto& line : currentPageLines) {
      // The ruby strip sits above the base line, so the extra height a ruby line
      // was given is skipped before the text and not after it. With no ruby on
      // the line this adds nothing and y walks by lineHeight exactly as before.
      const LineAttr attr =
          lineIndex < static_cast<int>(currentPageAttrs.size()) ? currentPageAttrs[lineIndex] : LineAttr{};
      const int pitch = pitchOf(attr);
      y += pitch - normalPitch;

      // Outside the empty-line guard below: a "- [ ]" with no text after it
      // still has a box to draw.
      if (drawBoxes) {
        for (size_t i = 0; i < pageCheckboxes.size(); i++) {
          if (pageCheckboxes[i].lineIndex == lineIndex) {
            drawCheckbox(cachedOrientedMarginLeft, y, lineHeight, pageCheckboxes[i].checked,
                         static_cast<int>(i) == selectedCheckbox);
            break;
          }
        }
      }
      if (!line.empty()) {
        // The indent is part of the line's left edge, so every alignment below
        // measures from it: a centred or right-aligned indented line keeps the
        // indent and loses the space on the other side, which is what an indent
        // means. contentWidth shrinks by the same amount for the same reason.
        const int indentPx = attr.indent * cellSize;
        int x = textLeft + indentPx;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth = renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = textLeft + indentPx + (contentWidth - indentPx - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = textLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str());
        // Drawn on the scan pass too: the ruby glyphs have to reach the prewarm
        // set or every one of them falls through the on-demand path.
        if (attr.rubyCount && rubyFontId) drawRubyHorizontal(line, attr, x, y);
        if (attr.boutenCount) drawBoutenHorizontal(line, attr, x, y);
      }
      y += normalPitch;
      lineIndex++;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines(false);  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines(true);
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(true); });
  }
  // scope destructor clears font cache via FontCacheManager
}

// Columns run right to left, characters down each one. The page index and the
// byte offsets are untouched: a "line" of currentPageLines is a column here, and
// nothing else in the activity has to know the difference.
void TxtReaderActivity::renderPageVertical(const int lineHeight) {
  (void)lineHeight;
  const int contentRight = cachedOrientedMarginLeft + viewportWidth;

  auto renderColumns = [&]() {
    int columnIndex = 0;
    int used = 0;  // how far in from the right edge the next column starts
    for (const auto& column : currentPageLines) {
      const LineAttr attr =
          columnIndex < static_cast<int>(currentPageAttrs.size()) ? currentPageAttrs[columnIndex] : LineAttr{};
      const int pitch = pitchOf(attr);
      if (!column.empty()) {
        // Columns are laid out from the right, so `used` names this column's
        // right edge and the cell sits at the left of its band: in vertical
        // writing ruby goes to the right of the base character, which is the
        // extra width a ruby column was given. With a constant pitch this is the
        // old (columnIndex + 1) * columnWidth to the byte.
        // The band holds the base cell, and on a ruby column the ruby strip to
        // its right as well; the group is centred in the band. With no ruby
        // anywhere this is (normalPitch - cellSize) / 2 to the byte, which is
        // what stage 2c placed.
        const int groupWidth = (attr.hasRuby && rubyEm > 0) ? cellSize + 1 + rubyEm : cellSize;
        const int x = contentRight - used - pitch + (pitch - groupWidth) / 2;
        // An indent runs down the column, because that is where a vertical line
        // starts. The ruby strips move with it: they are placed relative to the
        // column top, so passing the indented top keeps every annotation over
        // the character it belongs to.
        const int columnTop = cachedOrientedMarginTop + attr.indent * cellSize;
        renderer.drawTextVertical(cachedFontId, x, columnTop, cellSize, column.c_str());
        if (attr.rubyCount && rubyFontId) drawRubyVertical(column, attr, x, columnTop);
        if (attr.boutenCount) drawBoutenVertical(column, attr, x, columnTop);
      }
      used += pitch;
      columnIndex++;
    }
  };

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderColumns();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  renderColumns();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderColumns]() { renderColumns(); });
  }
}

void TxtReaderActivity::renderStatusBar() const {
  const size_t fileSize = txt->getFileSize();

  // Progress comes from the byte offset, not the page number: it is exact from the
  // moment the book opens, with or without a finished index.
  const float progress = fileSize > 0 ? static_cast<float>(currentOffset) * 100.0f / static_cast<float>(fileSize) : 0;

  // While the index is still growing, totalPages is only what has been laid out so
  // far. Extrapolate from the average page size measured over the indexed part —
  // the status bar marks the number as estimated, so it reads as provisional
  // rather than as a total that keeps changing.
  int displayPage = currentPage + 1;
  int displayTotal = totalPages;
  if (!indexComplete && !pageOffsets.empty() && indexBuildOffset > 0) {
    const size_t avgBytesPerPage = indexBuildOffset / pageOffsets.size();
    if (avgBytesPerPage > 0) {
      displayTotal = static_cast<int>(fileSize / avgBytesPerPage);
      // Past the indexed range the page number has to be estimated too; within it
      // the binary search already gave the exact one.
      if (currentOffset > pageOffsets.back()) {
        displayPage = static_cast<int>(currentOffset / avgBytesPerPage) + 1;
      }
      if (displayTotal < displayPage) {
        displayTotal = displayPage;  // never show 5/3
      }
    }
  }

  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, displayPage, displayTotal, title, 0, 0, true, false, !indexComplete);
}

int TxtReaderActivity::pageForOffset(const size_t offset) const {
  if (pageOffsets.empty()) {
    return 0;
  }
  // pageOffsets is ascending, so the page containing `offset` is the last entry
  // that does not exceed it.
  const auto it = std::upper_bound(pageOffsets.begin(), pageOffsets.end(), offset);
  const int index = static_cast<int>(it - pageOffsets.begin()) - 1;
  return index < 0 ? 0 : index;
}

void TxtReaderActivity::goToOffset(const size_t offset, const bool forward) {
  currentOffset = offset;
  currentPage = pageForOffset(offset);
  if (forward) {
    offsetHistory.push_back(offset);
    if (offsetHistory.size() > PROGRESS_MAX_OFFSETS) {
      offsetHistory.erase(offsetHistory.begin());
    }
  } else if (!offsetHistory.empty()) {
    // Retrace: the page being left is dropped, and the one now shown is the new tail.
    offsetHistory.pop_back();
  }
}

// Layout of progress.bin, written whole on every save:
//   uint32 magic | uint8 version | uint8 count | uint16 reserved | uint32 offset x count
// offset[0] is the current page; each following entry is one page further back.
// A 4-byte file is the old format (a page number) — see loadProgress().
void TxtReaderActivity::saveProgress() const {
  uint8_t data[PROGRESS_HEADER_SIZE + 4 * PROGRESS_MAX_OFFSETS];

  const size_t count = std::min(offsetHistory.size(), PROGRESS_MAX_OFFSETS);
  const uint32_t magic = PROGRESS_MAGIC;
  memcpy(data, &magic, sizeof(magic));
  data[4] = PROGRESS_VERSION;
  data[5] = static_cast<uint8_t>(count);
  data[6] = 0;
  data[7] = 0;

  // History runs oldest-first; the file runs newest-first.
  for (size_t i = 0; i < count; i++) {
    const uint32_t off = static_cast<uint32_t>(offsetHistory[offsetHistory.size() - 1 - i]);
    memcpy(data + PROGRESS_HEADER_SIZE + i * 4, &off, sizeof(off));
  }

  const size_t len = PROGRESS_HEADER_SIZE + count * 4;
  if (!ProgressFile::writeAtomic(txt->getCachePath(), data, len)) {
    LOG_ERR("TRS", "Failed to save progress: offset %zu", currentOffset);
  }
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (!Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    return;
  }

  const size_t size = f.size();
  uint8_t data[PROGRESS_HEADER_SIZE + 4 * PROGRESS_MAX_OFFSETS];

  // Old format: exactly four bytes holding a page number. Read it as before and
  // convert to an offset; the next save rewrites the file in the new format.
  if (size == 4) {
    if (f.read(data, 4) == 4) {
      int page = data[0] + (data[1] << 8);
      if (page >= totalPages) page = totalPages - 1;
      if (page < 0) page = 0;
      currentPage = page;
      currentOffset = pageOffsets.empty() ? 0 : pageOffsets[page];
      offsetHistory.assign(1, currentOffset);
      LOG_DBG("TRS", "Loaded progress (legacy): page %d/%d -> offset %zu", currentPage, totalPages, currentOffset);
    }
    return;
  }

  if (size < PROGRESS_HEADER_SIZE) {
    LOG_DBG("TRS", "Progress file too short (%zu bytes), ignoring", size);
    return;
  }

  const size_t toRead = std::min(size, sizeof(data));
  if (static_cast<size_t>(f.read(data, toRead)) != toRead) {
    return;
  }

  uint32_t magic = 0;
  memcpy(&magic, data, sizeof(magic));
  if (magic != PROGRESS_MAGIC || data[4] != PROGRESS_VERSION) {
    LOG_DBG("TRS", "Progress file unrecognised, ignoring");
    return;
  }

  size_t count = data[5];
  const size_t available = (toRead - PROGRESS_HEADER_SIZE) / 4;
  if (count > available) count = available;
  if (count == 0) {
    return;
  }

  // File is newest-first; history is oldest-first.
  offsetHistory.clear();
  offsetHistory.reserve(count);
  for (size_t i = count; i-- > 0;) {
    uint32_t off = 0;
    memcpy(&off, data + PROGRESS_HEADER_SIZE + i * 4, sizeof(off));
    offsetHistory.push_back(off);
  }

  currentOffset = offsetHistory.back();
  currentPage = pageForOffset(currentOffset);
  LOG_DBG("TRS", "Loaded progress: offset %zu -> page %d/%d (%zu history)", currentOffset, currentPage, totalPages,
          count);
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: viewport height
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint8_t: line spacing
  // - uint8_t: vertical writing
  //
  // Every field is a setting the index was built from, and none is anything
  // derived from them. The previous shape stored the line count and the
  // characters per column, which held the viewport height folded inside them —
  // and a fold has to be undone every time the way pages are cut changes, which
  // is how stage 2c arrived with a height that had nowhere left to go for the
  // second time. Inputs do not fold, so a later change to the way pages are cut
  // leaves this list alone (f8-ruby.md section 5.5).
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }
  // The file exists, so anything that fails from here on is a rejection rather
  // than a first open — the caller uses this to tell the two popups apart.
  indexCacheRejected = true;

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedHeight;
  serialization::readPod(f, cachedHeight);
  if (cachedHeight != viewportHeight) {
    LOG_DBG("TRS", "Cache viewport height mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t margin;
  serialization::readPod(f, margin);
  if (margin != cachedScreenMargin) {
    LOG_DBG("TRS", "Cache screen margin mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint8_t lineSpacing;
  serialization::readPod(f, lineSpacing);
  if (lineSpacing != cachedLineSpacing) {
    LOG_DBG("TRS", "Cache line spacing mismatch, rebuilding");
    return false;
  }

  uint8_t verticalWriting;
  serialization::readPod(f, verticalWriting);
  if (verticalWriting != cachedVerticalWriting) {
    LOG_DBG("TRS", "Cache writing direction mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Loaded page index cache: %d pages", totalPages);
  indexCacheRejected = false;  // accepted after all
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(viewportHeight));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, cachedLineSpacing);
  serialization::writePod(f, cachedVerticalWriting);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }

  LOG_DBG("TRS", "Saved page index cache: %d pages", totalPages);
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}
