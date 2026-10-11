#pragma once
#include "Camera.h"
#include "CloudFields.h"
#include "RendererResources.h"

#include <Cesium3DTilesSelection/Tile.h>

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

// One weather dataset on the GPU (Renderer::createCloudTexture): a 4-slice
// RGBA16F array (low / middle / high layer + the step-1 single layer: cover,
// base km, optical depth, thickness km) and a 3-slice RG16F array (the layer
// wind per cell, m/s east / north) on CloudGrid.
struct CloudTexture {
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> wind;
};

// The cloud state of a frame: two datasets blended in time with wind
// advection (motion-compensated per cell: a drifts forward from its valid time
// with its wind field, b backward from its own), cross-faded in from the last
// snapshot. The noise phases drift the same way (CSBlendClouds writes the
// per-cell offsets).
struct CloudFrame {
  const CloudTexture* a = nullptr; // dataset at / before the weather time (required)
  const CloudTexture* b = nullptr; // the next one (nullptr: a alone)
  float weightB = 0.0f;            // time blend a -> b
  float fade = 1.0f;               // snapshot -> (a, b) blend (Renderer::snapshotClouds)
  float driftA = 0.0f, driftB = 0.0f;  // s since the valid time of a / b (b: negative) that the winds act for
  // A newer run of the same hours replaced a / b: their winds glide from the old
  // datasets' (kept alive by the app) to the new ones by `glide` (1: no glide).
  const CloudTexture* oldA = nullptr;
  const CloudTexture* oldB = nullptr;
  float glide = 1.0f;
  glm::vec2 noiseA[3]{}, noiseB[3]{};  // per layer: seed offset of the noise phase of a / b (km east, north)
  glm::vec2 meanDriftA[3]{}, meanDriftB[3]{}; // per layer: drift of the phases with the mean wind (km): change key
  bool layer[3] = {true, true, true};  // low / middle / high drawn
  double manualAltitude = -1.0;        // m MSL for every layer (< 0: per-cell HRRR altitudes)
  // The step-1 look: one thin shell (the datasets' single layer in slice 0,
  // static noise of one phase, no slab samples); the app passes no drift.
  bool single = false;
};

struct SceneSettings {
  bool water = true;
  bool contours = false;
  bool wireframe = false;
  bool imagery = true;
  bool facades = true; // procedural building façades + roofs from the imagery
  bool fog = true;     // aerial perspective (haze over distance)
  // Time-of-day light: sky colours, sun colour and light levels follow the sun
  // elevation (night, twilight). Off: the fixed daylight look.
  bool daylight = false;
  double waterAbsorption = 0.12;
  double contourInterval = 1.0;
  glm::dvec3 sunDirectionEcef{0.0, 0.0, 1.0};
  double timeSeconds = 0.0;
  bool clouds = false;      // cloud layers (cloud.a set)
  bool cloudShadows = true; // the clouds shade terrain, buildings, vessels, water and lower layers
  CloudFrame cloud;
};

struct DrawStats {
  int drawCalls = 0;
  int64_t triangles = 0;
  int tilesWithoutImagery = 0;
};

class Renderer {
public:
  bool init(HWND hwnd, int width, int height, bool debugDevice, const std::wstring& shaderDir, int msaaSamples);
  void shutdown();
  bool resize(int width, int height);
  bool reloadShaders();
  // Changes the MSAA sample count (1 = off); falls back to the highest supported
  // count <= samples. Recompiles the shaders and recreates the render targets.
  bool setSampleCount(int samples);
  int sampleCount() const { return _samples; }

  ID3D11Device* device() const { return _device.Get(); }
  ID3D11DeviceContext* context() const { return _context.Get(); }
  int width() const { return _width; }
  int height() const { return _height; }

  // Frame: [updateRoofImagery] -> beginFrame -> draw* -> resolveScene -> drawHud -> [bindUiTarget
  // + Dear ImGui] -> present.
  // Refreshes (at most) one cascade of the top-down capture of the draped
  // terrain imagery that building roofs sample; cascades are re-rendered when
  // their snapped centre moves or the terrain tiles / imagery under them change.
  void updateRoofImagery(const Camera& camera,
                         const std::vector<Cesium3DTilesSelection::Tile::ConstPointer>& terrainTiles);
  void beginFrame(const Camera& camera, const SceneSettings& settings);
  void drawTiles(const std::vector<Cesium3DTilesSelection::Tile::ConstPointer>& tiles, bool terrain);
  void drawPrimitive(const GpuPrimitive& primitive, const glm::dmat4& model);
  void drawWater(const Camera& camera);
  // Cloud layers (after the water): low / middle / high curved shells at their
  // per-cell altitudes, ray-intersected per pixel by a full-screen pass.
  void drawClouds();
  // Uploads a weather dataset (main thread). Null on failure (logged).
  // The weather box of the cloud fields (same cell counts; "Fetch weather here"
  // moves it): the lon/lat mapping of every cloud shader and the shadow map.
  void setCloudGrid(const CloudGrid& grid);
  std::shared_ptr<CloudTexture> createCloudTexture(const CloudFields& fields);
  // Keeps the cloud state shown in the last frame as the source of the next
  // cross-fade (CloudFrame::fade). Call between frames.
  void snapshotClouds();
  // Resolves the multisampled scene into the back buffer (no-op without MSAA).
  void resolveScene();
  // leftInset: pixels kept free at the left of the text (the HUD collapse arrow).
  void drawHud(const std::vector<std::wstring>& lines, float leftInset = 0.0f);
  // HUD text size factor (DPI scale, 1 = 96 dpi).
  void setHudScale(float scale);
  // Binds the back buffer through a UNORM (non-sRGB) view for UI that blends in
  // gamma space (Dear ImGui), full-window viewport.
  void bindUiTarget();
  bool present(bool vsync);

