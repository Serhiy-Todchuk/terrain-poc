// TerrainPOC scene shaders (compiled at runtime with D3DCompileFromFile).
// Matrices are uploaded from glm (column-major) and used as mul(M, v).
// MSAA_SAMPLES (set by the renderer) is the sample count of the scene colour and
// depth targets; > 1 switches the depth reads to Texture2DMS.

#ifndef MSAA_SAMPLES
#define MSAA_SAMPLES 1
#endif

cbuffer FrameConstants : register(b0)
{
    float4x4 gProj;        // reversed-Z, infinite far plane
    float4x4 gViewRot;     // ECEF axes -> view space (rotation only)
    float4   gSunDirView;  // xyz: direction towards the sun (view space)
    float4   gUpView;      // xyz: local vertical at the camera (view space), w: camera altitude (m, MSL)
    float4   gScreen;      // width, height, 1/width, 1/height
    float4   gProjParams;  // tan(fovX/2), tan(fovY/2), near plane, time (s)
    float4   gFogParams;   // x: fog distance scale (m), y: max fog amount (0: fog off)
    float4   gWaterParams; // x: light absorption per m, y: contour interval (0 = off), z: sea level (NAVD88 m)
    float4   gHazeColor;   // rgb: aerial perspective / horizon colour (linear, time of day)
    float4   gSkyZenith;   // rgb: zenith colour (linear, time of day)
    float4x4 gRoofCascade[3]; // view space -> roof imagery uv (xy) of each capture cascade
    float4   gRoofParams;  // xyz: cascade holds imagery (1/0), w: building facades on (1/0)
    float4   gLight;       // x: time-of-day light on (1/0), y: sin(sun elevation) at the camera
    float4   gCloudOrigin; // xyz: camera - sphere centre (ECEF axes, m), w: camera height above the sphere (m)
    float4   gCloudCamera; // xyz: camera position (ECEF, m), w: sphere radius (m)
    float4   gCloudGrid;   // x: west, y: north (rad), z: 1/(east-west), w: 1/(north-south) (1/rad)
    float4   gCloudParams; // x: sea surface above the sphere (m), y/z: noise anchor lon/lat (rad),
                           // w: MSL -> height above the sphere (geoid, m)
    float4   gCloudLayerOn; // xyz: layer low / middle / high drawn (1/0), w: manual altitude (m MSL, < 0: per cell)
    float4   gCloudNoise[3]; // per layer: xy noise seed of phase a, zw of phase b (km east / north)
    float4   gCloudMisc;   // x: weight of phase b, y: cloud shadows on, z: clouds on
};

cbuffer DrawConstants : register(b1)
{
    float4x4 gModelView;   // imagery capture: model-view-projection
    float4   gOverlayST;   // xy: scale, zw: translation (Cesium raster overlay mapping)
    float4   gBaseColor;
    uint4    gFlags;       // x: has overlay, y: overlay uv set, z: kind (0 terrain, 1 mesh, 2 building),
                           // w: vertex colours
};

Texture2D        gOverlay     : register(t0);
Texture2DArray   gRoofImagery : register(t3); // top-down captures of the draped imagery (sRGB)
Texture2DArray   gCloudField  : register(t4); // blended weather per layer (slice): x cover, y base (km MSL),
                                              //   z in-cloud optical depth, w thickness (km)
Texture2DArray   gCloudDrift  : register(t9); // per layer and cell: xy drift of noise phase a, zw of b (km east / north)
#if MSAA_SAMPLES > 1
Texture2DMS<float>  gSceneDepth : register(t1);
Texture2DMS<float4> gSceneColor : register(t2); // resolve input (sRGB view: loads are linear)
#else
Texture2D<float> gSceneDepth : register(t1);
#endif
SamplerState     gLinearClamp : register(s0);

float LoadSceneDepth(int2 pixel, uint sampleIndex)
{
#if MSAA_SAMPLES > 1
    return gSceneDepth.Load(pixel, sampleIndex);
#else
    return gSceneDepth.Load(int3(pixel, 0));
#endif
}

// ---------------------------------------------------------------------------
// Time-of-day light (gLight.x = 1, "Apply to sun"): colour and strength of the
// direct sun and of the sky light relative to the fixed default sun (45 degrees
// elevation: both 1), as an auto-exposed camera would show them. Off: both 1,
// the fixed daylight look.
static const float kRefSinEl = 0.70710678;

float3 SunColor(float sinEl)
{
    float3 c = 1.0;
    if (gLight.x > 0.5)
    {
        // Air mass (Kasten-Young) -> Rayleigh + aerosol transmittance relative
        // to the reference elevation; brightness compressed (exposure), hue kept.
        float elDeg = degrees(asin(clamp(sinEl, -1.0, 1.0)));
        float m = 1.0 / (max(sinEl, 0.0) + 0.50572 * pow(max(elDeg + 6.07995, 0.5), -1.6364));
        float3 t = exp(-float3(0.11, 0.18, 0.32) * (m - 1.4126));
        float peak = max(max(t.r, t.g), t.b);
        c = t * pow(max(peak, 1e-6), -0.65) * smoothstep(-0.0145, 0.02, sinEl); // the disc sets at -0.83 deg
    }
    return c;
}

float3 SkyLight(float sinEl)
{
    float3 c = 1.0;
    if (gLight.x > 0.5)
    {
        // Day -> twilight (bluer) -> night (a dim blue floor, not black).
        float day = smoothstep(-0.30, 0.25, sinEl);
        float3 tint = lerp(float3(0.75, 0.80, 1.0), 1.0, smoothstep(0.0, 0.25, sinEl));
        c = lerp(float3(0.025, 0.032, 0.055), tint, day * day);
    }
    return c;
}

// Light on imagery-textured surfaces (the imagery already holds a sunlit look):
// a direct share, dimmed by cloud shadow, plus a sky share; 1 under the
// reference sun without clouds. sun / sky: SunColor / SkyLight at the point.
float3 ImageryLight(float sinEl, float shadow, float3 sun, float3 sky)
{
    float direct = gLight.x > 0.5 ? sqrt(saturate(sinEl / kRefSinEl)) : 1.0;
    return 0.75 * sun * (direct * shadow) + 0.25 * sky;
}

// Sine of the sun elevation at a point (rel = point - camera, ECEF axes).
float SunSinAt(float3 rel, float3 sunEcef)
{
    return dot(normalize(gCloudOrigin.xyz + rel), sunEcef);
}

// ---------------------------------------------------------------------------
float3 SkyColor(float3 dirView)
{
    float3 up = normalize(gUpView.xyz);
    float e = dot(dirView, up);
    // The visible horizon dips below the local horizontal with altitude.
    float alt = max(gUpView.w, 1.0);
    float dip = -sqrt(saturate(2.0 * alt / 6371000.0));
    float t = saturate((e - dip) / (1.0 - dip));
    float3 horizon = gHazeColor.rgb;
    float3 sky = lerp(horizon, gSkyZenith.rgb, pow(t, 0.45));
    float sun = saturate(dot(dirView, gSunDirView.xyz));
    sky += SunColor(gLight.y) * (pow(sun, 800.0) * 20.0 + pow(sun, 8.0) * 0.15);
    if (gLight.x > 0.5)
    {
        // Twilight: the horizon towards the sun glows while the sun is low or
        // just below the horizon.
        float s = gLight.y;
        float glow = smoothstep(-0.18, -0.01, s) * (1.0 - smoothstep(0.04, 0.30, s));
        float3 sunH = gSunDirView.xyz - up * dot(gSunDirView.xyz, up);
        float3 dirH = dirView - up * e;
        float toward = pow(saturate(dot(dirH, sunH) * rsqrt(max(dot(dirH, dirH) * dot(sunH, sunH), 1e-8)) * 0.5 + 0.5), 3.0);
        sky += float3(0.85, 0.38, 0.12) * (glow * toward * pow(1.0 - t, 3.0) * 0.9);
    }
    float3 ground = gHazeColor.rgb * 0.75;
    return e < dip ? lerp(horizon, ground, saturate((dip - e) * 8.0)) : sky;
}

