#include "pch.h"
#include "Config.h"

#include "WeatherService.h"

#include <cstdio>
#include <sstream>
#include <utility>

namespace {

std::vector<double> parseNumbers(const std::string& text) {
  std::vector<double> values;
  std::string token;
  std::stringstream ss(text);
  while (std::getline(ss, token, ',')) {
    try {
      values.push_back(std::stod(token));
    } catch (...) {
      return {};
    }
  }
  return values;
}

bool parseBool(const std::string& v) {
  return v == "1" || v == "on" || v == "true" || v == "yes";
}

} // namespace

bool parseCameraSpec(const std::string& text, CameraSpec::Mode mode, CameraSpec& out) {
  std::vector<double> v = parseNumbers(text);
  const size_t needed = mode == CameraSpec::Mode::LookAt ? 6 : 5;
  if (v.size() != needed) {
    return false;
  }
  out.mode = mode;
  out.lon = v[0];
  out.lat = v[1];
  out.alt = v[2];
  out.heading = v[3];
  out.pitch = v[4];
  if (mode == CameraSpec::Mode::LookAt) {
    out.range = v[5];
  }
  return true;
}

std::string usageText() {
  return R"(TerrainPOC - Cesium Native + Direct3D 11 geospatial terrain PoC (online sources)

Camera:
  --camera lon,lat,alt,heading,pitch        eye position (alt = meters above MSL)
  --lookat lon,lat,alt,heading,pitch,range  orbit view around a target point
  --fov deg                                 vertical field of view (default 60)
  --size WxH                                window client size (default 1600x900)

Harness:
  --screenshot file.png    wait until all tiles are loaded, save a screenshot, exit
  --script file.txt        run a command script (camera/wait/shot/drag/wheel/key/...)
  --shot-dir dir           output folder for script screenshots (default: shots)
  --timeout sec            max seconds to wait for loading (default 180)
  --keep-open              keep running after the screenshot/script
  --hidden                 do not show the window
  --log file               log file (default: TerrainPOC.log next to the exe)
  --verbose                debug-level logging (every HTTP request etc.)
  --d3d-debug              create the D3D11 debug device (needs Graphics Tools)
  --no-vsync
  --hud 0|1                statistics / info overlay on the left (default 1)
  --panel 0|1              settings panel on the right (default 1)

Data / rendering:
  --imagery esri|usgs|none     satellite imagery source (default esri)
  --imagery-max-level N
  --no-terrain --no-buildings --no-water --no-vessels --no-clouds
  --no-facades                 plain buildings (no procedural facades / roof imagery)
  --no-fog                     no aerial perspective (haze over distance)
  --no-cloud-shadows           clouds do not shade the scene
  --weather-time YYYYMMDDHH[MM]|-N|live
                               weather time (UTC): a fixed time, or N hours ago (replay from
                               the disk cache when downloaded before; NOMADS keeps ~2 days +
                               the forecast up to 48 h ahead); default live (the clock)
  --apply-sun                  the sun at the weather time (sky, light, shadows follow it);
                               default a fixed afternoon sun
  --cloud-altitude m           every cloud layer at this altitude above MSL (default: per-cell
                               HRRR altitudes of the low / middle / high layers)
  --weather-center lon,lat|default
                               centre of the ~1200 x 1130 km weather box (default: New York; the
                               panel's "Fetch weather here" moves it to the camera; HRRR covers
                               the contiguous US only)
  --contours                   draw elevation contour lines
  --wireframe
  --msaa 1|2|4|8               multisample anti-aliasing samples (default 4, 1 = off)
  --terrain-max-level N        (default 18, ~0.6 m vertex spacing)
  --terrain-grid N             samples per tile edge (default 129)
  --terrain-sse x              terrain max screen-space error (default 6)
  --buildings-sse x            buildings max screen-space error (default 12)
  --imagery-sse x              imagery max screen-space error (default 2)
  --water-clarity k            light absorption per meter of water (default 0.07)
  --cache-dir dir | --no-cache HTTP response disk cache (default: cache next to exe)
  --offline                    no network: serve everything from the disk cache
  --http-threads N             concurrent HTTP workers (default 24)
  --shader-dir dir
  --settings file | --no-settings
                               the persisted UI settings (toggles, MSAA, vsync, tunables, open
                               panel tab / sections, HUD / panel shown, window size; not the
                               weather time or the camera), read at start and written at exit
                               (default: TerrainPOC.ini next to the exe; command-line options
                               override it; harness runs never read or write it)

Mouse: left-drag orbit, right-drag pan, wheel zoom (towards cursor).
Keys:  WASD/QE fly, R reset view, 1 terrain, 2 imagery, 3 buildings, 4 water,
       5 vessels, 6 clouds, 7 cloud shadows, L sun at the weather time, O fog,
       C contours, F wireframe, M MSAA on/off, T building facades, H HUD,
       G settings panel, P screenshot, F5 reload shaders, Esc quit.
The settings panel (right; the arrow at its top or G hides it, the arrow at the
HUD or H hides the HUD) has two tabs. View: every toggle, MSAA samples, vsync,
water clarity, contour interval, field of view, fly speed, LOD (screen-space
error), camera views / go-to / copy, screenshot, shader reload. Time & weather:
the weather date / time (limited to the hours with weather data, Now = live),
sun at that time, cloud shadows, cloud layers low / middle / high, the history /
forecast table of the hours with data (+-24..72 h; a row pins the time), the
weather sources, the weather box centre + "Fetch weather here" and the loaded datasets.
)";
}

