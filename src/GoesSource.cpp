#include "pch.h"
#include "GoesSource.h"

#include "GoesGrid.h"
#include "Hdf5.h"
#include "Log.h"
#include "WeatherService.h"

#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>

#include <cmath>
#include <string_view>

using namespace CesiumAsync;

namespace {

using Clock = std::chrono::steady_clock;
constexpr int64_t kHour = 3600;
// An hour's last scan starts at HH:56 and all four products are published by
// ~HH+1:02 (checked 2026-10-11): 30 min after the hour its listing is complete.
constexpr int64_t kFinishedAfter = 30 * 60;
constexpr size_t kMaxLoaded = 3;
constexpr const char* kBucket = "https://noaa-goes19.s3.amazonaws.com/";
constexpr const char* kProductIds[kGoesProductCount] = {"ACHAC", "CCLC", "ACMC", "CODC"};

int64_t unixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t hourStart(int64_t t) { return t - ((t % kHour) + kHour) % kHour; }

std::string hhmm(int64_t t) { return WeatherService::formatTime(t).substr(11, 5); }

// "ABI-L2-ACHAC/2026/284/00/": a product's directory of one hour (day of year).
std::string hourPrefix(int product, int64_t hour) {
  using namespace std::chrono;
  const sys_seconds s{seconds{hour}};
  const sys_days d = floor<days>(s);
  const year_month_day ymd{d};
  const int dayOfYear = int((d - sys_days{ymd.year() / January / 1}).count()) + 1;
  char buf[64];
  snprintf(buf, sizeof(buf), "ABI-L2-%s/%04d/%03d/%02d/", kProductIds[product], int(ymd.year()), dayOfYear,
           int((s - d).count() / kHour));
  return buf;
}

std::string listUrl(int product, int64_t hour) {
  return std::string(kBucket) + "?list-type=2&prefix=" + hourPrefix(product, hour);
}

// The <Key> / <Size> pairs of an S3 ListObjectsV2 response.
std::vector<std::pair<std::string, uint64_t>> parseListing(std::span<const std::byte> body, bool& truncated) {
  const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
  truncated = text.find("<IsTruncated>true</IsTruncated>") != std::string_view::npos;
  std::vector<std::pair<std::string, uint64_t>> out;
  auto element = [&](std::string_view tag, size_t from, size_t to, std::string_view& value) {
    const std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
    const size_t a = text.find(open, from);
    const size_t b = a == std::string_view::npos ? a : text.find(close, a);
    if (b == std::string_view::npos || b > to) {
      return false;
    }
    value = text.substr(a + open.size(), b - a - open.size());
    return true;
  };
  for (size_t pos = text.find("<Contents>"); pos != std::string_view::npos;) {
    const size_t end = text.find("</Contents>", pos);
    if (end == std::string_view::npos) {
      break;
    }
    std::string_view key, size;
    if (element("Key", pos, end, key) && element("Size", pos, end, size)) {
      uint64_t bytes = 0;
      for (const char c : size) {
        bytes = c >= '0' && c <= '9' ? bytes * 10 + uint64_t(c - '0') : bytes;
      }
      out.emplace_back(std::string(key), bytes);
    }
    pos = text.find("<Contents>", end);
  }
  return out;
}

// Fraction of valid (non-NaN) cells and their mean.
double validMean(const std::vector<float>& values, double& mean) {
  size_t valid = 0;
  double sum = 0.0;
  for (const float v : values) {
    if (!std::isnan(v)) {
      ++valid;
      sum += v;
    }
  }
  mean = valid ? sum / double(valid) : 0.0;
  return values.empty() ? 0.0 : double(valid) / double(values.size());
}

struct ScanFile {
  int product = 0;
  std::string name;
  std::vector<std::byte> data;
};

struct Decoded {
  std::shared_ptr<GoesScan> scan;
  std::string error;               // the scan cannot be used (no cloud-top heights)
  std::vector<std::string> errors; // products that could not be decoded (the scan is used without them)
};

// The files of one scan -> GoesScan on the grid (worker thread). The cloud-top
// heights come first: their self-consistent lines of sight (parallax) are the
// sampling positions of every product.
Decoded decodeScan(std::vector<ScanFile>& files, const CloudGrid& grid, int64_t start) {
  Decoded out;
  const auto t0 = Clock::now();
  auto scan = std::make_shared<GoesScan>();
  scan->grid = grid;
  scan->start = start;
  std::array<Hdf5::File, kGoesProductCount> opened;
  std::array<bool, kGoesProductCount> have{};
  for (ScanFile& f : files) {
    scan->bytes += f.data.size();
    std::string error;
    if (!opened[size_t(f.product)].open(std::move(f.data), error)) {
      out.errors.push_back(f.name + ": " + error);
      continue;
    }
    have[size_t(f.product)] = true;
  }
  if (!have[kGoesCloudTop]) {
    out.error = "no readable cloud-top heights (ACHAC)";
    return out;
  }
  std::string error;
  Goes::Raster heights;
  if (!heights.read(opened[kGoesCloudTop], "HT", &grid, error)) {
    out.error = "ACHAC HT: " + error;
    return out;
  }
  Goes::ParallaxStats parallax;
  std::vector<float> lineOfSight;
  scan->cloudTop = Goes::cloudTopHeights(heights, grid, &parallax, &lineOfSight);
  scan->products |= 1 << kGoesCloudTop;
  const std::vector<glm::dvec2> angles = Goes::lineOfSightAngles(heights.projection, grid, lineOfSight);
  auto field = [&](int product, const std::string& variable, std::vector<float>& values) {
    if (!have[size_t(product)]) {
      return false;
    }
    Goes::Raster raster;
    if (!raster.read(opened[size_t(product)], variable, &grid, error)) {
      out.errors.push_back(std::string(kProductIds[product]) + " " + variable + ": " + error);
      return false;
    }
    const Goes::Projection &a = raster.projection, &b = heights.projection;
    if (a.perspectivePointHeight != b.perspectivePointHeight || a.semiMajor != b.semiMajor ||
        a.semiMinor != b.semiMinor || a.lonOrigin != b.lonOrigin) {
      out.errors.push_back(std::string(kProductIds[product]) + ": a different fixed grid than ACHAC's");
      return false;
    }
    values = Goes::resample(raster, angles);
    return true;
  };
  bool layers = field(kGoesCoverLayers, "TCF", scan->totalCover);
  for (int b = 0; b < kGoesBandCount && layers; ++b) {
    std::vector<float>& band = scan->bandCover[size_t(b)];
    layers = field(kGoesCoverLayers, "CF" + std::to_string(b + 1), band);
    for (float& v : band) {
      v *= 0.01f; // percent (u8, valid_range 0..100; the files say units "1")
    }
  }
  if (!layers) {
    scan->totalCover.clear();
    for (std::vector<float>& band : scan->bandCover) {
      band.clear();
    }
  }
  scan->products |= layers ? 1 << kGoesCoverLayers : 0;
  scan->products |= field(kGoesCloudMask, "BCM", scan->cloudMask) ? 1 << kGoesCloudMask : 0;
  scan->products |= field(kGoesOpticalDepth, "COD", scan->opticalDepth) ? 1 << kGoesOpticalDepth : 0;

  scan->cloudTopFraction = validMean(scan->cloudTop, scan->meanCloudTop);
  scan->meanShiftKm = parallax.meanShiftKm;
  scan->unconverged = parallax.unconverged;
  validMean(scan->cloudMask, scan->cloudyFraction);
  scan->opticalDepthFraction = validMean(scan->opticalDepth, scan->meanOpticalDepth);
  scan->totalCoverFraction = validMean(scan->totalCover, scan->meanTotalCover);
  scan->bandFraction = layers ? 1.0 : 0.0;
  for (int b = 0; b < kGoesBandCount && layers; ++b) {
    scan->bandFraction = std::min(scan->bandFraction, validMean(scan->bandCover[size_t(b)], scan->meanBandCover[b]));
  }
  scan->decodeMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  out.scan = std::move(scan);
  return out;
}

} // namespace

