#include "pch.h"
#include "GoesGrid.h"

#include "GeoRef.h"
#include "GridFill.h"
#include "Hdf5.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace Goes {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// Great-circle distance (km) on a sphere of the mean Earth radius.
double distanceKm(double lon1, double lat1, double lon2, double lat2) {
  const double p1 = lat1 * kDeg, p2 = lat2 * kDeg;
  const double a = std::sin(0.5 * (p2 - p1)), b = std::sin(0.5 * (lon2 - lon1) * kDeg);
  return 2.0 * 6371.0 * std::asin(std::min(1.0, std::sqrt(a * a + std::cos(p1) * std::cos(p2) * b * b)));
}

// The local vertical (ellipsoid normal) at a geodetic position.
glm::dvec3 up(double lonDeg, double latDeg) {
  const double lon = lonDeg * kDeg, lat = latDeg * kDeg;
  return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}

// First / step of a regular axis; false when the spacing varies by more than 1 %.
bool regularAxis(const std::vector<double>& a, double& first, double& step) {
  if (a.size() < 2) {
    return false;
  }
  first = a.front();
  step = (a.back() - a.front()) / double(a.size() - 1);
  if (!(std::abs(step) > 0.0)) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (!(std::abs(a[i] - (first + step * double(i))) <= 0.01 * std::abs(step))) {
      return false;
    }
  }
  return true;
}

} // namespace

// ---- projection ---------------------------------------------------------------------

bool Projection::read(const Hdf5::File& file, std::string& error) {
  const Hdf5::Variable* v = file.variable("goes_imager_projection");
  if (!v) {
    error = "no goes_imager_projection variable";
    return false;
  }
  auto text = [&](const char* key) {
    const Hdf5::Attribute* a = v->attribute(key);
    return a ? a->text() : std::string();
  };
  auto number = [&](const char* key, double& out) {
    const Hdf5::Attribute* a = v->attribute(key);
    const std::optional<double> x = a ? a->number() : std::nullopt;
    if (!x) {
      error = std::string("goes_imager_projection has no numeric ") + key;
      return false;
    }
    out = *x;
    return true;
  };
  if (text("grid_mapping_name") != "geostationary" || text("sweep_angle_axis") != "x") {
    error = "goes_imager_projection: grid mapping '" + text("grid_mapping_name") + "', sweep axis '" +
            text("sweep_angle_axis") + "' (supported: geostationary, x)";
    return false;
  }
  double latOrigin = 0.0;
  if (!number("perspective_point_height", perspectivePointHeight) || !number("semi_major_axis", semiMajor) ||
      !number("semi_minor_axis", semiMinor) || !number("longitude_of_projection_origin", lonOrigin) ||
      !number("latitude_of_projection_origin", latOrigin)) {
    return false;
  }
  if (latOrigin != 0.0 || !(perspectivePointHeight > 1.0e7) || !(semiMajor > 6.0e6) || !(semiMinor > 6.0e6) ||
      semiMinor > semiMajor || !(std::abs(lonOrigin) <= 180.0)) {
    char buf[200];
    snprintf(buf, sizeof(buf), "goes_imager_projection: unexpected values (height %.0f, axes %.3f / %.3f, origin %g / %g)",
             perspectivePointHeight, semiMajor, semiMinor, lonOrigin, latOrigin);
    error = buf;
    return false;
  }
  return true;
}

glm::dvec3 Projection::ecef(double lonDeg, double latDeg, double height) const {
  const double lon = lonDeg * kDeg, lat = latDeg * kDeg;
  const double e2 = 1.0 - (semiMinor * semiMinor) / (semiMajor * semiMajor);
  const double s = std::sin(lat), c = std::cos(lat);
  const double n = semiMajor / std::sqrt(1.0 - e2 * s * s); // prime vertical radius
  return {(n + height) * c * std::cos(lon), (n + height) * c * std::sin(lon), (n * (1.0 - e2) + height) * s};
}

