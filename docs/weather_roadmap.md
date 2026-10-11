# Live Weather / Clouds — Roadmap and Plan

Status (2026-10-11): **steps 1 and 2 implemented**, step 3 (GOES-19) started
(data facts, decision and plan in [Step 3](#step-3--goes-19-observed-clouds)) — see
[Step 1 — as built](#step-1--as-built) and
[Step 2 — as built](#step-2--height-aware-layers-motion-shadows-as-built)
(low / middle / high layers at their own altitudes, wind drift, time blending,
cheap cloud shadows; plus the weather date / time UI and the sun at that time).
2026-10-11: three of the step-2 limits lifted — the wind is a field (per cell,
semi-Lagrangian blend and noise drift), the layers are slabs with thickness
(sampled along the path inside them), the weather box is four times larger
(≈ 1200 × 1130 km). Steps 3–6 are the plan.

## Goal

Extend the NYC Terrain PoC with a **live cloud and weather layer** driven by open
NOAA data:

- stream current cloud/weather data for the NYC region;
- cache it through the existing HTTP/cache infrastructure (`HttpAssetAccessor`);
- render clouds independently from the Cesium terrain/building tiles;
- keep the system suitable for a later WASM build;
- evolve from a simple cloud layer to volumetric clouds with lighting and
  scattering.

The weather system stays a separate subsystem from `TerrainLoader`,
`BuildingsLoader` and Cesium tile selection: it owns its requests, has no tile
lifetimes, and hands the renderer one source-independent grid.

## Architecture

```text
                    WeatherService                (src/WeatherService.*)
                         |   weather time (live / pinned), catalog of hours,
                         |   datasets around the time, async fetch, replay
          +--------------+--------------+
          |                             |
      HrrrSource  (steps 1-2)       GoesSource  (step 3)
          |   NOMADS grib filter        |   GOES-19 ABI L2
          |   GRIB2 -> Grib2.*          |
   cover low/mid/high/total       mask / optical depth
   cloud base / top, winds,       observed top height
   cloud water / ice                    |
          +------- resample to ---------+
                         |
                   CloudFields on CloudGrid      (src/CloudFields.h)
                   per layer: cover, base, thickness, optical depth, wind
                         |
                         v
     dataset textures (3-slice RGBA16F each)       Renderer::createCloudTexture
                         |
                         v
     CSBlendClouds: time blend + wind drift + cross-fade -> cloud field (mips)
                         |                    \
                         v                     v
     cloud pass (PSClouds, 3 shells)      CSCloudShadow -> shadow map -> terrain,
        +----------------+----------+       buildings, water (shaders/Scene.hlsl)
        |                           |
 shells: ray/sphere hits     volumetric: ray march between
 (steps 1-2)                 base and top (steps 4-5)
```

Every source resamples its native projection (HRRR: Lambert conformal; GOES:
geostationary fixed grid) onto `CloudGrid`. The renderer only knows that grid, so
GOES can be added without touching the renderer, and the volumetric renderer
reads the same textures as the shell.

## Data facts (checked 2026-10-10)

**HRRR via NOMADS** (`https://nomads.ncep.noaa.gov/cgi-bin/filter_hrrr_2d.pl`)

- Hourly runs, forecasts f00–f18 every hour (f00–f48 at 00/06/12/18z), CONUS 3 km
  Lambert conformal (1799×1059, LoV 262.5°, standard parallel 38.5°, sphere
  R = 6 371 229 m). A run is published ≈ 50 min after its hour. The filter's
  top-level page (`filter_hrrr_2d.pl`, 2.5 KB) lists the day directories it
  keeps — **2 days** on 2026-10-10 (`hrrr.20261009`, `hrrr.20261010`), plus the
  forecast up to 48 h ahead.
- The **grib filter** cuts a lon/lat box out of the grid (`subregion=&leftlon=…`)
  and returns only the selected messages, **simple packing (template 5.0)**, so a
  ~150-line GRIB2 reader suffices (`src/Grib2.*`). Our box (−81.6…−66.4°,
  35.15…46.15° since 2026-10-11; before: −78…−70°, 37.7…43.6° = 273×269 points,
  six cloud fields ≈ 400 KB) gives ≈ 4 MB per dataset (cloud fields ≈ 1.5 MB +
  dynamics ≈ 2.5 MB). The filter clips a box that reaches past the HRRR domain
  (its north-eastern corner does: those cells are clear).
- **Only the 2D files have a filter**: `filter_hrrr_2d.pl` (wrfsfc) and
  `filter_hrrr_sub.pl` (sub-hourly); there is no pressure-level (`wrfprs`) filter
  (checked 2026-10-11), and the 2D file has no humidity profile (HGT at 1000 /
  850 / 700 / 500 mb, the LCL, the ceiling and the boundary-layer cloud cover
  exist). Per-layer cloud bases under lower layers therefore stay estimates; the
  3D file is only on the AWS archive as whole-CONUS messages (~1.5 MB each).
- **Never call the filter without `var_`/`lev_` selections**: it then returns
  the whole ~130 MB CONUS file.
- A run that is not published yet answers **HTTP 403** ("Request for Future
  Data"). The file list (`?dir=%2Fhrrr.YYYYMMDD%2Fconus`, 60–110 KB of HTML,
  `<option value="hrrr.tHHz.wrfsfcfFF.grib2">`) tells what exists without
  provoking errors. NOMADS blocks clients above ~120 requests/min (we make a few
  per hour, host limit 2).
- Fields used (wrfsfc): `TCDC` entire atmosphere (level 10), `LCDC`/`MCDC`/`HCDC`
  low/middle/high cloud layer (levels 214/224/234; NCEP splits at 642 and
  350 hPa ≈ 3.8 and 8 km), `HGT` at cloud base (2) and cloud top (3), in
  geopotential metres above MSL. Base/top are bitmapped (undefined where clear).
  **Cloud base is the base of the lowest layer**, and cloud top is defined at far
  fewer points than base — neither describes a multi-layer sky on its own.
- Step 2 adds (second request, "dynamics", 8 messages ≈ 0.75 MB): `UGRD`/`VGRD`
  at 850/500/250 mb (`lev_850_mb` …; GRIB2 0-2-2/3, level type 100, value in Pa)
  — **grid-relative** components (flag table 3.3 bit 5 set: rotate by
  n·(lon − LoV), ≈ 14.6° at NYC) — and `TCOLW`/`TCOLI` (0-1-69/70) at
  `lev_entire_atmosphere` (the form's `entire_atmosphere_(considered_as_a_single_layer)`
  returns nothing for them). **TCOLW is ~0 where HRRR reports most low cover**
  (p90 0.0006 kg/m², 2026-10-10 15z): the cover is largely sub-grid cloud
  without resolved condensate, so the paths only raise a thickness-based
  optical depth. Per-layer bases/tops do not exist in the 2D file (no
  `low_cloud_bottom_level` …).
- Also in the 2D file, for later: `UGRD`/`VGRD` at 300/700/925 mb and 10/80 m,
  `HGT` at cloud ceiling, `VIS`, `REFC` (radar), `PRES` at cloud base/top.
- **WASM**: NOMADS responses carry no CORS headers (checked) — a browser build
  needs a same-origin proxy, or the AWS archive below (CORS to be verified).
- Archive / fallback: `s3://noaa-hrrr-bdp-pds` (also Google/Azure) keeps every run
  forever but has no subsetting: per-message HTTP range requests via the `.idx`
  files (full CONUS fields in the native packing — complex packing or JPEG2000
  need a bigger decoder than 5.0; to be checked when needed).

## Step 1 — HRRR cloud shell (as built)

What the plan asked: download a small NYC subset; decode cover, base, top;
render a simple curved cloud shell; refresh periodically; cache responses.

**Data path** (`WeatherService`, `HrrrSource` in `src/WeatherService.cpp`):

- **Live**: every 10 min the file list (request header `Cache-Control: no-cache`
  bypasses the disk cache) picks the newest run with a forecast for the nearest
  whole hour V; the dataset is downloaded only if it differs from the shown one.
  Once run V−1 is shown nothing newer can exist, so the next check waits for the
  next hour. Typical traffic: 1–2 × 0.4 MB + a few lists per hour.
- **Offline** (`--offline`): the newest cached dataset of the last 24 h.
- **Replay** (`--weather-time YYYYMMDDHH` or `-N` hours ago; script
  `weather …|live`): that valid hour, analysis first (f00, then f01–f03 of older
  runs). Every downloaded dataset is in the disk cache under its URL, so replays
  work forever; NOMADS serves ~2 days back. The log prints the replay argument of
  each dataset shown.
- Grib-filter text responses are never cached (as `exportImage`); the accessor
  logs every HTTP failure, the service logs what it means (warning when online
  data is missing, error on undecodable GRIB2, info for an empty cache offline).
- Decode + bilinear resampling to `CloudGrid` (288×204 cells of 0.025°, lon
  −77.6…−70.4, lat 38.1…43.2, ≈ 600×570 km; since 2026-10-11 576×408 cells, lon
  −81.2…−66.8, lat 35.55…45.75, ≈ 1200×1130 km) on a worker thread; results reach
  the main thread through `thenInMainThread`; nothing touches Cesium tiles.

**Layer altitude** (one shell per dataset, as the plan allowed): the layer an
observer near NYC mostly sees from below — random overlap (low hides middle,
both hide high) over 150 km around the POI — at its cover-weighted median height
(low/middle: cloud base; high: base if ≥ 6.5 km, else top − 1.5 km). Manual
override: panel slider / `--cloud-altitude`. On a dataset switch the layer
moves to the new altitude during the 8 s cross-fade.

**GPU resources**: one `RGBA16F` texture per dataset (cover, base km, top km,
optical depth estimate) with mips; current + previous for the cross-fade
(≈ 0.6 MB). Optical depth for step 1 (until a physical source is wired up):
water clouds (low/middle) 3 + 4 per km of thickness, ice (high) 1, mixed by layer
cover.

**Renderer** (`Renderer::drawClouds`, `PSClouds`): a full-screen pass after the
water — not a mesh — so the step to ray marching keeps the same structure.

- Ray / sphere intersection with a sphere osculating the ellipsoid at the camera
  nadir (clamped into the grid), radius + layer altitude; the large terms are
  formed in double on the CPU, the shader uses the cancellation-free root form.
- Hit point → geodetic lon/lat → grid uv; soft round fade over the outer quarter
  of the grid (no data edge, also from orbit).
- Sub-grid shapes: footprint-filtered gradient-noise fBm (14 km … 0.45 km)
  thresholded by the cover (logistic CDF ⇒ cloudy share ≈ model cover);
  unresolvable octaves fall back to the mean cover, so distant clouds keep the
  right coverage and nothing aliases.
- Opacity = 1 − exp(−τ/μ) along the slant path; light from a two-stream
  non-absorbing layer: base = transmitted minus the direct beam (+ a
  Henyey-Greenstein glow near the sun through thin cloud), top = reflected;
  aerial perspective on the cloud *and* on the sky seen through gaps in cloudy
  areas.
- `SV_Depth` = shell depth with a read-only depth test, so terrain, buildings
  and vessels in front occlude it per MSAA sample; clouds never write depth
  (picking goes through them).
- Cost ≈ 0.2 ms/frame at 1600×900, 4× MSAA (RTX 3070 Laptop, A/B in one run).

**UI / harness**: key `6`, panel *Layers → Clouds* (+ auto/manual altitude),
HUD line (run, valid time and age, cover low/mid/high, layer altitude, replay
flag, activity / problems), `[stats]` field, `set clouds on|off`,
`weather live|YYYYMMDDHH|-N`, `--no-clouds`, `--weather-time`,
`--cloud-altitude`. `wait` includes the weather check and the cross-fade.
`scripts/clouds_test.txt`: below / overhead / 3 km / 30 km / globe, replay 6 h
ago with cross-fade, back to live, key toggle.

**Known limits of step 1**: one altitude for the whole grid; shapes are static
(no wind) and change in hourly steps (8 s cross-fade); the domain ends ≈ 300 km
from NYC (fades out); clouds do not dim the scene or cast shadows; fixed sun;
replays older than ~2 days need the disk cache. (Step 2 lifts most of these.)

### Success criteria (from the PoC brief)

| Criterion | Step 1 | Step 2 |
|---|---|---|
| Current clouds around NYC visibly driven by live NOAA data | yes (HRRR, newest run, checked hourly) | yes, plus winds and the time blend between hours |
| Cloud updates do not affect terrain/building streaming | yes (separate requests, host limit 2, no tile coupling) | yes (2 requests per dataset) |
| Weather data replayable from the disk cache | yes (`--weather-time`, `--offline`) | yes, to the minute; the date / time UI offers the cached days |
| Cloud altitude varies plausibly from HRRR | per dataset (dominant layer) | per layer and grid cell |
| GOES-19 can be added without restructuring the renderer | yes (`CloudFields`/`CloudGrid` boundary) | yes (per-layer `CloudLayer` fields) |
| Clear migration from shell to volumetric | yes (full-screen ray pass, same textures) | yes (per-layer base, thickness, optical depth on the GPU) |

## Step 2 — Height-aware layers, motion, shadows (as built)

What the plan asked: low / middle / high layers at their own altitudes,
thickness / optical depth from `TCOLW`/`TCOLI`, wind (noise advected with the
time since the valid time, deterministic; forecast hours blended in time
instead of switching at the hour), cheap cloud shadows. Added on request: a
weather date / time UI limited to the hours with data, "Now", and the sun at
that time ("Apply to sun") with a time-of-day light; a fog (aerial perspective)
toggle.

**Data** (`HrrrSource::decode` in `src/WeatherService.cpp`):

- Two requests per dataset: the step-1 cloud fields (same URL, so step-1 caches
  stay replayable) and "dynamics" (winds 850 / 500 / 250 mb, `TCOLW`/`TCOLI`,
  ≈ 0.75 MB). The dynamics are only requested where they can exist (cached, or
  the run is listed); without them the clouds do not drift and the optical depth
  is the heuristic (info in the log).
- Per layer and cell (`CloudLayer`): cover (LCDC / MCDC / HCDC); base altitude:
  HRRR's cloud base for the lowest layer present (if plausible for it),
  cloud top − typical thickness for the highest; everything else is filled by
  push-pull (weighted pyramid) and smoothed (2 × box r = 2); clamped into the
  NCEP bands and kept apart (middle ≥ low + 0.7 km, high ≥ middle + 1 km).
  Thickness: top − base where a single layer is present, else filled / defaults.
- In-cloud optical depth: max(heuristic, condensate): water layers 3 + 4 per km
  of thickness, ice 1; 150 · LWP / 55 · IWP (r_e 10 / 30 µm) spread over the
  cloudy part of the cell (TCOLW is ~0 for most HRRR low cover: data facts).
- Winds: earth-relative per cell and layer (850 → low, 500 → middle, 250 →
  high), lightly blurred; the mean within 200 km of the box centre feeds the HUD
  and the shadow map's change key. (Until 2026-10-11 one vector per layer.)
- Logged per dataset: cover, altitude near New York, mean optical depth and wind
  per layer, the layer seen from below.

**Time and catalog** (`WeatherService`):

- The weather time is live (the clock) or pinned to the minute
  (`--weather-time YYYYMMDDHH[MM]`, script `weather`, panel).
- Catalog of valid hours with data: the NOMADS listings (top-level day list +
  each day's file list; complete days are listed once, the newest day is
  re-read while a newer run can appear) plus a startup probe of the disk cache
  (`HttpAssetAccessor::isCached`: 30 days back f00–f03, the last day and the
  next two up to f48; ~6000 file checks on a worker thread). Newest run wins.
- Needed: the two available hours around the weather time (each ≤ 6 h away),
  plus the next one after 45 min past the hour while live; one dataset at a
  time, ≤ 8 kept. `bracket()` returns the loaded pair; the app switches to a new
  pair once the service is idle, so a new time appears in one step.
- Live traffic: the new run at ~:50 for the current and next hour, the next
  hour at :45 — about 3 datasets (~12 MB with the large box) and 2–3 listings
  per hour.

**Renderer** (`Renderer::createCloudTexture`, `blendClouds`, `renderCloudShadows`, `PSClouds`):

- Per dataset an immutable 3-slice RGBA16F array (cover, base km, optical
  depth, thickness km) and a 3-slice RG16F array (the layer wind per cell).
  `CSBlendClouds` (compute, each frame with clouds on): motion-compensated time
  blend, semi-Lagrangian per cell — a is read where the air now at uv was at
  t_a (uv − wind_a(x) × (t − t_a), two iterations on x), b where it will be at
  t_b (its wind backward), mixed by (t − t_a) / (t_b − t_a), then cross-faded
  from the snapshot of the previous state (8 s after a dataset change); mips
  generated. It also writes the drift texture: per cell the km offset of each
  noise phase (−wind × dt). Clouds, shadows and surfaces all read the one
  blended texture. A newer run of the same hours keeps the replaced datasets
  alive while their wind fields glide into the new ones (no jump of the drift).
- Noise in two phases, one per dataset, seeded per valid hour and layer and
  drifting with that dataset's wind field (the drift texture, so the shapes
  move with the local wind everywhere in the box), blended variance-preserving:
  n = (n_a (1 − w) + n_b w) / √((1 − w)² + w²). Clouds move with the wind and
  evolve during the hour; continuous across hours (an hour's phase is the same
  in both pairs it belongs to). Base wavelengths 9 / 14 / 24 km (low / middle / high).
- Soft threshold: a ramp of width 0.12 + 6 × footprint (in base wavelengths),
  centred so that the cloudy share of an area stays the cover — no cloud-edge
  aliasing from far away (step 1 aliased from orbit).
- `PSClouds`: three slabs between the per-cell base and top (since 2026-10-11;
  thin shells at the base before). The ray's segment inside each slab comes from
  both roots of the base and top spheres (the camera below, inside or above the
  slab), refined by a fixed-point iteration (the slab under the previous entry,
  coarse mip, 3 iterations). The cloud amount is sampled in the middle of the
  segment and, once the path exceeds 1.5 slab thicknesses (oblique rays, from
  inside the layer), also near the entry and the exit, with a vertical envelope
  (the noise threshold widens towards the base and narrows towards the top:
  cumulus columns, flatter decks); opacity = 1 − exp(−τ · path / thickness),
  the path capped at 16 thicknesses; the depth and the haze use the first
  cloudy sample. Composited front to back (a climbing ray meets low → high, a
  descending one high → low); lit as in step 1 (the base / inside look from
  below and inside, the top look from above), the sun dimmed by the mean cover
  of the layers above; `SV_Depth` = the nearest layer.
- Cheap cloud shadows: `CSCloudShadow` renders a 2048² map over the central
  half of the grid (~0.3 km texels; farther out the shadows are beyond the haze)
  of the sun's transmission through all layers for points at
  sea level (4 noise octaves, 1 / (1 + 0.3 τ / μ)). It is re-rendered only when
  its inputs change (drift > 50 m, time blend, fade, sun, layers): a drifting
  live sky every few seconds, never for camera motion. Terrain, buildings,
  vessels, water and the seabed read the point where their sun ray meets sea
  level (exact for any height) and dim their direct light (imagery: 25 % sky,
  75 % sun). A first version evaluated the shadow per pixel in the terrain
  shader: 17 ms/frame, because the 12 M-triangle terrain shades many triangles
  per pixel.
- Cost (RTX 3070 Laptop, 1600×900, 4× MSAA, A/B in one run, overcast sunset
  view): cloud pass ≈ 0.7 ms, shadows ≈ 0.3 ms (map updates + lookups). With
  the slabs and the per-cell drift (2026-10-11, 30 km / aircraft / vessel views):
  cloud pass ≈ 0.8–0.9 ms, shadows ≈ 0.1 ms.

**Sun and time of day**: `Ephemeris::sunDirectionEcef` (Astronomical Almanac
low-precision sun + GMST, ~0.01°). "Apply to sun" (panel, key `L`,
`--apply-sun`, script `set sun`) puts the sun at the weather time and turns on
the time-of-day light: sky colours day → dusk → night (CPU, from the sun
elevation at the camera), a twilight glow towards the sun, the direct sun's
colour from the air mass (Kasten–Young, Rayleigh + aerosol, brightness
compressed like an auto-exposed camera), the sky light level — per pixel from
the local sun elevation. Off: the fixed sun (215° / 45° at the POI) and the
fixed daylight look. Checked: 2026-10-10 22:15 UTC sun 1° up at 260° (sunset
22:26 UTC), 11:30 UTC 4° at 103°.

**UI / harness**: panel section *Time & weather*: Now, date (days with data,
◀ ▶), time slider (the day's minutes with data), ±1 h, Apply to sun, Cloud
shadows (key `7`), layers Low / Mid / High; *Rendering → Fog* (key `O`). HUD:
time (UTC, New York time, live / replay, sun elevation / azimuth), the two
datasets and the blend, per layer cover / altitude / wind. Script: `weather
… YYYYMMDDHHMM`, `set sun|shadows|fog|cloudlow|cloudmid|cloudhigh on|off`;
CLI: `--apply-sun`, `--no-fog`, `--no-cloud-shadows`, `--cloud-altitude` (every
layer at one altitude). `scripts/clouds_test.txt`: layers one by one, shadows
on / off, a 10-minute drift, fog off, sunrise / sunset / night, the domain from
700 km, the *Simple shell* level, live.

**Single layer — the step-1 look** (added 2026-10-11, for comparison; since the
same day the cloud quality level *Simple shell*, see step 3 → UI: panel combo,
key `8`, `--cloud-level shell`, script `cloudlevel shell`, persisted as
`weather.cloud_level`; Low / Mid / High are disabled at this level). The step-1
code is gone, so the look is emulated on the
step-2 pipeline: every dataset texture has a fourth slice with the total cover
(`TCDC`) at the dataset's `layerAltitude` (the layer mostly seen from below near
the box centre, one altitude for the whole grid), step 1's optical-depth
heuristic mixed by the layer covers (`CloudFields::singleOpticalDepth`) and
100 m of thickness; `CSBlendClouds` puts it into slice 0 and the other layers
are off. No drift, one noise phase with a fixed seed and the 14 km base
wavelength (6 octaves down to 0.45 km), no vertical envelope, no side samples:
a thin shell with static shapes. Differences from step 1 as built: the fields
still blend linearly between the two hours (step 1 switched to the nearest hour
with an 8 s cross-fade); the footprint-dependent soft threshold stays (step 1
aliased from orbit); cloud shadows and *Apply to sun* stay separate toggles
(step 1 had neither); the altitude is step 2's per-layer estimate (step 1 took
the cover-weighted median of the cloud base, or top − 1.5 km for high clouds).

**Known limits of step 2** (after 2026-10-11): middle / high bases are
estimates where lower layers exist (no humidity profile in the 2D file, no
pressure-level filter: data facts); HRRR condensate rarely raises the optical
depth; the slabs have no 3D detail (billows, wisps: step 4) and their sides are
three samples along the ray; cloud-on-cloud shadows use the mean cover;
altitudes come from a ~10 km mip; the domain ends ≈ 550 km out (HRRR's own
east edge is ≈ 600 km east of New York); the time-of-day light is an artistic
exposure model (no moon, no city lights — the window interiors keep their
brightness and read as lit windows at night). Lifted on 2026-10-11: one wind
vector per layer (now a field per cell), shells without thickness (now slabs),
the domain ending ≈ 300 km out (box doubled in both directions).

## Step 3 — GOES-19 observed clouds

GOES-19 has been the operational GOES-East since April 2025 (CONUS sector every
5 min). Useful ABI Level-2 products: clear sky mask (ACM), cloud optical depth
(COD — the reliable retrieval uses reflected sunlight, so expect daytime-only
quality), cloud top height (ACHA), derived motion winds (DMW). GOES drives the **current horizontal shape**
(mask, optical depth); HRRR keeps the vertical structure (base, layers) that
satellites cannot see from above. Blend both into the same `CloudFields`.

**Data facts (checked 2026-10-11)**

- Distribution: the NOAA Open Data bucket `noaa-goes19`
  (`https://noaa-goes19.s3.amazonaws.com/`, also Google / Azure). Keys are
  `ABI-L2-<product>/<year>/<day of year>/<hour>/OR_ABI-L2-<product>-M6_G19_s<start>_e<end>_c<created>.nc`
  (times `YYYYDDDHHMMSSt`). Listing: `?list-type=2&prefix=ABI-L2-ACHAC/2026/284/00/`
  (XML, `<Key>` / `<Size>`). One file per 5-min CONUS scan, published ≈ 2–5 min
  after the scan ends, kept forever (replays need no disk cache, unlike NOMADS).
  The whole CONUS sector, no spatial subsetting:

  | Product | Variables | Grid (CONUS) | One file |
  |---|---|---|---|
  | `ACMC` clear-sky mask | `BCM` (0/1), `ACM` (4 levels), `Cloud_Probabilities`, `DQF` | 2500 × 1500, 2 km | 4.5 MB |
  | `CODC` cloud optical depth (640 nm) | `COD`, `DQF` | 2500 × 1500, 2 km | 3.6 MB |
  | `ACHAC` cloud top height | `HT`, `DQF` | 500 × 300, 10 km | 0.33 MB |
  | `CCLC` cloud cover layers | `TCF` (total) + `CF1`…`CF5`: cover in altitude bands (ft above sea level) 0–5000, 5000–10 000, 10 000–18 000, 18 000–24 000, > 24 000; `CL` (layer flag), `DQF` | 500 × 300, 10 km | 0.42 MB |
  | `DMWC` derived motion winds | wind vectors at tracked features | points | 0.17 MB |

  `CCLC` uses heights, not pressures. Its bands map onto the HRRR layers roughly
  (low < 3.8 km ≈ CF1 + CF2 + the lower part of CF3; middle ≈ the rest of CF3 + CF4;
  high ≈ CF5). The 10 km products are coarser than HRRR's 3 km: they make the
  cloud positions **observed instead of forecast**, not sharper. Sharp shapes need
  the 2 km mask / COD.
- **File format**: NetCDF-4 = HDF5, written with the newer HDF5 structures
  (superblock version 2, version-2 object headers, "dense" link / attribute
  storage in a fractal heap indexed by v2 B-trees — the root group of `ACHAC` has
  36 links), chunked and deflate-compressed variables, packed integers with
  `scale_factor` / `add_offset` / `_FillValue` / `_Unsigned`.
- **No usable subsetting service**: Unidata THREDDS (`thredds.ucar.edu`,
  *GOES East Products*: CloudMask, CloudOpticalDepth, CloudTopHeight,
  CloudCoverLayers, DerivedMotionWinds, …; CONUS / full disk / mesoscale, 13
  days) serves these files with NCSS and OPeNDAP, but both fail on them: NCSS
  NetCDF-3 output rejects the unsigned types ("unknown DataType == ubyte"),
  NetCDF-4 output fails with an HTTP 500 IOException, OPeNDAP answers 500
  ("NcDDS Variable data type = long"). Its `dataset.xml` (variables, attributes)
  works and helps when checking a decoder.
- **WASM**: the bucket sends no CORS headers (checked with an `Origin` header) —
  like NOMADS, a browser build needs a same-origin proxy.
- **Deflate** is already linked: Cesium's dependencies include zlib-ng
  (`zlibstatic-ng.lib`).
- Pixels are on the geostationary fixed grid (scan angles x / y in radians,
  projection in the `goes_imager_projection` variable): resampling needs the
  GOES-R navigation formulas (simple, published in the Product User Guide) and a
  parallax correction for cloud height (the satellite sees cloud tops displaced
  away from nadir by ~height·tan(view zenith), roughly 1× the cloud height at NYC).
- Refresh GOES every 5–10 min, HRRR hourly; at night fall back to HRRR cover
  (COD comes from reflected sunlight).

**Decision (2026-10-11): our own minimal HDF5 reader** (`src/Hdf5.*`, pure C++
like `Grib2.*`, zlib-ng for deflate): only the structures these files use;
anything else is rejected with an error ("extend it, don't guess"). No new
dependency, WASM-friendly. It reads chunked variables chunk by chunk, so a later
version can fetch only the chunks over the weather box with HTTP range requests
(the 2 km files would then cost a fraction of their 4.5 MB). Rejected: HDF5 /
netcdf-c through vcpkg (heavy, a WASM problem), a preprocessing proxy (a server
component the PoC does not have), THREDDS (above).

**Plan**:

1. Cloud quality combo (UI below) — the frame for comparing HRRR and GOES.
   **Done 2026-10-11.**
2. `Hdf5` reader, checked on cached `ACHAC` / `CCLC` files (structure, attributes,
   decoded values against THREDDS `dataset.xml` and physical ranges).
3. GOES fixed-grid navigation (lon / lat ↔ scan angles) and the parallax
   correction from the cloud-top height.
4. `GoesSource` in `WeatherService`: bucket listing → the newest file at or before
   the weather time per product, every 10 min live; replay from the same listing.
5. Blend into `CloudFields`: GOES cover corrects the HRRR cover per layer
   (`CCLC` bands), `ACHA` top height the top of the highest layer, `COD` the
   optical depth by day; HRRR alone at night and outside the GOES data.
6. `goes_test.txt`: HRRR vs GOES shots through the quality combo.

**UI — cloud quality levels** (requested 2026-10-11): step 3 replaces the
*Single layer (step-1 look)* checkbox with a combo box of cloud quality levels
with meaningful names, one per step, each adding to the previous ones — e.g.
*Simple shell* (step 1), *Layers + wind* (step 2), *Satellite observed*
(step 3), later *Volumetric* (step 4), *Volumetric + scattering* (step 5). Every
later step adds its level, so all earlier looks stay selectable for comparison.
Key `8` cycles the levels; the script command, CLI option and persisted setting
(`weather.cloud_single` today) become one level value (read the old key as
*Simple shell* / *Layers + wind*).

As built (2026-10-11): `CloudLevel` in `src/Config.h` with a table of id / name /
tooltip (`kCloudLevels`, `cloudLevelInfo`, `parseCloudLevel`); levels `shell`
(*Simple shell*) and `layers` (*Layers + wind*, the default) so far. Panel: the
row *Quality* [combo] `8` in place of the *Single layer* checkbox, above the
Low / Mid / High row (disabled at *Simple shell*), so the rows below keep their
positions. `--cloud-level shell|layers`, script `cloudlevel shell|layers`,
persisted `weather.cloud_level` (the old `weather.cloud_single` is read once and
not written again); every change is logged with its `--cloud-level`. The next
level (*Satellite observed*) arrives with the GOES blend (plan item 5).

## Step 4 — Procedural volumetric clouds

Do not download 3D cloud geometry. Weather fields give the macro structure,
procedural noise the detail (billows, erosion, holes, wisps, sub-km detail).

```text
weather fields (cover, base, top, optical depth, wind)
      |
      v
large-scale density  --+-- procedural 3D noise (advected by wind)
                       v
             volumetric density, ray marched between base and top
```

Migration from step 1: `PSClouds` already intersects the shell per pixel and
reads the same textures; the volumetric version intersects the slab [base, top]
and marches it — at quarter resolution with depth-aware upsampling and temporal
reprojection (the full-resolution `SV_Depth` trick does not carry over; compose
with the scene depth instead).

## Step 5 — Lighting and scattering

Same sun direction as sky, terrain and water (`SceneSettings::sunDirectionEcef`:
fixed, or the sun at the weather time since step 2). Start with Beer-Lambert extinction, single scattering,
Henyey-Greenstein phase, a few secondary samples towards the sun. Later:
blue-noise ray offsets, approximate multiple scattering, volumetric cloud
shadows (step 2 has a 2D shadow map of the layers), silver lining, aerial perspective
through and around clouds, sky/ambient dimming under overcast. GOES optical depth
gives a physically meaningful extinction scale.

## Step 6 — Full 3D HRRR fields (optional)

HRRR 3D cloud water/ice on native/pressure levels (`wrfprs`/`wrfnat`) could drive
a meteorological density volume. Not planned: much larger downloads, GRIB
complex packing, vertical coordinate reconstruction, CPU/GPU memory,
interpolation. Expected production direction remains:

```text
GOES observed 2D structure + HRRR vertical structure / forecast + procedural volumetric detail
```
