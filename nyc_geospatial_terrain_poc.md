# NYC Geospatial Terrain PoC

## Goal

Build a proof of concept for extending the existing artist-authored **40 × 40 km New York area** with a lightweight geospatial world that can later run **fully offline from files located next to the executable**.

The surrounding world does not require photogrammetric 3D. The primary requirements are:

- ~**1 m terrain / shoreline accuracy** is sufficient.
- Land must have **satellite/aerial-map-style texturing**.
- The coastline around New York must align well enough for vessel visualization.
- The **seabed must be represented in 3D** so vessels, water, and underwater geometry share a consistent geospatial reference.
- Runtime must not depend on Cesium ion, a web service, or a localhost server.
- Cesium Native should be used as the runtime tiling/LOD backend where practical.

## Proposed Data Sources

Use two main public source families:

1. **USGS**
   - **3DEP 1 m DEM** for land elevation.
   - Public aerial/ortho imagery such as **NAIP** where suitable.

2. **NOAA**
   - **BlueTopo** for bathymetry.
   - NOAA shoreline/topobathymetric data can optionally be used where a more accurate land/water boundary is needed.

Higher-resolution state orthophotos or commercial imagery can replace the imagery source later without changing the runtime architecture.

## Preprocessing

All GIS work is performed offline as a build/preparation step.

### Elevation

Merge:

- USGS 3DEP on land.
- NOAA BlueTopo below water.

into one continuous elevation surface.

Convert the result to a local **Cesium quantized-mesh terrain pyramid**.

The generated terrain should include a land/water mask where possible.

### Imagery

Convert the selected aerial/ortho imagery into a tiled raster pyramid compatible with Cesium raster overlays.

Geometry LOD and imagery LOD remain independent.

## Runtime Layout

Example deployment:

```text
App.exe
Data/
  NYC/
    terrain/
      layer.json
      ...
    imagery/
      ...
```

Cesium Native accesses these resources through a custom filesystem-backed `IAssetAccessor` or equivalent abstraction:

```text
geo://nyc/terrain/...  ->  <exe>/Data/NYC/terrain/...
geo://nyc/imagery/...  ->  <exe>/Data/NYC/imagery/...
```

No HTTP server is required.

## Rendering

Cesium Native is responsible for:

- geospatial coordinate handling;
- terrain tile hierarchy and LOD selection;
- asynchronous tile loading;
- raster overlay selection;
- visibility / screen-space-error driven streaming.

The existing engine remains responsible for GPU resources and rendering through its normal D3D/Vulkan/Metal/WebGPU abstraction.

The sea surface is rendered separately by the engine. It should **not follow seabed geometry**. The terrain water mask determines where the terrain is replaced visually by the ocean surface, while the bathymetric mesh remains below it.

## Integration With Existing 40 × 40 km Model

The existing artist-authored area remains the highest-priority representation.

The geospatial terrain is clipped or disabled beneath that area and used outside it. A transition zone can later be added if elevation mismatches at the boundary are visible.

## PoC Scope

The PoC should demonstrate:

1. Local loading of Cesium quantized-mesh terrain.
2. Local raster imagery draped over the terrain.
3. A merged land + seabed elevation surface around New York Harbor.
4. Correct coastline placement at approximately meter-level accuracy.
5. Engine-rendered sea surface above the bathymetry.
6. Placement of test vessels in georeferenced coordinates.
7. Smooth LOD streaming while moving from the authored 40 × 40 km area into the generated terrain.
8. Operation with the network completely disabled.

## Out of Scope for Initial PoC

- Photogrammetric buildings.
- High-detail city geometry.
- Real-time online map streaming.
- Navigation-grade bathymetric guarantees.
- Dynamic tides/currents.
- Large-scale global coverage.

If required later, coarse buildings from OSM/Overture can be added as a separate 3D Tiles layer without changing the terrain/imagery architecture.