float3 BathymetryRamp(float depth)
{
    // linear-space colours: sand -> turquoise -> blue -> deep navy
    float3 c0 = float3(0.60, 0.52, 0.33);
    float3 c1 = float3(0.12, 0.42, 0.36);
    float3 c2 = float3(0.03, 0.17, 0.32);
    float3 c3 = float3(0.01, 0.04, 0.14);
    float3 shallow = lerp(c0, c1, saturate(depth / 3.0));
    float3 middle = lerp(c1, c2, saturate((depth - 3.0) / 9.0));
    float3 deep = lerp(c2, c3, saturate((depth - 12.0) / 25.0));
    return depth < 3.0 ? shallow : (depth < 12.0 ? middle : deep);
}

float3 ApplyFog(float3 color, float3 viewPos)
{
    float dist = length(viewPos);
    float fog = (1.0 - exp(-dist / gFogParams.x)) * gFogParams.y;
    return lerp(color, gHazeColor.rgb, fog);
}

// ---------------------------------------------------------------------------
// Clouds (live weather, docs/weather_roadmap.md steps 1-2): three layers (low /
// middle / high), each a curved slab between its per-cell base and top. Macro
// structure from the blended weather fields (gCloudField, CSBlendClouds), sub-
// grid shapes from procedural noise in two phases that drift with the layer
// wind field (one phase per dataset of the time blend; the per-cell drift comes
// from gCloudDrift). The same functions shade the ground (cheap cloud shadows)
// and the cloud pass (PSClouds).

uint PcgHash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Global (one immediate constant buffer; a local table is copied per call in
// unoptimised Debug builds).
static const float2 kCellGradients[8] = { float2(1.0, 0.0), float2(-1.0, 0.0), float2(0.0, 1.0), float2(0.0, -1.0),
                                          float2(0.7071, 0.7071), float2(-0.7071, 0.7071), float2(0.7071, -0.7071),
                                          float2(-0.7071, -0.7071) };

float2 CellGradient(int2 cell)
{
    return kCellGradients[PcgHash(uint(cell.x) + PcgHash(uint(cell.y))) & 7u];
}

// 2D gradient noise: zero mean, about [-0.7, 0.7].
float GradientNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = p - i;
    int2 c = int2(i);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = dot(CellGradient(c), f);
    float b = dot(CellGradient(c + int2(1, 0)), f - float2(1.0, 0.0));
    float d = dot(CellGradient(c + int2(0, 1)), f - float2(0.0, 1.0));
    float e = dot(CellGradient(c + int2(1, 1)), f - float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(d, e, u.x), u.y);
}

// Cloud-shape fBm (p and the pixel footprint fp in base wavelengths), up to 6
// octaves. Octaves the footprint cannot resolve fade out instead of aliasing,
// octaves left out (shadows: 4) count as faded; `removed` is the faded share of
// the noise variance (the caller falls back to the mean cover by that share,
// so distant clouds keep the right coverage).
float CloudNoise(float2 p, float fp, int octaves, out float removed)
{
    const float2x2 rot = float2x2(0.80, -0.60, 0.60, 0.80);
    float sum = 0.0, amp = 0.5, scale = 1.0, total = 0.0, kept = 0.0;
    [unroll] for (int k = 0; k < 6; ++k)
    {
        if (k < octaves)
        {
            float w = saturate(1.0 / (3.0 * scale * fp) - 1.0); // fewer than ~6 px per wavelength: fade
            sum += amp * w * GradientNoise(p * scale + float2(k * 17.31, k * 5.73));
            kept += amp * amp * w * w;
        }
        total += amp * amp;
        p = mul(rot, p);
        amp *= 0.5;
        scale *= 2.0;
    }
    removed = 1.0 - kept / total;
    return sum;
}

static const float kLayerWavelength[3] = { 9.0, 14.0, 24.0 }; // km: base wavelength of the shapes per layer

// Geodetic lon/lat (rad) of an ECEF position and its CloudGrid uv.
float2 CloudGridUv(float3 ecef, out float2 lonLat)
{
    lonLat = float2(atan2(ecef.y, ecef.x), atan2(ecef.z, length(ecef.xy) * (1.0 - 0.00669438))); // WGS84 e^2
    return float2((lonLat.x - gCloudGrid.x) * gCloudGrid.z, (gCloudGrid.y - lonLat.y) * gCloudGrid.w);
}

// Noise coordinates: km east / north of the fixed noise anchor.
float2 NoiseKm(float2 lonLat)
{
    return float2((lonLat.x - gCloudParams.y) * 6378.1 * cos(gCloudParams.z), (lonLat.y - gCloudParams.z) * 6356.8);
}

// Soft round fade over the outer quarter of the grid (no visible data edge).
float GridEdge(float2 uv)
{
    return 1.0 - smoothstep(0.72, 0.98, length((uv - 0.5) * 2.0));
}

// Altitude of a layer (m above the sphere) from a coarse mip: smooth enough for
// the fixed-point intersections below.
float LayerHeight(int layer, float2 uv)
{
    float msl = gCloudLayerOn.w >= 0.0 ? gCloudLayerOn.w
                                       : gCloudField.SampleLevel(gLinearClamp, float3(uv, layer), 2.0).y * 1000.0;
    return msl + gCloudParams.w;
}

// c = |oc|^2 - (R + h)^2 of a shell h metres above the sphere, formed from the
// camera height (no cancellation).
float ShellC(float h)
{
    float hc = gCloudOrigin.w;
    return (hc - h) * (2.0 * gCloudCamera.w + hc + h);
}

// Both roots of |oc + t dir| = R (b = dot(dir, oc), c = |oc|^2 - R^2) in the
// cancellation-free form (x near, y far); both -1 when the ray misses.
float2 RayShellRoots(float b, float c)
{
    float disc = b * b - c;
    float s = sqrt(max(disc, 0.0));
    float q = -(b + (b >= 0.0 ? s : -s));
    float r1 = c / (abs(q) > 1e-3 ? q : 1e-3);
    return disc > 0.0 ? float2(min(q, r1), max(q, r1)) : float2(-1.0, -1.0);
}

// Height above the sphere (m) of the point t metres along the ray (b = dot(dir,
// oc)), from the camera height: no cancellation.
float HeightAt(float t, float b)
{
    float hc = gCloudOrigin.w;
    float q = 2.0 * b * t + t * t;
    float h = hc + q / (2.0 * (gCloudCamera.w + hc));
    return hc + q / (2.0 * gCloudCamera.w + hc + h);
}

