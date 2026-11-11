#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// The indent notation 青空文庫 writes at the head of a line, and the two things a
// reader has to do with it: read it, and work out what a *continuation* of that
// line owes when the page it continues on no longer contains the head.
//
// Why this is a header of its own, and not three private methods: a page may
// begin in the middle of a source line (F-9 lazy page index), and until stage 2i
// the continuation was laid out at indent 0 because the notation had been left
// behind on the previous page. The bug was invisible to every test we had,
// because the only code that could see it was 300 lines deep inside the page
// fill loop, behind a renderer and a font. Measured on the device: a one-cell
// paragraph continued at 0 on p36, a three-cell one at 0 for four columns on
// p37, while indents *within* a page were correct throughout.
//
// Nothing here allocates, reads a file, or knows the viewport. The caller clamps
// the cells it gets back against the live viewport, exactly as it does for the
// notation it reads itself.
namespace aozora {

// Bytes, not literals, so the encoding of this file cannot change what they
// mean. Must stay the spelling AozoraStrip::beginContent() writes.
inline constexpr char NOTE_OPEN[] = "\xEF\xBC\xBB\xEF\xBC\x83";           // ［＃
inline constexpr char NOTE_CLOSE[] = "\xEF\xBC\xBD";                      // ］
inline constexpr char JISAGE[] = "\xE5\xAD\x97\xE4\xB8\x8B\xE3\x81\x92";  // 字下げ
inline constexpr char ORIKAESHI[] =
    "\xE3\x80\x81\xE6\x8A\x98\xE3\x82\x8A\xE8\xBF\x94\xE3\x81\x97\xE3\x81\xA6";  // 、折り返して
inline constexpr size_t NOTE_OPEN_LEN = 6;
inline constexpr size_t NOTE_CLOSE_LEN = 3;
inline constexpr size_t JISAGE_LEN = 9;
inline constexpr size_t ORIKAESHI_LEN = 18;

// Reads ［＃N字下げ］ / ［＃N字下げ、折り返してM字下げ］ at the head of `line`.
// Returns the bytes the notation occupies, or 0 when the line does not start
// with one. `indent` and `hanging` are the numbers as written, unclamped.
[[nodiscard]] inline size_t parseIndentNotation(const char* line, const size_t len, uint8_t& indent, uint8_t& hanging) {
  indent = 0;
  hanging = 0;
  if (line == nullptr || len < NOTE_OPEN_LEN || memcmp(line, NOTE_OPEN, NOTE_OPEN_LEN) != 0) return 0;

  // One full-width digit run, then 字下げ. Reading digits only where the grammar
  // puts them is what keeps ［＃ここから１字下げ、折り返して２字下げ］ from being read
  // as a single twelve-cell indent (the mistake the F-8 survey script made).
  const auto readNumber = [&](size_t& at, unsigned& value) {
    const size_t from = at;
    value = 0;
    while (at + 3 <= len && static_cast<uint8_t>(line[at]) == 0xEF && static_cast<uint8_t>(line[at + 1]) == 0xBC &&
           static_cast<uint8_t>(line[at + 2]) >= 0x90 && static_cast<uint8_t>(line[at + 2]) <= 0x99) {
      value = value * 10 + static_cast<unsigned>(static_cast<uint8_t>(line[at + 2]) - 0x90);
      at += 3;
    }
    return at > from;
  };
  const auto expect = [&](size_t& at, const char* want, const size_t wantLen) {
    if (len - at < wantLen || memcmp(line + at, want, wantLen) != 0) return false;
    at += wantLen;
    return true;
  };

  size_t at = NOTE_OPEN_LEN;
  unsigned n = 0;
  if (!readNumber(at, n) || !expect(at, JISAGE, JISAGE_LEN)) return 0;
  unsigned m = 0;
  if (expect(at, ORIKAESHI, ORIKAESHI_LEN)) {
    if (!readNumber(at, m) || !expect(at, JISAGE, JISAGE_LEN)) return 0;
  }
  if (!expect(at, NOTE_CLOSE, NOTE_CLOSE_LEN)) return 0;

  indent = static_cast<uint8_t>(n > 255u ? 255u : n);
  hanging = static_cast<uint8_t>(m > 255u ? 255u : m);
  return at;
}

// What a *continuation* of a line carrying this notation owes. No 折り返して
// means the continuations line up under the first line's text rather than back
// at the margin: that is what 青空文庫 writes ［＃ここからN字下げ］ for a block
// quote to mean (docs-jp/f8-aozora-notation.md section 5.5-8).
[[nodiscard]] inline int carryCells(const uint8_t indent, const uint8_t hanging) {
  return hanging > 0 ? hanging : indent;
}

// Not a byte in the window, used as "no newline here".
inline constexpr size_t NO_LINE_START = static_cast<size_t>(-1);

// Given a window of bytes that ends exactly where a page begins, returns the
// window offset of the first byte of the source line that page starts inside.
// `atFileStart` says whether the window begins at byte 0 of the file, which is
// a line head with no newline in front of it -- the one case a backward scan
// cannot see for itself, and so an argument rather than the caller's business.
// Leaving it to the caller is what made the first version of this header pass
// its own tests while disagreeing with the reader.
//
// Three distinct answers, and the caller must tell them apart:
//   - 0 .. windowLen-1 : the line starts there
//   - windowLen        : the page begins at a line head; there is no carry
//   - NO_LINE_START    : no '\n' in the window and it does not reach the start
//                        of the file, so the head is further back than the
//                        caller was willing to read
[[nodiscard]] inline size_t findSourceLineStart(const char* window, const size_t windowLen, const bool atFileStart) {
  if (window == nullptr || windowLen == 0) return atFileStart ? 0 : NO_LINE_START;
  if (window[windowLen - 1] == '\n') return windowLen;  // page starts at a line head
  for (size_t i = windowLen - 1; i > 0; i--) {
    if (window[i - 1] == '\n') return i;
  }
  return atFileStart ? 0 : NO_LINE_START;
}

}  // namespace aozora
