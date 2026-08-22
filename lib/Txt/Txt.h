#pragma once

#include <HalStorage.h>

#include <memory>
#include <string>

class Txt {
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  // Path actually read from. Same as filepath for UTF-8, or the converted copy in
  // the cache when the source turned out to be Shift-JIS.
  std::string readPath;
  bool loaded = false;
  bool converted = false;
  size_t fileSize = 0;  // size of readPath, i.e. what offsets are measured against

  // Open only for the duration of a sequential read run. Mutable because seeking
  // and reading it is an implementation detail of the logically const readContent.
  mutable HalFile sequentialFile;

  // Detect the source encoding and, for Shift-JIS, produce (or reuse) a UTF-8 copy
  // in the cache. Returns false only on I/O failure; a file we decline to convert
  // is a success that leaves readPath alone.
  bool prepareEncoding();
  [[nodiscard]] std::string getConvertedPath() const { return cachePath + "/content.u8"; }
  [[nodiscard]] std::string getEncodingInfoPath() const { return cachePath + "/encoding.bin"; }
  bool convertToUtf8(size_t sourceSize, size_t& convertedSize);
  // Sidecar accessors. Its presence is the invariant "this file has already been
  // classified"; absence means detection has never reached a conclusion for it.
  bool readEncodingInfo(size_t sourceSize, uint8_t& outEncoding) const;
  bool writeEncodingInfo(size_t sourceSize, size_t convertedSize, uint8_t encoding) const;

 public:
  explicit Txt(std::string path, std::string cacheBasePath);

  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  // True when the text being read is a converted copy rather than the file itself.
  // Callers that write back into the source (e.g. the Markdown checkbox toggle)
  // must not do so: their offsets refer to the converted bytes.
  [[nodiscard]] bool isConverted() const { return converted; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] size_t getFileSize() const { return fileSize; }

  void setupCacheDir() const;
  bool clearCache() const;

  // Cover image support - looks for cover.bmp/jpg/jpeg/png in same folder as txt file
  [[nodiscard]] std::string getCoverBmpPath() const;
  [[nodiscard]] bool generateCoverBmp() const;
  [[nodiscard]] std::string findCoverImage() const;

  // Read content from file
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;

  // Keep one handle open across a run of reads instead of reopening per call.
  // Worth it for the index build, which reads the whole file a page at a time.
  //
  // Deliberately not held for the life of the activity: the markdown checkbox
  // toggle reopens the same path O_RDWR to write a byte, and leaving a read handle
  // open across that is asking SdFat for trouble. The index build and a toggle
  // never overlap, so the window stays inside buildPageIndex().
  //
  // Failing to begin is not an error — reads simply fall back to reopening.
  bool beginSequentialRead();
  void endSequentialRead();

  // Overwrite a single byte in place. Closes any sequential read handle first, so
  // a read handle and an O_RDWR handle are never open on the same file at once.
  //
  // Refuses on converted files: the offset would belong to the UTF-8 copy in the
  // cache, and writing it into the Shift-JIS source would corrupt it. The reader
  // also guards this via isMarkdown, so this is the second line of defence.
  bool writeByteAt(size_t offset, char value);

  // Scope guard so every exit from the index build closes the handle.
  class SequentialReadScope {
    Txt& txt;

   public:
    explicit SequentialReadScope(Txt& t) : txt(t) { txt.beginSequentialRead(); }
    ~SequentialReadScope() { txt.endSequentialRead(); }
    SequentialReadScope(const SequentialReadScope&) = delete;
    SequentialReadScope& operator=(const SequentialReadScope&) = delete;
  };
};
