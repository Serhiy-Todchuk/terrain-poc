#pragma once
#include "HttpAssetAccessor.h"

#include <CesiumAsync/AsyncSystem.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Developer tool behind the script command `netcdf` (docs/weather_roadmap.md,
// step 3): reads a NetCDF-4 / HDF5 file - a URL through the HttpAssetAccessor
// (disk cache, host limits, retries) or a local path - with the Hdf5 reader on
// a worker thread and logs its structure: the key global attributes, every
// variable with its type, shape, layout and its decoded values or statistics
// (CF packing applied); all attributes at debug level. Each variable named in
// `images` with two dimensions is also written as a greyscale PNG (min..max,
// missing values dark red, row 0 at the top) to `imageDir` on the main thread.
// Every failure is logged as an error.
void inspectNetcdf(const std::shared_ptr<HttpAssetAccessor>& accessor, const CesiumAsync::AsyncSystem& asyncSystem,
                   const std::string& source, std::vector<std::string> images, std::filesystem::path imageDir);
