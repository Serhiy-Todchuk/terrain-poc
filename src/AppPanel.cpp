#include "pch.h"
#include "App.h"

#include "Ephemeris.h"
#include "Log.h"

#include <imgui.h>

#include <chrono>
#include <sstream>

// Settings panel (Dear ImGui) docked to the right edge of the window: every
// toggle of the app plus the runtime tunables. Statistics stay in the D2D HUD
// on the left. Each control has the same effect as its key / script command.

using CesiumUtility::Math;

namespace {

struct ViewPreset {
  const char* name;
  const char* tooltip;
  CameraSpec::Mode mode;
  const char* spec;
};

// The views used by the harness scripts (see CLAUDE.md "Useful views").
constexpr ViewPreset kViews[] = {
    {"POI overview", "Bush Terminal piers, Sunset Park (start view)", CameraSpec::Mode::LookAt,
     "-74.00753,40.66005,0,330,-30,1800"},
    {"POI close", "Piers and Sunset Park from 400 m", CameraSpec::Mode::LookAt, "-74.00753,40.66005,0,300,-35,400"},
    {"Seabed by piers", "Seabed next to the piers (hide the water to see the relief)", CameraSpec::Mode::LookAt,
     "-74.0150,40.6620,-9,330,-50,260"},
    {"Upper Bay", "Upper New York Bay towards Manhattan", CameraSpec::Mode::LookAt, "-74.0300,40.6650,0,20,-25,3500"},
    {"Lower Manhattan", "Financial District towers", CameraSpec::Mode::LookAt, "-74.0120,40.7080,60,20,-12,900"},
    {"Jamaica Bay", "Marsh islands and channels (topobathy merge)", CameraSpec::Mode::LookAt,
     "-73.83,40.61,0,0,-60,9000"},
    {"Vessel's eye", "Bridge height across the Upper Bay", CameraSpec::Mode::Eye, "-74.0400,40.6600,18,15,2"},
    {"Harbor 30 km", "New York Harbor from 30 km", CameraSpec::Mode::LookAt, "-74.0300,40.6500,0,0,-60,35000"},
    {"Globe", "Whole Earth", CameraSpec::Mode::LookAt, "-74.0,40.66,0,0,-89,4000000"},
};

void tooltip(const char* text) {
  if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled) &&
      ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// The keyboard shortcut of the previous control, right-aligned in the panel.
void shortcut(const char* key) {
  ImGui::SameLine();
  const float w = ImGui::CalcTextSize(key).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
  ImGui::TextColored(ImVec4(0.62f, 0.66f, 0.72f, 1.0f), "%s", key);
}

// Label without the "##id" suffix, for the log.
std::string visible(const char* label) {
  const std::string s(label);
  return s.substr(0, s.find("##"));
}

bool toggle(const char* label, bool* value, const char* key, const char* tip, bool enabled = true) {
  ImGui::BeginDisabled(!enabled);
  const bool changed = ImGui::Checkbox(label, value);
  ImGui::EndDisabled();
  tooltip(tip);
  if (key) {
    shortcut(key);
  }
  if (changed) {
    LOG_I("Settings panel: {} {}", visible(label), *value ? "on" : "off");
  }
  return changed;
}

// Slider over a double value; returns true when changed (while dragging too).
bool sliderDouble(const char* label, double* value, float lo, float hi, const char* format, const char* tip,
                  bool logarithmic = true) {
  float v = float(*value);
  const bool changed =
      ImGui::SliderFloat(label, &v, lo, hi, format, logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None);
  tooltip(tip);
  if (changed) {
    *value = double(v);
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    LOG_I("Settings panel: {} = {:.3f}", visible(label), *value);
  }
  return changed;
}

// "lon,lat,alt,heading,pitch[,range]", optionally prefixed with camera /
// lookat / --camera / --lookat (as printed in the log). 6 numbers = look-at.
bool parseGoto(const std::string& text, CameraSpec& out) {
  std::stringstream ss(text);
  std::vector<std::string> tokens;
  std::string t;
  while (ss >> t) {
    tokens.push_back(t);
  }
  std::string numbers;
  std::optional<CameraSpec::Mode> mode;
  if (tokens.size() == 1) {
    numbers = tokens[0];
  } else if (tokens.size() == 2) {
    std::string verb = tokens[0];
    while (!verb.empty() && verb.front() == '-') {
      verb.erase(verb.begin());
    }
    if (verb == "camera") {
      mode = CameraSpec::Mode::Eye;
    } else if (verb == "lookat") {
      mode = CameraSpec::Mode::LookAt;
    } else {
      return false;
    }
    numbers = tokens[1];
  } else {
    return false;
  }
  if (!mode) {
    mode = std::count(numbers.begin(), numbers.end(), ',') == 5 ? CameraSpec::Mode::LookAt : CameraSpec::Mode::Eye;
  }
  return parseCameraSpec(numbers, *mode, out);
}

constexpr int64_t kDay = 86400;

int64_t dayOf(int64_t t) { return t - ((t % kDay) + kDay) % kDay; }

// "Sat 2026-10-10"
std::string dayLabel(int64_t day) {
  using namespace std::chrono;
  const sys_days d{days{day / kDay}};
  const char* names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  return std::string(names[weekday{d}.c_encoding()]) + " " + WeatherService::formatTime(day).substr(0, 10);
}

} // namespace

