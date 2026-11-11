#pragma once

#include <cstddef>
#include <cstdint>

// Page turns that arrive while the screen is still being drawn.
//
// The problem it solves, and the one it must not create. A full refresh is about
// 548 ms, and the main task keeps polling the buttons throughout it, so two
// quick presses are two separate edges. Acting on the second one immediately is
// what the reader used to do, and it has a sharp edge: the forward turn reads
// `currentPageNextOffset`, which only render() sets, so the second press in one
// refresh window falls back to the page index -- and at the index frontier
// `currentPage + 1 >= totalPages` makes it fileSize, which closes the book.
//
// So presses that land during a render are counted and applied together when it
// finishes. The refresh time is the window, which means **no delay is added to
// the first press**: with nothing being drawn, ready() is true in the same pass
// the edge arrived in. A "wait and see if a second press comes" design would
// make every ordinary page turn feel slower, and that is a worse reader.
//
// Nothing here knows about offsets, the index, or the file. It decides only how
// many turns are owed and whether now is the time to do them, which is the part
// that can be tested on the host.
class PageTurnQueue {
 public:
  // Presses beyond this in one window are dropped rather than queued. Five is
  // what a deliberate double or triple tap produces; past that the presses are
  // no longer a request for a destination, and honouring forty of them would
  // lay out forty pages with the reader waiting.
  static constexpr int MAX_PENDING = 5;

  // One button edge. Forward and back cancel: pressing the other way while turns
  // are owed means the reader changed their mind mid-flight, and the alternative
  // -- doing both in sequence -- ends up where it started after two refreshes.
  void push(const bool forward) {
    const int step = forward ? 1 : -1;
    if (pending_ != 0 && (pending_ > 0) != forward) {
      pending_ += step;  // towards zero
      return;
    }
    if (pending_ * step >= MAX_PENDING) {
      clamped_++;
      return;
    }
    pending_ += step;
  }

  // May the owed turns be applied now? Only when no render is outstanding: the
  // forward walk needs the offset the last render left behind.
  [[nodiscard]] bool ready(const bool renderOutstanding) const { return pending_ != 0 && !renderOutstanding; }

  // How many pages to turn, signed: positive forward, negative back. Clears.
  [[nodiscard]] int take() {
    const int n = pending_;
    pending_ = 0;
    return n;
  }

  [[nodiscard]] int pending() const { return pending_; }
  // Edges dropped because MAX_PENDING was reached. Reported, not silent: a
  // non-zero value means a reader is pressing faster than the screen can answer,
  // and that is worth knowing before guessing at the cap again.
  [[nodiscard]] uint16_t clamped() const { return clamped_; }

  void reset() { pending_ = 0; }

 private:
  int pending_ = 0;
  uint16_t clamped_ = 0;
};
