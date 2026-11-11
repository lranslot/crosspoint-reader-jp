// Regression tests for the rule that bounds page assembly.
//
// The reader crashed on open because a page was laid out against a viewport that
// had not been measured: pageExtent and normalPitch were both still 0, the
// page-full test `used + pitch > extent` was `0 > 0`, and page assembly admitted
// a visual line per byte of an 8 KB chunk until operator new was asked for 98,304
// bytes and aborted. These tests exist so that hole cannot reopen silently.
//
// The header under test is the shipped one, included from
// src/activities/reader -- there is no copy of the arithmetic here. The oracle
// the equivalence test compares against is the formula as it stood before the
// budget was extracted, written out as a specification rather than lifted from
// the implementation.
#include <gtest/gtest.h>

#include <vector>

#include "PageBudget.h"

namespace {

// The pitches the reader actually produces, measured on the installed family:
// an ordinary line at 12/14/16/18 pt, and the taller pitches a line carrying
// ruby or emphasis dots asks for. See docs-jp/f8-ruby.md.
constexpr int kOrdinaryPitch[] = {32, 37, 42, 49};
constexpr int kRubyPitch[] = {42, 47, 52, 57};
// Portrait and landscape page extents, less the margins the reader reserves.
constexpr int kExtent[] = {464, 764, 440, 800};

// The page-full test as it stood before PageBudget existed, kept here as the
// specification of what must not change. Returns the number of lines admitted.
int admittedByOldRule(const int extent, const std::vector<int>& pitches) {
  int used = 0;
  int count = 0;
  for (const int pitch : pitches) {
    if (count > 0 && used + pitch > extent) break;  // `!outLines.empty() && ...`
    used += pitch;
    count++;
  }
  return count;
}

}  // namespace

// --- the crash ------------------------------------------------------------

TEST(PageBudget, UnmeasuredViewportIsNotUsable) {
  EXPECT_FALSE(PageBudget(0, 0).usable());    // both at their declared defaults
  EXPECT_FALSE(PageBudget(764, 0).usable());  // pitch alone missing
  EXPECT_FALSE(PageBudget(0, 37).usable());   // extent alone missing
  EXPECT_FALSE(PageBudget(-1, -1).usable());
  EXPECT_TRUE(PageBudget(764, 37).usable());
}

TEST(PageBudget, AnUnusableBudgetAdmitsNothingAtAll) {
  PageBudget budget(0, 0);
  // Not even a first line: the "always admit the first line" rule is about a
  // page too short for a tall line, not about a page that has no measurements.
  EXPECT_FALSE(budget.admit(37));
  EXPECT_FALSE(budget.admit(0));
  EXPECT_FALSE(budget.admit(1));
  EXPECT_EQ(budget.count(), 0);
  EXPECT_EQ(budget.used(), 0);
}

TEST(PageBudget, FillingALoopAgainstAnUnmeasuredViewportTerminatesImmediately) {
  // The shape of the loop in loadPageAtOffset(): it runs until the budget says
  // the page is full. Before the fix this never happened and the loop consumed
  // the whole chunk a byte at a time.
  PageBudget budget(0, 0);
  int admitted = 0;
  bool pageFull = false;
  for (int guard = 0; guard < 100000 && !pageFull; guard++) {
    if (budget.admit(37)) {
      admitted++;
    } else {
      pageFull = true;
    }
  }
  EXPECT_TRUE(pageFull);
  EXPECT_EQ(admitted, 0);
}

TEST(PageBudget, ZeroPitchCannotProduceAFreeLine) {
  // The other half of the unbounded loop: a line that costs nothing can be
  // admitted without limit even on a measured page. The floor forbids it.
  PageBudget budget(764, 37);
  int admitted = 0;
  while (budget.admit(0)) {
    admitted++;
    ASSERT_LT(admitted, 10000) << "a zero pitch is still admitting lines";
  }
  EXPECT_LE(admitted, 764);
}

TEST(PageBudget, LineCountIsBoundedByTheExtentForEveryPitch) {
  for (const int extent : kExtent) {
    for (int pitch = 0; pitch <= 64; pitch++) {
      PageBudget budget(extent, 37);
      int admitted = 0;
      while (budget.admit(pitch)) {
        admitted++;
        ASSERT_LE(admitted, extent) << "extent " << extent << " pitch " << pitch;
      }
      EXPECT_GE(admitted, 1) << "extent " << extent << " pitch " << pitch;
    }
  }
}

TEST(PageBudget, LinesAtFloorDoesNotDivideByZero) {
  EXPECT_EQ(PageBudget(764, 0).linesAtFloor(), 0);
  EXPECT_EQ(PageBudget(0, 0).linesAtFloor(), 0);
  EXPECT_EQ(PageBudget(764, 37).linesAtFloor(), 764 / 37);
}

// --- no page boundary moves ----------------------------------------------

TEST(PageBudget, MatchesTheOldRuleForEveryUniformPitch) {
  for (const int extent : kExtent) {
    for (int pitch = 1; pitch <= 96; pitch++) {
      const std::vector<int> pitches(400, pitch);
      PageBudget budget(extent, pitch);
      int admitted = 0;
      while (admitted < static_cast<int>(pitches.size()) && budget.admit(pitch)) {
        admitted++;
      }
      ASSERT_EQ(admitted, admittedByOldRule(extent, pitches)) << "extent " << extent << " pitch " << pitch;
    }
  }
}

TEST(PageBudget, MatchesTheOldRuleForMixedOrdinaryAndRubyLines) {
  // Every arrangement of ordinary and taller lines that fits in 14 lines, at
  // every point size and page extent: 4 extents * 4 sizes * 16,383 patterns.
  // A ruby or emphasis line costs more than its neighbours, so this is where an
  // off-by-one in the budget would move a page boundary.
  for (const int extent : kExtent) {
    for (size_t size = 0; size < std::size(kOrdinaryPitch); size++) {
      const int ordinary = kOrdinaryPitch[size];
      const int tall = kRubyPitch[size];
      for (unsigned pattern = 1; pattern < (1u << 14); pattern++) {
        std::vector<int> pitches;
        pitches.reserve(14);
        for (unsigned bit = 0; bit < 14; bit++) {
          pitches.push_back((pattern >> bit) & 1u ? tall : ordinary);
        }
        PageBudget budget(extent, ordinary);
        int admitted = 0;
        for (const int pitch : pitches) {
          if (!budget.admit(pitch)) break;
          admitted++;
        }
        ASSERT_EQ(admitted, admittedByOldRule(extent, pitches))
            << "extent " << extent << " size " << size << " pattern " << pattern;
      }
    }
  }
}

TEST(PageBudget, ALineTallerThanThePageStillMakesProgress) {
  // Otherwise the reader stops at that line for good.
  PageBudget budget(100, 37);
  EXPECT_TRUE(budget.admit(400));
  EXPECT_EQ(budget.count(), 1);
  EXPECT_FALSE(budget.admit(37));
}

TEST(PageBudget, UsedAndCountTrackWhatWasCharged) {
  PageBudget budget(764, 37);
  EXPECT_TRUE(budget.admit(37));
  EXPECT_TRUE(budget.admit(47));
  EXPECT_EQ(budget.count(), 2);
  EXPECT_EQ(budget.used(), 84);
}
