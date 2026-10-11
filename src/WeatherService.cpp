#include "pch.h"
#include "WeatherService.h"

#include "CloudBlend.h"
#include "Grib2.h"
#include "GridFill.h"
#include "Log.h"

#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>

#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

using namespace CesiumAsync;

namespace {

using Clock = std::chrono::steady_clock;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr int64_t kHour = 3600;
constexpr int64_t kMaxBracketGap = 6 * kHour; // datasets farther from the weather time are not blended
constexpr double kPi = 3.14159265358979323846;

int64_t unixNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Utc {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0;
};

Utc toUtc(int64_t t) {
  using namespace std::chrono;
  const sys_seconds s{seconds{t}};
  const sys_days d = floor<days>(s);
  const year_month_day ymd{d};
  const int64_t secs = (s - d).count();
  return {int(ymd.year()), int(unsigned(ymd.month())), int(unsigned(ymd.day())), int(secs / 3600),
          int(secs % 3600 / 60)};
}

int64_t dayStart(int64_t t) { return t - ((t % 86400) + 86400) % 86400; }
int64_t hourStart(int64_t t) { return t - ((t % kHour) + kHour) % kHour; }

std::string snippet(std::span<const std::byte> body) {
  std::string s;
  for (size_t i = 0; i < body.size() && s.size() < 160; ++i) {
    const char c = char(body[i]);
    s.push_back((c >= 32 && c < 127) ? c : ' ');
  }
  return s;
}

using GridFill::boxBlur;
using GridFill::fillGaps;

// ---- HRRR on NOMADS --------------------------------------------------------------
// The NOMADS grib filter cuts a lat/lon box out of the 3 km CONUS Lambert grid
// and returns only the selected messages (simple packing). Files appear ~50 min
// after the run hour, runs are hourly with forecasts f00-f18 (f48 at 00/06/12/18z);
// the filter keeps the last ~2 days. A file that is not published answers HTTP 403.
namespace HrrrSource {

constexpr const char* kFilter = "https://nomads.ncep.noaa.gov/cgi-bin/filter_hrrr_2d.pl";
// One message each: total / low / middle / high cloud cover, cloud base / top
// height. Never call the filter without variables: it then returns the whole
// ~130 MB CONUS file. (The step-1 selection: cached datasets keep their URL.)
constexpr const char* kFields =
    "&var_HCDC=on&var_HGT=on&var_LCDC=on&var_MCDC=on&var_TCDC=on"
    "&lev_cloud_base=on&lev_cloud_top=on&lev_entire_atmosphere=on"
    "&lev_high_cloud_layer=on&lev_low_cloud_layer=on&lev_middle_cloud_layer=on";
// "Dynamics": winds at 850 / 500 / 250 hPa (low / middle / high layer, grid-relative
// components) and total column cloud water / ice (kg/m2). 8 messages, ~0.75 MB.
constexpr const char* kDynamics = "&var_TCOLI=on&var_TCOLW=on&var_UGRD=on&var_VGRD=on"
                                  "&lev_250_mb=on&lev_500_mb=on&lev_850_mb=on&lev_entire_atmosphere=on";
// CloudGrid plus ~0.4 degree margin (the Lambert cut covers this box; the filter
// clips it to the HRRR domain, cells beyond are clear). %g keeps the New York
// box at "-81.6 / -66.4 / 46.15 / 35.15": its cached URLs stay valid.
std::string subregion(const CloudGrid& g) {
  char buf[96];
  snprintf(buf, sizeof(buf), "&subregion=&leftlon=%g&rightlon=%g&toplat=%g&bottomlat=%g", g.west - 0.4, g.east + 0.4,
           g.north + 0.4, g.south - 0.4);
  return buf;
}
constexpr int kMaxForecastHours = 48; // 00/06/12/18z runs; the other runs end at 18 h
constexpr double kWindLevels[kCloudLayerCount] = {85000.0, 50000.0, 25000.0}; // Pa

std::string dayDir(int64_t day) {
  const Utc u = toUtc(day);
  char buf[48];
  snprintf(buf, sizeof(buf), "?dir=%%2Fhrrr.%04d%02d%02d%%2Fconus", u.year, u.month, u.day);
  return buf;
}

std::string listUrl(int64_t day) { return std::string(kFilter) + dayDir(day); }

std::string fileUrl(int64_t cycle, int forecastHours) {
  char file[64];
  snprintf(file, sizeof(file), "&file=hrrr.t%02dz.wrfsfcf%02d.grib2", toUtc(cycle).hour, forecastHours);
  return std::string(kFilter) + dayDir(dayStart(cycle)) + file;
}

std::string dataUrl(int64_t cycle, int forecastHours, const CloudGrid& g) {
  return fileUrl(cycle, forecastHours) + kFields + subregion(g);
}
std::string dynamicsUrl(int64_t cycle, int forecastHours, const CloudGrid& g) {
  return fileUrl(cycle, forecastHours) + kDynamics + subregion(g);
}

bool validRun(int64_t cycle, int forecastHours) {
  return forecastHours >= 0 && (forecastHours <= 18 || (forecastHours <= kMaxForecastHours && toUtc(cycle).hour % 6 == 0));
}

// (run hour, forecast hour) of every "hrrr.tHHz.wrfsfcfFF.grib2" in a day's file list.
std::vector<std::pair<int, int>> parseListing(std::span<const std::byte> body) {
  const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
  constexpr std::string_view key = "value=\"hrrr.t";
  auto digits = [](std::string_view s) { return std::isdigit(uint8_t(s[0])) && std::isdigit(uint8_t(s[1])); };
  std::vector<std::pair<int, int>> runs;
  for (size_t pos = text.find(key); pos != std::string_view::npos; pos = text.find(key, pos)) {
    pos += key.size();
    const std::string_view rest = text.substr(pos, 19); // HHz.wrfsfcfFF.grib2
    if (rest.size() == 19 && digits(rest) && rest.substr(2, 9) == "z.wrfsfcf" && digits(rest.substr(11)) &&
        rest.substr(13) == ".grib2") {
      runs.emplace_back((rest[0] - '0') * 10 + (rest[1] - '0'), (rest[11] - '0') * 10 + (rest[12] - '0'));
    }
  }
  return runs;
}

// Day directories ("hrrr.YYYYMMDD") on the filter's top-level page.
std::vector<int64_t> parseDays(std::span<const std::byte> body) {
  const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
  constexpr std::string_view key = "hrrr.";
  std::vector<int64_t> days;
  for (size_t pos = text.find(key); pos != std::string_view::npos; pos = text.find(key, pos)) {
    pos += key.size();
    const std::string_view d = text.substr(pos, 9);
    if (d.size() < 9 || !std::all_of(d.begin(), d.begin() + 8, [](char c) { return std::isdigit(uint8_t(c)); }) ||
        std::isdigit(uint8_t(d[8]))) {
      continue;
    }
    using namespace std::chrono;
    const int y = std::stoi(std::string(d.substr(0, 4))), m = std::stoi(std::string(d.substr(4, 2))),
              dd = std::stoi(std::string(d.substr(6, 2)));
    const year_month_day ymd{std::chrono::year{y}, std::chrono::month{unsigned(m)}, std::chrono::day{unsigned(dd)}};
    if (ymd.ok()) {
      days.push_back(int64_t(sys_days(ymd).time_since_epoch().count()) * 86400);
    }
  }
  std::sort(days.begin(), days.end());
  days.erase(std::unique(days.begin(), days.end()), days.end());
  return days;
}

// A day whose 24 runs are all listed with their full forecast range.
bool dayComplete(const std::vector<std::pair<int, int>>& runs) { return runs.size() >= 4 * 49 + 20 * 19; }

struct Decoded {
  std::shared_ptr<CloudFields> fields;
  std::string error;         // the cloud fields cannot be used
  std::string notes;         // usable, but something is missing
  int outside = 0;           // grid cells beyond the HRRR domain (the box reaches past its edge): clear
  std::string dynamicsError; // the dynamics response is not decodable (GRIB2 the reader does not support)
};

// GRIB2 (cloud fields + optional dynamics) -> CloudFields: bilinear resampling
// of the Lambert grid at the CloudGrid cell centres, per-layer altitudes,
// thickness and optical depth, layer winds, and the summary values.
Decoded decode(std::span<const std::byte> cloudBytes, std::span<const std::byte> dynamicsBytes,
               const CloudGrid& grid) {
  Decoded out;
  auto note = [&](const std::string& text) { out.notes += (out.notes.empty() ? "" : ", ") + text; };
  std::vector<Grib2::Field> grib;
  std::string error;
  if (!Grib2::decode(cloudBytes, grib, error)) {
    out.error = "GRIB2 decoding failed: " + error;
    return out;
  }
  auto find = [](const std::vector<Grib2::Field>& fields, int category, int parameter, int level,
                 double levelValue = -1.0) -> const Grib2::Field* {
    for (const Grib2::Field& f : fields) {
      if (f.discipline == 0 && f.category == category && f.parameter == parameter && f.levelType == level &&
          (levelValue < 0.0 || std::abs(f.levelValue - levelValue) < 1.0)) {
        return &f;
      }
    }
    return nullptr;
  };
  const Grib2::Field* tcdc = find(grib, 6, 1, Grib2::kLevelEntireAtmosphere);
  if (!tcdc) {
    out.error = "the response has no total cloud cover (TCDC) field (" + std::to_string(grib.size()) + " fields)";
    return out;
  }
  auto sameGrid = [&](const Grib2::Field* f, const std::string& name) -> const Grib2::Field* {
    const Grib2::LambertGrid& a = tcdc->grid;
    if (f && (f->grid.nx != a.nx || f->grid.ny != a.ny || f->grid.la1 != a.la1 || f->grid.lo1 != a.lo1 ||
              f->referenceTime != tcdc->referenceTime || f->forecastMinutes != tcdc->forecastMinutes)) {
      note(name + " on a different grid or time (ignored)");
      return nullptr;
    }
    if (!f) {
      note(name + " missing");
    }
    return f;
  };
  const Grib2::Field* layerCover[kCloudLayerCount] = {
      sameGrid(find(grib, 6, 3, Grib2::kLevelLowCloudLayer), "LCDC"),
      sameGrid(find(grib, 6, 4, Grib2::kLevelMiddleCloudLayer), "MCDC"),
      sameGrid(find(grib, 6, 5, Grib2::kLevelHighCloudLayer), "HCDC")};
  const Grib2::Field* base = sameGrid(find(grib, 3, 5, Grib2::kLevelCloudBase), "cloud base height");
  const Grib2::Field* top = sameGrid(find(grib, 3, 5, Grib2::kLevelCloudTop), "cloud top height");

  std::vector<Grib2::Field> dyn;
  const Grib2::Field* windU[kCloudLayerCount] = {};
  const Grib2::Field* windV[kCloudLayerCount] = {};
  const Grib2::Field* water = nullptr;
  const Grib2::Field* ice = nullptr;
  if (!dynamicsBytes.empty()) {
    std::string dynError;
    if (dynamicsBytes.size() < 4 || std::memcmp(dynamicsBytes.data(), "GRIB", 4) != 0) {
      note("dynamics response is not GRIB2: " + snippet(dynamicsBytes));
    } else if (!Grib2::decode(dynamicsBytes, dyn, dynError)) {
      out.dynamicsError = "dynamics GRIB2 decoding failed: " + dynError;
      dyn.clear();
    } else {
      const char* names[kCloudLayerCount] = {"850", "500", "250"};
      for (int l = 0; l < kCloudLayerCount; ++l) {
        windU[l] = sameGrid(find(dyn, 2, 2, Grib2::kLevelIsobaric, HrrrSource::kWindLevels[l]),
                            std::string("UGRD ") + names[l] + " mb");
        windV[l] = sameGrid(find(dyn, 2, 3, Grib2::kLevelIsobaric, HrrrSource::kWindLevels[l]),
                            std::string("VGRD ") + names[l] + " mb");
      }
      water = sameGrid(find(dyn, 1, 69, Grib2::kLevelEntireAtmosphere), "TCOLW");
      ice = sameGrid(find(dyn, 1, 70, Grib2::kLevelEntireAtmosphere), "TCOLI");
    }
  }

  auto fields = std::make_shared<CloudFields>();
  CloudFields& cf = *fields;
  cf.grid = grid;
  const CloudGrid& g = cf.grid;
  const double poiLon = g.centreLon(), poiLat = g.centreLat(); // the summaries are about the box centre
  const int nx = g.nx, ny = g.ny;
  const size_t n = size_t(nx) * size_t(ny);
  cf.cover.assign(n, 0.0f);
  cf.base.assign(n, kNaN);
  cf.top.assign(n, kNaN);
  for (CloudLayer& layer : cf.layers) {
    layer.cover.assign(n, 0.0f);
  }
  std::vector<float> waterPath(n, kNaN), icePath(n, kNaN);
  auto fraction = [](const Grib2::Field* f, double i, double j) {
    const float v = f ? f->sample(i, j) : kNaN;
    return std::isnan(v) ? 0.0f : std::clamp(v * 0.01f, 0.0f, 1.0f); // percent
  };

  // Layer winds per cell, earth-relative (the macro fields and the noise phases
  // drift with them: CSBlendClouds), and their mean within 200 km of the box
  // centre (HUD, summaries, the shadow map's change key).
  const double kmPerLon = 111.32 * std::cos(poiLat * kPi / 180.0);
  double windSum[kCloudLayerCount][2] = {};
  int windCount[kCloudLayerCount] = {};
  for (CloudLayer& layer : cf.layers) {
    layer.windEast.assign(n, kNaN);
    layer.windNorth.assign(n, kNaN);
  }
  int outside = 0;
  for (int y = 0; y < ny; ++y) {
    for (int x = 0; x < nx; ++x) {
      double i = 0.0, j = 0.0;
      tcdc->grid.project(g.lon(x), g.lat(y), i, j);
      const size_t k = size_t(y) * size_t(nx) + size_t(x);
      if (std::isnan(tcdc->sample(i, j))) {
        ++outside;
      }
      cf.cover[k] = fraction(tcdc, i, j);
      for (int l = 0; l < kCloudLayerCount; ++l) {
        cf.layers[size_t(l)].cover[k] = fraction(layerCover[l], i, j);
      }
      cf.base[k] = base ? base->sample(i, j) : kNaN;
      cf.top[k] = top ? top->sample(i, j) : kNaN;
      if (water && ice) {
        waterPath[k] = std::max(0.0f, water->sample(i, j));
        icePath[k] = std::max(0.0f, ice->sample(i, j));
      }
      const double dx = (g.lon(x) - poiLon) * kmPerLon, dy = (g.lat(y) - poiLat) * 111.32;
      const bool nearCentre = dx * dx + dy * dy <= 200.0 * 200.0;
      for (int l = 0; l < kCloudLayerCount; ++l) {
        if (!windU[l] || !windV[l]) {
          continue;
        }
        const float ug = windU[l]->sample(i, j), vg = windV[l]->sample(i, j);
        if (std::isnan(ug) || std::isnan(vg)) {
          continue;
        }
        const double a = windU[l]->grid.uvGridRelative ? windU[l]->grid.rotation(g.lon(x)) : 0.0;
        const double east = std::cos(a) * ug + std::sin(a) * vg, north = -std::sin(a) * ug + std::cos(a) * vg;
        cf.layers[size_t(l)].windEast[k] = float(east);
        cf.layers[size_t(l)].windNorth[k] = float(north);
        if (nearCentre) {
          windSum[l][0] += east;
          windSum[l][1] += north;
          ++windCount[l];
        }
      }
    }
  }
  // The filter clips the box to the HRRR domain: a few cells beyond it (the
  // New York box's north-eastern corner) are expected; many are not.
  out.outside = outside;
  if (outside > int(n / 4)) {
    note(std::to_string(outside) + " grid cells outside the HRRR subset (no clouds there)");
  }
  cf.hasWind = true;
  for (int l = 0; l < kCloudLayerCount; ++l) {
    CloudLayer& layer = cf.layers[size_t(l)];
    if (windCount[l] > 0) {
      layer.windU = float(windSum[l][0] / windCount[l]);
      layer.windV = float(windSum[l][1] / windCount[l]);
    } else {
      cf.hasWind = false;
    }
    // Cells without a wind (outside the subset, or no dynamics) take the mean;
    // a light blur keeps the drift field smooth across the HRRR cells.
    for (size_t k = 0; k < n; ++k) {
      if (std::isnan(layer.windEast[k]) || std::isnan(layer.windNorth[k])) {
        layer.windEast[k] = layer.windU;
        layer.windNorth[k] = layer.windV;
      }
    }
    boxBlur(layer.windEast, nx, ny, 2);
    boxBlur(layer.windNorth, nx, ny, 2);
  }
  cf.hasCondensate = water && ice;

  cf.source = "HRRR";
  cf.cycle = tcdc->referenceTime;
  cf.forecastHours = tcdc->forecastMinutes / 60;
  cf.validTime = tcdc->referenceTime + int64_t(tcdc->forecastMinutes) * 60;

  // Per-layer altitude (base) and thickness. HRRR's cloud base is the base of
  // the lowest layer present, the cloud top the top of the highest one, so a
  // cell tells the base of its lowest layer and (top - typical thickness) of
  // its highest; everything else - and every cell without that layer - is
  // filled in from the neighbourhood (push-pull) and smoothed.
  constexpr float kMinBase[kCloudLayerCount] = {100.0f, 2000.0f, 5000.0f};   // plausible bases (m MSL)
  constexpr float kMaxBase[kCloudLayerCount] = {4500.0f, 9000.0f, 14000.0f};
  constexpr float kDefaultBase[kCloudLayerCount] = {1000.0f, 4500.0f, 9500.0f};
  constexpr float kDefaultThickness[kCloudLayerCount] = {800.0f, 1500.0f, 1500.0f};
  constexpr float kPresent = 0.05f;
  std::array<std::vector<float>, kCloudLayerCount> altitude, altitudeWeight, thickness, thicknessWeight;
  for (int l = 0; l < kCloudLayerCount; ++l) {
    altitude[size_t(l)].assign(n, 0.0f);
    altitudeWeight[size_t(l)].assign(n, 0.0f);
    thickness[size_t(l)].assign(n, 0.0f);
    thicknessWeight[size_t(l)].assign(n, 0.0f);
  }
  for (size_t k = 0; k < n; ++k) {
    int lowest = -1, highest = -1, count = 0;
    for (int l = 0; l < kCloudLayerCount; ++l) {
      if (cf.layers[size_t(l)].cover[k] >= kPresent) {
        lowest = lowest < 0 ? l : lowest;
        highest = l;
        ++count;
      }
    }
    if (lowest < 0) {
      continue;
    }
    const float b = cf.base[k], t = cf.top[k];
    const size_t lo = size_t(lowest), hi = size_t(highest);
    if (!std::isnan(b) && b >= kMinBase[lo] && b <= kMaxBase[lo]) {
      altitude[lo][k] = b;
      altitudeWeight[lo][k] = cf.layers[lo].cover[k];
    }
    if (highest != lowest && !std::isnan(t)) {
      const float estimate = t - kDefaultThickness[hi];
      if (estimate >= kMinBase[hi] && estimate <= kMaxBase[hi]) {
        altitude[hi][k] = estimate;
        altitudeWeight[hi][k] = cf.layers[hi].cover[k];
      }
    }
    if (count == 1 && !std::isnan(b) && !std::isnan(t) && t > b) {
      thickness[lo][k] = std::clamp(t - b, 100.0f, 6000.0f);
      thicknessWeight[lo][k] = cf.layers[lo].cover[k];
    }
  }
  for (int l = 0; l < kCloudLayerCount; ++l) {
    CloudLayer& layer = cf.layers[size_t(l)];
    layer.altitude = std::move(altitude[size_t(l)]);
    layer.thickness = std::move(thickness[size_t(l)]);
    if (!fillGaps(layer.altitude, altitudeWeight[size_t(l)], nx, ny)) {
      layer.altitude.assign(n, kDefaultBase[l]);
    }
    if (!fillGaps(layer.thickness, thicknessWeight[size_t(l)], nx, ny)) {
      layer.thickness.assign(n, kDefaultThickness[l]);
    }
    for (int pass = 0; pass < 2; ++pass) {
      boxBlur(layer.altitude, nx, ny, 2);
      boxBlur(layer.thickness, nx, ny, 2);
    }
  }
  // Keep the layers apart and inside their NCEP pressure bands.
  cf.constrainLayers();

  // In-cloud optical depth: the larger of a thickness heuristic (step 1: water
  // clouds 3 + 4 per km, ice 1) and the cloud water / ice paths, tau = 3/2 LWP /
  // (rho r_e) = 150 LWP (water, r_e 10 um) and 55 IWP (ice, r_e 30 um), kg/m2.
  // HRRR's cover is largely sub-grid cloud without resolved condensate (TCOLW
  // ~0 where LCDC reports cloud), so the paths only add thicker clouds (fronts,
  // convection) on top. Water goes to the low and middle layers, ice to the high
  // (and partly the middle) layer by their covers; a column value spreads over
  // the cloudy part of the cell.
  constexpr float kTauMax[kCloudLayerCount] = {60.0f, 60.0f, 12.0f};
  constexpr float kTauClear[kCloudLayerCount] = {6.0f, 9.0f, 1.0f}; // where the layer has no cloud (filtering)
  for (CloudLayer& layer : cf.layers) {
    layer.opticalDepth.assign(n, 0.0f);
  }
  cf.singleOpticalDepth.assign(n, 0.0f);
  for (size_t k = 0; k < n; ++k) {
    const float c[3] = {cf.layers[0].cover[k], cf.layers[1].cover[k], cf.layers[2].cover[k]};
    float tau[3] = {3.0f + 4.0f * cf.layers[0].thickness[k] * 0.001f, 3.0f + 4.0f * cf.layers[1].thickness[k] * 0.001f,
                    1.0f};
    // The step-1 single layer: the heuristic alone, mixed by the layer covers.
    const float sum = c[0] + c[1] + c[2];
    cf.singleOpticalDepth[k] =
        sum < 0.02f ? kTauClear[0] : std::min((c[0] * tau[0] + c[1] * tau[1] + c[2] * tau[2]) / sum, kTauMax[0]);
    if (cf.hasCondensate) {
      const float w = waterPath[k], i = icePath[k];
      const float waterLow = c[0] + c[1] > 0.01f ? c[0] / (c[0] + c[1]) : 0.5f;
      const float iceHigh = c[2] + 0.5f * c[1] > 0.01f ? c[2] / (c[2] + 0.5f * c[1]) : 1.0f;
      tau[0] = std::max(tau[0], 150.0f * w * waterLow / std::max(c[0], 0.05f));
      tau[1] = std::max(tau[1], (150.0f * w * (1.0f - waterLow) + 55.0f * i * (1.0f - iceHigh)) / std::max(c[1], 0.05f));
      tau[2] = std::max(tau[2], 55.0f * i * iceHigh / std::max(c[2], 0.05f));
    }
    for (int l = 0; l < kCloudLayerCount; ++l) {
      cf.layers[size_t(l)].opticalDepth[k] = c[l] < 0.02f ? kTauClear[l] : std::min(tau[l], kTauMax[l]);
    }
  }

  // Summary: domain means, altitudes near the box centre, the layer seen from below there.
  cf.summarize();

  out.fields = std::move(fields);
  return out;
}

} // namespace HrrrSource

} // namespace

