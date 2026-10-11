#include "pch.h"
#include "BuildingsLoader.h"

#include "GeoRef.h"
#include "GltfBuilder.h"
#include "Log.h"
#include "MvtReader.h"

#include <Cesium3DTilesSelection/Tile.h>
#include <Cesium3DTilesSelection/TileLoadResult.h>
#include <CesiumAsync/IAssetResponse.h>

#include <mapbox/earcut.hpp>

using namespace Cesium3DTilesSelection;
using namespace CesiumAsync;
using namespace CesiumGeometry;
using namespace CesiumGeospatial;

namespace {

constexpr double kPi = 3.14159265358979323846;

double tileXToLon(double x, double n) { return x / n * 360.0 - 180.0; }
double tileYToLat(double y, double n) {
  return std::atan(std::sinh(kPi * (1.0 - 2.0 * y / n))) * 180.0 / kPi;
}
uint32_t lonToTileX(double lon, uint32_t n) {
  return uint32_t(std::clamp((lon + 180.0) / 360.0 * n, 0.0, double(n - 1)));
}
uint32_t latToTileY(double lat, uint32_t n) {
  const double r = lat * kPi / 180.0;
  const double y = (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / kPi) / 2.0 * n;
  return uint32_t(std::clamp(y, 0.0, double(n - 1)));
}

using Vec2 = std::array<double, 2>;
using Ring2 = std::vector<Vec2>;

// Sutherland-Hodgman clip of a ring against the square [0, ext]^2.
Ring2 clipRing(const Ring2& ring, double ext) {
  Ring2 current = ring;
  for (int edge = 0; edge < 4 && !current.empty(); ++edge) {
    auto inside = [&](const Vec2& p) {
      switch (edge) {
      case 0: return p[0] >= 0.0;
      case 1: return p[0] <= ext;
      case 2: return p[1] >= 0.0;
      default: return p[1] <= ext;
      }
    };
    auto intersect = [&](const Vec2& a, const Vec2& b) {
      const double boundary = (edge == 0 || edge == 2) ? 0.0 : ext;
      const int axis = edge < 2 ? 0 : 1;
      const double t = (boundary - a[axis]) / (b[axis] - a[axis]);
      return Vec2{a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])};
    };
    Ring2 output;
    for (size_t i = 0; i < current.size(); ++i) {
      const Vec2& cur = current[i];
      const Vec2& prev = current[(i + current.size() - 1) % current.size()];
      const bool curIn = inside(cur), prevIn = inside(prev);
      if (curIn) {
        if (!prevIn) {
          output.push_back(intersect(prev, cur));
        }
        output.push_back(cur);
      } else if (prevIn) {
        output.push_back(intersect(prev, cur));
      }
    }
    current = std::move(output);
  }
  return current.size() >= 3 ? current : Ring2{};
}

// Bilinear sampling of the ElevationService grid (NAVD88 meters).
struct GroundSampler {
  const HeightGrid& g;
  double sample(double lon, double lat) const {
    const double dLon = (g.east - g.west) / (g.n - 1);
    const double dLat = (g.north - g.south) / (g.n - 1);
    const double fc = std::clamp((lon - g.west) / dLon + 1.0, 0.0, double(g.stride - 1));
    const double fr = std::clamp((g.north - lat) / dLat + 1.0, 0.0, double(g.stride - 1));
    const int c0 = std::min(int(fc), g.stride - 2), r0 = std::min(int(fr), g.stride - 2);
    const double tc = fc - c0, tr = fr - r0;
    const double a = g.at(r0, c0) * (1 - tc) + g.at(r0, c0 + 1) * tc;
    const double b = g.at(r0 + 1, c0) * (1 - tc) + g.at(r0 + 1, c0 + 1) * tc;
    return a * (1 - tr) + b * tr;
  }
};

