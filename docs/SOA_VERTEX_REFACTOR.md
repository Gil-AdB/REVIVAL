# SoA Vertex Refactor — Design

Branch: `feature/soa-vertex` (to be created off `feature/static-shadow-lightmaps`)

## MEASURED 2026-08-16s — PHASE 5's END STATE WAS BUILT AS A LADDER AND TIMED. It is **0.6 % of a greets frame, not 1.25 %**, the byte-slope that produced 1.25 % is the wrong model, and the item is **BLOCKED ON SCOPE** — with the scope counted, not estimated.

`16r` re-opened Phase 5 at **1.25 % of frame** by extrapolating a slope
(0.0086 ms per byte of `sizeof(Vertex)`, calibrated by *inflating* the struct
140 → 192 with `-DFDS_VERTEX_PAD_BYTES`) down to the 140 → 68 end state. That
extrapolation was never tested, because nothing in the tree can shrink `Vertex`
without the refactor itself. **So the loop was rebuilt instead of the struct.**

`--xfrm_ablate` gains a pricing ladder (census build only, bits 256/512/1024/
2048 for the shadow pass, 4096/8192/16384 for the main view): **replicas of the
per-vertex loop that differ ONLY in where the read and the write land.** Same
three `fmla.4s`, same `fdiv`, same `fmul` pair — verified in the disassembly,
and the read source is unswitched OUT of the loop (`tbz w10,#0xa` above the
back-edge) with both strides advanced per iteration (`#0x8c` = the 140-byte
Vertex, `#0x20` = the dense 32-byte record). **The arms differ in addressing
only**, which is what makes this a memory measurement and not an arithmetic one.

### THE LADDER — greets t=5743, his acceptance arm, 1920×1080, DynOmnis phase-A `xform`, face loop ablated (`|32`) so the vertex loop is isolated

Per-frame min over 24 frames, one pose per process, order ROTATED, dummy
drivers. The three arms that carry the verdict are **min-of-11 with their noise
floors** (floor = (2nd-min − min)/min); the two half-arms are min-of-5.

| arm | `Pos` read from | outputs written to | DynOmnis wall | floor | DynOmnis core | floor |
|---|---|---|--:|--:|--:|--:|
| **32** — what ships | per-light clone `Vertex` (140 B) | the same record | **0.870** | 0.00 % | **7.130** | 0.70 % |
| **288** — replica CONTROL | same | same | **0.840** | 1.19 % | **7.200** | 0.42 % |
| 544 | clone `Vertex` | a dense **32 B/vert** array | 0.99 **(+16 %)** | 1.01 % | 8.86 | 0.45 % |
| 1056 | **shared `T->Verts`** | clone `Vertex` | 1.13 **(+33 %)** | 1.77 % | 9.86 | 0.20 % |
| 2080 | a compact **shared 12 B/vert** `Pos` array | clone `Vertex` | 0.87 **(0 %)** | 0.00 % | 7.51 | 2.04 % |
| **1568 — Phase 5's END STATE** | **shared `T->Verts`** | **dense 32 B/vert** | **0.600 (−28.6 %)** | 1.67 % | **4.870 (−32.4 %)** | 2.26 % |

`DynMeshes`, same runs: **0.170 → 0.130 wall (−23.5 %)**, floors 0.00 % both.
**Signal-to-floor on the verdict row is 17×.**

**Read the control row first.** The replica reproduces the shipping loop —
0.840 vs 0.870 wall, 7.200 vs 7.130 core, i.e. inside 3.5 % on a 0–1.2 % floor
in opposite directions on the two columns — so the ladder is faithful, and any
branch the replica carries is carried by *every* arm and cancels.

**Then read 544, 1056 and 2080 together, because they are the finding.** Each is
*half* of Phase 5. **Every half, alone, is neutral or worse.** Only the pair
pays. A model in which per-vertex time is a function of `sizeof(Vertex)` cannot
produce that shape: it predicts monotone improvement as the walked record
shrinks, and 544 shrinks the written window out of the record for **+16 %**.

### THE MECHANISM, NAMED — it is the CLONE, not the struct

`--mem_census`, greets t=5743, his arm:

```
[MEM] 219 818 480  209.64 MiB  shadow.scratch/per-light mesh clones (Vertex[])
                                1269 live (shadow map x mesh) clones x VIndex x sizeof(Vertex)=140
```

The shadow bake gives every (light-face × mesh) pair its **own full copy** of the
mesh's `Vertex[]` (`fds::PerTriMeshClone::verts`), because 42 concurrent passes
cannot write one shared `Vertex`. **68 of every 140 bytes in those 209.6 MiB are
`Pos`/`N`/`Tangent`/UV/bary — read-only, byte-identical in all 1 269 copies.**

So the shadow loop today is **one** stream over 209.6 MiB of cold, duplicated
memory. Splitting the write out of it (544) makes **two** streams without
shrinking the first — hence +16 %. Moving the read to the shared array (1056)
makes two 140-byte strides — hence +33 %. Doing both (1568) leaves two streams
of which the read one collapses onto the single `T->Verts` (~15 MiB scene-wide,
warm across all 42 bakes) and the write one is dense. **That is the whole −31 %,
and it is a property of the clone, not of `sizeof(Vertex)`.**

Which is also why the main view — where no clone exists — cannot win it.

### THE MAIN VIEW — ±5 %, i.e. NEUTRAL. It does not regress, and it does not pay either.

`--xfrm_par=0 --xfrm_prof`, VERT bucket, greets t=5743, min over 5 rotated rounds:

| arm | | VERT ms | vs control |
|---|---|--:|--:|
| 0 | what ships | 0.963 | |
| **4096** | replica CONTROL (read + write `T->Verts`) | **0.911** | — |
| 8192 | write a dense 64 B/vert array | 0.959 | **+5.3 %** |
| **16384** | + read a compact 36 B/vert in-array | **0.882** | **−3.2 %** |

The main-view loop reads 36 B and writes 52 B of **one** record — a single
stream — and Phase 5 makes it two with nothing to share. −0.029 ms serial, and
the shipping path is parallel (`--xfrm_par`, 0.48 ms wall for the whole call),
so the realised main-view delta is smaller still. **It is inside the noise in
both directions.**

### PREDICTION vs MEASUREMENT

| | ms/frame | % of a 49.59 ms frame |
|---|--:|--:|
| **PREDICTED** (16r: 72 B × 0.0086 ms/B) | 0.62 | **1.25 %** |
| **MEASURED**, min-of-11 vs the replica control (DynOmnis −0.240, DynMeshes −0.040) | **0.28** | **0.56 %** |
| same, vs the shipping arm (DynOmnis −0.270, DynMeshes −0.040) | 0.31 | 0.63 % |

**Over-predicted by 2.0–2.2×** — outside this campaign's ±20 % bar, and the gap has
a named cause rather than an excuse: the slope was calibrated by *stretching* a
one-stream walk and then extrapolated to a *two-stream* end state, which is a
different access pattern, not a shorter one.

Reproduces at every bake-heavy pose (arm 32 → arm 1568, DynOmnis / DynMeshes
wall ms; frame minimums from 16r):

Cross-pose consistency (min-of-3/5 per pose, arm 32 → arm 1568):

| pose | DynOmnis | DynMeshes | Δ ms/frame | frame min | **% of frame** |
|---|--:|--:|--:|--:|--:|
| t=5743 | 0.85 → 0.59 | 0.17 → 0.13 | −0.30 | 49.59 | **0.61 %** |
| t=2845 | 0.89 → 0.61 | 0.17 → 0.14 | −0.31 | 50.51 | **0.61 %** |
| t=1588 | 1.05 → 0.70 | 0.19 → 0.14 | −0.40 | 60.78 | **0.66 %** |
| t=6097 (no RTT) | 0.87 → 0.63 | 0.17 → 0.13 | −0.28 | 41.26 | **0.68 %** |

city (0.309 % of self samples) and chase (0.190 %) bake no per-frame shadow map
and their half of this is the main-view row above — **nothing**.

### THE ONE HALF THAT IS BIT-EXACT IS ALSO THE ONE WORTH 0 %

Worth recording, because it is the obvious cheap shortcut and it is dead:
swapping the shadow loop's `Pos` read from the clone to the shared `T->Verts`
is **byte-null** — greets t=5743 `818f0336…` and t=1588 `756790e4…`, arm 256 vs
arm 1024, control repeated, identical — so the clone's `Pos` is not stale at
these poses. And it is worth **0 %** (arm 2080, 0.87 vs 0.86). The read was
already free: the line comes in for the *write* regardless. **There is no
bit-exact subset of Phase 5 that pays.** The paying arm needs the outputs out of
the mesh vertex, which is the whole refactor.

