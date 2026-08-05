# GPU_BENCHMARK_PLAN.md — a standalone GPU deferred renderer as a *measuring instrument*

**What this is.** A complete, independent GPU deferred path that renders the greets scene
from the same assets `DEMO` renders, so we can (a) quantify the software-renderer tax and
(b) have a ground truth for rendering questions this project keeps spending weeks on.

**What this is NOT.** Not a shipping backend. Not a hybrid. Not sharing the rasterizer, the
clipper, the fillers, or the deferred kernel. Not byte-matching our output. It does not touch
`DEMO/`, `FDS/FILLERS/`, or `FDS/RENDER/` behaviour, and it is not built unless you ask for it
(`-DFDS_GPU_BENCH=ON`).

Everything below marked **MEASURED** was run on this machine during planning. Everything marked
**ESTIMATE** is a guess and labelled as one. Nothing is asserted from memory.

---

## 0. Machine + toolchain reality (MEASURED)

| Fact | Value | How |
|---|---|---|
| Machine | MacBook Pro Mac14,5, Apple M2 Max, 8P+4E CPU, 64 GB | `system_profiler SPHardwareDataType` |
| GPU | Apple M2 Max, **38 cores**, unified memory, Metal 4 | `system_profiler SPDisplaysDataType`, `MTLDevice` probe |
| GPU families | `MTLGPUFamilyApple7` ✓, `Apple8` ✓, `Metal3` ✓ | `supportsFamily:` probe |
| Developer tools | **Command Line Tools only — no Xcode** (`/Library/Developer/CommandLineTools`) | `xcode-select -p`, `ls /Applications` |
| Offline `metal` compiler | **ABSENT** (`xcrun metal` → "not a developer tool") | direct invocation |
| `xctrace` / Instruments | **UNUSABLE** ("requires Xcode") | `xctrace list templates` |
| Metal / MetalKit / MPS headers | **present** in the CLT SDK, incl. all `MTL4*` | `ls $(xcrun --show-sdk-path)/.../Metal.framework/Headers` |
| **Runtime MSL compilation** | **WORKS** — compiled a vertex + fragment + `[[patch(quad,4)]]` tessellation vertex function via `newLibraryWithSource:` | built and ran a probe `.mm` with `clang++ -ObjC++ -framework Metal` |
| GPU counter sets | `["timestamp"]` | `[dev counterSets]` |
| `supportsCounterSampling:` | `AtStageBoundary` = **YES**, `AtDrawBoundary` = **NO** | direct query |
| Max threadgroup memory | 32768 B | `maxThreadgroupMemoryLength` |
| SDL2 | 2.32.10 (homebrew), ships `SDL_metal.h` | `pkg-config --modversion sdl2` |

Two consequences that shape everything:

1. **No Xcode ⇒ no GPU frame capture, no Metal System Trace.** The instrument must be
   *in-app timestamps*, not a profiler UI. Fortunately stage-boundary counter sampling is
   supported, which is exactly per-pass granularity — the same granularity our CPU-side
   `--xfrm_pass_prof` reports at. `MTLCaptureManager` can still write a `.gputrace` to disk for
   later inspection if Xcode is ever installed; that's a deferred artifact, not a live tool.
2. **No offline shader compiler ⇒ compile MSL at runtime from `.metal` source files.** Measured
   working. This is not a workaround, it's better for a research instrument: edit a shader,
   rerun, no C++ rebuild. Costs tens of ms at startup — irrelevant to steady-state frame timing.

---

## 1. API choice: **Metal**, via Objective-C++, with runtime shader compilation

### Recommendation

Write the benchmark as a small Objective-C++ (`.mm`) app against Metal directly. Shaders live in
`.metal` text files loaded and compiled at startup.

### Why Metal and not WebGPU/Dawn — for a *benchmark's* purpose

1. **Dawn measures Dawn.** On macOS, Dawn *is* a Metal backend. Every number taken through it is
   Metal plus a translation layer plus WebGPU's mandated validation and its conservative
   defaults, with no way to separate the layers. For an instrument whose entire job is "what does
   this scene cost on *this* GPU", inserting an abstraction you cannot subtract is the wrong
   trade. A benchmark's first duty is attributability.
