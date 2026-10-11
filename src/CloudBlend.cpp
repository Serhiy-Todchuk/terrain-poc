#include "pch.h"
#include "CloudBlend.h"

#include "Ephemeris.h"
#include "GridFill.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadius = 6371000.0;                 // as the renderer's CSBlendClouds
constexpr int64_t kFullWeight = 90 * 60, kZeroWeight = 3 * 3600; // the scan's reach in time (s)
// The CCLC height bands of each layer: low = CF1 + CF2 (up to 10 000 ft), middle =
// CF3 + CF4 (up to 24 000 ft), high = CF5; a cloud-top height belongs to the
// layer of its band.
constexpr int kFirstBand[kCloudLayerCount] = {0, 2, 4}, kLastBand[kCloudLayerCount] = {1, 3, 4};
constexpr float kMinThickness[kCloudLayerCount] = {300.0f, 500.0f, 500.0f}; // slab under an observed top (m)
constexpr float kMinBase[kCloudLayerCount] = {150.0f, 2000.0f, 5500.0f};    // as CloudFields::constrainLayers
constexpr float kMinObservedTau = 0.5f, kMaxObservedTau = 60.0f;
constexpr double kDayZenith = 65.0, kNightZenith = 82.0; // CODC: good-quality day region, day algorithm limit (deg)
constexpr float kMaskClear = 0.05f; // the mask's 10 km mean below this: clear there (no sharpening)
constexpr float kCloudy = 0.2f;     // band cover from which a layer can be the uppermost cloud
constexpr float kTopMargin = 500.0f; // a cloud-top height fits a layer this far beyond its bands (m)

using GridFill::boxBlur;
using GridFill::fillGaps;

int layerOfHeight(float h) {
  return h < kGoesBandTop[kLastBand[0]] ? 0 : h < kGoesBandTop[kLastBand[1]] ? 1 : 2;
}

// A cloud-top height inside the layer's bands (+ margin). HT is resampled from
// 10 km pixels: between a high and a low top it takes values in between, which
// must not become the top of the middle layer.
bool topFits(float h, int layer) {
  const float bottom = layer == 0 ? -1e9f : kGoesBandTop[kFirstBand[layer] - 1];
  const float top = kGoesBandTop[kLastBand[layer]];
  return h >= bottom - kTopMargin && h < top + kTopMargin;
}

// Bilinear interpolation at fractional cell coordinates (cell centres at
// integers), edges clamped: the four cells and their weights.
struct Taps {
  size_t k[4];
  float w[4];
};

Taps taps(int nx, int ny, double x, double y) {
  x = std::clamp(x, 0.0, double(nx - 1));
  y = std::clamp(y, 0.0, double(ny - 1));
  const int x0 = std::min(int(x), std::max(nx - 2, 0)), y0 = std::min(int(y), std::max(ny - 2, 0));
  const int x1 = std::min(x0 + 1, nx - 1), y1 = std::min(y0 + 1, ny - 1);
  const float fx = float(x - x0), fy = float(y - y0);
  auto at = [&](int i, int j) { return size_t(j) * size_t(nx) + size_t(i); };
  return {{at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)},
          {(1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy}};
}

float sample(const std::vector<float>& v, const Taps& t) {
  return v[t.k[0]] * t.w[0] + v[t.k[1]] * t.w[1] + v[t.k[2]] * t.w[2] + v[t.k[3]] * t.w[3];
}

// Weight-normalised value and the interpolated weight (0 where no tap has weight).
std::pair<float, float> sampleWeighted(const std::vector<float>& v, const std::vector<float>& weight, const Taps& t) {
  float sum = 0.0f, w = 0.0f;
  for (int i = 0; i < 4; ++i) {
    const float wi = t.w[i] * weight[t.k[i]];
    sum += wi * v[t.k[i]];
    w += wi;
  }
  return {w > 0.0f ? sum / w : 0.0f, w};
}

