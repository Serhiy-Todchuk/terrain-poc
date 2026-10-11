#include "pch.h"
#include "TerrainLoader.h"

#include "GeoRef.h"
#include "GltfBuilder.h"
#include "Log.h"

#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/TileLoadResult.h>
#include <CesiumGeospatial/GeographicProjection.h>

using namespace Cesium3DTilesSelection;
using namespace CesiumAsync;
using namespace CesiumGeometry;
using namespace CesiumGeospatial;

namespace {

struct BuiltMesh {
  CesiumGltf::Model model;
  double minHeight = 0;
  double maxHeight = 0;
};

BuiltMesh buildTerrainMesh(const HeightGrid& g, const glm::dvec3& center, double skirtHeight) {
  const Ellipsoid& ellipsoid = GeoRef::ellipsoid();
  const int n = g.n;
  const int stride = g.stride;
  const double dLon = (g.east - g.west) / (n - 1);
  const double dLat = (g.north - g.south) / (n - 1);

  // ECEF positions of all samples including the border ring.
  std::vector<glm::dvec3> ecef(size_t(stride) * stride);
  for (int row = 0; row < stride; ++row) {
    const double lat = std::clamp(g.north - (row - 1) * dLat, -90.0, 90.0);
    for (int col = 0; col < stride; ++col) {
      const double lon = g.west + (col - 1) * dLon;
      const double h = g.at(row, col) + GeoRef::geoidHeight(lon, lat);
      ecef[size_t(row) * stride + col] =
          ellipsoid.cartographicToCartesian(Cartographic::fromDegrees(lon, lat, h));
    }
  }
  auto P = [&](int row, int col) -> const glm::dvec3& { return ecef[size_t(row) * stride + col]; };

  const size_t innerCount = size_t(n) * n;
  const size_t skirtCount = size_t(4) * n;
  std::vector<glm::vec3> positions;
  std::vector<glm::vec3> normals;
  std::vector<float> heights;
  std::vector<float> water;
  positions.reserve(innerCount + skirtCount);
  normals.reserve(innerCount + skirtCount);
  heights.reserve(innerCount + skirtCount);
  water.reserve(innerCount + skirtCount);

  BuiltMesh out;
  out.minHeight = std::numeric_limits<double>::max();
  out.maxHeight = -std::numeric_limits<double>::max();

  for (int row = 1; row <= n; ++row) {
    for (int col = 1; col <= n; ++col) {
      const glm::dvec3& p = P(row, col);
      glm::dvec3 east = P(row, col + 1) - P(row, col - 1);
      glm::dvec3 north = P(row - 1, col) - P(row + 1, col);
      glm::dvec3 normal = glm::cross(east, north);
      const double len = glm::length(normal);
      normal = len > 1e-12 ? normal / len : ellipsoid.geodeticSurfaceNormal(p);
      positions.emplace_back(glm::vec3(p - center));
      normals.emplace_back(glm::vec3(normal));
      heights.push_back(g.at(row, col));
      water.push_back(float(g.water[size_t(row) * stride + col]));

      const double lat = std::clamp(g.north - (row - 1) * dLat, -90.0, 90.0);
      const double lon = g.west + (col - 1) * dLon;
      const double hEll = g.at(row, col) + GeoRef::geoidHeight(lon, lat);
      out.minHeight = std::min(out.minHeight, hEll);
      out.maxHeight = std::max(out.maxHeight, hEll);
    }
  }

  std::vector<uint32_t> indices;
  indices.reserve(size_t(n - 1) * (n - 1) * 6 + size_t(4) * (n - 1) * 6);
  auto vid = [n](int r, int c) { return uint32_t(r * n + c); }; // inner grid, 0-based
  for (int r = 0; r < n - 1; ++r) {
    for (int c = 0; c < n - 1; ++c) {
      const uint32_t nw = vid(r, c), ne = vid(r, c + 1), sw = vid(r + 1, c), se = vid(r + 1, c + 1);
      // Split along the diagonal whose end points have the most similar
      // heights, so creases follow bulkheads, shorelines and channel edges
      // instead of zig-zagging across them. Triangles are CCW seen from above.
      const float dNwSe = std::fabs(heights[nw] - heights[se]);
      const float dNeSw = std::fabs(heights[ne] - heights[sw]);
      if (dNwSe < dNeSw) {
        indices.insert(indices.end(), {nw, sw, se, nw, se, ne});
      } else {
        indices.insert(indices.end(), {sw, se, ne, sw, ne, nw});
      }
    }
  }

  // Skirts hide cracks between neighbouring tiles of different LOD.
  auto addSkirt = [&](auto edgeVertex) {
    const uint32_t base = uint32_t(positions.size());
    for (int i = 0; i < n; ++i) {
      const uint32_t v = edgeVertex(i);
      const glm::dvec3 p = glm::dvec3(positions[v]) + center;
      const glm::dvec3 down = -ellipsoid.geodeticSurfaceNormal(p);
      positions.emplace_back(glm::vec3(p + down * skirtHeight - center));
      normals.push_back(normals[v]);
      heights.push_back(heights[v]);
      water.push_back(water[v]);
    }
    for (int i = 0; i < n - 1; ++i) {
      const uint32_t a0 = edgeVertex(i), a1 = edgeVertex(i + 1);
      const uint32_t s0 = base + uint32_t(i), s1 = base + uint32_t(i + 1);
      indices.insert(indices.end(), {a0, s0, a1, a1, s0, s1});
    }
  };
  addSkirt([&](int i) { return vid(0, i); });         // north edge
  addSkirt([&](int i) { return vid(i, n - 1); });     // east edge
  addSkirt([&](int i) { return vid(n - 1, i); });     // south edge
  addSkirt([&](int i) { return vid(i, 0); });         // west edge

  GltfMeshBuilder builder;
  builder.setPositions(positions);
  builder.setNormals(normals);
  builder.addScalarAttribute("_HEIGHT", heights);
  builder.addScalarAttribute("_WATERMASK", water);
  if (positions.size() <= 65535) {
    std::vector<uint16_t> idx16(indices.begin(), indices.end());
    builder.setIndices(idx16);
  } else {
    builder.setIndices(indices);
  }
  out.model = builder.build();
  return out;
}

} // namespace

