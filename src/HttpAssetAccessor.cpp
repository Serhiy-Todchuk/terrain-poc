#include "pch.h"
#include "HttpAssetAccessor.h"

#include "Log.h"

#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>

#include <fstream>

using namespace CesiumAsync;

namespace {

class SimpleResponse final : public IAssetResponse {
public:
  uint16_t status = 0;
  std::string type;
  HttpHeaders hdrs;
  std::vector<std::byte> body;

  uint16_t statusCode() const override { return status; }
  std::string contentType() const override { return type; }
  const HttpHeaders& headers() const override { return hdrs; }
  std::span<const std::byte> data() const override { return std::span<const std::byte>(body); }
};

class SimpleRequest final : public IAssetRequest {
public:
  std::string verb;
  std::string address;
  HttpHeaders hdrs;
  std::unique_ptr<SimpleResponse> pResponse;

  const std::string& method() const override { return verb; }
  const std::string& url() const override { return address; }
  const HttpHeaders& headers() const override { return hdrs; }
  const IAssetResponse* response() const override { return pResponse.get(); }
};

uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string hostOf(const std::string& url) {
  size_t start = url.find("://");
  start = start == std::string::npos ? 0 : start + 3;
  size_t end = url.find_first_of(":/?", start);
  return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool isTextual(const std::string& contentType) {
  return contentType.find("json") != std::string::npos ||
         contentType.find("text") != std::string::npos ||
         contentType.find("html") != std::string::npos ||
         contentType.find("xml") != std::string::npos;
}

std::string bodySnippet(const std::vector<std::byte>& body) {
  std::string s;
  const size_t n = std::min<size_t>(body.size(), 300);
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    char c = static_cast<char>(body[i]);
    s.push_back((c >= 32 && c < 127) ? c : '.');
  }
  return s;
}

// ---- disk cache ---------------------------------------------------------------

constexpr uint32_t kCacheMagic = 0x31435054; // "TPC1"

std::filesystem::path cachePathFor(const std::filesystem::path& dir, const std::string& url) {
  char name[24];
  snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(fnv1a(url)));
  return dir / std::string(name, 2) / (std::string(name) + ".bin");
}

bool cacheHolds(const std::filesystem::path& path, const std::string& url) {
  std::ifstream f(path, std::ios::binary);
  uint32_t magic = 0, urlLen = 0;
  f.read(reinterpret_cast<char*>(&magic), 4);
  f.read(reinterpret_cast<char*>(&urlLen), 4);
  if (!f || magic != kCacheMagic || urlLen != url.size()) {
    return false;
  }
  std::string storedUrl(urlLen, '\0');
  f.read(storedUrl.data(), urlLen);
  return f && storedUrl == url;
}

bool readCache(const std::filesystem::path& path, const std::string& url, SimpleResponse& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  uint32_t magic = 0, urlLen = 0, typeLen = 0;
  uint16_t status = 0;
  uint64_t bodyLen = 0;
  f.read(reinterpret_cast<char*>(&magic), 4);
  f.read(reinterpret_cast<char*>(&urlLen), 4);
  if (!f || magic != kCacheMagic || urlLen != url.size()) {
    return false;
  }
  std::string storedUrl(urlLen, '\0');
  f.read(storedUrl.data(), urlLen);
  if (storedUrl != url) {
    return false;
  }
  f.read(reinterpret_cast<char*>(&status), 2);
  f.read(reinterpret_cast<char*>(&typeLen), 4);
  if (!f || typeLen > 1024) {
    return false;
  }
  out.type.assign(typeLen, '\0');
  f.read(out.type.data(), typeLen);
  f.read(reinterpret_cast<char*>(&bodyLen), 8);
  if (!f || bodyLen > (1ull << 31)) {
    return false;
  }
  out.body.resize(static_cast<size_t>(bodyLen));
  f.read(reinterpret_cast<char*>(out.body.data()), static_cast<std::streamsize>(bodyLen));
  if (!f) {
    return false;
  }
  out.status = status;
  out.hdrs["Content-Type"] = out.type;
  return true;
}

