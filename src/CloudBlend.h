#pragma once
#include "CloudFields.h"
#include "GoesSource.h"

#include <cstdint>
#include <memory>
#include <string>

// GOES-19 observations blended into the HRRR fields (docs/weather_roadmap.md,
// step 3, plan item 5): per layer the scan's cover (CCLC height bands, sharpened
// by the 2 km cloud mask) where the satellite sees the layer from above, the
// cloud-top height as the top of the uppermost layer, the optical depth by day.
// The scan is carried from its time to the dataset's valid time with the
// dataset's own layer winds (semi-Lagrangian, as CSBlendClouds), so the
// renderer's time blend carries it on to the weather time. Valid time, winds
// and everything the scan does not observe stay the model's. Plain C++ (no
// Windows / D3D).
namespace CloudBlend {

// Weight of a scan starting at scanStart in a dataset valid at validTime: 1
// within 90 min, falling linearly to 0 at 3 h.
double timeWeight(int64_t validTime, int64_t scanStart);

// What a blend changed (log).
struct Stats {
  double minutes = 0.0; // validTime - scan start: how far the wind carried the scan
  double weight = 0.0;  // timeWeight
  double coverBefore[kCloudLayerCount]{}, coverAfter[kCloudLayerCount]{}; // domain means, 0..1
  double seen[kCloudLayerCount]{};     // mean share of each layer the scan observed (seen from above, valid data)
  double topShare[kCloudLayerCount]{}; // mean weight of the cloud-top correction
  double daylight = 0.0;               // mean daylight weight of the optical depth (solar zenith at the scan)
  double opticalDepthShare = 0.0;      // mean weight of COD in the optical depth (its layer)
  double ms = 0.0;
};

// The model dataset corrected by the scan at the model's valid time (worker
// thread; reads both, writes only the copy). Null with `error` when they do not
// fit together (another box or grid size).
std::shared_ptr<CloudFields> blend(const CloudFields& model, const GoesScan& scan, Stats& stats, std::string& error);

} // namespace CloudBlend
