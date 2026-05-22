# Static Shadow Lightmaps — Design

Branch: `feature/static-shadow-lightmaps`

## Goal
Pre-bake the static-omni shadow contribution onto static meshes at scene init so the deferred lighting kernel can skip the expensive per-pixel cube shadow tap for the (static omni × static surface) combination. Dynamic surfaces and dynamic occluders still sample shadow maps at runtime.

Concretely for greets: cut ~10 cube-shadow taps per static-surface pixel (~30 cycles each) by replacing them with one per-omni lightmap byte read.

## Non-goals
- Bake diffuse / spec / ambient. Only **shadow factor**. Preserves bump-map detail on static-omni diffuse + spec.
- Dynamic omnis. Their shadow content changes per frame; can't precompute.
- Soft shadows / multi-tap PCF in the lightmap. One tap per (texel, omni); softness can be added later via blur or fatter taps at bake time.

## Cube-face per-pixel verdict (settled)
One cube face per (texel|pixel, omni) at both bake and runtime. The `CubeShadow_SelectFace` math picks the dominant-axis face from the world-direction vector; that face's depth buffer is the right thing to sample. No iteration over 6 faces. The earlier crash was a separate issue (frustum-edge `lz` rejection, already fixed).

## Iteration order (settled)
**Outer: static omni. Middle: mesh → face → lightmap-tile → texel.**

Rationale:
- Each omni's cube shadow buffer (~1.5 MB for 256² × 6 × uint16) stays L2-warm for the entire pass.
- Mesh / face / tile data is small and rewalked per omni — cheap.
- Per-omni work parallelizes trivially across `ThreadPool` workers (one omni per worker; no write contention because each omni writes its own slice of the lightmap).

The reverse (outer mesh, inner omni) thrashes the shadow-buffer cache between iterations because each per-omni buffer evicts the next.

## UV channel decision (the hard one)

Three viable storage forms; all avoid touching `Vertex.h`:

### Option A — per-face mini-atlas, indexed by barycentric
Each face gets its own NxN lightmap (e.g. 16×16 = 256 bytes per face per omni). Rasterizer interpolates barycentric (s, t) per pixel; (s, t) is already computed for vertex-attribute interpolation. We sample the face's lightmap directly.

- **Pros**: no `Vertex` change, no FLD change, no UV bleed between faces (each face is isolated), per-face cull is implicit (face has its own atlas).
- **Cons**: per-face memory overhead even for tiny faces. Indirection per pixel (face index → atlas offset).
- **Memory greets**: ~50 faces × 16² × 10 omnis × 1 byte = **130 KB**. Trivial.
- **Memory city** (rough): ~2000 static faces × 16² × 10 omnis × 1 byte = **5 MB**. Acceptable.

### Option B — per-mesh atlas, computed planar UVs at scene init
Each mesh gets one NxN lightmap shared across all its faces. At scene init, plan a non-overlapping UV unwrap (e.g., greedy planar projection per face, packed into the atlas). Per-vertex lightmap UVs computed and stored in a parallel per-mesh `std::vector<Vec2>`.

- **Pros**: better packing — small faces use tiny atlas regions, big faces use big regions. Roughly half the memory of Option A.
- **Cons**: requires UV unwrap logic (~100 lines for a greedy planar packer). Cross-face seams need padding to avoid bleed in bilinear-sampled lookups.
- **Memory greets**: ~256² × 10 omnis × 1 byte = **640 KB** per mesh. Heavier per-mesh but constant.

### Option C — reuse existing diffuse UVs
Use `V->U, V->V` as lightmap coords directly. No new storage for UVs.

- **Pros**: zero new data; falls out of existing rasterizer.
- **Cons**: diffuse textures often tile (UV > 1 or repeated by adjacent faces). Multiple faces mapping to the same lightmap texel → unrelated shadows blend together → **bleed everywhere**. Unusable for most assets.

**Decision: Option A.** Per-face mini-atlases at 16×16 each. Memory is fine at greets and city scales. No UV unwrap algorithm needed. No bleed possible. Barycentric is already in hand at rasterization time.

If 16×16 turns out coarse on big faces, the resolution is a per-face property — bump to 32×32 on the few large faces in the scene, leave 16×16 default.

## Data structures

```cpp
// New header: FDS/Base/StaticShadowLightmap.h
struct StaticShadowLightmap {
    int       lmRes;        // N per face (16 default; per-face override possible)
    int       numFaces;     // M, matches mesh face count
    int       numOmnis;     // K, # static-omni shadow casters at bake time

    // Layout: [face][y * N + x][omni] — omnis adjacent so the deferred
    // kernel reads K bytes contiguous per pixel.
    // Size: M * N * N * K bytes.
    std::vector<uint8_t> data;

    // Per-(face, omni) "any non-trivially-lit texel?" bitmask. One bit
    // per (face, omni); used by the runtime kernel to early-out when
    // an omni contributes nothing to a face. Size: ceil(M*K/8) bytes.
    std::vector<uint8_t> coverageBits;

    // Indexes into the parent scene's static-omni list. Lets the kernel
    // map omni-index-in-lightmap ↔ omni-index-in-ViewLightsSoA.
    std::vector<int> omniIndex;  // size K
};

// Hung off TriMesh as a pointer (null = "not a lightmapped mesh").
// One per static mesh; nullptr for dynamic meshes.
// Field added to TriMesh struct.
struct TriMesh {
    // ... existing fields ...
    StaticShadowLightmap *staticShadowLM = nullptr;
};
```

