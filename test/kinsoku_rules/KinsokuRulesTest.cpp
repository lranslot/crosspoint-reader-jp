// The character sets for 禁則処理, checked against the shipped tables.
//
// Why this exists even though the tables are generated: a generator is only as
// good as the classes it was told to keep, and three groups of characters fall
// outside those classes entirely (the Japanese quotation marks, the unambiguous
// hyphen, and the 2-em dash). Those are written out by hand in CjkBreaks.cpp,
// which makes them exactly the kind of hand-written list that goes stale. Here
// they are pinned to the examples JLReq itself gives.
//
// Sources, both opened rather than remembered:
//   https://www.unicode.org/Public/UCD/latest/ucd/LineBreak.txt
//     LineBreak-18.0.0.txt, 2026-08-07
//   https://www.w3.org/TR/jlreq/  sections 3.1.7, 3.1.8, 3.1.10, 3.9.2

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "CjkBreaks.h"

namespace {

using CjkBreaks::jp::inseparableFromPrevious;
using CjkBreaks::jp::prohibitedAtLineEnd;
using CjkBreaks::jp::prohibitedAtLineStart;

constexpr bool STRICT = true;
constexpr bool LOOSE = false;

// --- one case per class, from JLReq 3.9.2's own examples --------------------

TEST(KinsokuRules, ClosingBracketsMayNotStartALine) {
  // cl-02. Line_Break CL / CP.
  for (const uint32_t cp : {0x3009u, 0x300Bu, 0x300Du, 0x300Fu, 0x3011u, 0x3015u, 0xFF09u, 0xFF3Du, 0xFF5Du, 0x0029u,
                            0x005Du, 0x007Du}) {
    EXPECT_TRUE(prohibitedAtLineStart(cp, STRICT)) << std::hex << "U+" << cp;
    EXPECT_TRUE(prohibitedAtLineStart(cp, LOOSE)) << std::hex << "U+" << cp << " must hold under the loose rule too";
  }
}

TEST(KinsokuRules, ClosingQuotesMayNotStartALine) {
  // cl-02, and Line_Break QU -- not in any kept table, so this is the
  // hand-written half of the rule.
  EXPECT_TRUE(prohibitedAtLineStart(0x2019, STRICT)) << "RIGHT SINGLE QUOTATION MARK";
  EXPECT_TRUE(prohibitedAtLineStart(0x201D, STRICT)) << "RIGHT DOUBLE QUOTATION MARK";
  EXPECT_TRUE(prohibitedAtLineStart(0x2019, LOOSE));
  EXPECT_TRUE(prohibitedAtLineStart(0x201D, LOOSE));
}

TEST(KinsokuRules, HyphensAndWaveDashMayNotStartALine) {
  // cl-03. 〜 is Line_Break NS and comes from the table; ‐ is HH and does not.
  EXPECT_TRUE(prohibitedAtLineStart(0x301C, STRICT)) << "WAVE DASH";
  EXPECT_TRUE(prohibitedAtLineStart(0x2010, STRICT)) << "HYPHEN, the hand-written half";
}

TEST(KinsokuRules, DividingPunctuationMayNotStartALine) {
  EXPECT_TRUE(prohibitedAtLineStart(0xFF01, STRICT)) << "！";
  EXPECT_TRUE(prohibitedAtLineStart(0xFF1F, STRICT)) << "？";
}

TEST(KinsokuRules, MiddleDotsMayNotStartALine) {
  // cl-05: ・ ： ；, all Line_Break NS.
  EXPECT_TRUE(prohibitedAtLineStart(0x30FB, STRICT)) << "KATAKANA MIDDLE DOT";
  EXPECT_TRUE(prohibitedAtLineStart(0xFF1A, STRICT)) << "：";
  EXPECT_TRUE(prohibitedAtLineStart(0xFF1B, STRICT)) << "；";
}

TEST(KinsokuRules, FullStopsAndCommasMayNotStartALine) {
  for (const uint32_t cp : {0x3002u, 0xFF0Eu, 0x3001u, 0xFF0Cu}) {
    EXPECT_TRUE(prohibitedAtLineStart(cp, STRICT)) << std::hex << "U+" << cp;
  }
}

TEST(KinsokuRules, OpeningBracketsMayNotEndALine) {
  // cl-01. Line_Break OP, plus the two opening quotes by hand.
  for (const uint32_t cp : {0x3008u, 0x300Au, 0x300Cu, 0x300Eu, 0x3010u, 0x3014u, 0xFF08u, 0xFF3Bu, 0xFF5Bu, 0x0028u,
                            0x005Bu, 0x007Bu}) {
    EXPECT_TRUE(prohibitedAtLineEnd(cp)) << std::hex << "U+" << cp;
  }
  EXPECT_TRUE(prohibitedAtLineEnd(0x2018)) << "LEFT SINGLE QUOTATION MARK";
  EXPECT_TRUE(prohibitedAtLineEnd(0x201C)) << "LEFT DOUBLE QUOTATION MARK";
}

// --- the strict / loose distinction, which is the whole reason CJ is separate -

TEST(KinsokuRules, SmallKanaAndTheProlongedMarkAreTheRelaxableOnes) {
  // Line_Break CJ. Prohibited under the strict rule, allowed under the loose one.
  for (const uint32_t cp : {0x3041u, 0x3043u, 0x3045u, 0x3063u, 0x3083u, 0x30A1u, 0x30C3u, 0x30FCu}) {
    EXPECT_TRUE(prohibitedAtLineStart(cp, STRICT)) << std::hex << "U+" << cp << " strict";
    EXPECT_FALSE(prohibitedAtLineStart(cp, LOOSE)) << std::hex << "U+" << cp << " loose";
  }
}

TEST(KinsokuRules, TheIterationMarkStaysProhibitedEvenUnderTheLooseRule) {
  // 々 is Line_Break NS, not CJ, so Unicode keeps it prohibited where JLReq
  // groups it with the relaxable ones. Pinned here so the divergence is a
  // decision on the record rather than a surprise.
  EXPECT_TRUE(prohibitedAtLineStart(0x3005, STRICT)) << "々 strict";
  EXPECT_TRUE(prohibitedAtLineStart(0x3005, LOOSE)) << "々 loose -- NS, not CJ";
  // The kana iteration marks are CJ and do relax.
  for (const uint32_t cp : {0x309Du, 0x309Eu, 0x30FDu, 0x30FEu}) {
    EXPECT_TRUE(prohibitedAtLineStart(cp, STRICT)) << std::hex << "U+" << cp;
  }
}

// --- inseparable -----------------------------------------------------------

TEST(KinsokuRules, LeadersAreInseparableFromWhatPrecedesThem) {
  // Line_Break IN, so the table covers them whatever came before.
  EXPECT_TRUE(inseparableFromPrevious(0x3042, 0x2026)) << "…";
  EXPECT_TRUE(inseparableFromPrevious(0x2026, 0x2026)) << "……";
  EXPECT_TRUE(inseparableFromPrevious(0x3042, 0x2025)) << "‥";
}

TEST(KinsokuRules, TheTwoEmDashIsHeldTogetherOnlyAgainstItself) {
  // Neither U+2015 (Line_Break AI) nor U+2014 (B2) is IN, so this rule is
  // entirely hand-written. 青空文庫 writes the 2-em dash as U+2015 twice.
  EXPECT_TRUE(inseparableFromPrevious(0x2015, 0x2015)) << "aozora 2-em dash";
  EXPECT_TRUE(inseparableFromPrevious(0x2014, 0x2014)) << "em dash pair";
  // A single dash after ordinary text is breakable, and two different dashes
  // are two dashes, not a pair.
  EXPECT_FALSE(inseparableFromPrevious(0x3042, 0x2015)) << "one dash after kana";
  EXPECT_FALSE(inseparableFromPrevious(0x2014, 0x2015)) << "mixed pair is not a 2-em dash";
  EXPECT_FALSE(inseparableFromPrevious(0x2015, 0x2014));
}

// --- what must stay breakable ----------------------------------------------

TEST(KinsokuRules, OrdinaryTextIsNeitherProhibitedNorInseparable) {
  // The negative side matters as much: a rule that fires on ordinary kanji
  // would push every line.
  for (const uint32_t cp : {0x65E5u, 0x672Cu, 0x3042u, 0x30A2u, 0x0041u, 0x0061u, 0x0030u, 0xFF21u}) {
    EXPECT_FALSE(prohibitedAtLineStart(cp, STRICT)) << std::hex << "U+" << cp;
    EXPECT_FALSE(prohibitedAtLineEnd(cp)) << std::hex << "U+" << cp;
    EXPECT_FALSE(inseparableFromPrevious(0x65E5, cp)) << std::hex << "U+" << cp;
  }
}

TEST(KinsokuRules, AClosingBracketIsNotAlsoProhibitedAtTheLineEnd) {
  // The two directions are different sets; mixing them would stall a line that
  // both ends and starts with punctuation.
  EXPECT_TRUE(prohibitedAtLineStart(0x300D, STRICT));
  EXPECT_FALSE(prohibitedAtLineEnd(0x300D));
  EXPECT_TRUE(prohibitedAtLineEnd(0x300C));
  EXPECT_FALSE(prohibitedAtLineStart(0x300C, STRICT));
}

// --- the move itself -------------------------------------------------------

TEST(KinsokuRules, TheUpstreamHalfStillAnswersTheSameWay) {
  // These four came out of ParsedText.cpp byte for byte. A spot check that they
  // are reachable and still say what the EPUB line breaker relies on.
  EXPECT_TRUE(CjkBreaks::isNoBreakBeforeCjkPunctuation(0x3002)) << "。";
  EXPECT_TRUE(CjkBreaks::isNoBreakAfterCjkPunctuation(0x300C)) << "「";
  EXPECT_FALSE(CjkBreaks::isNoBreakBeforeCjkPunctuation(0x65E5)) << "日";
  EXPECT_FALSE(CjkBreaks::hasBreakOpportunityBetween(0x65E5, 0x3002)) << "no break before 。";
  EXPECT_TRUE(CjkBreaks::hasBreakOpportunityBetween(0x65E5, 0x672C)) << "between two kanji";
}

}  // namespace
