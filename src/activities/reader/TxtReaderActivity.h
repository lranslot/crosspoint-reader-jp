#pragma once

#include <Txt.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "CrossPointSettings.h"
#include "PageTurnQueue.h"
#include "ReaderUtils.h"
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
  // --- Per-line attributes (F-8 stage 2c) ---
  // A line's height stopped being a constant, so what fits on a page is a budget
  // in pixels rather than a count of lines. The attribute that moves the height
  // travels beside the line: currentPageAttrs[i] belongs to currentPageLines[i].
  //
  // A struct rather than a bare bool because stage 2g adds the indent and the
  // hanging indent here, and starting with a bool would mean rewriting every
  // hand-off to widen it (f8-ruby.md section 7.2).
  struct LineAttr {
    bool hasRuby = false;  // a ruby strip is reserved, so this line is taller
    // Which of pageRuby belongs to this line. A count rather than a list: the
    // entries for one line are contiguous because the page is filled in order.
    uint16_t rubyFirst = 0;
    uint8_t rubyCount = 0;
    // Cells of indent in front of this *visual* line, already resolved: the fill
    // loop picks the paragraph's indent for the first visual line of a source
    // line and its hanging indent for every continuation, and clamps both
    // against the live viewport. Deliberately one number and not two — a visual
    // line can only ever be drawn at one offset, and carrying both would let the
    // value the renderer uses disagree with the value the wrap was measured
    // against, which is exactly how a page break drifts away from what is drawn.
    // The hanging indent itself belongs to the source line and never outlives
    // the loop that applies it. See docs-jp/f8-aozora-notation.md section 5.5-8.
    uint8_t indent = 0;
    // --- A7: emphasis dots ---
    // Separate from hasRuby rather than folded into it, because the two reserve
    // different amounts: ruby needs a whole face's em, a dot needs a quarter of
    // one. A line with both is a ruby line — jlreq-d 7.6.2 would move the dots
    // outside the ruby, which is a wider band than either, and we draw no dots
    // there at all (f8-ruby.md 6.1, option 2), so the ruby band is what it costs.
    bool hasBouten = false;
    uint16_t boutenFirst = 0;
    uint8_t boutenCount = 0;
  };
  std::vector<std::string> currentPageLines;
  std::vector<LineAttr> currentPageAttrs;
  int viewportWidth = 0;
  int viewportHeight = 0;
  // What one page holds, and what one line spends out of it. Horizontally the
  // budget is the viewport's height and a line costs the font's line advance;
  // vertically the budget is its width and a column costs the column pitch. One
  // pair of names for both directions keeps the fill loop from asking which way
  // the text runs — only the wrap rule still has to.
  // --- Ruby placement for the page on screen (stages 2d/2e) ---
  // Built only when a page is loaded to be shown. The index-building pass asks
  // for no attributes and so pays none of this.
  struct RubyDraw {
    uint16_t at;        // parent run start, relative to this visual line's bytes
    uint16_t textAt;    // ruby text: offset into pageRubyText
    uint8_t parentLen;  // bytes of parent run on this line
    uint8_t textLen;    // bytes of ruby text
  };
  std::vector<RubyDraw> pageRuby;
  std::string pageRubyText;
  struct BoutenDraw {
    uint16_t at;        // run start, relative to this visual line's bytes
    uint8_t parentLen;  // bytes of it on this line
    bool hollow;
  };
  std::vector<BoutenDraw> pageBouten;
  // The font ruby is drawn in, and its em. Both 0 when the card has no SD family,
  // in which case no band is reserved either: nothing can be drawn there, and
  // reserving the space anyway cost such a reader lines for nothing (stage 2c
  // and 2d did reserve it). A reader with a built-in Latin font takes this path.
  int rubyFontId = 0;
  int rubyEm = 0;
  int pageExtent = 0;
  int normalPitch = 0;
  // What a line carrying ruby costs instead: max(normal pitch, em + ruby + 1).
  // The max() is what makes a generous line spacing absorb the ruby for free
  // rather than making ruby demand a generous line spacing (homework 2 —
  // f8-ruby.md section 2.5). Equal to normalPitch when it is absorbed, and also
  // when there is no ruby face to draw with.
  //
  // Where bouten (A7) will go, when it goes anywhere: this same band, extended
  // outward. jlreq-d section 7.6.2 puts emphasis dots on the same side as ruby —
  // right of the column in vertical writing, above the line in horizontal — and
  // moves them further out, not to the opposite side, when both apply to one
  // character (inner to outer: underline, then ruby, then dots). So the growth
  // is cellSize + rubyEm + dotBand + 2 along the axis that already has a band,
  // and nothing on the other side of the character. See f8-ruby.md section 6.1.
  int rubyPitch = 0;
  // A7. The mark is hand-drawn, so its size is ours to choose rather than a
  // font's to dictate: U+FE45 ﹅, the proper vertical sesame dot, is in none of
  // the six installed sizes. cellSize / 4 is what the table is cut for and what
  // the pitch below is measured against — at that size the band is absorbed by
  // the normal line spacing at every point size, so a page of emphasised text
  // costs nothing (f8-ruby.md 6.1 (c)).
  int boutenDot = 0;
  int boutenPitch = 0;
  // Written to the diagnostic line as bt=drawn/variant/collide/noparent, because
  // serial output does not reach this device's user. A variant we approximate and
  // a dot we refused to draw are both things we only learn about from a real book.
  //
  // ★ WHAT ONE COUNT IS. None of these four is "once per annotation in the
  // file". They are **counts of work done in this session**, accumulated from the
  // open until the line is rewritten in onExit(), and they are reset by nothing.
  // Measured against /book/f8-aozora-test-utf8.txt, which holds 136 annotations,
  // 4 of an unknown kind and 1 whose quoted run does not match: the device
  // reported bt=40/8/1/7.
  //
  //   drawn     one (visual line x span) pair actually drawn, in
  //             collectBoutenForLine() -- so a run broken across two visual lines
  //             counts twice, and a page drawn twice counts twice again. Only the
  //             render pass reaches it. Not comparable to any count of the file.
  //   variant   one annotation whose kind is neither 丸 nor ゴマ, in
  //             stripRubyNotation() -- which runs for every source line of every
  //             loadPageAtOffset() call, the index build included. A page that
  //             starts inside a long line rescans that line from the page start
  //             to its end, so an annotation late in a long line is counted once
  //             per page beginning before it (up to 5 times in the test file's
  //             23,838-byte line).
  //   collide   one (visual line x span) pair suppressed because ruby holds the
  //             band. Same place and same multiplicity as drawn.
  //   noparent  one annotation whose quoted run was not the bytes in front of it.
  //             Same place and same multiplicity as variant.
  //
  // So the file's own numbers are read from /aozora-strip.txt (the converter sees
  // each annotation exactly once), and these say how much the reader did with
  // them. The 8 above is 4 variants x 2 loads of the page holding them; the 7 is
  // 1 annotation x 7 loads of the page holding it. See
  // docs-jp/crash-postmortems.md section 7.
  uint16_t boutenDrawn = 0;
  uint16_t boutenVariant = 0;   // a kind beyond the two shapes we cut
  uint16_t boutenCollide = 0;   // ruby on the same characters, so no dots
  uint16_t boutenNoParent = 0;  // the quoted string was not there to match
  // Times page assembly was asked for with no viewport behind it. Expected to
  // stay 0 forever: loop() will not ask before initializeReader() has run. A
  // non-zero value means that ordering broke again, so it is reported rather
  // than only logged -- serial output does not reach this device's user.
  uint16_t geometryNotReady = 0;
  // Pages whose first line is a continuation whose head lies further back than
  // INDENT_CARRY_MAX_BACK, so its indent could not be recovered and the line was
  // laid out at the margin. Reported rather than only logged, for the same
  // reason as above: a silent wrong indent is how stage 2g's bug survived.
  uint16_t indentCarryLost = 0;
