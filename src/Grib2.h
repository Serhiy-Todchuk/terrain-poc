#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Minimal GRIB2 (WMO FM 92 edition 2) reader, enough for the subsets the NOMADS
// grib filter returns for HRRR: grid template 3.30 (Lambert conformal on a
// sphere), product templates 4.0 / 4.8 (fields on a horizontal level), data
// template 5.0 (simple packing) with an optional bitmap. Anything else is
// reported as an error, never guessed. Plain C++ (no Windows / D3D) so it can
// move into a WASM build unchanged.
namespace Grib2 {

struct LambertGrid {
  int nx = 0, ny = 0;
  double la1 = 0.0, lo1 = 0.0; // first grid point in storage order (degrees)
  double lov = 0.0;            // orientation longitude (degrees)
  double latin1 = 0.0, latin2 = 0.0;
  double dx = 0.0, dy = 0.0;   // grid lengths (m) at the standard parallel(s)
  double earthRadius = 6371229.0;
  uint8_t scanMode = 0;
  bool uvGridRelative = false; // flag table 3.3 bit 5: vector components along the grid's x / y axes

  // Fractional grid index of a geographic position in the normalised layout of
  // Field::values (i grows east, j grows north). Values outside [0, n-1] lie
  // outside the grid.
  void project(double lonDeg, double latDeg, double& i, double& j) const;
  // Angle (radians) between the grid's x axis and local east at a longitude:
  // earth-relative u = cos a * ug + sin a * vg, v = -sin a * ug + cos a * vg.
  double rotation(double lonDeg) const;
};

struct Field {
  int discipline = 0, category = 0, parameter = 0; // code table 4.2
  int levelType = 0;                               // first fixed surface (code table 4.5)
  double levelValue = 0.0;
  int64_t referenceTime = 0;                       // section 1, seconds since 1970-01-01 UTC
  int forecastMinutes = 0;
  LambertGrid grid;
  std::vector<float> values;                       // nx*ny, i fastest, row 0 = south; NaN = missing

  float at(int i, int j) const { return values[size_t(j) * size_t(grid.nx) + size_t(i)]; }
  // Bilinear sample at a fractional index; missing neighbours are left out
  // (weights renormalised). NaN when outside the grid or all four are missing.
  float sample(double i, double j) const;
};

// Decodes every field of every message in `data`. On failure returns false
// with a description in `error` (fields decoded so far stay in `out`).
bool decode(std::span<const std::byte> data, std::vector<Field>& out, std::string& error);

// Common code-table values used by the weather sources.
constexpr int kLevelCloudBase = 2;
constexpr int kLevelCloudTop = 3;
constexpr int kLevelEntireAtmosphere = 10;
constexpr int kLevelIsobaric = 100; // level value in Pa
constexpr int kLevelLowCloudLayer = 214;
constexpr int kLevelMiddleCloudLayer = 224;
constexpr int kLevelHighCloudLayer = 234;

} // namespace Grib2
