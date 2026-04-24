# FDS Engine — current state

This describes the rendering engine as it exists today on the macOS arm64 /
SDL2 build. It supersedes the "current rendering pipeline" / "planned
rendering pipeline" sections in the `REV.CPP` header comment, which are
1998–2002 design notes and no longer reflect reality. The `REV.CPP` block is
retained as a historical artifact.

File paths are repo-relative. Line numbers are approximate and drift; grep
by symbol if precision matters.

## Control flow

`DEMO/REV.CPP:main()`

1. Loads `rev.cfg` → globals `g_demoXRes`, `g_demoYRes`, `g_fullScreenMode`,
   `g_playMusic`, `g_profilerActive`.
2. `SDL_Init(SDL_INIT_VIDEO)`, `SDL_CreateWindow`, `SDL_RaiseWindow`.
3. `FDS_Init(x, y, 32)` — creates font table, message buffer, sets FPU to
   low-precision, calls `VESA_InitExternal` (allocates the software
   framebuffer + Z-buffer in the `VESA_Surface` struct).
4. `SDL2_InitDisplay(window)` — wires `MainSurf->Flip` to an SDL-backed
   routine that streams the VESA surface into an `SDL_Texture` and
   `SDL_RenderCopy`s it.
5. Spawns a worker thread running `StubbedThread` → `CodeEntry`.
6. Main thread enters `SDL_WaitEvent` loop: writes `Keyboard[scancode]` on
   `SDL_KEYDOWN`/`UP`, exits on `SDL_QUIT`.

`DEMO/REV.CPP:CodeEntry()` (worker) is the demo director:

1. Initializes the `ThreadPool` (Threads.h), starts music via
   `Modplayer_*`.
2. `Initialize_Glato()` synchronously.
3. Spawns a second `std::thread` that pre-initializes the rest:
   `Initialize_City/Chase/Fountain/Crash/Greets`.
4. Runs scenes sequentially: `Run_Glato → Run_City → Run_Chase →
   Run_Fountain → Run_Crash → Run_Greets`.

Each `Run_*` owns a **blocking** `while (!Keyboard[ScESC] && Timer<...)`
loop. Some delegate to `RENDER.CPP:RunScene()`; others (e.g. `Run_Chase`)
have bespoke loops with scene-specific effects setup.

## Per-frame pipeline

The sequence inside a scene loop iteration is roughly:

| Stage                 | Where                                      | What it does                                                                                                |
|-----------------------|--------------------------------------------|-------------------------------------------------------------------------------------------------------------|
| Clear framebuffer     | `memset(VPage, 0, PageSize)` or `FastWrite`| Zero color + Z-buffer (Z-buffer lives at `VPage + PageSize`).                                               |
| Advance time          | `CurFrame = lerp(StartFrame, EndFrame, t)` | Scene-local interpolated frame cursor. `Timer` is the global clock (atomic-ish `int32_t`).                  |
| Animate               | `RENDER.CPP:Animate_Objects`               | Evaluates position/rotation/scale/FOV/roll tracks (splines from 3DS/FLD tracks) onto `Object`/`Camera`.     |
| Transform             | `RENDER.CPP:Transform_Objects`             | 4×3 FP world→view→screen, per-vertex visibility flags, backface culling, bounding-sphere culling.           |
| Light                 | `RENDER.CPP:Lighting` (default)            | Per-vertex ambient + diffuse (+ optional specular). Uses the scene's `OmniHead` list plus `Cam_HeadLight`.  |
| Sort                  | `RENDER.CPP:Radix_Sorting` (SORTS.H)       | 256-bucket 4-pass radix on `Face::SortZ`. Front-to-back (`FRONT_TO_BACK_SORTING`) to exploit the Z-buffer.  |
| Render (tiled)        | `RENDER.CPP:Render`                        | Splits screen into 6×4 tiles, enqueues `RenderInner(x1,y1,x2,y2)` per tile onto `ThreadPool`, waits all.    |
| Sprites/TBR           | `TBR_Render(CurScene)` if `Scn_SpriteTBR`  | Tile-Based-Rendering pass for sprites that weren't batched with the triangle faces. See "What's *not* done". |
| Flip                  | `Flip(Screen)` → SDL `UpdateTexture+Copy`  | Present. Motion blur path (`ScM`) renders into a blurred copy first.                                        |

### `RenderInner` per-tile

For each face in sorted order:

1. Skip if `A==B` (particle/sprite marker — handled in the non-tiled
   post-pass in `Render`) or if all three vertices share a `Vtx_Visible`
   flag (fully offscreen).
2. Choose a rasterizer:
   - `Face_Reflective` → `TheOtherBarry<OVERWRITE, TEXTURETEXTURE>` (two-texture blend)
   - otherwise → whatever is stored in `F->Filler` (bound at face setup)
3. `clipper.Render(F, filler, isEnvCoords)`.

### `FrustumClipper::Render` (FRUSTRUM/FRUSTRUM.CPP)