// Where the air at every cell was dt seconds earlier (dt > 0; later for dt < 0),
// moving with the layer's wind field: two iterations as in CSBlendClouds. NaN
// (as x) where that point lies outside the grid.
std::vector<std::pair<double, double>> departures(const CloudLayer& layer, const CloudGrid& g, double dt) {
  const int nx = g.nx, ny = g.ny;
  const double dLon = (g.east - g.west) / nx * kPi / 180.0, dLat = (g.north - g.south) / ny * kPi / 180.0;
  std::vector<std::pair<double, double>> out(size_t(nx) * size_t(ny));
  for (int y = 0; y < ny; ++y) {
    const double lat = g.lat(y) * kPi / 180.0;
    const double sx = dt / (kEarthRadius * std::cos(lat) * dLon), sy = dt / (kEarthRadius * dLat);
    for (int x = 0; x < nx; ++x) {
      const size_t k = size_t(y) * size_t(nx) + size_t(x);
      double px = x - layer.windEast[k] * sx, py = y + layer.windNorth[k] * sy; // rows run south
      const Taps t = taps(nx, ny, px, py);
      px = x - sample(layer.windEast, t) * sx;
      py = y + sample(layer.windNorth, t) * sy;
      const bool inside = px >= -0.5 && px <= nx - 0.5 && py >= -0.5 && py <= ny - 0.5;
      out[k] = {inside ? px : std::numeric_limits<double>::quiet_NaN(), py};
    }
  }
  return out;
}

// One layer as the scan saw it, on the scan's time.
struct Observed {
  std::vector<float> cover;     // 0..1 where valid
  std::vector<float> valid;     // 1 = the bands have data
  std::vector<float> seen;      // share of the layer visible from above (no observed cloud over it)
  std::vector<float> top;       // m MSL: the uppermost cloud's top where it is in this layer, filled elsewhere
  std::vector<float> topWeight; // smoothed: this layer is the uppermost cloud (its cover) with a fitting top
  std::vector<float> tau;       // COD where this layer is the uppermost cloud
  std::vector<float> tauWeight; // daylight weight there
};

} // namespace