// Collapsing section header. Its open state is persisted (Settings.h) and
// handed to ImGui the first time the header is drawn; `_panelSections` (script
// "set sections on|off") opens / closes every header drawn this frame.
bool App::section(const char* label, PanelSection index) {
  if (_panelSections) {
    ImGui::SetNextItemOpen(*_panelSections, ImGuiCond_Always);
  } else if (!_sectionApplied[index]) {
    ImGui::SetNextItemOpen(_sectionOpen[index], ImGuiCond_Always);
  }
  _sectionApplied[index] = true;
  const bool open = ImGui::CollapsingHeader(label);
  _sectionOpen[index] = open;
  return open;
}

// The small arrows that hide / show the statistics HUD (top left, inside its
// box: the HUD text is inset by _hudInset) and the settings panel (top right;
// the arrow of a shown panel is in its header row). A hidden HUD / panel leaves
// just the arrow - except in harness runs without UI input, whose --hud 0 /
// --panel 0 shots stay clean.
void App::drawPanelArrows() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float s = _ui.scale();
  const float margin = 8.0f * s;
  const float size = ImGui::GetFrameHeight();
  const bool interactive = !_scriptMode || _scriptUiInput;
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoNav;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(size, size));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.5f));

  _hudInset = 0.0f;
  if (_cfg.hud || interactive) {
    const float inset = _cfg.hud ? 2.0f * s : 0.0f; // inside the HUD box (8 px margin + 2)
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + margin + inset, viewport->WorkPos.y + margin + inset),
                            ImGuiCond_Always);
    if (ImGui::Begin("##hudarrow", nullptr, flags)) {
      if (ImGui::ArrowButton("##hud", _cfg.hud ? ImGuiDir_Left : ImGuiDir_Right)) {
        _cfg.hud = !_cfg.hud;
        LOG_I("Settings panel: Statistics HUD {}", _cfg.hud ? "on" : "off");
      }
      tooltip(_cfg.hud ? "Hide the statistics HUD (H)" : "Show the statistics HUD (H)");
    }
    ImGui::End();
    if (_cfg.hud) {
      _hudInset = size + 6.0f * s;
    }
  }
  if (!_cfg.panel && interactive) {
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin, viewport->WorkPos.y + margin),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    if (ImGui::Begin("##panelarrow", nullptr, flags)) {
      if (ImGui::ArrowButton("##panel", ImGuiDir_Left)) {
        _cfg.panel = true;
        LOG_I("Settings panel: shown");
      }
      tooltip("Show the settings panel (G)");
    }
    ImGui::End();
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void App::drawPanel() {
  drawPanelArrows();
  if (!_cfg.panel) {
    return;
  }
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float s = _ui.scale();
  const float margin = 8.0f * s; // same as the HUD
  const float width = 300.0f * s;
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin, viewport->WorkPos.y + margin),
      ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f),
                                      ImVec2(width, std::max(100.0f, viewport->WorkSize.y - 2.0f * margin)));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoTitleBar;
  if (ImGui::Begin("###settings", nullptr, flags)) {
    // Header row: title + the hide arrow (right).
    const float arrow = ImGui::GetFrameHeight();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Settings");
    ImGui::SameLine();
    ImGui::TextDisabled("G");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - arrow));
    if (ImGui::ArrowButton("##hidepanel", ImGuiDir_Right)) {
      _cfg.panel = false;
      LOG_I("Settings panel: hidden");
    }
    tooltip("Hide the settings panel (G); the arrow at the top right brings it back");

    if (ImGui::BeginTabBar("##tabs")) {
      // A tab; `_panelTabRequest` (start, script "tab") selects one and is
      // cleared once ImGui reports it selected.
      auto tab = [&](const char* label, int index) {
        const ImGuiTabItemFlags f = _panelTabRequest == index ? ImGuiTabItemFlags_SetSelected : 0;
        if (!ImGui::BeginTabItem(label, nullptr, f)) {
          return false;
        }
        if (_panelTabRequest == index) {
          _panelTabRequest.reset();
        }
        if (!_panelTabRequest && _panelTab != index) {
          _panelTab = index;
          LOG_I("Settings panel: tab {}", label);
        }
        return true;
      };
      ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.58f);
      if (tab("View", 0)) {
        panelLayers();
        panelRendering();
        panelCamera();
        panelDetail();
        panelTools();
        ImGui::EndTabItem();
      }
      if (tab("Time & weather", 1)) {
        panelWeather();
        panelHistory();
        panelSources();
        ImGui::EndTabItem();
      }
      ImGui::PopItemWidth();
      ImGui::EndTabBar();
    }
    _panelSections.reset();
  }
  ImGui::End();
}