> **Latent hazard found on the way, unrelated to perf:** `PerTriMeshClone` is
> **never invalidated** — `cloneOf` sets `initialized = true` on first use and
> nothing in the tree clears `VertexScratch::clones` or resets that flag
> (grepped). A clone's `verts` is therefore a snapshot of `T->Verts` taken at
> that mesh's *first* shadow bake, `Pos` included, while five files
> (`DisplaceRebuild.cpp`, `MeshOps.cpp`, `GreetsDisco.cpp`, `MirrorShatter.cpp`,
> `FOUNTAIN.CPP`) write `Vertex::Pos`. The equality test above says it is not
> stale at the greets poses measured; **nothing enforces it.** Worth its own
> round.
>
> > **RESOLVED 2026-08-16t — and the byte-null note above is now measured, not
> > sampled.** `--clone_stale_census` counted a REUSED clone against the live
> > mesh field by field across a 13-pose greets sweep with the shatter's
> > second clone-backed pass live: **`Pos`, `N` and `Tangent` diverge 0 times in
> > 856 176 679 vertex compares over 630 622 clone reuses**, and the clone's
> > array size never disagrees with the live `VIndex`/`FIndex`. The only fields
> > that do go stale are `BGRA` (rewritten every frame on the LIVE mesh by
> > `Lighting(Scene*)`) and `Face::EU1..EV3` (stamped per pass by the
> > transform's own face loop, all on `__discoBall`) — and refreshing every one
> > of them (`--clone_refresh_inputs=1/2`) is byte-identical across 8 greets
> > configurations. The invariant holds because the only per-frame `Pos` writer
> > targets `Tri_NoShadowCast` meshes, which every shadow pass skips ~245 lines
> > before `cloneOf`. What DID need fixing was structural, not value-based: the
> > transform walks the clone to the LIVE `T->VIndex` while the storage is
> > first-use-sized, so `cloneOf` now rebuilds on size drift. Full account:
> > `docs/OPTIMIZATION_BACKLOG.md` **2026-08-16t**.

### SCOPE — counted, not estimated, and it is why this stops here

Phase 5 deletes the 72 bytes of `out` fields from `Vertex`. Every **mesh-side**
reader of one has to be re-pointed at the out array (clipper-transient readers
do not — `C_Verts` keeps the full type). Counted in this tree, `->FIELD` derefs
of `PX|PY|TPos_AOS|RZ|TN|TTangent|UZ|VZ|EUZ|EVZ|BGRA|LR|LG|LB|LA`:

| file | refs | |
|---|--:|---|
| `FDS/FILLERS/FILLERS.CPP` | 349 | clipper transients — **no migration** |
| `FDS/RENDER/Transform.cpp` | 155 | the vertex + face loops |
| **`DEMO/CITY.CPP`** | **128** | `Reflected_Transform` — an alternative transform pipeline |
| `FDS/FRUSTRUM/FRUSTRUM.CPP` | 99 | clipper internals + the `*A = *F->A` entry |
| **`DEMO/CHASE.CPP`** | **95** | its own face loop + `Reflected_Transform` |
| **`DEMO/FOUNTAIN.CPP`** | **51** | water / particle projection |
| `FDS/FRUSTRUM/FL.CPP` | 33 | not in CMakeLists — dead |
| `DEMO/FillerTest.cpp` | 30 | dev-gated |
| `DEMO/Snapshot.cpp` | 17 | |
| `FDS/Clipper.cpp` | 11 | `_2DClipper` + hand-built quads |
| `FDS/CAMERAS/CAMERAS.CPP` | 10 | plane / portal test |
| `DEMO/Raytracer.cpp` | 9 | |
| `FDS/FILLERS/ShadowMap.cpp` | 8 | |
| `DEMO/GREETS.CPP` | 6 | |
| `RENDER.CPP` / `Lighting.cpp` / `RADIO.CPP` / `TheOtherBarry.h` | 5 each | |
| `SkyCube.cpp` / `RenderInner.cpp` / `IMGGENR.CPP` | 4 each | |
| `FaceBBox.h` | 3 | |
| `VertexFrame.{h,cpp}` / `VertexScratch.h` / `PREPROC.CPP` / `SceneBuilder.cpp` / `FRUSTRUM.H` | 1–2 each | |

**274 of those are in DEMO scene code**, and three of them are whole alternative
transform pipelines that must each learn to write the out array **or the image
breaks silently in a way no gate in this project catches** — which is exactly
the list Phase 6.1/6.2 recorded below (`MakeFacesIndependent`, `BuildSkyCube`,
`tessellateWaterGrid`, `Reflected_Transform`) plus the one it never found
("Greets forward-mode wall fragments still missing after all those fixes —
there's at least one more transform path not yet found").

That is past "Vertex layout + the transform/filler readers". **Not started, and
that is the recommendation: 0.6 % of one scene's frame, greets-only, for a
refactor of the most-shared struct in the engine and three scene pipelines.**

### IF IT IS EVER RE-OPENED, THIS IS THE SHAPE — and it is NOT the doc's Phase 5

The ladder says the prize lives entirely in the **clone**, so the version worth
building is **shadow-only** and does not touch `Vertex`, any filler, or any DEMO
scene file:

1. `PerTriMeshClone` gains a dense out record (PX, PY, Flags, TPos.xyz, RZ — 32 B).
2. The shadow vertex loops write it and read `Pos` from `T->Verts` (proven
   byte-null above). All three loop shapes (Inside / Ahead / Regular) must keep
   their own `Flags` semantics — the ladder's single generic replica does **not**
   preserve them and is a pricing device only.
3. The two clone-backed readers learn to source from it: `Transform_Objects`'
   face loop (`VisibilityFlagsAll`, SortZ, the tile-bbox stamp) and
   `FrustumClipper::Render`'s entry, which overrides the `*A = *F->A` copy.
   `F->frame` and `F->A_idx` are **already plumbed for clone-backed faces**
   (`Transform.cpp:2833`), and `Shadows.cpp` already reads
   `F->frame->TPos_z[A_idx]` — so the machinery exists.

Its ceiling is the 0.6 % above. Its risk is step 3: a runtime branch inside
`Transform_Objects`' face loop is **not byte-null** under `-ffp-contract=fast`
(`docs/VISIBILITY_PLAN.md` §8 — 216 bytes on city from a never-taken `if`), so
it has to be built branch-free the way the `--xfrm_par` block test was.

> **RE-PRICED 2026-08-16t on the post-clone-invalidation tree, and STILL NOT
> BUILT.** The ladder reproduces: arm 32 / 288 / **1568** = 0.870 / 0.860 /
> **0.610** DynOmnis wall, 7.340 / 7.180 / **4.980** core, DynMeshes 0.170 /
> 0.170 / **0.140**, min-of-11 order-rotated — **0.280 ms/frame = 0.56 %**, the
> same number to two decimals. Two coherency requirements step 2 above does NOT
> cover, both found by reading the three loops for this round:
>
> 1. **`Vtx_Spike` (0x0040) shares the `Vertex::Flags` word.** Stamped at scene
>    init (`PREPROC.CPP:213-221`), read by `RENDER.CPP:1569` and
>    `CAMERAS.CPP:344`, and it survives the transform only because the mask is
>    `~Vtx_Visible` (0x003F). A dense record with a fresh `Flags` drops it.
> 2. **The `Ahead` loop does not always write `PX`/`PY`/`RZ`** — a vertex behind
>    `nearZ` gets only `Vtx_VisNear` and keeps the PREVIOUS pass's projection.
>    So the dense array must be per-clone, persistent, and SEEDED from the clone
>    `Vertex` at first use; zero-initialising it changes near-plane pixels.
>
> Both break the image silently, for 0.56 % of ONE scene's frame — city, chase
> and fountain run **zero** clone-backed passes (measured, `reuses=0`).

---

## AMENDED 2026-08-16r — PHASE 5's CEILING IS **1.25 % of frame**, NOT 0.24–0.31 %. The section below measured the MAIN VIEW ONLY.

> **SUPERSEDED 2026-08-16s by the section above: the 1.25 % is an
> extrapolation of a slope measured in the wrong direction, and the end state
> it prices measures 0.56-0.63 %. The 16r decomposition it rests on — 42 of 45 calls
> are the shadow bake — stands and is what pointed the ladder at the clone.**

`docs/PERF_STATE.md` §00g decomposed `Transform_Objects` by invocation source for
the first time. At greets t=5743 on the user's acceptance arm the symbol runs
**45× a frame and 42 of those calls are the shadow bake** — 596 446 of the
frame's 904 111 transformed vertices, **81 % of the core time**. The section
below prices Phase 5 off `--xfrm_prof`'s buckets, and those are **main-view only
by construction** (the gate is `_mainView && xresOverride < 0`), so it costed the
19 % and called the row closed.

Re-measured where the vertices actually are, with the `-DFDS_VERTEX_PAD_BYTES`
hook §3 below already shipped — this time on the SHADOW pass's phase-A wall time:

| `sizeof(Vertex)` | 140 (shipping) | 144 | 160 | **192 (64-byte aligned)** |
|---|--:|--:|--:|--:|
| `DynOmnis` xform ms/f | **1.21** | 1.23 | 1.33 | **1.58** |
| xform core-ms/f | 10.37 | 10.83 | 11.59 | 12.37 |

**Monotone in SIZE, and the 64-ALIGNED arm is the worst of the four** — which
also kills the obvious rival theory (the shadow pass touches `[0,28)` written +
`[52,64)` read, a 64-byte hot window at a 140-byte stride that straddles two
lines ~98 % of the time; aligning it made things worse, not better). §3's
conclusion — *`sizeof(Vertex)` is the ONLY variable this loop responds to* —
reproduces exactly, in a pass it had never been run in.

Slope **0.0086 ms per byte** across both bakes. Phase 5's 140 → 68 is therefore
**72 × 0.0086 = 0.62 ms/frame = 1.25 % of a 49.5 ms greets frame**, four to five
times the ceiling the section below closed it on.

**The VERDICT below may still stand** — 11 files, two alternative transform
pipelines, and a requirement to find every transform writer or the image breaks
silently. **The NUMBER does not.** Anyone re-opening Phase 5 should argue against
1.25 %, not 0.3 %. Also note what §00g refuted on the way: the ALU is not the
limit (the whole projection block is 0.01–0.03 ms of 1.22, and
`--shadow_cube_vert_cull`, which removes the 3-FMA matmul for out-of-pyramid
vertices, moves core time 10.67 → 10.77), and neither is scheduling (effPar
8.4–8.7 on an 8 P-core + 4 E-core box).

---

## MEASURED 2026-08-09 — PHASE 5 IS CLOSED. Re-measured on the current tree, its ceiling is 0.24–0.31 % of frame, and the effort went to a struct 15× bigger.

Phase 5 (`sizeof(Vertex)` 140 → 68 for mesh storage, or §6's interleaved 64-byte
output array) was re-opened as *"hot-struct bloat cost us once, is there more?"*.
It was re-measured on today's tree rather than re-argued. **The answer is no, and
the same question asked of the DOMINANT stage found a struct worth 15× more.**

### The front end today, per scene (`--xfrm_prof`, `--xfrm_par=0` for the buckets)

Serial buckets, so this is the *upper* bound — the shipping path is parallel
(`--xfrm_par`, default ON) and cheaper. 1920×1080, dummy drivers.

| scene / pose | TOTAL | VERT | FACE | verts | frame ms | VERT as % of frame |
|---|--:|--:|--:|--:|--:|--:|
| greets t=5743 | 0.639 (par: 0.433) | **0.345** | 0.254 | 49 401 | 43.5–57.4 | **0.60–0.79 %** |
| city t=1961 (p50; 2 calls/frame) | 1.178 | **0.611** | 0.539 | 69 287 | 92.6 | **0.66 %** |
| chase t=400 / 800 / 1200 / 1600 | 0.624 / 0.538 / 0.640 / 0.513 | **0.114–0.158** | 0.354–0.467 | 23 205–30 548 | — | — |

**`VERT` is the only bucket Phase 5 can touch.** Its own §3 slope puts the best
case at ~40 % of VERT (traffic 284 → 164 B/vert), i.e. **0.14 ms at greets,
0.24 ms at city, 0.05 ms at chase** — **0.24–0.31 % of frame**, before the fact
that the shipping path is parallel and greets is LPT-bound on one mirror clone
(55 % of its verts *and* faces in a single mesh), which caps the realised share
below that.

Against: 11 files, two alternative transform pipelines (`Reflected_Transform` in
CITY/CHASE, FOUNTAIN's water/particle projection), the clipper entry, and a
requirement to find *every* transform writer or the image breaks silently — the
Phase 6.1/6.2 history is precisely a list of those bugs (`MakeFacesIndependent`,
`BuildSkyCube`, `tessellateWaterGrid`, `Reflected_Transform`).

**chase reconfirms the doc's own note:** it is FACE-dominated, not VERT-dominated
(FACE is 2.9–3.3× VERT there). Phase 5 is the wrong lever for chase by shape, not
just by magnitude.

**Not attempted, and that is the recommendation.** Do it for the one-writer
contract if that is ever wanted; never for perf.

### Where the same question DID pay: the cube-shadow tap, 15× bigger

The identical mechanism — *how many cache lines does one access touch* — asked of
the dominant stage instead of the front end. At greets the deferred lighting wave
is 27.8 ms of a 43.5 ms frame, and **the cube-shadow tap alone is 10.28 ms of it**
(`--prof_no_cube_tap`, min-of-3: `lighting-w1` 30.518 → 20.238). A PolyId tap
gathered 32 bytes from **four separate `std::vector<uint16_t>` planes** that sat
512 KB apart, over two PCF rows = **up to 8 cache lines per tap**. Pair-interleaving
them into two `std::vector<uint32_t>` planes (now the SOURCE OF TRUTH, shipped ON)
halves that. See docs/OPTIMIZATION_BACKLOG.md item 3 for the measured table.

## MEASURED 2026-08-06 (c) — the main-view transform is PARALLEL now (`--xfrm_par`, default ON). Two arms, two DIFFERENT limits, both measured.

Item 2 of §5 below ("parallelise the MAIN-VIEW `Transform_Objects`… this has
never been costed") is built, measured, byte-gated and on by default. It should
have been done before any of the layout work: it is orthogonal to the layout and
it collects **−1.10 ms of a 1.55 ms phase** in the displaced arm.

### The ceiling, costed BEFORE building anything — and it is not `total / 12`

A mesh is the indivisible unit of this loop: its face loop reads its own
transformed vertices, so a mesh cannot be split across workers without a
barrier. The parallel critical path is therefore the **largest single mesh**.
That falls straight out of the existing `--xfrm_pass_mesh_prof` census, no new
code — and greets' geometry is dominated by one mirror clone:

| arm (greets, 1920×1080) | main-view verts | largest mesh | share of verts | its faces / tested | share |
|---|--:|--:|--:|--:|--:|
| `--greets_displace` t=5780 | 188 941 | `__mirrorClone_teleporter` 26 861 | 14.2 % | 9 198 / 63 290 | 14.5 % |
| shipping t=5743 | 49 401 | `__mirrorClone_teleporter` 27 370 | **55.4 %** | 9 198 / 16 607 | **55.4 %** |

Against the serial per-bucket split that gives an LPT bound of
`max(largest-mesh cost, total / nWorkers)`:

| arm | serial TOTAL | VERT | FACE | LPT bound | measured `WORK` |
|---|--:|--:|--:|--:|--:|
| displaced | 1.546 | 0.883 | 0.622 | 0.142·0.98 + 0.145·0.71 = **0.240** | 0.411 |
| shipping | 0.423 | 0.227 | 0.172 | 0.554·(0.227 + 0.172) = **0.221** | 0.246 |

**Read those two rows carefully — they are limited by different things, and both
are at their own mechanism:**

* **Shipping is at the per-mesh LPT bound** (0.246 measured vs 0.221 predicted,
  +11 %). One mirror clone is 55 % of the arm's main-view verts *and* 55 % of
  its faces, so eleven workers finish and wait on one. Nothing short of
  splitting that mesh moves this arm further.
* **Displaced is at an AGGREGATE-BANDWIDTH bound, not the LPT bound** (0.411
  measured against a 0.240 LPT bound). 188 941 verts × ~284 B = 53.7 MB of
  streamed traffic in 0.411 ms is **~131 GB/s** — about **2×** the 64 GB/s
  single-core ceiling §4 measures, not 12×. Confirmed by elimination: raising
  the block count from 12 to 24/26/52/104 (finer work-stealing, i.e. better
  balance) moves `WORK` 0.49 → 0.41 and then flattens, so the residual is not
  imbalance.

**That is the important architectural consequence of this whole exercise:** the
loop was bandwidth-bound on one core, and after parallelising it is
bandwidth-bound on the socket. So the ONLY remaining lever that can move the
displaced arm is **bytes per vertex** — lever 1 in §5 / §6 below (the
interleaved 64-byte output array, ~284 → ~164 B/vert). More threads cannot;
more SIMD never could.

### Measured

`--xfrm_par=N`: `-1` (default) = 2 blocks per worker, `0` = the old serial path,
`N>0` = exactly N blocks. Per-frame **min over 24 frames**, min-of-arm over 3
interleaved reps, 1920×1080, `SDL_VIDEODRIVER=dummy`, box shared with 3 other
agents (per-run 1-min load 14–23).

| arm | serial (`par=0`) | default (`par=-1`, 26 blocks) | delta |
|---|--:|--:|--:|
| `--greets_displace` t=5780 | **1.546** | **0.449** (PLAN 0.006 / WORK 0.411 / COMPACT 0.026 / EPI 0.004) | **−1.10 ms, 3.4×** |
| shipping t=5743 | **0.423** | **0.261** (PLAN 0.005 / WORK 0.246 / COMPACT 0.006 / EPI 0.003) | **−0.16 ms, 1.6×** |

**The load did not bias this**, which is worth stating because the box was
shared throughout. An orphaned process of the harness's own making (an
interactive `--scene-mirrortest` launched without `FDS_MIRRORTEST_MULTI_DUMP`
under a dummy driver, so nothing could ever press ESC) sat at ~3.5 cores
through every row above. Re-measured after killing it, on a genuinely quiet
box (1-min load 5.5–11), min-of-arm over 2 interleaved reps: displaced
**1.490 → 0.461** (3.2×), shipping **0.421 → 0.269** (1.6×). The parallel arm
is unchanged to within run-to-run spread — if anything marginally slower on the
quiet box — so the per-frame **min over 24** absorbed the background load, as
intended. Prefer these figures; the difference from the table above is noise.

Block-count sweep (same binary, min-of-arm over 2 reps, `WORK` only):

| blocks | 1 | 12 | 24 | 26 | 52 | 104 |
|---|--:|--:|--:|--:|--:|--:|
| displaced | 1.630 | 0.491 | 0.433 | 0.411 | 0.423 | 0.430 |
| shipping | 0.425 | 0.225 | 0.246 | 0.246 | 0.252 | 0.238 |

Three things to read off these:

1. **The `blocks=1` control is the honest overhead number.** Its `WORK` equals
   the serial `TOTAL` to within noise in both arms, so the entire machinery
   (mesh-sequence counter, segment reservation, compaction, epilogue re-entry)
   costs `PLAN + COMPACT + EPI` ≈ **10–35 µs**. The dispatch round-trip that
   Threads.h's "~12 µs per enqueue" comment implies did NOT materialise — the
   enqueues and the drain live inside `WORK`, and `WORK` lands on the predicted
   bound.
2. **`COMPACT` is 5–26 µs**: the price of determinism is ~2–6 % of the phase.
   36 147 × 16 B of `memmove`.
3. **Block count must exceed worker count.** The cost model can only see
   `VIndex`/`FIndex`; it cannot see which meshes the frustum + occlusion culls
   will discard, and most are discarded (greets: 471 TriMesh objects on the
   object list, **108** reach a vertex loop). One block per worker with a static
   assignment left ~15 % on the table (`WORK` 0.508 vs 0.411).

### DETERMINISM: reserved segments, not a bump allocator

The one thing that would make this useless is an FList whose ORDER depends on
which worker finished first — a different image run to run, which destroys every
byte gate in the project and is not a trade anyone can evaluate by eye. So:

* Each block appends into a segment of the shared FList **reserved in MESH
  ORDER** — the prefix sum of per-mesh `FIndex`, an exact upper bound on what a
  block can push (a mesh cannot emit more FList entries than it has faces).
* `COMPACT` then slides the runs down **in block order**. Destination never
  exceeds source and blocks are walked in increasing order, so it is a safe
  in-place compaction and the surviving order is exactly the serial insertion
  order.
* **Execution order is free; output position is pinned.** That separation is why
  blocks can be work-stolen off a shared cursor (and why the block count can be
  tuned for balance) without touching determinism.
* Cheapest way to see the property hold: **the block COUNT does not move the
  image.** `par=0 / 5 / 7 / 8 / 12 / 24 / 52 / 104` all hash the same.

Shared mutable state, enumerated and handled:

| thing | how |
|---|---|
| the FList append cursor (`Ins++`) | per-block reserved segment (above) |
| the tile-bbox stamp (`--tile_bbox_cull`) | written into the block's own `FListEntry` |
| the radix sort | consumes the compacted, mesh-ordered list — unchanged |
| per-vertex / per-face writes | into the mesh's own `Vertex`/`Face` arrays; blocks are disjoint mesh sets |
| `T->frame` lazy `new VertexFrame()` | per-mesh, and a mesh belongs to exactly one block |
| `T->BSphereScreenPos` | same — one owner per mesh |
| `g_inShadowPass`, `g_inDynamicShadowBake`, `g_currentShadowOmni`, `g_currentShadowMap`, `g_offAxisFrustumCull` | all `thread_local`; the worker task sets the main-view values explicitly, so a pool thread that last ran a shadow or mirror-shard task cannot leak state in |
| `g_chunkVisStats`, `g_xprof`, the `FDS_VIS_CENSUS` accumulators | plain counters — suppressed inside blocks; the driver has its own `[XFRM-PAR]` dump under `--xfrm_prof` instead |
| the omni + particle epilogue | mutates `Omni::V` / `Particle::V` in place and is not per-mesh work — runs exactly once, after COMPACT, in its own re-entry |
| two Objects sharing one TriMesh | would be a genuine race (the serial path merely gets an order-dependent answer). Audited once per `Scene` pointer; the driver shouts on stderr and falls back to 1 block. No scene in the demo trips it. |
| `FInterpolator` | not touched by `Transform_Objects` (clipper-side) |

The mesh-loop block test is an **unconditional counter + compare** (serial takes
`lo=0, hi=INT_MAX`), deliberately not a flag-predicated branch — a never-taken
`if (flag)` inside this `-ffp-contract=fast` function is not byte-null
(docs/VISIBILITY_PLAN.md 8a: 216 bytes on city, max |Δ| 44). The driver's own
timers live in the cold driver function for the same reason.

**Gates**, run twice (once on the static-assignment build, once on the shipped
work-stealing default-ON build); every OFF-vs-ON pair identical, dummy drivers,
1-min load 11–30:

* render_gate **3/3 PASS in BOTH arms** — mirrortest `4ac809e5`, conetest
  `b41894f9`, halotest `166fa25a`.
* city **`37e62845` PIN EXACT** off and on (env cache warmed once, held fixed
  across arms; on-arm run twice, same hash).
* fountain **`51fff7cd` PIN EXACT** off and on.
* greets t=1588 off == on (`f1297141`). This differs from the SESSION_STATE pin
  `f5778c7b` — it moved *identically in both arms*, which is the concurrent
  greets overlay work, not this change.
* chase 5-pose: t100/t400/t800/t1200 EXACT to their pins; t1600 differs from the
  pin in BOTH arms (`c8c93b88` vs `7265d785`) — the documented pre-existing
  effect of the uncommitted CHASE.FLD/.lwo. chase cinematic: same shape.
* greets t=5780 `--greets_displace`: one hash across every block count tried.
* **Nondeterminism gate: 24 sequential runs of the greets pin recipe with the
  parallel path on → 1 distinct hash, 0 flips** (`tools/flip_rate.sh -n 24`;
  95 % upper bound on the flip rate 0.117), and that hash equals the flag-OFF
  hash.

### What is LEFT, and what it would cost

* **Displaced arm: nothing more from threads.** It is at ~131 GB/s aggregate.
  The lever is bytes/vertex — §6's interleaved 64-byte output array
  (~284 → ~164 B/vert) should now move it roughly proportionally, and it is the
  only thing that can.
* **Shipping arm: the lever is the dominant mesh.** 55 % of its work is one
  mirror clone. Either split a mesh's vertex loop across workers (needs a
  vertex-phase/face-phase barrier, because a face reads vertices from ranges
  other workers own — a real refactor of the function), or shrink the clone: the
  mirror-clone spatial split already tracked in docs/VISIBILITY_PLAN.md §8e
  would cut the critical path here AND improve the serial arm.

---

## MEASURED 2026-08-06 (b) — the mechanism, nailed with a controlled experiment: `sizeof(Vertex)` is the ONLY variable this loop responds to. Read this before Phase 5.

Everything below this section reasons about the per-vertex loop being
"cache-line-bound" from *ablations*, which are confounded (they change the
arithmetic and the bytes together). This section changes one variable at a time
and gets a clean answer. Two of this doc's own predictions are refuted by it.

### Regime + instrument (all numbers in this section)

`./DEMO --bench=scene@scene=greets,t=5780,iters=24 --deferred --greets_displace
--xfrm_prof=24`, 1920×1080, `SDL_VIDEODRIVER=dummy`, per-frame **MIN** over 24
frames, box shared with other agents (1-min load quoted per run, 11–17
throughout). Main-view `Transform_Objects` sees **108 meshes / 253 280 verts
(inside 70 607 / ahead 166 647 / regular 16 026) / 63 290 faces tested / 36 147
pushed**.

> **The 958 204-vert figure this doc is built on no longer exists.** `9d`'s
> faceless-mesh cull plus the GREETS.CPP work on `fog-wt` took the displaced
> main-view count to 253 280. `--greets_displace` is still the largest available
> regime and is what everything here is measured in; the *ratios* below are the
> transferable part, not the absolute ms.

### 1. The dead `UZ`/`VZ` stores — deleted, and NEUTRAL (`fdc7a07`)

`FrustumClipper::Render` overwrites `A/B/C->UZ/VZ` unconditionally at entry, so
the transform's stores could never be read (full audit in the commit message and
in the note above the vertex loops in `Transform.cpp`). Removing 10 sites / 19
lines drops 2 loads, 2 muls and 2 stores per vertex per pass:

| arm | VERT | FACE | TOTAL |
|---|--:|--:|--:|
| base | 1.186 / 1.176 | 0.612 / 0.592 | 1.834 / 1.805 |
| deleted | 1.160 / 1.181 | 0.584 / 0.592 | 1.785 / 1.807 |

**Neutral** (|ΔVERT| ≤ 0.026 ms against a ~0.05 ms spread). Landed anyway: it is
byte-null at every gate, and it is what makes the untouched tail contiguous.

### 2. Field REORDER — +2.6 % on VERT, and it REFUTES the face-loop theory

`Vertex` regrouped (see the block comment in `FDS/Base/Vertex.h`) so that
**PX, PY, Flags, TPos_AOS.z sit in 24 contiguous bytes at offset 0** and
everything the per-vertex loop touches fits in **bytes 0..87**, with the
transform-untouched 52 bytes (BGRA, UZ/VZ, EUZ/EVZ, U/V, EU/EV, i, OrigBary,
ShellH) as a contiguous tail at 88..139. `FInterpolator` retuned to the new runs
(2-wide PX/PY, UZ/VZ, EUZ/EVZ, TTangent.y/z; 4-wide TN.x..TTangent.x) — same
`a + t*(b-a)` per lane, so bit-identical.

| arm | VERT min | FACE min | TOTAL min |
|---|--:|--:|--:|
| base | 1.156 / 1.193 | 0.583 / 0.604 | 1.775 / 1.846 |
| reordered | 1.135 / 1.155 | 0.588 / 0.606 | 1.763 / 1.835 |

**VERT −2.6 % (min) / −2.9 % (p50), same sign in both interleaved pairs.
FACE 0.**

### 2b. Secondary arm — the SHIPPING/flat regime gains MORE, not less

Both landed changes together (dead UZ/VZ + reorder) vs the tree before them,
greets **t=5743, `--parallax_pom=128`** (the review-pose shipping recipe, NOT
`--greets_displace`): 83 meshes / **49 447 verts** (in 8 285 / ahead 41 057 /
regular 105) / 16 607 faces tested / 7 916 pushed — the same census
docs/VISIBILITY_PLAN.md §9 reports. Per-frame min over 24, interleaved:

| arm | VERT | FACE | TOTAL |
|---|--:|--:|--:|
| base | 0.241 / 0.232 | 0.171 / 0.167 | 0.441 / 0.417 |
| HEAD | 0.227 / 0.225 | 0.165 / 0.166 | 0.411 / 0.412 |

**VERT −4.4 %, FACE −2.1 %, TOTAL −4.1 %**, same sign in both pairs — and the
second HEAD run was taken at load 29.98 against base's 12.18 and still won.

Note the inversion versus the displaced arm (−2.6 % VERT there, −4.4 % here):
at 49 447 verts over 83 meshes a mesh is ~596 verts ≈ 83 KB, which **fits L1**,
so this regime is instruction-bound rather than DRAM-bound and the removed
loads/muls/stores actually pay. At 253 280 verts over 108 meshes a mesh is
~2 345 verts ≈ 328 KB and the loop is at the bandwidth ceiling in §4, where
instruction count is free and only bytes matter. Two different regimes, two
different binding constraints — which is exactly why a single "the transform is
X % of frame" verdict has been misleading this campaign in both directions.

That FACE zero is the finding. The reorder cuts the *predicted* cache lines per
random 3-vertex deref from ~2.88 to ~1.56 (−46 %) — the exact win the
"§per-face visibility test = 73 % of FACE, read it from SoA instead" item in
docs/OPTIMIZATION_BACKLOG.md is priced on — **and FACE did not move.** The three
`F->A/B/C` derefs are already cache-resident (a face's vertex indices are
spatially coherent, so the lines are still hot from the previous face). So:

* **The per-face cost is NOT vertex line traffic.** Migrating `Flags`/`PX`/`PY`
  to 4-byte-stride SoA arrays will not buy the FACE bucket back, and it does not
  need the `A/B/C_idx`-trust invariant that item was blocked on. Repricing:
  whatever the 73 % is, it is the branch chain + the per-face `F->Txtr->Flags`
  chase + the Face stream, not the vertex reads.
* By symmetry it also explains why the reorder gave VERT only 2.6 %: the
  per-vertex walk is **sequential**, and at a 140-byte stride a 52-byte gap can
  never skip a whole 64-byte line. The line-span arithmetic only ever applied to
  random access, and the only random access here turned out to be cached.

### 3. The controlled experiment: `-DFDS_VERTEX_PAD_BYTES=N`

Dead tail padding inflates `sizeof(Vertex)` and changes **not one instruction**
in any loop. Same tree (reordered), same scene, fresh build dir per arm,
`pad=0` run first AND last as drift control:

| `sizeof(Vertex)` | VERT min | ns/vert | vs 140 | FACE min |
|---|--:|--:|--:|--:|
| **140** (control ×2) | **1.118 / 1.123** | 4.41 | — | 0.575 / 0.579 |
| 204 | 1.203 | 4.75 | **+7.6 %** | 0.617 |
| 268 | **2.315** | 9.14 | **+107 %** | 0.724 |

Drift between the two controls is 0.005 ms, so both steps are real. Per-vertex
time is a **steep, super-linear function of the struct's stride**, with a cliff
between 204 and 268 bytes (consistent with the hardware stride prefetcher giving
up past ~256 B — past that every vertex is a demand miss and the loop doubles).

### 4. Bandwidth: the loop is at a SINGLE-CORE streaming ceiling (~62–64 GB/s)

Per vertex the reordered walk pulls the lines covering bytes 0..87 at a 140-byte
stride (≈2.375 lines = 152 B), writes back the dirty lines covering 0..51
(≈1.81 lines = 116 B) and stores 16 B into the SoA arrays ⇒ **~284 B/vert**.
At 4.41 ns/vert that is **64.4 GB/s on ONE core** (main-view `Transform_Objects`
runs on the tick thread — `RENDER.CPP:479/493` and each scene's own call site;
`Shadows.cpp:422` is the only threaded caller).

Two independent cross-checks from this doc's own 2026-08-05 numbers, at 958 204
verts and the pre-reorder layout (touched span 4..123, i.e. effectively every
line: 140 B read + 140 B written back + 16 B SoA):

| loop | ms | bytes/vert | GB/s |
|---|--:|--:|--:|
| VERT (`--xfrm_soa_inline` on) | 4.41 | 296 | **64.3** |
| `VertexFrame_DumpFromAoS` sweep — 4 loads, 4 stores, ZERO arithmetic | 2.40 | 156 | **62.3** |
| VERT, this section, 253 280 verts | 1.118 | 284 | **64.4** |

Three unrelated loops pinned at 62–64 GB/s is a ceiling, not a coincidence. It
is the single explanation for every wash this campaign has recorded: Vec8f
across 8 verts, the reciprocal estimate, the single-precision divide, dropping
2 of 3 mat-vecs, and now the field reorder. **Nothing inside the loop can move a
loop that is waiting on DRAM; only bytes/vertex and more cores can.**

### 5. What follows, ranked

1. **Shrink the mesh-side struct — the interleaved-output design in the
   2026-08-06 (a) section below is CONFIRMED, and its ceiling is bigger than
   that section claims.** Mesh-side inputs 140 → 68 B (clean, no write-back),
   outputs to a separate 64 B/vert array (one full line, write-allocate
   elidable): streamed traffic **~284 → ~132 B/vert**. Against the measured
   slope in §3 that is worth on the order of **2× VERT**, not the −26 % / −45 %
   this doc argues about. The reorder already parked the split line: bytes
   88..139 are exactly the fields that do not belong on the transform's side.
   Cost is unchanged and still the real blocker — it needs `Vertex` to split
   into mesh storage vs the clipper's transient `C_Verts` type (every
   `RasterFunc` takes `Vertex**`), i.e. the "Phase 6.3, 2–3 days" work.
