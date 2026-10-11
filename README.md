# TerrainPOC — NYC geospatial terrain (online PoC)

C++ / Direct3D 11 application that uses **Cesium Native** as the tiling / LOD /
streaming backend and renders a merged **land + seabed** surface of the New York
harbor area with satellite imagery, simple 3D buildings, an engine-rendered sea
surface, georeferenced test vessels and **live cloud layers** from NOAA's HRRR
weather model (low / middle / high, drifting with the model winds, casting
shadows) at any date and time with weather data, optionally with the sun at that
time.

This is the *online* variant of `nyc_geospatial_terrain_poc.md`: instead of a
pre-processed local quantized-mesh pyramid, terrain tiles are generated at run
time from public web services. Cesium ion is **not** used, no API keys are needed.

Start view: lon −74.00753296050705, lat 40.66004718826513 (Bush Terminal piers,
Sunset Park, Brooklyn).

## Data sources

| Layer | Source | Resolution here | Datum |
|---|---|---|---|
| Land elevation | USGS 3DEP bare-earth DEM, `elevation.nationalmap.gov` ImageServer | 1 m lidar | NAVD88 |
| Seabed | NOAA NCEI BAG hydrographic survey mosaic, `gis.ngdc.noaa.gov/.../bag_bathymetry` | 0.5–1 m multibeam (harbor) | MLLW |
| Seabed fallback / global | NOAA NCEI DEM global mosaic (CUDEM 1/9″ topobathy, ETOPO 2022) | ~3 m | NAVD88 / MSL |
| Satellite imagery | Esri World Imagery (default) or USGS NAIP/HRO (`--imagery usgs`) | 0.3 m / ~2 m | — |
| Buildings | OpenStreetMap via OpenFreeMap vector tiles (OpenMapTiles `render_height`) | footprints | on 3DEP ground |
| Clouds | NOAA HRRR (hourly runs): cloud cover total / low / middle / high, cloud base / top, winds at 850 / 500 / 250 hPa, cloud water / ice; NOMADS grib-filter subset around NYC | 3 km | m above MSL |
| Clouds observed (in progress) | NOAA GOES-19 ABI L2, CONUS every 5 min (`noaa-goes19` open-data bucket): cloud-top height, cloud cover layers, cloud mask, cloud optical depth | 10 / 2 km | m above MSL |

Vertical datums are unified to WGS84 ellipsoidal heights (Cesium Native's
frame): `h = H_NAVD88 + N_GEOID18`, `H_NAVD88 = d_MLLW − (NAVD88 − MLLW)`.
GEOID18 is a 0.25° grid sampled from the NGS geoid API; NAVD88−MLLW (≈0.85 m in
the harbor) is interpolated between NOAA CO-OPS tide stations (The Battery, Sandy
Hook, Kings Point). Check: BAG and NCEI agree to within a few cm after this
conversion. The sea surface is drawn at NAVD88 0 m (≈ local MSL, no tides).

## How it works

* **`TerrainLoader`** — custom `Cesium3DTilesSelection::TilesetContentLoader`:
  geographic quadtree (2×1 roots, like quantized-mesh), 129×129 vertices per tile,
  max level 18 (≈0.6 m vertex spacing) around New York. Cesium Native does LOD
  selection (screen-space error), culling, load prioritisation, caching/unloading
  and drapes the imagery (`UrlTemplateRasterOverlay`) using its own overlay
  texture coordinates. Meshes have skirts, normals from a 1-sample border ring
  (seamless lighting across tiles), feature-aligned triangle diagonals, and two
  custom attributes: `_HEIGHT` (NAVD88) and `_WATERMASK`.
* **`ElevationService`** — requests float rasters
  (`exportImage?format=bsq&pixelType=F32`, one sample of border) and merges per
  sample: 3DEP lidar on land; where 3DEP is hydro-flattened (exactly flat water
  surface) or low-lying with the topobathy surface clearly deeper, the seabed
  from BAG (MLLW→NAVD88) or NCEI. Where the lidar flattened at low water, the
  seabed is kept below that surface (newer than CUDEM in the harbor); near-MSL
  flattening (Jamaica Bay marsh) defers to the topobathy model. Request
  reduction: 3DEP/bathymetry are skipped when the parent tile's height range
  shows they are irrelevant, deep tiles resample a cached level-15 NCEI raster.