void App::panelLayers() {
  if (!section("Layers", kSectionLayers)) {
    return;
  }
  toggle("Terrain", &_showTerrain, "1", "Land + seabed surface (USGS 3DEP, NOAA BAG / NCEI)", _terrain != nullptr);
  const std::string imageryTip =
      _imageryName.empty() ? "No imagery source (--imagery none)" : _imageryName + " draped on the terrain";
  toggle("Imagery", &_scene.imagery, "2", imageryTip.c_str(), !_imageryName.empty());
  toggle("Buildings", &_showBuildings, "3",
         _buildings ? "OpenStreetMap buildings (OpenFreeMap vector tiles)"
                    : "Not loaded (--no-buildings, or the OpenFreeMap TileJSON was unavailable at start)",
         _buildings != nullptr);
  toggle("Water", &_scene.water, "4", "Sea surface at NAVD88 0 m (seabed visible through it)");
  toggle("Vessels", &_showVessels, "5", "Georeferenced test vessels");
  toggle("Clouds", &_showClouds, "6",
         "Live cloud layers (low / middle / high) from the NOAA HRRR model (3 km, hourly runs) in the weather box (default: around New York), "
         "via the NOMADS grib filter; they drift with the HRRR winds. Date / time and layers below, status in the HUD");
  toggle("Contours", &_scene.contours, "C", "Elevation contour lines (NAVD88) on land and seabed");
  if (_scene.contours) {
    ImGui::Indent();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    sliderDouble("Interval", &_scene.contourInterval, 0.25f, 50.0f, "%.2f m", "Contour spacing in meters");
    ImGui::Unindent();
  }
}

