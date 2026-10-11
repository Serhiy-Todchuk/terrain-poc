#include "pch.h"
#include "App.h"

#include "BuildingsLoader.h"
#include "CloudBlend.h"
#include "Ephemeris.h"
#include "GeoRef.h"
#include "Log.h"
#include "NetcdfInspect.h"
#include "Settings.h"
#include "TerrainLoader.h"

#include <Cesium3DTilesSelection/TilesetLoadFailureDetails.h>
#include <CesiumRasterOverlays/ActivatedRasterOverlay.h>
#include <CesiumRasterOverlays/RasterOverlayLoadFailureDetails.h>
#include <CesiumRasterOverlays/RasterOverlayTile.h>
#include <CesiumRasterOverlays/UrlTemplateRasterOverlay.h>

#include <rapidjson/document.h>

#include <psapi.h>

#include <cstdarg>
#include <fstream>
#include <sstream>

using namespace Cesium3DTilesSelection;
using CesiumUtility::Math;

namespace {

constexpr wchar_t kWindowClass[] = L"TerrainPOCWindow";
constexpr double kCloudFadeSeconds = 8.0; // cross-fade between two weather datasets

std::wstring wformat(const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return Log::toWide(buf);
}

std::vector<std::string> splitArgs(const std::string& line) {
  std::vector<std::string> out;
  std::stringstream ss(line);
  std::string tok;
  while (ss >> tok) {
    out.push_back(tok);
  }
  return out;
}

double megabytes(int64_t bytes) { return double(bytes) / (1024.0 * 1024.0); }

double processPrivateMB() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    return double(pmc.PrivateUsage) / (1024.0 * 1024.0);
  }
  return 0.0;
}

glm::dvec2 layerWind(const CloudFields& f, int layer) {
  return glm::dvec2(f.layers[size_t(layer)].windU, f.layers[size_t(layer)].windV);
}

// Offset (km) of a dataset's noise phase: a different pattern for every hour
// and layer, so the two phases of a time blend are independent and the phase
// of an hour is the same whichever pair it belongs to.
glm::dvec2 noiseSeed(int64_t validTime, int layer) {
  uint64_t h = uint64_t(validTime / 3600) * 3u + uint64_t(layer) + 0x9E3779B97F4A7C15ull; // splitmix64
  h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
  h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
  h ^= h >> 31;
  return glm::dvec2(double(h & 0xffffu) / 65535.0 * 800.0 - 400.0, double((h >> 16) & 0xffffu) / 65535.0 * 800.0 - 400.0);
}

} // namespace

App::App(AppConfig config) : _cfg(std::move(config)) {}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

int App::run() {
  if (!init()) {
    LOG_E("Initialization failed");
    shutdown();
    return 1;
  }
  auto last = std::chrono::steady_clock::now();
  while (_running) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        _running = false;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!_running) {
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::clamp(std::chrono::duration<double>(now - last).count(), 0.0, 0.25);
    last = now;
    frame(dt);
  }
  shutdown();
  return _exitCode;
}

bool App::init() {
  _start = std::chrono::steady_clock::now();
  _showTerrain = _cfg.terrain;
  _showBuildings = _cfg.buildings;
  _showVessels = _cfg.vessels;
  _showClouds = _cfg.clouds;
  _cloudAltitude = _cfg.cloudAltitude;
  _cloudTransparency = std::clamp(_cfg.cloudTransparency, 0.0, 1.0);
  _sunFromTime = _cfg.sunFromTime;
  _scene.water = _cfg.water;
  _scene.contours = _cfg.contours;
  _scene.wireframe = _cfg.wireframe;
  _scene.facades = _cfg.facades;
  _scene.fog = _cfg.fog;
  _scene.cloudShadows = _cfg.cloudShadows;
  _scene.imagery = _cfg.imagery != "none" && _cfg.imageryOn;
  _scene.waterAbsorption = _cfg.waterClarity;
  _scene.contourInterval = _cfg.contourInterval;
  for (int l = 0; l < kCloudLayerCount; ++l) {
    _cloudLayerOn[l] = _cfg.cloudLayers[l];
  }
  _cloudLevel = _cfg.cloudLevel;
  _flySpeed = _cfg.flySpeed;
  _historyHours = _cfg.historyHours;
  _panelTab = std::clamp(_cfg.panelTab, 0, 1);
  _panelTabRequest = _panelTab;
  for (int i = 0; i < kSectionCount; ++i) {
    _sectionOpen[i] = (_cfg.panelSections >> i) & 1;
  }

  // Fixed afternoon sun from the south-west (45 degrees elevation) at the POI
  // (without "Apply to sun").
  {
    const glm::dvec3 poi = GeoRef::toEcef(-74.0075, 40.66, 0.0);
    const glm::dmat4 enu = CesiumGeospatial::GlobeTransforms::eastNorthUpToFixedFrame(poi, GeoRef::ellipsoid());
    const double az = Math::degreesToRadians(215.0), el = Math::degreesToRadians(45.0);
    _fixedSun = glm::normalize(glm::dvec3(enu[0]) * (std::sin(az) * std::cos(el)) +
                               glm::dvec3(enu[1]) * (std::cos(az) * std::cos(el)) + glm::dvec3(enu[2]) * std::sin(el));
    _scene.sunDirectionEcef = _fixedSun;
  }

  if (!createWindow()) {
    return false;
  }
  RECT rc{};
  GetClientRect(_hwnd, &rc);
  if (!_renderer.init(_hwnd, rc.right - rc.left, rc.bottom - rc.top, _cfg.d3dDebug, Log::toWide(_cfg.shaderDir),
                      _cfg.msaa)) {
    return false;
  }
  // The settings panel is optional: without it the app still runs (keys work).
  _ui.init(_hwnd, _renderer.device(), _renderer.context());
  const UINT dpi = GetDpiForWindow(_hwnd);
  setUiScale(dpi > 0 ? float(dpi) / 96.0f : 1.0f);

  _camera.fovY = Math::degreesToRadians(_cfg.fovY);
  _camera.aspect = double(_renderer.width()) / double(_renderer.height());
  _homeView = _cfg.camera;
  _camera.apply(_cfg.camera);
  LOG_I("Camera: --camera {}", _camera.describe());

  if (!createTilesets()) {
    return false;
  }
  // Always created (three small meshes) so the panel / key 5 can show them later.
  _vessels.init(_renderer.device(), Vessels::defaultFleet());
  // Always created too; it requests nothing while the cloud layer is hidden.
  WeatherService::Options weatherOptions;
  weatherOptions.pinnedTime = _cfg.weatherTime;
  weatherOptions.offline = _cfg.offline;
  _weatherCenter = _cfg.weatherCenter;
  _weatherGrid = _weatherCenter ? CloudGrid::centredAt(_weatherCenter->first, _weatherCenter->second) : CloudGrid();
  weatherOptions.grid = _weatherGrid;
  _renderer.setCloudGrid(_weatherGrid);
  _weather = std::make_shared<WeatherService>(_http, *_asyncSystem, weatherOptions);
  if (_weatherCenter) {
    LOG_I("Weather box centred at {:.4f}, {:.4f} (--weather-center)", _weatherCenter->first, _weatherCenter->second);
  }
  updateSun();
  if (_sunFromTime) {
    LOG_I("Sun at the weather time (--apply-sun)");
  }

  if (!_cfg.screenshot.empty()) {
    _script.push_back({"wait", {}, 0});
    _script.push_back({"shot", {_cfg.screenshot}, 0});
    if (!_cfg.keepOpen) {
      _script.push_back({"quit", {}, 0});
    }
    _scriptMode = true;
  } else if (!_cfg.script.empty()) {
    if (!loadScript(_cfg.script)) {
      return false;
    }
    _scriptMode = true;
  }
  return true;
}

