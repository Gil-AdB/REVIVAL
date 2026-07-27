# Sidecar-elimination migration plan

The `.MAT` sidecar (`Runtime/SCENES/<SCENE>.MAT`, read by
`MaterialImport_ApplySidecar` / `MaterialImport_ApplySceneDefaults` in
`DEMO/MaterialImport.cpp`) is being retired. Every editor-authored property it
persists moves into the **authoring sources** — per-surface values into a
custom LWO `SURF` sub-chunk, per-light / per-object / scene-level values into
LWS keywords — and flows through `tools/lwsread` into the `.FLD` using the
proven **flag-bit + conditional-payload record** idiom (the mechanism the
volumetric-beam / FdsFlareScale / FdsSceneEnvRefl work already validated
end-to-end). The runtime then reads the authored value from the FLD instead of
the sidecar.

This is a **writers-first** migration: Phase 1 lands every writer while the
existing sidecar reader keeps working byte-for-byte, so no shipping FLD and no
gate moves. Phase 2 (after the user re-saves greets once — the CHECKPOINT
below) retires the reader and rips out the `#k` split-marker scaffolding.

Provenance: SESSION_STATE.md "The big architecture decision (2026-07-11)" +
"The FLD/LWS extension mechanism". This doc is the concrete realization of that
direction.

---

## 0. The extension idiom (how every new authored property travels)

Proven by `Light_VolumetricCone` (2048), `Light_FlareScale` (4096),
`Scene_EnvDefaults` (2048 on the AmbientIntensity EndBehavior), `Scene_SkyColors`
(4096):

1. **Author** — LWS text keyword(s) per light/object/scene, or a custom LWO
   `SURF` sub-chunk per surface.
2. **`tools/lwsread`** parses it (LWSREAD.CPP for LWS keywords, LWOREAD.CPP for
   the SURF sub-chunk) into a new `FldMat`/`FldLight`/`FldObject`/`FldScene`
   field (`tools/lwsread/LWREAD.H`).
3. **`FLDSAVE.CPP`** sets a **free flag bit** on the record's flags word and
   writes a **conditional payload** after the record, ONLY when authored. FLDs
   without the bit stay byte-identical to the pre-extension format (prove with a
   regen diff — the shipping sources carry none of the new bits yet).
4. **Engine FLD loader** (`FDS/FLD/FLD_READ.CPP`) reads the conditional payload
   into the mirror `FldMat`/`FldLight`/`FldObject`/`FldScene` (`FDS/FLD/LWREAD.H`
   / `FLD_READ.H`), gated on the same bit; the scene-header reader STRIPS its bit
   after reading so EndBehavior consumers never see it.
5. **`FDS/FLD/FLD_CONV.CPP`** maps the field onto the live `Material` / `Omni` /
   `TriMesh` / `Scene`. `0`-sentinel = unset → legacy default. Mirror clones
   inherit via memcpy — sane by construction.
6. **Editor write-back** (`tools/editor_server.py`) patches the LWS/LWO on Save,
   regens via the scene's lwsread variant, installs the FLD (backup first).

`tools/lwsread` builds two binaries — `lwsread` and `lwsread_legacy`
(`-DLEGACY_VLUM`) — from ONE source tree. Editing the source updates both; rebuild
both. `FldVersion` stays **0.113** (the extension is bit-gated and backward
compatible; every prior extension kept 0.113).

### Free flag bits reserved by this migration

| record | flags word | bits in use | reserved here |
|---|---|---|---|
| material | `FldMat::Flags` (u16 `Surf->Flags`) | 1..1024 LWOB standard | **0x8000 = `Surf_RevExt`** (RVSF payload present). 0x0800/0x1000/0x2000/0x4000 stay free for future boolean markers (0x4000 earmarked `Rev_SingleShadowId`, see LWO_EDITOR_CUSTOM_FLAGS.md) |
| light | `FldLight::Flags` | 1/2/4/8/16/32/64/128/256 + EndBehavior 256/512/1024; 2048 VolCone; **4096 FlareScale (EXISTS)** | 8192 for a future per-light ext-payload |
| object | `FldObject::Flags` | 1 Parent / 2 Pivot / 4 PolygonSize / 4096 AlignToPath + EndBehavior 256/512/1024 | **8192 = `Object_FdsExt`** (per-object ext payload present) |
| scene header | `AmbientIntensity->EndBehavior` (no flags word of its own) | legacy ≤1024; 2048 EnvDefaults; 4096 SkyColors | **8192 = `Scene_FdsSceneDefaults`** (fastFog/boltFlash floats) |