const char* goesProductId(int product) { return kProductIds[std::clamp(product, 0, kGoesProductCount - 1)]; }

bool parseGoesFileName(const std::string& name, std::string& product, int64_t& start) {
  const size_t p = name.find("ABI-L2-");
  const size_t q = p == std::string::npos ? p : name.find("-M", p + 7);
  const size_t s = q == std::string::npos ? q : name.find("_s", q);
  if (s == std::string::npos || s + 15 > name.size()) {
    return false;
  }
  auto digits = [&](size_t at, int count) {
    int v = 0;
    for (int i = 0; i < count; ++i) {
      const char c = name[at + size_t(i)];
      if (c < '0' || c > '9') {
        return -1;
      }
      v = v * 10 + (c - '0');
    }
    return v;
  };
  const int year = digits(s + 2, 4), day = digits(s + 6, 3), hour = digits(s + 9, 2), minute = digits(s + 11, 2),
            second = digits(s + 13, 2);
  if (year < 1970 || day < 1 || hour < 0 || minute < 0 || second < 0) {
    return false;
  }
  int64_t days = 0;
  for (int y = 1970; y < year; ++y) {
    days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
  }
  product = name.substr(p + 7, q - p - 7);
  start = (days + day - 1) * 86400 + hour * 3600 + minute * 60 + second;
  return true;
}