namespace {

struct ObserveResult {
  std::shared_ptr<CloudFields> fields;
  CloudBlend::Stats stats;
  std::string error;
};

} // namespace

struct WeatherService::FetchResult {
  int status = 0; // HTTP status of the cloud fields, 0 = no response
  size_t bytes = 0;
  std::shared_ptr<CloudFields> fields;
  std::string error;        // the response cannot be used
  bool formatError = false; // GRIB2 content the decoder does not understand
  std::string notes;        // usable, something missing
  std::string dynamicsError;
  bool dynamicsMissing = false; // no dynamics response (not requested, offline miss, HTTP error)
  int outside = 0;              // grid cells beyond the HRRR domain
};

WeatherService::WeatherService(std::shared_ptr<HttpAssetAccessor> accessor, AsyncSystem asyncSystem, Options options)
    : _accessor(std::move(accessor)), _asyncSystem(std::move(asyncSystem)), _options(std::move(options)) {
  GoesSource::Options goesOptions;
  goesOptions.offline = _options.offline;
  goesOptions.refreshSeconds = _options.refreshSeconds;
  goesOptions.grid = _options.grid;
  _goes = std::make_shared<GoesSource>(_accessor, _asyncSystem, goesOptions);
  if (_options.pinnedTime) {
    LOG_I("Clouds: NOAA HRRR, weather time pinned to {} (replay / forecast)", formatTime(*_options.pinnedTime));
  } else {
    LOG_I("Clouds: NOAA HRRR, live{}", _options.offline ? " from the disk cache" : "");
  }
}