bool Projection::scanAngles(const glm::dvec3& p, double& x, double& y) const {
  const double h = perspectivePointHeight + semiMajor; // satellite distance from the Earth's centre
  const double l0 = lonOrigin * kDeg;
  // The point in the frame of the satellite's meridian (satellite at (h, 0, 0)).
  const double px = p.x * std::cos(l0) + p.y * std::sin(l0);
  const double py = -p.x * std::sin(l0) + p.y * std::cos(l0);
  const double pz = p.z;
  // Hidden when the line of sight enters the Earth before reaching the point:
  // with z scaled by a / b the ellipsoid is a sphere of radius a, and the
  // segment satellite + t (point - satellite), t in [0, 1], must not meet it
  // before t = 1 (a ground point itself is the root t = 1). Points below the
  // ellipsoid (MSL is ~30 m below it at New York) test against the sphere
  // through themselves instead.
  const glm::dvec3 q(px, py, pz * semiMajor / semiMinor);
  const double radius = std::min(semiMajor, glm::length(q));
  const glm::dvec3 d(q.x - h, q.y, q.z);
  const double qa = glm::dot(d, d), qb = 2.0 * h * d.x, qc = h * h - radius * radius;
  const double disc = qb * qb - 4.0 * qa * qc;
  if (disc > 0.0 && (-qb - std::sqrt(disc)) / (2.0 * qa) < 1.0 - 1e-7) {
    return false;
  }
  // PUG: s = satellite -> point in the satellite frame, x = asin(-sy / |s|), y = atan(sz / sx).
  const double sx = h - px, sy = -py, sz = pz;
  x = std::asin(-sy / std::sqrt(sx * sx + sy * sy + sz * sz));
  y = std::atan(sz / sx);
  return true;
}

bool Projection::scanAngles(double lonDeg, double latDeg, double height, double& x, double& y) const {
  return scanAngles(ecef(lonDeg, latDeg, height), x, y);
}

bool Projection::groundPoint(double x, double y, double& lonDeg, double& latDeg) const {
  const double h = perspectivePointHeight + semiMajor;
  const double k2 = (semiMajor * semiMajor) / (semiMinor * semiMinor);
  const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y);
  const double a = sx * sx + cx * cx * (cy * cy + k2 * sy * sy);
  const double b = -2.0 * h * cx * cy;
  const double c = h * h - semiMajor * semiMajor;
  const double disc = b * b - 4.0 * a * c;
  if (!(disc >= 0.0)) {
    return false;
  }
  const double rs = (-b - std::sqrt(disc)) / (2.0 * a); // distance to the first intersection
  const double px = rs * cx * cy, py = -rs * sx, pz = rs * cx * sy;
  latDeg = std::atan(k2 * pz / std::sqrt((h - px) * (h - px) + py * py)) / kDeg;
  lonDeg = lonOrigin - std::atan(py / (h - px)) / kDeg;
  return true;
}

double Projection::viewZenith(double lonDeg, double latDeg) const {
  const double h = perspectivePointHeight + semiMajor, l0 = lonOrigin * kDeg;
  const glm::dvec3 toSatellite = glm::dvec3(h * std::cos(l0), h * std::sin(l0), 0.0) - ecef(lonDeg, latDeg, 0.0);
  return std::acos(std::clamp(glm::dot(up(lonDeg, latDeg), glm::normalize(toSatellite)), -1.0, 1.0)) / kDeg;
}

// ---- raster ---------------------------------------------------------------------------