bool App::createWindow() {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &App::wndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.lpszClassName = kWindowClass;
  RegisterClassExW(&wc);

  const DWORD style = WS_OVERLAPPEDWINDOW;
  RECT r{0, 0, _cfg.width, _cfg.height};
  AdjustWindowRect(&r, style, FALSE);
  _hwnd = CreateWindowExW(0, kWindowClass, L"TerrainPOC - Cesium Native + D3D11 (NYC)", style, CW_USEDEFAULT,
                          CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, this);
  if (!_hwnd) {
    LOG_E("CreateWindowEx failed: {}", Log::win32ErrorToString(GetLastError()));
    return false;
  }
  ShowWindow(_hwnd, _cfg.hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
  UpdateWindow(_hwnd);
  return true;
}

bool App::createTilesets() {
  const unsigned hw = std::max(4u, std::thread::hardware_concurrency());
  _taskProcessor = std::make_shared<TaskProcessor>(hw > 4 ? hw - 2 : hw);
  _asyncSystem.emplace(_taskProcessor);

  HttpAssetAccessor::Options httpOptions;
  httpOptions.threads = _cfg.httpThreads;
  httpOptions.useCache = _cfg.useCache || _cfg.offline;
  httpOptions.offline = _cfg.offline;
  if (_cfg.offline) {
    LOG_I("OFFLINE mode: all data is served from the disk cache {}; nothing is downloaded", _cfg.cacheDir);
  }
  httpOptions.cacheDir = Log::toWide(_cfg.cacheDir);
  httpOptions.hostLimits = {{"elevation.nationalmap.gov", 8},
                            {"gis.ngdc.noaa.gov", 8},
                            {"server.arcgisonline.com", 12},
                            {"basemap.nationalmap.gov", 8},
                            {"tiles.openfreemap.org", 6},
                            {"nomads.ncep.noaa.gov", 2},  // weather; NOMADS blocks clients over ~120 req/min
                            {"noaa-goes19.s3.amazonaws.com", 4}}; // GOES-19 satellite products
  _http = std::make_shared<HttpAssetAccessor>(httpOptions);
  _resources = std::make_shared<RendererResources>(_renderer.device());
  _credits = std::make_shared<CesiumUtility::CreditSystem>();
  TilesetExternals externals{_http, _resources, *_asyncSystem, _credits, Log::cesium()};

  ElevationService::Options elevationOptions;
  elevationOptions.gridSize = _cfg.terrainGrid;
  _elevation = std::make_shared<ElevationService>(_http, elevationOptions);

  // ---- terrain (land + seabed) -----------------------------------------------
  TilesetOptions terrainOptions;
  terrainOptions.maximumScreenSpaceError = _cfg.terrainSse;
  terrainOptions.maximumSimultaneousTileLoads = 24;
  terrainOptions.maximumCachedBytes = 384LL * 1024 * 1024;
  terrainOptions.enableOcclusionCulling = false;
  terrainOptions.preloadSiblings = false; // every tile costs 1-3 web requests
  terrainOptions.forbidHoles = true;      // keep parents until all children are ready
  terrainOptions.loadErrorCallback = [](const TilesetLoadFailureDetails& d) {
    SPDLOG_LOGGER_ERROR(Log::cesium(), "Terrain tileset load failure (HTTP {}): {}", d.statusCode, d.message);
  };
  TerrainOptions terrain;
  terrain.gridSize = _cfg.terrainGrid;
  terrain.maxLevel = _cfg.terrainMaxLevel;
  _terrain = TerrainLoader::createTileset(externals, terrainOptions, _elevation, terrain);
  LOG_I("Terrain tileset: geographic quadtree, {}x{} samples/tile, max level {} (~{:.2f} m spacing), SSE {}",
        terrain.gridSize, terrain.gridSize, terrain.maxLevel,
        180.0 / double(1ull << terrain.maxLevel) / (terrain.gridSize - 1) * 111320.0, _cfg.terrainSse);

  // ---- satellite imagery draped over the terrain ------------------------------
  std::string url;
  int maxLevel = 19;
  if (_cfg.imagery == "esri") {
    url = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{reverseY}/{x}";
    maxLevel = 19;
    _imageryName = "Esri World Imagery";
    _imageryCredit = "Esri, Vantor, Earthstar Geographics, GIS User Community";
  } else if (_cfg.imagery == "usgs") {
    url = "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/{z}/{reverseY}/{x}";
    maxLevel = 16;
    _imageryName = "USGS NAIP/HRO";
    _imageryCredit = "USGS The National Map";
  } else if (_cfg.imagery != "none") {
    LOG_E("Unknown imagery source '{}', using none", _cfg.imagery);
  }
  if (!url.empty()) {
    CesiumRasterOverlays::UrlTemplateRasterOverlayOptions urlOptions;
    urlOptions.maximumLevel = uint32_t(_cfg.imageryMaxLevel >= 0 ? _cfg.imageryMaxLevel : maxLevel);
    urlOptions.credit = _imageryCredit;
    CesiumRasterOverlays::RasterOverlayOptions overlayOptions;
    overlayOptions.maximumScreenSpaceError = _cfg.imagerySse;
    overlayOptions.maximumSimultaneousTileLoads = 24;
    overlayOptions.loadErrorCallback = [](const CesiumRasterOverlays::RasterOverlayLoadFailureDetails& d) {
      SPDLOG_LOGGER_ERROR(Log::cesium(), "Imagery overlay load failure: {} ({})", d.message,
                          d.pRequest ? d.pRequest->url() : std::string("no request"));
    };
    CesiumUtility::IntrusivePointer<CesiumRasterOverlays::UrlTemplateRasterOverlay> pOverlay =
        new CesiumRasterOverlays::UrlTemplateRasterOverlay("imagery", url, {}, urlOptions, overlayOptions);
    _terrain->getOverlays().add(pOverlay);
    LOG_I("Imagery: {} (max level {}) {}", _imageryName, urlOptions.maximumLevel, url);
  }

  // ---- OSM buildings ------------------------------------------------------------
  if (_cfg.buildings) {
    std::string tileUrl;
    auto pRequest = _http->get(*_asyncSystem, "https://tiles.openfreemap.org/planet").waitInMainThread();
    const CesiumAsync::IAssetResponse* pResponse = pRequest ? pRequest->response() : nullptr;
    if (pResponse && pResponse->statusCode() == 200) {
      rapidjson::Document doc;
      doc.Parse(reinterpret_cast<const char*>(pResponse->data().data()), pResponse->data().size());
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("tiles") && doc["tiles"].IsArray() &&
          doc["tiles"].Size() > 0 && doc["tiles"][0].IsString()) {
        tileUrl = doc["tiles"][0].GetString();
      }
    }
    if (tileUrl.empty()) {
      LOG_E("Could not read the OpenFreeMap TileJSON (status {}); buildings disabled",
            pResponse ? pResponse->statusCode() : 0);
    } else {
      LOG_I("Buildings: OpenStreetMap via OpenFreeMap vector tiles {}", tileUrl);
      BuildingsOptions buildings;
      buildings.tileUrlTemplate = tileUrl;
      TilesetOptions buildingOptions;
      buildingOptions.maximumScreenSpaceError = _cfg.buildingsSse;
      buildingOptions.maximumSimultaneousTileLoads = 12;
      buildingOptions.enableOcclusionCulling = false;
      buildingOptions.preloadSiblings = false;
      buildingOptions.maximumCachedBytes = 192LL * 1024 * 1024;
      buildingOptions.loadErrorCallback = [](const TilesetLoadFailureDetails& d) {
        SPDLOG_LOGGER_ERROR(Log::cesium(), "Buildings tileset load failure (HTTP {}): {}", d.statusCode, d.message);
      };
      _buildings = BuildingsLoader::createTileset(externals, buildingOptions, _elevation, buildings);
    }
  }
  return _terrain != nullptr;
}

void App::shutdown() {
  logStats("shutdown");
  if (_cfg.settingsActive && _renderer.device()) {
    std::string report;
    if (Settings::save(_cfg.settingsPath, currentSettings(), report)) {
      LOG_I("Settings: {}", report);
    } else {
      LOG_W("Settings: {}", report);
    }
  }
  if (_http) {
    _http->shutdown();
  }
  std::vector<CesiumAsync::SharedFuture<void>> pending;
  if (_terrain) {
    pending.push_back(_terrain->getAsyncDestructionCompleteEvent());
  }
  if (_buildings) {
    pending.push_back(_buildings->getAsyncDestructionCompleteEvent());
  }
  _terrain.reset();
  _buildings.reset();
  _weather.reset(); // pending weather continuations hold weak references only
  if (_asyncSystem) {
    const auto t0 = std::chrono::steady_clock::now();
    for (auto& f : pending) {
      while (!f.isReady()) {
        _asyncSystem->dispatchMainThreadTasks();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) {
          LOG_W("Timed out waiting for tileset destruction");
          break;
        }
      }
    }
  }
  _ui.shutdown();
  _renderer.shutdown();
  if (_hwnd) {
    DestroyWindow(_hwnd);
    _hwnd = nullptr;
  }
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

