#pragma once

#include <Epub/CjkBreaks.h>

#include <cstddef>
#include <cstdint>

// 禁則処理 — moving a line break so it does not fall in a forbidden place.
//
// **Push-out only (追い出し), because squeezing in (追い込み) is not available
// on this device.** Measured over every point size and both writing
// directions: the slack left at the end of a line or column is 16 px at the
// most, against a cell of 17 to 38 px (docs-jp/STATUS.md, "列末の余りの実測").
// There is nowhere to put a character that has been squeezed, so the only
// adjustment left is to push characters out to the next line, which costs no
// space and shortens the line by a cell.
//
// What it does, given a break the wrap loop has already chosen:
//
//   ...日本。|語...     the next line would start with 。    (JLReq 3.1.7)
//   ...日本「|語...     this line would end with 「          (JLReq 3.1.8)
//   ...日本…|…語...     the pair would be split              (JLReq 3.1.10)
//
// In all three the break moves *earlier* by one character, repeatedly, until
// the position is legal. Nothing here ever moves a break later, so the line
// cannot stop fitting.
//
// The one limit is a line that cannot afford it. `minCharsOnLine` is what must
// remain, and when pushing out would go below it the adjustment is abandoned
// whole rather than applied halfway -- a half-pushed break is a break that is
// still illegal, with a shorter line to show for it.
//
// Pure, and takes bytes rather than a renderer, so tools/layout_harness drives
// this exact code rather than a model of it.
namespace kinsoku {

// The characters a line must keep. The same four cells the indent clamp
// guarantees (TxtReaderActivity::MIN_TEXT_CELLS): a line already down to four
// cells of text is not one we improve by making it shorter. At 18 pt a
// horizontal line holds twelve cells, so this is a third of the narrowest line
// the reader can produce.
constexpr size_t MIN_CHARS_ON_LINE = 4;

// Strict reading of JLReq 3.1.7: small kana, the prolonged sound mark and the
// kana iteration marks are prohibited at a line start. The loose reading exists
// in CjkBreaks (the CJ table is separate for exactly that reason) but is not
// offered as a setting yet, so it is pinned here rather than threaded through.
constexpr bool STRICT = true;

// Start of the UTF-8 character ending at `at`.
[[nodiscard]] inline size_t stepBack(const char* s, const size_t at) {
  if (at == 0) return 0;
  size_t i = at - 1;
  while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i--;
  return i;
}

// The codepoint starting at `at`. 0 when `at` is at or past the end.
[[nodiscard]] inline uint32_t codepointAt(const char* s, const size_t len, const size_t at) {
  if (at >= len) return 0;
  const auto b0 = static_cast<unsigned char>(s[at]);
  if (b0 < 0x80) return b0;
  const auto cont = [&](const size_t k) {
    return at + k < len ? static_cast<uint32_t>(static_cast<unsigned char>(s[at + k]) & 0x3F) : 0u;
  };
  if ((b0 & 0xE0) == 0xC0) return (static_cast<uint32_t>(b0 & 0x1F) << 6) | cont(1);
  if ((b0 & 0xF0) == 0xE0) return (static_cast<uint32_t>(b0 & 0x0F) << 12) | (cont(1) << 6) | cont(2);
  if ((b0 & 0xF8) == 0xF0) {
    return (static_cast<uint32_t>(b0 & 0x07) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3);
  }
  return b0;  // a stray continuation byte; treated as itself rather than skipped
}

// Would breaking at `at` violate one of the three rules?
[[nodiscard]] inline bool breakIsForbidden(const char* s, const size_t len, const size_t at) {
  if (at == 0 || at >= len) return false;  // nothing before, or nothing after
  const uint32_t after = codepointAt(s, len, at);
  const uint32_t before = codepointAt(s, len, stepBack(s, at));
  if (CjkBreaks::jp::prohibitedAtLineStart(after, STRICT)) return true;
  if (CjkBreaks::jp::prohibitedAtLineEnd(before)) return true;
  return CjkBreaks::jp::inseparableFromPrevious(before, after);
}

// Characters in [0, at).
[[nodiscard]] inline size_t countChars(const char* s, const size_t at) {
  size_t n = 0;
  for (size_t i = 0; i < at; i++) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) n++;
  }
  return n;
}

// Moves `breakPos` earlier until breaking there is allowed. Returns `breakPos`
// unchanged when it is already legal, or when the adjustment would leave fewer
// than `minChars` characters on the line.
//
// `segLen` is the length of what is still unplaced on this source line, so
// `breakPos == segLen` means the line ends here and there is nothing to push
// into -- the caller's "last visual line" case, left alone.
[[nodiscard]] inline size_t adjustBreak(const char* segment, const size_t segLen, const size_t breakPos,
                                        const size_t minChars = MIN_CHARS_ON_LINE) {
  if (segment == nullptr || breakPos == 0 || breakPos >= segLen) return breakPos;
  if (!breakIsForbidden(segment, segLen, breakPos)) return breakPos;

  size_t at = breakPos;
  size_t chars = countChars(segment, at);
  while (breakIsForbidden(segment, segLen, at)) {
    const size_t prev = stepBack(segment, at);
    if (prev == at) break;  // cannot move further
    at = prev;
    if (chars == 0) break;
    chars--;
    if (chars < minChars) return breakPos;  // too short to pay for it
  }
  // Still forbidden after running out of room: leave the break where the wrap
  // loop put it rather than returning a shorter line that is just as illegal.
  if (breakIsForbidden(segment, segLen, at)) return breakPos;
  return at;
}

}  // namespace kinsoku
