#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Strips Aozora Bunko markup from UTF-8 text, one chunk at a time.
//
// Aozora ships its texts as plain text with an inline notation: ruby as
// `漢字《かんじ》`, annotations as `［＃…］`, external characters as `※［＃…］`.
// Reading it as-is puts all of that on screen. This removes it during the
// conversion pass, so everything downstream sees a file that never had it —
// stripping at display time instead would shift every byte offset and break
// both the page index and the markdown checkbox write-back.
//
// Fed the same chunks the encoder produces, in order. Markup that straddles a
// chunk boundary is held back internally and re-examined with the next chunk,
// so callers do not need to align anything.
//
// Since stage 2h the emphasis notation is kept the same way: ［＃「○○」に傍点］
// reaches content.u8 and the reader turns it into dots. Only the form that names
// its own parent is kept — the ［＃傍点］…［＃傍点終わり］ range form appears nowhere
// in the corpus or the test file, so it is counted and dropped as before.
//
// Since stage 2a the ruby notation is kept: `漢字《かんじ》` reaches content.u8
// intact, and the reader removes it from the text it lays out while keeping the
// file offsets pointing at real bytes. Keeping one byte stream for both writing
// directions is what makes a reading position survive a switch between them
// (F-5 completion condition 7); a stream per direction would make one number
// mean two places. See f8-ruby.md section 3.2.
class AozoraStrip {
 public:
  struct Stats {
    uint32_t ruby = 0;           // 《…》 seen (kept since stage 2a, not removed)
    uint32_t range = 0;          // ｜ seen (kept since stage 2a, not removed)
    uint32_t note = 0;           // ［＃…］ removed
    uint32_t gaijiRestored = 0;  // ※［＃…U+xxxx…］ replaced with that codepoint
    uint32_t gaijiGeta = 0;      // ※［＃…］ with no U+ replaced with 〓
    uint32_t headerLines = 0;    // lines dropped with the legend block
    uint32_t indentLines = 0;    // ［＃N字下げ］ carried through as a line attribute
    uint32_t indentRanges = 0;   // ［＃ここからN字下げ］ ranges applied
    uint32_t indentHanging = 0;  // 、折り返してM字下げ — ranges that also set a hanging indent
    uint32_t blanksAdded = 0;    // blank lines inserted around block headings
    uint32_t bouten = 0;         // ［＃「…」に…傍点］ kept, for the reader to draw
    uint32_t boutenRange = 0;    // ［＃傍点］…［＃傍点終わり］ — unsupported, still dropped
    // Annotations naming a kind we do not cut a shape for (三角, 二重丸, 蛇の目,
    // ばつ). Counted here and nowhere else, because the converter is the one pass
    // that sees each annotation exactly once: the reader's own bt= counters count
    // scans and renders, which is a different question and was read as this one
    // (docs-jp/crash-postmortems.md section 7).
    uint32_t boutenVariant = 0;
  };

  // Widest indent the notation is allowed to ask for. The cap that used to live
  // here was 2, because the indent was replayed as leading ideographic spaces
  // and the number in the file was chosen against the book's line length rather
  // than ours: ［＃ここから１９字下げ］ is right-alignment faked with spaces, and at
  // 18 pt a horizontal line holds 12 full-width characters, so replaying 19 of
  // them overran the line outright.
  //
  // Since stage 2g the indent is a line attribute instead, and the only place
  // that knows how many cells a line actually has is the reader, which clamps it
  // against the live viewport (TxtReaderActivity, MIN_TEXT_CELLS). So the cap is
  // gone from here and what survives is a sanity bound on the notation itself:
  // 99 is two full-width digits, which is every value the grammar can spell
  // without the file being malformed. 戦争史大観's two ［＃ここから１９字下げ］ lines
  // now reach the reader and land as far right as the line allows, instead of
  // being dropped.
  static constexpr size_t MAX_INDENT = 99;

  // Drop lines [firstLine, secondLine] inclusive, 0-based, counted over the
  // whole file. Used for the legend block, whose bounds are two rule lines
  // located up front in prepareEncoding() — deciding them here, by watching for
  // rule lines as the scan runs, would eat the whole book when a file is missing
  // its closing rule.
  void setHeaderRange(size_t firstLine, size_t secondLine);

