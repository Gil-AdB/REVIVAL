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
   `g_playMusic`, `g_profilerActive`, `g_HiDPI`.
2. `SDL_Init(SDL_INIT_VIDEO)`, `SDL_CreateWindow` (with
   `SDL_WINDOW_RESIZABLE`, plus `SDL_WINDOW_ALLOW_HIGHDPI` when `HiDPI=1`).
3. `FDS_Init(x, y, 32)` — creates font table, message buffer, sets FPU to
   low-precision, calls `VESA_InitExternal` (just sizes a stub
   `VESA_Surface`; the actual framebuffer is allocated by the SDL backend).
4. `SDL2_InitDisplay(window)` — creates the renderer, calls `V_Create`
   (allocates the engine `SDL_Texture` and a separate `Z16` malloc),
   installs `MainSurf->Flip = V_Flip`, locks the engine texture and points
   `MainSurf->Data` straight at the lock pointer (zero-copy framebuffer
   write). Calls `VESA_VPageExternal` → `VESA_Surface2Global` to publish
   `XRes/YRes/PageSize/VPage/ZPage16/YOffs` etc.
5. Spawns a worker thread running `CodeEntry`.
6. Main thread enters `SDL_WaitEvent` loop: writes `Keyboard[scancode]` on
   `SDL_KEYDOWN`/`UP`, captures `SDL_WINDOWEVENT_SIZE_CHANGED`/`RESIZED`
   and publishes the new pixel size to `g_pendingResize` (atomic; see
   "Resize" below), exits on `SDL_QUIT`.

`DEMO/REV.CPP:CodeEntry()` (worker) is the demo director:

1. Initializes the `ThreadPool` (FDS/Threads.h) — every worker runs an init
   lambda that calls `FPU_LPrecision` + `InitPolyStats` and is QoS-hinted
   to a P-core via `HintHighPerfThread` on macOS.
2. Starts music via `Modplayer_*`.
3. `Initialize_Glato()` synchronously.
4. Spawns a second `std::thread` (`t1`) that pre-initializes the rest:
   `Initialize_City/Chase/Fountain/Crash/Greets`. `Initialize_City` does
   off-screen cube-map renders by temporarily swapping `MainSurf` to a
   stack-local `TmpSurf` — that swap holds `g_engineSurfaceMutex` so a
   resize landing on the demo thread can't `memcpy` the SDL surface back
   into `MainSurf` mid-render.
5. Runs scenes sequentially: `Run_Glato → Run_City → Run_Chase →
   Run_Fountain → Run_Crash → Run_Greets`.

Each scene is now a `SceneDriver` (DEMO/SceneTick.h) with three phases —
`init()`, `tick()` (returns `false` to stop), `cleanup()` — plus a virtual
`on_resize(int newX, int newY)` for resolution-dependent state (CITY's
`dispMap`, FOUNTAIN's TBR span buffer, GLAT's grid arrays). The `Run_*`
functions are thin shims that allocate the driver and call
`runSceneBlocking(*scene)`, which loops `poll_pending_resize` →
`driver.tick()` until `tick()` returns `false`. `SceneSequence` is the
multi-scene equivalent (one factory per scene, advances on tick boundary)
and is the shape an emscripten `set_main_loop` driver would use; native
currently still runs each `Run_*` blockingly back-to-back.

## Per-frame pipeline

The sequence inside a scene loop iteration is roughly:

