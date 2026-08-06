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
the other 3 are the `mech flare` lights, parented to `Hull.lwo` (VERIFIED in the FLD's parent
records), so they only resolve once the hierarchy/driver pass runs.

But note: **greets marks all 10 as shadow casters.** `GreetsApplyInitDefaults`
(`GREETS.CPP:1046-1053`, VERIFIED by reading) sets `greets_omni_shadows = true`,
`greets_omni_shadow_res = 512`, `greets_moving_omni_shadow_res = 128`, and
`greets_omni_default_range = 30`. So the shipped scene bakes **7 static cube maps at 512² and 3
moving cube maps at 128²**, not "7 × 256²". §3 and §4 use these numbers.

Materials — **26 material bindings over the faces**. Two distinct texture mechanisms:

- **19 materials use a single legacy diffuse JPG** named in the FLD. MEASURED via the standalone
  loader: 11 distinct such textures, all 256×256 24 bpp — `PELLOW`, `P_TEXT`, `MARB4`, `PMETALL`,
  `PLIGHS`, `MARB1`, `PBRK34`, `PSILING`, `P_PAVE`, `MECH_HUL`, `MECH_COK`. Three material
  bindings are untextured (base colour only).
- **7 materials carry a full PBR map-set** via the FLD's `RevMapMask` field
  (`FDS/FLD/FLD_READ.H:73-82`), resolving to `Runtime/TEXTURES/PBR/<set>/`:
  `momy-1`→`momy` (albedo/ao/height/metallic/normal/roughness), `momy-2`→`momy2`,
  `amudim`→`amudim`, `stairs`→`stairs` (full set), `rooms`→`wall_stone3` (no metallic),
  `screen emiter`→`screen_emiter`, `teleporter`→`teleporter` (height + normal only).

Per-material `Luminosity` / `Diffuse` / `Specular` / `Glossiness` are populated and range widely
(MEASURED: `MARB4` lum 0.36 dif 0.10 spec 0.05 gloss 48; `MECH_HUL` dif 1.0 spec 0.40 gloss 48;
`PLIGHS` emitters lum 1.0–2.25).

Parallax: only **`rooms`** has both a non-zero authored `ParallaxScale` (0.10) and a height map.
`stairs` and `siling` have the field present but zero. `--parallax` and `--parallax_pom` both
default ON, so `rooms` gets a real 8-step POM march with no flags passed.

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
- the `Piramid` spatial chunk split (`--greets-piramid-chunk-grid=8` → ~50–150 non-empty cells in
  practice; the original is retired by zeroing `FIndex`)
- the `stone_shadow_proxy` mesh
- the robot spotlight + 4 orbit spotlights (`makeSpotLight`, all `Omni_CastsShadow`) — **but
  `no_greets_spots` defaults to `true` for greets (`GREETS.CPP:1047`, VERIFIED), so these are OFF
  in the default run.** Excluding them from the benchmark is therefore *parity*, not a
  simplification.
- 10 disco cone spotlights + 1 glow clone (`GreetsDisco.cpp`), 256² shadow maps,
  `Omni_ForceVolCone`
- a 12-slot mirror "bounce pool", plus mirror **omni clones** (each mirror clones every
  not-yet-cloned omni across its plane) and mirror **mesh clones** (the whole non-wall scene as
  one TriMesh per mirror)
- the disco ball (procedural 10×14 UV sphere), glass shatter shards, blaster bolts, text wobbler
- displacement shell rebuilds, PBR sidecar imports, `NZP=0.01` / `FZP=150`,
  `Ambient_Factor=0.25` / `Diffusive_Factor=1.0` / `Specular_Factor=1.0`
- **`--greets_stone_tex` (default ON)** — `GREETS.CPP:1489-1591` **replaces** the `rooms` and
  `floor` materials' albedo + height + normal + roughness with a *different* sidecar set loaded
  by filename (`Runtime/TEXTURES/greets_wall{,_h,_n,_r}.png`,
  `greets_floor{,_h,_n,_r}.png`), bypassing the `RevMapMask` PBR-set mechanism entirely, and
  code-forces `floor`'s parallax to 0.25. **This is the sharpest instance of the caveat: the wall
  surface the user actually reviews is not the wall surface `LoadFLD` hands me.** Any displacement
  arm (Phase 3) must load these files explicitly — they are the subject of the whole S1 campaign.

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

**Lights.** 7 static omnis (+3 mech-parented), point, intensity `L × ISize`, **hard radius cutoff
at `IRange`** — *not* inverse-square; `rRange = 1/IRange`. Implement as **one full-screen lighting
pass looping the lights** — structurally the closest analogue to our tiled kernel. Our CPU path
additionally builds *per-tile* light lists (`DeferredLightLists.cpp`); at ~10 lights a GPU
tile/cluster pass is not worth writing, and this difference favours neither side meaningfully.
Say so rather than silently omitting it.

**The shading model must be PBR, not Blinn-Phong.** VERIFIED in `GreetsApplyRunDefaults`
(`GREETS.CPP:1107-1114`): greets sets `pbr`, `env_brdf_analytic`, `pbr_multiscatter`,
`diffuse_energy`, and `sh_ambient` all `true`. So the CPU kernel greets actually runs is
Cook-Torrance (GGX D + Smith-Schlick G + Schlick F), with the Karis split-sum analytic env BRDF,
Fdez-Aguera multiscatter energy compensation, `(1−F)` diffuse energy weighting, and **L2 SH
irradiance** in place of a flat ambient constant. All five are standard, well-documented GPU
shading — so matching them is *less* work on the GPU than on the CPU, but the spec has to say so
or the GPU arm would be measuring a cheaper shader. Also on by default: `hdr`, `hdr_linear`,
`bloom` (`bloom_intensity` 2.0), `hdr_refl_gain` 4.0, `hdr_exposure = cine::kGreetsExposure`.

**Checkerboard half-rate shading (`deferred_checkerboard = true`, VERIFIED `GREETS.CPP` run
defaults).** The CPU lighting kernel shades **half the pixels** on greets by default. This is a
first-order fairness item, not a footnote — see §5.3 item 11. Decide explicitly: either the GPU
also shades checkerboard (faithful but odd), or the GPU runs full-rate and every ratio states
that the CPU number is at half shading rate. **Recommendation: GPU full-rate, and report the CPU
number both with and without `--no-deferred_checkerboard`** so the reader can see the dial.

**Cube shadow maps.** **7 static lights × 6 faces × 512²** plus **3 moving lights × 6 faces ×
128²** (VERIFIED defaults), depth-only, one `MTLTextureTypeCube` depth texture per light, sampled
with hardware `sample_compare` + PCF.

Two CPU-side shadow mechanisms are deliberately **not** reproduced, both being amortisations of an
expensive CPU tap:
- **PolyId comparison** (`FDS_SHADOW_POLYID_DEFAULT_ON=1`) — an identity test against a baked
  material ID per texel, instead of a biased depth compare. It exists because a correct depth
  compare with bias was too expensive/leaky on the CPU. The GPU's `sample_compare` is the
  hardware primitive that makes the whole trick unnecessary.
- **Static shadow lightmaps** (`shadow_lightmap = true`, `shadow_lightmap_res = 128`,
  `shadow_lightmap_planar = true`) — a pre-baked per-face atlas of (texel, static-omni) → shadow
  byte, so the kernel can *skip* the per-pixel cube tap on static surfaces lit by static omnis.
  The GPU takes the tap.

**Both omissions make the GPU do strictly more work per pixel than the CPU does**, which is the
right direction for an honest comparison — but it must be stated, because a naive reader would
assume the reverse.

**HDR + tonemap.** `rgba16float` HDR target, exposure → ACES → 8-bit, one fragment pass. greets
defaults to HDR (`Hdr_ActivateNoFog` fires because greets has no fog), so keeping it is parity,
not extra.

**Displacement (Phase 3).** Silhouette-correct displacement is an **OPEN question that the user
believes is answered YES, on evidence he has seen**, and this plan treats it that way.

> An earlier revision of this section withdrew the "silhouette displacement is the prize" framing
> in favour of `DISPLACEMENT_RESEARCH_II.md` §5's R5 ("recess-only, no silhouette program").
> **That withdrawal is itself retracted.** The user rejected R5, and his grounds are empirical, not
> aesthetic: the geometric **mesh-displacement (tessellation) arm already demonstrated the
> protrusion look he wants**, and he has seen genuine see-through between stones and judged it
> good. The method fails in *some* cases, and finding out why is the job — not concluding the
> effect is unavailable.
>
> The "0–24 px see-through at 12 of 13 poses" figure that R5 rests on **does not measure what he
> observes.** Its threshold requires a surface >3 world units behind the wall — a distant-background
> criterion. What you actually see through a gap between stones is the adjacent wall or the mortar
> bed, a small fraction of that distance away, which that instrument scores as no see-through. The
> research agent has been asked to re-derive it. Until then treat the figure as not-yet-valid,
> **not** as a negative result.

