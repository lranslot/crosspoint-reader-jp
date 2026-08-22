#include "Txt.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>

namespace {
// Sniffing prefix. Large enough to get past an ASCII preamble (Aozora headers run
// a few hundred bytes) without reading a meaningful slice of a big file.
constexpr size_t DETECT_BYTES = 4096;
// Streaming chunk for the conversion pass.
constexpr size_t CONVERT_CHUNK = 8192;

constexpr uint32_t ENCODING_MAGIC = 0x54584543;  // "TXEC"
// Bump when the sidecar layout changes or when conversion output changes meaning.
// v2 added the converted size, which is what proves content.u8 is complete.
// v3 added the Aozora strip flag, and changes what content.u8 holds, so every
// existing copy has to be rebuilt.
constexpr uint8_t ENCODING_VERSION = 3;
}  // namespace

Txt::Txt(std::string path, std::string cacheBasePath)
    : filepath(std::move(path)), cacheBasePath(std::move(cacheBasePath)) {
  // Generate cache path from file path hash
  const size_t hash = std::hash<std::string>{}(filepath);
  cachePath = this->cacheBasePath + "/txt_" + std::to_string(hash);
  readPath = filepath;
}

bool Txt::load() {
  if (loaded) {
    return true;
  }

  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR("TXT", "File does not exist: %s", filepath.c_str());
    return false;
  }

  // May redirect readPath at a converted copy in the cache.
  if (!prepareEncoding()) {
    LOG_ERR("TXT", "Failed to prepare encoding for: %s", filepath.c_str());
    return false;
  }

  HalFile file;
  if (!Storage.openFileForRead("TXT", readPath, file)) {
    LOG_ERR("TXT", "Failed to open file: %s", readPath.c_str());
    return false;
  }

  fileSize = file.size();
  file.close();

  loaded = true;
  LOG_DBG("TXT", "Loaded TXT file: %s (%zu bytes%s)", readPath.c_str(), fileSize,
          converted ? ", converted from Shift-JIS" : "");
  return true;
}

// Read the sidecar and decide whether it still describes this file. Returns false
// whenever anything is missing or does not add up, which sends the caller back to
// a fresh detection.
//
// Known limitation: the only thing tying the sidecar to the file is its size, the
// same rule index.bin uses. Replacing a file with one of identical length in a
// different encoding will keep the recorded verdict. Accepted for now.
bool Txt::readEncodingInfo(const size_t sourceSize, uint8_t& outEncoding, bool& outStripped) const {
  HalFile info;
  if (!Storage.openFileForRead("TXT", getEncodingInfoPath(), info)) {
    return false;
  }
  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t cachedSize = 0;
  uint32_t cachedConvertedSize = 0;
  uint8_t encoding = 0;
  uint8_t stripped = 0;
  serialization::readPod(info, magic);
  serialization::readPod(info, version);
  serialization::readPod(info, cachedSize);
  serialization::readPod(info, cachedConvertedSize);
  serialization::readPod(info, encoding);
  serialization::readPod(info, stripped);
  info.close();

  if (magic != ENCODING_MAGIC || version != ENCODING_VERSION || cachedSize != sourceSize) {
    LOG_DBG("TXT", "Encoding sidecar stale (size %u, now %zu)", cachedSize, sourceSize);
    return false;
  }

  // Only a file with a cache copy has a second artefact to check — either it was
  // transcoded, or it was UTF-8 with markup stripped out. The sidecar is written
  // last and records how long content.u8 should be, so a conversion cut short by a
  // full or removed card fails here rather than presenting a truncated book.
  if (encoding == static_cast<uint8_t>(TextEncoding::ShiftJis) || stripped != 0) {
    size_t actual = 0;
    HalFile convertedFile;
    if (Storage.openFileForRead("TXT", getConvertedPath(), convertedFile)) {
      actual = convertedFile.size();
      convertedFile.close();
    }
    if (actual == 0 || actual != cachedConvertedSize) {
      LOG_DBG("TXT", "Converted file is %zu bytes, sidecar says %u", actual, cachedConvertedSize);
      return false;
    }
  }

  outEncoding = encoding;
  outStripped = (stripped != 0);
  return true;
}