void App::panelWeather() {
  if (!section("Time & clouds", kSectionWeather) || !_weather) {
    return;
  }
  const ImGuiStyle& style = ImGui::GetStyle();
  const int64_t t = int64_t(std::floor(weatherTime()));
  const bool live = _weather->live();
  auto pin = [&](int64_t time, bool log) {
    setWeatherTime(time);
    if (log) {
      LOG_I("Settings panel: weather time {} (--weather-time {})", WeatherService::formatTime(time),
            WeatherService::formatCompact(time));
    }
  };

  // Now (live) + the time in New York.
  ImGui::BeginDisabled(live);
  if (ImGui::Button("Now")) {
    setWeatherTime(std::nullopt);
    LOG_I("Settings panel: weather time live (Now)");
  }
  ImGui::EndDisabled();
  tooltip("Live: the clock, with the newest HRRR runs (re-checked while new ones can appear)");
  ImGui::SameLine();
  const int offset = Ephemeris::newYorkUtcOffsetHours(t);
  ImGui::Text("%s UTC", WeatherService::formatTime(t).substr(11, 5).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("= %s %s New York", WeatherService::formatTime(t + int64_t(offset) * 3600).substr(11, 5).c_str(),
                      offset == -4 ? "EDT" : "EST");
  shortcut(live ? "live" : "replay");

  // Days and hours with weather data (NOMADS file lists + disk cache).
  const std::vector<int64_t>& hours = _weather->availableHours();
  std::vector<int64_t> days;
  for (const int64_t h : hours) {
    if (days.empty() || days.back() != dayOf(h)) {
      days.push_back(dayOf(h));
    }
  }
  const int64_t today = dayOf(t);
  if (std::find(days.begin(), days.end(), today) == days.end()) {
    days.insert(std::upper_bound(days.begin(), days.end(), today), today); // the shown day, even without data
  }
  const int dayIndex = int(std::find(days.begin(), days.end(), today) - days.begin());
  // Minutes of a day that have data: from its first available hour to its last
  // (to midnight when the next hour, 00:00 of the next day, is available too).
  auto dayRange = [&](int64_t day, int& lo, int& hi) {
    lo = 0;
    hi = 1439;
    const auto first = std::lower_bound(hours.begin(), hours.end(), day);
    const auto end = std::lower_bound(hours.begin(), hours.end(), day + kDay);
    if (first == end) {
      return;
    }
    lo = int((*first - day) / 60);
    hi = end != hours.end() && *end == day + kDay ? 1439 : int((*std::prev(end) - day) / 60);
  };
  auto pickDay = [&](int64_t day) {
    int lo = 0, hi = 0;
    dayRange(day, lo, hi);
    pin(day + int64_t(std::clamp(int((t - today) / 60), lo, hi)) * 60, true);
  };

  const float arrow = ImGui::GetFrameHeight();
  ImGui::BeginDisabled(dayIndex <= 0);
  if (ImGui::ArrowButton("##prevday", ImGuiDir_Left)) {
    pickDay(days[size_t(dayIndex - 1)]);
  }
  ImGui::EndDisabled();
  tooltip("Previous day with weather data");
  ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - arrow - style.ItemInnerSpacing.x);
  if (ImGui::BeginCombo("##date", dayLabel(today).c_str())) {
    for (const int64_t day : days) {
      int lo = 0, hi = 0;
      dayRange(day, lo, hi);
      char label[64];
      snprintf(label, sizeof(label), "%s  %02d:%02d-%02d:%02d", dayLabel(day).c_str(), lo / 60, lo % 60, hi / 60,
               hi % 60);
      if (ImGui::Selectable(label, day == today) && day != today) {
        pickDay(day);
      }
    }
    ImGui::EndCombo();
  }
  tooltip("UTC date: only days with HRRR data (NOMADS keeps ~2 days and the forecast up to 48 h ahead; "
          "older days from the disk cache)");
  ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
  ImGui::BeginDisabled(dayIndex + 1 >= int(days.size()));
  if (ImGui::ArrowButton("##nextday", ImGuiDir_Right)) {
    pickDay(days[size_t(dayIndex + 1)]);
  }
  ImGui::EndDisabled();
  tooltip("Next day with weather data");

  // Time of day (UTC), limited to the hours with data; +-1 h across days.
  const float stepWidth = ImGui::CalcTextSize("+1h").x + style.FramePadding.x * 2.0f;
  const int64_t first = hours.empty() ? t - kDay : hours.front(), last = hours.empty() ? t + kDay : hours.back();
  ImGui::BeginDisabled(t - 3600 < first);
  if (ImGui::Button("-1h", ImVec2(stepWidth, 0.0f))) {
    pin(t - 3600, true);
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
  int lo = 0, hi = 0;
  dayRange(today, lo, hi);
  int minutes = std::clamp(int((t - today) / 60), lo, hi);
  char label[32];
  snprintf(label, sizeof(label), "%02d:%02d UTC", minutes / 60, minutes % 60);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - stepWidth - style.ItemInnerSpacing.x);
  if (ImGui::SliderInt("##time", &minutes, lo, hi, label, ImGuiSliderFlags_NoInput | ImGuiSliderFlags_AlwaysClamp)) {
    pin(today + int64_t(minutes) * 60, false); // logged on release
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    pin(int64_t(std::floor(weatherTime())), true);
  }
  tooltip("Time of day (UTC). The clouds blend the two HRRR hours around it and drift with the wind");
  ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
  ImGui::BeginDisabled(t + 3600 > last);
  if (ImGui::Button("+1h", ImVec2(stepWidth, 0.0f))) {
    pin(t + 3600, true);
  }
  ImGui::EndDisabled();

  if (toggle("Apply to sun", &_sunFromTime, "L",
             "The sun at this date / time: its position, sky colours and light (twilight, night), long cloud "
             "shadows when it is low. Off: a fixed afternoon sun from the south-west")) {
    updateSun();
  }
  toggle("Cloud shadows", &_scene.cloudShadows, "7",
         "The clouds shade terrain, buildings, vessels, water and lower cloud layers (the same cloud shapes, "
         "projected along the sun)");
  ImGui::TextUnformatted("Layers");
  const char* layerNames[kCloudLayerCount] = {"Low", "Mid", "High"};
  const char* layerTips[kCloudLayerCount] = {"Low clouds (below ~3.8 km): base from the HRRR cloud base",
                                             "Middle clouds (~3.8-8 km)",
                                             "High clouds (above ~8 km): from the HRRR cloud top"};
  for (int l = 0; l < kCloudLayerCount; ++l) {
    ImGui::SameLine();
    ImGui::BeginDisabled(!_showClouds || _cloudSingle);
    if (ImGui::Checkbox(layerNames[l], &_cloudLayerOn[l])) {
      LOG_I("Settings panel: {} clouds {}", layerNames[l], _cloudLayerOn[l] ? "on" : "off");
    }
    ImGui::EndDisabled();
    tooltip(layerTips[l]);
  }
  toggle("Single layer (step-1 look)", &_cloudSingle, "8",
         "One thin cloud shell as in step 1: the HRRR total cover at the altitude of the layer mostly seen from "
         "below (or --cloud-altitude), static shapes. Off: the low / middle / high slabs at their own altitudes, "
         "drifting with the wind",
         _showClouds);
}