1. Copies the face's three vertices, copies UVs (and env-map EU/EV if
   reflective), computes perspective-divided `UZ/VZ`.
2. Clips against near → far → correctCWOrder → left → right → up → down
   planes. Clipping extends the polygon into an n-gon via `FInterpolator`.
3. If surviving and has texture → `MiplevelClipper(F, filler)`. Else
   → `filler(F, C_Scnd, C_numVerts, g_MipLevel)` direct.

### Mipmapping via subdivision (`MiplevelClipper`)

Computes texel-area / pixel-area ratio to pick a mip level.

- If `Txtr_Nomip` or `pixArea < MinSize` (`XRes*YRes*0.02`) → one level,
  no subdivision, rasterize once.
- Else estimates the mip-level range `[iml, iMl]` from the polygon's
  1/z range. If `iml == iMl` → single call at that level. If the range
  spans multiple levels → the polygon is **recursively subdivided** along
  a 1/z-midpoint cut line until each sub-piece is single-level, then each
  piece is rasterized at its own level.

This is the "mipmapping based on triangle sub-division" referenced in
conversation.

## Rasterizers

The active rasterizer is **`TheOtherBarry`** (`FILLERS/TheOtherBarry.h`),
a header-only template parameterized on:

- `TBlendMode` — `XOR` / `OVERWRITE` / `TRANSPARENT` / `ADDITIVE`
- `TTextureMode` — `NORMAL` / `TEXTURETEXTURE` (env-map overlay)

Key characteristics:

- **8×8 tile grid** (`TILE_SIZE = 8`). The rasterizer walks tile
  coordinates, not scanlines — cache-friendly and SIMD-natural.
- **AVX2 via Agner Fog's `vectorclass`** (`Vec8i` / `Vec8f` / `Vec32uc`),
  with x86 intrinsics reaching ARM NEON through `simde`. Each tile row
  processes 8 pixels per iteration.
- **Edge function tests** (`orient2d`) on integer subpixel coordinates
  (8-bit subpixel, `SUBPIXEL_MULT = 256`). Sample mask = all three edges
  ≥ 0.
- **Z-buffer** at `VPage + PageSize`, 16-bit encoded as
  `0xFF80 - round(g_zscale * z)`, compared with SIMD `>`, blended via
  `_mm_blendv_epi8`.
- **Perspective-correct texturing** via per-pixel reciprocal of
  interpolated 1/z (`approx_recipr(p_rz)`), then `u = p_uz*p_z*scale`,
  `v = p_vz*p_z*scale`.
- **Swizzled/tiled texture addressing** (`packed_tile_u/v`,
  `swizzle_umask`) — textures are stored in a Z-order-ish layout so that
  neighbouring (u,v) samples hit near-neighbour cache lines rather than
  striding through a flat row layout.
- **Modulation** — the vertex `LR/LG/LB` lighting color is
  interpolated per-pixel and multiplied into the fetched texel
  (`colorize(texture_samples, blend_color)`).
- **Blend**: `TRANSPARENT` averages with dst; `ADDITIVE` saturated-adds;
  `OVERWRITE` writes through.
- **Multi-texture** (`TEXTURETEXTURE`): fetches a second sampler and
  blends `t1 + t0/2` (saturated). Used for `Face_Reflective`
  environment-map overlays.
- Final write is `_mm256_maskstore_ps` — writes only the pixels that
  passed the edge × Z-buffer mask.

Other rasterizers in `FILLERS/`:

- `Mekalele.cpp` / `.h` — an exploratory G-Buffer-style rasterizer.
  Reachable through `RenderInnerMekalele` but not wired into the active
  hot path; kept as a jumping-off point for a future deferred-style
  rewrite.
- `IX.cpp`, `IXFZ.cpp`, `IXGZ.cpp`, `IXTGZ.cpp`, `IXTZ.cpp` — C++
  wrappers around the original 1998 .asm fillers that make them
  callable from C++. Still in use as the active entry point for
  untextured faces: `PREPROC.CPP:Assign_Fillers` binds
  `IX_Prefiller_FZ` (flat), `IX_Prefiller_GZ` (gouraud), and
  `IX_Prefiller_FAcZ` (transparent flat). All textured / transparent-
  textured / additive cases route to `TheOtherBarry` variants.
- `F4Vec.h`, `SimdHelpers.h` — SIMD utilities (`v8_from_arith_seq`,
  `gather`, `packed_tile_u`, etc.) shared across rasterizers.

## Threading

- `FDS/Threads.h` provides `ThreadPool::instance()`. Workers each run
  the init lambda passed to `ThreadPool::instance().init(...)` — that
  lambda calls `FPU_LPrecision()` so every worker has low-precision FPU
  and `InitPolyStats`.
- `Render()` enqueues 24 jobs (6×4 tiles) and waits on
  `renderns::tileCounter == numTilesX*numTilesY` with a
  condition variable in `renderns::condition`.
- Each worker thread has a `thread_local FrustumClipper clipper;`
  (RENDER.CPP top), avoiding contention on the clip buffers.