2. ~~**NEW — parallelise the MAIN-VIEW `Transform_Objects`.**~~ **DONE
   2026-08-06 — `--xfrm_par`, default ON. See the 2026-08-06 (c) section at the
   top of this file.** Displaced 1.546 → 0.449 ms (3.4×), shipping 0.423 →
   0.261 (1.6×); byte-identical on every gate, 24/24 one hash. Two corrections
   this delivered to the reasoning below:
   * "the chip's aggregate bandwidth is several times 64 GB/s" — **measured at
     ~131 GB/s for THIS access pattern, i.e. ~2×, not 12×.** The displaced arm
     is now bandwidth-bound at the socket instead of at one core.
   * That makes item 1 (bytes/vertex) the **only** remaining lever on the
     displaced arm, and raises its priority rather than lowering it.
3. **Do NOT spend the migration's budget on the FACE bucket** — §2 measured that
   win as zero.

### 6. Lever 1 without the type split — the version to actually build

The "2–3 days / Phase 6.3" cost estimate below exists because everyone assumed
the traffic cut requires `sizeof(Vertex)` itself to shrink, which forces `Vertex`
to split into mesh-storage vs the clipper's transient type, which forces every
`RasterFunc(Face*, Vertex**, …)` and both clippers to change. **That assumption
is wrong.** The traffic the loop pays is not `sizeof(Vertex)`; it is *the lines
the per-vertex loop touches and dirties*. So:

**Keep `Vertex` exactly as it is (140 B, one type, no filler or clipper
signature changes). Just stop the transform from WRITING into it.**

* Per mesh, add an interleaved output array, one 64-byte record per vertex:
  `TPos×3, TN×3, TTangent×3, PX, PY, RZ, Flags, BGRA, EUZ, EVZ` = 64 B exactly
  (the layout the 2026-08-06 (a) section derived; `UZ`/`VZ` are correctly absent
  — `fdc7a07` proved they are clipper-owned).
* The per-vertex loop then **reads** only `Pos`/`N`/`Tangent` — bytes 52..87 of
  the reordered struct, a 36-byte span ⇒ ~1.56 lines ≈ 100 B — and **writes
  nothing** into the mesh array, so the ~116 B/vert of dirty write-back
  disappears. It writes one aligned 64-byte record instead (a full line ⇒ the
  write-allocate read is elidable).
* **Traffic: ~284 → ~164 B/vert (−42 %)** with `sizeof(Vertex)` untouched.
  Against §3's slope that is the bulk of lever 1's prize, for a fraction of the
  work.
* Follow-on, if it measures: a compact per-mesh *transform input* array
  (`Pos`/`N`/`Tangent`, 36 B/vert, contiguous) drops the read side to ~40–64 B
  and takes traffic to ~104–128 B/vert (−55…−63 %). It duplicates 36 B/vert and
  needs a re-sync wherever `Pos` changes (displacement, tessellation, water), so
  land the cheap half first.

The clipper entry keeps `*A = *F->A` for the INPUT fields (that memcpy is the
access pattern — see "Naming" below) and then overwrites the out fields from the
record. That is precisely the Phase 6.1/6.2 "override" this doc records as
BLOCKED — and **both of its blockers now have clean answers**:

* *"`F->A/B/C_idx` isn't trustworthy on every mesh"* — don't trust it. Store the
  vertex-array base alongside the record array and derive `idx = F->A - base`.
  Exact by construction, no per-mesh invariant, and it fixes the same hazard the
  tile-bbox comment in `Transform.cpp` documents (the conetest quad).
* *"stale frame after `Reflected_Transform`"* — that is now a hard requirement
  rather than a latent trap: with the out fields no longer in the `Vertex`, every
  alternative transform path (CITY/CHASE `Reflected_Transform`, FOUNTAIN's
  water/particle projection) **must** write the record array, and the compiler
  finds them if the field is renamed first (the rename-first technique below).
  `VertexFrame_DumpFromAoS` already exists as the vehicle.

Migration surface is the same 11 files inventoried below, but the *type* stays
put, so no rasterizer, no `FInterpolator`, no `C_Verts`, no `_2DClipper`, and no
hand-built sprite/water quad in DEMO has to change. Hottest new reader is
`RenderInner`'s per-tile `A->Flags & B->Flags & C->Flags` — §2 measured those
three derefs as cache-resident, so the extra indirection there should be cheap,
but measure it rather than assume.

---

## MEASURED 2026-08-06 (a) — the whole geometry front end is now ~1.5–3 % of a greets frame, and Phase 5's real ceiling is HALF what this doc claims. Read this before starting Phase 4/5.

Two things changed since the 2026-08-05 section below, and both cut the prize:

1. **The 7.92 ms premise is retired geometry.** That number is the main-view
   `Transform_Objects` under `--greets_displace` at 958 k verts. Tessellation is
   retired (docs/SESSION_STATE.md) and `799c808`/`9d`'s faceless-mesh cull landed.
2. **`--shadow_prof`'s "xform" bucket was not all transform** — see §"the phase-A
   clear" below.

### The front end, measured in WALL time (1920×1080, greets t=5743, shipping arm)

`./DEMO --bench=scene@scene=greets,t=5743,iters=24 --deferred --xfrm_prof=24
--shadow_prof --shadow_bake_time`, `FDS_SHADOW_PROF_INTERVAL=8`, load 9.4–11.5,
frame mean 78.9–109 ms depending on box load.

