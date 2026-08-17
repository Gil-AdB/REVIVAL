# Building REVIVAL / FLOOD on Windows

Two complete paths. **Path A (MSVC / Visual Studio 2022)** is the one to use on
a Windows box. **Path B (MinGW-w64)** is the one that was actually exercised
while writing this document — as a *cross*-build from macOS — so it is the path
whose every step has been executed at least once.

Every step below is labelled:

| label | meaning |
|---|---|
| **TESTED** | executed end-to-end during the port, on macOS, cross-compiling to `x86_64-w64-mingw32`. The command ran and did what the text says. |
| **UNTESTED** | reasoned from the code and the toolchain docs. Not executed. It may be wrong. |

Nothing in this file has been run **on Windows**. The honest summary is at the
bottom under [What remains untested](#what-remains-untested).

---

## 0. What you are building

- `DEMO.exe` — the demo executable. Console subsystem (it prints diagnostics),
  SDL2 for the window and audio device.
- `FDS` — static library, the software 3D engine.
- `modplayer` — a **Rust** static library built by `cargo`, for `.XM` playback.

There is no Windows-specific rendering backend. The 1998 DirectDraw/D3D8/GDI
paths were deleted years ago; SDL2 is the only display backend on every
platform, and `rev.cfg`'s `DisplayAPI=GDI` line is **ignored** (it is kept for
historical reasons only).

### x86-64 requires AVX2

The rasterizers and the deferred lighting kernels are written against `_mm256_*`
intrinsics. On arm64 those reach NEON through the vendored `simde` shim; on
x86-64 `SIMDE_ENABLE_NATIVE_ALIASES` resolves them to the *real* Intel
intrinsics, and a compiler will not inline an AVX2 intrinsic into a function
built without the matching flag. The build therefore sets `/arch:AVX2` (MSVC)
or `-mavx2 -mfma` (GCC/clang) automatically for x86-64 targets — see the
`x86-64 instruction set floor` block in the top-level `CMakeLists.txt`.

**Consequence: the produced `DEMO.exe` needs a Haswell (2013) or newer CPU.**

---

## Path A — Visual Studio 2022 / MSVC (recommended)

> **Which compiler?** The audit and the fixes below target **MSVC proper**
> (`cl.exe`). Everything that was clang-only in the build system is now
> gated per-compiler, and everything that was POSIX-only in the sources has
> an MSVC shim (`FDS/Base/WinCompat.h`). `clang-cl` is also wired up (the
> CMake logic detects it via `CMAKE_CXX_COMPILER_FRONTEND_VARIANT`) and is a
> reasonable fallback if MSVC surprises you — but MSVC is the intended
> target, so try it first.
>
> Both are **UNTESTED**: no MSVC compiler was available to this port.

### A.1 Prerequisites — UNTESTED

1. **Visual Studio 2022** (17.8 or newer) with the *Desktop development with
   C++* workload, **or** the standalone *Build Tools for Visual Studio 2022*.
   The tree is C++20 (`set(CMAKE_CXX_STANDARD 20)`), which 17.8's MSVC handles.

2. **CMake ≥ 3.16** and **Ninja**. Both ship inside the VS workload; the
   `x64 Native Tools Command Prompt for VS 2022` puts them on `PATH`. Otherwise
   `winget install Kitware.CMake Ninja-build.Ninja`.

3. **Rust, MSVC toolchain**:
   ```bat
   winget install Rustlang.Rustup
   rustup default stable-x86_64-pc-windows-msvc
   ```
   `cargo` must be on `PATH` — CMake calls it directly
   (`find_program(CARGO_EXECUTABLE cargo REQUIRED)`).

4. **Git** with submodule support.

5. **SDL2 development libraries** — see A.3.

### A.2 Clone — UNTESTED (the same commands are TESTED on macOS)

The Rust player lives in a submodule. A clone without it **fails at configure
time** with an explicit error from `Modplayer/CMakeLists.txt`.

```bat
git clone --recurse-submodules <repo-url> revival
cd revival
```

Already cloned without `--recurse-submodules`:

```bat
git submodule update --init --recursive
```

### A.3 SDL2 — UNTESTED