| Stage                 | Where                                      | What it does                                                                                                |
|-----------------------|--------------------------------------------|-------------------------------------------------------------------------------------------------------------|
| Clear framebuffer     | `parallel_memset(VPage, …)` + `parallel_memset(ZPage16, …)` | Zero color + Z-buffer. Two **separate** allocations now (Z16 used to live at `VPage + PageSize`; split out so the engine can render straight into a locked `SDL_Texture`). Split across `ThreadPool` workers via `FDS/Threads.h:parallel_memset`. |
| Advance time          | `CurFrame = lerp(StartFrame, EndFrame, t)` | Scene-local interpolated frame cursor. `Timer` is the global clock (atomic-ish `int32_t`).                  |
| Animate               | `RENDER.CPP:Animate_Objects`               | Evaluates position/rotation/scale/FOV/roll tracks (splines from 3DS/FLD tracks) onto `Object`/`Camera`.     |
| Transform             | `RENDER.CPP:Transform_Objects`             | 4×3 FP world→view→screen, per-vertex visibility flags, backface culling, bounding-sphere culling.           |
| Light                 | `RENDER.CPP:Lighting` (default)            | Per-vertex ambient + diffuse (+ optional specular). Uses the scene's `OmniHead` list plus `Cam_HeadLight`.  |
| Sort                  | `RENDER.CPP:Radix_Sorting` (SORTS.H)       | 256-bucket 4-pass radix on `Face::SortZ`. Front-to-back (`FRONT_TO_BACK_SORTING`) to exploit the Z-buffer.  |
| Render (tiled)        | `RENDER.CPP:Render`                        | Splits screen into **6×5 = 30** tiles (`--frame_tile_x` / `--frame_tile_y`, default 6×5), work-steals `RenderInner(x1,y1,x2,y2)` per tile across the `ThreadPool`, waits all. |
| Sprites/TBR           | `TBR_Render(CurScene)` if `Scn_SpriteTBR`  | Tile-Based-Rendering pass for sprites that weren't batched with the triangle faces. See "What's *not* done". |
| Flip                  | `Flip(Screen)` → `SDL_UnlockTexture` + `RenderCopy` (engine) or `UpdateTexture+RenderCopy` (child surfaces) | Present. Engine surface is dual-mode lock-render: writes go straight into the `SDL_Texture` lock pointer, V_Flip just unlocks/presents/re-locks for the next frame. See "Display backend". Motion blur path (`ScM`) renders into a blurred copy first. |

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
- **Z-buffer** in its own allocation (`ZPage16`, separate `word*` malloc;
  used to be the tail of `VPage`), 16-bit encoded as
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
  and `InitPolyStats`. `HintHighPerfThread` runs in the worker prologue
  to bias the macOS scheduler toward P-cores.
- `Render()` covers the screen with **30 tiles (6×5)** and hands them to
  `dispatchIndexed`, which enqueues one chunk task per worker rather than
  one per tile; the tile kernels release `renderns::tileDone` and the
  caller drains 30 permits. The grid is `--frame_tile_x` /
  `--frame_tile_y` (default 6×5, capped at 24×24). Tile sizes are rounded
  DOWN to a multiple of 8 (`& ~7`) and the last row/column absorbs the
  remainder, so every *interior* boundary stays 8-aligned at any grid —
  that alignment is what keeps two adjacent clipper workers off the same
  8×8 `blendv` read-modify-write. **This is not the 12×8 deferred
  *lighting* grid** (`DEFERRED_NUM_TILES_X/Y`); confusing the two is a bug
  the project has actually shipped (see `--xpar_tile_lights`).
- Each worker thread has a `thread_local FrustumClipper clipper;`
  (RENDER.CPP top), avoiding contention on the clip buffers.
- `parallel_memset(p, v, n)` (FDS/Threads.h) splits across the pool above
  a 256 KiB threshold and is used for per-frame color and Z clears (CITY
  uses it directly; `SkyCube`'s clear path also routes through it). Sync
  is a `shared_ptr<atomic<size_t>>` countdown — capturing by value so the
  caller's stack can't be UAF'd if a worker notifies after the predicate
  fires (regression seen in 60d0f46).
- SDL main thread only pumps events (input + window-resize publish to
  `g_pendingResize`). All rendering runs on the worker pool, driven from
  the `CodeEntry` thread.

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

#### Per-face vs per-vertex UVs — read UVs from the FACE, not the vertex

UVs exist in **two** places: per-vertex (`Vertex::U/V`) and per-face
(`Face::U1/V1, U2/V2, U3/V3`). **For any geometric derivation (tangents,
UV gradients, projection math) read the per-FACE `U1..V3`, never the
per-vertex `A->U/B->U/C->U`.**

Why: UVs are not stored in the `.lwo`/`.FLD` — they're computed at load by
`Get_UV`/`Get_Mapping` (`FLD/FLD_MAT.CPP`) from the material's projection
(Planar/Cubic/Cylindrical/Spherical). `Get_UV` writes the per-vertex `U/V`
*and* snapshots them into the face's `U1..V3`. But a vertex **shared**
between faces of different projection orientation (e.g. a box corner where
a +X wall meets a +Z wall) has its per-vertex `U/V` **clobbered by
whichever face is mapped last** — so it's correct for only one of the
sharers. The per-face `U1..V3` snapshot is taken at map time and is always
correct; it's what the rasterizer uses for the albedo (which is why a
mismatched per-vertex UV corrupts only derived data like tangents, while
the texture itself looks fine).