| stage | wall ms/frame | instrument |
|---|--:|---|
| MAIN `Transform_Objects` | **0.449–0.468** (min) / 0.485–0.503 (p50) | `--xfrm_prof` |
| — of which SETUP / VERT / SOA / FACE | 0.004 / 0.244–0.257 / 0.001 / 0.176–0.185 | " |
| SHADOW phase A, DynOmnis bake (28 maps) | 0.55–1.44 | `[SHADOW-CLEAR]` census |
| SHADOW phase A, DynMeshes bake (14 maps) | 0.19–0.63 | " |
| OFFSCREEN (env / SH probes) | not re-measured; §9d put it at 1.18 **core**-ms | `--xfrm_pass_prof` |
| **whole front end** | **≈1.2–2.6 ms of a ~79 ms frame = 1.5–3.3 %** | |

Main-view verts are **49 447** (in 8 285 / ahead 41 057 / regular 105), fTested
16 607, fPushed 7 916. The frame profiler independently rows XFRM at **0.5 %**.

**So: the transform phase is no longer a big chunk of the budget.** Anything left
inside it is worth at most a few tenths of a ms. That is the number any further
work here has to be justified against.

### The phase-A "clear" — LANDED, and it is why the shadow xform bucket looked big

`Render_DeferredShadowMaps` cleared each light's `depth`/`polyId` planes with a
serial `std::fill` on the tick thread, *inside* the window `--shadow_prof` reports
as `xform=`. Census build (`-DFDS_SHADOW_CLEAR_CENSUS`), greets t=5743, 12 frames:

| bake | maps | bytes cleared | clear core-ms | phase-A wall |
|---|--:|--:|--:|--:|
| DynMeshes | 14 | 10.50 MB | 0.19–0.40 | 0.19–0.63 |
| DynOmnis | 28 | 2.72 MB | 0.02–0.11 | 0.55–1.44 |

For the DynMeshes bake the clear was **most of the bucket** — in several frames the
summed clear core-time now EXCEEDS the whole phase-A wall, which is only possible
because it is spread across the pool. Fixed by clearing inside `runPhaseAXform`
(the clear is per-map private; nothing reads a map's planes before phase B, which
is behind the `shadowDone` barrier). Byte-null: greets t=1588 `06e1d4d1` (3/3 runs,
HEAD == FIX), all 5 chase poses HEAD == FIX, fountain `51fff7cd` == pin,
render_gate 3/3.

### Consumer inventory of the per-frame AoS fields (the Phase 4/5 surface)

`sizeof(Vertex)` **= 140** (verified, `pack(1)`), with these offsets:

```
0   BGRA/LB,LG,LR,LA   4      out
4   PX                 4      out      24  Pos        12   IN
8   PY                 4      out      48  N          12   IN
12  UZ                 4      out      72  Tangent    12   IN
16  VZ                 4      out      104 U,V         8   IN
20  RZ                 4      out      112 EU,EV       8   IN
36  TPos_AOS          12      out      124 i           4   IN
60  TN                12      out      128 OrigBaryB/C 8   IN
84  TTangent          12      out      136 ShellH      4   IN
96  EUZ,EVZ            8      out
120 Flags              4      out
```

**out = 72 bytes, IN = 68 bytes.** Phase 5's target is therefore
`sizeof(Vertex) == 68` for MESH storage. (The per-vertex loop touches 104 of the
140 bytes and they span offsets 4..123 — every cache line — which is why field
REORDERING is worth ~0: with a 140-byte stride the 36 untouched bytes never form
a whole line. Only shrinking helps.)

Readers of the `out` fields, in the built tree (`FL.CPP` is **not** in
CMakeLists — dead), split by which vertex they hold:

* **Transient / clipper-owned vertices (NOT mesh storage) — no migration needed.**
  `FrustumClipper::C_Verts[48]`, `Omni::V`, `Particle::V`. Everything in
  `FILLERS/FILLERS.CPP` (86 PX / 91 PY / 54 RZ / 33 UZ / 33 VZ),
  `FILLERS/TheOtherBarry.h`, `Mekalele.h`, `FILLERS/ShadowMap.cpp`,
  `Clipper.cpp`, and the bulk of `FRUSTRUM/FRUSTRUM.CPP` reads these.
  **Verified: every rasterized face reaches a filler through
  `FrustumClipper::Render`** (`RenderInner.cpp` 145/147/209/212/214/316/318/320/
  394/402 are the only raster entries) — so no rasterizer ever reads a mesh
  Vertex's `out` fields.
* **Reads `out` through a MESH vertex — this is the whole migration surface:**
  | site | fields | note |
  |---|---|---|
  | `FRUSTRUM.CPP:904` `*A = *F->A` ×3 + `A->RZ` | all | the entry copy; becomes IN-copy + one `out` read |
  | `Transform.cpp:2352` `F->VisibilityFlagsAll()` | Flags | 73 % of the FACE bucket |
  | `Transform.cpp:2606+` tile-bbox stamp | PX, PY, TPos.z | deliberately pointer-based (wrong `A_idx` on the conetest quad) |
  | `RenderInner.cpp:137,308` mirror centroid gate | PX, PY | |
  | `RENDER.CPP:1202` | TPos.z | (1420–1422 already read `ff->PX[ai]` — the precedent) |
  | `CAMERAS/CAMERAS.CPP:124–134` | TPos | plane/portal test |
  | `DEMO/CITY.CPP` (75 TPos / 51 RZ / 23 PX / 23 PY) | all | `Reflected_Transform`, already dual-writes `frame` |
  | `DEMO/CHASE.CPP:562–571` | Flags, TPos.z | its own face loop |
  | `DEMO/FOUNTAIN.CPP` (68 TPos / 14 PX) | TPos, PX, PY | water / particle projection |
  | `DEMO/Raytracer.cpp:93–103` | LR/LG/LB (the BGRA union) | |
  | `MISC/PREPROC.CPP`, `RADIO/RADIO.CPP`, `DEMO/Snapshot.cpp`, `DEMO/FillerTest.cpp` | 1–4 refs each | diagnostics / legacy |
  Migration vehicle stays the rename-first technique; `A_idx` trust is avoidable
  entirely by deriving the index as `F->A - tVerts` (exact, no invariant needed).

### CORRECTION: 13 SoA arrays is the WRONG output layout for this loop

This doc plans the `out` fields as 13+ separate `VertexFrame` arrays. Against the
measured mechanism (line-bound, not arithmetic-bound) that costs most of the win
back, and the evidence is already in the 2026-08-05 section:

* Today's inline store writes **4** streams (TPos_x/y/z + PY = 16 B/vert) and
  that alone measured **+0.4 ms** of VERT at 958 k verts. Phase 5 needs all 13
  (PX, PY, RZ, TPos×3, TN×3, TTangent×3, Flags ≈ 52 B/vert) — ~3× the streams
  and ~3× the store bytes.
* Net traffic: AoS walk 140 → 68 B/vert (−72) but SoA stores 16 → 52 B/vert
  (+36) = **−26 %**, not the "~45 % of VERT" this doc claims. Plus 13 concurrent
  write streams of DRAM page pressure.
* The per-face loop wants 4 fields of the SAME vertex (Flags, PX, PY, TPos.z).
  In 13-array SoA that is up to **4 cache lines per vertex**; interleaved it is 1.

**Recommended layout instead: keep inputs AoS (68 B) and make the outputs a
SECOND AoS array, one cache line per vertex.** `TPos×3 + TN×3 + TTangent×3 + PX
+ PY + RZ + Flags + BGRA = 56 B`, + `EUZ/EVZ` = **64 B exactly**. `UZ`/`VZ` do
not need to be in it at all: `FRUSTRUM.CPP:905–920` **unconditionally overwrites**
`A/B/C->UZ/VZ` from `F->U1..V3 * RZ` at clipper entry, so the transform's UZ/VZ
stores are dead for every rasterized face (`EUZ/EVZ` are overwritten too, but only
`if (F->Flags & Face_Reflective)` — those need care).

Then the loop is **2 sequential streams**: read 68 B, write 64 B (a full aligned
line, so the write-allocate read can be elided) versus today's ~280 B of
read+write-back per cold vertex — a ~50 % traffic cut with 1 write stream instead
of 13. The existing 13-array `VertexFrame` stays only for the few consumers that
want ONE field across many vertices (`SortZ`, `IsFrontFacingInViewSpace`,
`QuadAwareMaxViewZ`, `RENDER.CPP:1420`), or those move to the interleaved array too.

### Recommendation

Against a 1.2–2.6 ms front end, a ~50 % cut of the VERT portion is **~0.15–0.4 ms
of a ~79 ms frame (0.2–0.5 %)** for a refactor touching ~11 files including two
alternative transform pipelines (`Reflected_Transform` in CITY/CHASE, FOUNTAIN's
water/particle projection) and the clipper entry. **Do not start it as a perf
item.** If it is done, do it for the cleanliness/correctness reasons in this doc
(one transform-writer contract instead of three) and use the interleaved 64-byte
output layout, not 13 SoA arrays.

## MEASURED 2026-08-05 — the June verdict was measured in the WRONG REGIME. Read this first.

The June entry below ("Phase 2 washes; the transform is only ~0.35 ms; STOP") is correct
**for the flat greets scene** and wrong as a general statement. It was taken at ~16 k verts,
where the whole mesh is L2-resident. Under `--greets_displace` the main-view transform sees
**958 k verts/frame** and the front-end costs **7.9 ms**, not 0.35.

New instrument (default OFF, byte-null): **`--xfrm_prof=N`** — a per-frame breakdown of the
MAIN-VIEW `Transform_Objects` call, printed to stderr every N frames as per-frame MIN and p50.
Companion **`--xfrm_ablate=<bitmask>`** (diagnostic ablations, changes pixels; see
FeatureFlags.def). Harness: `./DEMO --bench=scene@scene=greets,t=<T>,iters=24`.

### The breakdown (1920×1080, this machine, per-frame MIN over 24 frames)

Box was shared with two other agents' renders throughout; 1-min load average is quoted per
run in the raw logs and ran 14–61. The MIN column is the least-contended frame — that is the
number to reason with; p50 is 5–10 % higher and tracks the load.

| scene / pose | TOTAL | SETUP | VERT | SOA | FACE | verts | facesTested | facesPushed |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| greets t=5780 `--greets_displace` | **7.92** | 0.01 | **4.01** | **2.40** | **1.45** | 958,204 | 144,982 | 74,962 |
| greets t=6097 `--greets_displace` | 6.04 | 0.01 | 3.45 | 2.14 | 0.40 | 887,573 | 52,570 | 16,551 |
| greets t=5780 flat (no displace) | 0.68 | 0.00 | 0.36 | 0.14 | 0.16 | 82,975 | 16,719 | 7,549 |
| city t=1961 (p50; 2 calls/frame) | 1.23 | 0.00 | 0.45 | 0.27 | 0.49 | 69,287 | 26,490 | 10,210 |
| chase t=800 (single frame) | 0.57 | 0.01 | 0.15 | 0.06 | 0.40 | 30,548 | 54,950 | 25,829 |

- **SETUP** = per-mesh matrix/bsphere/frustum work for meshes that survive to the vertex loop.
- **VERT** = the Inside/Ahead/Regular per-vertex loops.
- **SOA** = the Phase-1 `VertexFrame_DumpFromAoS` AoS→SoA post-pass sweep.
- **FACE** = the per-face visibility + reflective + SortZ + FList push + tile-bbox stamp loop.
- The flat row reproduces the June measurement; it is the same engine, a different regime.
- city's MIN is meaningless (city makes two main-view-classified `Transform_Objects` calls per
  frame and one is nearly empty, so the min picks that one); its p50 is quoted instead.
- chase has no `--bench=scene` driver (that harness only supports city and greets), so its row
  is a single snapshot frame at `--xfrm_prof=1`, not a distribution. Its whole front-end is
  0.35–0.64 ms across t=100..1600 and it is **FACE-dominated, not VERT-dominated** — ~1.8
  faces per vertex (shared verts, ~50 k faces over ~25 k verts) inverts the greets profile.
  Chase is not a front-end target; the SoA-inline win there is inside single-frame noise.

### The Phase-1 dual write is 30 % of the front-end

`VertexFrame_DumpFromAoS` is a SECOND full walk of the mesh's `Vertex` array that re-reads 16
of the 136(140) bytes per vertex the transform loop had just written. Measured **2.40 ms at
t=5780 / 2.14 ms at t=6097** — 30 % / 35 % of the whole main-view front-end, for zero
arithmetic. Cross-checked by ablation (`--xfrm_ablate=4`, sweep off): TOTAL 6.035 → 3.990 at
t=6097, i.e. −2.05 ms against a 2.14 ms bucket — the bucket is real, not a timer artifact.