int64_t WeatherService::time() const { return _options.pinnedTime ? *_options.pinnedTime : unixNow(); }

void WeatherService::setGrid(const CloudGrid& grid) {
  if (grid == _options.grid) {
    return;
  }
  _options.grid = grid;
  _goes->setGrid(grid);
  ++_generation;
  _loaded.clear();
  _summaries.clear();
  _cached.clear();
  _failed.clear();
  _observed.clear();
  _observing = 0;
  _observePending = false;
  _probeDone = false;
  _probeBusy = false;
  _fetchBusy = false;
  _pending = false;
  _activity.clear();
  _problem.clear();
  _reportedGap = 0;
  rebuildCatalog();
  LOG_I("Clouds: weather box moved to {:.3f}, {:.3f} (lon {:.2f}..{:.2f}, lat {:.2f}..{:.2f}): its datasets are "
        "downloaded again (or replayed from the disk cache)",
        grid.centreLon(), grid.centreLat(), grid.west, grid.east, grid.south, grid.north);
}

bool WeatherService::catalogReady() const {
  return _probeDone && (_options.offline || _listingDone || _listingFailed);
}

void WeatherService::setPinnedTime(std::optional<int64_t> t) {
  if (t == _options.pinnedTime) {
    return;
  }
  _options.pinnedTime = t;
  _reportedGap = 0;
  _problem.clear();
  // Debug level: a dragged time slider changes it every frame (callers log the result).
  if (t) {
    SPDLOG_LOGGER_DEBUG(Log::data(), "Clouds: weather time {} (--weather-time {})", formatTime(*t), formatCompact(*t));
  } else {
    SPDLOG_LOGGER_DEBUG(Log::data(), "Clouds: weather time live");
  }
}

