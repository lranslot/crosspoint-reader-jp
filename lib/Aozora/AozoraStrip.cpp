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

constexpr char IDEO_SPACE[] = "\xE3\x80\x80";                                    // 　 U+3000
constexpr char JISAGE[] = "\xE5\xAD\x97\xE4\xB8\x8B\xE3\x81\x92";                // 字下げ
constexpr char KOKOKARA[] = "\xE3\x81\x93\xE3\x81\x93\xE3\x81\x8B\xE3\x82\x89";  // ここから
constexpr char KOKODE[] = "\xE3\x81\x93\xE3\x81\x93\xE3\x81\xA7";                // ここで
constexpr char MIDASHI[] = "\xE8\xA6\x8B\xE5\x87\xBA\xE3\x81\x97";               // 見出し
constexpr char DOUGYOU[] = "\xE5\x90\x8C\xE8\xA1\x8C";                           // 同行
constexpr char MADO[] = "\xE7\xAA\x93";                                          // 窓
constexpr char OWARI[] = "\xE7\xB5\x82\xE3\x82\x8F\xE3\x82\x8A";                 // 終わり

constexpr size_t IDEO_SPACE_LEN = 3;
constexpr size_t JISAGE_LEN = 9;
constexpr size_t KOKOKARA_LEN = 12;
constexpr size_t KOKODE_LEN = 9;
constexpr size_t MIDASHI_LEN = 9;
constexpr size_t DOUGYOU_LEN = 6;
constexpr size_t MADO_LEN = 3;
constexpr size_t OWARI_LEN = 9;

// Is `pattern` anywhere inside `body`?
bool containsAt(const char* body, const size_t len, const char* pattern, const size_t patternLen) {
  for (size_t i = 0; i + patternLen <= len; i++) {
    if (memcmp(body + i, pattern, patternLen) == 0) return true;
  }
  return false;
}

// Read a run of fullwidth digits (U+FF10..U+FF19) from the front of `body`.
// Returns the bytes consumed; `value` is the number they spell.
//
// Stops at the first non-digit, which is the whole point. The F-8 survey script
// scanned for digits anywhere in an annotation and read
// ［＃ここから１字下げ、折り返して２字下げ］ as a 12-character indent — a value no file
// contained, which then drove a discussion of how badly it would break. Reading
// digits only where the grammar puts them cannot make that mistake.
size_t readFullWidthNumber(const char* body, const size_t len, size_t& value) {
  size_t i = 0;
  value = 0;
  while (i + 3 <= len && static_cast<uint8_t>(body[i]) == 0xEF && static_cast<uint8_t>(body[i + 1]) == 0xBC &&
         static_cast<uint8_t>(body[i + 2]) >= 0x90 && static_cast<uint8_t>(body[i + 2]) <= 0x99) {
    value = value * 10 + static_cast<size_t>(static_cast<uint8_t>(body[i + 2]) - 0x90);
    i += 3;
  }
  return i;
}

// What an annotation body (between ［＃ and ］) asks for. Only the forms acted on
// are named; everything else is Other and is dropped as it always was.
enum class NoteKind { Other, IndentLine, IndentRangeStart, IndentRangeEnd, HeadingBlock };

