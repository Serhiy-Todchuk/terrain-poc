#pragma once
#include "RendererResources.h"

#include <d3d11.h>

#include <string>
#include <vector>

class Renderer;

// Test vessels placed by geographic coordinates on the engine-rendered sea
// surface (PoC requirement: "placement of test vessels in georeferenced
// coordinates"). Hulls are simple procedural meshes in a local frame
// (+Y bow, +X starboard, +Z up) positioned with an east-north-up transform.
struct VesselSpec {
  std::string name;
  double lon = 0, lat = 0;   // degrees
  double heading = 0;        // degrees, clockwise from north
  double length = 100, beam = 20, draft = 6, freeboard = 6;
  enum class Kind { Container, Ferry, Tug } kind = Kind::Container;
};

class Vessels {
public:
  bool init(ID3D11Device* device, const std::vector<VesselSpec>& specs);
  void draw(Renderer& renderer) const;
  const std::vector<VesselSpec>& specs() const { return _specs; }

  static std::vector<VesselSpec> defaultFleet();

private:
  struct Instance {
    GpuPrimitive primitive;
    glm::dmat4 transform{1.0};
  };
  std::vector<VesselSpec> _specs;
  std::vector<Instance> _instances;
};
