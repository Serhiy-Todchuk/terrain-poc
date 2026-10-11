#include "pch.h"
#include "NetcdfInspect.h"

#include "GeoRef.h"
#include "GoesGrid.h"
#include "Hdf5.h"
#include "Log.h"
#include "Renderer.h"
#include "WeatherService.h"

#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <optional>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Image {
  std::filesystem::path path;
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> bgr;
  std::string range; // the value range mapped to black..white
};

double millisecondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Greyscale picture of a 2D field: min..max -> black..white, NaN -> dark red.
Image makeImage(const std::vector<double>& values, uint64_t rows, uint64_t columns, double lo, double hi) {
  Image image;
  image.width = uint32_t(columns);
  image.height = uint32_t(rows);
  image.bgr.resize(size_t(rows * columns) * 3);
  const double scale = hi > lo ? 255.0 / (hi - lo) : 0.0;
  for (size_t i = 0; i < values.size(); ++i) {
    uint8_t* px = image.bgr.data() + i * 3;
    if (std::isnan(values[i])) {
      px[0] = 0;
      px[1] = 0;
      px[2] = 110;
    } else {
      px[0] = px[1] = px[2] = uint8_t(std::clamp((values[i] - lo) * scale + 0.5, 0.0, 255.0));
    }
  }
  return image;
}

// One file's report, logged as one block at the end (files inspected in
// parallel would otherwise interleave line by line).
class Report {
public:
  enum class Level { Debug, Info, Error };
  void add(Level level, std::string line) { _lines.emplace_back(level, std::move(line)); }
  ~Report() {
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    for (const auto& [level, line] : _lines) {
      if (level == Level::Error) {
        SPDLOG_LOGGER_ERROR(Log::data(), "{}", line);
      } else if (level == Level::Info) {
        SPDLOG_LOGGER_INFO(Log::data(), "{}", line);
      } else {
        SPDLOG_LOGGER_DEBUG(Log::data(), "{}", line);
      }
    }
  }

private:
  std::vector<std::pair<Level, std::string>> _lines;
};