int GoesSource::Candidate::mask() const {
  int m = 0;
  for (int p = 0; p < kGoesProductCount; ++p) {
    m |= files[size_t(p)].url.empty() ? 0 : 1 << p;
  }
  return m;
}

struct GoesSource::FetchResult {
  std::shared_ptr<GoesScan> scan;
  std::string downloadError; // a file could not be downloaded: retried
  std::string error;         // the scan cannot be decoded: not retried
  std::vector<std::string> errors;
};

GoesSource::GoesSource(std::shared_ptr<HttpAssetAccessor> accessor, AsyncSystem asyncSystem, Options options)
    : _accessor(std::move(accessor)), _asyncSystem(std::move(asyncSystem)), _options(std::move(options)) {}

void GoesSource::setGrid(const CloudGrid& grid) {
  if (grid == _options.grid) {
    return;
  }
  _options.grid = grid;
  ++_generation;
  _loaded.clear();
  _failed.clear();
  _fetchBusy = false;
  _activity.clear();
  _problem.clear();
  _reportedGap = 0;
}

std::shared_ptr<const GoesScan> GoesSource::scan(int64_t t) const {
  const auto after = _loaded.upper_bound(t);
  if (after == _loaded.begin() || t - std::prev(after)->first > kMaxAge) {
    return nullptr;
  }
  return std::prev(after)->second.scan;
}

void GoesSource::update(int64_t t, bool active) {
  if (!active) {
    _pending = false;
    return;
  }
  const int64_t now = unixNow();
  const auto clockNow = Clock::now();
  bool listing = false; // a needed hour has not been listed yet
  for (int64_t h = hourStart(t - kMaxAge); h <= hourStart(t) && h <= now; h += kHour) {
    const Listing& l = _hours[h];
    if (!l.busy && (!l.listed || !l.final) && clockNow >= l.next) {
      list(h);
    }
    listing |= !l.listed && !l.failed;
  }
  // Scrubbing through days leaves listings behind: keep those near the weather time.
  while (_hours.size() > 48) {
    auto farthest = _hours.begin();
    for (auto it = _hours.begin(); it != _hours.end(); ++it) {
      if (!it->second.busy && std::llabs(it->first - t) > std::llabs(farthest->first - t)) {
        farthest = it;
      }
    }
    if (farthest->second.busy) {
      break;
    }
    _hours.erase(farthest);
  }

  // Choose once every needed hour is listed (or failed): the hour that answers
  // first must not start the download of a scan the other one replaces.
  _pending = listing;
  _gap.clear();
  if (listing) {
    return;
  }
  const std::optional<Candidate> want = choose(t, _status);
  _status.wanted = want ? want->start : 0;
  _status.wantedComplete = want && want->mask() == kGoesAllProducts;
  if (!want) {
    _gap = "no GOES-19 scan in the 30 min before " + hhmm(t) + " UTC";
    if (hourStart(t) != _reportedGap) {
      _reportedGap = hourStart(t);
      LOG_I("Clouds: {} ({}){}", _gap, WeatherService::formatTime(t),
            _options.offline ? " in the disk cache (offline: finished hours replay once listed online)" : "");
    }
    return;
  }
  const auto loaded = _loaded.find(want->start);
  if (loaded == _loaded.end() || (want->mask() & ~loaded->second.requested) != 0) {
    _pending = true;
    if (!_fetchBusy) {
      fetch(*want);
    }
  }
  // Keep the scans nearest to the weather time (the current one + some for scrubbing).
  while (_loaded.size() > kMaxLoaded) {
    auto farthest = _loaded.begin();
    for (auto it = _loaded.begin(); it != _loaded.end(); ++it) {
      if (std::llabs(it->first - t) > std::llabs(farthest->first - t)) {
        farthest = it;
      }
    }
    _loaded.erase(farthest);
  }
}

