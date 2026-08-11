# `FeatureFlags::setDefault` ordering audit

**Tree:** branch `fog-wt`, HEAD `97b13fd` (all cited line numbers verified against
`git show HEAD:<file>`; the working tree's concurrent edits are line-count-neutral
in the files cited).
**Date:** 2026-08-08.
**Status: AUDIT ONLY. Nothing in this document has been applied.** Every row below
is a latent look change; several are large. The recommended fixes are written as
proposals with an evaluation recipe each, per the standing instruction to test the
changed look first.

Everything labelled **MEASURED** was produced by the runs listed in §7. Everything
else is labelled **INFERRED** (read from source, not run) or **UNKNOWN**.

> **SUPERSEDED IN PART — read §0 first.** Two of this document's headline rows do
> not survive re-measurement at HEAD, and three more have been fixed since it was
> written. §0 (2026-08-11) is the current verdict table for every run-phase
> `setDefault`; where it disagrees with §3/§4 below, §0 wins.

---

## 0. RE-AUDIT 2026-08-11 — every run-phase `setDefault`, and two retractions

**Tree:** `fog-wt` at `0b466b7`. Built and measured in an isolated worktree so
concurrent agents' uncommitted work could not contaminate it; all runs headless
(`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`), 1920×1080, every flag its own
argv word.

### 0.1 What changed since 2026-08-08

| Row | Then | Now |
|---|---|---|
| §4.1 `mirror_rtt` | DROPPED (biggest look row) | **FIXED** — moved to `GreetsApplyInitDefaults` in `7953bab` |
| §4.3 `mirror_rtt_density` | DROPPED | **FIXED** — moved alongside it in `7953bab` |
| §4.2 `shadow_lightmap` | DROPPED, "fix is to move it to Init" | **RESOLVED DIFFERENTLY — see §0.3.** Moving it is the *wrong* fix. Comment corrected + the dead bake removed in `0b466b7` |
| §4.5 `--cinematic` city `shadows` | DROPPED, MEASURED ~2 000 px | **RETRACTED — see §0.4.** Not dropped, and the measurement was noise |
| §4.4 `greets_shard_randomness` | DROPPED | **STILL INERT, PARKED — see §0.5** |
| — | not audited | **NEW: `bolt_flash_range` is read at init — §0.6** |

### 0.2 The verdict table

Every flag written by a **run-phase** block (one that executes after all
`Initialize_*`). "Init/boot readers" lists only readers that run BEFORE the
write; a flag with none is correctly placed by construction.

**S2 — `GreetsApplyRunDefaults`** (`DEMO/GREETS.CPP`, runs at `createGreetsScene`)