// Distance from a point (height hP above the sphere, bb = dot(point - centre,
// sun)) along the sun ray up to the shell at height h; 0 above the shell.
float SunRayToShell(float bb, float hP, float h)
{
    float c = (hP - h) * (2.0 * gCloudCamera.w + hP + h);
    return max(-c / max(bb + sqrt(max(bb * bb - c, 0.0)), 1.0), 0.0);
}

// Cloudy share of a layer at a point: the cover thresholded by the noise, so
// the cloudy share of an area matches the model cover (puffs ... overcast).
// The noise is the variance-preserving blend of two phases (the drifting
// patterns of the two datasets of the time blend); each phase is read where
// the wind field has carried it (gCloudDrift at the grid uv). `shift` moves
// the threshold (the vertical envelope of a slab: + wider, - narrower).
struct CloudAmount
{
    float amount;  // 0..1
    float density; // optical depth factor (thicker / thinner patches)
};

CloudAmount LayerAmount(int layer, float cover, float2 uv, float2 km, float fpKm, int octaves, float shift)
{
    float lambda = kLayerWavelength[layer];
    float fp = fpKm / lambda;
    float wb = gCloudMisc.x;
    float4 drift = gCloudDrift.SampleLevel(gLinearClamp, float3(uv, layer), 0.0);
    float removed = 0.0, na = 0.0, nb = 0.0;
    if (wb < 0.999)
        na = CloudNoise((km + gCloudNoise[layer].xy + drift.xy) / lambda, fp, octaves, removed);
    if (wb > 0.001)
        nb = CloudNoise((km + gCloudNoise[layer].zw + drift.zw) / lambda, fp, octaves, removed);
    float n = (na * (1.0 - wb) + nb * wb) * rsqrt((1.0 - wb) * (1.0 - wb) + wb * wb);
    float u = 1.0 / (1.0 + exp(-13.7 * n)); // ~uniform on [0, 1] (logistic CDF of the fBm, sigma ~0.124)
    // Soft threshold: a ramp of width s in u, wider as the footprint grows (cloud
    // edges never get sharper than a pixel), centred so that the cloudy share of
    // an area stays the cover also where the ramp crosses 0 or 1.
    float s = clamp(0.12 + 6.0 * fp, 0.12, 1.0);
    float centre = cover < 0.5 * s ? sqrt(2.0 * s * cover) - 0.5 * s
                 : (cover > 1.0 - 0.5 * s ? 1.0 + 0.5 * s - sqrt(2.0 * s * (1.0 - cover)) : cover);
    centre += shift;
    CloudAmount r;
    r.amount = lerp(saturate((centre - u) / s + 0.5), cover, removed);
    r.density = lerp(0.4 + 1.2 * (1.0 - u), 1.0, removed);
    return r;
}

// Share of the direct sun that reaches a point through all layers: the sun
// ray meets each layer at its altitude there (refined once), the layer's cloud
// amount at that point (4 noise octaves) gives a transmission 1 / (1 + 0.3 tau
// / mu) (twice the two-stream value: the heuristic optical depths are low for
// cumulus, whose shadows are distinct). rel = point - camera (ECEF axes),
// footprintKm = the size the result stands for (the shadow pattern is the cloud
// pattern shifted along the sun). No derivatives inside. Used by CSCloudShadow.
float CloudTransmission(float3 rel, float3 sunEcef, float footprintKm)
{
    float transmission = 1.0;
    [branch] if (gCloudMisc.y > 0.5)
    {
        float3 upCam = normalize(gCloudOrigin.xyz);
        float hc = gCloudOrigin.w;
        float3 pc = gCloudOrigin.xyz + rel; // from the sphere centre
        float vertical = dot(rel, upCam);
        float hP = hc + vertical + (dot(rel, rel) - vertical * vertical) / (2.0 * (gCloudCamera.w + hc));
        float bb = dot(pc, sunEcef);
        float sinEl = bb / length(pc);
        float fpKm = max(footprintKm, 1e-6);
        float lod = max(log2(fpKm / 2.5), 0.0); // grid cells are ~2.5 km
        float2 lonLat;
        float2 uvP = CloudGridUv(gCloudCamera.xyz + rel, lonLat);
        [unroll] for (int l = 0; l < 3; ++l)
        {
            float h = LayerHeight(l, uvP);
            float s = SunRayToShell(bb, hP, h);
            float2 uv = CloudGridUv(gCloudCamera.xyz + rel + s * sunEcef, lonLat);
            h = LayerHeight(l, uv);
            s = SunRayToShell(bb, hP, h);
            uv = CloudGridUv(gCloudCamera.xyz + rel + s * sunEcef, lonLat);
            float2 km = NoiseKm(lonLat);
            float4 field = gCloudField.SampleLevel(gLinearClamp, float3(uv, l), lod);
            float cover = saturate(field.x) * GridEdge(uv) * gCloudLayerOn[l];
            float tau = 0.0;
            [branch] if (cover > 0.002 && hP < h)
            {
                CloudAmount a = LayerAmount(l, cover, uv, km, fpKm, 4, 0.0);
                tau = field.z * a.amount * a.density;
            }
            transmission *= 1.0 / (1.0 + 0.3 * tau / max(sinEl, 0.05));
        }
        transmission = sinEl > -0.02 ? transmission : 1.0;
    }
    return transmission;
}

// Cloud shadow map (CSCloudShadow, when the clouds or the sun changed):
// CloudTransmission of the points at sea level on the central half of the
// weather grid (~0.3 km texels; farther out the shadows are beyond the haze).
// A surface point reads the point where its sun ray meets sea level - the same
// ray, so the shadow of every layer lands in the right place at any height
// (seabed too).
Texture2D<float> gCloudShadow : register(t5);

float CloudShadow(float3 rel, float3 sunEcef)
{
    float shadow = 1.0;
    [branch] if (gCloudMisc.y > 0.5)
    {
        float3 upCam = normalize(gCloudOrigin.xyz);
        float hc = gCloudOrigin.w;
        float3 pc = gCloudOrigin.xyz + rel; // from the sphere centre
        float vertical = dot(rel, upCam);
        float hP = hc + vertical + (dot(rel, rel) - vertical * vertical) / (2.0 * (gCloudCamera.w + hc));
        float bb = dot(pc, sunEcef);
        float hSea = gCloudParams.x;
        float c = (hP - hSea) * (2.0 * gCloudCamera.w + hP + hSea);
        float d = c / max(bb + sqrt(max(bb * bb - c, 0.0)), 1.0); // back along the sun ray to sea level (< 0: below)
        float2 lonLat;
        float2 uv = (CloudGridUv(gCloudCamera.xyz + rel - d * sunEcef, lonLat) - 0.25) * 2.0; // the central half
        shadow = all(abs(uv - 0.5) < 0.5) ? gCloudShadow.SampleLevel(gLinearClamp, uv, 0.0) : 1.0;
        shadow = bb > 0.0 ? shadow : 1.0; // sun below the local horizon: no direct light anyway
    }
    return shadow;
}

// ---------------------------------------------------------------------------
// Buildings: procedural facades and roofs from the imagery capture.