glm::dvec3 parseColour(const std::string& s, bool& ok) {
  ok = true;
  if (s.size() == 7 && s[0] == '#') {
    unsigned v = 0;
    if (sscanf_s(s.c_str() + 1, "%x", &v) == 1) {
      return glm::dvec3((v >> 16) & 255, (v >> 8) & 255, v & 255) / 255.0;
    }
  }
  static const std::map<std::string, glm::dvec3> named = {
      {"white", {0.95, 0.95, 0.93}}, {"black", {0.25, 0.26, 0.28}}, {"gray", {0.6, 0.6, 0.6}},
      {"grey", {0.6, 0.6, 0.6}},     {"silver", {0.75, 0.75, 0.75}}, {"red", {0.7, 0.35, 0.3}},
      {"brown", {0.55, 0.42, 0.33}}, {"beige", {0.85, 0.8, 0.68}},   {"yellow", {0.9, 0.85, 0.55}},
      {"blue", {0.45, 0.55, 0.7}},   {"green", {0.5, 0.65, 0.5}},    {"tan", {0.82, 0.71, 0.55}},
      {"maroon", {0.5, 0.25, 0.25}}, {"orange", {0.85, 0.6, 0.4}},   {"cream", {0.95, 0.92, 0.8}},
  };
  auto it = named.find(s);
  if (it != named.end()) {
    return it->second;
  }
  ok = false;
  return {};
}

glm::u8vec4 toColor(const glm::dvec3& c, uint8_t alpha) {
  const glm::dvec3 v = glm::clamp(c, 0.0, 1.0) * 255.0;
  return glm::u8vec4(uint8_t(v.x), uint8_t(v.y), uint8_t(v.z), alpha);
}

