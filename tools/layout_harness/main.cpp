// See README.md. Two modes:
//
//   alloc <corpus-dir>            what one conversion asks the heap for
//   pages <corpus-dir> [--no-carry]  where the page boundaries land
//
// Both drive shipped code. `alloc` runs the real AozoraStrip and the real
// cp932ToUtf8 through the same loop shape Txt::convertToUtf8() and
// Txt::stripUtf8ToCache() use, with the global operator new replaced so the
// requests can be counted. `pages` drives the shipped PageBudget, AozoraIndent
// and VerticalWrap from a fill loop written to match the vertical path of
// TxtReaderActivity::loadPageAtOffset(); ruby and emphasis marks are left out,
// so every line costs one pitch and the page counts are comparable between two
// firmware revisions rather than to the device.

#include <AozoraStrip.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "AozoraIndent.h"
#include "BoutenNote.h"
#include "PageBudget.h"
#include "VerticalWrap.h"

// --- the heap, counted ------------------------------------------------------
//
// Every block carries a 16-byte header holding its own size, so operator delete
// can subtract it from the outstanding total without a side table. 16 and not 8
// keeps the returned pointer 16-aligned, which is what the default allocator
// guarantees and what std::string's growth assumes.
namespace heapcount {

constexpr size_t HEADER = 16;

struct Stats {
  size_t largestRequest = 0;
  size_t outstanding = 0;
  size_t peakOutstanding = 0;
  size_t requests = 0;
};

Stats g;
bool tracking = false;
size_t baseOutstanding = 0;

void* take(const size_t bytes) {
  void* raw = std::malloc(bytes + HEADER);
  if (raw == nullptr) {
    std::fputs("harness: out of memory\n", stderr);
    std::exit(2);
  }
  *static_cast<size_t*>(raw) = bytes;
  if (tracking) {
    g.requests++;
    g.largestRequest = std::max(g.largestRequest, bytes);
    g.outstanding += bytes;
    g.peakOutstanding = std::max(g.peakOutstanding, g.outstanding);
  }
  return static_cast<char*>(raw) + HEADER;
}

void give(void* p) {
  if (p == nullptr) return;
  char* raw = static_cast<char*>(p) - HEADER;
  const size_t bytes = *reinterpret_cast<size_t*>(raw);
  if (tracking && g.outstanding >= bytes) g.outstanding -= bytes;
  std::free(raw);
}

void begin() {
  g = Stats{};
  tracking = true;
}

// The firmware writes each stripped chunk straight to the card. The harness
// keeps it in memory instead so it can be compared byte for byte, and that
// growing buffer is the harness's cost, not the conversion's -- so it is taken
// outside the count rather than left to dominate it.
struct Untracked {
  bool was;
  Untracked() : was(tracking) { tracking = false; }
  ~Untracked() { tracking = was; }
};

Stats end() {
  tracking = false;
  return g;
}

}  // namespace heapcount