This bit `Compute_Vertex_Tangents` (`MISC/PREPROC.CPP`): reading per-vertex
UVs gave shared-corner faces a flipped tangent → a diagonal normal-map
relief seam through wall quads, visible only with a directional normal
map. Fixed by deriving the UV gradient from `U1..V3` (with a fallback to
per-vertex when the per-face triangle is degenerate, for procedural meshes
that set only per-vertex UVs).

### Material / Texture

`FDS/Base/Material.h`, `FDS/Base/Texture.h`. Each `Material` owns a
`Texture*` with `Mipmap[numMipmaps]` pre-generated at load time
(`IMGGENR/IMGGENR.CPP`). `LSizeX`/`LSizeY` are log2 dimensions used by
the tiled addressing functions.

#### Hand-built textures must be block-tiled ("shachletz") — `Convert_Image2Texture` is not enough

The rasterizers (`TheOtherBarry`, `Mekalele`) **always** sample textures
in a block-tile **swizzled** layout via `packed_tile_u/v` +
`swizzle_umask` (`FDS/FILLERS/SimdHelpers.h`). The data behind
`Mipmap[level]` must be stored in that interleaved order or every fetch
lands on the wrong texel.

`Convert_Image2Texture` (`FDS/IMGPROC/Imgproc.cpp`) does **not** produce
that layout — it only resamples to 256×256 and converts BPP, leaving the
pixels **linear / row-major**. The block-tiling is a **separate** step,
done by either `Sachletz(data, w, h)` (`FDS/IMGGENR/IMGGENR.CPP`, the
standalone in-place swizzle most call sites use — disco, mirrors, RTTs,
scene-builder, skybox, env-bake) or `Generate_Mipmaps(Tx,
DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, enableMip)`
(`FDS/IMGCODE/IMGCODE.CPP`, which also builds the mip chain), with the
`Txtr_Tiled` flag. Both produce the same 4×4-block, X-outer/Y-inner
layout. The disk-load path runs `Generate_Mipmaps`; code that bakes a
texture by hand and stops after `Convert_Image2Texture` (or just sets
`Mipmap[0] = Data; numMipmaps = 1`) ships **linear data read as
swizzled** → the texture renders as evenly-spaced repeated cells (looks
like 4× UV tiling, but the UVs and mip level are correct — the *bytes*
are in the wrong order).