* **`BuildingsLoader`** — second custom loader: OpenMapTiles z14 vector tiles
  (own MVT/protobuf decoder) → footprints clipped to the tile, extruded with
  earcut roofs, based on the 3DEP ground under the footprint. Each building gets
  a guessed façade style (punched windows, glass curtain wall, industrial, shop
  fronts) and wall material (brick, brownstone, limestone, concrete, …) from its
  height and footprint, plus façade coordinates (window bays fitted between
  corners, floors).
* **`RendererResources`** (`IPrepareRendererResources`) — glTF → D3D11 buffers
  (32-byte vertices) and imagery → mip-mapped sRGB textures, created on worker
  threads (D3D11 device is free-threaded).
* **`Renderer`** — reversed-Z infinite projection, camera-relative transforms
  composed in double precision, imagery with Cesium's overlay translation/scale,
  seabed tinted by depth with stronger hillshade, optional 1 m contour lines, a
  camera-centred curved sea surface (depth-aware transparency, Fresnel, ripples,
  sun glint), sky, aerial perspective, 4× MSAA (gamma-correct shader resolve,
  `--msaa 1|2|4|8`), Direct2D statistics HUD, Dear ImGui settings panel.
  Buildings: procedural, anti-aliased window
  façades with glass reflections; roofs show the satellite image of the roof,
  taken from a top-down capture of the terrain's already-loaded imagery
  (cascades around the camera, re-rendered only when tiles change — no extra
  downloads).
* **`WeatherService`** (+ `Grib2`) — live weather, independent of the tilesets.
  The weather time is the clock (live) or any minute with data (replay /
  forecast). A catalog of the hours with data comes from the NOMADS file lists
  (~2 days + the forecast up to 48 h) and a probe of the disk cache. The service
  loads the newest HRRR run for the two hours around the time: per hour ~4 MB
  for a ~1200 km box around New York, cloud fields plus winds and cloud water /
  ice, GRIB2 decoded by a small built-in reader. A worker thread resamples them
  to a lon/lat grid, with per-layer cover, base altitude (gaps filled from the
  neighbourhood), thickness, optical depth and wind. Responses go through the
  accessor's disk cache, so any hour seen once can be replayed
  (`--weather-time`, `--offline`).
* **Clouds** — three curved slabs (low / middle / high) between their per-cell
  base and top, crossed per pixel by a full-screen pass (sampled along the
  path inside the slab, so oblique views and flights at cloud level see the
  sides of the clouds). The two hourly datasets are blended in time with
  motion compensation, and procedural sub-grid shapes are thresholded by the
  model cover; both drift with each layer's wind field, cell by cell, and
  evolve during the hour. Slant-path opacity, two-stream
  lighting, shadowing between layers, aerial perspective, depth-tested against
  the scene, cross-faded after a data change. **Cloud quality** (panel combo,
  key `8` = the next level, `--cloud-level`): one level per roadmap step, all
  kept for comparison — *Simple shell* (step 1: one thin static shell with the
  HRRR total cover at the altitude of the layer mostly seen from below),
  *Layers + wind* (step 2, the default) and *Satellite observed* (step 3, in
  progress: the GOES-19 scan of the weather time is loaded — the newest at or
  before it, every 10 min live, ~9 MB, parallax-corrected onto the weather box —
  and shown in the HUD / Sources, not yet blended into the clouds). **Cloud shadows**: a shadow map of
  the sun's transmission through all layers over the weather domain, updated
  when the clouds or the sun change, darkens terrain, buildings, vessels, water
  and seabed. **Sun**: optionally at the weather time (solar ephemeris), with a
  time-of-day light: sky colours, sun colour and light levels from sunrise to
  night. Plan for the next steps (GOES-19, volumetric, lighting):
  [`docs/weather_roadmap.md`](docs/weather_roadmap.md).
