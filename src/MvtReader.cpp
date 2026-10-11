#include "pch.h"
#include "MvtReader.h"

namespace Mvt {
namespace {

class Reader {
public:
  Reader(const uint8_t* p, size_t n) : _p(p), _end(p + n) {}
  bool atEnd() const { return _p >= _end; }
  bool ok() const { return _ok; }

  uint64_t varint() {
    uint64_t result = 0;
    int shift = 0;
    while (_p < _end && shift < 64) {
      const uint8_t b = *_p++;
      result |= uint64_t(b & 0x7f) << shift;
      if ((b & 0x80) == 0) {
        return result;
      }
      shift += 7;
    }
    _ok = false;
    return 0;
  }

  bool next(uint32_t& field, uint32_t& wireType) {
    if (atEnd() || !_ok) {
      return false;
    }
    const uint64_t key = varint();
    field = uint32_t(key >> 3);
    wireType = uint32_t(key & 7);
    return _ok;
  }

  Reader bytes() {
    const uint64_t len = varint();
    if (!_ok || len > uint64_t(_end - _p)) {
      _ok = false;
      return Reader(_end, 0);
    }
    Reader r(_p, size_t(len));
    _p += len;
    return r;
  }

  std::string string() {
    Reader r = bytes();
    return std::string(reinterpret_cast<const char*>(r._p), size_t(r._end - r._p));
  }

  uint32_t fixed32() {
    if (_end - _p < 4) {
      _ok = false;
      return 0;
    }
    uint32_t v;
    std::memcpy(&v, _p, 4);
    _p += 4;
    return v;
  }

  uint64_t fixed64() {
    if (_end - _p < 8) {
      _ok = false;
      return 0;
    }
    uint64_t v;
    std::memcpy(&v, _p, 8);
    _p += 8;
    return v;
  }

  void skip(uint32_t wireType) {
    switch (wireType) {
    case 0: varint(); break;
    case 1: fixed64(); break;
    case 2: bytes(); break;
    case 5: fixed32(); break;
    default: _ok = false; break;
    }
  }

