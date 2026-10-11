#pragma once
#include "CloudFields.h"
#include "HttpAssetAccessor.h"

#include <CesiumAsync/AsyncSystem.h>

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Live weather for the cloud layer (docs/weather_roadmap.md, steps 1-2): NOAA
// HRRR cloud fields for a region around New York, requested from the NOMADS
// grib filter through the app's HttpAssetAccessor (disk cache, host limits,
// retries, logging), decoded and resampled to CloudGrid on a worker thread.
// Independent of the Cesium tilesets and their lifetimes. Main thread only
// (continuations run in dispatchMainThreadTasks).
//
//  * The weather time is live (the clock) or pinned (replay / forecast, minute
//    resolution). The datasets of the two available hours around it are loaded
//    (bracket()); the renderer blends them in time.
//  * Catalog of available hours: the NOMADS file lists of every day it keeps
//    (newest run per valid hour, forecasts included) plus a probe of the disk
//    cache (replays of hours seen before; the only source with --offline).
//  * live: while a newer run can appear the newest day's file list is re-read
//    (refreshSeconds); a dataset is downloaded when a newer run covers a needed
//    hour (the current hour, the next one, and the one after that late in the hour).
//  * Each dataset = two requests: the cloud fields (step-1 URL, so older caches
//    stay valid) and "dynamics" (layer winds, cloud water / ice); without the
//    second one clouds do not drift and optical depth falls back to a heuristic.
class WeatherService : public std::enable_shared_from_this<WeatherService> {
public:
  struct Options {
    std::optional<int64_t> pinnedTime; // unix seconds (UTC); nullopt = live
    bool offline = false;
    double refreshSeconds = 600.0;
    CloudGrid grid; // the weather box (default: around New York)
  };

  WeatherService(std::shared_ptr<HttpAssetAccessor> accessor, CesiumAsync::AsyncSystem asyncSystem, Options options);

  // Every frame. The disk cache is probed once regardless; nothing is
  // requested from the network while `active` is false (layer hidden).
  void update(bool active);
  // Live (nullopt) or a pinned weather time; takes effect at the next update.
  void setPinnedTime(std::optional<int64_t> time);
  std::optional<int64_t> pinnedTime() const { return _options.pinnedTime; }
  bool live() const { return !_options.pinnedTime; }
  int64_t time() const; // the weather time: pinned, or now

  // Moves the weather box ("Fetch weather here"). Datasets, summaries and the
  // disk-cache catalog belong to a box (the request URLs name it): they are
  // dropped and the cache probed again; the NOMADS listing is kept. Results of
  // requests in flight for the old box are discarded.
  void setGrid(const CloudGrid& grid);
  const CloudGrid& grid() const { return _options.grid; }



  // Loaded datasets around `t`: the latest valid at or before t and the
  // earliest after t, each at most 6 h away; either can be null.
  struct Bracket {
    std::shared_ptr<const CloudFields> before, after;
  };
  Bracket bracket(int64_t t) const;

  // Valid hours (unix seconds) with data, sorted: online file lists + disk cache.
  const std::vector<int64_t>& availableHours() const { return _available; }
  bool catalogReady() const;

  // Cloud summary of a dataset seen this session (kept after the dataset itself
  // was dropped): the history / forecast table of the settings panel.
  struct HourSummary {
    double meanCover = 0.0;                 // domain mean, 0..1
    double layerCover[kCloudLayerCount]{};  // per layer, 0..1
    double layerAltitude[kCloudLayerCount]{}; // m MSL near New York
    std::string layerName;                  // dominant layer seen from below near New York
    bool hasWind = false;
  };
  // One entry per available hour: the newest run (listed on NOMADS or in the
  // disk cache), where it comes from, and its summary when it was loaded.
  struct HourInfo {
    int64_t validTime = 0;
    int64_t cycle = 0; // model run
    int forecastHours = 0;
    bool listed = false; // on NOMADS now (downloadable)
    bool cached = false; // in the disk cache (replayable offline)
    bool loaded = false; // decoded, in memory
    const HourSummary* summary = nullptr; // null until loaded once
  };
  std::vector<HourInfo> catalog() const;
  int64_t newestRun() const; // newest listed / cached run (unix seconds), 0 = none

  // Catalog / download / decode in flight, or a needed hour not loaded yet.
  bool busy() const { return _probeBusy || _listingBusy || _fetchBusy || _pending; }
  std::string activity() const; // what is going on / the last problem ("" when idle and fine)

  // "YYYYMMDDHH[MM]" (UTC) or "-N" (N hours before the current hour) -> unix seconds.
  static std::optional<int64_t> parseTime(const std::string& text);
  static std::string formatTime(int64_t unixSeconds);    // "2026-10-10 11:37 UTC"
  static std::string formatCompact(int64_t unixSeconds); // "2026101011" / "202610101137" (--weather-time)
  static std::string describeRun(int64_t cycle, int forecastHours); // "10z +1 h"

private:
  struct Candidate {
    int64_t cycle = 0; // model run (unix seconds)
    int forecastHours = 0;
    bool operator==(const Candidate&) const = default;
  };
  struct Loaded {
    std::shared_ptr<const CloudFields> fields;
    Candidate candidate;
  };
  struct FetchResult;

  void startProbe();
  void startListing();
  void listDays(std::vector<int64_t> days, size_t index);
  void finishListing(bool ok);
  void fetch(int64_t validTime, Candidate c);
  std::optional<Candidate> best(int64_t validTime) const; // newest run listed or cached
  std::vector<int64_t> neededHours(int64_t t) const;
  void rebuildCatalog();

  std::shared_ptr<HttpAssetAccessor> _accessor;
  CesiumAsync::AsyncSystem _asyncSystem;
  Options _options;

  // catalog
  std::map<int64_t, std::vector<std::pair<int, int>>> _dayRuns; // day -> (run hour, forecast hour) listed
  std::map<int64_t, Candidate> _listed, _cached;                // valid hour -> newest run
  std::vector<int64_t> _available;
  bool _probeBusy = false, _probeDone = false;
  bool _listingBusy = false, _listingDone = false, _listingFailed = false;
  std::chrono::steady_clock::time_point _nextListing{};

  // datasets
  std::map<int64_t, Loaded> _loaded;                                             // valid hour -> dataset
  std::map<int64_t, HourSummary> _summaries;                                     // valid hour -> summary
  std::map<int64_t, std::pair<Candidate, std::chrono::steady_clock::time_point>> _failed; // retry after
  bool _fetchBusy = false;
  bool _pending = false;
  int64_t _reportedGap = 0; // weather time last reported as having no data

  int _generation = 0;   // incremented by setGrid: stale continuations are ignored
  std::string _activity; // what is in flight (HUD)
  std::string _problem;  // last failure (HUD), cleared by a successful download
};
