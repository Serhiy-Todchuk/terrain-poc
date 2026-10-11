#include "pch.h"
#include "Ephemeris.h"

namespace Ephemeris {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
} // namespace

glm::dvec3 sunDirectionEcef(double unixSeconds) {
  const double n = unixSeconds / 86400.0 + 2440587.5 - 2451545.0; // days since J2000.0 (UT ~ TT here)
  const double meanLongitude = 280.460 + 0.9856474 * n;          // degrees
  const double meanAnomaly = (357.528 + 0.9856003 * n) * kDeg;
  const double lambda =
      (meanLongitude + 1.915 * std::sin(meanAnomaly) + 0.020 * std::sin(2.0 * meanAnomaly)) * kDeg; // ecliptic
  const double obliquity = (23.439 - 0.0000004 * n) * kDeg;
  const double rightAscension = std::atan2(std::cos(obliquity) * std::sin(lambda), std::cos(lambda));
  const double declination = std::asin(std::sin(obliquity) * std::sin(lambda));
  const double gmst = std::fmod(280.46061837 + 360.98564736629 * n, 360.0) * kDeg;
  // The sun stands over the longitude where the local sidereal time equals its
  // right ascension.
  const double subsolarLon = rightAscension - gmst;
  return {std::cos(declination) * std::cos(subsolarLon), std::cos(declination) * std::sin(subsolarLon),
          std::sin(declination)};
}

void elevationAzimuth(const glm::dvec3& d, double lonDeg, double latDeg, double& elevationDeg, double& azimuthDeg) {
  const double lon = lonDeg * kDeg, lat = latDeg * kDeg;
  const glm::dvec3 east(-std::sin(lon), std::cos(lon), 0.0);
  const glm::dvec3 north(-std::sin(lat) * std::cos(lon), -std::sin(lat) * std::sin(lon), std::cos(lat));
  const glm::dvec3 up(std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat));
  elevationDeg = std::asin(std::clamp(glm::dot(d, up), -1.0, 1.0)) / kDeg;
  azimuthDeg = std::atan2(glm::dot(d, east), glm::dot(d, north)) / kDeg;
  if (azimuthDeg < 0.0) {
    azimuthDeg += 360.0;
  }
}

int newYorkUtcOffsetHours(int64_t unixSeconds) {
  using namespace std::chrono;
  const sys_days day = floor<days>(sys_seconds{seconds{unixSeconds}});
  const year y = year_month_day{day}.year();
  const int64_t dstStart = int64_t(sys_days{y / March / Sunday[2]}.time_since_epoch().count()) * 86400 + 7 * 3600;
  const int64_t dstEnd = int64_t(sys_days{y / November / Sunday[1]}.time_since_epoch().count()) * 86400 + 6 * 3600;
  return unixSeconds >= dstStart && unixSeconds < dstEnd ? -4 : -5;
}

} // namespace Ephemeris