void App::frame(double dt) {
  _time += dt;
  ++_frameIndex;
  _fpsAccum += dt;
  ++_fpsFrames;
  if (_fpsAccum >= 0.5) {
    _fps = _fpsFrames / _fpsAccum;
    _fpsAccum = 0.0;
    _fpsFrames = 0;
  }

  // UI first: the panel's changes apply to this frame. Its draw data stays
  // valid until the next frame, so screenshots taken below include it.
  _ui.newFrame(!_scriptMode || _scriptUiInput);
  drawPanel();
  _ui.endFrame();
  if (_screenshotRequested) {
    _screenshotRequested = false;
    manualScreenshot();
  }

  flyKeys(dt);
  if (_scriptMode) {
    stepScript(dt);
  }

  _hoverTimer += dt;
  if (_hoverDirty && _drag == DragMode::None && _hoverTimer > 0.2) {
    _hoverPoint = _renderer.pickWorld(_hoverX, _hoverY, _camera);
    _hoverDirty = false;
    _hoverTimer = 0.0;
  }

  updateTilesets(dt);
  render();

  _statsTimer += dt;
  if (_statsTimer > 10.0) {
    _statsTimer = 0.0;
    logStats("periodic");
  }
}

void App::updateTilesets(double dt) {
  _asyncSystem->dispatchMainThreadTasks();
  updateSun();
  updateClouds();
  _camera.aspect = double(_renderer.width()) / double(_renderer.height());
  // Keep the near plane proportional to the height above the ground.
  _camera.nearPlane = std::clamp(_camera.altitudeMsl() * 0.002, 0.05, 50.0);
  const double vfov = _camera.fovY;
  const double hfov = 2.0 * std::atan(std::tan(vfov * 0.5) * _camera.aspect);
  std::vector<ViewState> frustums;
  frustums.emplace_back(_camera.position, _camera.direction(), _camera.up(),
                        glm::dvec2(_renderer.width(), _renderer.height()), hfov, vfov, GeoRef::ellipsoid());
  if (_terrain && _showTerrain) {
    _terrainResult = &_terrain->updateViewGroup(_terrain->getDefaultViewGroup(), frustums, float(dt));
    _terrain->loadTiles();
  }
  if (_buildings && _showBuildings) {
    _buildingsResult = &_buildings->updateViewGroup(_buildings->getDefaultViewGroup(), frustums, float(dt));
    _buildings->loadTiles();
  }
}