**Option 1: vcpkg** (integrates with CMake's `find_package`)

```bat
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install sdl2:x64-windows
```

then add to the configure line:

```
-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

**Option 2: the official prebuilt VC package** — download
`SDL2-devel-2.32.10-VC.zip` from
<https://github.com/libsdl-org/SDL/releases> (the **2.x** line — this tree is
SDL2, *not* SDL3), unzip to e.g. `C:\SDL2`, and add to the configure line:

```
-DCMAKE_PREFIX_PATH=C:/SDL2/SDL2-2.32.10
```

`DEMO/CMakeLists.txt` does `find_package(SDL2 REQUIRED)` and links
`SDL2::SDL2`. Both options above provide that target.

> **`SDL2main` is deliberately not linked.** The build defines
> `SDL_MAIN_HANDLED` globally on Windows (top-level `CMakeLists.txt`) because
> `main()` in `DEMO/REV.CPP` is `int main(int, const char**)`, a signature
> SDL's `SDL_main` prototype rejects, and because the demo wants to stay a
> console program. `main()` calls `SDL_SetMainReady()` first thing instead.

### A.4 Configure + build — UNTESTED

From the **x64 Native Tools Command Prompt for VS 2022**, at the repo root:

```bat
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=C:/SDL2/SDL2-2.32.10
cmake --build build
```

Notes:

- Default build type is **Release** (`-O3`-equivalent + LTO). Override with
  `-DCMAKE_BUILD_TYPE=RelWithDebInfo` or `Debug`.
- Thin LTO is enabled for Release / RelWithDebInfo via `check_ipo_supported()`.
  If MSVC's `/GL` + `/LTCG` gives you trouble, `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF`
  turns it off.
- `cargo` runs automatically as part of the build (it is wired as a custom
  command that re-runs every build and no-ops in ~0.06 s when nothing changed).
- Building from the canonical `build/` directory **auto-copies `DEMO.exe` into
  `Runtime\`** after every link, so `Runtime\DEMO.exe` is always fresh. Other
  build dirs deliberately do not (they'd clobber it with an instrumented
  binary). `cmake --install build` does the same copy explicitly.

### A.5 Run — UNTESTED

```bat
cd Runtime
DEMO.exe
```

See [Runtime notes](#runtime-notes).

---

## Path B — MinGW-w64

This is the path that was actually driven during the port. **It was driven as a
cross-build from macOS**, which validates the *compilation and linking* of every
translation unit against the Windows headers and ABI. It does **not** validate
that the resulting `.exe` runs — no Windows machine was involved.

### B.1 Native MinGW-w64 on Windows (MSYS2) — UNTESTED

```bash
# In an MSYS2 UCRT64 shell
pacman -S --needed mingw-w64-ucrt-x86_64-gcc \
                   mingw-w64-ucrt-x86_64-cmake \
                   mingw-w64-ucrt-x86_64-ninja \
                   mingw-w64-ucrt-x86_64-SDL2 \
                   git
# Rust with the GNU ABI (must match the C++ ABI)
rustup default stable-x86_64-pc-windows-gnu
```

```bash
git clone --recurse-submodules <repo-url> revival && cd revival
cmake -S . -B build -G Ninja
cmake --build build
cd Runtime && ./DEMO.exe
```

The GNU-ABI Rust toolchain is **required** here — mixing
`x86_64-pc-windows-msvc` Rust with MinGW C++ will not link. `Modplayer/CMakeLists.txt`
picks the archive name (`libmodplayer.a` vs `modplayer.lib`) from the ABI, so
nothing else needs changing.

### B.2 Cross-build from macOS or Linux — TESTED

This is the recipe that was executed.

**Prerequisites — TESTED**

```sh
brew install mingw-w64          # provided x86_64-w64-mingw32-g++ (GCC) 15.2.0
rustup target add x86_64-pc-windows-gnu
```

**SDL2 MinGW development package — TESTED**

```sh
curl -fsSLO https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.tar.gz
tar xzf SDL2-devel-2.32.10-mingw.tar.gz
# the tree CMake wants is the per-triple subdirectory:
#   SDL2-2.32.10/x86_64-w64-mingw32
```

**Configure + build — TESTED**

```sh
cmake -S . -B build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake \
  -DCMAKE_PREFIX_PATH=/abs/path/to/SDL2-2.32.10/x86_64-w64-mingw32
cmake --build build-win
```

`cmake/toolchain-mingw-w64.cmake` (added by this port) sets
`CMAKE_SYSTEM_NAME=Windows`, finds the `x86_64-w64-mingw32-*` tools, and sets
`RUST_TARGET=x86_64-pc-windows-gnu` so `Modplayer/CMakeLists.txt` passes
`--target` to cargo and looks for the artifact in
`target/x86_64-pc-windows-gnu/release/`.

**Where the cross-build got to:** see
[Cross-compile status](#cross-compile-status) below.

---

## Runtime notes

### You must run from `Runtime/`… but the binary insists anyway

Every asset path (`rev.cfg`, `REVIVAL.XM`, `SCENES\`, `TEXTURES\`, `FONTS\`) is
resolved relative to the current directory. `main()` therefore chdirs to the
asset root **itself**, before anything else: `ChdirToAssetRoot()` in
`DEMO/REV.CPP` takes the *binary's own* directory (via `GetModuleFileNameA` on
Windows — **UNTESTED**; `_NSGetExecutablePath` on macOS — TESTED) and tries
`<bindir>`, `<bindir>\..\Runtime`, `<bindir>\..\..\Runtime`, taking the first
that contains `rev.cfg`.

Two consequences worth internalising:

- `DEMO.exe` **ignores the shell's CWD**. Launching a build-tree binary does
  not render the assets next to your prompt — it renders the ones next to the
  binary.
- `--no-chdir_assets` turns this off if you want CWD-relative behaviour.

### `rev.cfg`

`Runtime\rev.cfg` is tracked in git and controls resolution, fullscreen, music
and the profiler. `DisplayAPI` is **ignored** — SDL2 is the only backend. The
committed file is 1920×1080, HiDPI 0 (the pin battery in
`docs/SESSION_STATE.md` depends on those exact values, so think before editing
it).

### FeatureFlags via environment variables — UNTESTED on Windows

Every runtime knob is both a CLI flag and an environment variable
(`FDS/Base/FeatureFlags.def` is the registry). These are ordinary
`getenv()` reads, so ordinary Windows environment variables work:

```bat
set FDS_DEFERRED=1
set FDS_SHADOWS=1
DEMO.exe
```

or per-invocation in PowerShell:

```powershell
$env:FDS_DEFERRED=1; .\DEMO.exe
```

`DEMO.exe --help` lists every flag. CLI flags override the environment.

### Headless / snapshot runs

`--snapshot`, `--bench` and `--repro` force SDL's dummy video and audio drivers
before `SDL_Init`, so they never pop a window or grab the audio device:

```bat
DEMO.exe --snapshot=greets@t=2500 --out=snaps --deferred --shadows
```

### Expected first-run cache bakes

The **first** run in a fresh clone is slower and writes into `Runtime\cache\`:

- `city_envmap_cube*.bin` — the city environment cube map.
- Lightmap / env bakes for greets.

This is normal. It also means **the first run's frame hashes can differ from
every subsequent run's** — a cold cache is a different input. Discard run 1
when comparing hashes (this is documented at length in `docs/SESSION_STATE.md`
and it has bitten people repeatedly).

---

## Cross-compile status

**Result: `DEMO.exe` builds and links.** `x86_64-w64-mingw32-g++ (GCC) 15.2.0`
on macOS produced

```
build-win/DEMO/DEMO.exe: PE32+ executable (console) x86-64, for MS Windows
```

12 088 168 bytes, along with `tests/clipper_test.exe` and
`tests/pbr_import_test.exe`. Every translation unit in FDS, DEMO and tests
compiles; `libFDS.a` and `libmodplayer.a` both build; the final link resolves.

**It has never been executed.** A cross-build proves the code compiles and
links against the Windows headers and ABI. It proves nothing about behaviour.

### The one configuration choice you should know about: which SDL2

`modplayer-lib` depends on the `sdl2` crate with
`features = ["bundled", "static-link"]`, so **cargo compiles a complete SDL2
from source and archives it into `libmodplayer.a`**. If `DEMO` *also* links a
system SDL2, the executable gets two of them and the link fails with hundreds
of `multiple definition of SDL_CreateWindow`.

`DEMO/CMakeLists.txt` therefore links SDL2 **headers only** on Windows and
takes the symbols from `Modplayer`. `find_package(SDL2)` is still required —
it is what supplies those headers — and it should point at an SDL2 of the same
major version (2.x) as the crate's.

The alternative would be to build modplayer with
`--no-default-features --features external-audio` (what the emscripten build
does), which removes the crate's SDL2 instead. That is **not** what this build
does, because `DEMO/SDL2.cpp`'s `Modplayer_FillBufferPlanar` audio pump is
inside `#ifdef __EMSCRIPTEN__` — there is no native audio pump, so that route
would ship Windows without music.

### Known behavioural gap on x86-64: the FP environment

`FPU_LPrecision()` (`FDS/Base/FDS_VARS.H`) sets the arm64 FPCR to
round-to-nearest-even **plus flush-denormal-outputs-to-zero** (`FZ|AH`). There
is no x86-64 equivalent wired up — the function is a **no-op** there, and this
port deliberately left it that way rather than change rendering math on a
platform that has never been run.

The exact analogue, if you want to close the gap, is MXCSR `FTZ=1, DAZ=0`:

```c
_mm_setcsr((_mm_getcsr() & ~0x6000u) | 0x8000u);
```

Consequence as shipped: an x86-64 build computes denormals normally where the
arm64 build flushes them. That is a real difference from the macOS binary,
confined to magnitudes below ~1e-38. Whether it is visible at all is unknown.

(Related: the `#ifdef _MSVC` arm at the top of that function is a typo for
`_MSC_VER` and has therefore never compiled on any compiler. It was left as
found — its body is the 1998 Watcom round-toward-`+inf` setup that the code's
own comments record as abandoned in favour of RTNE, so "fixing" the typo would
resurrect the wrong rounding mode.)

### What the audit checked and found CLEAN

So nobody re-audits them:

- **`fopen` modes** — every binary reader/writer already passes `"b"`. No
  text-mode corruption hazard in the `.FLD` / `.LWO` / `.3DS` / `.XM` /
  texture / PPM / cache paths. (One cosmetic note: `FDS/RENDER/RENDER.CPP`
  writes a diagnostic file whose name ends in a `.` — Win32 strips trailing
  dots from filenames.)
- **Include case-sensitivity** — machine-checked, 0 mismatches between the
  spelling of every `#include "..."` and the on-disk name.
- **VLAs** — none. Every array bound is an integer constant expression, so
  MSVC's lack of VLA support is a non-issue.
- **Endianness** — clean. No `<endian.h>`, no byte-swap builtins, no
  `__BYTE_ORDER`.
- **No** `mmap`, signal handlers, `dlopen`, `fork`, `exec`, or `pthread_*`
  direct use. Threading is `std::thread` / `std::mutex` /
  `std::condition_variable` throughout (`FDS/Threads.h`); the only
  platform-specific thread call is a macOS QoS hint already inside
  `#if defined(__APPLE__)`.
- **`GpuBench`** (Metal, `.mm`) is behind `-DFDS_GPU_BENCH=ON` *and*
  `if(APPLE)`. No other Objective-C / Metal source exists in the tree.
- **`strdup`** (~50 sites) — MSVC warns C4996 only; `_CRT_SECURE_NO_WARNINGS`
  is set globally.

Lower-severity, left alone: `#pragma GCC poison` in
`FDS/RENDER/DeferredEdgeAA.cpp` becomes a C4068 warning on MSVC and silently
stops guarding; `#pragma clang loop` in `FDS/RENDER/DeferredVolumetric.cpp` is
warning-only; ~40 hard-coded `/tmp/...` paths in diagnostic dumps (not on any
render path) will not resolve on Windows.

---

## What remains untested

Nothing here has run on a Windows machine. Specifically:

1. **Every MSVC / clang-cl step (all of Path A).** The per-compiler CMake
   branches (`/arch:AVX2`, `/fp:contract`, `/bigobj`, `/EHsc`, `/utf-8`, the
   `/we4700 /we4715` warning set) are written from the MSVC documentation and
   have never been fed to `cl.exe`.
2. **`FDS/Base/WinCompat.h`'s MSVC arm.** The MinGW cross-build exercised the
   header, but MSVC differs (no `<unistd.h>`, no `ssize_t`).
3. **Runtime behaviour of anything.** The demo has never been launched on
   Windows: not the window, not audio, not the chdir, not the file I/O, not
   threading, not a single rendered frame.
4. **`fopen` text-vs-binary mode.** Windows' text mode mangles `\r\n` and
   `Ctrl-Z` in binary streams. Any asset reader/writer opening a binary file
   without `"b"` in its mode string is a latent corruption bug that **only**
   manifests on Windows. This was audited — see the final report — but no
   Windows run has confirmed it.
5. **The Rust `sdl2` crate's bundled SDL2 build on Windows.** `modplayer-lib`
   defaults to `features = ["bundled", "static-link"]`, so cargo compiles SDL2
   from source through CMake. That worked cross-compiling (with a C-dialect
   pin, see below) but has not been done natively on Windows.
6. **CTest.** `ctest` registers a Python-driven render test
   (`tests/pbr_studio_parity.py`) and two snapshot smoke tests. They are not
   known to be Windows-clean.

---

## Troubleshooting

**`modplayer not found at '.../Modplayer/modplayer'`**
You cloned without submodules. `git submodule update --init --recursive`.

**`sdl2-sys` fails building the bundled SDL2 with
`cannot use keyword 'false' as enumeration constant`**
GCC 15 defaults to `-std=gnu23`, where `bool`/`true`/`false` are keywords, and
SDL 2.32's `SDL_hidapi_steam.c` still writes `typedef enum { false, true }
bool;`. The build pins `CFLAGS=-std=gnu17` for the cargo invocation on
non-MSVC Windows targets (`Modplayer/CMakeLists.txt`) — TESTED. If you hit it
anyway, set `CFLAGS=-std=gnu17` in the environment before building.

**`find_package(SDL2)` cannot find SDL2**
Point `CMAKE_PREFIX_PATH` at the directory *containing* `lib/cmake/SDL2/`. For
the MinGW tarball that is the per-triple subdirectory
(`SDL2-2.32.10/x86_64-w64-mingw32`), not the top of the archive.

**The demo starts and immediately reports missing assets**
You are running a binary whose neighbouring directories contain no `rev.cfg`.
Copy/install it into `Runtime\` (`cmake --install build`) and run it there.