void writeCache(const std::filesystem::path& path, const std::string& url, const SimpleResponse& r) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path tmp = path;
  tmp += ".tmp" + std::to_string(GetCurrentThreadId());
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      return;
    }
    const uint32_t urlLen = static_cast<uint32_t>(url.size());
    const uint32_t typeLen = static_cast<uint32_t>(r.type.size());
    const uint64_t bodyLen = r.body.size();
    f.write(reinterpret_cast<const char*>(&kCacheMagic), 4);
    f.write(reinterpret_cast<const char*>(&urlLen), 4);
    f.write(url.data(), urlLen);
    f.write(reinterpret_cast<const char*>(&r.status), 2);
    f.write(reinterpret_cast<const char*>(&typeLen), 4);
    f.write(r.type.data(), typeLen);
    f.write(reinterpret_cast<const char*>(&bodyLen), 8);
    f.write(reinterpret_cast<const char*>(r.body.data()), static_cast<std::streamsize>(bodyLen));
  }
  MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---- WinHTTP --------------------------------------------------------------------

struct HttpResult {
  bool transportOk = false;
  DWORD error = 0;
  std::string errorWhere;
  uint16_t status = 0;
  std::string contentType;
  HttpHeaders headers;
  std::vector<std::byte> body;
};

struct InternetHandle {
  HINTERNET h = nullptr;
  ~InternetHandle() {
    if (h) {
      WinHttpCloseHandle(h);
    }
  }
};

HttpResult performHttp(
    HINTERNET session,
    const std::string& verb,
    const std::string& url,
    const HttpHeaders& headers,
    const std::vector<std::byte>& payload) {
  HttpResult result;
  auto fail = [&](const char* where) {
    result.transportOk = false;
    result.error = GetLastError();
    result.errorWhere = where;
    return result;
  };

  std::wstring wurl = Log::toWide(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwSchemeLength = static_cast<DWORD>(-1);
  uc.dwHostNameLength = static_cast<DWORD>(-1);
  uc.dwUrlPathLength = static_cast<DWORD>(-1);
  uc.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
    return fail("WinHttpCrackUrl");
  }
  std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
  std::wstring pathAndQuery(uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength);
  if (pathAndQuery.empty()) {
    pathAndQuery = L"/";
  }
  const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

  InternetHandle connect;
  connect.h = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
  if (!connect.h) {
    return fail("WinHttpConnect");
  }
  std::wstring wverb = Log::toWide(verb);
  InternetHandle request;
  request.h = WinHttpOpenRequest(
      connect.h, wverb.c_str(), pathAndQuery.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
  if (!request.h) {
    return fail("WinHttpOpenRequest");
  }
  DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
  WinHttpSetOption(request.h, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));

  std::wstring headerBlock;
  for (const auto& [name, value] : headers) {
    headerBlock += Log::toWide(name) + L": " + Log::toWide(value) + L"\r\n";
  }
  if (!headerBlock.empty()) {
    WinHttpAddRequestHeaders(request.h, headerBlock.c_str(), static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
  }

  LPVOID body = payload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<std::byte*>(payload.data());
  const DWORD bodySize = static_cast<DWORD>(payload.size());
  if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, bodySize, bodySize, 0)) {
    return fail("WinHttpSendRequest");
  }
  if (!WinHttpReceiveResponse(request.h, nullptr)) {
    return fail("WinHttpReceiveResponse");
  }

  DWORD statusCode = 0;
  DWORD size = sizeof(statusCode);
  if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size, WINHTTP_NO_HEADER_INDEX)) {
    return fail("WinHttpQueryHeaders(status)");
  }
  result.status = static_cast<uint16_t>(statusCode);

  DWORD rawLen = 0;
  WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                      WINHTTP_NO_OUTPUT_BUFFER, &rawLen, WINHTTP_NO_HEADER_INDEX);
  if (rawLen > 0) {
    std::wstring raw(rawLen / sizeof(wchar_t) + 1, L'\0');
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            raw.data(), &rawLen, WINHTTP_NO_HEADER_INDEX)) {
      std::string text = Log::toUtf8(raw.c_str());
      size_t pos = text.find("\r\n"); // skip status line
      while (pos != std::string::npos) {
        size_t start = pos + 2;
        size_t end = text.find("\r\n", start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
          std::string name = line.substr(0, colon);
          std::string value = line.substr(colon + 1);
          while (!value.empty() && value.front() == ' ') {
            value.erase(value.begin());
          }
          result.headers[name] = value;
        }
        pos = end;
      }
    }
  }
  auto ct = result.headers.find("Content-Type");
  if (ct != result.headers.end()) {
    result.contentType = ct->second;
  }

  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.h, &available)) {
      return fail("WinHttpQueryDataAvailable");
    }
    if (available == 0) {
      break;
    }
    size_t offset = result.body.size();
    result.body.resize(offset + available);
    DWORD read = 0;
    if (!WinHttpReadData(request.h, result.body.data() + offset, available, &read)) {
      return fail("WinHttpReadData");
    }
    result.body.resize(offset + read);
  }

  result.transportOk = true;
  return result;
}

} // namespace