// Parses and logs the file (worker thread); returns the requested images.
std::vector<Image> inspect(std::vector<std::byte> data, const std::string& name, const std::vector<std::string>& images,
                           const std::filesystem::path& imageDir) {
  using Level = Report::Level;
  Report report;
  const size_t bytes = data.size();
  const auto t0 = std::chrono::steady_clock::now();
  Hdf5::File file;
  std::string error;
  if (!file.open(std::move(data), error)) {
    report.add(Level::Error, fmt::format("NetCDF {}: {}", name, error));
    return {};
  }
  report.add(Level::Info, fmt::format("NetCDF {}: {} bytes, {} variables, {} global attributes (structure read in "
                                      "{:.1f} ms)",
                                      name, bytes, file.variables().size(), file.globalAttributes().size(),
                                      millisecondsSince(t0)));
  for (const char* key : {"title", "dataset_name", "platform_ID", "scene_id", "spatial_resolution",
                          "time_coverage_start", "time_coverage_end"}) {
    if (const Hdf5::Attribute* a = file.globalAttribute(key)) {
      report.add(Level::Info, fmt::format("  {} = {}", key, a->describe()));
    }
  }
  for (const Hdf5::Attribute& a : file.globalAttributes()) {
    report.add(Level::Debug, fmt::format("  @{} = {}", a.name, a.describe()));
  }

  std::vector<Image> out;
  for (const Hdf5::Variable& v : file.variables()) {
    std::string line = "  " + v.name + " " + v.describe();
    const bool numeric = v.type.cls == Hdf5::Datatype::Class::Integer || v.type.cls == Hdf5::Datatype::Class::Float;
    const bool wanted = std::find(images.begin(), images.end(), v.name) != images.end();
    bool failed = false;
    if (v.hasData() && numeric) {
      const auto t1 = std::chrono::steady_clock::now();
      std::vector<double> values;
      if (!file.readValues(v, {}, {}, values, error)) {
        failed = true;
      } else if (values.size() <= 8) {
        std::string list;
        for (const double x : values) {
          char buf[32];
          snprintf(buf, sizeof(buf), "%.9g", x);
          list += (list.empty() ? "" : ", ") + std::string(buf);
        }
        line += " = " + list;
      } else {
        size_t valid = 0;
        double lo = std::numeric_limits<double>::infinity(), hi = -lo, sum = 0.0;
        for (const double x : values) {
          if (!std::isnan(x)) {
            ++valid;
            lo = std::min(lo, x);
            hi = std::max(hi, x);
            sum += x;
          }
        }
        char buf[200];
        snprintf(buf, sizeof(buf), ": %zu valid (%.1f %%), min %.6g max %.6g mean %.6g (decoded in %.1f ms)", valid,
                 100.0 * double(valid) / double(values.size()), valid ? lo : 0.0, valid ? hi : 0.0,
                 valid ? sum / double(valid) : 0.0, millisecondsSince(t1));
        line += buf;
        if (v.layout == Hdf5::Variable::Layout::Chunked && v.dims.size() == 2) {
          // Self-check of the hyperslab path: a window across a chunk boundary
          // must equal the same part of the full read.
          const uint64_t rows = v.dims[0], cols = v.dims[1];
          const uint64_t r0 = v.chunk[0] > rows / 4 ? std::min(v.chunk[0], rows) - rows / 4 : 0;
          const uint64_t start[2] = {r0, cols / 3}, count[2] = {std::min(rows - r0, rows / 2), cols / 3};
          std::vector<double> window;
          size_t mismatches = 0;
          if (!file.readValues(v, start, count, window, error)) {
            report.add(Level::Error, fmt::format("NetCDF {}: window read: {}", name, error));
          } else {
            for (uint64_t r = 0; r < count[0]; ++r) {
              for (uint64_t c = 0; c < count[1]; ++c) {
                const double a = window[r * count[1] + c], b = values[(start[0] + r) * cols + start[1] + c];
                mismatches += !(a == b || (std::isnan(a) && std::isnan(b)));
              }
            }
            report.add(mismatches ? Level::Error : Level::Debug,
                       fmt::format("NetCDF {}: {} window rows {}+{} cols {}+{}: {} mismatches with the full read",
                                   name, v.name, start[0], count[0], start[1], count[1], mismatches));
          }
        }
        if (wanted && v.dims.size() == 2) {
          Image image = makeImage(values, v.dims[0], v.dims[1], lo, hi);
          image.path = imageDir / (std::filesystem::path(name).stem().string() + "_" + v.name + ".png");
          snprintf(buf, sizeof(buf), "%.6g .. %.6g", lo, hi);
          image.range = buf;
          out.push_back(std::move(image));
        }
      }
      if (const Hdf5::Attribute* units = v.attribute("units")) {
        line += " " + units->text();
      }
    }
    report.add(failed ? Level::Error : Level::Info,
               failed ? fmt::format("NetCDF {}: {} ({})", name, error, v.describe()) : line);
    for (const Hdf5::Attribute& a : v.attributes) {
      report.add(Level::Debug, fmt::format("    @{} = {}", a.name, a.describe()));
    }
    if (wanted && v.dims.size() != 2) {
      report.add(Level::Error, fmt::format("NetCDF {}: {} has {} dimensions, no image", name, v.name, v.dims.size()));
    }
  }
  for (const std::string& wanted : images) {
    if (!file.variable(wanted)) {
      report.add(Level::Error, fmt::format("NetCDF {}: no variable '{}'", name, wanted));
    }
  }
  return out;
}

void writeImages(const std::vector<Image>& images) {
  for (const Image& image : images) {
    if (Renderer::writePng(image.path.wstring(), image.width, image.height, image.bgr.data())) {
      SPDLOG_LOGGER_INFO(Log::data(), "NetCDF image {} ({}x{}, {} -> black..white, missing dark red)",
                         image.path.string(), image.width, image.height, image.range);
    } else {
      SPDLOG_LOGGER_ERROR(Log::data(), "NetCDF image {} could not be written", image.path.string());
    }
  }
}

// A file's bytes from the accessor (URL) or the disk (path); empty after an
// error (logged).
struct Loaded {
  std::string name; // file name
  std::vector<std::byte> data;
};

