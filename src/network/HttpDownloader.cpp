#include "HttpDownloader.h"

#include <HTTPClient.h>
#include <Logging.h>
#include <NetworkClient.h>
#include <NetworkClientSecure.h>
#include <StreamString.h>
#include <base64.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>
#include <memory>
#include <utility>

#include "util/UrlUtils.h"

namespace {
// Shared HTTP client storage. Allocating NetworkClientSecure per-call was the
// dominant fragmenter in trace runs (FreeBlocks 13 -> 37 across a single TLS
// session). Keeping one instance alive trades a permanent ~object footprint
// for no per-call alloc churn. Mutex serialises callers since mbedtls state
// isn't safe to share concurrently.
NetworkClient s_plainClient;
NetworkClientSecure s_secureClient;
StaticSemaphore_t s_clientMutexBuffer;
SemaphoreHandle_t s_clientMutex = nullptr;

// RAII lease over the shared client. Acquires the mutex on construction,
// releases on destruction. Every return path through HttpDownloader is now
// automatically cleaned up, no per-branch release call needed.
class ClientLease {
 public:
  explicit ClientLease(bool secure) {
    if (s_clientMutex == nullptr) {
      s_clientMutex = xSemaphoreCreateMutexStatic(&s_clientMutexBuffer);
    }
    xSemaphoreTake(s_clientMutex, portMAX_DELAY);
    if (secure) {
      s_secureClient.stop();
      s_secureClient.setInsecure();
      client_ = &s_secureClient;
    } else {
      s_plainClient.stop();
      client_ = &s_plainClient;
    }
  }
  ~ClientLease() {
    client_->stop();
    xSemaphoreGive(s_clientMutex);
  }
  ClientLease(const ClientLease&) = delete;
  ClientLease& operator=(const ClientLease&) = delete;

  NetworkClient& get() { return *client_; }

