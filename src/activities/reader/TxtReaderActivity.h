#pragma once

#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  // Byte offset of the page currently displayed. This — not currentPage — is the
  // authoritative reading position: an offset means something before the index
  // exists, and it survives a font size change, which renumbers every page.
  // currentPage is derived from it for display (status bar, page counter).
  size_t currentOffset = 0;
  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  // Offsets of the pages walked through this session, oldest first, current last.
  // Saved with the progress so a reopened book can still page backwards before the
  // index has caught up. Capped because the whole point is to cover the seconds
  // until the index lands, and SD writes a 512-byte sector either way.
  static constexpr size_t PROGRESS_MAX_OFFSETS = 16;
  std::vector<size_t> offsetHistory;

  // Streaming text reader - stores file offsets for each page.
  // Built lazily: starts as {0} and grows a page per loop() pass until it reaches
  // the end of the file. Only complete once indexComplete is set.
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  bool indexComplete = false;       // index has reached the end of the file
  size_t indexBuildOffset = 0;      // how far into the file the index has been built
  // Where the page currently on screen ends. Paging forward uses this rather than
  // the index, so the reader can walk past what has been indexed so far.
  //
  // Invariant: every path that draws a page sets this before drawing. render() is
  // the only such path and assigns it from loadPageAtOffset(). It runs on the
  // render task, asynchronously from loop(), so a page turn can in principle be
  // handled before the first render completes — hence the fallback in loop().
  size_t currentPageNextOffset = 0;
  // The "still indexing" popup costs a full-screen refresh (~1.7 s), so it is
  // shown once per open and the status bar's estimate carries the message after.
  bool loadingPopupShown = false;
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;
  // Set when an index cache existed but failed validation, i.e. a setting changed
  // under a book that had already been indexed. Distinguishes a rebuild from a
  // genuine first open, which need different wording on the popup.
  bool indexCacheRejected = false;

  // --- Markdown checkbox toggling (.md only) ---
  // A checkbox found on the current page. Only the byte inside the brackets is
  // ever rewritten, so the original notation (-/*/+, indent depth) survives.
  struct CheckboxRef {
    int lineIndex;      // visual line index within currentPageLines
    size_t markOffset;  // absolute file offset of the byte inside '[' ']'
    bool checked;
  };

  bool isMarkdown = false;

  // --- Vertical writing (.txt only) ---
  // outLines keeps its meaning: one visual unit in reading order. Horizontally
  // that is a line, vertically a column. Only the rule for where to break
  // changes, and linesPerPage counts columns instead of lines.
  //
  // .md is always horizontal whatever the setting. It is a tool for checklists,
  // not something to read, so typesetting features do not apply to it (spec
  // section 6.3). Keeping the exception here rather than in the setting means
  // the setting stays a plain yes/no.
  bool isVertical = false;
  int cellSize = 0;      // full-width advance; one codepoint occupies this square
  int columnWidth = 0;   // cell plus the gap between columns
  int charsPerUnit = 0;  // characters down one column; 0 when horizontal

  // Column pitch as a percentage of the em, indexed by CrossPointSettings
  // lineSpacing (TIGHT / NORMAL / WIDE). Tied to the em rather than to advanceY
  // because advanceY/em is a property of whichever font the reader installed —
  // measured at 1.14 and at 1.44 in the two halves of one family — and a column
  // pitch that moves with the font is not a typesetting decision.
  static constexpr int COLUMN_RATIO_PERCENT[] = {115, 130, 150};

  // Advance `count` codepoints from `text`, stopping at `len`. Vertical writing
  // breaks on a character count, not a measured width: every codepoint takes one
  // cell, so there is nothing to measure.
  static size_t advanceCodepoints(const char* text, size_t len, int count);
  int markerWidth = 0;  // gutter reserved for the checkbox, measured from "> "
  std::vector<CheckboxRef> pageCheckboxes;
  int selectedCheckbox = -1;
  int checkboxPageStamp = -1;  // which page selectedCheckbox belongs to
  // Box drawn in the gutter instead of showing the "- [ ] " notation. Its side is
  // whatever the gutter and the line height allow, with no nominal cap, so it
  // scales with the reading font. Below the minimum the gutter is left blank
  // rather than drawing something unreadable.
  static constexpr int CHECKBOX_MIN_SIDE = 5;  // under this the cross has no room
  void drawCheckbox(int gutterX, int y, int lineHeight, bool checked, bool selected) const;
  // Whether the last page move was backwards. A page entered from the front is
  // selected at its last checkbox, so walking backwards is not thrown to the top.
  bool enteredFromForward = false;

  bool writeCheckbox(const CheckboxRef& cb) const;

