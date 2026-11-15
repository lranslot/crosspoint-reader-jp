// CssLength stores its magnitude as a fixed-point int16_t rather than a float,
// to cut the cost of holding a book's stylesheet (8 bytes to 4 per length, 104
// to 56 per CssStyle, 136 to 88 per retained rule).
//
// This is the control for that change, and it exists because the control we
// wanted does not: of the five EPUBs on the test card, one declares no CSS at
// all, two declare only `margin: 0` and `padding: 0` (zero quantises exactly,
// so no rendering difference is even possible), and the only two that declare
// non-zero lengths are the two that currently cannot be rendered -- so there is
// no "before" image to compare a quantised "after" against.
//
// What can be checked is the arithmetic, over the values the real books
// actually contain. Every margin, padding, text-indent, height and width
// declaration in those five books was extracted: 963 percentages from 5 to
// 100, 1,668 em values from 0.1 to 40, four px values of 1, and 301 zeroes.
// The largest magnitude anywhere is 100, and the em values are written to one
// decimal place. The cases below pin the ends of that range, the representable
// limits, the rounding behaviour, and -- the one that carries the claim -- that
// the pixel figure layout consumes is unchanged.

#include <gtest/gtest.h>

#include <cmath>

#include "Epub/css/CssStyle.h"