// Written only once the encoding is settled — and, for Shift-JIS, once the whole
// input has been converted and flushed. Its presence is therefore the proof that
// this file needs no further work on open.
bool Txt::writeEncodingInfo(const size_t sourceSize, const size_t convertedSize, const uint8_t encoding,
                            const bool stripped) const {
  HalFile out;
  if (!Storage.openFileForWrite("TXT", getEncodingInfoPath(), out)) {
    LOG_ERR("TXT", "Could not open encoding sidecar for writing");
    return false;
  }
  const uint32_t magic = ENCODING_MAGIC;
  const uint8_t version = ENCODING_VERSION;
  const uint32_t size32 = static_cast<uint32_t>(sourceSize);
  const uint32_t convertedSize32 = static_cast<uint32_t>(convertedSize);
  // write() directly rather than writePod(): the byte layout is identical, but this
  // way a short write is caught. Sequence must match the reads above.
  const uint8_t strippedByte = stripped ? 1 : 0;
  bool ok = out.write(&magic, sizeof(magic)) == sizeof(magic);
  ok = ok && out.write(&version, sizeof(version)) == sizeof(version);
  ok = ok && out.write(&size32, sizeof(size32)) == sizeof(size32);
  ok = ok && out.write(&convertedSize32, sizeof(convertedSize32)) == sizeof(convertedSize32);
  ok = ok && out.write(&encoding, sizeof(encoding)) == sizeof(encoding);
  ok = ok && out.write(&strippedByte, sizeof(strippedByte)) == sizeof(strippedByte);
  out.flush();
  ok = out.close() && ok;
  if (!ok) {
    LOG_ERR("TXT", "Failed to write encoding sidecar");
    Storage.remove(getEncodingInfoPath().c_str());
  }
  return ok;
}