HttpAssetAccessor::HttpAssetAccessor(Options options) : _options(std::move(options)) {
  std::wstring ua = Log::toWide(_options.userAgent);
  HINTERNET session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    SPDLOG_LOGGER_ERROR(Log::net(), "WinHttpOpen failed: {}", Log::win32ErrorToString(GetLastError()));
  } else {
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols))) {
      SPDLOG_LOGGER_DEBUG(Log::net(), "HTTP/2 not available: {}", Log::win32ErrorToString(GetLastError()));
    }
    DWORD maxConns = 32;
    WinHttpSetOption(session, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &maxConns, sizeof(maxConns));
  }
  _session = session;

  if (_options.useCache) {
    std::error_code ec;
    std::filesystem::create_directories(_options.cacheDir, ec);
    if (ec) {
      SPDLOG_LOGGER_ERROR(Log::net(), "Cannot create cache dir {}: {}", _options.cacheDir.string(), ec.message());
      _options.useCache = false;
    }
  }

  for (int i = 0; i < _options.threads; ++i) {
    _threads.emplace_back([this, i] { workerLoop(i); });
  }
  SPDLOG_LOGGER_INFO(Log::net(), "HTTP accessor: {} workers, cache {}", _options.threads,
                     _options.useCache ? _options.cacheDir.string() : std::string("disabled"));
}

HttpAssetAccessor::~HttpAssetAccessor() {
  shutdown();
  if (_session) {
    WinHttpCloseHandle(static_cast<HINTERNET>(_session));
  }
}

void HttpAssetAccessor::shutdown() {
  std::deque<Job> pending;
  {
    std::lock_guard lock(_mutex);
    if (_stop && _threads.empty()) {
      return;
    }
    _stop = true;
    pending.swap(_queue);
  }
  _cv.notify_all();
  for (Job& job : pending) {
    auto pRequest = std::make_shared<SimpleRequest>();
    pRequest->verb = job.verb;
    pRequest->address = job.url;
    pRequest->hdrs = job.headers;
    job.promise.resolve(std::shared_ptr<IAssetRequest>(pRequest));
  }
  for (auto& t : _threads) {
    if (t.joinable()) {
      t.join();
    }
  }
  _threads.clear();
}

Future<std::shared_ptr<IAssetRequest>> HttpAssetAccessor::get(
    const AsyncSystem& asyncSystem, const std::string& url, const std::vector<THeader>& headers) {
  return request(asyncSystem, "GET", url, headers, {});
}