bool Raster::read(const Hdf5::File& file, std::string_view variable, const CloudGrid* box, std::string& error) {
  if (!projection.read(file, error)) {
    return false;
  }
  const Hdf5::Variable* v = file.variable(variable);
  const Hdf5::Variable* xv = file.variable("x");
  const Hdf5::Variable* yv = file.variable("y");
  if (!v) {
    error = "no variable '" + std::string(variable) + "'";
    return false;
  }
  if (!xv || !yv || xv->dims.size() != 1 || yv->dims.size() != 1) {
    error = "no one-dimensional x / y coordinate variables";
    return false;
  }
  if (v->dims.size() != 2 || v->dims[0] != yv->dims[0] || v->dims[1] != xv->dims[0]) {
    error = std::string(variable) + " is " + v->describe() + ", not (y, x) = (" + std::to_string(yv->dims[0]) +
            ", " + std::to_string(xv->dims[0]) + ")";
    return false;
  }
  std::vector<double> xs, ys;
  if (!file.readValues(*xv, {}, {}, xs, error) || !file.readValues(*yv, {}, {}, ys, error)) {
    error = "x / y: " + error;
    return false;
  }
  double xFirst = 0.0, yFirst = 0.0;
  if (!regularAxis(xs, xFirst, dx) || !regularAxis(ys, yFirst, dy)) {
    error = "x / y are not regular axes";
    return false;
  }
  if (!(dx > 0.0) || !(dy < 0.0)) {
    error = "unexpected axis directions (supported: x eastwards, y southwards along the rows)";
    return false;
  }
  const int allColumns = int(xs.size()), allRows = int(ys.size());
  int c0 = 0, c1 = allColumns - 1, r0 = 0, r1 = allRows - 1;
  if (box) {
    // The box's outline at the ground and at 20 km: scan angles vary
    // monotonically across it, so their extremes lie on the outline.
    double xMin = std::numeric_limits<double>::infinity(), xMax = -xMin, yMin = xMin, yMax = -xMin;
    constexpr int kSteps = 32;
    for (int edge = 0; edge < 4; ++edge) {
      for (int s = 0; s <= kSteps; ++s) {
        const double t = double(s) / kSteps;
        const double lon = edge < 2 ? box->west + t * (box->east - box->west) : (edge == 2 ? box->west : box->east);
        const double lat = edge < 2 ? (edge == 0 ? box->north : box->south) : box->south + t * (box->north - box->south);
        for (const double height : {0.0, 20000.0}) {
          double x = 0.0, y = 0.0;
          if (projection.scanAngles(lon, lat, height, x, y)) {
            xMin = std::min(xMin, x);
            xMax = std::max(xMax, x);
            yMin = std::min(yMin, y);
            yMax = std::max(yMax, y);
          }
        }
      }
    }
    if (!(xMin <= xMax)) {
      error = "the weather box cannot be seen from the satellite";
      return false;
    }
    c0 = int(std::floor((xMin - xFirst) / dx)) - 2;
    c1 = int(std::ceil((xMax - xFirst) / dx)) + 2;
    r0 = int(std::floor((yMax - yFirst) / dy)) - 2; // dy < 0: the northern edge is the first row
    r1 = int(std::ceil((yMin - yFirst) / dy)) + 2;
    if (c1 < 0 || r1 < 0 || c0 >= allColumns || r0 >= allRows) {
      error = "the weather box lies outside this image";
      return false;
    }
    c0 = std::max(c0, 0);
    r0 = std::max(r0, 0);
    c1 = std::min(c1, allColumns - 1);
    r1 = std::min(r1, allRows - 1);
  }
  const uint64_t start[2] = {uint64_t(r0), uint64_t(c0)};
  const uint64_t count[2] = {uint64_t(r1 - r0 + 1), uint64_t(c1 - c0 + 1)};
  if (!file.readValues(*v, start, count, values, error)) {
    return false;
  }
  name = std::string(variable);
  const Hdf5::Attribute* unitsAttribute = v->attribute("units");
  units = unitsAttribute ? unitsAttribute->text() : std::string();
  columns = int(count[1]);
  rows = int(count[0]);
  column0 = c0;
  row0 = r0;
  x0 = xFirst + dx * c0;
  y0 = yFirst + dy * r0;
  return true;
}

float Raster::sample(double x, double y) const {
  if (columns < 1 || rows < 1) {
    return kNaN;
  }
  double i = (x - x0) / dx, j = (y - y0) / dy;
  if (!(i >= -0.5 && j >= -0.5 && i <= columns - 0.5 && j <= rows - 0.5)) {
    return kNaN;
  }
  i = std::clamp(i, 0.0, double(columns - 1));
  j = std::clamp(j, 0.0, double(rows - 1));
  const int i0 = int(i), j0 = int(j);
  const int i1 = std::min(i0 + 1, columns - 1), j1 = std::min(j0 + 1, rows - 1);
  const double fi = i - i0, fj = j - j0;
  const double w[4] = {(1 - fi) * (1 - fj), fi * (1 - fj), (1 - fi) * fj, fi * fj};
  const float v[4] = {values[size_t(j0) * size_t(columns) + size_t(i0)],
                      values[size_t(j0) * size_t(columns) + size_t(i1)],
                      values[size_t(j1) * size_t(columns) + size_t(i0)],
                      values[size_t(j1) * size_t(columns) + size_t(i1)]};
  double sum = 0.0, weight = 0.0;
  for (int k = 0; k < 4; ++k) {
    if (!std::isnan(v[k])) {
      sum += w[k] * v[k];
      weight += w[k];
    }
  }
  return weight >= 0.5 ? float(sum / weight) : kNaN;
}

// ---- resampling onto CloudGrid ---------------------------------------------------

