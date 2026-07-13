# SESSION STATE — glass / editor / authoring campaign (updated 2026-07-11)

Read this when resuming. Branch **fog-wt**, nothing pushed. Previous campaign
(structural push): docs/posts/SESSION_STATE_2026-07-04_structural.md.
Range covered here: `1ca269d..7282f7a` (~60 commits, 2026-07-08..11).

## Verification protocol (THE gates — run before/after everything)

All runs headless from Runtime/: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.

| gate | recipe | pin |
|---|---|---|
| city | `FDS_CITY_ENV_PIXEL=1 ./DEMO --snapshot=city@t=1961 --out=<dir> --deferred` | `37e62845c4d30eefa321730c5bb7e0b8` |
| greets | `FDS_GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551" ./DEMO --snapshot=greets@t=1588 --out=<dir> --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --no-env_refl` | majority `de3e9a5fb3aa39e008ef41b83f2b8d1b` |
| fountain | `./DEMO --snapshot=fountain@t=2500 --out=<dir> --deferred --hdr --glass-refract=1 --glass-test --profiler=0` | `51fff7cd38767d619280afe0498a6f24` |
| chase (default, POST camera redesign, fast ~17.6s) | `./DEMO --snapshot=chase@t=100,400,800,1200,1600 --out=<dir> --deferred` | t100 `953135720c5f7a1c3977f3cfefe1b3de`<br>t400 `6e5935a65591a08fccea2c460ac94839`<br>t800 `20dd1a81f1806aac471e2a59af7a0dfd`<br>t1200 `0f3651509b69c784e1ad07df641d9c03`<br>t1600 `efd36808bbb330e19428e25e7e398635` |
| chase (cinematic, POST camera) | `./DEMO --cinematic --deferred --snapshot=chase@t=800,1600 --out=<dir>` | t800 `5c540964c181d6ea294d414fd5502c9a`<br>t1600 `7c91851b27084acf8793f45dddd3906c` |
| chase (cinematic) | `./DEMO --cinematic --deferred --snapshot=chase@t=800,1600 --out=<dir>` | t800 `7be2c67b4d98613c3cae2900646050c0`<br>t1600 `a718c07fada5297879e59bd1559a483f` |
| gate suite | `./tools/render_gate.sh` (repo root, dummy drivers) | ALL PASS |
| wasm | `make wasm` | links |

Traps:
- **greets flips ~1-in-12** to a random hash (unresolved kernel-internal
  nondeterminism, see Known Issues). Majority over 3+ runs decides; a single
  mismatch means rerun, not failure. Rate conclusions need the binomial
  (24-run gates for race claims — see memory `measurement-tool-traps`).
- **city cache**: `cache/city_envmap_cube.bin` is keyed on CITY.FLD bytes.
  After ANY CITY.FLD install, discard the first run (cache rebuild), then hash.
- Greets pin includes the USER'S UNCOMMITTED files (GREETS.FLD/MAT, momy
  textures, Piramid.lwo, Hull.lwo) — a clean checkout hashes differently.
  Those files are his: never stage, never overwrite, never `git add -A`.
- Editor page freshness: build tag in the panel header (currently b60/b61);
  bump it with every shell.html change or staleness is undiagnosable.
- **chase**: no bakes, no known nondeterminism (pinned srand, fine clock off
  in snapshots). Both pins above confirmed byte-identical over 3 runs each
  (2026-07-12, C0). **RECIPE-FRAGILE**: chase accumulates snapshot state across
  the timestamp-list loop, so a given t's hash depends on the WHOLE list —
  the pins are ONLY valid for the exact recipe `t=100,400,800,1200,1600` (and
  the cinematic `t=800,1600`). Running a subset/superset gives different (still
  deterministic) hashes — NOT a regression. Always gate with the exact list.
  Valid snapshot range **t=0..1698** (past 1699 the harness re-dumps the last
  rendered VPage). Regen from `Authoring/chase/` via `pin_scene.py
  --legacy-vlum` is byte-identical to the shipping FLD (delta=0, 747,511 B) —
  the pre-edit baseline for later authored chase stages.

## The big architecture decision (2026-07-11, user-set direction)

**Sidecars are being eliminated.** Persistence belongs in the authoring
sources: per-surface → custom LWO SURF sub-chunks; per-light / per-object /
scene-level → LWS keywords; everything flows through tools/lwsread into the
FLD via **flag-bit + conditional payload** records (the proven extension
idiom — see next section). Crash (no sources yet) falls back to fldpatch
writing the same extended records. Sequencing constraint: writers first, user
re-saves greets once (his GREETS.MAT is the only record of the momy map
assignments), THEN the sidecar reader dies. In-flight work (see bottom)
already follows this; a full migration campaign (all SURF_SIDECAR_KEYS +
light:/obj:/scene: keys, editor Save rewrite, reader retirement) is the next
major batch.

