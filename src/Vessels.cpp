#include "pch.h"
#include "Vessels.h"

#include "GeoRef.h"
#include "Log.h"
#include "Renderer.h"

namespace {

struct MeshBuilder {
  std::vector<GpuVertex> vertices;
  std::vector<uint32_t> indices;

  static uint32_t rgba(float r, float g, float b) {
    return uint32_t(r * 255) | (uint32_t(g * 255) << 8) | (uint32_t(b * 255) << 16) | 0xff000000u;
  }

  void quad(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& d, uint32_t color) {
    const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
    const uint32_t base = uint32_t(vertices.size());
    for (const glm::vec3* p : {&a, &b, &c, &d}) {
      GpuVertex v{};
      v.position = *p;
      v.normalWater = packNormalWater(n, 0.0f);
      v.color = color;
      vertices.push_back(v);
    }
    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }

  // Prism from a convex outline (CCW from above) between z0 and z1, with caps.
  void prism(const std::vector<glm::vec2>& outline, float z0, float z1, uint32_t side, uint32_t top, bool bottom) {
    const size_t n = outline.size();
    for (size_t i = 0; i < n; ++i) {
      const glm::vec2 a = outline[i], b = outline[(i + 1) % n];
      quad(glm::vec3(a, z0), glm::vec3(b, z0), glm::vec3(b, z1), glm::vec3(a, z1), side);
    }
    auto cap = [&](float z, uint32_t color, bool flip) {
      const uint32_t base = uint32_t(vertices.size());
      for (const glm::vec2& p : outline) {
        GpuVertex v{};
        v.position = glm::vec3(p, z);
        v.normalWater = packNormalWater(glm::vec3(0, 0, flip ? -1.0f : 1.0f), 0.0f);
        v.color = color;
        vertices.push_back(v);
      }
      for (uint32_t i = 1; i + 1 < n; ++i) {
        indices.insert(indices.end(), {base, base + i, base + i + 1});
      }
    };
    cap(z1, top, false);
    if (bottom) {
      cap(z0, side, true);
    }
  }

