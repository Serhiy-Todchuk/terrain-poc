#pragma once
#include "ElevationService.h"

#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetContentLoader.h>
#include <CesiumGeometry/QuadtreeTileID.h>
#include <CesiumGeospatial/GlobeRectangle.h>

#include <memory>
#include <mutex>
#include <unordered_map>

struct TerrainOptions {
  int gridSize = 129;    // samples per tile edge
  int maxLevel = 18;     // inside the high-resolution region (0.6 m spacing at 129)
  int maxLevelUs = 16;   // rest of CONUS (3DEP)
  int maxLevelGlobal = 11;
  // High-resolution region (greater New York + approaches).
  double hiWest = -75.2, hiSouth = 40.0, hiEast = -72.8, hiNorth = 41.4;
  double errorFactor = 2.0; // geometric error = factor * vertex spacing
};

// Cesium Native content loader that builds a quantized-mesh-like geographic
// quadtree terrain on the fly from online elevation rasters (USGS 3DEP + NOAA
// bathymetry merged by ElevationService). Cesium Native drives LOD selection,
// culling, loading priorities, caching and raster overlay draping; this class
// only produces the glTF content of each tile.
//
// Each tile mesh carries the custom vertex attributes
//   _HEIGHT     orthometric height (NAVD88, meters)
//   _WATERMASK  1 for seabed / water-body samples, 0 for land
class TerrainLoader final : public Cesium3DTilesSelection::TilesetContentLoader {
public:
  TerrainLoader(std::shared_ptr<ElevationService> pElevation, const TerrainOptions& options);

  static std::unique_ptr<Cesium3DTilesSelection::Tileset> createTileset(
      const Cesium3DTilesSelection::TilesetExternals& externals,
      const Cesium3DTilesSelection::TilesetOptions& options,
      std::shared_ptr<ElevationService> pElevation,
      const TerrainOptions& terrainOptions);

  CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResult>
  loadTileContent(const Cesium3DTilesSelection::TileLoadInput& input) override;

  Cesium3DTilesSelection::TileChildrenResult createTileChildren(
      const Cesium3DTilesSelection::Tile& tile,
      const CesiumGeospatial::Ellipsoid& ellipsoid) override;

  static CesiumGeospatial::GlobeRectangle tileRectangle(const CesiumGeometry::QuadtreeTileID& id);

private:
  struct TileInfo {
    float minHeight = 0.0f; // NAVD88, inner samples
    float maxHeight = 0.0f;
    bool any3depNoData = true;
  };
  struct InfoStore {
    std::mutex mutex;
    std::unordered_map<uint64_t, TileInfo> map;
  };
  static uint64_t key(const CesiumGeometry::QuadtreeTileID& id) {
    return (uint64_t(id.level) << 58) | (uint64_t(id.x) << 29) | uint64_t(id.y);
  }

  void createChild(
      const Cesium3DTilesSelection::Tile& parent,
      std::vector<Cesium3DTilesSelection::Tile>& children,
      const CesiumGeometry::QuadtreeTileID& id) const;
  int maxLevelFor(const CesiumGeospatial::GlobeRectangle& rect) const;
  double geometricError(uint32_t level) const;

  std::shared_ptr<ElevationService> _pElevation;
  TerrainOptions _options;
  std::shared_ptr<InfoStore> _pInfo;
};