uint32_t mixBits(uint32_t x) { // "lowbias32" integer hash
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// Procedural façade of one building part. `packed` goes to COLOR_0.a and is
// decoded by the shader (Scene.hlsl, ShadeBuilding): bits 0-1 type, bits 2-4
// window width variant, bits 5-7 glass tint variant. Bay width and floor height
// are baked into the façade coordinates (bays, floors) instead.
enum FacadeType : uint8_t { kPunched = 0, kCurtainWall = 1, kIndustrial = 2, kStorefront = 3 };
struct FacadeStyle {
  uint8_t packed = 0;
  double bayWidth = 3.0;    // meters per window bay
  double floorHeight = 3.2; // meters per floor
  glm::dvec3 wall{0.5};     // linear albedo of the wall material
};

glm::dvec3 srgbToLinear(int r, int g, int b) {
  auto f = [](int c) {
    const double v = c / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  };
  return glm::dvec3(f(r), f(g), f(b));
}

// Wall materials (sRGB) with weights per façade type: NYC is mostly brick and
// brownstone in the boroughs, stone and glass in the towers.
struct Material {
  int r, g, b;
  double weight[4]; // kPunched, kCurtainWall, kIndustrial, kStorefront
};
constexpr Material kMaterials[] = {
    {150, 78, 60, {0.30, 0.00, 0.35, 0.25}},   // red brick
    {112, 72, 58, {0.18, 0.00, 0.10, 0.12}},   // brown brick / brownstone
    {178, 124, 88, {0.12, 0.00, 0.10, 0.10}},  // orange-tan brick
    {200, 176, 138, {0.12, 0.00, 0.10, 0.15}}, // buff brick
    {206, 196, 172, {0.10, 0.00, 0.00, 0.20}}, // limestone
    {168, 166, 158, {0.06, 0.00, 0.35, 0.13}}, // concrete
    {222, 216, 204, {0.12, 0.00, 0.00, 0.05}}, // painted / stucco / siding
    {84, 96, 108, {0.00, 0.55, 0.00, 0.00}},   // blue-grey glass spandrels
    {62, 66, 70, {0.00, 0.30, 0.00, 0.00}},    // dark metal panels
    {150, 140, 125, {0.00, 0.15, 0.00, 0.00}}, // stone panels
};
// Towers with punched windows: stone and concrete rather than brick.
constexpr double kTowerPunchedWeights[std::size(kMaterials)] = {0.05, 0.0, 0.05, 0.25, 0.35, 0.30, 0.0, 0.0, 0.0, 0.0};

// OpenMapTiles has no building type, so the style is guessed from height and
// footprint area (towers: mostly curtain walls; large low footprints:
// industrial; mid-rise: masonry with or without shop fronts).
FacadeStyle chooseFacadeStyle(double height, double footprintArea, uint32_t seed) {
  auto rnd = [seed](uint32_t k) { return double(mixBits(seed + k * 0x9e3779b9u) & 0xffffu) / 65535.0; };
  FacadeType type = kPunched;
  FacadeStyle s;
  if (height >= 60.0) {
    type = rnd(0) < 0.65 ? kCurtainWall : kPunched;
    s.floorHeight = 3.8 + 0.4 * rnd(1);
    s.bayWidth = type == kCurtainWall ? 1.5 + 0.5 * rnd(2) : 2.6 + 0.8 * rnd(2);
  } else if (height <= 18.0 && footprintArea >= 2500.0) {
    type = kIndustrial;
    s.floorHeight = 4.5 + 1.5 * rnd(1);
    s.bayWidth = 5.0 + 2.0 * rnd(2);
  } else if (height > 10.0) {
    const double r = rnd(0);
    type = r < 0.45 ? kStorefront : (r < 0.52 ? kCurtainWall : kPunched);
    s.floorHeight = 3.1 + 0.4 * rnd(1);
    s.bayWidth = (type == kCurtainWall ? 1.6 : 2.8) + 0.9 * rnd(2);
  } else {
    s.floorHeight = 2.9 + 0.4 * rnd(1);
    s.bayWidth = 2.6 + 1.0 * rnd(2);
  }
  s.packed = uint8_t(type | (uint8_t(rnd(3) * 7.999) << 2) | (uint8_t(rnd(4) * 7.999) << 5));

  const bool towerPunched = type == kPunched && height >= 60.0;
  double pick = rnd(5);
  for (size_t m = 0; m < std::size(kMaterials); ++m) {
    pick -= towerPunched ? kTowerPunchedWeights[m] : kMaterials[m].weight[type];
    if (pick <= 0.0 || m + 1 == std::size(kMaterials)) {
      s.wall = srgbToLinear(kMaterials[m].r, kMaterials[m].g, kMaterials[m].b);
      break;
    }
  }
  return s;
}

// Façade u coordinate (in window bays) of a closed ring's vertices, in emission
// order starting at `start` (a corner). The ring is split into runs at real
// corners (turn > 30 degrees); each run gets a whole number of bays, so corners
// fall on bay boundaries and never cut a window, while runs of short segments
// (curved façades) share their bays. Runs shorter than 0.6 bays stay blank
// (constant u). Returns ring.size() + 1 values: the last closes the ring at a
// duplicate of the start vertex.
std::vector<double> facadeU(const Ring2& ring, double metersPerUnit, double bayWidth, size_t& start) {
  const size_t n = ring.size();
  const double cosCorner = std::cos(30.0 * kPi / 180.0);
  std::vector<double> len(n);
  std::vector<bool> corner(n, false);
  for (size_t i = 0; i < n; ++i) {
    const Vec2& prev = ring[(i + n - 1) % n];
    const Vec2& cur = ring[i];
    const Vec2& next = ring[(i + 1) % n];
    const double ax = cur[0] - prev[0], ay = cur[1] - prev[1];
    const double bx = next[0] - cur[0], by = next[1] - cur[1];
    const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
    len[i] = lb * metersPerUnit;
    corner[i] = la > 0.0 && lb > 0.0 && (ax * bx + ay * by) / (la * lb) < cosCorner;
  }
  start = 0;
  for (size_t i = 0; i < n; ++i) {
    if (corner[i]) {
      start = i;
      break;
    }
  }
  std::vector<double> u(n + 1, 0.0);
  double bays = 0.0;
  for (size_t e = 0; e < n;) {
    size_t e2 = e + 1;
    while (e2 < n && !corner[(start + e2) % n]) {
      ++e2;
    }
    double runLength = 0.0;
    for (size_t j = e; j < e2; ++j) {
      runLength += len[(start + j) % n];
    }
    const double runBays =
        runLength < 0.6 * bayWidth ? 0.0 : double(std::max<long>(1, std::lround(runLength / bayWidth)));
    double along = 0.0;
    for (size_t j = e; j < e2; ++j) {
      u[j] = bays + (runLength > 0.0 ? along / runLength * runBays : 0.0);
      along += len[(start + j) % n];
    }
    bays += runBays;
    e = e2;
  }
  u[n] = bays;
  return u;
}

struct BuildingMesh {
  std::vector<glm::vec3> positions;
  std::vector<glm::u8vec4> colors;
  std::vector<glm::vec2> facade; // x: window bays along the wall, y: floors above ground
  std::vector<uint32_t> indices;
  double minH = std::numeric_limits<double>::max();
  double maxH = -std::numeric_limits<double>::max();
  size_t buildings = 0;
};

void extrudeBuildings(
    const Mvt::Layer& layer,
    const QuadtreeTileID& id,
    const HeightGrid& ground,
    const glm::dvec3& center,
    BuildingMesh& mesh) {
  const Ellipsoid& ellipsoid = GeoRef::ellipsoid();
  const double n = double(1u << id.level);
  const double ext = double(layer.extent);
  GroundSampler sampler{ground};
  // Web Mercator is conformal: tile units scale uniformly to meters (at the
  // tile's centre latitude; < 0.1 % variation across a z14 tile).
  const double centerLat = tileYToLat(id.y + 0.5, n) * kPi / 180.0;
  const double metersPerUnit = 40075016.686 * std::cos(centerLat) / n / ext;

  for (const Mvt::Feature& feature : layer.features) {
    if (feature.type != 3) {
      continue;
    }
    const Mvt::Value* hide = layer.attribute(feature, "hide_3d");
    if (hide && hide->kind == Mvt::Value::Kind::Bool && hide->boolean) {
      continue;
    }
    const double height = std::clamp(layer.number(feature, "render_height", 6.0), 1.0, 600.0);
    const double minHeight = std::clamp(layer.number(feature, "render_min_height", 0.0), 0.0, height - 0.5);

    std::optional<glm::dvec3> osmColour;
    if (const Mvt::Value* colour = layer.attribute(feature, "colour");
        colour && colour->kind == Mvt::Value::Kind::String) {
      bool ok = false;
      const glm::dvec3 c = parseColour(colour->string, ok);
      if (ok) {
        osmColour = c;
      }
    }

    std::vector<Mvt::Polygon> polygons = Mvt::decodePolygons(feature);
    for (size_t pi = 0; pi < polygons.size(); ++pi) {
      const Mvt::Polygon& poly = polygons[pi];
      std::vector<Ring2> rings;
      auto addRing = [&](const Mvt::Ring& r) {
        Ring2 ring;
        ring.reserve(r.size());
        for (const Mvt::Point& p : r) {
          ring.push_back({double(p.x), double(p.y)});
        }
        Ring2 clipped = clipRing(ring, ext);
        // Drop repeated points (clipping creates them at corners): zero-length
        // walls would break the façade corner detection.
        Ring2 unique;
        unique.reserve(clipped.size());
        for (const Vec2& p : clipped) {
          if (unique.empty() || p != unique.back()) {
            unique.push_back(p);
          }
        }
        while (unique.size() > 1 && unique.front() == unique.back()) {
          unique.pop_back();
        }
        clipped = unique.size() >= 3 ? std::move(unique) : Ring2{};
        if (!clipped.empty() || rings.empty()) {
          rings.push_back(std::move(clipped));
        }
      };
      addRing(poly.outer);
      if (rings.empty() || rings[0].empty()) {
        continue;
      }
      for (const Mvt::Ring& hole : poly.holes) {
        addRing(hole);
      }

      // Per-building seed for style and colour variation. Not the feature id:
      // OpenFreeMap merges equal-height buildings (e.g. a row of houses) into
      // one multipolygon feature. The first vertex of the unclipped part in
      // global tile units identifies the building itself.
      const Mvt::Point& first = poly.outer.front();
      const uint32_t gx = uint32_t(int64_t(id.x) * layer.extent + first.x);
      const uint32_t gy = uint32_t(int64_t(id.y) * layer.extent + first.y);
      const uint32_t hash = mixBits(gx * 73856093u ^ gy * 19349663u);

      double area2 = 0.0; // shoelace, outer ring
      for (size_t k = 0; k < rings[0].size(); ++k) {
        const Vec2& a = rings[0][k];
        const Vec2& b = rings[0][(k + 1) % rings[0].size()];
        area2 += a[0] * b[1] - b[0] * a[1];
      }
      const double footprintArea = 0.5 * std::abs(area2) * metersPerUnit * metersPerUnit;
      const FacadeStyle style = chooseFacadeStyle(height, footprintArea, hash);

      // Linear-space albedo: the façade material, pulled towards the OSM colour
      // where mapped (OSM colours tend to be garish), with a little jitter so
      // neighbours are distinguishable.
      glm::dvec3 baseColor = style.wall;
      if (osmColour) {
        baseColor = glm::mix(baseColor, *osmColour * 0.7, 0.45);
      }
      const double jitter = ((hash % 1000) / 1000.0 - 0.5) * 0.12;
      const glm::dvec3 wallColor = glm::clamp(baseColor * (1.0 + jitter), 0.0, 1.0);
      const glm::u8vec4 color = toColor(wallColor, style.packed);

      // Geographic coordinates of all ring vertices, ground height under footprint.
      std::vector<std::vector<Cartographic>> geo(rings.size());
      double groundMin = std::numeric_limits<double>::max();
      for (size_t ri = 0; ri < rings.size(); ++ri) {
        for (const Vec2& p : rings[ri]) {
          const double lon = tileXToLon(id.x + p[0] / ext, n);
          const double lat = tileYToLat(id.y + p[1] / ext, n);
          geo[ri].push_back(Cartographic::fromDegrees(lon, lat, 0.0));
          if (ri == 0) {
            groundMin = std::min(groundMin, sampler.sample(lon, lat));
          }
        }
      }
      const double base = minHeight > 0.0 ? groundMin + minHeight : groundMin - 1.5;
      const double top = groundMin + height;

      auto ecef = [&](const Cartographic& c, double hNavd88) {
        const double lonDeg = CesiumUtility::Math::radiansToDegrees(c.longitude);
        const double latDeg = CesiumUtility::Math::radiansToDegrees(c.latitude);
        const double hEll = hNavd88 + GeoRef::geoidHeight(lonDeg, latDeg);
        mesh.minH = std::min(mesh.minH, hEll);
        mesh.maxH = std::max(mesh.maxH, hEll);
        return ellipsoid.cartographicToCartesian(Cartographic(c.longitude, c.latitude, hEll));
      };

      // One bottom and one top vertex per footprint vertex, shared by walls,
      // roof and underside (plus one duplicated pair per ring where the façade
      // u coordinate wraps). No normals are stored: the renderer derives flat
      // facet normals from screen-space derivatives (and treats upward-facing
      // facets as roofs), which keeps building tiles ~2.5x smaller.
      const float vBase = float((base - groundMin) / style.floorHeight);
      const float vTop = float(height / style.floorHeight);
      std::vector<uint32_t> bottomIndex, topIndex; // per flattened ring vertex (earcut order)
      for (size_t ri = 0; ri < rings.size(); ++ri) {
        if (rings[ri].empty()) {
          continue;
        }
        const auto& ring = geo[ri];
        const size_t count = ring.size();
        size_t start = 0;
        const std::vector<double> u = facadeU(rings[ri], metersPerUnit, style.bayWidth, start);
        const size_t ringStart = bottomIndex.size();
        bottomIndex.resize(ringStart + count);
        topIndex.resize(ringStart + count);
        std::vector<uint32_t> bottomE(count + 1), topE(count + 1); // emission order
        for (size_t e = 0; e <= count; ++e) {
          const size_t k = (start + e) % count;
          bottomE[e] = uint32_t(mesh.positions.size());
          mesh.positions.emplace_back(glm::vec3(ecef(ring[k], base) - center));
          mesh.colors.push_back(color);
          mesh.facade.emplace_back(float(u[e]), vBase);
          topE[e] = uint32_t(mesh.positions.size());
          mesh.positions.emplace_back(glm::vec3(ecef(ring[k], top) - center));
          mesh.colors.push_back(color);
          mesh.facade.emplace_back(float(u[e]), vTop);
          if (e < count) {
            bottomIndex[ringStart + k] = bottomE[e];
            topIndex[ringStart + k] = topE[e];
          }
        }
        for (size_t e = 0; e < count; ++e) {
          const uint32_t b0 = bottomE[e], b1 = bottomE[e + 1];
          const uint32_t t0 = topE[e], t1 = topE[e + 1];
          mesh.indices.insert(mesh.indices.end(), {b0, b1, t1, b0, t1, t0});
        }
      }

      // Roof (and underside for elevated parts) via earcut.
      std::vector<Ring2> earcutInput;
      for (const Ring2& r : rings) {
        if (!r.empty()) {
          earcutInput.push_back(r);
        }
      }
      std::vector<uint32_t> tri = mapbox::earcut<uint32_t>(earcutInput);
      for (size_t t = 0; t + 2 < tri.size(); t += 3) {
        mesh.indices.insert(mesh.indices.end(), {topIndex[tri[t]], topIndex[tri[t + 1]], topIndex[tri[t + 2]]});
        if (minHeight > 0.0) {
          mesh.indices.insert(mesh.indices.end(),
                              {bottomIndex[tri[t]], bottomIndex[tri[t + 2]], bottomIndex[tri[t + 1]]});
        }
      }
      ++mesh.buildings;
    }
  }
}

} // namespace

