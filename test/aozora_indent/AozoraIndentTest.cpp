// The defect this covers, in one sentence: a page that begins in the middle of
// an indented 青空文庫 line used to lay that line out at the margin, because the
// ［＃N字下げ］ that says otherwise is back on the previous page.
//
// Measured on the device before the fix (page, then the indent of each visual
// column in cells):
//   p35  0 0 1 1 1 1     a one-cell paragraph; continuations correct
//   p36  0 2 2 2 2 1 3   the first column is the continuation of p35's last
//   p37  0 0 0 0 1 3 0   four columns of a three-cell paragraph at zero
// Indents *inside* a page were right throughout, which is why this survived
// every check stage 2g shipped with.
//
// The units under test are the shipped ones, included from src/, not copies.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "AozoraIndent.h"

namespace {

// The notation as AozoraStrip writes it, built from the shipped byte spellings
// so the test cannot drift away from the converter.
std::string notation(const int indent, const int hanging = 0) {
  const auto fullWidthDigits = [](const int v) {
    std::string out;
    for (const char c : std::to_string(v)) {
      out += "\xEF\xBC";
      out += static_cast<char>(0x90 + (c - '0'));
    }
    return out;
  };
  std::string out(aozora::NOTE_OPEN, aozora::NOTE_OPEN_LEN);
  out += fullWidthDigits(indent);
  out.append(aozora::JISAGE, aozora::JISAGE_LEN);
  if (hanging > 0) {
    out.append(aozora::ORIKAESHI, aozora::ORIKAESHI_LEN);
    out += fullWidthDigits(hanging);
    out.append(aozora::JISAGE, aozora::JISAGE_LEN);
  }
  out.append(aozora::NOTE_CLOSE, aozora::NOTE_CLOSE_LEN);
  return out;
}

// A run of CJK text long enough that a page breaks inside it.
std::string body(const size_t chars) {
  std::string out;
  for (size_t i = 0; i < chars; i++) out += "\xE6\x97\xA5";  // 日
  return out;
}

// One source line as it appears in content.u8, with the cells a continuation of
// it owes and the bytes its notation occupies.
//
// `prefixLen` matters because it bounds where a page can begin inside the line.
// The fill loop starts laying out at `prefixLen` and breaks on a codepoint
// boundary after that, so a page start before the end of the notation is not a
// position the reader can ever ask about -- and asking anyway is just asking
// the parser to read a notation it has only been given half of.
struct Line {
  int carryCells;
  size_t prefixLen;
  std::string text;
};

struct Corpus {
  std::string bytes;
  std::vector<size_t> starts;
  std::vector<Line> lines;
};

Corpus build(std::vector<Line> lines) {
  Corpus c;
  c.lines = std::move(lines);
  for (const auto& l : c.lines) {
    c.starts.push_back(c.bytes.size());
    c.bytes += l.text;
    c.bytes += '\n';
  }
  return c;
}

Corpus defaultCorpus() {
  const std::string n2 = notation(2);
  const std::string n31 = notation(3, 1);
  const std::string n19 = notation(19);
  const std::string n7 = notation(7);
  return build({
      {0, 0, body(400)},                  // no notation at all
      {2, n2.size(), n2 + body(400)},     // continuations line up under the text
      {1, n31.size(), n31 + body(400)},   // an explicit hanging indent
      {19, n19.size(), n19 + body(400)},  // the fake right-align form
      {0, 0, body(3)},                    // too short to break inside
      {7, n7.size(), n7 + body(400)},
  });
}

// Results carryAt() can give that are not a cell count.
constexpr int AT_LINE_HEAD = -1;
constexpr int HEAD_OUT_OF_REACH = -2;

// What the reader does at a page start, with the file I/O taken out: hand it a
// window of bytes ending exactly at the page start and let the shipped units
// decide.
int carryAt(const Corpus& c, const size_t pageStart, const size_t maxBack) {
  const size_t windowStart = pageStart > maxBack ? pageStart - maxBack : 0;
  const size_t windowLen = pageStart - windowStart;
  const size_t found = aozora::findSourceLineStart(c.bytes.data() + windowStart, windowLen, windowStart == 0);
  if (found == windowLen) return AT_LINE_HEAD;
  if (found == aozora::NO_LINE_START) return HEAD_OUT_OF_REACH;
  const size_t lineStart = windowStart + found;
  uint8_t indent = 0;
  uint8_t hanging = 0;
  if (aozora::parseIndentNotation(c.bytes.data() + lineStart, pageStart - lineStart, indent, hanging) == 0) {
    return 0;
  }
  return aozora::carryCells(indent, hanging);
}

// What TxtReaderActivity::recoverIndentCarry() is willing to read back: 32 KB,
// in 4 KB steps. The corpus's longest line is 2,845 bytes, so one step resolves
// every real paragraph; the figure here has to track the reader's or this stops
// describing it.
constexpr size_t kMaxBack = 32 * 1024;

// --- the defect -------------------------------------------------------------

TEST(AozoraIndent, EveryOffsetInsideAnIndentedLineCarriesThatLinesHangingIndent) {
  const Corpus c = defaultCorpus();
  for (size_t li = 0; li < c.lines.size(); li++) {
    const size_t start = c.starts[li];
    const size_t len = c.lines[li].text.size();
    // Every byte of the line's text is a possible page start. The reader only
    // ever breaks on a codepoint boundary, but the recovery must not depend on
    // that, so every byte is tried and not just every third one.
    for (size_t at = start + std::max<size_t>(c.lines[li].prefixLen, 1); at < start + len; at++) {
      EXPECT_EQ(carryAt(c, at, kMaxBack), c.lines[li].carryCells)
          << "line " << li << ", " << (at - start) << " bytes into it";
    }
  }
}

TEST(AozoraIndent, APageThatBeginsAtALineHeadCarriesNothing) {
  const Corpus c = defaultCorpus();
  for (const size_t start : c.starts) {
    if (start == 0) continue;  // offset 0 never reaches the recovery at all
    EXPECT_EQ(carryAt(c, start, kMaxBack), AT_LINE_HEAD) << "line head at " << start;
  }
}

TEST(AozoraIndent, AHeadOutOfReachIsReportedAndNotGuessedAt) {
  const Corpus c = defaultCorpus();
  const size_t deepInside = c.starts[1] + c.lines[1].text.size() - 1;
  // Asked to look back only a few bytes, the scan must say it does not know
  // rather than read whatever it can see as a line head.
  EXPECT_EQ(carryAt(c, deepInside, 8), HEAD_OUT_OF_REACH);
  // Given room it finds the same answer as always.
  EXPECT_EQ(carryAt(c, deepInside, kMaxBack), c.lines[1].carryCells);
}

TEST(AozoraIndent, TheFirstLineOfTheFileIsALineHeadWithNoNewlineBehindIt) {
  // There is no '\n' to find, and the start of the file is a line head all the
  // same. The first line of the default corpus asks for no indent, so a page
  // starting inside it carries nothing -- but it must carry nothing *because
  // the line head said so*, not because the scan gave up.
  const Corpus c = defaultCorpus();
  EXPECT_EQ(carryAt(c, 40, kMaxBack), 0);
  EXPECT_EQ(aozora::findSourceLineStart(c.bytes.data(), 40, /*atFileStart=*/true), 0u);
  EXPECT_EQ(aozora::findSourceLineStart(c.bytes.data(), 40, /*atFileStart=*/false), aozora::NO_LINE_START);
  // An indented first line is carried from byte 0 just like any other.
  const std::string n = notation(6);
  const Corpus indented = build({{6, n.size(), n + body(400)}});
  EXPECT_EQ(carryAt(indented, 300, kMaxBack), 6);
}

TEST(AozoraIndent, ASingleIndentedLineFillingTheWholeFileIsCarriedFromItsHead) {
  // The shape that made the device drop four columns in a row: one paragraph
  // long enough to span several pages. 2,000 characters is 6 KB, twice the
  // longest line in the corpus (2,845 bytes, measured with tools/layout_harness)
  // and still inside a single 4 KB look-back step.
  const std::string n = notation(4);
  const Corpus c = build({{4, n.size(), n + body(2000)}});
  const size_t len = c.lines[0].text.size();
  for (size_t at = n.size(); at < len; at++) {
    EXPECT_EQ(carryAt(c, at, kMaxBack), 4) << at << " bytes in";
  }
}

TEST(AozoraIndent, WindowLenMeansTheLineStartsWhereTheWindowEnds) {
  // The reader walks back in 512-byte steps, so it calls this many times for one
  // page and the answer `windowLen` turns up in two different situations: on the
  // first step it is the page start itself (a line head, nothing to carry), on a
  // later step it is just a newline that happens to sit on a step boundary. The
  // reader tells them apart by which step it is on; this pins down what the
  // answer itself means, which is the same thing in both.
  const std::string text = "\xE6\x97\xA5\n\xE6\x97\xA5\xE6\x97\xA5";  // 日 \n 日日
  EXPECT_EQ(aozora::findSourceLineStart(text.data(), 4, /*atFileStart=*/true), 4u);
  EXPECT_EQ(aozora::findSourceLineStart(text.data(), 4, /*atFileStart=*/false), 4u);
  // One byte further on, the same bytes name the line start inside the window.
  EXPECT_EQ(aozora::findSourceLineStart(text.data(), 5, /*atFileStart=*/false), 4u);
}

TEST(AozoraIndent, AParagraphLongerThanTheLookBackLosesItsIndentAndSaysSo) {
  // The one case the fix does not cover, stated as a test so it cannot be
  // mistaken for a case that works. The reader counts these and puts the count
  // on the card as carrylost=N rather than quietly indenting at the margin.
  const std::string n = notation(4);
  const Corpus c = build({{4, n.size(), n + body(14000)}});  // 42 KB in one line
  EXPECT_EQ(carryAt(c, 20000, kMaxBack), 4);                 // still within reach
  EXPECT_EQ(carryAt(c, 41000, kMaxBack), HEAD_OUT_OF_REACH);
}

// --- the parser, which moved out of the activity unchanged ------------------

TEST(AozoraIndent, ParsesBothFormsAndReportsTheBytesTheyOccupy) {
  uint8_t indent = 0;
  uint8_t hanging = 0;
  const std::string a = notation(2);
  EXPECT_EQ(aozora::parseIndentNotation(a.data(), a.size(), indent, hanging), a.size());
  EXPECT_EQ(indent, 2);
  EXPECT_EQ(hanging, 0);

  const std::string b = notation(3, 1);
  EXPECT_EQ(aozora::parseIndentNotation(b.data(), b.size(), indent, hanging), b.size());
  EXPECT_EQ(indent, 3);
  EXPECT_EQ(hanging, 1);

  const std::string d = notation(19);
  EXPECT_EQ(aozora::parseIndentNotation(d.data(), d.size(), indent, hanging), d.size());
  EXPECT_EQ(indent, 19);
}

TEST(AozoraIndent, DoesNotReadTwoNumbersAsOne) {
  // ［＃１字下げ、折り返して２字下げ］ is 1 and 2, never 12. This is the mistake the
  // F-8 survey script made, and the reason the digits are read positionally.
  uint8_t indent = 0;
  uint8_t hanging = 0;
  const std::string n = notation(1, 2);
  ASSERT_GT(aozora::parseIndentNotation(n.data(), n.size(), indent, hanging), 0u);
  EXPECT_EQ(indent, 1);
  EXPECT_EQ(hanging, 2);
  EXPECT_EQ(aozora::carryCells(indent, hanging), 2);
}

TEST(AozoraIndent, RejectsAnythingThatIsNotTheNotation) {
  uint8_t indent = 0;
  uint8_t hanging = 0;
  const std::string full = notation(2);
  const std::string cases[] = {
      "",
      body(3),                                                // ordinary text
      std::string(aozora::NOTE_OPEN, aozora::NOTE_OPEN_LEN),  // an opener alone
      std::string(aozora::NOTE_OPEN, aozora::NOTE_OPEN_LEN) + std::string(aozora::JISAGE, aozora::JISAGE_LEN),
      full.substr(0, full.size() - 1),  // truncated closer
      body(1) + full,                   // not at the head of the line
  };
  for (const auto& s : cases) {
    EXPECT_EQ(aozora::parseIndentNotation(s.data(), s.size(), indent, hanging), 0u);
    EXPECT_EQ(indent, 0);
    EXPECT_EQ(hanging, 0);
  }
  EXPECT_EQ(aozora::parseIndentNotation(nullptr, 0, indent, hanging), 0u);
}

TEST(AozoraIndent, CarryCellsPrefersTheHangingNumberWhenThereIsOne) {
  EXPECT_EQ(aozora::carryCells(0, 0), 0);
  EXPECT_EQ(aozora::carryCells(5, 0), 5);  // no 折り返して: line up under the text
  EXPECT_EQ(aozora::carryCells(5, 2), 2);
  EXPECT_EQ(aozora::carryCells(0, 2), 2);
}

// --- the thing that must not move -------------------------------------------

TEST(AozoraIndent, AFileWithNoNotationCarriesNothingAnywhere) {
  // The equivalence check in the small: a book with no indent notation must
  // behave exactly as it did before any of this existed, at every offset. The
  // page-boundary equivalence itself is checked on the host harness against the
  // whole corpus; this is the part that can be asserted in one line.
  const Corpus c = build({{0, 0, body(500)}, {0, 0, body(500)}, {0, 0, body(1)}});
  for (size_t at = 1; at < c.bytes.size(); at++) {
    const int got = carryAt(c, at, kMaxBack);
    EXPECT_LE(got, 0) << "offset " << at << " asked for " << got << " cells";
  }
}

}  // namespace
