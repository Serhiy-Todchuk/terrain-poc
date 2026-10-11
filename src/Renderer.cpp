#include "pch.h"
#include "Renderer.h"

#include "GeoRef.h"
#include "Log.h"

#include <Cesium3DTilesSelection/BoundingVolume.h>
#include <CesiumRasterOverlays/RasterOverlayTile.h>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/packing.hpp>

using namespace Cesium3DTilesSelection;
using CesiumUtility::Math;

namespace {

ComPtr<ID3D11InfoQueue> g_infoQueue;

// Origin of the cloud noise pattern (fixed, so the cloud shapes stay put when
// the camera moves).
constexpr double kCloudNoiseLon = -74.0, kCloudNoiseLat = 40.65;

// The ready imagery draped on a tile (single overlay) and its mapping.
struct TileOverlay {
  ID3D11ShaderResourceView* srv = nullptr;
  glm::vec4 st{1.0f, 1.0f, 0.0f, 0.0f}; // xy: scale, zw: translation
  uint32_t uvSet = 0;
};

TileOverlay findOverlay(const Tile& tile) {
  TileOverlay o;
  for (const RasterMappedTo3DTile& mapped : tile.getMappedRasterTiles()) {
    const CesiumRasterOverlays::RasterOverlayTile* pRaster = mapped.getReadyTile();
    if (!pRaster) {
      continue;
    }
    const GpuRaster* pGpuRaster = static_cast<const GpuRaster*>(pRaster->getRendererResources());
    if (!pGpuRaster || !pGpuRaster->srv) {
      continue;
    }
    o.srv = pGpuRaster->srv.Get();
    o.st = glm::vec4(float(mapped.getScale().x), float(mapped.getScale().y), float(mapped.getTranslation().x),
                     float(mapped.getTranslation().y));
    o.uvSet = uint32_t(std::max(0, mapped.getTextureCoordinateID()));
    break;
  }
  return o;
}

const GpuTile* gpuTile(const Tile& tile) {
  const TileRenderContent* pContent = tile.getContent().getRenderContent();
  return pContent ? static_cast<const GpuTile*>(pContent->getRenderResources()) : nullptr;
}

ComPtr<ID3DBlob> compileShader(const std::wstring& file, const char* entry, const char* target, int msaaSamples) {
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
  flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
  const std::string samples = std::to_string(msaaSamples);
  const D3D_SHADER_MACRO defines[] = {{"MSAA_SAMPLES", samples.c_str()}, {nullptr, nullptr}};
  ComPtr<ID3DBlob> blob, errors;
  HRESULT hr = D3DCompileFromFile(file.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0,
                                  &blob, &errors);
  if (errors && errors->GetBufferSize() > 0) {
    std::string text(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r')) {
      text.pop_back();
    }
    if (FAILED(hr)) {
      SPDLOG_LOGGER_ERROR(Log::gpu(), "Shader {} ({}) failed to compile:\n{}", entry, target, text);
    } else {
      SPDLOG_LOGGER_WARN(Log::gpu(), "Shader {} ({}) warnings:\n{}", entry, target, text);
    }
  }
  if (FAILED(hr)) {
    SPDLOG_LOGGER_ERROR(Log::gpu(), "D3DCompileFromFile({}, {}) failed: {}", Log::toUtf8(file), entry,
                        Log::hrToString(hr));
    return nullptr;
  }
  return blob;
}

glm::vec4 srgbToLinear(float r, float g, float b) {
  auto f = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
  return glm::vec4(f(r), f(g), f(b), 1.0f);
}

// Sky colours of the time-of-day light (linear), from the daylight colours
// passed in: unchanged with the sun above ~15 degrees, deep blue twilight
// around sunset (the shader adds the glow towards the sun), night below about
// -15 degrees (moonless, but not black).
void skyColours(double sinElevation, glm::vec3& zenith, glm::vec3& haze) {
  auto smooth = [](double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return float(t * t * (3.0 - 2.0 * t));
  };
  const glm::vec3 duskZenith(0.040f, 0.075f, 0.20f), duskHaze(0.40f, 0.36f, 0.38f);
  const glm::vec3 nightZenith(0.0015f, 0.0025f, 0.0065f), nightHaze(0.0045f, 0.0065f, 0.012f);
  const float day = smooth(0.0, 0.26, sinElevation), night = 1.0f - smooth(-0.26, -0.03, sinElevation);
  zenith = glm::mix(glm::mix(duskZenith, zenith, day), nightZenith, night);
  haze = glm::mix(glm::mix(duskHaze, haze, day), nightHaze, night);
}

void pollInfoQueue() {
  if (!g_infoQueue) {
    return;
  }
  const UINT64 count = g_infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
  for (UINT64 i = 0; i < count; ++i) {
    SIZE_T size = 0;
    g_infoQueue->GetMessage(i, nullptr, &size);
    std::vector<char> buffer(size);
    auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buffer.data());
    if (FAILED(g_infoQueue->GetMessage(i, msg, &size))) {
      continue;
    }
    std::string text(msg->pDescription, msg->DescriptionByteLength);
    switch (msg->Severity) {
    case D3D11_MESSAGE_SEVERITY_CORRUPTION:
    case D3D11_MESSAGE_SEVERITY_ERROR: SPDLOG_LOGGER_ERROR(Log::gpu(), "D3D11: {}", text); break;
    case D3D11_MESSAGE_SEVERITY_WARNING: SPDLOG_LOGGER_WARN(Log::gpu(), "D3D11: {}", text); break;
    default: SPDLOG_LOGGER_DEBUG(Log::gpu(), "D3D11: {}", text); break;
    }
  }
  g_infoQueue->ClearStoredMessages();
}

} // namespace

bool Renderer::init(HWND hwnd, int width, int height, bool debugDevice, const std::wstring& shaderDir,
                    int msaaSamples) {
  _hwnd = hwnd;
  _width = std::max(1, width);
  _height = std::max(1, height);
  _shaderDir = shaderDir;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  if (debugDevice) {
    flags |= D3D11_CREATE_DEVICE_DEBUG;
  }
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL level{};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                                 &_device, &level, &_context);
  if (FAILED(hr) && debugDevice) {
    SPDLOG_LOGGER_WARN(Log::gpu(),
                       "D3D11 debug device unavailable ({}); install the 'Graphics Tools' optional Windows feature "
                       "for API validation. Continuing without it.",
                       Log::hrToString(hr));
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &_device,
                           &level, &_context);
  }
  if (!HR_OK(hr)) {
    return false;
  }
  if (flags & D3D11_CREATE_DEVICE_DEBUG) {
    if (SUCCEEDED(_device.As(&g_infoQueue))) {
      g_infoQueue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
      SPDLOG_LOGGER_INFO(Log::gpu(), "D3D11 debug layer enabled; messages are forwarded to the log");
    }
  }

  ComPtr<IDXGIDevice> dxgiDevice;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (!HR_OK(_device.As(&dxgiDevice)) || !HR_OK(dxgiDevice->GetAdapter(&adapter)) ||
      !HR_OK(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
    return false;
  }
  DXGI_ADAPTER_DESC adesc{};
  adapter->GetDesc(&adesc);
  SPDLOG_LOGGER_INFO(Log::gpu(), "GPU: {} ({} MB dedicated), feature level {:x}", Log::toUtf8(adesc.Description),
                     adesc.DedicatedVideoMemory / (1024 * 1024), unsigned(level));
  _samples = supportedSampleCount(msaaSamples);
  SPDLOG_LOGGER_INFO(Log::gpu(), "MSAA: {}", _samples > 1 ? std::to_string(_samples) + "x" : std::string("off"));

  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = UINT(_width);
  sd.Height = UINT(_height);
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  if (!HR_OK(factory->CreateSwapChainForHwnd(_device.Get(), hwnd, &sd, nullptr, nullptr, &_swapChain))) {
    return false;
  }
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

  // Direct2D / DirectWrite for the HUD.
  D2D1_FACTORY_OPTIONS d2dOptions{};
  if (HR_OK(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &d2dOptions,
                              reinterpret_cast<void**>(_d2dFactory.GetAddressOf()))) &&
      HR_OK(_d2dFactory->CreateDevice(dxgiDevice.Get(), &_d2dDevice)) &&
      HR_OK(_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &_d2dContext)) &&
      HR_OK(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(_dwFactory.GetAddressOf())))) {
    setHudScale(_hudScale);
    HR_OK(_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &_textBrush));
    HR_OK(_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &_bgBrush));
  }

  // Pipeline state.
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  rd.MultisampleEnable = TRUE; // only affects lines (wireframe) on MSAA targets: quadrilateral AA lines
  HR_OK(_device->CreateRasterizerState(&rd, &_rsSolid));
  rd.FillMode = D3D11_FILL_WIREFRAME;
  HR_OK(_device->CreateRasterizerState(&rd, &_rsWire));

  D3D11_DEPTH_STENCIL_DESC dsd{};
  dsd.DepthEnable = TRUE;
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dsd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; // reversed Z
  HR_OK(_device->CreateDepthStencilState(&dsd, &_dsOpaque));
  dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  dsd.DepthFunc = D3D11_COMPARISON_GREATER;
  HR_OK(_device->CreateDepthStencilState(&dsd, &_dsWater));
  dsd.DepthEnable = FALSE;
  HR_OK(_device->CreateDepthStencilState(&dsd, &_dsOff));

  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].BlendEnable = TRUE;
  bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  HR_OK(_device->CreateBlendState(&bd, &_bsAlpha));

  D3D11_SAMPLER_DESC smp{};
  smp.Filter = D3D11_FILTER_ANISOTROPIC;
  smp.MaxAnisotropy = 8;
  smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  smp.MaxLOD = D3D11_FLOAT32_MAX;
  HR_OK(_device->CreateSamplerState(&smp, &_samplerClamp));

  D3D11_BUFFER_DESC cbd{};
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  cbd.ByteWidth = UINT((sizeof(FrameConstants) + 15) & ~size_t(15));
  HR_OK(_device->CreateBuffer(&cbd, nullptr, &_frameCb));
  cbd.ByteWidth = UINT((sizeof(DrawConstants) + 15) & ~size_t(15));
  HR_OK(_device->CreateBuffer(&cbd, nullptr, &_drawCb));

  D3D11_BUFFER_DESC pbd{};
  pbd.ByteWidth = sizeof(float);
  pbd.Usage = D3D11_USAGE_DEFAULT;
  pbd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
  uavd.Format = DXGI_FORMAT_R32_FLOAT;
  uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uavd.Buffer.NumElements = 1;
  if (HR_OK(_device->CreateBuffer(&pbd, nullptr, &_pickBuffer))) {
    HR_OK(_device->CreateUnorderedAccessView(_pickBuffer.Get(), &uavd, &_pickUav));
  }
  pbd.Usage = D3D11_USAGE_STAGING;
  pbd.BindFlags = 0;
  pbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  HR_OK(_device->CreateBuffer(&pbd, nullptr, &_pickStaging));

  createRoofImagery();  // optional: roofs fall back to plain colours without it
  createCloudTargets(); // optional: no clouds without it

  if (!compileShaders()) {
    return false;
  }
  return createSizeDependent();
}