2. **WebGPU cannot express Phase 3, which is the strategic prize.** WebGPU has **no hardware
   tessellation** and no geometry shaders. The displacement comparison the user actually wants
   (flat POM vs shell/prism with real silhouettes vs hardware tessellation) is only fully
   expressible in Metal. Metal here has tessellation (post-tess vertex stage, max rate 16) *and*
   mesh shaders (`MTL4MeshRenderPipeline` is in this SDK) — MEASURED that a
   `[[patch(quad,4)]]` vertex function compiles on this machine.
3. **Dependency + build cost.** Dawn is a `depot_tools`/`gclient` checkout on the order of a
   gigabyte, built with GN, in tens of minutes. For a "just for the lulz" instrument in a tree
   whose whole build is a few seconds of Ninja, that tax is out of proportion — and it would have
   to be vendored or fetched, i.e. become a maintenance obligation.
4. **Per-pass GPU timing is native.** MEASURED: `timestamp` counter set present,
   stage-boundary sampling supported. That is precisely the data the comparison methodology in §5
   needs, and it comes with no third-party plumbing.
5. **The "editor already has a browser build" argument doesn't apply.** It's a real fact about
   `DEMO`'s wasm build (software rasterizer to canvas) and it *would* matter for a portable
   *shipping* backend — which the user explicitly excluded. There is no WebGPU code in this tree
   today, so portability here is a cost, not an existing asset.

### Costs of choosing Metal, stated honestly

- **Objective-C++ enters the tree** for the first time. Contained entirely in the new target;
  nothing else compiles as `.mm`.
