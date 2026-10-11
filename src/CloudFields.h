#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The common weather grid ("cloud resources" grid of docs/weather_roadmap.md):
// a regular longitude/latitude grid around New York. Every weather source
// resamples its native projection (HRRR: Lambert conformal; later GOES:
// geostationary) onto it, so the renderer never depends on a source.
struct CloudGrid {
  double west = -81.2, south = 35.55, east = -66.8, north = 45.75; // degrees, ~1200 x 1130 km (New York)
  int nx = 576, ny = 408;                                          // 0.025 degree cells (~2.1 x 2.8 km)
  static constexpr double kLonSpan = 14.4, kLatSpan = 10.2;

  // The same box centred elsewhere ("Fetch weather here": the weather hotspot).
  static CloudGrid centredAt(double lon, double lat) {
    CloudGrid g;
    g.west = lon - 0.5 * kLonSpan;
    g.east = lon + 0.5 * kLonSpan;
    g.south = lat - 0.5 * kLatSpan;
    g.north = lat + 0.5 * kLatSpan;
    return g;
  }
  double centreLon() const { return 0.5 * (west + east); }
  double centreLat() const { return 0.5 * (south + north); }
  bool operator==(const CloudGrid&) const = default;

  // Cell centres; row 0 is the northern edge (texture row order).
  double lon(int i) const { return west + (i + 0.5) * (east - west) / nx; }
  double lat(int j) const { return north - (j + 0.5) * (north - south) / ny; }
};

// The NCEP cloud layers (split at 642 / 350 hPa, ~3.8 / ~8 km).
enum CloudLayerIndex : int { kCloudLow = 0, kCloudMid = 1, kCloudHigh = 2, kCloudLayerCount = 3 };

// One cloud layer on CloudGrid (row-major, i fastest, row 0 north). Altitude and
// thickness are defined everywhere: where the layer has no cloud they are
// filled in from the neighbourhood (heights only exist where clouds are).
struct CloudLayer {
  std::vector<float> cover;        // 0..1
  std::vector<float> altitude;     // layer base, m above MSL
  std::vector<float> thickness;    // m
  std::vector<float> opticalDepth; // in-cloud optical depth (visible) where the layer is cloudy
  std::vector<float> windEast;     // m/s, the layer wind per cell (0 without winds): the macro fields and
  std::vector<float> windNorth;    //   the noise phase drift with it (CSBlendClouds)

  // Summary (computed by the source).
  double meanCover = 0.0;         // domain mean, 0..1
  double altitudeNearPoi = 0.0;   // m MSL: cover-weighted median within 150 km of New York
  float windU = 0.0f, windV = 0.0f; // m/s east / north at the layer (mean within 200 km of the box centre: HUD)
};

// Cloud state of one valid time on CloudGrid.
struct CloudFields {
  CloudGrid grid;
  std::vector<float> cover;     // total cloud cover, 0..1
  std::vector<float> base, top; // the source's cloud base / top height (m above MSL); NaN where undefined
  std::array<CloudLayer, kCloudLayerCount> layers; // low / middle / high
  // The single layer of the step-1 look ("Single layer" toggle): the total cover
  // at layerAltitude with this in-cloud optical depth (step 1's heuristic: water
  // layers 3 + 4 per km of thickness, ice 1, mixed by the layer covers).
  std::vector<float> singleOpticalDepth;

  std::string source;           // e.g. "HRRR", "HRRR + GOES-19"
  int64_t cycle = 0;            // model run / observation time (unix seconds, UTC)
  int forecastHours = 0;
  int64_t validTime = 0;        // unix seconds, UTC
  bool hasWind = false;         // layer winds from the source (else 0: shapes do not drift)
  bool hasCondensate = false;   // optical depths from cloud water / ice paths (else a thickness heuristic)
  // Step 3: the GOES-19 scan blended into the model fields (CloudBlend::blend); 0 = the model alone.
  int64_t observedTime = 0;     // scan start (unix seconds, UTC)
  double observedWeight = 0.0;  // the scan's time weight at validTime

  // Summary (computed by the source after resampling).
  double meanCover = 0.0;         // domain mean, 0..1
  std::string layerName = "none"; // dominant layer seen from below near New York: low / middle / high
  double layerAltitude = 3000.0;  // m MSL: its altitude there

  // Keeps every cell's layer bases apart and inside their NCEP pressure bands,
  // thickness 100 m .. 6 km (the sources call it after setting the heights).
  void constrainLayers();
  // The summary values above and each layer's meanCover / altitudeNearPoi from
  // the fields (the sources call it last).
  void summarize();
};
