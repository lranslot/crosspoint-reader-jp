#include "Utf8.h"

#include "Cp932Table.h"
#include "Utf8ComposeTable.h"

namespace {
// Halfwidth katakana occupy a single byte in CP932 and map to a contiguous
// Unicode run, so they need no table.
constexpr uint8_t kSjisKanaFirst = 0xA1;
constexpr uint8_t kSjisKanaLast = 0xDF;
constexpr uint32_t kSjisKanaBase = 0xFF61;

bool sjisIsLead(const uint8_t b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC); }
bool sjisIsTrail(const uint8_t b) { return (b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC); }
bool sjisIsSingle(const uint8_t b) { return b <= 0x7F || (b >= kSjisKanaFirst && b <= kSjisKanaLast); }

// Number of bytes in the UTF-8 sequence a lead byte starts, or 0 if it is not a
// valid lead. Overlong forms and the surrogate range are rejected by the caller.
int utf8SeqLen(const uint8_t b) {
  if (b < 0x80) return 1;
  if ((b & 0xE0) == 0xC0) return 2;
  if ((b & 0xF0) == 0xE0) return 3;
  if ((b & 0xF8) == 0xF0) return 4;
  return 0;
}

// Look up the canonical composition of (base + combining mark), or 0 if none.
uint32_t utf8ComposePair(const uint32_t base, const uint32_t mark) {
  if (base > 0xFFFF || mark > 0xFFFF) return 0;
  int lo = 0;
  int hi = kUtf8ComposeTableSize - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const Utf8ComposeEntry& e = kUtf8ComposeTable[mid];
    if (e.base < base || (e.base == base && e.mark < mark)) {
      lo = mid + 1;
    } else if (e.base > base || (e.base == base && e.mark > mark)) {
      hi = mid - 1;
    } else {
      return e.composed;
    }
  }
  return 0;
}

// Strict UTF-8 validity check over a prefix. Rejects overlong encodings, the
// surrogate range and anything past U+10FFFF, because those are exactly the shapes
// Shift-JIS bytes tend to produce when misread as UTF-8. A sequence cut off by the
// end of the buffer is not an error — the caller only ever sees a prefix.
bool looksLikeUtf8(const uint8_t* data, const size_t len) {
  size_t i = 0;
  while (i < len) {
    const uint8_t b = data[i];
    const int need = utf8SeqLen(b);
    if (need == 0) return false;  // continuation byte or 0xF8-0xFF in lead position
    if (need == 1) {
      i++;
      continue;
    }
    if (i + need > len) return true;  // truncated by the buffer edge, not by corruption
    uint32_t cp = b & (0xFF >> (need + 1));
    for (int k = 1; k < need; k++) {
      const uint8_t c = data[i + k];
      if ((c & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (c & 0x3F);
    }
    if (need == 2 && cp < 0x80) return false;
    if (need == 3 && cp < 0x800) return false;
    if (need == 4 && cp < 0x10000) return false;
    if (cp > 0x10FFFF) return false;
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;
    i += need;
  }
  return true;
}

// Shift-JIS validity over a prefix, under the same truncation rule.
bool looksLikeShiftJis(const uint8_t* data, const size_t len) {
  size_t i = 0;
  while (i < len) {
    const uint8_t b = data[i];
    if (sjisIsSingle(b)) {
      i++;
      continue;
    }
    if (!sjisIsLead(b)) return false;
    if (i + 1 >= len) return true;  // lead byte at the buffer edge
    if (!sjisIsTrail(data[i + 1])) return false;
    i += 2;
  }
  return true;
}
}  // namespace

size_t utf8BomLength(const uint8_t* data, const size_t len) {
  if (data != nullptr && len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) return 3;
  return 0;
}

TextEncoding detectTextEncoding(const uint8_t* data, const size_t len) {
  if (data == nullptr || len == 0) return TextEncoding::Utf8;

  if (utf8BomLength(data, len) != 0) return TextEncoding::Utf8;
  // UTF-16 is out of scope: report it as UTF-8 and leave the file alone rather
  // than half-converting it.
  if (len >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))) {
    return TextEncoding::Utf8;
  }

  bool allAscii = true;
  for (size_t i = 0; i < len; i++) {
    if (data[i] >= 0x80) {
      allAscii = false;
      break;
    }
  }
  if (allAscii) return TextEncoding::Utf8;

  if (looksLikeUtf8(data, len)) return TextEncoding::Utf8;
  if (looksLikeShiftJis(data, len)) return TextEncoding::ShiftJis;
  return TextEncoding::Utf8;
}

size_t cp932ToUtf8(const uint8_t* data, const size_t len, std::string& out, const bool flush) {
  if (data == nullptr || len == 0) return 0;

  size_t i = 0;
  while (i < len) {
    const uint8_t b = data[i];

    if (b <= 0x7F) {
      out.push_back(static_cast<char>(b));
      i++;
      continue;
    }
    if (b >= kSjisKanaFirst && b <= kSjisKanaLast) {
      utf8AppendCodepoint(kSjisKanaBase + (b - kSjisKanaFirst), out);
      i++;
      continue;
    }
    if (!sjisIsLead(b)) {
      utf8AppendCodepoint(REPLACEMENT_GLYPH, out);  // stray trail byte
      i++;
      continue;
    }
    if (i + 1 >= len) {
      // Lead byte with no trail byte in this buffer: hand it back to the caller so
      // it can be re-fed with the next chunk, unless this is the end of the file.
      if (!flush) return i;
      utf8AppendCodepoint(REPLACEMENT_GLYPH, out);
      return len;
    }
    const uint16_t cp = cp932Lookup(b, data[i + 1]);
    utf8AppendCodepoint(cp != 0 ? cp : REPLACEMENT_GLYPH, out);
    i += 2;
  }
  return len;
}