## The FLD/LWS extension mechanism (use this for every new authored property)

Proven end-to-end by the volumetric-beam work (9172c5d):
1. LWS text keyword(s) per light/object/scene (e.g. `VolumetricLight 1`,
   `VolumetricLightIntensity 3.0`) parsed in tools/lwsread/LWSREAD.CPP
   (BOTH build variants: lwsread + lwsread_legacy — same source).
2. FLDSAVE.CPP writes a NEW flag bit + conditional payload after the record
   (bit-gated fields are the FLD's native extension shape; FLDs without the
   bit stay byte-identical — prove with a regen diff).
   **TRAP: bits 256/512/1024 of the light flags are OR-contaminated by
   ReadEndBehavior — bit 512 was NOT free. Headlight beams use bit 2048.
   Always check what ORs into a flag word before claiming a bit.**
3. Engine FLD loader (FDS/FLD/FLD_CONV.CPP) reads the conditional payload
   into an Omni/Material/Scene field (0-sentinel = unset → legacy default;
   GreetsMirror clones inherit via memcpy — sane by construction).
4. Editor write-back: tools/editor_server.py patches the LWS/LWO, regen via
   the scene's lwsread variant (legacy for chase/fountain/city — VLUM×100
   era), installs the FLD (backup to Runtime/SCENES/.backups/ first).

## What landed (grouped; commit msgs carry the detail)

### Authoring recovery — city is a full authoring scene
- Sources found IN-REPO (Original/dos-rev/.../CITY/): CITY1.LWS identified by
  light-set fingerprint; 17/20 objects byte-exact. b1/b3/b6 shipped higher-
  poly than any surviving LWO → recovered FROM the shipping FLD via
  tools/fld2lwo/ (byte-parity regen: CITY1.LWS → shipping CITY.FLD exact).
  Authoring/city/README.md has provenance + regen commands. (d60f5ab,
  cc6244e, 4f943a1)
- **Search lessons** (for the crash hunt + future archaeology): match by
  embedded SRFS surface-name sets, not filenames (the b3 slot holds a "b7"
  building); list ARJ archives (first sweep missed them); lwsread maps LWO
  points 1:1 to FLD verts — NO seam splitting, count-matching is valid.
- 46 authored headlight spotlights baked into CITY1.LWS (two per vehicle,
  parented, warm 255/235/185, 15°/30°), engine gained LightType-2 spot
  conversion + parented aim + flare-stamp skip. Code headlight schemes
  retired (default off, kept for A/B). (48d57e5, e4e34cf)
- Authored volumetric beams: per-light `VolumetricLightIntensity` gain
  (gain 3.0 shipped); retune = tools/add_city_beam_flags.py + regen. (9172c5d,
  1275dea)

### Determinism
- `srand(time(NULL))` → pinned seed (GENERAL.H). The Omni_Rand flare twinkle
  made every run unique — bakes-on frames all-distinct, bakes-off glass-band
  flips. (5f325d4)
- TBR transparent order: facing rank precomputed at insert (torn reads in the
  concurrent sort flipped front/back). (1e91306, fb3a302)
- Glass band scheduler: B1/B2 back/front sub-phases fixed the deterministic
  greets "face pop". (8539e8f → 8539e8)
- Scene clock sawtooth (user-visible "city camera jumps back"): rate was an
  EMA of instantaneous dTimer/dWall (Jensen-biased ~10% high) + hard snaps on
  hitches. Now ratio-of-EMAs + hitch-hold + continuous anchor. (2541c32)

### Rendering features (all default-off unless noted)
- Screen-space glass refraction stack (Mat_Refractive opt-in, per-material
  IOR, band scheduler w/ barriers) — editor ON by default. (f4d470a..)
- HDR: 250 lit-cap now HDR-gated in vec+transparent kernels too → luminosity
  >1 blooms (was scalar-only; editor edits were silently capped). LDR keeps
  the cap. NOTE: OuterVec still stores 8-bit — radiance >255 needs the scalar
  kernel on PreferOuterVec scenes. (f6ec404)
- Cone turbulence/swirl: world-space value noise + helical swirl in ALL
  three cone integration paths, SIMD (+3.2ms greets); user's tuned values in
  Runtime/PRESETS/greets-beams.flags. TRAP: reshaping the fmadd chain moves
  hashes by ulps even at neutral values — off path keeps the exact legacy
  expression. (ab9a9c1, 189eeec)