NoteKind classifyNote(const char* body, const size_t len, size_t& indent) {
  indent = 0;

  // ［＃ここで字下げ終わり］ — but also ［＃ここで字詰め終わり］, which is not ours.
  if (matches(body, len, KOKODE, KOKODE_LEN)) {
    return containsAt(body, len, JISAGE, JISAGE_LEN) ? NoteKind::IndentRangeEnd : NoteKind::Other;
  }

  // ［＃ここからN字下げ］, and ［＃ここからN字下げ、折り返してM字下げ］. The hanging
  // indent M is read past, not used: only whoever breaks the lines can honour it,
  // and this runs at conversion time, before any of that is known (f8 5.5-4).
  if (matches(body, len, KOKOKARA, KOKOKARA_LEN)) {
    const size_t digits = readFullWidthNumber(body + KOKOKARA_LEN, len - KOKOKARA_LEN, indent);
    if (digits == 0) {
      // ［＃ここから改行天付き…］ indents by nothing, and every other ここから form
      // (罫囲み, ２段組, 字詰め) is not an indent at all. Both end any range in
      // force rather than extending it.
      return NoteKind::IndentRangeStart;
    }
    if (!matches(body + KOKOKARA_LEN + digits, len - KOKOKARA_LEN - digits, JISAGE, JISAGE_LEN)) {
      indent = 0;
      return NoteKind::Other;  // ［＃ここから２０字詰め］ — a line length, not an indent
    }
    return NoteKind::IndentRangeStart;
  }

  // ［＃N字下げ］ on its own, and nothing else in the annotation.
  {
    size_t value = 0;
    const size_t digits = readFullWidthNumber(body, len, value);
    if (digits > 0 && digits + JISAGE_LEN == len && matches(body + digits, len - digits, JISAGE, JISAGE_LEN)) {
      indent = value;
      return NoteKind::IndentLine;
    }
  }

  // ［＃「…」は大見出し］ and the ［＃中見出し］…［＃中見出し終わり］ pair.
  //
  // 同行見出し is excluded: it names a heading sharing its line with the paragraph
  // that follows it, so there is nowhere to put a blank line. 窓見出し sits inset
  // beside the text, for the same reason. Neither is rare — 15 of the 29 headings
  // in 最終戦争論 are 同行中見出し. The closing half of a pair is excluded too, or a
  // blank line would be asked for on the way out of the heading as well as in.
  if (containsAt(body, len, MIDASHI, MIDASHI_LEN) && !containsAt(body, len, DOUGYOU, DOUGYOU_LEN) &&
      !containsAt(body, len, MADO, MADO_LEN) && !containsAt(body, len, OWARI, OWARI_LEN)) {
    return NoteKind::HeadingBlock;
  }

  return NoteKind::Other;
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

void AozoraStrip::flushLine(std::string& out, const bool withNewline) {
  const bool blank = lineOut_.empty();

  // A blank line is wanted in front of this one when it is a block heading, or
  // when it follows one. Suppressed where the file already has a blank there,
  // which is the usual case and the reason this adds so little: all 14 block
  // headings in 最終戦争論 already have one in front, and only 6 lack the one
  // that follows.
  if (!blank && !lineReleased_ && !lastLineBlank_ && (headingLine_ || blankBeforeNext_)) {
    out.push_back('\n');
    stats_.blanksAdded++;
  }

  out.append(lineOut_);
  if (withNewline) out.push_back('\n');

  lastLineBlank_ = blank;
  blankBeforeNext_ = headingLine_;
  headingLine_ = false;
  lineReleased_ = false;
  lineOut_.clear();
}

void AozoraStrip::beginContent() {
  // A ranged indent opens every line it covers. A line-level ［＃N字下げ］ has
  // already put its own spaces in by the time this runs, so the two never stack.
  if (lineOut_.empty() && rangeIndent_ > 0) {
    for (size_t n = 0; n < rangeIndent_; n++) {
      lineOut_.append(IDEO_SPACE, IDEO_SPACE_LEN);
    }
  }
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
      flushLine(out, /*withNewline=*/true);
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
        beginContent();
        lineOut_.append(p, KOME_LEN);  // emit the ※ literally and carry on
        i += KOME_LEN;
        continue;
      }
      beginContent();
      uint32_t cp = 0;
      if (extractCodepoint(p + bodyStart, close, cp)) {
        utf8AppendCodepoint(cp, lineOut_);
        stats_.gaijiRestored++;
      } else {
        lineOut_.append(GETA, 3);
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
        beginContent();
        lineOut_.append(p, NOTE_OPEN_LEN);
        i += NOTE_OPEN_LEN;
        continue;
      }
      stats_.note++;

      // The annotation itself always goes. A few of them leave something behind.
      size_t indent = 0;
      switch (classifyNote(p + NOTE_OPEN_LEN, close, indent)) {
        case NoteKind::IndentLine:
          // Only at the head of a line. ［＃N字下げ］ anywhere else is not indenting
          // anything, and there is nothing sensible to do with it.
          if (lineOut_.empty() && indent > 0) {
            for (size_t n = 0; n < indent; n++) {
              lineOut_.append(IDEO_SPACE, IDEO_SPACE_LEN);
            }
            stats_.indentLines++;
          }
          break;
        case NoteKind::IndentRangeStart:
          if (indent > MAX_RANGE_INDENT) {
            rangeIndent_ = 0;
            stats_.indentTooWide++;
          } else {
            rangeIndent_ = indent;
            if (indent > 0) stats_.indentRanges++;
          }
          break;
        case NoteKind::IndentRangeEnd:
          rangeIndent_ = 0;
          break;
        case NoteKind::HeadingBlock:
          headingLine_ = true;
          break;
        case NoteKind::Other:
          break;
      }
      i += NOTE_OPEN_LEN + close + NOTE_CLOSE_LEN;
      continue;
    }

    // 3 and 4. Ruby: 《…》 and its range marker ｜.
    //
    // Both are kept. They are counted here and then fall through to the ordinary
    // text path, which copies them byte for byte into content.u8 — the reader
    // removes them from what it lays out (TxtReaderActivity::loadPageAtOffset).
    //
    // Nothing is held back for the next chunk any more. Dropping a run needed its
    // closing 》 to know how far to skip; copying it needs nothing beyond the byte
    // in hand, so a run split across two chunks now costs no special case at all.
    if (matches(p, remain, RUBY_OPEN, RUBY_LEN)) {
      stats_.ruby++;
    } else if (matches(p, remain, RANGE_MARK, RANGE_LEN)) {
      // Counted whether or not a 《 follows. On its own this is an ordinary
      // fullwidth vertical bar, and the count is only ever read by the log.
      stats_.range++;
    }

    // 5. Ordinary text — which now includes the ruby notation counted above.
    //
    // Note what is deliberately absent: ／＼ and ／″＼ (the
    // repeat marks) are body text, not notation, and 〔…〕 (decomposed accented
    // Latin) is out of scope for phase 1. Neither is touched.
    beginContent();
    lineOut_.push_back(*p);
    // Release a line that has outgrown the hold. The test is on the line's own
    // length, so it does not depend on how the caller chunks the file.
    if (lineOut_.size() > MAX_LINE_HOLD) {
      out.append(lineOut_);
      lineOut_.clear();
      lineReleased_ = true;
      lastLineBlank_ = false;
    }
    i++;
  }

  // A final line with no newline after it still has to come out.
  if (atEnd && !lineOut_.empty()) {
    flushLine(out, /*withNewline=*/false);
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