#if READER_OPEN_DIAG
  // Whether this open already has a line on the card, so the next write replaces
  // it instead of adding one. The "boot" flag is latched with it: APP_STATE
  // consumes the flag on the first write, and the replacement has to keep saying
  // what opened the book.
  bool diagLineWritten = false;
  bool diagBootOpen = false;
#endif
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

  // --- Ruby notation (Aozora .txt only) ---
  // content.u8 carries `漢字《かんじ》` as written. It is removed from the text the
  // wrap loop measures and the renderer draws, but never from the file: the
  // reading position and the page index are byte offsets into content.u8, and a
  // second byte stream for the other writing direction would make one number
  // mean two places (F-5 completion condition 7).
  //
  // So a break position found in stripped coordinates has to be put back into
  // file coordinates. What is remembered for that is the removals themselves,
  // not a per-byte map: a line carries a handful of them, never hundreds.
  struct RubySpan {
    uint16_t displayAt;  // where the removal sits in the stripped line
    uint16_t srcLen;     // bytes removed there
    // --- stage 2f: what the removal was, and what it belongs to ---
    // The parent run is in the same stripped coordinates as displayAt and ends
    // there. parentLen 0 means there is nothing to annotate — a `《》` at the
    // start of a line, or one after a character that belongs to no run, such as
    // `※《…》`. Both occur in real files; both must still be removed, so the
    // span exists and only the drawing is skipped.
    uint16_t parentAt = 0;
    uint16_t textAt = 0;    // ruby text: offset into the line's rubyText pool
    uint8_t parentLen = 0;  // bytes of parent run
    uint8_t textLen = 0;    // bytes of ruby text; 0 for a bare `｜` removal
  };
  // One ［＃「…」に…傍点］ removed from the source line. The note names its own
  // parent, so unlike ruby there is no class scan: the run is whatever the
  // quoted string matched, immediately before the note.
  struct BoutenSpan {
    uint16_t displayAt;     // where the note sat, i.e. the end of the parent run
    uint16_t parentAt = 0;  // start of the run, same stripped coordinates
    uint8_t parentLen = 0;  // bytes of it; 0 when the quoted string did not match
    bool hollow = false;    // 白丸 / 白ゴマ and the rest of the white kinds
  };
  bool aozoraMarkup = false;
  std::vector<RubySpan> rubySpans;
  std::vector<BoutenSpan> boutenSpans;
  // Every ruby string of the source line being laid out, packed end to end. One
  // buffer rather than a string per span: a line carries a handful of
  // annotations and each is a few bytes, so separate allocations would cost more
  // in heap headers than in text.
  std::string rubyText;

  // Remove the ruby notation from `line` in place, recording each removal.
  // Removes `《…》` whole, the `｜` that marks where a base run starts, and
  // ［＃「…」に…傍点］ with it.
  //
  // One pass and not two, because every removal's displayAt has to be in final
  // stripped coordinates for toSourceOffset() to add up: a second pass would
  // record its own offsets in coordinates the first pass's entries no longer
  // agree with. A bouten note also pushes a RubySpan, with no text and no
  // parent, purely so those bytes are counted there.
  void stripRubyNotation(std::string& line, std::vector<RubySpan>& spans, std::string& rubyText,
                         std::vector<BoutenSpan>& bouten);

  // The indent marker AozoraStrip writes at the head of an indented line —
  // ［＃N字下げ］ or ［＃N字下げ、折り返してM字下げ］ — is read by
  // aozora::parseIndentNotation() in AozoraIndent.h. The marker is left in the
  // line rather than removed: the caller skips it the same way it skips a
  // task-list marker, so no byte of the file is unaccounted for and the page
  // index keeps naming real offsets.
  //
  // Cells a page owes its first line because that line is the *continuation* of
  // an indented line whose head is on an earlier page. Returns false when the
  // page begins at a line head (nothing to carry) or when the head could not be
  // reached; `cells` is already clamped to this viewport. See stage 2i.
  [[nodiscard]] bool recoverIndentCarry(size_t offset, int& cells);

  // Cells of text a line must keep whatever the notation asks for. Aozora writes
  // ［＃ここから１９字下げ］ to fake right-alignment against its own line length, not
  // ours; clamping to this leaves such a line as far right as it can go instead
  // of pushing its text off the page. 4 cells of a 12-cell line at 18 pt.
  static constexpr int MIN_TEXT_CELLS = 4;
  // Clamp an indent asked for in cells to what this viewport can give.
  [[nodiscard]] int clampIndent(int cells) const;
  // What one horizontal line costs, line spacing included.
  //
  // The horizontal path used to spend the raw advanceY and ignore the setting:
  // getLineHeight(fontId) rather than the overload that takes the compression.
  // The setting was in the UI doing nothing, which is worse than not offering
  // it. Vertical never had the bug -- it multiplies the cell by
  // COLUMN_RATIO_PERCENT[lineSpacing] -- so the two directions now both honour
  // it, each in its own axis, and neither applies it twice.
  [[nodiscard]] static int horizontalLinePitch(int rawLineHeight) {
    return std::max(1,
                    static_cast<int>(static_cast<float>(rawLineHeight) * SETTINGS.getReaderLineCompression() + 0.5f));
  }
  // Where the parent run of an annotation at `end` begins, in `stripped`.
  //
  // Aozora names the parent either explicitly with `｜`, or implicitly as the
  // run of one character class ending at the `《`. The explicit form is not
  // redundant: `霧の｜ロンドン警視庁《スコットランドヤード》` crosses katakana
  // into kanji, and the class scan alone would take only `警視庁`. Measured over
  // the real corpus, every one of its 44 annotations is a kanji run of 1-4
  // characters and `｜` is never needed there; the two cases that need it, and
  // the two with no parent at all, come from the test file (f8-ruby.md 6.2).
  static size_t rubyParentStart(const std::string& stripped, size_t end, size_t markerAt);
  // Offset within the source line that `displayOffset` names. A removal sitting
  // exactly at the break is left for the next visual line rather than pulled
  // back, so the notation always travels with the text that follows it.
  static size_t toSourceOffset(size_t displayOffset, const std::vector<RubySpan>& spans);
  // Whether this visual line carries ruby, from the removals recorded for the
  // source line it came from. A removal sitting exactly on the break belongs to
  // the line that follows it, matching toSourceOffset(); the one case that has no
  // line after it is a removal at the very end, which is why the last visual line
  // of a source line takes that one too.
  static bool lineCarriesRuby(const std::vector<RubySpan>& spans, size_t start, size_t end, bool isLastOfSourceLine);
  // The height this line occupies, which is the only thing a page's capacity is
  // measured in.
  [[nodiscard]] int pitchOf(const LineAttr& attr) const {
    if (attr.hasRuby) return rubyPitch;
    return attr.hasBouten ? boutenPitch : normalPitch;
  }
  // Copy the annotations covering [start, end) of the current source line into
  // pageRuby, in the visual line's own coordinates, and point `attr` at them.
  void collectRubyForLine(size_t start, size_t end, bool isLastOfSourceLine, LineAttr& attr);
  // Ruby above the base run (horizontal) and to its right (vertical), centred on
  // the parent. Measured overhang in the real corpus is at most half a character
  // per side and no two annotations on a line collide, so centring needs no
  // collision handling (f8-ruby.md 6.3).
  void drawRubyHorizontal(const std::string& line, const LineAttr& attr, int lineX, int textTop);
  void drawRubyVertical(const std::string& column, const LineAttr& attr, int cellX, int columnTop) const;

  // --- A7: emphasis dots ---
  // Same shape as the ruby pair above. A dot is drawn per character of the run,
  // not once per run: that is what an emphasis mark means.
  static bool lineCarriesBouten(const std::vector<BoutenSpan>& spans, size_t start, size_t end,
                                bool isLastOfSourceLine);
  void collectBoutenForLine(size_t start, size_t end, bool isLastOfSourceLine, LineAttr& attr);
  void drawBoutenHorizontal(const std::string& line, const LineAttr& attr, int lineX, int textTop) const;
  void drawBoutenVertical(const std::string& column, const LineAttr& attr, int cellX, int columnTop) const;
  // One mark, from the bitmap table, with its top-left corner at (left, top).
  void drawBoutenMark(int left, int top, bool hollow) const;

  // --- Vertical writing (.txt only) ---
  // outLines keeps its meaning: one visual unit in reading order. Horizontally
  // that is a line, vertically a column. Only the rule for where to break
  // changes, and the page budget is spent across the width instead of the height.
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
  // Borrowing lineSpacing's enum means depending on how many values it has, not
  // just on their names. Adding a step to LINE_COMPRESSION without adding a
  // ratio here would leave the new step indexing past the array: the use site
  // falls back to NORMAL rather than reading out of bounds, so the column pitch
  // would quietly stop following the setting instead of failing. This turns that
  // into a build error that names the place. Upstream has already made this
  // change twice — #3037 added a fourth step and #4174 replaced the steps with
  // seven percentages — so following upstream will land here first.
  static_assert(std::size(COLUMN_RATIO_PERCENT) == CrossPointSettings::LINE_COMPRESSION_COUNT,
                "COLUMN_RATIO_PERCENT needs one column pitch per CrossPointSettings::LINE_COMPRESSION step");

  // Vertical writing breaks on a character count, not a measured width: every
  // codepoint takes one cell, so there is nothing to measure. The rule itself is
  // vertwrap::advanceCodepoints() in VerticalWrap.h, so tools/layout_harness can
  // drive the real one rather than a copy.
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

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  // Also stamped into the page index. Since stage 2c the index records the
  // settings it was built from and nothing it derived from them, so a new way of
  // cutting pages cannot leave an input with nowhere to be stored (see
  // f8-ruby.md section 5.5).
  uint8_t cachedLineSpacing = 0;
  uint8_t cachedVerticalWriting = 0;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderPageVertical(int lineHeight);
  void renderStatusBar() const;

  void initializeReader();