- Env live water (city): probe bake hides the water mesh, re-shades plane
  texels with the main-view procedural formula (Schlick + caustic cells);
  sample-time WaveSlope perturb animates. Bypasses the pristine cache when
  on (~4s init, 0 per-frame). Glints not baked (view-dependent). (5d28db7,
  0162d3b)
- fastfog dist-dim slice 4: sky dims at horizon + forward pixels dim
  (inert at default 0). (8acf8cd)

### Flags / presets
- `--flags-file=<path>` (nestable, comments, CLI-after-file wins) + unknown
  `--flag` WARNING (was silent — a typo'd `--fast-fog-blob` ran slab fog for
  weeks). Runtime/PRESETS/city-noir.flags = the user's city look, cinematic-
  based, measured byte-equivalent to his old 40-token line + blobs fix.
  (2482013)
- Per-surface migrations of former globals: waterProcedural (tri-state),
  envRefl (tri-state), envBakeRes (pow2, largest-wins on shared probes),
  RefractIor; scene-level sidecar defaults for boltFlash/fastFog bounds —
  NOTE: these sidecar forms are transitional; the LWS/LWO migration
  supersedes them. Precedence everywhere: per-surface > CLI/env > scene
  default > compile default. `SCRIPTS/<scene>.params` lines still override
  scene defaults (per-frame scripts yield only to explicit-set marks).

### Editor (browser, `make editor` → :8099/DEMO.html?editor&scene=<name>)
- Objects: FLD-tree hierarchy in all scenes (chunk-collapse `:cN`, engine
  helpers pooled in hidden "(engine)"; NAMED engine meshes with faces get
  visible entries — that's how the disco ball became reachable, plus its
  material needed MatLib linkage). Per-object scale knob (EditorScale on the
  Scale spline — pivots correctly, all instances; Tri_Possessed meshes are
  honestly inert). Focus = nearest-instance, in-context (2.5× radius).
- Lights: grouped by parent object, multi-select (ctrl/shift), group edit,
  click-to-select in viewport; authored city headlights appear grouped per
  vehicle.
- Surfaces: split w/ mirrors + #1/#2 naming (persistence via source-bake in
  flight — see below), map reset ✕ (restores authored incl. tangents), pack
  picker with FreePBR preview renders (98.7% coverage), procedural
  displacement generator (FBM), map-viz overlay, live smoothing, xpar PBR.
- Editor camera: instant (no momentum) in editor mode only.
- Boot race fixed: objects list retries (city published CurScene mid-init);
  console.warn when objects empty while surfaces exist.

## Known issues / deferred (honest list)

- **Greets ~1-in-12 flip**: kernel-internal nondeterminism, survives
  1-thread/noaslr/prescribble/pinned-seed with ALL kernel inputs
  hash-verified identical. Full evidence matrix + next probe (per-pixel term
  dump at amudim bake face 0 px 242,122) in memory `measurement-tool-traps`.
  Bounded impact: subtle pano slivers; frames deterministic with bakes off.
- Env-bake content varies run-to-run (same root cause family).
- The user's GREETS.MAT `momy#2|*` lines are DROPPED at load until he
  re-splits + re-saves in the editor (split-bake landed 6c6c972 — re-save now
  bakes momy2 into the LWO as a real surface; accepted, he regenerates).
- volumetric_unified (default-off Beer-Lambert pass) ignores per-light cone
  gain + turbulence.
- Mirror clones don't reflect a live object re-scale; tram return-leg beams
  face backwards (real shuttle behavior); fast_fog_blob_overlap clamps at
  1.5 (3×3×3 neighborhood); police strobe not in the authored lights.
- Legacy equirect env path (--no-env_cube) keeps static bake water.
- CITY.CPP line ~1575 unused `using std::min` (clang-tidy noise, off-limits
  era leftover — fine to fix opportunistically).

## Recently landed (was in-flight — verified + committed)

1. **Split persistence via SOURCE BAKE** (6c6c972): editor Save bakes runtime
   instance-splits into the LWO sources (lwopatch.split_surface reassigns
   non-primary polygon clusters to new real surfaces; bake_splits in
   editor_server.py matches live cluster centroids to source polygons). After
   reload the #k names are real authored surfaces. Crash/no-source scenes
   stay live-only. Recipe: re-split momy → Save → reload → momy/momy2 real
   surfaces with maps. Editor-flow verified by inspection (pieces + pins);
   NOT yet driven through a live browser split-save round trip.
