# SESSION STATE — glass / editor / authoring campaign (updated 2026-07-11)

> ## 2026-08-10 — `--shadow_lm_dynamic` IS A NO-OP, AND OPENING ITS GATE COSTS 1.7 ms FOR NO VISIBLE GAIN
>
> User: *"regarding `--shadow_lm_dynamic` — what would that give us? perf/looks/
> neither? can you show me? and will a longer/more complex bake give better
> results?"* **Answer: neither, because as shipped the flag does nothing at all;
> and when its second gate is forced open the lightmap path is 1.7 ms/frame
> SLOWER with a sub-visible look change that a richer bake cannot improve.**
> All numbers below measured on an isolated worktree built at HEAD `7953bab`
> (`/Users/gil-ad/work/rev-lmdyn`) so concurrent agents' uncommitted
> `FDS/RENDER` work could not contaminate them; run against the main tree's
> `Runtime/`. **No default was changed.**
>
> ### 1. THE FLAG IS INERT — THERE ARE TWO GATES AND IT ONLY OPENS ONE
>
> `--shadow_lm_dynamic` is **byte-identical to the shipping frame at all 18
> poses** (the 16 of `docs/greets_review_poses.txt` + his two new ones,
> `t=5967` / `t=5987`), **0 px, flat arm and `--greets_displace` arm alike**.
> Not "look-neutral" — *inert*. The scene is fully deterministic here (two A-vs-A
> reruns: 0 px), so 0 is a real zero.
>
> **The second gate is the G-buffer plane allocation, and greets closes it.**
> `resolvePixelLightmap` (`DeferredShadowSampling.h:52`) returns null unless
> `gb.lightmapMF` is non-empty, and that plane is allocated **only** by
> `EngineGBuffer_Resize` (`Mekalele.cpp:85`) under `FeatureFlags::shadow_lightmap()`.
> Greets sets `shadow_lightmap` in **`GreetsApplyRunDefaults`** (`GREETS.CPP:1228`),
> which runs at `createGreetsScene` (`:4393`) — *after* every resize call site
> (`Snapshot.cpp:153`, `SDL2.cpp:433` at boot, `ReproHarness.cpp:130`). So the
> planes are never allocated, every pixel's `pl.lm` is null, and no value of
> `shadow_lm_dynamic` can matter. **This is the same defect class as `mirror_rtt`,
> fixed 90 lines away in `7953bab`.**
>
> **MEASURED, the positive control that proves it is the allocation and not
> something else** (t=5743, one binary, only the flag set changes):
>
> | arm | vs shipping default |
> |---|--:|
> | `--shadow_lm_dynamic` | **0 px** |
> | `--shadow_lm_dynamic --shadow_lightmap_texel_density=1` (atlas crippled 4.5x coarser) | **0 px** |
> | `--shadow_lightmap` alone (planes allocated, `lmKernelEnabled` still false) | **0 px** |
> | `--shadow_lightmap --shadow_lm_dynamic` | **868 274 px (41.87 %)** |
>
> A deliberately crippled atlas changing nothing is the proof the atlas is not
> being read. Corroborated on the `--repro` (real per-frame) path: A-vs-B there
> is 1 444 px against that harness's own **1 051 px** A-vs-A noise floor of
> identical signature (all >32/255, mean |Δ| 124) — i.e. indistinguishable —
> while A-vs-C is 870 433 px.
>
> **Consequence: `shadow_lightmap` is read by NOTHING after init.** Its only
> readers are allocation sites (`Mekalele.cpp:85`, `GreetsMirror.cpp:3051`,
> `MirrorShatter.cpp:655/940`) and `LightmapStampOrigBary` / `LightmapBake_Static`
> (both force-enabled for greets). It is **not** a per-pixel sample gate — the
> comment at `GREETS.CPP:1112-1117` justifying its run-phase placement ("it's the
> per-pixel SAMPLE gate the deferred kernel reads for EVERY scene") is factually
> wrong, and is what put it on the wrong side of the resize.
>
> ### 2. WITH THE GATE FORCED OPEN: PERF IS WORSE, NOT BETTER
>
> `--bench=scene@scene=greets,t=T,iters=20 --deferred_prof=1`, **min-of-6
> interleaved**, run 1 after build discarded, load **9.8–11.6** throughout,
> 1920×1080, 12 workers. Arm A = shipping default, arm C = `--shadow_lightmap
> --shadow_lm_dynamic`.
>
> | | t=5743 | t=5780 | t=5814 |
> |---|--:|--:|--:|
> | frame ms A → C | 49.90 → **51.70** | 50.05 → **51.80** | 48.49 → **50.14** |
> | **Δ frame** | **+1.80** | **+1.75** | **+1.65** |
> | `gbuffer` wall (raster) | +0.17 | +0.23 | +0.22 |
> | `gbuffer` thrsum (core-ms) | +3.52 | +1.94 | +3.41 |
> | `lighting-w1` wall | +1.19 | +1.85 | +1.15 |
> | `lighting-w1` thrsum (core-ms) | **+25.49** | +13.73 | +6.84 |
> | `lighting-w2` wall | +0.02 | −0.02 | −0.02 |
> | static bake ms | 55.1 / 54.9 | 54.3 / 55.6 | 54.2 / 54.7 |
>
> **Same sign at all three poses, on the two phases the mechanism predicts.**
> The hypothesis under test — *"lm ON means cube taps only test movers
> (dynamic-only tap = 2 cache lines not 4 since `af1f8f8`), so lighting should
> get cheaper"* — **is refuted by measurement.** The saving is real but smaller
> than what replaces it, and there are two costs, both visible in the table:
> 1. **Raster:** the lightmap arm allocates and *writes* two extra G-buffer
>    planes (`lightmapMF` u32 + `lightmapST` u16 = 6 B/px = 12.4 MB at 1080p)
>    in Mekalele's hot loop → `gbuffer` +0.2 ms wall / +2–3.5 core-ms.
> 2. **Lighting:** `sampleBilinearPlanar` costs *more* than the tap it replaces —
>    a world-space projection onto the face's dominant cardinal plane, a bbox
>    map, and a bilinear gather from a 0.09 GB atlas of per-face mini-atlases
>    with far worse locality than the shadow cube — **and it still pays the
>    dynamic-only tap** on every face where `dynBaked` is true.
>
> The flag's own doc already said "MEASURED NEUTRAL-to-NEGATIVE… planar sampler
> +0.8 ms w1"; this is the same sign, roughly double the magnitude, and now with
> the raster half attributed too.
>
> ### 3. LOOKS: SUB-VISIBLE, AND IT IS 95 % A ONE-LSB SHIFT
>
> Contact sheet, all 18 poses, before | after | diff:
> `docs/img/fogwt/lmdyn_contactsheet.png`. Tight 4x crops at the four
> highest-amplitude poses: `lmdyn_t5743_tight.png`, `lmdyn_t5773_tight.png`,
> `lmdyn_t5814_tight.png`, `lmdyn_t5958a_tight.png`; displace arm:
> `lmdyn_disp_t5743_tight.png`, `lmdyn_disp_t5814_tight.png`.
>
> A-vs-C moves 17–55 % of pixels at every pose, which sounds enormous and is
> not. **Delta histogram at t=5743 (868 274 changed px):**
>
> | \|Δ\| | px | % of frame |
> |---|--:|--:|
> | **exactly 1** | **827 485** | **39.91** |
> | 2 | 19 463 | 0.94 |
> | 3–4 | 9 203 | 0.44 |
> | 5–8 | 5 511 | 0.27 |
> | 9–16 | 3 937 | 0.19 |
> | 17–32 | 2 221 | 0.11 |
> | 33–64 | 449 | 0.022 |
> | 65–255 | **5** | 0.000 |
>
> **95.3 % of all changed pixels differ by exactly one LSB**, and the shift is
> directional: **98.1 % of them get BRIGHTER**, signed mean +0.37 B / +0.21 G /
> −0.00 R over the whole frame. The lightmap composite very slightly
> *under*-shadows relative to the per-pixel reference, in the blue-green of the
> corridor lamps. The genuinely visible residual is a few hundred to a few
> thousand pixels on thin geometry silhouettes — the lintel top edge, the far
> lattice, column edges. Side by side the two frames are indistinguishable.
> `--greets_displace` behaves the same (t=5743 45.3 %, t=5814 41.2 %, t=6097
> 26.3 % — mean |Δ| 0.55–0.63, max 138/96/7).
>
> ### 4. "WILL A LONGER / MORE COMPLEX BAKE HELP?" — NO, AND THAT IS MEASURED
>
> The atlas is per-mesh `res = clamp(ceil(sqrt(meanFaceArea) x density), 8, 128)`.
> Swept the **cap and the density together** (so every mesh actually sharpens),
> comparing each against the per-pixel cube tap as reference at three poses:
>
> | | atlas | bake | t=5743 >12 / max | t=5773 >12 / max | t=5814 >12 / max |
> |---|--:|--:|--:|--:|--:|
> | cap 128 / density 14.2 (default) | 0.09 GB | 54 ms | 4 248 / 138 | 3 852 / 70 | 893 / 96 |
> | cap 256 / density 28.4 | 0.32 GB | 191 ms | 3 925 / 138 | 3 304 / 69 | 822 / 96 |
> | cap 512 / density 56.8 | **1.28 GB** | **670 ms** | 3 917 / **138** | 2 835 / **69** | 748 / **96** |
>
> **A 14x atlas and a 12x bake buys an 8–26 % reduction in an already-sub-visible
> population and moves the max channel delta by 0 or 1.** The 40 % one-LSB field
> does not move at all. Picture: `docs/img/fogwt/lmdyn_bakeres_t5773.png`
> (reference | cap 128 | cap 512 | residual — the three renders are
> indistinguishable and the residual is unchanged).
>
> **So visible quality is NOT bake-limited.** The residual is invariant under
> spatial resolution because it is not spatial: **inferred** mechanism is the
> atlas's **8-bit quantisation of the shadow factor** plus **double filtering**
> (a 4-tap bilinear PCF at bake time, quantised to a byte, then bilinearly
> re-interpolated at sample time) against the runtime's single 4-tap PCF
> evaluated at the pixel's own world position. More texels do not add bit depth,
> and no amount of them makes a bake-time evaluation land on the render-time
> sample point. Raising resolution is the one lever that was tested and it is
> the wrong lever.
>
> **What a richer bake could add, with honest estimates — all `inferred`:**
>
> | item | what it buys | effort | verdict |
> |---|---|---|---|
> | higher res where visible | measured above: ~nothing, at 14x the store | done | **no** |
> | more bits per texel (u16 factor) | would remove the 1-LSB field, which is invisible anyway | medium (format + sampler + 2x store) | **no** |
> | baked PCF / soft edges | bake already does a 4-tap PCF; wider kernel = softer than the runtime reference, i.e. a *different* look, not a truer one | small | only as a look choice |
> | baked penumbra (area light) | genuinely impossible at runtime today — real soft shadows. This is the ONLY item that buys something the cube tap cannot | large (multi-sample light, bake time x N) | the only interesting one, and it is a look project, not a perf one |
> | drop the 3 moving-omni slots | `allocate()` takes `numCubeOmnis` = **11** (`LightmapBake.cpp:373`) but the bake `continue`s on any omni without `Omni_StaticShadow` (`:487`) and the kernel's `cubeOmniStatic` gate can never read them — **3/11 = 27 % of the atlas is allocated, touched at 255, never written, never read** | small | free win *if* the path is ever used |
>
> ### 5. VERDICT
>
> **Neither perf nor looks — and as shipped, not even that: nothing.** Ranked:
> 1. **Best value, and it needs no look decision:** greets pays a **54 ms startup
>    bake and 0.09 GB** for an atlas that is provably never read. Skipping
>    `LightmapBake_Static` when the planes will not exist is pure win. Already on
>    the backlog; still not done.
> 2. **If you want the lightmap path evaluated for real**, the `shadow_lightmap`
>    `setDefault` has to move from `GreetsApplyRunDefaults` to
>    `GreetsApplyInitDefaults` (with the leak-onto-other-scenes concern the old
>    comment raised re-checked, since the flag is allocation-scoped, not
>    per-pixel). Until then `--shadow_lm_dynamic` cannot be evaluated by flag
>    alone, and **any past measurement of it that did not also pass
>    `--shadow_lightmap` measured nothing** — see the correction in
>    `docs/OPTIMIZATION_BACKLOG.md`.
> 3. **Do not default `--shadow_lm_dynamic` ON.** Even with the gate opened it is
>    +1.7 ms/frame (+3.5 %) for a change no one can see.
>
> Full 18-pose x 3-arm PPM set and the bench logs are on disk at `/tmp/lmdyn/`
> (untracked, ~2 GB); the committed evidence is the contact sheet + the six crops.

> ## 2026-08-10 — HIS 12-14 FPS: THE WIN IS 17 ms, BUT ONLY ON A LINE THAT OMITS `--deferred`
>
> Follow-up to `f4088a9` (`fds::DeferredPathEnabled()`). Three GREETS.CPP sites
> corrected: the Piramid chunk split (`:2427`) and the forward `Lighting()` gate
> (`:3867`) now ask `DeferredPathEnabled()` instead of `FeatureFlags::deferred()`,
> and `mirror_rtt` / `mirror_rtt_density` move from `GreetsApplyRunDefaults` to
> `GreetsApplyInitDefaults` — GreetsMirror's `wantRtt` (`:1401`) is evaluated
> during `Initialize_Greets`, so a run-phase default arrived **after** the
> decision and was inert (measured: `0 first-order RTT` slots, no `[MIRROR-RTT]
> slot` lines at all).
>
> **MEASURED at HEAD `af1f8f8`, his pose/res** (`t=3122`, 1512×848,
> `--greets_displace --texture_filter=1`, min-of-6 interleaved, run 1 discarded,
> load 7.2–8.3), two binaries from one tree differing only in GREETS.CPP:
>
> | | before | after |
> |---|--:|--:|
> | frame ms | 66.14 | **49.07** (−17.07, −25.8 %) |
> | BAKE | 15.54–16.41 | **3.22–3.61** |
> | LGHT | 6.38–6.45 | **0.91–0.95** |
> | RNDR | ~41.6 | ~41.3 |
>
> **THE FLAG THAT DECIDES THE SIGN.** The same A/B **with `--deferred` passed
> explicitly** measures **45.70 → 49.53 ms, i.e. +3.83 ms SLOWER**: there
> `FeatureFlags::deferred()` was already true, both predicate fixes are no-ops,
> and all that is left is the RTT slot build the `mirror_rtt` move switches on.
> So this change is a large win on **his** line and a small cost on any line that
> spells `--deferred` out — which includes the pin recipe and the render gates.
> A bench that passes `--deferred` cannot see this fix at all; the first batch
> here did exactly that and reported the wrong sign.
>
> **LOOK: this one MOVES, broadly.** 83–99.5 % of pixels change at every one of
> the 16 review poses (1920×1080; note `--repro_xres/--repro_yres` are read only
> by the `--repro` harness, `ReproHarness.cpp:240`, and are INERT on `--snapshot`),
> mean |Δ| 3.6–6.7/255, max ~200 — a broad, essentially zero-mean shift
> (mean luma +0.02 to +0.47), not a darkening. At his own pose it is 38.8 % of
> pixels and it **removes a defect**: near-black pixels (luma < 8) go
> **2 350 → 4** — the black gash on the right wall in
> `docs/img/fogwt/deferredfix_t3122_before.png` is gone in `_after.png`.
> Mechanism: the chunk split was never happening on his line, so the per-cube-face
> bsphere cull had nothing to reject and 59 556 displaced faces never got
> `NoShadowCast`. Contact sheet (all 17 poses, before | after):
> `docs/img/fogwt/deferredfix_contactsheet.png`.
>
> **PINS DO NOT MOVE — and that is a warning, not a comfort.** greets
> `778fa6acd85a69cf241babefcdaf598e` 4/4 on **both** arms, fountain
> `8db68ccb59416e9a44037e9f387b7bd9` 4/4, city `3cbe42b166847e40f7071eedb48d613c`
> 4/4, `render_gate` 3/3. The greets pin is **blind** to this change: its recipe
> passes `--deferred` (so the predicate fixes are inert) and `t=1588` shows no
> RTT panel. A byte gate that spells the flag out cannot certify a fix about the
> flag being absent.
>
> **CORRECTION to the block below (same session, better data).** The lightmap
> density per-frame delta was re-measured at HEAD on a quiet box (load 3.2–7.9,
> min-of-6 interleaved): `t=5743` 49.17 → 49.33 and `t=5780` 48.70 → 48.84 —
> **neutral at both poses, +0.15 ms, inside the run-to-run spread**. The −1.76 ms
> at `t=5780` recorded below was measured at load 11–30 and was noise. The bake
> and the memory reproduce exactly: atlas 5.61 → 0.09 GB, peak footprint
> 7.46 → 1.53 GB, `[GREETS-BAKE] waited` 1050.2 → 53.6 ms.

> ## 2026-08-09 — THE SHIPPING GREETS ARM BAKED A 5.61 GB LIGHTMAP AND NEVER READ IT
>
> Follow-up to the `--greets_displace` 19.4 GB finding below: the user approved
> extending `--shadow_lightmap_texel_density=14.2` to the SHIPPING arm, so the
> `setDefault` moved out of the `if (greets_displace())` branch into the main
> `GreetsApplyInitDefaults` block. `--greets_displace` now advertises **two**
> companions, not three.
>
> **THE WIN, flat arm, greets `t=5743`, same binary, `…density=0` vs default:**
>
> | | legacy | default 14.2 |
> |---|--:|--:|
> | atlas store (`[LM]` line) | 5.61 GB | **0.09 GB** |
> | peak footprint (`/usr/bin/time -l`) | 7.44 GB | **1.50 GB** |
> | static bake, min-of-9 interleaved, load 11–17 | 1104 ms | **54 ms** |
> | greets-entry join wait (`[GREETS-BAKE] waited`), load 31 | 3497 ms | **221 ms** |
> | frame ms `t=5743`, min-of-15 interleaved | 49.39 | 49.47 |
> | frame ms `t=5780`, min-of-15 interleaved | 51.84 | **50.08** |
>
> 347 of the 370 baked meshes fall under the 128 cap (mean face edge 1.303 world
> → res 19); the 23 that keep it are the big authored quads. Per-frame is
> neutral at `t=5743` (+0.08 ms, inside a several-ms run-to-run spread) and
> −1.76 ms at `t=5780`; the bake and the 5.94 GB are the certain wins.
>
> **THE LOOK MOVED NOTHING, and that is measured, not assumed.** Byte-identical
> at all 16 poses of `docs/greets_review_poses.txt` and at the pin pose — so
> **the greets pin `778fa6acd85a69cf241babefcdaf598e` did NOT move (4/4)**, city
> `3cbe42b166847e40f7071eedb48d613c` and fountain `8db68ccb59416e9a44037e9f387b7bd9`
> 4/4 each, `render_gate` 3/3. Images: `docs/img/fogwt/lmdensity_flat_*`.
> Two poses show 2–7 px at ≤15/255 — **that is the scene's own run-to-run
> nondeterminism, not the change**: same-arm reruns of `t=5773` differ by 6 px
> at max 15, i.e. more than the cross-arm diff.
>
> **WHY it is null, and the bigger finding underneath.** The shipping arm never
> SAMPLES the atlas. `DeferredSurfaceKernel.cpp:1619` gates the lightmap path on
> `lmKernelEnabled = !shadow_dynamic() || shadow_lm_dynamic()`; greets defaults
> `shadow_dynamic` ON and `shadow_lm_dynamic` is compile-default 0, so every
> pixel takes the per-pixel cube tap instead. MEASURED, not inferred:
> `--no-shadow_lightmap` renders **byte-identical** frames at `t=5743` and
> `t=6097`, and re-running the whole 16-pose battery under `--shadow_lm_dynamic`
> (atlas live) is byte-identical between the two densities as well. So greets
> spends a 1.1 s startup bake and 5.6 GB producing an array nothing reads. This
> commit makes that 54 ms and 0.09 GB; **skipping the bake outright when
> `shadow_dynamic && !shadow_lm_dynamic` is the real fix and is NOT done here**
> (FDS/RENDER, and the opposite call — defaulting `--shadow_lm_dynamic` ON — is
> a look decision for the user). Recorded in `docs/OPTIMIZATION_BACKLOG.md`.
>
> Revert: `--shadow_lightmap_texel_density=0` (verified — reproduces the pin
> 4/4). Stale comment left behind on purpose (lane discipline, another agent
> owns FDS/RENDER this session): `FDS/RENDER/LightmapBake.cpp:330-336` still
> claims the flat path never enters the density branch.

> ## 2026-08-10 — "I CAN'T SEE THE MECH IN THE UP-LOOKING BAKE": HIS OFFSET HYPOTHESIS IS RIGHT, AND IT IS 8 UNITS
>
> User: *"I can't see the mech in the up-looking dynamic bake, even when the mech
> is directly above the stairs — I think the camera is offset to one of the
> stairs' side."* **Confirmed, measured, and the offset is nearly the whole
> half-extent of the surface.**
>
> **THE NUMBERS.** `materialCentroid` (`FDS/RENDER/EnvBake.cpp`) derives a
> probe's capture point as the mean world position of **every vertex** of every
> face using the material, then — for a multi-instance surface — greedy-clusters
> at an **8-world-unit** radius and re-centroids on the **heaviest** cluster.
> greets `stairs` is **one pair of flights 9.5 u long**, and 9.5 > 8, so each
> flight splinters into a top cluster (n=22) and a bottom cluster (n=8). The
> function's own comment already concedes this — *"the greedy clustering
> splinters a single statue into several"* — but only in the self-exclusion
> logic, not in the probe placement. "Heaviest" then parks the probe on the top
> landing END:
>
> | | value |
> |---|---|
> | capture point (shipped) | **(45.4, 2.3, −54.9)** |
> | owner-faces AABB | [35.9, 0.0, −70.9] .. [49.1, 3.8, −54.8] |
> | footprint centre | (42.5, 1.9, −62.85) |
> | offset from centre | (+2.9, +0.4, **+7.95**) on a 16.2 u Z extent |
>
> The probe sits at **z = −54.9 against a −54.8 boundary** — literally on the
> z-extreme face of its own footprint. The mech ends its walk at
> **(44.4, 4.7, −62.2)**, directly over that footprint centre. From the shipped
> probe its direction is (−1.0, +2.4, −7.3) = **72° off vertical**, so it lands
> in the **−Z** cube face and +Y never sees it. From the footprint centre the
> same mech is **36° off vertical** — inside +Y.
>
> **THE DRAW SET IS NOT THE PROBLEM, and this was checked first.** Both
> mechanisms were tested. The mech IS a mover (`WorldAabb_MeshIsDynamic`), the
> store IS retained, and `[ENVDYN-WHY]` reports `'stairs' (store 1): OK —
> overlaid the mech into 3 touched face(s), **1754 mech texel(s)** composited
> over static`, every frame. The overlay is drawing the mech into this probe
> continuously; it is just drawing it into the wrong faces. `--env_bake_include_animated`
> (static-bake inclusion) is a separate mechanism and is not implicated.
>
> **THE PROOF PAIR** — the live post-overlay +Y face of the same probe, same
> pose (`--repro=greets@t=7100 --env_dynamic`), via the new `--env_dyn_dump`:
> * `docs/img/envmap/stairs_pY_before.png` — empty room, **no mech**
> * `docs/img/envmap/stairs_pY_after.png` — **the mech, dead centre**
> * `docs/img/envmap/stairs_mZ_before.png` — where it actually was: small, low,
>   near the edge of the −Z face, exactly as 72° predicts
> * whole cubes: `docs/img/envmap/stairs_atlas_before_half.png` /
>   `docs/img/envmap/stairs_atlas_after_half.png`
>
> **THE FIX — `--env_probe_center`, and it is general, not a stairs special-case.**
> Two changes inside `materialCentroid`: (1) **AREA weighting** — each face
> contributes its own centroid weighted by its world area, so the point stops
> being a function of tessellation density; (2) **INSTANCE-GROUP UNION** — the
> greedy clustering is left bit-identical (instance *detection* is untouched),
> but the heaviest cluster is then unioned transitively with every cluster
> within the **2× cluster radius the self-exclusion logic already calls
> "fragments of the probed instance"**, and the capture point is the area
> centroid of that union. The change simply makes the placement obey a rule the
> file already states. New stairs capture point: **(42.6, 0.4, −62.1)** — X and
> Z on the footprint centre.
>
> **The separation guard is exercised and it holds — measured on CITY, not
> asserted from the source comment.** greets turned out to be a bad witness for
> it: the only multi-cluster materials there are `stairs` and `stairs::mirUV`,
> and both merge 4-of-4. (The `materialCentroid` comment's example, "the two
> greets mummies share one material", does not match the scene as it stands —
> `momy-1` and `momy-2` are *separate* materials with one cluster each, so they
> never enter this path at all.) City's vehicle glass is the real test, and
> there the union correctly refuses to swallow the siblings: `cokpit` **1 of 4**
> clusters, `car 2 glass` **1 of 8**, `ambulans glass` **2 of 5**,
> `poliece  glass` **2 of 5**, `bike glass` 4 of 4. That the whole city frame
> then moves by **5 pixels** is the evidence that scattered-instance surfaces
> keep their per-instance probes.
>
> An **UP-FACING-FACES-ONLY** centroid was considered and rejected: three of
> greets' five flagged probes (`momy-1`, `momy-2`, `screen emiter`) are vertical
> reflectors with no up-facing faces at all, so the restriction is undefined
> exactly where it would have to be general.
>
> **DEFAULT OFF, AND THE FLIP WANTS HIS EYE.** Certified DIFFERENTIALLY (one
> binary, flag on vs off — the only valid method in a shared tree):
>
> | gate | flag OFF | flag ON |
> |---|---|---|
> | greets (pin recipe, `--no-env_refl`) | `778fa6ac…` ✅ unmoved | `778fa6ac…` **identical** |
> | fountain | `8db68ccb…` ✅ unmoved | `8db68ccb…` **identical** |
> | city | `3cbe42b1…` ✅ unmoved | `3c64e012…` **MOVES** — 5 px, max Δ 4/255 |
> | greets WITH env_refl (t=1588) | `e5f38b40…` | `757cae6d…` **MOVES** — 343 157 px (16.5 %), max Δ 102, but mean Δ-sum 3.3/765 and only 3 715 px > 10 luma |
>
> All four stable 2/2. The recorded greets pin recipe carries `--no-env_refl`,
> so it is blind to this by construction — the `greets WITH env_refl` row is the
> honest measurement and is why the flag ships OFF. Look pairs for his eye:
> `docs/img/envmap/greets_stairs_view_pair.png` (a camera on the stairs with the
> mech above them — the clearest one) and
> `docs/img/envmap/greets_t1588_probecentre_pair.png` (the pin pose).
>
> **BAKE COST: NO INCREASE, MEASURED.** greets bakes **one fewer probe** with the
> flag on (7 → 6): the new `stairs` and `stairs::mirUV` capture points land 2.2 u
> apart and fall inside the existing 4-unit store-sharing radius, so the two
> collapse onto one store — one 512² cube bake saved. Min-of-5 wall on the greets
> snapshot 1 813 ms OFF vs **1 792 ms** ON (load 8.9–13.4; the −21 ms is inside
> the noise of a 1.8 s run, so the claim is *no measured increase*, not a win).
> The derivation itself adds one cross product + sqrt per face *that uses the
> material*, inside a mesh walk that already happens.
>
> **THE AUTHORED OVERRIDE — `Material::EnvBakeOfs`, editor "probe offset X/Y/Z".**
> A derivation over a surface's own geometry cannot know that a probe wants to
> sit clear of a step nose or below a soffit, so the automated point is not the
> last word. Three floats, world units, **added on top of whichever derivation
> ran** (verified live: `'stairs': authored probe offset (+0.00 +3.00 +0.00) —
> capture point (42.6 0.4 −62.1) -> (42.6 3.4 −62.1)`). All zero = unset =
> byte-null. Live-applies — the edit drops just that store
> (`EnvReflection_InvalidateSurface`) so the probe re-bakes from the new point on
> the next frame and can be dialled in by eye.
>
> Persistence follows the §1a extension idiom: LWO **`RVSF` sub-chunk bit
> `0x1000`**, carrying **three floats under ONE bit** (X, Y, Z). **Proven end to
> end, not asserted:** `lwopatch` wrote `envBakeOfs = (0, 2.5, 0)` onto `stairs`
> in a scratch copy of `Authoring/greets/Piramid.lwo`, `lwsread` regenerated the
> FLD **+12 bytes exactly** (233 621 → 233 633), and the engine — run against a
> scratch asset root via `--no-chdir_assets`, so nothing under `Runtime/` or
> `Authoring/` was touched — reported `'stairs': authored probe offset (+0.00
> +2.50 +0.00) — capture point (45.4 2.3 −54.9) -> (45.4 4.8 −54.9)`, and the
> `::mirUV` clone inherited it. Inertness is proven too: with nothing authored,
> the greets regen is byte-identical at the golden `62c68fc9…`, and 300 random
> writer subsets over the 12 legacy RVSF keys reproduce the pre-change bytes
> exactly. That deviates
> from the one-bit-per-scalar convention `tintR/G/B` follows, deliberately: it is
> one semantic vector, and three bits would have left the u16 with a single free
> bit. **0x2000/0x4000/0x8000 remain free.** It is a SURFACE property, not an
> object one, because a probe's identity in `EnvBake` *is* its material
> (`env.byMat`, one store per material-centroid group) — a per-object value would
> have had no probe to attach to, and §1d's `Object_FdsExt` path is unimplemented.
>
> **NEW INSTRUMENT: `--env_dyn_dump=N`** (1-based store index, the `--env_map_probe`
> numbering) writes the **live, post-overlay** mip-0 cube of probe N to
> `/tmp/envdyn_<material>.ppm` as the standard 3×2 atlas. `FDS_ENVBAKE_DUMP` can
> only show the STATIC capture, which by construction contains no mover — so it
> could not have answered this question. Default 0, byte-null.
>
> **PROCESS NOTE, for the record:** the two `FeatureFlags.def` entries for this
> work were swept into commit `5079f6e` (`--shadow_plane_pack`) by a concurrent
> agent holding the shared tree. The content is correct and in HEAD; the
> attribution is wrong. Same hazard class as the 2026-08-09 note below.

> ## 2026-08-09 — HIS 12-14 FPS, EXPLAINED: THE SCENE RENDERS DEFERRED BUT WAS BUILT AS IF IT WOULD NOT
>
> User, interactive, `./DEMO --greets-displace`, window 1512×848, facing the
> mirror wall (`FDS_GREETS_CAM="-8.6249094,2.72651696,-53.2339516,0.210607708,
> 0.0055912463,-0.977554619"`, t=3122): **12-14 fps**, remembered "a lot better"
> (20-30). The bench said 54.7 ms at 1920×1080, which at 0.63× the pixels should
> be comfortably above 18 fps. The gap is real and it is **22.7 ms**, all of it
> from ONE root cause with two heads.
>
> **THE RENDER PATH IS NOT `FeatureFlags::deferred()`.** `RENDER.CPP:356`
> `deferredEnabled()` ORs five flags — `deferred || hdr || deferred_quarter ||
> deferred_checkerboard || shard_deferred` — and greets sets three of them, then
> additionally forces `Render(RenderPath::ForceDeferred)` whenever
> `greets_mirror` is on (`GREETS.CPP:3943`, and `greets_mirror` is
> `setDefault(true)` at `:1088`). So a plain `./DEMO --greets-displace` **renders
> a deferred frame**. But two SCENE-BUILD/TICK decisions ask the bare flag, which
> is still 0, and they get the opposite answer:
>
> | reader | what it does when it wrongly thinks "forward" | measured cost |
> |---|---|--:|
> | `GREETS.CPP:2414` | skips the Piramid chunk split entirely → the 59 556 displaced faces the chunk pass marks `NoShadowCast` stay casters, and the wall stays one room-sized mesh | **15.10 ms** (BAKE 3.9 → 16.0) |
> | `GREETS.CPP:3849` | runs the forward vertex `Lighting(GreetSc)` pass every frame, whose only consumer is the mirror-RTT offscreen pass | **6.49 ms** (LGHT 0.00 → 6.49) |
>
> **MEASURED**, `--bench=scene@scene=greets,t=3122,iters=20,xres=1512,yres=848`,
> min-of-6, load 9-15:
>
> | arm | fmin | BAKE | LGHT |
> |---|--:|--:|--:|
> | A — his line, `--greets_displace` | **68.49** | 16.02 | 6.49 |
> | B — A + `--deferred` | **44.69** | 3.88 | 0.00 |
> | C — B + `--greets_piramid_chunk_grid=0` | 59.79 | 16.27 | — |
> | flat, no `--deferred` | 45.31 | 3.71 | — |
>
> C isolates it: chunk split = B−C = **15.10 ms**, the rest = A−C = 8.70 ms, of
> which `Lighting()` is 6.49. The flat row is the tell — **the penalty needs the
> displaced geometry**; flat without `--deferred` costs the same as with it.
>
> **THE ARITHMETIC TO 12-14 FPS.** `DEMO/SDL2.cpp:622-626` creates the renderer
> with `SDL_RENDERER_PRESENTVSYNC` unless `--no_vsync` (default 0), so present
> quantises to the refresh. At 60 Hz:
>
> | render | intervals | presented | fps |
> |---|--:|--:|--:|
> | his line, 68.5 ms | 5 | 83.3 ms | **12.0** |
> | fixed, 44.7 ms | 3 | 50.0 ms | **20.0** |
>
> 12.0 is exactly what he reports; 20.0 is exactly the bottom of what he
> remembers. The quantisation is why it reads as a cliff rather than a slope —
> 68.5 and 44.7 straddle two whole steps. *Code-verified + arithmetic; the 60 Hz
> refresh is assumed, not measured, and I never opened a window.*
>
> **FIXED HERE (FDS half):** `fds::DeferredPathEnabled()`, declared in
> `RENDER/ChunkOcclusion.h`, defined in `RENDER.CPP` next to `deferredEnabled()`
> — one predicate, one definition, callable at init. Purely additive; nothing
> calls it yet, and all three pins + render_gate are unmoved.
>
> **HANDOFF (DEMO/GREETS.CPP is another agent's lane right now):**
> 1. `:2414` — `&& fds::FeatureFlags::deferred()` → `&& fds::DeferredPathEnabled()`
> 2. `:3849` — `!fds::FeatureFlags::deferred() ||` → `!fds::DeferredPathEnabled() ||`
> 3. move `setDefault(mirror_rtt, true)` and `setDefault(mirror_rtt_density, 1024.0f)`
>    out of `GreetsApplyRunDefaults` into `GreetsApplyInitDefaults`
>    (docs/SETDEFAULT_AUDIT.md §4.1/§4.3, recommended there and still unfixed).
>
> **DO NOT "fix" this with `setDefault(deferred, true)`.**
> `GreetsApplyInitDefaults` runs FIRST in the t1 init chain, so that would force
> city/chase/fountain/crash onto the deferred path — the exact `shard_deferred`
> leak recorded as §5 L1 in the audit.
>
> **The fix is a LOOK change, and needs his eye + a re-pin decision:** at his
> pose, A vs B differs by **557 589 px (26.9 %)**, mean Δ-sum 18.79/765 — a broad
> low-amplitude shading shift on the ceiling and right-hand wall (the chunk split
> moves per-chunk culling and lighting). Nothing is missing or broken in either.
> Strip: `docs/img/fogwt/deferred_flag_look_t3122.png`.
>
> **HYPOTHESES THAT DIED ON MEASUREMENT, with numbers:**
> * *Pose-dependent mirror/RTT cost.* No. The RTT bake DOES apply the flat-proxy
>   substitution — it takes `OffscreenViewScope` (`GreetsMirror.cpp:3067`) →
>   `g_offscreenViewDepth` → `_offscreenPass` (`Transform.cpp:1180`) →
>   `Face_MainOnly` skipped at `:2429`, proxy admitted at `:1432`. With all 7
>   slots live the displaced-vs-flat delta is **+3.05 ms**. Building the slots at
>   all costs +3.67 ms (tess) / +2.11 ms (flat).
> * *Tessellation is expensive at this pose.* The opposite: at t=3122, 1920×1080,
>   flat and tess are **70.93 vs 70.93 ms** — identical, min-of-6.
> * *Hyphen spelling.* `FeatureFlags.cpp:276-284` normalises dash→underscore
>   after the leading `--`. `--greets-displace --mirror-rtt --strict_flags` runs
>   with 0 unknown flags and the `[STONE]` line fires. Nothing to fix.
> * *Resolution scaling anomaly.* None: 1920×1080 → 1512×848 is 70.93 → 45.80
>   (0.646×) against a pixel ratio of 0.63. Pixel-bound, as expected.
>
> **E, the user's counterexample, upheld:** `--mirror-rtt` changes **9 471 px
> (0.457 %), max Δ 175/255** at his pose, because on the default path the RTT
> slots are *never built* — `mirror_rtt`'s setDefault lands in the RUN block,
> after `Initialize_Greets` has already decided (`GreetsMirror.cpp:1401`). A
> default run logs `0 first-order RTT` and zero `[MIRROR-RTT] slot` lines;
> `--mirror_rtt` logs seven. The "0 px on the authored path" generalization is
> retired in `docs/SETDEFAULT_AUDIT.md`.

> ## 2026-08-09 — THE TWO REPORTED `--greets_displace` REGRESSIONS: NEITHER IS ONE, AND THE REAL COST IS 19.4 GB
>
> User: *"tessellation is costing us now half the fps"* and *"tessellation bake
> seems to hang the starting scenes for quite a lot of time — this should be
> done concurrently — what changed?"*, with *"did we change some
> tessellation/vis params?"*.
>
> **PARAMS: NOTHING CHANGED. Not one.** Every flag in the displace family has
> the identical compile-time default at `HEAD` and at `1a91ed5` — `greets_displace`,
> `_amp` 0.3, `_mip` 2, `_adapt` 1.0, `_cpb` 1.0, `_edge`, `_seam_union`,
> `_fold_relax`, `_shadow_planes`, `_line_height`, `_smooth` 80, `_neighbor_pin`,
> `greets_stone_subdiv` 0, `greets_shadow_proxy`, `greets_displace_flat_mirror`,
> `displace_viz`, `chunk_occl_res`, `tile_bbox_cull`. Of the 40 `setDefault` calls
> in `GREETS.CPP`, exactly ONE moved: `greets_omni_default_range` 30.0 was DELETED
> (`00f7820`, ranges now authored per light in the LWS) — and it acts on both arms
> equally. `DisplaceTest.cpp`'s setDefaults are identical. `DisplaceStoneSubdiv`
> itself (`MeshOps.cpp:1970`) is **untouched**: every one of the +1082 lines in
> that file since `1a91ed5` is above line 4154, i.e. `PomShell_*` / prism, and
> `--pom_shell` is still default 0 (no shell/prism log fires in a displace run).
> `pom_shell_weld` 0→1 is real but inert here for the same reason.
>
> **SYMPTOM 1 (per-frame) DOES NOT REPRODUCE.** `--bench=scene@scene=greets,
> t=5743,iters=20`, 5 arms interleaved, min-of-6, load 8.8–13.1:
>
> | arm | fmin min | Δ flat, same tree |
> |---|--:|--:|
> | `1a91ed5` flat | 50.61 | — |
> | `1a91ed5` tess | 57.15 | **+6.54** |
> | HEAD flat | 53.62 | — |
> | HEAD tess (pre-fix) | 55.55 | **+1.93** |
> | HEAD tess (post-fix) | 54.72 | +1.10 |
>
> The tessellation delta did not grow, it **shrank**. What grew is the BASE cost
> of *both* arms: HEAD's flat arm is +3.0 ms over `1a91ed5`'s, which is the nine
> flags defaulted ON in `1782351` + `bd6e806` — they cost on every path and
> therefore cannot move a tess-vs-flat delta. A second batch at load 20–54 put the
> delta at +5.47 (HEAD) vs +5.44 (`1a91ed5`) — again equal. A whole-timeline sweep
> (`t=200..7000`, 137 frames, min-of-4) gives HEAD +3.9 ms on a 63.4 ms mean.
> **The delta measures 2–13 % depending on batch and load. Never 2×.**
>
> **The prime suspect died on measurement.** `704a5a8` does NOT touch
> `GreetsMirror.cpp`, and the flat-mirror clone is intact at HEAD: the displace
> run clones **9 198 / 9 166 faces** per mirror, exactly the documented figure, not
> the 42 870 of the pre-companion arm. The shatter scoping still reads
> 450 / 450 / 2 886 (flat / displace / displace with the scope off), reproducing
> `704a5a8`'s published table byte for byte.
>
> **SYMPTOM 2: THE BAKE IS ALREADY CONCURRENT, AND IT DID NOT GET SLOWER.** New
> `--init_timeline` (default OFF, byte-null) stamps every init milestone. Full
> demo path, dummy drivers:
>
> | mark | flat | `--greets_displace` |
> |---|--:|--:|
> | `Initialize_Greets` | 1 379 ms | 2 469 ms |
> | ├ `DisplaceStoneSubdiv` block | 0 ms | 573 ms |
> | t1 chain done (all five scenes) | 4 672 ms | 5 116 ms |
> | `Run_Glato` ends | 41 878 ms | 43 380 ms |
> | **`t1.join()` returns** | **+0.1 ms** | **+0.0 ms** |
> | City starts | 47 451 ms | 49 298 ms |
>
> The join is instantaneous in both arms — the 42 s intro absorbs the whole init.
> There is no stall on the demo path. What DOES block is the greets-ENTRY path
> (`--scene-greets`, `--snapshot=greets`, `--bench=scene@scene=greets`): those join
> `Greets_JoinBakeThread` immediately after init with nothing in between, so the
> lightmap bake is 100 % blocking wait — and `join_wait_ms == bake_ms` to the
> millisecond, measured. **That was equally true at `1a91ed5`:** bake 10 684 ms
> there vs 10 895 ms at HEAD (flat 1 341 vs 1 365). Nothing regressed.
>
> **WHAT IS ACTUALLY WRONG, and it is big.** `StaticShadowLightmap::data` is
> `numFaces * lmRes² * numOmnis` BYTES and `allocate()` fills it with 255, so every
> byte is touched and resident. greets sets `shadow_lightmap_res = 128`. That is
> calibrated for the authored wall quads and scales with **face count**, so
> tessellation multiplies it directly. `/usr/bin/time -l`, greets t=5743, 64 GB box:
>
> | arm | baked faces | atlas store | peak footprint | max RSS | bake |
> |---|--:|--:|--:|--:|--:|
> | flat | 33 396 | 5.61 GB | 6.93 GB | 7.44 GB | 1.08 s |
> | `--greets_displace` before | 115 346 | **19.36 GB** | **22.97 GB** | 14.05 GB | 6.2–11.7 s |
> | `--greets_displace` after | 115 346 | **0.14 GB** | **2.35 GB** | 2.36 GB | **0.09 s** |
>
> Max RSS *below* peak footprint is the OS already compressing it. A displaced
> cell is ~1/300 the AREA of the quad it replaces, so each was carrying ~300× the
> shadow texels per world unit that the FLAT wall ships with.
>
> **FIXED behind `--shadow_lightmap_texel_density`** (default 0 = OFF = byte-null;
> `--greets_displace` defaults it to 14.2 texels/world-unit as its **third** perf
> companion, named in the `[STONE]` line). Per-mesh
> `res = clamp(ceil(sqrt(meanFaceArea) * density), 8, shadow_lightmap_res)` —
> capped, so it can only reduce; the runtime sampler was already per-mesh
> (`StaticShadowLightmap::lmRes` is a member). **Look cost in the displaced arm:
> byte-identical at t=1588 / 2845 / 4871 / 6097 and 3 px at 1 LSB at t=5743.** The
> 19.2 GB was buying nothing. Per-frame effect at t=5743 is within noise (55.07 →
> 54.82 min-of-6); the certain wins are the bake (114×) and the memory.
>
> **PINS UNMOVED, all three, on this build:** greets `778fa6acd85a69cf241babefcdaf598e`
> (4/4), fountain `8db68ccb59416e9a44037e9f387b7bd9` (3/3), city
> `3cbe42b166847e40f7071eedb48d613c` (3/3). The flat path never enters the branch.
>
> **INFERENCE, stated as inference:** the user's "half the fps" is most consistent
> with the 23 GB footprint meeting a machine that also has other agents on it —
> the arm's cost becomes a function of memory pressure, which is exactly why it
> measured +5.5 ms at load 20–54 and +1.9 ms at load 9–13 in the same session. Not
> proven; the direct A/B on a memory-pressured box was not run.
>
> Evidence: `docs/img/fogwt/lm_atlas_density.png`,
> `docs/img/fogwt/shatter_wall_recheck_t6133.png`,
> `docs/img/fogwt/shatter_matscope_diff_t6133.png`.

> ## 2026-08-09 — THE CHECKERBOARD LATTICE IS A SECOND BRDF, NOT A RECONSTRUCTION BLUR
>
> The user pushed back on "half-rate shading is a third of the CPU's canopy
> detail" — *"this still doesn't make complete sense … could be an issue in the
> checkerboard path?"* He was right. It is a **defect**, and it is not in the
> reconstruction filter at all.
>
> **Mechanism, read from source.** The wave-2 fill refuses to AVERAGE an
> env-reflective pixel (`envForceFull`, `DeferredSurfaceKernel.cpp:5003` — both
> averaging models break on reflections) and instead re-shades it with the
> scalar fallback at `:5254`. **That fallback is a REDUCED kernel.** Against the
> wave-1 scalar kernel it is missing: the `--pbr` Cook-Torrance GGX lobe (it
> runs Blinn-Phong `std::pow(NdotH, gloss)` at `:5420`), **every** shadow term
> (`computeMapShadowAtten`, `resolveCubeAtten`, the static lightmap, the PolyId
> compare, the bias pair), the AO map, the normal-map LOD fade, and
> `--hdr_metal_kill`; and it applies the spot-cone penumbra to SPECULAR where
> wave 1 does not. greets sets `--pbr` and `--shadows` ON. So alternate pixels
> of every reflective surface are shaded **by two different BRDFs**, and the
> phase is `(px ^ py) & 1` with **no frame term** — a fixed lattice that never
> averages out under motion.
>
> **MEASURED** on greets t=4871 at the user's mech pose, over the 33 478-px
> canopy mask, as *mean luma of the wave-2 cells minus the wave-1 cells* (0 if
> the reconstruction were unbiased):
>
> | arm | ODD−EVEN luma |
> |---|--:|
> | shipped | **+6.82** |
> | `--no-shadows` | +5.51 |
> | `--no-pbr` | **+0.90** |
> | `--no-pbr --no-shadows` | **−0.01** |
> | `--deferred_checkerboard=0` (full rate) | +0.04 |
> | standalone Metal arm | −0.05 |
>
> `--pbr` owns ~5.9 luma of it and the shadow terms ~0.9–1.3; with both taken
> out of wave 1 the two kernels agree to a hundredth of a luma. Whole-frame bias
> is only +0.19, because the fallback only fires on reflective materials.
>
> **FIXED behind `--deferred_checker_env_full`** (default OFF, byte-null,
> verified: greets pin and the t=4871 frame both unchanged). It shades
> env-reflective pixels at FULL rate in wave 1 instead of letting the reduced
> fallback do it. Bias +6.82 → **+0.02**; against the full-rate render the
> canopy now agrees to mean |ΔY| **0.99** (was 4.48) with 174 px > 10 luma (was
> 4 349). **Cost: none.** The fill was already full-shading exactly this set, so
> `lighting-w2` FALLS 3.51 → 3.14 ms (3/3 reps) while `lighting-w1` moves within
> noise; `renderFrame` min-of-mins 53.11 vs 53.03 ms. For scale, the "just turn
> the checkerboard off" alternative is **53.1 → 79.3 ms**.
>
> Crop (A shipped / B fixed / C full-rate / D GPU): `/tmp/fogwt/task3_canopy_lattice.png`.
>
> **STILL OPEN, not mine this run:** the same reduced fallback also fires at
> every material/normal/Z EDGE (the `neighborCompatible` miss), where shadows
> matter most. That is a broader instance of the same defect and is unpriced.

> ## 2026-08-09 — E6 / E7 now have CPU-side flags, and E7 is much smaller than §11 implied
>
> `--env_bake_include_animated` (E6) and `--env_mip_chain` (E7), both default
> OFF / byte-null. Full rationale + numbers in `FeatureFlags.def`.
>
> **TRAP RECORDED:** `g_envBakeSkipDynamic` is NOT "skip animated meshes". It is
> read in THREE places in `Transform.cpp` — the animated-mesh skip (`:1274`),
> the legacy whole-mesh exclusion (`:1549`) and **the reflector's own-FACE skip**
> (`:2396`). The first cut of E6 cleared the global and thereby let the cockpit's
> own canopy glass into its own probe: the +Y face went **91 % VOID** and the
> probe mean **100.31 → 49.11**. The shipped flag hooks `:1274` and only that.
> With it scoped correctly: probe mean 100.31 → **89.14**, all faces 100 %
> nonvoid, −Y (toward the mech's own body) 96.22 → 74.55; canopy **2 817 px**
> changed, mean |ΔY| 22.86 on changed, max 102.4; frame-wide 39 473 px (1.90 %).
> The GPU's mirror-image `--env_bake_skip_animated` moves 5 268 px / mean 24.94.
>
> **E7 IS SMALL ON THE CPU, and this corrects the emphasis in §11.** The flag
> works and has full range — `--env_mip_chain=16` drives the select to the
> bottom of the store's chain (32² face) — but a WITHIN-ARM sweep of the isolated
> env term (render minus `--no-env_refl`, 7×7 high-pass RMS on the canopy) moves
> only **24.68 → 24.32 (chain 9) → 24.05 (chain 8 + `--env_bake_res=128`, the
> exact GPU emulation) → 23.92 (chain 16)**. That is **3 %** across the whole
> dial, against the GPU's own `--env_res` sweep spanning 16.22 → 17.95 (11 %).
> Conclusion: on the CPU the canopy's high-frequency energy is **not** reflected
> detail — it is Fresnel/normal modulation of an already-smooth reflection plus
> the frame ribs and the glass. Matching the lobe width will not make the CPU
> canopy look like the GPU's; what is left is the env term's BRIGHTNESS (CPU
> +131.0 vs GPU +107.6 over the mask, i.e. E0) and probe content.

> ## 2026-08-08 — POM CAMPAIGN RE-BASELINED AFTER THE MIP FLIP (`docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-8)
>
> Every S1d number was measured with `--mips` OFF. Re-measured as OFF/ON **pairs**
> on a private worktree build (so concurrent agents cannot contaminate a figure);
> the arm reproduces its published 10 void / 73 black **to the per-pose digit**,
> and the slip ladder and the silhouette table reproduce **exactly**.
>
> - **Void is mip-INVARIANT** — 10 at both settings, same three poses. Black falls
>   ~25 % (73→52) and is a sampling artefact; do not quote a pre-flip black figure.
> - **The grazing smear does not move** — slip p90 identical to 3 decimals at every
>   cap, and `--texture_filter=2` cannot move it because `slip` is a MARCHED-UV
>   metric and the filter runs downstream of the march. The campaign needs a
>   filter-sensitive motion metric before spending more on the smear.
> - **The silhouette table is byte-for-byte unchanged.** §S1d-6 stands in full.
> - **PERF, two corrections:** `--mips` ON is **not** neutral for the parallax arms
>   (−0.6/−0.7 ms on +POM / +tess / the shell arm, −0.1 on flat), and the
>   **"tessellation and the POM arm cost the same" result does NOT reproduce** —
>   the shell arm is **+1.0 to +1.6 ms MORE expensive** at matched amplitude, at
>   both mip settings.
> - **The quad-diagonal crease is ROOT-CAUSED AND FIXED.** The lid quad really is
>   non-planar (`rooms`: 133 pairs, lid-normal angle max 3.07°, plane gap 0.0878
>   world vs a 0.0900 offset; `floor`, which has no corner verts, measures 0.0000°).
>   **`--pom_shell_lid_planar`** (new, default 0, byte-null) removes the crease at
>   zero measured cost on void/black, the silhouette and perf. It does **not** fix
>   the silhouette — two defects, two fixes.
> - ~~**NOT MINE, FLAGGED: the city pin does not reproduce, stably (2/2)**~~ —
>   **RESOLVED 2026-08-08, and it was never a code drift.** See
>   "the city pin is a function of `cache/city_envmap_cube.bin`" below. Short
>   version: HEAD reproduces **both** recorded pins byte-exactly
>   (`e1221676` default, `37e62845` under the control) when the env cube on disk
>   is the pre-flip bake. The `5476be8c` / `b88ecb7b` pair came from a **fresh
>   worktree with a cold cache**, which re-bakes the cube under the new
>   `--mips` / `--mip_fix` defaults. No unowned commit; nothing to bisect.

> ## 2026-08-08 — THE CITY PIN IS A FUNCTION OF `cache/city_envmap_cube.bin` (the "unowned drift", resolved)
>
> **There is no unowned commit. There was nothing to bisect.** The reported city
> drift is a **stale-cache artifact**, and the reasoning that exonerated the mip
> flip ("both halves moved, so it cannot be the flip") was wrong for a specific,
> reproducible reason recorded below.
>
> **Root cause.** `ComputeCityPanoramaCacheKey` (`DEMO/CityPanoramaCache.cpp:49`)
> keys the 426 MiB cube cache on **CITY.FLD's bytes + the four dims + the format
> salt + the building names** — and on **nothing else**. The bake itself
> (`bakeBuildingCubeFaces`) runs the ordinary software rasterizer, so its output
> depends on the whole shading path *and on FeatureFlags*. **`--mips` and
> `--mip_fix` change the baked cube**, and the key cannot see them. The filename
> is fixed (`cache/city_envmap_cube.bin`), so a differing bake **overwrites** the
> old one rather than landing beside it.
>
> **Measured, the full 2×2** (HEAD `787361a`, clean worktree, dummy drivers). Rows
> = which cube is on disk, columns = the flags the *frame* renders under:
>
> | cube on disk | frame `--no-mips --no-mip_fix` | frame default (mips ON) |
> |---|---|---|
> | **pre-flip bake** (`d1d67f0f…`, what the user's `Runtime/cache/` holds, dated Aug 6 03:40) | `37e62845` ✅ **the recorded prior pin** | `e1221676` ✅ **the recorded current pin** |
> | **cold/current bake** (`63978a18…`, mips ON) | `b88ecb7b` ← the "control failure" | `5476be8c` ← the "drift" |
>
> Every cell is 2/2 stable. **HEAD is byte-faithful to both published pins**; the
> two anomalous hashes are simply the bottom row.
>
> **Why the control could not exonerate the flip.** `--no-mips --no-mip_fix`
> only changes the *frame*. It cannot un-bake a cube that is already on disk,
> because the key ignores flags — so in a fresh worktree the control arm hits the
> mips-ON cube the *preceding default run just wrote* and measures the hybrid
> cell (mips-ON bake + mips-OFF frame), which matches neither pin. That hybrid is
> exactly `b88ecb7b`. **A `--no-mips` control arm on city is only valid against a
> cube baked with mips off** — delete the cube first, or the arm is meaningless.
>
> **Proof the bake, and only the bake, moved:** cold-baking at HEAD with
> `--no-mips --no-mip_fix` reproduces the user's Aug-6 cube **byte-for-byte**
> (`d1d67f0f84fb4af3713e15a64a1b827b`, all 446 694 000 bytes). So across every
> commit from Aug 6 03:40 to HEAD, **no change altered the city env bake** other
> than the mip defaults. Both flags contribute (`--no-mips` alone → `1775b64c…`,
> `--no-mip_fix` alone → `88fec906…`; neither alone is either reference), which
> matches the known split: `--mips` zeroes the LEVEL, `--mip_fix` moves the
> subdivision cut lines.
>
> **Verdict: the new bake is CORRECT, not a regression** — it is the direct,
> intended consequence of the user's own `--mips` default flip finally reaching
> the env-cube bake, which the stale cache had been masking. It is also tiny:
> pinned vs cold-bake frame is 164 536 px changed (7.94 %) but **max channel Δ
> 6/255**, mean Δ-sum 1.63/765 — and the delta is confined to the **glass panes**
> (zero on the adjacent non-reflective wall), which is the expected signature
> since the cube feeds only the env-specular compose. Before/after/|Δ|×32 crop:
> `docs/img/mipsel/city_t1961_envbake_crop.png`. **Not re-pinned yet — the look
> change wants the user's eye first** (see the pin-table row).
>
> **TWO LIVE HAZARDS, both unowned:**
> 1. **The user's `Runtime/` is serving a pre-flip env cube.** His demo renders
>    city reflections baked under the *old* mip defaults, and will keep doing so
>    forever — the key will never invalidate on its own. To adopt the flip
>    properly: `rm Runtime/cache/city_envmap_cube.bin` and re-run.
> 2. **Any run in `Runtime/` by a binary whose bake differs silently overwrites
>    that cube**, permanently moving the main-tree city pin with no commit and no
>    trace. This is a live footgun for every agent.
>
> **The fix** (not applied — `DEMO/CITY.CPP` / `DEMO/CityPanoramaCache.cpp` were
> not mine to change this run): fold the bake-affecting FeatureFlags into the
> key, e.g. mix `mips`/`mip_fix` (and any future bake-affecting flag) into
> `cubeSalt` at the `ComputeCityPanoramaCacheKey` call site in
> `DEMO/CITY.CPP:2581`, **and** put the key in the *filename* the way
> `pom_cone_exact_%016llx.bin` / `pom_horizon_%016llx.bin` already do
> (`DEMO/MeshOps.cpp:775,959`) so variants coexist instead of clobbering. Note
> those two POM caches do **not** have this hole — `ConeExactCacheKey` hashes the
> actual input texels plus every parameter, so it is a real content key.
>
> **Stale analysis this corrects:** the `--mips` re-pin's recorded divergence for
> city ("133 854 px, mean |d| 7.04, max 192, building facades") measured only the
> **frame** half of the flip, because the bake half was masked by the cache. The
> bake half is the additional, much subtler 164 536 px / max 6 above.

> ## 2026-08-08 — MIP SELECTION IS ON BY DEFAULT; ALL SCENE PINS MOVED
>
> **`--mips` default 0 → 1** (user decision). Mip selection had been force-disabled
> since the legacy `NO_MIPMAPS` define: `MiplevelClipper` computed a level and then
> every exit path threw it away. That pinned LEVEL 0 for the albedo **and for the
> normal / roughness / metal / AO chains**, which the deferred kernel indexes by the
> same miplevel — five map sets whose levels 1..N were built, paid for in memory, and
> never read. The flag's old justification ("1998 textures are magnified so mips
> barely engage") argued from NEAR surfaces to justify disabling selection on DISTANT
> ones, and predated the sidecar PBR sets; it is retracted.
>
> **Measured** (greets t=2993, 1080p, `--deferred --texture_filter=1`, new `--mip_stats`
> histogram): OFF = 100 % of draws and 100 % of covered area at level 0. ON = 7.6 % of
> draws / 83.3 % of AREA at level 0, remainder across levels 1-8, **48.8 % of DRAWS at
> level 6**. Branches: 56 115 faces entered, 55 679 face-uniform, **436 (0.78 %) took the
> subdivision path** — rare, but it owns the large near faces.
>
> **Perf is NEUTRAL**: min-of-arm over 5 interleaved 20-iter rounds, greets t=2993 RNDR
> 39.855 ms off vs 39.965 ms on. No measurable texture-cache win, no measurable cost.
> The machine was loaded by concurrent agents (individual rounds 39.8-91.4 ms), so only
> the min is meaningful and nothing under ~0.2 ms is resolvable here.
>
> **ALL SCENE PINS MOVED** (city, fountain, greets, chase ×2) and are re-derived in the
> table below, each with a `--no-mips --no-mip_fix` control proving the move is the
> flip's and not some other drift. `tools/render_gate.sh` baselines did NOT move.
> **Two pre-existing drifts surfaced and are NOT mine: chase t1600 (both default and
> cinematic arms) no longer matches its 2026-07-30 pin even with mips off.**
>
> **`--mip_fix` default 0 → 1** — the split branch's depth ramp coefficient (K=1, not 2;
> texel area per pixel goes as z², independently re-derived). Its earlier "MEASURABLY
> BROKEN" verdict was a **zsh word-splitting artifact** (a `'--mips --mip_fix'` shell
> variable arrived as one argv token and was silently ignored); `--strict_flags` now
> makes that class of error fatal. **Correction: `--mip_fix` is NOT inert when `--mips`
> is off** — the mips gate zeroes the mip LEVEL but not the SUBDIVISION, and this flag
> moves the cut lines, so it changes geometry either way.
>
> **D3 (SHADING_CONTRACT) — the normal-map LOD fade after the flip: MEASURED, NO
> ACTION NEEDED.** The concern was that 48.8 % of draws sit at mip 6, so the flip
> pushes half the frame into the faded/flattened regime in one step. **That is true in
> DRAW count and false in SCREEN AREA — which is the number that matters, and the two
> differ by ~70x here.** Fade is `1-(mip-start+1)*step` with start=2, step=0.33, so
> full bump at mip 0-1, 0.67/0.34/0.01 at mip 2/3/4, fully FLAT from mip 5 up.
> Area-weighted, at six poses (`--mip_stats`):
>
> | pose | full bump | partial | FLAT | bump retained |
> |---|---|---|---|---|
> | greets t=2993 | 88.2 % | 9.1 % | **2.7 %** | 91.4 % |
> | greets t=4200 vista | 85.8 % | 11.0 % | **3.2 %** | 89.7 % |
> | greets t=5958 grazing | 85.9 % | 10.7 % | **3.4 %** | 89.3 % |
> | greets t=5743 review | 85.6 % | 11.2 % | **3.2 %** | 89.6 % |
> | city t=1961 (gate) | 80.3 % | 18.9 % | **0.8 %** | 90.2 % |
> | fountain t=2500 (gate) | 89.7 % | 10.3 % | **0.0 %** | 95.1 % |
>
> The 48.8 % of draws at mip 6 cover **0.7 % of screen area**. Direct check — disabling
> the fade ENTIRELY (`--nmap_lod_fade_start=16`): greets t=4200 changes **0.35 % of
> pixels** (77 px >12/255), city t=1961 changes **ZERO pixels**. Worst-region crop
> `docs/img/mipsel/t4200_nmap_fade_on_vs_off.png` is visually indistinguishable
> (mean \|d\| 0.16). **No "wall goes geometrically flat at distance" is occurring at a
> visible scale, so the threshold does NOT need retuning.**
>
> **Fade vs Toksvig/LEAN — settled by that same measurement: implement NEITHER.** The
> fade is a crude stand-in for proper normal-map mip filtering, and Toksvig would be a
> refinement of it. But the fade's total footprint post-flip is ≤0.35 % of pixels and
> 0 % at the city gate, so roughness coupling would be buying a correction to a term
> that barely fires. Revisit only if content changes push real area past mip 4.
>
> **GPU-PARITY WARNING: the GPU arm has NO normal-map fade at all.** After this flip
> the CPU flattens bump on ~3 % of greets' screen area that the GPU still perturbs, so
> CPU-vs-GPU pairs at distant surfaces now diverge BY CONSTRUCTION. Neither renderer is
> wrong. Do not chase it as a GPU bug.
>
> **Unrelated pre-existing hazard (D6), flagged so it is not misattributed to mips:**
> the CPU's AO is unclamped and can go negative at `ao_strength=2.0`, subtracting
> direct light. If a new artifact appears near AO'd geometry after the flip, check that
> first — the flip changes which AO texels are sampled but did not create the bug.
>
> **Two corrections to my own earlier claims, both measured:**
> 1. `--mip_fix` is **not** inert with `--mips` off (above). The mips gate zeroes the
>    mip LEVEL, not the SUBDIVISION.
> 2. The first cut's lazy-`BaseLod` refactor was **not byte-null**: `_C` was
>    `0.5 * fastLog2(...)` in **double** and the lambda made it float. `_C` positions
>    the subdivision cut lines, so re-associating that arithmetic moves geometry even
>    when the level is forced to 0. Fixed by restoring the legacy expression verbatim
>    on the non-aniso path — **the `0.5 *` there must stay double.**
>
> **Trilinear**: `--texture_filter=2` stops silently degrading to bilinear now that
> `mipFrac` is no longer force-zeroed — 53 888 px (2.60 %) differ at greets t=4200.
> **`mip_bias` 0.5 + truncation = round-to-nearest**, which is correct for point and
> bilinear but WRONG for trilinear: it offsets the inter-level blend by half a level.
> `--mip_bias=0` is the correct pairing with `--texture_filter=2` (derived, not yet
> visually validated).
>
> **Process hazard, recorded because it bit this work twice:** the `--mips` flip and
> then the whole re-pin changeset were both swept into OTHER agents' commits
> (`99c09e7`, `daeb147`) because `FeatureFlags.def` and the git index are shared. A
> commit titled "S1d-6: the shell's silhouette" is what actually flipped a default
> that moved every scene pin.
>
> New: **`--mip_stats`** (per-level draw/area histogram at exit, changes no pixel) and
> **`--mip_aniso`** (max-axis LOD instead of the geometric mean — default OFF, awaiting
> the user's eyes). Crops: `docs/img/mipsel/`. Full write-up in the commit message.


> ## 2026-08-06 — THE MITRE INVERSION IS ROOT-CAUSED; THE WELD IS NOW DEFAULT ON
>
> **`--pom_shell_weld` default 0 → 1** (commit `140b6a0`). Inert unless
> `--pom_shell` selects the lid, and `--pom_shell` is itself default OFF, so no
> shipping render moves — proven, all four gates re-run after the flip and still
> byte-exact. Within a lid arm the unwelded mesh is TORN: **232 612 → 14 163
> void px over the 16 review poses (−93.9 %)**, and the pixels it removes are a
> **full-height black gash between wall panels** at p9 t5958 plus the wall/floor
> wedge at p5 t5963 — the defect the user reported.
> `docs/img/s1d_2f/weld_gash_*.png`.
>
> **The open bug "the true mitre is geometrically correct and measures worse" is
> CLOSED, and the answer is: it optimises the wrong component.** At a fold of
> half-angle `T` the mean-normal weld moves a corner `off·cos T` along each
> incident plane's normal and `off·sin T` **tangentially**; the mitre divides by
> `cos T`, making the normal part exactly `off` and the tangential part
> `off·tan T`. Nothing consumes the normal exactness — `Vertex::ShellH` already
> records the height each corner reached — and the tangential part is what
> slides a patch's BOUNDARY sideways and opens the holes. Cleanest measurement,
> `weld=5` vs `=6` (identical pin set, differing only by the mitre): tangential
> slide 0.0450 → 0.0712 world (×1.58), **void 10 648 → 26 774 (×2.51)**.
> **98–100 % of every mode's extra void carries `--pom_path_viz` code 0 — no
> fragment rasterised at all — so it is geometry and no march-side hypothesis is
> involved.** Do NOT use `--pom_shell_weld=4` or `=6`.
>
> **The formula `off·(1−cos(half-fold))` in S1d-2e.5 and RESEARCH_II §8.5 R2 is
> retracted** — it is `off·sin T`, which is the 0.064 world S1d-2e.5 measured.
> The number was right; the formula was not.
>
> **Two things this changes for the prism (RESEARCH_II §8.6):** precondition 1
> must say "weld, but NOT with a mitre — minimise tangential slide", and there
> is a new precondition **5b, T-JUNCTIONS**: greets carries 140 (edge,T-vertex)
> pairs among the shelled faces alone and they own **70 % of `weld=4`'s void**.
> The mitre's whole difficulty is also specific to the LID-ONLY shell and is an
> argument FOR the prism — adjacent prisms share a side quad, so they stay
> watertight while their lids move apart.
>
> New diagnostic `--pom_shell_slit_census` (default OFF, init-time print, lives
> wholly outside `PomShell_Build`). Full write-up:
> **`docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-2f** (commits `2839c29`, `f2933f7`,
> `dc2e231`, `140b6a0`).

> ## 2026-08-06 — GEOMETRIC TESSELLATION IS BACK ON THE TABLE: +7.3 ms, NOT +54.5
>
> **`--greets_displace` was retired on a number that was wrong by 7.4×.** It is
> now a first-class, working, one-flag option and it is **CHEAPER than the
> recess-shell arm at three of six review poses**. Full tables, look crops, the
> §C4 re-verification and the gates are in
> **`docs/ENVDYN_DISPLACEMENT_PLAN.md` §ADDENDUM 2026-08-06** (commit `1a91ed5`).
>
> **Measured, t=5780, 1080p, 12 threads, iters=20, interleaved, min-of-arm:**
> flat POM **48.5–49.5** · recess shell **56.1** · **tessellation 55.6–55.9**.
> Per pose (min-of-5): tess−flat is +2.4 / +4.0 / +4.1 / +13.8 / +13.9 / +14.1
> and **tess−recess is −4.2 / −2.0 / −1.3** at the corner, grazing-close-up and
> corridor poses. The shell's cost is per-PIXEL and explodes at grazing;
> tessellation's is per-FACE and nearly pose-independent.
>
> **Why the old number was wrong — three landings, none of them tessellation:**
> `9b6d70d --tile_bbox_cull` (default ON) landed **1 h 40 m AFTER** the
> edge-carve commit whose "107.0 ms" the plan quotes, and its own message
> measures the displaced arm 100.0 → 87.0 ms; `a1f89d4 --xfrm_soa_inline` −2.0
> ms; `799c808` removed a faceless mesh that was **84.3 % of that arm's 6.83 M
> shadow verts/frame**. Then this session found the fourth: **the mirror clone
> was re-transforming and re-rasterising the entire tessellated wall** (11.40
> ms/frame vs 3.31 in the flat arm; the clone pushed 42 870 faces while the
> direct view pushed 28 598, because a clone is culled by the frustum and not by
> the mirror WINDOW).
>
> **`--greets_displace` now defaults two perf companions ON** (a `[STONE]` log
> line names them; `--no-<flag>` still wins; both inert without displacement, so
> the shipping flat-POM arm is byte-untouched): `--greets_shadow_proxy` (−5.9 ms;
> **not look-neutral** — byte-identical at 5 of 16 review pairs, worst t=6097
> 58 021 px >12/255 at the corner junction) and the new
> `--greets_displace_flat_mirror` (−5.9 ms; **byte-identical at both mirror
> review poses**, 2 990 px >12/255 at t=5743). One flag = the affordable arm,
> byte-verified identical to spelling all three out.
>
> **Per-face cost, the user's own question, answered:** 92 ns/face threaded ≈
> **0.60 µs/face core**, against the 2–2.8 µs serial the campaign has been
> reasoning with — **3.3–4.7× cheaper**, almost all of it `--tile_bbox_cull`.
> Which is also why the S2/S5 chunk LOD is **not built**: with the companions on,
> the 87 k-face edge carve and the 43 k-face dome path are **0.22 ms apart**, so
> halving the faces buys ≈0.2–3 ms. Ceiling measured, reasoning in §A4.
>
> **What tessellation still cannot do (§C4, re-verified today):** relief lives
> only at the lattice. At t=6097 it writes **0.0023 world = one zEnc code** over
> a 600×400 box that a depth-writing per-pixel arm resolves at 0.0110–0.0233; at
> t=2845 it carries 83 %. **What only it can do:** true silhouettes, real depth
> for every offscreen consumer, and geometry that cannot swim — at t=5958b, the
> grazing pose where the shell smears and slip p99 hits 501, it renders a crisp
> geometric step.
>
> **Still open:** `--greets_displace` at t=6097 is run-to-run nondeterministic
> (6 runs, 6 hashes) while t=5780 is stable 6/6 — not root-caused, and the one
> thing between this arm and full gate-worthiness.

> ## 2026-08-06 — WHERE THE DISPLACEMENT CAMPAIGN ACTUALLY STANDS (the shell half)
>
> **The per-pixel shell can produce protrusion the user likes ("fantastic when
> it works") but not at a setting that is also stable.** That tension is the
> campaign's central measured finding, and it is not a bug in one code path —
> four rounds of hypotheses (cone march, `--pom_normal`, step exhaustion,
> bitangent handedness) were each measured and each REFUTED as the cause.
>
> **The instrument that finally matched the user's eye: SLIP** — texels of
> texture sliding per frame at a fixed point on the stone (`--pom_path_viz`
> mode 2 + the `_uvgeo.bin` camera-free surface coordinate). Arms the user
> calls clean measure p90 0.01–0.12; the arm he called "swimming like a shark"
> measures **p90 15.3 / p99 501** — half a texture tile per frame. Every
> earlier metric (jerk, frame-diff, error-vs-reference) disagreed with his eyes;
> this one agrees.
>
> **Cap ladder** (recess arm, slip p99 / reach p90; clean floor 0.60):
> cap2 0.82/28.6 · cap4 **1.44/53.8** · cap8 3.41/94.5 · cap16 11.4/184 ·
> cap32 37/338 · cap64 **501**/109. Cap 64 has LESS reach than cap 32 — it
> pushes 18.5 % of the wall into the flat clamp. Cap 4 sits at the clean floor
> with ~14× the non-shell arm's reach.
>
> **Why the mechanism, not a bug:** our shell marches the TRUE view ray
> (÷V·N, capped) where classic POM uses the OFFSET-LIMITED form. That was a
> deliberate S1b choice ("grazing lateral travel is exactly what silhouettes
> are made of"). Slip scales with the CAP and NOT with step count (32/128/512
> identical). A hard offset clamp has no usable band (24 texels = no relief;
> 48/64 = polygon artifacts). **Recess-only is clean (0 void, 0 offscreen
> delta) but structurally cannot show a gap between blocks** — the user
> confirmed by eye that cap 4 recess "just gives something equivalent to the
> regular parallax".
>
> **OPEN, and it decides the campaign:** does the LID arm at cap 4/8 show real
> see-through between blocks? If yes, protrusion is viable at a stable setting.
> If no, the swim-free band and the see-through band do not overlap, and
> recess-only is the shippable result with real geometry the only path to
> silhouettes.
>
> **Also measured, do not re-litigate:** `--pom_cone_min_step=1` with only 32
> march steps leaves 9.5k–52k px UNRESOLVED (no parallax shift at all, a hard
> discontinuity clustered at steep block edges); `--parallax_pom=128` drives it
> to 0, and removing the floor makes it 20× worse (209k px — a true per-texel
> cone truncates to a zero step and the march stalls). The march was ALSO
> missing `Material::TbnHandedness` (real bug, fixed `7bfbc87`) — but applying
> it moved slip by nothing, and the per-face variant made the p99 tail WORSE,
> which is evidence the per-face determinant test itself is wrong (it only
> tests 3 verts, only on normal-mapped materials; scene-wide agreement is
> **40.1 %** — 302 of 406 `rooms` faces are negative-determinant but sit on a
> handedness=+1 material). That affects the DEFERRED KERNEL's normal mapping
> too, independent of displacement. Queued in OPTIMIZATION_BACKLOG.
>
> **In flight at write time:** (a) mirrors reflecting the flat proxy while
> still running the parallax march (user-approved; measured that the march DOES
> read in the reflection — 96.3 % of mirror pixels change without it) + a
> shadow-pass geometry decomposition; (b) `docs/DISPLACEMENT_RESEARCH_II.md` —
> a literature re-read against these measurements, whose hinge question is
> whether anyone ever SHIPPED silhouette-correct per-pixel displacement or
> whether shipped POM simply had flat edges; (c) `docs/GPU_BENCHMARK_PLAN.md` —
> a standalone GPU deferred path as a BENCHMARK and ground truth (user: "not as
> a shipping backend"), gated out of the normal build.
>
> **Perf, measured this session:** XFRM main-view 7.9 ms at 958k verts, of
> which the SoA dual-write was 2.40 ms — removed (`--xfrm_soa_inline`, default
> ON, bit-exact, **−25 %**). The transform loop is CACHE-LINE-BOUND (`Vertex`
> is pack(1) 140 B), NOT arithmetic-bound, which is why wider SIMD washed and
> why an approximate reciprocal measured SLOWER. A BVH/hierarchy is refuted
> with numbers (0.45 core-ms ceiling over ~9,150 mesh tests/frame). **The real
> geometry elephant is the SHADOW passes: 33–36 calls/frame, 7.6M verts,
> 340–790 core-ms**, vs main view's 0.96M / 4–7 ms.
>
> **Review poses live in `docs/greets_review_poses.txt`** — every camera the
> user has reported a defect from. Use them; agents kept re-deriving them.
> **F4/F3 scrub the scene clock** at `--scrub_speed`× (default 4).

> **2026-08-05 — GREETS RENDER NONDETERMINISM IS CLOSED. GREETS IS A GATE
> SCENE AGAIN.** Root cause: the opaque deferred kernel read AO maps with the
> WRONG TEXEL WIDTH. `Material::AoMap` arrives from the importer as
> single-channel **8-bit** (`MakeHeight8`, same as height/roughness/metallic),
> but `DeferredSurfaceKernel.cpp` fetched it as `dword` —
> `((const dword*)mip)[swizzledUV]` — so every AO sample sat at byte offset
> `4 × swizzledUV` inside a **1-byte-per-texel** allocation. **Measured** at a
> diverging pixel: `swizzledUV = 995355` in a 1024² (1 MiB) mip → byte offset
> **3,981,420**, i.e. **3.8 MB past the end**. The returned heap bytes differ
> per process; with `ao_map_strength` 2.0 they drove `ao = 1 - 2·(1-aoRaw)`
> down to **-2.22**, the ambient term went **negative**, and `lB<0 → 0` clamped
> it — the long-hunted "diffuse flips 0 ↔ full while specular stays identical".
> Every sibling map fetch (roughness, metallic, xpar-AO) already read bytes AND
> bound-checked `miplevel < numMipmaps`; only this one did not. Fix mirrors the
> transparent kernel's AO fetch. See the Known Issues entry for the full chain.
> - **RESULT [M]:** greets gate recipe **0 flips in 128 sequential runs**, one
>   hash `f5778c7b78a4d70655291363e4119c66` (95 % upper bound on the true rate
>   **0.023**, ~1 in 43). Pre-fix the same recipe flipped **~0.85**. Also 0/16
>   with `--env_refl` ON and 0/16 under `--vanilla` (forward path).
> - **LOOK CHANGE [M]:** greets now shades with the REAL AO map instead of heap
>   garbage — **26.3 % of the frame moves, mean |Δ| 12.4, max |Δ| 98** at the
>   gate pose. Four materials carry separate AO maps (`momy-1`, `amudim`,
>   `stairs`, `rooms`); `--greets-stone-tex` materials use `Mat_AoInAlpha`
>   (albedo alpha) and were never affected. **This wants the user's eye** — it
>   is a bug fix, not a tuning call, but the wall/pillar occlusion look changes.
> - **Gates unchanged:** city `37e62845`, fountain `51fff7cd` byte-exact;
>   render_gate 3/3. The bug only fires on materials with a separate 8-bit
>   `AoMap`, which only greets ships.

> **2026-08-05 — GREETS NOW SHIPS THE PBR STACK BY DEFAULT, and `--vanilla`
> turns everything back off.** Two user-requested changes on fog-wt.
>
> **(A) Greets scene defaults gained five flags** (`GreetsApplyRunDefaults`,
> DEMO/GREETS.CPP — the RUN block, not init, because all five are global render
> flags): `pbr`, `env_brdf_analytic`, `pbr_multiscatter`, `diffuse_energy`,
> `sh_ambient`. That is exactly the set the user typed on every greets launch.
> Applied via `FF::setDefault` behind the existing `GreetsScenePreempted()`
> guard, so an explicit `--no-pbr` still wins and `--scene-mirrortest` /
> `--scene-conetest` never inherit them.
> - Derived from the kernel, not from the flag list: `pbr` is read at
>   DeferredSurfaceKernel.cpp:1429 and drives BOTH the scalar per-light branch
>   (:2400) and the 8-wide vec loop (:2156) — the flag's own help text saying
>   "vec path only" is STALE, greets' normal-mapped pixels take the scalar
>   branch and do get GGX. `env_brdf_analytic` (:1439), `pbr_multiscatter`
>   (:1440) and `diffuse_energy` (:1442) all sit behind `env_refl`, which greets
>   gets for free: its RVSM metallic-map imports (momy / amudim / screen emiter)
>   call `setDefault(env_refl,true)` in MaterialImport at init — **measured in
>   the init log**, which is why those three are not dead defaults.
>   `pbr_multiscatter` is a strict no-op without `env_brdf_analytic` (it reuses
>   its A,B terms), so the pair ships together. `sh_ambient` (:1445 +
>   RENDER.CPP:489) is independent of env_refl.
> - `metal_map` / `roughness_map` / `ao_map` are ALREADY compile-default ON in
>   FeatureFlags.def — verified, nothing added for them.
> - NOT included: `pbr_roughness`, `deferred_vec_force` ([test] knobs), and
>   `xpar_pbr` — which is not a dependent of `pbr` at all (the transparent
>   kernel reads it independently at :2785 and never reads `pbr()`); turning the
>   greets glass PBR is a separate look call nobody has made.
> - **COST [M]** greets bench t=5780, 1920×1080, `FDS_THREADS=1` (one core is
>   the only load-robust arm on a box other agents are rendering on), 6
>   interleaved pairs: **+20.0 ms/frame (+5.3 %)** in the least-loaded pair,
>   +25.9 ms on min-of-arms, ~+39 ms median. The 12-thread A/B could NOT resolve
>   it — 10 interleaved pairs spanned 60.8–199 ms/iter under load 9–47 and min-ON
>   (60.8) came in *under* min-OFF (63.3). Inferred, not measured: at the
>   observed ~6.5× pool speedup that is **~3–6 ms/frame** in a normal run.
>
> **(B) `--vanilla` / `FDS_VANILLA=1`** (FeatureFlags.def + .cpp, category
> engine, default OFF): forces EVERY flag to its compile-time default AND marks
> it explicitly-set, so scene `setDefault` blocks (greets' new PBR set included)
> and `SCRIPTS/*.params` are suppressed too — without the set marks "vanilla"
> would be a lie. **Semantics are pure parse order: put it FIRST.** Proven on
> conetest: `--vanilla` + the render_gate cone recipe = `b41894f9…`, byte-equal
> to the gate baseline; the same recipe with `--vanilla` LAST = `1bc0dc35…`.
> It CLEARS ENV-SET VALUES (the eager FDS_* scan runs before argv, so the CLI
> form wipes it; the env form is applied after the env scan for the same rule),
> works inside `--flags-file` and `FDS_BAKED_ARGS`, and prints a one-line
> `[FLAGS] --vanilla: 424 flags forced…` note so a run is self-identifying. It
> is compile-time DEFAULTS, not all-off: `deferred` defaults off, so a vanilla
> run renders the FORWARD path. Startup-only — the tune console returns 400
> rather than pretending a live mass reset works.
>
> **(A) is REAL and frame-wide, measured against the noise:** three ON runs vs
> three OFF runs of the greets gate pose (`--no-shadows` variant), pairwise —
> within-arm run-to-run noise touches 0.64–14.0 % of the frame, cross-arm ON-vs-OFF
> touches **80.0 / 88.9 / 88.9 %** with max |Δ| 231 and mean |Δ| 4–10 on the changed
> pixels. A broad low-amplitude shift over nearly every lit pixel is exactly the
> signature of swapping the BRDF + the ambient model, and it is an order of
> magnitude outside the noise. `[SHAMB]` appears in every ON run's log and in no
> OFF run's — the SH probe really is baking.
>
> **Gates**: city `37e62845` and fountain `51fff7cd` byte-unchanged;
> render_gate 3/3 (the preempt guard holds); chase byte-identical to the SAME
> binary's pre-change run at all five poses + both cinematic poses. ~~The greets
> pin could not be re-taken — greets is currently 100 % nondeterministic~~
> **SUPERSEDED same day: the nondeterminism was the 8-bit-AO-map-read-as-dword
> bug (see the top block); greets is re-pinned and gate-worthy again.**

> **2026-08-05 — S1d-2d: THE LID ARM'S VOID WAS NEVER THE MARCH. It is the lid
> offset TEARING THE MESH.** Read `docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-2d. Four
> new flags, all default OFF and byte-null: `--pom_shell_weld`,
> `--pom_shell_lid_edge`, `--pom_shell_side_entry`, and mode 3 of
> `--pom_shell_side_faces`.
> - **The discriminator, before any code.** Lid arm void 413 100 px over the 13
>   review poses. With the domain kill AND the base clip both off: 228 411. With
>   `--no-parallax` (no march at all): 198 704. With `--pom_shell_lid_probe`
>   (offset forced to 0, everything else identical): **0**. Against the offset
>   0.02/0.06/0.18/0.36 world: 19 416/58 665/198 131/383 364 — **LINEAR**. The
>   void is a SLIT IN THE GEOMETRY whose width is the lid offset.
> - **Cause:** `PomShell_Build` extrudes along each vertex's OWN normal, and
>   `MakeFacesIndependentByAngle` leaves `rooms` with 588 verts over 196 faces —
>   exactly 3 per face, nothing shared. 155 distinct POSITIONS carry them; 420
>   uses disagree with their position's mean normal by >1°, worst 78.7°.
> - **`--pom_shell_weld=1`** extrudes along the shared (welded) normal, as shell
>   maps do — a mitred corner instead of a wedge. `Vertex::N` untouched, so
>   shading is unchanged; `ShellH` picks up the mitre automatically (min
>   0.955 → 0.598). Void 413 100 → 214 650, and 228 411 → **14 163** with the
>   other two kills off.
> - **`--pom_shell_lid_edge=1`** gives the lid arm the recess arm's CLAMP for a
>   lateral exit only — a non-crossing ray, a side-entry miss and lid overhang
>   still DISCARD, so the see-through survives. That was the other ~152 000 px.
> - **`--pom_shell_side_faces=3`**: the lean must bound the shell only BELOW the
>   authored plane (`dh = max(0, h0−h)`). Above it the neighbour's SHELL bounds
>   this one, and with the weld the two lids already meet at the ridge. Modes 1/2
>   narrow it instead and kill lid rays with real material under them —
>   **67 816 px of pure black**. Byte-identical to mode 1 under recess-only.
> - **`--pom_shell_side_entry=1`** — the restructure the task asked for IS BUILT
>   and is correct: the ray and all four leaning side planes are affine in the
>   slab height, so entry is one convex slab clip and the march starts at the
>   side-face crossing. Nothing serialises (`hStart` was already a `Vec8f`).
>   Depth needed no change (the S1a write is relative to the RASTERED surface).
>   **It is not what was blocking protrusion**, and with mode 3 it is inert by
>   construction — so the two are alternatives, not a stack.
> - **RESULT.** Recommended lid arm = `--pom_shell_weld=1
>   --pom_shell_side_faces=3 --pom_shell_lid_edge=1 --no-pom_shell_base_clip`:
>   **void 413 100 → 14 163 (−96.6 %)**, nine of thirteen poses at 0–159, and the
>   **rust stripe is gone** (crop `docs/img/s1d_entry/p5743_B_...`). Residue is
>   13 986/14 163 GEOMETRIC — the cross-material wall/ceiling junction the
>   per-material weld cannot close.
> - **WHAT IT DOES NOT FIX: offscreen.** Shadow cube vs flat: rec 0, tess 5.32 %,
>   lid 20.68 %, this arm 18.77 %. Non-stone colour >12/255 at p5743: rec 1 411,
>   tess 1 035, lid 9 605, this arm 9 033, and slightly WORSE at both mirror
>   poses. Moving vertices is the lid model's intrinsic cost.
> - **SEE-THROUGH IN THE VALLEYS: still not demonstrated**, and I believe
>   structurally so — greets is a closed room, so a mortar valley has nothing
>   behind it to reveal. Measured: 0–23 px per pose of "a surface >3 world behind
>   the wall wins", even at 3.3× amplitude.
> - **Concave-fold Z-competition hypothesis: FAILS in the recess arm.** 63.4 % of
>   ANGLED_IN clamped pixels (76 765 of 121 014) void under a discard — no second
>   fragment exists, because at an inside corner two walls ABUT on screen rather
>   than overlap.
> - Gates: render_gate 3/3, city `37e62845`, fountain `51fff7cd`, greets recess
>   AND lid arms **depth AND colour** byte-identical at all 13 poses with the
>   flags off — against a binary built from the PARENT COMMIT in a clean
>   worktree, run from the same `Runtime/`. wasm links, 0 bad flags in 578 run
>   logs. Perf: marginal cost ≤ ~0.5 ms/frame as an upper bound from min-of-10 —
>   the machine carried load average 5–15 all session and I could not resolve it
>   better.
> - **greets COLOUR is a usable byte gate again.** After `f4e81e9` (the
>   concurrent AO-width fix) the same recipe gives 3/3 identical colour hashes at
>   p5743/p5958b/p6097. Before it my flags-off pair differed at 13/13. That
>   commit also swapped `Runtime/DEMO` under one of my render batches, so every
>   table in §S1d-2d was re-rendered on the post-fix binary and reproduced to the
>   digit.
> - **Trap recorded:** `--pom_shell_side_edge` must NOT be used with the lid.
>   `PomShell_Build` runs once per material, so `floor`'s seam census sees
>   `rooms` already displaced and mis-labels 19 of 24 sides as free edges.

> **2026-08-05 — S1d-2 CLOSED SHELL (SIDE FACES) IS IN, all flags default OFF.**
> Read `docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-2. Flags: `--pom_shell_side_faces`
> (0/1/2) and `--pom_shell_side_edge` (0/1/2).
> - **PROVENANCE WARNING:** the code, the doc section and `docs/img/s1d_side/`
>   were swept into commits `3712f00` and `2c54ae9` ("editor: displacement
>   panel …") by a concurrent session running `git add -A` in the same worktree
>   while this stage was finishing. The commit titles do not describe the
>   S1d-2 content they carry. Nothing is lost; the log is misleading.
> - **Step 1 (side faces).** At a convex ridge the side face is the neighbour's
>   plane and it LEANS: the solid is the intersection of the half-spaces, so the
>   material reaches cot(fold)·depth past the ridge and the vertical UV box cuts
>   it off there. Four leaning half-planes, baked from S1d-1's topology.
>   Measured over all 13 review poses: pixels the march cannot answer
>   **809 415 → 629 711 (−22 %)**; the subset that goes BLACK
>   **231 073 → 129 579 (−44 %)**; at the user's gash pose t=5743
>   **85 065 → 28 634 (−66 %)**; at the smear pose t=5958b
>   **20 244 → 4 115 (−80 %)**. Void stays at 5 (tess 13, flat POM 5).
> - **LOOK is NOT a clean win.** The gash narrows to a sliver and the t=5958
>   mortar joint tightens toward tess — but at t=5743 the recovered band renders
>   as a **saturated rust stripe**: the lean puts the ray in the right place and
>   then samples patch A's chart EXTRAPOLATED (up to 0.06 UV = 61 texels = 0.36
>   world) where the content belongs to patch B. That is S1d-2c's hand-off,
>   now a measured argument rather than a projected one.
> - **Step 2 (per-class edge policy) is the cheapest win.** Keyed on a per-side
>   TRUE-BOUNDARY SUB-INTERVAL, not the dominant class — a free edge is 0.5 % of
>   `rooms` boundary length but owns 11.9 % of the unanswered pixels, so the
>   dominant-class version fired on **0 pixels**. With the interval:
>   **100 570 px at t=6097 (4.85 % of frame) at ZERO void cost**, and the corner
>   silhouette moves toward the tessellation reference.
> - **PROTRUSION IS NOT RESTORED.** Side faces make the lid arm WORSE: void
>   413 100 → 468 868 (clip kept) / 933 535 (clip replaced). Mechanism: the same
>   lean that widens the shell below the authored plane narrows it above, and my
>   side faces are only a domain TEST — a lid ray they reject is killed instead
>   of ENTERING the shell lower down through the side face. Side-face ENTRY (a
>   per-lane march start height) is the next increment and protrusion needs it.
> - Gates: render_gate 3/3, city `37e62845`, fountain `51fff7cd`, greets recess
>   arm depth byte-identical at all 13 review poses, wasm links, 0 bad flags in
>   299 run logs. Crops: `docs/img/s1d_side/`.
> - **Process note:** `DEMO/CMakeLists.txt` copies the freshly-linked binary into
>   `Runtime/DEMO` on every `cmake --build build`. Never build while a render
>   batch is in flight — it cost me one arm that looked like a regression.

> **2026-08-05 — S1d-1 SEAM CENSUS DONE, and it OVERTURNS the S1d plan's
> premise.** Read `docs/S1D_CLOSED_SHELL_PLAN.md` §S1d-1. Two new flags,
> **both default OFF, byte-null**: `--pom_seam_census` (patch-boundary topology
> + classification, init-time print) and `--pom_seam_viz` (class-coloured
> boundary overlay).
> - Of the **800 513 px** the march cannot answer across all 13 review poses,
>   **31 (0.004 %) sit at a COPLANAR seam.** 72.9 % sit at a CONVEX angled ridge,
>   15.1 % at a concave fold, 11.9 % at a true boundary. Of the 231 064 px that
>   actually go BLACK: 0.013 % coplanar, 66.8 % convex ridge, 33.2 % concave.
> - **Coplanar continuation is already shipping** as `--pom_shell_merge_uv`'s
>   sibling boxes (measured: carries 70 585 px, up to 67.6 % of the would-be
>   population at the mirror poses) and the coplanar UV transform is the
>   **IDENTITY** (worst disagreement 1e-6 UV). So S1d's option (A) as scoped is
>   already done and worth 31 more pixels.
> - **(B) side faces dominate**: 84.9 % of clamped / 66.8 % of void. The user's
>   "full-height black gash on the right wall" at t=5743 is ONE convex ridge
>   (`rooms` g=9, 27° fold) owning 482 171 of the 800 513 px.
> - TRUE boundaries void ZERO under a discard — there the discard is already
>   right and the recess arm's CLAMP is the bug. Cheapest available win.
> - Angled continuation is the expensive path: 27 fold angles, 71 distinct
>   (fold, scale, mirror) transforms, **41.8 % MIRRORED charts**, and 5.4 % of
>   its targets point at the unshelled ceiling. One hop suffices (p99 = 232
>   texels past the boundary).
> - Gates: render_gate 3/3, city `37e62845`, fountain `51fff7cd`, greets
>   shell/tess/flat t=6097 all byte-exact, wasm links. Crops:
>   `docs/img/s1d_seams/`.

> **2026-08-05 — S1 P2-A: `--pom_recess_only` IS IN, default OFF.** Read
> `docs/S1_DISCREPANCY_INVENTORY.md` §10. The user's BLACK HOLES (full-height
> gashes between wall panels, black bar in the mirror) are the LID model's
> mandatory lateral-exit discard firing at internal patch seams; the converged
> reference shares that boundary model, so **every P1 number was scored by a
> yardstick blind to it**. VOID (z==0 px) is now a mandatory column on every arm.
> Void at the user's t=5743 pose: tessellation **3**, lid shell **98 371**,
> recess-only **0** — and 0 at all seven poses measured.
> - Recess-only moves NO vertex; the height field's max sits at the authored
>   plane and all relief carves inward; a ray leaving the patch CLAMPS
>   (`--pom_recess_edge=2` restores the discard as a diagnostic and voids
>   68 k–130 k px on identical geometry, which is how the mechanism was pinned).
> - VERIFIED: shadow cube vs the no-shell arm **0 of 13 533 184 texels** (lid:
>   29.88 %) → C6 zero by construction; **0 px frame-wide drawn nearer than the
>   authored plane** at six poses (lid: 26–74 %) → S1a's ordering hazard retired.
> - COSTS: 0.8–8.5 % of the frame renders FLAT (the clamp) in bands along the
>   seams; the surface recedes half a slab (pair with
>   `--pom_shell_world_amp_set=0.18`); nothing can stand proud of the authored
>   plane ever again. Perf: no measured cost (−0.8 ± 0.9 ms vs the lid arm).
> - It is the cheap correct-by-construction option, NOT the literature one —
>   a CLOSED shell (Hirche'04 side faces + cross-patch march) is S1d.
> - Recipe: `--no-greets_displace --parallax_pom_cone --pom_shell
>   --pom_recess_only --pom_shell_cap=16 --parallax_pom=32 --pom_cone_exact=1
>   --pom_cone_min_step=1 --pom_march_earlyout --pom_shell_world_amp
>   --pom_shell_world_amp_set=0.18 --pom_normal`. Crops: `docs/img/s1_p2a/`.

> **2026-08-04 (session 3) — S1a + S1b + S1c ARE ALL IN, all default OFF, all
> byte-null** (render_gate 3/3, city `37e62845`, fountain `51fff7cd`, wasm
> links). Read `docs/S1_PIXEL_DISPLACEMENT_PLAN.md` for the full record.
> - S1a `--pom_depth_write` (c2616e4), S1b `--pom_shell` (c556148).
> - **Floor void CLOSED** (dfb4272): `--pom_shell_merge_uv` gives each patch a
>   SIBLING BOX LIST (coplanar patches whose UV rects abut), and the domain is
>   the UNION OF THE BOXES — never their bounding box, which was tried first
>   and destroyed the t=6097 corner silhouette. Void at t=5780 **6175 → 404 px**
>   with the corner discard pixel-identical.
> - **t=6097 corner band ADJUDICATED**: the discard is CORRECTING the lid, not
>   eating wall. Of 178 802 discard-affected px: 0 void, 100 % revealing a real
>   surface ~5 world units behind. Reference framing: tess == flat POM exactly;
>   shell-no-discard over-covers by 35 436 px, shell by 12 162 — the discard
>   removes 23 k px of lid inflation. Instrument: `FDS_SNAPSHOT_GBUFDUMP=1`
>   (G-buffer matID plane) + `scratchpad/classify.py`.
> - **S1c `--pom_horizon` LANDED**: 8-azimuth horizon bake (disk-cached, 99–128
>   ms, NOT minutes) + per-light tangent-space elevation-vs-horizon compare.
>   The groove shadow MOVES with the light — the one thing neither the
>   tessellation bake nor the shell march can do (PolyId shadows are
>   identity-only). Path-agnostic: works under `--greets_displace` too.
> - **Perf [M]** greets t=5780, iters=40, 4 interleaved pairs, load 2.3–4.9:
>   flat POM 56.9 · **shell 58.1** · **shell+horizon 61.0** · **tessellation
>   104.7** · tess+horizon 106.8 ms/iter. Horizon = +2.9 ms median for all 7
>   omnis.
> - OPEN: nothing blocking. The user picks the defaults; the three-way crop
>   list is at the end of the plan doc.
>
> **2026-08-04 — ACTIVE CAMPAIGN REDIRECT:** the current campaign is
> **S1 per-pixel shell displacement** — read `docs/S1_PIXEL_DISPLACEMENT_PLAN.md`
> FIRST (mission, stages S1a/S1b/S1c, validation battery, discipline). Research
> basis: `docs/DISPLACEMENT_RESEARCH.md` (07b72c7). In flight at write time:
> (a) S1a `--pom_depth_write` agent in a worktree; (b) groove-line zigzag fix
> agent in the main tree (tessellation path; diagnosis correction: the sawtooth
> verts were NEVER snapped groove-line verts — re-scoping via --displace_viz).
> Recently landed on fog-wt: chunk-occlusion experiment (VISIBILITY_PLAN §7 —
> occlusion refuted, default OFF), displacement fold-relax + parent-plane
> shadow ids + neighbor pin (all default ON; bleed root cause was the PolyId
> single-id collapse, fixed by `greets_displace_shadow_planes`). User note:
> bare `--parallax_pom_lod` in their flag list parses to NOTHING (needs =value).

Read this when resuming. Branch **fog-wt**, nothing pushed. Previous campaign
(structural push): docs/posts/SESSION_STATE_2026-07-04_structural.md.
Range covered here: `1ca269d..7282f7a` (~60 commits, 2026-07-08..11).

## Verification protocol (THE gates — run before/after everything)

All runs headless from Runtime/: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy`.

| gate | recipe | pin |
|---|---|---|
| city | `FDS_CITY_ENV_PIXEL=1 ./DEMO --snapshot=city@t=1961 --out=<dir> --deferred` | **⚠ THIS PIN IS CONDITIONAL ON THE ENV CUBE ON DISK — check `md5 Runtime/cache/city_envmap_cube.bin` BEFORE calling a mismatch a regression.** The cache key ignores FeatureFlags, so the cube is a hidden input the recipe does not state (full analysis + 2×2 matrix in the dated note above). `d1d67f0f84fb4af3713e15a64a1b827b` = pre-flip bake → the pins below hold. `63978a18ed31837348598014716f9932` = cold/current bake (mips ON) → **`5476be8c43864c761b94e2dd83f86aa8`** default and **`b88ecb7bbd0340145e35a80bc7a82f6b`** under the control; both are correct-for-that-cube, NOT drift. A **fresh worktree always cold-bakes**, so it lands in the second column unless you copy the cube in. Also: `DEMO` chdirs to its OWN directory (`ChdirToAssetRoot`, `DEMO/REV.CPP:503`) — launching a worktree binary from the main `Runtime/` does **not** render the main tree's assets or its cube. **Pending decision:** adopting the flip properly means `rm Runtime/cache/city_envmap_cube.bin` and re-pinning to `5476be8c…`; held for the user's eye on `docs/img/mipsel/city_t1961_envbake_crop.png` (max Δ 6/255, glass only). — **RE-PINNED 2026-08-08 (`--mips` default 0→1): `e1221676372e0bba6f65343f6d85b8e7`** (stable 2/2, pre-flip cube). Prior pin `37e62845c4d30eefa321730c5bb7e0b8` reproduces EXACTLY under `--no-mips --no-mip_fix` **on the pre-flip cube** (on a cold-baked cube that control arm is invalid — it measures a mips-ON bake under a mips-OFF frame). Divergence: 133 854 px changed (6.46 %), mean \|d\| 7.04 on changed, 24 761 px >12/255, max 192 — building facades, see `docs/img/mipsel/city_t1961_worst_crop.png`. |
| greets | `FDS_GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551" ./DEMO --snapshot=greets@t=1588 --out=<dir> --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --no-env_refl` | **CURRENT, measured on the settled tree at `7b5f1f8`+: `778fa6acd85a69cf241babefcdaf598e`.** Verified 4/4 before the `--shadow_lightmap_texel_density` flat-arm default, 4/4 after it, and 4/4 with the revert flag `--shadow_lightmap_texel_density=0` — **12 runs, one value; that change does not move this pin** (it is look-null at all 16 review poses too, see the dated block at the top). fountain `8db68ccb59416e9a44037e9f387b7bd9` 4/4 and city `3cbe42b166847e40f7071eedb48d613c` 4/4 alongside it, `render_gate` 3/3. NOTE for whoever reads the history below: the hashes in the older entries (`9eeaf860…`, `6ed5462e…`, `91ec081a…`) do **not** reproduce at HEAD — they were taken while other agents held uncommitted work in the shared tree, exactly the hazard the `2026-08-09c` note warns about. Trust the settled-tree value above. — history: **RE-PINNED 2026-08-09 (`hull`/`cockpit` removed from the Sobel normal-map name gate, `DEMO/GREETS.CPP:1951`; docs/SHADING_CONTRACT.md §11 row E8): `9eeaf860cb5a7f124884a89e0fc3ff5b`** (stable 3/3, across two binary revisions). REASON: `BakeNormalMapFromDiffuse` was Sobelling MECH_HUL.JPG / MECH_COK.JPG — camouflage PAINT — into geometric relief; the user compared the mech against the standalone Metal arm (which bakes no such map) and preferred the GPU's. Only four materials ever hit the gate (`!M->NormalMap` guard); `hull`, `hull not smooth` and `cockpit` are gone, `siling` remains. **AT THIS PIN POSE THE CHANGE IS 1 PIXEL AT 1 LSB** (702,172) — t=1588 barely shows the mech, so the pin move is not the measurement. The measurement is at the §11 mech pose (t=4871): **179 829 px (8.67 %), max channel Δ 164, 11 677 px > 10 luma**, hull pixel (767,723) Y **131.2 → 44.9** against the GPU's 41.0, canopy pixel (760,620) 146.4 → 157.4 against 161.4. Crop: `/tmp/fogwt/task1_mech_strip.png`. city `e1221676…` and fountain `8db68ccb…` do NOT move (greets-only, guarded on `M->RelScene != GreetSc`); fountain re-verified. Prior pin `6780642b30430efa4fd2f87810b2dfdb` reproduces by re-adding the two `strstr` terms. Preceding that: **RE-VERIFIED 2026-08-09c, 3/3 EACH, on a settled tree at HEAD `4f60493`** — these supersede every pin value recorded earlier today, several of which were taken while other agents held uncommitted work in the shared tree and are therefore not reproducible:
> * greets   `778fa6acd85a69cf241babefcdaf598e`
> * fountain `8db68ccb59416e9a44037e9f387b7bd9`  (the ONLY pin that held all day)
> * city     `3cbe42b166847e40f7071eedb48d613c`
>
> Two hazards that produced the bad values, both worth remembering: a shared tree with concurrent uncommitted edits makes an ABSOLUTE pin meaningless — certify byte-nullity DIFFERENTIALLY (two binaries from one tree, one with the diff reverted) instead; and the FIRST run after a rebuild can write a cache the later runs read, so discard it.

**RE-PINNED 2026-08-09b (five more flags defaulted ON at the user's instruction: `metal_spec_f0`, `env_mip_chain=9`, `env_bake_linear`, `sh_bake_linear`, `env_bake_sh_first`): greets `6ed5462e38ced22ecc98b39730d2e915` (2/2), city `3cbe42b166847e40f7071eedb48d613c` (2/2), fountain `8db68ccb59416e9a44037e9f387b7bd9` UNCHANGED (stable 4/4 — but the FIRST run after a rebuild returned a different hash, a cold-bake artifact of the same class as the city env cube; always discard run 1). Preceding it: **RE-PINNED 2026-08-09 (the Sobel name gate DELETED + four flags defaulted ON: `deferred_checker_env_full`, `env_bake_include_animated`, `env_metal_tint_linear`, `shadow_noncaster_depth`): `91ec081a4211554de8f36975fe1ac171`** (stable 2/2). city `5476be8c` and fountain `8db68ccb` did NOT move. Preceding it: **`9eeaf860cb5a7f124884a89e0fc3ff5b`** (gate removal for hull/cockpit only), and before that **RE-PINNED 2026-08-08 (`--hdr_metal_kill` default 0→2, the conductor diffuse kill): `6780642b30430efa4fd2f87810b2dfdb`** (stable 2/2). city `e1221676…` and fountain `8db68ccb…` did NOT move — neither scene has a metallic-mapped material, so the fix is greets-only. Prior pin `adfba8ba3a1971a7c9cac0da689581b1` reproduces under `--hdr_metal_kill=0`. Preceding that: **RE-PINNED 2026-08-08 (`--mips` default 0→1): `adfba8ba3a1971a7c9cac0da689581b1`** (stable 2/2). Prior pin `f1297141611c484bac7cc10a8bdcf630` reproduces EXACTLY under `--no-mips --no-mip_fix` — note BOTH flags are required, because `--mip_fix` moves the subdivision cut lines and the `--mips` gate zeroes only the mip LEVEL, not the geometry. Superseded pin history follows: **RE-PINNED 2026-08-06: `f1297141611c484bac7cc10a8bdcf630`** (3/3 identical runs). Two intended overlay removals moved it in sequence, both pure screen text: `f5778c7b` → `06e1d4d1` (earlier work) → `ae358a6a` (the "Shadow: Depth\|PolyId [F3]" indicator deleted, commit `6b5556d`) → `f1297141` (the always-on centre-pixel `[MAT@…]` material probe moved behind `--mat_probe`, default off, commit `35ec295`; re-running that arm WITH `--mat_probe` reproduces `ae358a6a` byte-exact, which is what proves nothing else moved). Prior pin, for the record: **`f5778c7b78a4d70655291363e4119c66`** — taken over **128 sequential runs, 0 flips** (95 % UB on the flip rate 0.023) after the 8-bit-AO-map fix closed the nondeterminism. This supersedes both `de3e9a5fb3aa39e008ef41b83f2b8d1b` (pre-PBR-defaults) and the "NO VALID PIN" state. Includes the PBR scene defaults AND the user's uncommitted GREETS.FLD / momy textures / Piramid.lwo — a clean checkout hashes differently. Verify with `tools/flip_rate.sh -n 24` if a mismatch appears; a single differing run is now a real regression, not noise. |
| fountain | `./DEMO --snapshot=fountain@t=2500 --out=<dir> --deferred --hdr --glass-refract=1 --glass-test --profiler=0` | **RE-PINNED 2026-08-08 (`--mips` default 0→1): `8db68ccb59416e9a44037e9f387b7bd9`** (stable 2/2). Prior pin `51fff7cd38767d619280afe0498a6f24` reproduces EXACTLY under `--no-mips --no-mip_fix`. Divergence: 266 063 px changed (12.83 %), mean \|d\| 9.27 on changed, 53 238 px >12/255, max 254. |
| chase (default) | `./DEMO --snapshot=chase@t=100,400,800,1200,1600 --out=<dir> --deferred` | per-frame color-PPM md5, re-pinned 2026-07-30 (cone-tile sky-clip fix — see below; 3-run stable, byte==spot_cone_cull=0 ground truth):<br>**RE-PINNED 2026-08-08 (`--mips` default 0→1):** t100 `76e7cf68714666bda278f094be4f2c72` t400 `d458e82bf4514c4ff2850468aab5743c` t800 `c145c7a5861fba81d56746f7c10764ee` t1200 `31aa52039f9b228fa6307c12e14811eb` t1600 `1544b0e775900b099ac9e38d42fd750d`.<br>Control under `--no-mips --no-mip_fix` reproduces the 2026-07-30 pins EXACTLY for t100/t400/t800/t1200 — **but t1600 gives `c8c93b886dd31fcc01363c806d7626de`, NOT the recorded `7265d7855bdaae74e39f3c21d4f7e612`. chase t1600 had ALREADY drifted before the mip work; cause unidentified, needs its own bisect.** Prior (2026-07-30): t100 `f1a567133a3d20e6f3702c5c560a1299` t400 `2adfb0e8f783c01ec0714b9b396c82f0` t800 `0e2a8804f4feef1bf56f6ee9102a11b9` t1200 `7cefbdb062517865ba29ca88965e999f` t1600 `7265d7855bdaae74e39f3c21d4f7e612` |
| chase (cinematic) | `./DEMO --cinematic --deferred --snapshot=chase@t=800,1600 --out=<dir>` | re-pinned 2026-07-30 (cone-tile sky-clip fix; 3-run stable, byte==cull-off): **RE-PINNED 2026-08-08 (`--mips` default 0→1):** t800 `857d899d48ca55a6ae67f03e30b9bf02` t1600 `567e61532fb075b6e590b53a26cea2b6`.<br>Control under `--no-mips --no-mip_fix`: t800 `28e5a2a78d64ae98a1fcc4b739991be2` matches the 2026-07-30 pin, **t1600 gives `debdb1f435a14949b2e05be0bb53b1e7`, NOT the recorded `1cbde501c26d231a4295632dfbebd34b` — same pre-existing t1600 drift as the default arm.** |
| gate suite | `./tools/render_gate.sh` (repo root, dummy drivers) | ALL PASS — **baselines UNCHANGED by the 2026-08-08 mips flip** (mirrortest `4ac809e5…`, conetest `b41894f9…`, halotest `166fa25a…` all byte-identical with mips on and off; those test scenes carry no minified textured geometry). |
| wasm | `make wasm` | links |

Traps:
- **`DEMO` ignores your shell's CWD.** `ChdirToAssetRoot` (`DEMO/REV.CPP:503`)
  chdirs to the *binary's own* directory (first of `<bindir>`,
  `<bindir>/../Runtime`, `<bindir>/../../Runtime` holding a `rev.cfg`). The build
  copies the binary to `<tree>/Runtime/DEMO`, so **a worktree build always
  renders the worktree's assets and writes the worktree's `cache/`**, no matter
  where you `cd` first. `cd Runtime && /path/to/worktree/Runtime/DEMO` does NOT
  do what it reads like. To gate against the user's uncommitted authoring
  assets you must put the binary in a directory whose asset root *is* that tree.
- **The city env cube is a hidden input to the city pin, and it is not keyed on
  flags.** A cold cache re-bakes it and the pin legitimately moves; a run whose
  bake differs silently *overwrites* `Runtime/cache/city_envmap_cube.bin` and
  moves the main-tree pin with no commit. Always `md5` the cube before calling a
  city mismatch a regression. This cost one full "unowned drift" bisect on
  2026-08-08 — see the dated note at the top.
- **greets IS deterministic and IS gate-worthy — FIXED 2026-08-05 (det-hunt
  round 3).** The whole "~1-in-12 flip", then "100 % nondeterministic", then
  "not a bake and not a race" chain resolved to ONE defect: the opaque deferred
  kernel read 8-bit AO maps as `dword`, indexing 4× past the mip allocation
  (root-cause detail in Known Issues). **Post-fix: 0 flips in 128 sequential
  runs of the gate recipe** (one hash, 95 % UB on the rate 0.023), 0/16 with
  `--env_refl` on, 0/16 under `--vanilla`. Treat a greets mismatch as a real
  regression again. Confirm with `tools/flip_rate.sh -n 24` before calling it.
  The instrument stays: **`tools/flip_rate.sh`** — N sequential runs, distinct-
  hash histogram, flip rate vs the modal hash, Wilson 95 % CI, zero-event upper
  bound. A 3-run arm proves nothing at any nonzero rate; that is how rounds 1–2
  lost a day to a "shadow/lightmap bake" bisect that was pure binomial noise
  (at p≈0.85 a 3-run arm shows 2-of-3 with P≈0.32).
  These instrument traps cost real time in rounds 1–3 and still apply:
  **TRAP: the in-process repeat is NOT a valid determinism instrument for
  greets.** The code-screen texture is an ITERATIVE SMEAR
  (`OldBuf → GridRendererT → ScaledBuf → OldBuf`), so it is a function of how
  many times `Render()` has run, not of `t`. Repeating a timestamp in one
  process legitimately changes it. Compare separate processes.
  **TRAP: hash textures at `SizeX*SizeY*(BPP/8)`.** `Texture::BPP` is in BITS.
  Hashing `SizeX*SizeY*BPP` over-reads 8× and manufactures a convincing "these
  8 PBR maps mutate run-to-run" result. Same over-read family as the bug that
  turned out to BE the root cause — when a per-texel width is in play, check it
  first, in both the instrument and the code under test.
  **TRAP: one greets frame runs `renderFrame` SEVEN+ times, at three
  resolutions.** Six 512×512 offscreen passes (shard reflection / mirror RTT)
  and six 32×32 `sh_ambient` probe cube faces run the SAME `renderFrame`
  before the 1920×1080 main pass. Consequences:
  (a) a stage-trace filter that caches "the main width" on its FIRST call
      captures 512, not 1920, and silently hides the main frame;
  (b) the 32² probe faces are the CHEAPEST place to reproduce a shading
      divergence — 1024 pixels, ~2–8 of them differing, versus 2 M at 1080p.
      Round 3's whole diagnosis ran there.
  Always print the pass resolution on every trace line.
  **NOTE on `ctx.gb`:** round 2 recorded "hashing the global `g_gbuffer` is
  wrong for nested passes". In fact `EngineGBuffer_Resize` installs ONE global
  buffer and the offscreen passes address it at their own (smaller) stride, so
  `ctx.gb == g_gbuffer` for the probe passes — the real requirement is to hash
  only the first `xres*yres` entries of each plane, and to hash ALL ELEVEN
  planes (normal, tangent, txtr, albedo, lightmapMF, lightmapST, shadowMatID,
  faceId, mirrorId, mirrorMask, mirrorMaskZ), several of which are empty by
  default. Shard/mirror bakes with their own `DeferredOverride::gb` are the
  genuine exception.
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
  **STALE AT t=1600 (measured 2026-08-05):** with the user's uncommitted
  `Runtime/SCENES/CHASE.FLD` + `Authoring/chase/*.lwo` in the tree, default
  t1600 is `c8c93b886dd31fcc01363c806d7626de` and cinematic t1600 is
  `debdb1f435a14949b2e05be0bb53b1e7`; t100/400/800/1200 and cinematic t800 all
  still match the pins above. Those two are the mountain edits, not a code
  regression — same binary, same values before and after that day's flag work.
  Re-pin them when he commits the FLD.
  Valid snapshot range **t=0..1698** (past 1699 the harness re-dumps the last
  rendered VPage). Regen from `Authoring/chase/` via `pin_scene.py
  --legacy-vlum` is byte-identical to the shipping FLD (delta=0, 747,511 B) —
  the pre-edit baseline for later authored chase stages.
- **Chase cone-tile "missing light on the rect" — FIXED (2026-07-30).** User
  saw rectangular seams in the lighthouse beams during chase (~t=211,
  cinematic). Root cause: the volumetric cone-pass tile cull
  (`Render_VolumetricCones`, `--spot_cone_cull`) computed each tile's far
  bound `zHiT` from `tileLights.zMax` = the farthest **opaque surface** only.
  `computeTileDepthBounds` excludes sky/untouched pixels from `zMax`, so a tile
  that MIXES surface + sky under-estimated its volumetric depth: a beam glowing
  in the tile's sky portion (rays that run to the fog cutoff) got clipped away
  there but kept in the adjacent pure-sky tile → a rectangular per-tile seam.
  Fix: `TileLights.hasSky` (set in `computeTileDepthBounds` when any pixel
  `zEnc==0`); the cone cull extends the far bound to the fog cutoff for
  has-sky tiles (tight opaque `zMax` retained for fully-covered tiles, so
  covered scenes keep the cull's perf). Result byte-== `--spot_cone_cull=0`
  ground truth at every pose; the chase pin move above IS this fix. Cone-pass
  cost +~1–2 ms at t≈211/700 (the previously-dropped correct beam work); still
  ~6–10 ms cheaper than no cull. city/fountain pins byte-unchanged (no
  mixed-sky cone tiles); render_gate 3/3 (conetest byte-identical).

## The big architecture decision (2026-07-11, user-set direction)

> **✅ DONE 2026-07-31 — the sidecar-elimination campaign is COMPLETE.** The
> `.MAT` reader (`MaterialImport_ApplySidecar` + `_ApplySceneDefaults` + helpers)
> is DELETED; every scene now calls `MaterialImport_ApplyRevMaps` (LWO RVSM) in
> its place. `Runtime/SCENES/GREETS.MAT` (last sidecar, data-empty) is DELETED —
> no scene ships sidecar data. The 7 (+1) `#k` split-collapse sites in
> `tools/editor_server.py` are DELETED; splits bake real surfaces via
> `payload.splits` geometric centroids. Save-completeness proven headlessly
> (byte-identical FLD idempotent regen + combined RVSF/RVSM/SMAN gain + split
> without `#k`). Gates: render_gate 3/3, city `37e62845`, fountain `51fff7cd`,
> momy close-cam `7d05a1be` byte-equal. Leftover WRITE-only, not-yet-FLD-backed
> (editor writes a `.MAT` nothing loads, warned): `obj:scale` (§1d FdsObjectScale
> unimplemented) and `normalFlip` (§1e RVSM write-back unimplemented). See
> docs/SIDECAR_MIGRATION_PLAN.md. Original direction preserved below.

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

- **Greets mirror: cones leak through wall + doubled screen text
  (2026-07-30, user-reported, NOT yet investigated).** Repro:
  `FDS_GREETS_CAM="-6.75174379,3.12747574,-51.7348709,-0.0600466765,-0.148574546,-0.987076521"`
  t=3430, looking at a text-screen mirror panel. Two symptoms in one frame:
  (a) volumetric cone shafts/blooms visible INSIDE the mirror view where a
  wall should occlude them — suspicion set: the eb36c1f hasSky far-bound
  extension interacting with the mirror RTT bake's G-buffer, or the RTT
  bake's cone pass integrating behind its near plane; (b) the greets text
  ("kombat") rendered TWICE — one crisp, one ghosted/offset below — likely
  the half-silvered composite (text + reflection) meeting a second text
  source (base panel texture vs RTT/recursion path; the recursion-composite
  interaction is a known open item from MIRROR_RECURSION_PLAN slice 3).
  User's exact launch flags (2026-07-30): `FDS_POM_CONE=1 FDS_TEXTURE_FILTER=1
  FDS_POM_SPIKE=8 FDS_PARALLAX_STRENGTH=3 ./DEMO --shadows
  --greets-omni-shadows --greets-omni-default-range=30
  --greets-omni-shadow-res=256 --shadow-skip-animated --greets-spots
  --shadow-dynamic --shadow-lightmap-planar --shadow-lightmap-res=64
  --shadow-lightmap --greets-mirror --mirror-rtt --greets-mirror
  --mirror-rtt-density=1024 --cone-strength=5 --bloom --disco-bloom=0
  --shard-deferred --greets-shard-fall-speed=1 --greets-shard-randomness=0.8
  --hdr-linear --greets-shard-res=64 --bloom-intensity=1.5 --hdr-refl-gain=4
  --cone-fine-tiles --anamorphic --anamorphic_intensity=1.5
  --anamorphic_vert=0 --anamorphic_decay=0.3 --anamorphic_passes=2
  --lens_ghosts --lens_ghost_intensity=0.05 --lens_ghost_count=0
  --lens_ghost_dispersal=0.01 --lens_ghost_halo=0.01 --chromatic
  --chromatic_amount=3 --vignette --vignette_strength=1 --dof --dof_range=20
  --dof_max=4 --greets-stone-tex --ssao-downscale=2 --ssao-gtao
  --ao_map_strength=1 --parallax_strength=0.1 --parallax --nmap_16bit --hdr
  --ssao --shadow_bake_time --aa --pbr --shadow_cube_face_cull
  --deferred-quarter --ssao_temporal --parallax --parallax_pom_lod
  --glass-refract=1 --glass-test --xpar-peel-passes=4 --cone-turbulence=3.5
  --cone-swirl=0.7 --env-brdf-analytic --sh-ambient --diffuse_energy
  --pbr_multiscatter` — note NO --mirror-recurse-depth (order-1/2 RTT path,
  not the recursion), and --deferred-quarter + --hdr are in play (the known
  wave-2/HDR checkerboard interaction family). **Does NOT repro on bare
  ./DEMO** (user-confirmed) — flag-gated; first bisect candidates when
  picked up: --shard-deferred, --hdr/--hdr-linear/--hdr-refl-gain=4,
  --deferred-quarter, --cone-fine-tiles. Parked deliberately
  ("finish the other threads first").

- **Greets render nondeterminism — CLOSED (2026-08-05, det-hunt rounds 1–3).
  TWO root causes, both proven, both fixed.** The old "~1-in-12 flip / subtle
  pano slivers / deterministic with bakes off" description was wrong on every
  count. Harness: **`tools/flip_rate.sh`** — N sequential runs of a scene's
  gate recipe, distinct-hash histogram, flip rate vs the modal hash, Wilson
  95 % CI, and a zero-event upper bound. Use it; a 3-run arm proves nothing at
  p≈0.85 (that is how rounds 1–2 chased a bake/race that never existed).
  - **FIXED (proven, this commit): `GreetsGenerator::Init()` read
    uninitialized heap as the greets code-screen SMEAR SEED.** `OldBuf` /
    `ScaledBuf` / `CodeBuf` were `_aligned_malloc`'d and never zeroed, and
    `OldBuf` is the feedback source that `Render()` resamples into the screen
    texture (`Txtr->Mipmap[0] == OutBuf`) every frame. Causal chain measured
    per pixel, not inferred: at px (1113,376) / material `screen2` every term
    matched across runs (matID, zEnc, refracted background, blend alpha,
    tile-light count) EXCEPT the sampled texel — `9bd0204f` vs `5ecf175c`;
    after the memset it is stable. Stage trace: the divergence entered at
    `TBR_Render` round 2 phase B1, with `beginframe`/`lighting`/`ssao`/
    `hdr-activate`/`pre-tbr` all byte-identical.
  - **Measured effect of the fix (N=48 per arm, same box, same load):** flip
    RATE essentially unchanged — pre 40/48 = 0.833 [0.704, 0.913], post 43/48
    = 0.896 [0.778, 0.955]. What moved is the SIZE of the divergence, over 6
    run-pairs each: differing pixels median **18.2 % → 14.2 %** and max
    per-channel |Δ| **251 → 95**. So the whole-object black-vs-lit flips are
    gone; a smaller, low-amplitude residual remains. Landing it anyway: it is
    a proven read of uninitialized memory into rendered output.
  - **RESIDUAL — CLOSED, ROUND 3 (2026-08-05). ROOT CAUSE: the opaque
    deferred kernel read 8-bit AO maps as `dword`.**
    `Material::AoMap` comes out of the importer as SINGLE-CHANNEL 8-BIT
    (`loadRoleMapCached` → `MakeHeight8`, same as height / roughness /
    metallic). `DeferredSurfaceKernel.cpp`'s ambient block fetched it as
    `((const dword*)aoTex->Mipmap[miplevel])[swizzledUV]`, so every AO sample
    landed at byte offset `4 × swizzledUV` inside a 1-byte-per-texel
    allocation, and it never bound-checked `miplevel < numMipmaps`. Every
    sibling fetch — roughness (:1125), metallic (:2576), the whole transparent
    kernel (:3156) — already read BYTES and checked the mip bound. This one
    site did not.
    **MEASURED per pixel, not inferred** (32² probe face 0, px (27,19),
    matID 11): `aoBPP = 8`, mip 0 = 1024² = 1,048,576 bytes,
    `swizzledUV = 995355` → dword read at byte **3,981,420..3,981,423**, i.e.
    **3.8 MB past the end of the allocation**. Across four runs everything
    else in the per-pixel record was byte-identical (matID, pmid, zEnc, x/y/z,
    normal, mip, swizzledUV, per-light `intensity`/`k`/reject-stage for every
    light in the tile) — only `aoRaw` moved: 0.489 / 0.615 / 0 / 0.051.
    THE CHAIN: `ao = 1 - ao_map_strength(2.0) × Mat->AoStrength × (1 - aoRaw)`
    → `aoRaw = 0` gives **ao = -2.22**, so the ambient seed went NEGATIVE
    (32 → -71.04) and `if (lB < 0) lB = 0` clamped it to zero. That is exactly
    round 2's "diffuse `lB/lG/lR` flips between 0 and a full value while
    SPECULAR is byte-identical" — AO multiplies the ambient (diffuse) term and
    never touches specular, which is why every light-loop hypothesis missed.
    It also explains `lB == lG == lR` at the flipping pixels: greets' ambient
    is grey (32/32/32), so the ambient seed is achromatic by construction.
    Round 3's hypothesis (a) — a stale tail lane in the 8-wide light batch —
    is **DEAD and should not be re-tried**: `zeroTileLightPadding`
    (DeferredLightLists.cpp) explicitly zeroes count..paddedCount and stamps
    `mirrorId = 0xffffffff` so padded lanes can never pass the mask, and the
    per-light dump showed every lane's `intensity`/`k`/stage identical at the
    diverging pixels.
    **THE DISCRIMINATOR THAT CRACKED IT was one run of `--no-ao_map`**: 4/4
    byte-identical frames and 0/1024 diverging probe pixels on all six faces,
    before any code was written. Cheapest-discriminator-first, again.
    **FIX (this commit):** read the mip as `byte*`, branch on `BPP == 8`, and
    bound-check `miplevel < numMipmaps` (mirrors the transparent kernel). The
    32-bit branch stays for the `ao_from_diffuse` dev fallback.
    **POST-FIX [M]:** 0 flips in **128 sequential runs**, one hash
    `f5778c7b78a4d70655291363e4119c66` (95 % UB 0.023); 0/16 with `--env_refl`;
    0/16 under `--vanilla`. Probe faces 0/1024 differing over 6 runs.
    **LOOK CHANGE [M]:** greets now shades with the real AO map — 26.3 % of
    the gate frame moves, mean |Δ| 12.4, max 98. Four materials carry separate
    AO maps (`momy-1`, `amudim`, `stairs`, `rooms`). Wants the user's eye.
  - Gates after both fixes: city `37e62845`, fountain `51fff7cd` byte-exact;
    mirrortest/conetest/halotest all PASS. Greets-only effect (no other scene
    ships a separate 8-bit AoMap).
- ~~Env-bake content varies run-to-run~~ **RESOLVED 2026-08-05 by the AO fix**:
  the env panorama bakes render through the same opaque deferred kernel, so
  they inherited the same out-of-bounds AO read. Measured after the fix: the
  greets gate recipe with `--env_refl` ON is **0 flips in 16 runs**
  (`33c73ac43520a8ff5be262a99fc61f98`). Re-measure with `tools/flip_rate.sh`
  if it ever looks unstable again.
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

- **CHASE WATER DARK BAND — DONE (604fd43).** Ported soa-vertex 9902349; chase
  water now bright/uniform, no band (t1600 verified); chase pins re-baselined
  (table above); city/fountain byte-identical; render_gate PASS; wasm links.
  Original note kept for context:
  The "lower missing water layer" (horizontal seam, dark band below)
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
- **CHASE SPOTS realign — NEEDS REDO** (user "not seeing the spotlights").
  CONFIRMED: L2.3 canyon spots don't visibly light the mountains with the new
  trail-follow camera (verified at t=1200 — moonlit grey, no warm/cool). A
  realign agent (aa4f40da) DIED at the session limit mid-work; its uncommitted
  variant-a checkpoint (surface-wash) did NOT make them visible (still grey) and
  was DISCARDED (reverted to HEAD). Redo needs: re-aim at the mountains the new
  camera frames (t≈1100-1300), BOOST intensity/contrast so warm/cool reads
  against the moon, and try visible VolumetricLight beams (bit-2048; L2.3 dropped
  them as near-invisible — tighter cones + higher gain, esp. under cinematic
  fog). Deliver beams-vs-no-beams A/B. Chase-only (CHASE.LWS + regen).
- **CHASE COMBAT — LANDED, all default-OFF, gated, deterministic, inert**
  (blasters agent a3471e22, resumed repeatedly): 22963db denser barrage
  t≈340-1700 · ff821a2 B2 impact-spark particles · 1e55078 C1 `chase_cam_fx`
  camera shake + FOV punch on hits · ecc3359 chase-scale bolt-light reach
  (blaster_light_range 90, intensity 260, via setDefault in createChaseScene) ·
  2d33373 B2 near-miss water-splash columns (a `water` aim mode in the fire
  table — tracers punch the sea, vertical spray). Combat CODE side is now
  feature-complete.
  All pure-t (snapshot-safe), OFF byte-identical (chase pins unchanged
  9cc80e9e…), city/fountain unmoved. Flags: chase_blasters, chase_spark_size
  (0.00005), chase_spark_bright (255), chase_cam_fx, chase_cam_shake_gain
  (0.04), chase_cam_fov_kick (5). Awaiting user look-approval before default-on.
  **KEY readability finding (agent):** combat reads subtle because ships are
  small in frame + Ship1's oversized L1 engine flare washes nearby sparks —
  the real levers are the FLARE TAME + closer combat FRAMING, not the bolts.
  Deferred combat follow-ups: water-splash columns (need bolt↔water-plane
  intersect), act-3 return-fire + venting hit, FdsMuzzle keyword, bloom-threshold
  tune for bolt cores. NOTE: this agent kept getting resumed and committing
  autonomously — verify+reconcile each time; consider routing chase work through
  one path.
- **EDITOR STABILITY — DONE** (30c2931 texture dedup by path + 339c65a wasm
  INITIAL_MEMORY 128→512MB). Fixes material-reuse + the unaligned-atomic import
  crash. Native pins byte-identical; user confirms in-browser (make editor →
  import same map on 2 surfaces = 1 decode + [reuse]; roughness import = no crash).
- **EDITOR STABILITY (2 issues, user 2026-07-14; blocked on build = blasters
  agent finishing):**
  (a) `unaligned memory access` crash on texture import (e.g. roughness on
  R_leg1.lwo::hull) — same trap class as the audio crash: a wasm ATOMIC op on
  a misaligned address, under the editor's `-pthread + ALLOW_MEMORY_GROWTH`
  build when an import allocation triggers a HEAP GROW while threadpool workers
  are live (emscripten#17816/#23806, already noted in DEMO/CMakeLists.txt).
  INITIAL_MEMORY=128MB → grows on every import. Mitigations: raise
  INITIAL_MEMORY so typical imports don't grow; and/or run the material apply
  single-threaded so no worker is mid-atomic during a grow.
  (b) **Material/texture NOT reused across objects** — same material re-decoded
  + re-allocated per object instead of sharing one loaded Texture. Wasteful AND
  a direct cause of (a): N copies = N allocs = more grow events = more crashes.
  Fix: a texture dedup cache keyed by source path in the import path (the code
  `new Texture` + re-decodes each ApplyMapFile). Do this FIRST — highest value,
  verifiable, and it cuts the crash rate. Distinct from the metallic-import OOM
  (8d936e0, already fixed).

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