// Where the weather comes from, what is loaded, and the nearest forecast.
void App::panelSources() {
  if (!section("Sources", kSectionSources) || !_weather) {
    return;
  }
  // The weather box: where it is centred, how far the camera is, move it here.
  {
    const CesiumGeospatial::Cartographic c = _camera.cartographic();
    const double camLon = Math::radiansToDegrees(c.longitude), camLat = Math::radiansToDegrees(c.latitude);
    const double kmPerLon = 111.32 * std::cos(Math::degreesToRadians(_weatherGrid.centreLat()));
    const double dx = (camLon - _weatherGrid.centreLon()) * kmPerLon;
    const double dy = (camLat - _weatherGrid.centreLat()) * 111.32;
    const double distanceKm = std::hypot(dx, dy);
    ImGui::Text("Weather box: %.2f, %.2f%s", _weatherGrid.centreLon(), _weatherGrid.centreLat(),
                _weatherCenter ? "" : " (New York)");
    tooltip("Centre (lon, lat) of the ~1200 x 1130 km box the clouds are requested for. Outside it there are no "
            "clouds; the summaries in the HUD are about its centre");
    ImGui::SameLine();
    ImGui::TextDisabled("camera %.0f km away", distanceKm);
    const std::optional<std::pair<double, double>> here = weatherCenterAtCamera();
    ImGui::BeginDisabled(!here || distanceKm < 1.0);
    if (ImGui::Button("Fetch weather here")) {
      setWeatherCenter(here, "Settings panel");
    }
    ImGui::EndDisabled();
    tooltip(here ? "Centre the weather box on the camera: the HRRR hours around the weather time are downloaded "
                   "for it (~1 MB per hour, cached on disk for replays)"
                 : "The camera is outside the HRRR domain (contiguous US)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!_weatherCenter);
    if (ImGui::Button("New York")) {
      setWeatherCenter(std::nullopt, "Settings panel");
    }
    ImGui::EndDisabled();
    tooltip("Back to the default box around New York");
  }
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted("Clouds: NOAA HRRR");
  tooltip("High-Resolution Rapid Refresh: NOAA's 3 km CONUS weather model, a new run every hour (published "
          "~50 min after the hour) with forecasts to 18 h (48 h at 00 / 06 / 12 / 18z). The app requests only "
          "the cloud fields of the weather box from the NOMADS grib filter and keeps them in the disk "
          "cache (replayable offline). NOMADS keeps ~2 days of runs");
  ImGui::SameLine();
  ImGui::TextDisabled("3 km model, hourly runs");
  ImGui::TextDisabled("Cover of the low / middle / high layers, cloud base / top, winds at 850 / 500 / 250 hPa "
                      "(drift), cloud water / ice (opacity)");
  const CloudGrid& grid = _weatherGrid;
  ImGui::TextDisabled("Box %.1f-%.1f W, %.1f-%.1f N (~1200 x 1130 km), %.3f deg cells; no clouds outside it",
                      -grid.west, -grid.east, grid.south, grid.north, (grid.east - grid.west) / grid.nx);

  // Catalog: hours with data, the newest run and how far its forecast reaches.
  const std::vector<WeatherService::HourInfo> hours = _weather->catalog();
  const int64_t newest = _weather->newestRun();
  int64_t forecastEnd = 0, analysisEnd = 0;
  for (const WeatherService::HourInfo& h : hours) {
    if (h.cycle == newest) {
      forecastEnd = std::max(forecastEnd, h.validTime);
    }
    if (h.forecastHours == 0) {
      analysisEnd = std::max(analysisEnd, h.validTime);
    }
  }
  ImGui::Spacing();
  if (hours.empty()) {
    ImGui::Text("Catalog: %s", _weather->catalogReady() ? "no HRRR data (cache empty, NOMADS unreachable)"
                                                        : "searching the disk cache / NOMADS...");
  } else {
    ImGui::Text("Catalog: %zu hours, %s .. %s", hours.size(),
                WeatherService::formatTime(hours.front().validTime).substr(5, 11).c_str(),
                WeatherService::formatTime(hours.back().validTime).substr(5, 11).c_str());
    tooltip("Valid hours with data: NOMADS file lists (newest run per hour, forecasts included) + the disk cache");
    if (newest) {
      ImGui::Text("Newest run %s %02dz", WeatherService::formatTime(newest).substr(0, 10).c_str(),
                  int(((newest % 86400) + 86400) % 86400 / 3600));
      ImGui::TextDisabled("Forecast to %s (+%lld h)", WeatherService::formatTime(forecastEnd).substr(5, 11).c_str(),
                          static_cast<long long>((forecastEnd - newest) / 3600));
      tooltip("The latest model run and the last hour it forecasts. Hours after the newest run's time are "
              "forecasts; hours before it come from the runs closest to them (analyses, +0 h)");
    }
    if (analysisEnd) {
      ImGui::TextDisabled("Analyses (+0 h) up to %s", WeatherService::formatTime(analysisEnd).substr(5, 11).c_str());
    }
  }

  // The datasets of the time blend and the service status.
  if (_showClouds && _cloudA.fields) {
    const CloudFields& a = *_cloudA.fields;
    auto hour = [](const CloudFields& f) {
      return WeatherService::formatTime(f.validTime).substr(11, 5) + " (" +
             WeatherService::describeRun(f.cycle, f.forecastHours) + ")";
    };
    std::string text = "Loaded " + hour(a);
    if (_cloudB.fields) {
      text += " -> " + hour(*_cloudB.fields) + " " + std::to_string(int(std::lround(_scene.cloud.weightB * 100.0))) +
              " %";
    }
    ImGui::TextUnformatted(text.c_str());
    tooltip("The two datasets around the weather time (valid hour and model run) and the blend weight of the "
            "later one; each dataset is one request for the cloud fields and one for the layer winds / "
            "condensate");
    ImGui::TextDisabled("Winds %s, condensate %s", a.hasWind ? "yes" : "no (no drift)",
                        a.hasCondensate ? "yes" : "no (heuristic)");
  } else {
    ImGui::TextDisabled(_showClouds ? "No dataset loaded yet" : "Clouds hidden: nothing is requested");
  }
  const std::string activity = _weather->activity();
  ImGui::Text("Status: %s",
              activity.empty() ? (_cfg.offline ? "idle (offline: disk cache only)" : "idle") : activity.c_str());
  ImGui::PopTextWrapPos();
}

// The hours with weather data around the weather time: run, where the data is,
// cloud cover of the datasets seen this session. A row pins the weather time.
void App::panelHistory() {
  if (!section("History & forecast", kSectionHistory) || !_weather) {
    return;
  }
  const ImGuiStyle& style = ImGui::GetStyle();
  const int64_t t = int64_t(std::floor(weatherTime()));
  const int64_t hourT = t - ((t % 3600) + 3600) % 3600;
  const int64_t now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const int64_t hourNow = now - ((now % 3600) + 3600) % 3600;

  char label[16];
  snprintf(label, sizeof(label), "+-%d h", _historyHours);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("+-72 h").x + style.FramePadding.x * 2.0f + ImGui::GetFrameHeight());
  if (ImGui::BeginCombo("##range", label)) {
    for (const int r : {24, 48, 72}) {
      snprintf(label, sizeof(label), "+-%d h", r);
      if (ImGui::Selectable(label, r == _historyHours) && r != _historyHours) {
        _historyHours = r;
        LOG_I("Settings panel: history +-{} h", r);
      }
    }
    ImGui::EndCombo();
  }
  tooltip("Hours around the weather time to list");
  const std::vector<WeatherService::HourInfo> hours = _weather->catalog();
  const int64_t lo = hourT - int64_t(_historyHours) * 3600, hi = hourT + int64_t(_historyHours) * 3600;
  size_t inRange = 0, seen = 0;
  for (const WeatherService::HourInfo& h : hours) {
    if (h.validTime >= lo && h.validTime <= hi) {
      ++inRange;
      seen += h.summary != nullptr;
    }
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%zu hours with data, %zu seen", inRange, seen);
  tooltip("Cover is known for the hours whose dataset was loaded this session (a row loads it); grey rows "
          "are listed on NOMADS but not downloaded yet. Hours after the newest run are forecasts");

  const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
  const ImGuiTableFlags tableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                     ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX;
  if (!ImGui::BeginTable("##history", 3, tableFlags, ImVec2(ImGui::GetContentRegionAvail().x, rowHeight * 9.6f))) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("UTC", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableSetupColumn("Run", ImGuiTableColumnFlags_WidthFixed);
  ImGui::TableSetupColumn("low / mid / high %", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();
  const ImVec4 disabled = style.Colors[ImGuiCol_TextDisabled];
  for (const WeatherService::HourInfo& h : hours) {
    if (h.validTime < lo || h.validTime > hi) {
      continue;
    }
    ImGui::TableNextRow();
    if (h.validTime == hourNow) {
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.55f, 0.45f, 0.1f, 0.35f)));
    }
    ImGui::TableNextColumn();
    const bool current = h.validTime == hourT;
    const bool downloaded = h.cached || h.loaded;
    ImGui::PushStyleColor(ImGuiCol_Text, downloaded ? style.Colors[ImGuiCol_Text] : disabled);
    ImGui::PushID(int(h.validTime / 3600));
    const std::string utc = WeatherService::formatTime(h.validTime).substr(5, 11); // "10-10 16:00"
    if (ImGui::Selectable(utc.c_str(), current, ImGuiSelectableFlags_SpanAllColumns) && h.validTime != t) {
      setWeatherTime(h.validTime);
      LOG_I("Settings panel: weather time {} (--weather-time {})", WeatherService::formatTime(h.validTime),
            WeatherService::formatCompact(h.validTime));
    }
    ImGui::PopID();
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::BeginTooltip()) {
      const int offset = Ephemeris::newYorkUtcOffsetHours(h.validTime);
      ImGui::Text("%s = %s %s New York%s", WeatherService::formatTime(h.validTime).c_str(),
                  WeatherService::formatTime(h.validTime + int64_t(offset) * 3600).substr(11, 5).c_str(),
                  offset == -4 ? "EDT" : "EST", h.validTime == hourNow ? " (this hour)" : "");
      ImGui::Text("HRRR run %s %s: %s", WeatherService::formatTime(h.cycle).substr(0, 10).c_str(),
                  WeatherService::describeRun(h.cycle, h.forecastHours).c_str(),
                  h.forecastHours == 0 ? "analysis" : "forecast");
      ImGui::Text("%s%s", h.loaded ? "loaded" : h.cached ? "in the disk cache" : "on NOMADS, not downloaded",
                  h.listed && downloaded ? ", on NOMADS" : "");
      if (h.summary) {
        ImGui::Text("cover %.0f %%, seen from below: %s", h.summary->meanCover * 100.0,
                    h.summary->layerName.c_str());
        ImGui::Text("altitudes %.1f / %.1f / %.1f km MSL near the box centre%s", h.summary->layerAltitude[0] / 1000.0,
                    h.summary->layerAltitude[1] / 1000.0, h.summary->layerAltitude[2] / 1000.0,
                    h.summary->hasWind ? "" : " (no wind data)");
      }
      ImGui::TextDisabled("Click: weather time %s", utc.c_str());
      ImGui::EndTooltip();
    }
    if (current && _historyScrollHour != hourT) {
      ImGui::SetScrollHereY(0.5f);
      _historyScrollHour = hourT;
    }
    ImGui::TableNextColumn();
    ImGui::TextColored(downloaded ? style.Colors[ImGuiCol_Text] : disabled, "%s",
                       WeatherService::describeRun(h.cycle, h.forecastHours).c_str());
    ImGui::TableNextColumn();
    if (h.summary) {
      ImGui::Text("%.0f / %.0f / %.0f", h.summary->layerCover[0] * 100.0, h.summary->layerCover[1] * 100.0,
                  h.summary->layerCover[2] * 100.0);
    } else {
      ImGui::TextDisabled("-");
    }
  }
  ImGui::EndTable();
}