void WeatherService::update(bool active, bool satellite) {
  _satellite = active && satellite;
  _goes->update(time(), _satellite);
  _observePending = false;
  if (!_probeDone && !_probeBusy) {
    startProbe();
  }
  if (!active) {
    _pending = false;
    return;
  }
  if (!_options.offline && !_listingBusy && Clock::now() >= _nextListing) {
    startListing();
  }
  if (!catalogReady()) {
    _pending = true;
    return;
  }

  const int64_t t = time();
  const std::vector<int64_t> needed = neededHours(t);
  _pending = false;
  if (needed.empty()) {
    if (hourStart(t) != _reportedGap) {
      _reportedGap = hourStart(t);
      _problem = "no HRRR data near " + formatTime(t);
      if (_options.offline) {
        LOG_I("Clouds: {} in the disk cache (offline); run once online to cache the weather", _problem);
      } else {
        SPDLOG_LOGGER_WARN(Log::data(),
                           "Clouds: {} (NOMADS keeps ~2 days + forecasts; older hours only from the disk cache)",
                           _problem);
      }
    }
    return;
  }
  const auto now = Clock::now();
  for (const int64_t v : needed) {
    const std::optional<Candidate> want = best(v);
    if (!want) {
      continue;
    }
    const auto loaded = _loaded.find(v);
    if (loaded != _loaded.end() && loaded->second.candidate.cycle >= want->cycle) {
      continue;
    }
    const auto failed = _failed.find(v);
    if (failed != _failed.end() && failed->second.first == *want && now < failed->second.second) {
      continue;
    }
    _pending = true;
    if (!_fetchBusy) {
      fetch(v, *want);
    }
    break;
  }
  // Keep the datasets nearest to the weather time (the bracket + some history for scrubbing).
  while (_loaded.size() > 8) {
    auto farthest = _loaded.begin();
    for (auto it = _loaded.begin(); it != _loaded.end(); ++it) {
      if (std::llabs(it->first - t) > std::llabs(farthest->first - t)) {
        farthest = it;
      }
    }
    _loaded.erase(farthest);
  }
  updateObserved(t);
}