std::optional<GoesSource::Candidate> GoesSource::choose(int64_t t, Status& status) const {
  std::map<int64_t, Candidate> scans;
  for (int64_t h = hourStart(t - kMaxAge); h <= hourStart(t); h += kHour) {
    const auto it = _hours.find(h);
    if (it == _hours.end() || !it->second.listed) {
      continue;
    }
    for (int p = 0; p < kGoesProductCount; ++p) {
      for (const auto& [start, file] : it->second.files[size_t(p)]) {
        if (start <= t && t - start <= kMaxAge) {
          Candidate& c = scans[start];
          c.start = start;
          c.files[size_t(p)] = file;
          c.cacheable = it->second.final;
        }
      }
    }
  }
  const auto clockNow = Clock::now();
  for (auto it = scans.begin(); it != scans.end();) {
    const auto failed = _failed.find(it->first);
    const bool skip = failed != _failed.end() && failed->second.first == it->second.mask() &&
                      clockNow < failed->second.second;
    it = skip ? scans.erase(it) : std::next(it);
  }
  status.scansListed = int(scans.size());
  status.newestListed = scans.empty() ? 0 : scans.rbegin()->first;
  for (auto it = scans.rbegin(); it != scans.rend(); ++it) {
    if (it->second.mask() == kGoesAllProducts) {
      return it->second;
    }
  }
  for (auto it = scans.rbegin(); it != scans.rend(); ++it) {
    if (it->second.mask() & (1 << kGoesCloudTop)) {
      return it->second;
    }
  }
  return std::nullopt;
}

