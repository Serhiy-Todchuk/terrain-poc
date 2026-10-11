#include "pch.h"
#include "Settings.h"

#include <fstream>
#include <functional>
#include <sstream>

namespace {

std::string trim(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) {
    return {};
  }
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool toBool(const std::string& v) { return v == "1" || v == "on" || v == "true" || v == "yes"; }

std::string fmt(double v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

bool readDouble(const std::string& v, double lo, double hi, double& out) {
  try {
    size_t used = 0;
    const double d = std::stod(v, &used);
    if (used != v.size() || !(d >= lo && d <= hi)) {
      return false;
    }
    out = d;
    return true;
  } catch (...) {
    return false;
  }
}

bool readInt(const std::string& v, int lo, int hi, int& out) {
  double d = 0.0;
  if (!readDouble(v, lo, hi, d) || d != std::floor(d)) {
    return false;
  }
  out = int(d);
  return true;
}

// A persisted key: a reader (false = bad value) and a writer over an AppConfig
// (none: a legacy key, read but no longer written).
struct Key {
  const char* name;
  std::function<bool(AppConfig&, const std::string&)> read;
  std::function<std::string(const AppConfig&)> write;
};

Key boolKey(const char* name, bool AppConfig::*field) {
  return {name,
          [field](AppConfig& c, const std::string& v) {
            c.*field = toBool(v);
            return true;
          },
          [field](const AppConfig& c) { return std::string(c.*field ? "1" : "0"); }};
}

Key doubleKey(const char* name, double AppConfig::*field, double lo, double hi) {
  return {name, [field, lo, hi](AppConfig& c, const std::string& v) { return readDouble(v, lo, hi, c.*field); },
          [field](const AppConfig& c) { return fmt(c.*field); }};
}

Key intKey(const char* name, int AppConfig::*field, int lo, int hi) {
  return {name, [field, lo, hi](AppConfig& c, const std::string& v) { return readInt(v, lo, hi, c.*field); },
          [field](const AppConfig& c) { return std::to_string(c.*field); }};
}

const std::vector<Key>& keys() {
  static const std::vector<Key> k = {
      {"window.size",
       [](AppConfig& c, const std::string& v) {
         int w = 0, h = 0;
         if (sscanf_s(v.c_str(), "%dx%d", &w, &h) != 2 || w < 320 || h < 200 || w > 16384 || h > 16384) {
           return false;
         }
         c.width = w;
         c.height = h;
         return true;
       },
       [](const AppConfig& c) { return std::to_string(c.width) + "x" + std::to_string(c.height); }},
      boolKey("layers.terrain", &AppConfig::terrain),
      boolKey("layers.imagery", &AppConfig::imageryOn),
      boolKey("layers.buildings", &AppConfig::buildings),
      boolKey("layers.water", &AppConfig::water),
      boolKey("layers.vessels", &AppConfig::vessels),
      boolKey("layers.clouds", &AppConfig::clouds),
      boolKey("layers.contours", &AppConfig::contours),
      doubleKey("layers.contour_interval", &AppConfig::contourInterval, 0.25, 50.0),
      boolKey("weather.apply_sun", &AppConfig::sunFromTime),
      boolKey("weather.cloud_shadows", &AppConfig::cloudShadows),
      doubleKey("weather.cloud_transparency", &AppConfig::cloudTransparency, 0.0, 1.0),
      {"weather.cloud_layers",
       [](AppConfig& c, const std::string& v) {
         if (v.size() != 3 || v.find_first_not_of("01") != std::string::npos) {
           return false;
         }
         for (int l = 0; l < 3; ++l) {
           c.cloudLayers[l] = v[size_t(l)] == '1';
         }
         return true;
       },
       [](const AppConfig& c) {
         std::string s;
         for (int l = 0; l < 3; ++l) {
           s += c.cloudLayers[l] ? '1' : '0';
         }
         return s;
       }},
      {"weather.cloud_level",
       [](AppConfig& c, const std::string& v) {
         const std::optional<CloudLevel> level = parseCloudLevel(v);
         c.cloudLevel = level.value_or(c.cloudLevel);
         return level.has_value();
       },
       [](const AppConfig& c) { return std::string(cloudLevelInfo(c.cloudLevel).id); }},
      // Before the quality levels (2026-10-11): the "Single layer (step-1 look)" toggle.
      {"weather.cloud_single",
       [](AppConfig& c, const std::string& v) {
         c.cloudLevel = toBool(v) ? CloudLevel::Shell : CloudLevel::Layers;
         return true;
       },
       nullptr},
      intKey("weather.history_hours", &AppConfig::historyHours, 24, 72),
      {"weather.center",
       [](AppConfig& c, const std::string& v) {
         if (v == "default") {
           c.weatherCenter.reset();
           return true;
         }
         double lon = 0.0, lat = 0.0;
         char rest = 0;
         if (sscanf_s(v.c_str(), "%lf,%lf%c", &lon, &lat, &rest, 1) != 2 || std::abs(lon) > 180.0 ||
             std::abs(lat) > 90.0) {
           return false;
         }
         c.weatherCenter = std::make_pair(lon, lat);
         return true;
       },
       [](const AppConfig& c) {
         if (!c.weatherCenter) {
           return std::string("default");
         }
         char buf[48];
         snprintf(buf, sizeof(buf), "%.4f,%.4f", c.weatherCenter->first, c.weatherCenter->second);
         return std::string(buf);
       }},
      boolKey("render.facades", &AppConfig::facades),
      boolKey("render.fog", &AppConfig::fog),
      boolKey("render.wireframe", &AppConfig::wireframe),
      {"render.msaa",
       [](AppConfig& c, const std::string& v) {
         int n = 0;
         if (!readInt(v, 1, 8, n) || (n != 1 && n != 2 && n != 4 && n != 8)) {
           return false;
         }
         c.msaa = n;
         return true;
       },
       [](const AppConfig& c) { return std::to_string(c.msaa); }},
      boolKey("render.vsync", &AppConfig::vsync),
      doubleKey("render.water_absorption", &AppConfig::waterClarity, 0.005, 0.5),
      boolKey("ui.hud", &AppConfig::hud),
      boolKey("ui.panel", &AppConfig::panel),
      intKey("ui.panel_tab", &AppConfig::panelTab, 0, 1),
      intKey("ui.panel_sections", &AppConfig::panelSections, 0, 0xFFFF),
      doubleKey("camera.fov", &AppConfig::fovY, 20.0, 100.0),
      doubleKey("camera.fly_speed", &AppConfig::flySpeed, 0.1, 10.0),
      doubleKey("lod.terrain_sse", &AppConfig::terrainSse, 1.0, 32.0),
      doubleKey("lod.buildings_sse", &AppConfig::buildingsSse, 2.0, 64.0),
  };
  return k;
}

} // namespace

namespace Settings {

bool load(const std::filesystem::path& path, AppConfig& cfg, std::string& report) {
  std::ifstream in(path);
  if (!in) {
    report = "no settings file " + path.string() + " (defaults)";
    return false;
  }
  int applied = 0, bad = 0, unknown = 0;
  std::string line;
  while (std::getline(in, line)) {
    const std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';') {
      continue;
    }
    const size_t eq = t.find('=');
    if (eq == std::string::npos) {
      ++bad;
      continue;
    }
    const std::string name = trim(t.substr(0, eq)), value = trim(t.substr(eq + 1));
    bool known = false;
    for (const Key& k : keys()) {
      if (name == k.name) {
        known = true;
        (k.read(cfg, value) ? applied : bad)++;
        break;
      }
    }
    if (!known) {
      ++unknown;
    }
  }
  report = "settings " + path.string() + ": " + std::to_string(applied) + " values";
  if (bad) {
    report += ", " + std::to_string(bad) + " invalid (ignored)";
  }
  if (unknown) {
    report += ", " + std::to_string(unknown) + " unknown (ignored)";
  }
  return true;
}

bool save(const std::filesystem::path& path, const AppConfig& cfg, std::string& report) {
  std::ostringstream out;
  out << "# TerrainPOC settings, written at exit (delete to reset; --no-settings ignores the file).\n"
         "# Command-line options override these values.\n";
  for (const Key& k : keys()) {
    if (k.write) {
      out << k.name << '=' << k.write(cfg) << '\n';
    }
  }
  std::ofstream file(path, std::ios::trunc);
  if (!file || !(file << out.str()) || !file.flush()) {
    report = "cannot write " + path.string();
    return false;
  }
  report = "settings saved to " + path.string();
  return true;
}

} // namespace Settings