void WeatherService::updateObserved(int64_t t) {
  // The corrections wait for the final datasets and scan of t.
  if (!_satellite || _fetchBusy || _pending || _goes->busy()) {
    return;
  }
  const std::shared_ptr<const GoesScan> scan = _goes->scan(t);
  if (!scan) {
    return;
  }
  const Bracket model = modelBracket(t);
  for (const std::shared_ptr<const CloudFields>& f : {model.before, model.after}) {
    if (!f || CloudBlend::timeWeight(f->validTime, scan->start) <= 0.0) {
      continue;
    }
    const Observed* o = findObserved(f, scan);
    if (!o) {
      observe(f, scan);
      _observePending = true;
    } else if (!o->fields && !o->failed) {
      _observePending = true;
    }
  }
  // Keep the bracket's corrections and a few older ones (scrubbing back).
  constexpr size_t kMaxObserved = 4;
  for (auto it = _observed.begin(); _observed.size() > kMaxObserved && it != _observed.end();) {
    const bool current = it->scan == scan && (it->model == model.before || it->model == model.after);
    const bool building = !it->fields && !it->failed;
    it = current || building ? std::next(it) : _observed.erase(it);
  }
}

const WeatherService::Observed* WeatherService::findObserved(const std::shared_ptr<const CloudFields>& model,
                                                             const std::shared_ptr<const GoesScan>& scan) const {
  for (const Observed& o : _observed) {
    if (o.model == model && o.scan == scan) {
      return &o;
    }
  }
  return nullptr;
}

void WeatherService::observe(std::shared_ptr<const CloudFields> model, std::shared_ptr<const GoesScan> scan) {
  _observed.push_back(Observed{model, scan, nullptr, false});
  ++_observing;
  const int generation = _generation;
  std::weak_ptr<WeatherService> weak = weak_from_this();
  _asyncSystem
      .runInWorkerThread([model, scan]() {
        ObserveResult r;
        r.fields = CloudBlend::blend(*model, *scan, r.stats, r.error);
        return r;
      })
      .thenInMainThread([weak, generation, model, scan](ObserveResult&& r) {
        const std::shared_ptr<WeatherService> self = weak.lock();
        if (!self || self->_generation != generation) {
          return; // the box moved meanwhile
        }
        --self->_observing;
        auto it = std::find_if(self->_observed.begin(), self->_observed.end(),
                               [&](const Observed& o) { return o.model == model && o.scan == scan; });
        if (it == self->_observed.end()) {
          return;
        }
        const std::string what = "HRRR " + formatTime(model->cycle).substr(0, 10) + " " +
                                 describeRun(model->cycle, model->forecastHours) + " + GOES-19 scan " +
                                 formatTime(scan->start).substr(11, 5);
        if (!r.fields) {
          it->failed = true; // shown without the scan
          SPDLOG_LOGGER_ERROR(Log::data(), "Clouds: {}: {}", what, r.error);
          return;
        }
        const CloudBlend::Stats& s = r.stats;
        LOG_I("Clouds: {} -> valid {} (the scan carried {:+.0f} min by the layer winds, weight {:.2f}): cover low "
              "{:.0f} -> {:.0f} %, middle {:.0f} -> {:.0f} %, high {:.0f} -> {:.0f} %; observed from above {:.0f} / "
              "{:.0f} / {:.0f} %; tops set {:.0f} / {:.0f} / {:.0f} %; optical depth from COD {:.0f} % (daylight "
              "{:.0f} %); seen from below near the box centre: {}; {:.0f} ms",
              what, formatTime(model->validTime).substr(11, 5), s.minutes, s.weight, s.coverBefore[0] * 100.0,
              s.coverAfter[0] * 100.0, s.coverBefore[1] * 100.0, s.coverAfter[1] * 100.0, s.coverBefore[2] * 100.0,
              s.coverAfter[2] * 100.0, s.seen[0] * 100.0, s.seen[1] * 100.0, s.seen[2] * 100.0, s.topShare[0] * 100.0,
              s.topShare[1] * 100.0, s.topShare[2] * 100.0, s.opticalDepthShare * 100.0, s.daylight * 100.0,
              r.fields->layerName, s.ms);
        it->fields = std::move(r.fields);
      });
}

