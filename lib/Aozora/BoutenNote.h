#pragma once

#include <cstddef>
#include <cstring>

// The emphasis-mark annotation ［＃「X」に<kind>傍点］, and nothing else.
//
// In its own header so tools/layout_harness can ask the shipped parser what a
// real file contains. The counters the reader reports on the card
// (`bt=drawn/variant/collide/noparent`) could not be checked against a file
// while this was a private function of a class that cannot be built on the
// host -- and two of the four numbers turned out to need checking.
namespace bouten {

inline constexpr char MARU[] = "\xE4\xB8\xB8";              // 丸
inline constexpr char GOMA[] = "\xE3\x82\xB4\xE3\x83\x9E";  // ゴマ

// Does `kind` name one of the two shapes the table actually cuts? 丸 and ゴマ
// are drawn as themselves; 三角, 二重丸, 蛇の目 and ばつ come out as the disc or
// the ring on their own side of the black/white split, and are counted so a book
// using one is heard from rather than silently approximated.
[[nodiscard]] inline bool kindIsExact(const char* kind, const size_t len) {
  size_t at = 0;
  if (len >= 3 && memcmp(kind, "\xE7\x99\xBD", 3) == 0) {
    at = 3;  // 白
  } else if (len >= 3 && memcmp(kind, "\xE9\xBB\x92", 3) == 0) {
    at = 3;  // 黒
  }
  const size_t rest = len - at;
  return (rest == 3 && memcmp(kind + at, MARU, 3) == 0) || (rest == 6 && memcmp(kind + at, GOMA, 6) == 0);
}

struct Note {
  size_t quotedAt = 0;
  size_t quotedLen = 0;
  bool hollow = false;
  bool approximated = false;
};

// Reads the body of an annotation (the bytes between ［＃ and ］). Returns false
// for every other annotation, which is what leaves the indent marker -- also an
// annotation -- in the line to be read as a prefix further down.
[[nodiscard]] inline bool parseNote(const char* body, const size_t len, Note& out) {
  static constexpr char KAGI_OPEN[] = "\xE3\x80\x8C";           // 「
  static constexpr char KAGI_CLOSE[] = "\xE3\x80\x8D";          // 」
  static constexpr char NI[] = "\xE3\x81\xAB";                  // に
  static constexpr char BOUTEN[] = "\xE5\x82\x8D\xE7\x82\xB9";  // 傍点
  static constexpr char SHIRO[] = "\xE7\x99\xBD";               // 白
  static constexpr size_t CH = 3;
  static constexpr size_t BOUTEN_LEN = 6;

  if (len < CH * 3 + BOUTEN_LEN) return false;
  if (memcmp(body, KAGI_OPEN, CH) != 0) return false;
  // The first 」 ends the quoted run, which is the rule the notation's own
  // grammar uses; a quoted run containing one is a malformed file.
  size_t closeAt = CH;
  while (closeAt + CH <= len && memcmp(body + closeAt, KAGI_CLOSE, CH) != 0) closeAt++;
  if (closeAt + CH > len) return false;
  const size_t quotedLen = closeAt - CH;
  size_t at = closeAt + CH;
  if (at + CH > len || memcmp(body + at, NI, CH) != 0) return false;
  at += CH;
  const size_t kindAt = at;
  if (len < BOUTEN_LEN || len - BOUTEN_LEN < kindAt) return false;
  if (memcmp(body + len - BOUTEN_LEN, BOUTEN, BOUTEN_LEN) != 0) {
    return false;  // ［＃「…」に傍線］ and the rest: not ours, and not removed
  }
  const size_t kindLen = len - BOUTEN_LEN - kindAt;
  out.quotedAt = CH;
  out.quotedLen = quotedLen;
  out.hollow = false;
  for (size_t k = 0; k + CH <= kindLen; k++) {
    if (memcmp(body + kindAt + k, SHIRO, CH) == 0) {
      out.hollow = true;
      break;
    }
  }
  out.approximated = kindLen > 0 && !kindIsExact(body + kindAt, kindLen);
  return true;
}

}  // namespace bouten
