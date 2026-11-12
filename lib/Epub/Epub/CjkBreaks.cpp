#include "CjkBreaks.h"

#include <Utf8.h>

#include "LineBreakTables.h"

// The four functions below were private statics in ParsedText.cpp and are moved
// here verbatim -- only the two names upstream spells differently were changed
// (hasCjkBreakOpportunityBetween -> hasBreakOpportunityBetween,
// cjkCharacterBreakByteOffsets -> characterBreakByteOffsets). The bodies are
// byte for byte what they were, so the EPUB line breaker cannot behave
// differently. See CjkBreaks.h for what is ours and what is upstream's.

namespace CjkBreaks {

bool isNoBreakBeforeCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
      return true;
    default:
      return false;
  }
}

bool isNoBreakAfterCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0x00AB:  // «
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
      return true;
    default:
      return false;
  }
}

bool hasBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunctuation(leftCp) || isNoBreakBeforeCjkPunctuation(rightCp)) return false;
  if (utf8IsCombiningMark(rightCp)) return false;
  return true;
}

std::vector<size_t> characterBreakByteOffsets(const std::string& text) {
  struct CodepointBoundary {
    uint32_t cp;
    size_t endOffset;
  };

  std::vector<CodepointBoundary> codepoints;
  codepoints.reserve(text.size());
  bool hasCjkBreakable = false;

  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  const auto* const start = ptr;
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) break;
    if (utf8IsCjkBreakable(cp)) {
      hasCjkBreakable = true;
    }
    codepoints.push_back({cp, static_cast<size_t>(ptr - start)});
  }

  if (!hasCjkBreakable || codepoints.size() < 2) return {};

  std::vector<size_t> allowedOffsets;
  allowedOffsets.reserve(codepoints.size() - 1);
  for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
    const uint32_t current = codepoints[i].cp;
    const uint32_t next = codepoints[i + 1].cp;
    if (!hasBreakOpportunityBetween(current, next)) continue;
    allowedOffsets.push_back(codepoints[i].endOffset);
  }
  return allowedOffsets;
}

namespace jp {

// ── ★ Where this does NOT follow LineBreak.txt ───────────────────────────────
//
// One table, so regenerating LineBreakTables.h cannot quietly drop any of it.
// The generator prints nothing about these; scripts/gen_line_break.py says so
// next to its regeneration command.
//
// | character        | Line_Break | what we do              | why                |
// |------------------|------------|-------------------------|--------------------|
// | ' U+2018         | QU         | added to line-end       | Unicode will not   |
// | " U+201C         | QU         | prohibition (cl-01)     | say which of open  |
// | ' U+2019         | QU         | added to line-start     | or close these are |
// | " U+201D         | QU         | prohibition (cl-02)     | -- it depends on   |
// |                  |            |                         | the language.      |
// |                  |            |                         | JLReq is Japanese  |
// |                  |            |                         | and can.           |
// | - U+2010 HYPHEN  | HH         | added to line-start     | HH is a break      |
// |                  |            | prohibition (cl-03)     | *opportunity* in   |
// |                  |            |                         | Unicode's terms;   |
// |                  |            |                         | JLReq cl-03 is a   |
// |                  |            |                         | prohibition        |
// | - U+2015 / U+2014| AI / B2    | explicit pair rule in   | neither is IN, so  |
// |                  |            | inseparableFromPrevious | no class holds the |
// |                  |            |                         | 2-em dash together |
//
// And one difference we looked at and did **not** overturn:
//
// | 々 U+3005 | NS | left as Unicode has it | Unicode: always prohibited at a
//                                            line start. JLReq: one of the ones
//                                            books relax. Under the strict rule
//                                            -- the only one shipped today --
//                                            the two agree, so nothing in the
//                                            current output differs. Revisit
//                                            when the loose rule becomes a
//                                            setting; the choice then is between
//                                            following Unicode and keeping a
//                                            hand-maintained exception.
//
// See docs-jp/f8-kinsoku.md for the same table with the measurements behind it.

namespace {

// The characters the generated tables cannot settle, with the reason each one is
// here. See the comment block in CjkBreaks.h.
constexpr uint32_t QUOTE_OPEN[] = {0x2018, 0x201C};   // Line_Break QU
constexpr uint32_t QUOTE_CLOSE[] = {0x2019, 0x201D};  // Line_Break QU
constexpr uint32_t HYPHEN = 0x2010;                   // Line_Break HH
constexpr uint32_t DASH_AOZORA = 0x2015;              // Line_Break AI
constexpr uint32_t DASH_EM = 0x2014;                  // Line_Break B2

bool isOneOf(const uint32_t* list, const size_t count, const uint32_t cp) {
  for (size_t i = 0; i < count; i++) {
    if (list[i] == cp) return true;
  }
  return false;
}

template <size_t N>
constexpr size_t rangeCount(const line_break::Range (&)[N]) {
  return N;
}

}  // namespace

bool prohibitedAtLineStart(const uint32_t cp, const bool strict) {
  if (line_break::inTable(line_break::PROHIBITED_AT_LINE_START, rangeCount(line_break::PROHIBITED_AT_LINE_START), cp)) {
    return true;
  }
  if (strict && line_break::inTable(line_break::CONDITIONAL_AT_LINE_START,
                                    rangeCount(line_break::CONDITIONAL_AT_LINE_START), cp)) {
    return true;
  }
  // Closing quotes and the unambiguous hyphen: JLReq cl-02 and cl-03, which
  // Unicode leaves as QU and HH because they are not Japanese-specific.
  return isOneOf(QUOTE_CLOSE, 2, cp) || cp == HYPHEN;
}

bool prohibitedAtLineEnd(const uint32_t cp) {
  if (line_break::inTable(line_break::PROHIBITED_AT_LINE_END, rangeCount(line_break::PROHIBITED_AT_LINE_END), cp)) {
    return true;
  }
  return isOneOf(QUOTE_OPEN, 2, cp);  // JLReq cl-01, Line_Break QU
}

bool inseparableFromPrevious(const uint32_t prevCp, const uint32_t cp) {
  // A *pair*, not a single character. JLReq 3.1.10 forbids splitting the
  // three-dot leader pair and the two-dot leader pair; it does not forbid a line
  // from starting with one leader, which is ordinary in Japanese text. Testing
  // only `cp` made the leader unable to start a line at all, which pushed breaks
  // back for no reason -- caught by the host test, not by reading.
  if (line_break::inTable(line_break::INSEPARABLE, rangeCount(line_break::INSEPARABLE), cp) &&
      line_break::inTable(line_break::INSEPARABLE, rangeCount(line_break::INSEPARABLE), prevCp)) {
    return true;
  }
  // The 2-em dash: a pair of the same dash, held together by no Unicode class.
  // 青空文庫 writes it as U+2015 U+2015 (Shift-JIS 0x815C twice); U+2014 is the
  // same shape in other sources, so both are matched, but only against
  // themselves -- U+2015 after U+2014 is not a 2-em dash, it is two dashes.
  return (cp == DASH_AOZORA || cp == DASH_EM) && cp == prevCp;
}

}  // namespace jp

}  // namespace CjkBreaks
