#include "pch.h"
#include "GeoRef.h"

namespace GeoRef {
namespace {

// GEOID18 heights (m), rows = latitude 40.00 .. 41.25 step 0.25,
// columns = longitude -75.00 .. -72.50 step 0.25. Sampled from
// https://geodesy.noaa.gov/api/geoid/ght (GEOID18).
constexpr double kLat0 = 40.0;
constexpr double kLon0 = -75.0;
constexpr double kStep = 0.25;
constexpr int kRows = 6;
constexpr int kCols = 11;
constexpr double kGeoid[kRows][kCols] = {
    {-32.865, -32.621, -32.610, -32.914, -33.080, -33.064, -33.091, -33.428, -33.803, -34.006, -34.052},
    {-33.708, -33.232, -32.613, -32.370, -32.794, -32.604, -32.520, -32.648, -33.035, -33.283, -33.333},
    {-34.020, -33.621, -33.071, -32.455, -32.394, -32.269, -31.914, -31.912, -32.246, -32.449, -32.489},
    {-33.597, -33.196, -32.934, -32.502, -31.793, -31.456, -31.158, -31.051, -31.440, -31.777, -31.854},
    {-33.120, -32.829, -32.446, -32.240, -31.656, -30.865, -30.495, -30.326, -30.529, -30.912, -31.256},
    {-32.344, -32.365, -32.303, -31.963, -31.524, -30.645, -29.928, -29.698, -29.799, -30.102, -30.412},
};

struct TideStation {
  double lon, lat;
  double navd88AboveMllw; // NAVD88 - MLLW, 1983-2001 epoch (m)
};
// NOAA CO-OPS datums (api.tidesandcurrents.noaa.gov/mdapi): 8518750 The Battery,
// 8531680 Sandy Hook, 8516945 Kings Point.
constexpr TideStation kStations[] = {
    {-74.0142, 40.7006, 1.848 - 1.002},
    {-74.0094, 40.4669, 1.624 - 0.766},
    {-73.7649, 40.8103, 5.209 - 3.927},
};

} // namespace

double geoidHeight(double lonDeg, double latDeg) {
  double fx = std::clamp((lonDeg - kLon0) / kStep, 0.0, double(kCols - 1));
  double fy = std::clamp((latDeg - kLat0) / kStep, 0.0, double(kRows - 1));
  int x0 = std::min(int(fx), kCols - 2);
  int y0 = std::min(int(fy), kRows - 2);
  double tx = fx - x0;
  double ty = fy - y0;
  double a = kGeoid[y0][x0] * (1 - tx) + kGeoid[y0][x0 + 1] * tx;
  double b = kGeoid[y0 + 1][x0] * (1 - tx) + kGeoid[y0 + 1][x0 + 1] * tx;
  return a * (1 - ty) + b * ty;
}

double navd88AboveMllw(double lonDeg, double latDeg) {
  double wsum = 0.0, vsum = 0.0;
  const double cosLat = std::cos(latDeg * CesiumUtility::Math::OnePi / 180.0);
  for (const TideStation& s : kStations) {
    double dx = (lonDeg - s.lon) * cosLat;
    double dy = latDeg - s.lat;
    double d2 = dx * dx + dy * dy + 1e-6;
    double w = 1.0 / d2;
    wsum += w;
    vsum += w * s.navd88AboveMllw;
  }
  return vsum / wsum;
}

const CesiumGeospatial::Ellipsoid& ellipsoid() { return CesiumGeospatial::Ellipsoid::WGS84; }

glm::dvec3 toEcef(double lonDeg, double latDeg, double hEllipsoid) {
  return ellipsoid().cartographicToCartesian(
      CesiumGeospatial::Cartographic::fromDegrees(lonDeg, latDeg, hEllipsoid));
}

} // namespace GeoRef