TerrainLoader::TerrainLoader(std::shared_ptr<ElevationService> pElevation, const TerrainOptions& options)
    : _pElevation(std::move(pElevation)), _options(options), _pInfo(std::make_shared<InfoStore>()) {}

std::unique_ptr<Tileset> TerrainLoader::createTileset(
    const TilesetExternals& externals,
    const TilesetOptions& options,
    std::shared_ptr<ElevationService> pElevation,
    const TerrainOptions& terrainOptions) {
  auto pLoader = std::make_unique<TerrainLoader>(std::move(pElevation), terrainOptions);
  auto pRoot = std::make_unique<Tile>(pLoader.get(), TileID(), TileEmptyContent{});
  pRoot->setRefine(TileRefine::Replace);
  pRoot->setUnconditionallyRefine();
  pRoot->setBoundingVolume(
      BoundingRegion(GeographicProjection::MAXIMUM_GLOBE_RECTANGLE, -11000.0, 9000.0, GeoRef::ellipsoid()));

  std::vector<Tile> children;
  children.reserve(2);
  for (uint32_t x = 0; x < 2; ++x) {
    pLoader->createChild(*pRoot, children, QuadtreeTileID(0, x, 0));
  }
  pRoot->createChildTiles(std::move(children));
  return std::make_unique<Tileset>(externals, std::move(pLoader), std::move(pRoot), options);
}

GlobeRectangle TerrainLoader::tileRectangle(const QuadtreeTileID& id) {
  const double size = 180.0 / double(1ull << id.level);
  const double west = -180.0 + id.x * size;
  const double south = -90.0 + id.y * size;
  return GlobeRectangle::fromDegrees(west, south, west + size, south + size);
}