**TRAP (from SESSION_STATE):** the light Flags word is OR-contaminated by
`ReadEndBehavior` (256/512/1024). Always confirm a bit is free of EndBehavior
contamination before claiming it — object flags carry the same contamination.

---

## 1. Complete inventory of sidecar-persisted keys → destination

### 1a. Per-surface — `SURF_SIDECAR_KEYS` (editor_server.py:113) + `SMOOTH_SIDECAR`

Destination: a **custom LWO `SURF` sub-chunk `RVSF`** ("ReViVal SurFace"),
carried by `FldMat` → `Surf_RevExt`-gated FLD material payload → live `Material`.

| key | type | engine target (`Material`) | sentinel / identity |
|---|---|---|---|
| `aoStrength` | float | `AoStrength` | 1.0 |
| `parallaxScale` | float | `ParallaxScale` | 1.0 |
| `tintR` / `tintG` / `tintB` | float×3 | `TintR/G/B` | 1.0 |
| `refractIor` | float | `RefractIor` | 0.0 → global |
| `refractive` | 0/1 | `Flags & Mat_Refractive` (0x1000) | 0 = off |
| `normalFlip` | 0/1 | NormalMap green-flip parity (`g_nmapFlipParity`) — **special, texture mutation, pairs with the normal-map assignment; see §1e** | 0 = as-loaded |
| `envRefl` | -1/0/1 | `EnvReflMode` | 0 = auto |
| `envBakeRes` | pow2 int | `EnvBakeRes` | 0 = unset |
| `waterProcedural` | -1/0/1 | `WaterProcMode` | 0 = auto |
| `smoothAngle` | float | **native LWO `SMAN` → `MaxSmoothingAngle`** (see §1b) | — |

**RVSF wire format (LWO, big-endian, even-padded IFF sub-chunk):**
`u16 revMask` + only the authored fields, in a fixed bit→field order. The mask
(one bit per field) preserves authored-vs-unset per field — required because
some fields are explicitly authored to a value that equals a default (e.g.
`siling|aoStrength|0`, `screen 4|refractive|0` in the live GREETS.MAT: authored
0, not "unset"). Booleans/tri-states store their value byte so "explicit 0"
survives.

**FLD material payload (native-endian, after the material record's `TFP1`, gated
on `Surf_RevExt`):** `u16 revMask` + the same authored fields. A material without
the bit reads nothing (memset-0 defaults → all identities). This is a
per-material, per-field-optional payload — the hottest record in the file
(hundreds per scene), so writer/reader field-order MUST stay locked; a single
desync corrupts the whole parse. It is provably inert for shipping content
(no shipping FLD carries `Surf_RevExt`; the material is `memset(0)` by the
caller before `ReadMaterial`).

### 1b. `smoothAngle` — native `SMAN`, not RVSF

`smoothAngle` is a **native LightWave surface field** (`SMAN` chunk →
`MaxSmoothingAngle`), so on authoring scenes it should take the native path,
not RVSF: add `smoothAngle` handling to `lwopatch.set_prop` (write `SMAN` as a
float radians value; the LWO stores radians, the editor shows degrees — convert),
and the value rides the existing `MaxSmoothingAngle` FLD field with no new bit.
This is round-trip-safe (survives stock LightWave) and drops the transitional
`SMOOTH_SIDECAR` peel entirely.

