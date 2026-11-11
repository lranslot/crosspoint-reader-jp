// What this covers, and why it is a class and not three members of the reader:
// the rule "a press that arrives while the screen is being drawn is counted,
// not acted on" has one property that matters more than the rest -- **it must
// add no delay to the first press**. A design that waits to see whether a second
// press is coming makes every ordinary page turn feel slower, which is a worse
// reader than the one we started with. That property is a property of ready(),
// so it can be pinned down here instead of being felt for on the device.
//
// The unit under test is the shipped header, included from src/, not a copy.

#include "PageTurnQueue.h"

#include <gtest/gtest.h>

namespace {

// --- the property that must not be lost ------------------------------------

TEST(PageTurnQueue, AFirstPressWithNothingBeingDrawnIsReadyAtOnce) {
  PageTurnQueue q;
  q.push(true);
  EXPECT_TRUE(q.ready(/*renderOutstanding=*/false));
  EXPECT_EQ(q.take(), 1);
}

TEST(PageTurnQueue, NothingIsReadyWhenNothingWasPressed) {
  PageTurnQueue q;
  EXPECT_FALSE(q.ready(false));
  EXPECT_FALSE(q.ready(true));
  EXPECT_EQ(q.take(), 0);
}

// --- the accumulation ------------------------------------------------------

TEST(PageTurnQueue, PressesDuringARenderAreHeldAndApplyTogether) {
  PageTurnQueue q;
  q.push(true);
  EXPECT_TRUE(q.ready(false));
  (void)q.take();  // the first press turns one page and starts a render

  q.push(true);
  q.push(true);
  EXPECT_FALSE(q.ready(/*renderOutstanding=*/true)) << "must not turn while the screen is being drawn";
  EXPECT_EQ(q.pending(), 2);

  EXPECT_TRUE(q.ready(false));
  EXPECT_EQ(q.take(), 2) << "both presses, one render";
  EXPECT_EQ(q.pending(), 0);
}

TEST(PageTurnQueue, TakeClearsSoAPressIsNeverAppliedTwice) {
  PageTurnQueue q;
  q.push(true);
  q.push(true);
  EXPECT_EQ(q.take(), 2);
  EXPECT_EQ(q.take(), 0);
  EXPECT_FALSE(q.ready(false));
}

// --- direction -------------------------------------------------------------

TEST(PageTurnQueue, BackAccumulatesTheSameWayForwardDoes) {
  PageTurnQueue q;
  q.push(false);
  q.push(false);
  q.push(false);
  EXPECT_EQ(q.pending(), -3);
  EXPECT_EQ(q.take(), -3);
}

TEST(PageTurnQueue, TheOtherDirectionCancelsRatherThanQueueingBehind) {
  // Three forward then one back should be two forward, not "three forward and
  // then one back" -- the second would cost two refreshes to end up one page
  // from where it started.
  PageTurnQueue q;
  q.push(true);
  q.push(true);
  q.push(true);
  q.push(false);
  EXPECT_EQ(q.pending(), 2);

  // Cancelling all the way through leaves nothing to do, not a turn the other way.
  q.push(false);
  q.push(false);
  EXPECT_EQ(q.pending(), 0);
  EXPECT_FALSE(q.ready(false));
}

TEST(PageTurnQueue, CancellingPastZeroDoesNotOvershootInOneStep) {
  PageTurnQueue q;
  q.push(true);
  q.push(false);
  EXPECT_EQ(q.pending(), 0);
  // The next press in that direction starts a fresh count rather than continuing
  // through zero in the same edge.
  q.push(false);
  EXPECT_EQ(q.pending(), -1);
}

// --- the cap ---------------------------------------------------------------

TEST(PageTurnQueue, PressesBeyondTheCapAreDroppedAndCounted) {
  PageTurnQueue q;
  for (int i = 0; i < PageTurnQueue::MAX_PENDING + 7; i++) q.push(true);
  EXPECT_EQ(q.pending(), PageTurnQueue::MAX_PENDING);
  EXPECT_EQ(q.clamped(), 7) << "a dropped press must be said out loud, not swallowed";
  EXPECT_EQ(q.take(), PageTurnQueue::MAX_PENDING);
}

TEST(PageTurnQueue, TheCapAppliesInBothDirections) {
  PageTurnQueue q;
  for (int i = 0; i < PageTurnQueue::MAX_PENDING + 3; i++) q.push(false);
  EXPECT_EQ(q.pending(), -PageTurnQueue::MAX_PENDING);
  EXPECT_EQ(q.clamped(), 3);
}

TEST(PageTurnQueue, TheClampCountSurvivesTake) {
  // It is a session total, like the other counters the card reports.
  PageTurnQueue q;
  for (int i = 0; i < PageTurnQueue::MAX_PENDING + 2; i++) q.push(true);
  (void)q.take();
  EXPECT_EQ(q.clamped(), 2);
  for (int i = 0; i < PageTurnQueue::MAX_PENDING + 1; i++) q.push(true);
  EXPECT_EQ(q.clamped(), 3);
}

TEST(PageTurnQueue, ANormalDoubleOrTripleTapIsWellInsideTheCap) {
  // The request this was built for: "two presses in quick succession, two or
  // three pages". Nothing about that should ever reach the cap.
  for (int taps = 2; taps <= 3; taps++) {
    PageTurnQueue q;
    for (int i = 0; i < taps; i++) q.push(true);
    EXPECT_EQ(q.pending(), taps);
    EXPECT_EQ(q.clamped(), 0) << taps << " taps";
  }
}

// --- reset -----------------------------------------------------------------

TEST(PageTurnQueue, ResetDropsOwedTurnsWithoutTurningThem) {
  // Leaving the book must not leave a turn owed for the next one.
  PageTurnQueue q;
  q.push(true);
  q.push(true);
  q.reset();
  EXPECT_EQ(q.pending(), 0);
  EXPECT_FALSE(q.ready(false));
}

}  // namespace