2. **Scene-wide env defaults as LWS keywords** (6c6c972): FdsSceneEnvRefl /
   FdsSceneEnvBakeRes → bit-2048 conditional FLD payload → Scene fields →
   FramePrep. VERIFIED end-to-end (round-trip +8B; live read-back envRefl=1/
   res=512 → 133 probes at 512). Editor 'scene env defaults' row, tag b61.
3. **Crash sources + registry flip DONE** (470d7f1 + 6c6c972): vintage "END"
   laptop scene, lt_scr FLD-recovered via fld2lwo_crash.py, byte-parity regen
   (md5 4f8aac84…). crash promoted to authoring. EVERY scene is now
   source-authored — the fldpatch fallback in the sidecar-elimination plan is
   dead.
4. **Chase upgrade plan** (docs/CHASE_UPGRADE_PLAN.md): 612-line staged plan
   (blasters, hit particles, camera, movement, lighting + more) — planning
   only, awaiting user stage-selection before any implementation.

## Queued next (user-requested, 2026-07-11)

- **CHASE WATER DARK BAND — port the known fix** (user 2026-07-14; blocked on
  the B1 blasters agent freeing CHASE.CPP/FeatureFlags.def, tree non-compiling
  mid-edit). The "lower missing water layer" (horizontal seam, dark band below)
  is the documented `chase water dark band` bug, FIXED on ~/work/revival
  `feature/soa-vertex` commit **9902349**. Two defects: (1) InsertTransparentToTBR
  (FDS/FILLERS/FILLERS.CPP ~1796) computed the strip span from projected PY,
  garbage for the camera-STRADDLING water quad → water inserted only above the
  horizon, vanished below (the band = mirrored-mountain underlay). Fix: verts
  in front of near plane → insert into EVERY strip, sort by FAR surface.
  (2) water_procedural kernel composite darkens vs chase's black night-sky
  reflection → new flag `water_fresnel_composite` (default ON=city), chase
  factory sets it OFF, city re-pins ON. fog-wt CONFIRMED at the exact
  before-state (FILLERS.CPP:1807/1816 old PY code, DeferredSurfaceKernel.cpp:2525
  waterProcOn, no flag yet, CHASE.CPP:996-999 factory). Port all 5 files
  (FILLERS.CPP, DeferredSurfaceKernel.cpp, FeatureFlags.def, CHASE.CPP, CITY.CPP),
  verify chase water bright/no-band + city/fountain byte-identical (fixes city's
  bottom-strip band too, brighten-only).
- **CHASE SPOTS verify/realign** (user "not seeing the spotlights"): L2.3 canyon
  spots are canyon-LOCAL (t≈1000-1300, mm7 gap) — invisible in open-water frames
  (expected). Authored for the OLD camera; the trail-follow redesign may have
  reframed off the lit walls. Verify they show in the canyon with the new camera;
  realign if needed. Blocked on same (chase files + build).

- **Editor UX batch** — DONE (2026-07-12, d5a6ae9, tag b66): solid
  metallic/roughness generators (the mech-metallic recipe), Save "what
  changed" receipt, persistent status bar, canvas-fits-beside-panel (letterbox
  via CSS; fill=engine-resize deferred), settings find/category-groups/
  changed-only. shell.html-only; native/pins untouched.
- **Chase upgrade** — plan in docs/CHASE_UPGRADE_PLAN.md. Provenance: chase is
  a scene BUILT-BUT-CUT in 1998 (lack of tuning time), hand-corrected in the
  revival — **NO sacred 1998 baseline; free to retune for look** (user,
  2026-07-12). **C0 + S0 + L1 landed (2026-07-12, fog-wt).**
  L1 (4a54af5/3bb68ea/4cb7513): flare sanity + SceneCorrections retirement +
  sky gradient. New identifiers: LWS `FdsFlareScale` → light-bit **4096**
  (Light_FlareScale) → Omni::FlareScale; LWS `ZenithColor/SkyColor/
  GroundColor/NadirColor` → scene-header bit **4096** (Scene_SkyColors, on
  AmbientIntensity EndBehavior — distinct word from the light bit) →
  Scene::Sky*; flags `chase_legacy_omni_hack` (default OFF, retired hack
  escape-hatch), `sky_gradient` (default OFF — the CANDIDATE, generic: would
  paint city/crash void sky too). Chase default pin RE-BASELINED (table above).
  Sky is opt-in pending look-approval + a default flip / chase preset (can't
  use SCRIPTS/chase.params — protected). Moon light is degenerate (IRange=0,
  no contribution) — preserved from the hack, future tuning target.
  Authoring/chase/README.md now STALE ("byte-parity 1998" no longer true) —
  small doc-pass TODO. C0 (b72e7a9): chase gate pins (RECIPE-FRAGILE) +
  stale-comment fix. S0 (30a9c2e):
  `tools/build_beatmap.py` + `Authoring/chase/chase.beatmap` (placement-
  agnostic — chase has NO track slot yet, arbitrary song+start-order
  scaffolding), `DEMO/ChaseEvents.{h,cpp}` (beat-map + event-table loader +
  pure-`t` `Events_ActiveAt` — the §8.B contract), flag `chase_event_test`
  (default off) + RunChaseSnapshot determinism proof.