float Hash12(float2 p)
{
    float3 p3 = frac(p.xyx * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// Integral of a period-1 pulse train that is 1 on [e0, e1].
float PulseIntegral(float x, float e0, float e1)
{
    return floor(x) * (e1 - e0) + clamp(frac(x), e0, e1) - e0;
}

// Pulse train box-filtered over the pixel footprint w (exact average), so the
// window grid converges to its mean colour instead of aliasing with distance.
float FilteredPulse(float x, float w, float e0, float e1)
{
    w = max(w, 1e-4);
    return (PulseIntegral(x + 0.5 * w, e0, e1) - PulseIntegral(x - 0.5 * w, e0, e1)) / w;
}

// Where to read the draped imagery under a roof pixel: the finest capture
// cascade that contains it. Gradients are taken per cascade before selecting
// one (mip selection stays smooth across cascade borders); call this in uniform
// control flow and sample with SampleRoofImagery in the roof branch.
struct RoofLookup
{
    float3 uvw; // uv + cascade
    float2 dx, dy;
    bool found;
};

RoofLookup FindRoofImagery(float3 viewPos)
{
    RoofLookup r;
    r.uvw = 0.0;
    r.dx = 0.0;
    r.dy = 0.0;
    r.found = false;
    [unroll] for (int c = 0; c < 3; ++c)
    {
        float2 uv = mul(gRoofCascade[c], float4(viewPos, 1.0)).xy;
        float2 dx = ddx(uv), dy = ddy(uv);
        bool inside = gRoofParams[c] > 0.5 && all(abs(uv - 0.5) < 0.495);
        if (inside && !r.found)
        {
            r.uvw = float3(uv, c);
            r.dx = dx;
            r.dy = dy;
            r.found = true;
        }
    }
    return r;
}

float4 SampleRoofImagery(RoofLookup r) // a = 0: no imagery there
{
    float4 imagery = 0.0;
    if (r.found)
        imagery = gRoofImagery.SampleGrad(gLinearClamp, r.uvw, r.dx, r.dy);
    return imagery;
}

// Window coverage of a facade pixel (box-filtered over the footprint fw).
// facade.x: window bays along the wall (whole numbers at corners), facade.y:
// floors above ground. style: bits 0-1 type (0 punched windows, 1 curtain wall,
// 2 industrial, 3 shop fronts), bits 2-4 window width, bits 5-7 glass tint.
float WindowMask(float2 facade, float2 fw, uint style)
{
    uint type = style & 3u;
    float wv = float((style >> 2) & 7u);
    float ww, y0, y1;
    if (type == 1u)      { ww = 0.86 + 0.015 * wv; y0 = 0.06; y1 = 0.78; } // glass between mullions/spandrels
    else if (type == 2u) { ww = 0.25 + 0.05 * wv;  y0 = 0.55; y1 = 0.82; } // high strip windows
    else                 { ww = 0.38 + 0.06 * wv;  y0 = 0.30; y1 = 0.80; }
    float mask = FilteredPulse(facade.x, fw.x, 0.5 - 0.5 * ww, 0.5 + 0.5 * ww) *
                 FilteredPulse(facade.y, fw.y, y0, y1);
    if (type == 3u)
    {
        float ground = 1.0 - smoothstep(1.0 - fw.y, 1.0 + fw.y, facade.y);
        float shop = FilteredPulse(facade.x, fw.x, 0.06, 0.94) * FilteredPulse(facade.y, fw.y, 0.05, 0.85);
        mask = lerp(mask, shop, ground);
    }
    return mask * smoothstep(0.0, 0.15, facade.y); // nothing below ground
}

// Glass: dim interior (varying per window) plus Fresnel reflection of the sky.
float3 GlassColor(float2 facade, float2 fw, uint style, float3 N, float3 V, float3 L, float3 up, float3 sun)
{
    const float3 tints[8] = { float3(0.55, 0.65, 0.70), float3(0.45, 0.60, 0.62), float3(0.60, 0.58, 0.52),
                              float3(0.40, 0.48, 0.60), float3(0.55, 0.55, 0.55), float3(0.50, 0.62, 0.58),
                              float3(0.62, 0.55, 0.45), float3(0.45, 0.50, 0.58) };
    float3 tint = tints[(style >> 5) & 7u];
    float detail = saturate(1.0 - 2.0 * max(fw.x, fw.y)); // per-window variation only while resolvable
    float interior = lerp(0.06, 0.02 + 0.12 * Hash12(floor(facade) + float(style) * 7.13), detail);
    float3 R = reflect(-V, N);
    float3 reflection = SkyColor(R) * lerp(0.35, 1.0, saturate(dot(R, up) * 4.0 + 0.5)); // ground reflects darker
    float f0 = (style & 3u) == 1u ? 0.12 : 0.05;
    float fresnel = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(N, V)), 5.0);
    float3 glass = lerp(tint * interior, reflection * lerp(tint, 1.0, 0.6), fresnel);
    return glass + sun * (pow(saturate(dot(R, L)), 300.0) * 1.5 * fresnel); // sun glint
}

// sun / sky: direct (cloud shadow included) and sky light (SunColor, SkyLight);
// imageryLight: ImageryLight for the roof imagery.
float3 ShadeBuilding(float3 viewPos, float2 facade, uint style, float3 albedo, float3 N, float3 V, float3 L,
                     float3 sun, float3 sky, float3 imageryLight)
{
    float3 up = normalize(gUpView.xyz);
    float upness = dot(N, up);
    float ndl = saturate(dot(N, L));
    float ambient = 0.32 + 0.14 * saturate(upness);
    // Derivatives first: the roof/wall branch below diverges within pixel quads.
    float2 fw = max(fwidth(facade), 1e-4);
    RoofLookup roofLookup = FindRoofImagery(viewPos);

    float3 color;
    if (upness > 0.85)
    {
        // Roof: the satellite image of the roof itself (it already contains the
        // sun shading, like the terrain). Fades out on towers, whose roofs are
        // displaced in the (not true-ortho) imagery by their lean.
        float3 roofAlbedo = lerp(albedo, float3(0.28, 0.28, 0.27), 0.7); // tar / gravel, not the wall material
        float3 roof = roofAlbedo * (ambient * sky + 0.75 * ndl * sun);
        float4 imagery = SampleRoofImagery(roofLookup);
        float w = imagery.a * (1.0 - smoothstep(10.0, 20.0, facade.y));
        color = lerp(roof, imagery.rgb * imageryLight, w);
    }
    else
    {
        float3 wall = albedo * (ambient * sky + 0.75 * ndl * sun);
        wall *= 0.78 + 0.22 * saturate(facade.y * 1.5); // darker at street level
        float mask = upness < -0.85 ? 0.0 : WindowMask(facade, fw, style); // no windows on undersides
        color = lerp(wall, GlassColor(facade, fw, style, N, V, L, up, sun), mask);
    }
    return color;
}

// ---------------------------------------------------------------------------
struct VSIn
{
    float3 pos    : POSITION;
    float4 nrmw   : NORMAL;    // R10G10B10A2_UNORM: normal*0.5+0.5, w = water mask
    float2 uv     : TEXCOORD0; // Cesium raster overlay texcoords
    float4 col    : COLOR0;
    float  height : TEXCOORD2;
};

