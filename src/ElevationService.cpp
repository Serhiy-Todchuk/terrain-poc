#include "pch.h"
#include "ElevationService.h"

#include "GeoRef.h"
#include "Log.h"

#include <CesiumAsync/SharedFuture.h>

#include <list>

using namespace CesiumAsync;

namespace elevation_detail {

constexpr const char* k3depUrl =
    "https://elevation.nationalmap.gov/arcgis/rest/services/3DEPElevation/ImageServer";
constexpr const char* kBagUrl = "https://gis.ngdc.noaa.gov/arcgis/rest/services/bag_bathymetry/ImageServer";
constexpr const char* kNceiUrl =
    "https://gis.ngdc.noaa.gov/arcgis/rest/services/DEM_mosaics/DEM_global_mosaic/ImageServer";

struct Raster {
  enum class Status { NotRequested, Ok, Transient, Failed };
  Status status = Status::NotRequested;
  std::vector<float> values;
  std::vector<uint8_t> valid;
  std::string message;
  bool reused = false;

  bool ok() const { return status == Status::Ok; }
  bool has(size_t i) const { return ok() && valid[i] != 0; }
};

} // namespace elevation_detail

using elevation_detail::Raster;

// LRU cache of NCEI rasters for reference-level tiles, shared by descendants.
struct RasterCache {
  std::mutex mutex;
  std::list<uint64_t> lru;
  std::unordered_map<uint64_t, std::pair<SharedFuture<Raster>, std::list<uint64_t>::iterator>> map;
  size_t capacity = 512;
};

