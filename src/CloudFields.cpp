#include "pch.h"
#include "CloudFields.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kPresent = 0.05f; // a layer counts as present in a cell from this cover on

} // namespace

void CloudFields::constrainLayers() {
  const size_t n = layers[0].altitude.size();
  for (size_t k = 0; k < n; ++k) {
    float& a0 = layers[0].altitude[k];
    float& a1 = layers[1].altitude[k];
    float& a2 = layers[2].altitude[k];
    a0 = std::clamp(a0, 150.0f, 4000.0f);
    a1 = std::max(std::clamp(a1, 2000.0f, 8500.0f), a0 + 700.0f);
    a2 = std::max(std::clamp(a2, 5500.0f, 13500.0f), a1 + 1000.0f);
    for (CloudLayer& layer : layers) {
      layer.thickness[k] = std::clamp(layer.thickness[k], 100.0f, 6000.0f);
    }
  }
}

// Domain means; per layer the cover-weighted median altitude within 150 km of
// the box centre; the layer an observer there mostly sees from below (random
// overlap: low clouds hide middle ones, both hide high ones).
void CloudFields::summarize() {
  const CloudGrid& g = grid;
  const double poiLon = g.centreLon(), poiLat = g.centreLat(); // the summaries are about the box centre
  const double kmPerLon = 111.32 * std::cos(poiLat * kPi / 180.0);
  const int nx = g.nx, ny = g.ny;
  const size_t n = size_t(nx) * size_t(ny);
  double total = 0.0;
  for (size_t k = 0; k < n; ++k) {
    total += cover[k];
  }
  meanCover = total / double(n);
  double seen[kCloudLayerCount] = {};
  for (int l = 0; l < kCloudLayerCount; ++l) {
    CloudLayer& layer = layers[size_t(l)];
    double sum = 0.0;
    std::vector<std::pair<float, float>> heights; // altitude, weight
    float nearest = 0.0f;
    double nearestD2 = 1e30;
    for (int y = 0; y < ny; ++y) {
      for (int x = 0; x < nx; ++x) {
        const size_t k = size_t(y) * size_t(nx) + size_t(x);
        sum += layer.cover[k];
        const double dx = (g.lon(x) - poiLon) * kmPerLon, dy = (g.lat(y) - poiLat) * 111.32;
        const double d2 = dx * dx + dy * dy;
        if (d2 < nearestD2) {
          nearestD2 = d2;
          nearest = layer.altitude[k];
        }
        if (d2 <= 150.0 * 150.0) {
          const float c[3] = {layers[0].cover[k], layers[1].cover[k], layers[2].cover[k]};
          const float visible = l == 0 ? c[0] : l == 1 ? c[1] * (1.0f - c[0]) : c[2] * (1.0f - c[0]) * (1.0f - c[1]);
          seen[l] += visible;
          if (layer.cover[k] > kPresent) {
            heights.emplace_back(layer.altitude[k], layer.cover[k]);
          }
        }
      }
    }
    layer.meanCover = sum / double(n);
    layer.altitudeNearPoi = nearest;
    double weight = 0.0;
    for (const auto& [h, w] : heights) {
      weight += w;
    }
    if (weight >= 2.0) {
      std::sort(heights.begin(), heights.end());
      double cumulative = 0.0;
      for (const auto& [h, w] : heights) {
        cumulative += w;
        if (cumulative >= 0.5 * weight) {
          layer.altitudeNearPoi = h;
          break;
        }
      }
    }
  }
  const int dominant = int(std::max_element(std::begin(seen), std::end(seen)) - std::begin(seen));
  const char* names[kCloudLayerCount] = {"low", "middle", "high"};
  layerName = seen[dominant] < 1.0 ? "none" : names[dominant];
  layerAltitude = layers[size_t(dominant)].altitudeNearPoi;
}