**Engine caveat to resolve in implementation:** today authoring-scene smoothing
is an *editor-only override* routed to the `MeshOps` registry
(`MeshOps_SetSurfaceSmoothAngle`, consumed by `MakeFacesIndependent`) that
"always wins" regardless of `--surf_smoothing_authored`. To retire the sidecar,
the authoring-scene normal rebuild must instead read the material's
`MaxSmoothingAngle` (the FLD-native value) so the authored `SMAN` drives it.
Verify `MakeFacesIndependent` honors `MaxSmoothingAngle` before dropping the
MeshOps override; if it doesn't, seed the MeshOps registry from
`MaxSmoothingAngle` at scene init. (FLD-patched scenes city/crash already
round-trip `smoothAngle` natively via `fldpatch`; this makes authoring scenes
match.)

### 1c. Per-light — `LIGHT_SIDECAR_KEYS` (editor_server.py:568)

| key | destination | status |
|---|---|---|
| `flareScale` | LWS **`FdsFlareScale <f>`** in the light's `AddLight` block → `Light_FlareScale` (4096) → `FldLight::FlareScale` → `Omni::FlareScale` | **Whole FLD + engine path ALREADY EXISTS** (chase L1 batch, 4a54af5). Migration = editor_server routing ONLY (a `patch_lws_light_flarescale` writer; drop `flareScale` from `LIGHT_SIDECAR_KEYS`). Zero engine/lwsread/FLD change. |

### 1d. Per-object — `OBJECT_KEYS` (editor_server.py:529)

| key | destination |
|---|---|
| `scale` | NEW per-object LWS keyword **`FdsObjectScale <f>`** (parsed in the object section like `PivotPoint`, sets `CurObj`) → `Object_FdsExt` (8192) + `u16 mask`+`f32 scale` payload → `FldObject` → `TriMesh::EditorScale`. `scale==1` DELETES the keyword (authored default). Rewriting every ObjectMotion keyframe's scale channel was judged too invasive (MaterialImport.cpp:523) — a single keyword is the agreed shape. |

### 1e. PBR map role assignments — `surface|role|path` (albedo/normal/height/roughness/ao/metallic)

NOT `SURF_SIDECAR_KEYS`; these are the map-file assignments (`ALLOWED_ROLES`).
**LWO1 has no PBR-map slots**, and these are the values the greets CHECKPOINT
protects ("his GREETS.MAT is the only record of the momy map assignments").

Two viable destinations:
- **(A, recommended for full sidecar death)** carry the six role paths as
  NUL-terminated strings inside the `RVSF` sub-chunk (mask bits + string block),
  mirror them through `FldMat` → FLD material payload → and load them at FLD
  time via the existing `MaterialImport_ApplyMapFile` path. Lets the sidecar die
  completely.
- **(B, minimal)** keep a **map-only sidecar** permanently (numeric + light/obj/
  scene keys all leave; only the six role→path lines remain). Cheaper, but the
  sidecar reader never fully dies.

The plan targets **(A)**, but it is the largest and least-critical sub-slice
(the maps render fine via the sidecar today), so it is sequenced LAST in Phase 1
and may be deferred to a follow-up without blocking the numeric/light/obj/scene
migration. `normalFlip` (§1a) rides with the normal-map assignment here (it is a
mutation of the assigned normal texture, not a scalar), so it is implemented
alongside (A).

### 1f. Scene-level — `kSceneDefaultKeys` (`scene:` lines, MaterialImport.cpp:626)

| key | FeatureFlags target | destination |
|---|---|---|
| `boltFlashPeak` | `bolt_flash_peak` | NEW top-level LWS **`FdsBoltFlashPeak <f>`** |
| `boltFlashRange` | `bolt_flash_range` | **`FdsBoltFlashRange <f>`** |
| `fastFogBottom` | `fast_fog_bottom` | **`FdsFastFogBottom <f>`** |
| `fastFogTop` | `fast_fog_top` | **`FdsFastFogTop <f>`** |
| `fastFogCell` | `fast_fog_cell` | **`FdsFastFogCell <f>`** |