void GoesSource::list(int64_t hour) {
  // A finished hour does not change: its listing goes through the disk cache
  // (replays, --offline). An unfinished one bypasses it (offline: whatever the
  // cache holds, i.e. nothing).
  const bool final = unixNow() >= hour + kHour + kFinishedAfter;
  std::vector<IAssetAccessor::THeader> headers;
  if (!final && !_options.offline) {
    headers.emplace_back("Cache-Control", "no-cache");
  }
  _hours[hour].busy = true;
  ++_listingsInFlight;
  std::vector<Future<std::shared_ptr<IAssetRequest>>> requests;
  for (int p = 0; p < kGoesProductCount; ++p) {
    requests.push_back(_accessor->get(_asyncSystem, listUrl(p, hour), headers));
  }
  std::weak_ptr<GoesSource> weak = weak_from_this();
  _asyncSystem.all(std::move(requests))
      .thenInMainThread([weak, hour, final](std::vector<std::shared_ptr<IAssetRequest>>&& responses) {
        const std::shared_ptr<GoesSource> self = weak.lock();
        if (!self) {
          return;
        }
        --self->_listingsInFlight;
        Listing& l = self->_hours[hour];
        l.busy = false;
        std::array<std::map<int64_t, ListedFile>, kGoesProductCount> files;
        std::string problem;
        int scans = 0;
        for (int p = 0; p < kGoesProductCount && problem.empty(); ++p) {
          const IAssetResponse* pResponse = responses[size_t(p)] ? responses[size_t(p)]->response() : nullptr;
          if (!pResponse || pResponse->statusCode() != 200) {
            problem = std::string(kProductIds[p]) + " " +
                      (pResponse ? "HTTP " + std::to_string(pResponse->statusCode()) : std::string("no response"));
            break;
          }
          bool truncated = false;
          for (const auto& [key, size] : parseListing(pResponse->data(), truncated)) {
            std::string product;
            int64_t start = 0;
            const std::string name = key.substr(key.rfind('/') + 1);
            if (!parseGoesFileName(name, product, start) || product != kProductIds[p]) {
              continue;
            }
            ListedFile& f = files[size_t(p)][start];
            const std::string url = kBucket + key;
            if (url > f.url) { // the same scan twice: the later file (creation time) wins
              f = ListedFile{url, size};
            }
          }
          if (truncated) {
            SPDLOG_LOGGER_WARN(Log::data(), "Clouds: GOES-19 listing {} is truncated (more than 1000 files per hour?)",
                               listUrl(p, hour));
          }
          scans = std::max(scans, int(files[size_t(p)].size()));
        }
        const std::string what = WeatherService::formatTime(hour).substr(0, 13) + " UTC";
        if (!problem.empty()) {
          // A failed refresh keeps the previous listing.
          l.failed = !l.listed;
          l.next = Clock::now() + std::chrono::minutes(2);
          self->_problem = "GOES-19 listing of " + what + " unavailable (" + problem + ")";
          if (self->_options.offline) {
            SPDLOG_LOGGER_DEBUG(Log::data(), "Clouds: {} (offline: not in the disk cache)", self->_problem);
          } else {
            SPDLOG_LOGGER_WARN(Log::data(), "Clouds: {}, retrying in 2 min: {}", self->_problem, listUrl(0, hour));
          }
          return;
        }
        l.files = std::move(files);
        l.listed = true;
        l.failed = false;
        l.final = final;
        l.next = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                    std::chrono::duration<double>(self->_options.refreshSeconds));
        int complete = 0;
        for (const auto& [start, file] : l.files[kGoesCloudTop]) {
          bool all = true;
          for (int p = 1; p < kGoesProductCount; ++p) {
            all = all && l.files[size_t(p)].count(start) != 0;
          }
          complete += all;
        }
        SPDLOG_LOGGER_DEBUG(Log::data(), "Clouds: GOES-19 hour {} listed{}: {} scans, {} with all four products", what,
                            final ? " (finished)" : "", scans, complete);
      });
}