* **`HttpAssetAccessor`** — WinHTTP `IAssetAccessor` with per-host concurrency
  limits, retries/backoff, full request/error logging and an on-disk response
  cache next to the exe (`cache\`).
* **`Vessels`** — three procedural test vessels placed by lon/lat/heading on the
  sea surface (container ship at the Upper Bay anchorage, ferry, tug).

## Build

Requirements: Visual Studio 2022 or 2026 with *Desktop development with C++*
(v143 toolset), Windows SDK 10, Git, CMake (only for the one-time Cesium Native
dependency build — VS ships one).

1. Build Cesium Native v0.65.0 and its dependencies once (≈30–60 min, ~9 GB in
   `deps\`):

   ```bat
   powershell -ExecutionPolicy Bypass -File scripts\build_deps.ps1
   ```

   It clones Cesium Native, builds Debug + Release with Cesium's own vcpkg helper
   (toolset pinned to v143), installs headers/libs into `deps\install`
   (`lib\` Release, `debug\lib\` Debug) and copies a few headers of Cesium's
   vcpkg dependencies to `third_party\` (`earcut.hpp`; the zlib-ng API, used by
   the HDF5 reader). It also clones Dear ImGui (v1.92.9b) into
   `third_party\imgui` — its sources are compiled by the app project. On an
   existing checkout, `build_deps.ps1 -ImGuiOnly` fetches just ImGui and copies
   the headers again (seconds).

2. Open `TerrainPOC.sln` and build x64 Debug/Release (or
   `powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Config Release`).
   The project links every library in the dependency install folder and fails
   with a clear message if step 1 has not been run.

Output: `bin\<Config>\TerrainPOC.exe` + `shaders\Scene.hlsl` (compiled at run
time; `F5` reloads it).

## Run / controls

```bat
bin\Release\TerrainPOC.exe
```

* **Left-drag**: orbit around the point under the cursor
* **Right-drag**: pan (the grabbed ground point follows the cursor)
* **Wheel**: zoom towards the point under the cursor
* WASD / QE fly, arrows look, `R` reset view
* `1` terrain, `2` imagery, `3` buildings, `4` water, `5` vessels, `6` clouds,
  `7` cloud shadows, `8` next cloud quality level, `L` sun at the
  weather time, `O` fog, `C` 1 m contours,
  `F` wireframe, `M` MSAA on/off, `T` building façades, `H` HUD,
  `G` settings panel, `P` screenshot, `F5` reload shaders, Esc quit

The **HUD** (left) shows statistics and info: fps, the camera in `--camera`
syntax, the coordinates and NAVD88 elevation of the point under the cursor,
tiles / load queues, the weather time (UTC and New York time, live / replay,
sun elevation and azimuth), the HRRR datasets of the time blend, cover /
altitude / wind per cloud layer, at *Satellite observed* the GOES-19 scan (its
time, cloud tops, mask, optical depth), HTTP, GPU and process memory, log
counts, data credits.

The **settings panel** (right, Dear ImGui) has two tabs. **View**: every
toggle with its key, MSAA sample count, fog, vsync, water absorption, contour
interval, camera views (the harness views), go-to
(`lon,lat,alt,heading,pitch[,range]` or a pasted `--camera …`), copy `--camera`
to the clipboard, field of view, fly speed, terrain / buildings screen-space
error (live), screenshot, shader reload, stats to the log. **Time & weather**:
*Now* (live), the date (only days with weather data, ◀ ▶), the time of day
(only hours with data), ±1 h, *Apply to sun*, cloud shadows, the cloud
quality (*Simple shell* / *Layers + wind* / *Satellite observed*), the cloud layers low / middle /
high; **History & forecast** — the hours with data ±24 / 48 /
72 h around the weather time (UTC, model run, where the data is, cover of the
low / middle / high layers once a dataset was seen; the current hour is tinted,
a row pins the time); **Sources** — the weather box (its centre, how far the
camera is, **Fetch weather here** moves it to the camera, *New York* brings it
back), the HRRR facts, the catalog (hours, newest run and how far it
forecasts), the loaded datasets, the GOES-19 scan and listings (at *Satellite
observed*) and the service status. The weather box is the
~1200 × 1130 km area the clouds are requested for; moving it downloads the HRRR
hours around the weather time for the new area (about 4 MB per hour, cached
on disk like everything else) and works anywhere HRRR covers, the contiguous
US; the HUD's `clouds` line then shows `HRRR @ lon, lat`. The box persists
with the UI settings (`--weather-center lon,lat|default` on the command line).
Panel changes are logged (`Settings panel: …`). While the mouse is over the
panel or a text field is being edited, the scene ignores that input (a drag
that started in the scene continues over the panel). Both scale with the
monitor DPI.

The small **arrows** hide and show both panels: the one in the HUD's top-left
corner (`H`) and the one in the panel header (`G`); a hidden panel leaves just
its arrow at the window edge.

The **UI settings persist** in `TerrainPOC.ini` next to the exe (written at
exit): layer / rendering / weather toggles, MSAA, vsync, water absorption,
contour interval, field of view, fly speed, screen-space errors, HUD / panel
shown, the open tab and sections, the history range, the window size. Not the
weather time (live at every start) and not the camera (`--camera` /
`--lookat`). Command-line options override the file; `--no-settings` ignores
it, `--settings file` uses another file; harness runs (`--script`,
`--screenshot`) never read or write it unless `--settings` is given. `--help`
lists all options (`--terrain-sse`, `--imagery usgs`, `--water-clarity`,
`--hud 0`, `--panel 0`, …).

## Test harness

* `--camera lon,lat,alt,heading,pitch` / `--lookat lon,lat,alt,heading,pitch,range`
  (alt in metres above MSL).
* `--screenshot out.png` — waits until **everything** is loaded (HTTP idle,
  worker pool idle, Cesium load queues empty, no pending imagery tiles, for 10
  consecutive frames), saves a PNG, exits (exit code 3 if loading timed out).
* `--script file.txt --shot-dir dir` — command scripts. Live mouse/keyboard input
  is ignored while a script runs.

  | Command | |
  |---|---|
  | `camera lon,lat,alt,heading,pitch` / `lookat …,range` | place the camera |
  | `fly lon,lat,alt,heading,pitch seconds` | smooth flight (streaming while moving) |
  | `wait [timeout]` | wait until all tiles/imagery are loaded |
  | `shot name` | screenshot to `<shot-dir>/name.png` |
  | `drag left\|right x0 y0 x1 y1 [steps]` | synthetic orbit / pan through the real handlers |
  | `wheel x y notches` | synthetic zoom towards a pixel |
  | `key K`, `set terrain\|imagery\|buildings\|water\|vessels\|clouds\|shadows\|cloudlow\|cloudmid\|cloudhigh\|sun\|fog\|contours\|hud\|panel\|wireframe\|msaa\|facades\|vsync on\|off` | toggles |
  | `cloudlevel shell\|layers\|satellite` | the cloud quality level |
  | `weather live\|YYYYMMDDHH[MM]\|-N` | the weather time: live, a fixed UTC time or N hours ago (replay / forecast, cross-fades) |
  | `weather here`, `weather center lon,lat\|default` | move the weather box to the camera / a point / back to New York |
  | `set sections on\|off` | open / close every section of the shown settings-panel tab (once) |
  | `tab view\|weather` | select a settings-panel tab |
  | `click x y`, `scroll x y notches`, `type text` / `type <enter>` | real window messages (panel first, then the scene) |
  | `netcdf url\|path [variable …]` | log a NetCDF-4 / HDF5 file (variables, decoded statistics) and save PNGs of the named 2D variables (developer tool for the GOES-19 data) |
  | `goes url\|path …` | the GOES-19 L2 files of one scan on the weather box: navigation checks, the main variable of each product resampled with and without the parallax correction (PNGs with a graticule and coastal landmarks), compared with the HRRR hour loaded for the weather time (developer tool); without arguments: PNGs of the scan the weather service loaded for the weather time (cloud tops, cover, the five CCLC height bands, mask, optical depth) |
  | `size WxH`, `sleep s`, `stats`, `log text`, `quit` | misc |

  Included: `scripts\shots_poi.txt` (POI views incl. 1 m contours on land and
  seabed), `nav_test.txt` (orbit/pan/zoom), `scenarios.txt` (vessel's-eye,
  30 km, globe), `flythrough.txt` (long flights, cache eviction, resize),
  `msaa_test.txt` (MSAA off/on comparison), `facades_test.txt` (plain vs
  textured buildings), `panel_test.txt` (settings panel: clicks, wheel and
  typing routed panel-vs-scene, both tabs, weather date / time and sun
  controls, the cloud quality combo, the history table, sections, the hide / show arrows, hidden,
  narrow window), `settings_test_a.txt` + `settings_test_b.txt` (persisted
  settings: phase A changes the UI and quits with `--settings file`, phase B
  starts with the same file and shows the restored state, `--hud 1` overriding
  it), `weather_center_test.txt` (the weather box moved to Boston and back;
  needs the network once), `clouds_test.txt` (cloud layers from below / 3 km /
  30 km, each layer alone, shadows on / off, 10 minutes of drift, fog off,
  sunrise / sunset / night, the weather domain, the simple shell level, live),
  `netcdf_test.txt` (the HDF5 reader on four GOES-19 cloud products from the
  NOAA bucket; expected values in its header), `goes_nav_test.txt` (GOES
  navigation and parallax correction: two scans on the weather box, a clear
  night for the coastline check, the HRRR hour for comparison; expected values
  in its header), `goes_source_test.txt` (the GOES-19 scans of the weather
  service at *Satellite observed*: replays, the previous hour / day, a moved
  box, idle at *Layers + wind*, live), `regression.txt`.
  Screenshots include the HUD and the panel (`--hud 0`, `--panel 0` or
  `set panel off` for clean scene shots). The clouds are the live weather unless
  `--weather-time` / `weather` pins a time (`--no-clouds` or `set clouds off`
  for weather-independent shots); `wait` includes the weather download and the
  cross-fade.
* `--offline` — no network at all: every request is served from the disk cache
  (misses behave like 404). Any area visited once can be replayed offline; the
  clouds show the cached hours around the weather time (the date / time controls
  offer exactly the cached hours).
* Logging (`bin\<cfg>\TerrainPOC.log`, console, debugger output): every HTTP
  request with status, size and latency or cache hit, plus per-tile summaries
  (`--verbose`), every HTTP/transport failure and retry, Cesium Native's own log,
  tileset/raster load failures, every failed HRESULT with expression and source
  location, shader compiler output, D3D11 debug-layer messages with
  `--d3d-debug` (needs the *Graphics Tools* Windows feature), Debug-CRT asserts
  (including Cesium Native's `CESIUM_ASSERT`, logged instead of a dialog),
  periodic stats (tiles, requests, GPU and process memory), and on a crash a
  symbolized stack trace plus `TerrainPOC_crash.dmp`. The last log line reports
  the number of errors and warnings.

## Measured (RTX 3070 Laptop, 1600×900)

* Start view, cold network: fully loaded in ~40 s (~1300 requests; public
  ImageServers answer in ~0.6–1 s each). From the disk cache: < 1 s.
* 100–120 fps loaded (vsync; MSAA 4× costs ~0.4–0.7 ms/frame, the three cloud
  layers up to ~0.9 ms/frame under a full sky, cloud shadows ~0.1–0.3 ms); ~300–700
  MB GPU meshes; 1.0–1.9 GB process memory (bounded by Cesium's cache budgets:
  terrain 384 MB, buildings 192 MB).
* Debug build runs the full flythrough with no Cesium assertion failures.

## Known limitations

* Bare-earth lidar removes piers on pilings; OSM buildings standing on such piers
  are based on the seabed under them.
* NCEI CUDEM is older than the lidar in places; the merge rules above prefer the
  lidar water extent in the harbor.
* Esri World Imagery is used for best resolution in this PoC; check its terms for
  production use or switch to public-domain USGS imagery (`--imagery usgs`).
* Building façades are procedural: OpenMapTiles has no building type or
  material, so style and material are guessed. Roof imagery fades out on towers
  (their roofs are displaced in non-true-ortho imagery).
* No tides: the sea surface is at NAVD88 0 m. The authored 40×40 km area
  integration from the PoC doc is out of scope here.
* Clouds (steps 1–2 of `docs/weather_roadmap.md`): slabs per layer without 3D
  detail yet (no billows or wisps); middle / high layer bases are estimates
  where lower layers exist (NOMADS offers no pressure-level filter for HRRR);
  data within ~550 km of New York (HRRR's own domain ends ~600 km to the east).
  NOMADS keeps ~2 days of HRRR (+ forecasts); older hours replay only from the
  disk cache. The time-of-day light is an exposure model (no moon, no city
  lights).
* GOES-19 (step 3, in progress): the scans are loaded and shown in the HUD /
  panel but not blended into the clouds yet. Whole files (~9 MB per scan, every
  10 min live: ~54 MB/h); scans of the running hour bypass the disk cache, and
  a finished hour replays offline only after it was viewed online once it ended.

## Layout

```
TerrainPOC.sln / TerrainPOC.vcxproj
src/          application sources
shaders/      Scene.hlsl
scripts/      build_deps.ps1, build.ps1, harness scripts
docs/         weather_roadmap.md (live weather / clouds plan and status)
third_party/  mapbox/earcut.hpp, zlib-ng/, imgui/ (fetched / copied by build_deps.ps1)
deps/         Cesium Native source, build and install (generated)
```
