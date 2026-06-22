# REVIVAL / FLOOD

A 1998 demoscene production by **FLOOD**, brought back to life on
modern hardware. The original ran on DOS / x86 with a hand-rolled
software 3D engine. This repository keeps that engine — no GPU
shortcut — and ports it forward to modern desktops and to the
browser via WebAssembly.

> **Watch it in your browser:** <https://gil-adb.github.io/REVIVAL/>

## What it is

A demo: six minutes of synchronized 3D scenes set to a tracker
soundtrack. The renderer is a software triangle pipeline — transform,
lighting, clipping, painter's-sort, scanline rasterization — that the
original team wrote in the late 90s when shipping a demo meant being
your own hardware abstraction layer. Modern SIMD has replaced the
inline x86 assembly, but the structure of the pipeline is the one
that ran on a Pentium II.

The interesting thing about reviving software-renderer code in 2026
is that the work it does is all still there to look at. There's no
"it gets sent to the GPU and then reappears in the framebuffer"
black box — you can step through every pixel.

## What's inside

```
DEMO/         the executable — main(), SDL event loop, scene drivers
FDS/          Flood Demo System — the software 3D engine
Modplayer/    .MOD / .XM tracker playback — Rust static lib
Runtime/      assets + rev.cfg
Original/     pristine 1998 sources, kept as a read-only reference
docs/         engine deep-dive and assorted write-ups
Scenes/       scene authoring sources (the .FLD files used at runtime
              are in Runtime/SCENES/)
```

## Build & run

The actively developed target is **macOS arm64**. The CMake recipe
should also work on Linux and Windows with a working SDL2 + emscripten
+ Rust toolchain, though those aren't being smoke-tested per commit.

### Native

```sh
brew install sdl2 ninja                # macOS — apt/dnf/winget on others
git submodule update --init --recursive
make build
./build/DEMO/DEMO                      # or `make run`
```

`Runtime/rev.cfg` controls resolution, fullscreen, music, and the
profiler overlay. The binary auto-locates the asset directory at
startup, so it runs from any CWD.

### WebAssembly

```sh
make wasm                              # emcmake + ninja
make serve                             # http://localhost:8000/DEMO.html
```

Needs an emscripten toolchain on PATH (`brew install emscripten` or
`source emsdk_env.sh`) and a Rust toolchain — the modplayer crate is
built into a wasm static lib by cargo as part of the build.

### Submodules

The Rust modplayer lives at `Modplayer/modplayer` as a submodule.
Clone with `--recurse-submodules` or run
`git submodule update --init --recursive` after checkout.

## Engine notes

The engine (`FDS/`) is a software 3D pipeline that produces pixels into
a CPU-side framebuffer with a 16-bit Z-buffer alongside it. SDL2's only
job is to upload that framebuffer to a streaming texture and present
it. There is no GL/DX/Metal path anywhere.

### Per-frame pipeline

Each scene tick runs roughly:

```
clear → animate → transform → light → sort → render-tiled → flip
```

- **Animate** evaluates spline tracks (position / rotation / scale /
  FOV / roll) loaded from the scene's `.FLD` file onto each object and
  camera.
- **Transform** is a 4×3 floating-point world→view→screen pass with
  per-vertex visibility flags, bounding-sphere culling, and backface
  culling.
- **Light** is per-vertex ambient + diffuse (+ optional specular). The
  vertex colors are interpolated and modulated into texels by the
  rasterizer, not pre-baked.
- **Sort** is a 256-bucket 4-pass radix on `Face::SortZ`, front-to-back
  to feed the Z-buffer with cheap rejections.
- **Render** splits the screen into a 6×4 tile grid and dispatches each
  tile to a worker on the engine's thread pool.

### Rasterizer

The active rasterizer (`FILLERS/TheOtherBarry.h`) is a header-only
template parameterized on blend mode (overwrite / transparent /
additive / xor) and texture mode (single / dual). Highlights:

- 8×8 tiles, AVX2 lane width — eight pixels per inner-loop iteration.
- Edge-function setup with 8-bit subpixel precision, sample mask is
  the AND of three orient-2D edges.
- Z-buffer compared with a SIMD greater-than, written through
  `_mm256_maskstore_ps` so only passing pixels touch memory.
- Perspective-correct UVs via per-pixel reciprocal of interpolated
  1/z, then `u = uz × z`, `v = vz × z`.
- Textures are stored in a swizzled tile layout so neighbouring (u,v)
  samples land on neighbouring cache lines.

Mipmap selection happens at the clipper, not the rasterizer:
polygons whose 1/z range crosses multiple mip levels are recursively
subdivided along a 1/z midpoint cut until each sub-piece is
single-level. Each sub-piece is then rasterized at its chosen level.

### Cross-platform SIMD

The intrinsics in the rasterizer read as AVX2 — `_mm256_*` with Agner
Fog's `Vec8i` / `Vec8f` / `Vec32uc` over the top. They reach ARM NEON
(macOS arm64) and wasm SIMD128 (browser) through
[simde](https://github.com/simd-everywhere/simde). The same source
files compile to all three targets without `#ifdef` ladders.

### Threading

A single thread pool (`FDS/Threads.h`) is created at startup and lives
for the whole demo. The SDL main thread only pumps events and writes
keyboard state. A worker thread drives the demo director (`CodeEntry`)
which sequences scenes; another worker pre-initializes upcoming
scenes ahead of time. Each tile-render job is a pool task. Each
worker has its own thread-local `FrustumClipper` so the clip buffers
don't contend.

For the architecture in depth — clipper internals, scene/material/
mesh data layout, the audit of per-frame mutable globals — see
[`docs/ENGINE.md`](docs/ENGINE.md).

## Credits

Original 1998 production by **FLOOD**. The `Original/` tree preserves
their sources and is kept read-only.
