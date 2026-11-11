#pragma once

#include <algorithm>

// How much of one page's extent has been spent, and whether another line fits.
//
// Split out of TxtReaderActivity::loadPageAtOffset() so that the single rule
// which bounds page assembly can be tested on the host. The loop that fills a
// page stops only when this reports the page full, so a budget that can never
// fill is an unbounded loop -- and that is not hypothetical:
//
//   pageExtent and normalPitch are plain ints that only initializeReader() sets
//   to anything, and the index extension in loop() used to be reachable before
//   it had run. With both still 0, `used + pitch > extent` is 0 > 0, which is
//   false forever: a chunk of 8 KB turned into thousands of visual lines and the
//   vector reallocation asked operator new for 98,304 bytes. Crash on open,
//   firmware 1.5.0-dev-main-e14f93e4.
//
// Termination is therefore a property of this class alone, independent of whoever
// calls it: an unusable budget admits nothing, and a usable one charges at least
// one pixel per line, so at most `extent` lines can ever be admitted.
//
// A value type holding four ints. No allocation, nothing to free, small enough
// to sit on the stack of the function that lays out one page.
class PageBudget {
 public:
  // extent: the page's budget -- its height when lines run down it, its width
  // when columns run across it. pitchFloor: the pitch of an ordinary line, which
  // is the smallest a line can cost and so bounds the line count.
  PageBudget(const int extent, const int pitchFloor) : extent_(extent), pitchFloor_(pitchFloor) {}

  // Whether the geometry this was built from has been measured at all. False is
  // a caller ordering mistake, not a thin page: it means the viewport is still
  // zero, and nothing useful can be laid out against it.
  [[nodiscard]] bool usable() const { return extent_ >= 1 && pitchFloor_ >= 1; }

  // Admit a line of the given pitch, or report the page full.
  //
  // The first line of a usable page is always admitted: a pitch taller than the
  // whole viewport must still make progress, or the reader stops at that line
  // for good. An unusable budget admits nothing at all -- not even a first line,
  // because making progress is meaningless when the measurements the progress is
  // against do not exist yet.
  [[nodiscard]] bool admit(const int pitch) {
    if (!usable()) return false;
    // The floor is what makes the count finite whatever a caller passes. With a
    // measured viewport every pitch is already >= 1, so this changes no page
    // boundary; it only removes the possibility of a zero-cost line.
    const int charged = std::max(pitch, 1);
    if (count_ > 0 && used_ + charged > extent_) return false;
    used_ += charged;
    count_++;
    return true;
  }

  [[nodiscard]] int used() const { return used_; }
  [[nodiscard]] int count() const { return count_; }

  // How many lines a page of unvarying pitch holds. What the reader reports as
  // its line (or column) count, and 0 when there is no usable geometry rather
  // than a division by zero.
  [[nodiscard]] int linesAtFloor() const { return usable() ? extent_ / pitchFloor_ : 0; }

 private:
  int extent_;
  int pitchFloor_;
  int used_ = 0;
  int count_ = 0;
};