  // Size the internal scan buffer for the largest chunk the caller will ever
  // feed, once, before the first process() call.
  //
  // Without it the buffer is sized by the first chunk and then has to grow by one
  // carry's worth the first time a chunk arrives behind one, which libstdc++
  // serves by doubling: an 8,205-byte need became a 16,385-byte request, and that
  // request is what aborted the reader on a heap fragmented by a page of Japanese
  // (crash_report.txt, 1.5.0-dev-main-e14f93e4). Reserving per chunk instead is
  // not enough -- MSVC's reserve() applies the same geometric rule, so a figure
  // that creeps up with the data still overshoots. One call with a constant is
  // the only form whose request size is the same on both standard libraries.
  void reserveForChunk(size_t maxChunkBytes);

  // Append the stripped form of `in` to `out`.
  //
  // Returns how many bytes were held back for the next call. `atEnd` marks the
  // final chunk, where anything held back is flushed rather than carried.
  size_t process(const char* in, size_t len, std::string& out, bool atEnd);

  [[nodiscard]] const Stats& stats() const { return stats_; }

 private:
  Stats stats_;
  std::string carry_;   // unterminated markup awaiting the next chunk
  std::string buffer_;  // carry_ + current chunk, scanned in place

  // Output is held one line at a time rather than appended straight to `out`.
  // A block heading announces itself at the *end* of its line —
  // ［＃「第一章…」は中見出し］ follows the text it names — so whether a blank line
  // belongs in front of that line is only known once the line has been read.
  // Holding the line is what allows that decision without reaching backwards
  // into output the caller may already have written to the card.
  //
  // Bounded: past MAX_LINE_HOLD the line is released and its blank-line decision
  // is skipped. The bound is on the line, not on the chunk, so a file converts to
  // the same bytes whatever size the caller happens to read in.
  static constexpr size_t MAX_LINE_HOLD = 4096;
  std::string lineOut_;
  size_t rangeIndent_ = 0;         // ［＃ここからN字下げ］ in force, 0 when none
  size_t rangeHanging_ = 0;        // 、折り返してM字下げ of that range, 0 when none
  size_t lineIndent_ = 0;          // ［＃N字下げ］ seen on this line, overrides the range
  size_t lineHanging_ = 0;         // hanging indent of this line's own notation
  bool lineHasOwnIndent_ = false;  // a line-level ［＃N字下げ］ was seen on this line
  bool headingLine_ = false;       // a block heading was named on this line
  bool blankBeforeNext_ = false;   // the line just flushed was a block heading
  bool lastLineBlank_ = true;      // true at the start of a file: no leading blank
  bool lineReleased_ = false;      // this line outgrew MAX_LINE_HOLD

  // Append the held line to `out`, inserting a blank line in front of it when a
  // block heading needs one and the file does not already have it.
  void flushLine(std::string& out, bool withNewline);
  // Open a line for ordinary content, writing the indent marker in front of it
  // when this line is indented. Called before the first character of a line
  // reaches lineOut_, so an annotation-only line never gets one.
  void beginContent();

  size_t lineNumber_ = 0;
  size_t headerFirst_ = 0;
  size_t headerLast_ = 0;
  bool headerActive_ = false;
  bool headerCounted_ = false;  // so a dropped line is only tallied once

  [[nodiscard]] bool inHeader() const {
    return headerActive_ && lineNumber_ >= headerFirst_ && lineNumber_ <= headerLast_;
  }
};

// True when `data` carries the legend block Aozora inserts whenever the text
// uses its notation ("記号を使わないですむ場合は、入れる必要はありません" in the
// official manual) — so its presence is a reliable marker, and its absence means
// there is nothing to strip. Expects UTF-8.
bool aozoraLooksLikeAozora(const char* data, size_t len);

// Locate the legend block by its two rule lines (runs of 10+ ASCII hyphens).
// Returns false unless both are present in `data`, in which case no legend
// removal should be attempted at all. Line numbers are 0-based. Expects UTF-8.
bool aozoraFindHeaderRange(const char* data, size_t len, size_t& firstLine, size_t& lastLine);