std::vector<int64_t> WeatherService::neededHours(int64_t t) const {
  std::vector<int64_t> out;
  const auto after = std::upper_bound(_available.begin(), _available.end(), t);
  const bool haveBefore = after != _available.begin() && t - *std::prev(after) <= kMaxBracketGap;
  const bool haveAfter = after != _available.end() && *after - t <= kMaxBracketGap;
  if (haveBefore) {
    out.push_back(*std::prev(after));
  }
  if (haveAfter && !(haveBefore && *std::prev(after) == t)) { // exactly on an hour: that one alone
    out.push_back(*after);
  }
  // Live: late in the hour the next hour as well, so the roll-over finds it loaded.
  if (live() && haveBefore && haveAfter && t - *std::prev(after) >= 45 * 60) {
    const auto next = std::next(after);
    if (next != _available.end() && *next - t <= kMaxBracketGap) {
      out.push_back(*next);
    }
  }
  return out;
}

std::optional<WeatherService::Candidate> WeatherService::best(int64_t validTime) const {
  std::optional<Candidate> b;
  if (const auto it = _listed.find(validTime); it != _listed.end()) {
    b = it->second;
  }
  if (const auto it = _cached.find(validTime); it != _cached.end() && (!b || it->second.cycle > b->cycle)) {
    b = it->second;
  }
  return b;
}

std::vector<WeatherService::HourInfo> WeatherService::catalog() const {
  std::vector<HourInfo> out;
  out.reserve(_available.size());
  for (const int64_t v : _available) {
    HourInfo info;
    info.validTime = v;
    const auto loaded = _loaded.find(v);
    if (const std::optional<Candidate> c = best(v)) {
      info.cycle = c->cycle;
      info.forecastHours = c->forecastHours;
    } else if (loaded != _loaded.end()) {
      info.cycle = loaded->second.candidate.cycle;
      info.forecastHours = loaded->second.candidate.forecastHours;
    }
    info.listed = _listed.count(v) != 0;
    info.cached = _cached.count(v) != 0;
    info.loaded = loaded != _loaded.end();
    if (const auto s = _summaries.find(v); s != _summaries.end()) {
      info.summary = &s->second;
    }
    out.push_back(info);
  }
  return out;
}

int64_t WeatherService::newestRun() const {
  int64_t newest = 0;
  for (const auto& [v, c] : _listed) {
    newest = std::max(newest, c.cycle);
  }
  for (const auto& [v, c] : _cached) {
    newest = std::max(newest, c.cycle);
  }
  return newest;
}

WeatherService::Bracket WeatherService::bracket(int64_t t) const {
  Bracket b = modelBracket(t);
  const std::shared_ptr<const GoesScan> scan = goesScan(t);
  if (!scan) {
    return b;
  }
  for (std::shared_ptr<const CloudFields>* f : {&b.before, &b.after}) {
    if (const Observed* o = *f ? findObserved(*f, scan) : nullptr; o && o->fields) {
      *f = o->fields;
    }
  }
  return b;
}

WeatherService::Bracket WeatherService::modelBracket(int64_t t) const {
  Bracket b;
  const auto after = _loaded.upper_bound(t);
  if (after != _loaded.begin() && t - std::prev(after)->first <= kMaxBracketGap) {
    b.before = std::prev(after)->second.fields;
  }
  if (after != _loaded.end() && after->first - t <= kMaxBracketGap) {
    b.after = after->second.fields;
  }
  return b;
}

void WeatherService::rebuildCatalog() {
  _available.clear();
  for (const auto& [v, c] : _listed) {
    _available.push_back(v);
  }
  for (const auto& [v, c] : _cached) {
    _available.push_back(v);
  }
  for (const auto& [v, l] : _loaded) {
    _available.push_back(v);
  }
  std::sort(_available.begin(), _available.end());
  _available.erase(std::unique(_available.begin(), _available.end()), _available.end());
}

void WeatherService::startProbe() {
  // Which hours can be replayed from the disk cache: the newest cached run per
  // valid hour, 30 days back (analysis + short forecasts, what live and replay
  // download) and around now (+ longer forecasts, pinned future hours).
  _probeBusy = true;
  const int64_t now = unixNow();
  const int generation = _generation;
  std::weak_ptr<WeatherService> weak = weak_from_this();
  _asyncSystem
      .runInWorkerThread([accessor = _accessor, now, grid = _options.grid]() {
        std::map<int64_t, Candidate> found;
        const int64_t first = hourStart(now) - 30 * 86400, last = hourStart(now) + 2 * 86400;
        for (int64_t v = first; v <= last; v += kHour) {
          const int maxForecast = v >= now - 86400 ? HrrrSource::kMaxForecastHours : 3;
          for (int f = 0; f <= maxForecast; ++f) {
            const int64_t cycle = v - int64_t(f) * kHour;
            if (HrrrSource::validRun(cycle, f) && accessor->isCached(HrrrSource::dataUrl(cycle, f, grid))) {
              found[v] = Candidate{cycle, f}; // smallest forecast hour = newest run
              break;
            }
          }
        }
        return found;
      })
      .thenInMainThread([weak, generation](std::map<int64_t, Candidate>&& found) {
        const std::shared_ptr<WeatherService> self = weak.lock();
        if (!self || self->_generation != generation) {
          return; // the box moved meanwhile: a new probe runs for it
        }
        for (const auto& [v, c] : found) {
          const auto it = self->_cached.find(v);
          if (it == self->_cached.end() || c.cycle > it->second.cycle) {
            self->_cached[v] = c;
          }
        }
        self->_probeBusy = false;
        self->_probeDone = true;
        self->rebuildCatalog();
        if (found.empty()) {
          LOG_I("Clouds: no HRRR data in the disk cache");
        } else {
          LOG_I("Clouds: the disk cache holds HRRR data for {} hours ({} ... {})", found.size(),
                formatTime(found.begin()->first), formatTime(found.rbegin()->first));
        }
      });
}

void WeatherService::startListing() {
  _listingBusy = true;
  const std::string url = HrrrSource::kFilter;
  std::weak_ptr<WeatherService> weak = weak_from_this();
  // Listings change every few minutes: bypass the disk cache.
  _accessor->get(_asyncSystem, url, {{"Cache-Control", "no-cache"}})
      .thenInMainThread([weak, url](std::shared_ptr<IAssetRequest>&& pRequest) {
        const std::shared_ptr<WeatherService> self = weak.lock();
        if (!self) {
          return;
        }
        const IAssetResponse* pResponse = pRequest ? pRequest->response() : nullptr;
        const std::vector<int64_t> days = pResponse && pResponse->statusCode() == 200
                                              ? HrrrSource::parseDays(pResponse->data())
                                              : std::vector<int64_t>{};
        if (days.empty()) {
          self->_problem = "NOMADS directory list unavailable (" +
                           (!pResponse                           ? std::string("no response")
                            : pResponse->statusCode() != 200     ? "HTTP " + std::to_string(pResponse->statusCode())
                                                                 : std::string("no HRRR days listed")) +
                           ")";
          SPDLOG_LOGGER_WARN(Log::data(), "Clouds: {}, retrying in 2 min: {}", self->_problem, url);
          self->finishListing(false);
          return;
        }
        // Days no longer listed were purged; complete days need no new listing.
        for (auto it = self->_dayRuns.begin(); it != self->_dayRuns.end();) {
          it = std::binary_search(days.begin(), days.end(), it->first) ? std::next(it) : self->_dayRuns.erase(it);
        }
        std::vector<int64_t> toList;
        for (auto it = days.rbegin(); it != days.rend(); ++it) {
          const auto known = self->_dayRuns.find(*it);
          if (known == self->_dayRuns.end() || !HrrrSource::dayComplete(known->second)) {
            toList.push_back(*it);
          }
        }
        self->listDays(std::move(toList), 0);
      });
}