- SDL main thread only pumps events. All rendering runs on the worker
  pool, driven from the `CodeEntry` thread.

## Data model

### Scene

`FDS/Base/Scene.h` — holds linked lists (`ObjectHead`, `TriMeshHead`,
`OmniHead`, `CameraHead`, `MatHead`), frame range (`StartFrame`,
`EndFrame`), near/far clip (`NZP`, `FZP`), global flags (`Scn_Nolighting`,
`Scn_SpriteTBR`, ...).

### TriMesh / Face / Vertex

`FDS/Base/TriMesh.h` — Face-of-3-Vertex-pointers. Each `Vertex` carries:

- `Pos` (object) / `TPos` (view) — 3D positions
- `PX`, `PY`, `RZ` — screen-space projected x,y and 1/z (view z)
- `U`, `V`, `UZ`, `VZ` — texture coords and perspective-divided variants
- `EU`, `EV`, `EUZ`, `EVZ` — environment-map coords (reflective faces)
- `LR`, `LG`, `LB`, `LA` — per-vertex lighting color (modulated onto
  texels in rasterizer)
- `Flags` — visibility bits (`Vtx_VisLeft/Right/Up/Down/Near/Far/Visible`),
  used by clipper to skip early.

### Material / Texture

`FDS/Base/Material.h`, `FDS/Base/Texture.h`. Each `Material` owns a
`Texture*` with `Mipmap[numMipmaps]` pre-generated at load time
(`IMGGENR/IMGGENR.CPP`). `LSizeX`/`LSizeY` are log2 dimensions used by
the tiled addressing functions.

## Global state (audit-relevant for WASM refactor)

The per-frame mutable globals that a tick-driven rewrite would need to
either thread through a context struct or promote to TLS:

- `Timer`, `Frames`, `CurFrame`, `dTime`, `g_FrameTime` — timing
- `FList`, `SList`, `CAll`, `CPolys`, `COmnies`, `CPcls`, `Polys` — the
  per-frame face list arrays and counters (`FList_Allocate(Sc)`)
- `CurScene`, `View`, `FC` (free camera), `Cam_HeadLight` — view state
- `Keyboard[]` — input, written by SDL main thread, read everywhere
- `MsgStr[]`, `MsgClock[]`, `MsgID[]` — on-screen message queue
- `g_zscale`, `g_zscale256` — scene-dependent Z-buffer scale (set by
  `SetCurrentScene`)
- `C_FZP`, `C_rFZP`, `C_NZP`, `C_rNZP` — clip-plane caches
- `g_MipLevel` — per-filler-call scratch. Written by
  `MiplevelClipper` / `Render` once per polygon (or per sub-polygon
  during subdivision) immediately before calling `filler(F, Verts,
  nVerts, g_MipLevel)`. Rasterizers consume the `miplevel` **argument**,
  not the global, so the store itself is redundant for correctness but
  kept for debug introspection.

Mostly-const-after-init (loaded once, not mutated per-frame):

- `MatLib`, `Font1`, `Active_Font`, `MMXState`, scene trees themselves
  (meshes, materials, textures, mipmaps — only the *transformed* copies
  change per frame), identity matrices, `Phong_Mapping`
- FPU control-word state
- `XRes`, `YRes`, `BPP`, `PageSize`, `VPage` (allocation is fixed;
  contents change every frame but the pointer is stable)

## Rendering backends

`DEMO/SDL2.cpp` is the only active backend. It installs a `Flip` hook that
streams `VPage` into a single streaming `SDL_Texture` (32-bit XRGB) and
`SDL_RenderCopy`s it. The legacy DirectDraw / D3D8 / GDI backends were
removed during Tier-1 cleanup.

## What's *not* done

The engine is modern in the places people expect it to be (Z-buffer,
persp-correct, mip, multi-texture, SIMD, threaded). The known soft spots
that any future renderer-hygiene pass should consider:

- `Lighting` — three historical variants (`LightingOld`, `StaticLighting`,
  `Lighting`) with overlapping responsibilities; the hot path is a
  dense, globals-heavy function that would benefit from decomposition.
- Runtime globals footprint (see list above) — the main reason Emscripten
  port is awkward. A `FrameContext` / `RenderContext` bundle would make
  scene loops tick-driven without rewriting each `Run_*` in isolation.
- `Run_*` duplication — most scenes re-implement the ESC check + timing
  + camera switch + flip pattern with small variations; factoring a
  scene-tick primitive would reduce the surface area for future work.
- Transparent / additive sprites and transparent triangle faces
  currently render in two separate passes: the sorted opaque pass
  (radix + tiled `TheOtherBarry`) and the `TBR_Render` sprite pass.
  The split causes depth-blend artifacts when transparent geometry and
  sprites overlap. TBR itself is a keeper — it's what makes rendering
  huge particle/sprite counts tractable by exploiting cache locality,
  so the fix direction is to **fold transparent faces into the TBR
  pass**, not the other way around. That way depth-sorted blending of
  both sprites and transparent triangles shares the same cache-local
  tile walk.
