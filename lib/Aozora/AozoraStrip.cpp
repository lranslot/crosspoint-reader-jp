#include "AozoraStrip.h"

#include <Utf8.h>

#include <cstring>

namespace {

// UTF-8 spellings of the notation. Written as escapes rather than literals so the
// encoding of this source file cannot change their meaning.
constexpr char KOME[] = "\xE2\x80\xBB";        // ※ U+203B
constexpr char NOTE_OPEN[] = "\xEF\xBC\xBB\xEF\xBC\x83";  // ［＃ U+FF3B U+FF03
constexpr char NOTE_CLOSE[] = "\xEF\xBC\xBD";  // ］ U+FF3D
constexpr char RUBY_OPEN[] = "\xE3\x80\x8A";   // 《 U+300A
constexpr char RUBY_CLOSE[] = "\xE3\x80\x8B";  // 》 U+300B
constexpr char RANGE_MARK[] = "\xEF\xBD\x9C";  // ｜ U+FF5C
constexpr char GETA[] = "\xE3\x80\x93";        // 〓 U+3013

constexpr size_t KOME_LEN = 3;
constexpr size_t NOTE_OPEN_LEN = 6;
constexpr size_t NOTE_CLOSE_LEN = 3;
constexpr size_t RUBY_LEN = 3;
constexpr size_t RANGE_LEN = 3;

// Beyond this, an unterminated opener is treated as literal text rather than held
// for the next chunk. Bounds the carry so a malformed file cannot stall the scan.
constexpr size_t MAX_CARRY = 256;

bool matches(const char* p, const size_t remain, const char* pattern, const size_t patternLen) {
  return remain >= patternLen && memcmp(p, pattern, patternLen) == 0;
}

// Search for `pattern` from `p`, stopping at the first newline: the notation is
// defined line by line, and scanning past the end of a line would swallow the body
// of a file whose markup is broken.
//
// Returns the offset of the match, or npos. `hitNewline` reports whether the line
// ended first, which distinguishes "not closed on this line" (leave the text
// alone) from "might close in the next chunk" (hold it back).
size_t findInLine(const char* p, const size_t remain, const char* pattern, const size_t patternLen,
                  bool& hitNewline) {
  hitNewline = false;
  for (size_t i = 0; i < remain; i++) {
    if (p[i] == '\n') {
      hitNewline = true;
      return std::string::npos;
    }
    if (matches(p + i, remain - i, pattern, patternLen)) {
      return i;
    }
  }
  return std::string::npos;
}

// Read "U+" followed by 4-6 hex digits somewhere inside a gaiji annotation.
// Only one of the four gaiji forms carries a codepoint; the other three name a
// JIS X 0213 row-cell position or describe the glyph, and are not recoverable
// here, so they fall through to 〓.
bool extractCodepoint(const char* p, const size_t len, uint32_t& cp) {
  for (size_t i = 0; i + 2 < len; i++) {
    if (p[i] != 'U' || p[i + 1] != '+') continue;
    uint32_t value = 0;
    size_t digits = 0;
    size_t j = i + 2;
    while (j < len && digits < 6) {
      const char c = p[j];
      uint32_t d;
      if (c >= '0' && c <= '9') {
        d = static_cast<uint32_t>(c - '0');
      } else if (c >= 'A' && c <= 'F') {
        d = static_cast<uint32_t>(c - 'A' + 10);
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<uint32_t>(c - 'a' + 10);
      } else {
        break;
      }
      value = (value << 4) | d;
      digits++;
      j++;
    }
    if (digits >= 4) {
      cp = value;
      return true;
    }
  }
  return false;
}

// A rule line is nothing but ASCII hyphens, at least ten of them. Aozora uses 55.
bool isRuleLine(const char* p, const size_t len) {
  size_t hyphens = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = p[i];
    if (c == '-') {
      hyphens++;
      continue;
    }
    if (c == '\r') continue;  // tolerate CRLF
    return false;             // anything else disqualifies the line
  }
  return hyphens >= 10;
}

}  // namespace

void AozoraStrip::setHeaderRange(const size_t firstLine, const size_t secondLine) {
  headerFirst_ = firstLine;
  headerLast_ = secondLine;
  headerActive_ = true;
}

