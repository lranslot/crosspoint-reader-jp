#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>

#include <functional>
#include <string>

#if defined(FREEINK_NET_WOLFSSL)
#include <HeapDiag.h>
#include <SecureHttpClient.h>

#include "NetDiag.h"

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#else
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif

namespace {
#if !defined(FREEINK_NET_WOLFSSL)
// RX holds the response headers. Smaller buffers leave enough contiguous heap
// for mbedTLS on redirect-heavy OPDS feeds while still preserving the headers
// we read directly (Location, Content-Length).
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 512;
#endif
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;
constexpr int MAX_REDIRECTS = 5;

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

#if defined(FREEINK_NET_WOLFSSL)
// Routes SecureClient's four TLS samples into /heap.txt. Installed once; the
// SDK holds a plain function pointer and fires it only if one is set, so this
// costs nothing in builds that never call a download.
//
// It lives here rather than in the SDK because the SDK must not depend on the
// application's diagnostics, and the samples cannot be taken from here: the
// receive buffer is grown and freed inside wolfSSL_read, between two calls this
// file makes.
void tlsHeapProbe(const char* tag, const char* detail) { heapdiag::sample(tag, detail); }

// The connection shared by everything inside a KeepAliveScope, or null when no
// scope is open. Not static-lifetime by accident: the scope's destructor
// releases it, because an idle HTTPS connection costs a wolfSSL session plus
// lwip's per-socket buffers.
std::unique_ptr<freeink::SecureHttpClient> g_shared;

// Applies the per-request settings that used to be applied to a fresh object.
// begin() resets headers, body and status but deliberately does NOT close the
// connection -- ensureConnected() decides that by comparing scheme/host/port --
// so a reused client needs exactly this and nothing else.
void configureRequest(freeink::SecureHttpClient& http, const std::string& url, const std::string& username,
                      const std::string& password, bool& ok) {
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
    ok = false;
    return;
  }
  // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
  // append a second User-Agent header, which strict servers reject (aiohttp
  // answers 400 "Duplicate 'User-Agent' header found").
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    const std::string credentials = username + ":" + password;
    const String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
  }
  ok = true;
}

// Why a transfer ended, written where the answer is actually known.
//
// The activity above can only see one of four DownloadError values, and three
// different failures collapse into HTTP_ERROR: a negative status from the
// client, a status that is neither 200 nor a redirect, and a body that stopped
// short. Burn 16 showed what that costs -- the screen said "Download failed:
// Pretendard_10.cpfont" and the card held a 0-byte file, and nothing recorded
// whether the server refused, the stream broke or the write failed. The serial
// LOG_ERR lines say which, and serial is not available on this device.
//
// got=/want= is the second half of the answer: 0 of 2,544,893 is a different
// fault from 2,100,000 of 2,544,893.
void dlResult(const char* why, const int status, const Sink& sink) {
  char detail[112];
  snprintf(detail, sizeof(detail), "why=%s status=%d got=%u want=%u", why, status, (unsigned)sink.downloaded,
           (unsigned)sink.total);
  heapdiag::sample("dl-result", detail);
}

HttpDownloader::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password, Sink& sink) {
  std::string url = startUrl;

  // Idempotent: the probe is a single pointer, and reinstalling it is cheaper
  // than tracking whether some other entry point got there first.
  freeink::SecureClient::setHeapProbe(&tlsHeapProbe);

  // One client for every hop of this request, and -- when a KeepAliveScope is
  // open -- for every request in the burst. Previously this was constructed
  // inside the loop, so the keep-alive socket SecureHttpClient maintains was
  // destroyed on each redirect and again on each file: the feature was
  // implemented, documented, and never once in effect (spec 8.8-39).
  //
  // github.com redirecting to its CDN still costs a second handshake, because
  // the host changes and connectionMatches() correctly refuses to reuse. What
  // this recovers is every request after the first to the SAME host, which for
  // a font family is every file but one.
  freeink::SecureHttpClient local;
  freeink::SecureHttpClient& http = g_shared ? *g_shared : local;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    bool configured = false;
    configureRequest(http, url, username, password, configured);
    if (!configured) return HttpDownloader::HTTP_ERROR;

    LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
    const int status = http.GET(
        [&http, &sink](const uint8_t* data, size_t len) {
          if (http.getStatus() != 200) return true;
          if (sink.total == 0 && http.hasContentLength()) sink.total = http.getContentLength();
          if (sink.downloaded == 0) {
            // The first body byte of this response. For a font this is the
            // point at which the connection is provably carrying payload, so
            // it is also the one place the PCB counts mean something.
            char detail[128];
            char pcbs[96];
            net_diag::describeTcpPcbs(pcbs, sizeof(pcbs));
            snprintf(detail, sizeof(detail), "want=%u %s", (unsigned)sink.total, pcbs);
            heapdiag::sample("dl-first-bytes", detail);
          }
          if (!sink.write(data, len)) return false;
          sink.downloaded += len;
          if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
          return true;
        },
        [&sink]() { return sink.cancelFlag && *sink.cancelFlag; });

    if (http.aborted()) {
      dlResult("cancelled", status, sink);
      return HttpDownloader::ABORTED;
    }
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed: %s", url.c_str());
      dlResult("client-failed", status, sink);
      return HttpDownloader::HTTP_ERROR;
    }
    if (isRedirect(status)) {
      const std::string location = http.getHeader("location");
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, url)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        dlResult("bad-redirect", status, sink);
        return HttpDownloader::HTTP_ERROR;
      }
      continue;
    }
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      dlResult("bad-status", status, sink);
      return HttpDownloader::HTTP_ERROR;
    }
    if (http.callbackAborted()) {
      // sink.write returned false: the SD write failed, not the network.
      dlResult("sd-write-failed", status, sink);
      return HttpDownloader::FILE_ERROR;
    }
    if (!http.responseComplete()) {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      dlResult("short-body", status, sink);
      return HttpDownloader::HTTP_ERROR;
    }
    dlResult("ok", status, sink);
    return HttpDownloader::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  dlResult("too-many-redirects", -1, sink);
  return HttpDownloader::HTTP_ERROR;
}
#endif