void App::panelRendering() {
  if (!section("Rendering", kSectionRendering)) {
    return;
  }
  toggle("Building facades", &_scene.facades, "T",
         "Procedural windows by floor + roof colours from the imagery (off: plain grey buildings)");
  toggle("Fog", &_scene.fog, "O", "Aerial perspective: haze over distance (also on the clouds)");
  toggle("Wireframe", &_scene.wireframe, "F", "Draw triangle edges");

  const int counts[] = {1, 2, 4, 8};
  const char* names[] = {"Off", "2x", "4x", "8x"};
  int current = 0;
  for (int i = 0; i < 4; ++i) {
    if (counts[i] == _renderer.sampleCount()) {
      current = i;
    }
  }
  if (ImGui::BeginCombo("MSAA", names[current])) {
    for (int i = 0; i < 4; ++i) {
      if (ImGui::Selectable(names[i], i == current) && i != current) {
        if (counts[i] > 1) {
          _cfg.msaa = counts[i]; // the count the M key toggles back to
        }
        _renderer.setSampleCount(counts[i]);
      }
    }
    ImGui::EndCombo();
  }
  tooltip("Multisample anti-aliasing (M toggles between off and the last count)");
  shortcut("M");

  toggle("VSync", &_cfg.vsync, nullptr, "Off: render as fast as possible (benchmarking)");
  sliderDouble("Water absorption", &_scene.waterAbsorption, 0.005f, 0.5f, "%.3f /m",
               "Light absorption per meter of water: lower = clearer (--water-clarity)");
  toggle("Statistics HUD", &_cfg.hud, "H", "Statistics and info on the left");
}