double App::weatherTime() const {
  if (const std::optional<int64_t> pinned = _weather ? _weather->pinnedTime() : std::nullopt) {
    return double(*pinned);
  }
  return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void App::setWeatherTime(std::optional<int64_t> pinned) {
  if (_weather) {
    _weather->setPinnedTime(pinned);
  }
  updateSun();
}

// HRRR covers the contiguous US; a box centred inside this range stays within
// the model domain (its edges curve on the Lambert grid, so keep a margin).
std::optional<std::pair<double, double>> App::weatherCenterAtCamera() const {
  const CesiumGeospatial::Cartographic c = _camera.cartographic();
  const double lon = Math::radiansToDegrees(c.longitude), lat = Math::radiansToDegrees(c.latitude);
  if (lon < -121.0 || lon > -71.0 || lat < 27.0 || lat > 47.0) {
    return std::nullopt;
  }
  return std::make_pair(lon, lat);
}

void App::setWeatherCenter(std::optional<std::pair<double, double>> lonLat, const char* reason) {
  const CloudGrid grid = lonLat ? CloudGrid::centredAt(lonLat->first, lonLat->second) : CloudGrid();
  if (grid == _weatherGrid) {
    return;
  }
  _weatherCenter = lonLat;
  _weatherGrid = grid;
  if (_weather) {
    _weather->setGrid(grid);
  }
  _renderer.setCloudGrid(grid);
  _cloudA = {};
  _cloudB = {};
  _cloudOldA = {};
  _cloudOldB = {};
  _cloudFadeStart = -1e9;
  _scene.clouds = false;
  if (lonLat) {
    LOG_I("{}: weather box centred at {:.4f}, {:.4f} (--weather-center {:.4f},{:.4f})", reason, lonLat->first,
          lonLat->second, lonLat->first, lonLat->second);
  } else {
    LOG_I("{}: weather box back to the default around New York (--weather-center default)", reason);
  }
}

void App::setCloudLevel(CloudLevel level, const char* reason) {
  if (level == _cloudLevel) {
    return;
  }
  _cloudLevel = level;
  LOG_I("{}: cloud quality {} (--cloud-level {})", reason, cloudLevelInfo(level).name, cloudLevelInfo(level).id);
}

void App::updateSun() {
  _scene.daylight = _sunFromTime;
  _scene.sunDirectionEcef = _sunFromTime ? Ephemeris::sunDirectionEcef(weatherTime()) : _fixedSun;
}

void App::updateClouds() {
  _scene.clouds = false;
  if (!_weather) {
    return;
  }
  _weather->update(_showClouds, _cloudLevel >= CloudLevel::Satellite);
  if (!_showClouds) {
    return;
  }
  // The two loaded datasets around the weather time (either may be missing).
  // A new pair is taken once the service has loaded what it needs, so a new
  // time appears in one step (one cross-fade; none for the first data).
  const double t = weatherTime();
  const WeatherService::Bracket bracket = _weather->bracket(int64_t(std::floor(t)));
  const std::shared_ptr<const CloudFields> a = bracket.before ? bracket.before : bracket.after;
  const std::shared_ptr<const CloudFields> b = bracket.before ? bracket.after : nullptr;
  if (a && (a != _cloudA.fields || b != _cloudB.fields) && !_weather->busy()) {
    auto slot = [&](const std::shared_ptr<const CloudFields>& f) -> CloudSlot {
      if (!f) {
        return {};
      }
      if (f == _cloudA.fields) {
        return _cloudA;
      }
      if (f == _cloudB.fields) {
        return _cloudB;
      }
      return {f, _renderer.createCloudTexture(*f)};
    };
    CloudSlot newA = slot(a), newB = slot(b);
    if (newA.texture && (!b || newB.texture)) {
      // Cross-fade from what was shown, except at the roll-over to the next
      // hour (the old b becomes a while it is shown alone) and for the first data.
      const bool rollOver = _cloudB.fields && a == _cloudB.fields && std::abs(t - double(a->validTime)) < 120.0;
      if (_cloudA.fields && !rollOver) {
        _renderer.snapshotClouds();
        _cloudFadeStart = _time;
        _cloudFadeSameHours = _cloudA.fields->validTime == a->validTime &&
                              (_cloudB.fields ? _cloudB.fields->validTime : 0) == (b ? b->validTime : 0);
        for (int l = 0; l < kCloudLayerCount; ++l) {
          _cloudFadeWind[0][l] = layerWind(*_cloudA.fields, l);
          _cloudFadeWind[1][l] = layerWind(_cloudB.fields ? *_cloudB.fields : *_cloudA.fields, l);
        }
        // The replaced datasets stay alive while their wind fields glide into the new ones.
        _cloudOldA = _cloudFadeSameHours ? _cloudA : CloudSlot{};
        _cloudOldB = _cloudFadeSameHours ? _cloudB : CloudSlot{};
      }
      _cloudA = std::move(newA);
      _cloudB = std::move(newB);
    }
  }
  if (!_cloudA.texture) {
    return;
  }

  // Time blend a -> b with motion-compensated drift: a moves forward from its
  // valid time with its wind field, b backward from its own; the noise phase of
  // each dataset moves the same way (CSBlendClouds, per cell; deterministic for
  // replays). Without b, a drifts for at most an hour.
  CloudFrame& cf = _scene.cloud;
  const double ta = double(_cloudA.fields->validTime);
  const double tb = _cloudB.fields ? double(_cloudB.fields->validTime) : ta;
  const double te = _cloudB.fields ? std::clamp(t, ta, tb) : std::clamp(t, ta - 3600.0, ta + 3600.0);
  const float fade = cloudFade();
  if (fade >= 1.0f) {
    _cloudOldA = {};
    _cloudOldB = {};
  }
  const bool glide = _cloudFadeSameHours && fade < 1.0f && _cloudOldA.texture; // newer run of the same hours
  cf.a = _cloudA.texture.get();
  cf.b = _cloudB.texture.get();
  cf.weightB = _cloudB.fields ? float((te - ta) / (tb - ta)) : 0.0f;
  cf.fade = fade;
  cf.driftA = float(te - ta);
  cf.driftB = _cloudB.fields ? float(te - tb) : cf.driftA;
  cf.oldA = glide ? _cloudOldA.texture.get() : nullptr;
  cf.oldB = glide ? (_cloudOldB.texture ? _cloudOldB.texture : _cloudOldA.texture).get() : nullptr;
  cf.glide = glide ? fade : 1.0f;
  for (int l = 0; l < kCloudLayerCount; ++l) {
    // The mean winds: the shadow map's change key (the drift of the shapes).
    glm::dvec2 windA = layerWind(*_cloudA.fields, l);
    glm::dvec2 windB = _cloudB.fields ? layerWind(*_cloudB.fields, l) : windA;
    if (glide) {
      windA = glm::mix(_cloudFadeWind[0][l], windA, double(fade));
      windB = glm::mix(_cloudFadeWind[1][l], windB, double(fade));
    }
    cf.meanDriftA[l] = glm::vec2(-windA * (te - ta) * 0.001);
    cf.meanDriftB[l] = _cloudB.fields ? glm::vec2(-windB * (te - tb) * 0.001) : cf.meanDriftA[l];
    cf.noiseA[l] = glm::vec2(noiseSeed(_cloudA.fields->validTime, l));
    cf.noiseB[l] = glm::vec2(noiseSeed(_cloudB.fields ? _cloudB.fields->validTime : _cloudA.fields->validTime, l));
    cf.layer[l] = _cloudLayerOn[l];
  }
  // Simple shell (the step-1 look): the single layer (slice 0) with static
  // shapes - no drift, one fixed noise pattern; the fields still blend in time
  // (without motion).
  cf.single = _cloudLevel == CloudLevel::Shell;
  if (cf.single) {
    cf.driftA = cf.driftB = 0.0f;
    for (int l = 0; l < kCloudLayerCount; ++l) {
      cf.meanDriftA[l] = cf.meanDriftB[l] = glm::vec2(0.0f);
      cf.noiseA[l] = cf.noiseB[l] = glm::vec2(0.0f);
      cf.layer[l] = l == 0;
    }
  }
  cf.manualAltitude = _cloudAltitude;
  cf.opacity = float(1.0 - _cloudTransparency);
  _scene.clouds = true;
}

float App::cloudFade() const {
  return float(std::clamp((_time - _cloudFadeStart) / kCloudFadeSeconds, 0.0, 1.0));
}

void App::drawFrame() {
  _scene.timeSeconds = _time;
  if (_scene.facades && _scene.imagery && _buildings && _showBuildings && _terrain && _showTerrain &&
      _terrainResult) {
    _renderer.updateRoofImagery(_camera, _terrainResult->tilesToRenderThisFrame);
  }
  _renderer.beginFrame(_camera, _scene);
  if (_terrain && _showTerrain && _terrainResult) {
    _renderer.drawTiles(_terrainResult->tilesToRenderThisFrame, true);
  }
  if (_buildings && _showBuildings && _buildingsResult) {
    _renderer.drawTiles(_buildingsResult->tilesToRenderThisFrame, false);
  }
  if (_showVessels) {
    _vessels.draw(_renderer);
  }
  _renderer.drawWater(_camera);
  _renderer.drawClouds();
  _renderer.resolveScene();
  if (_cfg.hud) {
    _renderer.drawHud(hudLines(), _hudInset);
  }
  _renderer.bindUiTarget();
  _ui.render();
}

void App::render() {
  drawFrame();
  _renderer.present(_cfg.vsync);
}

void App::setMsaa(bool on) {
  // Toggles between the configured sample count (4x if started with --msaa 1) and off.
  _renderer.setSampleCount(on ? (_cfg.msaa > 1 ? _cfg.msaa : 4) : 1);
}

void App::setUiScale(float scale) {
  _ui.setScale(scale);
  _renderer.setHudScale(_ui.scale());
  LOG_I("UI scale {:.2f} (HUD + settings panel)", _ui.scale());
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

LRESULT CALLBACK App::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    app = static_cast<App*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    app->_hwnd = hwnd;
  }
  if (app) {
    return app->onMessage(msg, wp, lp);
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::onMessage(UINT msg, WPARAM wp, LPARAM lp) {
  // Scripted harness runs must be deterministic: ignore live mouse/keyboard
  // input (except Esc) while a script is executing. Mouse-leave too: the
  // panel's mouse tracking must keep the position of an injected click.
  if (_scriptMode && !_injecting &&
      ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE || msg == WM_NCMOUSEMOVE ||
       msg == WM_NCMOUSELEAVE || ((msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR) && wp != VK_ESCAPE))) {
    return DefWindowProcW(_hwnd, msg, wp, lp);
  }
  // The settings panel sees every message first. Input it claims (mouse over /
  // dragging a widget, a text field or slider being edited) does not navigate;
  // a drag that started in the scene keeps going over the panel.
  if (const LRESULT handled = _ui.processMessage(_hwnd, msg, wp, lp)) {
    return handled; // cursor shape over a widget, IME characters
  }
  switch (msg) {
  case WM_CLOSE:
    _running = false;
    return 0;
  case WM_SIZE:
    if (_renderer.device() && wp != SIZE_MINIMIZED) {
      _renderer.resize(LOWORD(lp), HIWORD(lp));
    }
    return 0;
  case WM_DPICHANGED:
    setUiScale(float(HIWORD(wp)) / 96.0f);
    return DefWindowProcW(_hwnd, msg, wp, lp);
  case WM_LBUTTONDOWN:
  case WM_RBUTTONDOWN:
    if (!_ui.wantsMouse()) {
      mouseDown(msg == WM_LBUTTONDOWN ? DragMode::Orbit : DragMode::Pan, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    }
    return 0;
  case WM_LBUTTONUP:
  case WM_RBUTTONUP:
    if (_drag != DragMode::None) {
      mouseUp();
    }
    return 0;
  case WM_MOUSEMOVE:
    if (_drag != DragMode::None || !_ui.wantsMouse()) {
      mouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    }
    return 0;
  case WM_MOUSEWHEEL: {
    if (_ui.wantsMouse()) {
      return 0; // scrolls the panel
    }
    POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(_hwnd, &p);
    mouseWheel(p.x, p.y, double(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA);
    return 0;
  }
  case WM_KEYDOWN:
    if (wp < 256 && !_ui.wantsKeyboard()) {
      if (!_keys[wp]) {
        keyCommand(int(wp));
      }
      _keys[wp] = true;
    }
    return 0;
  case WM_KEYUP:
    if (wp < 256) {
      _keys[wp] = false;
    }
    return 0;
  case WM_KILLFOCUS:
    std::fill(std::begin(_keys), std::end(_keys), false);
    return 0;
  default:
    return DefWindowProcW(_hwnd, msg, wp, lp);
  }
}

glm::dvec3 App::fallbackPivot(int x, int y) const {
  const glm::dvec3 ray = _camera.rayDirection(x, y, _renderer.width(), _renderer.height());
  const auto c = _camera.cartographic();
  const double seaLevel = GeoRef::seaSurfaceEllipsoidHeight(Math::radiansToDegrees(c.longitude),
                                                            Math::radiansToDegrees(c.latitude));
  if (auto hit = intersectEllipsoid(_camera.position, ray, seaLevel)) {
    return *hit;
  }
  return _camera.position + ray * std::max(_camera.altitudeMsl(), 100.0);
}

void App::mouseDown(DragMode mode, int x, int y) {
  _drag = mode;
  _lastX = x;
  _lastY = y;
  std::optional<glm::dvec3> hit = _renderer.pickWorld(x, y, _camera);
  if (mode == DragMode::Orbit) {
    _pivot = hit ? *hit : fallbackPivot(x, y);
  } else {
    _panAnchor = hit ? *hit : fallbackPivot(x, y);
    _panNormal = GeoRef::ellipsoid().geodeticSurfaceNormal(_panAnchor);
  }
  if (_hwnd) {
    SetCapture(_hwnd);
  }
}

void App::mouseMove(int x, int y) {
  const int dx = x - _lastX, dy = y - _lastY;
  _lastX = x;
  _lastY = y;
  if (_drag == DragMode::Orbit) {
    const double k = Math::degreesToRadians(0.25);
    _camera.orbit(_pivot, dx * k, -dy * k);
  } else if (_drag == DragMode::Pan) {
    const glm::dvec3 ray = _camera.rayDirection(x, y, _renderer.width(), _renderer.height());
    const double denom = glm::dot(ray, _panNormal);
    if (denom < -1e-4) {
      const double t = glm::dot(_panAnchor - _camera.position, _panNormal) / denom;
      if (t > 0.0) {
        const glm::dvec3 q = _camera.position + ray * t;
        glm::dvec3 delta = _panAnchor - q;
        const double maxStep = 0.5 * glm::length(_panAnchor - _camera.position) + 10.0;
        const double len = glm::length(delta);
        if (len > maxStep) {
          delta *= maxStep / len;
        }
        _camera.translate(delta);
      }
    }
  } else {
    _hoverX = x;
    _hoverY = y;
    _hoverDirty = true;
  }
}

void App::mouseUp() {
  _drag = DragMode::None;
  ReleaseCapture();
}

void App::mouseWheel(int x, int y, double notches) {
  std::optional<glm::dvec3> hit = _renderer.pickWorld(x, y, _camera);
  const glm::dvec3 target = hit ? *hit : fallbackPivot(x, y);
  const double factor = std::pow(0.8, notches);
  if (factor > 1.0 && _camera.altitudeMsl() > 2.0e7) {
    return;
  }
  _camera.moveTowards(target, factor, 1.0);
}

void App::resetView() {
  _camera.apply(_homeView);
  LOG_I("View reset: --camera {}", _camera.describe());
}

void App::keyCommand(int vk) {
  switch (vk) {
  case '1': _showTerrain = !_showTerrain; break;
  case '2': _scene.imagery = !_scene.imagery; break;
  case '3': _showBuildings = !_showBuildings; break;
  case '4': _scene.water = !_scene.water; break;
  case '5': _showVessels = !_showVessels; break;
  case '6': _showClouds = !_showClouds; break;
  case '7': _scene.cloudShadows = !_scene.cloudShadows; break;
  case '8': setCloudLevel(CloudLevel((int(_cloudLevel) + 1) % kCloudLevelCount), "key 8"); break;
  case 'L': _sunFromTime = !_sunFromTime; updateSun(); break;
  case 'O': _scene.fog = !_scene.fog; break;
  case 'C': _scene.contours = !_scene.contours; break;
  case 'F': _scene.wireframe = !_scene.wireframe; break;
  case 'M': setMsaa(_renderer.sampleCount() == 1); break;
  case 'T': _scene.facades = !_scene.facades; break;
  case 'H': _cfg.hud = !_cfg.hud; break;
  case 'G': _cfg.panel = !_cfg.panel; break;
  case 'R': resetView(); break;
  case 'P': manualScreenshot(); break;
  case VK_F5:
    _renderer.reloadShaders();
    break;
  case VK_ESCAPE:
    _running = false;
    break;
  default:
    break;
  }
}

void App::flyKeys(double dt) {
  if (GetForegroundWindow() != _hwnd && !_scriptMode) {
    return;
  }
  const bool fast = _keys[VK_SHIFT];
  const double speed = std::max(_camera.altitudeMsl(), 5.0) * (fast ? 2.5 : 0.7) * _flySpeed;
  const glm::dmat4 enu = _camera.enuFrame();
  const glm::dvec3 e(enu[0]), n(enu[1]), u(enu[2]);
  const glm::dvec3 forward = e * std::sin(_camera.heading) + n * std::cos(_camera.heading);
  const glm::dvec3 right = e * std::cos(_camera.heading) - n * std::sin(_camera.heading);
  glm::dvec3 move(0.0);
  if (_keys['W']) move += forward;
  if (_keys['S']) move -= forward;
  if (_keys['D']) move += right;
  if (_keys['A']) move -= right;
  if (_keys['E']) move += u;
  if (_keys['Q']) move -= u;
  if (glm::length(move) > 0.0) {
    _camera.translate(glm::normalize(move) * speed * dt);
  }
  const double turn = Math::degreesToRadians(60.0) * dt;
  if (_keys[VK_LEFT]) _camera.heading -= turn;
  if (_keys[VK_RIGHT]) _camera.heading += turn;
  if (_keys[VK_UP]) _camera.pitch = std::min(_camera.pitch + turn, Math::degreesToRadians(89.0));
  if (_keys[VK_DOWN]) _camera.pitch = std::max(_camera.pitch - turn, Math::degreesToRadians(-89.0));
}

// ---------------------------------------------------------------------------
// harness
// ---------------------------------------------------------------------------

bool App::loadScript(const std::string& path) {
  std::ifstream f(path);
  if (!f) {
    LOG_E("Cannot open script {}", path);
    return false;
  }
  std::string line;
  int lineNo = 0;
  while (std::getline(f, line)) {
    ++lineNo;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    std::vector<std::string> args = splitArgs(line);
    if (args.empty()) {
      continue;
    }
    ScriptCommand cmd;
    cmd.verb = args[0];
    cmd.args.assign(args.begin() + 1, args.end());
    cmd.line = lineNo;
    _script.push_back(std::move(cmd));
  }
  LOG_I("Loaded script {} ({} commands)", path, _script.size());
  return true;
}

void App::injectMessage(UINT msg, WPARAM wp, LPARAM lp) {
  _injecting = true;
  onMessage(msg, wp, lp);
  _injecting = false;
}

bool App::computeIdle() {
  // Clouds count as loaded once the weather check is done and a dataset switch
  // has finished cross-fading.
  if (_showClouds && _weather && (_weather->busy() || cloudFade() < 1.0f)) {
    return false;
  }
  const HttpAssetAccessor::Stats s = _http->stats();
  if (s.inFlight > 0 || s.queued > 0) {
    return false;
  }
  if (_taskProcessor->activeTasks() > 0 || _taskProcessor->queuedTasks() > 0) {
    return false;
  }
  auto tilesetIdle = [](const Tileset* pTileset, const ViewUpdateResult* pResult) {
    if (!pTileset || !pResult) {
      return true;
    }
    if (pResult->workerThreadTileLoadQueueLength > 0 || pResult->mainThreadTileLoadQueueLength > 0) {
      return false;
    }
    for (const auto& pActivated : pTileset->getOverlays().getActivatedOverlays()) {
      if (pActivated->getNumberOfTilesLoading() > 0) {
        return false;
      }
    }
    for (const Tile::ConstPointer& pTile : pResult->tilesToRenderThisFrame) {
      for (const RasterMappedTo3DTile& mapped : pTile->getMappedRasterTiles()) {
        if (mapped.getLoadingTile() != nullptr) {
          return false;
        }
      }
    }
    return true;
  };
  return tilesetIdle(_showTerrain ? _terrain.get() : nullptr, _terrainResult) &&
         tilesetIdle(_showBuildings ? _buildings.get() : nullptr, _buildingsResult);
}

void App::takeScreenshot(const std::string& path) {
  // Render a fresh frame so the screenshot reflects the current state, then
  // read back the back buffer before presenting.
  drawFrame();
  _renderer.saveScreenshot(Log::toWide(path));
  _renderer.present(false);
  logStats(("screenshot " + path).c_str());
}

void App::manualScreenshot() {
  char name[64];
  snprintf(name, sizeof(name), "manual_%lld.png", static_cast<long long>(_frameIndex));
  _lastScreenshot = (std::filesystem::path(_cfg.shotDir) / name).string();
  takeScreenshot(_lastScreenshot);
}

bool App::setOption(const std::string& name, bool value) {
  if (name == "terrain") _showTerrain = value;
  else if (name == "buildings") _showBuildings = value;
  else if (name == "vessels") _showVessels = value;
  else if (name == "clouds") _showClouds = value;
  else if (name == "shadows") _scene.cloudShadows = value;
  else if (name == "cloudlow") _cloudLayerOn[0] = value;
  else if (name == "cloudmid") _cloudLayerOn[1] = value;
  else if (name == "cloudhigh") _cloudLayerOn[2] = value;
  else if (name == "sun") { _sunFromTime = value; updateSun(); }
  else if (name == "fog") _scene.fog = value;
  else if (name == "water") _scene.water = value;
  else if (name == "imagery") _scene.imagery = value;
  else if (name == "contours") _scene.contours = value;
  else if (name == "wireframe") _scene.wireframe = value;
  else if (name == "msaa") setMsaa(value);
  else if (name == "facades") _scene.facades = value;
  else if (name == "hud") _cfg.hud = value;
  else if (name == "panel") _cfg.panel = value;
  else if (name == "vsync") _cfg.vsync = value;
  else if (name == "sections") _panelSections = value; // one-shot: expand / collapse all panel sections
  else return false;
  return true;
}

void App::stepScript(double dt) {
  if (_scriptPos >= _script.size()) {
    if (!_cfg.keepOpen) {
      _running = false;
    }
    _scriptMode = false;
    return;
  }
  ScriptCommand& c = _script[_scriptPos];
  if (!_cmdStarted) {
    _cmdStarted = true;
    _cmdTime = 0.0;
    _cmdStep = 0;
    _idleFrames = 0;
    std::string joined;
    for (const auto& a : c.args) {
      joined += " " + a;
    }
    LOG_I("script[{}] {}{}", c.line, c.verb, joined);
  } else {
    _cmdTime += dt;
  }

  auto arg = [&](size_t i, const std::string& fallback = "") { return i < c.args.size() ? c.args[i] : fallback; };
  auto num = [&](size_t i, double fallback) {
    try {
      return i < c.args.size() ? std::stod(c.args[i]) : fallback;
    } catch (...) {
      return fallback;
    }
  };
  std::string joinedArgs;
  for (const auto& a : c.args) {
    joinedArgs += (joinedArgs.empty() ? "" : ",") + a;
  }

  bool done = true;
  if (c.verb == "camera" || c.verb == "lookat") {
    CameraSpec spec;
    const auto mode = c.verb == "camera" ? CameraSpec::Mode::Eye : CameraSpec::Mode::LookAt;
    if (parseCameraSpec(joinedArgs, mode, spec)) {
      _camera.apply(spec);
      LOG_I("Camera now --camera {}", _camera.describe());
    } else {
      LOG_E("script[{}]: bad {} arguments", c.line, c.verb);
    }
  } else if (c.verb == "wait") {
    const double timeout = num(0, _cfg.loadTimeoutSec);
    _idleFrames = computeIdle() ? _idleFrames + 1 : 0;
    done = (_idleFrames >= 10 && _cmdTime > 0.5) || _cmdTime > timeout;
    if (done) {
      if (_idleFrames >= 10) {
        LOG_I("All tiles loaded after {:.1f} s", _cmdTime);
      } else {
        LOG_W("wait timed out after {:.0f} s (still loading)", _cmdTime);
        _exitCode = 3;
      }
    }
  } else if (c.verb == "sleep") {
    done = _cmdTime >= num(0, 1.0);
  } else if (c.verb == "fly") {
    // fly lon,lat,alt,heading,pitch seconds : smooth eye-space interpolation,
    // used to test streaming while the camera moves.
    const double duration = std::max(0.1, num(1, 5.0));
    if (_cmdStep == 0) {
      const auto carto = _camera.cartographic();
      _flyFrom.mode = CameraSpec::Mode::Eye;
      _flyFrom.lon = Math::radiansToDegrees(carto.longitude);
      _flyFrom.lat = Math::radiansToDegrees(carto.latitude);
      _flyFrom.alt = _camera.altitudeMsl();
      _flyFrom.heading = Math::radiansToDegrees(_camera.heading);
      _flyFrom.pitch = Math::radiansToDegrees(_camera.pitch);
      if (!parseCameraSpec(arg(0), CameraSpec::Mode::Eye, _flyTo)) {
        LOG_E("script[{}]: fly expects lon,lat,alt,heading,pitch seconds", c.line);
        _flyTo = _flyFrom;
      }
    }
    ++_cmdStep;
    const double t = std::clamp(_cmdTime / duration, 0.0, 1.0);
    const double s = t * t * (3.0 - 2.0 * t);
    CameraSpec spec = _flyFrom;
    spec.lon = glm::mix(_flyFrom.lon, _flyTo.lon, s);
    spec.lat = glm::mix(_flyFrom.lat, _flyTo.lat, s);
    spec.alt = glm::mix(_flyFrom.alt, _flyTo.alt, s);
    const double dh = std::remainder(_flyTo.heading - _flyFrom.heading, 360.0);
    spec.heading = _flyFrom.heading + dh * s;
    spec.pitch = glm::mix(_flyFrom.pitch, _flyTo.pitch, s);
    _camera.apply(spec);
    done = t >= 1.0;
  } else if (c.verb == "size") {
    int w = 0, h = 0;
    if (sscanf_s(arg(0).c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      RECT r{0, 0, w, h};
      AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
      SetWindowPos(_hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
      LOG_I("Window resized to {}x{} (renderer {}x{})", w, h, _renderer.width(), _renderer.height());
    } else {
      LOG_E("script[{}]: size expects WxH", c.line);
    }
  } else if (c.verb == "shot") {
    std::filesystem::path p = arg(0, "shot");
    if (!p.has_extension()) {
      p += ".png";
    }
    if (p.is_relative() && c.line > 0) {
      p = std::filesystem::path(_cfg.shotDir) / p;
    }
    takeScreenshot(p.string());
  } else if (c.verb == "drag") {
    const DragMode mode = arg(0) == "right" ? DragMode::Pan : DragMode::Orbit;
    const int x0 = int(num(1, 0)), y0 = int(num(2, 0)), x1 = int(num(3, 0)), y1 = int(num(4, 0));
    const int steps = std::max(1, int(num(5, 20)));
    if (_cmdStep == 0) {
      mouseDown(mode, x0, y0);
    } else if (_cmdStep <= steps) {
      const double t = double(_cmdStep) / steps;
      mouseMove(int(std::lround(x0 + (x1 - x0) * t)), int(std::lround(y0 + (y1 - y0) * t)));
    } else {
      mouseUp();
      LOG_I("After drag: --camera {}", _camera.describe());
    }
    done = _cmdStep > steps;
    ++_cmdStep;
  } else if (c.verb == "wheel") {
    mouseWheel(int(num(0, 0)), int(num(1, 0)), num(2, 1));
    LOG_I("After wheel: --camera {}", _camera.describe());
  } else if (c.verb == "click" || c.verb == "scroll") {
    // click x y / scroll x y notches as window messages, one per frame: the
    // settings panel gets them first; the scene only what the panel leaves.
    _scriptUiInput = true;
    const int x = int(num(0, 0)), y = int(num(1, 0));
    const LPARAM pos = MAKELPARAM(x, y);
    if (_cmdStep == 0) {
      injectMessage(WM_MOUSEMOVE, 0, pos);
    } else if (_cmdStep == 1 && c.verb == "click") {
      injectMessage(WM_LBUTTONDOWN, MK_LBUTTON, pos);
    } else if (_cmdStep == 1) {
      POINT screen{x, y};
      ClientToScreen(_hwnd, &screen);
      const short delta = short(num(2, 1) * WHEEL_DELTA);
      injectMessage(WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(delta)), MAKELPARAM(screen.x, screen.y));
    } else if (_cmdStep == 2 && c.verb == "click") {
      injectMessage(WM_LBUTTONUP, 0, pos);
    }
    done = _cmdStep >= 3; // one more frame: the panel acts on the release
    if (done) {
      LOG_I("After {}: --camera {}", c.verb, _camera.describe());
    }
    ++_cmdStep;
  } else if (c.verb == "type") {
    // type text | type <enter> : key + character messages, one key per frame
    // (a focused panel text field takes them; otherwise they are shortcuts).
    _scriptUiInput = true;
    std::string text;
    for (const auto& a : c.args) {
      text += (text.empty() ? "" : " ") + a;
    }
    const bool enter = text == "<enter>";
    const size_t count = enter ? 1 : text.size();
    if (size_t(_cmdStep) < count) {
      const char ch = enter ? '\r' : text[size_t(_cmdStep)];
      const WPARAM vk = enter ? VK_RETURN : WPARAM(LOBYTE(VkKeyScanA(ch)));
      injectMessage(WM_KEYDOWN, vk, 1);
      if (!enter) {
        injectMessage(WM_CHAR, WPARAM(static_cast<unsigned char>(ch)), 1);
      }
      injectMessage(WM_KEYUP, vk, LPARAM(0xC0000001));
    }
    done = size_t(_cmdStep) >= count;
    ++_cmdStep;
  } else if (c.verb == "key") {
    const std::string k = arg(0);
    int vk = 0;
    if (k.size() == 1) vk = std::toupper(static_cast<unsigned char>(k[0]));
    else if (k == "F5") vk = VK_F5;
    else if (k == "ESC") vk = VK_ESCAPE;
    if (vk) keyCommand(vk);
  } else if (c.verb == "set") {
    if (!setOption(arg(0), arg(1) == "on" || arg(1) == "1" || arg(1) == "true")) {
      LOG_E("script[{}]: unknown option {}", c.line, arg(0));
    }
  } else if (c.verb == "cloudtransparency") {
    // cloudtransparency PCT : the drawn clouds' transparency, 0..100 %
    char* end = nullptr;
    const double pct = std::strtod(arg(0).c_str(), &end);
    if (arg(0).empty() || *end != '\0' || pct < 0.0 || pct > 100.0) {
      LOG_E("script[{}]: cloudtransparency expects a percentage 0..100", c.line);
    } else {
      _cloudTransparency = pct / 100.0;
      LOG_I("script cloudtransparency: cloud transparency {:.0f} % (--cloud-transparency {:.0f})", pct, pct);
    }
  } else if (c.verb == "cloudlevel") {
    // cloudlevel shell | layers | satellite | N : the cloud quality level
    if (const std::optional<CloudLevel> level = parseCloudLevel(arg(0))) {
      setCloudLevel(*level, "script cloudlevel");
    } else {
      LOG_E("script[{}]: cloudlevel expects shell, layers or satellite", c.line);
    }
  } else if (c.verb == "tab") {
    // tab view | weather : select a settings-panel tab
    if (arg(0) == "view" || arg(0) == "weather") {
      _panelTabRequest = arg(0) == "view" ? 0 : 1;
    } else {
      LOG_E("script[{}]: tab expects view or weather", c.line);
    }
  } else if (c.verb == "weather") {
    // weather live | YYYYMMDDHH[MM] | -N : the clock, a fixed UTC time or N hours ago (replay / forecast)
    // weather here | center lon,lat | center default : the weather box ("Fetch weather here")
    if (arg(0) == "live") {
      setWeatherTime(std::nullopt);
      LOG_I("Weather time live");
    } else if (arg(0) == "here") {
      if (const auto here = weatherCenterAtCamera()) {
        setWeatherCenter(here, "script weather here");
      } else {
        LOG_E("script[{}]: weather here: the camera is outside the HRRR domain (contiguous US)", c.line);
      }
    } else if (arg(0) == "center" && arg(1) == "default") {
      setWeatherCenter(std::nullopt, "script weather center");
    } else if (arg(0) == "center") {
      std::vector<std::string> parts;
      std::stringstream ss(arg(1));
      std::string part;
      while (std::getline(ss, part, ',')) {
        parts.push_back(part);
      }
      try {
        if (parts.size() != 2) {
          throw std::invalid_argument("two numbers");
        }
        setWeatherCenter(std::make_pair(std::stod(parts[0]), std::stod(parts[1])), "script weather center");
      } catch (const std::exception&) {
        LOG_E("script[{}]: weather center expects lon,lat or default", c.line);
      }
    } else if (const std::optional<int64_t> t = WeatherService::parseTime(arg(0))) {
      setWeatherTime(t);
      LOG_I("Weather time {} (--weather-time {})", WeatherService::formatTime(*t), WeatherService::formatCompact(*t));
    } else {
      LOG_E("script[{}]: weather expects live, YYYYMMDDHH[MM] (UTC), -N (hours ago), here or center lon,lat|default",
            c.line);
    }
  } else if (c.verb == "netcdf") {
    // netcdf <url|path> [variable ...] : log a NetCDF-4 file (HDF5 reader); PNGs
    // of the named 2D variables in the shot dir. `wait` covers the download.
    if (c.args.empty()) {
      LOG_E("script[{}]: netcdf expects a URL or a file path (+ variables to draw)", c.line);
    } else {
      inspectNetcdf(_http, *_asyncSystem, arg(0), std::vector<std::string>(c.args.begin() + 1, c.args.end()),
                    _cfg.shotDir);
    }
  } else if (c.verb == "goes") {
    // goes <url|path> ... : the GOES-R L2 files of one scan on the weather box
    // (navigation + parallax checks, grid PNGs in the shot dir, compared with the
    // loaded HRRR datasets around the weather time). `wait` covers it.
    // goes (no arguments): PNGs of the scan the weather service has loaded for
    // the weather time (satellite level).
    if (!_weather) {
      LOG_E("script[{}]: goes: no weather service", c.line);
    } else if (c.args.empty()) {
      if (const std::shared_ptr<const GoesScan> scan = _weather->goesScan(int64_t(std::floor(weatherTime())))) {
        writeGoesScanImages(*_asyncSystem, scan, _cfg.shotDir);
      } else {
        LOG_E("script[{}]: goes: no GOES scan loaded for the weather time (cloudlevel satellite, then wait)", c.line);
      }
    } else {
      const WeatherService::Bracket bracket = _weather->modelBracket(int64_t(std::floor(weatherTime())));
      inspectGoesScan(_http, *_asyncSystem, c.args, _weather->grid(), {bracket.before, bracket.after}, _cfg.shotDir);
    }
  } else if (c.verb == "cloudfields") {
    // cloudfields : PNGs of the datasets the clouds show (per layer cover, base,
    // top, optical depth), HRRR or corrected by the GOES-19 scan. `wait` covers it.
    if (!_cloudA.fields) {
      LOG_E("script[{}]: cloudfields: no cloud dataset shown (clouds on, then wait)", c.line);
    } else {
      for (const std::shared_ptr<const CloudFields>& f : {_cloudA.fields, _cloudB.fields}) {
        if (f) {
          writeCloudFieldImages(*_asyncSystem, f, _cfg.shotDir);
        }
      }
    }
  } else if (c.verb == "log") {
    LOG_I("script note: {}", joinedArgs);
  } else if (c.verb == "stats") {
    logStats("script");
  } else if (c.verb == "quit") {
    _running = false;
  } else {
    LOG_E("script[{}]: unknown command '{}'", c.line, c.verb);
  }

  if (done) {
    ++_scriptPos;
    _cmdStarted = false;
  }
}

AppConfig App::currentSettings() const {
  AppConfig c = _cfg;
  c.width = _renderer.width();
  c.height = _renderer.height();
  c.terrain = _showTerrain;
  c.imageryOn = _imageryName.empty() ? _cfg.imageryOn : _scene.imagery;
  c.buildings = _showBuildings;
  c.water = _scene.water;
  c.vessels = _showVessels;
  c.clouds = _showClouds;
  c.contours = _scene.contours;
  c.contourInterval = _scene.contourInterval;
  c.sunFromTime = _sunFromTime;
  c.cloudShadows = _scene.cloudShadows;
  for (int l = 0; l < kCloudLayerCount; ++l) {
    c.cloudLayers[l] = _cloudLayerOn[l];
  }
  c.cloudLevel = _cloudLevel;
  c.cloudTransparency = _cloudTransparency;
  c.historyHours = _historyHours;
  c.weatherCenter = _weatherCenter;
  c.facades = _scene.facades;
  c.fog = _scene.fog;
  c.wireframe = _scene.wireframe;
  c.msaa = _renderer.sampleCount();
  c.waterClarity = _scene.waterAbsorption;
  c.panelTab = _panelTab;
  c.panelSections = 0;
  for (int i = 0; i < kSectionCount; ++i) {
    c.panelSections |= (_sectionOpen[i] ? 1 : 0) << i;
  }
  c.fovY = Math::radiansToDegrees(_camera.fovY);
  c.flySpeed = _flySpeed;
  return c;
}

std::vector<std::wstring> App::hudLines() {
  std::vector<std::wstring> lines;
  const auto c = _camera.cartographic();
  const HttpAssetAccessor::Stats hs = _http->stats();
  const RendererResources::Stats rs = _resources->stats();
  const DrawStats& ds = _renderer.stats();

  lines.push_back(wformat("TerrainPOC  Cesium Native 0.65 + Direct3D 11   %.0f fps", _fps));
  lines.push_back(wformat("camera %s   (lon,lat,alt MSL,heading,pitch)", _camera.describe().c_str()));
  if (_hoverPoint) {
    const auto hc = GeoRef::ellipsoid().cartesianToCartographic(*_hoverPoint);
    if (hc) {
      const double lon = Math::radiansToDegrees(hc->longitude), lat = Math::radiansToDegrees(hc->latitude);
      lines.push_back(wformat("cursor %.6f, %.6f   elevation %.2f m NAVD88 (%.2f m ellipsoid)   %.0f m away", lon,
                              lat, hc->height - GeoRef::geoidHeight(lon, lat), hc->height,
                              glm::length(*_hoverPoint - _camera.position)));
    }
  }
  if (_terrainResult) {
    lines.push_back(wformat("terrain   %3zu tiles  depth %2u  load queue %d/%d  %s",
                            _terrainResult->tilesToRenderThisFrame.size(), _terrainResult->maxDepthVisited,
                            _terrainResult->workerThreadTileLoadQueueLength,
                            _terrainResult->mainThreadTileLoadQueueLength,
                            ds.tilesWithoutImagery ? "(imagery loading)" : ""));
  }
  if (_buildingsResult && _buildings) {
    lines.push_back(wformat("buildings %3zu tiles  load queue %d", _buildingsResult->tilesToRenderThisFrame.size(),
                            _buildingsResult->workerThreadTileLoadQueueLength));
  }
  for (std::wstring& line : weatherHudLines()) {
    lines.push_back(std::move(line));
  }
  if (_cfg.offline) {
    lines.push_back(wformat("OFFLINE  %lld requests served from disk cache, %lld not cached", hs.cacheHits,
                            hs.offlineMisses));
  } else {
    lines.push_back(wformat("http %lld req (%lld cached)  %d active  %d queued  %lld errors  %.1f MB", hs.requests,
                            hs.cacheHits, hs.inFlight, hs.queued, hs.httpErrors + hs.transportFailures,
                            megabytes(hs.bytesDownloaded)));
  }
  lines.push_back(wformat("gpu  %lld meshes %.0f MB, %lld images %.0f MB, %d draws, %.2f M tris", rs.tiles,
                          megabytes(rs.tileBytes), rs.rasters, megabytes(rs.rasterBytes), ds.drawCalls,
                          double(ds.triangles) / 1e6));
  lines.push_back(wformat("log  %lld errors, %lld warnings   process memory %.0f MB", Log::errorCount(),
                          Log::warningCount(), processPrivateMB()));
  if (!_cfg.panel) {
    lines.push_back(L"G settings panel (layers, rendering, camera views) | H hide this HUD");
  }
  // Credits on two short lines: keeps the HUD clear of the settings panel.
  lines.push_back(wformat("Elevation: USGS 3DEP, NOAA BAG + NCEI | Buildings: (c) OpenStreetMap | Weather: NOAA HRRR%s",
                          _cloudLevel >= CloudLevel::Satellite ? " + GOES-19" : ""));
  lines.push_back(wformat("Imagery: %s", _imageryName.empty() ? "none" : _imageryCredit.c_str()));
  return lines;
}

std::vector<std::wstring> App::weatherHudLines() const {
  std::vector<std::wstring> lines;
  if (!_weather) {
    return lines;
  }
  const int64_t t = int64_t(std::floor(weatherTime()));
  const int offset = Ephemeris::newYorkUtcOffsetHours(t);
  double elevation = 0.0, azimuth = 0.0;
  Ephemeris::elevationAzimuth(_scene.sunDirectionEcef, -74.0075, 40.66, elevation, azimuth);
  lines.push_back(wformat("time      %s = %s %s New York, %s   sun %.0f° up, %.0f° az %s",
                          WeatherService::formatTime(t).c_str(),
                          WeatherService::formatTime(t + int64_t(offset) * 3600).substr(11, 5).c_str(),
                          offset == -4 ? "EDT" : "EST", _weather->live() ? "live" : "replay", elevation, azimuth,
                          _sunFromTime ? "(at this time)" : "(fixed)"));
  if (!_showClouds) {
    lines.push_back(L"clouds    off");
    return lines;
  }
  const std::string activity = _weather->activity();
  // Step 3: the GOES-19 scan loaded for the weather time and whether the shown
  // datasets carry it (WeatherService::bracket corrects them once it is loaded).
  auto goesLine = [&]() {
    if (_cloudLevel < CloudLevel::Satellite) {
      return;
    }
    const std::shared_ptr<const GoesScan> scan = _weather->goesScan(t);
    if (!scan) {
      lines.push_back(L"goes      no GOES-19 scan for this time yet");
      return;
    }
    const GoesScan& s = *scan;
    std::string text = WeatherService::formatTime(s.start).substr(11, 5) + " UTC, " +
                       std::to_string((t - s.start) / 60) + " min before: tops on " +
                       std::to_string(int(std::lround(s.cloudTopFraction * 100.0))) + " %";
    char buf[96];
    snprintf(buf, sizeof(buf), " (%.1f km)", s.meanCloudTop / 1000.0);
    text += buf;
    if (s.products & (1 << kGoesCloudMask)) {
      snprintf(buf, sizeof(buf), ", mask %.0f %% cloudy", s.cloudyFraction * 100.0);
      text += buf;
    }
    if (s.products & (1 << kGoesOpticalDepth)) {
      snprintf(buf, sizeof(buf), ", tau %.1f", s.meanOpticalDepth);
      text += buf;
    }
    std::string blended;
    for (const CloudFields* f : {_cloudA.fields.get(), _cloudB.fields.get()}) {
      if (!f) {
        continue;
      }
      if (f->observedTime == s.start) {
        blended = "blended into HRRR";
        break;
      }
      if (f->observedTime) {
        blended = "blended: scan " + WeatherService::formatTime(f->observedTime).substr(11, 5) + " (updating)";
      } else if (blended.empty() && CloudBlend::timeWeight(f->validTime, s.start) > 0.0) {
        blended = "not blended yet";
      }
    }
    if (blended.empty()) {
      blended = "not blended: no HRRR hour within 3 h of the scan";
    }
    lines.push_back(wformat("goes      GOES-19 %s | %s", text.c_str(), blended.c_str()));
  };
  if (!_cloudA.fields) {
    lines.push_back(wformat("clouds    %s", activity.empty() ? "no data" : activity.c_str()));
    goesLine();
    return lines;
  }
  const CloudFields& a = *_cloudA.fields;
  const CloudFields& b = _cloudB.fields ? *_cloudB.fields : a;
  const double w = _cloudB.fields ? double(_scene.cloud.weightB) : 0.0;
  auto hour = [](const CloudFields& f) {
    return WeatherService::formatTime(f.validTime).substr(11, 5) + " (" +
           WeatherService::describeRun(f.cycle, f.forecastHours) + ")";
  };
  std::string data = "HRRR " + hour(a);
  if (_weatherCenter) {
    char at[48];
    snprintf(at, sizeof(at), "@ %.2f, %.2f ", _weatherCenter->first, _weatherCenter->second);
    data = "HRRR " + std::string(at) + hour(a);
  }
  if (_cloudB.fields) {
    data += " -> " + hour(b) + " " + std::to_string(int(std::lround(w * 100.0))) + " %";
  }
  if (_cloudTransparency > 0.0) {
    data += " | " + std::to_string(int(std::lround(_cloudTransparency * 100.0))) + " % transparent";
  }
  lines.push_back(wformat("clouds    %s%s%s", data.c_str(), activity.empty() ? "" : " | ", activity.c_str()));
  if (_cloudLevel == CloudLevel::Shell) {
    const double altitude = _cloudAltitude >= 0.0 ? _cloudAltitude : glm::mix(a.layerAltitude, b.layerAltitude, w);
    const std::string from = _cloudAltitude >= 0.0 ? "manual altitude" : a.layerName + " layer seen from below";
    lines.push_back(wformat("layers    simple shell (step 1): total cover %.0f %% at %.1f km (%s), static shapes",
                            glm::mix(a.meanCover, b.meanCover, w) * 100.0, altitude / 1000.0, from.c_str()));
    return lines;
  }
  std::string layers;
  const char* names[kCloudLayerCount] = {"low", "mid", "high"};
  for (int l = 0; l < kCloudLayerCount; ++l) {
    const CloudLayer &la = a.layers[size_t(l)], &lb = b.layers[size_t(l)];
    const double altitude = _cloudAltitude >= 0.0 ? _cloudAltitude : glm::mix(la.altitudeNearPoi, lb.altitudeNearPoi, w);
    const glm::dvec2 wind = glm::mix(layerWind(a, l), layerWind(b, l), w);
    char buf[80];
    snprintf(buf, sizeof(buf), "%s%s %.0f %% %.1f km", l ? ", " : "", names[l],
             glm::mix(la.meanCover, lb.meanCover, w) * 100.0, altitude / 1000.0);
    layers += buf;
    if (a.hasWind) {
      snprintf(buf, sizeof(buf), " %.0f m/s", glm::length(wind));
      layers += buf;
    }
    layers += _cloudLayerOn[l] ? "" : " (hidden)";
  }
  lines.push_back(wformat("layers    %s%s%s", layers.c_str(), _cloudAltitude >= 0.0 ? " (manual altitude)" : "",
                          a.hasWind ? "" : " (no wind data)"));
  goesLine();
  return lines;
}

void App::logStats(const char* reason) {
  if (!_http || !_resources) {
    return;
  }
  const HttpAssetAccessor::Stats hs = _http->stats();
  const RendererResources::Stats rs = _resources->stats();
  std::string weather;
  for (const std::wstring& line : weatherHudLines()) {
    weather += (weather.empty() ? "" : " | ") + Log::toUtf8(line.substr(std::min<size_t>(10, line.size())));
  }
  LOG_I("[stats:{}] camera {} | terrain tiles {} depth {} | buildings tiles {} | http req {} cache {} errors {} "
        "retries {} {:.1f} MB | gpu meshes {} ({:.0f} MB) images {} ({:.0f} MB) roof captures {} | process {:.0f} MB | "
        "fps {:.0f} | weather {} | log errors {} warnings {}",
        reason, _camera.describe(), _terrainResult ? _terrainResult->tilesToRenderThisFrame.size() : 0,
        _terrainResult ? _terrainResult->maxDepthVisited : 0,
        _buildingsResult ? _buildingsResult->tilesToRenderThisFrame.size() : 0, hs.requests, hs.cacheHits,
        hs.httpErrors + hs.transportFailures, hs.retries, megabytes(hs.bytesDownloaded), rs.tiles,
        megabytes(rs.tileBytes), rs.rasters, megabytes(rs.rasterBytes), _renderer.roofCaptures(), processPrivateMB(),
        _fps, weather.empty() ? std::string("off") : weather, Log::errorCount(), Log::warningCount());
}
