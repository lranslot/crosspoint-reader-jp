#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
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
constexpr uint8_t CACHE_VERSION = 5;

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
  if (isMarkdown && SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::CHECKBOX &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
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
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (prevTriggered && currentPage > 0) {
    goToOffset(pageOffsets[currentPage - 1], false);
    enteredFromForward = true;
    requestUpdate();
  } else if (nextTriggered) {
    if (currentPage < totalPages - 1) {
      goToOffset(pageOffsets[currentPage + 1], true);
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

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
  // Reserve a fixed gutter for the "> " cursor marker on .md files. Taken out of
  // viewportWidth before wrapping so the marker never shifts the text, and before
  // loadPageIndexCache() reads it — viewportWidth is a cache validation field, so
  // an existing .md index rebuilds exactly once and stays consistent afterwards.
  markerWidth = isMarkdown ? renderer.getTextAdvanceX(cachedFontId, "> ", EpdFontFamily::REGULAR) : 0;
  viewportWidth -= markerWidth;
  // The three mark states are not the same width in a proportional font, so
  // "- [ ]", "- [x]" and "- [X]" do not measure alike. Left alone, toggling a line
  // sitting on a wrap boundary would add or remove a visual line while
  // pageOffsets[currentPage + 1] stays put, and the displaced text would fall out
  // of every page. Measuring every state at the width of 'x' keeps wrapping fixed.
  // Computed before loadPageIndexCache()/buildPageIndex() below so the index is
  // built with the same measurements render() will use.
  if (isMarkdown) {
    const int advX = renderer.getTextAdvanceX(cachedFontId, "x", EpdFontFamily::REGULAR);
    checkboxMarkPadSpace = advX - renderer.getTextAdvanceX(cachedFontId, " ", EpdFontFamily::REGULAR);
    checkboxMarkPadUpper = advX - renderer.getTextAdvanceX(cachedFontId, "X", EpdFontFamily::REGULAR);
  } else {
    checkboxMarkPadSpace = 0;
    checkboxMarkPadUpper = 0;
  }
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = renderer.getLineHeight(cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  LOG_DBG("TRS", "Viewport: %dx%d, lines per page: %d", viewportWidth, viewportHeight, linesPerPage);

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
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
  if (!pageOffsets.empty()) {
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

  // TEMPORARY instrumentation (F-2): confirms the checkbox glyphs are the same
  // width, i.e. that toggling cannot change how a line wraps.
  LOG_INF("TRS", "advance space=%d x=%d X=%d | pad space=%d upper=%d marker=%d md=%d",
          renderer.getTextAdvanceX(cachedFontId, " ", EpdFontFamily::REGULAR),
          renderer.getTextAdvanceX(cachedFontId, "x", EpdFontFamily::REGULAR),
          renderer.getTextAdvanceX(cachedFontId, "X", EpdFontFamily::REGULAR), checkboxMarkPadSpace,
          checkboxMarkPadUpper, markerWidth, isMarkdown ? 1 : 0);

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  LOG_DBG("TRS", "Building page index for %zu bytes...", fileSize);

  // A rebuild means the reader changed a setting under a book they had already
  // indexed; say so, because it also costs them a little re-reading (see the
  // snap-to-page-start in initializeReader()).
  GUI.drawPopup(renderer, indexCacheRejected ? tr(STR_REINDEXING) : tr(STR_INDEXING));

  // One handle for the whole sweep instead of an open per page. Scoped so every
  // exit below — including the two breaks — closes it, and so it is never held
  // while the reader could be writing back a checkbox.
  const Txt::SequentialReadScope sequentialRead(*txt);

  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset)) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
  LOG_DBG("TRS", "Built page index: %d pages", totalPages);
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

// Width to add so a checkbox mark measures as if it were 'x', keeping wrapping
// identical across all three states. Negative for 'X', which is wider than 'x'.
int TxtReaderActivity::markPadFor(const char mark) const {
  switch (mark) {
    case ' ':
      return checkboxMarkPadSpace;
    case 'X':
      return checkboxMarkPadUpper;
    default:  // 'x' — already the reference width
      return 0;
  }
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                                         std::vector<CheckboxRef>* outCheckboxes) {
  outLines.clear();
  if (outCheckboxes) outCheckboxes->clear();
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

  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
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

    // Detect a task list marker before wrapping: the notation only ever appears
    // at the head of a source line, which is the first visual line it produces.
    // The match itself runs for every .md line, not just when outCheckboxes was
    // asked for: buildPageIndex() must wrap exactly as render() does, or the two
    // disagree on where pages start.
    char lineMark = 0;  // 0 when this source line is not a task list item
    if (isMarkdown) {
      size_t markPos = 0;
      char mark = 0;
      if (matchCheckbox(line.c_str(), displayLen, markPos, mark)) {
        lineMark = mark;
        if (outCheckboxes != nullptr) {
          outCheckboxes->push_back({static_cast<int>(outLines.size()), offset + pos + markPos, mark != ' '});
        }
      }
    }

    // Track position within this source line (in bytes from pos)
    size_t lineBytePos = 0;

    // Emit at least one visual line for each source line (including blank lines),
    // then continue with wrapping when needed. The remainder is carried as an
    // offset into `line` rather than by rebuilding the string each time, which
    // used to make a long paragraph quadratic in memcpy alone.
    do {
      if (displayLen == 0) {
        outLines.emplace_back();
        break;
      }

      const char* segment = line.c_str() + lineBytePos;
      const size_t segmentLen = displayLen - lineBytePos;

      // Charge the mark at the width of 'x' whatever state it is in, but only
      // while this segment still holds it — once a break has consumed the mark,
      // the rest of the source line measures normally.
      const int markPad = (lineMark != 0 && lineBytePos == 0) ? markPadFor(lineMark) : 0;

      size_t breakPos =
          renderer.findWrapOffset(cachedFontId, segment, viewportWidth, EpdFontFamily::REGULAR, true, markPad);

      if (breakPos >= segmentLen) {
        outLines.emplace_back(segment, segmentLen);
        lineBytePos = displayLen;  // Consumed entire display content
        break;
      }

      if (breakPos == 0) {
        breakPos = 1;  // always make progress, as the previous loop did
      }

      outLines.emplace_back(segment, breakPos);

      // Skip space at break point
      size_t skipChars = breakPos;
      if (breakPos < segmentLen && segment[breakPos] == ' ') {
        skipChars++;
      }
      lineBytePos += skipChars;
    } while (lineBytePos < displayLen && static_cast<int>(outLines.size()) < linesPerPage);

    // Determine how much of the source buffer we consumed
    if (lineBytePos >= displayLen) {
      // Fully consumed this source line, move past the newline
      pos = lineEnd + 1;
    } else {
      // Partially consumed - page is full mid-line
      // Move pos to where we stopped in the line (NOT past the line)
      pos = pos + lineBytePos;
      break;
    }
  }

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
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(currentOffset, currentPageLines, nextOffset, isMarkdown ? &pageCheckboxes : nullptr);

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
  const int contentWidth = viewportWidth;

  // Text starts after the marker gutter (zero-width unless this is a .md file),
  // so the cursor marker never overlaps the text at any alignment.
  const int textLeft = cachedOrientedMarginLeft + markerWidth;
  const int selectedLine = (selectedCheckbox >= 0 && selectedCheckbox < static_cast<int>(pageCheckboxes.size()))
                               ? pageCheckboxes[selectedCheckbox].lineIndex
                               : -1;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    int lineIndex = 0;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = textLeft;
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
            x = textLeft + (contentWidth - textWidth) / 2;
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

        // Drawn into the reserved gutter as a separate call: the line string is
        // never modified, so wrapping stays identical whatever is selected.
        if (lineIndex == selectedLine) {
          renderer.drawText(cachedFontId, cachedOrientedMarginLeft, y, "> ");
        }
        renderer.drawText(cachedFontId, x, y, line.c_str());
      }
      y += lineHeight;
      lineIndex++;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  std::string title;
  if (SETTINGS.statusBarSpec().showsTitle()) {
    title = txt->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, title);
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
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: screen margin (to invalidate cache on margin change)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
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

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
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
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedScreenMargin));
  serialization::writePod(f, cachedParagraphAlignment);
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
