# Session state — feature/fog (written 2026-06-10, pre-compact)

**Read this first when resuming on feature/fog.** (The previous content —
the refactor/frame-state-shadow checkpoint — is finished work; see git
history of this file if ever needed.)

Branch `feature/fog`, all work committed and pushed through `1a6ff1f`.
Working tree clean except untracked Runtime/*.ppm debris (never
`git add -A`). Build: `cmake --build build` (POST_BUILD copies to
Runtime/DEMO — never tell the user to "pull"). ASan tree at `build-asan/`,
TSan at `build-tsan/`.

## What landed this session (newest first)

- `1a6ff1f` screen-space fast_fog fogs transparents (fogAtDepth split) +
  blend-correct acc weight (legacy additive `lit+dst/2` gets acc·0.5 —
  full acc 1.5x-counts path fog, blew city water white; alpha path acc·1)
  + glow_max knee wired into SS glow.
- `7c1f99e` flares integrated into unified TBR depth order (user rejected
  center-Z hide). The_MMX_Scalar diverts to TBR_Sprite for Scn_SpriteTBR
  scenes; TBR sprite draw dispatches 256-res flare (white colorize — omni
  V.LR/G/B unmaintained, tints black) vs 32 particle (vertex tint).
  TRAPS: sprite loop must run BEFORE TBR_Render (fillers only INSERT for
  TBR scenes); FOUNTAIN.CPP particle textures had stale SizeX=256
  metadata over a 32×32 buffer → res dispatch sampled into the font atlas.
- `4f7e5d9` --fast_fog_glow_max soft-knee compressor (froxel populate):
  "reduce intensity without affecting density" — linear below max/2,
  asymptote at max, hue-preserving. Try 250-400.
- `e8e5843` --fast_fog_blob_overlap metaball field: ADDITIVE blob sums
  (worley F1 cannot overlap); value = radius in cell units (cap 1.5);
  decouples blob size from spacing; worley_thresh reused as iso [0,3].
- `12e926d` froxel fog on transparents (--fast_fog_xpar, default on):
  out = α·(C·T(z)+acc(z)) + (1−α)·Bg, one grid fetch per xpar pixel.
- `5683c89` CITY WHITE-WATER ROOT CAUSE: Reflected_Transform never set
  F.FlareSize → ±4e36 heap garbage → screen-covering additive flare,
  per-process coin flip, "cured" by free-cam 360 (main frustum sweep
  initializes the field). One line. Five misattributions first — method
  writeup in memory project_city_pass1_stale_vpage.
- Earlier: froxel temporal reprojection (XY-only jitter — z-jitter was a
  limit-cycle flicker engine; per-column Halton phases), per-slice clipped
  analytic light glow (fixed "light moving wildly" under small camera
  moves), slab feather in froxelDensity, worley puffs, snapshot @iters=N
  now WRITES the last frame (converged temporal; old skip caused a bogus
  byte-identical A/B), city@t=N1,N2,... multi-timestamp lists fixed,
  FDS_THREADS=N pool cap (Threads.h).

## Agreed next steps (user decisions, in order)

1. **cv-pull reflection instability** — user said "we can try". Memory:
   project_cv_pull_instability. Reflective_Mapper_Setup divides by
   step = pullDir·N which passes through small values; reflections swing
   with small camera moves. Reproducer: --snapshot=seaside distsweep block.
2. **Per-scene FULL SCRIPTING** (user-decided scope): time-keyed parameter
   values, lerped between keys, built on the FeatureFlags registry
   (name/type/parse already there), per-scene files, hot-reload via mtime.
   Scene-.FLD editing explicitly OUT of scope (separate later project).
   Design-heavy — draft the file-format proposal first.
3. Parked: SIMD froxel populate for x64 (docs/fast_fog_simd_x64.md);
   froxel 128×72+temporal may match 256×144 quality cheaper (untuned);
   merging feature/fog → master at some point.

## Open questions / loose ends

- User has NOT visually confirmed in motion: TBR flare ordering (fountain
  purple omni), SS-xpar fog, metaball look, glow_max feel. Expect feedback.
- fast_fog_worley_thresh doubles as metaball iso — ranges differ by mode
  (0–0.9 worley, 0–3 metaball). Candidate rename during scripting work.
- SS xpar fog pays full per-pixel blob DDA on transparent pixels — fine
  fountain/greets, heavy for city water (froxel recommended there). No
  perf numbers taken.
- User asked whether my runs open windows ("ran the whole executable in
  front?") — snapshot mode initializes video; if windows bother them, add
  SDL_VIDEODRIVER=dummy to headless runs (verified render-identical).

## Working agreements refreshed this session

- Verify the CLASSIFIER and the FULL repro before attributing a bug (the
  white-water hunt logged five wrong attributions; the user's repro
  mechanics were right every time; a z==0-unmasked water classifier was
  fooled by the deferred skybox repaint).
- User corrections are load-bearing: "I can wait indefinitely" killed the
  wallclock theory; "reproducible without fog" killed three fog theories.
- Headless testing: --snapshot=<scene>@t=N / @iters=N from Runtime/; cp
  the output ppm IMMEDIATELY (next run overwrites).
- conetest repro cams: FDS_CONETEST_CAM="px,py,pz,fx,fy,fz" (env).
- User cmdline for interactive fog testing:
  ./DEMO --fast_fog_froxel --deferred --shadows --draw_cones
    --cone_strength=4 --fast_fog --fast_fog_blobs --fast_fog_worley
    --fast_fog_density=5..10 --fast_fog_bottom=-400 --fast_fog_top=420
    --fast_fog_cell=500 --fast_fog_inscatter=4 [--fast_fog_glow_max=300]
    [--fast_fog_blob_overlap=1.3 --fast_fog_worley_thresh=2.0]
  (city wants bottom=0 — bottom=-400 puts 400 units of fog under the water.)