size_t AozoraStrip::process(const char* in, const size_t len, std::string& out, const bool atEnd) {
  // Anything held back last time belongs in front of this chunk.
  if (carry_.empty()) {
    buffer_.assign(in, len);
  } else {
    buffer_ = carry_;
    buffer_.append(in, len);
    carry_.clear();
  }

  const char* const base = buffer_.data();
  const size_t total = buffer_.size();
  size_t i = 0;

  while (i < total) {
    const char* p = base + i;
    const size_t remain = total - i;

    // Legend block: drop whole lines. The range was fixed up front, so this only
    // ever counts newlines and cannot run away.
    if (inHeader()) {
      if (!headerCounted_) {
        stats_.headerLines++;
        headerCounted_ = true;
      }
      if (*p == '\n') {
        lineNumber_++;
        headerCounted_ = false;
      }
      i++;
      continue;
    }

    if (*p == '\n') {
      lineNumber_++;
      out.push_back('\n');
      i++;
      continue;
    }

    // 1. Gaiji: ※［＃…］. Checked before the bare ［＃ so the ※ is consumed with it.
    if (matches(p, remain, KOME, KOME_LEN) && matches(p + KOME_LEN, remain - KOME_LEN, NOTE_OPEN, NOTE_OPEN_LEN)) {
      const size_t bodyStart = KOME_LEN + NOTE_OPEN_LEN;
      bool hitNewline = false;
      const size_t close = findInLine(p + bodyStart, remain - bodyStart, NOTE_CLOSE, NOTE_CLOSE_LEN, hitNewline);
      if (close == std::string::npos) {
        // Unterminated. Hold it for the next chunk unless the line already ended
        // (then it is simply broken markup) or it has grown implausibly long.
        if (!hitNewline && !atEnd && remain < MAX_CARRY) {
          carry_.assign(p, remain);
          return carry_.size();
        }
        out.append(p, KOME_LEN);  // emit the ※ literally and carry on
        i += KOME_LEN;
        continue;
      }
      uint32_t cp = 0;
      if (extractCodepoint(p + bodyStart, close, cp)) {
        utf8AppendCodepoint(cp, out);
        stats_.gaijiRestored++;
      } else {
        out.append(GETA, 3);
        stats_.gaijiGeta++;
      }
      i += bodyStart + close + NOTE_CLOSE_LEN;
      continue;
    }

    // 2. Annotation: ［＃…］ — dropped entirely.
    if (matches(p, remain, NOTE_OPEN, NOTE_OPEN_LEN)) {
      bool hitNewline = false;
      const size_t close = findInLine(p + NOTE_OPEN_LEN, remain - NOTE_OPEN_LEN, NOTE_CLOSE, NOTE_CLOSE_LEN, hitNewline);
      if (close == std::string::npos) {
        if (!hitNewline && !atEnd && remain < MAX_CARRY) {
          carry_.assign(p, remain);
          return carry_.size();
        }
        out.append(p, NOTE_OPEN_LEN);
        i += NOTE_OPEN_LEN;
        continue;
      }
      stats_.note++;
      i += NOTE_OPEN_LEN + close + NOTE_CLOSE_LEN;
      continue;
    }

    // 3. Ruby: 《…》 — dropped entirely.
    if (matches(p, remain, RUBY_OPEN, RUBY_LEN)) {
      bool hitNewline = false;
      const size_t close = findInLine(p + RUBY_LEN, remain - RUBY_LEN, RUBY_CLOSE, RUBY_LEN, hitNewline);
      if (close == std::string::npos) {
        if (!hitNewline && !atEnd && remain < MAX_CARRY) {
          carry_.assign(p, remain);
          return carry_.size();
        }
        out.append(p, RUBY_LEN);
        i += RUBY_LEN;
        continue;
      }
      stats_.ruby++;
      i += RUBY_LEN + close + RUBY_LEN;
      continue;
    }

    // 4. Ruby range marker: ｜ — only meaningful when a 《 follows on the same
    // line. On its own it is an ordinary fullwidth vertical bar.
    if (matches(p, remain, RANGE_MARK, RANGE_LEN)) {
      bool hitNewline = false;
      const size_t ruby = findInLine(p + RANGE_LEN, remain - RANGE_LEN, RUBY_OPEN, RUBY_LEN, hitNewline);
      if (ruby == std::string::npos) {
        if (!hitNewline && !atEnd && remain < MAX_CARRY) {
          carry_.assign(p, remain);
          return carry_.size();
        }
        out.append(p, RANGE_LEN);
        i += RANGE_LEN;
        continue;
      }
      stats_.range++;
      i += RANGE_LEN;
      continue;
    }

    // 5. Ordinary text. Note what is deliberately absent: ／＼ and ／″＼ (the
    // repeat marks) are body text, not notation, and 〔…〕 (decomposed accented
    // Latin) is out of scope for phase 1. Neither is touched.
    out.push_back(*p);
    i++;
  }

  return 0;
}

bool aozoraLooksLikeAozora(const char* data, const size_t len) {
  // 【テキスト中に現れる記号について】
  static constexpr char MARKER[] =
      "\xE3\x80\x90\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88\xE4\xB8\xAD\xE3\x81\xAB\xE7\x8F\xBE\xE3\x82\x8C"
      "\xE3\x82\x8B\xE8\xA8\x98\xE5\x8F\xB7\xE3\x81\xAB\xE3\x81\xA4\xE3\x81\x84\xE3\x81\xA6\xE3\x80\x91";
  const size_t markerLen = sizeof(MARKER) - 1;
  if (data == nullptr || len < markerLen) return false;
  for (size_t i = 0; i + markerLen <= len; i++) {
    if (memcmp(data + i, MARKER, markerLen) == 0) return true;
  }
  return false;
}

bool aozoraFindHeaderRange(const char* data, const size_t len, size_t& firstLine, size_t& lastLine) {
  if (data == nullptr || len == 0) return false;

  size_t line = 0;
  size_t lineStart = 0;
  bool haveFirst = false;
  size_t found = 0;

  for (size_t i = 0; i <= len; i++) {
    const bool atEnd = (i == len);
    if (!atEnd && data[i] != '\n') continue;

    if (isRuleLine(data + lineStart, i - lineStart)) {
      if (!haveFirst) {
        firstLine = line;
        haveFirst = true;
        found = 1;
      } else {
        lastLine = line;
        return true;  // both rules located
      }
    }

    if (atEnd) break;
    line++;
    lineStart = i + 1;
  }

  (void)found;
  // Only one rule (or none) inside the probe: leave the legend alone entirely
  // rather than guess where it ends.
  return false;
}