// Decide the source encoding and make sure readPath points at UTF-8 bytes.
//
// The invariant is that a sidecar exists for every file whose encoding has been
// settled, whatever that verdict was. A missing sidecar means detection has never
// reached a conclusion, so an unconverted UTF-8 file is not re-probed on every open
// (and does not keep triggering the first-open popup in ReaderActivity::loadTxt).
bool Txt::prepareEncoding() {
  HalFile src;
  if (!Storage.openFileForRead("TXT", filepath, src)) {
    return false;
  }
  const size_t sourceSize = src.size();

  uint8_t cachedEncoding = 0;
  bool cachedStripped = false;
  if (readEncodingInfo(sourceSize, cachedEncoding, cachedStripped)) {
    src.close();
    const bool isSjis = cachedEncoding == static_cast<uint8_t>(TextEncoding::ShiftJis);
    LOG_INF("TXT", "Encoding from sidecar: %s%s (detection skipped)", isSjis ? "Shift-JIS" : "UTF-8",
            cachedStripped ? ", Aozora stripped" : "");
    // A UTF-8 file can have a cache copy too, when it only needed markup removing.
    if (isSjis || cachedStripped) {
      readPath = getConvertedPath();
      converted = true;
      stripAozora = cachedStripped;
    }
    return true;
  }

  TextEncoding encoding = TextEncoding::Utf8;
  if (sourceSize == 0) {
    // Not a failure but a conclusion: there is nothing to convert. Recording it
    // keeps the invariant, and the size check re-runs detection if content appears.
    src.close();
  } else {
    const size_t probeLen = std::min(DETECT_BYTES, sourceSize);
    auto probe = makeUniqueNoThrow<uint8_t[]>(probeLen);
    if (!probe) {
      LOG_ERR("TXT", "OOM: %zu bytes for encoding probe", probeLen);
      return false;
    }
    const int read = src.read(probe.get(), probeLen);
    src.close();
    if (read <= 0) {
      // An I/O failure is not a verdict — write nothing, so the next open retries.
      LOG_ERR("TXT", "Could not read %zu bytes to detect encoding", probeLen);
      return false;
    }
    encoding = detectTextEncoding(probe.get(), static_cast<size_t>(read));
    LOG_INF("TXT", "Encoding detected: %s (probed %d bytes)",
            encoding == TextEncoding::ShiftJis ? "Shift-JIS" : "UTF-8", read);

    // The legend block and the notation are only recognisable once the probe is
    // UTF-8; searching the raw Shift-JIS bytes would never match. The tail of the
    // probe may cut a pair in half, but the marker sits within the first kilobyte,
    // so dropping those few bytes (atEnd = false) costs nothing.
    if (encoding == TextEncoding::ShiftJis) {
      std::string probeUtf8;
      probeUtf8.reserve(static_cast<size_t>(read) * 3 / 2);
      cp932ToUtf8(probe.get(), static_cast<size_t>(read), probeUtf8, /*flush=*/false);
      detectAozora(probeUtf8.data(), probeUtf8.size());
    } else {
      detectAozora(reinterpret_cast<const char*>(probe.get()), static_cast<size_t>(read));
    }
  }

  setupCacheDir();
  // Never leave a stale verdict behind while a new one is being established.
  Storage.remove(getEncodingInfoPath().c_str());

  // A UTF-8 file with Aozora markup still needs a cache copy — there is nothing to
  // transcode, but the markup has to come out somewhere, and doing it at display
  // time would move every byte offset the page index and checkbox writes depend on.
  const bool needsCacheCopy = (encoding == TextEncoding::ShiftJis) || stripAozora;

  size_t convertedSize = 0;
  if (needsCacheCopy) {
    const bool produced = (encoding == TextEncoding::ShiftJis) ? convertToUtf8(sourceSize, convertedSize)
                                                               : stripUtf8ToCache(sourceSize, convertedSize);
    if (!produced) {
      LOG_ERR("TXT", "Cache copy failed, reading source as-is");
      Storage.remove(getConvertedPath().c_str());
      return true;  // fall back to the original rather than refusing to open the file
    }
    // The page index describes wrapping over the *converted* bytes, so an index
    // built before this conversion is meaningless.
    const std::string indexPath = cachePath + "/index.bin";
    if (Storage.exists(indexPath.c_str())) {
      Storage.remove(indexPath.c_str());
      LOG_DBG("TXT", "Dropped stale page index after conversion");
    }
  }

  if (!writeEncodingInfo(sourceSize, convertedSize, static_cast<uint8_t>(encoding), stripAozora)) {
    // Without the sidecar the conversion cannot be trusted next time; drop it and
    // read the source, rather than leaving an unverifiable copy behind.
    if (needsCacheCopy) {
      Storage.remove(getConvertedPath().c_str());
    }
    return true;
  }

  if (needsCacheCopy) {
    readPath = getConvertedPath();
    converted = true;
  }
  return true;
}

void Txt::detectAozora(const char* probeUtf8, const size_t len) {
  stripAozora = aozoraLooksLikeAozora(probeUtf8, len);
  if (!stripAozora) {
    return;
  }
  // Both rule lines must be inside the probe. If only one is (or neither), the
  // legend is left in place and only the inline notation is removed — better a
  // visible legend than a book with its opening chapters eaten.
  aozoraHeaderFound = aozoraFindHeaderRange(probeUtf8, len, aozoraHeaderFirst, aozoraHeaderLast);
  LOG_INF("TXT", "Aozora markup detected (legend lines %s)",
          aozoraHeaderFound ? "located" : "NOT found - keeping legend");
}