## Bake pipeline

Runs once at scene init, after `Animate_Objects()` (for IPos) and after `ShadowMaps_BakeStatic()` (so cube buffers are populated).

Per-omni cube buffers are sampled with the same `CubeShadow_SelectFace` + per-face projection math as the runtime kernel — exactly mirrors what we'd do at sample time. Code shared with `CubeShadow_Sample` via a helper that takes `(omni, worldPos)` and returns `uint8_t` shadow factor.

```
for each static omni o (parallel via ThreadPool):
  if not (O->Flags & Omni_StaticShadow): continue

  for each static mesh m in scene (Pos.NumKeys == 1 or extent < eps, no animated ancestor):
    if m doesn't have staticShadowLM: allocate now
    if dist(o.IPos, m.bsphere) > o.IRange + m.bsphereRadius: continue  // whole-mesh range cull

    for each face f in m:
      // Face back-face cull
      if (faceNormal · (o.IPos - faceCenter)) < 0: continue
      // Face range cull
      if dist(o.IPos, faceCenter) > o.IRange + faceBSphereRadius: continue

      // Walk the face's mini-atlas
      for each (y, x) in NxN:
        // Compute world position at (s=x/N, t=y/N) barycentric on face
        worldPos = face.A + s * (face.B - face.A) + t * (face.C - face.A)
        L = o.IPos - worldPos
        if |L|² > o.IRange²: store 0 (out of range); continue

        // Per-texel back-face refinement (interpolated normal would help on
        // smoothed meshes — but face normal works for flat faces, so use it
        // here for simplicity; revisit if quality matters).

        // Sample static cube shadow buffer at this direction
        shadow_factor = SampleCubeShadowAtWorldPos(o, worldPos)  // shared helper
        lightmap[f][y][x][o_lm_idx] = shadow_factor

        if shadow_factor > 0: set coverageBits[f * K + o_lm_idx]
```

The "tile" granularity I mentioned earlier becomes implicit: each face *is* a tile (16×16 texels). The per-face cull at the top of the per-face loop is the coarse stage; the per-texel `|L|² > range²` is the fine stage.

## Mekalele rasterizer changes

Mekalele currently interpolates vertex attributes (RZ, UVs, normals, tangent) per pixel. We need to add:

1. **Per-pixel `(face_index, lm_s, lm_t)`** — pack into a new G-buffer slot or compute on-the-fly.

   `face_index` is what Mekalele uses internally already (the current triangle being rasterized). At pixel emission, we know which face this pixel belongs to → store that in a new G-buffer u32 slot.

   `lm_s, lm_t` ∈ [0,1] are the barycentric (s, t) of the pixel inside the triangle. Mekalele already computes barycentric per pixel for UV interp; we just pack 16 bits of `lm_s` + 16 bits of `lm_t` into another G-buffer slot.

2. **New G-buffer field**: `lightmapKey` = `(face_index_24 << 8) | lm_face_size_8`, plus a separate field for `(lm_s_16 | lm_t_16)`. Two uint32s per pixel.

   Alternative: pack everything into one uint32 — `face_index_16 | lm_s_8 | lm_t_8`. 16-bit face index supports 64K faces (fine), 8-bit s/t gives 1/256 precision on a 16² atlas → exactly maps to a texel.

   **Decision: one uint32 G-buffer slot, packed as `face16 | s8 | t8`.**

3. **Storage growth**: G-buffer adds 4 bytes per pixel (1920×1080 = ~8 MB). Acceptable. Lives in the same `meka::GBuffer` struct.

## Deferred lighting kernel changes

For pixels whose face has a lightmap (mesh is in static set):
```cpp
const uint32_t lmKey = gb.lightmapKey[pixelIdx];   // face16 | s8 | t8
const int faceIdx = int(lmKey >> 16);
const int s = int((lmKey >> 8) & 0xFF);   // 0..255, lookup as s * N / 256
const int t = int(lmKey & 0xFF);
const StaticShadowLightmap *lm = currentMesh->staticShadowLM;
const uint8_t *facePtr = lm->data.data() + faceIdx * (lm->lmRes * lm->lmRes * lm->numOmnis);
const int tx = (s * lm->lmRes) >> 8;
const int ty = (t * lm->lmRes) >> 8;
const uint8_t *texelPtr = facePtr + (ty * lm->lmRes + tx) * lm->numOmnis;

// Per omni in tile:
for (each omni in tile-light-list):
    if (omni is in lm->omniIndex) {
        const int lmIdx = ... find lmIdx for this omni ...
        const uint8_t shadowFactor = texelPtr[lmIdx];
        // Skip the runtime cube shadow tap — we have the answer.
        // Multiply shadowFactor / 255.0f into the omni's diffuse + spec contribution.
    } else {
        // Dynamic omni → standard per-pixel shadow tap path
    }
```

