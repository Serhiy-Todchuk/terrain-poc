# CLAUDE.md — TerrainPOC

C++20 / Direct3D 11 Windows app: **Cesium Native v0.65** does tiling/LOD/streaming,
the app renders a merged **land + seabed** surface of the New York harbor with
satellite imagery, extruded OSM buildings, an engine-rendered sea surface,
georeferenced test vessels and live cloud layers from NOAA HRRR (low / middle / high,
wind drift, cloud shadows; weather date / time, optional sun at that time; plan and
status: `docs\weather_roadmap.md`, steps 1–2 done). It is the *online* variant of the PoC described in
`nyc_geospatial_terrain_poc.md` (D:\Downloads): terrain tiles are generated at
run time from public web services — **no Cesium ion, no API keys**.
POI / start view: lon −74.00753296050705, lat 40.66004718826513 (Bush Terminal
piers, Brooklyn). User-facing docs: `README.md`. Representative output: `shots\final\`.

Status: working end-to-end; Debug+Release build with 0 warnings; all harness
scripts end with "0 errors, 0 warnings"; Debug flythrough has no Cesium asserts.

## Quick start

```powershell
# once (30-60 min, ~9 GB in deps\): Cesium Native + its vcpkg dependencies
powershell -ExecutionPolicy Bypass -File D:\Projects\TerrainPOC\scripts\build_deps.ps1
# app
powershell -ExecutionPolicy Bypass -File D:\Projects\TerrainPOC\scripts\build.ps1 -Config Release
# verify
cd D:\Projects\TerrainPOC\bin\Release; .\TerrainPOC.exe --script ..\..\scripts\regression.txt --shot-dir ..\..\shots\reg *> ..\..\shots\reg.txt; "exit=$LASTEXITCODE"
```
Then `Read` the PNGs in the shot dir and grep `bin\Release\TerrainPOC.log`.
The shell is PowerShell 5.1 (no `&&`; use `;` / `if ($?)`).

## Rules

- **No CMake for the app.** The app is `TerrainPOC.sln` + `TerrainPOC.vcxproj`
  (+ `.filters`), toolset **v143**. CMake is used only inside `build_deps.ps1`
  because Cesium Native ships nothing else. Never add a CMakeLists for the app.
- Keep **v143**: all deps are built with MSVC 14.44 (v143); the project must build
  in both VS 2022 and VS 2026.
- New source files: add to `TerrainPOC.vcxproj` (ClCompile/ClInclude) **and**
  `TerrainPOC.vcxproj.filters`; every `.cpp` starts with `#include "pch.h"`
  (exception: the Dear ImGui sources in `third_party\imgui`, `PrecompiledHeader
  NotUsing` per item). Don't modify `third_party\imgui` (fetched, pinned tag).
- `pch.h` includes `windows.h` and then `#undef`s `OPAQUE TRANSPARENT ERROR near far
  GetObject` (they break `CesiumGltf::Material::AlphaMode`, rapidjson, …). Don't
  include Windows headers outside pch.h. `DrawText`/`GetMessage` macros are kept on
  purpose (declaration and call sites both expand consistently).
- glm is compiled with `GLM_FORCE_EXPLICIT_CTOR` and `GLM_FORCE_XYZW_ONLY`:
  write explicit conversions (`glm::vec3(dvec3)`, `glm::mat4(dmat4)`), no `.rgba`.
- Log through `LOG_I/W/E` (app) or `SPDLOG_LOGGER_*(Log::net()/data()/gpu()/cesium(), …)`;
  wrap every HRESULT-returning call in `HR_OK(expr)`. Errors must never be silent.
- Verify every change with the harness (screenshots + log), not by reasoning alone.
  A change is done when the relevant scripts exit 0 and the log ends with
  `0 errors, 0 warnings logged`.
- After touching tile lifetime, loaders or renderer-resource code, also run the
  **Debug** build through `flythrough.txt` (Cesium asserts active, cache eviction
  exercised): `bin\Debug\TerrainPOC.exe --script ..\..\scripts\flythrough.txt --cache-dir D:\Projects\TerrainPOC\bin\Release\cache`.
- Be polite to the public servers: keep the per-host limits, keep the disk cache
  (`bin\<cfg>\cache`, ~0.7 GB; a cold start view = ~1300 requests / ~40 s). Use
  `--offline` for fast deterministic iteration in already-visited areas.
- Shader iteration without rebuilding: run with
  `--shader-dir D:\Projects\TerrainPOC\shaders` and press F5 (the build copies
  `shaders\Scene.hlsl` to `bin\<cfg>\shaders`).
- Do not modify `deps\cesium-native`; work around in app code.

## Architecture

```
main.cpp ─ App (window, loop, input, harness, HUD; settings panel in AppPanel.cpp)
            │ per frame: Ui::newFrame → App::drawPanel → Ui::endFrame (ImGui::Render)
            │            asyncSystem.dispatchMainThreadTasks()
            │            tileset.updateViewGroup(defaultViewGroup, {ViewState}, dt); tileset.loadTiles()
            ├─ Tileset "terrain"  ── TerrainLoader (custom TilesetContentLoader, geographic quadtree)
            │     │ + UrlTemplateRasterOverlay (Esri/USGS imagery; Cesium drapes it)
            │     └─ ElevationService ── HttpAssetAccessor ── 3DEP / BAG / NCEI ImageServers
            ├─ Tileset "buildings" ── BuildingsLoader (custom loader, WebMercator z14)
            │     ├─ OpenFreeMap MVT ── MvtReader (protobuf) → clip → earcut → extrude
            │     └─ ElevationService (3DEP ground under footprints)
            ├─ RendererResources (IPrepareRendererResources): glTF → D3D11 buffers,
            │     imagery → mipmapped sRGB textures (worker threads)
            ├─ WeatherService (no Cesium tiles): weather time (live / pinned), catalog of hours
            │     (NOMADS listings + disk-cache probe), the datasets around the time:
            │     HttpAssetAccessor (clouds + dynamics requests) → Grib2 decode → CloudFields
            │     per layer (worker) → App::updateClouds (bracket, drift, fade)
            │     → Renderer::createCloudTexture per dataset, CloudFrame per frame
            ├─ Renderer: [roof imagery capture] → [cloud blend CS, cloud shadow map CS] → sky
            │     → terrain tiles → building tiles → vessels → water → clouds → MSAA resolve
            │     → HUD (D2D, left) → settings panel (Dear ImGui, right)
            ├─ Ui (Dear ImGui context + Win32/DX11 backends, style, fonts, DPI scale)
            ├─ Vessels (procedural hulls at lon/lat/heading)
            └─ Camera (ECEF + heading/pitch, roll 0)
TaskProcessor = Cesium worker pool (hw−2 threads); HttpAssetAccessor has its own 24 WinHTTP threads.
```
Loader flow: `loadTileContent` → `ElevationService::fetch` (parallel raster futures,
`asyncSystem.all`) → `merge` (worker) → `buildTerrainMesh` → `GltfMeshBuilder` →
`TileLoadResult` → Cesium post-processing (adds `_CESIUMOVERLAY_0`) →
`RendererResources::prepareInLoadThread` (D3D buffers, worker) → main thread →
`Renderer::drawTiles` reads `tile.getContent().getRenderContent()->getRenderResources()`.