std::string utf8ComposeNfc(const std::string& in) {
  // Fast path: NFC composition can only change text that contains a combining
  // diacritical mark U+0300-036F (UTF-8 lead byte 0xCC or 0xCD). Plain ASCII and
  // already-precomposed (NFC) text -- the vast majority of words -- have none, so
  // return them untouched without walking codepoints or allocating. A 0xCD that is
  // actually a non-combining codepoint just falls through to the full pass below.
  bool maybeHasMarks = false;
  for (const unsigned char c : in) {
    if (c == 0xCC || c == 0xCD) {
      maybeHasMarks = true;
      break;
    }
  }
  if (!maybeHasMarks) return in;

  std::string out;
  out.reserve(in.size());
  const unsigned char* p = reinterpret_cast<const unsigned char*>(in.c_str());
  uint32_t base = 0;
  bool haveBase = false;
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (utf8IsCombiningMark(cp)) {
      const uint32_t composed = haveBase ? utf8ComposePair(base, cp) : 0;
      if (composed) {
        base = composed;  // keep accumulating further marks onto the composed char
        continue;
      }
      // No composition: flush the pending base, then emit the mark unchanged.
      if (haveBase) {
        utf8AppendCodepoint(base, out);
        haveBase = false;
      }
      utf8AppendCodepoint(cp, out);
    } else {
      if (haveBase) utf8AppendCodepoint(base, out);
      base = cp;
      haveBase = true;
    }
  }
  if (haveBase) utf8AppendCodepoint(base, out);
  return out;
}

int utf8CodepointLen(const unsigned char c) {
  if (c < 0x80) return 1;          // 0xxxxxxx
  if ((c >> 5) == 0x6) return 2;   // 110xxxxx
  if ((c >> 4) == 0xE) return 3;   // 1110xxxx
  if ((c >> 3) == 0x1E) return 4;  // 11110xxx
  return 1;                        // fallback for invalid
}

uint32_t utf8NextCodepoint(const unsigned char** string) {
  if (**string == 0) {
    return 0;
  }

  const unsigned char lead = **string;
  const int bytes = utf8CodepointLen(lead);
  const uint8_t* chr = *string;

  // Invalid lead byte (stray continuation byte 0x80-0xBF, or 0xFE/0xFF)
  if (bytes == 1 && lead >= 0x80) {
    (*string)++;
    return REPLACEMENT_GLYPH;
  }

  if (bytes == 1) {
    (*string)++;
    return chr[0];
  }

  // Validate continuation bytes before consuming them
  for (int i = 1; i < bytes; i++) {
    if ((chr[i] & 0xC0) != 0x80) {
      // Missing or invalid continuation byte — skip all bytes consumed so far
      *string += i;
      return REPLACEMENT_GLYPH;
    }
  }

  uint32_t cp = chr[0] & ((1 << (7 - bytes)) - 1);  // mask header bits

  for (int i = 1; i < bytes; i++) {
    cp = (cp << 6) | (chr[i] & 0x3F);
  }

  // Reject overlong encodings, surrogates, and out-of-range values
  const bool overlong = (bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000);
  const bool surrogate = (cp >= 0xD800 && cp <= 0xDFFF);
  if (overlong || surrogate || cp > 0x10FFFF) {
    (*string)++;
    return REPLACEMENT_GLYPH;
  }

  *string += bytes;

  return cp;
}

void utf8AppendCodepoint(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

int utf8SafeTruncateBuffer(const char* buf, int len) {
  if (len <= 0) return 0;

  // Walk back past continuation bytes (10xxxxxx) to find the lead byte
  int leadPos = len - 1;
  while (leadPos > 0 && (static_cast<uint8_t>(buf[leadPos]) & 0xC0) == 0x80) {
    leadPos--;
  }

  // Determine expected length of the sequence starting at leadPos
  int expectedLen = utf8CodepointLen(static_cast<unsigned char>(buf[leadPos]));
  int actualLen = len - leadPos;

  if (actualLen < expectedLen && leadPos > 0) {
    // Incomplete UTF-8 sequence at the end — exclude it
    return leadPos;
  }
  return len;
}

size_t utf8RemoveLastChar(std::string& str) {
  if (str.empty()) return 0;
  size_t pos = str.size() - 1;
  while (pos > 0 && (static_cast<unsigned char>(str[pos]) & 0xC0) == 0x80) {
    --pos;
  }
  str.resize(pos);
  return pos;
}

// Truncate string by removing N UTF-8 characters from the end
void utf8TruncateChars(std::string& str, const size_t numChars) {
  for (size_t i = 0; i < numChars && !str.empty(); ++i) {
    utf8RemoveLastChar(str);
  }
}
