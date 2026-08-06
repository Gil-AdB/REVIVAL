# SESSION STATE — glass / editor / authoring campaign (updated 2026-07-11)

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
| city | `FDS_CITY_ENV_PIXEL=1 ./DEMO --snapshot=city@t=1961 --out=<dir> --deferred` | `37e62845c4d30eefa321730c5bb7e0b8` |
| greets | `FDS_GREETS_CAM="-0.616376519,2.79000092,-24.4848595,0.164780021,-0.314234257,0.93493551" ./DEMO --snapshot=greets@t=1588 --out=<dir> --deferred --hdr --glass-refract=1 --glass-test --xpar-peel-passes=4 --profiler=0 --no-env_refl` | **RE-PINNED 2026-08-06: `f1297141611c484bac7cc10a8bdcf630`** (3/3 identical runs). Two intended overlay removals moved it in sequence, both pure screen text: `f5778c7b` → `06e1d4d1` (earlier work) → `ae358a6a` (the "Shadow: Depth\|PolyId [F3]" indicator deleted, commit `6b5556d`) → `f1297141` (the always-on centre-pixel `[MAT@…]` material probe moved behind `--mat_probe`, default off, commit `35ec295`; re-running that arm WITH `--mat_probe` reproduces `ae358a6a` byte-exact, which is what proves nothing else moved). Prior pin, for the record: **`f5778c7b78a4d70655291363e4119c66`** — taken over **128 sequential runs, 0 flips** (95 % UB on the flip rate 0.023) after the 8-bit-AO-map fix closed the nondeterminism. This supersedes both `de3e9a5fb3aa39e008ef41b83f2b8d1b` (pre-PBR-defaults) and the "NO VALID PIN" state. Includes the PBR scene defaults AND the user's uncommitted GREETS.FLD / momy textures / Piramid.lwo — a clean checkout hashes differently. Verify with `tools/flip_rate.sh -n 24` if a mismatch appears; a single differing run is now a real regression, not noise. |
| fountain | `./DEMO --snapshot=fountain@t=2500 --out=<dir> --deferred --hdr --glass-refract=1 --glass-test --profiler=0` | `51fff7cd38767d619280afe0498a6f24` |
| chase (default) | `./DEMO --snapshot=chase@t=100,400,800,1200,1600 --out=<dir> --deferred` | per-frame color-PPM md5, re-pinned 2026-07-30 (cone-tile sky-clip fix — see below; 3-run stable, byte==spot_cone_cull=0 ground truth):<br>t100 `f1a567133a3d20e6f3702c5c560a1299` t400 `2adfb0e8f783c01ec0714b9b396c82f0` t800 `0e2a8804f4feef1bf56f6ee9102a11b9` t1200 `7cefbdb062517865ba29ca88965e999f` t1600 `7265d7855bdaae74e39f3c21d4f7e612` (t1600 unchanged) |
| chase (cinematic) | `./DEMO --cinematic --deferred --snapshot=chase@t=800,1600 --out=<dir>` | re-pinned 2026-07-30 (cone-tile sky-clip fix; 3-run stable, byte==cull-off): t800 `28e5a2a78d64ae98a1fcc4b739991be2` t1600 `1cbde501c26d231a4295632dfbebd34b` (t1600 unchanged) |
| gate suite | `./tools/render_gate.sh` (repo root, dummy drivers) | ALL PASS |
| wasm | `make wasm` | links |

Traps:
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