**Fix landed: `--xfrm_soa_inline` (Phase 2a, DEFAULT ON).** Each per-vertex loop stores
TPos_x/y/z + PY into the SoA arrays as it goes; the sweep is skipped. **Bit-exact by
construction** — same values, same source, stored one loop earlier.

Measured, interleaved off/on/off/on inside one script, per-frame MIN over 24 frames:

| pose | OFF (TOTAL / VERT / SOA) | ON (TOTAL / VERT / SOA) | ΔTOTAL |
|---|--:|--:|--:|
| greets t=5780 displaced | 7.94 / 4.03 / 2.39<br>7.92 / 3.98 / 2.38<br>7.87 / 4.00 / 2.37 | 5.98 / 4.41 / 0.00<br>5.94 / 4.41 / 0.00<br>5.83 / 4.31 / 0.00 | **−2.00 ms (−25 %)** |
| greets t=6097 displaced | 6.41 / 3.61 / 2.23<br>6.36 / 3.60 / 2.28 | 4.35 / 3.90 / 0.00<br>4.50 / 4.04 / 0.00 | **−1.96 ms (−31 %)** |
| greets t=5780 flat | 0.696 / 0.366 / 0.142 | 0.574 / 0.379 / 0.001 | −0.12 ms (−18 %) |
| city t=1961 (p50) | 1.28 / 0.45 / 0.29<br>1.21 / 0.44 / 0.26 | 1.14 / 0.60 / 0.00<br>1.13 / 0.61 / 0.00 | −0.11 ms (−9 %) |

The mechanism is visible in the split: SOA goes to zero and VERT rises by ~0.4 ms (the inline
stores — 4 stores × 958 k verts ≈ 15 MB to four streams, i.e. store-throughput-limited and
about as cheap as it can be). Net −2 ms.

Byte evidence: city pin `37e62845` exact, fountain pin `51fff7cd` exact, greets t=1588 pin
recipe off==on, chase 5-pose + cinematic 2-pose lists off==on (and t100/400/800/1200 match the
committed pins), greets displaced t=5780 off==on over 6 runs, and `--soa-verify` (which
compares the inline stores against the AoS the sweep would have copied, bit-for-bit) reports
0 mismatches on greets/city/fountain/chase. `--no-xfrm_soa_inline` restores the sweep.

### Rejected after measuring: a cheaper reciprocal (`--xfrm_rcp`, default 0)

Since the projection's `1/z` is the only divide in the loop, both a single-precision divide
(the Ahead/Regular loops write `1.0/z`, which promotes the float to DOUBLE) and a NEON
reciprocal estimate + one Newton step were built and measured. greets t=5780 displaced,
per-frame MIN, interleaved with a repeated control:

| mode | VERT | TOTAL | pixels changed vs mode 0 | max abs Δ |
|---|--:|--:|--:|--:|
| 0 — today's arithmetic (control ×2) | 4.238 / 4.450 | 5.725 / 6.013 | — | — |
| 1 — single-precision divide everywhere | 4.369 | 5.917 | **0 / 2,073,600** | 0 |
| 2 — recip estimate + 1 Newton step | 4.731 | 6.257 | 697 / 2,073,600 (0.034 %) | 103/255 |
| 2 — same, city t=1961 | — | — | 76 / 2,073,600 (0.004 %) | 141/255 |

**Both are rejected on PERF, not on bytes.** Mode 2 is 0.3–0.5 ms SLOWER than the plain
divide, and mode 1 is inside the control's own spread. Apple's FDIV is fast and fully
pipelined, the estimate+Newton is three dependent ops plus scalar vector↔GPR moves, and the
loop is waiting on the 140-byte `Vertex` stride either way — so there is no divide latency to
hide. There is no perf/divergence trade to put to a reviewer here: the approximation costs
0.034 % of the frame's pixels (max |Δ| ≈ 100–140, i.e. edge pixels flipping surface, not a
shading nudge) and buys negative time. Kept in-tree behind the flag as the measured record.

### Why widening the SIMD did not and will not help (the June wash, explained)

`Vertex` is `pack(1)`, 140 bytes, and the fields the per-frame loop touches span offsets
4..123 — i.e. **every cache line of the struct**. At 958 k verts that is ~134 MB of lines
pulled per frame plus the write-back. Two measurements say the loop is line/bandwidth-bound,
not arithmetic-bound:

- `--xfrm_ablate=1` removes the TN **and** TTangent mat-vecs — 2 of the 3 per-vertex
  matrix-vector products, 24 B of loads and 24 B of stores per vertex, 34 % of the struct.
  VERT moves 4.009 → 3.661 ms: **−8.7 %**, not −34 %. The lines come in either way; only the
  stores were saved.
- `--xfrm_ablate=2` removes the whole projection block (1/z + PX/PY/UZ/VZ): 4.009 → 3.464 ms,
  **−0.55 ms**.
- The SoA sweep, which does *no* arithmetic at all — 4 loads + 4 stores per vertex — costs
  60 % of what the entire transform loop costs.

So the lever is **bytes touched per vertex**, not lanes per instruction. Ranked:
1. ~~kill the redundant second pass~~ — DONE (`--xfrm_soa_inline`).
2. **Phase 5 (shrink `Vertex`)** is now the real perf item, not just a cleanliness one:
   moving the per-frame-written outputs out of the AoS struct cuts the stride the transform
   walks. Its ceiling is proportional to the byte reduction, ~45 % of VERT at best.
3. Phase 2 (Vec8f across 8 verts) remains a wash and should stay parked — an 8-vertex gather
   out of a 140-byte stride buys nothing when the stride is the problem.

### Where the 958 k verts come from (a bigger, separate lever)

The displaced Piramid is 261,768 verts. The main view transforms 958,204. The greets mirror
system clones the whole scene per mirror (`[MIRROR 'teleporter'] cloned 534,356 verts /
90,890 faces`, and again for `P_TEXT.JPG#6`), and those clone meshes are ordinary scene
meshes gated only by `HTrack_Visible`. **Most of the displaced front-end at t=5780 is
mirror-clone geometry, not the wall the camera is looking at.** Culling/LOD-ing the clones is
worth more than anything left inside `Transform_Objects`; it belongs to `GreetsMirror.cpp`,
not to this refactor. See docs/OPTIMIZATION_BACKLOG.md.

### The per-face loop

At t=6097 (the pose where the ablation arms are internally consistent — at t=5780 the face
ablations delete ~60 % of the frame's raster work and the cache state at Transform time is no
longer comparable):

| arm | FACE |
|---|--:|
| base (test + SortZ + FList push + tile bbox) | 0.395 |
| `--xfrm_ablate=8` (visibility/backface test only, never push) | 0.322 |
| `--xfrm_ablate=16` (loop + Face walk only) | 0.032 |

**The visibility/backface TEST is ~73 % of the per-face cost**, the accepted-face work
(SortZ + push + bbox for 16.5 k of 52.5 k faces) only ~0.07 ms. `Face::VisibilityFlagsAll()`
is `A->Flags & B->Flags & C->Flags` — three pointer chases into 140-byte `Vertex` structs, and
the tile-bbox stamp chases the same three again for PX/PY/TPos_z. Same mechanism as above:
the cost is the AoS walk. Reading those from the SoA arrays instead (4-byte stride) is the
obvious follow-up, but it needs `F->A/B/C_idx` to be trustworthy on every mesh — the
tile-bbox comment in Transform.cpp records meshes where they are not (the conetest quad), and
a wrong bbox DROPS a face where a wrong SortZ was harmless. Not attempted here.

### Note: `--greets_displace` at t=6097 is NONDETERMINISTIC

6 runs (3 flags-off, 3 flags-on) produced **6 distinct hashes** at t=6097 while t=5780 was
byte-stable 6/6 in both arms. This is pre-existing (it reproduces with all new flags off) and
unrelated to the SoA work, but it means **t=6097 cannot be used as a byte gate** and the
"greets is deterministic again" claim in SESSION_STATE should be read as scoped to the
non-displaced pin recipe.

## MEASURED 2026-06-19 — Phase 2 WASHES; the transform is only ~0.35 ms. STOP.
## (correct for the FLAT scene only — superseded by the section above)

Built the Vec8f-across-8-verts Inside loop (gated `--soa-wide-xform`, FMA association
matched → byte-identical to the scalar path: flag-off == flag-on confirmed). **Result on
greets: XFRM 0.353 ms (off) vs 0.358 ms (on) — no change.** Reverted.

Two facts kill the SoA-perf premise:
1. **The per-frame transform is ~0.35 ms (0.7% of frame), not the 3–5 ms this doc's Goal
   estimated.** That estimate was load-inflated / from an earlier state. There is no 3–5 ms
   to win here.
2. Even so, the Vec8f path didn't beat the per-vertex Vec4f-broadcast: the AoS gather of
   ~11 input fields + scatter of ~14 output fields per 8 verts (strided, no HW gather on
   arm64) dominates the wide-compute saving — the documented wash-risk, confirmed.

The refactor still has *correctness/cleanliness* value (Phase 5 shrinks `Vertex` 136 B→60 B,
helps the clipper's `*A=*F->A` copy), but **NOT a perf justification.** Do not pursue Phase 2+
for speed. If continued, justify it on cache-density/cleanliness, measure the clipper-copy
win directly, and expect the transform itself to stay ~0.35 ms.

## CURRENT STATE (2026-06-19) — Phase 2 is the next + biggest lever, NOT yet started

Verified live state of the refactor:
- **Phase 1 done** but as a *post-pass sweep*: the scalar per-vertex transform loop
  (`Transform.cpp:317`, `MatrixXVector` + project + divide) writes AoS, then a separate
  sweep (`Transform.cpp:1383`, `VertexFrame_DumpFromAoS`) copies AoS→SoA. That sweep
  currently dual-writes **only `TPos_z`** (the one field a consumer — SortZ — migrated to),
  so eliminating it alone saves ~nothing.
- **Phase 4 barely started** (SortZ on SoA; Transform.cpp:419/461/1540/1609). **Phase 6.1/6.2
  tried + REVERTED** (clipper TPos/PX override — stale-frame after Reflected_Transform).
- **Phase 2 NOT started** — the transform loop is still scalar/1-wide-broadcast. **This is
  the perf win** (~3–5 ms greets per the Goal below): rewrite the three loops
  (Inside/Ahead/Regular) to Vec4f/Vec8f over 4–8 verts. The hard part is the near-clip mask
  (the Ahead/Regular branch flags). Validation matrix: 1-LSB pixel-diff on city@1500 /
  greets@2500 / fountain / chase + TSan + bench (`--soa-verify` gate exists for AoS-vs-SoA
  bit checks). Foundation-critical: a wrong transform breaks every pixel — do it fresh, not
  at the tail of a long session.

  **CORRECTED entry point (verified 2026-06-19):** NOT `Vertex_Loop1` (`Transform.cpp:312`) —
  that is **DEAD CODE** (legacy MMX-era reference, line 7). The live per-vertex transform is
  the **Inside / Ahead / EAhead loops inside `Transform_Objects` (~lines 1100-1370; labels
  `Ahead:` @1165, `EAhead:` @1306, Inside just above @1155)** + the near-clip path
  `calcVisibilityFlags` (@343, called @395). Those loops **already do Vec4f "1-wide-broadcast"**
  — one vertex, SIMD across the matrix columns (`mul_add(m34_col_x, Vec4f(vpx), m34_col_w)`),
  cda338e (~1 ms). Phase 2 = the **Vec8f-across-4-to-8-VERTS** restructure: transpose/gather 8
  verts' AoS `Pos.x/y/z` into Vec8f lanes (the crux — AoS stride forces a transpose; whether it
  pays vs the scalar math saved is THE open question — could wash like the other levers if the
  gather/scatter dominates), wide matrix-mul + reciprocal + project, scatter back to AoS (Phase 2
  keeps the dual-write, so the scatter stays until Phase 5 drops AoS for aligned SoA stores — the
  *full* win likely needs Phase 4->5, not Phase 2 alone). Plus the tricky near-clip mask. Gate
  behind a flag (default off) so the scalar path stays correct during validation.

## Goal