void Renderer::shutdown() {
  if (_context) {
    _context->ClearState();
    _context->Flush();
  }
  if (_d2dContext) {
    _d2dContext->SetTarget(nullptr);
  }
  g_infoQueue.Reset();
}

bool Renderer::createRoofImagery() {
  const double extents[kRoofCascades] = {1000.0, 4000.0, 16000.0};
  D3D11_TEXTURE2D_DESC td{};
  td.Width = kRoofSize;
  td.Height = kRoofSize;
  td.MipLevels = 0; // full chain
  td.ArraySize = kRoofCascades;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  if (!HR_OK(_device->CreateTexture2D(&td, nullptr, &_roofTexture)) ||
      !HR_OK(_device->CreateShaderResourceView(_roofTexture.Get(), nullptr, &_roofSrv))) {
    _roofTexture.Reset();
    return false;
  }
  for (int c = 0; c < kRoofCascades; ++c) {
    RoofCascade& rc = _roof[c];
    rc.extent = extents[c];
    D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format = td.Format;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rtvd.Texture2DArray.FirstArraySlice = UINT(c);
    rtvd.Texture2DArray.ArraySize = 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = td.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvd.Texture2DArray.MipLevels = UINT(-1);
    srvd.Texture2DArray.FirstArraySlice = UINT(c);
    srvd.Texture2DArray.ArraySize = 1;
    if (!HR_OK(_device->CreateRenderTargetView(_roofTexture.Get(), &rtvd, &rc.rtv)) ||
        !HR_OK(_device->CreateShaderResourceView(_roofTexture.Get(), &srvd, &rc.srv))) {
      _roofTexture.Reset();
      return false;
    }
  }
  SPDLOG_LOGGER_INFO(Log::gpu(), "Roof imagery capture: {} cascades ({:.0f}/{:.0f}/{:.0f} m) of {}x{} px, {:.0f} MB",
                     kRoofCascades, extents[0], extents[1], extents[2], kRoofSize, kRoofSize,
                     kRoofCascades * double(kRoofSize) * kRoofSize * 4.0 * 4.0 / 3.0 / (1024.0 * 1024.0));
  return true;
}

bool Renderer::createCloudTargets() {
  const CloudGrid grid;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = UINT(grid.nx);
  td.Height = UINT(grid.ny);
  td.MipLevels = 0; // full chain (seen from far away the grid is minified)
  td.ArraySize = kCloudLayerCount;
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET; // RT: GenerateMips
  td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  D3D11_UNORDERED_ACCESS_VIEW_DESC uavd{};
  uavd.Format = td.Format;
  uavd.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
  uavd.Texture2DArray.MipSlice = 0;
  uavd.Texture2DArray.FirstArraySlice = 0;
  uavd.Texture2DArray.ArraySize = kCloudLayerCount;
  if (!HR_OK(_device->CreateTexture2D(&td, nullptr, &_cloudBlend)) ||
      !HR_OK(_device->CreateShaderResourceView(_cloudBlend.Get(), nullptr, &_cloudBlendSrv)) ||
      !HR_OK(_device->CreateUnorderedAccessView(_cloudBlend.Get(), &uavd, &_cloudBlendUav))) {
    _cloudBlendUav.Reset();
    return false;
  }
  _cloudBlend->GetDesc(&td);
  _cloudBlendMips = td.MipLevels;
  td.MipLevels = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = 0;
  D3D11_BUFFER_DESC cbd{};
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  cbd.ByteWidth = UINT((sizeof(CloudBlendConstants) + 15) & ~size_t(15));
  if (!HR_OK(_device->CreateTexture2D(&td, nullptr, &_cloudSnapshot)) ||
      !HR_OK(_device->CreateShaderResourceView(_cloudSnapshot.Get(), nullptr, &_cloudSnapshotSrv)) ||
      !HR_OK(_device->CreateBuffer(&cbd, nullptr, &_cloudBlendCb))) {
    _cloudBlendUav.Reset();
    return false;
  }
  // Per-cell drift of the two noise phases (km): written by CSBlendClouds, read
  // wherever the noise is evaluated (smooth: no mips).
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  if (!HR_OK(_device->CreateTexture2D(&td, nullptr, &_cloudDrift)) ||
      !HR_OK(_device->CreateShaderResourceView(_cloudDrift.Get(), nullptr, &_cloudDriftSrv)) ||
      !HR_OK(_device->CreateUnorderedAccessView(_cloudDrift.Get(), &uavd, &_cloudDriftUav))) {
    _cloudBlendUav.Reset();
    return false;
  }
  D3D11_TEXTURE2D_DESC sd{};
  sd.Width = kCloudShadowSize;
  sd.Height = kCloudShadowSize;
  sd.MipLevels = 1;
  sd.ArraySize = 1;
  sd.Format = DXGI_FORMAT_R16_FLOAT;
  sd.SampleDesc.Count = 1;
  sd.Usage = D3D11_USAGE_DEFAULT;
  sd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  if (!HR_OK(_device->CreateTexture2D(&sd, nullptr, &_cloudShadow)) ||
      !HR_OK(_device->CreateShaderResourceView(_cloudShadow.Get(), nullptr, &_cloudShadowSrv)) ||
      !HR_OK(_device->CreateUnorderedAccessView(_cloudShadow.Get(), nullptr, &_cloudShadowUav))) {
    _cloudShadowUav.Reset(); // clouds without shadows
  }
  _cloudGrid = grid;
  return true;
}

