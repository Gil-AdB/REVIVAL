# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

REVIVAL / FLOOD — a 1998 demoscene production being revived on modern platforms. Three CMake targets in a single tree:

- **DEMO/** — executable. Orchestrates scenes, owns `main()` + SDL event loop.
- **FDS/** — static library. Custom software 3D engine (transform, lighting, clipper, rasterizer, model loaders, SIMD fillers).
- **Modplayer/** — thin C++ static lib that links a *Rust* `libmodplayer.a` for `.MOD`/`.XM` playback.

## Build (macOS arm64, current working config)

The active branch is `macos`; `master` is the PR target. Builds out-of-tree with Ninja:

```sh
cmake -S . -B build -G Ninja
cmake --build build
```

Default build type is `Release`; override with `-DCMAKE_BUILD_TYPE=Debug`. `CMAKE_OSX_ARCHITECTURES` defaults to `arm64` but is a CACHE variable — override for Intel Macs.

Prerequisites:

- **SDL2** discovered via `find_package(SDL2)`. On macOS: `brew install sdl2`.
- **Rust toolchain** (`cargo` on PATH) — CMake invokes `cargo build` on the modplayer workspace automatically.
- **Submodules** — the Rust workspace lives at `Modplayer/modplayer` as a submodule. Clone with `--recurse-submodules` or run `git submodule update --init --recursive` after checkout. Override with `-DMODPLAYER2_DIR=/path` to point at an external checkout (e.g. for cross-repo development).

Stale/obsolete build trees that may still be on disk from pre-Tier-1 work (`CMake/`, `cmake-build-*`, `build.local/`) are gitignored — delete them freely.

### Running

The demo **must be run from `Runtime/`** — all asset paths (`rev.cfg`, `Revival.xM`, `SCENES/`, `TEXTURES/`, `FONTS/`) are resolved relative to CWD. Either `cmake --install build` (installs into `Runtime/`) or copy/symlink the binary then:

```sh
cd Runtime && ./DEMO
```

`Runtime/rev.cfg` controls resolution, fullscreen, music, profiler, and `DisplayAPI` (kept as `GDI` historically — ignored on macOS, which always uses SDL2).

### No test or lint targets

There is no test suite, no linter, no CI. "Testing" means running the demo and watching the scenes render correctly. `DEMO/FillerTest.cpp` is a dev-only rasterizer smoke test, gated behind commented-out code in `REV.CPP`.

## Architecture

### Control flow

`DEMO/REV.CPP:main()` reads `rev.cfg`, inits SDL + FDS, then spawns `StubbedThread` which calls `CodeEntry`. `CodeEntry` is the demo director: it initializes all scenes on a worker thread (`Initialize_City/Chase/Fountain/Crash/Greets`), starts music via `Modplayer_*`, then runs scenes in sequence (`Run_Glato`, `Run_City`, `Run_Chase`, `Run_Fountain`, `Run_Crash`, `Run_Greets`). The SDL main thread only pumps events → writes `Keyboard[scancode]` — scenes poll this array to detect ESC / skip.

Each scene lives in its own `DEMO/<NAME>.CPP` + `.H` with a matching `.FLD` file in `Runtime/SCENES/` (and `Scenes/` at repo root — the latter is the authoring source, the former is what ships).

### FDS engine layout

Organized as subdirectories with source groups mirrored in `FDS/CMakeLists.txt`:

- `Base/` — core types (`Vector`, `Matrix`, `Quaternion`, `Camera`, `TriMesh`, `Scene`, `Texture`, `Material`). `FDS_VARS.H`/`FDS_DECS.H`/`FDS_DEFS.H` are the engine-wide globals/declarations/defines headers — nearly every translation unit includes these.
- `3DS/`, `FLD/`, `V3D/` — model-format readers. `.FLD` is the project's own scene format (used by the demo); `.3DS` and `.V3D` are legacy.
- `FILLERS/` — triangle rasterizers. The `.ASM` files are Win32 legacy and **not built** — the active rasterizers are the `.cpp` ports (`IX.cpp`, `IXFZ.cpp`, `IXGZ.cpp`, `IXTGZ.cpp`, `IXTZ.cpp`, `Mekalele.cpp`, `TheOtherBarry.cpp`) using SIMD via `simde` + Agner Fog's `vectorclass`.
- `RENDER/`, `FRUSTRUM/`, `Clipper.cpp` — the rendering pipeline (radix sort → frustrum clip → fill).
- `MATH/`, `IMGCODE/`, `IMGGENR/`, `IMGPROC/` — math tables, image loading (`stb_image`), procedural generation, post-processing.
- `simd/` — vendored Agner Fog Vector Class Library (do not edit, see `simd/README.md`).
- `simde/` — vendored SIMDe (maps x86 intrinsics to ARM NEON). Required for the arm64 build.

### Portability story

The preprocessor split is `PORTABLE_CODE=1` (defined for both `DEMO` and `FDS`) vs `NON_PORTABLE_CODE` (MSVC/x86-only paths: thread-local filler initializers, inline x86 asm). The macOS build compiles only the portable path. The legacy Windows display backends (DirectDraw/D3D8/GDI) were deleted in the Tier 1 cleanup; SDL2 is the only remaining backend. `SIMDE_ENABLE_NATIVE_ALIASES` lets intrinsics code read as if on x86.

### Display backend

On macOS, `DEMO/SDL2.cpp` is the only display backend. It allocates a single streaming `SDL_Texture`, lets FDS render into a software VESA-style surface, then `SDL_UpdateTexture` + `SDL_RenderCopy` on flip. The `VESA_Surface` struct (FDS) holds the software framebuffer + Z-buffer; `MainSurf->Flip` is the function-pointer hook SDL2 installs.

### Emscripten

Supported via `if (EMSCRIPTEN) add_compile_options(-msimd128 -sUSE_SDL=2)` at the top-level. `WASMEXE.ZIP` + `FDS/wasm.exe` are prior WASM build artifacts.

## Conventions to respect

- Source files are mixed UPPERCASE (`REV.CPP`, `CITY.CPP`, `FDS_VARS.H`) and CamelCase (`Config.cpp`, `SkyCube.cpp`). Keep the style of the surrounding directory when adding files; `CMakeLists.txt` source lists are explicit, so new files must be added there.
- `REV.CPP` contains a large historical dev log as a leading comment — preserve it when editing.
- Many code paths are commented-out alternative implementations (e.g. FMOD vs Modplayer, pre-SIMD asm vs `.cpp` ports). Treat these as historical reference; don't mass-delete without checking `NON_PORTABLE_CODE` gates.
- The `Original/` directory holds pristine 1998 sources — read-only reference.