int TerrainLoader::maxLevelFor(const GlobeRectangle& rect) const {
  using CesiumUtility::Math;
  const double w = Math::radiansToDegrees(rect.getWest()), e = Math::radiansToDegrees(rect.getEast());
  const double s = Math::radiansToDegrees(rect.getSouth()), n = Math::radiansToDegrees(rect.getNorth());
  if (e > _options.hiWest && w < _options.hiEast && n > _options.hiSouth && s < _options.hiNorth) {
    return _options.maxLevel;
  }
  if (e > -125.0 && w < -66.0 && n > 24.0 && s < 50.0) {
    return _options.maxLevelUs;
  }
  return _options.maxLevelGlobal;
}

double TerrainLoader::geometricError(uint32_t level) const {
  const double widthRadians = CesiumUtility::Math::OnePi / double(1ull << level);
  const double spacing = widthRadians * GeoRef::ellipsoid().getMaximumRadius() / (_options.gridSize - 1);
  return _options.errorFactor * spacing;
}

void TerrainLoader::createChild(const Tile& parent, std::vector<Tile>& children, const QuadtreeTileID& id) const {
  const GlobeRectangle rect = tileRectangle(id);

  double minH = -11000.0, maxH = 9000.0;
  const BoundingVolume& parentVolume = parent.getBoundingVolume();
  if (const auto* pRegion = std::get_if<BoundingRegion>(&parentVolume)) {
    // Parent content is loaded: its exact height range bounds the children
    // (with a margin because higher-resolution data can exceed it slightly).
    const double margin = 10.0 + 0.1 * (pRegion->getMaximumHeight() - pRegion->getMinimumHeight());
    minH = pRegion->getMinimumHeight() - margin;
    maxH = pRegion->getMaximumHeight() + margin;
  } else if (const auto* pLoose = std::get_if<BoundingRegionWithLooseFittingHeights>(&parentVolume)) {
    minH = pLoose->getBoundingRegion().getMinimumHeight();
    maxH = pLoose->getBoundingRegion().getMaximumHeight();
  }
  if (std::holds_alternative<QuadtreeTileID>(parent.getTileID()) == false) {
    minH = -11000.0;
    maxH = 9000.0;
  }
  if (maxLevelFor(rect) == _options.maxLevel && id.level >= 8) {
    // Greater New York: seabed > -80 m, terrain < 600 m (ellipsoidal).
    minH = std::max(minH, -150.0);
    maxH = std::min(maxH, 650.0);
    if (minH >= maxH) {
      minH = -150.0;
      maxH = 650.0;
    }
  }

  Tile& child = children.emplace_back(const_cast<TerrainLoader*>(this));
  child.setTileID(id);
  child.setRefine(TileRefine::Replace);
  child.setTransform(
      glm::translate(glm::dmat4(1.0), GeoRef::ellipsoid().cartographicToCartesian(rect.computeCenter())));
  child.setBoundingVolume(
      BoundingRegionWithLooseFittingHeights(BoundingRegion(rect, minH, maxH, GeoRef::ellipsoid())));
  child.setGeometricError(geometricError(id.level));
}

TileChildrenResult TerrainLoader::createTileChildren(const Tile& tile, const Ellipsoid& /*ellipsoid*/) {
  const QuadtreeTileID* pId = std::get_if<QuadtreeTileID>(&tile.getTileID());
  if (!pId) {
    return {{}, TileLoadResultState::Failed};
  }
  if (int(pId->level) >= maxLevelFor(tileRectangle(*pId))) {
    return {{}, TileLoadResultState::Failed};
  }
  std::vector<Tile> children;
  children.reserve(4);
  const uint32_t level = pId->level + 1;
  const uint32_t x = pId->x * 2;
  const uint32_t y = pId->y * 2;
  createChild(tile, children, QuadtreeTileID(level, x, y));
  createChild(tile, children, QuadtreeTileID(level, x + 1, y));
  createChild(tile, children, QuadtreeTileID(level, x, y + 1));
  createChild(tile, children, QuadtreeTileID(level, x + 1, y + 1));
  return {std::move(children), TileLoadResultState::Success};
}