All five → `Scene_FdsSceneDefaults` (8192) header payload (`u16 mask` + present
floats) → `FldScene` → `Scene` fields → applied via `FeatureFlags::setDefault`
**at the scene FACTORY, after `ApplyCinematicProfile`** (precedence: CLI/env >
authored > cinematic profile > compile default), exactly where
`MaterialImport_ApplySceneDefaults` runs today.

Scene-level `envRefl`/`envBakeRes` ALREADY migrated (`FdsSceneEnvRefl` /
`FdsSceneEnvBakeRes` → `Scene_EnvDefaults` 2048). These five are the remainder.
**Note:** no sidecar currently carries a `scene:` line and the editor has no
`scene:` writer — these keys are defined-but-unused today, so this slice is
low-urgency (defines the LWS/FLD path; an editor UI can drive it later).

### 1g. Already-native (NOT sidecar — listed for completeness)

`ALLOWED_PROPS` numeric (baseR/G/B, diffuse, specular, glossiness, luminosity,
transparency, reflection) round-trip through the native LWO chunks
(`lwopatch.VALUE_PROPS`) / `fldpatch`. `UV_KEYS` (uvProj/uvScale*/uvAxis)
round-trip through `CTEX`/`TSIZ`/`TFLG`. These are unaffected.

---

## 2. The `fldpatch` fallback for crash — DEAD

`do_save_fld` / `tools/fldpatch.py` were the write-back for scenes without
authoring sources. **Every scene is now source-authored** (crash promoted
2026-07-11, 470d7f1 — byte-parity regen from `Authoring/crash/CRASH.LWS`). So the
non-authoring branch (`if not SCENES[scene]["authoring"]`) is unreachable for the
registered scenes. The migration routes every per-surface value through the LWO
`RVSF`; `fldpatch`'s material patcher stays only as a dormant safety net. No new
crash-specific path is needed. (`fldpatch` is still used for the FLD *restore*
backup mechanism and remains a valid direct-FLD editor for any future
sourceless scene.)

---

## 3. Writer-first sequencing + the USER CHECKPOINT

```
Phase 1 (writers; reader untouched; every FLD + gate byte-identical)
  1.1  light flareScale → FdsFlareScale        (editor_server only; FLD path exists)
  1.2  scene: keys      → FdsFastFog*/FdsBoltFlash*  (LWS+lwsread+FLD reader+conv+editor)
  1.3  object scale     → FdsObjectScale        (LWS+lwsread+FLD reader+conv+editor)
  1.4  per-surface RVSF → Surf_RevExt payload   (LWO+lwsread+FLD reader+conv+lwopatch+editor)
  1.5  smoothAngle      → native SMAN           (lwopatch+engine smoothing-source fix)
  1.6  PBR maps + normalFlip → RVSF strings (§1e, option A)   [deferrable]

  ── writers all landed; sidecar reader STILL LIVE; both paths coexist ──

  ★ USER CHECKPOINT (below) ★

Phase 2 (STOP boundary for this campaign — do NOT start here)
  2.1  re-verify greets renders from LWO/LWS alone (sidecar empty/deleted)
  2.2  retire the sidecar reader (MaterialImport_ApplySidecar / _ApplySceneDefaults)
  2.3  DELETE the 7 `#k` collapse sites (§5) — not guard, DELETE
```

### ★ USER CHECKPOINT — required before Phase 2 (reader retirement) ★

The sidecar reader cannot be retired until the user re-saves greets **once**
through the browser editor, because the live `Runtime/SCENES/GREETS.MAT` holds
data that is not yet in the LWO/LWS sources:

- **The `momy#2` map assignments.** `momy#2` is a *runtime split* name; the
  second mummy is not a real surface in `Piramid.lwo` yet. The split-bake
  (6c6c972) is in place: on Save with the split in the payload, `bake_splits`
  reassigns the second polygon cluster to a real `momy2` surface in the LWO and
  rewrites the `momy#2|*` payload keys onto `momy2`. Until the user re-splits
  momy and Saves, `momy#2|*` references a surface that only exists live.