struct VSOut
{
    float4 svpos   : SV_Position;
    float3 viewPos : TEXCOORD0;
    float3 nrm     : NORMAL;
    float2 uv      : TEXCOORD1;
    float4 col     : COLOR0;
    float  height  : TEXCOORD2;
    float  water   : TEXCOORD3;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 vp = mul(gModelView, float4(v.pos, 1.0));
    o.viewPos = vp.xyz;
    o.svpos = mul(gProj, vp);
    float3 n = v.nrmw.xyz * 2.0 - 1.0; // ~0 length => no normal (flat shading in PS)
    o.nrm = dot(n, n) > 0.01 ? mul((float3x3)gModelView, n) : float3(0, 0, 0);
    o.uv = v.uv;
    o.col = v.col;
    o.height = v.height;
    o.water = v.nrmw.w;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 V = normalize(-i.viewPos);
    float3 N = i.nrm;
    if (dot(N, N) < 1e-8)
        N = normalize(cross(ddy(i.viewPos), ddx(i.viewPos)));
    else
        N = normalize(N);
    if (dot(N, V) < 0.0 && gFlags.z != 0)
        N = -N; // two-sided lighting for meshes

    float3 albedo = gBaseColor.rgb;
    if (gFlags.x != 0)
    {
        float2 uv = i.uv * gOverlayST.xy + gOverlayST.zw;
        uv.y = 1.0 - uv.y; // overlay v runs south->north, image rows north->south
        albedo = gOverlay.Sample(gLinearClamp, uv).rgb;
    }
    if (gFlags.w != 0)
        albedo *= i.col.rgb;

    float3 L = normalize(gSunDirView.xyz);
    float ndl = saturate(dot(N, L));
    // Sun at this point: elevation (time-of-day light) and cloud shadow.
    float3 rel = mul(i.viewPos, (float3x3)gViewRot); // point - camera (ECEF axes)
    float3 sunEcef = mul(L, (float3x3)gViewRot);
    float shadow = CloudShadow(rel, sunEcef);
    float sinEl = SunSinAt(rel, sunEcef);
    float3 sunColor = SunColor(sinEl);
    float3 sky = SkyLight(sinEl);
    float3 sun = sunColor * shadow;
    float3 imageryLight = ImageryLight(sinEl, shadow, sunColor, sky);
    float3 color;
    if (gFlags.z == 0)
    {
        // Terrain: imagery already contains shading, so only add the relief
        // relative to flat ground. Seabed gets a bathymetric tint and stronger
        // relief so the underwater topology is readable.
        float flatNdl = saturate(dot(normalize(gUpView.xyz), L));
        bool under = i.water > 0.5 && i.height < gWaterParams.z;
        if (under)
        {
            float depth = gWaterParams.z - i.height;
            albedo = lerp(albedo, BathymetryRamp(depth), 0.85);
        }
        float relief = under ? 1.3 : 0.9;
        float shade = clamp(1.0 + relief * (ndl - flatNdl), under ? 0.45 : 0.3, 1.6);
        color = albedo * shade * imageryLight;

        if (gWaterParams.y > 0.0)
        {
            float f = i.height / gWaterParams.y;
            float w = max(fwidth(f), 1e-5);
            float d = abs(frac(f + 0.5) - 0.5);
            float lineAmount = 1.0 - smoothstep(0.0, w * 1.2, d);
            float major = abs(frac(f / 5.0 + 0.5) - 0.5) * 5.0 < 0.5 ? 1.0 : 0.6;
            float3 lineColor = under ? float3(0.85, 0.95, 1.0) : float3(0.05, 0.04, 0.02);
            color = lerp(color, lineColor, lineAmount * major * 0.7);
        }
    }
    else if (gFlags.z == 2 && gRoofParams.w > 0.5)
    {
        color = ShadeBuilding(i.viewPos, i.uv, uint(i.col.a * 255.0 + 0.5), albedo, N, V, L, sun, sky,
                              imageryLight);
    }
    else
    {
        float upness = dot(N, normalize(gUpView.xyz));
        if (upness > 0.85)
            albedo *= 0.82; // roofs / decks slightly darker than walls
        float ambient = 0.32 + 0.14 * saturate(upness);
        color = albedo * (ambient * sky + 0.75 * ndl * sun);
    }
    return float4(ApplyFog(color, i.viewPos), 1.0);
}

// ---------------------------------------------------------------------------
struct SkyOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

SkyOut VSSky(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    SkyOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.ndc = o.pos.xy;
    return o;
}

float4 PSSky(SkyOut i) : SV_Target
{
    float3 dirView = normalize(float3(i.ndc.x * gProjParams.x, i.ndc.y * gProjParams.y, -1.0));
    return float4(SkyColor(dirView), 1.0);
}

// ---------------------------------------------------------------------------
struct WaterIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 wave : TEXCOORD0; // local east/north metres (for wave pattern)
};

struct WaterOut
{
    float4 svpos   : SV_Position;
    float3 viewPos : TEXCOORD0;
    float3 nrm     : NORMAL;
    float2 wave    : TEXCOORD1;
};

WaterOut VSWater(WaterIn v)
{
    WaterOut o;
    float4 vp = mul(gModelView, float4(v.pos, 1.0));
    o.viewPos = vp.xyz;
    o.svpos = mul(gProj, vp);
    o.nrm = mul((float3x3)gModelView, v.nrm);
    o.wave = v.wave;
    return o;
}

// With MSAA the water runs per sample (SV_SampleIndex): the water column is
// measured against the depth of the same sample, so shorelines and pier edges
// do not get a fringe of wrongly thin water.
float4 PSWater(WaterOut i
#if MSAA_SAMPLES > 1
               , uint sampleIndex : SV_SampleIndex
#endif
               ) : SV_Target
{
#if MSAA_SAMPLES > 1
    float sceneDepth = LoadSceneDepth(int2(i.svpos.xy), sampleIndex);
#else
    float sceneDepth = LoadSceneDepth(int2(i.svpos.xy), 0);
#endif
    float waterZ = max(-i.viewPos.z, 1e-3);
    float sceneZ = sceneDepth > 0.0 ? gProjParams.z / sceneDepth : 1e9;
    // Water column length along the view ray between surface and seabed.
    float thickness = max(sceneZ - waterZ, 0.0) * length(i.viewPos) / waterZ;
    float transmittance = exp(-thickness * gWaterParams.x);

    float3 V = normalize(-i.viewPos);
    float3 N = normalize(i.nrm);
    // Procedural ripples: a sum of directional waves with unrelated
    // wavelengths/directions (no visible tiling), faded out with distance and
    // with the pixel footprint to avoid aliasing.
    float t = gProjParams.w;
    float2 p = i.wave;
    float footprint = max(length(fwidth(p)), 1e-4);
    float fade = saturate(1.0 - length(i.viewPos) / 2500.0);
    const float2 dirs[7] = { float2(0.96, 0.28), float2(-0.37, 0.93), float2(0.75, -0.66), float2(-0.98, -0.21),
                             float2(0.31, 0.95), float2(0.64, 0.77), float2(-0.81, 0.59) };
    const float lens[7] = { 13.7, 9.1, 6.3, 4.4, 3.1, 2.3, 1.7 };
    float2 slope = 0.0;
    [unroll] for (int k = 0; k < 7; ++k)
    {
        float w = 6.2831853 / lens[k];
        float phase = dot(dirs[k], p) * w + t * sqrt(9.81 * w) + k * 1.7;
        float antiAlias = saturate(lens[k] / (3.0 * footprint) - 1.0); // drop waves below ~3 px
        slope += dirs[k] * cos(phase) * (0.012 + 0.004 * k) * antiAlias;
    }
    slope *= fade;
    float3 east = normalize(cross(mul((float3x3)gViewRot, float3(0, 0, 1)), N));
    float3 northV = normalize(cross(N, east));
    N = normalize(N - east * slope.x - northV * slope.y);

    // Sun at this point: elevation (time-of-day light) and cloud shadow.
    float3 L = normalize(gSunDirView.xyz);
    float3 rel = mul(i.viewPos, (float3x3)gViewRot);
    float3 sunEcef = mul(L, (float3x3)gViewRot);
    float shadow = CloudShadow(rel, sunEcef);
    float sinEl = SunSinAt(rel, sunEcef);
    float3 sunColor = SunColor(sinEl);

    float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(N, V)), 5.0);
    float3 R = reflect(-V, N);
    float3 reflection = SkyColor(R);
    float3 body = float3(0.010, 0.045, 0.060) * ImageryLight(sinEl, shadow, sunColor, SkyLight(sinEl));
    float3 color = lerp(body, reflection, fresnel);
    float alpha = saturate((1.0 - transmittance) + fresnel * transmittance);
    float3 spec = sunColor * (shadow * pow(saturate(dot(R, L)), 250.0) * 1.6);
    color += spec;
    alpha = saturate(alpha + max(max(spec.r, spec.g), spec.b));
    color = ApplyFog(color, i.viewPos);
    return float4(color, alpha);
}