BuildingsLoader::BuildingsLoader(std::shared_ptr<ElevationService> pElevation, BuildingsOptions options)
    : _pElevation(std::move(pElevation)), _options(std::move(options)) {}

GlobeRectangle BuildingsLoader::tileRectangle(const QuadtreeTileID& id) {
  const double n = double(1u << id.level);
  return GlobeRectangle::fromDegrees(tileXToLon(id.x, n), tileYToLat(id.y + 1.0, n), tileXToLon(id.x + 1.0, n),
                                     tileYToLat(id.y, n));
}

double BuildingsLoader::geometricError(uint32_t zoom) const {
  if (int(zoom) >= _options.contentZoom) {
    return 1.0;
  }
  return _options.baseGeometricError * double(1u << (_options.contentZoom - 1 - int(zoom)));
}

void BuildingsLoader::addTile(std::vector<Tile>& out, const QuadtreeTileID& id) {
  const GlobeRectangle rect = tileRectangle(id);
  using CesiumUtility::Math;
  if (Math::radiansToDegrees(rect.getEast()) < _options.west ||
      Math::radiansToDegrees(rect.getWest()) > _options.east ||
      Math::radiansToDegrees(rect.getNorth()) < _options.south ||
      Math::radiansToDegrees(rect.getSouth()) > _options.north) {
    return;
  }
  // Note: tiles are always created with *unknown* content. A Tile constructed
  // with TileEmptyContent and a loadable ID takes a content reference and is
  // registered for unloading while it still lives in this temporary vector;
  // moving it later leaves dangling pointers in Cesium's unload queue. Levels
  // without buildings resolve to TileEmptyContent in loadTileContent instead
  // (no request is made).
  Tile& tile = out.emplace_back(this, TileID(id));
  tile.setRefine(TileRefine::Replace);
  tile.setTransform(
      glm::translate(glm::dmat4(1.0), GeoRef::ellipsoid().cartographicToCartesian(rect.computeCenter())));
  tile.setBoundingVolume(BoundingRegion(rect, -100.0, 700.0, GeoRef::ellipsoid()));
  tile.setGeometricError(geometricError(id.level));
}