// Stream the source through the CP932 table into <cachePath>/content.u8. No header
// is written: offset 0 of the output is offset 0 of the text.
//
// Returns false unless every byte of the source was consumed and written, so the
// caller can treat success as "the conversion is complete" before recording it.
bool Txt::convertToUtf8(const size_t sourceSize, size_t& convertedSize) {
  const unsigned long startMs = millis();
  convertedSize = 0;

  HalFile in;
  if (!Storage.openFileForRead("TXT", filepath, in)) {
    return false;
  }
  HalFile out;
  if (!Storage.openFileForWrite("TXT", getConvertedPath(), out)) {
    return false;
  }

  auto buffer = makeUniqueNoThrow<uint8_t[]>(CONVERT_CHUNK);
  if (!buffer) {
    LOG_ERR("TXT", "OOM: %zu bytes for conversion buffer", CONVERT_CHUNK);
    return false;
  }

  std::string encoded;
  encoded.reserve(CONVERT_CHUNK * 3 / 2);  // kana and kanji grow 1:2 and 2:3
  std::string stripped;

  // Second, independent carry. The first (inside cp932ToUtf8) holds back a
  // Shift-JIS pair split across a read; this one holds back markup split across
  // one, and works on the UTF-8 the first stage produced.
  AozoraStrip strip;
  if (stripAozora && aozoraHeaderFound) {
    strip.setHeaderRange(aozoraHeaderFirst, aozoraHeaderLast);
  }

  size_t carry = 0;  // bytes held back because a pair straddled the chunk edge
  size_t inputBytes = 0;
  size_t outputBytes = 0;
  bool ok = true;

  while (true) {
    const int read = in.read(buffer.get() + carry, CONVERT_CHUNK - carry);
    if (read < 0) {
      ok = false;
      break;
    }
    const size_t avail = carry + static_cast<size_t>(read);
    if (avail == 0) {
      break;
    }
    const bool atEnd = (read == 0) || (static_cast<size_t>(read) < CONVERT_CHUNK - carry);

    encoded.clear();
    const size_t consumed = cp932ToUtf8(buffer.get(), avail, encoded, atEnd);

    const std::string* payload = &encoded;
    if (stripAozora) {
      stripped.clear();
      strip.process(encoded.data(), encoded.size(), stripped, atEnd);
      payload = &stripped;
    }

    if (!payload->empty() && out.write(payload->data(), payload->size()) != payload->size()) {
      LOG_ERR("TXT", "Conversion write failed");
      ok = false;
      break;
    }
    inputBytes += consumed;
    outputBytes += payload->size();

    carry = avail - consumed;
    if (carry > 0) {
      // Move the unconsumed tail to the front for the next read.
      memmove(buffer.get(), buffer.get() + consumed, carry);
    }
    if (atEnd) {
      break;
    }
  }

  out.flush();
  const bool closedCleanly = out.close();
  in.close();

  if (!ok || !closedCleanly) {
    LOG_ERR("TXT", "Conversion aborted after %zu/%zu input bytes", inputBytes, sourceSize);
    return false;
  }
  // Anything short of the whole file means the read loop gave up early — treat it
  // as a failure so no sidecar is written and the next open retries.
  if (inputBytes != sourceSize) {
    LOG_ERR("TXT", "Conversion consumed %zu of %zu bytes; discarding", inputBytes, sourceSize);
    return false;
  }
  // Re-stat the file rather than trusting the running total: this is the number the
  // sidecar records and the next open validates against.
  HalFile check;
  if (!Storage.openFileForRead("TXT", getConvertedPath(), check)) {
    LOG_ERR("TXT", "Converted file missing after write");
    return false;
  }
  convertedSize = check.size();
  check.close();
  if (convertedSize != outputBytes) {
    LOG_ERR("TXT", "Converted file is %zu bytes, expected %zu", convertedSize, outputBytes);
    return false;
  }

  LOG_INF("TXT", "Shift-JIS -> UTF-8: %zu -> %zu bytes in %lu ms", inputBytes, convertedSize, millis() - startMs);
  if (stripAozora) {
    logAozoraResult(sourceSize, convertedSize, strip.stats());
  }
  return true;
}