- **No Instruments** until/unless Xcode is installed. Mitigation: in-app timestamps are the
  primary instrument (and are what we'd trust anyway); ship a `--gputrace` flag using
  `MTLCaptureManager` so a capture exists on disk for later. Switching to a full Xcode is
  `xcode-select` — no code change.
- **Metal-only.** Accepted: this is an instrument for *this* machine, which is where all the CPU
  baselines were also measured. A cross-vendor number would be a different project.

---

## 2. Scene ingestion — VERIFIED by linking, not proposed

The strong option in the brief was: a new CMake target that links FDS *purely for loading*, so
the input is provably identical to what `DEMO` renders. **This works today, with no FDS refactor.
It was tested, not reasoned about.**

### 2.1 FDS has no SDL dependency at all (MEASURED)

- `FDS/CMakeLists.txt` links exactly one thing: `target_link_libraries(FDS PUBLIC pgo_flags)`.
  No SDL2, no include dirs for it. `find_package(SDL2 REQUIRED)` appears **only** in
  `DEMO/CMakeLists.txt`.
- `nm -u build/FDS/libFDS.a | grep -c SDL_` → **0**. Zero undefined SDL symbols in the archive.

### 2.2 A standalone binary loads greets (MEASURED)

Two throwaway programs were compiled against the *existing* `build/FDS/libFDS.a` with no SDL and
no window:

| Test | Result |
|---|---|
| `ReadFLD("SCENES/GREETS.FLD")` only | links; **50 KB** binary, 4 text symbols. **No rasterizer, no deferred kernel, no VESA pulled in** — static-lib dead-strip does its job. |
| `LoadFLD(&scene, "SCENES/GREETS.FLD")` (read + convert + materials) | links; **353 KB** binary; succeeds in **3.5–4.7 ms** |

Include prelude that works: `Base/FDS_DEFS.H`, `Base/FDS_VARS.H`, `Base/FDS_DECS.H`,
`Base/Scene.h`, `FLD/FLD_READ.H`; compile with `-DSIMDE_ENABLE_NATIVE_ALIASES -IFDS`.

So the answer to "verify FDS can be linked without dragging in the software rasterizer and SDL
display path" is **yes, measured**. **No FDS refactor is proposed.** The fallback the brief asked
me to evaluate — a scene-dump tool plus a standalone loader — is *rejected*: it would add a
serialisation format to keep in sync, and it would destroy the property that makes this
instrument worth building (the input is provably the same bytes through the same code).

### 2.3 What the standalone load actually yields (MEASURED census, `CurFrame = 5743`)

```
LoadFLD ok=1  3.5 ms      StartFrame=0 EndFrame=2400
Animate_Objects(frame=5743)  0.01 ms
```

Geometry — 10 TriMeshes, **5,936 verts, 8,952 faces**:

| Object | Verts | Faces |
|---|--:|--:|
| `Piramid.lwo` (the room) | 3,704 | 5,532 |
| `Hull.lwo` (mech body) | 1,500 | 2,400 |
| `Hull2.lwo` | 64 | 72 |
| `L_leg1` / `R_leg1` | 146 each | 186 each |
| `L_leg2` / `R_leg2` | 188 each | 288 each |

Non-mesh objects present: `mech null`, `Camera Target`, `DiscoBall`, 3× `mech flare`, 7× `Light`.

Lights — **10 `Light_Omni`, 0 spots, 0 with `Omni_CastsShadow` set in the FLD**:

| # | position | colour (R,G,B) | `ISize` | `IRange` |
|--:|---|---|--:|--:|
| 1 | (−3.36, 3.79, 0.05) | 255,255,0 | 1.000 | 3 |
| 2 | (3.37, 3.79, 0.05) | 255,255,0 | 1.000 | 3 |
| 3 | (−13.10, 4.73, −21.57) | 255,255,0 | 0.500 | 10 |
| 4 | (13.10, 4.73, −21.57) | 255,255,0 | 0.500 | 10 |
| 5 | (−11.89, 3.41, −51.33) | 255,255,0 | 1.000 | 7 |
| 6 | (33.50, 10.85, −49.82) | 255,255,0 | 0.500 | 20 |
| 7 | (33.51, 10.89, −75.46) | 255,255,0 | 0.500 | 20 |
| 8–10 | NaN at this frame (mech-parented) | 0,128,255 | 0.500 | 2 |

**That is where the "7 omnis" figure comes from** — 7 have finite world positions from the FLD;
the other 3 are parented to the mech and only resolve once the hierarchy/driver pass runs.

Materials — **26 material bindings over the faces, 11 distinct textures**, all 256×256 24 bpp:
`PELLOW`, `P_TEXT`, `MARB4`, `PMETALL`, `PLIGHS`, `MARB1`, `PBRK34`, `PSILING`, `P_PAVE`,
`MECH_HUL`, `MECH_COK`. Three material bindings are untextured (base colour only). Per-material
`Luminosity` / `Diffuse` / `Specular` / `Glossiness` are populated and range widely
(e.g. `MARB4` lum 0.36 dif 0.10 spec 0.05 gloss 48; `MECH_HUL` dif 1.0 spec 0.40 gloss 48;
`PLIGHS` emitters lum 1.0–2.25).

World extent of the room: X[−13.6 .. 49.4], Y[0 .. 18.5], Z[−75.9 .. 4.9]. Consistent with
`GRAPHICS_PIPELINE.md` §5's "view-Z ≈ 5..80 units" warning — scale constants must be sized to
this, not to thousands.

### 2.4 Two ingestion findings that materially simplify the GPU side

**(a) `LoadFLD` does NOT decode texture pixels (MEASURED).** After `LoadFLD`, every
`Material->Txtr` has `Data == nullptr`, `numMipmaps == 0`, `blockSizeX/Y == 0×0`; only
`FileName`, `SizeX`, `SizeY`, `BPP` are set. Pixel decode is a separate `Load_Texture(Texture*)`
call, and the block-tile swizzle + mip chain is a *further* `Generate_Mipmaps(...)` call.

This is a gift. `ENGINE.md` warns at length that engine textures live in a 4×4-block **swizzled**
layout that the rasterizers require — which would have meant de-swizzling before upload. Instead:
**call `Load_Texture` and stop.** `Data` is then linear row-major, uploads straight into an
`MTLTexture`, and `generateMipmapsForTexture:` on a blit encoder gives GPU-native mips. The
block-tiled representation is never touched.

**(b) Per-face UVs are populated and must be the ones used (MEASURED).** `Face::U1..V3` are
filled by `LoadFLD` (5,440 of 5,532 `Piramid` faces non-zero). `ENGINE.md` is explicit that the
per-*vertex* `U/V` are clobbered on corners shared between faces of different projection
orientation, and that all geometric derivation must read the per-face values. Consequence for the
GPU: **the vertex buffer must be de-indexed** — 3 unshared vertices per triangle, each carrying
its face's UV. 8,952 faces → 26,856 vertices. That is trivially small and removes the whole
class of shared-corner UV bugs.

Measured UV span on `Piramid`: U[−561 .. 38], V[−40 .. 562]. Heavy tiling → **`MTLSamplerAddressModeRepeat`
is mandatory**, and the wrap must happen in the sampler, not in the shader.

### 2.5 Camera and object transforms come from FDS too

- `Kick_Camera(&source, &look, roll, Mat)` and `CalcPersp(Camera*)` are FDS functions
  (`FDS/CAMERAS/CAMERAS.CPP`). `CalcPersp` derives `PerspX/PerspY` from `IFOV` and the
  `XRes/YRes/CntrX/AspectRatio` globals. So the GPU app builds its view and projection **with the
  engine's own code**, not a re-derivation — closing the largest source of "are we even looking at
  the same thing" doubt.
- `Animate_Objects(Scene*, Camera*)` runs standalone (MEASURED 0.01 ms) and fills per-mesh
  `IPos` / `IScale` / `RotMat` from the global `CurFrame`.
- Review poses use the same mechanism `DEMO` does: `FDS_GREETS_CAM="px,py,pz,fx,fy,fz"` →
  `ISource = p`, `look = p + f`, `Kick_Camera`, `IFOV` from the FOV spline's first key with a 75°
  fallback (`DEMO/GREETS.CPP` ~2963–3000). The GPU tool reads the same env var and calls the same
  functions.

### 2.6 The one big honesty item: `LoadFLD` gives the **authored** greets, not the **shipped** greets

Everything `Initialize_Greets` does lives in `DEMO/GREETS.CPP` and is **not** reachable from FDS:

- `Piramid` material clustering into ~600 per-plane shadow groups
- the `Piramid` spatial chunk split (`--greets-piramid-chunk-grid=8` → up to 512 sub-meshes; the
  original is retired by zeroing `FIndex`)
- the `stone_shadow_proxy` mesh
- the robot spotlight + 3 orbit spotlights (`makeSpotLight`, all `Omni_CastsShadow`)
- mirrors (`GreetsMirror`), the disco ball, blaster bolts, the text wobbler
- displacement shell rebuilds, PBR sidecar imports, `NZP=0.01` / `FZP=150`,
  `Ambient_Factor=0.25` / `Diffusive_Factor=1.0` / `Specular_Factor=1.0`

**This is the single most important caveat in the document**, because it defines what the
benchmark actually compares. Handling, per item:

- **Reproduce (cheap, and required for meaning):** the 7 FLD omnis; the ambient/diffuse/specular
  factors; NZP/FZP; per-material Luminosity/Diffuse/Specular/Glossiness. These are literal
  constants read out of `GREETS.CPP` and pasted into the tool's config.
- **Deliberately do not reproduce:** the chunk split and the shadow-group clustering. Both exist
  to help a *CPU* mesh-level frustum cull and a *CPU* shadow-identity test. A GPU has neither
  problem. Not reproducing them is not cheating — **but it means the GPU issues ~10 draws where
  the CPU walks ~250 meshes, and that must be stated with every number** (§5).
- **Exclude from both sides:** the spotlights, mirrors, disco, bolts, text. See §4.

---

## 3. What gets reimplemented, for parity of MEANING

Our CPU G-buffer's packing exists to dodge costs a GPU doesn't have. Reproducing the *packing*
would be reproducing the *workaround*. So: reproduce what each field **means**.

| Our CPU representation | GPU analogue | Why the substitution is meaning-preserving |
|---|---|---|
| `txtr` word: `mip:4 \| matID:8 \| swizzledUV:20` — defers the texel fetch to the lighting pass to save a G-buffer channel | **Sample albedo in the G-buffer pass**, write `rgba8unorm` | The packed word is a bandwidth trick for a CPU with no texture units. On the GPU the texture unit is free and the fetch belongs in the geometry pass. |
| `normal`: octahedral-packed u16, view space | `rg16snorm` oct-encoded view normal (keep oct — it's cheap and identical in meaning) | Same encoding, same space. |
| `tangent`: oct u16 | `rg16snorm`, only when normal maps are on | Same. |
| depth: global `ZPage16`, `0xFF80 − round(g_zscale·z)` | `depth32float` attachment | GPU depth is *more* precise; noted as an asymmetry in §5. |
| `shadowMatID` u16 / `mirrorId` u8 | **not implemented** | Identity hacks for a CPU shadow test with no hardware depth compare. |
| `lightmapMF`/`lightmapST` (static shadow lightmaps) | **not implemented** | An amortisation of expensive CPU shadow taps. The GPU's whole point is that the tap is cheap. |
| — | new: `rgba8unorm` material params (diffuse, specular, glossiness, flags) | The CPU kernel reads these from `Material*` via matID; the GPU writes them per pixel. |

**Lights.** 7 omnis, point, intensity `L × ISize`, hard cutoff at `IRange` (with `rRange`
reciprocal). Implement as **one full-screen lighting pass looping 7 lights** — structurally the
closest analogue to our tiled kernel. Our CPU path additionally builds *per-tile* light lists
(`DeferredLightLists.cpp`); at 7 lights a GPU tile/cluster pass is not worth writing, and this
difference favours neither side meaningfully. Say so rather than silently omitting it.

**Cube shadow maps.** 7 lights × 6 faces × 256² (matching `--greets-omni-shadow-res` default),
depth-only, as a `MTLTextureTypeCube` depth texture per light, sampled with hardware
`sample_compare` + PCF. Optional 512² arm since our `--greets-omni-shadow-res` accepts it.

**HDR + tonemap.** `rgba16float` HDR target, exposure → ACES → 8-bit, one fragment pass. greets
defaults to HDR (`Hdr_ActivateNoFog` fires because greets has no fog), so keeping it is parity,
not extra.

**Displacement (Phase 3).** Three arms, all fragment/tessellation-stage:
1. **flat POM** — per-fragment height-field march in the fragment shader, no silhouette.
2. **shell / prism with real silhouettes** — extruded prisms, per-fragment march with lateral-exit
   discard and **depth write**. This is the arm the S1 campaign has been unable to make work on
   the CPU; on the GPU `discard_fragment()` and `[[depth(any)]]` are first-class.
3. **hardware tessellation** — displace real geometry. MEASURED: tessellation vertex functions
   compile on this machine; max rate 16.

Which arms to build, and in what order, should be settled **after** reading the concurrent
displacement-literature verdict (`docs/DISPLACEMENT_RESEARCH_II.md` — not present in the tree as
of this writing).

### What is explicitly NOT implemented, and why that's fine for a benchmark

| Excluded | Why |
|---|---|
| **Mirrors / planar RTT** (`GreetsMirror.cpp`) | A whole-room mesh-clone + recursion system. It's a *scene-authoring* feature, not a renderer-cost question, and another agent owns that file. **Because it duplicates an entire room, the CPU baseline must also be captured with mirrors off, or the comparison is invalid.** |
| **Volumetric cones / god-rays / froxel fog** | Ray-march passes whose cost is a *tuning dial* (`cone_strength`, `halo_strength`, froxel res). Including them makes the headline number a function of a knob. Excluded on both sides. |
| **SSAO / GTAO** | Same argument — it has a whole downscale ladder (`--ssao_downscale 1\|2\|4`). Excluded on both sides. A GPU GTAO arm is a well-understood optional extension later. |
| **Transparent depth-peel layers** | `PERF_STATE.md` records greets' xpar contribution as "small"; it doubles the G-buffer plumbing for little benchmark value. |
| **Spotlights (robot + 3 orbit)** | Installed by `GREETS.CPP`, not the FLD, and each is a shadow caster — they'd change the light count on one side only. Excluded on both sides; the FLD's 7 omnis are the light set. |
| **Sprites, particles, TBR, disco ball, blaster bolts, text wobbler** | Content, not renderer cost. |
| **Mod player, demo director, SDL event loop, resize coordination** | Irrelevant to a frame-cost question. |
| **Mipmap-via-subdivision clipper** | **Cannot and must not be reimplemented** — it *is* the CPU's substitute for a texture unit. See §5, item 1: this is the single largest apples-to-oranges item and it gets its own treatment rather than being hidden. |

---

## 4. Fixed experimental conditions (identical on both sides)

| Knob | Value |
|---|---|
| Resolution | 1920 × 1080, exactly |
| Primary pose | `t=5743`, `FDS_GREETS_CAM="9.07557869,3.19592357,-52.9277191,-0.20672597,-0.140846997,0.968207836"` |
| Secondary poses | `t=6097` (corner / light-bleed), `t=2845` (grazing close-up), `t=6133` (mirror panel), `t=5958` (grazing smear) — from `docs/greets_review_poses.txt` |
| Camera construction | `Kick_Camera` + `CalcPersp` from FDS, same `XRes/YRes/AspectRatio` globals |
| Clip planes | NZP 0.01, FZP 150 (`GREETS.CPP:1477-8`) |
| Lights | the 7 FLD omnis, authored position / colour / `ISize` / `IRange` |
| Global light factors | `Ambient_Factor` 0.25, `Diffusive_Factor` 1.0, `Specular_Factor` 1.0 |
| Materials | per-material Luminosity / Diffuse / Specular / Glossiness from the FLD |
| Textures | the 11 256² albedos, decoded by FDS's own `Load_Texture` |
| Effects ON | deferred, HDR + ACES tonemap, cube shadows (stage 3+) |
| Effects OFF (both sides) | mirrors, volumetrics, fog, SSAO, xpar peel, sprites, spotlights |
| Shadows | 7 lights × 6 faces × 256² |
| MSAA | **1× for the headline number.** 4× reported separately, labelled "what you'd ship" |

---

## 5. Comparison methodology — the actual deliverable

### 5.1 What is measured, on each side

**CPU side — never wall-clock.** This session alone saw whole-frame numbers ranging **51–250
ms/iter at machine load 15–20**. A wall-clock comparison against that is noise dressed as data.
Use the per-phase counters:

- `--xfrm_prof` — main-view `Transform_Objects` broken into SETUP / VERT / SOA / FACE / OTHER,
  plus meshes / verts / facesTested / facesPushed per frame. Instrument cost ≈ 5 clock reads per
  surviving mesh (~30 µs/frame at greets' ~250 meshes); sub-100-µs deltas are instrument noise.
- `--xfrm_pass_prof` — per-pass census: MAIN / MIRROR-RTT / SHADOW / OFFSCREEN. **Requires a
  census build (`cmake -DFDS_VIS_CENSUS=ON`)** — it is compiled out of the shipping build because
  merely carrying its never-taken branches inside `Transform_Objects` moved pixels under
  `-ffp-contract=fast`.
- `--xfrm_pass_mesh_prof` — per-mesh / per-material decomposition of the above.
- `--bench=scene@iters=N,t=…,xres=…,yres=…` for the frame-level mean, **with load recorded**.

Every CPU number in the final report must carry: build config, exact flag set, `uptime` load
average, iteration count, and whether it is *frame-ms* or *core-ms*.

**GPU side.**
- Whole frame: `MTLCommandBuffer.GPUStartTime` / `GPUEndTime`.
- Per pass: `MTLCounterSampleBuffer` with the `timestamp` counter set at **stage boundaries**
  (MEASURED supported). Per-**draw** boundaries are **NOT** supported on this device (MEASURED
  `supportsCounterSampling(AtDrawBoundary) == NO`), so per-encoder is the finest available
  granularity — which happens to match "per pass", the granularity we want.
- Report **median of ≥200 frames plus p5/p95**, after **≥60 warmup frames** — Apple GPUs clock
  up, and a cold-start mean is a lie.
- Record whether the app is rendering offscreen or to a connected display, and exclude present /
  vsync from the timed region.

**Both sides: median and spread, never a bare mean. Interleave A/B runs** — don't run all-CPU
then all-GPU, or the thermal state becomes a hidden variable.

### 5.2 What makes the comparison FAIR

- **Identical scene bytes** — same `GREETS.FLD`, same LWO geometry, same 11 textures, decoded by
  the *same code* (`LoadFLD` + `Load_Texture` from `libFDS.a`).
- **Identical camera, projection, lights, and material constants**, computed by the same FDS
  functions rather than re-derived.
- **Identical output resolution** and **identical set of enabled effects**.
- **Same machine, interleaved, same thermal state.**
- **Both time render only** — load excluded, present/vsync excluded.
- **Same shadow-caster count at the same shadow resolution.**

### 5.3 What makes it apples-to-oranges — state these WITH every number

1. **Texture filtering.** CPU: point-sampled, with the mip level chosen by **polygon
   subdivision** — `MiplevelClipper` recursively splits a triangle along 1/z midpoints until each
   piece is single-mip. GPU: hardware trilinear (+ optional 16× aniso), free. **The CPU pays
   *geometry* work for what the GPU gets from a sampler**, and that cost does not appear in the
   face count. This is the largest structural asymmetry in the whole exercise.
2. **MSAA.** 4× MSAA is near-free in Apple tile memory; the CPU path has no MSAA at all (it has
   `DeferredEdgeAA` instead). Headline at 1×; 4× reported separately.
3. **Shadow filtering.** GPU `sample_compare` + hardware PCF vs the CPU's hand-rolled taps —
   `PERF_STATE.md` records `shadow_polyid_no_pcf` (single nearest tap instead of 4) as
   **~9 ms saved on greets**. Free vs 9 ms is not a small footnote.
4. **Tonemap / HDR.** A full-screen ACES pass is a rounding error on the GPU and multi-ms on the
   CPU.
5. **Memory architecture.** On Apple silicon a GPU G-buffer lives in **tile memory** for the
   duration of a render pass; our CPU G-buffer is DRAM-resident and re-read by the lighting pass.
   Our own `RENDER_DAG_SCOPING.md` names a tile-resident fused pass as the architectural win we
   *want* — **the GPU gets that for free.** So part of the gap is not "the GPU is faster at math",
   it is "the GPU has the memory architecture on our roadmap".
6. **Precision.** CPU: 16-bit Z, 8-bit saturation in the LDR path, oct-u16 normals, 12-bit `rcp`
   approximations in several kernels. GPU: f32 depth, f16 HDR, f32 math. **The GPU is doing
   strictly more accurate work**, so any "GPU is N× faster" figure *understates* the gap.
7. **Threading and attribution — the most dangerous one.** Our SHADOW numbers are **core-ms
   summed across workers**, not elapsed. GPU timestamps are elapsed. Comparing 10.67 core-ms to a
   GPU elapsed number is a category error. Every such comparison must state the conversion
   (divide by effective worker count, or compare against elapsed frame time) explicitly.
8. **Work the GPU has no analogue for.** `Transform_Objects`' per-vertex visibility flags,
   backface culling, bounding-sphere culling, the radix sort, the clipper's n-gon extension — the
   GPU does clipping and culling in fixed-function silicon. A "front end" comparison is really
   *software emulation of fixed-function hardware vs the hardware*.
9. **Draw-call / mesh partition.** The CPU runs its production chunking (~250 meshes, up to 512
   `Piramid` chunks) because that is its *best* configuration; the GPU draws ~10 meshes. This cuts
   both ways — better culling for the CPU, fewer draws for the GPU — and must be reported, not
   normalised away. Recommendation: run the CPU in its best configuration and state the
   difference, rather than crippling it with `chunk-grid=0` to "match".
10. **Machine load.** Recorded for every CPU number; this is precisely why per-phase counters are
    used instead of wall clock.

### 5.4 The headline number, and why it's that one

The retired-mesh fix (`799c808`) removed an orphan `Piramid.lwo` with zero faces that every pass
was transforming — **66.3–69.5 % of shadow-pass verts, up to 72 % of main-view verts**. Post-fix
(MEASURED, reported by the coordinator): main-view `Transform_Objects` **0.567 → 0.424 ms**
(−25.2 %), SHADOW front end **23.07 → 10.67 core-ms**, OFFSCREEN 2.12 → 1.18. Front end is now
**~13.5 core-ms/frame total**, and the frame is **deferred-lighting-bound, not geometry-bound**.

So the interesting comparison is **the lighting pass**: 1920×1080 × 7 omnis × cube shadows.
Report a per-stage table, each row with its own ratio and its own caveat references:

| Stage | GPU (median ms, p5/p95) | CPU (per-phase counter, frame-ms or core-ms, load stated) | Ratio | Caveats |
|---|---|---|---|---|
| Geometry front end / G-buffer fill | | | | §5.3 items 1, 8, 9 |
| Shadow bake (7×6×256²) | | | | §5.3 items 3, 7 |
| Deferred lighting | | | | §5.3 items 5, 6 |
| Tonemap | | | | §5.3 item 4 |
| **Whole frame** | | | | all |

**Anti-goals.** No single "GPU is N× faster" headline without the stage breakdown. No comparison
of GPU elapsed against CPU core-ms. No wall-clock CPU numbers. No claim that a stage is
"negligible" — the project's standing rule is that single-digit ms matter.

---

## 6. Staged effort estimate

Estimates are **ESTIMATE** unless a stage is marked as already measured.

| Stage | Work | Effort | What it buys |
|---|---|--:|---|
| **0. Scaffolding** | `option(FDS_GPU_BENCH … OFF)` + new target; `.mm`; Metal device; runtime MSL compile; link `libFDS.a` | ~½ day, **largely de-risked already** (FDS standalone link and runtime MSL compile both MEASURED) | Proof the approach works at all |
| **1. Phase 2 spike** | greets geometry via `LoadFLD`, albedo via `Load_Texture`, de-indexed triangles with per-face UVs, one review pose, GPU frame timing, offscreen preferred | ~1 day | **The first real number**: this exact geometry, textured, on the GPU. Answers the vertex/raster half of the question. |
| **2. Deferred G-buffer + 7 omnis, no shadows** | MRT G-buffer, full-screen lighting pass | ~1–1.5 days | The lighting comparison without the shadow confound. **Highest value per day, because the frame is lighting-bound.** |
| **3. Cube shadow maps** | 7 lights × 6 faces × 256², `sample_compare` + PCF | ~1.5–2 days | The full greets-frame comparison, and makes the PCF asymmetry concrete |
| **4. HDR + ACES tonemap** | `rgba16float` target + tonemap pass | ~½ day | Parity with greets' default look; makes screenshots visually comparable |
| **5. Displacement arms** | flat POM (~1 d), shell/prism with silhouettes + depth write (~2–3 d), hardware tessellation (~2 d) | ~5–6 days | **The strategic prize**: a visual ground truth for the S1 campaign, and a direct answer to whether silhouette-correct per-pixel displacement works, on hardware where a fragment shader can `discard` and write depth at full rate |
| **6. (optional) Snapshot output** | offscreen PPM/PNG matching our snapshot naming | ~½ day | Cross-renderer image diffs with existing tooling |

**~3 days to the first meaningful lighting number. ~2 weeks to the strategic answer.**

### Risks

- **Objective-C++ enters the tree.** Contained to the new target.
- **No Instruments** (no Xcode). Mitigated by in-app timestamps; `.gputrace` written for later.
- **Apple GPU clock ramp** will make naive first measurements too slow. Mitigated by warmup +
  median + p5/p95.
- **The authored-vs-shipped greets gap (§2.6)** is the biggest threat to the comparison's
  *meaning*, not its mechanics. It is handled by exclusion-on-both-sides plus explicit statement,
  and it is the thing to re-check before any number is quoted.
- **`FDS_VIS_CENSUS=ON` is a separate build.** The CPU per-pass numbers require it; the shipping
  build cannot produce them. Budget a second build tree.

---

## 7. Build integration (constraint: invisible by default)

```cmake
# top-level CMakeLists.txt
option(FDS_GPU_BENCH "Build the standalone Metal deferred benchmark (macOS only)" OFF)
if(FDS_GPU_BENCH AND APPLE AND NOT EMSCRIPTEN)
    add_subdirectory(GpuBench)
endif()
```

- Default **OFF**. `cmake --build build` must stay byte-for-byte the same set of compile commands.
- The new target links `FDS` only (plus `Metal`, `QuartzCore`, `Foundation`, and `AppKit` or SDL2
  for the window). It does **not** link `Modplayer` and does **not** compile any `DEMO/` source.
- Shaders are `.metal` text files copied next to the binary, compiled at runtime.
- Run from `Runtime/` like everything else (asset paths are CWD-relative).
- Gate verification required after any CMake edit: `cmake --build build` unchanged, and
  `tools/render_gate.sh` 3/3.
- Offscreen rendering is the default. **A visible window is only opened on explicit request** —
  per the standing rule that visible runs are the user's to launch.

---

## 8. Open questions to settle before Phase 3

1. Which displacement arms, in what order — pending `docs/DISPLACEMENT_RESEARCH_II.md`.
2. Shadow resolution arm: 256² (our default) only, or also 512²/1024² to show the GPU's
   scaling where the CPU's is prohibitive?
3. Should the GPU path also render the 3 mech-parented omnis (needs the `GREETS.CPP` hierarchy
   logic replicated), or stay at 7 and state it?
4. Is a GPU **forward** arm worth ~½ day, to isolate "deferred vs forward" separately from
   "CPU vs GPU"?