Convert the per-vertex transformed-state from AoS (pack(1) `Vertex` struct,
136 bytes/vert) to per-mesh SoA arrays, so:
- `Transform_Objects` can do true 4-wide (Vec4f) or 8-wide (Vec8f) SIMD
  batches without paying gather/scatter cost on every field access
- The rasterizer's per-triangle vertex reads stay cache-coherent within
  each accessed plane
- 1/x divides and matrix-vector products amortize across 4-8 verts per
  instruction instead of broadcast-once-per-vert

Current 1-wide-broadcast SIMD inside `Transform_Objects` (committed as
`cda338e`) lands ~1 ms on greets. True wide SIMD with SoA layout is
estimated **3-5 ms additional** on greets transform, plus secondary wins
in rasterizer-side reads of transformed fields.

## Naming: this is option **B**, not "hybrid B/C"

An earlier framing of this refactor (from the menu in chat) called full
SoA "option C" and presented C as strictly more aggressive than B. That
framing was misleading. C is more aggressive but **architecturally worse
for the clipper**:

- Clipper's `*A = *F->A` whole-Vertex copy IS the access pattern — it
  consumes EVERY input field of A in a single contiguous memcpy. With
  inputs as AoS that's one cache-line-friendly copy; with inputs as SoA
  it's 12+ gather loads. Strict regression for the clipper hot path.
- Inputs are read by Transform sequentially per-vert: AoS @ ~60 B/vert
  (after the transformed fields move out in Phase 5) is fine for
  sequential cache prefetch.
- Inputs are not read by the rasterizer at all.

So input fields (Pos/N/Tangent/U/V/OrigBary) stay AoS as the **end
state**, not as a stepping stone. Only the per-frame-written transformed
fields move to SoA. That's option B, and B is the destination.

A theoretical Phase 7 could push inputs to SoA too (full C), but the
clipper-side hit is real and the rasterizer-side win is speculative.
Defer unless concrete post-Phase-5 profiling shows input-SoA would
help — see "Open questions" at the end.

## What stays AoS, what goes SoA

**Stays in current `Vertex` (AoS, read-only across a frame):**
- `Pos` (object-space position)
- `N` (object-space normal)
- `Tangent` (object-space tangent)
- `U, V, EU, EV` (texture coords)
- `OrigBaryB, OrigBaryC` (lightmap bary, scene-init constants)
- `i` (vertex index)

**Moves to per-mesh SoA (per-frame writable):**
- `TPos.x/y/z` (view-space position)
- `TN.x/y/z` (view-space normal)
- `TTangent.x/y/z` (view-space tangent)
- `PX, PY` (projected pixel coords)
- `RZ, UZ, VZ, EUZ, EVZ` (perspective-divided)
- `Flags` (visibility bits)
- `BGRA / LR/LG/LB/LA` (per-vertex lit color, when computed)

This is option B (see "Naming" above): inputs AoS, outputs SoA.

## Per-mesh storage

Each `TriMesh` gets a `VertexFrame` companion struct:

```cpp
struct VertexFrame {
    // SoA arrays, all of length T->VIndex, aligned to 32 bytes for
    // Vec8f load/store. Allocated once at scene init; reused across
    // frames. Per-thread is not needed — Transform_Objects runs
    // per-mesh on one thread at a time (the parallelism is at the
    // tile-job level downstream of Transform).
    float *TPos_x, *TPos_y, *TPos_z;
    float *TN_x,   *TN_y,   *TN_z;
    float *TTangent_x, *TTangent_y, *TTangent_z;
    float *PX, *PY, *RZ, *UZ, *VZ, *EUZ, *EVZ;
    uint32_t *Flags;
    uint32_t *BGRA;
    int      capacity;  // == T->VIndex; tracked for realloc safety
};
```

Allocation: one big slab per mesh in `Scene_RebuildMatTable` (or
wherever VIndex first stabilizes). Free in mesh dtor. The 32-byte
alignment lets the wide-SIMD Transform loop use aligned loads/stores
(`vld1q_f32`-aligned variants) — material perf delta vs unaligned on
arm64.

## Face dispatch

Today `Face` holds three `Vertex*` (A/B/C). With SoA outputs, those
pointers need to either stay (rasterizer reads the AoS read-only
fields via them) or get replaced by indices (so SoA outputs are
addressable via `T->frame.TPos_x[A_idx]`).

**Two paths:**

1. **Keep `Vertex*` for the static input fields.** Add three
   `uint32_t A_idx/B_idx/C_idx` to `Face` so the SoA output arrays
   are addressable. ~8 extra bytes per face. Migration is purely
   additive — existing `F->A->Pos` etc. still works.

2. **Replace `Vertex*` with indices entirely.** Saves 16 B/face vs
   path 1 (pointer is 8B, idx is 4B; 3 pointers → 3 indices = 24 B
   → 12 B). Requires either a per-mesh pointer to the input AoS
   array (the rasterizer needs to know which TriMesh to dereference
   for `Pos/N/Tangent`) or a global `VertexInput` registry.

**Path 1 is the safe migration vehicle.** Path 2 is a follow-up
optimization once Path 1 is verified.

## Clipper's transient buffer

`FrustumClipper::C_Verts[CLIPPER_MAXVERTS]` holds the clipper's
working set: 3 input verts copied from `F->A/B/C`, plus up to 45 new
verts created during Near/Far/Left/Right/Up/Down clipping. These are
NOT in any mesh's SoA arrays — they're transient scratch.

Two options:

1. **Keep clipper as AoS internally.** Convert mesh SoA → AoS at
   `Render()` entry (3 vertices), do clipping in AoS as today, hand
   clipped output to rasterizer as AoS. Adds 3-50 vertex-converts
   per `Render()` call (~250 bytes copied per call); rasterizer
   continues to read via `Vertex*`.

2. **Convert clipper to SoA scratch.** New `ClipperSoAScratch` with
   the same field set, sized to CLIPPER_MAXVERTS. `FInterpolator`
   becomes SIMD-friendly (Vec4f lerp across PX/PY/UZ/VZ + etc.).
   More invasive but unlocks `FInterpolator` wins on top of the
   transform wins.

**Phase the work — option 1 first**, since it preserves the existing
clipper/rasterizer code unchanged. Option 2 becomes a follow-up.

## Phased migration

Each phase is independently shippable + benchable.

### Phase 0 — Branch setup + plan

- Branch off `feature/static-shadow-lightmaps`.
- This document committed as the design reference.
- No code changes.

### Phase 1 — Allocate per-mesh `VertexFrame`, populate alongside AoS

- Add `VertexFrame` struct + per-`TriMesh` instance.
- Alloc on scene init (`Scene_RebuildMatTable` or mesh load).
- Free in mesh dtor.
- `Transform_Objects` writes to BOTH the existing AoS fields AND the
  new SoA arrays each frame (duplicated writes).
- Add a runtime assertion in DEBUG that the SoA and AoS values match
  bit-for-bit after each frame's Transform.
- No consumer changes. Bench: ~0 (slight regression from duplicated
  writes).

**Validates:** alloc/free hygiene, parallel write safety, the
"transform writes the right thing to SoA" basic correctness.

### Phase 2 — Switch `Transform_Objects` to wide-SIMD over SoA

- Rewrite the three per-vertex loops (Inside / Ahead / Regular) to:
  - Read AoS input fields (Pos, N, Tangent, U, V) sequentially as
    today — this is fine, the AoS layout for inputs is cache-coherent
    for sequential vertex iteration since pack(1) packs them all
    contiguously.
  - Compute via Vec4f or Vec8f wide SIMD over chunks of 4 or 8 verts.
  - Write outputs to SoA via aligned `vst1q_f32` (4-wide) or 8-wide.
- Keep the AoS output writes for now (Phase 3 removes them).
- Bench: ~1-2 ms savings on Transform.

**Validates:** wide SIMD compute correctness, alignment, mask-based
flag computation for the Ahead/Regular near-clip branch.

### Phase 3 — Add `A_idx/B_idx/C_idx` to `Face` (additive)

- New `uint32_t A_idx, B_idx, C_idx` fields on `Face`, populated at
  scene init (alongside the existing A/B/C pointers).
- All existing consumers continue using `Vertex*`. New consumers can
  use the indices.
- ~12 bytes/face overhead. Probably not measurable.

### Phase 4 — Migrate hot consumers to SoA-aware reads

Each consumer migration is independent + benchable. Order by ease:

1. **SortZ** (`Transform.cpp:1338+`): reads `F->A/B/C->TPos.z`. Switch
   to `T->frame.TPos_z[A_idx/B_idx/C_idx]`. One file. Verify sort
   order matches.
2. **Backface cull / visibility flags**: similar.
3. **`Rasterize_triangle` setup** in TheOtherBarry / Mekalele /
   ShadowBarry: each reads `V[i]->PX/PY/RZ/UZ/VZ/etc`. Local change
   to use indices into the mesh's SoA arrays.
4. **`FrustumClipper::Render`** entry: instead of `*A = *F->A`, read
   per-field from SoA outputs + AoS inputs into the clipper's
   working set (still AoS internally per "Option 1" above).
5. **Deferred lighting `IsFrontFacingInViewSpace`** and similar
   per-face TPos reads.

After each migration, the corresponding AoS write in `Transform_Objects`
becomes dead — remove it, bench, confirm.

### Phase 5 — Remove AoS transformed fields from `Vertex`

- Delete `TPos, TN, TTangent, PX, PY, RZ, UZ, VZ, EUZ, EVZ, Flags,
  BGRA, LR/LG/LB/LA` from the `Vertex` struct.
- Verify no remaining references.
- `Vertex` shrinks from 136 B to ~60 B. Better cache density on the
  AoS read-only side, helps clipper's `*A = *F->A` copy.

### Phase 6 — SoA-ify the clipper's transient buffer

Note: this is the clipper's TRANSIENT working buffer (`C_Verts`), not the
mesh's input AoS. Mesh inputs stay AoS — see Phase 7 footnote for the
"convert mesh inputs to SoA too" non-goal.

#### Phase 6.1 — TPos override (LANDED eb50b2f, REVERTED 2026-05-30)