Future<std::shared_ptr<IAssetRequest>> HttpAssetAccessor::request(
    const AsyncSystem& asyncSystem,
    const std::string& verb,
    const std::string& url,
    const std::vector<THeader>& headers,
    const std::span<const std::byte>& contentPayload) {
  auto promise = asyncSystem.createPromise<std::shared_ptr<IAssetRequest>>();
  auto future = promise.getFuture();
  ++_requests;

  Job job{promise, verb, url, hostOf(url), {}, {}, std::chrono::steady_clock::now()};
  for (const auto& [k, v] : headers) {
    job.headers[k] = v;
  }
  job.payload.assign(contentPayload.begin(), contentPayload.end());

  {
    std::lock_guard lock(_mutex);
    if (_stop) {
      auto pRequest = std::make_shared<SimpleRequest>();
      pRequest->verb = verb;
      pRequest->address = url;
      promise.resolve(std::shared_ptr<IAssetRequest>(pRequest));
      return future;
    }
    _queue.push_back(std::move(job));
  }
  _cv.notify_all();
  return future;
}

int HttpAssetAccessor::hostLimit(const std::string& host) const {
  auto it = _options.hostLimits.find(host);
  return it != _options.hostLimits.end() ? it->second : _options.defaultHostLimit;
}

HttpAssetAccessor::Stats HttpAssetAccessor::stats() const {
  Stats s;
  s.requests = _requests.load();
  s.networkCompleted = _networkCompleted.load();
  s.cacheHits = _cacheHits.load();
  s.httpErrors = _httpErrors.load();
  s.transportFailures = _transportFailures.load();
  s.retries = _retries.load();
  s.bytesDownloaded = _bytes.load();
  s.offlineMisses = _offlineMisses.load();
  s.inFlight = _inFlight.load();
  {
    std::lock_guard lock(_mutex);
    s.queued = static_cast<int>(_queue.size());
  }
  return s;
}

bool HttpAssetAccessor::idle() const {
  std::lock_guard lock(_mutex);
  return _queue.empty() && _inFlight.load() == 0;
}

bool HttpAssetAccessor::isCached(const std::string& url) const {
  return _options.useCache && cacheHolds(cachePathFor(_options.cacheDir, url), url);
}

void HttpAssetAccessor::workerLoop(int index) {
  std::wstring name = L"HttpWorker" + std::to_wstring(index);
  SetThreadDescription(GetCurrentThread(), name.c_str());
  for (;;) {
    std::optional<Job> job;
    {
      std::unique_lock lock(_mutex);
      _cv.wait(lock, [&] {
        if (_stop) {
          return true;
        }
        for (const Job& j : _queue) {
          if (_hostActive[j.host] < hostLimit(j.host)) {
            return true;
          }
        }
        return false;
      });
      if (_stop) {
        return;
      }
      for (auto it = _queue.begin(); it != _queue.end(); ++it) {
        if (_hostActive[it->host] < hostLimit(it->host)) {
          job.emplace(std::move(*it));
          _queue.erase(it);
          break;
        }
      }
      if (!job) {
        continue;
      }
      ++_hostActive[job->host];
      ++_inFlight;
    }
    process(*job);
    {
      std::lock_guard lock(_mutex);
      --_hostActive[job->host];
      --_inFlight;
    }
    _cv.notify_all();
  }
}

