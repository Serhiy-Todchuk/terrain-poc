#pragma once
#include <Cesium3DTilesSelection/IPrepareRendererResources.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <atomic>
#include <vector>

// Vertex layout shared by all tile meshes (terrain, buildings, generic glTF).
struct GpuVertex {
  glm::vec3 position;   // relative to the primitive's model matrix
  uint32_t normalWater; // R10G10B10A2_UNORM: normal * 0.5 + 0.5, A = water mask
                        // (normal (0.5,0.5,0.5) = "no normal" -> flat shading)
  glm::vec2 overlayUv;  // _CESIUMOVERLAY_0 (Cesium raster overlay texcoords), or
                        // _FACADE for buildings (x: window bays, y: floors above ground)
  uint32_t color;       // COLOR_0 as RGBA8 (0xffffffff if absent); buildings: A = façade style
  float height;         // _HEIGHT (orthometric height, terrain only)
};
static_assert(sizeof(GpuVertex) == 32, "unexpected GpuVertex layout");

inline uint32_t packNormalWater(const glm::vec3& n, float water) {
  auto q = [](float v) { return uint32_t(std::clamp(v * 0.5f + 0.5f, 0.0f, 1.0f) * 1023.0f + 0.5f); };
  return q(n.x) | (q(n.y) << 10) | (q(n.z) << 20) | (uint32_t(water > 0.5f ? 3 : 0) << 30);
}

struct GpuPrimitive {
  Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
  Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
  DXGI_FORMAT indexFormat = DXGI_FORMAT_R32_UINT;
  UINT indexCount = 0;
  glm::dmat4 localTransform{1.0}; // glTF node * RTC * up-axis, relative to the tile transform
  glm::vec4 baseColor{1.0f};
  bool hasNormals = false;
  bool hasVertexColors = false;
  bool isTerrain = false;
  bool isBuilding = false; // has _FACADE coordinates (procedural façades + roof imagery)
};

struct GpuTile {
  std::vector<GpuPrimitive> primitives;
  size_t gpuBytes = 0;
};

struct GpuRaster {
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  int width = 0;
  int height = 0;
  size_t gpuBytes = 0;
};

// Bridges Cesium Native's tile/raster lifecycle to Direct3D 11 resources.
// Load-thread callbacks run on worker threads; ID3D11Device resource creation
// is free-threaded, so buffers and textures are created there directly.
class RendererResources final : public Cesium3DTilesSelection::IPrepareRendererResources {
public:
  explicit RendererResources(ID3D11Device* pDevice);

  CesiumAsync::Future<Cesium3DTilesSelection::TileLoadResultAndRenderResources> prepareInLoadThread(
      const CesiumAsync::AsyncSystem& asyncSystem,
      Cesium3DTilesSelection::TileLoadResult&& tileLoadResult,
      const glm::dmat4& transform,
      const std::any& rendererOptions) override;

  void* prepareInMainThread(Cesium3DTilesSelection::Tile& tile, void* pLoadThreadResult) override;

  void free(Cesium3DTilesSelection::Tile& tile, void* pLoadThreadResult, void* pMainThreadResult) noexcept override;

  void* prepareRasterInLoadThread(CesiumImage::ImageAsset& image, const std::any& rendererOptions) override;

  void* prepareRasterInMainThread(CesiumRasterOverlays::RasterOverlayTile& rasterTile,
                                  void* pLoadThreadResult) override;

  void freeRaster(const CesiumRasterOverlays::RasterOverlayTile& rasterTile,
                  void* pLoadThreadResult,
                  void* pMainThreadResult) noexcept override;

  void attachRasterInMainThread(const Cesium3DTilesSelection::Tile& tile,
                                int32_t overlayTextureCoordinateID,
                                const CesiumRasterOverlays::RasterOverlayTile& rasterTile,
                                void* pMainThreadRendererResources,
                                const glm::dvec2& translation,
                                const glm::dvec2& scale) override;

  void detachRasterInMainThread(const Cesium3DTilesSelection::Tile& tile,
                                int32_t overlayTextureCoordinateID,
                                const CesiumRasterOverlays::RasterOverlayTile& rasterTile,
                                void* pMainThreadRendererResources) noexcept override;

  struct Stats {
    int64_t tiles = 0;
    int64_t tileBytes = 0;
    int64_t rasters = 0;
    int64_t rasterBytes = 0;
    int64_t failures = 0;
  };
  Stats stats() const;

private:
  GpuTile* createTile(CesiumGltf::Model& model);

  Microsoft::WRL::ComPtr<ID3D11Device> _device;
  std::atomic<int64_t> _tiles{0}, _tileBytes{0}, _rasters{0}, _rasterBytes{0}, _failures{0};
};
