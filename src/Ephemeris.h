#pragma once
#include <glm/vec3.hpp>

#include <cstdint>

// Sun position and New York civil time. Plain C++ (no Windows / D3D).
namespace Ephemeris {

// Unit vector from the Earth's centre towards the Sun in ECEF (WGS84 axes) at a
// UTC time (unix seconds): low-precision solar coordinates of the Astronomical
// Almanac (~0.01 degree, 1950-2050) and Greenwich mean sidereal time; nutation,
// polar motion and refraction are ignored (all well below the sun's disc).
glm::dvec3 sunDirectionEcef(double unixSeconds);

// Elevation and azimuth (degrees, azimuth clockwise from north) of an ECEF
// direction seen from a geodetic position.
void elevationAzimuth(const glm::dvec3& directionEcef, double lonDeg, double latDeg, double& elevationDeg,
                      double& azimuthDeg);

// UTC offset of New York civil time in hours: -4 (EDT) or -5 (EST), US rules
// since 2007 (second Sunday of March 2:00 to first Sunday of November 2:00).
int newYorkUtcOffsetHours(int64_t unixSeconds);

} // namespace Ephemeris