#if defined(TXT_INDEX_VERIFY) && TXT_INDEX_VERIFY
  // --- TEMPORARY: index equivalence verification (F-5 completion condition 2) ---
  // Rebuilds the whole index in one pass and compares it page for page with the
  // one built incrementally, then writes the result to /index-verify-v.txt.
  // Runs from onExit() rather than when the index completes, so the reader never
  // stalls mid-book for the ~20 s the one-shot build takes.
  //
  // Remove this declaration, its definition, the onExit() call, the render timing
  // under the same guard, and -DTXT_INDEX_VERIFY from platformio.ini together.
  // See docs-jp/f5-verify-procedure.md.
  void writeIndexVerifyReport();

  // Page render timing, accumulated over the session by renderPage().
  unsigned long renderMsCount = 0;
  unsigned long renderMsTotal = 0;
  unsigned long renderMsMin = 0;
  unsigned long renderMsMax = 0;
#endif

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderPageVertical(int lineHeight);
  void renderStatusBar() const;

  void initializeReader();
  // outCheckboxes is only passed by render(); buildPageIndex() sweeps every page
  // and must not pay for checkbox scanning.
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                        std::vector<CheckboxRef>* outCheckboxes = nullptr);
  void buildPageIndex();
  // Extend the index by at most maxPages. Returns true once it has reached the end.
  // Never loops back to the caller's caller: one call lays out a bounded number of
  // pages and returns, so loop() keeps sampling the buttons (see f9 design 7.3).
  bool advanceIndex(int maxPages);
  // Pages laid out synchronously when a book opens without a usable index. Bounded
  // by a page count, never by the saved position: tying it to how far the reader
  // had got is exactly what stalled the EPUB reader for 48 s with input unreachable.
  static constexpr int INDEX_WARMUP_PAGES = 16;
  // Sanity floor on the measured average page size before it is trusted to size
  // the index. A page is tens of lines of text, so an average this small means
  // the warm-up measured something unrepresentative (a run of blank lines, a
  // file of one-word lines) and extrapolating from it would reserve wildly too
  // much. Below this the vector is left to grow on its own.
  static constexpr size_t MIN_AVG_BYTES_PER_PAGE = 64;
  // Ceiling on that reservation, in pages. At 4 bytes an entry this is 16 KB,
  // already generous against a ~200 KB DRAM budget; a book past it pays a few
  // reallocations, which is far cheaper than reserving hundreds of KB on the
  // strength of an estimate.
  static constexpr size_t INDEX_RESERVE_MAX_PAGES = 4096;

  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();
  // Index of the page starting at or before `offset`. Binary search over
  // pageOffsets, which is sorted ascending by construction.
  [[nodiscard]] int pageForOffset(size_t offset) const;
  // Move to `offset`, keeping the history in step: forward pushes, backward pops.
  void goToOffset(size_t offset, bool forward);

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             int initialRefreshCountdown)
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        pagesUntilFullRefresh(initialRefreshCountdown) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  // Suppress the loop's tail delay and power saving while the index is still being
  // built incrementally, and only then.
  //
  // Without this, three idle seconds drop the CPU from 160 MHz to 10 MHz (X4 has no
  // PSRAM, so LOW_POWER_FREQ is 10) and add a 50 ms delay per pass. Laying out a
  // page goes from ~80 ms to ~1.3 s, and a 217-page book from ~18 s to five or ten
  // minutes.
  //
  // It also breaks input. gpio.update() runs once per pass, and edges are the
  // difference between consecutive samples, so a press and release completing
  // inside one slow pass is never sampled at all. Latching carries an edge across a
  // long pass; it cannot invent one that was never seen.
  //
  // Normal power saving resumes the moment the index completes.
  bool skipLoopDelay() override { return !indexComplete; }
  bool handleForcedRefresh() override {
    {
      RenderLock lock(*this);
      pagesUntilFullRefresh = 1;
    }
    requestUpdate();
    return true;
  }
  ScreenshotInfo getScreenshotInfo() const override;
};