namespace CloudBlend {

double timeWeight(int64_t validTime, int64_t scanStart) {
  const double d = std::abs(double(validTime - scanStart));
  return std::clamp((double(kZeroWeight) - d) / double(kZeroWeight - kFullWeight), 0.0, 1.0);
}

std::shared_ptr<CloudFields> blend(const CloudFields& model, const GoesScan& scan, Stats& stats, std::string& error) {
  const auto t0 = Clock::now();
  const CloudGrid& g = model.grid;
  const int nx = g.nx, ny = g.ny;
  const size_t n = size_t(nx) * size_t(ny);
  if (!(scan.grid == g) || scan.cloudTop.size() != n || model.cover.size() != n) {
    error = "the scan and the dataset are on different grids";
    return nullptr;
  }
  bool haveBands = (scan.products & (1 << kGoesCoverLayers)) != 0;
  for (const std::vector<float>& band : scan.bandCover) {
    haveBands = haveBands && band.size() == n;
  }
  const bool haveMask = (scan.products & (1 << kGoesCloudMask)) != 0 && scan.cloudMask.size() == n;
  const bool haveTau = (scan.products & (1 << kGoesOpticalDepth)) != 0 && scan.opticalDepth.size() == n;

  stats = Stats{};
  stats.minutes = double(model.validTime - scan.start) / 60.0;
  stats.weight = timeWeight(model.validTime, scan.start);
  auto out = std::make_shared<CloudFields>(model);
  CloudFields& f = *out;
  f.source = model.source + " + GOES-19";
  f.observedTime = scan.start;
  f.observedWeight = stats.weight;
  for (int l = 0; l < kCloudLayerCount; ++l) {
    stats.coverBefore[l] = stats.coverAfter[l] = model.layers[size_t(l)].meanCover;
  }
  if (stats.weight <= 0.0) {
    stats.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return out;
  }

  // The 2 km mask over its 10 km mean: sharpens the 10 km bands (1 where the
  // mask is missing or clear on 10 km).
  std::vector<float> ratio(n, 1.0f);
  if (haveMask) {
    std::vector<float> mask(n), weight(n);
    for (size_t k = 0; k < n; ++k) {
      const bool valid = !std::isnan(scan.cloudMask[k]);
      mask[k] = valid ? scan.cloudMask[k] : 0.0f;
      weight[k] = valid ? 1.0f : 0.0f;
    }
    std::vector<float> meanMask = mask, meanWeight = weight;
    boxBlur(meanMask, nx, ny, 2);
    boxBlur(meanWeight, nx, ny, 2);
    for (size_t k = 0; k < n; ++k) {
      const float mean = meanWeight[k] > 0.0f ? meanMask[k] / meanWeight[k] : 0.0f;
      if (weight[k] > 0.0f && mean >= kMaskClear) {
        ratio[k] = mask[k] / mean;
      }
    }
  }

  // Daylight at the scan time: COD's day algorithm (solar zenith < 82 deg, good
  // quality < 65 deg); the night algorithm is not used.
  const glm::dvec3 sun = Ephemeris::sunDirectionEcef(double(scan.start));
  std::vector<float> daylight(n);
  double daylightSum = 0.0;
  for (int y = 0; y < ny; ++y) {
    const double lat = g.lat(y) * kPi / 180.0;
    for (int x = 0; x < nx; ++x) {
      const double lon = g.lon(x) * kPi / 180.0;
      const double cosZenith =
          sun.x * std::cos(lat) * std::cos(lon) + sun.y * std::cos(lat) * std::sin(lon) + sun.z * std::sin(lat);
      const double zenith = std::acos(std::clamp(cosZenith, -1.0, 1.0)) * 180.0 / kPi;
      const float d = float(std::clamp((kNightZenith - zenith) / (kNightZenith - kDayZenith), 0.0, 1.0));
      daylight[size_t(y) * size_t(nx) + size_t(x)] = d;
      daylightSum += d;
    }
  }
  stats.daylight = daylightSum / double(n);

  // Each layer as observed: cover, what is visible from above, the uppermost
  // cloud's top and optical depth in the layer they belong to.
  std::array<Observed, kCloudLayerCount> obs;
  for (Observed& o : obs) {
    for (std::vector<float>* v : {&o.cover, &o.valid, &o.seen, &o.top, &o.topWeight, &o.tau, &o.tauWeight}) {
      v->assign(n, 0.0f);
    }
  }
  for (size_t k = 0; k < n; ++k) {
    float cover[kCloudLayerCount] = {};
    bool valid[kCloudLayerCount] = {};
    for (int l = 0; l < kCloudLayerCount && haveBands; ++l) {
      valid[l] = true;
      for (int b = kFirstBand[l]; b <= kLastBand[l]; ++b) {
        const float c = scan.bandCover[size_t(b)][k];
        valid[l] = valid[l] && !std::isnan(c);
        cover[l] = std::max(cover[l], std::isnan(c) ? 0.0f : c); // max overlap inside a layer
      }
      cover[l] = std::clamp(cover[l] * ratio[k], 0.0f, 1.0f);
    }
    float seen = 1.0f; // from the top down
    for (int l = kCloudLayerCount - 1; l >= 0; --l) {
      Observed& o = obs[size_t(l)];
      o.cover[k] = valid[l] ? cover[l] : 0.0f;
      o.valid[k] = valid[l] ? 1.0f : 0.0f;
      o.seen[k] = valid[l] ? seen : 0.0f;
      seen = valid[l] ? seen * (1.0f - cover[l]) : 0.0f; // unknown above: not seen
    }
    // The uppermost cloud: the highest layer the bands see cloudy (without the
    // bands, the band of the cloud-top height); its top where HT fits that
    // layer, weighted by the layer's cover (edge cells take the deck's height).
    const float top = scan.cloudTop[k];
    int uppermost = -1;
    float topWeight = 1.0f;
    for (int l = kCloudLayerCount - 1; l >= 0 && uppermost < 0 && haveBands; --l) {
      uppermost = valid[l] && cover[l] >= kCloudy ? l : -1;
      topWeight = uppermost >= 0 ? cover[l] : 1.0f;
    }
    if (!haveBands && !std::isnan(top)) {
      uppermost = layerOfHeight(top);
    }
    if (uppermost >= 0 && !std::isnan(top) && topFits(top, uppermost)) {
      obs[size_t(uppermost)].top[k] = top;
      obs[size_t(uppermost)].topWeight[k] = topWeight;
    }
    const float tau = haveTau ? scan.opticalDepth[k] : std::numeric_limits<float>::quiet_NaN();
    if (uppermost >= 0 && !std::isnan(tau) && daylight[k] > 0.0f) {
      obs[size_t(uppermost)].tau[k] = std::clamp(tau, kMinObservedTau, kMaxObservedTau);
      obs[size_t(uppermost)].tauWeight[k] = daylight[k];
    }
  }
  // Tops into the layer's other cells (push-pull, by their weights) and smoothed
  // like the HRRR heights; the correction's weight = the smoothed top weights.
  for (Observed& o : obs) {
    const std::vector<float> indicator = o.topWeight;
    if (fillGaps(o.top, indicator, nx, ny)) {
      for (int pass = 0; pass < 2; ++pass) {
        boxBlur(o.top, nx, ny, 2);
        boxBlur(o.topWeight, nx, ny, 2);
      }
    } else {
      o.topWeight.assign(n, 0.0f);
    }
  }

  // Carried to the valid time with each layer's wind and blended in.
  const double dt = double(model.validTime - scan.start);
  const float w0 = float(stats.weight);
  std::vector<float> tauShare(n, 0.0f);
  for (int l = 0; l < kCloudLayerCount; ++l) {
    const Observed& o = obs[size_t(l)];
    CloudLayer& layer = f.layers[size_t(l)];
    const std::vector<std::pair<double, double>> from = departures(model.layers[size_t(l)], g, dt);
    double seenSum = 0.0, topSum = 0.0;
    for (size_t k = 0; k < n; ++k) {
      if (std::isnan(from[k].first)) {
        continue; // the air came from outside the box: the model alone
      }
      const Taps t = taps(nx, ny, from[k].first, from[k].second);
      const auto [cover, valid] = sampleWeighted(o.cover, o.valid, t);
      const auto [seen, seenValid] = sampleWeighted(o.seen, o.valid, t);
      const float w = w0 * std::min(valid, 1.0f);
      if (w > 0.0f) {
        // Seen from above: the observation; hidden under an observed deck: the
        // model keeps its clouds unless the scan has more.
        const float h = layer.cover[k];
        const float target = seen * cover + (1.0f - seen) * std::max(h, cover);
        layer.cover[k] = h + w * (target - h);
        seenSum += seen * std::min(valid, 1.0f);
      }
      const float wTop = w0 * std::clamp(sample(o.topWeight, t), 0.0f, 1.0f);
      if (wTop > 0.0f) {
        // The slab ends at the observed top; the base drops where it was above it.
        const float top = sample(o.top, t);
        const float base = layer.altitude[k], thickness = layer.thickness[k];
        const float newBase = std::max(std::min(base, top - kMinThickness[l]), kMinBase[l]);
        const float newThickness = std::max(top - newBase, 100.0f);
        layer.altitude[k] = base + wTop * (newBase - base);
        layer.thickness[k] = thickness + wTop * (newThickness - thickness);
        topSum += wTop;
      }
      const auto [tau, tauWeight] = sampleWeighted(o.tau, o.tauWeight, t);
      const float wTau = w0 * std::min(tauWeight, 1.0f);
      if (wTau > 0.0f) {
        layer.opticalDepth[k] += wTau * (tau - layer.opticalDepth[k]);
        tauShare[k] = std::max(tauShare[k], wTau);
      }
    }
    stats.seen[l] = seenSum / double(n);
    stats.topShare[l] = topSum / double(n);
  }
  double tauSum = 0.0;
  for (size_t k = 0; k < n; ++k) {
    tauSum += tauShare[k];
  }
  stats.opticalDepthShare = tauSum / double(n);

  f.constrainLayers();
  // Total cover (summaries, the step-1 shell): random overlap of the layers.
  for (size_t k = 0; k < n; ++k) {
    float clear = 1.0f;
    for (const CloudLayer& layer : f.layers) {
      clear *= 1.0f - layer.cover[k];
    }
    f.cover[k] = 1.0f - clear;
  }
  f.summarize();
  for (int l = 0; l < kCloudLayerCount; ++l) {
    stats.coverAfter[l] = f.layers[size_t(l)].meanCover;
  }
  stats.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  return out;
}

} // namespace CloudBlend
