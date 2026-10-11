#pragma once
#include <cstdint>
#include <optional>
#include <string>

// Camera placement given on the command line or in a script.
struct CameraSpec {
  enum class Mode { Eye, LookAt } mode = Mode::LookAt;
  double lon = -74.00753296050705; // degrees
  double lat = 40.66004718826513;  // degrees
  double alt = 0.0;                // meters above mean sea level (NAVD88)
  double heading = 330.0;          // degrees, 0 = north, clockwise
  double pitch = -30.0;            // degrees, negative looks down
  double range = 1800.0;           // LookAt only: distance eye <-> target
};

struct AppConfig {
  int width = 1600;
  int height = 900;
  double fovY = 60.0;
  bool vsync = true;
  int msaa = 4;            // MSAA samples: 1 (off), 2, 4, 8
  bool d3dDebug = false;
  bool verbose = false;
  bool hud = true;     // statistics / info overlay (left)
  bool panel = true;   // settings panel (Dear ImGui, right)
  int panelTab = 0;    // open panel tab: 0 View, 1 Time & weather
  int panelSections = 0x00E3; // bitmask of the open panel sections (App::PanelSection)
  bool hidden = false; // do not show the window (offscreen-style harness runs)

  // Persisted UI settings (Settings.h): read before the command line is applied,
  // written at exit. Not used by harness runs (--script / --screenshot).
  std::string settingsPath; // default: <exe dir>/TerrainPOC.ini
  bool useSettings = true;  // --no-settings
  bool settingsActive = false; // set by main: the file is read / written in this run

  CameraSpec camera;

  // Harness
  std::string screenshot;  // take one screenshot when loaded, then exit
  std::string script;      // command script (see README)
  std::string shotDir = "shots";
  double loadTimeoutSec = 180.0;
  bool keepOpen = false;   // keep running after screenshot/script

  // Data / layers
  std::string logPath;     // default: <exe dir>/TerrainPOC.log
  std::string cacheDir;    // default: <exe dir>/cache
  bool useCache = true;
  bool offline = false;    // serve all data from the disk cache only (no network)
  std::string imagery = "esri"; // esri | usgs | none
  int imageryMaxLevel = -1;     // -1: provider default
  bool imageryOn = true;        // the imagery toggle (key 2); "none" above = not created at all
  bool terrain = true;
  bool buildings = true;
  bool water = true;
  bool vessels = true;
  bool contours = false;
  double contourInterval = 1.0; // m
  bool wireframe = false;
  bool facades = true;          // procedural building façades + roofs from the imagery
  bool fog = true;              // aerial perspective
  bool clouds = true;           // live cloud layers (NOAA HRRR)
  bool cloudShadows = true;     // the clouds shade the scene
  bool cloudLayers[3] = {true, true, true}; // low / middle / high layers shown
  std::optional<std::pair<double, double>> weatherCenter; // lon, lat of the weather box; nullopt = New York
  int historyHours = 48;        // panel: weather history / forecast table +- hours
  std::optional<int64_t> weatherTime; // pinned weather time (unix s, UTC); nullopt = live
  bool sunFromTime = false;     // the sun (and time-of-day light) at the weather time
  double cloudAltitude = -1.0;  // every cloud layer at this altitude (m MSL); < 0: per-cell HRRR altitudes
  double flySpeed = 1.0;        // WASD / QE speed factor
  int terrainMaxLevel = 18;
  int terrainGrid = 129;        // samples per tile edge
  double terrainSse = 6.0;
  double buildingsSse = 12.0;
  double imagerySse = 2.0;
  double waterClarity = 0.07;   // absorption per meter (lower = clearer)
  int httpThreads = 24;
  std::string shaderDir;        // default: <exe dir>/shaders
};

// Parses argv over `base` (the defaults, or the persisted settings: options on
// the command line override them). Returns std::nullopt (after printing usage)
// on error or --help.
std::optional<AppConfig> parseCommandLine(int argc, char** argv, const AppConfig& base = AppConfig());

// Parses "lon,lat,alt,heading,pitch[,range]" (range => LookAt mode).
bool parseCameraSpec(const std::string& text, CameraSpec::Mode mode, CameraSpec& out);

std::string usageText();