- The numeric per-surface values in GREETS.MAT (`amudim/stairs/... envBakeRes,
  smoothAngle, aoStrength, parallaxScale, envRefl, refractive`) — Phase 1's
  editor Save writes these into the LWO `RVSF` / `SMAN`, but only ON a Save.

**What the user must do (precise):** open the greets editor
(`make editor` → `DEMO.html?editor&scene=greets`), re-split `momy` into its two
instances, then hit **Save**. That single Save: (a) bakes the momy split into
`Piramid.lwo` (momy2 becomes a real authored surface), (b) writes every numeric
per-surface value into the LWO `RVSF` sub-chunks and `SMAN`, (c) with §1e-A,
writes the PBR map paths into `RVSF`, (d) regenerates + installs `GREETS.FLD`.
After that Save, `GREETS.MAT` is redundant and can be emptied/deleted, and the
scene reproduces from LWO/LWS alone — the precondition for retiring the reader.

Because GREETS.MAT + the momy textures + `Piramid.lwo`/`Hull.lwo` are the user's
**uncommitted, sacred** files, this re-save is his to perform (memory:
"Never run DEMO on screen"; the greets pin includes his uncommitted files). The
agent must NOT fabricate the momy2 bake or touch GREETS.MAT semantics.

---

## 4. Validation matrix (which gate pins which step)

HEADLESS only, dummy SDL drivers, from `Runtime/`.

| gate | recipe | pin | what it proves |
|---|---|---|---|
| render_gate | `tools/render_gate.sh` | mirror `4ac809e5f5323076de1a6d5ef2fb9e92`, cone `b41894f969d1f89dd2d7d794f160e286`, halo `166fa25a846668cc9b2d4dae2d800a7b` | deferred kernel / mirror / cones / fog unmoved by any engine FLD-reader or converter change |
| city | `FDS_CITY_ENV_PIXEL=1 ./DEMO --snapshot=city@t=1961 --out=<d> --deferred` (discard 1st run — cache keyed on CITY.FLD) | `37e62845c4d30eefa321730c5bb7e0b8` | CITY.FLD regen is byte-identical; converter change inert |
| fountain | `./DEMO --snapshot=fountain@t=2500 --out=<d> --deferred --hdr --glass-refract=1 --glass-test --profiler=0` | `51fff7cd38767d619280afe0498a6f24` | FOUNTAIN.FLD (legacy converter) unmoved; glass path inert |
| greets | pin recipe in SESSION_STATE (`FDS_GREETS_CAM=... --snapshot=greets@t=1588 ... --no-env_refl`) | **UNRELIABLE in this tree — see §6 deviation** | (spot-check the hash FAMILY is unchanged; not a byte-gate here) |
| chase | — | **DO NOT RUN.** Chase pins are stale + another agent owns chase now. |

**Per-slice protocol:** after every slice — (1) rebuild `tools/lwsread` (+legacy)
if its source changed; (2) rebuild native `DEMO` (`cmake --build build`) if the
engine changed; (3) regenerate each authoring scene's FLD from its unchanged
sources and `cmp` against the shipping FLD — **must be byte-identical** (the
extension bits are absent in the sources, so regen output is unchanged; this is
the core "writers landed, FLDs unmoved" proof); (4) `render_gate.sh` 3/3 PASS;
(5) city + fountain pins (city: discard-first). Greets: characterize the hash
family, confirm no NEW mode appears.

Traps carried from SESSION_STATE: city cache (discard first run after any
CITY.FLD install); greets 1-in-12 flip (here worse — §6); never `git add -A`
(stage explicitly; GREETS.FLD/MAT, momy textures, Piramid.lwo, Hull.lwo,
Authoring/chase/CHASE.FLD are sacred/uncommitted).

**New-path proof (the writers actually work):** since the shipping-FLD gates only
prove *inertness*, each slice ALSO proves the authored path offline without
disturbing shipping FLDs: patch a COPY of an authoring source with a non-default
value → run lwsread → confirm the FLD carries the bit + payload (a small
FLD-bytes check, or `FDS_FLD_VERBOSE=1` load trace showing the value reaches the
material/omni) → confirm the engine loads it without desync. Greets'
nondeterminism makes a rendered A/B unreliable, so the proof is at the
bytes/trace level, not the pixel level.