void* operator new(size_t n) { return heapcount::take(n); }
void* operator new[](size_t n) { return heapcount::take(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return heapcount::take(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return heapcount::take(n); }
void operator delete(void* p) noexcept { heapcount::give(p); }
void operator delete[](void* p) noexcept { heapcount::give(p); }
void operator delete(void* p, size_t) noexcept { heapcount::give(p); }
void operator delete[](void* p, size_t) noexcept { heapcount::give(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { heapcount::give(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { heapcount::give(p); }

// --- the firmware's constants ----------------------------------------------

constexpr size_t CONVERT_CHUNK = 8 * 1024;  // lib/Txt/Txt.cpp
constexpr size_t PAGE_CHUNK = 8 * 1024;     // CHUNK_SIZE in TxtReaderActivity.cpp
constexpr size_t PROBE_BYTES = 4096;        // the encoding probe in prepareEncoding()
constexpr int MIN_TEXT_CELLS = 4;           // TxtReaderActivity::MIN_TEXT_CELLS

// The reserves the firmware does before converting, switchable so the before and
// after of that change can be measured by the same binary. `--no-reserve` is the
// shape the firmware had up to and including 1.5.0-dev-main-38af4c6e.
bool g_reserve = true;

// --- input ------------------------------------------------------------------

std::string readWholeFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "harness: cannot read %s\n", path.c_str());
    std::exit(2);
  }
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Valid UTF-8 means UTF-8; anything else is taken for Shift-JIS, which is the
// decision prepareEncoding() reaches by the same elimination.
bool looksUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    size_t extra = 0;
    if (c < 0x80) {
      extra = 0;
    } else if ((c & 0xE0) == 0xC0) {
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
    } else {
      return false;
    }
    if (i + extra >= s.size()) return false;
    for (size_t k = 1; k <= extra; k++) {
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    }
    i += extra + 1;
  }
  return true;
}

// --- the conversion, as the firmware runs it --------------------------------

struct ConvertResult {
  std::string out;
  heapcount::Stats heap;
  size_t maxChunkOut = 0;  // largest single strip output, which is what `stripped` needs
  size_t maxStripIn = 0;   // largest single strip input, which is what `buffer_` needs
};

// Mirrors Txt::stripUtf8ToCache(): the source is already UTF-8, only the markup
// has to come out.
ConvertResult convertUtf8(const std::string& src) {
  ConvertResult r;
  const size_t probeLen = std::min(PROBE_BYTES, src.size());
  AozoraStrip strip;
  size_t first = 0;
  size_t last = 0;
  const bool isAozora = aozoraLooksLikeAozora(src.data(), probeLen);
  if (isAozora && aozoraFindHeaderRange(src.data(), probeLen, first, last)) {
    strip.setHeaderRange(first, last);
  }
  if (g_reserve) strip.reserveForChunk(CONVERT_CHUNK);

  heapcount::begin();
  {
    auto buffer = std::make_unique<uint8_t[]>(CONVERT_CHUNK);
    std::string stripped;
    if (g_reserve) stripped.reserve(CONVERT_CHUNK + CONVERT_CHUNK / 4);
    size_t pos = 0;
    size_t inputBytes = 0;
    while (true) {
      const size_t read = std::min(CONVERT_CHUNK, src.size() - pos);
      std::memcpy(buffer.get(), src.data() + pos, read);
      pos += read;
      if (read == 0) {
        stripped.clear();
        strip.process("", 0, stripped, /*atEnd=*/true);
        {
          heapcount::Untracked u;
          r.out += stripped;
        }
        break;
      }
      inputBytes += read;
      const bool atEnd = inputBytes >= src.size();
      stripped.clear();
      strip.process(reinterpret_cast<const char*>(buffer.get()), read, stripped, atEnd);
      r.maxStripIn = std::max(r.maxStripIn, read);
      r.maxChunkOut = std::max(r.maxChunkOut, stripped.size());
      {
        heapcount::Untracked u;
        r.out += stripped;
      }
      if (atEnd) break;
    }
  }
  r.heap = heapcount::end();
  return r;
}

// Mirrors Txt::convertToUtf8(): transcode, then strip the UTF-8 that came out.
// Two independent carries, one inside cp932ToUtf8 for a split Shift-JIS pair and
// one inside AozoraStrip for split markup.
ConvertResult convertSjis(const std::string& src) {
  ConvertResult r;
  AozoraStrip strip;
  {
    const size_t probeLen = std::min(PROBE_BYTES, src.size());
    std::string probeUtf8;
    probeUtf8.reserve(probeLen * 3 / 2);
    cp932ToUtf8(reinterpret_cast<const uint8_t*>(src.data()), probeLen, probeUtf8, /*flush=*/false);
    size_t first = 0;
    size_t last = 0;
    if (aozoraLooksLikeAozora(probeUtf8.data(), probeUtf8.size()) &&
        aozoraFindHeaderRange(probeUtf8.data(), probeUtf8.size(), first, last)) {
      strip.setHeaderRange(first, last);
    }
  }
  if (g_reserve) strip.reserveForChunk(CONVERT_CHUNK * 3 / 2);

  heapcount::begin();
  {
    auto buffer = std::make_unique<uint8_t[]>(CONVERT_CHUNK);
    std::string encoded;
    encoded.reserve(CONVERT_CHUNK * 3 / 2);
    std::string stripped;
    if (g_reserve) stripped.reserve(encoded.capacity() + encoded.capacity() / 4);
    size_t pos = 0;
    size_t carry = 0;
    while (true) {
      const size_t read = std::min(CONVERT_CHUNK - carry, src.size() - pos);
      std::memcpy(buffer.get() + carry, src.data() + pos, read);
      pos += read;
      const size_t avail = carry + read;
      if (avail == 0) {
        stripped.clear();
        strip.process("", 0, stripped, /*atEnd=*/true);
        {
          heapcount::Untracked u;
          r.out += stripped;
        }
        break;
      }
      const bool atEnd = (read == 0) || (read < CONVERT_CHUNK - carry);
      encoded.clear();
      const size_t consumed = cp932ToUtf8(buffer.get(), avail, encoded, atEnd);
      stripped.clear();
      strip.process(encoded.data(), encoded.size(), stripped, atEnd);
      r.maxStripIn = std::max(r.maxStripIn, encoded.size());
      r.maxChunkOut = std::max(r.maxChunkOut, stripped.size());
      {
        heapcount::Untracked u;
        r.out += stripped;
      }
      carry = avail - consumed;
      if (carry > 0) std::memmove(buffer.get(), buffer.get() + consumed, carry);
      if (atEnd) break;
    }
  }
  r.heap = heapcount::end();
  return r;
}

// --- the page fill, as close to the vertical path as the host can get -------

struct Setting {
  const char* name;
  int cellSize;      // one full-width cell, px
  int normalPitch;   // what one column costs out of the page's budget
  int pageExtent;    // the viewport's width, which is the budget vertically
  int charsPerUnit;  // cells one column holds
};

// 14 pt on the development card: a 29 px cell (14 * 150 / 72 rounded) in a
// 464x764 viewport, at the three line spacings the reader offers. The pitch for
// "normal" is the one measured on the device (reader-open.txt reports n=12,
// which is 464 / 37).
constexpr Setting SETTINGS[] = {
    {"narrow", 29, 32, 464, 764 / 29},
    {"normal", 29, 37, 464, 764 / 29},
    {"wide", 29, 42, 464, 764 / 29},
};

int clampIndent(const int cells, const Setting& s) {
  if (cells <= 0) return 0;
  const int room = s.charsPerUnit - MIN_TEXT_CELLS;
  if (room <= 0) return 0;
  return std::min(cells, room);
}

// The carry recovery, driven the way TxtReaderActivity::recoverIndentCarry()
// drives it: 512-byte steps back, at most 8 KB, the shipped scan and the shipped
// parser deciding.
constexpr size_t CARRY_STEP = 4 * 1024;
constexpr size_t CARRY_MAX_BACK = 32 * 1024;
constexpr size_t CARRY_HEAD = 96;

bool recoverIndentCarry(const std::string& content, const size_t offset, const Setting& s, int& cells, bool& lost) {
  cells = 0;
  lost = false;
  if (offset == 0) return false;
  size_t lineStart = 0;
  bool have = false;
  size_t scanned = 0;
  while (scanned < CARRY_MAX_BACK && scanned < offset) {
    const size_t want = std::min(CARRY_STEP, offset - scanned);
    const size_t from = offset - scanned - want;
    const size_t found = aozora::findSourceLineStart(content.data() + from, want, /*atFileStart=*/from == 0);
    if (found == want) {
      if (scanned == 0) return false;
      lineStart = from + want;
      have = true;
      break;
    }
    if (found != aozora::NO_LINE_START) {
      lineStart = from + found;
      have = true;
      break;
    }
    scanned += want;
  }
  if (!have) {
    lost = true;
    return false;
  }
  const size_t headLen = std::min(CARRY_HEAD, offset - lineStart);
  if (headLen < aozora::NOTE_OPEN_LEN) return false;
  uint8_t indent = 0;
  uint8_t hanging = 0;
  if (aozora::parseIndentNotation(content.data() + lineStart, headLen, indent, hanging) == 0) return false;
  cells = clampIndent(aozora::carryCells(indent, hanging), s);
  return cells > 0;
}

struct FillStats {
  size_t carryLost = 0;
};

// One page, vertically. Returns the offset the next page starts at.
size_t fillPage(const std::string& content, const size_t offset, const Setting& s, const bool useCarry,
                FillStats& stats, int& columnsOut) {
  PageBudget budget(s.pageExtent, s.normalPitch);
  columnsOut = 0;
  const size_t chunkSize = std::min(PAGE_CHUNK, content.size() - offset);
  const char* buffer = content.data() + offset;

  int carriedIndent = 0;
  if (useCarry && offset > 0) {
    bool lost = false;
    if (!recoverIndentCarry(content, offset, s, carriedIndent, lost)) carriedIndent = 0;
    if (lost) stats.carryLost++;
  }

  bool pageFull = false;
  size_t pos = 0;
  while (pos < chunkSize && !pageFull) {
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') lineEnd++;
    const bool lineComplete = (lineEnd < chunkSize) || (offset + lineEnd >= content.size());
    if (!lineComplete && columnsOut > 0) break;

    size_t contentLen = lineEnd - pos;
    if (contentLen > 0 && buffer[pos + contentLen - 1] == '\r') contentLen--;
    const char* line = buffer + pos;
    size_t displayLen = contentLen;

    uint8_t asked = 0;
    uint8_t askedHanging = 0;
    const size_t prefixLen = aozora::parseIndentNotation(line, displayLen, asked, askedHanging);
    int firstIndent = 0;
    int hangIndent = 0;
    if (prefixLen > 0) {
      firstIndent = clampIndent(asked, s);
      hangIndent = askedHanging > 0 ? clampIndent(askedHanging, s) : firstIndent;
    } else if (pos == 0 && carriedIndent > 0) {
      firstIndent = carriedIndent;
      hangIndent = carriedIndent;
    }

    size_t lineBytePos = prefixLen;
    do {
      if (displayLen == 0) {
        if (!budget.admit(s.normalPitch)) {
          pageFull = true;
        } else {
          columnsOut++;
        }
        break;
      }
      const char* segment = line + lineBytePos;
      const size_t segmentLen = displayLen - lineBytePos;
      const int indentCells = (lineBytePos == prefixLen) ? firstIndent : hangIndent;
      size_t breakPos = vertwrap::advanceCodepoints(segment, segmentLen, std::max(1, s.charsPerUnit - indentCells));
      const bool lastOfLine = breakPos >= segmentLen;
      if (lastOfLine) {
        if (!budget.admit(s.normalPitch)) {
          pageFull = true;
          break;
        }
        columnsOut++;
        lineBytePos = displayLen;
        break;
      }
      breakPos = (breakPos == 0) ? 1 : breakPos;
      if (!budget.admit(s.normalPitch)) {
        pageFull = true;
        break;
      }
      columnsOut++;
      size_t skip = breakPos;
      if (breakPos < segmentLen && segment[breakPos] == ' ') skip++;
      lineBytePos += skip;
    } while (lineBytePos < displayLen && !pageFull);

    if (pageFull) {
      if (lineBytePos > prefixLen) pos = pos + lineBytePos;
      break;
    }
    pos = lineEnd + 1;
  }

  if (pos == 0 && columnsOut > 0) pos = 1;
  size_t next = offset + pos;
  if (next > content.size()) next = content.size();
  return next;
}

// FNV-1a over the boundary list, so one line can say whether anything moved.
uint64_t hashOffsets(const std::vector<size_t>& offsets) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (const size_t o : offsets) {
    for (int b = 0; b < 8; b++) {
      h ^= static_cast<uint8_t>(o >> (b * 8));
      h *= 0x100000001b3ULL;
    }
  }
  return h;
}

// --- modes ------------------------------------------------------------------

std::vector<std::string> corpusFiles(const std::string& dir) {
  std::vector<std::string> out;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    const std::string p = e.path().string();
    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".txt") == 0) out.push_back(p);
  }
  std::sort(out.begin(), out.end());
  return out;
}

