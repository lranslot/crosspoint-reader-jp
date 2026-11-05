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
    uint32_t indentLines = 0;    // ［＃N字下げ］ turned into N ideographic spaces
    uint32_t indentRanges = 0;   // ［＃ここからN字下げ］ ranges applied
    uint32_t indentTooWide = 0;  // ranged indents dropped as too wide to be indents
    uint32_t blanksAdded = 0;    // blank lines inserted around block headings
  };

  // Widest ranged indent still treated as an indent. Beyond this the notation is
  // not indenting a paragraph at all: Aozora writes ［＃ここから１９字下げ］ to push a
  // date or an attribution out to the right margin, which is right-alignment
  // faked with spaces. Replaying that as leading spaces breaks, because the
  // number was chosen against the book's line length and not ours — at 18 pt a
  // horizontal line holds 12 full-width characters, so a 19-character indent
  // overruns the line outright, and a vertical one splits a single line across
  // two columns with its halves at opposite ends of the page.
  //
  // 2 is safe at every size offered: 2 of 12 is 17% of the narrowest line. It
  // also covers every real indent in the sample books — 27 of the 29 ranged
  // indents in 戦争史大観 ask for 0 to 2, and the other two are exactly the
  // faked right-alignment this cap excludes.
  //
  // Those two belong to 地付き / 字上げ, which is stage 3's work and needs a
  // renderer that can right-align. Until then they are dropped — which is what
  // the previous version did with every ranged indent, so nothing regresses.
  static constexpr size_t MAX_RANGE_INDENT = 2;

  // Drop lines [firstLine, secondLine] inclusive, 0-based, counted over the
  // whole file. Used for the legend block, whose bounds are two rule lines
  // located up front in prepareEncoding() — deciding them here, by watching for
  // rule lines as the scan runs, would eat the whole book when a file is missing
  // its closing rule.
  void setHeaderRange(size_t firstLine, size_t secondLine);

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
  size_t rangeIndent_ = 0;        // ［＃ここからN字下げ］ in force, 0 when none
  bool headingLine_ = false;      // a block heading was named on this line
  bool blankBeforeNext_ = false;  // the line just flushed was a block heading
  bool lastLineBlank_ = true;     // true at the start of a file: no leading blank
  bool lineReleased_ = false;     // this line outgrew MAX_LINE_HOLD

  // Append the held line to `out`, inserting a blank line in front of it when a
  // block heading needs one and the file does not already have it.
  void flushLine(std::string& out, bool withNewline);
  // Open a line for ordinary content, applying a ranged indent if one is in
  // force. Called before the first character of a line reaches lineOut_.
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
