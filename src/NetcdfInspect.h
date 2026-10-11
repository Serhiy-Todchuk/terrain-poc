#pragma once
#include "CloudFields.h"
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

// Developer tool behind the script command `goes` (docs/weather_roadmap.md,
// step 3): the GOES-R L2 files of one scan (URLs or paths; products ACHAC,
// CCLC, ACMC, CODC, CMIPC) on the weather box. Logs the navigation checks (the
// Product User Guide's worked example, the file's own image centre and extent,
// a round trip over every pixel, the pixels on the Earth against the cloud
// mask's count, the parallax geometry at the box centre), resamples the main
// variable of each product onto `grid` without and with the parallax
// correction (cloud-top heights from the set's ACHAC file, solved
// self-consistently: Goes::cloudTopHeights) and writes them as PNGs with a
// 1-degree graticule and coastal landmarks (green crosses) to `imageDir`, plus
// the total cover of the HRRR dataset in `hrrr` valid within 30 min of the
// scan, with the agreement of the cloud masks with it.
void inspectGoesScan(const std::shared_ptr<HttpAssetAccessor>& accessor, const CesiumAsync::AsyncSystem& asyncSystem,
                     std::vector<std::string> sources, const CloudGrid& grid,
                     std::vector<std::shared_ptr<const CloudFields>> hrrr, std::filesystem::path imageDir);