std::vector<float> resample(const Raster& raster, const CloudGrid& grid, std::span<const float> heights,
                            float height) {
  const size_t n = size_t(grid.nx) * size_t(grid.ny);
  std::vector<float> out(n, kNaN);
  if (!heights.empty() && heights.size() != n) {
    return out;
  }
  for (int y = 0; y < grid.ny; ++y) {
    const double lat = grid.lat(y);
    for (int x = 0; x < grid.nx; ++x) {
      const double lon = grid.lon(x);
      const size_t k = size_t(y) * size_t(grid.nx) + size_t(x);
      float h = heights.empty() ? height : heights[k];
      if (std::isnan(h)) {
        h = 0.0f;
      }
      double sx = 0.0, sy = 0.0;
      if (raster.projection.scanAngles(lon, lat, GeoRef::navd88ToEllipsoid(h, lon, lat), sx, sy)) {
        out[k] = raster.sample(sx, sy);
      }
    }
  }
  return out;
}

std::vector<float> cloudTopHeights(const Raster& heights, const CloudGrid& grid, ParallaxStats* stats,
                                   std::vector<float>* lineOfSightHeights) {
  const size_t n = size_t(grid.nx) * size_t(grid.ny);
  std::vector<float> out(n, kNaN), solved(n, kNaN);
  ParallaxStats st;
  // The gap-filled copy that the iteration runs on: clear pixels take the
  // heights of the clouds around them.
  Raster filled = heights;
  std::vector<float> weight(filled.values.size());
  for (size_t i = 0; i < filled.values.size(); ++i) {
    const bool valid = !std::isnan(filled.values[i]);
    weight[i] = valid ? 1.0f : 0.0f;
    if (!valid) {
      filled.values[i] = 0.0f;
    }
  }
  const Projection& proj = heights.projection;
  double shiftSum = 0.0, shiftNorth = 0.0, shiftEast = 0.0;
  if (GridFill::fillGaps(filled.values, weight, filled.columns, filled.rows)) {
    // h -> HT(line of sight(cell, h)) moves the solution by ~1.07 x the field's
    // slope per step: plain steps first, damped ones (half steps) after them so
    // that steep cloud edges (a slope near -1) do not oscillate.
    constexpr int kIterations = 8;
    for (int y = 0; y < grid.ny; ++y) {
      const double lat = grid.lat(y);
      for (int x = 0; x < grid.nx; ++x) {
        const double lon = grid.lon(x);
        const size_t k = size_t(y) * size_t(grid.nx) + size_t(x);
        const glm::dvec3 ground = proj.ecef(lon, lat, GeoRef::geoidHeight(lon, lat)); // MSL
        const glm::dvec3 vertical = up(lon, lat);
        double h = 0.0, residual = 0.0, sx = 0.0, sy = 0.0;
        bool ok = true;
        for (int it = 0; it < kIterations; ++it) {
          const float target = proj.scanAngles(ground + h * vertical, sx, sy) ? filled.sample(sx, sy) : kNaN;
          if (std::isnan(target)) {
            ok = false;
            break;
          }
          residual = std::max(0.0, double(target)) - h;
          h += (it < kIterations / 2 ? 1.0 : 0.5) * residual;
        }
        if (!ok || !proj.scanAngles(ground + h * vertical, sx, sy)) {
          continue;
        }
        solved[k] = float(h);
        st.unconverged += std::abs(residual) > 100.0;
        const float top = heights.sample(sx, sy);
        if (std::isnan(top)) {
          continue;
        }
        out[k] = top;
        ++st.cloudy;
        double apparentLon = 0.0, apparentLat = 0.0;
        if (proj.groundPoint(sx, sy, apparentLon, apparentLat)) {
          const double d = distanceKm(lon, lat, apparentLon, apparentLat);
          shiftSum += d;
          st.maxShiftKm = std::max(st.maxShiftKm, d);
          shiftNorth += (apparentLat - lat) * 111.32;
          shiftEast += (apparentLon - lon) * 111.32 * std::cos(lat * kDeg);
        }
      }
    }
  }
  if (st.cloudy) {
    st.meanShiftKm = shiftSum / st.cloudy;
    st.meanShiftNorthKm = shiftNorth / st.cloudy;
    st.meanShiftEastKm = shiftEast / st.cloudy;
  }
  if (stats) {
    *stats = st;
  }
  if (lineOfSightHeights) {
    *lineOfSightHeights = std::move(solved);
  }
  return out;
}

} // namespace Goes
