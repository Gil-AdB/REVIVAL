# CHASE UPGRADE PLAN

Status: **planning deliverable — no implementation yet.** Written 2026-07-11 on
`fog-wt` from a full investigation pass (code read + LWS read + headless
snapshot survey). Builds on the campaign architecture in
`docs/SESSION_STATE.md`: **authored-first** (persistence in LWS/LWO via the
flag-bit + conditional-payload extension idiom), FeatureFlags (never raw
getenv), default-off code paths until user approval, headless md5 gates.

**REVISED 2026-07-12** after a code-grounded review pass — six gaps folded in
and **music sync committed** (was a punt). See **§8 Review revisions** for the
deltas; they modify C0/M1/C1/B1/B2/L1 and add **Stage S0 (music-sync
foundation)**. Read §8 alongside each stage — where §8 and an original stage
disagree, §8 wins.

The user's ask, verbatim:

> "I want to have blasters firing, I want to improve the camera work and
> object movement, particles on hits, the lighting, and everything else you
> can think of." — for the chase scene.

---

## 1. Ground truth — what the chase scene is today

### 1.1 Timeline and clock (measured, not assumed)

- `Authoring/chase/CHASE.LWS`: frames **1..1700 @ 30 fps** (authored intent
  ≈ 56.7 s). `FramesPerSecond 30` survives into the FLD; `FirstFrame 1`,
  `LastFrame 1700` are stored as ints (`FldScene::FirstFrame/LastFrame`,
  `FDS/FLD/FLD_READ.H:149-150`) and become `Scene::StartFrame/EndFrame`.
- `DEMO/CHASE.CPP:743`: `CHPartTime = EndFrame - StartFrame = 1699` Timer
  ticks (10 ms each) → **the scene runs ~17 s** and advances ~100 FLD
  frames/second: the animation plays at **3.3× the authored speed**.
  `CurFrame = 1 + t` exactly (CHASE.CPP:815).
- Consequence: Ship1's authored finale (a climb into the sky, keys at frames
  1702–1760; ship2's at 1734) is **never rendered** — the scene ends at frame
  1700, and `tick()` refuses to render at `t >= 1699`.