---

## 5. `#k` split-marker rip-out (Phase 2 — DELETE, do not guard)

User-confirmed 2026-07-11 (SESSION_STATE). `#k` is vestigial: it existed only
because the OLD live-only-split path couldn't bake, so a `momy#2` edit had to
COLLAPSE back to the real `momy` surface. Two facts kill it once sidecars go:
(1) splits now BAKE into the LWO (`lwopatch.split_surface`/`bake_splits`) → parts
are real authored surfaces, nothing transient to collapse; (2) the shell sends
explicit `payload.splits` with per-part world centroids, and `bake_splits`
matches parts GEOMETRICALLY, never by parsing `#k` from the name.

The **7 collapse sites** in `tools/editor_server.py` (each a
`re.sub(r"(#\d+)+$", "", name)` or `SPLIT_CHAIN_RE`), to be DELETED (not guarded)
in Phase 2:

1. `split_surface_sidecar_keys` (~L146) — `base = re.sub(...)`
2. `SPLIT_CHAIN_RE` matcher in `bake_splits` (~L271/286) — chained-split reject
3. `pop_uv_props` (~L389) — `out[re.sub(...)]`
4. `map_surface_name` (~L424) — `name = re.sub(...)`
5. `do_save_fld` (~L761) — `base = re.sub(...)`
6. `do_save_main` per-surface loop (~L932) — `if re.search(r"(#\d+)+$", ...)`
7. `do_save_main` UV loop (~L958) — via `map_surface_name`

After deletion a split becomes "make a real surface, reassign polygons, any
plain name" — free cosmetic naming. Do NOT add existence-aware-collapse or build
further on `#k`. This also dissolves the momy#1/#2-vs-momy2 naming question
(becomes a free cosmetic choice). Engine side is already clean —
`Editor_BaseSurfName` strips only `::mirUV`, never `#`.

**Constraint:** do not delete these while the Save rewrite is unproven — a
half-migrated Save that still emits `#k` names needs the collapse to route them.
They are load-bearing until §3's checkpoint + reader retirement, hence Phase 2.

---

## 6. Deviations found on contact with the code (recorded per mission)

- **The greets pin is not reproducible in this working tree.** Baseline
  measurement (2026-07-28, before any change): 10 headless runs of the exact
  SESSION_STATE greets recipe produced a scattered distribution — top mode
  `59220c4a9a8c65ad882905918d406af5` at only 4/10, `36a8b559...` 3/10, plus three
  singletons; NONE equals the pin `de3e9a5fb3aa39e008ef41b83f2b8d1b`. Causes:
  (a) render_gate.sh itself documents greets as nondeterministic (timing-dependent
  background lightmap bake) and EXCLUDES it for that reason; (b) the user's
  in-progress uncommitted greets wall/texture work (GREETS-new.FLD,
  `_bak_greets_wall_pre_*`, greets_t*_color.ppm in the tree) has moved the scene
  state off the 2026-07-11 pin. **Consequence:** greets cannot serve as a
  byte-identical gate for this migration. The hard byte-gates are render_gate +
  city + fountain (all reproduce exactly). Migration changes are proven inert on
  greets by CODE PATH (bit-gated FLD reads absent from the shipping FLD; sidecar
  reader untouched; lwsread regen not triggered during gates), and each slice
  spot-checks that greets' hash FAMILY gains no new mode.
- **`fldpatch` fallback is dead** (§2) — every scene is source-authored.
- **`flareScale` is half-migrated already** — the FLD/engine path exists; only
  the editor still routes it to the sidecar (§1c). Slice 1.1 finishes it with an
  editor-only change.
- **`smoothAngle` is native, not engine-only** (§1b) — it maps to `SMAN` /
  `MaxSmoothingAngle`, not RVSF, but needs the authoring-scene smoothing source
  switched from the MeshOps override to the material's `MaxSmoothingAngle`.