| Flag | Init/boot readers | Verdict |
|---|---|---|
| `shadows` | `CITY.CPP:3126` — but inside `if (city_test_spots())`, **default 0**, so dead by default | correctly-placed\* (greets compensates anyway: `ShadowMaps_BakeStatic(…, forceEnable)` `GREETS.CPP:2990`) |
| `shadow_dynamic` | none (`CITY.CPP:3515` is inside `struct CityScene`, per-frame) | correctly-placed |
| `shadow_lightmap` | **`Mekalele.cpp:85` (BOOT)**, `MirrorShatter.cpp:655` (init), `LightmapBake.cpp:200/222` (init), `GreetsMirror.cpp:3051` + `MirrorShatter.cpp:940` (lazy-run) | **INERT for the boot reader — and unfixable by moving.** §0.3 |
| `pbr` | `CITY.CPP:2592` (init — env-cube cache-key salt) | correctly-placed **today**; latent coupling, §0.7 |
| `env_brdf_analytic` / `pbr_multiscatter` / `diffuse_energy` | none (kernel only) | correctly-placed |
| `sh_ambient` | none (`RENDER.CPP:520` is lazy *inside* `renderFrame`) | correctly-placed |
| `hdr` / `hdr_linear` | none | correctly-placed |
| `deferred_checkerboard` | none (`RENDER.CPP:368` `deferredEnabled()` is per-frame; greets' init-time `DeferredPathEnabled()` at `GREETS.CPP:2477` is already true via `shard_deferred`, set in the **Init** block) | correctly-placed |
| `bloom` / `bloom_intensity` / `hdr_refl_gain` / `hdr_exposure` | none (`Hdr.cpp` 172/206/212/78/822, all per-frame) | correctly-placed |
| `cone_strength` / `cone_fine_tiles` / `disco_bloom` | none | correctly-placed |
| `greets_shard_fall_speed` | none (`MirrorShatter.cpp:369`, `update`, per-frame) | correctly-placed |
| `greets_shard_randomness` | **`MirrorShatter.cpp:225`** (`MirrorShatter::build` ← `BuildGreetsShatter` ← `Initialize_Greets`) | **INERT** — parked, §0.5 |
| `anamorphic`/`chromatic`/`vignette`/`grade`/`grain` (`--cinematic`) | none | correctly-placed |

**S4 — `ApplyCinematicProfile`** (`DEMO/SceneTick.h`; city/chase/fountain/crash)

| Flag | Init/boot readers | Verdict |
|---|---|---|
| `shadows` | `CITY.CPP:3126`, dead behind `city_test_spots` (default 0) | **correctly-placed** — §4.5 RETRACTED, §0.4 |
| `bolt_flash_range` | **`FOUNTAIN.CPP:1195`** (inside `Initialize_Fountain`, 1020–) | **partially-inert (vacuous today)** — §0.6 |
| `bolt_flash_peak` | none (`FOUNTAIN.CPP:2971`, per-frame) | correctly-placed |
| `rain`, `fast_fog`, `fast_fog_froxel`, `fast_fog_xpar`, `fast_fog_worley`, `fast_fog_*` floats | none (`DeferredFastFog.cpp` only, per-frame) | correctly-placed |
| `anamorphic*`, `chromatic*`, `vignette*`, `grade`, `grain`, `bloom*`, `hdr*`, `hdr_glow_scale`, `cone_strength`, `xpar_peel_passes` | none | correctly-placed |

**S5/S6/S7/S10 — the scene factories**

| Source | Flag | Init/boot readers | Verdict |
|---|---|---|---|
| `createCityScene` / `createChaseScene` / `RunCitySnapshot` | `water_procedural` | `CITY.CPP:2550` (init) | correctly-placed\* — already worked around in place via `isSet()` (`CITY.CPP:2549-2551`) |
| `createCityScene` / `createChaseScene` | `water_fresnel_composite` | none | correctly-placed |
| `createChaseScene` | `water_variation` | none (`CHASE.CPP:1417` is inside `struct ChaseScene`, 939–1463) | correctly-placed |
| `createChaseScene` | `blaster_light_range` / `_intensity` | none (`BlasterBolts.cpp:216-217`, per-frame) | correctly-placed |
| `createPBRTestScene` | `deferred` | none (`RENDER.CPP:365`, per-frame) | correctly-placed |

**Net: of ~60 run-phase writes, exactly two are inert** — `shadow_lightmap`
(addressed, not by moving) and `greets_shard_randomness` (parked) — plus one
latent trap (`bolt_flash_range`). Everything else is read per-frame and the
write always lands first.

### 0.3 `shadow_lightmap` — moving it is the wrong fix, and it is now moot

The 2026-08-08 recommendation was "move it to `GreetsApplyInitDefaults`". That
would not work and is not wanted:

* **It cannot fix the reader that matters.** `EngineGBuffer_Resize`
  (`Mekalele.cpp:85`) runs at **boot** — `SDL2.cpp:433/441`, `Snapshot.cpp:153`,
  `ReproHarness.cpp:130` — i.e. before *every* scene init, not just before
  greets'. An init-block write is still too late.
* **It would switch on real work to feed a dead path.** Moving it enables
  `LightmapStampOrigBary`, the offscreen lightmap planes, and (before `0b466b7`)
  a 54 ms atlas bake — all to feed `resolveCubeAtten`'s lightmap branch, which is
  independently gated shut by `lmKernelEnabled = !shadow_dynamic() ||
  shadow_lm_dynamic()` (`DeferredSurfaceKernel.cpp:1622`). Opening *that* gate is
  MEASURED at **+1.7 ms/frame for a 95 %-one-LSB change** (SESSION_STATE
  2026-08-10). That is a look/perf decision, not a placement fix.
* **The run-phase placement does still buy something**, just not what its comment
  claimed: scope. `Initialize_Greets` runs first natively, so an init-block write
  would allocate the two planes for city/chase/fountain as well.

**Applied instead (`0b466b7`):** the false comment is corrected in both
`GREETS.CPP` blocks and in the `FeatureFlags.def` row — `shadow_lightmap` is an
**allocation** gate read at boot, never the "per-pixel SAMPLE gate the deferred
kernel reads for EVERY scene" — and greets now skips the bake and the 0.09 GB
atlas outright when the flag is off at init.

**Residual, reported not fixed:** the run-phase write *does* still reach two
lazily-built offscreen G-buffers (`GreetsMirror.cpp:3051` RTT,
`MirrorShatter.cpp:940` shard workers), which therefore allocate `lightmapMF` +
`lightmapST` (6 B/px each) and pay Mekalele's per-pixel write into them, for
planes `lmKernelEnabled` guarantees nobody reads. Both files are other agents'
active surface right now, so this is a note, not a patch.

### 0.4 RETRACTION — `--cinematic` city shadows is NOT a dropped row

§4.5 claims `cine::kCity.shadows = true` never reaches its init consumer, and
prices the loss at ~2 000 px/frame. **Both halves fail at HEAD.**

1. **The consumer is dead by default.** `CITY.CPP:3126`'s
   `if (shadows()) { ShadowMaps_Rebuild; CubeShadowMaps_Rebuild;
   ShadowMaps_BakeStatic; }` is nested inside
   `if (fds::FeatureFlags::city_test_spots())` (`CITY.CPP:3027`), whose
   `.def` default is **0** — "Install 6 test spotlights in city for cone-overlay
   experimentation". Production city installs no shadow-casting spot at all, so
   there is nothing for the ordering to drop. §4.5 read the inner `if` without
   the outer one.
2. **The measurement was the scene's own nondeterminism.** MEASURED at HEAD,
   `--snapshot=city@t=1401,1961,2521 --deferred`:

   | pose | `--cinematic` vs `--cinematic --shadows` | **`--cinematic` vs itself (A-vs-A, ×2)** |
   |---|--:|--:|
   | t=1401 | 0 px | 0 px / 0 px |
   | t=1961 | 1 357 px | **1 465 px / 1 683 px** |
   | t=2521 | 1 738 px | **1 418 px / 1 390 px** |

   The "effect" is inside its own noise floor, at the same poses, with the same
   small-area/saturating-amplitude signature. `cine::kCity.rain = true` and the
   lightning strike is stochastic. Note also that in the city snapshot path
   `g_shadowMaps` is **empty in both arms** (no `[SHADOW]` line in either
   stderr, and the two init logs are otherwise byte-identical) — there is no
   mechanism for the flag to change a pixel there.

   **Lesson for the campaign, not just this row: §4.5, §4.1's sweep and §5 L2
   were all taken on `--cinematic` city/greets arms with no A-vs-A control.**
   Any of those numbers below ~2 000 px on a rain-enabled scene should be
   re-taken against a control before being believed.

**No action.** `shadows` is read per-frame (`DeferredSurfaceKernel.cpp:5933/
5959/5978`) and the profile write lands before every frame. §4.6 (chase) is
unchanged in status: chase allocates no shadow maps anywhere, so its
`kChase.shadows = true` samples whatever the previous scene left — still
**Group D, investigate**, and still not an ordering bug.

### 0.5 `greets_shard_randomness` — genuinely inert, deliberately PARKED

Confirmed at HEAD: the only reader is `MirrorShatter.cpp:225`, inside
`MirrorShatter::build`, reached from `Initialize_Greets` — so the run-phase
`setDefault(0.8)` never lands and the shards are built at the compile default
**1.0**. A move to `GreetsApplyInitDefaults` is leak-free (the flag is
`[greets]`-category, no other scene reads it).

Parked on purpose, for three reasons:
1. **It is a look change to shard SHAPES**, and §4.4's own reading stands — 1.0
   may be the look he prefers, in which case the correct fix is to change the
   intent line to 1.0, not to move it. Wrong-intent and wrong-order are equally
   likely and only he can tell them apart.
2. **It is not evaluable headlessly.** The shatter only exists after the `Y`
   key; there is no flag that arms it at init (checked), so no snapshot or bench
   recipe can produce a before/after image.
3. `FDS/RENDER/MirrorShatter.cpp` is another agent's live surface this session
   (`983cdb4`, the shard-reflection fix). Measuring shard geometry against a
   moving target would produce a number nobody could reproduce.

**To land it later:** move the line, then compare interactively (press `Y`)
against `--greets_shard_randomness=1.0`, which reproduces today's build exactly.

### 0.6 NEW — `bolt_flash_range` is read during `Initialize_Fountain`

`ApplyCinematicProfile` writes `bolt_flash_range` (`SceneTick.h`), but
`FOUNTAIN.CPP:1195` reads it inside `Initialize_Fountain` (1020–) to size the
strike-flash omni at creation:

```cpp
g_BoltFlashOmni = AppendFountainOmni(FntSc, FntHead, …,
                                     fds::FeatureFlags::bolt_flash_range());
```

Every `create*Scene` runs after every `Initialize_*`, so this reader can never
see a profile value. **Vacuous today**: the consumer belongs to fountain, and
`cine::kFountain` leaves `boltFlashRange` at the `CinematicProfile` member
default `500.0f`, which is also the `.def` default (`FeatureFlags.def:306`) — so
the value it reads is the value the profile wants. `kCity`/`kChase` ask for
600.0f, but they own no bolt-flash omni; that write reaches nothing.

**Not fixed** — there is no bug to fix today, and the move is impossible anyway
(the profile is scene-scoped; hoisting it before `Initialize_Fountain` would
apply *city's* whole cinematic profile globally, the §6.3 / §5 L1 trap). It is
recorded because it is a **latent trap**: change `kFountain.boltFlashRange` and
it will silently not take, with no diagnostic. The `--strict_setdefault`
detector in §6.1 is what catches this class.

### 0.7 `pbr` and the city env-cube cache key — latent, not broken

`Initialize_City` folds `FF::pbr()` into `bakeFlagSalt` (`CITY.CPP:2592`), the
key for the 426 MiB env cube on disk. `GreetsApplyRunDefaults` sets `pbr = true`
— but at `Run_Greets`, i.e. after `Run_City`, so city both bakes and renders at
the compile default `0` and the key is self-consistent. **Correct today by
ordering luck.** Anything that moves `setDefault(pbr, …)` earlier — into an
Init block, or a future scene that runs before city — silently re-keys and
re-bakes that artifact. Worth a comment at the salt site if anyone touches it.

---

## 1. The mechanism, and the two ways it fails

`FeatureFlags::setDefault(id, v)` (`FDS/Base/FeatureFlags.h:147-149`) is:

```cpp
static inline void setDefault(BoolId id, bool v) { if (!isSet(id)) g_boolVals[int(id)] = v; }
```

It writes the value unless the flag was marked **explicitly set** — and the set
mark is written only by argv/env parsing (`FeatureFlags.cpp:305-338`, `:398-415`),
by `--vanilla` (`:181-195`), and by the live tune console
(`setParamFromText`, `:692`). `setDefault` itself never marks. The precedence model
is documented on the `vanilla` flag (`FeatureFlags.def:19`) and is sound: CLI/env >
scene default > compile-time default.

Two consequences follow directly, and both are real bugs in this tree:

**F1 — ORDERING.** Because `setDefault` does not mark, *the last writer wins* among
scene defaults, and any consumer that ran *before* the write got the previous value.
A `setDefault` placed after its consumer is a silent no-op with no diagnostic.
Whether a row is fine or broken depends entirely on **when the flag is read**:

* read **per-frame** (deferred kernel, tonemap, post stack, tick) → the write always
  lands first, because every frame happens after every `create*Scene()`. HONOURED.
* read at **init** (geometry build, mesh/slot allocation, bake, G-buffer sizing) →
  a `setDefault` in `create*Scene()` is too late. DROPPED.
* read at **boot**, before any scene exists (`EngineGBuffer_Resize`) → even an
  init-time `setDefault` is too late.

**F2 — SCOPING.** `setDefault` writes a *process-global*. `GreetsApplyInitDefaults`
runs inside `Initialize_Greets`, which in the native demo is the **first** init
(`REV.CPP:1148`) — so any global it touches is in force for city, chase, fountain
and crash, both their inits and their renders. The split into Init/Run defaults
(`GREETS.CPP:1027-1048`) was built to prevent exactly this, and it is correct for
every flag it names — but `shard_deferred` escapes it, because a helper reads it
for every scene. §5.

The code is already aware of F1 in two places, which is the strongest evidence that
it is a recognised trap rather than a one-off: `CITY.CPP:2529-2545` re-derives
`water_procedural` through `isSet()` because "createCityScene / RunCitySnapshot both
setDefault it AFTER this init runs", and `GREETS.CPP:2848-2855` force-enables the
static shadow bake because "greets turns --shadows on at RUN … AFTER this init
bake". Both are point fixes for one flag each. Neither generalises, and §3 shows
four flags that needed the same treatment and did not get it.

---

## 2. Where the `setDefault` blocks are, and when each runs

95 `setDefault` call sites across 11 entry points.

| # | Entry point | Site | Runs | Position in the lifecycle |
|---|---|---|---|---|
| S1 | `GreetsApplyInitDefaults` | `GREETS.CPP:1055` (12 calls) | `Initialize_Greets:1522` — the function's **first statement** | before all greets build/bake; **first of all scene inits** natively (`REV.CPP:1148`), **last** under wasm (`MainLoop.cpp:410`) |
| S2 | `GreetsApplyRunDefaults` | `GREETS.CPP:1133` (22 + 5 `--cinematic` calls) | `createGreetsScene:4287` | **after every `Initialize_*`** |
| S3 | `BuildDiscoBall` | `GreetsDisco.cpp:434,439` (2 calls) | `Initialize_Greets:2674` | mid greets init |
| S4 | `ApplyCinematicProfile` | `SceneTick.h:159` (35 calls) | `createCityScene:3694`, `createChaseScene:1474`, `createFountainScene:2917`, `createCrashScene:130`, `RunCitySnapshot:1118`, `RunFountainSnapshot:247`, `RunCrashSnapshot:856`, `Snapshot.cpp:3898` | **after** the matching `Initialize_*` in every case |
| S5 | `createCityScene` | `CITY.CPP:3689,3693` (2) | after `Initialize_City` | |
| S6 | `createChaseScene` | `CHASE.CPP:1463,1468,1473,1480,1481` (5) | after `Initialize_Chase` | |
| S7 | `RunCitySnapshot` | `Snapshot.cpp:1117` (1) | after `Initialize_City` | |
| S8 | `MaterialImport_Apply` / `_ApplyMapFile` | `MaterialImport.cpp:418,419,791,792` (4) | `Initialize_Greets:1733` | mid greets init |
| S9 | `main()` flag implication | `REV.CPP:1462` (1) | immediately after `parseArgs` | before everything |
| S10 | `createPBRTestScene` | `PBRTEST.CPP:119` (1) | after `Initialize_PBRTest` | |
| S11 | `Run_DisplaceTest` / `build()` | `DisplaceTest.cpp:242,344,754,816,831` (5) | inside the test rig, before each bake it gates | |

Boot-time reads that precede **all** of these: `EngineGBuffer_Resize`
(`Mekalele.cpp:71`) from `SDL2.cpp:433/441` (V_Create), `Snapshot.cpp:153`,
`ReproHarness.cpp:130`.

---

## 3. Verdict table

Legend — **When**: `boot` / `init` / `frame`. **Verdict**: HONOURED, DROPPED, or
HONOURED\* (only because something else compensates).

### 3.1 S1 — `GreetsApplyInitDefaults` (runs before all greets init)

| Flag | Intent | Consumer (file:line) | When | Verdict |
|---|---|---|---|---|
| `greets_mirror` | 1 | `GREETS.CPP:740` (`BuildGreetsShatter`), `:2871` (mirror build), `:3872` | init + frame | HONOURED |
| `no_greets_spots` | 1 | `GREETS.CPP:2665` | init | HONOURED |
| `greets_omni_shadows` | 1 | `GREETS.CPP:2742` | init | HONOURED |
| `greets_omni_shadow_res` | 512 | `GREETS.CPP:2740` | init | HONOURED |
| `greets_moving_omni_shadow_res` | 128 | `GREETS.CPP:2809` | init | HONOURED |
| `shard_deferred` | 1 | `MirrorShatter.cpp:645`, `GreetsMirror.cpp:3040`, **`RENDER.CPP:368`** | init + frame | HONOURED for greets — but see **§5 L1**, it leaks to every other scene |
| `greets_shard_res` | 64 | `GREETS.CPP:1008` (`BuildGreetsShatter`) | init | HONOURED |
| `shadow_lightmap_res` | 128 | `LightmapBake.cpp:226` | init (bake thread) | HONOURED — leaks (§5 L4) |
| `shadow_lightmap_planar` | 1 | `LightmapBake.cpp:335`, `DeferredSurfaceKernel.cpp:1478` | init + frame | HONOURED — leaks (§5 L4) |
| `shadow_skip_animated` | 1 | `Transform.cpp:1273` | init (bakes) + frame | HONOURED — leaks (§5 L4) |
| `greets_shadow_proxy` | 1 iff `--greets_displace` | `GREETS.CPP:1785` | init | HONOURED |
| `greets_displace_flat_mirror` | 1 iff `--greets_displace` | `GreetsMirror.cpp:214` | init + frame | HONOURED |

### 3.2 S3 — `BuildDiscoBall` (runs inside greets init)

| Flag | Intent | Consumer | When | Verdict |
|---|---|---|---|---|
| `cone_strength` | 1.2 | `DeferredVolumetric.cpp:1797`, `DeferredFastFog.cpp:4075` | frame | **overwritten by S2's 2.0** — see §4.0; leaks to earlier-running scenes (§5 L2) |
| `shadow_cone_cull` | 1 | `Transform.cpp:1213` | frame (shadow pass) | HONOURED — leaks (§5 L3) |

### 3.3 S2 — `GreetsApplyRunDefaults` (runs AFTER every `Initialize_*`)

This is the block the brief called "suspect in its entirety". It is 27 rows;
**four** are dropped, the rest are genuinely fine.

| Flag | Intent | Consumer (file:line) | When | Verdict |
|---|---|---|---|---|
| `shadows` | 1 | `Shadows.cpp:102`, `DeferredSurfaceKernel.cpp:5587/5613/5632` | frame | HONOURED\* — greets' init calls `ShadowMaps_Rebuild`/`CubeShadowMaps_Rebuild` unconditionally (`GREETS.CPP:2846-2847`) and passes `forceEnable=true` to `ShadowMaps_BakeStatic` (`:2855`) precisely to compensate |
| `shadow_dynamic` | 1 | `Shadows.cpp:1191`, `DeferredSurfaceKernel.cpp:1456/1477` | frame | HONOURED |
| `shadow_lightmap` | 1 | `Mekalele.cpp:80` (**boot**), `LightmapBake.cpp:197`, `MirrorShatter.cpp:655` / `LightmapBake.cpp:219`, `Mekalele.cpp:80`, `DeferredSurfaceKernel.cpp:1456` | **boot + init** + frame | **DROPPED** — §4.2 |
| `pbr` | 1 | `DeferredSurfaceKernel.cpp:1429` | frame | HONOURED |
| `env_brdf_analytic` | 1 | `DeferredSurfaceKernel.cpp:1439/3955/4813` | frame | HONOURED |
| `pbr_multiscatter` | 1 | `DeferredSurfaceKernel.cpp:1440/3956/4814` | frame | HONOURED |
| `diffuse_energy` | 1 | `DeferredSurfaceKernel.cpp:1442/3960/4816` | frame | HONOURED |
| `sh_ambient` | 1 | `RENDER.CPP:509` (one-shot probe, inside `renderFrame`), `DeferredSurfaceKernel.cpp:1445/…` | frame | HONOURED — the "one-shot" bake is lazy inside the frame, not at init |
| `hdr` | 1 | `RENDER.CPP:365/646/733/1308`, `Hdr.cpp`, kernel | frame | HONOURED |
| `hdr_linear` | 1 | `Hdr.cpp:69/815`, kernel `:1381/2781/4823` | frame | HONOURED |
| `deferred_checkerboard` | 1 | `RENDER.CPP:367`, kernel `:514`, `DeferredVolumetric.cpp:584` | frame | HONOURED |
| `bloom` | 1 | `Hdr.cpp:161/195`, `GREETS.CPP:3951` | frame | HONOURED |
| `bloom_intensity` | 2.0 | `Hdr.cpp:201` | frame | HONOURED |
| `hdr_refl_gain` | 4.0 | `Hdr.cpp:77` | frame | HONOURED |
| `cone_strength` | 2.0 | `DeferredVolumetric.cpp:1797`, `DeferredFastFog.cpp:4075` | frame | **HONOURED — MEASURED**, §4.0 |
| `cone_fine_tiles` | 1 | `DeferredVolumetric.cpp:1890` | frame | HONOURED |
| `disco_bloom` | 0.0 | `GreetsDisco.cpp:763` (`DiscoBloomPost`, called `GREETS.CPP:3952`) | frame | HONOURED |
| `mirror_rtt` | 1 | **`GreetsMirror.cpp:2651`** (`PrepareSecondOrderMirrorRtt` ← `GREETS.CPP:2917`), **`GreetsMirror.cpp:1401`** (`BuildMirrorsByTextureName` ← `GREETS.CPP:2905`), `GREETS.CPP:3779` | **init** + frame | **DROPPED** — §4.1 |
| `mirror_rtt_density` | 1024 | **`GreetsMirror.cpp:1591`, `:2754`** — both init, no frame consumer | **init only** | **DROPPED** — §4.3 |
| `greets_shard_fall_speed` | 0.8 | `MirrorShatter.cpp:359` (`update`) | frame | HONOURED |
| `greets_shard_randomness` | 0.8 | **`MirrorShatter.cpp:215`** (`MirrorShatter::build` ← `GREETS.CPP:999` ← `BuildGreetsShatter` ← `:2922`) | **init only** | **DROPPED** — §4.4 |
| `hdr_exposure` | `kGreetsExposure` = 1.0 | `Hdr.cpp:813` | frame | HONOURED |
| `anamorphic`, `chromatic`, `vignette`, `grade`, `grain` (only under `--cinematic`) | 1 | post stack, `Hdr.cpp` / post passes | frame | HONOURED |

### 3.4 S4 — `ApplyCinematicProfile` (city / chase / fountain / crash, all `--cinematic`-only)

34 of the 35 fields are post-process / froxel-fog / tick-time values read per frame
→ **HONOURED**. The exception:

| Flag | Intent | Consumer | When | Verdict |
|---|---|---|---|---|
| `shadows` | `kCity`/`kChase` = **true**; `kFountain`/`kCrash` = false | **`CITY.CPP:3065`** — inside `Initialize_City` (2370-3077), gating `ShadowMaps_Rebuild` + `CubeShadowMaps_Rebuild` + `ShadowMaps_BakeStatic` | **init** | **DROPPED for city and chase** — §4.5 / §4.6. Vacuous for fountain/crash (they ask for `false`, which is the compile default). |

(`cone_strength` = 2.0 in `kCity`/`kChase` is frame-read → honoured, but it is
overwritten in the demo sequence by greets' init leak, §5 L2.)

### 3.5 S5-S11

| Source | Flag | Intent | Consumer | When | Verdict |
|---|---|---|---|---|---|
| S5 `createCityScene` | `water_procedural` | 1 | `CITY.CPP:2543` (`Initialize_City`), `RENDER.CPP:399`, kernel `:2793` | **init** + frame | **HONOURED\*** — dropped at the init consumer, but `CITY.CPP:2543-2545` re-derives it with `isSet()` and hard-codes the factory's `true`. Works; fragile (§6.4). |
| S5 | `water_fresnel_composite` | 1 | kernel `:2794` | frame | HONOURED |
| S6 `createChaseScene` | `water_procedural` | 1 | as above | init + frame | HONOURED\* (same workaround, same value) |
| S6 | `water_fresnel_composite` | **0** | kernel `:2794` | frame | HONOURED — but leaks forward (§5 L5) |
| S6 | `water_variation` | 1 | `CHASE.CPP:1413` (tick) | frame | HONOURED — leaks forward (§5 L5) |
| S6 | `blaster_light_range` | 90 | `BlasterBolts.cpp:217` | frame | HONOURED |
| S6 | `blaster_light_intensity` | 260 | `BlasterBolts.cpp:216` | frame | HONOURED |
| S7 `RunCitySnapshot` | `water_procedural` | 1 | as above | init + frame | HONOURED\* (same workaround) |
| S8 `MaterialImport` | `env_refl` | 1 | `RENDER.CPP:489`, kernel `:1434/3952/4810` | frame | HONOURED |
| S8 | `env_bake_fix` | 1 | `EnvBake.cpp:705/924/1190` (inside the per-frame `FramePrep`) | frame | HONOURED |
| S9 `main()` | `env_refl` | 1 iff `city_env_pixel` | as above | frame | HONOURED (runs immediately after `parseArgs`) |
| S10 `createPBRTestScene` | `deferred` | 1 | `RENDER.CPP:364` | frame | HONOURED (`Initialize_PBRTest` reads no flag) |
| S11 `DisplaceTest` | `displace_viz` ×3, `greets_displace_amp` ×2 | test-rig values | `DisplaceTest.cpp:311/422/666/1162`, `WorldAabb.cpp` | set immediately before the bake they gate, in the same function | HONOURED |

---

## 4. The dropped rows, in detail

### 4.0 First, a correction: `cone_strength` is NOT dropped — MEASURED

The brief listed `cone_strength` as confirmed-dropped ("the cones ran at 1.2 where
2.0 was intended"). **That is backwards, and it is measured backwards.**

`GreetsDisco.cpp:434` is not a *consumer*, it is a second `setDefault`. Because
`setDefault` never sets the explicit mark, both writes land, and the **later** one
wins: `BuildDiscoBall` (init, 1.2) then `GreetsApplyRunDefaults` (run, 2.0). The
only consumers (`DeferredVolumetric.cpp:1797`, `DeferredFastFog.cpp:4075`) are
per-frame, so they see 2.0. `GpuBench/Deferred.h:109-114` already records this
conclusion.

MEASURED, greets `t=5780`, 1920×1080:

| arm | vs default |
|---|---|
| `--cone_strength=2.0` | **byte-identical** (0 px differ) |
| `--cone_strength=1.2` | 59 029 px differ, 41 315 > 4/255, 22 262 > 12/255, max 36 |

A default greets run is already at 2.0. **No action.** The residual problem with
this pair is scope, not order — see §5 L2.

---

### 4.1 `mirror_rtt` — greets ships with second-order mirrors OFF (biggest look row)

* **Intent:** `GREETS.CPP:1191` `setDefault(mirror_rtt, true)`.
* **Consumed at init, twice:**
  * `GreetsMirror.cpp:2651` — `PrepareSecondOrderMirrorRtt` returns 0 on the first
    line unless the flag is true. Called from `GREETS.CPP:2917`, inside
    `Initialize_Greets`.
  * `GreetsMirror.cpp:1401` — `wantRtt = rttSlots && (mirror_rtt() || recurse)` in
    `BuildMirrorsByTextureName`, called from `GREETS.CPP:2905`. This is the
    *first-order* RTT panel path.
* **Ordering:** `createGreetsScene:4287` runs after `Initialize_Greets`. **Too late.**
* **Current value:** `0` (compile default, `FeatureFlags.def:457`). **Intended:** `1`.

**MEASURED (census).** greets snapshot, stderr:

```
default        : (no [MIRROR-RTT] line at all — early return)   → 0 slots
--mirror_rtt   : [MIRROR-RTT] prepared 8 slot(s)                → 8 slots
                 m1->m2 512x512, m1->m3, m1->m4, m2->m1, m2->m3, m2->m4, m3->m2, m4->m1
```

Both arms report `[GREETS-MIRROR] active mirrors: 4`, and neither emits a
`[MIRROR-RTT1]` line — so at the current `greets_mirror_rtt_min_area` (1.5) the
*first-order* RTT path adds nothing either way. The whole delta is the 8
second-order slots.

**MEASURED (frame), greets default camera sweep, 1920×1080, default vs `--mirror_rtt`:**

| t | changed px | > 4/255 | > 12/255 | > 32/255 | max | mean\|d\| on changed |
|---|---|---|---|---|---|---|
| 100 | 0 | 0 | 0 | 0 | 0 | — |
| 600 | 2 131 | 2 131 | 2 131 | 2 131 | 235 | 132.3 |
| 1000 | 1 693 | 1 693 | 1 693 | 1 693 | 209 | 150.2 |
| 1500 | 2 125 | 2 125 | 2 125 | 2 125 | 214 | 149.6 |
| 2100 | 2 500 | 2 500 | 2 500 | 2 500 | 247 | 151.9 |

Small **area** (0.1 % of frame), **maximal amplitude** — every changed pixel is
> 32/255 and the mean change is ~150/255. This is a panel switching between "flat
screen texture" and "live mirror-in-mirror reflection", which is exactly what the
user reports seeing only with an explicit `--mirror-rtt`. At the two wall/mirror
review poses (`t=5780` scene camera; `t=6133` review cam) the delta is 43 px and 0
px respectively — the second-order panels are simply not in shot there, which is why
this never showed up in the displacement campaign's pose set.

**CONFIRMED AGAIN 2026-08-09 AT THE USER'S OWN POSE, and the "0 px on the
authored path" generalization is retired.** The user reported `--mirror-rtt`
visibly changing his frame; an earlier GPU-side note had generalized the t=100
row above into "CPU `--mirror_rtt` changes 0 px on the authored path". It does
not. At his live pose — `FDS_GREETS_CAM="-8.6249094,2.72651696,-53.2339516,
0.210607708,0.0055912463,-0.977554619"`, `t=3122`, `--greets_displace` — default
vs `--mirror_rtt` differs by **9 471 px (0.457 %), max channel Δ 175/255, mean
Δ-sum 207/765 on the changed pixels** (and 9 473 px with `--deferred` added, so
the two bugs are independent). The init log is the direct proof: a default run
prints `3 mirrors + 0 first-order RTT` and **no** `[MIRROR-RTT] slot` lines at
all, while `--mirror_rtt` prints **seven** (`m1->m2`, `m1->m3`, `m1->m4`,
`m2->m1`, `m2->m3`, `m2->m4`, `m3->m2`, each 512×512). 0 px is what you measure
at a pose where no RTT panel is in shot; it is not a property of the flag.

**PERF, now measured** (the §4.1 row above says "NOT MEASURED"): greets t=3122,
1512×848, min-of-6, `--deferred --greets_displace`, load 9–15 — building the 7
slots costs **+3.67 ms** (47.29 → 50.96 ms) and the flat arm **+2.11 ms**
(45.80 → 47.91). Also measured: the RTT bake DOES honour the
`--greets_shadow_proxy` substitution, so it does not rasterise the displaced
wall per slot — it takes `OffscreenViewScope` (`GreetsMirror.cpp:3067`), which
raises `g_offscreenViewDepth`, which is half of Transform.cpp's
`_offscreenPass` predicate (`:1180`), so `Face_MainOnly` is skipped at `:2429`
and the flat proxy mesh is admitted at `:1432`. The displaced-vs-flat delta with
all 7 slots live is +3.05 ms, not the hundreds a per-slot full-wall raster would
cost.

* **Recommended fix:** move both `mirror_rtt` and `mirror_rtt_density` from
  `GreetsApplyRunDefaults` into `GreetsApplyInitDefaults` (before `GREETS.CPP:2905`).
  Both are `[greets]`-category and read by no other scene, so the leak argument that
  keeps the render flags in the Run block does not apply. One-line move each.
* **Evaluate with:** the sweep above (`--snapshot=greets` default timestamps,
  no `FDS_GREETS_CAM`) — poses `t=600/1000/1500/2100` all show the panels. Compare
  post-fix default against today's `--mirror_rtt` arm; they should become
  byte-identical.
* **PERF: NOT MEASURED.** This adds 8 per-frame RTT jobs bounded at 512×512 each
  (2.1 Mtexel/frame worst case) — `RenderSecondOrderMirrors`, `GREETS.CPP:3713`.
  Per-panel visibility culling means only visible slots render (`g_rttJobsLastFrame`).
  There is a partial offset that is *also* unpriced: `GREETS.CPP:3777-3781` skips the
  whole forward `Lighting(GreetSc)` pass when `mirror_rtt` is on and no RTT job ran,
  which the comment prices at ~4 ms — so turning the flag on **may** pay for part of
  itself on frames with no visible panel. Price both with the `TailProf` `"RTT"`
  scope at `GREETS.CPP:3713` before deciding. **Do not assume it is cheap.**

---

### 4.2 `shadow_lightmap` — the flag is set too late for three consumers, and one of them is at BOOT

> **SUPERSEDED by §0.3.** The diagnosis below is right; the recommended fix
> ("move it to the Init block") is wrong — an Init-block write is still after the
> BOOT reader, and it would switch on work to feed a path a second gate keeps shut.
> Resolved in `0b466b7` by correcting the comment and deleting the dead bake.

* **Intent:** `GREETS.CPP:1142` `setDefault(shadow_lightmap, true)`, deliberately in
  the Run block (`:1140-1141`: "set at RUN (not init) so it doesn't leak onto earlier
  scenes; the bake itself was force-enabled at init").
* **The Run placement is right for the *sampling* gate. It is wrong for three other
  consumers that were not considered:**

  | Consumer | Where | When | What it does when the flag is false |
  |---|---|---|---|
  | `EngineGBuffer_Resize` | `Mekalele.cpp:80` | **BOOT** (`SDL2.cpp:433/441` V_Create; `Snapshot.cpp:153`; `ReproHarness.cpp:130`) | does not allocate `lightmapMF` / `lightmapST`; Mekalele then treats `span.lightmapMF == nullptr` as "off" and never writes the planes |
  | `LightmapStampOrigBary` | `LightmapBake.cpp:197` — `if (!shadow_lightmap()) return;`, **no `forceEnable` parameter** | `GREETS.CPP:2856`, init | the per-vertex barycentric stamp is skipped; `Vertex::OrigBaryB/C` stay at their `0.0f` initialisers (`Vertex.h:107`), so every vertex claims bary (0,0) |
  | `MirrorShatter::prepareReflectionAtlas` | `MirrorShatter.cpp:655` | `GREETS.CPP:2922` → `:1010`, init | the shard reflection G-buffers get no lightmap planes (whereas `ensureReflWorkers`, `:940`, allocates them lazily per-frame **when the flag is by then true** — the two disagree) |

  Note `LightmapBake_Static` **is** protected: it takes `forceEnable`
  (`LightmapBake.cpp:219`) and `GREETS.CPP:2945-2947` passes it. Its sibling
  `LightmapStampOrigBary` on the very next line was not given the same treatment.

* **Today this is MASKED, which is why nobody noticed.** The kernel gate is
  `lmKernelEnabled = !shadow_dynamic() || shadow_lm_dynamic()`
  (`DeferredSurfaceKernel.cpp:1456`). Greets sets `shadow_dynamic = 1` and
  `shadow_lm_dynamic` defaults `0`, so the lightmap kernel path is off regardless.

* **MEASURED** (greets `t=5780`, 1920×1080):

  | arm | vs default |
  |---|---|
  | `--shadow_lightmap` | **byte-identical** (masked, as expected) |
  | `--shadow_lm_dynamic` | **byte-identical** — the documented A/B for the lightmap fast path is a **complete no-op**, because the G-buffer planes were never allocated |
  | `--shadow_lm_dynamic --shadow_lightmap` | **1 075 598 px (51.9 %) changed**, 7 538 > 4/255, 2 218 > 12/255, 251 > 32/255, max 59 |

  So the mechanism is live and measurable — it is only the *default run* that never
  reaches it. Anyone who tries to evaluate or re-enable the lightmap fast path with
  `--shadow_lm_dynamic` alone will measure zero and conclude the feature does
  nothing. That is the same class of silent false negative `--strict_flags` exists to
  prevent.

* **Recommended fix (three parts, all small):**
  1. give `LightmapStampOrigBary` a `forceEnable` parameter and pass the same
     `!GreetsScenePreempted()` the bake gets, so the pair stays in lockstep;
  2. either allocate the two lightmap planes unconditionally in
     `EngineGBuffer_Resize` (≈6 bytes/px — 12.4 MB at 1080p, and the per-pixel write
     cost in the Mekalele hot loop, which the comment at `Mekalele.cpp:76-79` says is
     why they are conditional — **so this is a perf decision, not a free fix**), or
     re-call `EngineGBuffer_Resize` after `createGreetsScene` (cheap, but it is a
     realloc mid-lifecycle and other agents are in `Mekalele`/`RENDER` right now);
  3. make `prepareReflectionAtlas` agree with `ensureReflWorkers`.
* **Evaluate with:** `--shadow_lm_dynamic` alone, at `t=5780`, before and after. Today
  it is byte-identical to default; after the fix it must reproduce the 51.9 % arm.
  Then judge that arm's look on the wall/floor poses (`t=5743`, `5963`, `6097`).
* **Priority note:** because it is masked, this changes **no shipping pixel today**.
  Its cost is that a measurement tool is silently lying.

---

### 4.3 `mirror_rtt_density` — dropped, and MEASURED to be inert at current geometry

* **Intent:** `GREETS.CPP:1192` → 1024. **Actual:** 256 (`FeatureFlags.def:463`).
* **Consumed only at init:** `GreetsMirror.cpp:1591` (first-order) and `:2754`
  (second-order). There is no per-frame consumer, so the setDefault is a pure no-op.
* **MEASURED:** with `--mirror_rtt`, `--mirror_rtt_density=256` vs `=1024` produces
  **byte-identical frames** and identical slot sizes — every slot clamps to the
  512×512 cap at both densities (windows are 2.5-14.6 world units; 2.5 × 256 = 640
  already rounds past 512).
* **Recommended fix:** move it alongside `mirror_rtt` (§4.1) for correctness of
  intent. **Do not expect a visible change**, and do not use it as evidence the fix
  worked.

---

### 4.4 `greets_shard_randomness` — the shatter is built with the wrong irregularity

* **Intent:** `GREETS.CPP:1194` → 0.8. **Actual:** 1.0 (`FeatureFlags.def:467`).
* **Consumed once, at init:** `MirrorShatter.cpp:215`, inside `MirrorShatter::build`,
  called from `GREETS.CPP:999` inside `BuildGreetsShatter`, called from `:2922`
  inside `Initialize_Greets`. The value scales both the non-uniform grid spacing and
  the per-intersection jitter — i.e. the *shard shapes are baked* with it.
* Its sibling `greets_shard_fall_speed` (0.8, default 1.5) is read in
  `MirrorShatter::update` per-frame and **is** honoured — so the pair currently
  ships mismatched: intended-slow fall, unintended-chaotic shapes.
* **NOT MEASURED.** The shatter only exists after the `Y` key, which neither
  `--snapshot` nor my runs trigger.
* **Recommended fix:** move to `GreetsApplyInitDefaults` (greets-scoped flag, no leak
  risk).
* **Evaluate with:** `--repro=greets@...` with the shatter key scripted, or
  interactively press `Y` and compare against `--greets_shard_randomness=1.0`. This
  is a *shape* change; it needs his eyes, and 1.0-vs-0.8 may well be a look he
  prefers as-is — in which case the correct fix is to change the intent line to 1.0,
  not to move it.

---

### 4.5 `--cinematic` city: shadows are requested and never allocated

> **RETRACTED — see §0.4.** The init consumer cited below sits inside
> `if (city_test_spots())` (default 0), so it is dead by default; and the measured
> delta reproduces as the scene's own rain/lightning nondeterminism against an
> A-vs-A control. No action.

* **Intent:** `SceneTick.h:199` `setDefault(shadows, p.shadows)` with
  `cine::kCity.shadows = true` (`SceneTick.h:222`).
* **Consumed at init:** `CITY.CPP:3065` — inside `Initialize_City` (2370-3077):
  ```
  if (fds::FeatureFlags::shadows()) {
      ShadowMaps_Rebuild(CitySc, 512);
      CubeShadowMaps_Rebuild(CitySc, 256);
      ShadowMaps_BakeStatic(CitySc);
  }
  ```
* **Ordering:** `ApplyCinematicProfile` runs at `createCityScene:3694` and at
  `RunCitySnapshot:1118` — both **after** `Initialize_City`. **Too late** in both
  paths. `shadows` compile default is `FDS_SHADOWS_DEFAULT_ON` = 0.
* So `--cinematic` turns the *sampling* on for the frame but the maps were never
  allocated and the static bake never ran. The cone pass, whose whole point per the
  comment at `CITY.CPP:3057-3061` is "cones get occluded by buildings", has nothing
  to sample.
* **MEASURED**, city snapshot sweep, 1920×1080, `--cinematic` vs `--cinematic --shadows`:

  | t | changed px | > 32/255 | max | mean\|d\| on changed |
  |---|---|---|---|---|
  | 280 | 0 | 0 | 0 | — |
  | 840 | 1 886 | 1 886 | 255 | 238.3 |
  | 1401 | 2 050 | 2 050 | 226 | 180.1 |
  | 1961 | 2 059 | 2 059 | 255 | 245.8 |
  | 2521 | 1 972 | 1 972 | 255 | 254.5 |

  Same signature as §4.1: small area, saturating amplitude.
* **Recommended fix (targeted):** resolve `shadows` inside `Initialize_City` the way
  `water_procedural` is already resolved 500 lines earlier — read `isSet()` and fall
  back to `cinematic() ? cine::kCity.shadows : <compile default>`. That keeps the
  cinematic profile where it is and does not leak city's fog/exposure into the other
  scenes' inits. Alternative (cleaner but wider blast radius): hoist only the
  `shadows` line of the profile to before `Initialize_City`.
* **Evaluate with:** the city sweep above, `--cinematic` before vs after; the fixed
  default must match today's `--cinematic --shadows`.
* **PERF: NOT MEASURED.** This adds a 512² spot atlas + 256² cube rebuild + a static
  bake at city init, and per-frame shadow sampling for the city's spots. Price before
  landing.

---

### 4.6 `--cinematic` chase: same setDefault, no init consumer of its own — INFERRED

`cine::kChase` also asks for `shadows = true` (`SceneTick.h:226` copies `kCity`), and
`createChaseScene:1474` runs after `Initialize_Chase`. Unlike city, **chase has no
`ShadowMaps_Rebuild` anywhere** — the only init-time rebuild sites in the tree are
`GREETS.CPP:2846`, `CITY.CPP:3066` and the snapshot harnesses
(`Snapshot.cpp:2981/3294/4507`). So chase renders with whatever `g_shadowMaps`
the last scene that rebuilt it left behind.

**INFERRED, not measured.** I did not determine what chase actually samples. It is
listed because the *intent* line exists and there is provably nothing in chase that
consumes it correctly. Recommend investigating separately; it may be that chase
never wanted shadows and the `kCity` copy is the bug.

---

## 5. The other failure mode: scope leaks out of `Initialize_Greets`

These are not ordering bugs — the `setDefault` lands fine. They are *global* writes
made by greets' init, which in the native demo runs **first** (`REV.CPP:1148`), so
they are in force for city, chase, fountain and crash. I list them because they come
from the same call sites and any fix will touch the same code.

### L1 — `shard_deferred` forces the whole demo onto the deferred path

`GreetsApplyInitDefaults:1071` sets `shard_deferred = true`. `RENDER.CPP:356-368`:

```cpp
bool deferredEnabled() {
    return deferred() || hdr() || deferred_quarter()
        || deferred_checkerboard() || shard_deferred();
}
```

and `RENDER.CPP:522` is the per-frame path selector for **every** scene. So in a bare
`./DEMO` (native, no flags — `FDS_DEFERRED_DEFAULT_ON` is 0), `Initialize_Greets`
runs first, sets `shard_deferred`, and city / chase / fountain / crash all render
**deferred** as a side effect of a greets shatter-quality flag.

**MEASURED by proxy** (the flag's value is what matters, not who wrote it) —
city `t=1401`, bare vs `--shard_deferred`:

```
2 063 903 px changed (99.53 %)  >4/255: 1 889 464  >12/255: 1 568 595
>32/255: 1 344 216  max 246  mean|d| on changed 77.3
```

A whole-frame render-path flip. Two riders:

* The `GreetsApplyInitDefaults` header comment (`GREETS.CPP:1036-1038`) states the
  init block holds "only flags the greets BUILD/BAKE reads … no other scene's render
  reads them". `shard_deferred` is the counter-example.
* Greets' own chunk split reads the **bare** `deferred()` (`GREETS.CPP:2353`), not
  `deferredEnabled()`. So in a bare run greets renders deferred *without* its
  chunk split — an inconsistent pair on the same frame.
* Under wasm the init order is reversed (`MainLoop.cpp:400-411`, greets **last**), so
  the two builds render city/chase/fountain/crash through different paths.

**UNKNOWN whether this changes the user's shipping look**, because he may always pass
`--deferred`. Verify with: `./DEMO --no-shard_deferred` vs bare and look at city.

### L2 — `cone_strength` 1.2 leaks backwards onto city/chase/fountain/crash

`BuildDiscoBall` (`GreetsDisco.cpp:434`, inside `Initialize_Greets`) sets 1.2, a
24× lift over the 0.05 compile default, before any other scene runs.
**MEASURED**, city deferred `t=1401`, 0.05 vs 1.2: 5 979 px changed but only **18 px
> 4/255, max 8** — negligible at that pose (city's cones are faint without
`--cinematic`, which would override to 2.0 anyway). Reported for completeness; I did
not test a fog-heavy city pose.

### L3 — `shadow_cone_cull` leaks

`GreetsDisco.cpp:439` sets it true at greets init; `Transform.cpp:1213` reads it in
**every** scene's shadow pass. The `.def` help prices it at ~900 px of shadow-edge
delta scene-wide on greets. **NOT MEASURED** on the other scenes.

### L4 — `shadow_lightmap_res` / `_planar` / `shadow_skip_animated` leak

Set in `GreetsApplyInitDefaults:1080-1082`, all three are engine-wide. The comment at
`GREETS.CPP:1073-1079` argues they are "inert on other scenes" because they only
matter while `shadow_lightmap` is on. That is right for `_res` and `_planar`.
`shadow_skip_animated` is **not** covered by that argument — `Transform.cpp:1273`
reads it in any scene's static shadow bake, and city does one when `--shadows` is on.
**MEASURED byte-null** at city `t=1401` with `--shard_deferred --shadows`
(0 px differ with vs without `--shadow_skip_animated`), which is one pose, not a proof.

### L5 — chase's water flags leak forward

`createChaseScene` sets `water_procedural=1`, `water_fresnel_composite=0`,
`water_variation=1`. Nothing resets them, so fountain, crash and greets inherit them.
`ApplyCinematicProfile`'s "every scene sets every field so nothing bleeds"
(`SceneTick.h:117-121`) covers the cinematic set only; the water trio is outside it.
**NOT MEASURED**; probably inert (no water material in those scenes), but the
invariant the cinematic profile relies on does not hold here.

### L6 — native and wasm disagree on init order

`REV.CPP:1148` inits greets **first**; `MainLoop.cpp:410` inits it **last**. Every row
in this section is therefore present natively and absent in the browser build.

---

## 6. Structural fix proposals

### 6.1 (Recommended) A late-write DETECTOR — makes the whole class loud

The bug is undetectable because nothing observes "this flag was read before it was
written". Add that observation.

```cpp
// FeatureFlags.h — sketch
#ifndef FDS_FLAG_READ_TRACKING
#define FDS_FLAG_READ_TRACKING 1        // -DFDS_FLAG_READ_TRACKING=0 to compile out
#endif

static bool * const g_boolRead;         // + g_floatRead / g_intRead, same shape
                                        // as the existing g_*Set arrays

static inline bool get(BoolId id) {
#if FDS_FLAG_READ_TRACKING
    g_boolRead[int(id)] = true;         // plain, idempotent store
#endif
    return g_boolVals[int(id)];
}

static inline void setDefault(BoolId id, bool v) {
    if (isSet(id)) return;
#if FDS_FLAG_READ_TRACKING
    if (g_boolRead[int(id)] && g_boolVals[int(id)] != v)
        reportLateDefault(kBoolDefs[int(id)].name, g_boolVals[int(id)] ? "1":"0",
                          v ? "1":"0");
#endif
    g_boolVals[int(id)] = v;
}
```

`reportLateDefault` prints, and exits 2 under a new `--strict_setdefault`:

```
[FLAGS] LATE setDefault: 'mirror_rtt' is being set to 1, but it was already READ
        (the reader saw 0). Move the setDefault before its first consumer.
        See docs/SETDEFAULT_AUDIT.md.
```

To keep the signal clean, add `FeatureFlags::clearReadMarks()` and call it at the top
of each `create*Scene()` / `Initialize_*`, so the detector reports the real bug shape
("read then re-defaulted *within one scene's setup*") and not the legitimate
cross-scene re-default (greets pinning `hdr_exposure` after crash read it) — although
arguably those deserve a quieter note too, since §5 shows several of them are bugs.

**Cost — honest accounting, NOT measured.** One byte store per `get()`. The flags read
per-pixel are already hoisted to tile-level constants
(`DeferredSurfaceKernel.cpp:1429-1485` hoists ~20 of them, and `CubeAttenFlags`
bundles 7 more specifically to stop per-tap flag reads), so the added stores land on
the per-tile prologue, not the inner loop. It is still a real store on a real hot
path and I will not call it free. Recommendation: land it default-ON in Debug and
behind an explicit `-D` in Release, and measure on the greets `t=5780` wall bench
before considering it for the shipping build. There is also a benign write race
(worker threads storing `true` over `true`); either accept it with a comment or make
the array `std::atomic<bool>` with relaxed ordering.

**What it would have caught:** every row in §4 (each is literally "written after
read with a different value"), on the first run, for free. It does **not** catch §5
(scope leaks are not ordering).

### 6.2 (Cheap companion) A one-line self-report

Have each `create*Scene()` call `FeatureFlags::printActive(stderr)`-style dump of the
*scene-default* writes it just made, so a run is self-describing. Near-zero cost;
does not catch anything by itself, but makes a bisect trivial.

### 6.3 (Not recommended) Move every scene's defaults to one point before all init

This is the obvious "fix the class" move and it collides with an existing, deliberate
design decision: the Init/Run split exists *because* a global set during one scene's
init leaks into every other scene's render (`GREETS.CPP:1031-1043`), and
`ApplyCinematicProfile` explicitly rejects save/restore ("No global save / restore, no
ordering assumptions", `SceneTick.h:119-121`). Doing this properly means a
per-scene flag *overlay* with save/restore at scene boundaries — a real refactor of
the flag system, not a move. Mentioned for completeness; I would not start here.

### 6.4 Retire the two ad-hoc workarounds once §6.1 exists

`CITY.CPP:2543-2545` (`water_procedural` re-derived via `isSet`) and
`GREETS.CPP:2855` (`forceEnable` on the static bake) are correct but hand-maintained:
the city one hard-codes `true` and must be kept in sync with **three** separate
`setDefault(water_procedural, true)` sites (`CITY.CPP:3689`, `CHASE.CPP:1463`,
`Snapshot.cpp:1117`). With a detector in place these can become ordinary moves.

---

## 7. Worklist, grouped for review

Counts: **6 dropped rows** (plus 1 dropped-but-worked-around), **6 scope leaks**,
**1 brief correction**. 95 `setDefault` call sites audited; everything not listed
below is HONOURED.

### Group A — obviously safe (intent clearly right, change is a line move, greets-scoped)

| Row | Change | Expected visible effect | Evaluate with |
|---|---|---|---|
| §4.3 `mirror_rtt_density` | move `GREETS.CPP:1192` into `GreetsApplyInitDefaults` | **none** (MEASURED: slots clamp to 512² at both 256 and 1024) | any greets snapshot; must stay byte-identical |
| §4.2 part 1 — `LightmapStampOrigBary` `forceEnable` | mirror what `LightmapBake_Static` already does | none today (masked) | `--shadow_lm_dynamic` alone at `t=5780`: today 0 px, after fix must reproduce the 51.9 % arm |
| §6.2 self-report line | additive logging | none | — |

### Group B — needs his eyes (real look change, cost is small or nil)

| Row | Change | What moves | Show him |
|---|---|---|---|
| **§4.1 `mirror_rtt`** | move `GREETS.CPP:1191` to the Init block | mirror-in-mirror panels go from flat screen texture to live reflection: ~1 700-2 500 px/frame, **every one > 32/255**, mean ~150/255 (MEASURED) | `--snapshot=greets` default sweep, `t=600/1000/1500/2100`, default vs `--mirror_rtt` — that pair **is** the before/after |
| §4.4 `greets_shard_randomness` | move to Init block, or change the intent to 1.0 | shard *shapes* at the `Y` shatter; 1.0 (today) vs 0.8 (intent) | interactive `Y`, or a scripted `--repro`; **he may prefer today's 1.0** — this is as likely a wrong-intent as a wrong-order |
| §5 L2 `cone_strength` scope | move the disco 1.2 out of init, or accept | city/chase/fountain/crash cones: MEASURED 18 px > 4/255 at city `t=1401`, max 8 | a fog-heavy city pose, which I did not test |

### Group C — needs a perf decision as well as a look decision

| Row | Change | Why perf |
|---|---|---|
| **§4.1 `mirror_rtt`** (also in B) | as above | 8 RTT jobs/frame ≤ 512² each; partially offset by the `Lighting()` skip at `GREETS.CPP:3777-3781` (~4 ms per its own comment). **Both unpriced.** Measure the `TailProf` `"RTT"` scope at `GREETS.CPP:3713` first |
| **§4.5 `--cinematic` city shadows** | resolve `shadows` at `CITY.CPP:3065` via `isSet` + profile | adds a 512² spot atlas + 256² cube rebuild + a static bake at city init, plus per-frame sampling. MEASURED look delta ~2 000 px/frame at mean \|d\| 180-254 |
| §4.2 part 2 — G-buffer lightmap planes | allocate unconditionally, or re-size after `createGreetsScene` | ~12.4 MB at 1080p **plus** the per-pixel write in the Mekalele hot loop that `Mekalele.cpp:76-79` says is exactly why they are conditional |
| §6.1 the detector | read-tracking store in `get()` | one store per flag read; hoisted out of the per-pixel loops but not free. Measure on the greets wall bench |

### Group D — investigate before deciding (I do not know the answer)

* §4.6 `--cinematic` chase shadows — chase has no shadow-map allocation at all;
  the `kChase` profile copies `kCity.shadows = true`. Is that intent or a copy bug?
* §5 L1 `shard_deferred` forcing the deferred path for the whole native demo —
  MEASURED to be a 99.5 % frame change *as a flag*, but UNKNOWN whether the user's
  normal invocation already passes `--deferred`, in which case it is latent.
  Also makes native and wasm render city/chase/fountain/crash differently (L6).
* §5 L3 `shadow_cone_cull` on the non-greets scenes — unmeasured.
* §5 L5 chase's water trio leaking into fountain/crash/greets — unmeasured, likely
  inert, but it breaks the invariant `ApplyCinematicProfile` documents.

---

## 8. How the measurements were made

All runs headless from `Runtime/`, `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`,
1920×1080 (`rev.cfg`), every flag passed as its own argv word (zsh does not
word-split; `--strict_flags` is on and would have aborted on a typo). Binary:
`Runtime/DEMO` as built at 2026-08-08 15:37 — note this build carries other agents'
in-flight working-tree edits to `FDS/RENDER/*` and `DEMO/FrameProfiler.cpp`; none of
those touch the flag-ordering paths measured here, but the frames are not from a
pristine HEAD build.

```sh
# census + frame, mirror_rtt
./DEMO --snapshot=greets --out=/tmp/base
./DEMO --snapshot=greets --out=/tmp/rtt --mirror_rtt

# cone_strength (which of 1.2 / 2.0 is live)
./DEMO --snapshot=greets@t=5780 --out=/tmp/cs12 --cone_strength=1.2
./DEMO --snapshot=greets@t=5780 --out=/tmp/cs20 --cone_strength=2.0

# shadow_lightmap, isolating the boot-time G-buffer allocation
./DEMO --snapshot=greets@t=5780 --out=/tmp/lmdyn    --shadow_lm_dynamic
./DEMO --snapshot=greets@t=5780 --out=/tmp/lmdyn_lm --shadow_lm_dynamic --shadow_lightmap

# cinematic city shadows
./DEMO --snapshot=city --out=/tmp/cine    --cinematic
./DEMO --snapshot=city --out=/tmp/cine_sh --cinematic --shadows

# shard_deferred as a render-path flip
./DEMO --snapshot=city@t=1401 --out=/tmp/city_bare
./DEMO --snapshot=city@t=1401 --out=/tmp/city_shard --shard_deferred
```

Diffs are per-pixel max-channel absolute difference over the `.ppm` pairs
(changed / >4 / >12 / >32 of 255, max, and mean |d| over changed pixels only).
