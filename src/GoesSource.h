#pragma once
#include "CloudFields.h"
#include "HttpAssetAccessor.h"

#include <CesiumAsync/AsyncSystem.h>

#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The GOES-19 ABI Level-2 products of the cloud layer (docs/weather_roadmap.md,
// step 3), CONUS sector, one file per product and 5-minute scan.
enum GoesProduct : int {
  kGoesCloudTop,     // ACHAC: cloud-top height HT (10 km pixels)
  kGoesCoverLayers,  // CCLC: total cloud fraction TCF + cover in five height bands CF1..CF5 (10 km)
  kGoesCloudMask,    // ACMC: binary cloud mask BCM (2 km)
  kGoesOpticalDepth, // CODC: cloud optical depth COD at 640 nm (2 km)
  kGoesProductCount
};
constexpr int kGoesAllProducts = (1 << kGoesProductCount) - 1;
const char* goesProductId(int product); // "ACHAC", "CCLC", "ACMC", "CODC"

// CCLC height bands, tops in m above MSL (0-5000, 5000-10 000, 10 000-18 000,
// 18 000-24 000 ft, above).
constexpr int kGoesBandCount = 5;
constexpr float kGoesBandTop[kGoesBandCount] = {1524.0f, 3048.0f, 5486.4f, 7315.2f,
                                                std::numeric_limits<float>::infinity()};

// "OR_ABI-L2-ACHAC-M6_G19_s20262840001181_e..._c....nc" -> the product ("ACHAC",
// "CMIPC", ...) and the scan start (unix seconds, whole seconds).
bool parseGoesFileName(const std::string& name, std::string& product, int64_t& start);

// One GOES scan on CloudGrid, every product parallax-corrected along the lines
// of sight solved from its cloud-top heights (Goes::cloudTopHeights): the
// observed cloud state that corrects the HRRR fields (plan item 5). Row-major
// like CloudFields; NaN = missing (outside the image, no retrieval); a product
// that is not part of the scan leaves its vectors empty.
struct GoesScan {
  CloudGrid grid;
  int64_t start = 0; // scan start, unix seconds (UTC)
  int products = 0;  // bitmask of the GoesProduct values present

  std::vector<float> cloudTop;     // ACHA HT, m above MSL; NaN = clear or no retrieval
  std::vector<float> totalCover;   // CCLC TCF, 0..1; NaN = missing (also over large high-cloud systems)
  std::array<std::vector<float>, kGoesBandCount> bandCover; // CCLC CF1..CF5, 0..1 per height band; valid
                                                            //   almost everywhere (also where TCF is missing)
  std::vector<float> cloudMask;    // ACM BCM, 0 clear .. 1 cloudy (bilinear between pixels); counts thin cirrus
  std::vector<float> opticalDepth; // COD (640 nm); NaN = clear or no retrieval

  // Summary over the box (log, HUD, panel).
  double cloudTopFraction = 0.0; // cells with a cloud top
  double meanCloudTop = 0.0;     // m, their mean
  double meanShiftKm = 0.0;      // the parallax correction's mean shift of the cloud tops
  int unconverged = 0;           // cells whose line-of-sight height still moved by > 100 m
  double cloudyFraction = 0.0;   // cloud mask: mean over its valid cells
  double opticalDepthFraction = 0.0, meanOpticalDepth = 0.0; // COD: cells with a retrieval, their mean
  double totalCoverFraction = 0.0, meanTotalCover = 0.0;     // TCF: valid cells, their mean
  double bandFraction = 0.0;                    // CF1..CF5: the smallest valid fraction of the five
  double meanBandCover[kGoesBandCount]{};       //   and each band's mean over its valid cells
  size_t bytes = 0;      // the files' size
  double decodeMs = 0.0; // worker time: HDF5, parallax, resampling
};