std::optional<AppConfig> parseCommandLine(int argc, char** argv, const AppConfig& base) {
  AppConfig cfg = base;
  auto fail = [](const std::string& msg) -> std::optional<AppConfig> {
    fprintf(stderr, "error: %s\n\n%s", msg.c_str(), usageText().c_str());
    return std::nullopt;
  };

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](std::string& out) -> bool {
      if (i + 1 >= argc) {
        return false;
      }
      out = argv[++i];
      return true;
    };
    std::string v;

    if (a == "--help" || a == "-h" || a == "/?") {
      printf("%s", usageText().c_str());
      return std::nullopt;
    } else if (a == "--camera") {
      if (!next(v) || !parseCameraSpec(v, CameraSpec::Mode::Eye, cfg.camera))
        return fail("--camera expects lon,lat,alt,heading,pitch");
    } else if (a == "--lookat") {
      if (!next(v) || !parseCameraSpec(v, CameraSpec::Mode::LookAt, cfg.camera))
        return fail("--lookat expects lon,lat,alt,heading,pitch,range");
    } else if (a == "--size") {
      if (!next(v) || sscanf_s(v.c_str(), "%dx%d", &cfg.width, &cfg.height) != 2)
        return fail("--size expects WxH");
    } else if (a == "--fov") {
      if (!next(v)) return fail("--fov expects degrees");
      cfg.fovY = std::stod(v);
    } else if (a == "--screenshot") {
      if (!next(cfg.screenshot)) return fail("--screenshot expects a file");
    } else if (a == "--script") {
      if (!next(cfg.script)) return fail("--script expects a file");
    } else if (a == "--shot-dir") {
      if (!next(cfg.shotDir)) return fail("--shot-dir expects a folder");
    } else if (a == "--timeout") {
      if (!next(v)) return fail("--timeout expects seconds");
      cfg.loadTimeoutSec = std::stod(v);
    } else if (a == "--keep-open") {
      cfg.keepOpen = true;
    } else if (a == "--hidden") {
      cfg.hidden = true;
    } else if (a == "--log") {
      if (!next(cfg.logPath)) return fail("--log expects a file");
    } else if (a == "--verbose") {
      cfg.verbose = true;
    } else if (a == "--d3d-debug") {
      cfg.d3dDebug = true;
    } else if (a == "--no-vsync") {
      cfg.vsync = false;
    } else if (a == "--hud") {
      if (!next(v)) return fail("--hud expects 0|1");
      cfg.hud = parseBool(v);
    } else if (a == "--panel") {
      if (!next(v)) return fail("--panel expects 0|1");
      cfg.panel = parseBool(v);
    } else if (a == "--imagery") {
      if (!next(cfg.imagery)) return fail("--imagery expects esri|usgs|none");
    } else if (a == "--imagery-max-level") {
      if (!next(v)) return fail("--imagery-max-level expects N");
      cfg.imageryMaxLevel = std::stoi(v);
    } else if (a == "--no-terrain") {
      cfg.terrain = false;
    } else if (a == "--no-buildings") {
      cfg.buildings = false;
    } else if (a == "--no-water") {
      cfg.water = false;
    } else if (a == "--no-vessels") {
      cfg.vessels = false;
    } else if (a == "--no-facades") {
      cfg.facades = false;
    } else if (a == "--no-clouds") {
      cfg.clouds = false;
    } else if (a == "--no-fog") {
      cfg.fog = false;
    } else if (a == "--no-cloud-shadows") {
      cfg.cloudShadows = false;
    } else if (a == "--apply-sun") {
      cfg.sunFromTime = true;
    } else if (a == "--weather-time") {
      if (!next(v)) return fail("--weather-time expects YYYYMMDDHH[MM] (UTC), -N (hours ago) or live");
      if (v != "live") {
        cfg.weatherTime = WeatherService::parseTime(v);
        if (!cfg.weatherTime) return fail("--weather-time expects YYYYMMDDHH[MM] (UTC), -N (hours ago) or live");
      }
    } else if (a == "--weather-center") {
      if (!next(v)) return fail("--weather-center expects lon,lat or default");
      if (v == "default") {
        cfg.weatherCenter.reset();
      } else {
        const std::vector<double> ll = parseNumbers(v);
        if (ll.size() != 2 || std::abs(ll[0]) > 180.0 || std::abs(ll[1]) > 90.0)
          return fail("--weather-center expects lon,lat or default");
        cfg.weatherCenter = std::make_pair(ll[0], ll[1]);
      }
    } else if (a == "--cloud-altitude") {
      if (!next(v)) return fail("--cloud-altitude expects meters");
      cfg.cloudAltitude = std::stod(v);
    } else if (a == "--contours") {
      cfg.contours = true;
    } else if (a == "--wireframe") {
      cfg.wireframe = true;
    } else if (a == "--msaa") {
      if (!next(v) || (v != "1" && v != "2" && v != "4" && v != "8")) return fail("--msaa expects 1, 2, 4 or 8");
      cfg.msaa = std::stoi(v);
    } else if (a == "--terrain-max-level") {
      if (!next(v)) return fail("--terrain-max-level expects N");
      cfg.terrainMaxLevel = std::stoi(v);
    } else if (a == "--terrain-grid") {
      if (!next(v)) return fail("--terrain-grid expects N");
      cfg.terrainGrid = std::clamp(std::stoi(v), 9, 255);
    } else if (a == "--terrain-sse") {
      if (!next(v)) return fail("--terrain-sse expects a number");
      cfg.terrainSse = std::stod(v);
    } else if (a == "--buildings-sse") {
      if (!next(v)) return fail("--buildings-sse expects a number");
      cfg.buildingsSse = std::stod(v);
    } else if (a == "--imagery-sse") {
      if (!next(v)) return fail("--imagery-sse expects a number");
      cfg.imagerySse = std::stod(v);
    } else if (a == "--water-clarity") {
      if (!next(v)) return fail("--water-clarity expects a number");
      cfg.waterClarity = std::stod(v);
    } else if (a == "--cache-dir") {
      if (!next(cfg.cacheDir)) return fail("--cache-dir expects a folder");
    } else if (a == "--no-cache") {
      cfg.useCache = false;
    } else if (a == "--offline") {
      cfg.offline = true;
    } else if (a == "--http-threads") {
      if (!next(v)) return fail("--http-threads expects N");
      cfg.httpThreads = std::clamp(std::stoi(v), 1, 64);
    } else if (a == "--shader-dir") {
      if (!next(cfg.shaderDir)) return fail("--shader-dir expects a folder");
    } else if (a == "--settings") {
      if (!next(cfg.settingsPath)) return fail("--settings expects a file");
    } else if (a == "--no-settings") {
      cfg.useSettings = false;
    } else {
      return fail("unknown argument: " + a);
    }
  }
  return cfg;
}
