#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps NetworkClientSecure and HTTPClient for HTTPS requests.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

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
   * Diagnostic fetch path: bypasses HTTPClient and drives the static
   * NetworkClient(Secure) directly. Streams bytes to `outStream` as they
   * arrive (no body buffering). Logs per-read counts + intervals so we can
   * see where reads stall.
   *
   * If `outContentLength` is non-null, it is written **before** body
   * streaming begins, with the value from the Content-Length header, or -1
   * for chunked / unknown-size responses. Lets a Stream sink see the total
   * size live during the transfer (e.g. for progress UI).
   */
  static bool fetchUrlVerbose(const std::string& url, Stream& outStream, const std::string& username = "",
                              const std::string& password = "", int64_t* outContentLength = nullptr);

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");
};
