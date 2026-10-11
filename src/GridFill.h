#pragma once
#include <vector>

// Operations on row-major float grids (x fastest) shared by the weather sources
// (HRRR layer heights, GOES cloud-top heights). Plain C++ (no Windows / D3D).
namespace GridFill {

// Fills cells without a value (weight 0) from their surroundings by push-pull:
// a weighted pyramid down to one cell, then back up, each cell keeping its own
// value by its weight (capped at 1). False when no cell has a weight.
bool fillGaps(std::vector<float>& value, const std::vector<float>& weight, int nx, int ny);

// Separable box blur with radius r (edges clamped), in place.
void boxBlur(std::vector<float>& v, int nx, int ny, int r);

} // namespace GridFill
