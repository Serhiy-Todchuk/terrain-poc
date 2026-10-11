#pragma once
#include "ElevationService.h"

#include <Cesium3DTilesSelection/Tileset.h>
#include <Cesium3DTilesSelection/TilesetContentLoader.h>
#include <CesiumGeometry/QuadtreeTileID.h>
#include <CesiumGeospatial/GlobeRectangle.h>

#include <memory>
#include <string>

struct BuildingsOptions {
  std::string tileUrlTemplate; // OpenMapTiles vector tiles, {z}/{x}/{y}
  int contentZoom = 14;        // OpenFreeMap carries buildings at z14
  int rootZoom = 10;
  double baseGeometricError = 100.0; // geometric error of z13 tiles
  // Region in which building tiles are generated.
  double west = -75.2, south = 40.0, east = -72.8, north = 41.4;
};

// Cesium Native content loader producing simple extruded 3D buildings from
// OpenStreetMap footprints (OpenMapTiles schema: render_height /
// render_min_height) placed on the USGS 3DEP ground surface. Tiles follow the
// Web Mercator XYZ quadtree (QuadtreeTileID.y counts from the north).
class BuildingsLoader final : public Cesium3DTilesSelection::TilesetContentLoader {
public:
  BuildingsLoader(std::shared_ptr<ElevationService> pElevation, BuildingsOptions options);

  static std::unique_ptr<Cesium3DTilesSelection::Tileset> createTileset(
      const Cesium3DTilesSelection::TilesetExternals& externals,
      const Cesium3DTilesSelection::TilesetOptions& options,
      std::shared_ptr<ElevationService> pElevation,
      const BuildingsOptions& buildingsOptions);

  CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResult>
  loadTileContent(const Cesium3DTilesSelection::TileLoadInput& input) override;

  Cesium3DTilesSelection::TileChildrenResult createTileChildren(
      const Cesium3DTilesSelection::Tile& tile,
      const CesiumGeospatial::Ellipsoid& ellipsoid) override;

  static CesiumGeospatial::GlobeRectangle tileRectangle(const CesiumGeometry::QuadtreeTileID& id);

private:
  void addTile(std::vector<Cesium3DTilesSelection::Tile>& out, const CesiumGeometry::QuadtreeTileID& id);
  double geometricError(uint32_t zoom) const;

  std::shared_ptr<ElevationService> _pElevation;
  BuildingsOptions _options;
};