- The `--snapshot=chase` comment in `DEMO/Snapshot.cpp:800` ("chase runs
  ~0..2500") is **stale**: verified by md5 — frames at t=1600 and t=2400 are
  byte-identical (the harness re-dumps the last rendered VPage once
  `t >= 1699`). Valid snapshot range is **t = 0..1698**.

### 1.2 Authored content (Authoring/chase/CHASE.LWS, byte-parity source)

- **80 objects**: 78 static instances built from 9 LWOs — the water plane
  (`water.lwo`, uniform scale 37.568), `big_m` and ~76 mountain tiles
  (`m1..m5`, `mm7`, many instances each, single-key motions) — plus the two
  animated ships:
  - **Ship1.lwo** (the big "VELA" cruiser, 423 KB, textured, luminous
    `in engine` surface): **53 keys**, heading/pitch channels animated, bank
    channel almost always 0 (one -15.4 at key 460). Key density is *very*
    uneven — dense clusters (keys every 2–3 frames) at ~1408–1428, ~1495–1503,
    ~1623–1632: authored evasive wiggles.
  - **ship2.lwo** (small escort/pursuer): **29 keys**. Heading is 0 for most
    of the scene (it slides through turns without yawing); the one authored
    flourish is a bank move (B −37/−39.5) at keys 460–547.
- **7 lights**: one white point "Light" at (−2, 20127, −2) intensity 1.0 (the
  key/moon), plus **6 authored orange engine glows** (253,65,2, intensity
  0.5, range 50, `LensFlare 1`): 4 parented to Ship1 (±28, ±21, −88), 2 to
  ship2 (±15, 0, −64). No spots, no volumetric bits — chase predates all of
  that.
- **Camera**: ONE camera, 18-key dolly, `TargetObject 79` → **look-at Ship1
  for the entire scene**, `ZoomFactor 3.2`, constant rotation keys (the
  look-at overrides them). No cuts, no FOV animation, no roll.
- **Authored sky that never ships**: the LWS carries a 1998 backdrop
  gradient — `ZenithColor 0 40 80`, `SkyColor 120 180 240`, `GroundColor /
  NadirColor` — which lwsread ignores, so the engine renders a **black void
  sky**. The original author wanted a blue night-sky gradient.

### 1.3 Runtime code path (DEMO/CHASE.CPP)

- `Initialize_Chase` (740): loads `SCENES/CHASE.FLD`, `FZP=50000, NZP=2`,
  `Scn_Fogged|Scn_ZBuffer`, ambient (8,8,128) deep blue.
- `SceneCorrections` (501–544): a 1998-era hack that **bulldozes every
  authored omni**: `Size ×0.1`, `IRange=rRange=1000`, `FallOff=5000`,
  `Range ×100000`. Any newly authored light values will be stomped until this
  is migrated (see Stage L1).
- Water: `water.lwo` becomes a planar mirror (`Reflective_Surface_Setup`),
  rendered via a dedicated reflection pass `Reflected_Transform` (77–497)
  that y-flips every mesh + omni. **Particles are excluded from the
  reflection** — the FList insert at CHASE.CPP:491 is commented out.
- Shares city's procedural water: `water_procedural` default-on for chase
  (877), `pwater::RenderGlints` under `water_bump` (855).
- `ApplyCinematicProfile(cine::kChase)` (878) = kCity storm look (froxel
  worley fog, rain, shadows, HDR+bloom, anamorphic 3.0, CA/vignette) at
  exposure 0.45 — **but only under `--cinematic`**
  (`DEMO/SceneTick.h:162`). Default runs get none of it.
- `MaterialImport_ApplySceneDefaults("SCENES/CHASE.MAT")` (881) is a no-op:
  **no CHASE.MAT, no SCRIPTS/chase.params, no PRESETS/chase\*.flags exist.**
- TBR is already wired: `TBR_EnsureInit(ChaseSc, 256)` + `Scn_SpriteTBR`
  under `deferred_unified_tbr` (default 1). Deferred pipeline is compile-time
  default-on (`FDS_DEFERRED_DEFAULT_ON=1`, FDS/CMakeLists.txt:272).
- No music-sync hooks; scene time is the only clock.

### 1.4 What the frames actually look like (headless snapshot survey)

Default flags, `--snapshot=chase@t=100,400,800,1200,1600`:

- **t=100**: camera far behind; both ships are sub-20-pixel specks against a
  dark mountain; ONE huge orange flare blob (the merged engine glows)
  dominates the frame. Water is saturated procedural blue; sky pitch black.
- **t=400**: both ships silhouetted inside a single fused orange fireball —
  the flare sprites are several times larger than the ships. Pretty water
  reflection of the blob, but the ships read as "two dots in an explosion".
- **t=800**: best frame of the set — Ship1 large, center, over glinting
  water; but the four engine flares fuse into a white blob that swallows the
  whole tail half of the ship; ship2 is an unreadable speck at frame edge.
- **t=1200**: ships completely invisible inside one enormous white/pink
  flare; frame is effectively "glow blob over water".
- **t=1600**: both ships are distant dots with red halos, low over open
  water; mountains have run out; black featureless horizon.

`--cinematic --deferred` pass: dramatic improvement in mood (rain streaks,
purple worley-fog inscatter, vignette/grain, water reads violet-pink), but
the same composition problems: flares still swallow the ships, ships still
tiny for most of the timeline, black sky above the fog band, and full-screen
rain over open ocean reads odd with nothing for the rain to land against.

### 1.5 Defect / gap list distilled

1. **Engine-glow flares are catastrophically oversized** relative to ships
   (FlareSize = ISize, stamped in both render passes; `SceneCorrections`
   scaling makes it worse). The #1 visual defect today.
2. **Black void sky** — authored gradient dropped by the pipeline.
3. **Camera**: single distant trailing dolly; ships subtend a few pixels for
   >60% of the scene; zero cuts; no reaction to anything.
4. **Ship motion**: no banking into turns (bank ≈ 0 through hairpins at 3.3×
   speed), ship2 doesn't even yaw; motion reads as "sliding on rails".
5. **3.3× playback speed** — frantic, and amputates the authored finale.
6. Nothing happens: no blasters, no hits, no particles (`Sc->Pcl` never
   allocated for chase), no events of any kind.
7. No chase pin in the gate protocol, no params script, stale snapshot
   comment, particles excluded from water reflection.

---

## 2. Reusable systems inventory (what we build on)

| System | Where | State | Chase-relevant gaps |
|---|---|---|---|
| **BlasterBolts** module | `DEMO/BlasterBolts.{h,cpp}` (flags `greets_blasters`, `blaster_light[_intensity/_range]`) | **Scene-agnostic, built for chase.** 48-bolt pool, straight-line motion, additive billboard w/ baked 256² gradient tex, pool of 12 travelling omnis (`Omni_Active|Omni_FogTransient`) that re-homes per scene. Test harness in GREETS.CPP:2857-2877, 3193-3195 | No muzzle/impact flash, no decay envelope, no `hdr_glow_scale` compliance, billboard drawn post-tonemap (no bloom from the sprite itself), only 12/48 bolts get lights |
| **Particle machinery** | `Particle`/`Sc->Pcl` (`FDS/Base/FDS_VARS.H:540`), projection+insert `FDS/RENDER/Transform.cpp:1852-1888`, `TBR_Sprite`+`Spriter` AVX2 (`FDS/FILLERS/FILLERS.CPP`), trail quads `addParticleTrail` (Transform.cpp:398) | Engine-level, any scene. Fountain = worked example (8250 pcls, spawn/kill/gravity in `Particle_Kinematics`, 32×32 additive discs, HDR accumulate). Chase already has TBR + `Scn_SpriteTBR` | Chase never allocates `Pcl`; needs a spawn/update fn + per-particle Face wiring; reflection-pass insert commented out (CHASE.CPP:491) |
| **bolt_flash** transient light | FOUNTAIN.CPP:178-191, 1009-1018, 2770-2789; flags `bolt_flash_peak/range/decay` | Pooled non-stationary omni, exponential decay envelope, `Omni_FogTransient` (froxel fog flashes), `hdr_glow_scale`-compliant | The exact pattern BlasterBolts' lights *lack* — port the envelope for muzzle/impact flashes |
| **Camera machinery** | `Camera` = 4 TCB splines (`FDS/Base/Camera.h`), eval in `FDS/RENDER/Transform.cpp:262-286`, `Kick_Camera`/`CalcPersp` (`FDS/CAMERAS/CAMERAS.CPP:27-33,78`), `View` is a swappable pointer (`FDS/Base/FrameState.cpp:98`) | Look-at via FLD TargetObject works today (chase uses it). FOV is a per-frame float + `CalcPersp` — cheap to punch. Pose-override-after-Animate pattern proven (CITY.CPP:3310-3343, GREETS.CPP:2330-2368) | **No cuts, no shake, no shot-list anywhere in the codebase** — camera-director would be new (small) code; AABB framing helper exists (`Editor_ComputeFocus`, MaterialEditor.cpp:1349) |
| **Authored spots + volumetric beams** | LWS `LightType 2` + `ParentObject` + `VolumetricLight 1` + `VolumetricLightIntensity g` → FLD bit 2048 + payload → `Omni_ForceVolCone` + `VolBeamGain` (FLD_CONV.CPP:579-583); per-frame parent-aim Transform.cpp:320-343 | End-to-end proven by city's 46 headlights; beams render without `--draw_cones`; cone turbulence/swirl flags (`cone_turbulence/_scale/_speed`, `cone_swirl`) SIMD in all cone paths | Chase FLD has zero spots today; `SceneCorrections` would stomp new lights (must migrate first). FLD light-flag bits 256/512/1024 are EndBehavior-contaminated; 2048 taken; **next free bit is 4096** |
| **Atmosphere stack** | froxel `fast_fog` + blobs/worley/inscatter (`FDS/RENDER/DeferredFastFog.cpp`), `fast_fog_dist_dim` horizon dimming, rain (kCity profile), HDR/bloom/anamorphic (`FDS/RENDER/Hdr.cpp`), shadows | All reachable for chase through `cine::kChase` under `--cinematic`; scene-level sidecar keys exist for fog bounds (`MaterialImport.cpp:578-601`) | Chase-specific tuning absent (fog bounds are city's −400..420); rain/anamorphic values are city's |
| **Authoring loop** | `lwsread_legacy` regen (byte-parity proven: 747,511 B, `Authoring/chase/README.md`), editor registry has `"chase": authoring:True, legacy:True`, editor writes LightColor/LgtIntensity/LightRange to LWS + surface props to LWO + `flareScale` sidecar, `pin_scene.py` verifies identity | The full extension recipe (SESSION_STATE §"FLD/LWS extension mechanism") applies directly | `FdsScene*` scene-level keywords not implemented yet (in flight by another agent — coordinate, don't duplicate); no light-position write-back; `tools/lwsread` + `editor_server.py` owned by another agent at write time |
| **Params scripts** | `Runtime/SCRIPTS/<scene>.params` (docs/PARAM_SCRIPTS.md): any flag, `name @ t = v` keyframes, lerp floats/step bools, hot-reload, CLI wins | Chase is a supported name; file just doesn't exist | Event-*like* but not event-*based*: good for ramps/moods, wrong tool for discrete fire/hit events |

---

## 3. Prerequisites / infra (Stage C0 — do this first)

**Goal:** a chase gate so every later stage is verifiable, plus the two traps
cleared out of the way.

1. **Chase pin.** Recipe (from `Runtime/`, dummy drivers, like every gate):
   - `./DEMO --snapshot=chase@t=100,400,800,1200,1600 --out=<dir> --deferred`
     → md5 per frame (default look), and
   - `./DEMO --cinematic --deferred --snapshot=chase@t=800,1600 --out=<dir>`
     → md5 (cinematic look).
   Record both pin sets in SESSION_STATE's gate table. Chase has no bakes and
   no known nondeterminism (pinned srand, fine clock off in snapshots), but
   prove stability with a 3-run majority before declaring the pin.
2. **Regen parity pre-check.** Before ANY authoring edit:
   `python3 tools/pin_scene.py CHASE.LWS Runtime/SCENES/CHASE.FLD
   --legacy-vlum --pick ...` (exact command in `Authoring/chase/README.md`)
   must still exit 0. After the first intentional authored edit, byte-parity
   with 1998 is gone *by design* — from then on the FLD md5 + frame pins are
   the regression net, and the pre-edit FLD goes to
   `Runtime/SCENES/.backups/` (editor flow does this automatically).
3. **Stale-comment fix** (Snapshot.cpp:800 "~0..2500" → "0..1698") — fold
   into the first code slice, zero risk.
4. **`SceneCorrections` migration is a gating prerequisite for all authored
   light work** (it overwrites every omni's Size/Range/FallOff at load).
   Plan: reproduce today's *effective* values as authored LWS values
   (editor write-back covers LgtIntensity/LightRange; `flareScale` via the
   light sidecar), regen, verify the default pin moves only in the intended
   way (flare size), then delete the hack. This is Stage L1 below but listed
   here because Stages B2/L2/L3 depend on it.
5. **Coordination:** `tools/lwsread/*` and `tools/editor_server.py` are being
   edited by another agent right now. Any new LWS keyword slices must rebase
   on their landed state; if their `FdsScene*` scene-level keyword mechanism
   lands first, reuse it verbatim for chase scene-level keys.

Size: **S**. Risk: none (read-only + doc + one comment).

---

## 4. The stages

Naming: C = camera/clock, M = motion, B = blasters/combat, L = lighting/look,
X = extras. Each stage is independently landable and gated. All new code
paths are FeatureFlags, **default off** until the user approves the look;
authored changes go through `Authoring/chase/` + `lwsread_legacy` regen.

---

### Stage L1 — Look foundation: flare sanity, sky, chase-tuned atmosphere

**Goal:** make every frame readable before adding content. Kills defect #1
(flare blobs), #2 (void sky), and tunes the cinematic profile for an ocean
chase instead of a city street.

**Design:**
1. **Engine-glow flare retune (authored).** The six engine glows keep their
   omni light contribution but get sane sprite sizes: author per-light
   `flareScale` (editor light sidecar key exists today; `Omni::FlareScale`,
   Omni.h:92) — target: flare diameter ≲ ship silhouette at the t=800
   framing. Longer-term this becomes an LWS extension keyword
   (`FdsFlareScale <f>`, same slice as other light keywords) so the sidecar
   can die per the campaign direction.
2. **`SceneCorrections` retirement** (prereq item 4): move today's effective
   range/falloff into authored `LightRange`/`LgtIntensity` lines, delete the
   code hack behind an A/B flag first (`chase_legacy_omni_hack`, default
   on→off after approval), then remove.
3. **Sky restoration (authored intent from 1998).** Options:
   - **(a) Backdrop-gradient keywords** *(recommended)*: parse the standard
     LWS `ZenithColor/SkyColor/GroundColor/NadirColor` lines (already in
     CHASE.LWS!) through lwsread → FLD scene-header extension (flag bit +
     conditional payload, FLDs without it byte-identical) → engine paints a
     vertical gradient into sky pixels before fog (the froxel sky-paint hook
     `DeferredFastFog.cpp:2897` and the legacy clear are both candidates).
     Works for city/crash later too — it's the generic answer.
   - (b) Authored sky-dome LWO (zero engine code, but adds fill-rate and
     interacts with FZP=50000 + the reflection pass).
   - (c) Star particle layer only (cheap, doesn't fix the horizon).
4. **Chase-tuned cinematic profile.** Fork `cine::kChase` from "kCity +
   exposure" into real values: fog bottom near the water plane, top below
   the mountain peaks so summits poke out; `fast_fog_dist_dim` for the
   horizon fade; decide rain (see decisions); anamorphic tuned to water
   glints. Deliver as profile edits + a `SCRIPTS/chase.params` for
   time-varying density (e.g. thicker fog in the mountain gap section
   t≈900–1200).

**Builds on:** editor light write-back, flareScale sidecar, extension
mechanism, froxel fog + dist-dim, params scripts.
**Authored vs code:** flare/light values + sky colors authored; gradient-sky
render + profile fork are code (gradient default-off until approved).
**Verification:** default pin should be *unchanged* until the flag flips;
cinematic-pin A/B screenshots at t=100/400/800/1200/1600; regen byte-diff
limited to the edited light/scene records.
**Size: M.** **Risk: low-medium** (sky pass touches the deferred composite;
keep it a separate pass, off path preserved exactly).

**Decision points:**
- Sky: gradient (authored 1998 colors) vs starfield vs both (gradient +
  sparse stars)?
- Rain over open ocean: keep the city storm, thin it, or drop rain and use
  spray/mist instead?
- How prominent should the engine glows be — navigation-light subtle or
  afterburner hero?

---

### Stage M1 — Object movement: banking, ship2 heading, pacing, finale

**Goal:** ships that fly like aircraft, at a pace the user chooses, with the
authored ending restored.

**Design:**
1. **Banking from spline curvature (authored via tool, not runtime code).**
   A patcher script (`tools/chase_bank.py`, same family as
   `add_city_beam_flags.py`) that: evaluates each ship's position keys,
   computes heading-rate per key, writes bank angles into the LWS **B
   channel** (clamped, smoothed, e.g. max 40–55°), and *preserves* every
   authored non-zero bank/roll (Ship1's −15.4 @460, ship2's −37/−39.5 roll
   move @460–547). Idempotent and re-runnable with a gain parameter, exactly
   like the city beam stamper. Authored-first: the result is keyframes in the
   LWS, no engine change, fully editor-visible.
2. **Ship2 heading fix**: same script fills the near-constant-0 H channel
   from the velocity tangent (again preserving authored non-zero values).
3. **Pacing (decision, then one-line code change).** `CHPartTime` mapping in
   CHASE.CPP:743/815 controls playback rate. Options: keep 17 s (status
   quo), 1.5× longer (~25 s, matches the old "~2500" comment — likely the
   original intent), or full 30 fps (57 s, probably drags). Music has no
   event sync — lengthening chase shifts the following scenes against the
   track; the user must arbitrate pacing by eye/ear.
4. **Finale restoration**: extend `LastFrame`/`PreviewLastFrame` to 1760 so
   Ship1's authored climb-out plays as the scene button (needs pacing choice
   first; independent of it, +60 frames ≈ +0.6–2 s depending on rate).
5. **Engine-glow throttle response (code, default-off `chase_engine_mod`)**:
   modulate the 4+2 engine-glow `ISize` by spline-derived speed/acceleration
   (deterministic function of CurFrame — snapshot-safe). Punches the glows
   during the evasive clusters and the finale climb.

**Builds on:** LWS keyframe format (fully documented in the toolchain
inventory), pin_scene regen, `Spline_Calc_3D` determinism.
**Verification:** regen + eyeball A/B at the four evasive clusters
(t≈939–973, 1408–1428, 1495–1503, 1623–1632); default pin re-pinned (motion
changes every frame — expected); bank gain=0 run must regen byte-identical
(idempotence proof).
**Size: M** (script + one clock constant + small ISize hook).
**Risk: low** — all authored data; the script is rerunnable/tunable.

**Decision points:**
- Scene duration: 17 s / ~25 s / other?
- Bank aggressiveness (arcade 60° vs stately 30°)?
- Restore the climb finale? (Recommended yes — free authored drama.)

---

### Stage C1 — Camera work: shot structure, then polish

**Goal:** replace "one distant trailing dolly, 17 s, no cuts" with a cut
sequence that keeps the ships > ~15% of frame height most of the time.

**Design — two layers:**
1. **Authored shot list (no engine code).** The engine supports exactly one
   FLD camera, but *cuts don't need engine support*: author them as
   **near-instant spline discontinuities** — two camera keys 1 frame apart
   (18 keys today → ~30–40). TCB interpolation across a 1-frame gap at 100
   frames/s is a clean hard cut. Keep `TargetObject 79` (Ship1 lock) for
   most shots. Proposed storyboard to iterate with the user in the editor:
   - t≈0–250: low water-level shot, ships scream toward camera and past
     (composition: ships grow from dots to full frame — fixes the current
     "dots leaving us behind" opening);
   - t≈250–650: close chase cam tucked behind/beside ship2, Ship1 large
     ahead (both ships finally share a readable frame);
   - t≈650–950: side-on tracking shot through the mountain gap, parallax;
   - t≈950–1250: the current high crane shot (keep — it's the best of the
     existing keys) but ~40% closer;
   - t≈1250–1550: frontal shot during the evasive clusters — blaster fire
     (Stage B1/B2) plays toward camera;
   - t≈1550–end: pull-back + tilt up for the restored climb finale.
   Where per-shot framing needs to look at ship2 or a midpoint instead of
   Ship1, drop TargetObject for that stretch by authoring explicit rotation
   keys (Euler mode is what the FLD falls back to without a target object —
   but that's per-camera, not per-shot; so shots that can't use the Ship1
   lock use authored H/P keys, which the 1-frame-cut idiom handles fine).
2. **Camera-director polish (small code, default-off `chase_cam_fx`).** A
   post-`Animate_Objects` pose modifier in the chase driver (the proven
   override point — same slot CITY/GREETS pins use):
   - **shake**: deterministic damped noise on `View->ISource` + `IRoll`,
     driven by scene-time-keyed events (near-miss/hit table from Stage B2),
     amplitude ∝ 1/distance-to-event; value-noise on CurFrame → identical in
     snapshots;
   - **FOV punch**: brief IFOV kick (−5..−10°) + `CalcPersp(View)` on flyby
     /hit events (FOV-after-Animate pattern verified in inventory);
   - both zeroed when the event table is empty → flag-off = pin-identical.

**Builds on:** TCB camera splines + TargetObject (already in the FLD),
View-override pattern, deterministic scene clock.
**Authored vs code:** shot list 100% authored (LWS camera keys); shake/FOV
punch = code, default-off.
**Verification:** per-shot snapshot at each cut boundary ±1 tick (prove no
interpolation smear); cinematic pin re-pinned after user approves the
storyboard; `chase_cam_fx=0` must reproduce the approved pin exactly.
**Size: L** for the authored storyboard iteration (it's the artistic bulk),
**S** for the shake/FOV code.
**Risk: medium** — purely aesthetic iteration cost; technically trivial.
The 1-frame-cut idiom should be validated with a 2-key probe first (TCB
overshoot check) before authoring the full list.

**Decision points (the big one):**
- Approve/edit the six-shot storyboard above (this is a taste call — I can
  deliver 3 candidate shot lists as snapshot strips to choose from).
- Keep permanent Ship1 lock vs mixed framing?
- Shake: subtle documentary vs heavy action-cam?

---

### Stage B1 — Blasters firing

**Goal:** ship2 fires visible bolts at Ship1 (and optionally receives return
fire), with muzzle flashes and travelling light, integrated with the water
reflection.

**Design:**
1. **Wire the existing BlasterBolts module into the chase driver** — it was
   built for this (pool re-homes per scene automatically). Hook points per
   the greets template: `BlasterBolts_Init()` in init;
   `Fire/Update/EmitLights` after `Animate_Objects`, before `Lighting`;
   `Draw()` after the main `Render`. New flag `chase_blasters` (default 0)
   so greets' `greets_blasters` stays independent.
2. **Muzzle positions (authored-first).** Recommended: an object-level LWS
   extension keyword on the ship `LoadObject` blocks —
   `FdsMuzzle <x> <y> <z>` (repeatable, parent-local coords, flows through
   the standard flag-bit 4096 + conditional payload into the FLD object
   record; ship2 gets 2 wing muzzles, Ship1 one tail turret). Interim (same
   slice, behind the flag): hardcoded parent-local offsets in the chase
   driver so the look can be iterated before the keyword lands — but the
   keyword is the deliverable; the campaign is killing exactly this kind of
   code residue.
3. **Fire pattern**: a small deterministic fire table in the driver keyed on
   scene time (`{t, shooter, burstLen, targetLead}`), bursts aligned to
   Ship1's authored evasive clusters (that's *why* it wiggles at 1408–1428!).
   Aiming = evaluate Ship1's position spline at t+flightTime and fire at the
   lead point with an authored miss-offset per burst (miss ⇒ near-miss water
   hits for Stage B2; hit ⇒ impact events). All spline-derived →
   deterministic → snapshot-gateable.
4. **Module upgrades (gaps found in the inventory):**
   - muzzle flash + per-bolt decay: port the fountain `bolt_flash`
     exponential-decay envelope into BlasterBolts as an optional
     spawn/impact flash API (pooled, `Omni_FogTransient`);
   - multiply bolt-light ISize by `hdr_glow_scale()` under HDR (parity with
     fountain — currently missing, will over-glow the cinematic pin);
   - bolt colors: per-emitter, not per-module (ship2 hot red/orange vs
     Ship1's return fire cyan — both contrast the blue water; final palette
     is a user pick).
5. **Reflections:** enable the commented particle/bolt insert in
   `Reflected_Transform` (CHASE.CPP:491) — or, cheaper and sufficient for
   bolts: mirror the 12 bolt omnis' flare stamps, which the reflection pass
   already does for scene omnis. Bolt streaks doubling on the water is a
   big cheap win; validate cost at 48 bolts.

**Builds on:** BlasterBolts, bolt_flash envelope, extension mechanism,
deterministic clock, HDR stack.
**Authored vs code:** muzzles + (later) fire events authored; module wiring
+ fire table + envelope port = code, default-off.
**Verification:** `--chase_blasters` off ⇒ pins byte-identical; on ⇒ new
reference strip at the firing windows (t≈1000, 1410, 1500, 1630); 3-run md5
majority (bolt spawn uses the pinned-srand path — must be stable); wasm
link gate.
**Size: M.** **Risk: low-medium** (module exists; the new surface is the
envelope + muzzle keyword; TBR strip budget 256/strip needs a bump check
with bolts + Stage B2 particles).

**Decision points:**
- Combat story: one-sided pursuit (ship2 → Ship1 only) vs exchange
  (recommended: pursuit for acts 1–2, one return-fire beat in act 3)?
- Bolt palette + cadence (sparse sniper shots vs strafing bursts)?
- Do bolts bloom (draw pre-tonemap, heavier) or stay crisp post-tonemap
  cores with omni-driven glow (current module behavior, recommended)?

---

### Stage B2 — Hit particles and impact feedback

**Goal:** bolts land somewhere: sparks on hulls, splashes on water, flashes
in the fog, camera reacts.

**Design:**
1. **Event source:** the Stage B1 fire table already knows each bolt's
   outcome at spawn (hit ship / miss → water intersection point + time,
   both spline-deterministic). Emit an impact-event list consumed by
   particles (here), flashes (here), and camera shake (C1). No physics, no
   broadphase — this is choreography, not simulation.
2. **Impact bursts (fountain machinery, chase-owned spawner):** allocate
   `ChaseSc->Pcl` (~2048), a `Chase_ParticleKinematics` modeled on the
   fountain outer-spray loop (radial cone burst from the impact normal,
   gravity, ~0.5–0.8 s `Charge` lifetime), `TBR_Sprite` 32×32 additive discs
   tinted by bolt color for hull sparks; **water misses spawn vertical spray
   columns** (white-blue, gravity-heavy) — near-misses marching across the
   water toward Ship1 is the money shot of the whole plan.
3. **Debris:** short-lived streak particles using the existing
   `InitTrail`/`addParticleTrail` quad path (glowing shrapnel arcs with
   gravity). No mesh chunks — no art exists, sprites read fine at chase
   distances.
4. **Hit flash:** transient pooled omni at the impact (bolt_flash pattern:
   peak ~300–500 pre-`hdr_glow_scale`, decay ~0.1) — under the cinematic
   froxel fog these become visible air-flashes for free
   (`Omni_FogTransient` is already froxel-aware).
5. **Scorch decals: recommend AGAINST** in this campaign — the hull shares
   materials across the ship (per-face albedo surgery on a shared texture, or
   a decal system that doesn't exist). Poor payoff at these camera distances;
   the damage story reads through flashes/smoke instead. If damage
   persistence is wanted: a luminous "venting" trail from a `FdsMuzzle`-style
   authored point after the big act-3 hit (smoke-gray trail particles),
   which is cheap and dramatic.
6. **Reflection**: hull-spark bursts near the water should reflect — this is
   the same CHASE.CPP:491 insert as B1; enable once, gate both.

**Builds on:** Particle struct/TBR sprite stack, fountain kinematics
patterns, bolt_flash, froxel fog, B1 event list.
**Authored vs code:** all code (default-off `chase_impacts`), event times
derive from the B1 table; nothing new authored except (optional) the venting
point.
**Verification:** flag-off pin-identity; deterministic re-run md5 ×3 at
impact timestamps; TBR overflow check (spans are drop-safe but visible —
budget test at max simultaneous burst); perf snapshot (`--profiler`) at the
busiest event.
**Size: M–L** (the spawner + water-splash tuning is the bulk).
**Risk: medium** — first new per-scene particle spawner since fountain;
mitigated by copying its proven loop structure and the snapshot determinism
protocol (24-run gate if any flake appears, per memory).

**Decision points:**
- Damage narrative: sparks only / one dramatic hit on Ship1 with venting
  trail through act 3 (recommended) / shields-flare look (additive shell
  flash instead of sparks)?
- Splash height/density of the near-miss water columns (subtle vs
  Hollywood)?

---

### Stage L2 — Lighting redesign

**Goal:** from "one white point + six orange blobs" to a lit ocean chase:
moonlight key, engine trails, combat light, mood.

**Design:**
1. **Key light**: author the white point into a cool moon (color/intensity
   via editor write-back today), high angle kept. Optionally a second, dim
   warm rim light opposite (authored, cheap — forward path handles omni
   count fine at 7+few).
2. **Engine exhaust cones (the flagship item):** per ship add 1–2 authored
   `LightType 2` spots at the engine cluster, parented, aimed backward,
   `VolumetricLight 1` + `VolumetricLightIntensity ~2–3`, warm orange —
   volumetric beams render *without* `--draw_cones` (bit-2048 path, city
   precedent). Add `cone_turbulence`/`cone_swirl` (chase.params) →
   **turbulent glowing engine wakes**. Cost bound: 2–4 small cones vs city's
   46 (+3.2 ms was greets' *many-cone* figure; chase will be far under).
   Requires L1's SceneCorrections retirement (spot params must survive
   load); note authored spots get flare-stamp skip automatically
   (FLD_CONV.CPP:130-137) so this doesn't re-introduce flare blobs.
3. **Combat lights**: B1/B2 already emit; this stage tunes them against the
   HDR/bloom pin (bloom_threshold currently kCity's 245 — likely down to
   ~230 so bolt cores bloom but water glints don't).
4. **Shadows**: keep cinematic-only (`kChase.shadows=true` already). Over
   open water there are almost no receivers; the mountain-gap shots (C1
   shot 3) are the only stretch where ship shadows could land — evaluate
   there with `--shadows` A/B and decide by eye; not worth default cost
   otherwise.
5. **Env probes on hulls**: per-surface `envRefl` force-bake on Ship1's hull
   surfaces would give the cruiser painted-metal sky/water reflections in
   the close shots. Bake is init-time; EnvBake.cpp is under another agent's
   hands right now → schedule last, coordinate. Optional (only worth it if
   C1 delivers real close-ups).

**Builds on:** VolumetricLight end-to-end mechanism, cone turbulence, editor
light write-back, HDR/bloom stack, env_refl tri-state.
**Authored vs code:** almost entirely authored (LWS lights + params values);
zero new engine features except possibly a `FdsFlareScale` keyword landing
here if not in L1.
**Verification:** regen diff shows only new/edited light records; cinematic
pin re-pin with user approval; `--no-draw_cones` remains irrelevant (bit-2048
per-light opt-in); perf check with cones on.
**Size: M.** **Risk: low** — every mechanism proven in city/greets.

**Decision points:**
- Engine wake: subtle heat-shimmer cones vs bright afterburner spears?
- Moonlight color temperature and how dark the ocean troughs go
  (exposure 0.45 today under cinematic — re-tune after sky lands)?

---

### Stage X1 — Cherry-pick extras (each independent, sized, user-selectable)

| Item | What | Effort | Payoff |
|---|---|---|---|
| **Spray wake** | When a ship flies low over water (t≈290–530 Ship1 is at y≈100–200), spawn a fan of spray particles + brighten glints along the track. Pure code atop the B2 spawner. | S–M | High — screams "speed", sells the water |
| **Speed streaks** | Sparse `addParticleTrail` motes streaming past the camera in close shots (camera-relative spawn box, deterministic). | S | Medium-high in cockpit-adjacent shots |
| **Searchlight beat** | Act-2 authored spot on ship2 sweeping a volumetric cone across the water hunting Ship1 (TargetObject-style aim at an authored null path or simple parent-aim). | M | High mood; unique frame |
| **Horizon treatment** | `fast_fog_dist_dim` + `_far` tuned so the black horizon becomes a fog-luminous band (inert-at-0 flag already merged). Params-only. | S | Medium — fixes t≥1500 emptiness with L1's sky |
| **Near-miss audio stub** | No music-event system exists; do NOT build one for this. Camera-shake events land on musical accents by *authoring the fire table times* against the track once, by ear. Document the chosen times in the fire table comments. | S (discipline, not code) | Medium |
| **Lens droplets** | Fountain's `Render_LensDrops` post pass exists; a 2–3 s borrow after the biggest splash near-miss. | S | Small-medium, cheeky |
| **Motion blur** | Does not exist in the engine (LWS `MotionBlur 0` ignored; no velocity buffer). Honest assessment: out of scope — the anamorphic+shake+streaks stack fakes the energy cheaper. | — | (listed to close the question) |
| **Ship1 climb sparkle** | During the restored finale climb, engine-mod (M1.5) to max + a one-off long exhaust cone + bloom — a "punch out" ending frame. | S | High as a closer |

---

## 5. Proposed landing order (REVISED — see §8)

```
C0  infra: pins + parity + stale comment + occlusion + determinism  (S→M)
S0  music-sync foundation: Modplayer_GetPosition + beat-map + event table (M)
L1  flare sanity (+fusion) + SceneCorrections retire + sky + atmosphere  (M)
M1  banking / heading / pacing(from music) / finale     (M)
B1  blasters firing (event-table-driven, occluded, HDR-parity)  (M)
B2  hit particles + flashes + splashes (occluded, reflect-capped)  (M–L)
C1  camera storyboard (TCB-cut params) + shake/FOV polish  (L authoring, S code)
L2  lighting redesign (engine cones, key, bloom tune)   (M)
X1  cherry-picks per user taste                         (S each)
```

S0 lands right after C0 because the beat-map + event table is the spine both
combat stages and the camera cuts hang off (§8.A). Its two-layer design also
*is* the fix for the snapshot-determinism gap (§8.B), so it must precede B1.

Rationale:
- **L1 before everything visual**: no point judging blasters or camera work
  while flares swallow the ships and the sky is void. It also unblocks all
  authored-light stages (SceneCorrections).
- **M1 before C1**: camera shots are framed against final ship motion —
  authoring shots first would mean re-authoring after banking/pacing change.
- **B1→B2 before C1's final pass**: the storyboard's action shots (frontal
  fire, near-miss splashes) need the action to exist to frame it. C1's
  1-frame-cut probe (risk item) can be validated any time earlier.
- **L2 last of the majors**: bloom/exposure tuning is only meaningful once
  sky, fog, bolts and splashes are all emitting.
- Each stage re-pins on user approval; default-off flags mean `master`-style
  safety on `fog-wt` throughout — flag-off runs must stay pin-identical at
  every intermediate commit.

---

## 6. Decision summary for the user (look choices, not implementation)

0. **Music sync**: DECIDED (2026-07-12) — YES, do it. Events (fire/hit/cut)
   lock to the track via a beat-map (§8.A). This also reframes #1: pacing
   should follow the **music length for chase's slot**, not a free taste
   number — see §8.G.
1. **Pacing**: keep the frantic 17 s, or slow to ~25 s (and restore the
   authored climb-out finale — recommended)? Now constrained by the music
   (§8.G): the "right" duration is however long chase's segment of the track
   runs. Establish that first, then this becomes "keep vs restore finale".
2. **Combat story**: ship2 hunts Ship1 one-sided, or an exchange with one
   return-fire beat? Does Ship1 take a visible hit (venting-trail damage
   through act 3) or stay untouchable?
3. **Weather & sky**: authored blue gradient night sky — with or without the
   city-style rain storm? (Alternative: clear night + spray/mist, saving
   rain for city's identity.)
4. **Camera storyboard**: approve/amend the six-shot list (I can render 2–3
   candidate storyboards as snapshot strips before any authoring).
5. **Fire & flame palette**: bolt colors (red-orange vs green vs cyan),
   engine wake subtle vs afterburner, bloom amount on both.

---

## 7. Verification appendix

- **Every run**: from `Runtime/`, `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.
- **Chase pins (to be established in C0):**
  - default: `./DEMO --snapshot=chase@t=100,400,800,1200,1600 --out=<dir> --deferred`
  - cinematic: `./DEMO --cinematic --deferred --snapshot=chase@t=800,1600 --out=<dir>`
  - md5 each ppm; 3-run majority to establish; any flake ⇒ the 24-run
    binomial protocol (memory: measurement-tool-traps) before blaming a race.
- **Authoring edits**: regen via `lwsread_legacy` from `Authoring/chase/`
  (README recipe); byte-diff the FLD against the backup — only the intended
  records may move. Pre-first-edit: `pin_scene.py --legacy-vlum` must pass.
- **Default-off proof per code stage**: flag off ⇒ pins byte-identical.
- **Suite**: `./tools/render_gate.sh` ALL PASS + `make wasm` links, per the
  SESSION_STATE gate table (chase gets added to that table in C0).
- **Traps carried over**: none of the chase path touches the greets 1-in-12
  flip or the city env cache; chase has no bakes. New traps to watch: TBR
  span budget (256/strip today) once bolts+bursts land, and the
  `Reflected_Transform` particle insert doubling sprite counts when enabled.

---

## 8. Review revisions (2026-07-12) — six gaps folded in + music sync

A code-grounded review found six things the original stages under-addressed.
Two are correctness/methodology holes (B, C) that must be resolved before the
stages that depend on them; one is a committed new capability (A, music sync);
three are refinements (D/E/F). Verified against the code, not assumed.

### 8.A — Music sync (DECIDED: do it) + the unifying event-table design

The modplayer interface (`Modplayer/Modplayer.h`) has `Modplayer_SetOrder`
(jump the track) but **no position getter** — so today event timing can only
be authored by ear. Fix is small: add **`Modplayer_GetPosition(handle) ->
{order,row,tickInRow, songTick}`** to the Rust submodule (`Modplayer/
modplayer`, it already tracks this internally to play) + the C header.

**The design that makes sync and determinism the same thing:**
1. **Beat-map** (authoring-time, deterministic, checked in): a table mapping
   song position (order:row) → chase scene-tick, built once by scanning the
   song's tempo/speed schedule from chase's start order (MOD/XM tempo can
   change mid-song, so this can't be a constant BPM — it's a scan). Ship it
   as `Authoring/chase/chase.beatmap` (or generate at load from the getter +
   a pinned start order). A small tool builds/refreshes it (add_city_beam
   family).
2. **Event table** keyed in **musical units** (bar/beat or order:row):
   `{musicPos, kind(fire/hit/cut/camfx), params}`. Resolved through the
   beat-map to **scene-tick offsets at load**.
3. **Runtime = pure function of `t`.** Per frame, bolts/particles/camera-fx
   state is **reconstructed from the resolved event table** at the current
   CurFrame — NOT accumulated across ticks. So a bolt in flight at t is
   "event fired at t_fire, position = lerp along its path by (t−t_fire)",
   computed fresh. This is beat-locked (times came from the beat-map) AND
   snapshot-deterministic (jump-to-t reproduces it — see 8.B).
4. **Live tightening (demo only, not snapshots):** optionally read
   `Modplayer_GetPosition` live to correct drift; the authoritative timing
   stays the pre-resolved table so snapshots and live agree.

This is **Stage S0** in the landing order, right after C0.

### 8.B — Snapshot determinism for stateful systems (BLOCKING for B1/B2)

Verified: `RunChaseSnapshot` (Snapshot.cpp:816-818) does `Timer=ts;
tick()` — **jumps to t and ticks ONCE**, no simulation from 0. Splines
survive (pure function of Timer); a stateful bolt/particle POOL would not (a
bolt fired at 1400, alive at 1410, wouldn't exist). The original B1/B2
"3-run md5 at firing windows" gate is therefore **unworkable against a
pool-based module.** Resolution = 8.A.3: reconstruct bolt/particle state as
a pure function of `t` from the event table. The existing `BlasterBolts`
module is a stateful spawn/update pool — S0 either wraps it with a
stateless "which bolts are alive at t & where" reconstruction, or the chase
combat path drives a pure-`t` variant. Decide in S0; **B1 cannot be gated
until this is in place.** (Alternative — make the chase snapshot simulate
from 0 — is heavier and fights the dt-from-Timer snapshot model; rejected
unless S0's pure-`t` reconstruction proves impractical.)

### 8.C — Bolt/particle depth occlusion vs terrain (BLOCKING for B1/B2 look)

Verified: `BlasterBolts_Draw` (BlasterBolts.cpp:197) projects the bolt quad
and blits it **post-tonemap, additive, with only a near-plane clip — no
scene-depth test**. Fine in greets (open space); over chase's mountains a
bolt/particle behind a peak will render **in front** of it. Fold into B1
(bolts) and B2 (sprites): sample `ZPage16` at the fragment (or per-vertex
depth-reject the quad against the tile's depth) so combat elements occlude
against terrain and ships. The bolt quad already carries per-vertex RZ, so
the data is there; the draw just doesn't test it. Small but non-optional —
without it the whole combat layer reads as an overlay, not in-world.

### 8.D — TCB hard cuts need spline-param control, not just 1-frame spacing

The original C1 "1-frame discontinuity = clean cut" is incomplete: TCB keys
take tangents from **neighboring** keys, so two keys 1 frame apart still
produce a fast smooth *swoop* unless the cut keys are authored with
**tension = 1 (or linear continuity)** at the boundary. C1's "2-key probe"
must specifically validate the tension/continuity params that kill the
tangent, not just the spacing. If the FLD camera format / lwsread doesn't
carry per-key TCB params through, that's the real work item to surface early.

### 8.E — Water-reflection cost: reflect bolts, cap particles

Enabling the CHASE.CPP:491 reflection insert re-runs `Reflected_Transform`
(re-transforms every mesh + omni) over the added elements. 48 bolts ≈ fine;
B2's ~2048 impact particles → ~4096 reflected sprites through that pass is a
perf cliff. **Reflect the bolts (cheap, big visual win); do NOT reflect
dense impact bursts** — or cap reflected particles to the nearest-N / the
water-splash columns only (which are the ones that read in reflection
anyway). Measure at max simultaneous burst (`--profiler`).

### 8.F — Flare fix is additive fusion, not just size

L1's `flareScale`-down helps but six co-located additive engine glows still
**fuse into a blob** at intensity. Alongside the size reduction: consolidate
Ship1's four glows into fewer (2?) and/or drop the additive intensity, so
the fix addresses the overlap, not only the per-sprite footprint. Judge at
t=400/800/1200 where the fusion is worst.

### 8.G — Pacing is a music decision, not a free number

The 3.3× playback (§1.1) and the pacing question (M1.3, decision #1) are now
constrained: chase's duration should match **its slice of the track**.
Establish where chase sits in the song (start order → end order for the
scene's musical phrase) via `Modplayer_GetPosition` + the beat-map, then the
scene length falls out of the music instead of taste. The finale-restoration
(+60 frames) then either fits the phrase or gets its own musical cue.

### Stage S0 — music-sync foundation (NEW, lands after C0)

**Goal:** the getter + beat-map + event-table spine, proving out 8.A/8.B
with zero visual change (default-off; no events authored yet).
**Ships:** `Modplayer_GetPosition` (Rust submodule + header + cargo rebuild
path already wired), the beat-map builder tool + `Authoring/chase/chase.
beatmap`, the event-table loader (musical-units → resolved scene-ticks), and
a pure-`t` reconstruction harness proven on a throwaway test event.
**Verification:** getter returns monotonic position under live play (manual/
logged); beat-map resolves a known order:row to the expected tick; a test
"fire at bar N" event reconstructs identically across a 3-run jump-to-t
snapshot (the determinism proof for 8.B); default chase pins byte-identical
(nothing authored yet). Submodule change → note the modplayer commit SHA in
the FLD/asset provenance.
**Size: M.** **Risk: low-medium** — Rust submodule edit + cargo rebuild is
the only unusual surface; the getter is a read of existing playback state.
**Decision points:** beat-map granularity (row vs beat vs bar) for authoring
events; whether live-tightening is worth it or pre-resolved timing is enough
(recommend pre-resolved only, add live later if drift shows).
