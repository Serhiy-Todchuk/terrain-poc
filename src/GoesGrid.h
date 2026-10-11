#pragma once
#include "CloudFields.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Hdf5 {
class File;
}

// GOES-R ABI fixed grid (docs/weather_roadmap.md, step 3): navigation between
// geodetic positions and the scan angles of the geostationary imager (GOES-R
// Product User Guide, "navigation of image data"; CF grid mapping
// "geostationary", sweep axis x), rasters of Level-2 variables on that grid,
// and their resampling onto CloudGrid with the parallax correction. The
// satellite sees a cloud top at height h displaced away from the sub-satellite
// point by about h * tan(view zenith) (1.07 h at New York: zenith angle 47 deg,
// northwards),
// so a cell samples the line of sight to the point above it at the cloud's
// height instead of the line of sight to the ground. Plain C++ (no Windows /
// D3D).
namespace Goes {

struct Projection {
  double perspectivePointHeight = 35786023.0; // m above the ellipsoid at the sub-satellite point
  double semiMajor = 6378137.0;               // GRS80 (m)
  double semiMinor = 6356752.31414;
  double lonOrigin = -75.0;                   // sub-satellite longitude (degrees; GOES-East)

  // From the goes_imager_projection variable of a GOES-R L2 file
  // (grid_mapping_name geostationary, sweep_angle_axis x, latitude of origin 0).
  bool read(const Hdf5::File& file, std::string& error);

  // Earth-centred position of a geodetic point on this ellipsoid (m).
  glm::dvec3 ecef(double lonDeg, double latDeg, double height) const;
  // Scan angles (radians; x grows eastwards, y northwards) of the line of
  // sight to an Earth-centred position. False when the Earth hides it.
  bool scanAngles(const glm::dvec3& ecef, double& x, double& y) const;
  bool scanAngles(double lonDeg, double latDeg, double height, double& x, double& y) const;
  // The ellipsoid point seen at scan angles; false when the line of sight
  // misses the Earth.
  bool groundPoint(double x, double y, double& lonDeg, double& latDeg) const;
  // Zenith angle of the satellite seen from a ground position (degrees; 90 =
  // on the horizon).
  double viewZenith(double lonDeg, double latDeg) const;
};

// A two-dimensional L2 variable on the fixed grid: row 0 north, column 0 west.
struct Raster {
  Projection projection;
  std::string name, units;
  int columns = 0, rows = 0;  // of the stored window
  int column0 = 0, row0 = 0;  // its offset in the file's grid
  double x0 = 0.0, dx = 0.0;  // scan angle of the first stored column's centre and the step (radians)
  double y0 = 0.0, dy = 0.0;  // the same for rows (dy < 0: rows run southwards)
  std::vector<float> values;  // rows x columns, CF packing applied; NaN = missing

  // Reads a variable with dimensions (y, x) (the NetCDF order of these files),
  // its x / y coordinate variables (regular spacing required) and the
  // projection. With a box, only the rows / columns that can show it (heights
  // up to 20 km, plus two pixels; the chunks are whole image rows, so this
  // saves decoding mostly for rows).
  bool read(const Hdf5::File& file, std::string_view variable, const CloudGrid* box, std::string& error);

  // Bilinear sample at scan angles; missing neighbours are left out (weights
  // renormalised). NaN outside the raster, or when the valid neighbours carry
  // less than half of the weight (data edges end halfway between pixels instead
  // of growing by one pixel).
  float sample(double x, double y) const;
};

// Resamples a raster onto the CloudGrid cell centres. Each cell samples the line
// of sight to the point above it at a height above MSL: `heights` per cell
// (CloudGrid layout; NaN = 0) or `height` everywhere when it is empty. 0 = no
// parallax correction (surface products, the uncorrected look).
std::vector<float> resample(const Raster& raster, const CloudGrid& grid, std::span<const float> heights,
                            float height = 0.0f);

// The same in two parts, for several products of one scan (one projection, one
// set of heights): the scan angles of every cell's line of sight (NaN where the
// Earth hides it), then any raster sampled at them.
std::vector<glm::dvec2> lineOfSightAngles(const Projection& projection, const CloudGrid& grid,
                                          std::span<const float> heights, float height = 0.0f);
std::vector<float> resample(const Raster& raster, std::span<const glm::dvec2> angles);

struct ParallaxStats {
  int cloudy = 0;            // cells with a cloud top
  int unconverged = 0;       // cells whose height still moved by > 100 m in the last iteration
  double meanShiftKm = 0.0;  // cloudy cells: distance between the corrected position and the
  double maxShiftKm = 0.0;   //   uncorrected one (where the cloud top appears on the ground)
  double meanShiftNorthKm = 0.0, meanShiftEastKm = 0.0; // the mean of that shift as a vector
};

// Cloud-top heights (m above MSL, NaN = no cloud top) of a raster of them (ACHA
// `HT`) on CloudGrid with a self-consistent parallax correction: a cloud top at
// height h above a cell is seen along the line of sight to (cell, h), so each
// cell solves h = HT(line of sight(cell, h)) by fixed-point iteration on a
// gap-filled copy of the field (clear pixels take the heights of the clouds
// around them, so cloud edges move with their clouds) and then takes the
// unfilled value along the converged line of sight. `lineOfSightHeights` (every
// cell, NaN outside the raster) are the heights solved for: resample the other
// products of the same scan with them (the 2 km mask / optical depth) for the
// same correction.
std::vector<float> cloudTopHeights(const Raster& heights, const CloudGrid& grid, ParallaxStats* stats = nullptr,
                                   std::vector<float>* lineOfSightHeights = nullptr);

} // namespace Goes
