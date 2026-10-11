#include "pch.h"
#include "NetcdfInspect.h"

#include "Hdf5.h"
#include "Log.h"
#include "Renderer.h"

#include <CesiumAsync/IAssetRequest.h>
#include <CesiumAsync/IAssetResponse.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>

namespace {

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

} // namespace

void inspectNetcdf(const std::shared_ptr<HttpAssetAccessor>& accessor, const CesiumAsync::AsyncSystem& asyncSystem,
                   const std::string& source, std::vector<std::string> images, std::filesystem::path imageDir) {
  std::string name = source.substr(source.find_last_of("/\\") + 1);
  name = name.substr(0, name.find('?'));
  if (source.rfind("http://", 0) == 0 || source.rfind("https://", 0) == 0) {
    accessor->get(asyncSystem, source)
        .thenInWorkerThread([source, name, images, imageDir](std::shared_ptr<CesiumAsync::IAssetRequest>&& pRequest) {
          const CesiumAsync::IAssetResponse* pResponse = pRequest ? pRequest->response() : nullptr;
          if (!pResponse || pResponse->statusCode() != 200) {
            SPDLOG_LOGGER_ERROR(Log::data(), "NetCDF {}: {} ({})", name,
                                pResponse ? "HTTP " + std::to_string(pResponse->statusCode()) : "no response", source);
            return std::vector<Image>{};
          }
          const auto body = pResponse->data();
          return inspect(std::vector<std::byte>(body.begin(), body.end()), name, images, imageDir);
        })
        .thenInMainThread([](std::vector<Image>&& out) { writeImages(out); });
    return;
  }
  asyncSystem
      .runInWorkerThread([source, name, images, imageDir]() {
        std::ifstream in(std::filesystem::path(source), std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in.good() && !in.eof()) {
          raw.clear();
        }
        if (raw.empty()) {
          SPDLOG_LOGGER_ERROR(Log::data(), "NetCDF {}: cannot read {}", name, source);
          return std::vector<Image>{};
        }
        std::vector<std::byte> data(raw.size());
        std::memcpy(data.data(), raw.data(), raw.size());
        return inspect(std::move(data), name, images, imageDir);
      })
      .thenInMainThread([](std::vector<Image>&& out) { writeImages(out); });
}