// ---------------------------------------------------------------------------
// Roof imagery capture: terrain tiles rendered top-down (orthographic,
// gModelView = full MVP) with only their draped imagery, into one slice of
// gRoofImagery. Alpha 1 marks texels that received imagery.
struct CaptureOut
{
    float4 svpos : SV_Position;
    float2 uv    : TEXCOORD0;
};

CaptureOut VSCapture(VSIn v)
{
    CaptureOut o;
    o.svpos = mul(gModelView, float4(v.pos, 1.0));
    o.uv = v.uv;
    return o;
}

float4 PSCapture(CaptureOut i) : SV_Target
{
    float2 uv = i.uv * gOverlayST.xy + gOverlayST.zw;
    uv.y = 1.0 - uv.y;
    return float4(gOverlay.Sample(gLinearClamp, uv).rgb, 1.0);
}

// ---------------------------------------------------------------------------
// Cloud pass: a full-screen pass (VSSky) after the water. Per layer the ray
// crosses the slab between the layer's base and top (per-cell heights, found by
// a fixed-point iteration: the slab under the previous entry; a coarse mip keeps
// it convergent). The cloud amount is sampled along the segment inside the slab:
// in the middle, and for oblique rays and from inside the layer also near the
// entry and the exit (the sides of the clouds), with a vertical envelope (wider
// at the base, narrower at the top). Lit as a non-absorbing two-stream layer,
// shadowed by the layers above (mean cover), hazed by aerial perspective;
// composited front to back. SV_Depth = the nearest layer, so terrain, buildings
// and vessels in front occlude it per MSAA sample. The step to volumetric clouds
// replaces the slab samples by a ray march with 3D detail.

// Base and top of a layer (m above the sphere) from a coarse mip.
float2 LayerBaseTop(int layer, float2 uv)
{
    float4 f = gCloudField.SampleLevel(gLinearClamp, float3(uv, layer), 2.0);
    float base = (gCloudLayerOn.w >= 0.0 ? gCloudLayerOn.w : f.y * 1000.0) + gCloudParams.w;
    return float2(base, base + max(f.w * 1000.0, 100.0));
}

struct LayerSpan
{
    float t0, t1;      // the ray inside the slab from t0 to t1 (m); t0 < 0: it misses
    float hBase, hTop; // the slab at the entry (m above the sphere)
    int side;          // the camera: -1 below the base, 0 inside, 1 above the top
};

LayerSpan TraceLayer(int layer, float3 dir, float b)
{
    float hc = gCloudOrigin.w;
    float2 lonLat;
    float2 uv = CloudGridUv(gCloudCamera.xyz, lonLat);
    float2 bt = LayerBaseTop(layer, uv);
    int side = hc < bt.x ? -1 : (hc > bt.y ? 1 : 0);
    float t0 = -1.0, t1 = -1.0;
    [unroll] for (int k = 0; k < 3; ++k)
    {
        float2 rb = RayShellRoots(b, ShellC(bt.x)); // the base sphere
        float2 rt = RayShellRoots(b, ShellC(bt.y)); // the top sphere
        if (side < 0)
        {
            // Inside both spheres: the ray leaves the base sphere, then the top one.
            t0 = rb.y;
            t1 = rt.y;
        }
        else if (side > 0)
        {
            // Outside both: a descending ray enters the top sphere and leaves the
            // slab through the base sphere, or through the top again (grazing).
            t0 = rt.x;
            t1 = rb.x > 0.0 ? rb.x : rt.y;
        }
        else
        {
            // Inside the top sphere, outside the base one: out through either.
            t0 = 0.0;
            t1 = rb.x > 0.0 ? min(rb.x, rt.y) : rt.y;
        }
        if (k < 2 && t0 >= 0.0)
        {
            float tRef = side == 0 ? min(t1, 20000.0) : t0; // the slab under the entry (the exit from inside)
            uv = CloudGridUv(gCloudCamera.xyz + tRef * dir, lonLat);
            bt = LayerBaseTop(layer, uv);
        }
    }
    LayerSpan o;
    o.hBase = bt.x;
    o.hTop = bt.y;
    o.side = side;
    // Below the layer a ray that meets the ground first sees no cloud (matters
    // where nothing else was drawn, e.g. with the terrain hidden).
    float cGround = ShellC(gCloudParams.x);
    bool blocked = side < 0 && b < 0.0 && b * b > cGround;
    o.t0 = blocked ? -1.0 : t0;
    o.t1 = max(t1, t0);
    return o;
}

// Vertical envelope per layer: the threshold shift (in u) between the base and
// the top of the slab (cumulus columns narrow upwards; decks are flatter).
static const float kLayerShape[3] = { 0.35, 0.18, 0.12 };

// Mean transmission of the layers above `layer` along the sun ray from a point
// of it (macro cover only, no noise).
float LayersAbove(int layer, float3 rel, float h, float3 sunEcef, float sunUp)
{
    float transmission = 1.0;
    [branch] if (gCloudMisc.y > 0.5)
    {
        float bb = dot(gCloudOrigin.xyz + rel, sunEcef);
        [unroll] for (int m = 0; m < 3; ++m)
        {
            if (m > layer)
            {
                float2 lonLat;
                float2 uv = CloudGridUv(gCloudCamera.xyz + rel, lonLat);
                float s = SunRayToShell(bb, h, LayerHeight(m, uv));
                uv = CloudGridUv(gCloudCamera.xyz + rel + s * sunEcef, lonLat);
                float4 field = gCloudField.SampleLevel(gLinearClamp, float3(uv, m), 1.0);
                float cover = saturate(field.x) * GridEdge(uv) * gCloudLayerOn[m];
                transmission *= 1.0 - cover * (1.0 - 1.0 / (1.0 + 0.15 * field.z / max(sunUp, 0.05)));
            }
        }
    }
    return transmission;
}