After `*A = *F->A` (memcpy of the AoS Vertex from the mesh into the
clipper's `C_Verts[0..2]`), override `A/B/C->TPos_AOS.x/y/z` from
`F->frame->TPos_x/_y/_z[F->A_idx/B_idx/C_idx]`. This decouples the
clipper's TPos source from the AoS struct on the mesh — every
subsequent internal `A->TPos_AOS` read inside the clipper still reads
from the *AoS copy*, but the values came from the SoA arrays.

This is the smallest possible step toward Phase 5b: it proves the
machinery (F->frame, F->A/B/C_idx, the chunk-rebuild path in GREETS
that re-stamps indices on remapped Faces) works end-to-end across all
6 scenes.

**Two bugs found via instrumentation:**

1. Greets pyramid-chunk Faces inherited stale A/B/C_idx from the
   parent piramid mesh (`Compute_FaceVertexIndices` ran before
   chunking, stamping 1276-style indices that pointed into the parent's
   16596-vert frame). After chunking, `chunk->Verts` capacity was ~64
   per chunk but A_idx was still 1276 → would have segfaulted as soon
   as anything read from chunk->frame->TPos_x[1276]. Fix: re-stamp
   A/B/C_idx after the chunk pointer remap. See GREETS.CPP:993+.

2. Particle Faces (`Sc->Pcl[I].F` in `InsertSpriteToTBR`) have no
   `F->frame` — their TPos is written by the per-particle projection
   in `Transform_Objects`, not by the SoA Transform. The Phase 6.1
   override gates on `F->frame != nullptr` so particles keep working.
   `InsertSpriteToTBR` itself reverted to pure AoS in Phase 4.3.

**Why reverted (2026-05-30):** Phase 6.1 was zero-perf-benefit (it's
a no-op when working, harmful when wrong) but kept surfacing latent
A_idx mismatches and frame-staleness bugs across many code paths:

- `MakeFacesIndependent` rebuilt T->Verts without restamping A_idx
  → fixed in 203c14b.
- `BuildSkyCube` hand-wired Faces without stamping A_idx → fixed in
  203c14b.
- `Reflected_Transform` (CITY/CHASE cube-map bake + reflection
  passes) wrote T->Verts.PX/PY/RZ/TPos without dual-writing T->frame
  → fixed by Reflected_Transform dual-write in ae5b023 +
  VertexFrame_DumpFromAoS helper.
- `tessellateWaterGrid` rebuilt water mesh without restamping A_idx
  → fixed in 0c06930.
- Greets forward-mode wall fragments still missing after all those
  fixes — there's at least one more transform path not yet found.

Cost-benefit: chasing the rest of the alternative-transform paths
buys us nothing today (the override is a no-op against fresh frame)
and we keep introducing visual regressions. The path to Phase 5b
runs through Phase 6.3 (ClipperSoAScratch) anyway — that work
naturally requires migrating every transform path because deleting
TPos_AOS from Vertex forces all writers to use frame instead.

**Kept landed:** the A_idx restamp fixes (genuine bugs even without
Phase 6.1) and the VertexFrame_DumpFromAoS helper + Reflected_
Transform dual-write (infrastructure Phase 6.3 will need anyway).

#### Phase 6.2 — PX/PY/RZ override (BLOCKED on alternative transform paths)

The next step would be: override `A/B/C->PX/PY/RZ` from frame as
well, decoupling the clipper's vertex-visibility classification
(`Vtx_VisLeft/Right/Up/Down`) from the AoS PX/PY.

**Tried 2026-05-30. Reverted.** Two bugs uncovered, one fixable, one
genuinely blocking:

**Bug 1 (fixable; fixed): wrong A_idx in two mesh-rebuild paths.**

- `MakeFacesIndependent` (DEMO/MeshOps.cpp) — per-face crease pass
  reallocates T->Verts and repoints F->A/B/C, but never re-stamped
  F->A/B/C_idx. Fix: call `Compute_FaceVertexIndices(T)` after the
  rewire. Called from CITY/CHASE/GREETS at scene init.
- `BuildSkyCube` (FDS/SkyCube/SkyCube.cpp) — hand-wires SkyCube Faces
  bypassing `Scene_Computations`; A/B/C_idx stayed at default 0. Fix:
  same — call `Compute_FaceVertexIndices(T)` after the loop.

Both bugs were *latent* (TPos override happened to alias correctly
in Phase 6.1 because vertices with the same TPos values existed at
the wrong-index slots). The PX/PY/RZ override surfaced them via a
diagnostic that crashes on AoS vs frame value divergence.

**Bug 2 (the real blocker): stale frame after `Reflected_Transform`.**

CITY (and CHASE) reflection cube-map bake runs:
```
Reflected_Transform(CitySc);    // writes T->Verts.PX/PY/RZ/TPos
Radix_Sort(FList, SList, CAll);
Render(RenderPath::ForceForward);  // forward-path clipper render
Transform_Objects(CitySc, ...);  // writes BOTH T->Verts AND T->frame
Render(...);
```

`Reflected_Transform` is a wholly separate transform pipeline
(DEMO/CITY.CPP:367) — own matrices, own per-vertex loop, writes
Vtx->PX/PY/RZ/TPos in place. **It never touches T->frame.** So
between Reflected_Transform and the next Transform_Objects, T->Verts
holds reflection-camera values while T->frame still holds the prior
frame's main-camera values.

F->frame remains set to T->frame (stamped during the LAST
Transform_Objects's FList build), so the Phase 6.2 override sources
PX/PY/RZ from stale main-camera frame data into reflection's AoS
slot → garbage clipper math → visible breakage.

The TPos-only Phase 6.1 override hit this too but the bug was hidden:
when reflection rendered with stale frame TPos, the clipper still
projected reasonable-looking pixels (TPos magnitudes are usually in
the same ballpark across frames). PX (post-perspective-divide) is
much more sensitive — values can swing thousands of pixels.

**Options for Phase 6.2:**

a. **Patch every alternative transform path to dual-write frame.**
   Reflected_Transform (CITY/CHASE) is the known offender; there may
   be others (RenderSkyCube uses Transform_Objects directly, fine;
   particle-projection branch in Transform_Objects already covered).
   Adds 1 SoA dump loop per alternative path. Mechanical.

b. **Reset F->frame to nullptr after Reflected_Transform.** Cheaper
   patch — just makes the override skip on those Faces. But this
   means the reflection render can never benefit from SoA reads.
   And Phase 5b (delete TPos_AOS from Vertex) becomes impossible
   for the reflection path since there'd be nowhere to read TPos
   from.

c. **Build ClipperSoAScratch (Phase 6.3, below) first.** This shifts
   the clipper's internal reads off the AoS Vertex copy entirely,
   sourcing from per-clipper SoA scratch instead of F->frame. The
   `Render()` entry would copy AoS Vertex's PX/PY/RZ (which are
   always correct, regardless of frame staleness) into the scratch,
   bypassing the frame-staleness issue. Then Phase 5b deletes the
   AoS slots and *forces* every transform-writer to write somewhere
   else (likely frame), at which point option (a) becomes mandatory.

**Recommended sequencing: (c) then (a).** Build ClipperSoAScratch
first (unblocks the work without depending on every transform path
being fixed), then migrate transform-writer paths to dual-write frame
(unblocks deleting the AoS slots).

#### Phase 6.3 — ClipperSoAScratch (DESIGN)

- New `ClipperSoAScratch` struct with the field set
  (PX/PY/RZ/UZ/VZ/TPos_xyz/TN_xyz/TTangent_xyz/EUZ/EVZ/U/V/EU/EV/
  OrigBaryB/C/Flags/BGRA), sized to `CLIPPER_MAXVERTS=48`, owned
  per-thread (matches the existing `C_Verts` lifetime: per
  FrustumClipper instance, which is per-thread).
- `C_Verts` deleted. `C_Prim/Scnd/Tetr` become `uint8_t[]` index
  arrays (max 48 indices each, so 6×48=288 bytes vs today's
  3*48*sizeof(Vertex*) = ~1152 bytes of pointers).
- `Render()` entry: instead of `*A = *F->A`, read PX/PY/RZ etc from
  `F->frame` directly into `ClipperSoAScratch[0/1/2]`; copy
  inputs (Pos/N/Tangent/U/V/OrigBary) from AoS — those stay in
  Vertex.
- `FInterpolator` becomes a wide-SIMD lerp across the SoA fields.
  Currently it's 4-lane SIMD across PX/PY/UZ/VZ + 4-lane across
  TTangent/EUZ; SoA layout lets us do 8-lane across more fields at
  once.
- Rasterizer dispatch: today `rasterize_triangle(Vertex* A, Vertex*
  B, Vertex* C)`. New signature reads from
  `ClipperSoAScratch + 3 indices`. Each rasterizer (`IX.cpp`,
  `Mekalele.cpp`, `TheOtherBarry.cpp`, `IXFZ.cpp`, etc.) needs
  this change.

**Estimated effort: 2-3 days.** Touches ~10 files but the surface
is well-scoped (FrustumClipper internals + rasterizer call sites).

**Wins:** unblocks Phase 5b (delete TPos_AOS / TN / TTangent / PX /
PY / RZ / UZ / VZ / Flags / BGRA from `Vertex`, shrinking it from
136 B to ~60 B), saves the per-Render `*A = *F->A` memcpy
(replaced with 9 SIMD loads + indexed scatter), and gives
`FInterpolator` a 2× lane-width upgrade.

## Migration technique for Phase 5 (compiler-catch reads)

When removing AoS fields in Phase 5, **rename the fields first** (e.g.
`Vertex::TPos` → `Vertex::TPos_deprecated_use_frame`) before deletion.
The compiler then catches every remaining reader at build time, instead
of having migration gaps segfault at runtime on code paths the test
matrix didn't cover.

Lesson from Phase 4.x: a sprite-Face migration (`InsertSpriteToTBR`)
slipped through grep because the surface looked like the mesh case but
the underlying Faces (particles) don't have `F->frame` stamped. Crashed
on fountain at offset 0x10 (= `VertexFrame::TPos_z`). A rename-first
pass would have made the compiler list every site that needed manual
attention.

Plan for Phase 5: do the rename in a prep commit, fix every site the
compiler flags, only then drop the renamed field. Build (not just
bench) is the validation step.

## Validation strategy

Per phase:
1. Build clean (no new warnings).
2. Run smoke-test snapshots: `city@t=1500`, `greets@t=2500`,
   `fountain`, `chase`.
3. Pixel-diff each against `master` baseline (or against the Phase 0
   baseline if FP determinism shifts from `-ffp-contract=fast` show
   up). Tolerance: 1 LSB per channel; fail on >1.
4. Bench greets + city (default + halos-on). Variance band ~1 ms;
   expect each phase to either be neutral or move in the expected
   direction.
5. TSan run at Phase 1, 2, 4-end, 5-end (catches the parallel write
   bugs from the dual-write SoA/AoS period).

## Risks

- **Mesh dtor reach.** Some mesh allocation paths in 3DS / FLD /
  V3D loaders may not call the standard dtor — VertexFrame's slab
  could leak. Mitigate: RAII-wrap the slab in a unique_ptr inside
  TriMesh.
- **Clipper's `*A = *F->A` doesn't have a clean SoA alternative.**
  In Phase 4 we copy field-by-field from SoA (outputs) + AoS (inputs)
  into the clipper's working AoS Vertex. Slightly slower per-Render
  setup than the current memcpy. Net win still expected from the
  Transform side.
- **VertexScratch interaction.** The shadow pass uses its own
  `VertexScratch` for per-light parallel transforms. Each scratch
  needs its own SoA pair (the per-mesh SoA arrays are shared across
  all consumers, which conflicts with the per-light parallel writes
  the shadow path does). Solution: per-light shadow path uses
  per-light SoA scratch, not the per-mesh SoA. To be designed in
  Phase 1.
- **-ffp-contract=fast** is enabled globally on this base branch
  (commit `7e4c2ac`). SoA refactor may need to verify the same
  numerical outputs hold under the slightly-different SIMD code
  paths. Pixel-diff tolerance addresses this.

## Estimated effort

- Phase 0: ½ day (design + branch setup)
- Phase 1: 1 day (alloc + dual-write + verify)
- Phase 2: 1-2 days (wide-SIMD Transform; tricky near-clip mask)
- Phase 3: ½ day (additive Face fields)
- Phase 4: 2-3 days (per-consumer migration; risk-bearing)
- Phase 5: ½ day (struct shrink + cleanup)
- Phase 6 (optional): 2-3 days
- Validation throughout: 1-2 days

**Total: ~2 weeks for Phases 0-5, +3-4 days for Phase 6.**

### Phase 7 (footnote — deferred, possibly-not-worth-doing)

Convert input fields (Pos/N/Tangent/U/V/OrigBary) to per-mesh SoA too,
landing at "full C" — every Vertex field in arrays per mesh.

**Why it might be worth doing:**
- Wide-SIMD per-face setup in the rasterizer: gather PX/PY of A/B/C
  across mesh SoA arrays into one Vec3 per axis, do the orient2d /
  edge-function setup wide.

**Why it might NOT be worth doing:**
- Clipper's `*A = *F->A` whole-Vertex copy regresses to 12+ gather
  loads (today: one cache-line-friendly memcpy).
- Inputs are read sequentially by Transform — AoS at the reduced
  (~60 B/vert) post-Phase-5 size is already cache-friendly.
- Rasterizer doesn't read inputs.

**Decision criterion:** evaluate after Phase 5 with concrete profiling.
If `*A = *F->A` (clipper input copy) is significantly hot and the
rasterizer face-setup is significantly cold, skip Phase 7. If both
are roughly equal, Phase 7 might be net-positive — but the work
should not be planned ahead of that profile data.

## Open questions

- Should `VertexFrame` slabs be allocated per-`TriMesh` or pooled
  globally with offset-per-mesh indexing? Pooling improves cache
  density across meshes but complicates per-mesh resizing. Per-mesh
  alloc is simpler; revisit if profiling shows mesh-boundary cache
  misses.
- Is there appetite for changing `Face` size? Adding 12 B/face has a
  measurable cost for scenes with many faces (greets has ~tens of
  thousands). Could go index-only and recover the bytes after Phase 5.
- Does the lightmap atlas baker (`LightmapBake.cpp`) need SoA-aware
  reads? It runs at scene init, not per-frame — probably fine to
  keep reading AoS.
