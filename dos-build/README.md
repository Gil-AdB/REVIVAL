# dos-build — recompile & run the original DOS demo

Working area for building and running the 1998 DOS demo. The pristine
sources in `Original/dos-rev/` are **read-only reference** and never
edited here. Full plan: `docs/DOS_REBUILD_PLAN.md`.

## Layout
- `runtime/` — Phase-1 run folder the emulator mounts as `C:`. A working
  copy of `Original/dos-rev/REVIVAL/` (prebuilt `REV.EXE` + `DOS4GW.EXE`
  + `.ARJ` assets the demo self-unpacks via `UNARJ.LIB`). The demo writes
  `Runtime.LOG` here — primary diagnostic. Gitignored (reproducible via
  `cp -R Original/dos-rev/REVIVAL dos-build/runtime`).
- `rev/` — (Phase 2) a copy of the `dos-rev` sources to cross-compile
  with Open Watcom v2. Gitignored; reconstructed build scripts under
  `build/` are tracked.

## Status
- [x] **Phase 1 COMPLETE** — the ORIGINAL RELEASE runs end-to-end under
      DOSBox-X (City → Fountain → Greets), user-confirmed.
- [~] **Phase 2 IN PROGRESS** — Open Watcom cross-compile via Docker.
      Pipeline WORKS (linux/amd64 container runs OW2 `binl64`, emits DOS4GW
      objects). Whole FDS engine (17 modules) + 17 ASM + demo scenes
      (REV/CITY/FOUNTAIN/GREETS/GLAT) COMPILE CLEANLY. Dialect fixes captured
      in `ow2-port.patch`: implicit-int decls need a type (MMXState/
      Basic_Rate), inline-asm mem operands need `PTR`, MMX asm needs `.mmx`,
      DOS `\` include separators → `/`, for-loop var scope leak (FOUNTAIN
      `Mat`), pointer signedness (FLD_READ `new unsigned short`), INCLUDE
      search paths. Build+link: `build-dos.sh` in the container
      (`build-dos-container.sh`). REMAINING = LINK RESOLUTION: SHIT.CPP is a
      needed TU (globals + LoadARJ + Grid_Texture_Mapper) but has a GridPoint
      struct-visibility issue; undefined globals still to home:
      ImageSize, C_FZP, C_rFZP, CParticle_ISize, The_Seven_UP_HALFSIZE/_64,
      WobPointsHeight.
- [x] Phase 0: run folder assembled.

## Phase 2 build (container)
- Toolchain: `ow/` (Open Watcom v2 from ow-snapshot; `binl64` = Linux x64
  cross-compiler, `h/`, `lib386/dos`). Gitignored.
- Source: `rev-build/` = working copy of `Original/dos-rev` + `ow2-port.patch`
  fixes. Gitignored; reproduce = copy Original + `git apply ow2-port.patch`.
- Build: `docker run --platform linux/amd64 -v ow:/ow -v rev-build:/work
  debian:stable-slim bash build-dos.sh` → objects in `rev-build/_build/`,
  links `REV.EXE` via `wlink system dos4g` + MIDAS/JPEGLIB/UNARJ libs.

## HOW TO RUN THE ORIGINAL (working, 2026-07-04)
- Folder: `dos-build/runtime-m8d/` — the actual Movement'98 release
  (`m8d_rev.zip`, files.scene.org/…/flerp/.1/DEMO.'98/Movement.'98/DEMO/).
  REV.EXE is byte-identical to `Original/dos-rev/REVIVAL/REV.EXE`; the
  release ships only CITY/FOUNTAIN/GREETS scenes + loose TEXTURES +
  MUSIC/MUSIC.ARJ (no bundled extender).
- Extender: genuine **Watcom DOS4GW.EXE** (from Open Watcom `ow-snapshot`
  `binw/dos4gw.exe`), copied into the folder; invoke `dos4gw.exe rev.exe`.
  The bundled Rational DOS/4G + DOS/16M does NOT work under DOSBox.
- DOSBox-X: `machine=svga_s3`, **`memsize=32`** (demo needs ~24 MB per
  PRODUCT.TXT — 16 MB OOMs mid-texture-load → `descriptor type 8 for int
  31`), `core=dynamic`, `cputype=pentium_mmx`, `cycles=max`, `sbtype=sb16`.
- Config: `dos-build/dosbox-x-m8d.conf`. Launch:
  `open -a /Applications/dosbox-x.app --args -conf .../dosbox-x-m8d.conf`.
- Manual video UI: pick **320×240 32-BPP** (native mode); pick Sound
  Blaster for music (MIDAS↔SB16 under DOSBox is finicky).

## Dead ends cleared en route (all red herrings)
MMX INT-6 #UD probe (patched out — no effect), extender swaps (Rational
DOS/16M → int 21 desc 1E; DOS/32A & Watcom DOS4GW → int 31 desc 8, all the
same OOM downstream), MIDAS (no-sound still crashed), VESA LFB DPMI 0x800
mapping. ROOT CAUSE = 16 MB out-of-memory during city texture load. The
`dos-build/runtime/` hand-assembly "stops after glat" because its layout
differs from the release; use `runtime-m8d/`.
- [~] Phase 1: DOSBox-X keeps aborting in its protected-mode/DPMI descriptor
      validation, at different points depending on extender:
        - Rational DOS/4G + DOS/16M (bundled): `Illegal descriptor type 1E
          for int 21` right after the VESA mode set (before scenes).
        - DOS/32A (`dos32a.exe rev.exe`): got FURTHER — executed demo code,
          video modes cycled — then `Illegal descriptor type 8 for int 31`
          (a DPMI call). Pristine REV.EXE; INT-6 patch ruled irrelevant.
      Two different extenders → two different DOSBox DPMI aborts ⇒ the wall
      is DOSBox-X's DPMI/descriptor handling of the engine's low-level
      selector use (VESA LFB, ISRs), not just the extender.
      EXTENDER RULED OUT: genuine Watcom DOS4GW (from Open Watcom snapshot,
      runtime `DOS4GW.EXE`; Rational saved as `DOS4GW.RAT`) fails IDENTICALLY
      to DOS/32A — `int 31`/descriptor 8, same spot. Both proper extenders
      → same DPMI abort ⇒ it's a DPMI (INT 31h) call the DEMO makes right
      after the VESA mode set. Two candidates: the VESA LFB physical→selector
      mapping (core, undisable-able) or MIDAS's timer/DMA callback.
      DECISIVE TEST: MIDAS off. Still int-31 → it's the LFB path → 86Box.
      Clears → MIDAS was it → runs under DOSBox.
      RESULT: no-sound run STILL aborts `int 31`/desc 8 → **MIDAS ruled out.**
      ROOT CAUSE: the engine's own DPMI wrapper (`SOURCE/DPMI/DPMI.CPP`,
      `int386(0x31,…)`) calls DPMI fn 0x800 "Map Physical→Linear"
      (`DPMI_Map_P2L`) to map the VESA **linear framebuffer** phys base to a
      usable pointer — `VESA.CPP:650`, right after mode-set. DOSBox-X rejects
      that DPMI/selector op (`descriptor type 8`). It's mandatory to draw ⇒
      DOSBox-X CANNOT run this demo's video path, regardless of extender or
      sound. **This is why it never ran under DOSBox.** → Phase 1 = 86Box.
- [ ] Phase 2: cross-compile with Open Watcom v2, A/B vs prebuilt.
- [ ] Phase 3: fixes (Crash scene init, …).

## Phase-1 findings (DOSBox-X)
1. `DOS4GW.EXE` here is **Rational DOS/4G + DOS/16M**, not Watcom DOS/4GW.
   `REV.EXE` is a stub whose self-exec of it drops the program arg under
   DOSBox (fatal 1004). Workaround: invoke explicitly — `dos4gw.exe rev.exe`.
   (In autoexec.)
2. With that, the program loads, MIDAS inits, and the **VESA mode is set
   (640×480 → 720×540 logged)** — extender + sound + video all work.
3. It then hard-aborts:
   `E_Exit: Illegal descriptor type 1E for int 21` — DOSBox-X's DPMI host
   rejects a protected-mode interrupt **descriptor** installed by the demo's
   own ISR hooks (INT 6 for the `Detect_MMX` #UD probe, INT 8 timer, INT 9
   keyboard in `FDS_Init`). A real DOS/4G environment handles these; DOSBox
   does not. This matches the original author's recollection that the demo
   never ran under DOSBox, and confirms the "weird MMX/interrupt detection"
   as the root.
   Instrumentation: `core=normal` + `[log] logfile=dosbox-x.log`.
4. **INT-6 probe ruled out by experiment.** Binary-patched `runtime/REV.EXE`
   (backup `REV.EXE.orig`): NOPed the two `_dos_setvect(6,…)` calls +
   the `emmshit` (emms) call inside `Detect_MMX` @ file 0x239B0 (verified
   via ndisasm; `MMXState=1` store preserved). Crash UNCHANGED — same
   `Illegal descriptor type 1E for int 21` at the same spot. So the INT-6
   #UD probe is not the blocker.
5. **Prime suspect now: the extender.** Bundled `DOS4GW.EXE` = Rational
   DOS/4G + DOS/16M (1993), not Watcom DOS/4GW. DOSBox-X's DPMI targets
   DOS/4GW & PMODE/W; DOS/16M's protected-mode mechanics are the likely
   incompatibility. Clean test: swap in a DOSBox-friendly extender
   (DOS/32A, or a real Watcom DOS4GW) via `<extender> rev.exe`. If that
   clears it → confirmed; if not → deeper, use 86Box or rebuild.

## Two paths from here
- **86Box** (authentic): real DOS + DOS/4G handles the interrupt hooks
  normally → runs the *unmodified* original. Needs a DOS install in 86Box.
- **Open Watcom rebuild** (Phase 2): neutralize the DOSBox-hostile bits
  (skip/replace the INT-6 MMX probe; the ISR hooks) for a DOSBox-friendly
  build — also the permanent home for the Crash-scene init fix.
