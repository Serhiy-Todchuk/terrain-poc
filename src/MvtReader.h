#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Minimal Mapbox Vector Tile (v2) decoder: layers, features, attributes and
// polygon geometry. Enough to read OpenMapTiles "building" layers.
namespace Mvt {

struct Value {
  enum class Kind { None, String, Number, Bool } kind = Kind::None;
  std::string string;
  double number = 0.0;
  bool boolean = false;
};

struct Point {
  int32_t x = 0;
  int32_t y = 0;
};
using Ring = std::vector<Point>;

struct Polygon {
  Ring outer;               // positive area in tile coordinates (MVT spec)
  std::vector<Ring> holes;
};

struct Feature {
  uint64_t id = 0;
  int type = 0; // 1 point, 2 linestring, 3 polygon
  std::vector<uint32_t> tags;
  std::vector<uint32_t> geometry;
};

struct Layer {
  std::string name;
  uint32_t extent = 4096;
  std::vector<std::string> keys;
  std::vector<Value> values;
  std::vector<Feature> features;

  // Returns the attribute value for a key, or nullptr.
  const Value* attribute(const Feature& feature, const std::string& key) const;
  double number(const Feature& feature, const std::string& key, double fallback) const;
};

bool decode(std::span<const std::byte> data, std::vector<Layer>& layers, std::string& error);

// Decodes polygon geometry into polygons with holes.
std::vector<Polygon> decodePolygons(const Feature& feature);

} // namespace Mvt