// The source is already UTF-8; only the markup has to come out. Same output file
// and same completeness rules as convertToUtf8(), minus the transcoding stage.
bool Txt::stripUtf8ToCache(const size_t sourceSize, size_t& convertedSize) {
  const unsigned long startMs = millis();
  convertedSize = 0;

  HalFile in;
  if (!Storage.openFileForRead("TXT", filepath, in)) {
    return false;
  }
  HalFile out;
  if (!Storage.openFileForWrite("TXT", getConvertedPath(), out)) {
    return false;
  }

  auto buffer = makeUniqueNoThrow<uint8_t[]>(CONVERT_CHUNK);
  if (!buffer) {
    LOG_ERR("TXT", "OOM: %zu bytes for strip buffer", CONVERT_CHUNK);
    return false;
  }

  AozoraStrip strip;
  if (aozoraHeaderFound) {
    strip.setHeaderRange(aozoraHeaderFirst, aozoraHeaderLast);
  }

  std::string stripped;
  size_t inputBytes = 0;
  size_t outputBytes = 0;
  bool ok = true;

  while (true) {
    const int read = in.read(buffer.get(), CONVERT_CHUNK);
    if (read < 0) {
      ok = false;
      break;
    }
    if (read == 0) {
      // Nothing left: flush whatever the scan is still holding.
      stripped.clear();
      strip.process("", 0, stripped, /*atEnd=*/true);
      if (!stripped.empty() && out.write(stripped.data(), stripped.size()) != stripped.size()) {
        ok = false;
      }
      outputBytes += stripped.size();
      break;
    }
    inputBytes += static_cast<size_t>(read);
    const bool atEnd = inputBytes >= sourceSize;

    stripped.clear();
    strip.process(reinterpret_cast<const char*>(buffer.get()), static_cast<size_t>(read), stripped, atEnd);

    if (!stripped.empty() && out.write(stripped.data(), stripped.size()) != stripped.size()) {
      LOG_ERR("TXT", "Strip write failed");
      ok = false;
      break;
    }
    outputBytes += stripped.size();
    if (atEnd) {
      break;
    }
  }

  out.flush();
  const bool closedCleanly = out.close();
  in.close();

  if (!ok || !closedCleanly) {
    LOG_ERR("TXT", "Strip aborted after %zu/%zu input bytes", inputBytes, sourceSize);
    return false;
  }
  if (inputBytes != sourceSize) {
    LOG_ERR("TXT", "Strip consumed %zu of %zu bytes; discarding", inputBytes, sourceSize);
    return false;
  }

  HalFile check;
  if (!Storage.openFileForRead("TXT", getConvertedPath(), check)) {
    LOG_ERR("TXT", "Stripped file missing after write");
    return false;
  }
  convertedSize = check.size();
  check.close();
  if (convertedSize != outputBytes) {
    LOG_ERR("TXT", "Stripped file is %zu bytes, expected %zu", convertedSize, outputBytes);
    return false;
  }

  LOG_INF("TXT", "Aozora strip (UTF-8): %zu -> %zu bytes in %lu ms", inputBytes, convertedSize, millis() - startMs);
  logAozoraResult(sourceSize, convertedSize, strip.stats());
  return true;
}