void WeatherService::listDays(std::vector<int64_t> days, size_t index) {
  if (index >= days.size()) {
    finishListing(true);
    return;
  }
  const int64_t day = days[index];
  const std::string url = HrrrSource::listUrl(day);
  std::weak_ptr<WeatherService> weak = weak_from_this();
  _accessor->get(_asyncSystem, url, {{"Cache-Control", "no-cache"}})
      .thenInMainThread([weak, url, day, days = std::move(days), index](std::shared_ptr<IAssetRequest>&& pRequest) mutable {
        const std::shared_ptr<WeatherService> self = weak.lock();
        if (!self) {
          return;
        }
        const IAssetResponse* pResponse = pRequest ? pRequest->response() : nullptr;
        if (!pResponse || pResponse->statusCode() != 200) {
          self->_problem = "NOMADS file list unavailable (" +
                           (pResponse ? "HTTP " + std::to_string(pResponse->statusCode()) : std::string("no response")) +
                           ")";
          SPDLOG_LOGGER_WARN(Log::data(), "Clouds: {}, retrying in 2 min: {}", self->_problem, url);
          self->finishListing(false);
          return;
        }
        self->_dayRuns[day] = HrrrSource::parseListing(pResponse->data());
        self->listDays(std::move(days), index + 1);
      });
}

void WeatherService::finishListing(bool ok) {
  _listingBusy = false;
  (ok ? _listingDone : _listingFailed) = true;
  _listed.clear();
  int64_t newestRun = 0;
  for (const auto& [day, runs] : _dayRuns) {
    for (const auto& [hour, forecast] : runs) {
      const int64_t cycle = day + int64_t(hour) * kHour;
      if (!HrrrSource::validRun(cycle, forecast)) {
        continue;
      }
      newestRun = std::max(newestRun, cycle);
      Candidate& c = _listed[cycle + int64_t(forecast) * kHour];
      if (cycle > c.cycle) {
        c = Candidate{cycle, forecast};
      }
    }
  }
  rebuildCatalog();
  // A run is published ~50 min after its hour: once the current hour's run is
  // listed nothing newer can appear before the next hour.
  const int64_t now = unixNow();
  double wait = 120.0;
  if (ok) {
    wait = newestRun >= hourStart(now) ? double(hourStart(now) + kHour + 45 * 60 - now) : _options.refreshSeconds;
    SPDLOG_LOGGER_DEBUG(Log::data(), "Clouds: NOMADS lists {} days, newest run {}, {} valid hours; next check in {:.0f} min",
                        _dayRuns.size(), newestRun ? formatTime(newestRun) : std::string("none"), _listed.size(),
                        wait / 60.0);
  }
  _nextListing = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(wait));
}

