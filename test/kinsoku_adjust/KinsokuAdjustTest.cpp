// 禁則処理 as the reader applies it: moving a break that the wrap loop already
// chose. The rules themselves are covered in test/kinsoku_rules; this is about
// the adjustment -- that it only ever moves a break earlier, that it gives up
// rather than half-applying, and that it leaves alone everything it should.
//
// The unit under test is the shipped header, included from src/, not a copy.

#include <gtest/gtest.h>

#include <string>

#include "Kinsoku.h"

namespace {

// Three bytes each, so a break position is a multiple of three and the
// expectations read as character counts.
const std::string KA = "\xE6\x97\xA5";      // 日
const std::string MARU = "\xE3\x80\x82";    // 。
const std::string TEN = "\xE3\x80\x81";     // 、
const std::string OPEN = "\xE3\x80\x8C";    // 「
const std::string CLOSE = "\xE3\x80\x8D";   // 」
const std::string LEADER = "\xE2\x80\xA6";  // …
const std::string DASH = "\xE2\x80\x95";    // ―
const std::string SMALL = "\xE3\x81\x81";   // ぁ
const std::string CHOON = "\xE3\x83\xBC";   // ー

std::string kanji(const size_t n) {
  std::string s;
  for (size_t i = 0; i < n; i++) s += KA;
  return s;
}

// --- it never moves a break later, and never touches a legal one ------------

TEST(KinsokuAdjust, ALegalBreakIsLeftWhereItWas) {
  const std::string text = kanji(20);
  for (size_t at = 3; at < text.size(); at += 3) {
    EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), at), at) << "break at " << at;
  }
}

TEST(KinsokuAdjust, TheBreakOnlyEverMovesEarlier) {
  // Swept over a line built from every interesting character, at every break.
  const std::string text = kanji(4) + MARU + OPEN + kanji(3) + CLOSE + TEN + kanji(4) + LEADER + LEADER + kanji(4);
  for (size_t at = 0; at <= text.size(); at += 3) {
    const size_t got = kinsoku::adjustBreak(text.data(), text.size(), at);
    EXPECT_LE(got, at) << "break at " << at << " moved later";
  }
}

// --- one case per rule ------------------------------------------------------

TEST(KinsokuAdjust, AFullStopIsNotLeftAtTheStartOfTheNextLine) {
  // ...日日日日日 | 。...  ->  ...日日日日 | 日。...
  const std::string text = kanji(6) + MARU + kanji(6);
  const size_t at = 6 * 3;  // the break would put 。 first on the next line
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), at), 5 * 3);
}

TEST(KinsokuAdjust, ACommaIsTreatedTheSameWay) {
  const std::string text = kanji(6) + TEN + kanji(6);
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), 6 * 3), 5 * 3);
}

TEST(KinsokuAdjust, AnOpeningBracketIsNotLeftAtTheEndOfALine) {
  // ...日日日日日「 | 日...  ->  ...日日日日日 | 「日...
  const std::string text = kanji(5) + OPEN + kanji(6);
  const size_t at = 6 * 3;  // 「 is the last character of this line
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), at), 5 * 3);
}

TEST(KinsokuAdjust, AClosingBracketIsNotLeftAtTheStartOfTheNextLine) {
  const std::string text = kanji(6) + CLOSE + kanji(6);
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), 6 * 3), 5 * 3);
}

TEST(KinsokuAdjust, SmallKanaAndTheProlongedMarkMoveUnderTheStrictRule) {
  for (const std::string& ch : {SMALL, CHOON}) {
    const std::string text = kanji(6) + ch + kanji(6);
    EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), 6 * 3), 5 * 3) << ch;
  }
}

TEST(KinsokuAdjust, ALeaderPairIsNotSplit) {
  // ...日日日日日… | …日...  ->  ...日日日日日 | ……日...
  const std::string text = kanji(5) + LEADER + LEADER + kanji(5);
  const size_t at = 6 * 3;  // between the two leaders
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), at), 5 * 3);
}

TEST(KinsokuAdjust, TheTwoEmDashIsNotSplit) {
  const std::string text = kanji(5) + DASH + DASH + kanji(5);
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), 6 * 3), 5 * 3);
}

TEST(KinsokuAdjust, ARunOfForbiddenCharactersIsPushedOutWhole) {
  // ...日日日日日日」。 would need two characters pushed out.
  const std::string text = kanji(6) + CLOSE + MARU + kanji(5);
  // Breaking before 。 is illegal (。 would start the line), and so is breaking
  // before 」, so the break lands two characters earlier.
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), 7 * 3), 5 * 3);
  // Breaking *after* the run is legal and must be left alone: 。 at the end of a
  // line is fine, it is only at a line start that it is not.
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), 8 * 3), 8 * 3);
}

// --- the limits -------------------------------------------------------------

TEST(KinsokuAdjust, ALineTooShortToPayIsLeftAlone) {
  // Four characters must remain. A break at four with 。 after it cannot move
  // without going below that, so the illegal break stands rather than being
  // made worse.
  const std::string text = kanji(4) + MARU + kanji(4);
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), 4 * 3), 4 * 3);
  // One more character of room, and it moves.
  const std::string longer = kanji(5) + MARU + kanji(4);
  EXPECT_EQ(kinsoku::adjustBreak(longer.data(), longer.size(), 5 * 3), 4 * 3);
}

TEST(KinsokuAdjust, AbandonedRatherThanHalfApplied) {
  // Six forbidden characters in a row, on a line that can only give up two.
  const std::string text = kanji(6) + CLOSE + CLOSE + CLOSE + CLOSE + MARU + kanji(3);
  const size_t at = 11 * 3;
  const size_t got = kinsoku::adjustBreak(text.data(), text.size(), at, /*minChars=*/10);
  EXPECT_EQ(got, at) << "a shorter line that is still illegal is not an improvement";
}

TEST(KinsokuAdjust, TheEndOfTheSourceLineIsNotABreak) {
  // breakPos == segLen means the line ends here; there is no next line to push
  // into, so a 。 at the very end is not a violation and nothing moves.
  const std::string text = kanji(5) + MARU;
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), text.size()), text.size());
}

TEST(KinsokuAdjust, BreakAtZeroIsNotABreak) {
  const std::string text = MARU + kanji(5);
  EXPECT_EQ(kinsoku::adjustBreak(text.data(), text.size(), 0), 0u);
}

TEST(KinsokuAdjust, NullAndEmptyAreSafe) {
  EXPECT_EQ(kinsoku::adjustBreak(nullptr, 10, 3), 3u);
  const std::string empty;
  EXPECT_EQ(kinsoku::adjustBreak(empty.data(), 0, 0), 0u);
}

// --- the result is always legal, or the input --------------------------------

TEST(KinsokuAdjust, TheResultIsLegalOrUnchangedForEveryBreakOfAHardLine) {
  // The property the whole-corpus scan checks, in the small: after adjustment a
  // break is either allowed, or exactly the one that came in (the too-short
  // escape hatch). Never a different illegal break.
  const std::string text =
      kanji(3) + MARU + CLOSE + OPEN + kanji(2) + TEN + LEADER + LEADER + SMALL + kanji(3) + DASH + DASH + kanji(3);
  for (size_t at = 0; at <= text.size(); at += 3) {
    const size_t got = kinsoku::adjustBreak(text.data(), text.size(), at);
    const bool legal = !kinsoku::breakIsForbidden(text.data(), text.size(), got);
    EXPECT_TRUE(legal || got == at) << "break " << at << " became " << got;
  }
}

}  // namespace