void HttpAssetAccessor::process(Job& job) {
  auto pRequest = std::make_shared<SimpleRequest>();
  pRequest->verb = job.verb;
  pRequest->address = job.url;
  pRequest->hdrs = job.headers;

  // A "Cache-Control: no-cache" request header (listings that change, e.g. the
  // NOMADS file list) bypasses the disk cache in both directions.
  const auto cacheControl = job.headers.find("Cache-Control");
  const bool noCache = cacheControl != job.headers.end() && cacheControl->second.find("no-cache") != std::string::npos;
  const bool cacheable = _options.useCache && job.verb == "GET" && !noCache;
  std::filesystem::path cachePath;
  if (cacheable) {
    cachePath = cachePathFor(_options.cacheDir, job.url);
    auto pResponse = std::make_unique<SimpleResponse>();
    if (readCache(cachePath, job.url, *pResponse)) {
      ++_cacheHits;
      SPDLOG_LOGGER_DEBUG(Log::net(), "{} {} -> {} {}B [cache]", job.verb, job.url, pResponse->status,
                          pResponse->body.size());
      pRequest->pResponse = std::move(pResponse);
      job.promise.resolve(std::shared_ptr<IAssetRequest>(pRequest));
      return;
    }
  }

  if (_options.offline) {
    // Network disabled: a cache miss behaves like "not found" so every layer
    // degrades gracefully (coarser ancestor tiles stay visible).
    ++_offlineMisses;
    SPDLOG_LOGGER_DEBUG(Log::net(), "{} {} -> offline cache miss", job.verb, job.url);
    auto pResponse = std::make_unique<SimpleResponse>();
    pResponse->status = 404;
    pResponse->type = "text/plain";
    const std::string text = "offline: not in cache";
    pResponse->body.resize(text.size());
    std::memcpy(pResponse->body.data(), text.data(), text.size());
    pRequest->pResponse = std::move(pResponse);
    job.promise.resolve(std::shared_ptr<IAssetRequest>(pRequest));
    return;
  }

  const auto t0 = std::chrono::steady_clock::now();
  HttpResult result;
  for (int attempt = 1; attempt <= _options.maxAttempts; ++attempt) {
    result = performHttp(static_cast<HINTERNET>(_session), job.verb, job.url, job.headers, job.payload);
    const bool retryable = !result.transportOk || result.status == 429 || result.status == 500 ||
                           result.status == 502 || result.status == 503 || result.status == 504;
    if (!retryable || attempt == _options.maxAttempts) {
      break;
    }
    {
      std::lock_guard lock(_mutex);
      if (_stop) {
        break;
      }
    }
    ++_retries;
    if (!result.transportOk) {
      SPDLOG_LOGGER_WARN(Log::net(), "{} {} attempt {} failed in {}: {} - retrying", job.verb, job.url, attempt,
                         result.errorWhere, Log::win32ErrorToString(result.error));
    } else {
      SPDLOG_LOGGER_WARN(Log::net(), "{} {} attempt {} -> HTTP {} - retrying", job.verb, job.url, attempt,
                         result.status);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300 * (1 << (attempt - 1))));
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

  if (!result.transportOk) {
    ++_transportFailures;
    SPDLOG_LOGGER_ERROR(Log::net(), "{} {} failed in {}: {} (error {})", job.verb, job.url, result.errorWhere,
                        Log::win32ErrorToString(result.error), result.error);
    job.promise.resolve(std::shared_ptr<IAssetRequest>(pRequest)); // no response
    return;
  }

  ++_networkCompleted;
  _bytes += static_cast<int64_t>(result.body.size());
  if (result.status < 200 || result.status >= 300) {
    ++_httpErrors;
    SPDLOG_LOGGER_WARN(Log::net(), "{} {} -> HTTP {} in {:.0f} ms, body: {}", job.verb, job.url, result.status, ms,
                       bodySnippet(result.body));
  } else {
    SPDLOG_LOGGER_DEBUG(Log::net(), "{} {} -> {} {}B {:.0f} ms", job.verb, job.url, result.status,
                        result.body.size(), ms);
  }

  auto pResponse = std::make_unique<SimpleResponse>();
  pResponse->status = result.status;
  pResponse->type = result.contentType;
  pResponse->hdrs = std::move(result.headers);
  pResponse->body = std::move(result.body);

  // Textual bodies are only cached for non-raster endpoints (e.g. TileJSON);
  // ArcGIS exportImage and the NOMADS grib filter report errors as text, which
  // must not be cached.
  const bool cacheText = job.url.find("exportImage") == std::string::npos &&
                         job.url.find("/cgi-bin/filter_") == std::string::npos;
  if (cacheable && pResponse->status == 200 && !pResponse->body.empty() &&
      (cacheText || !isTextual(pResponse->type))) {
    writeCache(cachePath, job.url, *pResponse);
  }
  pRequest->pResponse = std::move(pResponse);
  job.promise.resolve(std::shared_ptr<IAssetRequest>(pRequest));
}
