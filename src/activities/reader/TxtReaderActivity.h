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

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // --- Markdown checkbox toggling (.md only) ---
  // A checkbox found on the current page. Only the byte inside the brackets is
  // ever rewritten, so the original notation (-/*/+, indent depth) survives.
  struct CheckboxRef {
    int lineIndex;      // visual line index within currentPageLines
    size_t markOffset;  // absolute file offset of the byte inside '[' ']'
    bool checked;
  };

  bool isMarkdown = false;
  int markerWidth = 0;  // gutter reserved for the "> " cursor marker
  std::vector<CheckboxRef> pageCheckboxes;
  int selectedCheckbox = -1;
  int checkboxPageStamp = -1;  // which page selectedCheckbox belongs to
  // Width corrections that make all three mark states (' ' / 'x' / 'X') measure as
  // wide as 'x', so toggling can never change how a line wraps. 'x' itself is 0;
  // the upper-case pad is normally negative because 'X' is wider than 'x'.
  int checkboxMarkPadSpace = 0;
  int checkboxMarkPadUpper = 0;
  int markPadFor(char mark) const;
  // Whether the last page move was backwards. A page entered from the front is
  // selected at its last checkbox, so walking backwards is not thrown to the top.
  bool enteredFromForward = false;

  bool writeCheckbox(const CheckboxRef& cb) const;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  // outCheckboxes is only passed by render(); buildPageIndex() sweeps every page
  // and must not pay for checkbox scanning.
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                        std::vector<CheckboxRef>* outCheckboxes = nullptr);
  void buildPageIndex();
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