// Adds one layer behind what is accumulated so far (front-to-back "under").
void CompositeLayer(inout float3 color, inout float transmittance, inout float nearest, float3 premult, float alpha,
                    float dist)
{
    color += transmittance * premult;
    transmittance *= 1.0 - alpha;
    nearest = alpha > 0.004 ? min(nearest, dist) : nearest;
}

float4 PSClouds(SkyOut i, out float depthOut : SV_Depth) : SV_Target
{
    float3 dirView = normalize(float3(i.ndc.x * gProjParams.x, i.ndc.y * gProjParams.y, -1.0));
    float3 dir = mul(dirView, (float3x3)gViewRot); // view space -> ECEF axes (inverse rotation)
    float3 sunEcef = mul(gSunDirView.xyz, (float3x3)gViewRot);
    float b = dot(dir, gCloudOrigin.xyz); // > 0: the ray climbs
    const float3 sunTint = float3(1.0, 0.97, 0.92);

    float3 premult[3];
    float alpha[3], dist[3], gap[3];
    [unroll] for (int l = 0; l < 3; ++l)
    {
        LayerSpan span = TraceLayer(l, dir, b);
        float tEntry = max(span.t0, 0.0);
        float tMid = 0.5 * (tEntry + span.t1);
        float2 lonLat;
        float2 uvMid = CloudGridUv(gCloudCamera.xyz + tMid * dir, lonLat);
        float2 kmMid = NoiseKm(lonLat);
        // Derivatives before any branch on the hit.
        float2 duvx = ddx(uvMid), duvy = ddy(uvMid);
        float fpKm = max(length(fwidth(kmMid)), 1e-6);
        float4 field = gCloudField.SampleGrad(gLinearClamp, float3(uvMid, l), duvx, duvy);
        float cover = saturate(field.x) * GridEdge(uvMid) * gCloudLayerOn[l];
        premult[l] = 0.0;
        alpha[l] = 0.0;
        dist[l] = 1e30;
        gap[l] = 0.0;
        [branch] if (span.t0 >= 0.0 && cover > 0.002)
        {
            float thickness = span.hTop - span.hBase;
            float len = span.t1 - tEntry;
            float ratio = len / thickness; // path length in slab thicknesses (1: straight through)
            float shape = kLayerShape[l];
            float zMid = saturate((HeightAt(tMid, b) - span.hBase) / thickness);
            CloudAmount a = LayerAmount(l, cover, uvMid, kmMid, fpKm, 6, shape * (0.5 - zMid));
            float amount = a.amount, density = a.density, first = tEntry;
            // Oblique rays and rays from inside the layer: two more samples near
            // the entry and the exit (blended in with the obliqueness).
            float w = saturate(ratio - 1.5);
            [branch] if (w > 0.0)
            {
                float tA = tEntry + 0.12 * len, tB = tEntry + 0.88 * len;
                float2 uvA = CloudGridUv(gCloudCamera.xyz + tA * dir, lonLat);
                float2 kmA = NoiseKm(lonLat);
                float zA = saturate((HeightAt(tA, b) - span.hBase) / thickness);
                CloudAmount sA = LayerAmount(l, cover, uvA, kmA, fpKm, 6, shape * (0.5 - zA));
                float2 uvB = CloudGridUv(gCloudCamera.xyz + tB * dir, lonLat);
                float2 kmB = NoiseKm(lonLat);
                float zB = saturate((HeightAt(tB, b) - span.hBase) / thickness);
                CloudAmount sB = LayerAmount(l, cover, uvB, kmB, fpKm, 6, shape * (0.5 - zB));
                amount = (a.amount + w * (sA.amount + sB.amount)) / (1.0 + 2.0 * w);
                density = (a.density + w * (sA.density + sB.density)) / (1.0 + 2.0 * w);
                first = sA.amount > 0.3 ? tA : (a.amount > 0.3 ? tMid : (sB.amount > 0.3 ? tB : tEntry));
            }
            // Opacity = 1 - direct transmission along the path through the slab.
            float tau = field.z * amount * density;
            float a0 = 1.0 - exp(-tau * min(ratio, 16.0));
            float3 relEntry = tEntry * dir;
            float3 nrm = normalize(gCloudOrigin.xyz + relEntry); // up at the entry
            float hEntry = span.side > 0 ? span.hTop : (span.side < 0 ? span.hBase : gCloudOrigin.w);

            // Light the layer scatters towards the camera (premultiplied by
            // coverage): two-stream total transmission T = 1 / (1 + 3/4 (1 - g)
            // tau) of a non-absorbing layer (g = 0.8). Seen from below or from
            // inside: T minus the unscattered sun beam (the base of a thick cloud
            // is grey, thin cloud glows); from above: 1 - T (bright tops).
            float sunUp = dot(nrm, sunEcef);
            float3 sun = sunTint * SunColor(sunUp) * LayersAbove(l, relEntry, hEntry, sunEcef, sunUp);
            float3 irradiance = 0.9 * saturate(sunUp) * sun + 0.25 * sunTint * SkyLight(sunUp); // sun + sky
            float trans = 1.0 / (1.0 + 0.15 * tau);
            float3 scattered;
            if (span.side <= 0)
            {
                float direct = exp(-tau / max(saturate(sunUp), 0.1));
                // Forward scattering around the sun (Henyey-Greenstein, g = 0.6).
                float hg = 0.64 / pow(abs(1.36 - 1.2 * dot(dir, sunEcef)), 1.5); // base >= 0.16
                scattered = irradiance * max(trans - direct, 0.0) + sun * (0.08 * hg * a0 * exp(-0.5 * tau)) +
                            gSkyZenith.rgb * 0.12 * a0;
            }
            else
            {
                scattered = irradiance * (1.05 * (1.0 - trans)) + sun * (0.1 * a0) + gSkyZenith.rgb * 0.12 * a0;
            }
            // Aerial perspective (as ApplyFog) on the cloud.
            float fog = (1.0 - exp(-first / gFogParams.x)) * gFogParams.y;
            premult[l] = scattered * (1.0 - fog) + gHazeColor.rgb * (a0 * fog);
            alpha[l] = a0;
            dist[l] = first;
            gap[l] = fog * saturate(cover * 2.0);
        }
    }

    // Front to back: a climbing ray meets the layers low -> high, a descending one high -> low.
    float3 color = 0.0;
    float transmittance = 1.0;
    float nearest = 1e30;
    if (b >= 0.0)
    {
        CompositeLayer(color, transmittance, nearest, premult[0], alpha[0], dist[0]);
        CompositeLayer(color, transmittance, nearest, premult[1], alpha[1], dist[1]);
        CompositeLayer(color, transmittance, nearest, premult[2], alpha[2], dist[2]);
    }
    else
    {
        CompositeLayer(color, transmittance, nearest, premult[2], alpha[2], dist[2]);
        CompositeLayer(color, transmittance, nearest, premult[1], alpha[1], dist[1]);
        CompositeLayer(color, transmittance, nearest, premult[0], alpha[0], dist[0]);
    }
    // The sky seen through gaps inside cloudy areas gets the same aerial
    // perspective as the clouds around it (no bluer gaps in distant cloud fields).
    float gapFog = max(gap[0], max(gap[1], gap[2]));
    float alphaOut = 1.0 - transmittance * (1.0 - gapFog);
    color += gHazeColor.rgb * (transmittance * gapFog);

    float viewZ = (nearest < 1e29 ? nearest : 1.0) * -dirView.z;
    depthOut = gProjParams.z / max(viewZ, 1e-3); // reversed Z
    if (nearest >= 1e29 || alphaOut < 0.004)
        discard;
    return float4(color / max(alphaOut, 1e-4), alphaOut);
}

