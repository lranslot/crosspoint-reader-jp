#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Shared CJK break-opportunity rules. Used by ParsedText (line breaking of body text) and by
// TextBlock (splitting a ruby annotation into the same "characters" for ruby-align: space-around).
//
// ── What came from where ───────────────────────────────────────────────────────
// The four functions in `CjkBreaks` were private statics inside ParsedText.cpp and
// are moved here unchanged, under the names and at the path upstream chose in PR
// #4162, so a later catch-up is a no-op rather than a merge. We did not
// cherry-pick that PR: the rules were already ours, and the only thing it adds is
// an explicit-hyphen test for Western text (`allowsBreakAfterExplicitHyphen`,
// 29 lines) that this fork has no use for. Upstream also has a
// `boundaryAllowsBreak()` that we never had; it is deliberately absent rather
// than stubbed, because an unused function is a thing to keep in step for
// nothing. Everything in the nested `CjkBreaks::jp` namespace is **ours** --
// the kinsoku rules that EPUB never needed and the vertical Japanese reader
// does. Nothing in `jp` is called from the four above, so upstream's half can be
// replaced wholesale without touching ours.
namespace CjkBreaks {

// True when a line break is not allowed before the CJK punctuation codepoint
bool isNoBreakBeforeCjkPunctuation(uint32_t cp);

// True when a line break is not allowed after the CJK punctuation codepoint
bool isNoBreakAfterCjkPunctuation(uint32_t cp);

// True when a line break is allowed between two adjacent codepoints (UAX#14-like CJK rules).
bool hasBreakOpportunityBetween(uint32_t leftCp, uint32_t rightCp);

// Byte offsets inside `text` where it splits into tokens. Empty => the text is a single token.
// Combining marks and no-break punctuation stay attached to their neighbour.
std::vector<size_t> characterBreakByteOffsets(const std::string& text);

// ── Ours: 禁則処理 (kinsoku), for the vertical Japanese reader ────────────────
//
// The character sets come from LineBreakTables.h, generated from the Unicode
// Character Database by scripts/gen_line_break.py. They are not written out by
// hand here for the reason the vertical-orientation table is not: a list of
// punctuation transcribed from a specification is a list with something missing
// from it.
//
// What the generated tables cannot settle is written out explicitly below, with
// the measurement that says so. Three things fall in that gap, and all three are
// in the corpus:
//
//   ‘ ’ “ ”  Line_Break QU (Quotation), because in Unicode these open or close
//            depending on the language. JLReq cl-01/cl-02 assigns them because
//            it is a Japanese document and can.
//   ‐        Line_Break HH (unambiguous hyphen), a break *opportunity* in
//            Unicode's terms. JLReq cl-03 puts it in the line-start prohibition.
//   ― ―      U+2015 is Line_Break AI (ambiguous) and U+2014 is B2. Neither is
//            IN, so the 2-em dash 青空文庫 writes as U+2015 U+2015 is not held
//            together by any class. 青空文庫 uses U+2015 (Shift-JIS 0x815C).
namespace jp {

// Must a line not *begin* with this character? JLReq 3.1.7.
//
// `strict` decides the characters JLReq says some books relax: the prolonged
// sound mark, small kana, and iteration marks -- Line_Break's CJ class exists
// for exactly that distinction, which is why it is a table of its own. Passing
// false allows them at a line start.
//
// One wrinkle worth knowing: Unicode puts 々 (U+3005) in NS, not CJ, so it stays
// prohibited even under the loose rule. JLReq groups it with the relaxable ones.
// We follow Unicode, because the alternative is a hand-maintained exception.
[[nodiscard]] bool prohibitedAtLineStart(uint32_t cp, bool strict);

// Must a line not *end* with this character? JLReq 3.1.8. Opening brackets.
[[nodiscard]] bool prohibitedAtLineEnd(uint32_t cp);

// Must this character not be separated from the one before it? JLReq 3.1.10.
// Covers the ellipsis and two-dot leader (Line_Break IN) and, by the explicit
// rule above, a second U+2015 or U+2014 following the first.
[[nodiscard]] bool inseparableFromPrevious(uint32_t prevCp, uint32_t cp);

}  // namespace jp

}  // namespace CjkBreaks