Caveats:
- The kernel needs `currentMesh` per pixel. We currently don't carry that through the G-buffer — `mat32`'s matID gets us to the material but not the mesh. Two options:
  - **Option α**: Add `meshId_8` to the G-buffer (giving up 8 bits somewhere else, or growing the slot).
  - **Option β**: Bake the `StaticShadowLightmap*` directly into the per-material's user-data, indexed via `mat32`'s matID. Works if material is uniquely owned by mesh — which is the case for static meshes after the per-mesh material cloning we did in Phase 6.
  - **Decision: Option β.** Reuse the existing material→mesh association.
- The `omniIndex` array search per pixel is O(K). For K=10 it's negligible, but cleaner to store a per-omni *index-into-lightmap* alongside the omni's ViewLightsSoA slot at frame setup. Then per-pixel: read `lmIdx = lights->staticLMIdx[li]`, branch on lmIdx >= 0.

## Flag plan

```
--shadow-lightmap (bool, default off) — opt-in for now.
  When on: bake at scene init, kernel reads lightmap for static-omni
  contributions on lightmapped meshes; skip the runtime cube tap.
  When off: behavior identical to today (per-pixel cube taps).

--shadow-lightmap-res (int, default 16) — N per face. 16, 32, 64 typical.
--shadow-lightmap-viz (bool, default off) — debug overlay that paints
  lightmap shadow factor as a tinted color so we can see what was baked.
```

## Performance expectations

Per greets static-surface pixel today:
- 10 omnis × (~30 cycles cube shadow tap + ~20 cycles light math) ≈ **500 cycles**
- + 10 omnis × ~30 cycles dynamic shadow tap (`--shadow-dynamic` on) ≈ **300 cycles**

With lightmap:
- One uint32 G-buffer fetch + 10 bytes lightmap read ≈ **15 cycles**
- 10 omnis × ~20 cycles light math (still runs, no shadow tap) ≈ **200 cycles**
- + 10 omnis × ~30 cycles dynamic shadow tap (unchanged) ≈ **300 cycles**

**Net per static-surface pixel: ~515 cycles → ~515 - 285 = ~30-40% saved** when both flags on. Bigger relative savings when `--shadow-dynamic` is off (~60% saved).

For city (95% static-surface pixels), the win scales: lit pixels are dominated by lightmap reads instead of cube taps.

Bake time at scene init:
- Greets: 10 omnis × 50 faces × 16² × 1 shadow tap = **128k taps**. ~5-10 ms. Acceptable.
- City: ~10 omnis × 2000 faces × 16² = **5M taps**. ~200 ms. One-time, ok.

## Risks / open questions

1. **Bilinear within face vs nearest** — 16² mini-atlas with 4-bilinear-tap reads near face edges could read into the next face's column when texel-y is 15 of 15. Solutions: pad the atlas to 17×17 with the edge values, OR clamp at face boundary, OR start with nearest-neighbor and add bilinear later.
   - **Initial choice: nearest-neighbor.** Coarser look but no padding logic. Upgrade later if visible texelation.

2. **Per-face cube-buffer cache pattern at bake** — each face's 16² texels project to ~256 different cube-buffer locations. Within a face, those locations are spatially clustered, so cache stays warm. Across faces, no guarantees. Doesn't matter for one-time bake cost.

3. **Materials shared across static + dynamic meshes** — if a material is referenced by both a static mesh (lightmap present) and a dynamic mesh (no lightmap), the matID-based lookup conflates them. Need to verify: are the per-mesh material clones from Phase 6 active for ALL meshes, including dynamic ones? If yes, no conflation. If no, we need a per-pixel "is lightmapped" bit (1 bit suffices, store anywhere).

4. **What about static meshes that don't make sense to lightmap?** Very small props with only a few faces — overhead might exceed savings. Add a per-mesh opt-out: meshes below N faces skip lightmap. Tunable.

5. **Bake-time SIMD?** Probably not worth it for a one-shot scene-init pass that takes <500 ms in the worst case. Premature.

## Implementation order

1. **Header + storage struct** (StaticShadowLightmap.h, hook into TriMesh).
2. **Bake driver** — `LightmapBake_Static(Scene*)` callable from scene init. Outer-omni dispatch via ThreadPool. Uses a shared `SampleCubeShadowAtWorldPos` helper extracted from `CubeShadow_Sample`.
3. **G-buffer slot for lightmap key** — Mekalele writes `face16|s8|t8` per pixel into a new field.
4. **Kernel hookup** — read lightmap, skip cube tap when bit-set, multiply shadow factor.
5. **Flag + viz** — `--shadow-lightmap` opt-in, debug viz that paints raw lightmap factor.
6. **Bench greets + city** — confirm the predicted perf shift, look for visible artifacts.

Risk (3) gets verified during step 4 — if material clones aren't 1:1 with meshes, we add a `lightmap_present_bit` to the G-buffer at that point.

Step 3 is the riskiest — Mekalele's hot path is sensitive. We'll be adding ~4 bytes per pixel + one packing op per pixel. Worth profiling before/after on city.
