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
};