std::unique_ptr<Tileset> BuildingsLoader::createTileset(
    const TilesetExternals& externals,
    const TilesetOptions& options,
    std::shared_ptr<ElevationService> pElevation,
    const BuildingsOptions& buildingsOptions) {
  auto pLoader = std::make_unique<BuildingsLoader>(std::move(pElevation), buildingsOptions);
  auto pRoot = std::make_unique<Tile>(pLoader.get(), TileID(), TileEmptyContent{});
  pRoot->setRefine(TileRefine::Replace);
  pRoot->setUnconditionallyRefine();
  pRoot->setBoundingVolume(BoundingRegion(
      GlobeRectangle::fromDegrees(buildingsOptions.west, buildingsOptions.south, buildingsOptions.east,
                                  buildingsOptions.north),
      -100.0, 700.0, GeoRef::ellipsoid()));

  const uint32_t z = uint32_t(buildingsOptions.rootZoom);
  const uint32_t n = 1u << z;
  const uint32_t x0 = lonToTileX(buildingsOptions.west, n), x1 = lonToTileX(buildingsOptions.east, n);
  const uint32_t y0 = latToTileY(buildingsOptions.north, n), y1 = latToTileY(buildingsOptions.south, n);
  std::vector<Tile> children;
  children.reserve(size_t(x1 - x0 + 1) * size_t(y1 - y0 + 1));
  for (uint32_t y = y0; y <= y1; ++y) {
    for (uint32_t x = x0; x <= x1; ++x) {
      pLoader->addTile(children, QuadtreeTileID(z, x, y));
    }
  }
  LOG_I("Buildings tileset: {} root tiles at z{}, content at z{}", children.size(), z, buildingsOptions.contentZoom);
  pRoot->createChildTiles(std::move(children));
  return std::make_unique<Tileset>(externals, std::move(pLoader), std::move(pRoot), options);
}