CesiumAsync::Future<Loaded> load(const std::shared_ptr<HttpAssetAccessor>& accessor,
                                 const CesiumAsync::AsyncSystem& asyncSystem, const std::string& source) {
  std::string name = source.substr(source.find_last_of("/\\") + 1);
  name = name.substr(0, name.find('?'));
  if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
    return accessor->get(asyncSystem, source)
        .thenInWorkerThread([source, name](std::shared_ptr<CesiumAsync::IAssetRequest>&& pRequest) {
          const CesiumAsync::IAssetResponse* pResponse = pRequest ? pRequest->response() : nullptr;
          if (!pResponse || pResponse->statusCode() != 200) {
            SPDLOG_LOGGER_ERROR(Log::data(), "NetCDF {}: {} ({})", name,
                                pResponse ? "HTTP " + std::to_string(pResponse->statusCode()) : "no response", source);
            return Loaded{name, {}};
          }
          const auto body = pResponse->data();
          return Loaded{name, std::vector<std::byte>(body.begin(), body.end())};
        });
  }
  return asyncSystem.runInWorkerThread([source, name]() {
    std::ifstream in(std::filesystem::path(source), std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
      raw.clear();
    }
    if (raw.empty()) {
      SPDLOG_LOGGER_ERROR(Log::data(), "NetCDF {}: cannot read {}", name, source);
      return Loaded{name, {}};
    }
    Loaded out{name, std::vector<std::byte>(raw.size())};
    std::memcpy(out.data.data(), raw.data(), raw.size());
    return out;
  });
}

// ---- a GOES scan on the weather box (script command `goes`) ----------------------

// Coastal landmarks on the grid images (lon, lat): the coastline of a surface
// image (band 13 brightness temperature at night: land colder than water) must
// pass them where the sky is clear.
constexpr double kLandmarks[][2] = {
    {-74.0075, 40.6600}, // the POI, Bush Terminal piers
    {-71.857, 41.071},   // Montauk Point
    {-74.960, 38.933},   // Cape May Point
    {-75.088, 38.799},   // Cape Henlopen
    {-70.243, 42.081},   // Race Point, Cape Cod
    {-70.100, 41.283},   // Nantucket
    {-76.006, 36.928},   // Cape Henry
    {-70.585, 42.637},   // Cape Ann
    {-70.208, 43.623},   // Portland Head
    {-79.070, 43.262},   // Niagara River mouth, Lake Ontario
    {-76.510, 43.464},   // Oswego, Lake Ontario
    {-76.480, 44.230},   // Kingston, Lake Ontario
    {-78.900, 42.880},   // Buffalo, Lake Erie
};

// How each product is drawn: its main variable and display range (NaN = the
// range of the values; brightness temperatures inverted: cold clouds white).
// Surface imagery gets no parallax correction (the coastline check).
struct ProductView {
  const char* product;
  const char* variable;
  double lo, hi;
  bool parallax;
};
constexpr double kAuto = std::numeric_limits<double>::quiet_NaN();
constexpr ProductView kProductViews[] = {
    {"ACHAC", "HT", 0.0, 15000.0, true}, {"CCLC", "TCF", 0.0, 1.0, true}, {"ACMC", "BCM", 0.0, 1.0, true},
    {"CODC", "COD", 0.0, 40.0, true},    {"CMIPC", "CMI", kAuto, kAuto, false},
};

// "OR_ABI-L2-ACHAC-M6_G19_s20262840001181_e..." -> "ACHAC" and the scan start
// (unix seconds; year, day of year, hour, minute, second).
bool parseGoesName(const std::string& name, std::string& product, int64_t& start) {
  const size_t p = name.find("ABI-L2-");
  const size_t q = p == std::string::npos ? p : name.find("-M", p + 7);
  const size_t s = q == std::string::npos ? q : name.find("_s", q);
  if (s == std::string::npos || s + 15 > name.size()) {
    return false;
  }
  auto digits = [&](size_t at, int count) {
    int v = 0;
    for (int i = 0; i < count; ++i) {
      const char c = name[at + size_t(i)];
      if (c < '0' || c > '9') {
        return -1;
      }
      v = v * 10 + (c - '0');
    }
    return v;
  };
  const int year = digits(s + 2, 4), day = digits(s + 6, 3), hour = digits(s + 9, 2), minute = digits(s + 11, 2),
            second = digits(s + 13, 2);
  if (year < 1970 || day < 1 || hour < 0 || minute < 0 || second < 0) {
    return false;
  }
  int64_t days = 0;
  for (int y = 1970; y < year; ++y) {
    days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
  }
  product = name.substr(p + 7, q - p - 7);
  start = (days + day - 1) * 86400 + hour * 3600 + minute * 60 + second;
  return true;
}

std::optional<double> scalarValue(const Hdf5::File& file, std::string_view name) {
  const Hdf5::Variable* v = file.variable(name);
  std::vector<double> values;
  std::string error;
  if (!v || v->count() != 1 || !file.readValues(*v, {}, {}, values, error) || values.size() != 1 ||
      std::isnan(values[0])) {
    return std::nullopt;
  }
  return values[0];
}