GPU arms, in the priority order the green-light specifies:

1. **Hardware tessellation — the instrument for the H1/H2 discriminator.** The grazing "swim" is an
   *unattributed* symptom: either the march is wrong (H1), or the motion is physically correct and
   reads as sliding because it is painted on a flat polygon (H2). `DISPLACEMENT_RESEARCH_II.md` §4
   states the discriminator has never been run and that it requires an arm which **moves real
   geometry** through the identical camera path, measured on the same surface-registered metric.
   That is what hardware tessellation is, and the GPU is the only place we can build it cheaply.
   **This is the single highest-value item in the GPU plan.** MEASURED: `[[patch(quad,4)]]`
   tessellation vertex functions compile on this machine; max tessellation rate 16.
2. **Prism / closed-shell arm.** The dismissal of this family was wrong. On **flat quads** the
   prism collapses the expensive half (tetrahedralisation, rippling, Coons patches) while keeping
   the cheap half that actually buys silhouettes: rasterise the extruded box, bound the ray to its
   interior, discard on exit, let the Z-buffer arbitrate between overlapping prisms. Hirche's own
   blocking restriction — **prism side faces must be planar** — is exactly satisfied by our flat
   walls. Estimated ~1,800 faces against the tessellation carve's 86,600. All three mechanisms it
   needs are native on a GPU (`discard_fragment()`, `[[depth(any)]]`, per-fragment depth
   arbitration) and are precisely what the CPU's `--pom_shell_side_entry` can only approximate.
   **A GPU implementation answers in days what has cost a week, and yields a visual ground truth to
   port against.**
3. **Correctly filtered wall as ground truth.** `--texture_filter` defaults to 0, so the CPU
   deferred kernel point-samples at texel granularity; one height texel covers ~15 screen pixels at
   these poses, drawing the wall as ~4 px blocks that crawl under motion — present in flat POM and
   `--no-parallax` alike. **This may account for part of what is being called swim.** The GPU gets
   trilinear + aniso for free, so it can show that same wall correctly filtered — something the CPU
   path structurally cannot do.
4. **Flat POM** as the cheap baseline arm, for A/B against 1-3.

`MTL4MeshRenderPipeline` is in this SDK if mesh shaders are ever wanted.

### 3.1 Fidelity note — the `rooms` / `rooms::mirUV` boundary, and why the GPU arm may not show it

From the user's own `--poly_viz` capture: the CPU failures **cluster on the `rooms` /
`rooms::mirUV` boundary**, where three things coincide — UV handedness flips, the material changes
(the handedness split *clones* the material), and the patch domain ends (the shell's union-find
groups per material).

The GPU arm takes per-face UVs straight from FDS and **does not inherit the material split**, so it
may simply not exhibit this failure. **If that is observed it is a strong result and must be stated
explicitly**, because it localises the CPU defect to the handedness/material-clone split rather
than to displacement itself. It also means the GPU arm is *not* automatically a like-for-like
reproduction at that boundary — check it before concluding anything either way.

### 3.2 The `--greets_stone_tex` gate — DECISION: replicate the override in GpuBench

§2.6 established that `--greets_stone_tex` (default ON) replaces the `rooms` and `floor`
albedo/height/normal/roughness with `Runtime/TEXTURES/greets_*` sidecars, DEMO-side, so the wall the
user reviews is not the wall `LoadFLD` hands us. **No displacement arm may run on the authored
wall.** Of the two options:

**CHOSEN: replicate the override inside `GpuBench/SceneIngest.cpp`.** Reading
`DEMO/GREETS.CPP:1508-1600` shows the mechanism is a *filename repoint performed before textures are
loaded* — `M->Txtr->FileName = strdup(albedo); M->Txtr->BPP = 0;` — plus three sibling maps loaded
onto `M->HeightMap` / `M->NormalMap` / `M->RoughnessMap`. Since GpuBench controls when
`Load_Texture` runs, this is a per-material-name repoint of a handful of lines keyed on
`Material::Name` (`"rooms"` -> `greets_wall*.png`, `"floor"` -> `greets_floor*.png`, floor
`ParallaxScale` 0.25). Two details that must not be lost: the albedo is **RGBA with baked AO in
alpha** (`Mat_AoInAlpha`), so the ingest must stop forcing alpha to 255 for these; and an
*authored* `ParallaxScale` (!= the 1.0 default) wins over the code default, matching `GREETS.CPP`.

**REJECTED: dump post-`Initialize_Greets` material state from DEMO.** It needs a dump path added to
`DEMO/` (which this work is scoped out of), and it reintroduces exactly the stale-artifact hazard
that got the scene-dump-tool option rejected in §2.2 — the dump would silently drift from
`GREETS.CPP` the next time the override changes.

### What is explicitly NOT implemented, and why that's fine for a benchmark

| Excluded | Why |
|---|---|
| **Mirrors / planar RTT** (`GreetsMirror.cpp`) | ≥3 mirrors, each cloning the entire non-wall scene as one TriMesh **and** cloning every omni across its plane, plus second-order `mirror_rtt` re-renders at density 1024 — all ON by default. It's a *scene-authoring* feature, not a renderer-cost question, and another agent owns that file. **Because it duplicates an entire room and multiplies the light count, the CPU baseline MUST also be captured with mirrors off, or the comparison is invalid.** |
| **Volumetric cones / god-rays / froxel fog** | Ray-march passes whose cost is a *tuning dial*. The global `--draw_cones` is OFF, but the 10 disco spots force-enable beams per-light (`Omni_ForceVolCone`), and `cone_strength` ends up at the disco ball's 1.2 rather than the scene-init 2.0. A number that depends on that ordering is not a benchmark. Excluded on both sides (disco off). |
| **SSAO / GTAO** | OFF in greets by default (VERIFIED: no `setDefault` for it anywhere in `GREETS.CPP`), and it has a whole downscale ladder. Excluding it is parity. A GPU GTAO arm is a well-understood optional extension later. |
| **Transparent depth-peel layers** | `PERF_STATE.md` records greets' xpar contribution as "small", and `xpar_pbr` is deliberately left OFF; it doubles the G-buffer plumbing for little benchmark value. |
| **Spotlights (robot + 4 orbit)** | Installed by `GREETS.CPP`, not the FLD — **and `no_greets_spots` defaults to `true`, so they are already off in the default run.** Excluding them is parity. |
| **Disco (10 cone spots + glow clone + procedural ball)** | Adds 11 shadow-casting lights and a runtime-built mesh, and mutates `cone_strength` on first tick. Excluded on both sides. |
| **Sprites, particles, TBR, glass shards, blaster bolts, text wobbler** | Content, not renderer cost. |
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
| Lights | the 7 static FLD omnis (+3 mech-parented in later stages), authored position / colour / `ISize` / `IRange`, **hard radius cutoff** |
| Global light factors | `Ambient_Factor` 0.25, `Diffusive_Factor` 1.0, `Specular_Factor` 1.0 |
| Shading model | PBR: GGX + Smith-Schlick + Schlick F, Karis split-sum env BRDF, Fdez-Aguera multiscatter, `(1−F)` diffuse energy, L2 SH ambient |
| Materials | per-material Luminosity / Diffuse / Specular / Glossiness from the FLD |
| Textures | the 11 legacy 256² albedos, decoded by FDS's own `Load_Texture`. The 7 `RevMapMask` PBR sets and the `greets_stone_tex` wall/floor sidecars are stage-3+ work |
| Effects ON | deferred, HDR (`hdr_linear`) + ACES tonemap, bloom 2.0, cube shadows (stage 3+) |
| Effects OFF (both sides) | mirrors + `mirror_rtt`, disco, volumetrics, fog, SSAO, xpar peel, sprites, spotlights |
| Shadows | 7 static × 6 faces × 512², 3 moving × 6 faces × 128². No PolyId trick, no static lightmap on the GPU side |
| Shading rate | GPU full-rate; CPU reported **both** at its default `deferred_checkerboard` half-rate and at `--no-deferred_checkerboard` |
| MSAA | **1× for the headline number.** 4× reported separately, labelled "what you'd ship" |
| `--deferred` | **must be passed explicitly** — it is compile-default OFF (`FeatureFlags.h:13-14`), even though every tool in this repo passes it |

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
- `--bench=scene@scene=greets,t=<timer>,iters=N[,tend=T][,xres=,yres=]` for the frame-level mean,
  **with load recorded**. It runs `Initialize_Greets` + `Greets_JoinBakeThread` + one untimed
  warm-up tick, then N timed ticks (pinned at `t`, or sweeping `t → tend`), and prints
  `[BENCH] scene=greets t=… iters=… total=… mean=…` to stderr. Run from `Runtime/`.