void GoesSource::fetch(const Candidate& c) {
  _fetchBusy = true;
  _activity = "GOES-19 " + hhmm(c.start);
  // Files of an unfinished hour bypass the disk cache too: live scans could not
  // be found again offline anyway (their listing is not cached), ~9 MB each.
  std::vector<IAssetAccessor::THeader> headers;
  if (!c.cacheable && !_options.offline) {
    headers.emplace_back("Cache-Control", "no-cache");
  }
  std::vector<Future<std::shared_ptr<IAssetRequest>>> requests;
  std::vector<std::pair<int, std::string>> files; // product, file name
  for (int p = 0; p < kGoesProductCount; ++p) {
    const std::string& url = c.files[size_t(p)].url;
    if (!url.empty()) {
      requests.push_back(_accessor->get(_asyncSystem, url, headers));
      files.emplace_back(p, url.substr(url.rfind('/') + 1));
    }
  }
  const CloudGrid grid = _options.grid;
  const int generation = _generation, mask = c.mask();
  const int64_t start = c.start;
  std::weak_ptr<GoesSource> weak = weak_from_this();
  _asyncSystem.all(std::move(requests))
      .thenInWorkerThread([grid, start, files](std::vector<std::shared_ptr<IAssetRequest>>&& responses) {
        FetchResult result;
        std::vector<ScanFile> data;
        for (size_t i = 0; i < responses.size(); ++i) {
          const IAssetResponse* pResponse = responses[i] ? responses[i]->response() : nullptr;
          if (!pResponse || pResponse->statusCode() != 200) {
            result.downloadError = std::string(kProductIds[files[i].first]) + " " +
                                   (pResponse ? "HTTP " + std::to_string(pResponse->statusCode()) : "no response");
            return result;
          }
          const std::span<const std::byte> body = pResponse->data();
          data.push_back(ScanFile{files[i].first, files[i].second, std::vector<std::byte>(body.begin(), body.end())});
        }
        Decoded decoded = decodeScan(data, grid, start);
        result.scan = std::move(decoded.scan);
        result.error = std::move(decoded.error);
        result.errors = std::move(decoded.errors);
        return result;
      })
      .thenInMainThread([weak, generation, start, mask](FetchResult&& result) {
        const std::shared_ptr<GoesSource> self = weak.lock();
        if (!self || self->_generation != generation) {
          return; // the box moved meanwhile
        }
        self->_fetchBusy = false;
        self->_activity.clear();
        const std::string scan = "GOES-19 scan " + WeatherService::formatTime(start);
        for (const std::string& e : result.errors) {
          SPDLOG_LOGGER_ERROR(Log::data(), "Clouds: {}: {}", scan, e);
        }
        if (result.scan) {
          const GoesScan& s = *result.scan;
          std::string products;
          for (int p = 0; p < kGoesProductCount; ++p) {
            products += (s.products & (1 << p)) ? std::string(products.empty() ? "" : " ") + kProductIds[p] : "";
          }
          std::string text = fmt::format("cloud tops on {:.1f} % of the box (mean {:.1f} km, parallax shift {:.1f} km, "
                                         "{} cells unconverged)",
                                         s.cloudTopFraction * 100.0, s.meanCloudTop / 1000.0, s.meanShiftKm,
                                         s.unconverged);
          if (s.products & (1 << kGoesCloudMask)) {
            text += fmt::format(", cloud mask {:.1f} % cloudy", s.cloudyFraction * 100.0);
          }
          if (s.products & (1 << kGoesOpticalDepth)) {
            text += fmt::format(", optical depth {:.2f} on {:.1f} %", s.meanOpticalDepth, s.opticalDepthFraction * 100.0);
          }
          if (s.products & (1 << kGoesCoverLayers)) {
            text += fmt::format(", CCLC total cover {:.3f} on {:.1f} %, height bands CF1..CF5 {:.2f} / {:.2f} / "
                                "{:.2f} / {:.2f} / {:.2f} on {:.1f} %",
                                s.meanTotalCover, s.totalCoverFraction * 100.0, s.meanBandCover[0], s.meanBandCover[1],
                                s.meanBandCover[2], s.meanBandCover[3], s.meanBandCover[4], s.bandFraction * 100.0);
          }
          LOG_I("Clouds: {} ({}, {:.1f} MB): {}; decoded in {:.0f} ms; replay with --weather-time {}", scan, products,
                double(s.bytes) / (1024.0 * 1024.0), text, s.decodeMs,
                WeatherService::formatCompact((start + 59) / 60 * 60));
          if (mask != kGoesAllProducts) {
            LOG_I("Clouds: {}: not every product is listed for this scan (the newest complete one is older than "
                  "30 min)",
                  scan);
          }
          self->_loaded[start] = Loaded{std::move(result.scan), mask};
          self->_failed.erase(start);
          self->_problem.clear();
          return;
        }
        if (!result.downloadError.empty()) {
          // HTTP failures are logged by the accessor (offline misses at debug level);
          // an older scan is tried meanwhile.
          SPDLOG_LOGGER_DEBUG(Log::data(), "Clouds: {} not available ({})", scan, result.downloadError);
          self->_failed[start] = {mask, Clock::now() + std::chrono::minutes(2)};
          self->_problem = "GOES-19 " + hhmm(start) + " failed (" + result.downloadError + ")";
          return;
        }
        SPDLOG_LOGGER_ERROR(Log::data(), "Clouds: {}: {}", scan, result.error);
        self->_failed[start] = {mask, Clock::time_point::max()}; // the same files would fail again
        self->_problem = "GOES-19 " + hhmm(start) + ": " + result.error;
      });
}

std::string GoesSource::activity() const {
  std::string s;
  if (_listingsInFlight > 0) {
    s = "listing GOES-19 scans";
  }
  if (_fetchBusy) {
    s += (s.empty() ? "" : " | ") + std::string("loading ") + _activity;
  }
  for (const std::string* part : {&_gap, &_problem}) {
    if (!part->empty()) {
      s += (s.empty() ? "" : " | ") + *part;
    }
  }
  return s;
}
