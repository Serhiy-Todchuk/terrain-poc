#pragma once
#include <CesiumGltf/Model.h>

#include <glm/gtc/type_precision.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Small helper to assemble a single-primitive glTF model from CPU arrays.
// Used by the procedural terrain and building loaders.
class GltfMeshBuilder {
public:
  void setPositions(const std::vector<glm::vec3>& positions);
  void setNormals(const std::vector<glm::vec3>& normals);
  void setColors(const std::vector<glm::u8vec4>& colors); // COLOR_0, normalized
  void addScalarAttribute(const std::string& name, const std::vector<float>& values);
  void addVec2Attribute(const std::string& name, const std::vector<glm::vec2>& values);
  void setIndices(const std::vector<uint16_t>& indices);
  void setIndices(const std::vector<uint32_t>& indices);

  // Builds a model whose positions are relative to the tile transform and use
  // the Z-up convention (ECEF-aligned axes).
  CesiumGltf::Model build() const;

private:
  struct Stream {
    std::string attribute; // empty for indices
    int32_t componentType = 0;
    std::string type;
    bool normalized = false;
    int64_t count = 0;
    std::vector<std::byte> bytes;
    std::vector<double> min, max;
  };
  std::vector<Stream> _streams;
  int _indexStream = -1;
};
