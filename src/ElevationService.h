#pragma once
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumAsync/Future.h>
#include <CesiumAsync/IAssetAccessor.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// A merged land + seabed height grid for one terrain tile.
// The grid has n x n "inner" samples whose corner samples lie exactly on the
// tile corners, plus a one-sample border ring (stride = n + 2) used for
// seamless normals and water classification. Row 0 is the northern border.
struct HeightGrid {
  int n = 0;
  int stride = 0;
  double west = 0, south = 0, east = 0, north = 0; // degrees, inner corners
  std::vector<float> heightNavd88;                 // stride * stride
  std::vector<uint8_t> water;                      // 1 = seabed / water body
  float minH = 0, maxH = 0;                        // inner samples (NAVD88)
  bool hasWater = false;
  bool hasLand = false;
  bool any3depNoData = false;
  unsigned sources = 0; // bitmask of ElevationService::Source

  float at(int row, int col) const { return heightNavd88[size_t(row) * stride + col]; }
};

struct ElevationRequest {
  double west = 0, south = 0, east = 0, north = 0; // degrees, inner corners
  int n = 65;
  int level = 0;
  bool needBathymetry = true; // false when the parent tile had no water
  bool needLand = true;       // false when the parent tile had no land
  // Geographic quadtree tile (180/2^level degrees, y from the south) when the
  // request is for a terrain tile; enables reuse of cached ancestor rasters.
  int64_t tileX = -1, tileY = -1;
};

struct ElevationResult {
  enum class Status { Ok, RetryLater, Failed };
  Status status = Status::Failed;
  HeightGrid grid;
  std::string message;
};

class ElevationService {
public:
  enum Source : unsigned {
    Usgs3dep = 1u << 0, // USGS 3DEP bare earth DEM (1 m lidar where available), NAVD88
    NoaaBag = 1u << 1,  // NOAA NCEI BAG hydrographic surveys (0.5-4 m), MLLW
    NoaaNcei = 1u << 2, // NOAA NCEI DEM global mosaic (CUDEM 1/9" topobathy, ETOPO), NAVD88/MSL
  };

  struct Options {
    int minNceiLevel = 5;        // below: flat ellipsoid at sea level
    int min3depLevel = 9;        // 3DEP (CONUS) from this level
    int minBagLevel = 14;        // BAG mosaic only serves pixel sizes <= 0.00016 deg
    int nceiReferenceLevel = 15; // deeper tiles resample the cached NCEI raster of
                                 // their ancestor at this level (CUDEM is ~3 m)
    int gridSize = 129;          // samples per edge used for reference rasters
  };

  ElevationService(std::shared_ptr<CesiumAsync::IAssetAccessor> pAccessor, Options options);

  CesiumAsync::Future<ElevationResult> fetch(
      const CesiumAsync::AsyncSystem& asyncSystem,
      const ElevationRequest& request) const;

  // Builds a flat sea-level grid (used for coarse global levels and fallbacks).
  static HeightGrid makeFlatGrid(const ElevationRequest& request, float heightNavd88);

  static std::string sourceNames(unsigned sources);

  struct Counters {
    std::atomic<int64_t> tiles{0}, rasterRequests{0}, rasterFailures{0}, retries{0}, cacheReuse{0};
  };
  const Counters& counters() const { return *_pCounters; }

  static ElevationRequest geographicTile(int level, int64_t x, int64_t y, int n);

private:
  std::shared_ptr<CesiumAsync::IAssetAccessor> _pAccessor;
  Options _options;
  std::shared_ptr<Counters> _pCounters;
  std::shared_ptr<struct RasterCache> _pCache;
};