// Picture of a field on CloudGrid, twice the cell resolution: lo..hi ->
// black..white (reversed with `invert`), NaN dark red, a 1-degree graticule
// (blue) and the landmarks (green crosses with an open centre).
Image gridImage(const std::vector<float>& values, const CloudGrid& g, double lo, double hi, bool invert) {
  const int nx = g.nx, ny = g.ny;
  std::vector<uint8_t> bgr(size_t(nx) * size_t(ny) * 3);
  const double scale = hi > lo ? 1.0 / (hi - lo) : 0.0;
  const double cellLon = (g.east - g.west) / nx, cellLat = (g.north - g.south) / ny;
  for (int y = 0; y < ny; ++y) {
    const bool parallel = std::floor(g.lat(y) - 0.5 * cellLat) != std::floor(g.lat(y) + 0.5 * cellLat);
    for (int x = 0; x < nx; ++x) {
      const bool meridian = std::floor(g.lon(x) - 0.5 * cellLon) != std::floor(g.lon(x) + 0.5 * cellLon);
      const size_t k = size_t(y) * size_t(nx) + size_t(x);
      uint8_t* px = bgr.data() + k * 3;
      if (std::isnan(values[k])) {
        px[0] = 0;
        px[1] = 0;
        px[2] = 110;
      } else {
        const double t = std::clamp((values[k] - lo) * scale, 0.0, 1.0);
        px[0] = px[1] = px[2] = uint8_t((invert ? 1.0 - t : t) * 255.0 + 0.5);
      }
      if (parallel || meridian) {
        px[0] = uint8_t((px[0] + 255) / 2);
        px[1] = uint8_t((px[1] + 90) / 2);
        px[2] = uint8_t(px[2] / 2);
      }
    }
  }
  for (const auto& l : kLandmarks) {
    const int cx = int(std::floor((l[0] - g.west) / cellLon)), cy = int(std::floor((g.north - l[1]) / cellLat));
    for (int d = -5; d <= 5; ++d) {
      if (std::abs(d) < 2) {
        continue;
      }
      for (const auto& [x, y] : {std::pair{cx + d, cy}, std::pair{cx, cy + d}}) {
        if (x >= 0 && y >= 0 && x < nx && y < ny) {
          uint8_t* px = bgr.data() + (size_t(y) * size_t(nx) + size_t(x)) * 3;
          px[0] = 0;
          px[1] = 255;
          px[2] = 0;
        }
      }
    }
  }
  Image image;
  image.width = uint32_t(nx * 2);
  image.height = uint32_t(ny * 2);
  image.bgr.resize(size_t(image.width) * image.height * 3);
  for (uint32_t y = 0; y < image.height; ++y) {
    for (uint32_t x = 0; x < image.width; ++x) {
      std::memcpy(image.bgr.data() + (size_t(y) * image.width + x) * 3,
                  bgr.data() + (size_t(y / 2) * size_t(nx) + x / 2) * 3, 3);
    }
  }
  char buf[64];
  snprintf(buf, sizeof(buf), invert ? "%.6g .. %.6g (inverted)" : "%.6g .. %.6g", lo, hi);
  image.range = buf;
  return image;
}

struct OpenFile {
  std::string name, product;
  int64_t start = 0;
  Hdf5::File file;
  const ProductView* view = nullptr;
};