void App::panelCamera() {
  if (!section("Camera", kSectionCamera)) {
    return;
  }
  const ImGuiStyle& style = ImGui::GetStyle();
  const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
  if (ImGui::Button("Home (R)", ImVec2(half, 0.0f))) {
    resetView();
  }
  tooltip("The start view (--camera / --lookat)");
  int column = 1;
  for (const ViewPreset& view : kViews) {
    if (column % 2 == 1) {
      ImGui::SameLine();
    }
    ++column;
    if (ImGui::Button(view.name, ImVec2(half, 0.0f))) {
      CameraSpec spec;
      if (parseCameraSpec(view.spec, view.mode, spec)) {
        _camera.apply(spec);
        LOG_I("View '{}': --camera {}", view.name, _camera.describe());
      }
    }
    tooltip(view.tooltip);
  }

  ImGui::Spacing();
  const float goWidth = ImGui::CalcTextSize("Go").x + style.FramePadding.x * 2.0f;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - goWidth - style.ItemSpacing.x);
  const bool enter = ImGui::InputTextWithHint("##goto", "lon,lat,alt,heading,pitch[,range]", _gotoText,
                                              sizeof(_gotoText), ImGuiInputTextFlags_EnterReturnsTrue);
  tooltip("Eye position (5 numbers) or look-at (6 numbers: + range); alt = m above MSL. "
          "A pasted '--camera ...' / '--lookat ...' works too.");
  ImGui::SameLine();
  if (ImGui::Button("Go") || enter) {
    CameraSpec spec;
    _gotoInvalid = !parseGoto(_gotoText, spec);
    if (!_gotoInvalid) {
      _camera.apply(spec);
      LOG_I("Go to: --camera {}", _camera.describe());
    }
  }
  if (_gotoInvalid) {
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Expected lon,lat,alt,heading,pitch[,range]");
  }
  const std::string current = "--camera " + _camera.describe();
  if (ImGui::Button("Copy --camera")) {
    ImGui::SetClipboardText(current.c_str());
  }
  tooltip(current.c_str());

  double fovDeg = Math::radiansToDegrees(_camera.fovY);
  if (sliderDouble("Field of view", &fovDeg, 20.0f, 100.0f, "%.0f deg", "Vertical field of view (--fov)", false)) {
    _camera.fovY = Math::degreesToRadians(fovDeg);
  }
  sliderDouble("Fly speed", &_flySpeed, 0.1f, 10.0f, "%.2fx",
               "WASD / QE speed factor (the speed also scales with the altitude; Shift = faster)");
}