## Code map

| File | Responsibility |
|---|---|
| `src/main.cpp` | Entry; default paths (log/cache/shaders next to exe); DPI awareness; COM; crash handler (symbolized DbgHelp stack + `TerrainPOC_crash.dmp`); terminate/abort handlers; Debug CRT report hook (asserts → log, continue) |
| `src/App.{h,cpp}` | Window/message loop, tileset + overlay creation and options, weather service, `weatherTime`/`setWeatherTime`, `updateSun` (fixed sun or `Ephemeris` at the weather time), `updateClouds` (bracket → GPU datasets, cross-fade + snapshot, per-layer drift and noise phases → `SceneSettings::cloud`), frame update, mouse/keyboard navigation (panel-vs-scene input routing in `onMessage`), script harness (`stepScript`, `computeIdle`, `takeScreenshot`, `injectMessage`), HUD lines (`weatherHudLines`), `logStats`, shutdown order |
| `src/AppPanel.cpp` | `App::drawPanel` (header row with the hide arrow, tab bar) + `drawPanelArrows` (HUD / panel hide-show arrows, `_hudInset`) + `section` (collapsing header with persisted open state) + `panelLayers/Rendering/Camera/Detail/Tools` (tab **View**: toggles with key hints, MSAA combo, fog, vsync, water absorption, contour interval, view presets `kViews`, go-to / copy `--camera`, FOV, fly speed, live terrain/buildings SSE, screenshot, shader reload, log stats) + `panelWeather/History/Sources` (tab **Time & weather**: Now, date combo of days with data + ◀ ▶, time slider over the day's hours with data, ±1 h, Apply to sun, cloud shadows, Low/Mid/High, Single layer (step-1 look); history / forecast table of `WeatherService::catalog()` ±24/48/72 h with a row pinning the time; HRRR facts, catalog / newest run / forecast reach, loaded datasets, status); `currentSettings()` (in App.cpp) = the persisted state; changes logged as `Settings panel: …` |
| `src/Settings.{h,cpp}` | Persisted UI settings `TerrainPOC.ini` (next to the exe; `--settings file`, `--no-settings`): `Settings::load` (key=value lines into an `AppConfig`, before the command line is applied over it — main.cpp parses argv twice) and `Settings::save` (at `App::shutdown` when `cfg.settingsActive`). The key table lists every persisted field: toggles, MSAA, vsync, tunables, HUD / panel / tab / sections bitmask, history range, window size — not the weather time, not the camera. Harness runs never touch it unless `--settings` is given explicitly |
| `src/Ui.{h,cpp}` | Dear ImGui plumbing: context (`IniFilename` null), Win32 + DX11 backends, Segoe UI 15 px (fallback: built-in font), dark translucent style, `setScale` (DPI: `ScaleAllSizes` + `FontScaleDpi`), `processMessage`, `wantsMouse/Keyboard`, `newFrame(inputEnabled)` / `endFrame` / `render` |
| `src/Config.{h,cpp}` | `AppConfig` defaults (incl. the persisted-only fields `imageryOn`, `contourInterval`, `cloudLayers`, `historyHours`, `flySpeed`, `panelTab`, `panelSections`), CLI parsing over a base config, `--help` text, `parseCameraSpec` |
| `src/Camera.{h,cpp}` | ECEF camera, heading/pitch in local ENU, `apply(CameraSpec)` (eye / look-at), reversed-Z infinite projection, `orbit` / `moveTowards` / `translate`, `rayDirection`, `describe()` (prints `--camera` syntax), `intersectEllipsoid` |
| `src/TerrainLoader.{h,cpp}` | Custom loader: tile rectangles, children + bounding volumes + geometric error, parent→child source hints (`TileInfo`), `buildTerrainMesh` (border-ring normals, skirts, adaptive diagonals, `_HEIGHT`, `_WATERMASK`) |
| `src/ElevationService.{h,cpp}` | ImageServer URL building, bsq parsing, source selection per level, NCEI ancestor LRU cache + bilinear `resample`, `isHydroFlat`, **`merge`** (the land/seabed rules), `makeFlatGrid`, `geographicTile` |
| `src/BuildingsLoader.{h,cpp}` | XYZ quadtree, TileJSON-provided URL, MVT → clipped rings (Sutherland–Hodgman) → shared-vertex extrusion + earcut roofs; ground from 3DEP; façade style + wall material (`chooseFacadeStyle`), façade coordinates (`facadeU`, `_FACADE`) |
| `src/MvtReader.{h,cpp}` | Minimal MVT v2 / protobuf decoder (layers, keys/values, polygons with holes; exterior = positive area in tile coords) |
| `src/GltfBuilder.{h,cpp}` | `GltfMeshBuilder`: one-primitive glTF (Z-up extras, POSITION min/max, NORMAL, COLOR_0 u8 normalized, scalar / vec2 custom attributes, u16/u32 indices) |
| `src/RendererResources.{h,cpp}` | `GpuVertex` (32 B) + `packNormalWater`, glTF→GPU conversion (`forEachPrimitiveInScene`, RTC/up-axis; `_FACADE` → `isBuilding`), raster textures (+CPU mips), GPU stats |
| `src/Renderer.{h,cpp}` | D3D11 device/flip swapchain, sRGB RTV + UNORM UI RTV (`bindUiTarget`), MSAA scene target + `resolveScene`, R32 depth (DSV + read-only DSV + SRV), pipelines/states, runtime shader compile (`MSAA_SAMPLES` define), `drawTiles` (overlay mapping), water ring mesh, clouds (`createCloudTexture` = fields (+ the step-1 single layer as slice 3) + wind textures, `blendClouds` = CSBlendClouds → the blended field + the noise drift texture, `renderCloudShadows` = CSCloudShadow with a change key, `snapshotClouds`, `drawClouds`, sphere / cloud constants and time-of-day sky colours (`skyColours`) in `beginFrame`), D2D/DWrite HUD (`setHudScale`), WIC PNG screenshot, `pickWorld` (compute-shader depth read), roof imagery capture (`updateRoofImagery`) |
| `src/HttpAssetAccessor.{h,cpp}` | WinHTTP `IAssetAccessor`: queue with per-host limits, retries/backoff, logging, disk cache (request header `Cache-Control: no-cache` bypasses it), `isCached(url)` (header probe, thread-safe), `--offline`, stats, `shutdown()` |
| `src/WeatherService.{h,cpp}` | Live clouds: weather time (live / pinned to the minute), catalog of available hours (NOMADS top-level day list + day file lists, disk-cache probe), needed hours around the time, one fetch at a time (clouds + dynamics requests), `bracket(t)`, `activity()` for HUD/stats, `catalog()` (per available hour: run, listed / cached / loaded, `HourSummary` of every dataset seen this session) + `newestRun()` for the panel, `setGrid` (the weather box moved: datasets / summaries / cache catalog dropped, re-probed; `_generation` discards in-flight results of the old box), `parseTime` (`YYYYMMDDHH[MM]` / `-N`); `HrrrSource` (filter URLs, list/day parsers, `decode` = GRIB2 → bilinear resample to `CloudGrid`, layer winds per cell (grid → earth rotation; the mean near the centre for the HUD / change key), per-layer altitude/thickness (push-pull `fillGaps`, `boxBlur`), optical depth, summary) |
| `src/Grib2.{h,cpp}` | Minimal GRIB2 reader: templates 3.30 (Lambert conformal, spherical earth, any scan mode but boustrophedon, `uvGridRelative` flag), 4.0/4.8, 5.0 (simple packing) + bitmap; `LambertGrid::project` (lon/lat → fractional i/j), `rotation(lon)` (grid → earth vector components), `Field::sample` (bilinear, NaN-aware). Pure C++ |
| `src/CloudFields.h` | `CloudGrid` (default lon −81.2…−66.8, lat 35.55…45.75; `centredAt(lon, lat)` = the same 14.4°×10.2° box elsewhere; 576×408 cells of 0.025°, row 0 north) + `CloudLayer` (per layer: cover, base altitude, thickness, optical depth, wind per cell; summary: mean cover, altitude near the centre, mean wind) + `CloudFields` (total cover, raw base/top, 3 layers, `singleOpticalDepth` (step-1 look), run/valid time, `hasWind`/`hasCondensate`): the only weather type the renderer sees |
| `src/Ephemeris.{h,cpp}` | `sunDirectionEcef(unix s)` (low-precision solar coordinates + GMST), `elevationAzimuth`, `newYorkUtcOffsetHours` (EDT/EST). Pure C++ |
| `src/GeoRef.{h,cpp}` | GEOID18 grid, tide-station MLLW↔NAVD88 offset, sea level, ECEF helpers |
| `src/Vessels.{h,cpp}` | Procedural container ship / ferry / tug; ENU placement at sea-surface height |
| `src/TaskProcessor.{h,cpp}` | `CesiumAsync::ITaskProcessor` thread pool (exceptions logged) |
| `src/Log.{h,cpp}` | spdlog sinks (file, MSVC, console, counting), named loggers, `HR_OK`, HRESULT/Win32 text, UTF-8/UTF-16 |
| `shaders/Scene.hlsl` | `VSMain/PSMain` (terrain + meshes), `VSSky/PSSky`, `VSWater/PSWater`, `PSClouds` (full-screen, with `VSSky`), `CSBlendClouds`, `CSCloudShadow`; time-of-day light (`SunColor`, `SkyLight`, `ImageryLight`), cloud functions (`LayerAmount`, `CloudTransmission`, `CloudShadow`, `TraceLayer`); cbuffers mirror `Renderer::FrameConstants/DrawConstants/CloudBlendConstants` |
| `docs/weather_roadmap.md` | Weather / clouds roadmap: data facts (NOMADS, GRIB2, fields), steps 1–2 as built, steps 3–6 plan (GOES-19, volumetric, lighting) |
| `scripts/build_deps.ps1` | Clone + build + install Cesium Native (both configs), copy earcut, clone Dear ImGui `v1.92.9b` → `third_party\imgui` (`-ImGuiOnly`: just that) |
| `scripts/build.ps1` | MSBuild via vswhere (`-Config`, `-Rebuild`) |
| `scripts/*.txt` | Harness scripts (see below) |

## Key invariants (hard-won — do not break)

**Cesium Native v0.65 API**
1. **Never construct a `Tile` with `TileEmptyContent` and a loadable `TileID`** in a
   loader. That constructor takes a content reference and registers the tile with
   the unload queue while it still lives in a temporary vector; moving it leaves
   dangling pointers → crash in `TilesetContentManager::unloadCachedBytes` at the
   first cache eviction (this happened). Create children as `Tile(loader, TileID(id))`
   (unknown content), `reserve` the vector, and return `TileEmptyContent` from
   `loadTileContent` for content-less levels. Only the root may use
   `TileEmptyContent`, with the non-loadable `TileID()` (empty string).
2. `TileLoadInput` holds references — copy asyncSystem, accessor, tile ID, transform
   before going async.
3. `TileLoadResult.ellipsoid` defaults to **UNIT_SPHERE**: always set WGS84; set
   `glTFUpAxis = Z`, `pAssetAccessor`, `state`, and a tight `updatedBoundingVolume`.
   Transient failures → `createRetryLaterResult` (retried when revisited), permanent
   → `createFailedResult`.
4. Overlay texcoords `_CESIUMOVERLAY_n` are added by Cesium automatically. `v` runs
   **south→north** while image rows run north→south ⇒ shader samples
   `uv = overlayUv * scale + translation; uv.y = 1 − uv.y`. Only set 0 is uploaded
   (single overlay).
5. Raster attachments are read **at draw time** from `tile.getMappedRasterTiles()`
   (`getReadyTile()->getRendererResources()`, `getTranslation/Scale`); the attach/detach
   callbacks are no-ops. (The 4th arg of `attachRasterInMainThread` is the *raster*
   tile's resources despite its name.)
6. Per frame: `dispatchMainThreadTasks()` then `updateViewGroup(...)` + `loadTiles()`
   (`updateView` is deprecated). The returned `ViewUpdateResult&` is valid until the
   next update.
7. `TilesetExternals` must be aggregate-initialised (`AsyncSystem` has no default ctor).
8. Shutdown order: `http->shutdown()` (resolves queued requests) → copy each
   `getAsyncDestructionCompleteEvent()` → reset tilesets → pump
   `dispatchMainThreadTasks()` until ready. `RendererResources` outlives tilesets.
9. Accessor transport failure ⇒ resolve with `response() == nullptr`; never status 0
   (Cesium treats 0 as success for file URLs).
10. D3D11 device calls are free-threaded: create buffers/textures in the load-thread
    callbacks; the immediate context is main-thread only. Raster mips via
    `CesiumImage::ImageDecoder::generateMipMaps` (CPU).
11. `UrlTemplateRasterOverlay`: `{y}` is Cesium's internal (south-origin) row; XYZ
    servers need `{reverseY}`.
12. Cesium's fog culling practically never culls (exp underflow); the renderer does
    its own aerial perspective. `CESIUM_ASSERT` = `assert` in the Debug libs.

**Tiling / LOD**
- Terrain: geographic quadtree, tile = 180/2^L°, **y from the south**, 2×1 roots,
  129 samples/edge, max level 18 in the hi-res box (lon −75.2…−72.8, lat 40.0…41.4,
  ≈0.6 m spacing), 16 in CONUS, 11 elsewhere. Geometric error = 2 × spacing (at the
  equator); terrain SSE 6. Leaves return `Failed` from `createTileChildren`.
- Child bounds: `BoundingRegionWithLooseFittingHeights` from the parent's tight
  region + margin, clamped to [−150, 650] m ellipsoidal in the NYC box (level ≥ 8).
- Source hints from the parent's `TileInfo` (NAVD88 min/max, 3DEP nodata):
  skip bathymetry iff parent `minH > 2` and no 3DEP nodata; skip 3DEP iff parent
  `maxH < −1.5` and level ≥ 12. (Boolean hasWater/hasLand hints broke Jamaica Bay.)
- Buildings: **Web Mercator XYZ, `QuadtreeTileID.y` counts from the north**; roots z10
  over the region, content only at z14 (OpenFreeMap has buildings only at z14);
  ge(z<14) = 100·2^(13−z), z14 ge = 1; SSE 12 ⇒ z14 within ~6.5 km. Building tiles
  never refine, so **don't drape a Cesium raster overlay on them** (every visible z14
  tile would get full-resolution imagery, ~GBs) — roofs use the imagery capture below.
- OpenFreeMap **merges equal-height buildings into one multipolygon feature** (a
  Sunset Park z14 tile has ~60 features); per-building variation is seeded from the
  part's first *unclipped* vertex in global tile units, not from the feature id.
- Terrain options: `preloadSiblings=false` (each tile = 1–3 web requests),
  `forbidHoles=true`, cache 384 MB; buildings cache 192 MB.

**Data & vertical datums**
- Everything handed to Cesium is **WGS84 ellipsoidal**:
  `h = H_NAVD88 + N_GEOID18` (`GeoRef::geoidHeight`, 0.25° grid lat 40–41.25,
  lon −75…−72.5, clamped outside). BAG is **MLLW**: `H = d − navd88AboveMllw`
  (≈0.85 m in the harbor, IDW of Battery/Sandy Hook/Kings Point). Sea surface =
  NAVD88 0 (`kSeaLevelNavd88`). BAG and NCEI agree to a few cm after conversion.
- `exportImage` grid: n inner samples + 1-sample border ring (stride n+2); bbox =
  vertex extent ± 1.5 spacing so pixel centres land exactly on vertices (adjacent
  tiles sample identical positions). `format=bsq&pixelType=F32` ⇒ stride² float32
  rows **north→south**, then an **MSB-first** validity bitmask. Values outside
  [−12000, 10000] are invalid. Latency ~0.6–1 s/request regardless of size.
- Sources per level: < 5 flat (−1 m); 3DEP from 9 (CONUS box); BAG from 14 (server
  MaxPS 0.00016°/px); NCEI direct up to 15, deeper levels resample the cached
  level-15 ancestor (LRU 512, transient failures evicted).
- **Merge rule** (`merge()` in ElevationService.cpp): if 3DEP valid and
  (hydro-flat [4-neighbours equal within 3 mm, h < 3 m] or deeper [h3 < 1 m and
  bathy < h3 − 0.25]) and bathymetry exists → seabed = `h3 < −0.5 ? min(bathy, h3−0.05)
  : bathy`; else 3DEP. No 3DEP → bathy (BAG first, NCEI fallback) → else −5 m.
  **Water mask = merged height < sea level.** Rationale: 3DEP flattens harbor water
  at −1.62 m (low water ⇒ keep seabed below it even where 2015 CUDEM shows fill in
  Bush Terminal slips); in Jamaica Bay it flattens ~0.0–0.1 m *including marsh
  islands* ⇒ trust the topobathy model there.
- Other quirks: 3DEP sometimes returns HTTP 502 (retried); BAG has gaps between
  surveys; OSM contains bridge towers as buildings; bare-earth lidar removes piers.
- OpenFreeMap tile URLs are versioned weekly ⇒ the TileJSON
  (`https://tiles.openfreemap.org/planet`) is fetched at startup (and cached).

**Rendering**
- Reversed-Z infinite projection (`P[2][3]=−1, P[3][2]=near`, depth = near/viewZ),
  depth clear 0, `GREATER_EQUAL`; water `GREATER` without depth write. Near plane =
  clamp(altMSL·0.002, 0.05, 50). Picking: `viewZ = near / depth` (water never writes
  depth ⇒ picks hit the seabed); `CSPickDepth` reads one pixel (nearest sample) into a
  1-float buffer → 4-byte staging copy (MS depth cannot be copied to staging); it
  unbinds the OM targets, so call `pickWorld` only outside beginFrame…present.
- MSAA (`--msaa 1|2|4|8`, default 4, key `M`): scene colour = MS `B8G8R8A8_UNORM_SRGB`
  texture, depth = MS R32; the flip back buffer stays single-sample. `resolveScene()`
  (after water, before the HUD) runs `PSResolve`: averages samples in **linear** space
  (sRGB SRV → sRGB RTV); a hardware resolve into the UNORM back buffer would average
  gamma values. Shaders are compiled with `MSAA_SAMPLES` (depth SRV = `Texture2DMS`),
  so changing the count recompiles shaders + recreates targets (`setSampleCount`).
  `PSWater` runs per sample (`SV_SampleIndex`) to read the matching depth sample (no
  thin-water fringe at shorelines/piers). Cost ~0.4–1.5 ms/frame at 1600×900
  (grows with triangle density; Lower Manhattan is the heavy case).
- Precision: model-view = view · tileTransform · localTransform composed in
  **double**, then cast to float. Vertex positions are relative to the tile centre
  (tile transform = translation to the tile centre on the ellipsoid). Never put ECEF
  coordinates into float vertex data.
- `GpuVertex` (32 B): pos f32×3 | normal R10G10B10A2 (`n·0.5+0.5`, A = water) |
  overlay uv f32×2 | colour RGBA8 | height f32. Zero normal ⇒ PS derives a flat facet
  normal from `ddx/ddy` (buildings store no normals). Buildings reuse slots: uv =
  `_FACADE` (x window bays, y floors above ground), colour A = façade style byte
  (bits 0–1 type punched/curtain/industrial/storefront, 2–4 window width, 5–7 tint);
  draw kind `gFlags.z` = 0 terrain, 1 mesh, 2 building. Keep it in sync in **three
  places**: `RendererResources.h`, `Renderer::compileShaders` input layout, `VSIn` in
  Scene.hlsl. `FrameConstants/DrawConstants` must mirror the HLSL cbuffers (16-byte
  packing; matrices uploaded from glm column-major, used as `mul(M, v)`).
- Colour: imagery textures `R8G8B8A8_UNORM_SRGB`, RTV `B8G8R8A8_UNORM_SRGB` on a
  UNORM flip-model swapchain; D2D HUD draws on the UNORM surface; shaders work in linear.
- Terrain shading is relative to flat ground (imagery already contains shading);
  underwater samples blend 85 % with a depth ramp, relief ×1.3, clamp [0.45, 1.6],
  times `ImageryLight` (75 % sun × cloud shadow + 25 % sky; 1 without clouds and
  time-of-day light). Meshes: two-sided, upward facets (`dot(N,up) > 0.85`) tinted as
  roofs; `ambient × sky + 0.75 ndl × sun × shadow`.
- Aerial perspective (`ApplyFog`, clouds too): amount `gFogParams.y` = 0.85 below
  150 km, 0 with fog off (key `O`, `--no-fog`); the haze colour is the sky's horizon
  colour (time of day).
- Building façades (`ShadeBuilding`, key `T`, `--no-facades`): window grid from the
  façade coordinates with an **analytic box filter** (`FilteredPulse` over `fwidth`) —
  converges to the mean colour with distance, no aliasing; glass = dim interior +
  Fresnel sky reflection. Bays are fitted per run between real corners (> 30°), so
  corners land on bay boundaries; one duplicated vertex pair per ring closes u.
  Derivatives (`fwidth`, roof-uv `ddx/ddy`) are taken **before** the roof/wall branch
  (it diverges inside pixel quads). Keep `ShadeBuilding` single-return (fxc X4000).
- Roof imagery: `updateRoofImagery` (before `beginFrame`) renders the frame's terrain
  tiles top-down (ortho, imagery only, no depth — the selected tiles don't overlap)
  into a 3-slice 2048² sRGB `Texture2DArray` (cascades 1/4/16 km, centred 0.35·extent
  ahead of the nadir, snapped to extent/16), then `GenerateMips`. A cascade is
  re-rendered only when its snapped centre or the signature of its tiles + ready
  rasters changes; at most one per frame, the longest-waiting first (static view ⇒
  0 captures; `roof captures` in `[stats]`). Roofs sample with the matrices each
  cascade was **rendered** with (never stale mappings). Imagery weight fades out
  from 10 to 20 floors (tower roofs are displaced in non-true-ortho imagery); the
  fallback roof colour is neutral grey, not the wall material. 64 MB VRAM.
- Water: camera-centred ring mesh (64×128) on the ellipsoid at sea level, rebuilt when
  the camera moves > max(0.5 m, 1 % altitude), off above 250 km; reads scene depth via
  read-only DSV + SRV; transmittance `exp(−k·len)`, k = `--water-clarity` (0.07/m);
  ripple coordinates anchored to the first build origin, per-wave anti-aliasing;
  body colour × `ImageryLight`, sun glint × sun colour × cloud shadow.

**Weather / clouds** (`docs\weather_roadmap.md`: data facts, steps 1–2 as built, next steps)
- `WeatherService` is independent of Cesium tiles; all its state is main-thread
  (continuations `thenInMainThread`, weak_ptr to the service); decode + resample run
  in `thenInWorkerThread` on captured data only. Always created; the disk-cache probe
  runs once regardless, nothing goes to the network while the layer is hidden;
  `computeIdle` waits for `busy()` (probe, listing, fetch, or a needed hour pending)
  and the cross-fade.
- NOMADS grib filter: **always pass `var_`/`lev_` selections** (without them it returns
  the whole ~130 MB CONUS file); unpublished runs answer **403** — never probe runs by
  requesting them online (warnings): the catalog comes from listings (top-level page =
  day directories, `?dir=…` = a day's files, both with `Cache-Control: no-cache`) and
  `HttpAssetAccessor::isCached`. The filter keeps ~2 days (+ forecasts to 48 h). Host
  limit 2 (NOMADS bans > ~120 req/min). Complete days are listed once; the newest is
  re-read every 10 min until the current hour's run is listed, then at :45 of the next hour.
- Two requests per dataset: the **step-1 cloud URL (unchanged — cached datasets stay
  replayable)** + dynamics (`UGRD`/`VGRD` 850/500/250 mb, `TCOLW`/`TCOLI` at
  `lev_entire_atmosphere`). Dynamics only where they can exist (cached, or that run is
  listed) — never request a purged file. HRRR winds are **grid-relative** (flag 3.3
  bit 5): rotate with `LambertGrid::rotation`. TCOLW is ~0 for most HRRR cover
  (sub-grid clouds): optical depth = max(thickness heuristic, condensate).
- The filter returns simple packing (5.0), Lambert grid 3.30 (sphere 6 371 229 m,
  scan 0x40); `Grib2` rejects anything else with an error — extend it, don't guess.
  Cloud base = lowest layer's base, cloud top = highest layer's top; per-layer bases are
  estimated per cell and filled by push-pull; heights in m above MSL (`navd88ToEllipsoid`).
- `CloudGrid` has fixed cell counts and span (14.4°×10.2°, ≈ 1200×1130 km, default around
  New York; request box ±0.4° larger, formatted with `%g` so the New York URLs stay
  cache-compatible; the filter clips the box to the HRRR domain — cells beyond it are
  clear, counted in the dataset's info line, a warning only above 25 %): the
  renderer only sees `CloudFields`. **Fetch weather here** (`App::setWeatherCenter`, script
  `weather here` / `weather center lon,lat|default`, `--weather-center`, persisted as
  `weather.center`) re-centres the box on the camera: `WeatherService::setGrid` +
  `Renderer::setCloudGrid` (shadow map + snapshot invalidated) + the GPU slots dropped. The
  URLs name the box, so each box has its own cached datasets; the camera must be inside
  lon −121…−71, lat 27…47 (a margin inside the HRRR CONUS Lambert domain). The summaries
  (`altitudeNearPoi`, winds, dominant layer) are about the box centre. Per dataset an immutable 4-slice RGBA16F array
  (low / middle / high + the single layer: cover, base km, optical depth, thickness km)
  + a 3-slice RG16F array (the layer wind
  per cell, m/s east / north). `CSBlendClouds` (every frame with clouds on) blends the
  two datasets of `WeatherService::bracket` by time with per-cell motion compensation
  (semi-Lagrangian: each cell reads where its air was at the dataset's valid time, a
  forward from its time, b backward from its own, 2 iterations) and cross-fades from
  `snapshotClouds` (8 s after a dataset change) into the mip-mapped `gCloudField` (t4)
  everything reads; it also writes `gCloudDrift` (t9, no mips): per cell the km offset
  of each noise phase (−wind × dt). The app takes a new pair only when the service is
  idle (no partial states, no fade for the first data, none at the hour roll-over);
  during a fade of the same hours the replaced datasets stay alive (`_cloudOldA/B`)
  and their wind fields glide into the new ones (`CloudFrame::glide`, no jump).
- **Single layer** (the step-1 look, key `8`, `set cloudsingle`, `--single-cloud-layer`,
  persisted `weather.cloud_single`): emulated on the step-2 pipeline, the step-1 code is
  gone. Slice 3 of each dataset = TCDC at the dataset's `layerAltitude` (uniform), step 1's
  τ heuristic (`CloudFields::singleOpticalDepth`), 100 m thick; `CloudFrame::single` →
  `CSBlendClouds` copies it into slice 0 (`gBlendTimes.w`), the app passes no drift, zero
  noise seeds and layers (1, 0, 0); the shaders (`gCloudMisc.w`) use one noise phase, the
  14 km wavelength, no envelope, no side samples. The fields still blend in time; shadows /
  sun stay separate toggles. Differences from step 1 as built: `docs\weather_roadmap.md`.
- Noise: two phases (seeded per valid hour + layer, drifting per cell with their
  dataset's wind field through `gCloudDrift`), variance-preserving blend by the time
  weight; mean-preserving soft threshold whose width grows with the footprint (no
  aliasing from far away).
- `PSClouds` (after water, before resolve): full-screen, three slabs (base … base +
  thickness per cell); ray vs the base / top spheres osculating the ellipsoid at the
  camera nadir clamped into the grid. The CPU passes the camera height above that
  sphere (`cloudOrigin.w`, double) and the radius; the shader forms `c = (hc − h)(2R +
  hc + h)` (no cancellation) and uses both stable roots (`RayShellRoots`: the camera
  below / inside / above each slab is a separate case in `TraceLayer`). Per-layer slab:
  fixed-point iteration on a coarse mip (3 steps). The cloud amount is sampled in the
  middle of the segment inside the slab, plus near its entry and exit (blended in) once
  the path exceeds 1.5 thicknesses (oblique rays, from inside the layer), each with a
  vertical envelope (`kLayerShape`: the threshold widens towards the base, narrows
  towards the top); opacity = 1 − exp(−τ · path / thickness), path ≤ 16 thicknesses;
  depth / fog from the first cloudy sample. Front to back (climbing ray low → high,
  descending high → low). Geodetic lat =
  `atan2(z, p(1−e²))`. `SV_Depth` = nearest layer, read-only DSV + `_dsWater`
  (GREATER, no writes): scene geometry occludes per MSAA sample, picking ignores
  clouds. Derivatives (`fwidth`, `SampleGrad`) are taken before the per-layer
  branches. Output premultiplied-then-divided with `_bsAlpha`.
- Cloud shadows: `CSCloudShadow` → 2048² R16F map over the central half of the grid
  (~0.3 km texels; sun transmission at sea level, `CloudTransmission`), re-rendered only when its key changes (drift >
  50 m, time weight, fade, sun, layers, datasets) — never per camera move. Surfaces
  call `CloudShadow(rel, sun)` = the sea-level point of their sun ray → one sample.
  **Never evaluate cloud noise per pixel in `PSMain`**: the dense terrain shades many
  triangles per pixel (that cost 17 ms/frame). No gradient ops inside dynamic branches
  (fxc warnings fail the harness); no local const arrays in noise code (unoptimised
  Debug compiles copy them per call → X4714).
- Cost ≈ 0.8–0.9 ms clouds + 0.1–0.3 ms shadows (1600×900, 4× MSAA, full sky; A/B 2026-10-11). Cloud constants
  are part of `FrameConstants`, the blend's in `CloudBlendConstants` (b2) — keep the
  HLSL cbuffers in sync.
- Sun: fixed (215° / 45° at the POI) or `Ephemeris::sunDirectionEcef(weatherTime)`
  ("Apply to sun"), which also turns on the time-of-day light (`gLight.x`): CPU sky
  colours (`skyColours`), shader `SunColor`/`SkyLight`/`ImageryLight` relative to the
  45° reference — with it off every factor is exactly 1 (the fixed look).

**UI (HUD left, settings panel right)**
- Statistics/info stay in the D2D HUD (`hudLines`); every toggle and runtime
  tunable lives in the ImGui panel (`AppPanel.cpp`), keys still work. A new
  toggle goes in **five places**: key in `keyCommand`, `setOption` (script `set`),
  a `toggle(...)` row in the panel, `--help`/README, and — if it should persist —
  an `AppConfig` field + a key in `Settings.cpp` + `App::currentSettings`.
- Panel = header row ("Settings G" + hide arrow, no ImGui title bar) + tab bar
  **View** / **Time & weather** (`_panelTab`, `_panelTabRequest` selects one at
  start / script `tab`). Section open states are `_sectionOpen[PanelSection]`
  (persisted bitmask `panelSections`, handed to ImGui once per section by
  `section()`); `set sections on|off` acts on the shown tab only. The hide /
  show arrows (`drawPanelArrows`) are tiny borderless ImGui windows; the HUD's
  sits inside the HUD box (`Renderer::drawHud(lines, leftInset)`), hidden
  panels leave only an arrow at the edge — not in script runs without UI input
  (`--panel 0` / `--hud 0` shots stay clean). The weather tab's History table
  comes **before** Sources so its rows do not move with the (data-dependent)
  Sources text; `_historyScrollHour` recentres the table once per hour change.
- ImGui frame order: `frame()` builds the UI **first** (`newFrame → drawPanel →
  endFrame`), so panel changes apply to the same frame. `Ui::render` draws the
  last finished draw data, which stays valid until the next `newFrame`: the
  screenshot path (`takeScreenshot → drawFrame`, also from WndProc for `P`)
  re-renders it. Never call `takeScreenshot`/`drawFrame` from widget code —
  the panel only sets `_screenshotRequested` (handled after `endFrame`).
  MSAA/shader changes from widgets are fine (outside beginFrame…present).
- ImGui draws through `Renderer::bindUiTarget` = a **UNORM** view of the back
  buffer (ImGui colours are gamma-space; through the sRGB RTV they'd wash out).
  Order: resolve → D2D HUD → ImGui → present.
- Input: `onMessage` forwards every message to ImGui first; mouse down/wheel go
  to the scene only if `!wantsMouse()`, keys only if `!wantsKeyboard()` (a text
  field/slider is active); button-up / mouse-move always reach an active scene
  drag. In script mode live mouse/keyboard (incl. WM_MOUSELEAVE / NC mouse /
  WM_CHAR) is dropped and ImGui gets `NoMouse|NoKeyboard` — unless the script
  uses `click`/`scroll`/`type`, which inject messages (`injectMessage`, one per
  frame) and enable ImGui input. Dropping WM_MOUSELEAVE matters: TrackMouseEvent
  posts it at once when the real cursor is elsewhere and the injected click
  would lose its position.
- Fixed layout (`IniFilename = nullptr`, `NoMove`, anchored top-right, width
  300·scale, auto-resize clamped to the window height → scrollbar). Widget IDs
  must be unique per window (`"Terrain##sse"`); 1.92 highlights conflicts.
  Persistence is our own `TerrainPOC.ini` (Settings.cpp), not imgui.ini.
- DPI: `GetDpiForWindow` at start and `WM_DPICHANGED` → `App::setUiScale`
  (ImGui style/font + HUD font). Vessels and the weather service are always created
  (toggles work after `--no-vessels` / `--no-clouds`); Imagery/Buildings rows are
  disabled when not created. Keys: `6` clouds, `7` cloud shadows, `8` single layer, `L` Apply to sun,
  `O` fog. Default layout at 1600×900 (panel_test.txt relies on it): header arrow
  (1572, 27), tabs y 54 (View x 1320, Time & weather x 1390). View tab: Layers
  rows y 106…262 (Water 184, Clouds 236), Rendering 288 (Fog 340), Camera header
  496 (Jamaica Bay 600, go-to 657 when open), LOD 522, Tools 548. Time & weather
  tab: Now (1319, 105), date ◀ / ▶ 131, −1h / slider / +1h 157, Apply to sun 184,
  Cloud shadows 210, Low/Mid/High 236, Single layer 262, History header 288 (range
  combo (1330, 314), items 339 / 359 / 379, table header 339, rows every 19 px from
  350 with the current hour centred ≈ 426 — lower when the scroll is clamped near the
  newest hours: the next row is hit at y 449 either way), Sources 537. x of the
  checkboxes 1312. HUD arrow
  (19, 20); hidden stubs: HUD (17, 16), panel (1580, 16).
- Weather time controls: the date combo lists the days of
  `WeatherService::availableHours()` (± the shown day), the slider covers that day's
  minutes with data; dragging pins the time every frame and logs on release
  (`WeatherService::setPinnedTime` logs at debug level only); Now = live.

**HTTP / cache**
- 24 WinHTTP threads; host limits: elevation.nationalmap.gov 8, gis.ngdc.noaa.gov 8,
  server.arcgisonline.com 12, basemap.nationalmap.gov 8, tiles.openfreemap.org 6,
  nomads.ncep.noaa.gov 2. 4 attempts on transport errors/429/5xx (300 ms × 2^n backoff).
- Disk cache `cache\<2 hex>\<fnv64>.bin` (URL verified on read), status 200 only;
  textual bodies are cached **except** for `exportImage` and `/cgi-bin/filter_` URLs
  (ArcGIS / NOMADS return errors as text). A `Cache-Control: no-cache` request header
  skips the cache both ways (NOMADS file list). Debug and Release have separate caches
  unless `--cache-dir` is shared. `--offline`: WinHTTP never used, misses → 404.

**Build / toolchain**
- `build_deps.ps1`: Cesium Native **v0.65.0**, generator VS 2022, `-T v143`, overlay
  triplet `deps\triplets\x64-windows-static-md.cmake` pinning `VCPKG_PLATFORM_TOOLSET
  v143`, `VCPKG_VISUAL_STUDIO_PATH` = VS 2022, `EZVCPKG_BASEDIR=deps\ezvcpkg`, curl /
  tests / clang-tidy off. Dear ImGui sources compiled by the vcxproj (core +
  `imgui_impl_win32/dx11`; defines `IMGUI_DISABLE_OBSOLETE_FUNCTIONS;
  IMGUI_IMPL_WIN32_DISABLE_GAMEPAD`; `CheckCesiumNative` also checks them).
  **`CMAKE_INSTALL_PREFIX` must be set at configure time**
  (Cesium installs headers to the configure-time prefix). Layout: `deps\install\include`,
  `lib\` (Release), `debug\lib\` (Debug).
- vcxproj target `AddCesiumNativeLibraries` adds `$(CesiumLibDir)*.lib` at build time
  (BeforeTargets `ComputeLinkInputsFromProject`); `CheckCesiumNative` fails early if
  deps are missing. Required defines: `GLM_FORCE_XYZW_ONLY; GLM_FORCE_EXPLICIT_CTOR;
  GLM_FORCE_INTRINSICS; GLM_ENABLE_EXPERIMENTAL; SPDLOG_COMPILED_LIB; SPDLOG_FMT_EXTERNAL;
  SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG; LIBASYNC_STATIC` (+ NOMINMAX, WIN32_LEAN_AND_MEAN). `/bigobj /utf-8 /Zc:__cplusplus`,
  C++20, `/MD(d)`, console subsystem, PCH `pch.h`.

## Test harness

CLI: `--camera lon,lat,alt,heading,pitch` | `--lookat lon,lat,alt,heading,pitch,range`
(alt = m above MSL; heading 0 = north, clockwise; pitch < 0 looks down),
`--screenshot file.png` (wait for full load, save, exit), `--script f --shot-dir d`,
`--timeout s` (180), `--keep-open`, `--offline`, `--cache-dir`, `--no-cache`,
`--verbose` (`[D]` lines: every request, tile summaries), `--d3d-debug` (needs the *Graphics Tools* Windows
feature — not installed on the dev machine), `--hud 0|1`, `--panel 0|1`, `--size WxH`, layer flags
(`--no-terrain/--no-buildings/--no-water/--no-vessels/--no-clouds`, `--contours`, `--wireframe`,
`--imagery esri|usgs|none`, `--no-facades`, `--no-fog`, `--no-cloud-shadows`, `--single-cloud-layer`), `--msaa 1|2|4|8`,
LOD tunables (`--terrain-sse`, `--terrain-max-level`, `--terrain-grid`, `--buildings-sse`,
`--imagery-sse`), `--water-clarity`, `--shader-dir`, weather (`--weather-time
YYYYMMDDHH[MM]|-N|live` = replay / forecast at that UTC time, `--apply-sun` = the sun at
the weather time, `--cloud-altitude m` = every layer at one altitude, `--weather-center lon,lat|default`
= the weather box), `--settings file` /
`--no-settings` (persisted UI settings; harness runs ignore the default file). Clouds are the
**live weather** by default, so shots with sky or a 30 km view differ between runs: pin
a time (cached forever once seen) or `set clouds off` when comparing.

Script commands (one per line, `#` comments; live input is ignored while a script runs):

| Command | Effect |
|---|---|
| `camera lon,lat,alt,heading,pitch` / `lookat …,range` | place camera |
| `fly lon,lat,alt,heading,pitch seconds` | smooth flight (tests streaming while moving) |
| `wait [timeout]` | until idle: HTTP idle, worker pool idle, both tilesets' load queues 0, no raster tiles loading, no rendered tile with a pending raster, weather check done + cross-fade finished (clouds on) — for 10 consecutive frames and ≥ 0.5 s |
| `shot name` | renders a fresh frame (with HUD) and saves `<shot-dir>/name.png` |
| `drag left\|right x0 y0 x1 y1 [steps]` | synthetic orbit / pan through the real handlers |
| `wheel x y notches` | synthetic zoom towards a pixel (logs the new `--camera`) |
| `set terrain\|imagery\|buildings\|water\|vessels\|clouds\|shadows\|cloudlow\|cloudmid\|cloudhigh\|cloudsingle\|sun\|fog\|contours\|hud\|panel\|wireframe\|msaa\|facades\|vsync on\|off`, `key K` | toggles (`msaa on` = configured count, 4x if started with `--msaa 1`; `sun` = Apply to sun); `key` calls `keyCommand` directly |
| `weather live\|YYYYMMDDHH[MM]\|-N` | the weather time: live / a UTC time to the minute / N hours ago (replay or forecast; cross-fades) |
| `weather here`, `weather center lon,lat\|default` | move the weather box to the camera (inside the HRRR domain) / a point / back to New York |
| `set sections on\|off` | open / close every section of the shown settings-panel tab (once, next frame) |
| `tab view\|weather` | select a settings-panel tab |
| `click x y`, `scroll x y notches`, `type text` / `type <enter>` | real window messages through `onMessage` (panel first, scene gets the rest); logs `After click/scroll: --camera …` |
| `size WxH`, `sleep s`, `stats`, `log text`, `quit` | misc |

Scripts: `regression.txt` (4 key views — run after every change), `shots_poi.txt`
(POI views incl. 1 m contours on land/seabed), `nav_test.txt` (orbit/tilt/pan/zoom
with expected camera deltas), `scenarios.txt` (vessel's-eye, 30 km, globe),
`flythrough.txt` (long flights + cache eviction + resize; memory check),
`diag_jamaica.txt` (coastline diagnosis: water/imagery off), `msaa_test.txt` (same
views MSAA off/on, wireframe, resize, picking on MS depth), `facades_test.txt`
(plain vs façades: POI, Sunset Park, roofs from above, Lower Manhattan),
`panel_test.txt` (settings panel, run `--offline`: click Water ×2 and Clouds ×2 with
unchanged camera, Fog ×2, the Time & weather tab: next day with data / −1 h / Apply to
sun / Now, single layer on / off, history table row + ±72 h range, wheel over panel = no zoom / over scene =
zoom, Camera header + view button, typed go-to with digits not toggling layers, all
sections of both tabs, script toggles shown, hide / show arrows of both panels, G hides,
1280×720 no overlap — pixel positions assume 1600×900 and the default layout, see UI),
`settings_test_a.txt` + `settings_test_b.txt` (persisted settings: A changes the UI and
quits with `--settings ..\..\shots\settings\test.ini --offline`, B starts with the same
file + `--hud 1` and shots the restored state; the log says `Settings: … N values`),
`weather_center_test.txt` (the box moved to Boston from 20 km and the water, back to New
York from the cache; needs the network once), `clouds_test.txt` (replays 2026-10-10/11 hours: water / overhead / 3 km, cumulus
shadows on / off, each layer alone at 30 km, 10 min of drift, fog off, sunrise /
sunset / night with the sun applied, the domain from 700 km, the single layer (static over 10 min), live, keys 6 / 7 / 8).
Shots include the HUD and the panel (`--panel 0` / `set panel off` for clean scenes).

Exit codes: 0 ok, 1 init failure, 2 bad arguments / `--help`, 3 a `wait` timed out; crash =
0xC0000005 etc. Log: `bin\<cfg>\TerrainPOC.log` (truncated per run). Grep for
`\[E\]|\[W\]|\[C\]|ASSERT|CRASH|timed out|loaded after|\[stats:`. On a crash the log
contains a symbolized stack (function + file:line) and `TerrainPOC_crash.dmp` is
written next to the exe. HUD/`stats` lines show tiles, load queues, requests/cache
hits/errors, GPU mesh/image MB, roof imagery captures and process memory.
`--verbose` sets the loggers (and console sink) to debug; `SPDLOG_LOGGER_DEBUG` is
compiled in (`SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG`) and filtered at run time, so
without it there are no `[D]` lines (arguments are still evaluated — keep them cheap).
It logs `[net]` every request (`-> 200 NB T ms`, `[cache]`, `offline cache miss`),
`[data]` terrain/buildings tile summaries (sources, NAVD88 range), `[d3d]` roof
captures and D3D11 info messages. Regression run: ~3300 lines / 0.8 MB, no
measurable slowdown (a real console window adds ~0.2 s to a fully cached burst).
Benchmark GPU changes with `--no-vsync` and **A/B alternation inside one run**
(`set X on` / `sleep 2` / `stats` / `set X off` …): the laptop GPU's clocks vary
by 2× between runs.

Debugging data problems: `set water off` + `set imagery off` (+ `set contours on`)
exposes classification/merge issues; probe sources directly, e.g.
```bash
curl -s "https://elevation.nationalmap.gov/arcgis/rest/services/3DEPElevation/ImageServer/exportImage?bbox=W,S,E,N&bboxSR=4326&imageSR=4326&size=65,65&format=bsq&pixelType=F32&interpolation=RSP_BilinearInterpolation&f=image" -o r.bin
curl -s "https://gis.ngdc.noaa.gov/arcgis/rest/services/bag_bathymetry/ImageServer/identify?geometry={\"x\":-74.03,\"y\":40.66,\"spatialReference\":{\"wkid\":4326}}&geometryType=esriGeometryPoint&returnCatalogItems=true&f=json"
```
(same `identify` on `DEM_mosaics/DEM_global_mosaic` shows which CUDEM/ETOPO tile is used).

Useful views: POI overview `lookat -74.00753,40.66005,0,330,-30,1800`;
seabed by the piers `lookat -74.0150,40.6620,-9,330,-50,260`; Upper Bay
`lookat -74.0300,40.6650,0,20,-25,3500`; Jamaica Bay `lookat -73.83,40.61,0,0,-60,9000`;
vessel's eye `camera -74.04,40.66,18,15,2`; 30 km `lookat -74.03,40.65,0,0,-60,35000`.

## Baseline (RTX 3070 Laptop, 1600×900, Release)

Start view cold ≈ 40 s / ~1300 requests, cached < 1 s; ~182 terrain tiles (depth 17)
+ 77 building tiles; 100–120 fps loaded; GPU meshes ~0.3–0.7 GB; process memory
~1.0 GB at start, ≤ ~1.9 GB after long flights (bounded by cache budgets). Debug
runs at ~15–30 fps while loading.

## Limitations / roadmap toward the offline PoC

- Weather: steps 1–2 of `docs\weather_roadmap.md` are done (HRRR low / middle / high
  shells at per-cell altitudes, wind drift, time blending, cheap cloud shadows, date /
  time UI, sun at the weather time); next is step 3 (GOES-19 observed clouds), then
  volumetric clouds and lighting.

- Known: no tides (sea at NAVD88 0); pier decks absent in bare-earth lidar; Esri
  imagery terms for production (USGS alternative is ~2 m); first visit to an area is
  bound by public server latency; the authored 40×40 km area integration is not done.
- Offline path (doc goal): reuse `ElevationService::merge` to write a quantized-mesh
  pyramid (`layer.json` + `.terrain`, water mask extension) and an imagery pyramid
  next to the exe; load with Cesium's `LayerJsonTerrainLoader` through a file-backed
  `IAssetAccessor` mapping `geo://nyc/...`; clip terrain under the authored area with
  an `ITileExcluder` / `RasterizedPolygonsTileExcluder`. `--offline` already proves
  the runtime can run with zero network from files next to the exe.