// The file's navigation against its own metadata: the image centre against
// geospatial_lat_center / lon_center, the pixel centres on the Earth against
// the extent (and the cloud mask's pixel count), a round trip ground point ->
// scan angles over every pixel.
void checkNavigation(const OpenFile& f, const Goes::Projection& proj, Report& report) {
  using Level = Report::Level;
  const Hdf5::Variable* extent = f.file.variable("geospatial_lat_lon_extent");
  auto extentValue = [&](const char* key) -> std::optional<double> {
    const Hdf5::Attribute* a = extent ? extent->attribute(key) : nullptr;
    return a ? a->number() : std::nullopt;
  };
  const std::optional<double> xc = scalarValue(f.file, "x_image"), yc = scalarValue(f.file, "y_image");
  const std::optional<double> lonC = extentValue("geospatial_lon_center"), latC = extentValue("geospatial_lat_center");
  double lon = 0.0, lat = 0.0;
  if (xc && yc && lonC && latC && proj.groundPoint(*xc, *yc, lon, lat)) {
    const double d = std::max(std::abs(lon - *lonC), std::abs(lat - *latC));
    report.add(d > 1e-3 ? Level::Error : Level::Info,
               fmt::format("GOES {}: image centre ({:.6f}, {:.6f}) rad -> {:.5f} E {:.5f} N, file {:.5f} / {:.5f} "
                           "(difference {:.1e} deg)",
                           f.product, *xc, *yc, lon, lat, *lonC, *latC, d));
  } else {
    report.add(Level::Error, fmt::format("GOES {}: no image centre / geospatial_lat_lon_extent to check the "
                                         "navigation against",
                                         f.product));
  }

  const Hdf5::Variable* xv = f.file.variable("x");
  const Hdf5::Variable* yv = f.file.variable("y");
  std::vector<double> xs, ys;
  std::string error;
  if (!xv || !yv || !f.file.readValues(*xv, {}, {}, xs, error) || !f.file.readValues(*yv, {}, {}, ys, error)) {
    report.add(Level::Error, fmt::format("GOES {}: x / y axes: {}", f.product, error.empty() ? "missing" : error));
    return;
  }
  size_t onEarth = 0, lost = 0;
  double lonMin = 1e9, lonMax = -1e9, latMin = 1e9, latMax = -1e9, roundTrip = 0.0;
  for (const double y : ys) {
    for (const double x : xs) {
      if (!proj.groundPoint(x, y, lon, lat)) {
        continue;
      }
      ++onEarth;
      lonMin = std::min(lonMin, lon);
      lonMax = std::max(lonMax, lon);
      latMin = std::min(latMin, lat);
      latMax = std::max(latMax, lat);
      // Back from the ground point; also below the ellipsoid (the surface is up
      // to ~110 m below it: geoid), where the point must stay visible too.
      double x2 = 0.0, y2 = 0.0, x3 = 0.0, y3 = 0.0;
      if (proj.scanAngles(lon, lat, 0.0, x2, y2) && proj.scanAngles(lon, lat, -110.0, x3, y3)) {
        roundTrip = std::max({roundTrip, std::abs(x2 - x), std::abs(y2 - y)});
      } else {
        ++lost;
      }
    }
  }
  const std::optional<double> west = extentValue("geospatial_westbound_longitude"),
                              east = extentValue("geospatial_eastbound_longitude"),
                              south = extentValue("geospatial_southbound_latitude"),
                              north = extentValue("geospatial_northbound_latitude");
  // Pixel centres must lie inside the file's extent (which NOAA computes on a
  // finer grid towards the limb; the 2 km centres match it at the other edges).
  const bool inside = west && east && south && north && lonMin >= *west - 0.01 && lonMax <= *east + 0.01 &&
                      latMin >= *south - 0.01 && latMax <= *north + 0.01;
  report.add(inside && lost == 0 && roundTrip < 1e-9 ? Level::Info : Level::Error,
             fmt::format("GOES {}: {}x{} pixel centres, {} on the Earth: lon {:.5f} .. {:.5f}, lat {:.5f} .. {:.5f} "
                         "(file extent {:.5f} .. {:.5f}, {:.5f} .. {:.5f}: {}); round trip max error {:.2e} rad, {} "
                         "hidden on the way back (at 0 / -110 m)",
                         f.product, xs.size(), ys.size(), onEarth, lonMin, lonMax, latMin, latMax, west.value_or(0.0),
                         east.value_or(0.0), south.value_or(0.0), north.value_or(0.0),
                         inside ? "inside" : "OUTSIDE", roundTrip, lost));
  if (const std::optional<double> points = scalarValue(f.file, "total_number_of_cloud_mask_points")) {
    const double d = std::abs(double(onEarth) - *points);
    report.add(d > std::max(2.0, 1e-5 * *points) ? Level::Error : Level::Info,
               fmt::format("GOES {}: pixels on the Earth {} vs total_number_of_cloud_mask_points {:.0f}", f.product,
                           onEarth, *points));
  }
}