- **`Modplayer_GetPosition` — LANDED on master** (2026-07-12): decision (b),
  decoupled from the refactor migration. Parent ce615c2 bumps submodule
  e6429cf → **9d2a1ca** + adds the header decl. Pure lock-free FFI accessor
  over the EXISTING display triple-buffer (`SongState::get_position` reads the
  `TripleBuffer<PlayData>` the display path already publishes — NO new atomics,
  per the user's correction). Fields: order/row/tickInRow + songTick =
  **milliseconds-since-start** (PlayData has no u64 tick; ms is the monotonic
  clock; add total_samples to PlayData if sample-exact ever needed). **Getter
  requires `Modplayer_SetDisplay(handle,true)`** — the demo currently sets it
  FALSE (REV.CPP:1077/1619) for perf, so a sync consumer must re-enable it.
  Verified monotonic under playback; dead-stripped from DEMO (0 refs → pins
  unchanged); both builds link.
  **ACTION NEEDED FROM USER: merge modplayer PR #20**
  (github.com/Gil-AdB/modplayer/pull/20 — direct push to origin/master was
  branch-protected, so 9d2a1ca is on branch `feat/modplayer-getposition`). If
  it SQUASH-merges to a new SHA, re-bump the parent pointer to the merged
  commit (noted in ce615c2's message).
- **DEFERRED: modplayer `feat/s3m-refactor` adoption** — a more accurate S3M
  player, but it PREDATES the embedder FFI (verified: has Create/Start/
  SetOrder; MISSING SetDisplay/FillBuffer/FillBufferPlanar + the external-audio
  cargo feature). Adopting = a full re-port of the embedder audio layer onto
  the refactored core, its own focused task. Pick up when S3M playback
  accuracy becomes the priority.
- **Sidecar-elimination migration** (the big one): now that all scenes are
  source-authored and the scene-env keywords proved the pattern, migrate the
  remaining SURF_SIDECAR_KEYS + light:/obj:/scene: keys to LWO SURF
  sub-chunks / LWS keywords → FLD payloads, rewrite editor Save, retire the
  sidecar reader (writers first; user re-saves greets once; then reader dies).
  **INCLUDES: rip out the `#k` split-marker scaffolding** (user-confirmed
  2026-07-11). `#k` is vestigial — it exists only because the OLD sidecar/
  live-only-split path couldn't bake, so a `momy#2` edit had to COLLAPSE back
  to the real `momy` surface (the 7 `re.sub(r"(#\d+)+$","")` sites in
  editor_server.py: lines ~146/271/287/389/424/761/932/958). Two facts make
  it dead once sidecars go: (1) splits now BAKE into the LWO → parts are real
  surfaces, nothing transient to collapse; (2) the shell sends explicit
  `payload.splits` with per-part world centroids and bake_splits matches
  parts GEOMETRICALLY, never by parsing `#k` from the name. So in the
  migration those collapse sites get DELETED (not guarded), and a split
  becomes "make a real surface, reassign polygons, any plain name". Do NOT
  add existence-aware-collapse or build further on `#k`. Post-migration the
  only transient label needed is cosmetic (the live window between "split"
  and "Save") — nothing functional keys off it. That also dissolves the
  momy#1/#2-vs-momy2 naming question (currently unresolved, left as-is on
  purpose): naming becomes a free cosmetic choice, not a load-bearing
  convention. Engine side is already clean — Editor_BaseSurfName strips only
  ::mirUV, never `#`.

## Where the rest of the knowledge lives

- Cross-session traps + pins history: memory `measurement-tool-traps`
  (~/.claude/.../memory/) — race-hunt methodology, instrument pitfalls,
  binomial rule, pin re-pin log with justifications.
- Authoring provenance: Authoring/city/README.md (parity math, regen).
- Pipeline wiring: docs/GRAPHICS_PIPELINE.md, docs/ENGINE.md (pre-campaign
  but still accurate for the core).
- Old shipping FLDs: Runtime/SCENES/.backups/ + git history (last commit
  carrying each noted in the promotion commit messages).