  std::vector<uint32_t> packedUint32() {
    std::vector<uint32_t> out;
    Reader r = bytes();
    while (!r.atEnd() && r.ok()) {
      out.push_back(uint32_t(r.varint()));
    }
    _ok = _ok && r.ok();
    return out;
  }

private:
  const uint8_t* _p;
  const uint8_t* _end;
  bool _ok = true;
};

Value decodeValue(Reader r) {
  Value v;
  uint32_t field, wire;
  while (r.next(field, wire)) {
    switch (field) {
    case 1: v.kind = Value::Kind::String; v.string = r.string(); break;
    case 2: {
      uint32_t bits = r.fixed32();
      float f;
      std::memcpy(&f, &bits, 4);
      v.kind = Value::Kind::Number;
      v.number = f;
      break;
    }
    case 3: {
      uint64_t bits = r.fixed64();
      double d;
      std::memcpy(&d, &bits, 8);
      v.kind = Value::Kind::Number;
      v.number = d;
      break;
    }
    case 4: v.kind = Value::Kind::Number; v.number = double(int64_t(r.varint())); break;
    case 5: v.kind = Value::Kind::Number; v.number = double(r.varint()); break;
    case 6: {
      uint64_t z = r.varint();
      v.kind = Value::Kind::Number;
      v.number = double(int64_t((z >> 1) ^ (~(z & 1) + 1)));
      break;
    }
    case 7: v.kind = Value::Kind::Bool; v.boolean = r.varint() != 0; break;
    default: r.skip(wire); break;
    }
  }
  return v;
}

Feature decodeFeature(Reader r) {
  Feature f;
  uint32_t field, wire;
  while (r.next(field, wire)) {
    switch (field) {
    case 1: f.id = r.varint(); break;
    case 2: f.tags = r.packedUint32(); break;
    case 3: f.type = int(r.varint()); break;
    case 4: f.geometry = r.packedUint32(); break;
    default: r.skip(wire); break;
    }
  }
  return f;
}

int64_t ringArea2(const Ring& ring) {
  int64_t a = 0;
  for (size_t i = 0, n = ring.size(); i < n; ++i) {
    const Point& p = ring[i];
    const Point& q = ring[(i + 1) % n];
    a += int64_t(p.x) * q.y - int64_t(q.x) * p.y;
  }
  return a;
}

} // namespace

const Value* Layer::attribute(const Feature& feature, const std::string& key) const {
  for (size_t i = 0; i + 1 < feature.tags.size(); i += 2) {
    const uint32_t k = feature.tags[i];
    const uint32_t v = feature.tags[i + 1];
    if (k < keys.size() && v < values.size() && keys[k] == key) {
      return &values[v];
    }
  }
  return nullptr;
}

double Layer::number(const Feature& feature, const std::string& key, double fallback) const {
  const Value* v = attribute(feature, key);
  if (!v) {
    return fallback;
  }
  if (v->kind == Value::Kind::Number) {
    return v->number;
  }
  if (v->kind == Value::Kind::String) {
    try {
      return std::stod(v->string);
    } catch (...) {
    }
  }
  return fallback;
}

bool decode(std::span<const std::byte> data, std::vector<Layer>& layers, std::string& error) {
  Reader tile(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  uint32_t field, wire;
  while (tile.next(field, wire)) {
    if (field != 3 || wire != 2) {
      tile.skip(wire);
      continue;
    }
    Reader lr = tile.bytes();
    Layer layer;
    uint32_t lf, lw;
    while (lr.next(lf, lw)) {
      switch (lf) {
      case 1: layer.name = lr.string(); break;
      case 2: layer.features.push_back(decodeFeature(lr.bytes())); break;
      case 3: layer.keys.push_back(lr.string()); break;
      case 4: layer.values.push_back(decodeValue(lr.bytes())); break;
      case 5: layer.extent = uint32_t(lr.varint()); break;
      default: lr.skip(lw); break;
      }
    }
    if (!lr.ok()) {
      error = "malformed layer";
      return false;
    }
    layers.push_back(std::move(layer));
  }
  if (!tile.ok()) {
    error = "malformed tile";
    return false;
  }
  return true;
}

std::vector<Polygon> decodePolygons(const Feature& feature) {
  std::vector<Ring> rings;
  Ring current;
  int32_t x = 0, y = 0;
  const std::vector<uint32_t>& g = feature.geometry;
  size_t i = 0;
  while (i < g.size()) {
    const uint32_t cmd = g[i] & 7;
    const uint32_t count = g[i] >> 3;
    ++i;
    if (cmd == 7) { // ClosePath
      if (current.size() >= 3) {
        rings.push_back(std::move(current));
      }
      current.clear();
      continue;
    }
    for (uint32_t k = 0; k < count; ++k) {
      if (i + 1 >= g.size()) {
        i = g.size();
        break;
      }
      const uint32_t px = g[i], py = g[i + 1];
      i += 2;
      x += int32_t((px >> 1) ^ (~(px & 1) + 1));
      y += int32_t((py >> 1) ^ (~(py & 1) + 1));
      if (cmd == 1) { // MoveTo
        if (current.size() >= 3) {
          rings.push_back(std::move(current));
        }
        current.clear();
      }
      current.push_back({x, y});
    }
  }
  if (current.size() >= 3) {
    rings.push_back(std::move(current));
  }

  std::vector<Polygon> polygons;
  for (Ring& ring : rings) {
    const int64_t area = ringArea2(ring);
    if (area > 0) {
      polygons.emplace_back();
      polygons.back().outer = std::move(ring);
    } else if (area < 0 && !polygons.empty()) {
      polygons.back().holes.push_back(std::move(ring));
    }
  }
  return polygons;
}

} // namespace Mvt