// Logs the scan's navigation and parallax checks and returns its grid images
// (worker thread).
std::vector<Image> inspectScan(std::vector<Loaded>& loaded, const CloudGrid& grid,
                               const std::vector<std::shared_ptr<const CloudFields>>& hrrr,
                               const std::filesystem::path& imageDir) {
  using Level = Report::Level;
  Report report;
  std::vector<OpenFile> files;
  for (Loaded& l : loaded) {
    if (l.data.empty()) {
      continue; // logged by load
    }
    OpenFile f;
    f.name = l.name;
    std::string error;
    if (!parseGoesName(l.name, f.product, f.start)) {
      report.add(Level::Error, fmt::format("GOES {}: not a GOES-R L2 file name (OR_ABI-L2-<product>-M<mode>_G<nn>_s"
                                           "<start>_...)",
                                           l.name));
      continue;
    }
    if (!f.file.open(std::move(l.data), error)) {
      report.add(Level::Error, fmt::format("GOES {}: {}", l.name, error));
      continue;
    }
    for (const ProductView& v : kProductViews) {
      if (f.product == v.product) {
        f.view = &v;
      }
    }
    if (!f.view) {
      report.add(Level::Error, fmt::format("GOES {}: no view for product {} (known: ACHAC, CCLC, ACMC, CODC, CMIPC)",
                                           l.name, f.product));
      continue;
    }
    files.push_back(std::move(f));
  }
  if (files.empty()) {
    report.add(Level::Error, "GOES scan: no usable file");
    return {};
  }
  // Cloud-top heights first: their lines of sight correct the other products.
  std::stable_sort(files.begin(), files.end(),
                   [](const OpenFile& a, const OpenFile& b) { return a.product == "ACHAC" && b.product != "ACHAC"; });
  const int64_t start = files.front().start;
  for (const OpenFile& f : files) {
    if (std::abs(f.start - start) > 60) {
      report.add(Level::Error, fmt::format("GOES {}: scan start {} differs from {} (files of different scans)",
                                           f.product, WeatherService::formatTime(f.start),
                                           WeatherService::formatTime(start)));
    }
  }
  const std::string tag = "goes_" + WeatherService::formatCompact(start);
  report.add(Level::Info, fmt::format("GOES scan {} ({} files) on the weather box {:.2f} .. {:.2f} E, {:.2f} .. "
                                      "{:.2f} N ({}x{} cells)",
                                      WeatherService::formatTime(start), files.size(), grid.west, grid.east,
                                      grid.south, grid.north, grid.nx, grid.ny));
  Goes::Projection proj;
  std::string error;
  if (!proj.read(files.front().file, error)) {
    report.add(Level::Error, fmt::format("GOES {}: {}", files.front().product, error));
    return {};
  }
  report.add(Level::Info, fmt::format("GOES projection: sub-satellite longitude {} deg, perspective point height {:.0f} "
                                      "m, axes {:.3f} / {:.5f} m",
                                      proj.lonOrigin, proj.perspectivePointHeight, proj.semiMajor, proj.semiMinor));

  // The Product User Guide's worked example (GOES-East, GRS80).
  if (proj.lonOrigin == -75.0 && std::abs(proj.semiMajor - 6378137.0) < 1.0 &&
      std::abs(proj.semiMinor - 6356752.31414) < 1.0 && std::abs(proj.perspectivePointHeight - 35786023.0) < 1.0) {
    double x = 0.0, y = 0.0, lon = 0.0, lat = 0.0;
    const bool seen = proj.scanAngles(-84.690932, 33.846162, 0.0, x, y);
    const bool back = seen && proj.groundPoint(-0.024052, 0.095340, lon, lat);
    const double dx = std::abs(x + 0.024052), dy = std::abs(y - 0.095340);
    report.add(seen && back && dx <= 1e-6 && dy <= 1e-6 ? Level::Info : Level::Error,
               fmt::format("GOES navigation: PUG example 33.846162 N 84.690932 W -> x {:.7f} y {:.7f} rad "
                           "(published -0.024052 / 0.095340: differences {:.2f} / {:.2f} urad); back: {:.5f} N "
                           "{:.5f} W",
                           x, y, dx * 1e6, dy * 1e6, lat, -lon));
  }
  for (const OpenFile& f : files) {
    checkNavigation(f, proj, report);
  }
  // Parallax geometry at the box centre: where a cloud top 10 km above it
  // appears on the ground, against the flat-Earth estimate h * tan(zenith).
  {
    const double lon = grid.centreLon(), lat = grid.centreLat();
    const double zenith = proj.viewZenith(lon, lat);
    double x = 0.0, y = 0.0, apparentLon = 0.0, apparentLat = 0.0;
    if (proj.scanAngles(lon, lat, GeoRef::navd88ToEllipsoid(10000.0, lon, lat), x, y) &&
        proj.groundPoint(x, y, apparentLon, apparentLat)) {
      const double north = (apparentLat - lat) * 111.32, east = (apparentLon - lon) * 111.32 * std::cos(lat * kPi / 180.0);
      report.add(Level::Info,
                 fmt::format("GOES parallax at the box centre ({:.3f}, {:.3f}): satellite zenith angle {:.2f} deg; a "
                             "cloud top 10 km above it appears {:.2f} km away towards {:.1f} deg (10 km x "
                             "tan(zenith) = {:.2f} km)",
                             lon, lat, zenith, std::hypot(north, east),
                             std::fmod(std::atan2(east, north) * 180.0 / kPi + 360.0, 360.0),
                             10.0 * std::tan(zenith * kPi / 180.0)));
    } else {
      report.add(Level::Error, "GOES parallax: the box centre cannot be seen from the satellite");
    }
  }

  std::vector<Image> out;
  auto addImage = [&](const std::vector<float>& values, const std::string& suffix, double lo, double hi, bool invert) {
    Image image = gridImage(values, grid, lo, hi, invert);
    image.path = imageDir / (tag + "_" + suffix + ".png");
    out.push_back(std::move(image));
  };
  std::vector<float> lineOfSight, topsFlat, topsCorrected, maskFlat, maskCorrected;
  for (const OpenFile& f : files) {
    const ProductView& view = *f.view;
    Goes::Raster raster;
    const auto t0 = std::chrono::steady_clock::now();
    if (!raster.read(f.file, view.variable, &grid, error)) {
      report.add(Level::Error, fmt::format("GOES {}: {}", f.product, error));
      continue;
    }
    const double readMs = millisecondsSince(t0);
    const auto t1 = std::chrono::steady_clock::now();
    const std::vector<float> flat = Goes::resample(raster, grid, {}, 0.0f);
    std::vector<float> corrected;
    std::string parallax;
    if (f.product == "ACHAC") {
      Goes::ParallaxStats st;
      corrected = Goes::cloudTopHeights(raster, grid, &st, &lineOfSight);
      const size_t solved = size_t(std::count_if(lineOfSight.begin(), lineOfSight.end(),
                                                 [](float h) { return !std::isnan(h); }));
      parallax = fmt::format("; parallax: {} cells solved, cloud tops on {}, shift mean {:.2f} km (north {:.2f}, east "
                             "{:.2f}) max {:.2f} km, {} unconverged (> 100 m)",
                             solved, st.cloudy, st.meanShiftKm, st.meanShiftNorthKm, st.meanShiftEastKm,
                             st.maxShiftKm, st.unconverged);
      topsFlat = flat;
      topsCorrected = corrected;
    } else if (view.parallax && !lineOfSight.empty()) {
      corrected = Goes::resample(raster, grid, lineOfSight);
    }
    const double resampleMs = millisecondsSince(t1);
    double lo = view.lo, hi = view.hi;
    const bool invert = std::isnan(lo) && raster.units == "K";
    if (std::isnan(lo)) {
      lo = std::numeric_limits<double>::infinity();
      hi = -lo;
      for (const float v : flat) {
        if (!std::isnan(v)) {
          lo = std::min(lo, double(v));
          hi = std::max(hi, double(v));
        }
      }
      if (!(lo <= hi)) {
        lo = 0.0;
        hi = 1.0;
      }
    }
    size_t valid = 0, validCorrected = 0, changed = 0;
    double sum = 0.0, sumCorrected = 0.0;
    for (size_t k = 0; k < flat.size(); ++k) {
      if (!std::isnan(flat[k])) {
        ++valid;
        sum += flat[k];
      }
      if (!corrected.empty()) {
        if (!std::isnan(corrected[k])) {
          ++validCorrected;
          sumCorrected += corrected[k];
        }
        changed += std::isnan(flat[k]) != std::isnan(corrected[k]) ||
                   std::abs(flat[k] - corrected[k]) > 0.25 * (hi - lo);
      }
    }
    std::string line = fmt::format("GOES {} {}: window rows {}+{} cols {}+{} (read {:.1f} ms); on the grid {} valid "
                                   "({:.1f} %), mean {:.4g} {}",
                                   f.product, view.variable, raster.row0, raster.rows, raster.column0, raster.columns,
                                   readMs, valid, 100.0 * double(valid) / double(flat.size()),
                                   valid ? sum / double(valid) : 0.0, raster.units);
    if (!corrected.empty()) {
      line += fmt::format("; parallax-corrected {} valid, mean {:.4g}, {} cells changed by > 25 % of {:.4g} .. "
                          "{:.4g} or validity ({:.1f} %)",
                          validCorrected, validCorrected ? sumCorrected / double(validCorrected) : 0.0, changed, lo,
                          hi, 100.0 * double(changed) / double(flat.size()));
    }
    report.add(Level::Info, line + parallax + fmt::format(" (resampled in {:.0f} ms)", resampleMs));
    addImage(flat, f.product + "_" + view.variable + "_flat", lo, hi, invert);
    if (!corrected.empty()) {
      addImage(corrected, f.product + "_" + view.variable + "_parallax", lo, hi, invert);
    }
    if (f.product == "ACMC") {
      maskFlat = flat;
      maskCorrected = corrected;
    }
  }

  // The HRRR dataset of the same time on the same box.
  std::shared_ptr<const CloudFields> model;
  for (const std::shared_ptr<const CloudFields>& h : hrrr) {
    if (h && h->grid == grid && std::abs(h->validTime - start) <= 1800 &&
        (!model || std::abs(h->validTime - start) < std::abs(model->validTime - start))) {
      model = h;
    }
  }
  if (!model) {
    report.add(Level::Info, fmt::format("GOES scan {}: no HRRR dataset of this box valid within 30 min (pin the "
                                        "weather time and wait first)",
                                        WeatherService::formatTime(start)));
    return out;
  }
  addImage(model->cover, "hrrr_TCDC", 0.0, 1.0, false);
  // Cells agreeing on cloudy (cover >= 50 %) or clear; the cloud mask's NaN is
  // missing (skipped), a cloud top's NaN is clear.
  auto agreement = [&](const std::vector<float>& goes, bool tops) {
    size_t both = 0, agree = 0;
    for (size_t k = 0; k < goes.size() && k < model->cover.size(); ++k) {
      if (!tops && std::isnan(goes[k])) {
        continue;
      }
      const bool goesCloudy = tops ? !std::isnan(goes[k]) : goes[k] >= 0.5f;
      ++both;
      agree += goesCloudy == (model->cover[k] >= 0.5f);
    }
    return both ? 100.0 * double(agree) / double(both) : 0.0;
  };
  std::string line = fmt::format("GOES scan vs HRRR {} valid {}: HRRR mean cover {:.3f}; cells agreeing on cloudy "
                                 "(>= 50 %) / clear:",
                                 WeatherService::describeRun(model->cycle, model->forecastHours),
                                 WeatherService::formatTime(model->validTime), model->meanCover);
  if (!maskFlat.empty()) {
    line += fmt::format(" cloud mask {:.1f} % uncorrected, {:.1f} % parallax-corrected;", agreement(maskFlat, false),
                        maskCorrected.empty() ? 0.0 : agreement(maskCorrected, false));
  }
  if (!topsFlat.empty()) {
    line += fmt::format(" cloud tops {:.1f} % uncorrected, {:.1f} % parallax-corrected", agreement(topsFlat, true),
                        agreement(topsCorrected, true));
  }
  report.add(Level::Info, line);
  return out;
}

} // namespace