  bool saveScreenshot(const std::wstring& path);
  // Writes 24-bit BGR pixels (rows top-down, no padding) as a PNG (WIC; COM must
  // be initialised on the calling thread: the main thread). Creates the folder.
  static bool writePng(const std::wstring& path, uint32_t width, uint32_t height, const uint8_t* bgr);
  // Reads the depth buffer of the last rendered frame and reconstructs the
  // ECEF position under a pixel (nullopt for sky). Call outside a frame.
  std::optional<glm::dvec3> pickWorld(int x, int y, const Camera& camera);

  const DrawStats& stats() const { return _stats; }
  int64_t roofCaptures() const { return _roofCaptures; } // cascades rendered since start

private:
  struct FrameConstants {
    glm::mat4 proj;
    glm::mat4 viewRot;
    glm::vec4 sunDirView;
    glm::vec4 upView;
    glm::vec4 screen;
    glm::vec4 projParams;
    glm::vec4 fogParams;
    glm::vec4 waterParams;
    glm::vec4 hazeColor;
    glm::vec4 skyZenith;
    glm::mat4 roofCascade[3]; // view space -> roof imagery uv per cascade
    glm::vec4 roofParams;     // xyz: cascade valid, w: façades on
    glm::vec4 light;          // x: time-of-day light on (1/0), y: sin(sun elevation) at the camera
    glm::vec4 cloudOrigin;    // xyz: camera - sphere centre (ECEF axes, m), w: camera height above the sphere (m)
    glm::vec4 cloudCamera;    // xyz: camera ECEF (m), w: sphere radius (m)
    glm::vec4 cloudGrid;      // x: west, y: north (rad), z: 1/(east-west), w: 1/(north-south) (1/rad)
    glm::vec4 cloudParams;    // x: sea surface above the sphere (m), y/z: noise anchor lon/lat (rad),
                              // w: MSL -> height above the sphere (geoid, m)
    glm::vec4 cloudLayers;    // xyz: layer drawn (1/0), w: manual altitude (m MSL, < 0: per cell)
    glm::vec4 cloudNoise[3];  // per layer: xy noise seed of phase a, zw of phase b (km; + the per-cell drift)
    glm::vec4 cloudMisc;      // x: weight of phase b, y: shadows on, z: clouds on, w: single layer (step-1 look)
  };
  // Mirrors cbuffer CloudBlendConstants (CSBlendClouds).
  struct CloudBlendConstants {
    glm::vec4 params; // x: weight of b, y: fade from the snapshot, zw: 1 / texture size
    glm::vec4 times;  // x: drift time of a (s), y: of b, z: wind glide (old -> new datasets), w: single layer (1/0)
    glm::vec4 grid;   // x: north (rad), y: lat span (rad), z: 1 / (R lon span), w: 1 / (R lat span) (uv per m)
  };
  struct DrawConstants {
    glm::mat4 modelView;
    glm::vec4 overlayST;
    glm::vec4 baseColor;
    uint32_t flags[4];
  };
  struct WaterVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 wave;
  };

  bool createSizeDependent();
  void releaseSizeDependent();
  bool compileShaders();
  int supportedSampleCount(int requested) const;
  void setDrawConstants(const DrawConstants& dc);
  void buildWaterMesh(const Camera& camera);
  // Scene colour target: the MSAA texture, or the back buffer without MSAA.
  ID3D11RenderTargetView* sceneRtv() const { return _samples > 1 ? _msaaRtv.Get() : _rtv.Get(); }

  HWND _hwnd = nullptr;
  int _width = 0, _height = 0;
  int _samples = 1;
  std::wstring _shaderDir;

  Microsoft::WRL::ComPtr<ID3D11Device> _device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> _context;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> _swapChain;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> _backBuffer;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _rtv;   // sRGB view of the back buffer
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _uiRtv; // UNORM view of the back buffer (UI)
  Microsoft::WRL::ComPtr<ID3D11Texture2D> _msaaColor; // only with MSAA
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> _msaaRtv;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _msaaSrv;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> _depth; // _samples samples
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> _dsv;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilView> _dsvReadOnly;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _depthSrv;

  // Depth picking: CSPickDepth writes one float, copied to a 4-byte staging buffer.
  Microsoft::WRL::ComPtr<ID3D11Buffer> _pickBuffer, _pickStaging;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _pickUav;

  // Roof imagery: top-down orthographic captures of the draped terrain imagery
  // in nested squares around the camera (like shadow cascades), one slice each
  // of a mip-mapped Texture2DArray. Reuses the imagery the terrain has already
  // streamed at view-dependent resolution (no extra requests).
  static constexpr int kRoofCascades = 3;
  static constexpr UINT kRoofSize = 2048;
  struct RoofCascade {
    double extent = 0.0;        // side of the captured square (meters)
    glm::dmat4 view{1.0};       // ECEF -> local ENU at the capture centre (as rendered)
    glm::dvec2 key{0.0};        // snapped lon/lat of the rendered centre (degrees)
    uint64_t signature = 0;     // tiles + imagery that went into the capture
    int64_t renderedFrame = -1; // -1: never rendered
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv; // this slice only (GenerateMips)
  };
  bool createRoofImagery();
  RoofCascade _roof[kRoofCascades];
  Microsoft::WRL::ComPtr<ID3D11Texture2D> _roofTexture;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _roofSrv;
  int64_t _roofFrame = 0;
  int64_t _roofCaptures = 0;

  // Cloud layers: the frame's blend of two datasets (CSBlendClouds, every frame
  // the clouds are on) in a mip-mapped 3-slice RGBA16F array that the cloud,
  // terrain and water shaders read; the snapshot is the cross-fade source.
  // The shadow map (CSCloudShadow, after the frame constants) holds the sun's
  // transmission through all layers at sea level over the same domain.
  // It is re-rendered only when its inputs change noticeably (a drifting live
  // sky: every few seconds), never for camera motion.
  struct CloudShadowKey {
    const CloudTexture* a = nullptr;
    const CloudTexture* b = nullptr;
    glm::vec4 drift[3]{}; // mean drift of the phases (km)
    glm::vec4 layers{0.0f};
    glm::vec3 sun{0.0f};
    float weightB = 0.0f, fade = 0.0f;
    bool single = false;
  };
  bool createCloudTargets();
  void blendClouds(const CloudFrame& frame);
  void renderCloudShadows();
  static constexpr UINT kCloudShadowSize = 2048; // over the central half of the grid: ~0.3 km texels
  CloudShadowKey _cloudShadowKey;
  bool _cloudShadowValid = false;
  int64_t _cloudShadowUpdates = 0;
  CloudGrid _cloudGrid; // the weather box the cloud textures cover (setCloudGrid)
  Microsoft::WRL::ComPtr<ID3D11Texture2D> _cloudBlend, _cloudSnapshot, _cloudShadow, _cloudDrift;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _cloudBlendSrv, _cloudSnapshotSrv, _cloudShadowSrv, _cloudDriftSrv;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _cloudBlendUav, _cloudShadowUav, _cloudDriftUav;
  Microsoft::WRL::ComPtr<ID3D11Buffer> _cloudBlendCb;
  UINT _cloudBlendMips = 1;
  bool _cloudBlendValid = false, _cloudSnapshotValid = false;

  Microsoft::WRL::ComPtr<ID3D11VertexShader> _vsMain, _vsWater, _vsSky, _vsCapture;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> _psMain, _psWater, _psSky, _psResolve, _psCapture, _psClouds;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> _csPickDepth, _csBlendClouds, _csCloudShadow;
  Microsoft::WRL::ComPtr<ID3D11InputLayout> _layoutMain, _layoutWater;
  Microsoft::WRL::ComPtr<ID3D11Buffer> _frameCb, _drawCb;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rsSolid, _rsWire;
  Microsoft::WRL::ComPtr<ID3D11DepthStencilState> _dsOpaque, _dsWater, _dsOff;
  Microsoft::WRL::ComPtr<ID3D11BlendState> _bsAlpha;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> _samplerClamp;

  Microsoft::WRL::ComPtr<ID3D11Buffer> _waterVb, _waterIb;
  UINT _waterIndexCount = 0;
  glm::dvec3 _waterCenter{0.0};
  glm::dvec3 _waterBuiltFrom{0.0};
  glm::dvec3 _waveOrigin{0.0};
  bool _waveOriginSet = false;

  Microsoft::WRL::ComPtr<ID2D1Factory1> _d2dFactory;
  Microsoft::WRL::ComPtr<ID2D1Device> _d2dDevice;
  Microsoft::WRL::ComPtr<ID2D1DeviceContext> _d2dContext;
  Microsoft::WRL::ComPtr<ID2D1Bitmap1> _d2dTarget;
  Microsoft::WRL::ComPtr<IDWriteFactory> _dwFactory;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> _textFormat;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> _textBrush, _bgBrush;
  float _hudScale = 1.0f;

  glm::dmat4 _view{1.0};
  glm::dmat4 _proj{1.0};
  SceneSettings _settings;
  DrawStats _stats;
  bool _depthValidForPick = false;
};
