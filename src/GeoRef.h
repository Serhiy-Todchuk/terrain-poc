#pragma once
#include <CesiumGeospatial/Ellipsoid.h>

#include <glm/vec3.hpp>

// Vertical datum handling. All geometry handed to Cesium Native is in WGS84
// ellipsoidal heights; the public data sources use other vertical datums:
//   * USGS 3DEP, NOAA NCEI CUDEM: NAVD88 (orthometric)
//   * NOAA BAG hydrographic surveys: MLLW (chart datum)
// h_ellipsoid = H_navd88 + N_geoid18, and H_navd88 = d_mllw - (NAVD88 - MLLW).
namespace GeoRef {

// GEOID18 geoid undulation (meters) from a 0.25 degree grid sampled from the
// NGS geoid API around New York; clamps outside the grid.
double geoidHeight(double lonDeg, double latDeg);

// Height of NAVD88 above MLLW (meters), inverse-distance weighted between NOAA
// CO-OPS tide stations (The Battery, Sandy Hook, Kings Point).
double navd88AboveMllw(double lonDeg, double latDeg);

// Mean sea level used for the engine-rendered sea surface (NAVD88 meters).
constexpr double kSeaLevelNavd88 = 0.0;

inline double navd88ToEllipsoid(double hNavd88, double lonDeg, double latDeg) {
  return hNavd88 + geoidHeight(lonDeg, latDeg);
}

glm::dvec3 toEcef(double lonDeg, double latDeg, double hEllipsoid);

// Height of the sea surface above the ellipsoid at a location.
inline double seaSurfaceEllipsoidHeight(double lonDeg, double latDeg) {
  return navd88ToEllipsoid(kSeaLevelNavd88, lonDeg, latDeg);
}

const CesiumGeospatial::Ellipsoid& ellipsoid();

} // namespace GeoRef