// ---------------------------------------------------------------------------
// Cloud blend (compute, before the frame): the two datasets of the time blend,
// each carried by its own wind field (a forward from its valid time, b backward
// from its own: motion-compensated interpolation, semi-Lagrangian per cell),
// blended by time and cross-faded in from the snapshot of the previous state.
// Output mip 0 (the renderer generates the mips) and the drift of the two noise
// phases per cell (km), which the shapes add to their coordinates.
Texture2DArray<float4>   gBlendA        : register(t6);
Texture2DArray<float4>   gBlendB        : register(t7);
Texture2DArray<float4>   gBlendSnapshot : register(t8);
Texture2DArray<float2>   gBlendWindA    : register(t10); // layer wind per cell of a (m/s east / north)
Texture2DArray<float2>   gBlendWindB    : register(t11);
Texture2DArray<float2>   gBlendWindOldA : register(t12); // of the datasets a / b replaced (their winds glide)
Texture2DArray<float2>   gBlendWindOldB : register(t13);
RWTexture2DArray<float4> gBlendOut      : register(u1);
RWTexture2DArray<float4> gDriftOut      : register(u3);

cbuffer CloudBlendConstants : register(b2)
{
    float4 gBlendParams; // x: weight of b, y: fade from the snapshot, zw: 1 / texture size
    float4 gBlendTimes;  // x: drift time of a (s), y: of b, z: wind glide (old -> new datasets)
    float4 gBlendGrid;   // x: north (rad), y: lat span (rad), z: 1 / (R lon span), w: 1 / (R lat span)
};

float2 BlendWindA(float2 uv, uint layer)
{
    return lerp(gBlendWindOldA.SampleLevel(gLinearClamp, float3(uv, layer), 0.0),
                gBlendWindA.SampleLevel(gLinearClamp, float3(uv, layer), 0.0), gBlendTimes.z);
}

float2 BlendWindB(float2 uv, uint layer)
{
    return lerp(gBlendWindOldB.SampleLevel(gLinearClamp, float3(uv, layer), 0.0),
                gBlendWindB.SampleLevel(gLinearClamp, float3(uv, layer), 0.0), gBlendTimes.z);
}

// Grid displacement of air moving with `wind` (m/s east / north) for dt seconds
// from a point at uv (the row's latitude).
float2 WindUv(float2 wind, float dt, float2 uv)
{
    float lat = gBlendGrid.x - uv.y * gBlendGrid.y;
    return float2(wind.x * dt * gBlendGrid.z / cos(lat), -wind.y * dt * gBlendGrid.w);
}

[numthreads(8, 8, 1)]
void CSBlendClouds(uint3 id : SV_DispatchThreadID)
{
    uint w, h, slices;
    gBlendOut.GetDimensions(w, h, slices);
    if (id.x >= w || id.y >= h || id.z >= slices)
        return;
    float2 uv = (float2(id.xy) + 0.5) * gBlendParams.zw;
    // Where the air now at uv was at each dataset's valid time (two iterations).
    float2 wa = BlendWindA(uv, id.z);
    float2 ua = uv - WindUv(wa, gBlendTimes.x, uv);
    wa = BlendWindA(ua, id.z);
    ua = uv - WindUv(wa, gBlendTimes.x, uv);
    float2 wb = BlendWindB(uv, id.z);
    float2 ub = uv - WindUv(wb, gBlendTimes.y, uv);
    wb = BlendWindB(ub, id.z);
    ub = uv - WindUv(wb, gBlendTimes.y, uv);
    float4 a = gBlendA.SampleLevel(gLinearClamp, float3(ua, id.z), 0.0);
    float4 b = gBlendB.SampleLevel(gLinearClamp, float3(ub, id.z), 0.0);
    float4 blended = lerp(a, b, gBlendParams.x);
    gBlendOut[id] = lerp(gBlendSnapshot.Load(int4(id, 0)), blended, gBlendParams.y);
    // The noise phases drift with the same air (offsets in km east / north).
    gDriftOut[id] = float4(-wa * gBlendTimes.x, -wb * gBlendTimes.y) * 0.001;
}

// Cloud shadow map (compute, after the blend, with the frame constants):
// CloudTransmission of the point at sea level under each texel of the central
// half of the weather grid, for CloudShadow.
RWTexture2D<float> gShadowOut : register(u2);

[numthreads(8, 8, 1)]
void CSCloudShadow(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gShadowOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h)
        return;
    float2 uv = 0.25 + 0.5 * (float2(id.xy) + 0.5) / float2(w, h);
    float lon = gCloudGrid.x + uv.x / gCloudGrid.z, lat = gCloudGrid.y - uv.y / gCloudGrid.w;
    float sinLat = sin(lat), cosLat = cos(lat);
    float n = 6378137.0 * rsqrt(1.0 - 0.00669438 * sinLat * sinLat); // WGS84 prime vertical radius
    float hSea = gCloudParams.x;
    float3 p = float3((n + hSea) * cosLat * cos(lon), (n + hSea) * cosLat * sin(lon),
                      (n * (1.0 - 0.00669438) + hSea) * sinLat);
    float3 sunEcef = mul(gSunDirView.xyz, (float3x3)gViewRot);
    // Footprint: half a texel - edges about a texel wide, bilinear filtering does the rest.
    float texelKm = 0.5 * max(6378.1 * cosLat / (gCloudGrid.z * float(w)), 6356.8 / (gCloudGrid.w * float(h)));
    gShadowOut[id.xy] = CloudTransmission(p - gCloudCamera.xyz, sunEcef, 0.5 * texelKm);
}

// ---------------------------------------------------------------------------
#if MSAA_SAMPLES > 1
// MSAA resolve (full-screen triangle from VSSky) into the back buffer's sRGB
// RTV. Averaging happens in linear space: the sRGB SRV decodes on load and the
// RTV encodes on write (a hardware resolve into the UNORM back buffer would
// average gamma-encoded values and darken edges).
float4 PSResolve(SkyOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    float4 sum = 0.0;
    [unroll] for (uint s = 0; s < MSAA_SAMPLES; ++s)
        sum += gSceneColor.Load(p, s);
    return sum / MSAA_SAMPLES;
}
#endif

// ---------------------------------------------------------------------------
// Depth picking: reads the scene depth under one pixel (gFlags.xy) into a
// 1-element buffer. With MSAA the nearest sample wins (reversed Z: max), so a
// click on an anti-aliased edge picks the object rather than what lies behind.
RWBuffer<float> gPickResult : register(u0);

[numthreads(1, 1, 1)]
void CSPickDepth()
{
    int2 p = int2(gFlags.xy);
    float d = 0.0;
    [unroll] for (uint s = 0; s < MSAA_SAMPLES; ++s)
        d = max(d, LoadSceneDepth(p, s));
    gPickResult[0] = d;
}
