#include "pch.h"
#include "RendererResources.h"

#include "Log.h"

#include <CesiumGltf/AccessorView.h>
#include <CesiumGltfContent/GltfUtilities.h>
#include <CesiumImage/ImageAsset.h>
#include <CesiumImage/ImageDecoder.h>
#include <CesiumRasterOverlays/RasterOverlayTile.h>

using namespace Cesium3DTilesSelection;
using namespace CesiumGltf;

namespace {

int32_t findAttribute(const MeshPrimitive& primitive, const std::string& name) {
  auto it = primitive.attributes.find(name);
  return it == primitive.attributes.end() ? -1 : it->second;
}

template <typename T> bool readAll(const Model& model, int32_t accessor, std::vector<T>& out) {
  if (accessor < 0) {
    return false;
  }
  AccessorView<T> view(model, accessor);
  if (view.status() != AccessorViewStatus::Valid) {
    return false;
  }
  out.resize(size_t(view.size()));
  for (int64_t i = 0; i < view.size(); ++i) {
    out[size_t(i)] = view[i];
  }
  return true;
}

uint32_t packColor(const glm::vec4& c) {
  const glm::vec4 v = glm::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f;
  return uint32_t(v.x) | (uint32_t(v.y) << 8) | (uint32_t(v.z) << 16) | (uint32_t(v.w) << 24);
}

bool readColors(const Model& model, int32_t accessorIndex, std::vector<uint32_t>& out) {
  const Accessor* pAccessor = Model::getSafe(&model.accessors, accessorIndex);
  if (!pAccessor) {
    return false;
  }
  const bool vec4 = pAccessor->type == Accessor::Type::VEC4;
  switch (pAccessor->componentType) {
  case Accessor::ComponentType::UNSIGNED_BYTE:
    if (vec4) {
      std::vector<glm::u8vec4> v;
      if (!readAll(model, accessorIndex, v)) return false;
      out.resize(v.size());
      for (size_t i = 0; i < v.size(); ++i)
        out[i] = uint32_t(v[i].x) | (uint32_t(v[i].y) << 8) | (uint32_t(v[i].z) << 16) | (uint32_t(v[i].w) << 24);
    } else {
      std::vector<glm::u8vec3> v;
      if (!readAll(model, accessorIndex, v)) return false;
      out.resize(v.size());
      for (size_t i = 0; i < v.size(); ++i)
        out[i] = uint32_t(v[i].x) | (uint32_t(v[i].y) << 8) | (uint32_t(v[i].z) << 16) | 0xff000000u;
    }
    return true;
  case Accessor::ComponentType::UNSIGNED_SHORT:
    if (vec4) {
      std::vector<glm::u16vec4> v;
      if (!readAll(model, accessorIndex, v)) return false;
      out.resize(v.size());
      for (size_t i = 0; i < v.size(); ++i) out[i] = packColor(glm::vec4(v[i]) / 65535.0f);
    } else {
      std::vector<glm::u16vec3> v;
      if (!readAll(model, accessorIndex, v)) return false;
      out.resize(v.size());
      for (size_t i = 0; i < v.size(); ++i) out[i] = packColor(glm::vec4(glm::vec3(v[i]) / 65535.0f, 1.0f));
    }
    return true;
  case Accessor::ComponentType::FLOAT:
    if (vec4) {
      std::vector<glm::vec4> v;
      if (!readAll(model, accessorIndex, v)) return false;
      out.resize(v.size());
      for (size_t i = 0; i < v.size(); ++i) out[i] = packColor(v[i]);
    } else {
      std::vector<glm::vec3> v;
      if (!readAll(model, accessorIndex, v)) return false;
      out.resize(v.size());
      for (size_t i = 0; i < v.size(); ++i) out[i] = packColor(glm::vec4(v[i], 1.0f));
    }
    return true;
  default:
    return false;
  }
}

bool readIndices(const Model& model, int32_t accessorIndex, std::vector<uint32_t>& out) {
  const Accessor* pAccessor = Model::getSafe(&model.accessors, accessorIndex);
  if (!pAccessor) {
    return false;
  }
  switch (pAccessor->componentType) {
  case Accessor::ComponentType::UNSIGNED_BYTE: {
    std::vector<uint8_t> v;
    if (!readAll(model, accessorIndex, v)) return false;
    out.assign(v.begin(), v.end());
    return true;
  }
  case Accessor::ComponentType::UNSIGNED_SHORT: {
    std::vector<uint16_t> v;
    if (!readAll(model, accessorIndex, v)) return false;
    out.assign(v.begin(), v.end());
    return true;
  }
  case Accessor::ComponentType::UNSIGNED_INT:
    return readAll(model, accessorIndex, out);
  default:
    return false;
  }
}

} // namespace

RendererResources::RendererResources(ID3D11Device* pDevice) : _device(pDevice) {}