Future<TileLoadResult> TerrainLoader::loadTileContent(const TileLoadInput& input) {
  const QuadtreeTileID* pId = std::get_if<QuadtreeTileID>(&input.tile.getTileID());
  if (!pId) {
    return input.asyncSystem.createResolvedFuture(TileLoadResult::createFailedResult(input.pAssetAccessor, nullptr));
  }
  const QuadtreeTileID id = *pId;
  const GlobeRectangle rect = tileRectangle(id);

  ElevationRequest request;
  request.west = CesiumUtility::Math::radiansToDegrees(rect.getWest());
  request.south = CesiumUtility::Math::radiansToDegrees(rect.getSouth());
  request.east = CesiumUtility::Math::radiansToDegrees(rect.getEast());
  request.north = CesiumUtility::Math::radiansToDegrees(rect.getNorth());
  request.n = _options.gridSize;
  request.level = int(id.level);
  request.tileX = id.x;
  request.tileY = id.y;
  if (id.level > 0) {
    // The parent's content tells which sources this tile can skip.
    std::lock_guard lock(_pInfo->mutex);
    auto it = _pInfo->map.find(key(QuadtreeTileID(id.level - 1, id.x / 2, id.y / 2)));
    if (it != _pInfo->map.end()) {
      // Bathymetry only matters near or below sea level; 3DEP only above it.
      if (it->second.minHeight > 2.0f && !it->second.any3depNoData) {
        request.needBathymetry = false;
      }
      if (it->second.maxHeight < -1.5f && id.level >= 12) {
        request.needLand = false;
      }
    }
  }

  const glm::dvec3 center(input.tile.getTransform()[3]);
  const double skirtHeight = std::max(5.0, 2.0 * input.tile.getGeometricError());
  std::shared_ptr<IAssetAccessor> pAccessor = input.pAssetAccessor;
  std::shared_ptr<InfoStore> pInfo = _pInfo;

  return _pElevation->fetch(input.asyncSystem, request)
      .thenInWorkerThread([id, rect, center, skirtHeight, pAccessor, pInfo](ElevationResult&& er) {
        if (er.status == ElevationResult::Status::RetryLater) {
          SPDLOG_LOGGER_WARN(Log::data(), "Terrain tile L{} x{} y{}: will retry ({})", id.level, id.x, id.y,
                             er.message);
          return TileLoadResult::createRetryLaterResult(pAccessor, nullptr);
        }
        if (er.status == ElevationResult::Status::Failed) {
          SPDLOG_LOGGER_ERROR(Log::data(), "Terrain tile L{} x{} y{} failed: {}", id.level, id.x, id.y,
                              er.message);
          return TileLoadResult::createFailedResult(pAccessor, nullptr);
        }
        {
          std::lock_guard lock(pInfo->mutex);
          pInfo->map[key(id)] = TileInfo{er.grid.minH, er.grid.maxH, er.grid.any3depNoData};
        }
        BuiltMesh mesh = buildTerrainMesh(er.grid, center, skirtHeight);
        SPDLOG_LOGGER_DEBUG(Log::data(), "Terrain tile L{} x{} y{}: {} | NAVD88 [{:.1f}, {:.1f}] water={} land={}",
                            id.level, id.x, id.y, ElevationService::sourceNames(er.grid.sources), er.grid.minH,
                            er.grid.maxH, er.grid.hasWater, er.grid.hasLand);

        TileLoadResult result;
        result.contentKind = std::move(mesh.model);
        result.glTFUpAxis = CesiumGeometry::Axis::Z;
        result.updatedBoundingVolume = BoundingRegion(rect, mesh.minHeight, mesh.maxHeight, GeoRef::ellipsoid());
        result.pAssetAccessor = pAccessor;
        result.state = TileLoadResultState::Success;
        result.ellipsoid = GeoRef::ellipsoid();
        return result;
      });
}