 private:
  NetworkClient* client_;
};

// Split a URL into scheme/host/port/path. Defaults port to 80/443 based on
// scheme. Used by fetchUrlVerbose's manual HTTP path.
struct ParsedUrl {
  bool secure;
  std::string host;
  uint16_t port;
  std::string path;
};

bool parseUrl(const std::string& url, ParsedUrl& out) {
  out.secure = UrlUtils::isHttpsUrl(url);
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;
  const size_t hostStart = schemeEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  std::string hostPort = pathStart == std::string::npos ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
  out.path = pathStart == std::string::npos ? "/" : url.substr(pathStart);
  const size_t colon = hostPort.find(':');
  if (colon == std::string::npos) {
    out.host = hostPort;
    out.port = out.secure ? 443 : 80;
  } else {
    out.host = hostPort.substr(0, colon);
    out.port = static_cast<uint16_t>(std::atoi(hostPort.c_str() + colon + 1));
  }
  return !out.host.empty();
}

// Read one CRLF-terminated line from the client. Strips trailing \r\n.
// Bounded by deadlineMs (wall-clock millis()).  Returns false on timeout or
// connection close before a complete line.
bool manualReadLine(NetworkClient& c, std::string& out, unsigned long deadlineMs) {
  out.clear();
  while (millis() < deadlineMs) {
    if (!c.connected() && !c.available()) return false;
    if (c.available() <= 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    const int ch = c.read();
    if (ch < 0) continue;
    if (ch == '\n') {
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
    out.push_back(static_cast<char>(ch));
    if (out.size() > 4096) return false;  // header line shouldn't be this long
  }
  return false;
}

// Read exactly n bytes into out (streaming, no body buffering), with
// per-batch logging so we can see where stalls happen. deadlineMs is the
// wall-clock cap. Returns false on timeout/error before n bytes arrive.
bool manualReadExact(NetworkClient& c, Stream& out, size_t n, unsigned long deadlineMs, unsigned long startMs,
                     size_t& totalSoFar) {
  uint8_t buf[512];
  unsigned long lastLogMs = millis();
  size_t sinceLastLog = 0;
  while (n > 0) {
    if (millis() > deadlineMs) {
      LOG_ERR("HTTP", "manual: read timeout, %u bytes left in chunk (total received=%u)", static_cast<unsigned>(n),
              static_cast<unsigned>(totalSoFar));
      return false;
    }
    if (!c.connected() && !c.available()) {
      LOG_ERR("HTTP", "manual: connection closed mid-chunk, %u bytes left", static_cast<unsigned>(n));
      return false;
    }
    const int avail = c.available();
    if (avail <= 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    const size_t toRead = std::min({static_cast<size_t>(avail), static_cast<size_t>(sizeof(buf)), n});
    const int got = c.read(buf, toRead);
    if (got < 0) {
      LOG_ERR("HTTP", "manual: read() returned %d", got);
      return false;
    }
    if (got == 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    out.write(buf, static_cast<size_t>(got));
    n -= static_cast<size_t>(got);
    totalSoFar += static_cast<size_t>(got);
    sinceLastLog += static_cast<size_t>(got);
    const unsigned long now = millis();
    if (now - lastLogMs >= 1000 || sinceLastLog >= 8192) {
      LOG_INF("HTTP", "manual: +%u bytes (total=%u, t=%lu ms, free=%u)", static_cast<unsigned>(sinceLastLog),
              static_cast<unsigned>(totalSoFar), now - startMs, ESP.getFreeHeap());
      lastLogMs = now;
      sinceLastLog = 0;
    }
  }
  return true;
}

class FileWriteStream final : public Stream {
 public:
  // totalPtr is read on every write so a parallel writer (e.g. fetchUrlVerbose
  // populating Content-Length after the header parse) can publish the total
  // size live. Pass nullptr or a pointer that stays at -1 for "unknown total"
  // (progress callback is suppressed in that case).
  FileWriteStream(FsFile& file, const int64_t* totalPtr, HttpDownloader::ProgressCallback progress, bool* cancelFlag)
      : file_(file), totalPtr_(totalPtr), progress_(std::move(progress)), cancelFlag_(cancelFlag) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (cancelFlag_ && *cancelFlag_) {
      writeOk_ = false;
      return 0;
    }
    const size_t written = file_.write(buffer, size);
    if (written != size) {
      writeOk_ = false;
    }
    downloaded_ += written;
    const int64_t total = totalPtr_ ? *totalPtr_ : -1;
    if (progress_ && total > 0) {
      progress_(downloaded_, static_cast<size_t>(total));
    }
    return written;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { file_.flush(); }

  size_t downloaded() const { return downloaded_; }
  bool ok() const { return writeOk_; }

 private:
  FsFile& file_;
  const int64_t* totalPtr_;
  size_t downloaded_ = 0;
  bool writeOk_ = true;
  HttpDownloader::ProgressCallback progress_;
  bool* cancelFlag_;
};
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  ClientLease lease(UrlUtils::isHttpsUrl(url));
  HTTPClient http;

  LOG_DBG("HTTP", "Fetching: %s", url.c_str());

  http.begin(lease.get(), url.c_str());
  // Default HTTPClient TCP timeout is 5 s per read. TCP_WND=5760 in the
  // arduino-esp32 framework makes a 16 KB TLS record take three roundtrips,
  // and chunked-encoding catalogs can stall mid-body well past 5 s. Bump
  // generously since reuse means we pay no per-call setup cost anyway.
  // HTTPClient::setTimeout takes uint16_t; 60s is just under the 65535 ceiling.
  // Anything larger silently truncates (mod 65536). If the chip genuinely needs
  // more than ~60s for a fetch, the fix is bypassing HTTPClient's per-read
  // deadline, not bumping this value.
  http.setTimeout(60000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);

  if (!username.empty() && !password.empty()) {
    std::string credentials = username + ":" + password;
    String encoded = base64::encode(credentials.c_str());
    http.addHeader("Authorization", "Basic " + encoded);
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    LOG_ERR("HTTP", "Fetch failed: %d", httpCode);
    http.end();
    return false;
  }

  const int64_t reportedLength = http.getSize();
  const int written = http.writeToStream(&outContent);
  http.end();

  if (written < 0) {
    LOG_ERR("HTTP", "writeToStream failed: %d (Content-Length=%lld)", written, static_cast<long long>(reportedLength));
    return false;
  }

  LOG_DBG("HTTP", "Fetch success: %d bytes (Content-Length=%lld)", written, static_cast<long long>(reportedLength));
  return true;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  StreamString stream;
  if (!fetchUrl(url, stream, username, password)) {
    return false;
  }
  outContent = stream.c_str();
  return true;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s", url.c_str());
  LOG_DBG("HTTP", "Destination: %s", destPath.c_str());

  // Remove existing file if present.
  if (Storage.exists(destPath.c_str())) {
    Storage.remove(destPath.c_str());
  }

  FsFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  // contentLength is written by fetchUrlVerbose as soon as it has parsed the
  // Content-Length header (or set to -1 for chunked / unknown). FileWriteStream
  // reads through the pointer on every write so the progress UI updates live.
  int64_t contentLength = -1;
  FileWriteStream fileStream(file, &contentLength, progress, cancelFlag);
  const bool fetchOk = fetchUrlVerbose(url, fileStream, username, password, &contentLength);
  const size_t downloaded = fileStream.downloaded();
  file.close();

  if (!fetchOk) {
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  if (cancelFlag && *cancelFlag) {
    Storage.remove(destPath.c_str());
    return ABORTED;
  }

  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);

  if (!fileStream.ok()) {
    LOG_ERR("HTTP", "Write failed during download");
    Storage.remove(destPath.c_str());
    return FILE_ERROR;
  }

  if (downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  if (contentLength > 0 && downloaded != static_cast<size_t>(contentLength)) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %lld", downloaded, static_cast<long long>(contentLength));
    Storage.remove(destPath.c_str());
    return HTTP_ERROR;
  }

  return OK;
}

// Resolve a redirect Location value against the request that produced it.
// Handles absolute URLs (scheme://host/...) and absolute paths (/...). Other
// forms are unsupported and produce an empty result.
std::string resolveLocation(const std::string& location, const ParsedUrl& base) {
  if (location.find("://") != std::string::npos) return location;
  if (!location.empty() && location[0] == '/') {
    std::string out;
    out.reserve(base.host.size() + location.size() + 16);
    out += base.secure ? "https://" : "http://";
    out += base.host;
    if ((base.secure && base.port != 443) || (!base.secure && base.port != 80)) {
      out += ':';
      out += std::to_string(base.port);
    }
    out += location;
    return out;
  }
  return {};
}

bool HttpDownloader::fetchUrlVerbose(const std::string& url, Stream& outStream, const std::string& username,
                                     const std::string& password, int64_t* outContentLength) {
  if (outContentLength) *outContentLength = -1;

  std::string currentUrl = url;
  ParsedUrl parsed;
  if (!parseUrl(currentUrl, parsed)) {
    LOG_ERR("HTTP", "manual: bad URL: %s", currentUrl.c_str());
    return false;
  }
  const bool initialSecure = parsed.secure;

  ClientLease lease(initialSecure);
  NetworkClient& client = lease.get();
  client.setTimeout(60000);  // 60 s per-read at the socket level (uint32_t, no truncation here)

  const unsigned long startMs = millis();
  const unsigned long deadline = startMs + 90000;  // 90 s wall-clock for the whole fetch (including any redirects)
  constexpr int MAX_REDIRECTS = 5;

  bool chunked = false;
  int64_t contentLength = -1;
  std::string statusLine;
  bool gotFinalResponse = false;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    if (hop > 0) {
      // Coming back round after a redirect — re-parse currentUrl, close the
      // previous connection (the unread redirect body is discarded with it),
      // reset header state. Connection: close means the server already
      // intends to close after each response, so this is cheap.
      if (!parseUrl(currentUrl, parsed)) {
        LOG_ERR("HTTP", "manual: bad redirect URL: %s", currentUrl.c_str());
        return false;
      }
      if (parsed.secure != initialSecure) {
        LOG_ERR("HTTP", "manual: cross-scheme redirect not supported (%s -> %s)",
                initialSecure ? "https" : "http", parsed.secure ? "https" : "http");
        return false;
      }
      client.stop();
      chunked = false;
      contentLength = -1;
    }

    LOG_INF("HTTP", "manual: parsed scheme=%s host=%s port=%u path=%s", parsed.secure ? "https" : "http",
            parsed.host.c_str(), parsed.port, parsed.path.c_str());

    if (!client.connect(parsed.host.c_str(), parsed.port)) {
      LOG_ERR("HTTP", "manual: connect failed");
      return false;
    }
    LOG_INF("HTTP", "manual: connected (%lu ms)", millis() - startMs);

    // Send request. Connection: close means the server closes after each
    // response, which gives us a natural redirect-body-discard via stop().
    std::string req;
    req.reserve(512);
    req += "GET ";
    req += parsed.path;
    req += " HTTP/1.1\r\nHost: ";
    req += parsed.host;
    req += "\r\nUser-Agent: CrossPoint-ESP32-" CROSSPOINT_VERSION "\r\nAccept: */*\r\nConnection: close\r\n";
    if (!username.empty() && !password.empty()) {
      const std::string creds = username + ":" + password;
      const String encoded = base64::encode(creds.c_str());
      req += "Authorization: Basic ";
      req += encoded.c_str();
      req += "\r\n";
    }
    req += "\r\n";
    client.write(reinterpret_cast<const uint8_t*>(req.data()), req.size());
    LOG_INF("HTTP", "manual: sent %u-byte request (%lu ms)", static_cast<unsigned>(req.size()), millis() - startMs);

    // Status line.
    if (!manualReadLine(client, statusLine, deadline)) {
      LOG_ERR("HTTP", "manual: status line read failed (%lu ms)", millis() - startMs);
      return false;
    }
    LOG_INF("HTTP", "manual: status = %s", statusLine.c_str());

    int statusCode = 0;
    {
      const size_t sp = statusLine.find(' ');
      if (sp != std::string::npos) statusCode = std::atoi(statusLine.c_str() + sp + 1);
    }

    // Headers.
    std::string location;
    std::string header;
    while (manualReadLine(client, header, deadline)) {
      if (header.empty()) break;  // end of headers
      auto starts_with_ci = [&](const char* prefix) {
        const size_t plen = strlen(prefix);
        if (header.size() < plen) return false;
        for (size_t i = 0; i < plen; ++i) {
          if (tolower(static_cast<unsigned char>(header[i])) != tolower(static_cast<unsigned char>(prefix[i])))
            return false;
        }
        return true;
      };
      auto valueAfter = [&](const char* prefix) {
        const char* p = header.c_str() + strlen(prefix);
        while (*p == ' ' || *p == '\t') ++p;
        return std::string(p);
      };
      if (starts_with_ci("transfer-encoding:")) {
        if (header.find("chunked") != std::string::npos) chunked = true;
        LOG_INF("HTTP", "manual: %s", header.c_str());
      } else if (starts_with_ci("content-length:")) {
        contentLength = std::atoll(header.c_str() + 15);
        LOG_INF("HTTP", "manual: %s", header.c_str());
      } else if (starts_with_ci("location:")) {
        location = valueAfter("location:");
        LOG_INF("HTTP", "manual: %s", header.c_str());
      } else if (starts_with_ci("content-type:") || starts_with_ci("connection:")) {
        LOG_INF("HTTP", "manual: %s", header.c_str());
      }
    }
    LOG_INF("HTTP", "manual: headers done (%lu ms) status=%d chunked=%d content-length=%lld",
            millis() - startMs, statusCode, chunked, static_cast<long long>(contentLength));

    if (statusCode >= 300 && statusCode < 400 && !location.empty()) {
      std::string resolved = resolveLocation(location, parsed);
      if (resolved.empty()) {
        LOG_ERR("HTTP", "manual: unsupported redirect Location: %s", location.c_str());
        return false;
      }
      LOG_INF("HTTP", "manual: redirect %d -> %s", statusCode, resolved.c_str());
      currentUrl = resolved;
      continue;  // loop top: re-parse, reconnect, re-request
    }
    if (statusCode < 200 || statusCode >= 300) {
      LOG_ERR("HTTP", "manual: non-2xx status %d (no Location): %s", statusCode, statusLine.c_str());
      return false;
    }
    // 2xx — fall through to body decode below.
    if (outContentLength) *outContentLength = chunked ? -1 : contentLength;
    gotFinalResponse = true;
    break;
  }
  if (!gotFinalResponse) {
    LOG_ERR("HTTP", "manual: too many redirects (>%d)", MAX_REDIRECTS);
    return false;
  }

  // Body decode — streams to outStream as bytes arrive, no body buffering.
  size_t totalSoFar = 0;
  if (chunked) {
    int chunkIdx = 0;
    while (true) {
      std::string sizeLine;
      if (!manualReadLine(client, sizeLine, deadline)) {
        LOG_ERR("HTTP", "manual: chunk-size line read failed at chunk %d (%lu ms)", chunkIdx, millis() - startMs);
        return false;
      }
      const size_t chunkSize = strtoul(sizeLine.c_str(), nullptr, 16);
      LOG_INF("HTTP", "manual: chunk %d size=%u (t=%lu ms, total so far=%u, free=%u)", chunkIdx,
              static_cast<unsigned>(chunkSize), millis() - startMs, static_cast<unsigned>(totalSoFar),
              ESP.getFreeHeap());
      if (chunkSize == 0) {
        // Trailer lines until empty.
        std::string trailer;
        while (manualReadLine(client, trailer, deadline) && !trailer.empty()) {}
        break;
      }
      if (!manualReadExact(client, outStream, chunkSize, deadline, startMs, totalSoFar)) return false;
      // Trailing CRLF after chunk body.
      std::string crlf;
      manualReadLine(client, crlf, deadline);
      ++chunkIdx;
    }
  } else if (contentLength >= 0) {
    if (!manualReadExact(client, outStream, static_cast<size_t>(contentLength), deadline, startMs, totalSoFar))
      return false;
  } else {
    // Read until connection close.
    uint8_t buf[512];
    while (client.connected() || client.available() > 0) {
      if (millis() > deadline) {
        LOG_ERR("HTTP", "manual: read-until-close timeout (%u bytes received)", static_cast<unsigned>(totalSoFar));
        return false;
      }
      const int avail = client.available();
      if (avail <= 0) {
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      const int got = client.read(buf, std::min(avail, static_cast<int>(sizeof(buf))));
      if (got <= 0) {
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      outStream.write(buf, static_cast<size_t>(got));
      totalSoFar += static_cast<size_t>(got);
    }
  }

  LOG_INF("HTTP", "manual: fetch ok, %u bytes in %lu ms", static_cast<unsigned>(totalSoFar), millis() - startMs);
  return true;
}