namespace {

using namespace elevation_detail;

// Rough CONUS extent for 3DEP and the US coastal extent for BAG surveys.
bool intersects3dep(const ElevationRequest& r) {
  return r.east > -125.0 && r.west < -66.0 && r.north > 24.0 && r.south < 50.0;
}
bool intersectsBag(const ElevationRequest& r) {
  return r.east > -126.0 && r.west < -65.0 && r.north > 23.0 && r.south < 50.0;
}

std::string exportImageUrl(const char* base, const ElevationRequest& r, int stride) {
  const double dLon = (r.east - r.west) / (r.n - 1);
  const double dLat = (r.north - r.south) / (r.n - 1);
  // Sample positions run from (west - dLon) to (east + dLon); ArcGIS pixel
  // centers sit half a pixel inside the bbox, so expand by half a pixel more.
  const double bw = std::max(r.west - 1.5 * dLon, -180.0);
  const double be = std::min(r.east + 1.5 * dLon, 180.0);
  const double bs = std::max(r.south - 1.5 * dLat, -90.0);
  const double bn = std::min(r.north + 1.5 * dLat, 90.0);
  char buf[512];
  snprintf(buf, sizeof(buf),
           "%s/exportImage?bbox=%.10f,%.10f,%.10f,%.10f&bboxSR=4326&imageSR=4326&size=%d,%d"
           "&format=bsq&pixelType=F32&interpolation=RSP_BilinearInterpolation&f=image",
           base, bw, bs, be, bn, stride, stride);
  return buf;
}

std::string snippet(std::span<const std::byte> data) {
  std::string s;
  for (size_t i = 0; i < std::min<size_t>(data.size(), 200); ++i) {
    char c = static_cast<char>(data[i]);
    s.push_back((c >= 32 && c < 127) ? c : '.');
  }
  return s;
}

Raster parseRaster(const std::shared_ptr<IAssetRequest>& pRequest, int stride) {
  Raster r;
  const IAssetResponse* pResponse = pRequest ? pRequest->response() : nullptr;
  if (!pResponse) {
    r.status = Raster::Status::Transient;
    r.message = "no response";
    return r;
  }
  const uint16_t status = pResponse->statusCode();
  if (status == 429 || status >= 500) {
    r.status = Raster::Status::Transient;
    r.message = "HTTP " + std::to_string(status);
    return r;
  }
  if (status != 200) {
    r.status = Raster::Status::Failed;
    r.message = "HTTP " + std::to_string(status) + ": " + snippet(pResponse->data());
    return r;
  }
  std::span<const std::byte> data = pResponse->data();
  const size_t count = size_t(stride) * stride;
  const size_t expected = count * sizeof(float);
  if (data.size() < expected) {
    r.status = Raster::Status::Failed;
    r.message = "unexpected payload (" + std::to_string(data.size()) + " bytes, " + pResponse->contentType() +
                "): " + snippet(data);
    return r;
  }
  r.values.resize(count);
  std::memcpy(r.values.data(), data.data(), expected);
  r.valid.assign(count, 1);
  const size_t maskBytes = (count + 7) / 8;
  if (data.size() >= expected + maskBytes) {
    const auto* mask = reinterpret_cast<const uint8_t*>(data.data() + expected);
    for (size_t i = 0; i < count; ++i) {
      r.valid[i] = (mask[i >> 3] >> (7 - (i & 7))) & 1u;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const float v = r.values[i];
    if (!std::isfinite(v) || v < -12000.0f || v > 10000.0f) {
      r.valid[i] = 0;
    }
  }
  r.status = Raster::Status::Ok;
  return r;
}

Future<Raster> fetchRaster(
    const AsyncSystem& asyncSystem,
    const std::shared_ptr<IAssetAccessor>& pAccessor,
    const char* base,
    const ElevationRequest& request,
    const std::shared_ptr<ElevationService::Counters>& pCounters) {
  const int stride = request.n + 2;
  ++pCounters->rasterRequests;
  return pAccessor->get(asyncSystem, exportImageUrl(base, request, stride))
      .thenInWorkerThread([stride](std::shared_ptr<IAssetRequest>&& pRequest) {
        Raster r = parseRaster(pRequest, stride);
        if (!r.ok()) {
          r.message += " [" + (pRequest ? pRequest->url() : std::string("?")) + "]";
        }
        return r;
      });
}

// Bilinear resampling of a raster sampled on grid `src` to the grid `dst`
// (both with a one-sample border ring). Samples whose 4 neighbours are not all
// valid take the nearest neighbour.
Raster resample(const Raster& in, const ElevationRequest& src, const ElevationRequest& dst) {
  Raster out;
  out.status = in.status;
  out.message = in.message;
  out.reused = true;
  if (!in.ok()) {
    return out;
  }
  const int sStride = src.n + 2, dStride = dst.n + 2;
  const double sdLon = (src.east - src.west) / (src.n - 1), sdLat = (src.north - src.south) / (src.n - 1);
  const double ddLon = (dst.east - dst.west) / (dst.n - 1), ddLat = (dst.north - dst.south) / (dst.n - 1);
  out.values.resize(size_t(dStride) * dStride);
  out.valid.resize(size_t(dStride) * dStride);
  for (int r = 0; r < dStride; ++r) {
    const double lat = dst.north - (r - 1) * ddLat;
    const double fr = std::clamp((src.north - lat) / sdLat + 1.0, 0.0, double(sStride - 1));
    const int r0 = std::min(int(fr), sStride - 2);
    const double tr = fr - r0;
    for (int c = 0; c < dStride; ++c) {
      const double lon = dst.west + (c - 1) * ddLon;
      const double fc = std::clamp((lon - src.west) / sdLon + 1.0, 0.0, double(sStride - 1));
      const int c0 = std::min(int(fc), sStride - 2);
      const double tc = fc - c0;
      const size_t i00 = size_t(r0) * sStride + c0, i01 = i00 + 1, i10 = i00 + sStride, i11 = i10 + 1;
      const size_t o = size_t(r) * dStride + c;
      if (in.valid[i00] && in.valid[i01] && in.valid[i10] && in.valid[i11]) {
        const double a = in.values[i00] * (1 - tc) + in.values[i01] * tc;
        const double b = in.values[i10] * (1 - tc) + in.values[i11] * tc;
        out.values[o] = float(a * (1 - tr) + b * tr);
        out.valid[o] = 1;
      } else {
        const size_t nearest = size_t(tr < 0.5 ? r0 : r0 + 1) * sStride + (tc < 0.5 ? c0 : c0 + 1);
        out.values[o] = in.values[nearest];
        out.valid[o] = in.valid[nearest];
      }
    }
  }
  return out;
}

// 3DEP hydro-flattens water bodies: water samples are exactly constant over
// their neighbourhood, which lidar-derived ground never is.
bool isHydroFlat(const Raster& a, int stride, int row, int col) {
  const size_t i = size_t(row) * stride + col;
  if (!a.has(i)) {
    return false;
  }
  const float v = a.values[i];
  int compared = 0;
  const int dr[4] = {-1, 1, 0, 0};
  const int dc[4] = {0, 0, -1, 1};
  for (int k = 0; k < 4; ++k) {
    int r = row + dr[k], c = col + dc[k];
    if (r < 0 || c < 0 || r >= stride || c >= stride) {
      continue;
    }
    const size_t j = size_t(r) * stride + c;
    if (!a.has(j)) {
      continue;
    }
    if (std::fabs(a.values[j] - v) > 0.003f) {
      return false;
    }
    ++compared;
  }
  return compared >= 2;
}

HeightGrid merge(const ElevationRequest& req, const Raster& a3dep, const Raster& bag, const Raster& ncei) {
  HeightGrid g;
  g.n = req.n;
  g.stride = req.n + 2;
  g.west = req.west;
  g.south = req.south;
  g.east = req.east;
  g.north = req.north;
  const size_t count = size_t(g.stride) * g.stride;
  g.heightNavd88.resize(count);
  g.water.resize(count);
  g.sources = (a3dep.ok() ? ElevationService::Usgs3dep : 0u) | (bag.ok() ? ElevationService::NoaaBag : 0u) |
              (ncei.ok() ? ElevationService::NoaaNcei : 0u);

  const double centerLon = 0.5 * (req.west + req.east);
  const double centerLat = 0.5 * (req.south + req.north);
  const float mllwOffset = static_cast<float>(GeoRef::navd88AboveMllw(centerLon, centerLat));
  constexpr float kHydroFlatMax = 3.0f; // flattened water surfaces near sea level
  constexpr float kNoDataSea = -5.0f;   // fallback when nothing covers a water sample

  float minH = std::numeric_limits<float>::max();
  float maxH = -std::numeric_limits<float>::max();
  for (int row = 0; row < g.stride; ++row) {
    for (int col = 0; col < g.stride; ++col) {
      const size_t i = size_t(row) * g.stride + col;
      const bool has3 = a3dep.has(i);
      const float h3 = has3 ? a3dep.values[i] : 0.0f;
      float bathy = std::numeric_limits<float>::quiet_NaN();
      if (bag.has(i)) {
        bathy = bag.values[i] - mllwOffset; // MLLW -> NAVD88
      } else if (ncei.has(i)) {
        bathy = ncei.values[i];
      }

      // 3DEP is authoritative on land. Where it is hydro-flattened (exactly
      // flat surface near sea level), or low-lying with the bathymetric
      // topobathy surface clearly deeper (coarse 3DEP overviews are not
      // perfectly flat), it is replaced by the seabed. The water mask then
      // simply marks samples below mean sea level.
      float h;
      if (has3) {
        const bool flat = h3 < kHydroFlatMax && isHydroFlat(a3dep, g.stride, row, col);
        const bool deeper = !std::isnan(bathy) && h3 < 1.0f && bathy < h3 - 0.25f;
        if ((flat || deeper) && !std::isnan(bathy)) {
          // Flattened at low water (well below MSL): everything flattened was
          // under water when the lidar was flown, so keep the seabed below that
          // surface even where older bathymetry shows fill. Flattening near
          // MSL (e.g. Jamaica Bay marsh) defers to the topobathy model.
          h = h3 < -0.5f ? std::min(bathy, h3 - 0.05f) : bathy;
        } else {
          h = h3;
        }
      } else if (!std::isnan(bathy)) {
        h = bathy;
        if (a3dep.ok()) {
          g.any3depNoData = true;
        }
      } else {
        h = kNoDataSea;
        if (a3dep.ok()) {
          g.any3depNoData = true;
        }
      }
      const bool water = h < float(GeoRef::kSeaLevelNavd88);
      g.heightNavd88[i] = h;
      g.water[i] = water ? 1 : 0;

      const bool inner = row >= 1 && col >= 1 && row <= g.n && col <= g.n;
      if (inner) {
        minH = std::min(minH, h);
        maxH = std::max(maxH, h);
        g.hasWater |= water;
        g.hasLand |= !water;
      }
    }
  }
  g.minH = minH;
  g.maxH = maxH;
  return g;
}

} // namespace

ElevationService::ElevationService(std::shared_ptr<IAssetAccessor> pAccessor, Options options)
    : _pAccessor(std::move(pAccessor)),
      _options(options),
      _pCounters(std::make_shared<Counters>()),
      _pCache(std::make_shared<RasterCache>()) {}

std::string ElevationService::sourceNames(unsigned sources) {
  std::string s;
  if (sources & Usgs3dep) s += "3DEP ";
  if (sources & NoaaBag) s += "BAG ";
  if (sources & NoaaNcei) s += "NCEI ";
  if (s.empty()) s = "flat ";
  s.pop_back();
  return s;
}

ElevationRequest ElevationService::geographicTile(int level, int64_t x, int64_t y, int n) {
  const double size = 180.0 / double(1ull << level);
  ElevationRequest r;
  r.west = -180.0 + double(x) * size;
  r.south = -90.0 + double(y) * size;
  r.east = r.west + size;
  r.north = r.south + size;
  r.n = n;
  r.level = level;
  r.tileX = x;
  r.tileY = y;
  return r;
}

HeightGrid ElevationService::makeFlatGrid(const ElevationRequest& req, float heightNavd88) {
  HeightGrid g;
  g.n = req.n;
  g.stride = req.n + 2;
  g.west = req.west;
  g.south = req.south;
  g.east = req.east;
  g.north = req.north;
  const size_t count = size_t(g.stride) * g.stride;
  g.heightNavd88.assign(count, heightNavd88);
  g.water.assign(count, 0);
  g.minH = g.maxH = heightNavd88;
  g.hasLand = true;
  return g;
}

Future<ElevationResult> ElevationService::fetch(const AsyncSystem& asyncSystem, const ElevationRequest& req) const {
  ++_pCounters->tiles;
  if (req.level < _options.minNceiLevel) {
    ElevationResult result;
    result.status = ElevationResult::Status::Ok;
    result.grid = makeFlatGrid(req, -1.0f);
    return asyncSystem.createResolvedFuture(std::move(result));
  }

  const bool want3dep = req.needLand && req.level >= _options.min3depLevel && intersects3dep(req);
  const bool wantBag = req.needBathymetry && req.level >= _options.minBagLevel && intersectsBag(req);
  // NCEI is the global fallback and the bathymetry source where BAG has gaps.
  const bool wantNcei = !want3dep || req.needBathymetry;

  auto notRequested = [&]() { return asyncSystem.createResolvedFuture(Raster{}); };
  std::vector<Future<Raster>> futures;
  futures.reserve(3);
  futures.emplace_back(want3dep ? fetchRaster(asyncSystem, _pAccessor, k3depUrl, req, _pCounters) : notRequested());
  futures.emplace_back(wantBag ? fetchRaster(asyncSystem, _pAccessor, kBagUrl, req, _pCounters) : notRequested());

  if (!wantNcei) {
    futures.emplace_back(notRequested());
  } else if (req.tileX >= 0 && req.level > _options.nceiReferenceLevel) {
    // Resample the cached raster of the reference-level ancestor.
    const int shift = req.level - _options.nceiReferenceLevel;
    const ElevationRequest ancestor =
        geographicTile(_options.nceiReferenceLevel, req.tileX >> shift, req.tileY >> shift, _options.gridSize);
    const uint64_t key = (uint64_t(ancestor.level) << 58) | (uint64_t(ancestor.tileX) << 29) | uint64_t(ancestor.tileY);
    std::optional<SharedFuture<Raster>> shared;
    {
      std::lock_guard lock(_pCache->mutex);
      auto it = _pCache->map.find(key);
      if (it != _pCache->map.end()) {
        _pCache->lru.splice(_pCache->lru.begin(), _pCache->lru, it->second.second);
        shared = it->second.first;
        ++_pCounters->cacheReuse;
      } else {
        SharedFuture<Raster> f = fetchRaster(asyncSystem, _pAccessor, kNceiUrl, ancestor, _pCounters).share();
        _pCache->lru.push_front(key);
        _pCache->map.emplace(key, std::make_pair(f, _pCache->lru.begin()));
        while (_pCache->map.size() > _pCache->capacity) {
          _pCache->map.erase(_pCache->lru.back());
          _pCache->lru.pop_back();
        }
        shared = f;
      }
    }
    std::weak_ptr<RasterCache> pWeakCache = _pCache;
    futures.emplace_back(shared->thenInWorkerThread([ancestor, req, key, pWeakCache](const Raster& r) {
      if (r.status == Raster::Status::Transient) {
        // Do not keep failed downloads in the cache.
        if (auto pCache = pWeakCache.lock()) {
          std::lock_guard lock(pCache->mutex);
          auto it = pCache->map.find(key);
          if (it != pCache->map.end()) {
            pCache->lru.erase(it->second.second);
            pCache->map.erase(it);
          }
        }
      }
      return resample(r, ancestor, req);
    }));
  } else {
    futures.emplace_back(fetchRaster(asyncSystem, _pAccessor, kNceiUrl, req, _pCounters));
  }

  return asyncSystem.all(std::move(futures))
      .thenInWorkerThread([req, pCounters = _pCounters](std::vector<Raster>&& rasters) {
        ElevationResult result;
        const Raster& a3dep = rasters[0];
        const Raster& bag = rasters[1];
        const Raster& ncei = rasters[2];

        std::string problems;
        bool transient = false;
        const char* names[3] = {"3DEP", "BAG", "NCEI"};
        for (int k = 0; k < 3; ++k) {
          const Raster& r = rasters[size_t(k)];
          if (r.status == Raster::Status::Transient || r.status == Raster::Status::Failed) {
            ++pCounters->rasterFailures;
            transient |= r.status == Raster::Status::Transient;
            problems += std::string(names[k]) + ": " + r.message + "; ";
          }
        }
        if (transient) {
          ++pCounters->retries;
          result.status = ElevationResult::Status::RetryLater;
          result.message = problems;
          return result;
        }
        if (!problems.empty()) {
          SPDLOG_LOGGER_WARN(Log::data(), "Tile L{} [{:.5f},{:.5f}]: degraded elevation ({})", req.level,
                             req.west, req.south, problems);
        }
        if (!a3dep.ok() && !bag.ok() && !ncei.ok()) {
          result.status = ElevationResult::Status::Ok;
          result.grid = ElevationService::makeFlatGrid(req, -1.0f);
          result.message = "no elevation source available";
          return result;
        }
        result.grid = merge(req, a3dep, bag, ncei);
        result.status = ElevationResult::Status::Ok;
        return result;
      });
}