GpuTile* RendererResources::createTile(Model& model) {
  auto pTile = std::make_unique<GpuTile>();

  glm::dmat4 root(1.0);
  root = CesiumGltfContent::GltfUtilities::applyRtcCenter(model, root);
  root = CesiumGltfContent::GltfUtilities::applyGltfUpAxisTransform(model, root);

  model.forEachPrimitiveInScene(
      -1, [&](const Model& gltf, const Node& /*node*/, const Mesh& /*mesh*/, const MeshPrimitive& primitive,
              const glm::dmat4& transform) {
        if (primitive.mode != MeshPrimitive::Mode::TRIANGLES) {
          SPDLOG_LOGGER_DEBUG(Log::gpu(), "Skipping primitive with unsupported mode {}", primitive.mode);
          return;
        }
        std::vector<glm::vec3> positions;
        if (!readAll(gltf, findAttribute(primitive, "POSITION"), positions) || positions.empty()) {
          SPDLOG_LOGGER_WARN(Log::gpu(), "Primitive without readable float POSITION skipped");
          ++_failures;
          return;
        }
        const size_t count = positions.size();
        std::vector<glm::vec3> normals;
        const bool hasNormals = readAll(gltf, findAttribute(primitive, "NORMAL"), normals) && normals.size() == count;
        // Buildings carry façade coordinates instead of overlay texcoords (the
        // buildings tileset has no raster overlay) in the same vertex slot.
        std::vector<glm::vec2> uv0;
        const bool hasFacade = readAll(gltf, findAttribute(primitive, "_FACADE"), uv0) && uv0.size() == count;
        const bool hasUv0 =
            hasFacade || (readAll(gltf, findAttribute(primitive, "_CESIUMOVERLAY_0"), uv0) && uv0.size() == count);
        std::vector<uint32_t> colors;
        const bool hasColors = readColors(gltf, findAttribute(primitive, "COLOR_0"), colors) && colors.size() == count;
        std::vector<float> heights, water;
        const bool hasHeight = readAll(gltf, findAttribute(primitive, "_HEIGHT"), heights) && heights.size() == count;
        const bool hasWater = readAll(gltf, findAttribute(primitive, "_WATERMASK"), water) && water.size() == count;

        std::vector<GpuVertex> vertices(count);
        for (size_t i = 0; i < count; ++i) {
          GpuVertex& v = vertices[i];
          v.position = positions[i];
          v.normalWater = packNormalWater(hasNormals ? normals[i] : glm::vec3(0.0f), hasWater ? water[i] : 0.0f);
          v.overlayUv = hasUv0 ? uv0[i] : glm::vec2(0.0f);
          v.color = hasColors ? colors[i] : 0xffffffffu;
          v.height = hasHeight ? heights[i] : 0.0f;
        }

        std::vector<uint32_t> indices;
        if (primitive.indices >= 0) {
          if (!readIndices(gltf, primitive.indices, indices)) {
            SPDLOG_LOGGER_WARN(Log::gpu(), "Primitive with unreadable indices skipped");
            ++_failures;
            return;
          }
        } else {
          indices.resize(count);
          for (size_t i = 0; i < count; ++i) indices[i] = uint32_t(i);
        }
        if (indices.empty()) {
          return;
        }

        GpuPrimitive gp;
        D3D11_BUFFER_DESC vbDesc{};
        vbDesc.ByteWidth = UINT(vertices.size() * sizeof(GpuVertex));
        vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vbData{vertices.data(), 0, 0};
        if (!HR_OK(_device->CreateBuffer(&vbDesc, &vbData, &gp.vertexBuffer))) {
          ++_failures;
          return;
        }

        D3D11_BUFFER_DESC ibDesc{};
        ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA ibData{};
        std::vector<uint16_t> indices16;
        if (count <= 65535) {
          indices16.assign(indices.begin(), indices.end());
          ibDesc.ByteWidth = UINT(indices16.size() * sizeof(uint16_t));
          ibData.pSysMem = indices16.data();
          gp.indexFormat = DXGI_FORMAT_R16_UINT;
        } else {
          ibDesc.ByteWidth = UINT(indices.size() * sizeof(uint32_t));
          ibData.pSysMem = indices.data();
          gp.indexFormat = DXGI_FORMAT_R32_UINT;
        }
        if (!HR_OK(_device->CreateBuffer(&ibDesc, &ibData, &gp.indexBuffer))) {
          ++_failures;
          return;
        }
        gp.indexCount = UINT(indices.size());
        gp.localTransform = root * transform;
        gp.hasNormals = hasNormals;
        gp.hasVertexColors = hasColors;
        gp.isTerrain = hasHeight;
        gp.isBuilding = hasFacade;

        const Material* pMaterial = Model::getSafe(&gltf.materials, primitive.material);
        if (pMaterial && pMaterial->pbrMetallicRoughness) {
          const auto& f = pMaterial->pbrMetallicRoughness->baseColorFactor;
          if (f.size() >= 4) {
            gp.baseColor = glm::vec4(float(f[0]), float(f[1]), float(f[2]), float(f[3]));
          }
        }
        pTile->gpuBytes += vbDesc.ByteWidth + ibDesc.ByteWidth;
        pTile->primitives.push_back(std::move(gp));
      });

  _tiles += 1;
  _tileBytes += int64_t(pTile->gpuBytes);
  return pTile.release();
}

