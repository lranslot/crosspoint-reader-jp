#pragma once

#include <cstddef>

// Where a column breaks when the text runs vertically.
//
// Horizontally a break is a measured width and needs the font. Vertically it is
// a count: every codepoint occupies one cell, so there is nothing to measure and
// the rule is pure. Keeping it here rather than inside the activity is what lets
// tools/layout_harness drive the real rule instead of a copy of it.
namespace vertwrap {

// How many bytes of `text` the first `count` codepoints occupy, stopping at
// `len`. UTF-8 continuation bytes (10xxxxxx) belong to the codepoint in front of
// them and are never counted on their own.
[[nodiscard]] inline size_t advanceCodepoints(const char* text, const size_t len, const int count) {
  size_t i = 0;
  for (int taken = 0; taken < count && i < len; taken++) {
    i++;
    while (i < len && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) i++;
  }
  return i;
}

}  // namespace vertwrap