#if !defined(FREEINK_NET_WOLFSSL)
// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  // Verify HTTPS against the bundled CA roots. This build has esp-tls
  // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
  // up at all; the model is public servers over verified https and local
  // servers over plain http (esp_http_client picks the transport from the URL
  // scheme, so http:// needs no cert config). The prior setInsecure() worked
  // only because Arduino's ssl_client drives mbedtls directly.
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return HttpDownloader::HTTP_ERROR;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < MAX_REDIRECTS; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    esp_http_client_close(client);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}
#endif  // !FREEINK_NET_WOLFSSL

// All HTTP(S) fetches go through wolfSSL when it is the active TLS stack: it
// speaks TLS 1.3 and reads large bodies from servers where the esp_http_client/
// mbedTLS path fails to connect or stalls mid-stream. Plain-http URLs still use a
// WiFiClient inside runGetWolf, so this is safe for non-TLS targets too.
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink) {
#if defined(FREEINK_NET_WOLFSSL)
  return runGetWolf(url, username, password, sink);
#else
  return runGet(url, username, password, sink);
#endif
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetSecure(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, sink) == OK;
}

// --- KeepAliveScope ---------------------------------------------------------
//
// Reports the PCB counts either side of the burst as well, because the question
// left open by burn 14 is what a closed connection keeps hold of: the receive
// buffer turned out to be 2,065 bytes while the largest free block fell 23,552,
// so the cost is not the record buffer. See net_diag::describeTcpPcbs.
#if defined(FREEINK_NET_WOLFSSL)
HttpDownloader::KeepAliveScope::KeepAliveScope() {
  if (g_shared) return;  // an outer scope already owns the connection
  g_shared = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!g_shared) {
    // Not fatal, and not silent: every request falls back to its own
    // connection, which is exactly the behaviour this replaced.
    LOG_ERR("HTTP", "OOM: keep-alive client, falling back to one connection per request");
    return;
  }
  owner_ = true;
  active_ = true;
  // Nothing is connected yet -- this only allocated the client -- so the counts
  // here are the baseline the pair in the destructor is read against, not a
  // measurement of an open connection.
  char detail[112];
  char pcbs[96];
  net_diag::describeTcpPcbs(pcbs, sizeof(pcbs));
  snprintf(detail, sizeof(detail), "baseline (nothing open yet) %s", pcbs);
  heapdiag::sample("http-keepalive-open", detail);
}

HttpDownloader::KeepAliveScope::~KeepAliveScope() {
  if (!owner_) return;

  // Two samples, and the pair is the measurement. The single sample burn 16
  // took was after end() had already closed the socket, so its act=0 was
  // correct and useless. This one runs first, while the kept-alive connection
  // is still up, and is the positive control for the walk itself.
  char detail[96];
  net_diag::describeTcpPcbs(detail, sizeof(detail));
  heapdiag::sample("http-keepalive-live", detail);

  // end() closes the socket; destroying the client would too, but saying it
  // explicitly is the contract this class exists for.
  g_shared->end();
  g_shared.reset();

  // And this one after. act should fall and tw should rise, which is the
  // TIME-WAIT hypothesis stated as a before/after rather than argued.
  net_diag::describeTcpPcbs(detail, sizeof(detail));
  heapdiag::sample("http-keepalive-close", detail);
}
#else
HttpDownloader::KeepAliveScope::KeepAliveScope() = default;
HttpDownloader::KeepAliveScope::~KeepAliveScope() = default;
#endif

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGetSecure(url, username, password, sink);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.close();

  if (result != OK) {
    Storage.remove(destPath.c_str());
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  return OK;
}