CesiumAsync::Future<TileLoadResultAndRenderResources> RendererResources::prepareInLoadThread(
    const CesiumAsync::AsyncSystem& asyncSystem,
    TileLoadResult&& tileLoadResult,
    const glm::dmat4& /*transform*/,
    const std::any& /*rendererOptions*/) {
  void* pResources = nullptr;
  if (auto* pModel = std::get_if<Model>(&tileLoadResult.contentKind)) {
    pResources = createTile(*pModel);
  }
  return asyncSystem.createResolvedFuture(TileLoadResultAndRenderResources{std::move(tileLoadResult), pResources});
}

void* RendererResources::prepareInMainThread(Tile& /*tile*/, void* pLoadThreadResult) { return pLoadThreadResult; }

void RendererResources::free(Tile& /*tile*/, void* pLoadThreadResult, void* pMainThreadResult) noexcept {
  GpuTile* pTile = static_cast<GpuTile*>(pMainThreadResult ? pMainThreadResult : pLoadThreadResult);
  if (pTile) {
    _tiles -= 1;
    _tileBytes -= int64_t(pTile->gpuBytes);
    delete pTile;
  }
}

void* RendererResources::prepareRasterInLoadThread(CesiumImage::ImageAsset& image, const std::any& /*options*/) {
  if (image.compressedPixelFormat != CesiumImage::GpuCompressedPixelFormat::NONE || image.bytesPerChannel != 1 ||
      image.width <= 0 || image.height <= 0) {
    SPDLOG_LOGGER_WARN(Log::gpu(), "Unsupported raster image ({}x{}, {} channels, {} bytes/channel)", image.width,
                       image.height, image.channels, image.bytesPerChannel);
    ++_failures;
    return nullptr;
  }
  if (image.channels != 4) {
    image.changeNumberOfChannels(4, std::byte{255});
  }
  if (image.mipPositions.empty()) {
    std::optional<std::string> error = CesiumImage::ImageDecoder::generateMipMaps(image);
    if (error) {
      SPDLOG_LOGGER_WARN(Log::gpu(), "Mipmap generation failed: {}", *error);
    }
  }

  const UINT mipCount = image.mipPositions.empty() ? 1u : UINT(image.mipPositions.size());
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = UINT(image.width);
  desc.Height = UINT(image.height);
  desc.MipLevels = mipCount;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  std::vector<D3D11_SUBRESOURCE_DATA> init(mipCount);
  size_t bytes = 0;
  for (UINT m = 0; m < mipCount; ++m) {
    const UINT w = std::max(1u, desc.Width >> m);
    const size_t offset = image.mipPositions.empty() ? 0 : image.mipPositions[m].byteOffset;
    init[m].pSysMem = image.pixelData.data() + offset;
    init[m].SysMemPitch = w * 4;
    bytes += size_t(w) * std::max(1u, desc.Height >> m) * 4;
  }

  auto pRaster = std::make_unique<GpuRaster>();
  ComPtr<ID3D11Texture2D> texture;
  if (!HR_OK(_device->CreateTexture2D(&desc, init.data(), &texture)) ||
      !HR_OK(_device->CreateShaderResourceView(texture.Get(), nullptr, &pRaster->srv))) {
    ++_failures;
    return nullptr;
  }
  pRaster->width = image.width;
  pRaster->height = image.height;
  pRaster->gpuBytes = bytes;
  _rasters += 1;
  _rasterBytes += int64_t(bytes);
  return pRaster.release();
}

void* RendererResources::prepareRasterInMainThread(CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
                                                   void* pLoadThreadResult) {
  return pLoadThreadResult;
}

void RendererResources::freeRaster(const CesiumRasterOverlays::RasterOverlayTile& /*rasterTile*/,
                                   void* pLoadThreadResult,
                                   void* pMainThreadResult) noexcept {
  GpuRaster* pRaster = static_cast<GpuRaster*>(pMainThreadResult ? pMainThreadResult : pLoadThreadResult);
  if (pRaster) {
    _rasters -= 1;
    _rasterBytes -= int64_t(pRaster->gpuBytes);
    delete pRaster;
  }
}

// Raster attachments are read directly from Tile::getMappedRasterTiles() at
// draw time, so attach/detach only need to exist.
void RendererResources::attachRasterInMainThread(const Tile&, int32_t, const CesiumRasterOverlays::RasterOverlayTile&,
                                                 void*, const glm::dvec2&, const glm::dvec2&) {}

void RendererResources::detachRasterInMainThread(const Tile&, int32_t, const CesiumRasterOverlays::RasterOverlayTile&,
                                                 void*) noexcept {}

RendererResources::Stats RendererResources::stats() const {
  return Stats{_tiles.load(), _tileBytes.load(), _rasters.load(), _rasterBytes.load(), _failures.load()};
}