- The closest thing to a canonical greets invocation already in the tree is
  `tools/flip_rate.sh:64-69`:
  `--snapshot=greets@t=1588 --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --no-env_refl`.
  Useful as the starting flag set for reference captures; strip the glass/xpar flags for the
  benchmark condition set in §4.

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
11. **Shading rate.** `deferred_checkerboard` is ON for greets — **the CPU lighting kernel shades
    half the pixels.** A GPU full-rate number compared against it understates the CPU by up to 2×
    on the lighting stage. Report the CPU lighting stage both ways.
12. **Shadow amortisations the GPU declines.** The CPU skips the per-pixel cube tap entirely on
    static surfaces lit by static omnis (static shadow lightmaps, 128² planar atlas) and uses a
    PolyId identity test rather than a depth compare. The GPU takes every tap with hardware
    compare. **This asymmetry runs the opposite way from most of the others** — it makes the GPU
    do more work — and must be stated, because a reader will assume the reverse.

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
| Shadow bake (7×6×512² + 3×6×128²) | | | | §5.3 items 3, 7, 12 |
| Deferred lighting (PBR, GGX) | | | | §5.3 items 5, 6, 11, 12 |
| Tonemap + bloom | | | | §5.3 item 4 |
| **Whole frame** | | | | all |

**Anti-goals.** No single "GPU is N× faster" headline without the stage breakdown. No comparison
of GPU elapsed against CPU core-ms. No wall-clock CPU numbers. No claim that a stage is
"negligible" — the project's standing rule is that single-digit ms matter.

---

## 6. Staged effort estimate

Estimates are **ESTIMATE** unless a stage is marked as already measured.

| Stage | Work | Effort | What it buys |
|---|---|--:|---|
| **0. Scaffolding** | `option(FDS_GPU_BENCH … OFF)` + new target; `.mm`; Metal device; runtime MSL compile; link `libFDS.a` | **DONE** | Proof the approach works at all |
| **1. Phase 2 spike** | greets geometry via `LoadFLD`, albedo via `Load_Texture`, de-indexed triangles with per-face UVs, one review pose, GPU frame timing, offscreen | **DONE** — result in §6.1 | **The first real number**: this exact geometry, textured, on the GPU |
| **2. Deferred G-buffer + 7 omnis, no shadows** | MRT G-buffer, full-screen **PBR** lighting pass (GGX + split-sum env BRDF + multiscatter + SH ambient — the stack greets actually runs) | ~1.5–2 days | The lighting comparison without the shadow confound. **Highest value per day, because the frame is lighting-bound.** |
| **3. Cube shadow maps** | 7 × 6 × 512² + 3 × 6 × 128², `sample_compare` + PCF | ~1.5–2 days | The full greets-frame comparison, and makes the PCF / lightmap-amortisation asymmetries concrete |
| **4. HDR + ACES tonemap** | `rgba16float` target + tonemap pass | ~½ day | Parity with greets' default look; makes screenshots visually comparable |
| **5a. Hardware tessellation** | displace real geometry; run sweep-A and measure surface-registered slip | ~2 days | **The H1/H2 discriminator that has never been run** — the highest-value item in the plan. Also reproduces the protrusion look the user has already seen work. |
| **5b. Filtered-wall ground truth** | falls out of 5a (trilinear + aniso are free) | ~½ day | Shows the same wall without the CPU's point-sampled ~4 px crawling texel blocks — may be part of "swim" |
| **5c. Prism / closed-shell** | rasterise extruded boxes, bound the ray to the interior, discard on exit, Z-buffer arbitrates overlaps | ~2–3 days | **Answers in days what has cost a week**, and gives a visual ground truth for a CPU port. Flat quads satisfy Hirche's planar-side-face restriction exactly; ~1,800 faces vs the carve's 86,600 |
| **5d. Flat POM baseline** | fragment-shader march, no silhouette | ~1 day | The cheap A/B arm against 5a/5c |
| **6. (optional) Snapshot output** | offscreen PPM/PNG matching our snapshot naming | ~½ day | Cross-renderer image diffs with existing tooling |

**~3 days to the first meaningful lighting number. ~2 weeks to the strategic answer.**

### 6.1 Phase 2 result (MEASURED, `GpuBench/`)

Built and run. `cmake -S . -B build-gpu -G Ninja -DFDS_GPU_BENCH=ON`, then from `Runtime/`:
`build-gpu/GpuBench/GpuBench` (offscreen, writes a PPM; it never opens a window).

Primary review pose `t=5743` → `CurFrame` 1722.9, 1920×1080, MSAA 1×, median of 300 frames after
60 warmup, timed with `GPUEndTime − GPUStartTime`:

| Configuration | Median GPU ms | p5 / p95 |
|---|--:|---|
| Full scene — 35 draws, 8,952 tris, 11 textures, 100 % screen coverage | **0.0962** | 0.056 / 0.101 |
| `--no-draw` — render-pass floor (clear + store the 1080p BGRA8 target) | **0.0176** | 0.017 / 0.019 |

**Net scene work ≈ 0.079 ms**, i.e. ~4.5× the pass floor. The `--no-draw` arm exists so this is
*reported* rather than assumed: at this triangle count it is a real question whether the number
measures the scene or just the cost of beginning and ending a render pass, and it turns out to be
the scene.

Ingest, same run: `LoadFLD` + 11 `Load_Texture` decodes in **18.9 ms**, 7 meshes with faces,
8,952 faces, 5,936 source verts → **26,856 de-indexed GPU verts**, 35 (mesh × material) draws,
10 lights. Camera from FDS: `perspX` 1728.00, `perspY` 1296.00, FOV 58.11°, `cntrE` (959.5, 539.5).

**Correctness check.** A `DEMO --snapshot=greets@t=5743` at the same `FDS_GREETS_CAM` pose was
rendered headless and compared: **framing, wall/floor geometry and the mech pose align.** The
visible differences are exactly the ones this plan predicts — DEMO has PBR shading, cube shadows,
HDR/bloom, flare sprites, and the `greets_stone_tex` wall/floor albedo override applied DEMO-side.
That last difference is the **empirical confirmation of §2.6**: the wall surface in the GPU render
is the FLD's, not the one the user reviews.

Two honest caveats on the 0.0962 ms:
- **Backface culling is OFF** in the GPU arm (the engine's cull lives in `Transform_Objects` and
  is not reproduced), so the GPU is doing *more* fragment work than it needs to, not less.
- **There is no scene lighting.** The fragment shader applies one fixed camera-facing term as a
  depth cue so the image is verifiable by eye. It is two FMAs and cannot move the number.

**Do NOT quote a CPU comparison number yet.** The snapshot run above reported `TOTL 1122 ms` for
its single frame, but that is one cold frame including first-frame costs, on a tree carrying other
agents' uncommitted edits — it is not a steady-state measurement and comparing it to 0.0962 ms
would be exactly the category error §5.3 warns about. The CPU side of the comparison needs
`--bench=scene@scene=greets,…` with load recorded and the per-phase counters, under the §4
condition set (mirrors/disco off).

### 6.2a THE DIRECT-LIGHTING BUG — why the comparison table is NOT in this document yet

**Read this before §6.2.** The user said he was "still not seeing actual lights per pixel work". He
was right, and the arm had **two** independent defects in the direct term. One is fixed; the second
is diagnosed but **not** fixed, and until it is, no timing from the lighting pass means anything —
a pass that computes an almost-black result is not a comparison point in either direction.

All figures below: t=5743 primary review pose, `FDS_GREETS_CAM=
"9.07557869,3.19592357,-52.9277191,-0.20672597,-0.140846997,0.968207836"`, 1920×1080, Rec.601 luma
over the whole frame, load 3.9–4.2, macOS arm64 M2 Max.

#### Defect 1 — the omni RANGE PATCH. FIXED (commit `dd21682`).

`Initialize_Greets` (`GREETS.CPP:2652-2673`) rewrites every `Light_Omni` whose `IRange` is 0 to
`greets_omni_default_range` = **30**. It runs *before* its own `Animate_Objects`, so the FLD's Range
envelope has not been evaluated yet, `IRange` is 0 for **all ten** omnis, and all ten are patched. It
overwrites `Range.Keys[0]` too, which is what makes 30 survive the spline evaluation that follows.
DEMO says so itself: `[GREETS] patched IRange=30 on 10 FLD omnis (had 0)` — read out of a real run
log, not inferred.

GpuBench ran `Animate_Objects` first and ingested the **authored** ranges — 3, 3, 10, 10, 7, 20, 20,
2, 2, 2 — against the CPU's uniform 30. **The three mech omnis ran at 2.0 against the CPU's 30: 15×
the radius, 225× the area**, in a room 60+ units across.

The fix replicates the patch *mechanism* (not the constant 30) between `LoadFLD` and
`Animate_Objects`, exactly where greets' own patch sits, so it stays correct for multi-key Range
splines. `--no-range_patch` / `--omni_range=F` for A/B.