// Counts the notation still present in the output as well as what was removed:
// a few survivors are expected (deliberately broken markup in the test file, and
// ※ used as an ordinary character), and the totals are how a regression shows up.
void Txt::logAozoraResult(const size_t sourceSize, const size_t convertedSize, const AozoraStrip::Stats& stats) const {
  size_t rubyOpen = 0, rubyClose = 0, noteOpen = 0, range = 0, kome = 0;
  HalFile f;
  if (Storage.openFileForRead("TXT", getConvertedPath(), f)) {
    auto buf = makeUniqueNoThrow<uint8_t[]>(CONVERT_CHUNK);
    if (buf) {
      // Markers are 3 or 6 bytes, so overlap the reads to avoid missing one that
      // lands on a boundary.
      std::string tail;
      while (true) {
        const int read = f.read(buf.get(), CONVERT_CHUNK);
        if (read <= 0) break;
        std::string window = tail;
        window.append(reinterpret_cast<const char*>(buf.get()), static_cast<size_t>(read));
        for (size_t i = 0; i < window.size(); i++) {
          const char* p = window.data() + i;
          const size_t remain = window.size() - i;
          if (remain >= 6 && memcmp(p, "\xEF\xBC\xBB\xEF\xBC\x83", 6) == 0) noteOpen++;
          if (remain >= 3 && memcmp(p, "\xE3\x80\x8A", 3) == 0) rubyOpen++;
          if (remain >= 3 && memcmp(p, "\xE3\x80\x8B", 3) == 0) rubyClose++;
          if (remain >= 3 && memcmp(p, "\xEF\xBD\x9C", 3) == 0) range++;
          if (remain >= 3 && memcmp(p, "\xE2\x80\xBB", 3) == 0) kome++;
        }
        if (window.size() >= 8) {
          tail = window.substr(window.size() - 8);
        } else {
          tail = window;
        }
        if (static_cast<size_t>(read) < CONVERT_CHUNK) break;
      }
    }
    f.close();
  }

  const std::string& path = filepath;
  char buf[512];
  const int n = snprintf(buf, sizeof(buf),
                         "file: %s\ndetected: aozora\nsource bytes: %zu -> content.u8 bytes: %zu\n"
                         "removed: ruby=%lu range=%lu note=%lu headerLines=%lu\n"
                         "gaiji: restored=%lu geta=%lu\n"
                         "remaining: \xE3\x80\x8A=%zu \xE3\x80\x8B=%zu \xEF\xBC\xBB\xEF\xBC\x83=%zu "
                         "\xEF\xBD\x9C=%zu \xE2\x80\xBB=%zu\n\n",
                         path.c_str(), sourceSize, convertedSize, static_cast<unsigned long>(stats.ruby),
                         static_cast<unsigned long>(stats.range), static_cast<unsigned long>(stats.note),
                         static_cast<unsigned long>(stats.headerLines),
                         static_cast<unsigned long>(stats.gaijiRestored),
                         static_cast<unsigned long>(stats.gaijiGeta), rubyOpen, rubyClose, noteOpen, range, kome);

  HalFile log = Storage.open("/aozora-strip.txt", O_WRITE | O_CREAT | O_AT_END);
  if (log && n > 0) {
    log.write(buf, static_cast<size_t>(n));
    log.flush();
    log.close();
  }
}

std::string Txt::getTitle() const {
  // Extract filename without path and extension
  size_t lastSlash = filepath.find_last_of('/');
  std::string filename = (lastSlash != std::string::npos) ? filepath.substr(lastSlash + 1) : filepath;

  // Remove .txt extension
  if (FsHelpers::hasTxtExtension(filename)) {
    filename.resize(filename.length() - 4);
  }

  return filename;
}

void Txt::setupCacheDir() const {
  if (!Storage.exists(cacheBasePath.c_str())) {
    Storage.mkdir(cacheBasePath.c_str());
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }
}

std::string Txt::findCoverImage() const {
  // Get the folder containing the txt file
  size_t lastSlash = filepath.find_last_of('/');
  std::string folder = (lastSlash != std::string::npos) ? filepath.substr(0, lastSlash) : "";
  if (folder.empty()) {
    folder = "/";
  }

  // Get the base filename without extension (e.g., "mybook" from "/books/mybook.txt")
  std::string baseName = getTitle();

  // Image extensions to try
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  // First priority: look for image with same name as txt file (e.g., mybook.jpg)
  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) {
      LOG_DBG("TXT", "Found matching cover image: %s", coverPath.c_str());
      return coverPath;
    }
  }

  // Fallback: look for cover image files
  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) {
        LOG_DBG("TXT", "Found fallback cover image: %s", coverPath.c_str());
        return coverPath;
      }
    }
  }

  return "";
}

