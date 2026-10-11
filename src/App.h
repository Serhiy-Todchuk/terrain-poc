#pragma once
#include "Camera.h"
#include "Config.h"
#include "ElevationService.h"
#include "HttpAssetAccessor.h"
#include "Renderer.h"
#include "RendererResources.h"
#include "TaskProcessor.h"
#include "Ui.h"
#include "Vessels.h"
#include "WeatherService.h"

#include <Cesium3DTilesSelection/Tileset.h>
#include <CesiumAsync/AsyncSystem.h>
#include <CesiumUtility/CreditSystem.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class App {
public:
  explicit App(AppConfig config);
  int run();

private:
  // ---- lifecycle
  bool init();
  bool createWindow();
  bool createTilesets();
  void shutdown();
  void frame(double dt);
  void updateTilesets(double dt);
  void drawFrame(); // scene + resolve + HUD, without present
  void render();
  void setMsaa(bool on);
  void setUiScale(float scale); // DPI scale of the HUD and the settings panel
  // Weather: polls the WeatherService, keeps the datasets around the weather
  // time on the GPU and builds the frame's cloud state (time blend, wind drift,
  // cross-fade after a dataset change).
  void updateClouds();
  float cloudFade() const; // cross-fade progress after a dataset change (1 = done)
  // The weather time: the clock while live (fractional seconds: smooth drift),
  // else the pinned time. Drives the clouds and, with "Apply to sun", the sun.
  double weatherTime() const;
  void setWeatherTime(std::optional<int64_t> pinned); // nullopt: live ("Now")
  void updateSun(); // fixed afternoon sun, or the sun at the weather time
  std::vector<std::wstring> weatherHudLines() const;
  // The weather box ("Fetch weather here"): nullopt = the default box around
  // New York. Moves the service's and the renderer's grid, drops the shown
  // datasets (the new ones are downloaded or replayed from the disk cache).
  void setWeatherCenter(std::optional<std::pair<double, double>> lonLat, const char* reason);
  // The camera's lon/lat if a box centred there lies inside the HRRR domain.
  std::optional<std::pair<double, double>> weatherCenterAtCamera() const;

  // ---- settings panel (Dear ImGui, right side; AppPanel.cpp)
  // Tabs: View (Layers, Rendering, Camera, Level of detail, Tools) and
  // Time & weather (Time & clouds, Sources, History & forecast).
  enum PanelSection {
    kSectionLayers = 0,
    kSectionRendering,
    kSectionCamera,
    kSectionDetail,
    kSectionTools,
    kSectionWeather,
    kSectionSources,
    kSectionHistory,
    kSectionCount
  };
  void drawPanel();
  void drawPanelArrows(); // hide / show arrows of the HUD (top left) and the panel (top right)
  bool section(const char* label, PanelSection index); // collapsing header with persisted state
  void panelLayers();
  void panelWeather();
  void panelSources();
  void panelHistory();
  void panelRendering();
  void panelDetail();
  void panelCamera();
  void panelTools();
  AppConfig currentSettings() const; // the persisted UI state (Settings.h) as an AppConfig

  // ---- input / navigation
  static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp);
  enum class DragMode { None, Orbit, Pan };
  void mouseDown(DragMode mode, int x, int y);
  void mouseMove(int x, int y);
  void mouseUp();
  void mouseWheel(int x, int y, double notches);
  void keyCommand(int vk);
  void flyKeys(double dt);
  glm::dvec3 fallbackPivot(int x, int y) const;
  void resetView();

  // ---- harness
  struct ScriptCommand {
    std::string verb;
    std::vector<std::string> args;
    int line = 0;
  };
  bool loadScript(const std::string& path);
  void stepScript(double dt);
  // Script click / scroll / type: a window message through onMessage (the
  // settings panel sees it first), bypassing the live-input filter.
  void injectMessage(UINT msg, WPARAM wp, LPARAM lp);
  bool computeIdle();
  void takeScreenshot(const std::string& path);
  void manualScreenshot(); // P / panel: <shot-dir>/manual_<frame>.png
  std::vector<std::wstring> hudLines();
  void logStats(const char* reason);
  bool setOption(const std::string& name, bool value);

  AppConfig _cfg;
  HWND _hwnd = nullptr;
  bool _running = true;
  int _exitCode = 0;

  Renderer _renderer;
  Ui _ui;
  Camera _camera;
  CameraSpec _homeView;
  SceneSettings _scene;
  Vessels _vessels;

  std::shared_ptr<TaskProcessor> _taskProcessor;
  std::optional<CesiumAsync::AsyncSystem> _asyncSystem;
  std::shared_ptr<HttpAssetAccessor> _http;
  std::shared_ptr<RendererResources> _resources;
  std::shared_ptr<CesiumUtility::CreditSystem> _credits;
  std::shared_ptr<ElevationService> _elevation;
  std::unique_ptr<Cesium3DTilesSelection::Tileset> _terrain;
  std::unique_ptr<Cesium3DTilesSelection::Tileset> _buildings;
  const Cesium3DTilesSelection::ViewUpdateResult* _terrainResult = nullptr;
  const Cesium3DTilesSelection::ViewUpdateResult* _buildingsResult = nullptr;
  std::string _imageryName;
  std::string _imageryCredit;
  std::shared_ptr<WeatherService> _weather;

  bool _showTerrain = true, _showBuildings = true, _showVessels = true, _showClouds = true;
  bool _cloudLayerOn[3] = {true, true, true}; // low / middle / high
  bool _cloudSingle = false;     // the step-1 look: one static shell, the total cover at the dominant layer's altitude
  double _cloudAltitude = -1.0;  // all layers at this altitude (m MSL, --cloud-altitude); < 0: per-cell HRRR
  bool _sunFromTime = false;     // "Apply to sun": the sun (and the time-of-day light) at the weather time
  std::optional<std::pair<double, double>> _weatherCenter; // the weather box centre; nullopt = New York
  CloudGrid _weatherGrid;        // the box itself (service + renderer)
  glm::dvec3 _fixedSun{0.0};     // afternoon sun from the south-west at the POI
  // Datasets of the frame's time blend on the GPU (a: at / before the weather time, b: after).
  struct CloudSlot {
    std::shared_ptr<const CloudFields> fields;
    std::shared_ptr<CloudTexture> texture;
  };
  CloudSlot _cloudA, _cloudB;
  CloudSlot _cloudOldA, _cloudOldB; // the datasets replaced by a newer run of the same hours, while their winds glide
  double _cloudFadeStart = -1e9; // _time of the last dataset change
  bool _cloudFadeSameHours = false; // the change kept the hours (newer run): the drift glides between winds
  glm::dvec2 _cloudFadeWind[2][3]{}; // winds of a / b per layer before the change (m/s)
  double _flySpeed = 1.0; // multiplier of the altitude-proportional WASD speed

  // settings panel state
  bool _screenshotRequested = false; // deferred until the UI frame is finished
  std::string _lastScreenshot;
  char _gotoText[160] = {};
  bool _gotoInvalid = false;
  std::optional<bool> _panelSections; // script "set sections on|off": open / close all headers once
  int _panelTab = 0;                  // 0 View, 1 Time & weather
  std::optional<int> _panelTabRequest; // select this tab (start, script "tab")
  bool _sectionOpen[kSectionCount] = {};    // persisted open state
  bool _sectionApplied[kSectionCount] = {}; // the saved state was handed to ImGui
  int _historyHours = 48;             // history / forecast table: +- hours around the weather time
  int64_t _historyScrollHour = 0;     // the table was scrolled to this hour
  float _hudInset = 0.0f;             // HUD text inset for its collapse arrow (pixels)

  // input state
  DragMode _drag = DragMode::None;
  int _lastX = 0, _lastY = 0;
  glm::dvec3 _pivot{0.0};
  glm::dvec3 _panAnchor{0.0};
  glm::dvec3 _panNormal{0.0};
  bool _keys[256] = {};
  int _hoverX = -1, _hoverY = -1;
  bool _hoverDirty = false;
  double _hoverTimer = 0.0;
  std::optional<glm::dvec3> _hoverPoint;

  // timing / stats
  std::chrono::steady_clock::time_point _start;
  double _time = 0.0;
  double _fps = 0.0;
  double _fpsAccum = 0.0;
  int _fpsFrames = 0;
  double _statsTimer = 0.0;
  int64_t _frameIndex = 0;

  // harness state
  CameraSpec _flyFrom, _flyTo;
  std::vector<ScriptCommand> _script;
  size_t _scriptPos = 0;
  double _cmdTime = 0.0;
  int _cmdStep = 0;
  int _idleFrames = 0;
  bool _cmdStarted = false;
  bool _scriptMode = false;
  bool _injecting = false;      // onMessage called by injectMessage
  bool _scriptUiInput = false;  // the script clicks/types: the panel takes (injected) input
};