| term (`--viz=…`) | before (authored ranges) | after (parity) |
|---|--:|--:|
| **direct** | mean **0.17**, median **0**, >8/255 **0.40 %** | mean **25.68**, median **17**, >8/255 **89.44 %** |
| ambient | mean 34.08, median 31 | mean 34.08, median 31 — unchanged, as it must be |
| emissive | mean 64.07, median 77 | mean 64.07, median 77 — unchanged |
| `--viz=lights` in-range coverage | **15.95 %** | **100.00 %** |

**This retracts a documented "scene property".** §6.2's "the scene genuinely has very little direct
omni light at t=5743 — only 15.95 % of covered pixels have even one light in range … that is a
property of the *scene*, not a bug" was **the bug's own symptom**. The buggy arm reproduces 15.95 %
exactly; with the patch it is 100.00 %. Likewise "greets does NOT patch these omnis' ranges" was
exactly backwards. Both are struck in §6.2.

Ruled out by reading the CPU kernel rather than assuming: the **attenuation shape already matched**
(`DeferredSurfaceKernel.cpp:3230-3258` is `falloff = 1 − dist·rRange`, hard cutoff at range,
`k = NoL·falloff·Material::Diffuse` — the shape the shader already had), so the range was the whole
of defect 1.

#### Defect 2 — the shadow tap zeroes ~95 % of the direct term. DIAGNOSED here, **RESOLVED in §6.2b** (commit `dd4bb92`) — and it was NOT the tap.

Measured by ablating direct light on **both** arms and differencing the composited frames — the same
knob on each side, so it is a two-sided measurement rather than a GPU-only self-report. CPU:
`--prof_no_lights` (skip the omni loop). GPU: `--light_range_scale=0` (every light out of range).
Matched tier on both: PBR + cube shadows + HDR + bloom + ACES, mirrors/disco/POM/lightmap off,
full-rate.

| arm | full frame | direct ablated | **what direct contributes** |
|---|--:|--:|--:|
| **CPU** (DEMO, pinned `0846811`) | mean 115.04, med 119 | mean 93.64, med 95 | **+21.40 mean, +24 med** |
| **GPU**, shadows ON | mean 95.22, med 98 | mean 94.66, med 97 | **+0.56 mean, +1 med** |
| **GPU**, `--no-shadows` | mean **105.37**, med 107 | mean 94.66, med 97 | **+10.71 mean, +10 med** |

Two things fall out, and the second is the more important:

1. **The ambient + emissive base is already right.** GPU 94.66 against CPU 93.64 — within ~1 %. The
   *entire* remaining tone gap is the direct term, not the SH-ambient gap that had been assumed to
   explain the whole ~20 % under-read.
2. **Turning shadows off recovers 19× of the direct contribution** (+0.56 → +10.71). The cube tap is
   over-occluding almost everything. It is still 2× short of the CPU's +21.40 even then, so a third
   factor remains, but the tap is the dominant one.

**And the diagnostic was lying, which is why this survived a whole round of "shadows now correct".**
`--viz=direct` is **byte-identical with and without `--no-shadows`** (mean 25.68, median 17, >8/255
89.44 % in both). Its own comment claims "10 = direct only (all omnis + spots, **with shadows**)",
but mode 10 never applies the shadow factor. So the per-term viz reported a healthy direct term
while the composited direct term was being zeroed — the same "the instrument shows a value where
there is no data" trap §6.2 already records twice, in a third costume. **Fix the viz first**, then
the tap: a diagnostic that cannot fail its own test cannot settle anything.

Prime suspect for the tap, untested: defect 1 changed every light's range from 2–20 to 30, and the
bake's far plane is derived from the range. §6.2's bug #4 is precisely this failure mode
(`--light_range_scale` not scaling the baked cube's far plane put every surface outside the frustum
it was baked in and read as occluded). The bake and the tap must be re-checked *at the patched
range*, with `--dump_cube` read back as before — that is the method that settled the housing bug and
it should settle this one.

### 6.2b Defect 2 — RESOLVED. It was TWO bugs, and neither was the cube tap.