void App::panelDetail() {
  if (!section("Level of detail", kSectionDetail)) {
    return;
  }
  ImGui::TextDisabled("Max screen-space error: lower = sharper,");
  ImGui::TextDisabled("more tiles and web requests");
  if (_terrain && sliderDouble("Terrain##sse", &_cfg.terrainSse, 1.0f, 32.0f, "%.1f px", "--terrain-sse (default 6)")) {
    _terrain->getOptions().maximumScreenSpaceError = _cfg.terrainSse;
  }
  if (_buildings &&
      sliderDouble("Buildings##sse", &_cfg.buildingsSse, 2.0f, 64.0f, "%.1f px", "--buildings-sse (default 12)")) {
    _buildings->getOptions().maximumScreenSpaceError = _cfg.buildingsSse;
  }
}

void App::panelTools() {
  if (!section("Tools", kSectionTools)) {
    return;
  }
  if (ImGui::Button("Screenshot (P)")) {
    _screenshotRequested = true; // taken after the UI frame is finished
  }
  tooltip("Saves <shot-dir>/manual_<frame>.png (what is on screen, including this panel)");
  ImGui::SameLine();
  if (ImGui::Button("Reload shaders (F5)")) {
    _renderer.reloadShaders();
  }
  tooltip("Recompiles Scene.hlsl from --shader-dir");
  if (!_lastScreenshot.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Saved %s", _lastScreenshot.c_str());
    ImGui::PopTextWrapPos();
  }
  if (ImGui::Button("Log statistics")) {
    logStats("panel");
  }
  tooltip("Writes a [stats:panel] line to the log");

  ImGui::SeparatorText("Mouse / keys");
  ImGui::TextDisabled("Left-drag orbit, right-drag pan,");
  ImGui::TextDisabled("wheel zoom towards the cursor");
  ImGui::TextDisabled("WASD / QE fly (Shift faster), arrows turn");
  ImGui::TextDisabled("Esc quit");
}