void inspectNetcdf(const std::shared_ptr<HttpAssetAccessor>& accessor, const CesiumAsync::AsyncSystem& asyncSystem,
                   const std::string& source, std::vector<std::string> images, std::filesystem::path imageDir) {
  load(accessor, asyncSystem, source)
      .thenInWorkerThread([images = std::move(images), imageDir = std::move(imageDir)](Loaded&& file) {
        return file.data.empty() ? std::vector<Image>{} : inspect(std::move(file.data), file.name, images, imageDir);
      })
      .thenInMainThread([](std::vector<Image>&& out) { writeImages(out); });
}

void inspectGoesScan(const std::shared_ptr<HttpAssetAccessor>& accessor, const CesiumAsync::AsyncSystem& asyncSystem,
                     std::vector<std::string> sources, const CloudGrid& grid,
                     std::vector<std::shared_ptr<const CloudFields>> hrrr, std::filesystem::path imageDir) {
  std::vector<CesiumAsync::Future<Loaded>> files;
  files.reserve(sources.size());
  for (const std::string& source : sources) {
    files.push_back(load(accessor, asyncSystem, source));
  }
  asyncSystem.all(std::move(files))
      .thenInWorkerThread([grid, hrrr = std::move(hrrr), imageDir = std::move(imageDir)](std::vector<Loaded>&& loaded) {
        return inspectScan(loaded, grid, hrrr, imageDir);
      })
      .thenInMainThread([](std::vector<Image>&& out) { writeImages(out); });
}
