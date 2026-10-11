#include "pch.h"
#include "GltfBuilder.h"

#include <CesiumGeometry/Axis.h>
#include <CesiumGltf/Accessor.h>
#include <CesiumGltf/BufferView.h>
#include <CesiumGltf/Material.h>
#include <CesiumGltf/MeshPrimitive.h>
#include <CesiumUtility/JsonValue.h>

using namespace CesiumGltf;

namespace {
template <typename T> std::vector<std::byte> toBytes(const std::vector<T>& v) {
  std::vector<std::byte> b(v.size() * sizeof(T));
  if (!v.empty()) {
    std::memcpy(b.data(), v.data(), b.size());
  }
  return b;
}
} // namespace

void GltfMeshBuilder::setPositions(const std::vector<glm::vec3>& positions) {
  Stream s;
  s.attribute = "POSITION";
  s.componentType = Accessor::ComponentType::FLOAT;
  s.type = Accessor::Type::VEC3;
  s.count = static_cast<int64_t>(positions.size());
  s.bytes = toBytes(positions);
  glm::vec3 mn(std::numeric_limits<float>::max()), mx(-std::numeric_limits<float>::max());
  for (const glm::vec3& p : positions) {
    mn = glm::min(mn, p);
    mx = glm::max(mx, p);
  }
  if (!positions.empty()) {
    s.min = {mn.x, mn.y, mn.z};
    s.max = {mx.x, mx.y, mx.z};
  }
  _streams.push_back(std::move(s));
}

void GltfMeshBuilder::setNormals(const std::vector<glm::vec3>& normals) {
  Stream s;
  s.attribute = "NORMAL";
  s.componentType = Accessor::ComponentType::FLOAT;
  s.type = Accessor::Type::VEC3;
  s.count = static_cast<int64_t>(normals.size());
  s.bytes = toBytes(normals);
  _streams.push_back(std::move(s));
}

void GltfMeshBuilder::setColors(const std::vector<glm::u8vec4>& colors) {
  Stream s;
  s.attribute = "COLOR_0";
  s.componentType = Accessor::ComponentType::UNSIGNED_BYTE;
  s.type = Accessor::Type::VEC4;
  s.normalized = true;
  s.count = static_cast<int64_t>(colors.size());
  s.bytes = toBytes(colors);
  _streams.push_back(std::move(s));
}

void GltfMeshBuilder::addScalarAttribute(const std::string& name, const std::vector<float>& values) {
  Stream s;
  s.attribute = name;
  s.componentType = Accessor::ComponentType::FLOAT;
  s.type = Accessor::Type::SCALAR;
  s.count = static_cast<int64_t>(values.size());
  s.bytes = toBytes(values);
  _streams.push_back(std::move(s));
}

void GltfMeshBuilder::addVec2Attribute(const std::string& name, const std::vector<glm::vec2>& values) {
  Stream s;
  s.attribute = name;
  s.componentType = Accessor::ComponentType::FLOAT;
  s.type = Accessor::Type::VEC2;
  s.count = static_cast<int64_t>(values.size());
  s.bytes = toBytes(values);
  _streams.push_back(std::move(s));
}

void GltfMeshBuilder::setIndices(const std::vector<uint16_t>& indices) {
  Stream s;
  s.componentType = Accessor::ComponentType::UNSIGNED_SHORT;
  s.type = Accessor::Type::SCALAR;
  s.count = static_cast<int64_t>(indices.size());
  s.bytes = toBytes(indices);
  _indexStream = static_cast<int>(_streams.size());
  _streams.push_back(std::move(s));
}

void GltfMeshBuilder::setIndices(const std::vector<uint32_t>& indices) {
  Stream s;
  s.componentType = Accessor::ComponentType::UNSIGNED_INT;
  s.type = Accessor::Type::SCALAR;
  s.count = static_cast<int64_t>(indices.size());
  s.bytes = toBytes(indices);
  _indexStream = static_cast<int>(_streams.size());
  _streams.push_back(std::move(s));
}

Model GltfMeshBuilder::build() const {
  Model model;
  model.asset.version = "2.0";
  model.extras["gltfUpAxis"] =
      CesiumUtility::JsonValue(std::underlying_type_t<CesiumGeometry::Axis>(CesiumGeometry::Axis::Z));

  size_t total = 0;
  for (const Stream& s : _streams) {
    total += (s.bytes.size() + 3) & ~size_t(3);
  }
  model.buffers.resize(1);
  std::vector<std::byte>& data = model.buffers[0].cesium.data;
  data.resize(total);
  model.buffers[0].byteLength = static_cast<int64_t>(total);

  model.materials.resize(1);
  model.materials[0].pbrMetallicRoughness.emplace();
  model.materials[0].pbrMetallicRoughness->baseColorFactor = {1.0, 1.0, 1.0, 1.0};
  model.materials[0].pbrMetallicRoughness->metallicFactor = 0.0;
  model.materials[0].pbrMetallicRoughness->roughnessFactor = 1.0;

  model.meshes.resize(1);
  model.meshes[0].primitives.resize(1);
  MeshPrimitive& primitive = model.meshes[0].primitives[0];
  primitive.mode = MeshPrimitive::Mode::TRIANGLES;
  primitive.material = 0;

  size_t offset = 0;
  for (size_t i = 0; i < _streams.size(); ++i) {
    const Stream& s = _streams[i];
    std::memcpy(data.data() + offset, s.bytes.data(), s.bytes.size());

    BufferView& view = model.bufferViews.emplace_back();
    view.buffer = 0;
    view.byteOffset = static_cast<int64_t>(offset);
    view.byteLength = static_cast<int64_t>(s.bytes.size());
    view.target = (static_cast<int>(i) == _indexStream) ? BufferView::Target::ELEMENT_ARRAY_BUFFER
                                                        : BufferView::Target::ARRAY_BUFFER;

    Accessor& accessor = model.accessors.emplace_back();
    accessor.bufferView = static_cast<int32_t>(model.bufferViews.size() - 1);
    accessor.byteOffset = 0;
    accessor.componentType = s.componentType;
    accessor.type = s.type;
    accessor.normalized = s.normalized;
    accessor.count = s.count;
    accessor.min = s.min;
    accessor.max = s.max;

    const int32_t accessorIndex = static_cast<int32_t>(model.accessors.size() - 1);
    if (static_cast<int>(i) == _indexStream) {
      primitive.indices = accessorIndex;
    } else {
      primitive.attributes[s.attribute] = accessorIndex;
    }
    offset += (s.bytes.size() + 3) & ~size_t(3);
  }

  model.nodes.resize(1);
  model.nodes[0].mesh = 0;
  model.scenes.resize(1);
  model.scenes[0].nodes.push_back(0);
  model.scene = 0;
  return model;
}