**Prime suspect ruled out first, by reading the data.** §6.2a nominated the bake's far
plane at the patched range 30 (bug #4's failure mode). `--dump_cube` says no: every face of
every static cube is **100 % valid, 0 non-finite, 0 cleared**, and the decoded distances are
the room's real dimensions to two decimals — light 3's `-Y` face reads exactly **4.73**
(the floor, at light y 4.73 → y 0.000) and its `-X` face **26.679** (the left wall, at
13.10 − 26.68 = −13.58, against the FLD's X min −13.6). Its `+X` face is a uniform 0.477 and
that same plane reappears on all four side faces following an exact `0.477 / (sc/ma)` law.
The bake, the face convention and the reversed-Z encode/decode were all already right.

#### Step 1 — the instrument, before the bug. THREE more no-data faults found.

`--viz=direct` was byte-identical with and without `--no-shadows` because mode 10 carried a
**private copy of the light loop** that never applied the shadow factor. The structural fix
is not "add the missing line": `fs_viz` and `fs_lighting` now call the same
`DecodeSurface` / `LightReaches` / `ShadowFactor` / `DirectRadiance` / `AmbientRadiance`, so
a mode cannot compute a different quantity from the one the frame applies.

Modes corrected in the audit:

| mode | fault | fix |
|---|---|---|
| 2 `ao` | `fs_gbuffer` forces alpha to exactly 1 for every material without `Mat_AoInAlpha`, so **"no AO map" and "AO says fully open" were the same white** | graded AO stays grey; the no-data case is **cyan** |
| 5 `shadow` | fixed 0.0015 bias and **no NoL gate**, while the frame used `mix(0.0025,0.0004,NoL)` and skipped `NoL<=0` lights — it could show occlusion the frame never applied, and vice versa | calls the frame's own `LightReaches` + `ShadowFactor` |
| 6 `lights` | its own range/cone test, not the frame's; an out-of-range `--viz_light` read as "no light in range" (black), a legitimate value | calls `LightReaches`; bad selection is **magenta** |
| 7 `shadowraw` | `max(vizLight,0)` **silently reported light 0** when none was selected; returned pure **BLUE** for "no cube" while blue is one of its own data channels | `--viz_light` required; both no-data cases magenta |
| 8 `ambient` | omitted the multiscatter env term the frame adds, so it **under-reported the frame's own ambient** | shares `AmbientRadiance` |
| 10 `direct` | never applied the shadow factor despite claiming "with shadows" | shares `DirectRadiance`, honours `--no-shadows` |
| (any unmapped `--viz`) | **silently fell through to mode 5** and looked like a real answer | magenta |

Added: `--viz=direct_noshadow` (mode 11 — `direct` minus `direct_noshadow` is exactly what
the tap removes, in one pair of runs), `--viz=worldpos` (mode 12, fixed decode so a pixel can
be turned back into a world point), and `--viz_light` now applies to the direct decomposition
so the term is attributable per light.

Two **ground-truth** instruments, both host-side, both because two rounds here were spent
inferring geometry from decoded depths instead of asking the geometry:

- `--probe=x,y,z` — ray-cast the *same casting triangles the bake rasterised*, naming the
  nearest hit's mesh and material, **and** replay the shader's cube tap on the host, side by
  side. Agreement ⇒ the cube is honest; disagreement ⇒ the tap's conventions are wrong.
- `--probe_px=X,Y` — same for a screen pixel: build its camera ray from the constants the
  vertex shader projects with, ray-cast all geometry, then replay the lighting pass's
  per-light gate and **name the test that failed** (out of range / backfacing / reaches).

`--dump_cube` now also prints an **8×8 grid of decoded world distance per face**. Min/max
alone hid the thing that mattered: a face can be 100 % valid, span 0.5–30 units, and still be
a near wall over most of its solid angle.

> One trap this section fell into itself, recorded so it is not repeated: the first version of
> `--probe_px` computed the triangle normal as `e1×e2`. FDS's `Compute_Face_Normals`
> (`PREPROC.CPP:29`) uses `Cross_Product(V,U)` with `U=B−A, V=C−A`, i.e. **`e2×e1`** — the
> negation. The probe therefore reported every room surface as outward-facing and sent a whole
> round chasing an inverted-normal hypothesis. A ground-truth instrument needs its own ground
> truth checked: the floor must come out `(0,+1,0)`.

#### Step 2 — bug A: `LoadFLD` does not compute normals, so **every G-buffer normal was (0,0,1)**

`Face::N`, `Vertex::N`, `Vertex::Tangent` and `Face::NormProd` are produced by
`Scene_Computations` (`FDS/MISC/PREPROC.CPP:632`), which `DEMO` reaches through
`Preprocess_Scene` and this ingest **never called**. Every `Vertex::N` was zero, so the
G-buffer stored `oct_decode(0,0)` — a constant `(0,0,1)` view normal on every surface.

MEASURED before the fix: `--viz=normal` returned **rgb (128,128,255) at both a side wall and
the floor**, two surfaces at right angles.

The consequence was not "flat shading". `N·L` degenerated into the **sign of the light's
view-space Z**, so a light *behind* a surface down the corridor lit it while a light three
units *in front* did not. The three mech omnis — which `--probe_px` shows dominate this pose
(`NoL` 0.58–1.00, `atten` 0.88–0.91 on both the near wall and the floor) — reached **15,381
of 2,073,600 pixels (0.74 %)**, and tripling `--light_range_scale` did not change that number
by one pixel, which is what proved the gate was `NoL`, not range.

Fixed by calling the engine's own `Scene_Computations` between `LoadFLD` and
`Animate_Objects` — the same "same bytes through the same code" principle as the rest of the
ingest. After: wall `(+1.000,+0.075,+0.035)`, floor `(+0.004,+0.992,−0.106)`, ceiling
`(+0.027,−0.969,+0.255)`.

**This also invalidates §2.4's implicit assumption** that `LoadFLD` hands over a
render-ready mesh. It hands over positions, topology and UVs; normals, tangents, plane
products and bounding spheres are a separate call.

#### Step 2 — bug B: the view→world inverse was **transposed the wrong way**

The reconstruction gathered the view matrix's **columns**, i.e. computed `Mat·P` — the
forward transform, a second time — under a comment asserting it was the transpose. `vs_gbuffer`
builds view space as `P = Mat·(world − camSrc)` with `Mat`'s **rows** being (right, up,
forward), so the inverse is `world = camSrc + P.x·row0 + P.y·row1 + P.z·row2`.

MEASURED: a screen-centre pixel reconstructed 4.81 units from the eye along
**(0.217, 0.129, 0.973)** — the matrix's third *column* — where the camera's forward is
**(−0.207, −0.141, 0.968)**. Same magnitude, X and Y mirrored: the signature of the wrong
gather. Every **cube-shadow direction**, every **spot cone test** and the **SH ambient's world
normal** were therefore evaluated along a rotated ray. That is why the tap read a nearly
constant ~2.8 units of stored depth across the whole frame while the host replica of the same
tap, on the same cube, agreed with the ray-cast on all ten lights.

After the fix, a bottom-edge pixel reconstructs to **y = 0.000 exactly** — the floor.

#### Result, measured two-sided at t=5743

Same knob on each arm (CPU `--prof_no_lights`, GPU `--light_range_scale=0`), same tier
(PBR + cube shadows + HDR + bloom + ACES; mirrors, disco, POM, lightmap off; full rate),
1920×1080, Rec.601 luma over the whole frame. CPU arm is the pinned `DEMO` from `0846811`.

| arm | full | direct ablated | **direct contributes** | shadows off | **what the tap removes** |
|---|--:|--:|--:|--:|--:|
| **CPU** (DEMO) | 115.07 | 93.50 | **+21.57** | 132.11 | 17.04 = **44.1 %** |
| **GPU** before | 95.22 | 94.66 | **+0.56** | 105.37 | 10.15 = **94.8 %** |
| **GPU** after | 106.25 | 94.82 | **+11.43** | 117.70 | 11.45 = **50.0 %** |

The tap's removal fraction now agrees with the CPU's (50.0 % vs 44.1 %) and the direct term
is **20× closer** to it. `--viz=shadow` shows the mech silhouette cast on both walls and the
floor with per-light penumbra bands; the lit frame has per-pixel light pools with falloff and
the same blue-near / warm-far structure as the DEMO reference's own direct-only difference
image.

Per light at this pose (`--viz=direct[_noshadow] --viz_light=N`, mean tonemapped luma over
the frame; "reaches" = pixels passing the frame's own range + `NoL` + cone gate):

| light | direct, shadowed | direct, unshadowed | tap removes | reaches px | unshadowed % |
|--:|--:|--:|--:|--:|--:|
| 0 | 1.333 | 1.750 | 23.8 % | 102,297 | 69.1 |
| 1 | 0.999 | 2.001 | 50.0 % | 96,294 | 39.8 |
| 2 | 1.085 | 1.103 | 1.7 % | 278,841 | 94.2 |
| 3 | 6.546 | 7.928 | 17.4 % | 1,447,284 | 73.2 |
| 4 | 0.000 | 13.502 | **100 %** | 898,194 | 0.0 |
| 5 | 0.029 | 1.411 | 98.0 % | 868,668 | 1.2 |
| 6 | 0.000 | 0.000 | — | 0 | — |
| 7 | 4.358 | 10.260 | 57.5 % | 1,779,039 | 32.1 |
| 8 | 3.627 | 10.032 | 63.8 % | 1,758,228 | 26.1 |
| 9 | 11.453 | 12.267 | 6.6 % | 1,795,017 | 93.5 |

**Light 4's 100 % is CORRECT, and that is established by ground truth rather than assumed**:
`--probe=4.38,0,-30.95` ray-casts light 4 → surface and reports `BLOCKED t=2.530
(Piramid.lwo/rooms)` against a 26.30-unit separation, and the host replica of the cube tap
reads `storedDist 1.961` at the same texel the shader samples. Light 4 sits at x = −11.89 —
in a different corridor, behind a wall. Light 5 (98 %) is the same situation.

#### What is still open, stated as a residual and not explained away

The GPU's **unshadowed** direct term is +22.88 against the CPU's +38.61 — a factor of 1.69
that has **nothing to do with the shadow tap** (it is measured with the tap off on both
arms). The ambient+emissive base is matched to within 1.4 % (GPU 94.82 vs CPU 93.50), so the
whole residual is still direct. Candidates not yet discriminated: the CPU's PolyId identity
test being more permissive than a depth compare at material boundaries (worth ~6 points of
the *shadowed* difference, not the unshadowed one), the specular model's magnitude, and the
tonemap's compressive response at slightly different bases. **Not diagnosed — do not quote a
cause for it.**

#### Consequence for the deliverable

The matched-workload CPU-vs-GPU table is **deliberately not published here.** A full tiered dataset
was collected (9 CPU tiers × 2 poses × 2–3 reps with per-pass TailProf decomposition, and a 9-config
GPU stage-differencing ladder × 2 poses × 3 reps, interleaved, min-of-arm). The **CPU half remains
valid** and is preserved — it was taken against a pinned `DEMO` binary from `0846811`, and the CPU
was never affected by either defect. The **GPU half is withheld**: its lighting pass is currently
computing a direct term that is ~38× too weak in the composite, and both its cost and its shape will
change when the tap is fixed. Publishing a ratio against it would be exactly the category error §5.3
exists to prevent.

Two findings from that dataset are safe to record now because they do not depend on the GPU arm:

- **The CPU's cube-shadow tap is the single largest cost in the greets frame.** Adding cube shadows
  to the matched base tier moves the CPU's deferred lighting wave from **31.20 → 59.42 ms**
  (+28.2 ms elapsed) at 1920×1080 full-rate. `PERF_STATE.md`'s "~9 ms saved by
  `shadow_polyid_no_pcf`" is the PCF component of that same tap.
- **The static shadow lightmap does not pay for itself at these poses.** t=5743: lighting wave
  59.42 ms without it, **62.80 ms with it**. t=2000: 72.14 vs 70.26. It is a wash to slightly
  negative — worth re-examining, since §3 describes it as an amortisation that lets the CPU *skip*
  taps.

**Method note, and it is the same one this document already learned twice:** the run that mattered
was not another convention adjustment, it was ablating the *same* quantity on *both* arms and
comparing composited frames. And the reason the bug survived so long is that the one diagnostic
built to see it silently ignored the term that was breaking it.

#### The CPU half of the dataset — recorded so it does not have to be re-measured

Pinned `DEMO` from `0846811` (binary snapshotted before the run so a concurrent agent's edits could
not shift it mid-campaign), 1920×1080, `--bench=scene@scene=greets,…,iters=125`, `--deferred
--profiler=1`, `FDS_TAIL_PROF=1`. **min-of-arm over 2–3 interleaved reps**, machine load 4–12
(a Spotlight reindex burst to 24–56 was caught by the load stamp and discarded by min-of-arm).
`f_min` is min frame-ms over 125 iters; the per-pass columns are TailProf **means** over the second
60-frame window (the first carries the cold frame), so they are a different statistic from `f_min`
and must not be summed against it.

Tiers, cumulative. All exclude mirrors + mirror RTT, disco, volumetric cones — and **`--no-parallax`,
because the GPU G-buffer does no POM march at all** (verified by reading `fs_gbuffer`; the CPU's
8-step march is priced separately by the `Cpom` row). `A/B` are non-PBR ("basic lights"), `C` adds
the full PBR stack + HDR + bloom + ACES, `D` is the untouched shipping default.

| tier | what it is | t=5743 `f_min` | lighting wave | G-buf wave | bake wall / core-ms |
|---|---|--:|--:|--:|--:|
| `A` | base + basic lights, **full-rate** | **31.49** | 31.20 | 2.70 | — |
| `Acb` | same, CPU's default checkerboard half-rate | **20.67** | 15.79 | 2.61 | — |
| `B` | `A` + cube shadows | **54.81** | 59.42 | 2.88 | 1.14 / 8.19 (effPar 7.2) |
| `Blm` | `B` + static shadow lightmap | 54.43 | 62.80 | 3.02 | 1.46 / 11.50 |
| `C` | `B` + full PBR + HDR + bloom + ACES, full-rate | **63.34** | 60.98 | 2.77 | 1.43 / 11.64 |
| `Ccb` | `C` at half-rate | 40.58 | 30.66 | 2.64 | 1.48 / 11.71 |
| `Clmcb` | `C` + lightmap + half-rate | 41.45 | 30.93 | 2.63 | 1.49 / 11.66 |
| `Cpom` | `C` + the POM march restored | 67.38 | 62.65 | **5.80** | 1.41 / 11.30 |
| `D` | full shipping stack (mirrors, disco, POM, lightmap, half-rate) | 48.31 | 31.71 | 6.92 | 1.24 / 9.16 |

At t=2000 (disco glow covering 68.4 % of frame, authored spline camera on both arms): `A` 35.25,
`Acb` 23.24, `B` 67.07, `Blm` 69.55, `C` 79.41, `Ccb` 70.45, `Clmcb` 57.62, `Cpom` 91.90, `D` 80.50.
The pose is materially heavier for the CPU — tier `B`'s lighting wave is 72.14 ms against 59.42 at
t=5743 — which is why a table built on t=5743 alone would understate the light workload.

What this half already establishes, independent of the GPU arm:

- **The cube-shadow tap is the largest single cost in the frame**: `A`→`B` moves the lighting wave
  **31.20 → 59.42 ms**, +28.2 ms elapsed, at full rate.
- **Checkerboard is worth almost exactly 2×** on the lighting wave (31.20→15.79, 60.98→30.66) —
  so any GPU full-rate comparison against a CPU default number must say which it used.
- **The static shadow lightmap does not pay for itself here** (59.42 without vs 62.80 with at
  t=5743; 72.14 vs 70.26 at t=2000) — a wash to slightly negative, though §3 describes it as an
  amortisation that lets the CPU skip taps.
- **The POM march costs ~4 ms of frame, and it lands in the G-buffer/raster pass, not the kernel**
  (G-buffer wave 2.77 → 5.80 ms) — it is gated in `Mekalele.h:3171`.
- **The geometry front end is not the problem**: `Tick-Xfrm` is 0.093–0.101 ms/frame at t=5743.
  The frame is deferred-lighting-bound, exactly as §5.4 predicted.
- **`bakeWall` vs `bakeBusy` is the elapsed-vs-core-ms conversion, measured rather than assumed**:
  1.43 ms elapsed against 11.64 core-ms at effective parallelism 7.6–8.1. §5.3 item 7's category
  error is a factor of ~8 here.

---

### 6.2 Phase 3 stage 1 — deferred arm built, shadow CASTER FILTER correct, timings RETRACTED

> **Superseded in part by §6.2a.** This section's conclusion that "shadows are now correct" was
> established against the **caster filter** (lamps no longer self-occlude) and that part stands. It
> does **not** cover the over-occlusion defect 6.2a documents, which was invisible here because the
> per-light acceptance table below was computed on the buggy authored ranges *and* through a viz mode
> that ignores the shadow factor.

The deferred path (G-buffer -> cube shadow bake -> PBR lighting -> ACES tonemap) is implemented and
runs. Its shadows were wrong through the first two rounds; **the caster filter is now correct**
(evidence below)
and the cause turned out to be a missing shadow-caster filter, not a broken tap. **Every timing
figure produced before that fix is retracted** and no replacement has been measured yet — a shadow
tap whose reference trivially fails is cheaper *and* differently shaped than a correct one, so the
old numbers were suspect in both directions.

**Retracted:** the earlier "G-buffer 0.275 / lighting ~0.25 / dynamic bake 0.546 / tonemap 0.030 /
frame 1.113 ms" table, and any ratio derived from it.

#### Bugs found and fixed (all real, all in this arm)

1. **Cube-face orientation.** Derived, not guessed. Metal maps a direction with
   `u = ½(sc/ma+1)`, `v = ½(tc/ma+1)`; Metal's viewport origin is **upper-left**, so texel row 0
   corresponds to `dot(up,d) = +ma` while the sampler puts `v=0` at `tc = -ma`. Hence
   `right = sc_direction` and **`up = -tc_direction`** — all six of my `up` rows had the wrong
   sign, baking every face vertically mirrored.
2. **Light intensity.** Linearising by squaring `(colour x ISize)` also squared the intensity,
   making every `ISize = 0.5` omni 4x too dim.
3. **The viz itself was misleading** — it returned WHITE both for "lit and unshadowed" and for "no
   shadow-casting light within range". That is what made the frame look like a broken shadow test
   before anything was diagnosed. Now three distinct states. *Never encode "no data" as "the value
   is 1".*
4. **`--light_range_scale` didn't scale the baked cube's far plane**, so scaling a light's reach
   put every surface outside the frustum it was baked in and read as occluded — a broken test, not
   a broken tap.
5. **Bake policy now matches greets**: static cubes baked ONCE and cached (`Omni_StaticShadow`),
   only mech-parented omnis re-bake per frame — detected from the FLD Object **parent chain**, not
   from a non-finite `IPos` (which only occurs outside the authored frame range and reported zero
   moving lights at every real pose).
6. **Render targets were `Shared`, now `Private`** — `Shared` forces write-through and disables
   lossless compression; it inflated every pass several-fold.

#### The shadow bug — RESOLVED. It was a missing caster filter, not a bad tap.

The standing hypothesis was that `depthcube::sample()` returned a non-finite value (0 % of in-range
pixels fully unshadowed; `--viz=shadowraw`'s two channels, computed from the *same* `storedDist`
expression, disagreeing irreconcilably). **That hypothesis was wrong**, and it was settled by
*looking at the baked bytes* rather than adjusting conventions again.

`--dump_cube=N` (new) reads a light's baked cube into host memory and prints, per face,
non-finite / cleared / min-max stored depth **and the decoded world distance**, plus a 3×2 face
atlas PPM. First run, light 5 (range 20, room 60+ units across):

```
+X valid=100.0% cleared=0 nonfinite=0  storedZ[0.1617..0.2623]  dist[0.189..0.305]
-X valid=100.0% cleared=0 nonfinite=0  storedZ[0.1621..0.2626]  dist[0.189..0.305]
+Y valid=100.0% cleared=0 nonfinite=0  storedZ[0.0040..0.2387]  dist[0.208..7.670]
-Y valid=100.0% cleared=0 nonfinite=0  storedZ[0.0021..0.2626]  dist[0.189..10.845]
+Z valid=100.0% cleared=0 nonfinite=0  storedZ[0.1585..0.2561]  dist[0.194..0.311]
-Z valid=100.0% cleared=0 nonfinite=0  storedZ[0.1655..0.2626]  dist[0.189..0.298]
```

**Finite, 100 % covered, and ~0.25 units away on every face** — a shell around the light. (The
±Y faces reaching 7.670 and 10.845 are the ceiling 7.65 above and the floor 10.85 below, which
also confirms the depth *encode/decode is exact*.)

A nearest-geometry probe added to the light inventory named the cause outright:

| light | nearest geometry | material | verts within 1 u |
|--:|--:|---|--:|
| 0–4, 6 | 0.285–0.335 | `Piramid.lwo` / **`lamp light`** | ~1,180 |
| 5 | 0.327 | `Piramid.lwo` / **`lamp`** | 1,143 |
| 7, 8 | 0.051–0.057 | `Hull.lwo` / `canons` | 222 |
| 9 | 0.006 | `Hull.lwo` / `hull not smooth` | 2,170 |

**Every greets omni is authored INSIDE its own lamp fixture**, and the three mech omnis are inside
the mech hull. Each light was faithfully baking the *inside of its housing*, so every surface in
the room was correctly reported as occluded. The bake, the face convention, the reversed-Z encoding
and the tap were all already right.

The CPU engine has an explicit filter for exactly this, at `FDS/RENDER/Shadows.cpp:703-724`: its
shadow bake skips any material that is `Mat_Transparent | Mat_Additive | Mat_SkipZ` **or whose NAME
contains "lamp" or "emi"** — the FLD carries no emissive flag, so the engine infers it from the
name — with a source comment about chasing *"lamps still cast shadows"*. GpuBench now reproduces
that predicate byte-for-byte (`Batch::castsShadow`, applied in the shadow pass only). This is
**parity, not an optimisation**: 27 of 35 batches and 5,988 of 8,952 tris cast; the 8 non-casters
are `lamp`, `lamp light`, `screen2`, `screen 3`, `screen 4`, `screen emiter{, fance, green}`.

**Acceptance, MEASURED** (t=5743, 1920×1080, `--viz=shadow`, three-state encoding). Of the 15.95 %
of covered pixels that have any light in range:

> **These per-light percentages were taken on the pre-§6.2a arm, i.e. at the AUTHORED omni ranges,
> and the 15.95 % denominator is the bug's symptom, not a scene property.** The *conclusion* they
> support — that the cube tap works and the caster filter is right — still stands, because it was
> settled by reading the baked cubes directly. The *numbers* in this table are superseded: with the
> range patch every light reaches far more pixels, so re-derive them before quoting them.

| arm | unshadowed | shadowed | partial | in-range px |
|---|--:|--:|--:|--:|
| all lights | **85.20 %** | 14.11 % | 0.69 % | 330,674 |
| `--viz_light=7` (mech flare) | 46.03 % | 53.18 % | 0.79 % | 27,223 |
| `--viz_light=8` | 100.00 % | 0 % | 0 % | 9,897 |
| `--viz_light=9` | 97.21 % | 2.79 % | 0 % | 282,655 |
| `--viz_light=3` | 0 % | 100.00 % | 0 % | 26,389 |

Before the fix this column read **0 % unshadowed for every light**. Light 3's 100 % was verified
*real* rather than a residual bug with `--viz=shadowraw`: its in-range pixels sit 9.3–9.7 units
from the light while the cube's nearest occluder along the same direction is at 2.1 units — and
the two `shadowraw` channels now **agree** (G ⇒ 2.20 u, B saturated ⇒ ≥2 u). The earlier
irreconcilable disagreement was a symptom of the housing, not of NaN.

One more no-data-as-a-value trap closed in the same pass: mode 5 returned **black** both for "lit
but fully shadowed" and for "no G-buffer". Uncovered pixels are now dark **red** — and the count is
**2 of 2,073,600**, which is precisely what proves light 3's reading is geometry and not sky.
`--viz=lights` now honours `--viz_light` so the two diagnostics can be compared per light.

**The timings in this section remain RETRACTED.** Correct shadows change the tap's cost, and no
re-measurement to the §5.1 discipline (load-guarded, min-of-arm, stage differencing) has been run
yet.

**Method lesson worth keeping:** two rounds were spent adjusting the face convention and the bias
against an indirect symptom. The thing that settled it in one run was reading the baked buffer back
and printing decoded world distances next to a name for the nearest mesh. Prefer a direct look at
the data over any amount of convention arithmetic.

#### Ruled out along the way

- **The walls ARE in the shadow draw list.** (At the time: all 35 batches. Now 27 of 35 — the
  `lamp` / `screen*` non-casters are excluded, matching `Shadows.cpp`. The walls still cast.)
- ~~**greets does NOT patch these omnis' ranges.**~~ **RETRACTED — this was exactly backwards, and
  it was the direct-lighting bug.** See §6.2a. `GREETS.CPP:2652` patches omnis whose `IRange` is
  **0**, and it runs *before* `Animate_Objects`, so `IRange` is 0 for **all ten** and all ten are
  patched to 30. DEMO prints `[GREETS] patched IRange=30 on 10 FLD omnis (had 0)` — verified in a
  real run log. The 3,3,10,10,7,20,20,2,2,2 figures are the *authored* spline values, which the CPU
  never uses.
- ~~**The scene genuinely has very little direct omni light at t=5743** (only 15.95 % of covered
  pixels have a light in range) — a property of the *scene*, not a bug.~~ **RETRACTED. It was a
  bug, and this figure was its symptom.** The buggy arm reproduces 15.95 % exactly; with the range
  patch applied it is **100.00 %**. See §6.2a.

#### Light inventory — what this arm has, and what it is missing

MEASURED, printed every run (`[DEFERRED] LIGHT INVENTORY`). **21 lights.**

| # | origin | world position | linear rgb | range | shadow |
|--:|---|---|---|--:|---|
| 0,1 | fld | (±3.4, 3.79, 0.05) | (1,1,0) | 3.0 | 512² cube, static |
| 2,3 | fld | (±13.1, 4.73, −21.57) | (0.5,0.5,0) | 10.0 | 512² cube, static |
| 4 | fld | (−11.89, 3.41, −51.33) | (1,1,0) | 7.0 | 512² cube, static |
| 5,6 | fld | (33.5, 10.9, −49.8 / −75.5) | (0.5,0.5,0) | 20.0 | 512² cube, static |
| 7,8,9 | fld | mech-attached, ~(8–9, 2–3, −48..−50) | (0,0.126,0.5) | 2.0 | 128² cube, per frame |
| 10–19 | **disco-spot** | on the ball at (0, 4.98, −21), spun per tick | (4.27,5.10,6.00) | 38.0 | **256² single map**, per frame |
| 20 | **disco-glow** | (0, 4.98, −21) | (0.30,0.34,0.44) | 6.0 | none |

Plus **10 flare sprites** (additive 256² procedural, 2 distinct colours).

**What the disco is actually worth, MEASURED — and it is not what was assumed.**
At t=5743 each disco spot and the glow reach **0 pixels** (`--viz=lights --viz_light=N`),
because the ball is 33 units down the corridor pointing downward. They are not broken: at
t=2000, with the camera beside the ball, spot 0 reaches 8,178 px and the glow 1,417,526 px
(68.4 % of the frame). So the earlier expectation that "the disco spots are very likely most of
the lighting I'm not seeing" is **REFUTED at the primary review pose** — they are a real part of
the scene elsewhere on the camera path, but not there.

**What the missing light actually was: the FLARE SPRITES.** Settled by rendering a DEMO
reference at the same pose and looking at it. Its three large blue-white bursts (mech legs and
mast) are the omni flare sprites, not omni surface lighting — exactly as the previous inventory
suspected but had not confirmed. They are implemented and appear in the right places at roughly
the right core size.

**Still missing, in order of how much they cost the LOOK** (each identified by comparison with
the DEMO reference, not guessed):

1. **Bloom** (`bloom_intensity` 2.0, ON by default). The reference's bursts are noticeably larger
   and softer than ours, and its shoulder lamps carry a glow ours renders as flat dots. That is
   bloom, not a flare-sizing error — our burst *cores* are the same size.
2. **Exposure / tone.** `hdr_exposure = cine::kGreetsExposure` and `hdr_refl_gain` 4.0 are still
   not matched. Our mid-tones are visibly paler and less contrasty than the reference, whose walls
   read cooler and whose floor reads warmer.
3. The warm pool at the corridor's far end.
4. **The MIRROR — and it is a missing PASS, not a missing texture.** *(Label corrected 2026-08-06.
   Successive revisions of this document, including this one, called the black rectangle at the far
   end of the corridor "the procedural code screen, CPU-generated by the greets generator, so
   `LoadFLD` cannot produce it". **That was wrong.** It is the mirror.)* Filling it needs an RTT
   pass that renders the reflected scene into a texture — `FDS/RENDER/GreetsMirror.cpp`'s
   `BuildMirror` / `UpdateMirror`, the off-axis projection, and the mirror clone geometry. That is a
   substantially larger gap than a skipped procedural texture, and the comparison table must list it
   as such: a reader told "a procedural texture is missing" would assume it is cosmetic.
   **The mirror omni clones are absent for the same reason** — they belong to that pass, so the two
   share one cause rather than being two independent gaps.
5. **The code screen is a SEPARATE question, and it is not the black rectangle.** The screen
   materials do exist in this arm's batch list — `screen2`, `screen 3`, `screen 4` all ingest with
   `Luminosity = 1.000, Diffuse = 1.00` (MEASURED, `[INGEST] emissive material …`), plus
   `screen emiter{, fance, green}`; all eight are the shadow non-casters. So they render, as
   emissive surfaces carrying their FLD texture. What `LoadFLD` cannot produce is the *animated*
   content `GreetsGenerator` writes into them per frame. Reported separately from the mirror, and
   not counted as the black rectangle.
6. Volumetric cones.

The robot spot + 4 orbit spots remain **correctly** absent: `no_greets_spots` defaults `true`.

#### A fidelity bug this work surfaced: the direct diffuse was π times too dim

The GPU arm's direct diffuse carried a Lambert `1/π` that **the CPU kernel does not have**. The
CPU's direct term is `intensity = NoL * atten * Material::Diffuse; lR += intensity * colR`
(`DeferredSurfaceKernel.cpp:3245-3258`; the `--pbr` path at 2568 only adds the `(1−F)` factor),
and its only `1/π` factors are inside the GGX **D** term (lines 402 / 442 / 2431), which is
standard. So every direct light here was π× dimmer than the reference image.

MEASURED: with the `1/π`, all 11 disco lights together moved the t=2000 frame by **at most
2/255, with 0 pixels changing by more than 2/255**; without it, 525 px change by >2/255 (max 4).
Removed, and documented in the shader as parity rather than physics — the reference for this
comparison is the CPU image, and the CPU's Lambert is not normalised.

#### Tone: the frame was washed out because the albedo was never linearised

Diagnosed by DECOMPOSITION, not by eye. `--viz=ambient|emissive|direct` (new) tonemap each term
on its own so a patch can be compared straight against a DEMO reference patch and the offending
term **named**. That said *emissive* — and emissive alone (left wall p50 149.5) already exceeded
the reference's **total** (124.9), which ruled out the ambient source that had been assumed at
fault.

Cause, read out of the engine: under `--hdr_linear` the CPU kernel "square[s] the (normalized)
albedo and let[s] light enter at power 1", storing the result re-encoded with `sqrt`
(`DeferredSurfaceKernel.cpp:1374-1381`) — which is exactly what this arm's tonemap already does on
the way out. GpuBench stored the raw **gamma** texel and used it as linear radiance.

MEASURED, median luma of identical patches against a DEMO render at t=5743:

| patch | DEMO reference | before | after |
|---|--:|--:|--:|
| left wall | 124.9 | 168.2 | **102.0** |
| floor | 66.1 | 148.4 | **76.0** |
| whole frame | 121.4 | 163.7 | **97.3** |

From ~+35 % over the reference to ~−20 % under, with the floor near exact.

**Two suspected tone gaps are now STRUCK OFF rather than left as suspicions:** `hdr_exposure` needs
no work (`cine::kGreetsExposure` is **1.0**, `SceneTick.h:237`, already this arm's default), and
`hdr_refl_gain` 4.0 only affects `Mat_HdrEmissive` *forward* surfaces (the disco ball mesh), which
this arm does not have.

**The one ambient gap that remains** and explains the residual under-read: the CPU's `--sh_ambient`
coefficients come from a real 32² **env-probe bake at the room centre** (`EnvBake.cpp:1652+`,
`SHAmbient_EnsureBaked`) — actual room bounce — while this arm projects the FLD's zenith/nadir
**backdrop** gradient. `Scene::Ambient` is (0,0,0) here, so there is no cheap authored fallback; a
real probe bake is the fix and it is not a small one.

#### The direct diffuse deliberately has NO Lambert 1/π — do not "fix" it

The CPU's direct term is `intensity = NoL·atten·Material::Diffuse; lR += intensity·colR`
(`DeferredSurfaceKernel.cpp:3245-3258`), and its only `1/π` factors are inside the GGX **D**. That
is a long-standing engine convention, not a bug in the CPU. **This arm reproduces it on purpose**,
because the reference for this comparison is the CPU image. Normalising it would be physically
tidier and would put the two renderers π apart. The shader says so at the site.

#### Bloom

Implemented to match `Hdr.cpp`'s `Render_BloomPass` exactly — soft-knee bright pass
(`(lum−thresh)/lum` weighting, not a hard cut) into a DS=4 box downsample, separable 5-tap
`[1 4 6 4 1]/16` run **twice**, bilinear upsample × intensity added **before** the tonemap. greets
sets `bloom` ON with `bloom_intensity` 2.0 (`GREETS.CPP:1168-9`); threshold 200 on the CPU's linear
0–255 scale, /255 for this arm's 0..1 buffer. Not reproduced: `HdrClamp` on the add-back (f16
carries the range) and the shared bright-pass cache (anamorphic / lens-ghost are off).

#### Interactive window (`--window`)

A real SDL2 window with a `CAMetalLayer`, the scene **animated** through FDS's own
`Animate_Objects`, free-fly **and** the authored camera spline, and live per-pass GPU ms overlaid.
Offscreen remains the default — nothing is displayed without the flag.

- **Time.** `--time_scale` centiseconds per real second (100 = real time), mapped to `CurFrame` by
  greets' own formula, the same one `RENDER.CPP` derives (`t = Timer/SceneTime`,
  `CurFrame = lerp(StartFrame, EndFrame, t)`).
- **Per frame:** `Reanimate()` re-runs `Animate_Objects`, then refreshes object model matrices, the
  scripted camera, and the **whole light list** — the mech omnis ride the hierarchy and the 10
  disco spots rotate, so their positions, directions and spot shadow bases all change.
- **Vertices are never re-uploaded, and that is a property of the engine, not a shortcut.**
  `Animate_Objects` fills per-mesh `IPos`/`IScale`/`RotMat`; it does **not** deform vertices — the
  mech animates as a hierarchy of rigid TriMeshes. `VertexHash()` keeps that claim measured rather
  than asserted, and the HUD reports which it observed. So "re-upload only meshes whose verts
  changed" resolves to "none of them do"; the per-frame upload is 35 batch uniform blocks.
- **CPU-side per-frame work is timed separately** from GPU pass time (animation vs upload), which
  is the split the comparison table needs.
- SDL2 over raw Cocoa: already a dependency of DEMO and installed (2.32.10), ships `SDL_metal.h`
  so the layer is three calls instead of an `NSApplication`+delegate, and gives keyboard/mouse
  capture for free. Cost: one extra link dependency on a target that otherwise links only
  Metal/Foundation/QuartzCore; the offscreen path never touches it.

#### Build note — shader staging

Staging the `.metal` files is now its own target with them as `DEPENDS`, not a `POST_BUILD` on the
executable. `POST_BUILD` only fires when GpuBench relinks, so editing a shader alone left a **stale**
copy next to the binary and the runtime compiled the old source, reporting errors at line numbers
that no longer existed.

#### Other known gaps in this arm

- **Tone is not matched to DEMO** (`hdr_exposure` = `cine::kGreetsExposure` not looked up,
  `hdr_refl_gain` 4.0 and bloom absent). Does not affect cost, but the image is not a visual ground
  truth yet.
- Tangent basis is derived from screen-space derivatives, not FDS's `Compute_Vertex_Tangents`
  (unreachable from the loader). Relevant to §3.1 — this arm does not inherit the
  handedness/material-clone split.
- **Backface culling off**; **no frustum culling anywhere**, including the shadow bake, where the
  CPU culls per cube face. Both make the GPU do more work than the CPU, not less.

#### Method note that survives, and should be reused

Per-pass costs must come from **differencing whole-frame `GPUEndTime - GPUStartTime` intervals**
across `--stages=1|2|3`, not from per-encoder timestamps: stage-boundary timestamps are available
and are reported, but on Apple GPUs a pass's vertex stage can begin before the previous pass's
fragment stage retires, so they overlap and **sum to more than the frame**. Also, at machine load
13-24 the run-to-run spread on sub-millisecond passes reaches +/-0.15 ms, which is comparable to
the differences being taken — so every configuration needs repeated runs, min-of-medians, and the
load recorded. One single-run set measured `stages=2` *slower* than `stages=3`, which is impossible;
that is the size of the noise.

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
- Gate verification required after any CMake edit: `cmake --build build` unchanged (baseline:
  **94 `compile_commands.json` entries**, `ninja -n` clean), and `tools/render_gate.sh` 3/3.
  Note that gate covers `mirrortest` / `conetest` / `halotest` — **not greets**, whose pin depends
  on the user's uncommitted authoring files and is checked out-of-band via
  `tools/flip_rate.sh -n 24`.
- Offscreen rendering is the default. **A visible window is only opened on explicit request** —
  per the standing rule that visible runs are the user's to launch.

---

## 8. Open questions to settle before Phase 3

1. ~~Which displacement arms, in what order~~ — **SETTLED by the green-light**: deferred G-buffer +
   PBR lighting + cube shadows first (report that number), then **tessellation before prism**, then
   the filtered-wall ground truth and flat-POM baseline. See §3 and stages 5a–5d. The R5
   "no silhouette program" recommendation is **rejected** — §3's blockquote records why.
2. Shadow resolution sweep: 512² (our static default) only, or also 1024²/2048² to show the GPU
   scaling where the CPU's is prohibitive?
3. The 3 mech-parented omnis resolve through the hierarchy pass, which `Animate_Objects` already
   runs — so they should be free. Confirm at implementation time rather than assuming.
4. Is a GPU **forward** arm worth ~½ day, to isolate "deferred vs forward" separately from
   "CPU vs GPU"?
5. Checkerboard: does the GPU arm implement it for faithfulness, or stay full-rate and report the
   CPU both ways (current recommendation)?
6. Phase 3 must load the `greets_stone_tex` wall/floor sidecars directly (§2.6) — those, not the
   `wall_stone3` PBR set, are the surfaces the S1 campaign is about. Confirm which files are live
   by reading the `[GREETS] stone-tex …` line from a real run rather than from the source.