#if READER_OPEN_DIAG
  // One line in /reader-open.txt per book opened, rewritten in place rather than
  // appended a second time: the open is announced before anything is laid out so
  // a crash still leaves a line, then the same line is replaced on close with the
  // counters the reading actually produced. [env:default] only.
  void writeOpenDiagnostic();
#endif
  // outCheckboxes is only passed by render(); buildPageIndex() sweeps every page
  // and must not pay for checkbox scanning.
  // Page turns owed, and whether a render is still outstanding for the last
  // batch. The flag is written by loop() on the main task and cleared by
  // render() on the render task, so it is atomic; nothing else about a page turn
  // crosses tasks.
  PageTurnQueue turnQueue;
  ReaderUtils::HeldPageSkip heldSkip;
  std::atomic<bool> renderOutstanding{false};
  // Times fewer pages were turned than were asked for -- the end of the book, or
  // the index frontier going backwards. Reported on the diagnostic line for the
  // same reason as the others: serial output does not reach this device's user.
  uint16_t turnsShort = 0;
  // Where the page starting at `offset` ends. Lays the page out when neither the
  // last render nor the index can say, which is what lets a multi-page turn work
  // past the index frontier.
  [[nodiscard]] size_t nextPageOffset(size_t offset);
  // Turn `n` pages, positive forward, in one render.
  void applyPageTurns(int n);
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, std::vector<LineAttr>* outAttrs,
                        size_t& nextOffset, std::vector<CheckboxRef>* outCheckboxes = nullptr);
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
  // BY_SCREEN turns the page here instead of confirming, and toggles the checkbox
  // in a .md — both handled in loop(), neither of them Confirm.
  bool powerButtonActsAsConfirm() const override { return false; }
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