std::string Txt::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

bool Txt::generateCoverBmp() const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath().c_str())) {
    return true;
  }

  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    LOG_DBG("TXT", "No cover image found for TXT file");
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  if (FsHelpers::hasBmpExtension(coverImagePath)) {
    // Copy BMP file to cache
    LOG_DBG("TXT", "Copying BMP cover image to cache");
    HalFile src, dst;
    if (!Storage.openFileForRead("TXT", coverImagePath, src)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", getCoverBmpPath(), dst)) {
      return false;
    }
    uint8_t buffer[1024];
    while (src.available()) {
      size_t bytesRead = src.read(buffer, sizeof(buffer));
      dst.write(buffer, bytesRead);
    }
    LOG_DBG("TXT", "Copied BMP cover to cache");
    return true;
  } else if (FsHelpers::hasJpgExtension(coverImagePath)) {
    // Convert JPG/JPEG to BMP (same approach as Epub)
    LOG_DBG("TXT", "Generating BMP from JPG cover image");
    HalFile coverJpg, coverBmp;
    if (!Storage.openFileForRead("TXT", coverImagePath, coverJpg)) {
      return false;
    }
    if (!Storage.openFileForWrite("TXT", getCoverBmpPath(), coverBmp)) {
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);

    if (!success) {
      LOG_ERR("TXT", "Failed to generate BMP from JPG cover image");
      Storage.remove(getCoverBmpPath().c_str());
    } else {
      LOG_DBG("TXT", "Generated BMP from JPG cover image");
    }
    return success;
  }

  // PNG files are not supported (would need a PNG decoder)
  LOG_ERR("TXT", "Cover image format not supported (only BMP/JPG/JPEG)");
  return false;
}

bool Txt::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("TXT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("TXT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("TXT", "Cache cleared successfully");
  return true;
}

bool Txt::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return false;
  }

  // Reuse the handle when a sequential run is in progress. The caller still passes
  // an arbitrary offset, so the seek happens either way — only the open is saved.
  if (sequentialFile) {
    if (!sequentialFile.seek(offset)) {
      return false;
    }
    return sequentialFile.read(buffer, length) > 0;
  }

  HalFile file;
  // readPath, not filepath: offsets are measured against the converted copy when
  // the source was Shift-JIS.
  if (!Storage.openFileForRead("TXT", readPath, file)) {
    return false;
  }

  if (!file.seek(offset)) {
    return false;
  }

  size_t bytesRead = file.read(buffer, length);
  return bytesRead > 0;
}

bool Txt::beginSequentialRead() {
  if (!loaded) {
    return false;
  }
  if (sequentialFile) {
    return true;  // already open; scopes are not nested today, but be tolerant
  }
  if (!Storage.openFileForRead("TXT", readPath, sequentialFile)) {
    LOG_ERR("TXT", "Sequential read unavailable for %s, reopening per read", readPath.c_str());
    return false;
  }
  return true;
}

void Txt::endSequentialRead() {
  if (sequentialFile) {
    sequentialFile.close();
  }
}

bool Txt::writeByteAt(const size_t offset, const char value) {
  if (converted) {
    LOG_ERR("TXT", "Refusing to write into a converted file: %s", filepath.c_str());
    return false;
  }
  // A read handle must not be open on the same path while it is reopened O_RDWR.
  endSequentialRead();

  HalFile f = Storage.open(readPath.c_str(), O_RDWR);
  if (!f) {
    LOG_ERR("TXT", "writeByteAt: open failed");
    return false;
  }
  if (!f.seek(offset)) {
    LOG_ERR("TXT", "writeByteAt: seek to %zu failed", offset);
    f.close();
    return false;
  }
  const bool ok = (f.write(&value, 1) == 1);
  f.flush();
  f.close();
  if (!ok) {
    LOG_ERR("TXT", "writeByteAt: write failed at %zu", offset);
  }
  return ok;
}