int modeAlloc(const std::string& dir) {
  std::printf("%-28s %-6s %10s %10s %12s %9s %12s %10s %10s\n", "file", "path", "src", "out", "largest-req", "requests",
              "peak-live", "strip-in", "strip-out");
  for (const std::string& path : corpusFiles(dir)) {
    const std::string src = readWholeFile(path);
    const bool utf8 = looksUtf8(src);
    const ConvertResult r = utf8 ? convertUtf8(src) : convertSjis(src);
    const std::string name = std::filesystem::path(path).filename().string();
    std::printf("%-28s %-6s %10zu %10zu %12zu %9zu %12zu %10zu %10zu\n", name.c_str(), utf8 ? "utf8" : "sjis",
                src.size(), r.out.size(), r.heap.largestRequest, r.heap.requests, r.heap.peakOutstanding, r.maxStripIn,
                r.maxChunkOut);
    // The converted bytes go beside the input so a change can be diffed.
    std::ofstream out(path + ".converted", std::ios::binary);
    out.write(r.out.data(), static_cast<std::streamsize>(r.out.size()));
  }
  return 0;
}

int modePages(const std::string& dir, const bool useCarry) {
  std::printf("# carry=%s\n", useCarry ? "on" : "off");
  for (const std::string& path : corpusFiles(dir)) {
    const std::string src = readWholeFile(path);
    const ConvertResult conv = looksUtf8(src) ? convertUtf8(src) : convertSjis(src);
    const std::string name = std::filesystem::path(path).filename().string();
    for (const Setting& s : SETTINGS) {
      std::vector<size_t> offsets;
      FillStats stats;
      size_t at = 0;
      while (at < conv.out.size()) {
        offsets.push_back(at);
        int columns = 0;
        const size_t next = fillPage(conv.out, at, s, useCarry, stats, columns);
        if (next <= at) break;
        at = next;
      }
      std::printf("%-28s %-7s pages=%-6zu carrylost=%-4zu hash=%016llx\n", name.c_str(), s.name, offsets.size(),
                  stats.carryLost, static_cast<unsigned long long>(hashOffsets(offsets)));
      for (size_t i = 0; i < offsets.size(); i++) {
        std::printf("    %s %s p%zu %zu\n", name.c_str(), s.name, i + 1, offsets[i]);
      }
    }
  }
  return 0;
}