void WeatherService::fetch(int64_t validTime, Candidate c) {
  _fetchBusy = true;
  const Utc v = toUtc(validTime);
  char what[64];
  snprintf(what, sizeof(what), "HRRR %s for %02d:00", describeRun(c.cycle, c.forecastHours).c_str(), v.hour);
  _activity = what;
  // The dynamics only where they can exist: cached, or a run NOMADS lists
  // (older step-1 caches hold cloud fields only; a purged file would answer 404).
  const CloudGrid grid = _options.grid;
  const int generation = _generation;
  const std::string dynamicsUrl = HrrrSource::dynamicsUrl(c.cycle, c.forecastHours, grid);
  const auto listed = _listed.find(validTime);
  const bool requestDynamics =
      _accessor->isCached(dynamicsUrl) || (!_options.offline && listed != _listed.end() && listed->second == c);
  std::vector<Future<std::shared_ptr<IAssetRequest>>> requests;
  requests.push_back(_accessor->get(_asyncSystem, HrrrSource::dataUrl(c.cycle, c.forecastHours, grid)));
  if (requestDynamics) {
    requests.push_back(_accessor->get(_asyncSystem, dynamicsUrl));
  }
  std::weak_ptr<WeatherService> weak = weak_from_this();
  _asyncSystem.all(std::move(requests))
      .thenInWorkerThread([grid](std::vector<std::shared_ptr<IAssetRequest>>&& responses) {
        FetchResult result;
        const IAssetResponse* pClouds = responses[0] ? responses[0]->response() : nullptr;
        const IAssetResponse* pDynamics =
            responses.size() > 1 && responses[1] ? responses[1]->response() : nullptr;
        if (!pClouds) {
          return result;
        }
        result.status = pClouds->statusCode();
        result.bytes = pClouds->data().size();
        if (result.status != 200) {
          return result;
        }
        const std::span<const std::byte> body = pClouds->data();
        if (body.size() < 4 || std::memcmp(body.data(), "GRIB", 4) != 0) {
          result.error = "the response is not GRIB2: " + snippet(body);
          return result;
        }
        std::span<const std::byte> dynamics;
        if (pDynamics && pDynamics->statusCode() == 200) {
          dynamics = pDynamics->data();
          result.bytes += dynamics.size();
        } else {
          result.dynamicsMissing = true;
        }
        HrrrSource::Decoded decoded = HrrrSource::decode(body, dynamics, grid);
        result.fields = std::move(decoded.fields);
        result.error = std::move(decoded.error);
        result.formatError = !result.error.empty();
        result.notes = std::move(decoded.notes);
        result.dynamicsError = std::move(decoded.dynamicsError);
        result.outside = decoded.outside;
        return result;
      })
      .thenInMainThread([weak, validTime, c, generation](FetchResult&& result) {
        const std::shared_ptr<WeatherService> self = weak.lock();
        if (!self || self->_generation != generation) {
          return; // the box moved meanwhile: this dataset is for the old one
        }
        self->_fetchBusy = false;
        self->_activity.clear();
        const std::string run = formatTime(c.cycle).substr(0, 10) + " " + describeRun(c.cycle, c.forecastHours);
        if (result.fields) {
          CloudFields& f = *result.fields;
          if (f.cycle != c.cycle || f.forecastHours != c.forecastHours) {
            SPDLOG_LOGGER_WARN(Log::data(), "Clouds: requested HRRR {}, the file holds {} {}", run,
                               formatTime(f.cycle), describeRun(f.cycle, f.forecastHours));
          }
          if (!result.dynamicsError.empty()) {
            SPDLOG_LOGGER_ERROR(Log::data(), "Clouds: HRRR {}: {}", run, result.dynamicsError);
          }
          if (result.dynamicsMissing) {
            // Datasets cached by step 1 have no dynamics: expected offline / for purged runs.
            LOG_I("Clouds: HRRR {}: no layer winds / cloud water (cloud fields only): shapes do not drift", run);
          } else if (!result.notes.empty()) {
            SPDLOG_LOGGER_WARN(Log::data(), "Clouds: HRRR {}: {}", run, result.notes);
          }
          auto layerText = [&](int l) {
            const CloudLayer& layer = f.layers[size_t(l)];
            const double speed = std::hypot(layer.windU, layer.windV);
            const double from = std::fmod(std::atan2(-layer.windU, -layer.windV) * 180.0 / kPi + 360.0, 360.0);
            double tau = 0.0, weight = 0.0;
            for (size_t k = 0; k < layer.cover.size(); ++k) {
              tau += layer.cover[k] * layer.opticalDepth[k];
              weight += layer.cover[k];
            }
            char buf[96];
            snprintf(buf, sizeof(buf), "%.0f %% at %.1f km, tau %.1f", layer.meanCover * 100.0,
                     layer.altitudeNearPoi / 1000.0, weight > 0.0 ? tau / weight : 0.0);
            std::string text = buf;
            if (f.hasWind) {
              snprintf(buf, sizeof(buf), ", wind %.0f m/s from %.0f deg", speed, from);
              text += buf;
            }
            return text;
          };
          const std::string outside =
              result.outside > 0 ? "; " + std::to_string(result.outside) + " cells beyond the HRRR domain" : "";
          LOG_I("Clouds: HRRR {} -> valid {} ({:.0f} KB): cover {:.0f} %; low {}; middle {}; high {}; seen from "
                "below near the box centre: {}{}; replay with --weather-time {}",
                run, formatTime(f.validTime), double(result.bytes) / 1024.0, f.meanCover * 100.0, layerText(0),
                layerText(1), layerText(2), f.layerName, outside, formatCompact(f.validTime));
          HourSummary& summary = self->_summaries[validTime];
          summary.meanCover = f.meanCover;
          for (int l = 0; l < kCloudLayerCount; ++l) {
            summary.layerCover[l] = f.layers[size_t(l)].meanCover;
            summary.layerAltitude[l] = f.layers[size_t(l)].altitudeNearPoi;
          }
          summary.layerName = f.layerName;
          summary.hasWind = f.hasWind;
          self->_loaded[validTime] = Loaded{std::move(result.fields), c};
          const auto cached = self->_cached.find(validTime);
          if (cached == self->_cached.end() || c.cycle > cached->second.cycle) {
            self->_cached[validTime] = c;
          }
          self->_failed.erase(validTime);
          self->_problem.clear();
          self->rebuildCatalog();
          return;
        }

        const std::string what = !result.error.empty() ? result.error
                                 : result.status == 0  ? std::string("no response")
                                                       : "HTTP " + std::to_string(result.status);
        if (result.formatError) {
          SPDLOG_LOGGER_ERROR(Log::data(), "Clouds: HRRR {}: {}", run, what);
        } else if (!result.error.empty()) {
          SPDLOG_LOGGER_WARN(Log::data(), "Clouds: HRRR {}: {}", run, what);
        } else {
          // HTTP failures are already logged by the accessor (offline misses at debug level).
          SPDLOG_LOGGER_DEBUG(Log::data(), "Clouds: HRRR {} not available ({})", run, what);
        }
        self->_failed[validTime] = {c, Clock::now() + std::chrono::minutes(2)};
        self->_problem = "HRRR " + describeRun(c.cycle, c.forecastHours) + " failed (" + what + ")";
      });
}

std::string WeatherService::activity() const {
  std::string s;
  auto add = [&](const std::string& part) {
    if (!part.empty()) {
      s += (s.empty() ? "" : " | ") + part;
    }
  };
  if (_probeBusy) {
    add("searching the disk cache");
  }
  if (_listingBusy) {
    add("checking NOMADS for runs");
  }
  if (_fetchBusy) {
    add("loading " + _activity);
  }
  add(_problem);
  if (_satellite) {
    add(_goes->activity());
  }
  if (_observing > 0) {
    add("blending the GOES-19 scan into HRRR");
  }
  return s;
}

std::string WeatherService::describeRun(int64_t cycle, int forecastHours) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%02dz +%d h", toUtc(cycle).hour, forecastHours);
  return buf;
}

std::optional<int64_t> WeatherService::parseTime(const std::string& text) {
  if (text.size() >= 2 && text.size() <= 4 && text[0] == '-' &&
      std::all_of(text.begin() + 1, text.end(), [](char c) { return std::isdigit(uint8_t(c)); })) {
    const int64_t hoursAgo = std::stoi(text.substr(1));
    return (unixNow() + 1800) / 3600 * 3600 - hoursAgo * 3600; // N hours before the current (nearest) hour
  }
  std::string t = text;
  if (!t.empty() && (t.back() == 'z' || t.back() == 'Z')) {
    t.pop_back();
  }
  if ((t.size() != 10 && t.size() != 12) ||
      !std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit(uint8_t(c)); })) {
    return std::nullopt;
  }
  using namespace std::chrono;
  const int y = std::stoi(t.substr(0, 4)), m = std::stoi(t.substr(4, 2)), d = std::stoi(t.substr(6, 2)),
            h = std::stoi(t.substr(8, 2)), minute = t.size() == 12 ? std::stoi(t.substr(10, 2)) : 0;
  const year_month_day ymd{std::chrono::year{y}, std::chrono::month{unsigned(m)}, std::chrono::day{unsigned(d)}};
  if (!ymd.ok() || h > 23 || minute > 59) {
    return std::nullopt;
  }
  return int64_t(sys_days(ymd).time_since_epoch().count()) * 86400 + int64_t(h) * 3600 + int64_t(minute) * 60;
}

std::string WeatherService::formatTime(int64_t t) {
  const Utc u = toUtc(t);
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d UTC", u.year, u.month, u.day, u.hour, u.minute);
  return buf;
}

std::string WeatherService::formatCompact(int64_t t) {
  const Utc u = toUtc(t);
  char buf[16];
  if (u.minute == 0) {
    snprintf(buf, sizeof(buf), "%04d%02d%02d%02d", u.year, u.month, u.day, u.hour);
  } else {
    snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d", u.year, u.month, u.day, u.hour, u.minute);
  }
  return buf;
}