// GOES-19 scans from the NOAA open-data bucket (noaa-goes19 on AWS; keys
// ABI-L2-<product>/<year>/<day of year>/<hour>/OR_..._s<start>_e<end>_c<created>.nc)
// through the app's HttpAssetAccessor, decoded and resampled onto CloudGrid on a
// worker thread. Part of WeatherService (main thread only; continuations run in
// dispatchMainThreadTasks), active only while the satellite level is selected.
//
//  * The weather time t picks the scan: the newest one starting at or before t
//    (at most kMaxAge earlier) whose four products are listed, else the newest
//    with cloud-top heights; scans that failed are skipped until their retry.
//  * The hours overlapping [t - kMaxAge, t] are listed (one S3 listing per
//    product and hour). A finished hour (listed > 30 min after its end) is
//    complete: its listing and scan files go through the disk cache (replays,
//    --offline). An unfinished hour bypasses the cache and is listed again every
//    refreshSeconds while needed - live, a newer scan every 10 min.
//  * One scan download at a time (~9 MB, host limit 4); the 3 scans nearest to
//    the weather time stay loaded.
class GoesSource : public std::enable_shared_from_this<GoesSource> {
public:
  struct Options {
    bool offline = false;
    double refreshSeconds = 600.0; // unfinished hours are listed again after this
    CloudGrid grid;                // the weather box
  };
  static constexpr int64_t kMaxAge = 30 * 60; // older scans (before the weather time) are not used

  GoesSource(std::shared_ptr<HttpAssetAccessor> accessor, CesiumAsync::AsyncSystem asyncSystem, Options options);

  // Every frame. `active` = scans are wanted; nothing is requested otherwise.
  void update(int64_t t, bool active);
  // The weather box moved: the loaded scans are dropped (the files come back
  // from the disk cache when their hour is finished); listings are kept.
  void setGrid(const CloudGrid& grid);

  // The loaded scan for t: the newest starting at or before t and at most
  // kMaxAge earlier; null when there is none.
  std::shared_ptr<const GoesScan> scan(int64_t t) const;

  // What the listings offer before the weather time (panel), from the last update.
  struct Status {
    int scansListed = 0;     // scans in the listed hours before the weather time
    int64_t newestListed = 0; // their newest start (0 = none)
    int64_t wanted = 0;      // the scan chosen for the weather time (0 = none)
    bool wantedComplete = false; // with all four products
  };
  const Status& status() const { return _status; }

  // A listing or download in flight, or the wanted scan / a needed listing not there yet.
  bool busy() const { return _listingsInFlight > 0 || _fetchBusy || _pending; }
  std::string activity() const; // what is going on / the last problem ("" when idle and fine)

private:
  struct ListedFile {
    std::string url;
    uint64_t size = 0;
  };
  struct Listing {
    std::array<std::map<int64_t, ListedFile>, kGoesProductCount> files; // per product: scan start -> file
    bool listed = false, failed = false, busy = false;
    bool final = false;                                  // listed after the hour was finished: complete
    std::chrono::steady_clock::time_point next{};        // the next listing (refresh / retry)
  };
  struct Candidate {
    int64_t start = 0;
    std::array<ListedFile, kGoesProductCount> files; // empty url = not listed
    bool cacheable = false;                           // its hour is finished
    int mask() const;
  };
  struct Loaded {
    std::shared_ptr<const GoesScan> scan;
    int requested = 0; // the products requested (decoding may have dropped some)
  };
  struct FetchResult;

  void list(int64_t hour);
  void fetch(const Candidate& c);
  std::optional<Candidate> choose(int64_t t, Status& status) const; // also fills the listing counts

  std::shared_ptr<HttpAssetAccessor> _accessor;
  CesiumAsync::AsyncSystem _asyncSystem;
  Options _options;

  std::map<int64_t, Listing> _hours; // hour start -> listing
  int _listingsInFlight = 0;
  std::map<int64_t, Loaded> _loaded; // scan start -> scan
  std::map<int64_t, std::pair<int, std::chrono::steady_clock::time_point>> _failed; // start -> (products, retry after)
  bool _fetchBusy = false;
  bool _pending = false;
  Status _status;
  int64_t _reportedGap = 0; // weather time (hour) last reported as having no scan
  int _generation = 0;      // incremented by setGrid: stale results are ignored
  std::string _activity;    // what is in flight (HUD)
  std::string _problem;     // last failure, cleared by a loaded scan
  std::string _gap;         // no scan for the weather time (this frame)
};