  void box(float x0, float x1, float y0, float y1, float z0, float z1, uint32_t color) {
    prism({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, z0, z1, color, color, false);
  }
};

void buildHull(MeshBuilder& m, const VesselSpec& s) {
  const float L = float(s.length), B = float(s.beam), D = float(s.draft), F = float(s.freeboard);
  const float hb = B * 0.5f, bowStart = L * 0.5f - std::min(B * 1.2f, L * 0.25f);
  const std::vector<glm::vec2> outline = {
      {-hb * 0.9f, -L * 0.5f}, {hb * 0.9f, -L * 0.5f}, {hb, -L * 0.42f}, {hb, bowStart},
      {hb * 0.45f, L * 0.45f}, {0.0f, L * 0.5f},       {-hb * 0.45f, L * 0.45f}, {-hb, bowStart},
      {-hb, -L * 0.42f},
  };
  const uint32_t below = MeshBuilder::rgba(0.45f, 0.12f, 0.10f); // antifouling red
  uint32_t hull = MeshBuilder::rgba(0.12f, 0.14f, 0.18f);
  uint32_t deck = MeshBuilder::rgba(0.35f, 0.36f, 0.35f);
  if (s.kind == VesselSpec::Kind::Ferry) {
    hull = MeshBuilder::rgba(0.85f, 0.45f, 0.12f); // Staten Island ferry orange
    deck = MeshBuilder::rgba(0.75f, 0.74f, 0.70f);
  } else if (s.kind == VesselSpec::Kind::Tug) {
    hull = MeshBuilder::rgba(0.10f, 0.10f, 0.10f);
    deck = MeshBuilder::rgba(0.55f, 0.15f, 0.12f);
  }
  m.prism(outline, -D, 0.0f, below, below, true);
  m.prism(outline, 0.0f, F, hull, deck, false);

  const uint32_t white = MeshBuilder::rgba(0.92f, 0.92f, 0.90f);
  switch (s.kind) {
  case VesselSpec::Kind::Container: {
    // Accommodation block aft, container stacks forward.
    m.box(-hb * 0.8f, hb * 0.8f, -L * 0.46f, -L * 0.38f, F, F + 22.0f, white);
    m.box(-hb * 0.15f, hb * 0.15f, -L * 0.44f, -L * 0.41f, F + 22.0f, F + 30.0f, MeshBuilder::rgba(0.2f, 0.2f, 0.2f));
    const uint32_t colors[] = {MeshBuilder::rgba(0.70f, 0.20f, 0.15f), MeshBuilder::rgba(0.15f, 0.35f, 0.60f),
                               MeshBuilder::rgba(0.20f, 0.50f, 0.30f), MeshBuilder::rgba(0.80f, 0.65f, 0.20f),
                               MeshBuilder::rgba(0.55f, 0.55f, 0.58f)};
    int bay = 0;
    for (float y = -L * 0.36f; y < bowStart - 6.0f; y += 13.5f, ++bay) {
      for (int row = 0; row < 5; ++row) {
        const float x0 = -hb * 0.85f + row * (hb * 1.7f / 5.0f);
        const float height = 2.6f * float(3 + (bay * 7 + row * 3) % 4);
        m.box(x0 + 0.2f, x0 + hb * 1.7f / 5.0f - 0.2f, y, y + 12.2f, F, F + height, colors[(bay + row * 2) % 5]);
      }
    }
    break;
  }
  case VesselSpec::Kind::Ferry:
    m.box(-hb * 0.85f, hb * 0.85f, -L * 0.40f, L * 0.36f, F, F + 4.0f, hull);
    m.box(-hb * 0.75f, hb * 0.75f, -L * 0.35f, L * 0.30f, F + 4.0f, F + 7.5f, white);
    m.box(-hb * 0.30f, hb * 0.30f, -L * 0.08f, L * 0.08f, F + 7.5f, F + 10.5f, white);
    break;
  case VesselSpec::Kind::Tug:
    m.box(-hb * 0.6f, hb * 0.6f, -L * 0.10f, L * 0.20f, F, F + 3.5f, white);
    m.box(-hb * 0.45f, hb * 0.45f, -L * 0.02f, L * 0.18f, F + 3.5f, F + 6.0f, white);
    m.box(-0.6f, 0.6f, -L * 0.12f, -L * 0.04f, F + 3.5f, F + 9.0f, MeshBuilder::rgba(0.1f, 0.1f, 0.1f));
    break;
  }
}

} // namespace

std::vector<VesselSpec> Vessels::defaultFleet() {
  // Positions in Upper New York Bay near the point of interest, chosen in water
  // deep enough for each draft (NOAA BAG survey depths 4-16 m).
  std::vector<VesselSpec> fleet;
  fleet.push_back({"Container ship (anchorage)", -74.0455, 40.6690, 15.0, 300.0, 45.0, 12.0, 10.0,
                   VesselSpec::Kind::Container});
  fleet.push_back({"Ferry", -74.0270, 40.6800, 30.0, 95.0, 21.0, 4.0, 5.0, VesselSpec::Kind::Ferry});
  fleet.push_back({"Tug", -74.0190, 40.6605, 300.0, 30.0, 10.0, 4.0, 3.0, VesselSpec::Kind::Tug});
  return fleet;
}

bool Vessels::init(ID3D11Device* device, const std::vector<VesselSpec>& specs) {
  _specs = specs;
  _instances.clear();
  for (const VesselSpec& s : specs) {
    MeshBuilder m;
    buildHull(m, s);

    Instance inst;
    GpuPrimitive& p = inst.primitive;
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = UINT(m.vertices.size() * sizeof(GpuVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vdata{m.vertices.data(), 0, 0};
    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = UINT(m.indices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA idata{m.indices.data(), 0, 0};
    if (!HR_OK(device->CreateBuffer(&vbd, &vdata, &p.vertexBuffer)) ||
        !HR_OK(device->CreateBuffer(&ibd, &idata, &p.indexBuffer))) {
      return false;
    }
    p.indexFormat = DXGI_FORMAT_R32_UINT;
    p.indexCount = UINT(m.indices.size());
    p.hasNormals = true;
    p.hasVertexColors = true;
    p.isTerrain = false;

    // Georeference: ENU frame at the vessel position on the sea surface,
    // rotated so +Y (bow) points along the heading.
    const double h = GeoRef::seaSurfaceEllipsoidHeight(s.lon, s.lat);
    const glm::dvec3 origin = GeoRef::toEcef(s.lon, s.lat, h);
    const glm::dmat4 enu = CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(origin, GeoRef::ellipsoid());
    inst.transform =
        enu * glm::rotate(glm::dmat4(1.0), -CesiumUtility::Math::degreesToRadians(s.heading), glm::dvec3(0, 0, 1));
    LOG_I("Vessel '{}' at lon {:.5f} lat {:.5f}, heading {:.0f} deg, sea surface h_ellipsoid {:.2f} m", s.name, s.lon,
          s.lat, s.heading, h);
    _instances.push_back(std::move(inst));
  }
  return true;
}

void Vessels::draw(Renderer& renderer) const {
  for (const Instance& inst : _instances) {
    renderer.drawPrimitive(inst.primitive, inst.transform);
  }
}
