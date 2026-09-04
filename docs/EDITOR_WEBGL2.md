# The browser editor's WebGL2 renderer (`?editor&gpu`)

A hardware renderer for the LWO surface editor's viewport: the greets scene ingested through the
GpuBench scene reader (`GpuBench/SceneIngest.cpp`, the same ingest the Metal bench uses), drawn by
`DEMO/GpuWeb.cpp` as a WebGL2 deferred pipeline (G-buffer, spot shadow maps, deferred resolve,
volumetric cones, forward transparents, env reflection) instead of the software rasterizer.

It began in Gil-Ad's Antigravity session of 2026-09-03 ("can you create a version of the demo using
the gpu on web?"), was reviewed and reworked on `rev-gpuweb` on 2026-09-04, and landed on `fog-wt`.

## What it is not

It is a **third implementation**, beside the CPU engine and the Metal bench, not an authority
(see `docs/SHADING_CONTRACT.md` on the GPU's standing). Its parity with the CPU frame is
**asserted from screenshots, not measured**: the CPU editor viewport carries the profiler overlay
and its own fog/tonemap, so no pixel metric between the two pages has been established yet.
Treat every "matches the CPU" statement about it as a claim until a headless numeric comparison
exists.

## Isolation rules (why the native binary does not know it exists)

- `GpuWeb.cpp`, `GpuWeb.h` and `GpuBench/SceneIngest.cpp` are compiled **only under EMSCRIPTEN**
  (`DEMO/CMakeLists.txt`). The native `DEMO` links no `GpuWeb` or `gpubench` symbol (`nm` finds
  none), and the greets pins and `tools/render_gate.sh` are byte-identical with it in the tree.
- `MainLoop.cpp` (wasm-only file) drives it; `SDL2.cpp`'s `Wasm_InitGL` creates the WebGL2
  context through emscripten (with a depth buffer) **only when `Module.revGpuMode` is set**; the
  CPU page keeps its original JS-side context, no depth buffer.
- The engine exposes two wasm-only hooks in `GREETS.CPP` (`Greets_GetDynamicScreenLinearBuffer`,
  `Greets_RenderDynamicScreen`, inside `#ifdef __EMSCRIPTEN__`) and one read-only accessor in
  `GreetsDisco.cpp` (`fds::GreetsDiscoPanoTexture()`) for the env map. Nothing else in FDS/DEMO
  changes for it.
- `?gpu` is a **page mode**, parsed once by `shell.html` into `Module.revGpuMode` (the same
  pattern as `?editor` → `revEditorMode`). It is not a FeatureFlag; the URL-token → flag parser
  drops both words.
- `gpubench::Light` gained `shadowSlot` (which of GpuWeb's 8 spot shadow maps a light owns this
  frame). The Metal bench keys its maps by light index and never reads it; `shadowRes` keeps its
  meaning (a resolution).

## Texture ingest, and why it reads the files again

Inside the running demo the in-memory textures have been through scene init: tiled 4×4 "waffle"
mips, and `AttachMatToScene` rewrites `Flags`, so `Txtr_Tiled` is not trustworthy in either
direction (the floor said tiled and was linear; `P_TEXT.JPG` said linear and was tiled).
`acquireTexture` therefore reads every **file-backed** texture again into a private linear copy
for the GPU (freed with `delete[]`: every loader in `IMGCODE.CPP` now allocates `Texture::Data`
with `new[]`, `LoadPNG` was the one `malloc`). Procedural textures keep the flag and are
unswizzled when it says tiled. The **bench path** (data not loaded yet) is unchanged: load into
the engine object, expand as linear.

The greets screen (`P_TEXT.JPG`) is dynamic: while the editor plays, each frame re-renders it
on the CPU (`Greets_RenderDynamicScreen`) and uploads the linear 256×256 buffer with
`glTexSubImage2D`.

## The cone constants come from the CPU's flags

The volumetric cone pass is the Metal bench's `fs_cones` term for term (which is the CPU's
analytic cone with one shadow tap per segment): the exact distance-attenuation integral along the
chord in 8 segments, the "N × mean" calibration, the midpoint softness. Its three constants are
uploaded per frame from FeatureFlags, live in the same process:

    density   = cone_strength * 1e-3 * (hdr_cone_softknee ? 1 : hdr_glow_scale)
    nSamples  = vol_n_samples
    fadeFloor = 12 * farZ * 1.1 / 65280        (the CPU's 12 z16 quanta, in world units)

Greets sets `cone_strength` to 1.2 in `GreetsDisco.cpp`; nothing in the shader is a hand-tuned
brightness constant any more.

## Build, run, look

```sh
cd /Users/gil-ad/work/revival-fog
emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-wasm DEMO.html
python3 tools/editor_server.py --port 8099
# CPU:  http://localhost:8099/DEMO.html?editor
# GPU:  http://localhost:8099/DEMO.html?editor&gpu     (the ⚡ GPU button toggles the same thing)
```

Headless, nothing on screen (Chrome `--headless=new` through CDP; the script lives in the
2026-09-04 session's evidence, `docs/evidence/editor_webgl/shoot.js`):

```sh
node docs/evidence/editor_webgl/shoot.js "http://localhost:8099/DEMO.html?editor&gpu" /tmp/gpu.png /tmp/gpu.log 600
```

## Status on 2026-09-04

Verified: native byte-null (pins + gate), wasm builds, the Metal bench builds with the shared
ingest change, the GPU page ingests greets (textures clean, 21 lights, 8 spot shadow maps, the
disco panorama as env map), renders paused and playing without shader errors. Not verified:
parity with the CPU frame (no metric yet), the Metal bench's picture after the ingest change
(builds; not re-rendered), and anything on the M5.