TileChildrenResult BuildingsLoader::createTileChildren(const Tile& tile, const Ellipsoid& /*ellipsoid*/) {
  const QuadtreeTileID* pId = std::get_if<QuadtreeTileID>(&tile.getTileID());
  if (!pId || int(pId->level) >= _options.contentZoom) {
    return {{}, TileLoadResultState::Failed};
  }
  std::vector<Tile> children;
  children.reserve(4);
  const uint32_t level = pId->level + 1;
  for (uint32_t dy = 0; dy < 2; ++dy) {
    for (uint32_t dx = 0; dx < 2; ++dx) {
      addTile(children, QuadtreeTileID(level, pId->x * 2 + dx, pId->y * 2 + dy));
    }
  }
  return {std::move(children), TileLoadResultState::Success};
}

Future<TileLoadResult> BuildingsLoader::loadTileContent(const TileLoadInput& input) {
  const QuadtreeTileID* pId = std::get_if<QuadtreeTileID>(&input.tile.getTileID());
  if (!pId || int(pId->level) < _options.contentZoom) {
    TileLoadResult empty;
    empty.contentKind = TileEmptyContent{};
    empty.state = TileLoadResultState::Success;
    empty.pAssetAccessor = input.pAssetAccessor;
    empty.ellipsoid = GeoRef::ellipsoid();
    return input.asyncSystem.createResolvedFuture(std::move(empty));
  }
  const QuadtreeTileID id = *pId;
  const GlobeRectangle rect = tileRectangle(id);

  std::string url = _options.tileUrlTemplate;
  auto replace = [&url](const std::string& key, uint32_t value) {
    size_t pos = url.find(key);
    if (pos != std::string::npos) {
      url.replace(pos, key.size(), std::to_string(value));
    }
  };
  replace("{z}", id.level);
  replace("{x}", id.x);
  replace("{y}", id.y);

  ElevationRequest ground;
  ground.west = CesiumUtility::Math::radiansToDegrees(rect.getWest());
  ground.east = CesiumUtility::Math::radiansToDegrees(rect.getEast());
  ground.south = CesiumUtility::Math::radiansToDegrees(rect.getSouth());
  ground.north = CesiumUtility::Math::radiansToDegrees(rect.getNorth());
  ground.n = 129;
  ground.level = 17;
  ground.needBathymetry = false;

  const glm::dvec3 center(input.tile.getTransform()[3]);
  std::shared_ptr<IAssetAccessor> pAccessor = input.pAssetAccessor;
  const AsyncSystem& asyncSystem = input.asyncSystem;

  return asyncSystem
      .all(pAccessor->get(asyncSystem, url, input.requestHeaders), _pElevation->fetch(asyncSystem, ground))
      .thenInWorkerThread([id, rect, center, pAccessor, url](
                              std::tuple<std::shared_ptr<IAssetRequest>, ElevationResult>&& results) {
        auto& [pRequest, groundResult] = results;
        const IAssetResponse* pResponse = pRequest ? pRequest->response() : nullptr;
        if (!pResponse || pResponse->statusCode() == 429 || pResponse->statusCode() >= 500 ||
            groundResult.status == ElevationResult::Status::RetryLater) {
          SPDLOG_LOGGER_WARN(Log::data(), "Buildings tile z{} {}/{}: will retry", id.level, id.x, id.y);
          return TileLoadResult::createRetryLaterResult(pAccessor, pRequest);
        }

        TileLoadResult result;
        result.pAssetAccessor = pAccessor;
        result.pCompletedRequest = pRequest;
        result.ellipsoid = GeoRef::ellipsoid();
        result.glTFUpAxis = CesiumGeometry::Axis::Z;
        result.state = TileLoadResultState::Success;
        result.contentKind = TileEmptyContent{};

        if (pResponse->statusCode() == 204 || pResponse->statusCode() == 404 || pResponse->data().empty()) {
          return result;
        }
        if (pResponse->statusCode() != 200) {
          SPDLOG_LOGGER_ERROR(Log::data(), "Buildings tile {} -> HTTP {}", url, pResponse->statusCode());
          return TileLoadResult::createFailedResult(pAccessor, pRequest);
        }

        std::vector<Mvt::Layer> layers;
        std::string error;
        if (!Mvt::decode(pResponse->data(), layers, error)) {
          SPDLOG_LOGGER_ERROR(Log::data(), "Buildings tile {}: cannot decode vector tile: {}", url, error);
          return TileLoadResult::createFailedResult(pAccessor, pRequest);
        }
        const HeightGrid flatGround = ElevationService::makeFlatGrid(
            ElevationRequest{CesiumUtility::Math::radiansToDegrees(rect.getWest()),
                             CesiumUtility::Math::radiansToDegrees(rect.getSouth()),
                             CesiumUtility::Math::radiansToDegrees(rect.getEast()),
                             CesiumUtility::Math::radiansToDegrees(rect.getNorth()), 3, 17, false},
            2.0f);
        const HeightGrid& groundGrid =
            groundResult.status == ElevationResult::Status::Ok ? groundResult.grid : flatGround;

        BuildingMesh mesh;
        for (const Mvt::Layer& layer : layers) {
          if (layer.name == "building") {
            extrudeBuildings(layer, id, groundGrid, center, mesh);
          }
        }
        if (mesh.indices.empty()) {
          return result;
        }

        GltfMeshBuilder builder;
        builder.setPositions(mesh.positions);
        builder.setColors(mesh.colors);
        builder.addVec2Attribute("_FACADE", mesh.facade);
        builder.setIndices(mesh.indices);
        result.contentKind = builder.build();
        result.updatedBoundingVolume = BoundingRegion(rect, mesh.minH, mesh.maxH, GeoRef::ellipsoid());
        SPDLOG_LOGGER_DEBUG(Log::data(), "Buildings tile z{} {}/{}: {} buildings, {} vertices", id.level, id.x,
                            id.y, mesh.buildings, mesh.positions.size());
        return result;
      });
}
