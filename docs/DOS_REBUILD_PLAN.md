# DOS rebuild plan — run the original FLOOD demo in an emulator, then improve it

Goal: compile the original 1998 demo so it runs in an emulated x86 PC
(86Box), first from the untouched original code, then with our own
improvements. The pristine sources stay read-only; all build work happens
in a separate directory.

## What the original actually is (verified from `Original/dos-rev/`)

The preserved tree is a complete, self-contained **MS-DOS** production —
not a Windows app. Concretely:

| Aspect        | Finding |
|---------------|---------|
| Target OS     | MS-DOS, **32-bit protected mode via the DOS/4GW extender** (`REV.EXE` is confirmed `LE executable … DOS4GW DOS extender`; `DOS4GW.EXE` ships alongside it) |
| Toolchain     | **Watcom C/C++** — `wpp386` (C++), `wasm` (assembler), `wlib`, `wlink`, `wmake` |
| Compile flags | `-bt=dos -5r -5 -fp5 -fpi87 -oxl+` (Pentium register calling convention, inline 387 FPU, max optimisation) |
| CPU floor     | **Pentium MMX** — the rasterisers include MMX asm (`SCALEMMX`, `GOURMMX`, `GOURMMXT`, `MMXASM`, `WOBTR`) |
| Graphics      | **VESA / VBE** linear framebuffer, own `SOURCE/VESA` + `SOURCE/DPMI` |
| Sound         | **MIDAS Digital Audio System v1.1.1** (`MIDAS.LIB`), plays `MUSIC/REVIVAL.XM` |
| Other libs    | `JPEGLIBD.LIB` (JPEG textures), `UNARJ.LIB` (unpack `.ARJ` assets) |
| Prebuilt      | **`REVIVAL/REV.EXE` (the demo) and `FDS/FDS.EXE` (dev/scene tool) already exist and are runnable** |
| Assets        | `REVIVAL/{SCENES,TEXTURES,MUSIC,FONTS}` present (some also `.ARJ`-packed) |

Build orchestration is batch-file driven, and simple to reconstruct:
- **FDS engine** (`Original/dos-rev/FDS/`): `LINKALL.BAT` compiles each
  `SOURCE/<module>` via a per-module `MAKE*.BAT`, then `MAKEFDS.BAT`
  links. `Link /c x.cpp` = "compile with `wpp386` + flags"; `comp x.asm`
  = "assemble with `wasm`"; `LinkD` = the debug variant. The real command
  is the `wpp386 -bt=dos -5r -5 -fp5 -fpi87 -oxl+` line.
- **Demo** (`Original/dos-rev/REVIVAL/`): a `wmake` `MAKEFILE` builds the
  scene `.CPP`s into `Rev.lib`, then `wlink @REV.LNK` (`system dos4g`)
  links `rev.cpp` + `fds.lib` + `Rev.lib` + `MIDAS/JPEG/UNARJ`.

Key consequence: **Phase 1 needs no compiler at all** — we already have a
known-good `REV.EXE`. That lets us de-risk the emulator before touching
the toolchain, and gives us a reference binary to validate our rebuild
against.

## Recommended strategy

Three phases, each a checkpoint. Recommendation in **bold**; the two real
decisions are collected at the end.

### Phase 0 — preserve + scaffold (low risk)
- `Original/dos-rev/` is already git-tracked (535 files, 31 MB) and
  documented read-only. **Do not touch it.**
- Create a separate build dir, e.g. **`dos-build/`** at repo root:
  - `dos-build/rev/` — a *copy* of the `dos-rev` sources to compile in.
  - Reconstructed `Link`/`comp`/`LinkD` wrappers + one driver script (or a
    portable `wmake`/Makefile) that invokes the host toolchain with the
    exact original flags.
  - `dos-build/runtime/` — the assembled DOS run folder (REV.EXE +
    DOS4GW.EXE + SCENES/TEXTURES/MUSIC/FONTS) that the emulator mounts.
  - Gitignore `*.OBJ`/`*.EXE`/`*.LIB` build outputs; track the scripts.

### Phase 1 — run the *prebuilt* original in the emulator (proves the target)
No build. Assemble `dos-build/runtime/` from the prebuilt `REV.EXE` +
`DOS4GW.EXE` + the `REVIVAL/` assets (unpack any needed `.ARJ` with a host
`arj`/`7z`), then run it. Success = the demo plays with music.

Emulator hardware profile (86Box):
- **CPU**: Pentium II 300–450 (headroom for a software 3D renderer; MMX
  is required, so no pre-MMX Pentium).
- **RAM**: 64 MB.
- **Video**: an SVGA card with **VBE 2.0 linear-framebuffer** support
  (S3 ViRGE/DX or Trio64V2). The LFB is the single finickiest piece —
  if the card's built-in VBE won't give a 2.0 LFB mode, install
  **SciTech Display Doctor / UniVBE** in DOS to guarantee it.
- **Sound**: Sound Blaster 16 (MIDAS supports it); set the `BLASTER` env.
- **OS/disk**: MS-DOS 6.22 (or FreeDOS) on a hard-disk image.

