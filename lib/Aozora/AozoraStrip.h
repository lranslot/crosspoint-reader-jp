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
// The scan is deliberately a separate object rather than inline in the
// conversion loop: phase 2 keeps ruby instead of dropping it, and will reuse
// this exact traversal with a different exit.
class AozoraStrip {
 public:
  struct Stats {
    uint32_t ruby = 0;           // 《…》 removed
    uint32_t range = 0;          // ｜ removed (ruby range marker)
    uint32_t note = 0;           // ［＃…］ removed
    uint32_t gaijiRestored = 0;  // ※［＃…U+xxxx…］ replaced with that codepoint
    uint32_t gaijiGeta = 0;      // ※［＃…］ with no U+ replaced with 〓
    uint32_t headerLines = 0;    // lines dropped with the legend block
  };

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