// --- what the bt= counters are counting ------------------------------------
//
// The reader's four emphasis-mark counters are incremented in two different
// places with two different multiplicities, and neither is "once per note in
// the file":
//
//   variant / noparent  in stripRubyNotation(), which runs for *every source
//                       line of every loadPageAtOffset() call* -- the index
//                       build included. A page that starts in the middle of a
//                       long line scans that line from the page start to its
//                       end, so a note sitting late in a long line is scanned
//                       once per page that begins before it.
//   drawn / collide     in collectBoutenForLine(), which runs only on the
//                       render pass, once per (visual line x span).
//
// This mode prints every note in the converted file with the position it sits
// at inside its own source line, and how many page starts fall at or before
// it -- which is exactly the number of times one index build counts it.
int modeBouten(const std::string& dir, const char* only) {
  for (const std::string& path : corpusFiles(dir)) {
    const std::string name = std::filesystem::path(path).filename().string();
    if (only != nullptr && name.find(only) == std::string::npos) continue;
    const std::string src = readWholeFile(path);
    const ConvertResult conv = looksUtf8(src) ? convertUtf8(src) : convertSjis(src);
    const std::string& text = conv.out;

    for (const Setting& s : SETTINGS) {
      // Page starts, as the reader walks them.
      std::vector<size_t> starts;
      FillStats stats;
      size_t at = 0;
      while (at < text.size()) {
        starts.push_back(at);
        int columns = 0;
        const size_t next = fillPage(text, at, s, /*useCarry=*/true, stats, columns);
        if (next <= at) break;
        at = next;
      }

      size_t notes = 0;
      size_t variants = 0;
      size_t variantScans = 0;
      size_t noteScans = 0;
      // Walk source lines, and the notes inside each.
      size_t lineStart = 0;
      while (lineStart <= text.size()) {
        size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        const size_t lineLen = lineEnd - lineStart;
        for (size_t i = 0; i + aozora::NOTE_OPEN_LEN <= lineLen;) {
          if (memcmp(text.data() + lineStart + i, aozora::NOTE_OPEN, aozora::NOTE_OPEN_LEN) != 0) {
            i++;
            continue;
          }
          const size_t bodyAt = i + aozora::NOTE_OPEN_LEN;
          const size_t close = text.find(aozora::NOTE_CLOSE, lineStart + bodyAt);
          if (close == std::string::npos || close > lineEnd) break;
          bouten::Note note;
          if (bouten::parseNote(text.data() + lineStart + bodyAt, close - (lineStart + bodyAt), note)) {
            // How many page starts land inside this line at or before the note.
            size_t scans = 0;
            for (const size_t st : starts) {
              if (st >= lineStart && st <= lineStart + i) scans++;
            }
            if (scans == 0) scans = 1;  // the line began before the first page start
            notes++;
            noteScans += scans;
            const std::string quoted(text.data() + lineStart + bodyAt + note.quotedAt, note.quotedLen);
            if (note.approximated) {
              variants++;
              variantScans += scans;
            }
            std::printf("  %-20s %-7s %-8s line@%-8zu at+%-6zu linelen=%-6zu scans=%-3zu %s\n", name.c_str(), s.name,
                        note.approximated ? "variant" : "exact", lineStart, i, lineLen, scans, quoted.c_str());
          }
          i = (close - lineStart) + aozora::NOTE_CLOSE_LEN;
        }
        if (lineEnd == text.size()) break;
        lineStart = lineEnd + 1;
      }
      std::printf(
          "%-20s %-7s pages=%-5zu notes=%-5zu variants=%-4zu | one index build counts: notes=%zu variants=%zu\n",
          name.c_str(), s.name, starts.size(), notes, variants, noteScans, variantScans);
    }
  }
  return 0;
}

int main(const int argc, char** argv) {
  if (argc < 3) {
    std::fputs("usage: layout_harness alloc|pages <corpus-dir> [--no-carry]\n", stderr);
    return 1;
  }
  const std::string mode = argv[1];
  const std::string dir = argv[2];
  bool useCarry = true;
  for (int i = 3; i < argc; i++) {
    if (std::strcmp(argv[i], "--no-carry") == 0) useCarry = false;
    if (std::strcmp(argv[i], "--no-reserve") == 0) g_reserve = false;
  }
  if (mode == "alloc") return modeAlloc(dir);
  if (mode == "pages") return modePages(dir, useCarry);
  if (mode == "bouten") return modeBouten(dir, argc > 3 && argv[3][0] != '-' ? argv[3] : nullptr);
  std::fprintf(stderr, "harness: unknown mode %s\n", mode.c_str());
  return 1;
}