### Phase 2 — rebuild the original from source (the actual "compile" task)
**Recommended: cross-compile on the host (macOS/Linux) with Open Watcom
v2**, which still produces DOS/4GW `LE` executables and is the modern
standard for DOS retro-dev. The original flags port over unchanged. Then
run the rebuilt `REV.EXE` in the *same* emulator config from Phase 1.
- Reconstruct the two build steps (FDS lib, then demo link) as scripts
  calling `wpp386`/`wasm`/`wlib`/`wlink`.
- Link against the existing `MIDAS.LIB`/`JPEGLIBD.LIB`/`UNARJ.LIB` (OMF
  format — OW2's `wlink` consumes them; if an old lib refuses, MIDAS is
  open source and can be rebuilt).
- **Validation is behavioural/visual, not byte-identical**: OW2 codegen
  won't match 1998 Watcom, so the bar is "our rebuilt demo looks/plays
  the same as the prebuilt `REV.EXE`" — A/B them in the emulator, same
  discipline as the modern engine work (compare frames, not just "it
  linked").
- Fallback if OW2's stricter dialect breaks too much old code: install
  period **Watcom 11.0 inside a DOS dev image** and build there. Slower
  loop, but guarantees toolchain parity.

### Phase 3 — improvements (scoped after Phase 2 works)
With a working rebuild loop, candidate improvements — to be prioritised
later:
- Backport specific fixes we already found in the modern engine where the
  same root cause exists in the DOS code (e.g. the fountain-lightning RNG
  wrap).
- Higher resolution / better VBE modes; MMX → later SIMD; perf on the
  emulated CPU. Each validated the same way: A/B in the emulator.

## Risks & mitigations
- **VBE 2.0 LFB in the emulator** (most likely snag) → UniVBE/SDD fallback;
  or use DOSBox-X, which provides VESA LFB natively.
- **OW2 dialect drift** breaking 1990s C++ → keep exact old flags; fall
  back to period Watcom 11.0 in-emulator.
- **Precompiled `.LIB` link compatibility** → OMF is stable; MIDAS
  rebuildable from source if needed.
- **Source completeness** → the whole `dos-rev` tree is tracked; Phase 2
  starts by confirming every `.CPP`/`.ASM`/`.H` the batch files reference
  is present before the first build.
- **Asset paths** → `REV.EXE` almost certainly resolves assets relative to
  CWD (same lineage as the modern build); run from the folder holding
  `SCENES/` etc.

## Decisions (resolved with the original author)
1. **Emulator**: DOSBox-X for the dev loop (author's note: he doesn't
   recall ever getting the demo to run under DOSBox — see the MMX-detect
   issue below, the likely reason); 86Box for authenticity/verification.
2. **Toolchain**: Open Watcom v2 cross-compile on the host. Confirmed.

## Known issues from the original author (concrete targets)

### A. End-of-demo crash in the Crash scene (`REVIVAL/CRASH.CPP`)
The demo crashes at the end, in the Crash scene. If this is a code bug
(not an emulator artifact) the prebuilt `REV.EXE` will crash too — which
makes it the ideal validation arc: rebuild reproduces the crash → fix →
confirm the fix holds in the emulator. The demo writes **`Runtime.LOG`**
(`REV.CPP:40`), which is our primary in-emulator diagnostic for both this
crash and any asset issues.

### B. MMX detection trips DOSBox (`FDS/SOURCE/RENDER/GENERAL.H:321`)
`Detect_MMX()` probes MMX the "weird" 1998 way: install a custom INT 6
(#UD / invalid-opcode) handler in DOS4GW protected mode, execute an MMX
instruction (`emmshit()`), and let the handler flip `MMXState` to 0 if it
faults. DOSBox's protected-mode exception reflection through the DPMI host
is historically unreliable, so this probe is the prime suspect for the
"never ran under DOSBox" experience. **Easily neutralised** (author
agrees): force `MMXState` from a config/CLI flag, or replace the probe
with a `cpuid`-based check, and skip the INT-6 dance entirely. This is
also the natural *first* improvement — it directly unblocks DOSBox
testing. (The `emms`-based fillers themselves are separate; DOSBox does
emulate MMX, so once detection is bypassed the render path should run.)

## Confirmed mechanics (since first draft)
- **Assets unpack at runtime from `.ARJ`** via the linked `UNARJ.LIB`
  (e.g. `REV.CPP:49` `LoadARJ("Music\\Music.ARJ","Revival.XM")`). So the
  run folder ships the `.ARJ` archives (`REVIVAL/{TXTR,DSCN,DSRC,REV}.ARJ`
  + `MUSIC/MUSIC.ARJ`) — **no host ARJ tool needed**; the demo unpacks
  itself. Paths are CWD-relative, DOS-style.
- **FDS source tree is build-complete**: all 17 modules referenced by
  `LINKALL.BAT` have their `SOURCE/<dir>/*.CPP` present.
- **Host tooling status**: nothing installed yet — no emulator, no Open
  Watcom, no unpacker. Installs needed: DOSBox-X (`brew install --cask
  dosbox-x`), Open Watcom v2 (GitHub release binaries; macOS/arm64 build
  needs checking), and (optional, only for inspecting archives on the
  host) `arj`/`unar`.
