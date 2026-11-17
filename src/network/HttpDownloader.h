#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  // Keeps one HTTPS connection alive across several calls into this namespace.
  //
  // SecureHttpClient implements keep-alive and its header says what for: "a
  // burst of requests -- an OPDS crawl, a sync exchange, a multi-file download
  // -- pays for one TLS handshake instead of one per request"
  // (freeink-sdk/.../SecureHttpClient.h). Nothing was getting that, because the
  // client was constructed inside the redirect loop and destroyed on every hop,
  // and downloadToFile() is called once per file on top of that. The feature
  // was fully implemented and disabled by object lifetime (spec 8.8-39).
  //
  // Hold one of these for as long as a burst lasts -- a whole font family, not
  // a single file -- and every request that targets the same scheme://host:port
  // as the last one reuses its socket and its TLS session. Requests to a
  // different host reconnect exactly as before.
  //
  // RAII, and the destructor closing the socket is the point rather than
  // tidiness: an idle kept-alive connection holds a wolfSSL session plus lwip's
  // per-socket send and receive buffers (5,744 + 5,760 bytes configured), which
  // is the cost this whole exercise is trying to reduce. Never make one static
  // or long-lived.
  //
  // Nestable only in the sense that an inner scope is a no-op; the outermost
  // one owns the connection. Safe to construct when the build has no wolfSSL:
  // it does nothing.
  class KeepAliveScope {
   public:
    KeepAliveScope();
    ~KeepAliveScope();
    KeepAliveScope(const KeepAliveScope&) = delete;
    KeepAliveScope& operator=(const KeepAliveScope&) = delete;

    // False when the shared client could not be allocated. Not an error: every
    // request then opens its own connection, exactly as before this existed.
    [[nodiscard]] bool active() const { return active_; }

   private:
    bool active_ = false;
    bool owner_ = false;
  };
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");
};