Once correctly tiled, UV→texel mapping is the **standard** `U →
texture-column`, `V → texture-row`. Any baker that reads its UVs as
"swapped" (e.g. the fountain bolt's `UZ → texture-Y` comment) was
silently compensating for the un-tiled bug and bakes its source image
transposed — do **not** copy that as a convention.

Use the consolidated helper `Scene_MakeTiledTexture(w, h, pixels,
buildMips)` (`DEMO/MeshOps.h`) for any hand-built texture; it does the
`Convert_Image2Texture` → `Txtr_Tiled` → `Generate_Mipmaps` dance in one
call.

### VESA_Surface

`FDS/Base/FDS_VARS.H:352`. Holds the per-surface state — `Data`
(framebuffer pointer; for the engine surface this is the SDL_Texture's
locked pixel buffer, refreshed each frame in `V_Flip`), `Z16`
(separate malloc for the 16-bit depth buffer; used to live at the tail
of `Data`), `X/Y/BPP/CPP/BPSL/PageSize`, perspective ratios, the
`Flip` callback (set to `V_Flip`), `YTable` (Y-offset lookup), and
`Handle`/`Renderer` (the SDL_Texture and SDL_Renderer pointers).
`Flags` carries `VSurf_LockRender` for the engine surface so `V_Flip`
takes the lock-render branch and skips `SDL_UpdateTexture`.

## Resize coordination

Resize crosses three threads (SDL main / demo / pool worker), so it's
buffered through an atomic and a mutex (DEMO/Resize.h, DEMO/Resize.cpp):

- `std::atomic<uint64_t> g_pendingResize` — packed `(X, Y)`. SDL main
  thread `store`s on `SDL_WINDOWEVENT_SIZE_CHANGED`/`RESIZED`. Demo
  thread `exchange`s at frame top via `poll_pending_resize` (called from
  inside `runSceneBlocking` and `SceneSequence::tick`).
- `std::mutex g_engineSurfaceMutex` — held during the engine's surface
  swap (`SDL2_HandleResize` and `Initialize_City`'s cube-map render).
  `EngineResize` uses `try_lock` and re-publishes the size into
  `g_pendingResize` if the lock is contended, so the demo thread never
  blocks for the ~10 s of a wasm cube-map render.
- Engine dimensions are clamped to the demo's authoring AR
  (`g_demoXRes/g_demoYRes`) and snapped down to a multiple of
  `TILE_SIZE` (8). `TheOtherBarry::apply_exact` walks 8 rows per tile
  unconditionally — non-/8 height steps past the Z-buffer end. The
  window itself can be any size; `V_Flip` letterboxes the 0–7 px slack
  by filling only the bar regions (not a full-renderer clear, which was
  ~20 MB/frame on the wasm software renderer).
- `SDL2_HandleResize` (DEMO/SDL2.cpp) frees + reallocates the engine
  `SDL_Texture`, `Z16`, and `MainSurf->YTable`, then calls
  `VESA_VPageExternal` → `Build_YOffs_Table` + `VESA_Surface2Global` to
  republish `XRes/YRes/PageSize/VPage/ZPage16/CntrX/CntrY/BPSL/g_fontScale`.
- Each scene that owns resolution-dependent state overrides
  `SceneDriver::on_resize`: CITY rebuilds `dispMap` (it bakes the
  current `XRes` row stride) and `backBuffer`; FOUNTAIN rebuilds the
  blur backing page and TBR `SBufferHead` (sized by `YRes / TILESIZE`);
  GLAT rebuilds `Page1-4 / FinalPage / FinalSurf`'s child
  `SDL_Texture`, reloads + re-Scales `LogoImage`, and rebuilds the
  grid arrays.

## Global state (audit-relevant for any future tick-context refactor)

The per-frame mutable globals that a `FrameContext`/`RenderContext`
rewrite would need to either thread through a struct or promote to TLS:

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

Surface globals — **mutate on resize**, and `VPage` additionally cycles
each frame in lock-render mode:

- `XRes`, `YRes`, `XRes_1`, `YRes_1`, `CntrX`, `CntrY`, `CntrEX`,
  `CntrEY`, `BPP`, `CPP`, `BPP_Shift`, `VESA_BPSL`, `PageSize`,
  `PageSizeW`, `PageSizeDW`, `YOffs` — re-published by
  `VESA_Surface2Global` on every resize.
- `VPage` (color framebuffer) — points at the SDL_Texture's locked
  pixel buffer in lock-render mode; updated each frame in `V_Flip`'s
  re-lock step (in practice the SW renderer returns the same pointer,
  but the engine must not assume so).
- `ZPage16` — 16-bit Z-buffer, separate `word*` malloc, freed +
  reallocated on resize.
- `g_fontScale` — auto-doubles to 2 at `XRes >= 1600` so HiDPI overlays
  remain legible; per-line +15 advance in callers also multiplied.
- `g_pendingResize`, `g_engineSurfaceMutex` — the cross-thread resize
  channel itself (see "Resize coordination").

Mostly-const-after-init:

- `MatLib`, `Font1`, `Active_Font`, `MMXState`, scene trees themselves
  (meshes, materials, textures, mipmaps — only the *transformed* copies
  change per frame), identity matrices, `Phong_Mapping`
- FPU control-word state

## Rendering backends

`DEMO/SDL2.cpp` is the only active backend (native: a streaming
`SDL_Texture`; wasm: a small WebGL2 present path — see "Wasm-specific
architecture" below). The legacy DirectDraw / D3D8 / GDI backends were
removed during Tier-1 cleanup.

`V_Flip` is dual-mode:

- **Lock-render** (engine surface, `VSurf_LockRender` flag set in
  `V_Create`): the engine writes directly into the `SDL_Texture`'s
  locked pixel buffer for the duration of a frame. `V_Flip` only needs
  to draw the resolution overlay, `SDL_UnlockTexture` (commit),
  letterbox-fill the bar regions, `SDL_RenderCopy` + `Present`, then
  `SDL_LockTexture` again and republish `VS->Data` + the surface
  globals via `VESA_Surface2Global` for the next frame's writes. No
  per-frame `SDL_UpdateTexture` memcpy. Saves ~8 ms/frame at HiDPI.
- **Update-texture** (child surfaces — currently just GLAT's
  `FinalSurf`): `VS->Data` is a separate malloc'd buffer that the
  scene's compositing path writes to; `V_Flip` does
  `SDL_UpdateTexture(handle, NULL, Data, BPSL)` and then
  letterbox+RenderCopy. Child surfaces must **not** trigger
  `VESA_Surface2Global` in `V_Flip` — that would clobber engine
  globals with the child's dimensions (see commit 9668bae for the
  regression that motivated the dual-mode split).
## Wasm-specific architecture

The wasm build diverges from native in three places: the main loop, the
present path, and audio. All three are gated by `#ifdef __EMSCRIPTEN__`;
native paths are untouched.

### Main loop on the browser main thread

`main()` runs on the actual browser main thread (no `-sPROXY_TO_PTHREAD`).
`DEMO/MainLoop.cpp` owns a state machine (`WAIT_GESTURE → RUN_GLATO →
RUN_CITY → ...`) driven by `emscripten_set_main_loop`. The state machine
walks the existing `SceneDriver` factories one frame at a time. Heavy
init (`Initialize_City`, `Fountain`, etc.) still runs off-main: a
`std::thread` spawned at boot does all `Initialize_*` in sequence,
signalling per-scene atomic ready flags as it completes.

This was the right answer after a multi-day investigation showed that
worker-thread WebGL via OffscreenCanvas + `emscripten_webgl_commit_frame`
silently fails on modern Chromium/Firefox (emscripten#17816, #23806):
GL operations succeed, the placeholder canvas never composites. Browser-
main rendering is the only reliably-working path; sub-millisecond FLIP
times confirm we don't pay any proxy cost.

### Present path (WebGL2)

`Wasm_PresentGL` in `SDL2.cpp` uploads `VPage` to a streaming RGBA8
texture via `texSubImage2D` and draws a fullscreen triangle strip. The
fragment shader handles BGRA → RGBA swizzle (the engine writes ARGB8888
packed, which is BGRA byte order) and forces `alpha = 1.0` (the
rasterizer never writes alpha). Letterboxing is done via CSS on the
canvas element rather than in-shader.

Sub-pieces worth knowing:

- The vertex shader synthesizes the quad from `gl_VertexID` so the VAO
  needs no real attribute data, but a 4-float dummy VBO bound to
  `location = 0` is required to keep desktop GL drivers (Mac in
  particular) off the slow attrib-emulation path.
- `uFade` uniform multiplies sampled colour, used by `MainLoop`'s
  `FADE_OUT` state to animate the end-of-Greets transition to black.
- Native build keeps the SDL_Renderer + SDL_Texture path; the WebGL2
  code is wasm-only.

### Audio (AudioWorklet, not SDL audio)

SDL2's emscripten port still uses `ScriptProcessorNode` (deprecated; runs
the audio callback on browser main, gets starved by heavy rAF ticks at
HD). We bypass it on wasm.

`DEMO/audio-worklet.js` is an `AudioWorkletProcessor` with a 2-second
Float32 ring buffer per channel. It signals `'needData'` when its ring
drops below 50 ms; the main thread responds by calling
`Modplayer_FillBufferPlanar` (a planar-output FFI we added to modplayer-
lib) and posting the chunk to the worklet's port. Audio decode + playback
live entirely on the audio thread; main is just a forwarder.

The AudioContext setup is gated on a real user-gesture call stack — the
state machine's `WAIT_GESTURE` runs from rAF, which has no gesture
context, so `shell.html` listens for the first `pointerdown` / `keydown`
directly and pre-arms `_floodAudioStart` from the gesture handler.

### Cross-origin isolation

We use `coi-serviceworker` to inject the `COOP` + `COEP` headers needed
for `SharedArrayBuffer` (which the pthread render workers depend on).
`shell.html` overrides `coi.coepCredentialless` to `false` on Firefox
Android, where the credentialless top-level isolation path doesn't
activate; require-corp works there.

If isolation fails entirely (private browsing, very old browser, some
mobile setups), `shell.html` shows a friendly error overlay instead of
the cryptic `WebAssembly.Memory cannot be serialized` stack trace,
including a captured `[coop-diag]` / `[audio-diag]` log so the user can
report what state they're in.

### Build flags

In `DEMO/CMakeLists.txt`:

- `-pthread` + `-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency+4`:
  ThreadPool render workers are real Web Workers.
- `-sALLOW_BLOCKING_ON_MAIN_THREAD=1`: the rasterizer dispatches tile
  jobs and waits on a condvar — that's a sync block on main by design.
- `-sEXIT_RUNTIME=0`: main returns after registering the rAF callback;
  the runtime stays alive for the callback to keep firing. End-of-demo
  cleanup is detached on a worker thread so the runtime doesn't drag.
- `-sUSE_SDL=2` + `--preload-file Runtime`: we still use SDL2 for window
  creation and event delivery, just not for audio or the present path.

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