bool Renderer::compileShaders() {
  const std::wstring file = _shaderDir + L"\\Scene.hlsl";
  auto vsMain = compileShader(file, "VSMain", "vs_5_0", _samples);
  auto psMain = compileShader(file, "PSMain", "ps_5_0", _samples);
  auto vsWater = compileShader(file, "VSWater", "vs_5_0", _samples);
  auto psWater = compileShader(file, "PSWater", "ps_5_0", _samples);
  auto vsSky = compileShader(file, "VSSky", "vs_5_0", _samples);
  auto psSky = compileShader(file, "PSSky", "ps_5_0", _samples);
  auto vsCapture = compileShader(file, "VSCapture", "vs_5_0", _samples);
  auto psCapture = compileShader(file, "PSCapture", "ps_5_0", _samples);
  auto psClouds = compileShader(file, "PSClouds", "ps_5_0", _samples);
  auto csPick = compileShader(file, "CSPickDepth", "cs_5_0", _samples);
  auto csBlend = compileShader(file, "CSBlendClouds", "cs_5_0", _samples);
  auto csShadow = compileShader(file, "CSCloudShadow", "cs_5_0", _samples);
  auto psResolve = _samples > 1 ? compileShader(file, "PSResolve", "ps_5_0", _samples) : nullptr;
  if (!vsMain || !psMain || !vsWater || !psWater || !vsSky || !psSky || !vsCapture || !psCapture || !psClouds ||
      !csPick || !csBlend || !csShadow || (_samples > 1 && !psResolve)) {
    return false;
  }
  ComPtr<ID3D11VertexShader> vs1, vs2, vs3, vs4;
  ComPtr<ID3D11PixelShader> ps1, ps2, ps3, ps4, ps5, ps6;
  ComPtr<ID3D11ComputeShader> cs1, cs2, cs3;
  ComPtr<ID3D11InputLayout> layoutMain, layoutWater;
  bool ok = HR_OK(_device->CreateVertexShader(vsMain->GetBufferPointer(), vsMain->GetBufferSize(), nullptr, &vs1)) &&
            HR_OK(_device->CreatePixelShader(psMain->GetBufferPointer(), psMain->GetBufferSize(), nullptr, &ps1)) &&
            HR_OK(_device->CreateVertexShader(vsWater->GetBufferPointer(), vsWater->GetBufferSize(), nullptr, &vs2)) &&
            HR_OK(_device->CreatePixelShader(psWater->GetBufferPointer(), psWater->GetBufferSize(), nullptr, &ps2)) &&
            HR_OK(_device->CreateVertexShader(vsSky->GetBufferPointer(), vsSky->GetBufferSize(), nullptr, &vs3)) &&
            HR_OK(_device->CreatePixelShader(psSky->GetBufferPointer(), psSky->GetBufferSize(), nullptr, &ps3)) &&
            HR_OK(_device->CreateVertexShader(vsCapture->GetBufferPointer(), vsCapture->GetBufferSize(), nullptr,
                                              &vs4)) &&
            HR_OK(_device->CreatePixelShader(psCapture->GetBufferPointer(), psCapture->GetBufferSize(), nullptr,
                                             &ps5)) &&
            HR_OK(_device->CreatePixelShader(psClouds->GetBufferPointer(), psClouds->GetBufferSize(), nullptr,
                                             &ps6)) &&
            HR_OK(_device->CreateComputeShader(csPick->GetBufferPointer(), csPick->GetBufferSize(), nullptr, &cs1)) &&
            HR_OK(_device->CreateComputeShader(csBlend->GetBufferPointer(), csBlend->GetBufferSize(), nullptr, &cs2)) &&
            HR_OK(_device->CreateComputeShader(csShadow->GetBufferPointer(), csShadow->GetBufferSize(), nullptr, &cs3)) &&
            (!psResolve ||
             HR_OK(_device->CreatePixelShader(psResolve->GetBufferPointer(), psResolve->GetBufferSize(), nullptr, &ps4)));
  if (!ok) {
    return false;
  }
  const D3D11_INPUT_ELEMENT_DESC mainLayout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  const D3D11_INPUT_ELEMENT_DESC waterLayout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  ok = HR_OK(_device->CreateInputLayout(mainLayout, UINT(std::size(mainLayout)), vsMain->GetBufferPointer(),
                                        vsMain->GetBufferSize(), &layoutMain)) &&
       HR_OK(_device->CreateInputLayout(waterLayout, UINT(std::size(waterLayout)), vsWater->GetBufferPointer(),
                                        vsWater->GetBufferSize(), &layoutWater));
  if (!ok) {
    return false;
  }
  _vsMain = vs1;
  _psMain = ps1;
  _vsWater = vs2;
  _psWater = ps2;
  _vsSky = vs3;
  _psSky = ps3;
  _psResolve = ps4;
  _vsCapture = vs4;
  _psCapture = ps5;
  _psClouds = ps6;
  _csPickDepth = cs1;
  _csBlendClouds = cs2;
  _csCloudShadow = cs3;
  _layoutMain = layoutMain;
  _layoutWater = layoutWater;
  SPDLOG_LOGGER_INFO(Log::gpu(), "Shaders compiled from {} (MSAA_SAMPLES={})", Log::toUtf8(file), _samples);
  return true;
}

bool Renderer::reloadShaders() { return compileShaders(); }

int Renderer::supportedSampleCount(int requested) const {
  for (const int count : {8, 4, 2}) {
    UINT colorQuality = 0, depthQuality = 0;
    if (count <= requested &&
        SUCCEEDED(_device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, UINT(count),
                                                         &colorQuality)) &&
        SUCCEEDED(_device->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, UINT(count), &depthQuality)) &&
        colorQuality > 0 && depthQuality > 0) {
      if (count != requested) {
        SPDLOG_LOGGER_WARN(Log::gpu(), "MSAA {}x is not supported by this GPU, using {}x", requested, count);
      }
      return count;
    }
  }
  if (requested > 1) {
    SPDLOG_LOGGER_WARN(Log::gpu(), "MSAA {}x is not supported by this GPU, MSAA disabled", requested);
  }
  return 1;
}

bool Renderer::setSampleCount(int samples) {
  samples = supportedSampleCount(samples);
  if (samples == _samples) {
    return true;
  }
  const int previous = _samples;
  _samples = samples;
  if (!compileShaders()) {
    _samples = previous; // the shaders of the previous count are still in place
    return false;
  }
  releaseSizeDependent();
  if (!createSizeDependent()) {
    return false;
  }
  SPDLOG_LOGGER_INFO(Log::gpu(), "MSAA: {}", _samples > 1 ? std::to_string(_samples) + "x" : std::string("off"));
  return true;
}

bool Renderer::createSizeDependent() {
  if (!HR_OK(_swapChain->GetBuffer(0, IID_PPV_ARGS(&_backBuffer)))) {
    return false;
  }
  D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
  rtvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  if (!HR_OK(_device->CreateRenderTargetView(_backBuffer.Get(), &rtvd, &_rtv))) {
    return false;
  }
  rtvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  if (!HR_OK(_device->CreateRenderTargetView(_backBuffer.Get(), &rtvd, &_uiRtv))) {
    return false;
  }

  const bool msaa = _samples > 1;
  if (msaa) {
    // The scene is rendered into this texture and resolved into the back buffer
    // by PSResolve (the flip-model back buffer itself cannot be multisampled).
    D3D11_TEXTURE2D_DESC cd{};
    cd.Width = UINT(_width);
    cd.Height = UINT(_height);
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    cd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    cd.SampleDesc.Count = UINT(_samples);
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (!HR_OK(_device->CreateTexture2D(&cd, nullptr, &_msaaColor)) ||
        !HR_OK(_device->CreateRenderTargetView(_msaaColor.Get(), nullptr, &_msaaRtv)) ||
        !HR_OK(_device->CreateShaderResourceView(_msaaColor.Get(), nullptr, &_msaaSrv))) {
      return false;
    }
  }

  D3D11_TEXTURE2D_DESC dd{};
  dd.Width = UINT(_width);
  dd.Height = UINT(_height);
  dd.MipLevels = 1;
  dd.ArraySize = 1;
  dd.Format = DXGI_FORMAT_R32_TYPELESS;
  dd.SampleDesc.Count = UINT(_samples);
  dd.Usage = D3D11_USAGE_DEFAULT;
  dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
  if (!HR_OK(_device->CreateTexture2D(&dd, nullptr, &_depth))) {
    return false;
  }
  D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
  dsvd.Format = DXGI_FORMAT_D32_FLOAT;
  dsvd.ViewDimension = msaa ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
  HR_OK(_device->CreateDepthStencilView(_depth.Get(), &dsvd, &_dsv));
  dsvd.Flags = D3D11_DSV_READ_ONLY_DEPTH;
  HR_OK(_device->CreateDepthStencilView(_depth.Get(), &dsvd, &_dsvReadOnly));
  D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
  srvd.Format = DXGI_FORMAT_R32_FLOAT;
  if (msaa) {
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
  }
  HR_OK(_device->CreateShaderResourceView(_depth.Get(), &srvd, &_depthSrv));
  _depthValidForPick = false;

  if (_d2dContext) {
    ComPtr<IDXGISurface> surface;
    if (HR_OK(_backBuffer.As(&surface))) {
      const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
          D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
      if (HR_OK(_d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &props, &_d2dTarget))) {
        _d2dContext->SetTarget(_d2dTarget.Get());
      }
    }
  }
  return _rtv && _dsv && _depthSrv;
}

void Renderer::releaseSizeDependent() {
  if (_d2dContext) {
    _d2dContext->SetTarget(nullptr);
  }
  _d2dTarget.Reset();
  _context->OMSetRenderTargets(0, nullptr, nullptr);
  _rtv.Reset();
  _uiRtv.Reset();
  _backBuffer.Reset();
  _msaaSrv.Reset();
  _msaaRtv.Reset();
  _msaaColor.Reset();
  _dsv.Reset();
  _dsvReadOnly.Reset();
  _depthSrv.Reset();
  _depth.Reset();
  _context->Flush();
}

bool Renderer::resize(int width, int height) {
  if (width <= 0 || height <= 0 || (width == _width && height == _height)) {
    return true;
  }
  releaseSizeDependent();
  _width = width;
  _height = height;
  if (!HR_OK(_swapChain->ResizeBuffers(0, UINT(width), UINT(height), DXGI_FORMAT_UNKNOWN, 0))) {
    return false;
  }
  SPDLOG_LOGGER_INFO(Log::gpu(), "Resized to {}x{}", width, height);
  return createSizeDependent();
}