namespace {

constexpr float kTolerance = 1.0f / (2 * CssLength::FRACTION);  // half a step

// --- The range the real books occupy -----------------------------------------

TEST(CssLength, ZeroIsExact) {
  const CssLength len(0.0f, CssUnit::Em);
  EXPECT_EQ(len.raw, 0);
  EXPECT_FLOAT_EQ(len.value(), 0.0f);
}

TEST(CssLength, LargestValueSeenInRealBooks) {
  // 100%, the largest magnitude declared anywhere in the five test books.
  const CssLength len(100.0f, CssUnit::Percent);
  EXPECT_NEAR(len.value(), 100.0f, kTolerance);
}

TEST(CssLength, LargestEmSeenInRealBooks) {
  const CssLength len(40.0f, CssUnit::Em);
  EXPECT_NEAR(len.value(), 40.0f, kTolerance);
}

TEST(CssLength, SmallestNonZeroEmSeenInRealBooks) {
  // 0.1em. Not representable exactly at 1/64; it must land within half a step,
  // which at a 25 px em is 0.2 px -- below the whole-pixel rounding that
  // toPixelsInt16 applies afterwards.
  const CssLength len(0.1f, CssUnit::Em);
  EXPECT_NEAR(len.value(), 0.1f, kTolerance);
}

TEST(CssLength, EveryTenthOfAnEmUpToFortyStaysWithinHalfAStep) {
  // The em values in the books are written to one decimal place. Sweeping the
  // whole observed range at that granularity is the broadest statement that
  // can be made about them.
  for (int tenths = 0; tenths <= 400; tenths++) {
    const float declared = static_cast<float>(tenths) / 10.0f;
    const CssLength len(declared, CssUnit::Em);
    EXPECT_NEAR(len.value(), declared, kTolerance) << "declared " << declared << "em";
  }
}

TEST(CssLength, WholePercentagesAreExact) {
  // A whole number of percent is a multiple of 1, and 1 is a multiple of 1/64,
  // so these must come back bit-exact rather than merely close.
  for (int pct = 0; pct <= 100; pct++) {
    const CssLength len(static_cast<float>(pct), CssUnit::Percent);
    EXPECT_FLOAT_EQ(len.value(), static_cast<float>(pct)) << "declared " << pct << "%";
  }
}

// --- The ends of what the type can represent ---------------------------------

TEST(CssLength, RepresentableLimitIsLargerThanAnyUsefulLength) {
  // +-409.59 at 1/80. Not larger than the panel's 800 px height, which is the
  // price paid for exactness (see the note in CssStyle.h); it is larger than
  // the 464 px viewport width, and a block with a 409 px margin against a
  // 464 px viewport is already off the page, so saturating there cannot change
  // a visible outcome. No book on the test card declares a px length above 1.
  constexpr float limit = 32767.0f / CssLength::FRACTION;
  EXPECT_GT(limit, 464.0f);
}

TEST(CssLength, SaturatesRatherThanWrapping) {
  // Wrapping would be the dangerous failure: a huge margin becoming a small or
  // negative one would move text rather than push it off the page.
  const CssLength huge(100000.0f, CssUnit::Pixels);
  EXPECT_EQ(huge.raw, 32767);
  EXPECT_GT(huge.value(), 800.0f);

  const CssLength hugeNegative(-100000.0f, CssUnit::Pixels);
  EXPECT_EQ(hugeNegative.raw, -32768);
  EXPECT_LT(hugeNegative.value(), -800.0f);
}

TEST(CssLength, NegativeValuesRoundAwayFromZeroSymmetrically) {
  // text-indent is legitimately negative (a hanging indent), so the negative
  // side has to behave like the positive one rather than truncating towards 0.
  const CssLength positive(1.5f, CssUnit::Em);
  const CssLength negative(-1.5f, CssUnit::Em);
  EXPECT_EQ(positive.raw, -negative.raw);
  EXPECT_NEAR(negative.value(), -1.5f, kTolerance);
}

// --- Resolution to pixels, which is what layout actually consumes ------------

TEST(CssLength, EmResolvesAgainstTheEmSize) {
  const CssLength len(2.0f, CssUnit::Em);
  EXPECT_FLOAT_EQ(len.toPixels(25.0f), 50.0f);
}

TEST(CssLength, PercentResolvesAgainstTheContainer) {
  const CssLength len(50.0f, CssUnit::Percent);
  EXPECT_FLOAT_EQ(len.toPixels(25.0f, 464.0f), 232.0f);
}

TEST(CssLength, PercentIsNotResolvableWithoutAContainer) {
  const CssLength len(50.0f, CssUnit::Percent);
  EXPECT_FALSE(len.isResolvable(0));
  EXPECT_TRUE(len.isResolvable(464.0f));
}

TEST(CssLength, QuantisationDoesNotMoveASinglePixel) {
  // The claim the whole change rests on, stated exactly rather than loosely:
  // for every em value the books declare, at every font size the reader
  // offers, the pixel figure is *identical* to what the float produced. Not
  // within a pixel -- identical. 2,406 combinations.
  //
  // This is why FRACTION is 80 and not 64. At 64 this test fails 159 times.
  constexpr float emSizes[] = {17.0f, 21.0f, 25.0f, 29.0f, 33.0f, 38.0f};
  for (const float em : emSizes) {
    for (int tenths = 0; tenths <= 400; tenths++) {
      const float declared = static_cast<float>(tenths) / 10.0f;
      const int16_t viaFloat = static_cast<int16_t>(declared * em);
      const int16_t viaFixed = CssLength(declared, CssUnit::Em).toPixelsInt16(em);
      EXPECT_EQ(viaFloat, viaFixed) << declared << "em at em=" << em;
    }
  }
}

TEST(CssLength, WholePercentagesResolveIdenticallyToTheFloat) {
  // The other unit that occurs in quantity: 963 declarations, all whole
  // percentages, resolved against the 464 px viewport.
  for (int pct = 0; pct <= 100; pct++) {
    const float declared = static_cast<float>(pct);
    const int16_t viaFloat = static_cast<int16_t>(declared * 464.0f / 100.0f);
    const int16_t viaFixed = CssLength(declared, CssUnit::Percent).toPixelsInt16(25.0f, 464.0f);
    EXPECT_EQ(viaFloat, viaFixed) << declared << "% of 464px";
  }
}

// --- The size claim itself ---------------------------------------------------

TEST(CssLength, IsFourBytes) {
  // If this grows, the per-rule cost and therefore MAX_RULES in CssParser.cpp
  // are both wrong. RULE_BYTES there is 88, measured with this at 4.
  EXPECT_EQ(sizeof(CssLength), 4u);
}

TEST(CssStyleSize, IsFiftySixBytes) {
  EXPECT_EQ(sizeof(CssStyle), 56u);
}

}  // namespace
