#pragma once
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/HttpHeaders.h>
#include <CesiumAsync/IAssetAccessor.h>
#include <CesiumAsync/Promise.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// IAssetAccessor implemented with WinHTTP.
//  * every request is logged (debug level), every failure at warn/error level
//  * transient failures (transport errors, 429, 5xx) are retried with backoff
//  * a per-host concurrency limit keeps us polite to public servers
//  * successful GET responses are stored in an on-disk cache keyed by URL so
//    repeated harness runs do not re-download the same data (a request header
//    "Cache-Control: no-cache" bypasses it, for listings that change)
class HttpAssetAccessor final : public CesiumAsync::IAssetAccessor {
public:
  struct Options {
    int threads = 24;
    bool useCache = true;
    bool offline = false; // serve from the disk cache only; misses -> 404
    std::filesystem::path cacheDir;
    std::string userAgent = "TerrainPOC/1.0 (Cesium Native PoC; WinHTTP)";
    int defaultHostLimit = 8;
    std::map<std::string, int> hostLimits;
    int maxAttempts = 4;
  };

  struct Stats {
    int64_t requests = 0;
    int64_t networkCompleted = 0;
    int64_t cacheHits = 0;
    int64_t httpErrors = 0;
    int64_t transportFailures = 0;
    int64_t retries = 0;
    int64_t bytesDownloaded = 0;
    int64_t offlineMisses = 0;
    int inFlight = 0;
    int queued = 0;
  };

  explicit HttpAssetAccessor(Options options);
  ~HttpAssetAccessor() override;

  CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>>
  get(const CesiumAsync::AsyncSystem& asyncSystem,
      const std::string& url,
      const std::vector<THeader>& headers = {}) override;

  CesiumAsync::Future<std::shared_ptr<CesiumAsync::IAssetRequest>> request(
      const CesiumAsync::AsyncSystem& asyncSystem,
      const std::string& verb,
      const std::string& url,
      const std::vector<THeader>& headers = std::vector<THeader>(),
      const std::span<const std::byte>& contentPayload = {}) override;

  void tick() noexcept override {}

  Stats stats() const;
  bool idle() const;
  // True when a GET of this URL would be answered from the disk cache. Reads
  // only the cache file's header; thread-safe.
  bool isCached(const std::string& url) const;

  // Resolves all queued (not yet started) requests with "no response" and
  // stops the workers once in-flight requests complete.
  void shutdown();

private:
  struct Job {
    CesiumAsync::Promise<std::shared_ptr<CesiumAsync::IAssetRequest>> promise;
    std::string verb;
    std::string url;
    std::string host;
    CesiumAsync::HttpHeaders headers;
    std::vector<std::byte> payload;
    std::chrono::steady_clock::time_point enqueued;
  };

  void workerLoop(int index);
  void process(Job& job);
  int hostLimit(const std::string& host) const;

  Options _options;
  void* _session = nullptr; // HINTERNET

  mutable std::mutex _mutex;
  std::condition_variable _cv;
  std::deque<Job> _queue;
  std::map<std::string, int> _hostActive;
  std::vector<std::thread> _threads;
  bool _stop = false;

  std::atomic<int64_t> _requests{0}, _networkCompleted{0}, _cacheHits{0}, _httpErrors{0},
      _transportFailures{0}, _retries{0}, _bytes{0}, _offlineMisses{0};
  std::atomic<int> _inFlight{0};
};