void Renderer::setDrawConstants(const DrawConstants& dc) {
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (SUCCEEDED(_context->Map(_drawCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    std::memcpy(mapped.pData, &dc, sizeof(dc));
    _context->Unmap(_drawCb.Get(), 0);
  }
}

void Renderer::updateRoofImagery(const Camera& camera, const std::vector<Tile::ConstPointer>& terrainTiles) {
  ++_roofFrame;
  if (!_roofTexture || !_vsCapture || !_psCapture || terrainTiles.empty() || camera.altitudeMsl() > 50000.0) {
    return;
  }
  const CesiumGeospatial::Ellipsoid& ellipsoid = GeoRef::ellipsoid();
  const glm::dmat4 enu = camera.enuFrame();
  const glm::dvec3 forward =
      glm::dvec3(enu[0]) * std::sin(camera.heading) + glm::dvec3(enu[1]) * std::cos(camera.heading);
  const glm::dvec3 nadir = ellipsoid.scaleToGeodeticSurface(camera.position).value_or(camera.position);

  // Desired state of each cascade: centred ahead of the nadir (visible roofs lie
  // in the view direction), snapped to a grid of extent/16 so the capture only
  // moves in steps; the signature covers the tiles + imagery inside it.
  struct Candidate {
    glm::dvec2 key{0.0};
    uint64_t signature = 1469598103934665603ull;
    std::vector<const Tile*> tiles;
  };
  Candidate wanted[kRoofCascades];
  int pick = -1;
  for (int c = 0; c < kRoofCascades; ++c) {
    const RoofCascade& rc = _roof[c];
    const auto carto = ellipsoid.cartesianToCartographic(nadir + forward * (0.35 * rc.extent));
    if (!carto) {
      continue;
    }
    const double step = rc.extent / 16.0;
    const double stepLat = step / 111320.0;
    const double lat = std::round(Math::radiansToDegrees(carto->latitude) / stepLat) * stepLat;
    const double metersPerLonDeg = 111320.0 * std::max(0.05, std::cos(Math::degreesToRadians(lat)));
    const double lon = std::round(Math::radiansToDegrees(carto->longitude) / (step / metersPerLonDeg)) *
                       (step / metersPerLonDeg);
    Candidate& w = wanted[c];
    w.key = glm::dvec2(lon, lat);
    const double halfLat = 0.6 * rc.extent / 111320.0, halfLon = 0.6 * rc.extent / metersPerLonDeg;
    const auto area = CesiumGeospatial::GlobeRectangle::fromDegrees(lon - halfLon, lat - halfLat, lon + halfLon,
                                                                    lat + halfLat);
    for (const Tile::ConstPointer& pTile : terrainTiles) {
      const auto rect = estimateGlobeRectangle(pTile->getBoundingVolume(), ellipsoid);
      const TileOverlay overlay = findOverlay(*pTile);
      if (!rect || !rect->computeIntersection(area) || !overlay.srv || !gpuTile(*pTile)) {
        continue;
      }
      w.tiles.push_back(pTile.get());
      w.signature = (w.signature ^ uint64_t(reinterpret_cast<uintptr_t>(pTile.get()))) * 1099511628211ull;
      w.signature = (w.signature ^ uint64_t(reinterpret_cast<uintptr_t>(overlay.srv))) * 1099511628211ull;
    }
    const bool dirty = rc.renderedFrame < 0 || w.signature != rc.signature || w.key != rc.key;
    if (dirty && !w.tiles.empty() && (pick < 0 || rc.renderedFrame < _roof[pick].renderedFrame)) {
      pick = c; // the one waiting longest
    }
  }
  if (pick < 0) {
    return;
  }

  RoofCascade& rc = _roof[pick];
  const Candidate& w = wanted[pick];
  const glm::dvec3 center =
      ellipsoid.cartographicToCartesian(CesiumGeospatial::Cartographic::fromDegrees(w.key.x, w.key.y, 0.0));
  const glm::dmat4 view =
      glm::affineInverse(CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(center, ellipsoid));
  glm::dmat4 ortho(1.0); // x, y: +-extent/2 -> +-1 (north up); z: +-10 km -> [0, 1]
  ortho[0][0] = 2.0 / rc.extent;
  ortho[1][1] = 2.0 / rc.extent;
  ortho[2][2] = 1.0 / 20000.0;
  ortho[3][2] = 0.5;
  const glm::dmat4 viewProj = ortho * view;

  ID3D11ShaderResourceView* none = nullptr;
  _context->PSSetShaderResources(3, 1, &none); // still bound from the last frame
  ID3D11RenderTargetView* rtv = rc.rtv.Get();
  _context->OMSetRenderTargets(1, &rtv, nullptr);
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  _context->ClearRenderTargetView(rtv, clear);
  const D3D11_VIEWPORT vp{0.0f, 0.0f, float(kRoofSize), float(kRoofSize), 0.0f, 1.0f};
  _context->RSSetViewports(1, &vp);
  _context->RSSetState(_rsSolid.Get());
  _context->OMSetDepthStencilState(_dsOff.Get(), 0); // the selected tiles do not overlap
  _context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  _context->IASetInputLayout(_layoutMain.Get());
  _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  _context->VSSetShader(_vsCapture.Get(), nullptr, 0);
  _context->PSSetShader(_psCapture.Get(), nullptr, 0);
  ID3D11Buffer* cbs[2] = {_frameCb.Get(), _drawCb.Get()};
  _context->VSSetConstantBuffers(0, 2, cbs);
  _context->PSSetConstantBuffers(0, 2, cbs);
  ID3D11SamplerState* sampler = _samplerClamp.Get();
  _context->PSSetSamplers(0, 1, &sampler);

  int draws = 0;
  for (const Tile* pTile : w.tiles) {
    const TileOverlay overlay = findOverlay(*pTile);
    ID3D11ShaderResourceView* srv = overlay.srv;
    _context->PSSetShaderResources(0, 1, &srv);
    for (const GpuPrimitive& prim : gpuTile(*pTile)->primitives) {
      if (!prim.isTerrain) {
        continue;
      }
      DrawConstants dc{};
      dc.modelView = glm::mat4(viewProj * pTile->getTransform() * prim.localTransform);
      dc.overlayST = overlay.st;
      dc.flags[0] = 1u;
      dc.flags[1] = overlay.uvSet;
      setDrawConstants(dc);
      const UINT stride = sizeof(GpuVertex), offset = 0;
      ID3D11Buffer* vb = prim.vertexBuffer.Get();
      _context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
      _context->IASetIndexBuffer(prim.indexBuffer.Get(), prim.indexFormat, 0);
      _context->DrawIndexed(prim.indexCount, 0, 0);
      ++draws;
    }
  }
  _context->PSSetShaderResources(0, 1, &none);
  _context->OMSetRenderTargets(0, nullptr, nullptr);
  _context->GenerateMips(rc.srv.Get());

  rc.view = view;
  rc.key = w.key;
  rc.signature = w.signature;
  rc.renderedFrame = _roofFrame;
  ++_roofCaptures;
  SPDLOG_LOGGER_DEBUG(Log::gpu(), "Roof imagery cascade {} ({:.0f} m) captured at {:.5f},{:.5f}: {} tiles, {} draws",
                      pick, rc.extent, w.key.x, w.key.y, w.tiles.size(), draws);
}

void Renderer::beginFrame(const Camera& camera, const SceneSettings& settings) {
  _settings = settings;
  _stats = {};
  _view = camera.viewMatrix();
  _proj = camera.projectionMatrix();

  // This frame's cloud state (compute) before anything reads it.
  const bool clouds = settings.clouds && settings.cloud.a && settings.cloud.a->srv && _csBlendClouds && _cloudBlendUav;
  if (clouds) {
    blendClouds(settings.cloud);
  }
  _settings.clouds = clouds && _cloudBlendValid;

  ID3D11RenderTargetView* rtv = sceneRtv();
  _context->OMSetRenderTargets(1, &rtv, _dsv.Get());
  D3D11_VIEWPORT vp{0.0f, 0.0f, float(_width), float(_height), 0.0f, 1.0f};
  _context->RSSetViewports(1, &vp);
  const float clear[4] = {0.5f, 0.6f, 0.7f, 1.0f};
  _context->ClearRenderTargetView(rtv, clear);
  _context->ClearDepthStencilView(_dsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);

  glm::dmat4 rot = _view;
  rot[3] = glm::dvec4(0.0, 0.0, 0.0, 1.0);
  const glm::dmat3 rot3(rot);
  const glm::dvec3 upEcef = GeoRef::ellipsoid().geodeticSurfaceNormal(camera.position);
  const double altitude = camera.altitudeMsl();

  FrameConstants fc{};
  fc.proj = glm::mat4(_proj);
  fc.viewRot = glm::mat4(rot);
  fc.sunDirView = glm::vec4(glm::vec3(glm::normalize(rot3 * settings.sunDirectionEcef)), 0.0f);
  fc.upView = glm::vec4(glm::vec3(rot3 * upEcef), float(altitude));
  fc.screen = glm::vec4(float(_width), float(_height), 1.0f / float(_width), 1.0f / float(_height));
  const double tanY = std::tan(camera.fovY * 0.5);
  fc.projParams = glm::vec4(float(tanY * camera.aspect), float(tanY), float(camera.nearPlane),
                            float(std::fmod(settings.timeSeconds, 10000.0)));
  // Aerial perspective: visibility grows with altitude.
  const double fogDistance = 70000.0 + 6.0 * std::max(altitude, 0.0);
  fc.fogParams = glm::vec4(float(fogDistance), settings.fog && altitude <= 150000.0 ? 0.85f : 0.0f, 0.0f, 0.0f);
  fc.waterParams = glm::vec4(float(settings.waterAbsorption), settings.contours ? float(settings.contourInterval) : 0.0f,
                             float(GeoRef::kSeaLevelNavd88), 0.0f);
  const double sinSunElevation = glm::dot(glm::normalize(settings.sunDirectionEcef), upEcef);
  glm::vec3 zenith(srgbToLinear(0.30f, 0.50f, 0.82f)), haze(srgbToLinear(0.78f, 0.84f, 0.90f));
  if (settings.daylight) {
    skyColours(sinSunElevation, zenith, haze);
  }
  fc.hazeColor = glm::vec4(haze, 1.0f);
  fc.skyZenith = glm::vec4(zenith, 1.0f);
  fc.light = glm::vec4(settings.daylight ? 1.0f : 0.0f, float(sinSunElevation), 0.0f, 0.0f);
  // Roof imagery lookup: view space -> ECEF -> capture ENU -> texture uv
  // (u east, v south), composed in double with the matrices each cascade was
  // rendered with.
  const glm::dmat4 invView = glm::affineInverse(_view);
  float roofValid[kRoofCascades] = {};
  for (int c = 0; c < kRoofCascades; ++c) {
    const RoofCascade& rc = _roof[c];
    if (rc.renderedFrame < 0 || !_roofSrv) {
      continue;
    }
    glm::dmat4 toUv(1.0);
    toUv[0][0] = 1.0 / rc.extent;
    toUv[1][1] = -1.0 / rc.extent;
    toUv[3][0] = 0.5;
    toUv[3][1] = 0.5;
    fc.roofCascade[c] = glm::mat4(toUv * rc.view * invView);
    roofValid[c] = settings.imagery ? 1.0f : 0.0f;
  }
  fc.roofParams = glm::vec4(roofValid[0], roofValid[1], roofValid[2], settings.facades ? 1.0f : 0.0f);

  // A sphere osculating the ellipsoid (Gaussian radius of curvature) at the
  // camera nadir clamped to the weather grid - within the grid it stays within
  // metres of "height above the ellipsoid": the cloud shells (radius + layer
  // altitude), the cloud shadows and the per-pixel sun elevation use it. The
  // large terms are formed here in double; the shaders get the camera height.
  {
    const CesiumGeospatial::Ellipsoid& ellipsoid = GeoRef::ellipsoid();
    const CesiumGeospatial::Cartographic carto = camera.cartographic();
    const double refLon = std::clamp(Math::radiansToDegrees(carto.longitude), _cloudGrid.west, _cloudGrid.east);
    const double refLat = std::clamp(Math::radiansToDegrees(carto.latitude), _cloudGrid.south, _cloudGrid.north);
    const glm::dvec3 radii = ellipsoid.getRadii();
    const double e2 = 1.0 - (radii.z * radii.z) / (radii.x * radii.x);
    const double sinLat = std::sin(Math::degreesToRadians(refLat));
    const double w = 1.0 - e2 * sinLat * sinLat;
    const double meridional = radii.x * (1.0 - e2) / (w * std::sqrt(w)), normal = radii.x / std::sqrt(w);
    const double radius = std::sqrt(meridional * normal);
    const glm::dvec3 surface =
        ellipsoid.cartographicToCartesian(CesiumGeospatial::Cartographic::fromDegrees(refLon, refLat, 0.0));
    const glm::dvec3 centre = surface - ellipsoid.geodeticSurfaceNormal(surface) * radius;
    const glm::dvec3 oc = camera.position - centre;
    fc.cloudOrigin = glm::vec4(glm::vec3(oc), float(glm::length(oc) - radius));
    fc.cloudCamera = glm::vec4(glm::vec3(camera.position), float(radius));
    const double west = Math::degreesToRadians(_cloudGrid.west), north = Math::degreesToRadians(_cloudGrid.north);
    fc.cloudGrid = glm::vec4(float(west), float(north), float(1.0 / (Math::degreesToRadians(_cloudGrid.east) - west)),
                             float(1.0 / (north - Math::degreesToRadians(_cloudGrid.south))));
    fc.cloudParams = glm::vec4(float(GeoRef::seaSurfaceEllipsoidHeight(refLon, refLat)),
                               float(Math::degreesToRadians(kCloudNoiseLon)),
                               float(Math::degreesToRadians(kCloudNoiseLat)),
                               float(GeoRef::navd88ToEllipsoid(0.0, refLon, refLat)));
    const CloudFrame& cf = settings.cloud;
    fc.cloudLayers = glm::vec4(cf.layer[0] ? 1.0f : 0.0f, cf.layer[1] ? 1.0f : 0.0f, cf.layer[2] ? 1.0f : 0.0f,
                               float(cf.manualAltitude));
    for (int l = 0; l < kCloudLayerCount; ++l) {
      fc.cloudNoise[l] = glm::vec4(cf.noiseA[l], cf.noiseB[l]);
    }
    const bool shadows = _settings.clouds && settings.cloudShadows && _cloudShadowUav && _csCloudShadow;
    // The single layer keeps one static noise phase (the fields still blend in time).
    fc.cloudMisc = glm::vec4(cf.b && !cf.single ? std::clamp(cf.weightB, 0.0f, 1.0f) : 0.0f, shadows ? 1.0f : 0.0f,
                             _settings.clouds ? 1.0f : 0.0f, cf.single ? 1.0f : 0.0f);
    fc.cloudView = glm::vec4(std::clamp(cf.opacity, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f);
  }

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (SUCCEEDED(_context->Map(_frameCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    std::memcpy(mapped.pData, &fc, sizeof(fc));
    _context->Unmap(_frameCb.Get(), 0);
  }
  if (fc.cloudMisc.y > 0.5f) {
    CloudShadowKey key;
    key.a = settings.cloud.a;
    key.b = settings.cloud.b;
    for (int l = 0; l < kCloudLayerCount; ++l) {
      key.drift[l] = glm::vec4(settings.cloud.meanDriftA[l], settings.cloud.meanDriftB[l]);
    }
    key.layers = fc.cloudLayers;
    key.sun = glm::vec3(glm::normalize(settings.sunDirectionEcef));
    key.weightB = settings.cloud.b ? std::clamp(settings.cloud.weightB, 0.0f, 1.0f) : 0.0f;
    key.fade = _cloudSnapshotValid ? settings.cloud.fade : 1.0f;
    key.single = settings.cloud.single;
    const CloudShadowKey& last = _cloudShadowKey;
    bool changed = !_cloudShadowValid || key.a != last.a || key.b != last.b || key.layers != last.layers ||
                   key.single != last.single ||
                   glm::length(key.sun - last.sun) > 3e-4f || std::abs(key.weightB - last.weightB) > 0.002f ||
                   std::abs(key.fade - last.fade) > 0.01f || (key.fade == 1.0f) != (last.fade == 1.0f);
    for (int l = 0; l < kCloudLayerCount && !changed; ++l) {
      const glm::vec4 d = glm::abs(key.drift[l] - last.drift[l]);
      changed = std::max(std::max(d.x, d.y), std::max(d.z, d.w)) > 0.05f; // km of drift
    }
    if (changed) {
      renderCloudShadows(); // reads the frame constants
      _cloudShadowKey = key;
      _cloudShadowValid = true;
      ++_cloudShadowUpdates;
    }
  }
  ID3D11Buffer* cbs[2] = {_frameCb.Get(), _drawCb.Get()};
  _context->VSSetConstantBuffers(0, 2, cbs);
  _context->PSSetConstantBuffers(0, 2, cbs);
  ID3D11SamplerState* sampler = _samplerClamp.Get();
  _context->PSSetSamplers(0, 1, &sampler);
  ID3D11ShaderResourceView* frameTextures[3] = {_roofSrv.Get(), _settings.clouds ? _cloudBlendSrv.Get() : nullptr,
                                                _settings.clouds ? _cloudShadowSrv.Get() : nullptr};
  _context->PSSetShaderResources(3, 3, frameTextures); // roof imagery, cloud layers, cloud shadow map
  ID3D11ShaderResourceView* drift = _settings.clouds ? _cloudDriftSrv.Get() : nullptr;
  _context->PSSetShaderResources(9, 1, &drift); // noise phase drift per cell

  // Sky (full-screen triangle).
  _context->IASetInputLayout(nullptr);
  _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  _context->VSSetShader(_vsSky.Get(), nullptr, 0);
  _context->PSSetShader(_psSky.Get(), nullptr, 0);
  _context->OMSetDepthStencilState(_dsOff.Get(), 0);
  _context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  _context->RSSetState(_rsSolid.Get());
  _context->Draw(3, 0);

  // Opaque scene state.
  _context->IASetInputLayout(_layoutMain.Get());
  _context->VSSetShader(_vsMain.Get(), nullptr, 0);
  _context->PSSetShader(_psMain.Get(), nullptr, 0);
  _context->OMSetDepthStencilState(_dsOpaque.Get(), 0);
  _context->RSSetState(settings.wireframe ? _rsWire.Get() : _rsSolid.Get());
}

void Renderer::drawTiles(const std::vector<Tile::ConstPointer>& tiles, bool terrain) {
  const glm::vec4 terrainFallback(0.42f, 0.44f, 0.40f, 1.0f);
  for (const Tile::ConstPointer& pTile : tiles) {
    const Tile& tile = *pTile;
    const GpuTile* pGpu = gpuTile(tile);
    if (!pGpu) {
      continue;
    }

    const TileOverlay overlay = _settings.imagery ? findOverlay(tile) : TileOverlay{};
    if (terrain && !overlay.srv) {
      ++_stats.tilesWithoutImagery;
    }
    ID3D11ShaderResourceView* srv = overlay.srv;
    _context->PSSetShaderResources(0, 1, &srv);

    for (const GpuPrimitive& prim : pGpu->primitives) {
      DrawConstants dc{};
      dc.modelView = glm::mat4(_view * tile.getTransform() * prim.localTransform);
      dc.overlayST = overlay.st;
      dc.baseColor = (prim.isTerrain && !overlay.srv) ? terrainFallback : prim.baseColor;
      dc.flags[0] = overlay.srv ? 1u : 0u;
      dc.flags[1] = overlay.uvSet;
      dc.flags[2] = prim.isTerrain ? 0u : (prim.isBuilding ? 2u : 1u);
      dc.flags[3] = prim.hasVertexColors ? 1u : 0u;
      setDrawConstants(dc);
      const UINT stride = sizeof(GpuVertex), offset = 0;
      ID3D11Buffer* vb = prim.vertexBuffer.Get();
      _context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
      _context->IASetIndexBuffer(prim.indexBuffer.Get(), prim.indexFormat, 0);
      _context->DrawIndexed(prim.indexCount, 0, 0);
      ++_stats.drawCalls;
      _stats.triangles += prim.indexCount / 3;
    }
  }
  ID3D11ShaderResourceView* none = nullptr;
  _context->PSSetShaderResources(0, 1, &none);
}

void Renderer::drawPrimitive(const GpuPrimitive& prim, const glm::dmat4& model) {
  DrawConstants dc{};
  dc.modelView = glm::mat4(_view * model * prim.localTransform);
  dc.overlayST = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
  dc.baseColor = prim.baseColor;
  dc.flags[0] = 0;
  dc.flags[1] = 0;
  dc.flags[2] = prim.isTerrain ? 0u : (prim.isBuilding ? 2u : 1u);
  dc.flags[3] = prim.hasVertexColors ? 1u : 0u;
  setDrawConstants(dc);
  const UINT stride = sizeof(GpuVertex), offset = 0;
  ID3D11Buffer* vb = prim.vertexBuffer.Get();
  _context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  _context->IASetIndexBuffer(prim.indexBuffer.Get(), prim.indexFormat, 0);
  _context->DrawIndexed(prim.indexCount, 0, 0);
  ++_stats.drawCalls;
  _stats.triangles += prim.indexCount / 3;
}

void Renderer::buildWaterMesh(const Camera& camera) {
  const CesiumGeospatial::Ellipsoid& ellipsoid = GeoRef::ellipsoid();
  const double altitude = std::max(camera.altitudeMsl(), 1.0);
  if (_waterVb && glm::length(camera.position - _waterBuiltFrom) < std::max(0.5, 0.01 * altitude)) {
    return;
  }
  constexpr int kRings = 64;
  constexpr int kSegments = 128;
  const glm::dvec3 nadir = ellipsoid.scaleToGeodeticSurface(camera.position).value_or(camera.position);
  const glm::dmat4 enu = CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(nadir, ellipsoid);
  const glm::dvec3 east(enu[0]), north(enu[1]);
  const double horizon = std::sqrt(2.0 * 6371000.0 * altitude + altitude * altitude);
  const double radius = std::clamp(horizon * 1.3 + 2000.0, 4000.0, 600000.0);

  if (!_waveOriginSet) {
    _waveOrigin = nadir;
    _waveOriginSet = true;
  }
  const glm::dmat4 waveEnu = CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(_waveOrigin, ellipsoid);
  const glm::dvec3 waveEast(waveEnu[0]), waveNorth(waveEnu[1]);

  std::vector<WaterVertex> vertices;
  vertices.reserve(size_t(kRings + 1) * kSegments);
  for (int k = 0; k <= kRings; ++k) {
    const double t = double(k) / kRings;
    const double r = radius * t * t * t;
    for (int m = 0; m < kSegments; ++m) {
      const double a = 2.0 * CesiumUtility::Math::OnePi * m / kSegments;
      const glm::dvec3 planar = nadir + east * (r * std::cos(a)) + north * (r * std::sin(a));
      const glm::dvec3 surface = ellipsoid.scaleToGeodeticSurface(planar).value_or(planar);
      const auto carto = ellipsoid.cartesianToCartographic(surface).value_or(CesiumGeospatial::Cartographic(0, 0, 0));
      const double h = GeoRef::seaSurfaceEllipsoidHeight(CesiumUtility::Math::radiansToDegrees(carto.longitude),
                                                         CesiumUtility::Math::radiansToDegrees(carto.latitude));
      const glm::dvec3 p =
          ellipsoid.cartographicToCartesian(CesiumGeospatial::Cartographic(carto.longitude, carto.latitude, h));
      const glm::dvec3 rel = p - _waveOrigin;
      WaterVertex v;
      v.position = glm::vec3(p - nadir);
      v.normal = glm::vec3(ellipsoid.geodeticSurfaceNormal(p));
      v.wave = glm::vec2(float(glm::dot(rel, waveEast)), float(glm::dot(rel, waveNorth)));
      vertices.push_back(v);
    }
  }

  if (!_waterVb) {
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = UINT(vertices.size() * sizeof(WaterVertex));
    vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!HR_OK(_device->CreateBuffer(&vbd, nullptr, &_waterVb))) {
      return;
    }
    std::vector<uint32_t> indices;
    for (int k = 0; k < kRings; ++k) {
      for (int m = 0; m < kSegments; ++m) {
        const uint32_t a = uint32_t(k * kSegments + m);
        const uint32_t b = uint32_t(k * kSegments + (m + 1) % kSegments);
        const uint32_t c = uint32_t((k + 1) * kSegments + m);
        const uint32_t d = uint32_t((k + 1) * kSegments + (m + 1) % kSegments);
        indices.insert(indices.end(), {a, c, d, a, d, b});
      }
    }
    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = UINT(indices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{indices.data(), 0, 0};
    if (!HR_OK(_device->CreateBuffer(&ibd, &init, &_waterIb))) {
      _waterVb.Reset();
      return;
    }
    _waterIndexCount = UINT(indices.size());
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (HR_OK(_context->Map(_waterVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(WaterVertex));
    _context->Unmap(_waterVb.Get(), 0);
  }
  _waterCenter = nadir;
  _waterBuiltFrom = camera.position;
}

void Renderer::drawWater(const Camera& camera) {
  if (!_settings.water || camera.altitudeMsl() > 250000.0) {
    return;
  }
  buildWaterMesh(camera);
  if (!_waterVb || !_waterIb) {
    return;
  }
  ID3D11RenderTargetView* rtv = sceneRtv();
  _context->OMSetRenderTargets(1, &rtv, _dsvReadOnly.Get());
  _context->IASetInputLayout(_layoutWater.Get());
  _context->VSSetShader(_vsWater.Get(), nullptr, 0);
  _context->PSSetShader(_psWater.Get(), nullptr, 0);
  _context->OMSetDepthStencilState(_dsWater.Get(), 0);
  _context->OMSetBlendState(_bsAlpha.Get(), nullptr, 0xffffffff);
  ID3D11ShaderResourceView* depth = _depthSrv.Get();
  _context->PSSetShaderResources(1, 1, &depth);

  DrawConstants dc{};
  dc.modelView = glm::mat4(_view * glm::translate(glm::dmat4(1.0), _waterCenter));
  dc.baseColor = glm::vec4(1.0f);
  setDrawConstants(dc);
  const UINT stride = sizeof(WaterVertex), offset = 0;
  ID3D11Buffer* vb = _waterVb.Get();
  _context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
  _context->IASetIndexBuffer(_waterIb.Get(), DXGI_FORMAT_R32_UINT, 0);
  _context->DrawIndexed(_waterIndexCount, 0, 0);
  ++_stats.drawCalls;

  ID3D11ShaderResourceView* none = nullptr;
  _context->PSSetShaderResources(1, 1, &none);
  _context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  _context->OMSetRenderTargets(1, &rtv, _dsv.Get());
  _context->IASetInputLayout(_layoutMain.Get());
  _context->VSSetShader(_vsMain.Get(), nullptr, 0);
  _context->PSSetShader(_psMain.Get(), nullptr, 0);
  _context->OMSetDepthStencilState(_dsOpaque.Get(), 0);
}

void Renderer::setCloudGrid(const CloudGrid& grid) {
  if (grid == _cloudGrid) {
    return;
  }
  _cloudGrid.west = grid.west;
  _cloudGrid.east = grid.east;
  _cloudGrid.south = grid.south;
  _cloudGrid.north = grid.north;
  _cloudShadowValid = false;
  _cloudSnapshotValid = false;
}

std::shared_ptr<CloudTexture> Renderer::createCloudTexture(const CloudFields& f) {
  const CloudGrid& g = f.grid;
  const size_t n = size_t(g.nx) * size_t(g.ny);
  bool consistent = n > 0 && g == _cloudGrid && f.cover.size() == n && f.singleOpticalDepth.size() == n;
  for (const CloudLayer& layer : f.layers) {
    consistent = consistent && layer.cover.size() == n && layer.altitude.size() == n &&
                 layer.opticalDepth.size() == n && layer.thickness.size() == n && layer.windEast.size() == n &&
                 layer.windNorth.size() == n;
  }
  if (!consistent || !_cloudBlend) {
    SPDLOG_LOGGER_ERROR(Log::gpu(), "Cloud fields of another box or size ({}x{} grid at {:.2f}, {:.2f}) or no cloud "
                                    "targets",
                        g.nx, g.ny, g.centreLon(), g.centreLat());
    return nullptr;
  }
  // One RGBA16F slice per layer: cover, base (km MSL), in-cloud optical depth,
  // thickness (km). Read only by CSBlendClouds (mip 0, bilinear). Slice 3: the
  // single layer of the step-1 look (the total cover at the dominant layer's
  // altitude, as a thin shell; CloudFrame::single).
  constexpr size_t kSlices = size_t(kCloudLayerCount) + 1;
  constexpr float kSingleThicknessKm = 0.1f; // the minimum slab of PSClouds
  std::vector<uint16_t> texels(n * 4 * kSlices);
  D3D11_SUBRESOURCE_DATA init[kSlices]{};
  for (size_t l = 0; l < size_t(kCloudLayerCount); ++l) {
    const CloudLayer& layer = f.layers[l];
    uint16_t* out = texels.data() + l * n * 4;
    for (size_t k = 0; k < n; ++k) {
      out[k * 4 + 0] = glm::packHalf1x16(layer.cover[k]);
      out[k * 4 + 1] = glm::packHalf1x16(layer.altitude[k] * 0.001f);
      out[k * 4 + 2] = glm::packHalf1x16(layer.opticalDepth[k]);
      out[k * 4 + 3] = glm::packHalf1x16(layer.thickness[k] * 0.001f);
    }
    init[l] = {out, UINT(g.nx) * 8u, 0};
  }
  {
    uint16_t* out = texels.data() + size_t(kCloudLayerCount) * n * 4;
    const uint16_t base = glm::packHalf1x16(float(f.layerAltitude) * 0.001f);
    const uint16_t thickness = glm::packHalf1x16(kSingleThicknessKm);
    for (size_t k = 0; k < n; ++k) {
      out[k * 4 + 0] = glm::packHalf1x16(f.cover[k]);
      out[k * 4 + 1] = base;
      out[k * 4 + 2] = glm::packHalf1x16(f.singleOpticalDepth[k]);
      out[k * 4 + 3] = thickness;
    }
    init[kCloudLayerCount] = {out, UINT(g.nx) * 8u, 0};
  }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = UINT(g.nx);
  td.Height = UINT(g.ny);
  td.MipLevels = 1;
  td.ArraySize = UINT(kSlices);
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> texture;
  auto result = std::make_shared<CloudTexture>();
  if (!HR_OK(_device->CreateTexture2D(&td, init, &texture)) ||
      !HR_OK(_device->CreateShaderResourceView(texture.Get(), nullptr, &result->srv))) {
    return nullptr;
  }
  // The layer wind per cell (m/s east / north): one RG16F slice per layer.
  std::vector<uint16_t> wind(n * 2 * kCloudLayerCount);
  for (size_t l = 0; l < size_t(kCloudLayerCount); ++l) {
    const CloudLayer& layer = f.layers[l];
    uint16_t* out = wind.data() + l * n * 2;
    for (size_t k = 0; k < n; ++k) {
      out[k * 2 + 0] = glm::packHalf1x16(layer.windEast[k]);
      out[k * 2 + 1] = glm::packHalf1x16(layer.windNorth[k]);
    }
    init[l] = {out, UINT(g.nx) * 4u, 0};
  }
  td.ArraySize = kCloudLayerCount;
  td.Format = DXGI_FORMAT_R16G16_FLOAT;
  ComPtr<ID3D11Texture2D> windTexture;
  if (!HR_OK(_device->CreateTexture2D(&td, init, &windTexture)) ||
      !HR_OK(_device->CreateShaderResourceView(windTexture.Get(), nullptr, &result->wind))) {
    return nullptr;
  }
  SPDLOG_LOGGER_DEBUG(Log::gpu(), "Cloud dataset uploaded: {}x{}x{} RGBA16F + RG16F winds ({:.1f} MB)", g.nx, g.ny,
                      int(kSlices), double(texels.size() + wind.size()) * 2.0 / (1024.0 * 1024.0));
  return result;
}

void Renderer::snapshotClouds() {
  if (!_cloudBlendValid || !_cloudSnapshot) {
    return;
  }
  for (UINT slice = 0; slice < UINT(kCloudLayerCount); ++slice) {
    _context->CopySubresourceRegion(_cloudSnapshot.Get(), D3D11CalcSubresource(0, slice, 1), 0, 0, 0,
                                    _cloudBlend.Get(), D3D11CalcSubresource(0, slice, _cloudBlendMips), nullptr);
  }
  _cloudSnapshotValid = true;
}

void Renderer::blendClouds(const CloudFrame& f) {
  // The shader turns winds (m/s east / north) into uv per metre at the latitude of each row.
  const double lonSpan = Math::degreesToRadians(_cloudGrid.east - _cloudGrid.west);
  const double latSpan = Math::degreesToRadians(_cloudGrid.north - _cloudGrid.south);
  constexpr double kEarthRadius = 6371000.0;
  CloudBlendConstants cb{};
  const float fade = _cloudSnapshotValid ? std::clamp(f.fade, 0.0f, 1.0f) : 1.0f;
  cb.params = glm::vec4(f.b ? std::clamp(f.weightB, 0.0f, 1.0f) : 0.0f, fade, 1.0f / float(_cloudGrid.nx),
                        1.0f / float(_cloudGrid.ny));
  cb.times = glm::vec4(f.driftA, f.b ? f.driftB : f.driftA, std::clamp(f.glide, 0.0f, 1.0f), f.single ? 1.0f : 0.0f);
  cb.grid = glm::vec4(float(Math::degreesToRadians(_cloudGrid.north)), float(latSpan),
                      float(1.0 / (kEarthRadius * lonSpan)), float(1.0 / (kEarthRadius * latSpan)));
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (!HR_OK(_context->Map(_cloudBlendCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    return;
  }
  std::memcpy(mapped.pData, &cb, sizeof(cb));
  _context->Unmap(_cloudBlendCb.Get(), 0);

  ID3D11ShaderResourceView* none = nullptr;
  _context->PSSetShaderResources(4, 1, &none); // the outputs are still bound from the last frame
  _context->PSSetShaderResources(9, 1, &none);
  const CloudTexture* b = f.b ? f.b : f.a;
  const CloudTexture* oldA = f.oldA ? f.oldA : f.a;
  const CloudTexture* oldB = f.oldB ? f.oldB : (f.b ? f.b : oldA);
  ID3D11ShaderResourceView* inputs[3] = {f.a->srv.Get(), b->srv.Get(),
                                         _cloudSnapshotValid ? _cloudSnapshotSrv.Get() : f.a->srv.Get()};
  ID3D11ShaderResourceView* winds[4] = {f.a->wind.Get(), b->wind.Get(), oldA->wind.Get(), oldB->wind.Get()};
  ID3D11UnorderedAccessView* uavs[3] = {_cloudBlendUav.Get(), nullptr, _cloudDriftUav.Get()};
  ID3D11Buffer* cbuffer = _cloudBlendCb.Get();
  ID3D11SamplerState* sampler = _samplerClamp.Get();
  _context->CSSetShader(_csBlendClouds.Get(), nullptr, 0);
  _context->CSSetShaderResources(6, 3, inputs);
  _context->CSSetShaderResources(10, 4, winds);
  _context->CSSetUnorderedAccessViews(1, 3, uavs, nullptr);
  _context->CSSetConstantBuffers(2, 1, &cbuffer);
  _context->CSSetSamplers(0, 1, &sampler);
  _context->Dispatch(UINT(_cloudGrid.nx + 7) / 8u, UINT(_cloudGrid.ny + 7) / 8u, UINT(kCloudLayerCount));
  ID3D11ShaderResourceView* noInputs[4] = {nullptr, nullptr, nullptr, nullptr};
  ID3D11UnorderedAccessView* noUavs[3] = {nullptr, nullptr, nullptr};
  _context->CSSetShaderResources(6, 3, noInputs);
  _context->CSSetShaderResources(10, 4, noInputs);
  _context->CSSetUnorderedAccessViews(1, 3, noUavs, nullptr);
  _context->GenerateMips(_cloudBlendSrv.Get());
  _cloudBlendValid = true;
}

void Renderer::renderCloudShadows() {
  ID3D11ShaderResourceView* none = nullptr;
  _context->PSSetShaderResources(5, 1, &none); // the map is still bound from the last frame
  ID3D11Buffer* frame = _frameCb.Get();
  ID3D11ShaderResourceView* field = _cloudBlendSrv.Get();
  ID3D11ShaderResourceView* drift = _cloudDriftSrv.Get();
  ID3D11UnorderedAccessView* uav = _cloudShadowUav.Get();
  ID3D11SamplerState* sampler = _samplerClamp.Get();
  _context->CSSetShader(_csCloudShadow.Get(), nullptr, 0);
  _context->CSSetConstantBuffers(0, 1, &frame);
  _context->CSSetShaderResources(4, 1, &field);
  _context->CSSetShaderResources(9, 1, &drift);
  _context->CSSetUnorderedAccessViews(2, 1, &uav, nullptr);
  _context->CSSetSamplers(0, 1, &sampler);
  _context->Dispatch((kCloudShadowSize + 7) / 8, (kCloudShadowSize + 7) / 8, 1);
  ID3D11UnorderedAccessView* noUav = nullptr;
  _context->CSSetShaderResources(4, 1, &none);
  _context->CSSetShaderResources(9, 1, &none);
  _context->CSSetUnorderedAccessViews(2, 1, &noUav, nullptr);
}

void Renderer::drawClouds() {
  if (!_settings.clouds || !_psClouds || _settings.cloud.opacity <= 0.0f) {
    return; // fully transparent: the shadows (CSCloudShadow) still apply
  }
  // Full-screen pass; depth test against the scene (read-only DSV), no writes:
  // PSClouds outputs the shell depth.
  ID3D11RenderTargetView* rtv = sceneRtv();
  _context->OMSetRenderTargets(1, &rtv, _dsvReadOnly.Get());
  _context->IASetInputLayout(nullptr);
  _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  _context->VSSetShader(_vsSky.Get(), nullptr, 0);
  _context->PSSetShader(_psClouds.Get(), nullptr, 0);
  _context->OMSetDepthStencilState(_dsWater.Get(), 0);
  _context->OMSetBlendState(_bsAlpha.Get(), nullptr, 0xffffffff);
  _context->RSSetState(_rsSolid.Get());
  _context->Draw(3, 0); // the cloud layers (t4) are bound for the whole frame
  ++_stats.drawCalls;

  _context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  _context->OMSetRenderTargets(1, &rtv, _dsv.Get());
  _context->IASetInputLayout(_layoutMain.Get());
  _context->VSSetShader(_vsMain.Get(), nullptr, 0);
  _context->PSSetShader(_psMain.Get(), nullptr, 0);
  _context->OMSetDepthStencilState(_dsOpaque.Get(), 0);
  _context->RSSetState(_settings.wireframe ? _rsWire.Get() : _rsSolid.Get());
}

void Renderer::resolveScene() {
  if (_samples <= 1 || !_psResolve || !_msaaSrv) {
    return;
  }
  // Unbind the MSAA target before reading it; the resolve writes every pixel of
  // the back buffer (sRGB RTV), the HUD is drawn on top afterwards.
  ID3D11RenderTargetView* rtv = _rtv.Get();
  _context->OMSetRenderTargets(1, &rtv, nullptr);
  _context->IASetInputLayout(nullptr);
  _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  _context->VSSetShader(_vsSky.Get(), nullptr, 0);
  _context->PSSetShader(_psResolve.Get(), nullptr, 0);
  _context->OMSetDepthStencilState(_dsOff.Get(), 0);
  _context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  _context->RSSetState(_rsSolid.Get());
  ID3D11ShaderResourceView* color = _msaaSrv.Get();
  _context->PSSetShaderResources(2, 1, &color);
  _context->Draw(3, 0);
  ID3D11ShaderResourceView* none = nullptr;
  _context->PSSetShaderResources(2, 1, &none);
}

void Renderer::drawHud(const std::vector<std::wstring>& lines, float leftInset) {
  if (!_d2dContext || !_d2dTarget || !_textFormat || lines.empty()) {
    return;
  }
  size_t maxChars = 0;
  for (const auto& l : lines) {
    maxChars = std::max(maxChars, l.size());
  }
  const float s = _hudScale;
  const float lineHeight = 17.0f * s;
  const float w = float(maxChars) * 7.75f * s + 16.0f * s + leftInset;
  const float h = float(lines.size()) * lineHeight + 12.0f * s;
  _d2dContext->BeginDraw();
  _d2dContext->FillRectangle(D2D1::RectF(8.0f * s, 8.0f * s, 8.0f * s + w, 8.0f * s + h), _bgBrush.Get());
  float y = 14.0f * s;
  const float x = 16.0f * s + leftInset;
  for (const auto& l : lines) {
    _d2dContext->DrawText(l.c_str(), UINT32(l.size()), _textFormat.Get(), D2D1::RectF(x, y, x + w, y + lineHeight),
                          _textBrush.Get());
    y += lineHeight;
  }
  HRESULT hr = _d2dContext->EndDraw();
  if (FAILED(hr)) {
    SPDLOG_LOGGER_ERROR(Log::gpu(), "Direct2D EndDraw failed: {}", Log::hrToString(hr));
  }
}

void Renderer::setHudScale(float scale) {
  _hudScale = std::clamp(scale, 0.5f, 4.0f);
  if (!_dwFactory) {
    return;
  }
  ComPtr<IDWriteTextFormat> format;
  if (HR_OK(_dwFactory->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, 14.0f * _hudScale, L"en-us", &format))) {
    _textFormat = format;
  }
}

void Renderer::bindUiTarget() {
  ID3D11RenderTargetView* rtv = _uiRtv.Get();
  _context->OMSetRenderTargets(1, &rtv, nullptr);
  const D3D11_VIEWPORT vp{0.0f, 0.0f, float(_width), float(_height), 0.0f, 1.0f};
  _context->RSSetViewports(1, &vp);
}

bool Renderer::present(bool vsync) {
  _depthValidForPick = true;
  HRESULT hr = _swapChain->Present(vsync ? 1 : 0, 0);
  pollInfoQueue();
  if (FAILED(hr)) {
    SPDLOG_LOGGER_ERROR(Log::gpu(), "Present failed: {}", Log::hrToString(hr));
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
      SPDLOG_LOGGER_ERROR(Log::gpu(), "Device removed reason: {}", Log::hrToString(_device->GetDeviceRemovedReason()));
    }
    return false;
  }
  return true;
}

bool Renderer::saveScreenshot(const std::wstring& path) {
  D3D11_TEXTURE2D_DESC desc{};
  _backBuffer->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (!HR_OK(_device->CreateTexture2D(&desc, nullptr, &staging))) {
    return false;
  }
  _context->CopyResource(staging.Get(), _backBuffer.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (!HR_OK(_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }
  const UINT w = desc.Width, h = desc.Height;
  std::vector<uint8_t> rgb(size_t(w) * h * 3);
  for (UINT y = 0; y < h; ++y) {
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
    uint8_t* dst = rgb.data() + size_t(y) * w * 3;
    for (UINT x = 0; x < w; ++x) {
      dst[x * 3 + 0] = src[x * 4 + 0]; // B
      dst[x * 3 + 1] = src[x * 4 + 1]; // G
      dst[x * 3 + 2] = src[x * 4 + 2]; // R
    }
  }
  _context->Unmap(staging.Get(), 0);
  const bool ok = writePng(path, w, h, rgb.data());
  if (ok) {
    LOG_I("Screenshot saved: {}", Log::toUtf8(path));
  } else {
    LOG_E("Screenshot failed: {}", Log::toUtf8(path));
  }
  return ok;
}

bool Renderer::writePng(const std::wstring& path, uint32_t w, uint32_t h, const uint8_t* bgr) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  return HR_OK(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
         HR_OK(wic->CreateStream(&stream)) && HR_OK(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
         HR_OK(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         HR_OK(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         HR_OK(encoder->CreateNewFrame(&frame, nullptr)) && HR_OK(frame->Initialize(nullptr)) &&
         HR_OK(frame->SetSize(w, h)) && HR_OK(frame->SetPixelFormat(&format)) &&
         HR_OK(frame->WritePixels(h, w * 3, w * h * 3, const_cast<BYTE*>(bgr))) && HR_OK(frame->Commit()) &&
         HR_OK(encoder->Commit());
}

std::optional<glm::dvec3> Renderer::pickWorld(int x, int y, const Camera& camera) {
  if (!_csPickDepth || !_pickUav || !_pickStaging || !_depthValidForPick || x < 0 || y < 0 || x >= _width ||
      y >= _height) {
    return std::nullopt;
  }
  // A (multisampled) depth buffer cannot be copied to a staging texture, so a
  // compute shader reads the pixel into a 1-float buffer. The depth buffer may
  // still be bound as the last frame's DSV: unbind it before reading it.
  _context->OMSetRenderTargets(0, nullptr, nullptr);
  DrawConstants dc{};
  dc.flags[0] = uint32_t(x);
  dc.flags[1] = uint32_t(y);
  setDrawConstants(dc);
  ID3D11Buffer* cb = _drawCb.Get();
  ID3D11ShaderResourceView* depth = _depthSrv.Get();
  ID3D11UnorderedAccessView* uav = _pickUav.Get();
  _context->CSSetShader(_csPickDepth.Get(), nullptr, 0);
  _context->CSSetConstantBuffers(1, 1, &cb);
  _context->CSSetShaderResources(1, 1, &depth);
  _context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  _context->Dispatch(1, 1, 1);
  ID3D11ShaderResourceView* noSrv = nullptr;
  ID3D11UnorderedAccessView* noUav = nullptr;
  _context->CSSetShaderResources(1, 1, &noSrv);
  _context->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
  _context->CopyResource(_pickStaging.Get(), _pickBuffer.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (!HR_OK(_context->Map(_pickStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return std::nullopt;
  }
  const float d = *static_cast<const float*>(mapped.pData);
  _context->Unmap(_pickStaging.Get(), 0);
  if (!(d > 0.0f)) {
    return std::nullopt;
  }
  const double viewZ = camera.nearPlane / double(d);
  const glm::dvec3 ray = camera.rayDirection(x, y, _width, _height);
  const double cosAngle = glm::dot(ray, camera.direction());
  if (cosAngle <= 1e-6) {
    return std::nullopt;
  }
  return camera.position + ray * (viewZ / cosAngle);
}
