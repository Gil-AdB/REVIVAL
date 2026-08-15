# PERF_STATE.md — current state of the deferred pipeline (greets, 2026-05)

> **2026-08-15: §00 rows 1 and 5 are amended in place by the omni-loop ladder**
> (`docs/OPTIMIZATION_BACKLOG.md` 2026-08-15). Row 1's `Ginstr/f` is confirmed
> to 0.16 %; row 5's *share* is corrected upward and its attack is refuted.
>
> **§00 (2026-08-14) is the CURRENT baseline and supersedes §0's numbers.** §0
> (2026-08-08) is kept because its *ablation method* and its `--texture_filter`
> adjudication still stand, and because it is the anchor §00 measures drift
> against. §1–§2 and §9 are 2026-05 estimates, superseded twice over; read them
> for mechanism only.
>
> **§0 below (2026-08-08) supersedes the numbers in §1–§2 and §9.** Those were
> estimates and partial brackets from 2026-05, before `renderFrame` had any interior
> instrumentation and before the PBR/mip/env defaults moved. §0 is a measured,
> self-checking phase split of five poses across three scenes. The rest of the
> document remains the best description of the *mechanism* (kernel structure, cube
> tap, sampling modes, flag inventory) — read it for "how", read §0 for "how much".

---

## 00c. THE G-BUFFER FILL — 2026-08-16c: the mirror pass had no tile cull at all

**§00b row 3 (`FrustumClipper::Render`, 6.2 %) is closed, and its stated
mechanism was wrong.** The row read "every raster tile re-walks the whole face
list"; the walk turned out to be the cheap half. What cost the frame was that
`--tile_bbox_cull` — default ON since the S2/B5 work — was **completely inert
on the mirror pass**, so 30 tiles × ~20 700 faces reached the clipper where
~30 000 (face, tile) pairs actually exist.

### THE CENSUS THAT SPLIT IT (city t=1961, 1512×848, 6×5 tiles)

`Transform_Objects` stamps each pushed face's projected screen bbox into its
`FListEntry`. The two hand-written mirror transforms —
`Reflected_Transform` in `DEMO/CITY.CPP` and `DEMO/CHASE.CPP`, which build
FList themselves for the water-reflection underlay — pushed
`*Ins++ = { F->SortZ.DW, F };`, a two-field aggregate that leaves the rest on
their default member initialisers, i.e. the **cover-all sentinel**:

| pass of the frame | cover-all entries | avg tiles/face | (face, tile) pairs |
|---|--:|--:|--:|
| main (`Transform_Objects`) | 1.2 % | 1.45 | 29 671 |
| mirror (`Reflected_Transform`) | **100 %** | **30.00** | **621 180** |

City runs both every frame (`renderFrame` calls/f = 2). chase has the same
transform and the same hole.

### WHAT LANDED

1. **`c26c1c35` — stamp the bbox on the mirror pushes**
   (`fds::FaceBBox_Stamp`, `FDS/Base/FaceBBox.h`). This is the whole prize.
2. **`d9dfa527` — `--face_tile_bin`** (default ON): `renderFrame` walks FList
   twice (count, scatter) and hands each tile a dense `Face*` run, so no tile
   walks the list at all. Order inside a tile is preserved exactly (chunk-major
   prefix sum over a contiguous split), verified element-by-element by a
   throwaway `FDS_BINVERIFY` build over 7 poses — 0 mismatches. New `face-bin`
   phase row prices the build at **0.200 ms/frame** at city t=1961;
   `--mem_census` reports 403 KiB arena + 113 KiB scratch there.

### MEASURED, parent `b2de6323` → `d9dfa527`

Two binaries in ONE worktree against ONE asset tree, interleaved, min-of-arm
with r0 dropped, 1512×848. **The box ran at load 17–24 for this batch, so the
wall columns below are floors, not clean numbers — `Ginstr/f` and `Gcyc/f` are
what decide** (the same rule §00b was measured under).

**CITY ARM** (`--env_live_water --deferred --city_env_pixel`):

| row | t=1961 (min-of-8) | t=2400 (min-of-5) | t=400 (min-of-5) |
|---|--:|--:|--:|
| **`gbuffer` Ginstr/f** | **0.658 → 0.444 (−32.5 %)** | 0.185 → 0.175 (−5.4 %) | 0.359 → 0.318 (−11.4 %) |
| `gbuffer` thrsum (core-ms) | 79.3 → 43.1 (−45.7 %) | 17.42 → 17.36 | 35.3 → 31.8 (−10.0 %) |
| `gbuffer` wall | 8.69 → 6.86 (−21.0 %) | 5.37 → 5.76 † | 4.00 → 3.53 (−11.9 %) |
| **`renderFrame` Ginstr/f** | **4.383 → 4.174 (−4.8 %)** | 2.273 → 2.269 | 3.216 → 3.180 (−1.1 %) |
| `renderFrame` Gcyc/f | 1.196 → 1.115 (−6.8 %) | 0.623 → 0.625 | 0.864 → 0.851 (−1.5 %) |

**GREETS ARM** (`--deferred --hdr --hdr-linear --texture-filter=2 --ssao
--ssao-gtao --greets-displace`):

| row | t=2845 (min-of-4) | t=5743 (min-of-8) | t=6097 (min-of-2) |
|---|--:|--:|--:|
| `gbuffer` Ginstr/f | 0.978 → 0.968 (−1.0 %) | 1.001 → 0.991 (−1.0 %) | 0.793 → 0.788 (−0.6 %) |
| `gbuffer` thrsum (core-ms) | 102.0 → 92.6 (−9.2 %) | 96.7 → 94.8 (−2.0 %) | 97.2 → 86.0 (−11.6 %) |
| `gbuffer` wall | 9.17 → 8.68 | 8.81 → 8.69 | 7.87 → 7.53 |
| `renderFrame` Ginstr/f | 4.983 → 4.981 | 4.996 → 4.990 | 4.405 → 4.406 |
| `renderFrame` Gcyc/f | 1.351 → 1.362 | 1.369 → 1.347 | 1.207 → 1.220 |

† t=2400's `gbuffer` wall is the only row that reads worse, at flat
instructions (−0.2 % of `renderFrame`) and flat thrsum (−0.3 %) — a 5.4/5.8 ms
wall pair under load 17–24, not a mechanism. The pose barely has the effect to
begin with.

### THE ROW ITSELF — sampled self time, city t=1961, his arm

`sample <pid> 25` on the running bench (leaf histogram; unprivileged, no
Instruments needed — `scratchpad/selftime.sh`), shares of DEMO self samples.
**The parent column reproduces §00b's 6.2 % at 6.46 %**, which is what makes the
after column quotable:

| symbol | parent | tip | absolute samples |
|---|--:|--:|---|
| **`FrustumClipper::Render`** | **6.46 %** | **1.21 %** | 9 223 → 1 644 (**−82.2 %**) |
| `RenderInnerMekalele` (the per-tile walk) | 2.53 % | 0.16 % | 3 605 → 217 (−94.0 %) |
| `fds::FaceTileBins_Build` + its dispatch | — | 0.10 % | 0 → 133 (new) |
| **the three together** | **8.99 %** | **1.47 %** | |
| `Render_VolumetricCones_Tile` (control) | 25.13 % | 27.16 % | 35 850 → 36 843 |
| `apply_exact<false>` (control) | 6.13 % | 6.23 % | 8 749 → 8 453 |

Both controls are flat in absolute samples — the shares only move because the
denominator shrank — so the two rows that did move, moved.

Note what the walk row says: at 2.53 % it was **not** negligible, and replacing
it with a 0.10 % build is a 27× cut in traversal. But it was 39 % the size of
the clipper row it was being blamed for; the clipper's own 6.46 % came from
being CALLED 21× too often on the mirror pass, which is why `c26c1c35` is the
commit that carries this round and `d9dfa527` is the smaller half.

**SPOT CHECKS — and chase is the biggest winner of the whole round**, because it
runs the same `Reflected_Transform` and its mirror frustum is full. chase t=800
(`--deferred`, profiled through the snapshot harness at one repeated timestamp,
min-of-5); fountain t=2500 (`--deferred --hdr --glass-refract=1 --glass-test`,
min-of-5, binning only — it has no mirror transform):

| row | chase t=800 | fountain t=2500 |
|---|--:|--:|
| **`gbuffer` Ginstr/f** | **0.597 → 0.334 (−44.1 %)** | 0.136 → 0.128 (−5.9 %) |
| `gbuffer` thrsum (core-ms) | 60.5 → 26.1 (**−56.9 %**) | 12.34 → 11.36 (−7.9 %) |
| `gbuffer` wall | 11.07 → 8.53 (−22.9 %) | 2.73 → 2.78 |
| **`renderFrame` Ginstr/f** | **3.729 → 3.475 (−6.8 %)** | 1.030 → 1.024 (−0.6 %) |
| `renderFrame` Gcyc/f | 0.894 → 0.815 (−8.8 %) | 0.409 → 0.408 |
| `renderFrame` wall | 38.43 → 36.09 (−6.1 %) | 34.49 → 34.21 |
| frame min | — (no `--bench` arm) | 37.40 → 36.95 (−1.2 %) |

§00 row 9 measured chase's `gbuffer` at `effPar` **5.5 of 12** and called it "half
the pool is idle". Half of what the pool was busy WITH is now gone: 60.5 → 26.1
core-ms at the same pose.

**Two things the city/greets table says, and both are the mechanism confirming
itself.**
t=2400 and t=400 move far less than t=1961: the mirror frustum there holds far
less of the scene-sized building, exactly the geometry-dependence that made
2026-08-16b's `LGHT` finding move only at t=1961. And **greets is NEUTRAL**, not
a win — it has no `Reflected_Transform` (its mirror is the RTT), so it gets only
the binning half: `renderFrame` instructions flat to −0.1 %, cycles ±1.6 %,
`gbuffer` instructions −1.0 %. Do not quote greets as a gain.

### THE WALL NUMBER, MEASURED PROPERLY

The tables above ran A-then-B in every round, which under this box's structured
load systematically penalises whichever arm runs second. Re-run with the arm
order ROTATED per round (`scratchpad/ab2.sh`), 13 rounds, r0 dropped — this is
the row to quote for city:

| city t=1961, his arm | parent `b2de6323` | tip `d9dfa527` | |
|---|--:|--:|--:|
| **frame min** | 50.61 | **47.74** | **−5.7 %** |
| **frame mean (TOTL)** | 57.71 | **54.24** | −6.0 % |
| `renderFrame` wall | 40.47 | 37.96 | −6.2 % |
| `gbuffer` wall | 8.571 | 6.023 | **−29.7 %** |
| `gbuffer` thrsum | 81.71 | 39.24 | **−52.0 %** |
| `gbuffer` Ginstr/f | 0.658 | 0.443 | −32.7 % |
| `renderFrame` Ginstr/f | 4.384 | 4.173 | −4.8 % |
| `renderFrame` Gcyc/f | 1.165 | 1.083 | −7.0 % |

Same treatment on greets t=5743 (11 rounds, rotated) leaves `renderFrame` at
**−0.1 % instructions / +0.1 % cycles** — flat, as the neutral verdict above
says. Its frame-min column swings ±10 % between batches in BOTH directions at
those flat counters; that is the box, not the change.

**BYTE-NULL** — all nine pins unmoved 2/2 on each binary, `render_gate.sh` 4/4.
The S2 contract is why: the box is a conservative superset of the un-clipped
triangle and the clipper only shrinks coverage, so a box that misses a tile
means zero output there. The escape hatches agree too — city t=1961 under his
arm gives `925ecd43…` on all four of default / `--no-face_tile_bin` /
`--no-tile_bbox_cull` / both off.

### WHAT THIS UNBLOCKS

§00 row 9's refuted finer-grid experiment ("a 12×10 grid cost +139 %
instructions, because each clipper tile re-walks the whole face list") was
blocked on exactly the traversal that is now gone. It is worth re-running on
chase, whose `gbuffer` `effPar` is 5.5 of 12 — but note it moves tile
boundaries, so unlike everything above it will not be byte-null.

---

## 00b. THE USER'S ACCEPTANCE ARM — city, 1512x848, 2026-08-16

```
./DEMO --env_live_water --deferred --city-env-pixel
```

**Nobody had ever profiled these two flags together**, and the arm is not a
small perturbation of §00's city rows — it moves the glass out of the forward
renderer and into the deferred kernel, and it turns on a per-pixel path in
`--env_live_water` that had only ever been priced on the FORWARD one. Poses are
the scene's own scripted camera at **t=1961 / 2400 / 400**, `iters=30`,
`--profiler=1 --deferred_prof=5 --hw_prof`, **min-of-10 over 11 interleaved
rounds** with round 0 dropped, two binaries in ONE worktree against ONE asset
tree. Loads 12–39 across the session — read `Ginstr/f` when a wall figure looks
surprising.

### TWO THINGS THE ARM INVERTS

* **`--city_env_pixel` makes city FASTER.** It moves the window glass into the
  G-buffer (`lighting-w1` 0.715 → 0.966 Ginstr/f at t=1961) and deletes the
  forward per-vertex reflective path, which cost more: clean single runs give
  frame min **77.11 ms** for plain `--deferred` against **59.22 ms** with the
  flag. Any city number quoted against plain `--deferred` is a slower frame
  than the one he runs.
* **`--env_live_water` is NOT free per pixel here.** The 2026-08-13b "free at
  the noise floor" was the FORWARD paraboloid path, per vertex. Through the
  deferred compose it is **+0.041 / +0.015 / +0.047 Ginstr/f of `lighting-w1`**
  at t=1961 / 2400 / 400 = **+4.4 % / +3.8 % / +6.3 %** of that phase, ≈ 0.74 ms
  at t=1961. The cost is the wave-slope evaluation the tilt calls per glass
  pixel, not the mask read.

Three-arm ablation of `lighting-w1` at t=1961 (Ginstr/f), which is how those
numbers were separated: plain `--deferred` **0.715** → + glass in the G-buffer
(`--city_env_pixel --no-env_refl`) **0.844** → + the env compose **0.933** → +
`--env_live_water` **0.974**.

### BEFORE / AFTER — his exact command, parent `44c8aeed` vs `545e96d9`

| | t=1961 par | t=1961 **new** | t=2400 par | t=2400 **new** | t=400 par | t=400 **new** |
|---|--:|--:|--:|--:|--:|--:|
| **frame min** | 54.720 | **49.760** | 30.950 | 30.890 | 36.360 | 35.990 |
| **frame mean (TOTL)** | 59.618 | **54.196** | 35.365 | 35.105 | 39.877 | 39.794 |
| **`LGHT` p50** | **5.904** | **0.974** | 0.189 | 0.164 | 0.281 | 0.266 |
| `renderFrame` (×2) | 40.011 | 40.133 | 24.149 | 24.043 | 28.425 | 28.164 |
| `cones-call` (×1) | 9.436 | 9.378 | 5.133 | 5.157 | 6.517 | 6.477 |
| `gbuffer` (×2) | 8.207 | 8.381 | 5.167 | 5.102 | 3.663 | 3.634 |
| `DeferredLighting-call` (×2) | 8.133 | 8.128 | 3.863 | 3.762 | 6.455 | 6.346 |
| `fastfog` (×1) | 7.524 | 7.452 | 4.148 | 4.106 | 4.631 | 4.530 |
| ⤷ `fog-columns` | 3.636 | 3.641 | 1.218 | 1.214 | 1.500 | 1.509 |
| ⤷ `fog-composite` | 3.111 | 3.004 | 2.398 | 2.309 | 2.639 | 2.591 |
| `TBR-render` (×2) | 4.200 | 4.168 | 3.264 | 3.269 | 4.617 | 4.627 |
| `water-glints` | 2.459 | 2.416 | 2.003 | 1.973 | 2.551 | 2.530 |
| `water-ripple` | 1.986 | 1.847 | 1.560 | 1.467 | 2.327 | 2.155 |
| `gbuf-clear` | 0.556 | 0.543 | 0.553 | 0.568 | 0.547 | 0.528 |
| **`Ginstr/f` `renderFrame`** | 4.409 | **4.387** | 2.288 | **2.278** | 3.243 | **3.222** |
| `Ginstr/f` `water-ripple` | 0.252 | **0.221** | 0.205 | **0.181** | 0.295 | **0.259** |
| `Ginstr/f` `water-glints` | 0.248 | **0.230** | 0.196 | **0.182** | 0.258 | **0.241** |
| `Ginstr/f` `fog-composite` | 0.392 | **0.377** | 0.311 | **0.301** | 0.347 | **0.335** |
| `Ginstr/f` `lighting-w1` | 0.963 | **0.957** | 0.391 | **0.389** | 0.736 | **0.728** |
| `Ginstr/f` `cones-call` | 1.288 | 1.288 | 0.686 | 0.686 | 0.894 | 0.894 |

**−4.96 ms of frame min and −5.42 ms of frame mean at t=1961 (−9.1 %), and
almost all of it is ONE row that had no instrument.** t=2400 and t=400 barely
move, and that is the mechanism confirming itself — see `LGHT` below.

### `LGHT` — the biggest WALL row in the frame and the smallest CPU row

`LGHT` runs OUTSIDE `renderFrame`, so no `--deferred_prof` table ever carried
it and no round of this campaign had opened it. Under this arm at t=1961 it
reads **5.9 ms p50** — ~10 % of the frame — while `LightMeshVerts` is **1.67 %
of DEMO self time** in a frame-dominated Time Profiler, i.e. ~9 CORE-ms. Wall 6
against 9 core-ms over 12 workers is `effPar` ≈ 1.5: a LOAD-IMBALANCE row, not
a compute row. The fan's unit was a MESH and city is 56 visible meshes of which
one is the scene-sized building; a work-stealing cursor cannot split a task.
Chunked to 1 024-vertex ranges it is **0.97 ms**, byte-null. t=2400/t=400 do not
have that mesh flagged visible, so they had no imbalance and gain nothing —
which is the check that the diagnosis is right.

`--prof_no_vertex_light` (default off, byte-null) is the ceiling instrument this
row needed; it is MEASUREMENT ONLY and changes pixels, because the forward
`TheOtherBarry` filler reads `Vertex::L{R,G,B}` for transparents, water, sprites
and the forward mirror-pass glass.

### INIT — `--env_live_water` bypasses `city_envmap_cache` BY DESIGN

The mask is a product of the bake's depth buffer, so a colour-only cache hit
would leave the tilt silently inert. Whole-process wall, `iters=5` (init
dominates), 3 runs each, all within 0.01 s:

| arm | process wall |
|---|--:|
| `--city_env_pixel`, cache warm | **0.98 s** |
| `--city_env_pixel --no-city_envmap_cache` (cold bake) | 2.18 s |
| **his arm** (`--env_live_water --city_env_pixel`) | **3.06 s** |

So his arm pays **+2.08 s of init on every launch**, of which 1.20 s is the
cold bake itself and 0.88 s the live-water re-shade. That is the prize for
`docs/BACKLOG_PLANS.md` item **2e** (a salted cache for the live bake, which
must also persist the mask plane). 2d/2e/2f are LOOK items and stay in his
queue — priced here, not landed.

### THE RANKED TABLE UNDER THIS ARM — city t=1961

Per-symbol shares are Instruments Time Profiler self time over a 400-iteration
bench (frame-dominated: init is ~3 s of ~28 s).

| # | item | symbol share | Ginstr/f | attack |
|---|---|--:|--:|---|
| 1 | `Render_VolumetricCones_Tile` | **20.6 %** | 1.288 (`cones-call`, ×1) | rounds 6–7 took −29 % of chase's; city's is §13's dependency chain. Only "fewer (px × spot) pairs" is left |
| 2 | `Render_DeferredLighting_Tile_OuterVec` | **15.3 %** | 0.957 (`lighting-w1`, ×2) | of which `--city_env_pixel` +0.129, the env compose +0.089, `--env_live_water` +0.041 |
| 3 | `FrustumClipper::Render` | **6.2 %** → **1.2 %** | inside `gbuffer` | **DONE 2026-08-16c (`c26c1c35` + `d9dfa527`), and the mechanism in this cell was only half right.** The walk was the smaller half: `--tile_bbox_cull` was **INERT on the mirror pass**, whose hand-written `Reflected_Transform` pushed a 2-field aggregate and left the cover-all bbox default on every entry — 621 180 (face, tile) clipper entries against the main pass's 29 671 on the same geometry. Stamping it + binning faces to tiles: this symbol **6.46 → 1.21 % of self time**, and with the walk row (2.53 → 0.16 %) **8.99 → 1.47 %**; `gbuffer` **0.658 → 0.444 Ginstr/f (−32.5 %)**, thrsum 79.3 → 43.1 core-ms; `renderFrame` 4.383 → 4.174 Ginstr/f. Byte-null. See §00c |
| 4 | `pwater::waterWaveSlope` | **6.2 %** → ~5.4 % | 0.451 (ripple + glints) | −12 % / −7 % taken; what is left is an 8-wide form of the two bilinear taps |
| 5 | `meka::TileRasterizer::apply_exact<false>` | 5.4 % | 0.659 (`gbuffer`, ×2) | `effPar` 8.5–8.7 of 12 — better than chase's 5.5, so §00 row 9's "half the pool is idle" does not hold in city |
| 6 | fastfog lambdas + `Froxel_CompositePixel` + `FastFog_SampleGrid` + `SkyPaint` | 6.2 + 3.4 + 2.7 + 1.6 % | 0.869 (`fastfog`, ×1) | `fog-columns` 0.400 is the residue; `Froxel_GlowTile`'s per-(column × light × slice) `atanf` is unpriced |
| 7 | `vFogNoise` + `vBlobNoise` | 4.7 + 0.8 % | inside `fog-columns` | §00 row 8's "the noise is the cost" reproduces exactly here |
| 8 | `Render_DeferredTransparentLighting_Tile<0>` | 3.4 % | 0.531 (`TBR-render`, ×2) | |
| 9 | `LightMeshVerts` | 1.7 % | none (outside `renderFrame`) | DONE above — the 1.7 %/6 ms split is the whole story |
| 10 | `logf` / `powf` / `atanf` | 0.9 / 0.9 / 0.3 % | — | `logf` was the froxel composite (fixed); the rest is the glint lobe and the glow integral |

---

## 00a. THE USER'S ACCEPTANCE ARM — greets, 1512x848, 2026-08-16

```
./DEMO --deferred --hdr --hdr-linear --texture-filter=2 --ssao --ssao-gtao --greets-displace
```

**This is a different frame from every table in 00 and 0.** No prior greets
round carried `--ssao --ssao_gtao`; `--ssao_downscale` defaults to **1**, so
GTAO runs at FULL resolution. Poses are the SCENE'S OWN scripted camera at
t=2845 / 3409 / 5743 / 5813 / 6097 (no `FDS_GREETS_CAM`), 12 iters,
`--deferred_prof=1 --hw_prof --profiler=1`, min-of-6 over 7 interleaved rounds
with round 0 dropped. Loads ran 15–39 across the session, so **read `Ginstr/f`
whenever a wall figure looks surprising** — `ssao`'s is stable to 0.2 % across
poses and batches, the wall is not.

### BEFORE — the map as his command found it (parent `f25bb992`)

| # | phase | ms t=5743 | Ginstr/f | share of `renderFrame` |
|---|---|--:|--:|--:|
| 1 | **`ssao`** | **19.4** | **2.143** | **39 %** |
| 2 | `DeferredLighting-call` | 17.6 | 2.040 | 35 % |
| 3 | `gbuffer` | 8.5 | 0.986 | 17 % |
| 4 | `shadow-bake` (outside `renderFrame`) | 2.5 | 0.207 | — |
| 5 | `RTT` | 2.1 | 0.024 | — |
| 6 | `bloom-chain` | 1.19 | 0.136 | 2 % |
| 7 | `cones` | 0.73 | 0.063 | 1 % |
| 8 | `tonemap-post` | 0.45 | 0.037 | 1 % |
| 9 | `mirror-grid` | 0.45 | — | 1 % |

Per-flag ablation at t=5743, one batch: `--no-ssao` frame **58.26 -> 39.33**;
hemisphere instead of GTAO `ssao` 19.4 -> 12.8; **`--texture_filter=2` costs
1.4 ms**, all of it `gbuffer` (8.50 -> 7.07, 0.986 -> 0.813 Ginstr) — the same
verdict 0 reached for `=1`, re-measured for `=2` on this arm;
`--no-bloom` 1.19 ms; `--greets_displace` itself 5.1 ms of frame (his look).

### AFTER — `b0905ee1`, the four SSAO rungs (docs/OPTIMIZATION_BACKLOG.md 2026-08-16)

Interleaved parent-vs-tip, min-of-8, both binaries one worktree one asset tree:

| pose | frame min | `renderFrame` | `ssao` ms | `ssao` Ginstr/f | `ssao` Gcyc/f |
|---|--:|--:|--:|--:|--:|
| t=2845 | 59.53 -> 59.05 | 49.67 -> 48.92 | 19.55 -> 15.06 | 2.150 -> 1.654 | 0.649 -> 0.488 |
| t=3409 | 68.45 -> 65.54 | 59.47 -> 52.94 | 22.27 -> 16.28 | 2.141 -> 1.653 | 0.637 -> 0.487 |
| t=5743 | 77.02 -> 71.29 | 66.45 -> 59.16 | 23.55 -> 16.36 | 2.143 -> 1.652 | 0.651 -> 0.492 |
| t=5813 | 58.30 -> 52.54 | 50.68 -> 45.20 | 20.18 -> 14.26 | 2.149 -> 1.650 | 0.655 -> 0.483 |
| t=6097 | 50.17 -> 44.41 | 45.10 -> 39.23 | 19.81 -> 14.14 | 2.155 -> 1.653 | 0.647 -> 0.487 |

`renderFrame` **Ginstr/f -9.0 to -10.2 %, Gcyc/f -9.3 to -10.5 %** at every
pose. Image cost of all four rungs: 343 px of 24.9 M at 1/255.

### The ranking AFTER, by instructions — the lighting kernel is #1 again

**Amendment 2026-08-16c (§00c) — and it is a NEUTRAL result, say so.** greets
gets only the binning half of that round: it has no `Reflected_Transform`, so
the 21× mirror-pass finding that moved city does not reach it. On this arm
`--face_tile_bin` is **`gbuffer` 1.001 → 0.991 Ginstr/f (−1.0 %), thrsum
96.7 → 94.8 core-ms, wall 8.81 → 8.69**, and `renderFrame` moves **−0.1 %
instructions / −1.6 % cycles at t=5743 and 0.0 % / +0.8 % at t=2845** — i.e.
inside the noise at frame level. It is kept because it is byte-null, because
the city arm does gain from it, and because it is what unblocks the finer-grid
experiment; it is NOT a greets win and nothing here should be quoted as one.

t=5743: `DeferredLighting-call` 2.041 (41 % of `renderFrame`'s 4.999),
**`ssao` 1.651 (33 %)**, `gbuffer` 1.002 (20 %), `bloom-chain` 0.136,
`cones` 0.084, `tonemap-post` 0.037. So 00's rows 1 and 5 (the omni loop and
the cube tap) are the next target on this arm too — **and the single largest
lever of all is a dial, `--ssao_downscale=2`, worth another -9.1 ms of frame
for a look change nobody has approved yet** (numbers + crops in
docs/OPTIMIZATION_BACKLOG.md 2026-08-16).

---

## 00. ROUND-1 REBASELINE — all five scenes, 2026-08-14

Ordered by what a frame-time-reduction campaign should attack next. Everything
below was measured on `a9ca03dc` in a throwaway worktree, `SDL_VIDEODRIVER=dummy`,
1920×1080 unless stated, 12 pool workers, **min-of-6 over 7 interleaved rounds
with round 0 discarded**, arm order rotating every round. Load average is quoted
per table and ran **2.9–24.6** across the session — so read `Ginstr/f` (which
reproduces to <0.5 % across loads) whenever a wall figure looks surprising.

Instruments: `--deferred_prof=1 --hw_prof` for the phase/counter tables,
Instruments Time Profiler for the per-symbol tables, `--prof_*` gates for the
kernel interior. Drivers are committed: `scratchpad/prof1.py` (interleaved
multi-arm runner + report; arms are JSON argv **lists**, so the zsh
word-splitting scar cannot recur), `scratchpad/prof1_arms.py` (the arm matrix),
`scratchpad/prof1_symbols.py` (xctrace record + the id/ref-resolving export
parser).

### THE RANKED TABLE

`ms` is that item's own `wall_min` at the named pose. "Bound by" is measured, not
argued: IPC against the box's ladder (2.42 compute / 0.96 L1 / 0.14 L2), `effPar`
against 12 workers, and per-symbol self time.

| # | item | scene / pose / arm | ms | Ginstr/f | bound by | achievable (INFERRED) | attack |
|---|---|---|--:|--:|---|---|---|
| 1 | **deferred omni loop** (per-light shading + shadow taps) inside `lighting-w1` | greets t=5743 flat | **19.9** of 47.8 `renderFrame` | 2.490 | compute; IPC 3.51, `effPar` 11.4/12 — no idle, no stall | 4–6 ms | **PARTLY DONE — 2026-08-15, docs/OPTIMIZATION_BACKLOG.md 2026-08-15.** Itemized by a committed staged-continue ladder (`-DFDS_OMNI_ABLATE=n`, two sessions agreeing to 0.10 %), which independently reproduces this cell's 2.490 as 2.486. **The loop is a SHADOW loop: 73.5 % of it is the shadow chain at this pose (67.7 % at his), and the largest non-shadow item is the specular lobe at 10.6 %** — the attenuation / N·L / cone / portal / accumulate that this row hoped to find are 12 % between them. Shipped: `computeMapShadowAtten` is skipped when its three index planes are all absent, which `--omni_census` says is **99.50 % of its 4.749 M calls a frame** — bit-exact, no flag, **−1.75 ms `lighting-w1` / −1.78 ms frame here, −22.5 % of chase's `lighting-w1`**, city + fountain neutral. What is left is the cube tap (row 5) and it is closed to cheap edits — see the refutation there |
| 2 | **volumetric cones** | chase t=800 | ~~21.6~~ → ~~17.6~~ → **16.0** of 46.9 (×2 passes) | ~~2.817~~ → ~~2.192~~ → **1.992** | compute; IPC 4.29; was **46.6 % of chase's entire CPU self-time** | 2–4 ms | **PARTLY DONE — round 6 (2026-08-14), docs/HW_PROFILING.md §14.** The guess in this cell was right: chase runs 32 NARROW cones of 34 spots, so 91 % of its pairs took the *segmented* branch, and `--vol_cone_solve_vec` was gated `!segPath` on a round-1 GREETS measurement. Un-gating it + a new `FDS_CONE_QUADEARLYOUT` (round 1's early-out moved from the range sphere, which rejects 0.0 % of chase's pairs, to just after the cone quadratic, which rejects 83.9 %) took the cone pass **−18.4 % wall / −21.6 % instructions / −19.6 % cycles** at t=800, −19.6 %/−21.2 % at t=400/t=100, −9.4 % at t=1200, **+4.1 % at t=1600** (no segmented pairs there, 3.4 % fire rate, 3.1 ms pass). greets IMPROVED (−9.7 % at its pin pose), city cycles −0.6 %. Judge call on bytes: chase t100/t400/t800 and greets pins moved by 20–381 px at max |Δ| 2/255. **ROUND 7 (2026-08-15), docs/HW_PROFILING.md §15** then cashed the parked prologue: the spot loop is the INNERMOST of three, so 12 SoA loads, two dot products and `1/(cosI−cosO)` ran per (batch × spot) for values that depend only on the LIGHT. A per-tile `ConeSpotPre` record collapses 10 800 evaluations per tile to one — **cones −9.1 % instructions / −7.6 % cycles / −7.9 % wall at chase t=800, −10.7 % at t=400, and −8.1 % instructions on CITY too** (the first cone change that helps all three cone scenes: the prologue ran for every pair regardless of branch). **BIT-EXACT — all eight pins unmoved, render_gate 4/4 byte-identical, no judge call.** **WHAT IS LEFT**: the 8-segment body is ~31 % of the pass; its `W²`/`D·W` closed form was built and REFUTED (+0.1..+0.7 % instructions — that loop runs only on ALIVE pairs, 8.1 % of chase's — see §15.5); the per-spot shadow-map block behind `spotAlive` is unpriced; greets' `shadowed=51` per-segment tap is unpriced. |
| 3 | ~~**two-layer transparent lighting** (`TBR-render`)~~ **CLOSED 2026-08-14** | fountain t=1200 | ~~**21.7** of 28.1 (77 %)~~ → **8.17 of 13.27** | ~~3.032~~ → **1.081** | — | **−11.99 ms taken (−43.7 % of the frame)** | `<0>`/`<1>` are front-/back-FACING, not depth layers; the depth peel is `XparPeelPasses=4` on top. Cause: the per-strip dispatch bounded Y and **only** Y, so all 12 884 composite invocations a frame scanned the full 1920 px strip width — **197.90 M px scanned for 0.97 M live fragments (0.491 %)** — and peel passes 1–3 were 100 % empty. Fixed byte-null by `--xpar_strip_extent` + `--xpar_peel_early_out` (both default ON). See `docs/OPTIMIZATION_BACKLOG.md` 2026-08-14b |
| 4 | **volumetric cones** | city t=1961 | **16.6** of 62.7 (×2 passes) | 2.257 | dependency chain, not op count (HW_PROFILING §13) | 1.5–3 ms | closed as a spelling problem. Only "do less work" is left: fewer (px × spot) pairs. Culling is capped at ~3 %, unroll-and-jam at ~8 % |
| 5 | **cube shadow sampling** inside the omni loop | greets t=5743 flat | **9.1** (6.8 of it the cube tap, 2.1 the 3 extra PCF taps) | 1.159 | compute; per-symbol `CubeShadow_Sample` 16.1 % + `resolveCubeAtten` 6.1 % + `computeMapShadowAtten` 4.0 % = **26 %** of DEMO self-time | 2–3 ms | **RE-MEASURED 2026-08-15: the cube tap alone is 1.422 Ginstr/f = 43.8 % of `lighting-w1`, not the 24.5 % `--prof_no_cube_tap` reported** — that diagnostic returns *fully lit*, so the 54.6 % of taps that come back shadowed stop taking their `continue` and pay diffuse+specular in the no-tap arm (2.59 M extra shaded pairs a frame, 0.385 Gi of the gap). The interior is now REFUTED as a target: adding one bool to skip the provably-all-zero dynamic plane — four loads and eight compares per tap, algebraically byte-null — measured **+12.4 % of `lighting-w1`'s instructions**, the third independent instance of the tap being at its register-allocation limit. **Fewer taps is the only lever**; the census gives the parked 8×8 PolyId-uniformity pyramid its denominator (76.1 % of taps sit in an 8×8-uniform block) |
| 6 | **non-light kernel remainder** (G-buffer decode, matID→`Material*`, normal/metal/rough/AO/horizon fetches, ambient+SH, env compose, store) | greets t=5743 flat | **9.9** | 0.801 | compute; IPC ~2.6 in this slice | 1–2 ms | this is the one interior slice that **grew** since 2026-08-08 (8.28 → 9.88 ms). Not root-caused. Bisect it before optimising it |
| 7 | **water simulation + glints** | chase t=800 / city t=1961 | ~~chase 16.9 % of CPU~~ → chase **14.195 → 10.021**; city glints **7.720 → 4.602**, city ripple **4.015 → 3.117** | chase 1.128, city 0.434 + 0.406 | **was PARALLELISM** — contiguous row bands over a screen whose top half is a 10-instruction reject; now compute, `effPar` 11.1/12 | — | **DONE 2026-08-15d, docs/SESSION_STATE.md.** The passes had no phase row at all (they run outside `renderFrame`); `--water_census` + `runRowBands`' `effPar` column are the instruments that land with the fix. **−4.174 ms chase / −4.016 ms city / −3.550 ms of city's FRAME, BIT-EXACT** (eight pins 3/3, render_gate 4/4): 8-row dynamic chunks off an atomic cursor, plus a provable `ndhMin` skip ahead of `powf`. **This cell's premise was wrong twice**: greets has NO water, and the full-screen scan is NOT the fountain 198M-px pattern — the reject path is ~10 instructions against ~1050 per LIVE pixel, so all three bit-exact scan-side levers measured flat (0.7–1.4 %). **2026-08-15e** then took the caustic sampler (three bilinear channel taps collapsed to one number by every caller) for a further −16.9 % chase / −11.4 % city — a judge call at **7 px of 12.4 M, all |Δ|=1/255**, moving four pin values. **End to end: chase 17.559 → 9.175 (−47.7 %), city glints 8.035 → 4.574 (−43.1 %), city ripple 4.561 → 3.557 (−22.0 %).** What is left is **five libm calls per live pixel** in the chase path, priced at −20.1 % instructions for the four swell transcendentals |
| 8 | **fastfog** | city t=1961 | **10.0** | 1.105 | compute; IPC 3.51 | 1–2 ms | **never per-symbol profiled before this round.** It resolves: `FastFog_SampleGrid` 4.3 %, `Froxel_CompositePixel` 4.1 %, `vFogNoise` 3.9 %, the three `Render_DeferredFastFog` tile lambdas 5.9 %, `SkyPaint` 1.8 %, `vBlobNoise` 0.7 %. **The noise is the cost** — `vFogNoise`+`vBlobNoise` ≈ 4.6 % of city's CPU |
| 9 | **G-buffer fill parallelism** | chase t=800 | 11.3 ms at `effPar` **5.5 of 12** | 0.602 | **parallelism**, not compute — half the pool is idle | **NOT 3–5 ms: the uniform-finer-grid fix is REFUTED (2026-08-14)** | the raster grid is a fixed **6×5 = 30 tiles** (`RENDER.CPP:449`). A 12×10 probe build does raise `effPar` 5.2 → 9.1 on chase t=800 — and costs **+41 % wall, +139 % instructions** (thrsum 57.2 → 141.9 ms), because each clipper tile re-walks the whole face list. Not a serial section. What is left: split only the HEAVY tiles, or cut per-tile traversal. Numbers in `docs/SESSION_STATE.md` 2026-08-14c |
| 10 | **`lighting-w2`** (checkerboard fill wave 2) | greets t=5743 flat | **3.2** | 0.498 | compute; per-symbol `Render_DeferredLighting_TileFill` 7.3 % | 0.5–1 ms | `TileFill` is always scalar and its fallback replays the full wave-1 kernel. Turning checkerboard OFF costs 53.1 → 79.3 ms — do not propose that |
| 11 | `shadow-bake` | greets t=5743 | 2.4 | 0.205 | compute, `effPar` 8.3/12 | — | mature |
| 12 | `bloom-chain` | greets t=5743 | 1.78 | 0.220 | compute, IPC 4.28 | — | unattacked; sat outside every timer until 2026-08 |
| 13 | mirror `RTT` | greets t=5743 | 1.53 (flat) / 2.09 (displaced) | 0.018 | — | — | 0.00 ms in §0; it is nonzero now because `7953bab5` moved `setDefault(mirror_rtt)` into the INIT block. Expected, priced, not a defect |
| 14 | `mirror-grid` | greets t=5743 | 0.67 | — | full-res scalar scan of the mirrorMask plane, every frame | 0.3–0.5 ms | still unattacked since the backlog flagged it |
| 15 | `gbuf-clear` | city t=1961 | 0.75 | 0.009 | **memory** — IPC **0.61**, the one bandwidth-bound phase in the tree | — | the instrument's own control: it proves IPC discriminates rather than reading 3.5 everywhere |

### Per-scene phase tables

**greets t=5743, 1920×1080, load 3.5→18.9.** Arms: flat / `--greets_displace` /
+`--greets_displace_free_edge --greets_displace_border_mean=2 --greets_displace_seam_weld`
/ + `--mip_aniso --texture_filter=1`.

| phase | flat | disp | dispfull | dispfull+mip |
|---|--:|--:|--:|--:|
| **frame min** | **55.59** | **57.99** | **58.69** | **59.94** |
| `renderFrame` | 48.26 | 49.15 | 49.40 | 50.48 |
| `gbuffer` | 5.97 | 9.93 | 9.76 | 11.79 |
| `DeferredLighting-call` | 34.94 | 32.73 | 32.26 | 31.84 |
| ⤷ `lighting-w1` | 29.35 | 27.55 | 27.22 | 26.63 |
| ⤷ `lighting-w2` | 3.22 | 3.07 | 3.08 | 3.07 |
| `cones` | 1.24 | 1.24 | 1.48 | 1.44 |
| `bloom-chain` | 1.78 | 1.76 | 1.76 | 1.76 |
| `tonemap-post` | 0.69 | 0.69 | 0.70 | 0.69 |
| `TBR-render` | 0.58 | 0.49 | 0.51 | 0.48 |
| `mirror-grid` | 0.67 | 0.71 | 0.71 | 0.70 |
| `gbuf-clear` | 0.34 | 0.35 | 0.35 | 0.35 |
| `hdr-begin` | 0.26 | 0.26 | 0.26 | 0.26 |
| — outside `renderFrame` — | | | | |
| `shadow-bake` | 2.41 | 2.44 | 2.47 | 2.37 |
| `RTT` | 1.53 | 2.09 | 2.19 | 2.14 |
| `Tick-Light` | 0.30 | 0.86 | 0.90 | 0.91 |
| `Tick-Xfrm` | 0.26 | 0.45 | 0.48 | 0.49 |
| `Tick-Radix` | 0.07 | 0.28 | 0.30 | 0.30 |
| `Ginstr/f` (`renderFrame`) | 4.964 | 5.44 | 5.54 | 5.72 |
| `IPC` (`lighting-w1`) | 3.51 | 3.55 | 3.55 | 3.56 |
| `effPar` (`lighting-w1`) | 11.4 | 11.4 | 11.5 | 11.4 |

**The displaced arm shades FASTER and rasterises SLOWER.** `gbuffer` +3.96 ms,
`lighting-w1` −1.80 ms. Displaced stone puts more geometry through the raster and
changes which materials cover the screen; the net at this pose is +2.40 ms.

**greets t=3122, 1512×848, `FDS_GREETS_CAM="-8.6249094,…"` — the user's real
workload.** Load 10.4→7.6.

| phase | flat | disp | dispfull | dispfull+mip |
|---|--:|--:|--:|--:|
| **frame min** | **50.97** | **54.32** | **54.71** | **54.69** |
| `renderFrame` | 43.85 | 45.14 | 44.85 | 44.79 |
| `lighting-w1` | 17.95 | 18.86 | 17.76 | 16.92 |
| **`cones`** | **7.11** | 7.16 | 6.89 | 6.49 |
| **`TBR-render`** | **6.15** | 6.25 | 6.93 | 6.44 |
| `gbuffer` | 4.02 | 4.55 | 4.53 | 5.62 |
| `RTT` | 2.05 | 2.86 | 2.78 | 2.81 |
| `lighting-w2` | 1.96 | 2.17 | 1.94 | 2.05 |
| `bloom-chain` | 1.22 | 1.26 | 1.21 | 1.27 |
| `Ginstr/f` (`renderFrame`) | 5.074 | 5.116 | 5.116 | 5.210 |

His pose is a **different frame from t=5743**: cones 7.1 ms and TBR 6.2 ms
(1.185 Ginstr — the shards) are first-class items here and near-zero at t=5743.
Anyone tuning "the greets frame" against t=5743 alone is tuning the wrong frame
for him.

**greets t=6001 — the corner pose, where the new band/weld machinery lands.** Load 7.6→8.7.

| phase | flat | disp | dispfull | dispfull+mip |
|---|--:|--:|--:|--:|
| **frame min** | **44.74** | **46.89** | **51.24** | **52.28** |
| `renderFrame` | 39.80 | 41.07 | 45.30 | 46.57 |
| `gbuffer` | 5.36 | 7.87 | 9.00 | 10.59 |
| `lighting-w1` | 23.63 | 22.68 | 24.88 | 25.03 |
| `cones` | 0.02 | 0.02 | 0.20 | 0.20 |
| `Ginstr/f` (`renderFrame`) | 4.175 | 4.488 | 4.575 | 4.742 |

**city t=1961 / t=2400 / t=400, `--deferred`.** Load 2.9→8.7. `renderFrame` runs
**twice** per city frame; `cones-call` once.

| phase | t=1961 | t=2400 | t=400 |
|---|--:|--:|--:|
| **frame min** | **86.94** | **50.59** | **62.96** |
| `RNDR` | 72.14 | 44.89 | 55.22 |
| `renderFrame` (×2) | 62.75 | 36.67 | 46.93 |
| **`cones-call`** (×1) | **16.65** | 9.05 | 13.20 |
| `DeferredLighting-call` (×2) | 12.35 | 5.40 | 9.08 |
| **`fastfog`** (×1) | **9.98** | 5.55 | 6.29 |
| `gbuffer` (×2) | 9.85 | 5.97 | 5.08 |
| `TBR-render` (×2) | 7.49 | 5.52 | 7.96 |
| `gbuf-clear` | 0.75 | 0.72 | 0.72 |
| `ANIM` | 4.30 | 3.27 | 4.66 |
| `Ginstr/f` (`renderFrame`) | 6.366 | 3.418 | 4.925 |
| `Ginstr/f` (`cones-call`) | 2.257 | 1.222 | 1.691 |

> **`--deferred` is REQUIRED to bench city, fountain and chase.** greets forces the
> deferred path from inside the scene (`greets_mirror` → `Render(ForceDeferred)`);
> the others do not, so a bench line without the flag silently profiles the
> **forward** renderer — no cones, no `DeferredLighting`, no `fastfog`. It reads
> as a 9.3 ms city `renderFrame` and it is not the shipping path. Caught here by
> the missing phases; recorded so the next round does not lose a batch to it.

**chase t=800 / t=1600, `--deferred`.** chase has **no `--bench=scene` arm**
("scene 'chase' not supported") and **no `--repro` wiring**, so it is profiled by
asking the snapshot harness for the same timestamp ten times — the driver
re-ticks and re-renders each one and `--deferred_prof`'s warmup exclusion drops
the cold frame exactly as under `--bench`.

| phase | chase t=800 | chase t=1600 | fountain t=2500 | fountain t=1200 |
|---|--:|--:|--:|--:|
| **frame min** | — | — | **26.51** | **30.73** |
| `renderFrame` | 46.88 (×2) | 20.91 (×2) | 23.03 | 28.12 |
| **`cones-call`** | **21.60** | 3.41 | 0.00 | 0.00 |
| `gbuffer` | 11.30 | 5.76 | 2.64 | 1.57 |
| `DeferredLighting-call` | 6.81 | 3.64 | 1.92 | 3.31 |
| **`TBR-render`** | 2.87 | 5.00 | **16.70** | **21.69** |
| `gbuf-clear` | 0.73 | 0.75 | 0.36 | 0.35 |
| `Ginstr/f` (`renderFrame`) | 4.737 | 1.915 | 2.569 | 3.553 |
| `Ginstr/f` (`TBR-render`) | 0.490 | 0.860 | 2.228 | **3.032** |
| `IPC` (`TBR-render`) | 5.10 | 5.16 | 4.14 | 4.19 |
| `effPar` (`gbuffer`) | **5.5** | **5.5** | **5.2** | 9.5 |

chase's historical description as a FACE-dominated front end no longer holds:
**it is a cone scene**, and its second-largest cost is water glints.

### Per-symbol (Instruments Time Profiler, running samples, self time, DEMO only)

Shares are of DEMO's own running self-time over the whole recorded run, so they
include init (`stbi__do_zlib`, `stbi__create_png_image_raw`, `Initialize_*` are
texture load, not frame work). Read the *ratios between frame symbols*.

| greets t=5743 flat | share | city t=1961 | share |
|---|--:|---|--:|
| `Render_DeferredLighting_Tile` | **35.0 %** | `Render_VolumetricCones_Tile` | **28.5 %** |
| `CubeShadow_Sample` | **16.1 %** | `Render_DeferredLighting_Tile_OuterVec` | **15.1 %** |
| `Render_DeferredLighting_TileFill` | 7.3 % | `meka::TileRasterizer::apply_exact<false>` | 5.3 % |
| `meka::TileRasterizer::apply_exact<true>` | 7.0 % | **`pwater::waterWaveSlope`** | **5.2 %** |
| `resolveCubeAtten` | 6.1 % | `FrustumClipper::Render` | 4.5 % |
| `meka::TileRasterizer::apply_exact<false>` | 4.2 % | **`pwater::RenderGlints` λ** | **4.4 %** |
| `computeMapShadowAtten` | 4.0 % | `FastFog_SampleGrid` | 4.3 % |
| bloom `hdrDispatchRows` | 2.1 % | `Render_DeferredTransparentLighting_Tile<0>` | 4.2 % |
| `Render_VolumetricCones_Tile` | 1.8 % | `Froxel_CompositePixel` | 4.1 % |
| `Render_TonemapToVPage` λ | 1.4 % | `vFogNoise` | 3.9 % |
| `MekaleleShadowDepth` | 1.4 % | `Render_DeferredFastFog` λ$_3 / λ$_2 / λ$_1 | 2.9 / 2.0 / 1.0 % |
| `Transform_Objects` | 1.2 % | `CityScene::updateRippleDispMap` λ | 1.8 % |

| chase t=800 | share | fountain t=1200 | share |
|---|--:|---|--:|
| `Render_VolumetricCones_Tile` | **46.6 %** | `Render_DeferredTransparentLighting_Tile<0>` | **28.2 %** |
| **`pwater::RenderGlintsVaried` λ** | **16.9 %** | `Render_DeferredTransparentLighting_Tile<1>` | **20.9 %** |
| `Render_DeferredLighting_Tile` | 8.5 % | `meka::TileRasterizer::apply_exact<false>` | 16.6 % |
| `FrustumClipper::Render` | 5.2 % | `Render_DeferredLighting_Tile_OuterVec` | 11.6 % |
| `Render_DeferredTransparentLighting_Tile<0>` | 5.1 % | `SpriterRT<32>` | 5.8 % |
| `meka::TileRasterizer::apply_exact<false>` | 4.4 % | `barry::TileRasterizer<Blend1,Tex0,…>` | 2.0 % |
| `Render_DeferredFogPass` λ | 2.7 % | `meka::TileRasterizer::apply_exact<true>` | 1.5 % |
| `computeMapShadowAtten` | 1.8 % | `FrustumClipper::Render` | 1.4 % |

Three of these had never been per-symbol profiled: **city's `fastfog`** (it is
noise + froxel composite + three tile lambdas, not one kernel), **city's
`DeferredLighting`** (one `_OuterVec` monolith at ~100 % self — per-symbol
bottoms out, ablation is the only way inside), and **fountain's `TBR`** (two
template instantiations of the transparent lighting kernel, one per layer).
`Shadow_MaterialSkipsCasting`, 4 % of the lighting stage in the 2026-08-10
profile, is **gone** — `f481db36` hoisted it to a per-matID bitmask and the
symbol no longer appears.

### Inside the greets lighting wave — ablation re-run on today's tree

greets t=5743 flat, load 12.8→9.8, min-of-6. Same method as §0; the shadow diet
and the packed planes have landed since, so these supersede §0's split.

| arm | `renderFrame` | `lighting-w1` | `lighting-w1` Ginstr/f |
|---|--:|--:|--:|
| base | 47.80 | **29.78** | 3.291 |
| `--prof_no_lights` | 27.67 | 9.88 | 0.801 |
| `--no-shadows` | 38.13 | 20.66 | 2.132 |
| `--prof_no_cube_tap` | 40.65 | 23.00 | 2.380 |
| `--shadow_polyid_no_pcf` | 45.51 | 27.65 | 3.064 |
| `--prof_no_spec` | 46.33 | 28.48 | 3.069 |
| `--prof_no_tex` | 47.46 | 29.27 | 3.271 |
| `--no-env_refl` | 47.48 | 29.37 | 3.242 |

| component | ms | Ginstr | share of `lighting-w1` | vs §0 (2026-08-08) |
|---|--:|--:|--:|---|
| omni loop (whole) | **19.91** | 2.490 | 67 % | 19.0–21.25 → **unchanged** |
| ⤷ shadow sampling | 9.12 | 1.159 | 31 % | 10.80 → **−1.7 (the diet + packed planes)** |
| ⤷⤷ cube tap alone | 6.79 | 0.911 | 23 % | 10.28 (2026-08-10) → **−3.5** |
| ⤷⤷ the 3 extra PCF taps | 2.14 | 0.227 | 7 % | 1.80 → +0.3 |
| ⤷ specular | 1.30 | 0.222 | 4 % | 3.07 → **−1.8** |
| non-light remainder | **9.88** | 0.801 | 33 % | 8.26–8.29 → **+1.6, the one interior slice that grew** |
| albedo gather | 0.52 | 0.020 | 2 % | 0.71 → −0.2 |
| env reflection | 0.41 | 0.049 | 1 % | not measured before |

### Tessellation / displacement flag costs — per-flag, MEASURED

greets t=5743, one batch, load 8.8→12.8 (so compare **within** the table). The
instruction column is the load-robust one.

| arm | frame min | `renderFrame` | `renderFrame` Ginstr/f | Δ instr vs `disp` |
|---|--:|--:|--:|--:|
| `--greets_displace` | 61.68 | 52.07 | 5.445 | — |
| + `--greets_displace_free_edge` | 63.13 | 53.13 | 5.521 | **+1.4 %** |
| + `--greets_displace_border_mean=2` | 62.02 | 52.64 | 5.443 | **0.0 %** |
| + `--greets_displace_seam_weld` | 61.66 | 52.36 | 5.445 | **0.0 %** |
| `--no-greets_displace_groove_shade` | 62.06 | 52.63 | 5.426 | −0.3 % |
| `--no-greets_displace_mitre` | 62.29 | 52.84 | 5.441 | −0.1 % |
| full (free_edge + bmean=2 + seam_weld) | 63.17 | 53.47 | 5.536 | **+1.7 %** |
| full + `--mip_aniso` | 63.26 | 53.27 | 5.545 | +1.8 % |
| full + `--texture_filter=1` | 64.85 | 55.18 | 5.724 | **+5.1 %** |

* **`--greets_displace_free_edge` is the only per-frame cost in the family** —
  +1.06 ms `renderFrame`, +1.4 % instructions, and it is *geometry*: it frees
  7 908 border verts and densifies the border profile by +7 017 verts, which the
  raster then pays for (`gbuffer` +0.36 ms, `cones` +0.15 ms).
* `border_mean`, `seam_weld`, `groove_shade` and `mitre` are **per-frame free**
  — they are bake-time shape decisions. The ±0.3 % readings are noise.
* **`--mip_aniso` is free at this pose** (+0.02 % instructions), consistent with
  the +0.12 % recorded at t=5970. It costs at the fan-split sites only.
* **`--texture_filter=1` still costs and still does not pay** — +1.7 ms, +5.1 %
  instructions, all of it in `gbuffer` (10.59 → 12.33). §0's adjudication holds
  on today's tree; do not re-propose it as a perf lever.
* Whole-family cost of tessellation at the three poses, flat → full arm:
  **t=5743 +3.10 ms, his pose +3.74 ms, t=6001 +6.50 ms.**

### Scene-init cost of the displacement bake — the veto-soup worry, priced

`--init_timeline`, min over 5 rounds, greets-entry path.

| mark | flat | `--greets_displace` | full arm | recorded 2026-08-09 |
|---|--:|--:|--:|--:|
| `Initialize_Greets` total | **1 358** | **1 976** | **2 026** | 1 379 flat / 2 469 displaced |
| ├ `DisplaceStoneSubdiv` block | 0 | **440** | **483** | **573** |
| ├ `MakeFacesIndependentByAngle` | 4.5 | — | 54 | — |
| ├ chunking / clustering | 3.6 | — | 77 | — |
| ├ `ShadowMaps_BakeStatic` | 10.7 | — | 32 | — |
| └ shadow/env bake | 13.8 | — | 49 | — |

**VERDICT: the init did not regress; it improved.** The subdivision block is
**483 ms against the 573 ms recorded** before the weld/band/blend/veto machinery
existed, and `Initialize_Greets` is **2 026 ms against 2 469 ms**. The full
machinery — free-edge densification, the abuttal veto's scene-wide face soup at
5 samples per candidate edge, the mitre weld, the corner-band blend — adds
**+43 ms** over plain `--greets_displace` (440 → 483). The O(edges × faces) fear
is not borne out at this scene's size. What the displaced arm actually pays at
init is **downstream** of the subdivision: `MakeFacesIndependentByAngle`
+50 ms, chunking/clustering +74 ms, the static shadow bake +21 ms, the env bake
+35 ms — all of them proportional to the face count the subdivision produced.

### REGRESSION HUNT — a real parent-binary A/B, not a cross-session ms comparison

Parent `a3a72cc5` (2026-08-13 22:25, the tip of cone round 5) against HEAD
`a9ca03dc`, **both binaries built in one worktree and run against one asset tree**
(`git diff a3a72cc5 a9ca03dc -- Runtime/` is empty, so this is a pure code A/B),
interleaved round-robin, min-of-6, load 24.6→13.2. The window spans every
2026-08-14 landing: the mitre weld, the band pre-split, the corner-band blend,
groove shading, the edge-notch densification, free_edge rounds 4–5, and the env
water mask.

| arm | parent frame | HEAD frame | Δ | parent Ginstr/f | HEAD Ginstr/f | Δ instr |
|---|--:|--:|--:|--:|--:|--:|
| greets t=5743 flat | 55.47 | 55.50 | **+0.03** | 4.964 | 4.962 | **0.0 %** |
| greets t=5743 `--greets_displace` | 63.15 | 62.96 | −0.19 | 5.423 | 5.443 | +0.4 % |
| **greets t=6001 full displace arm** | **43.49** | **50.02** | **+6.53** | **4.455** | **4.577** | **+2.7 %** |
| greets t=3122 his pose `--greets_displace` | 54.33 | 54.03 | −0.30 | 5.112 | 5.115 | 0.0 % |
| city t=1961 `--deferred` | 86.10 | 85.08 | −1.02 | 6.365 | 6.370 | 0.0 % |

**One regression, and it is geometry, not code: +6.53 ms at the corner pose in
the full displacement arm.** `gbuffer` carries it — 7.34 → 8.77 ms wall and
0.947 → 1.020 Ginstr/f (+7.7 %) — with `lighting-w1` +3.24 ms and `cones`
0.02 → 0.20 ms behind it. The 08-14 commits generate more geometry at the corner
than the parent's same flags did, which is what they were written to do; nobody
priced it. **Everything else in the window is perf-null**, including the flat
arm, the plain displaced arm, the user's pose and city.

**The shatter bracket does NOT regress.** `--repro=greets@t=3122
--repro_from=3112 --repro_settle=0`, `FDS_GREETS_SHATTER=1`,
`FDS_GREETS_CAM="28.8,10.8,-62.85,1,0,0"`, `FDS_SHARD_REFL_PROF=1`, second
shatter frame, **min-of-8**:

| | parent | HEAD | recorded anchor |
|---|--:|--:|--:|
| shard bake wall | 12.0 ms | **11.7 ms** | **11.5 ms** |
| `Render_DeferredLighting` core-ms | 99.8 | **94.6** | **94.3** |

The anchor reproduces. **Method note that cost me a wrong answer first:** the
same bracket read **16.5 ms** at min-of-6 in a two-arm batch at load ~8 and
**11.7 ms** at min-of-8 — this measurement needs 8 rounds, and the campaign that
set the anchor said so (min-of-8 paired). Do not quote it from 6.

**What is NOT a regression, stated because the raw numbers look like one.**
greets t=5743 flat `renderFrame` reads **48.3 ms** here against §0's **43.65**,
and `lighting-w1` **29.4** against **27.32**. That is not code:

* the parent binary reads the **same** 48.4 ms in the A/B above, so nothing in
  the 08-13→08-14 window did it;
* `lighting-w1` Ginstr/f is **3.290**, against the **3.271** the shadow diet
  left it at on 2026-08-12 — the load-robust column says the kernel retires the
  instructions it is supposed to;
* cycles read +7 % and wall +7.5 % on flat instructions, which is the signature
  of a busier machine, not a fatter kernel.

Likewise fountain t=2500 `renderFrame` 23.03 against §0's 20.09 and the shard
bake's first reading: same shape, same cause. **Cross-session ms comparisons on
this box are worth less than the Ginstr column; when they disagree, believe the
counter.**

### Levers this round did NOT find

* No new memory-bound phase. Every hot phase reads IPC 3.5–5.2 against a 2.42
  compute anchor. `gbuf-clear` (IPC 0.61) and `hdr-begin` (1.12) are the only
  bandwidth-bound phases and together they are 0.6 ms.
* No barrier-tail problem in greets: `lighting-w1` `effPar` is 11.4/12.
* No init regression (above).
* No trivially-safe one-liner. Nothing in this round is fixable in one line.

---

State of the engine on `feature/static-shadow-lightmaps`, gathered for invasive perf work on
the deferred kernel. Numbers measured at greets `t=500`, 1920×1080, low-poly Piramid (5.5k
faces), chunked at `--greets-piramid-chunk-grid=8`, per-cube-face cull on. Read this top-to-
bottom once; thereafter use the tables.

This is descriptive, not prescriptive. Things that look wasteful are flagged; fixes are not
proposed here.

---

## 0. MEASURED phase split of `renderFrame` — 2026-08-08

Instrument: `--deferred_prof=<warmup>` (`FDS/RENDER/TailProf.h`, see
`docs/GRAPHICS_PIPELINE.md` §8b). 1920×1080, 12 pool workers, scene defaults as
shipped (greets: PBR + HDR + bloom 2.0 + checkerboard + mirror + `cone_fine_tiles`;
city: `cine::kCity`; fountain: `cine::kFountain`).
`--bench=scene@scene=<s>,t=<T>,iters=60`, warmup 5, **min over steady frames, then
min-of-arm over 3 interleaved repetitions.**

**Load: the machine was shared for the whole campaign** (two other agents running
`./DEMO`; 1-min load average 16–57 per run, recorded per row). Serial phases
(`hdr-begin`, `mirror-grid`, `depth-bounds`, `tile-cull`) came out identical to 3
decimals across every arm and act as the internal control; the *parallel* waves are
the load-sensitive ones, which is why every comparison below is interleaved and
min-of-arm. **Sub-0.1 ms phases are not resolvable here and are not interpreted.**

`OTHER` (per-frame `renderFrame` minus Σ of its phases) is **0.048–0.056 ms on every
greets/fountain row and 0.15 ms on city** — i.e. the frame is fully attributed, which
is what makes the rest of the table quotable.

**The instrument is byte-null with the flag off** — verified, not assumed:
`tools/render_gate.sh` 3/3 PASS and the city (`e1221676`) and fountain (`8db68ccb`)
pins unchanged. The greets pin reads `6780642b` on this tree rather than `adfba8ba`,
and that is **not** the instrument: `--hdr_metal_kill=0` reproduces `adfba8ba`
byte-exactly, so the mover is that flag's default of 2 (a separate, intended look
change landed the same day — `docs/SHADING_CONTRACT.md` D1). Values 0/1/2 give
`adfba8ba` / `3d82e4b6` / `6780642b`, all stable 3/3.

### The five poses (ms/frame, `wall_min`)

| phase | greets 5743 | greets 2000 | greets 4200 | fountain 2500 | city 1961 |
|---|--:|--:|--:|--:|--:|
| **renderFrame (= essentially all of RNDR)** | **43.65** | **63.82** | **38.81** | **20.09** | **69.82** |
| G-buffer clear | 0.34 | 0.32 | 0.32 | 0.34 | 0.73 |
| **G-buffer fill (`gbuffer`)** | **5.59** | **4.77** | **5.12** | **2.51** | **9.20** |
| HDR buffer begin (33 MB f32 clear) | 0.13 | 0.13 | 0.13 | — | — |
| **deferred lighting (total)** | **32.59** | **47.14** | **29.18** | **1.74** | **10.48** |
| ⤷ light list + per-tile cull + depth bounds + mirror grid | 1.04 | 1.07 | 1.03 | 0.29 | 0.74 |
| ⤷ shading wave 1 (`lighting-w1`) | 27.32 | 35.57 | 24.25 | 1.36 | 9.38 |
| ⤷ checkerboard fill wave 2 | 3.14 | 9.20 | 3.04 | — | — |
| froxel fog (`fastfog` + sky paint) | — | — | — | — | 10.39 |
| volumetric cones (`cones-call`) | 1.22 | 6.20 | 0.53 | — | **30.72** |
| transparent peel (`xpar-peel`) | 0.08 | 0.05 | 0.05 | 0.14 | 0.25 |
| **TBR (sprites + unified transparents)** | 0.59 | 2.19 | 0.21 | **14.77** | 7.24 |
| bloom chain (DoF+bright+anam+bloom+ghosts) | 1.74 | 1.78 | 1.77 | — | — |
| tonemap + LDR post | 0.69 | 0.67 | 0.69 | — | — |
| everything else (sprite insert, prologue, overlays, edge AA) | <0.03 | <0.03 | <0.03 | 0.16 | 0.06 |
| `OTHER` (unattributed) | 0.049 | 0.051 | 0.049 | 0.053 | 0.149 |
| — outside RNDR — | | | | | |
| shadow cube bake (`BAKE` section) | 2.05 | 3.39 | 2.28 | 0 | 0 |

Notes on shape:
- city's `renderFrame` runs **twice per frame** (the water-reflection underlay plus
  the final view); the column is the per-frame total of both.
- **Mirror RTT and the shadow bake are NOT inside RNDR.** In `GREETS.CPP` the RTT sits
  in `PROF_ANIM` (~L3712) and `ShadowBake_DispatchGreets` in `PROF_BAKE` (~L3813); both
  measured 0.00 and 2.05 ms respectively at t=5743. Nothing hides in RNDR on their behalf.
- fountain is the only scene with a TBR, and it is **73 % of its frame**.

### Wall vs thread-sum (they answer different questions)

`wall` above is ELAPSED on the tick thread and sums to the frame. The parallel waves
also report `thrsum` = Σ tile-task durations (CORE-ms) and `effPar = thrsum/wall`:

| wave (greets 5743) | wall | thrsum | effPar (of 12) |
|---|--:|--:|--:|
| `lighting-w1` | 27.3 | ~370–520 | **10.2–11.4** |
| `lighting-w2` | 3.1 | ~40–49 | 5.9–11.3 |
| `gbuffer` | 5.6 | ~62–76 | 6.7–9.7 |
| `cones` | 1.2 | ~9–13 | 4.1–7.4 |

**The lighting wave is compute-bound and balanced, not barrier-tail-bound** (effPar
10–11 of 12 workers). There is no reclaimable idle there: the fix has to remove
per-pixel work, not rebalance tiles. The `gbuffer` and `cones` waves *do* leave
parallelism on the table (effPar 4–10), and `cones` runs on the coarse 6×4 grid
outside greets.

### Inside the lighting wave — ablation

`--prof_no_lights` (omni loop off) against the same-pose baseline, control phases
matched within 2–9 %:

| pose | `lighting-w1` base | `--prof_no_lights` | ⇒ omni loop | share of frame |
|---|--:|--:|--:|--:|
| greets 5743 | 27.32 | 8.28 | **19.0** | **44 %** |
| greets 2000 | 35.57 | 10.14 | **25.4** | **40 %** |
| city 1961 | 9.38 | 5.99 | 3.4 | 5 % |

So on greets the **per-light loop (cube-shadow taps + per-light PBR) is the single
largest slice of the frame**, and the remaining ~8 ms of the wave is the G-buffer
decode + material resolve + all the map fetches + ambient/SH + env compose.

Three independent sets at greets 5743 (this one, plus the mechanism and shadow sets
below, taken at different loads on different arms) put the omni loop at **19.0 / 21.25
/ 20.61 ms** — call it **~20 ms, 43–46 % of the frame**. The spread is entirely in the
baseline `lighting-w1` (27.32 / 29.50 / 28.90, load-driven); `--prof_no_lights` itself
lands at **8.28 / 8.26 / 8.29 ms across all three**, which is as tight a repeat as this
machine gives and is why the non-light remainder is quoted with confidence.

#### Splitting the omni loop — shadow sampling vs per-light shading

Tightly interleaved, greets t=5743, **3 reps each at load 22–26** (the quietest set of
the campaign; controls `hdr-begin` 0.128–0.129, `mirror-grid` 0.645–0.661,
`bloom-chain` 1.82–1.89, `gbuffer` 5.52–5.80 — all matched within 4 %):

| arm | `renderFrame` | `lighting-w1` | removes |
|---|--:|--:|---|
| base | 46.34 | **28.90** | — |
| `--shadow_polyid_no_pcf` | 44.59 | **27.10** | 3 of the 4 PCF taps |
| `--no-shadows` | 35.09 | **18.10** | all shadow sampling |
| `--prof_no_lights` | 25.85 | **8.29** | the whole omni loop |

Differencing the shading wave:

| component | ms | % of `lighting-w1` | % of the 46.3 ms frame |
|---|--:|--:|--:|
| everything before/around the light loop (G-buffer decode, matID→Material\*, normal/metal/rough/AO/horizon fetches, ambient + SH, env compose, store) | **8.29** | 29 % | 18 % |
| per-light shading math, shadows excluded (range/cone tests, attenuation, GGX/Fresnel, accumulate) | **9.81** | 34 % | 21 % |
| shadow sampling (cube taps + lightmap + spot maps) | **10.80** | 37 % | **23 %** |
| ⤷ of which the 3 extra PCF taps | 1.80 | 6 % | 4 % |

The 2026-05 estimate at the top of this document put "per-pixel cube-shadow taps" at
~32 ms and called it the #1 cost; on today's content and defaults it is **10.8 ms** and
it is roughly TIED with the per-light shading math it sits inside. The headline has
changed: no single stage owns the greets frame — shadows 23 %, per-light math 21 %,
the rest of the kernel 18 %, the checkerboard fill wave 7 %, the G-buffer fill 12 %.

### `--texture_filter` 0 / 1 / 2 — MEASURED, and it does NOT pay

Hypothesis under test: the G-buffer stores a texel ADDRESS, so the kernel pays a
dependent random gather per pixel; `--texture_filter>0` makes the rasterizer write a
filtered BGRA plane the kernel reads LINEARLY, which might be a free perf win.

`renderFrame` ms/frame, min-of-arm over 3 interleaved reps:

| pose | tf=0 | tf=1 (bilinear) | tf=2 (trilinear) |
|---|--:|--:|--:|
| greets 5743 | **43.65** | 44.58 | 45.36 |
| greets 2000 | **63.82** | 65.85 | 65.87 |
| greets 4200 | **38.81** | 39.68 | 41.17 |
| fountain 2500 | 20.09 | **20.03** | 21.09 |
| city 1961 | **69.82** | 71.32 | 73.79 |

It **costs** 0.9–4.0 ms and never wins. The two phases that move say why:

| pose | `lighting-w1` tf0→tf1→tf2 | `gbuffer` tf0→tf1→tf2 |
|---|---|---|
| greets 5743 | 27.32 → 27.29 → 27.53 | 5.59 → 6.76 → 6.86 |
| greets 2000 | 35.57 → 35.52 → 35.93 | 4.77 → 6.11 → 6.23 |
| greets 4200 | 24.25 → 23.99 → 25.03 | 5.12 → 6.52 → 6.71 |
| city 1961 | 9.38 → 9.39 → 9.59 | 9.20 → 10.06 → 10.95 |
| fountain 2500 | 1.36 → 1.32 → 1.39 | 2.51 → 2.84 → 3.07 |

**The kernel does not get faster — at all, at any pose.** The mechanism: the filtered
plane replaces only the *albedo* gather. The normal, metal, roughness, AO and horizon
maps are still fetched at the same `Mipmap[miplevel][swizzledUV]` address
(`DeferredSurfaceKernel.cpp` ~1659/2519/2535/1905/1972), so the dependent address
chase and its miss pattern survive intact — one of five gathers removed changes
nothing measurable. Meanwhile the raster pass pays a consistent **+1.2–1.4 ms** to
bilinear-sample and write an extra full-res BGRA plane.

`--texture_filter` remains a QUALITY flag (it fixes texel crawl). It is not a perf
lever, and it should not be defaulted on for performance reasons.

#### The direct proof: `--prof_no_tex` makes the kernel no faster

A separate, TIGHTLY interleaved set at greets t=5743 (every arm back-to-back inside
each rep so they share the same competing load; 4 reps; control phases `hdr-begin`
0.129-0.130, `cones-call` 1.218-1.274, `TBR-render` 0.619-0.650 confirm comparability):

| arm | `renderFrame` | `lighting-w1` | `gbuffer` |
|---|--:|--:|--:|
| baseline | 46.42 | **29.50** | 5.93 |
| `--prof_no_tex` (albedo fetch -> constant) | 47.47 | **28.79** | 5.87 |
| `--texture_filter=1` | 49.13 | **29.46** | 7.30 |
| `--texture_filter=1 --prof_no_tex` | 47.51 | 28.92 | 7.23 |
| `--prof_no_spec` | 44.42 | 26.43 | 5.50 |
| `--prof_no_lights` | 25.53 | **8.26** | 5.59 |

Two rows kill candidate (C) between them:

- **The albedo gather is worth only 0.71 ms** (29.50 - 28.79 = 2.4 % of the wave,
  1.5 % of the frame). Deleting it *outright* - not replacing it, deleting it - buys
  0.7 ms. That is the entire prize the filtered-albedo plane is competing for.
- **The filtered plane does not even collect it: 29.46 vs 29.50, a 0.14 % difference**,
  while `gbuffer` pays +1.37 ms for the extra plane. Net loss by construction.

The reason is structural, not incidental: the plane replaces one of *five* fetches at
the same `Mipmap[miplevel][swizzledUV]` address - normal, metal, roughness, AO and
horizon maps all still chase it (`DeferredSurfaceKernel.cpp` ~1659 / 2519 / 2535 /
1905 / 1972). Removing one of five leaves the address chase and its miss pattern
intact.

Same set, other splits: **specular = 3.07 ms** (29.50 - 26.43, 10 % of the wave);
**omni loop = 21.25 ms** (29.50 - 8.26, **72 % of the wave, 46 % of the frame**),
which agrees with the independent 20.61 ms from the shadow set above (different arms,
different load) and whose non-light remainder agrees to 8.26 vs 8.29.

### Anchor — the `RNDR 64.017` in GPU_BENCHMARK_PLAN §6.2c, split

§6.2c's CPU column was taken at greets t=5743 under the matched-tier flags
(`--no-greets_mirror --no-mirror_rtt --no-greets_disco --no-parallax
--no-shadow_lightmap --no-deferred_checkerboard`). Re-run today on this tree
alongside the shipped-defaults arm, 3 interleaved reps:

| | shipped defaults | §6.2c tier C | §6.2c reported |
|---|--:|--:|--:|
| frame min | 50.59 | **66.67** | 67.61 |
| `RNDR` min | 46.72 | **62.93** | 64.017 |
| `BAKE` min | 3.06 | 3.04 | 2.992 |
| `renderFrame` | 46.10 | 62.80 | — |
| ⤷ `lighting-w1` | 28.83 | **56.22** | — |
| ⤷ `gbuffer` | 5.69 | 2.30 | — |
| ⤷ `lighting-w2` (checkerboard fill) | 3.35 | n/a (full rate) | — |

So §6.2c reproduces within **1.7 %**, and its 64 ms is **89.5 % one thing: the
deferred lighting shading wave** at full shading rate. (The tier's `--no-greets_mirror`
also removes the mirror clone geometry, which is why its G-buffer fill is 2.30 vs 5.69
— the clone costs ~3.4 ms of raster in the shipped configuration.)

### Per-tile light census (`FDS_TILE_LIGHT_PROF=1`), 12×8 grid = 160×135 px

| pose | view lights | avg/tile | avg/non-empty tile | max |
|---|--:|--:|--:|--:|
| greets 5743 | 117 | 6.9 | 8.3 | 39 |
| greets 2000 | 117 | 7.3 | 8.8 | 41 |
| city 1961 | 76 | 26.1 | 33.0 | 53 |

**Read this next to the omni-loop table above and it settles an argument:** city
carries **3.8× more lights per tile than greets** and its omni loop costs **3.4 ms**;
greets carries 6.9 and its omni loop costs **19–21 ms**. Whatever sets the per-light
cost, it is **not the per-tile light count** — the two move in opposite directions by
a factor of ~23. Any proposal that attacks lights-per-pixel has to get past this row
first.

I did **not** determine why the per-light cost differs so much between the two
scenes, and will not guess: the obvious candidate (greets on the scalar path, city on
the 8-wide vec path) is **wrong** — `deferred_vec` defaults OFF on arm64
(`FDS_DEFERRED_VEC_DEFAULT`, FeatureFlags.h), so both scenes run the scalar per-light
loop on this machine. Plausible remaining differences (unmeasured): how many of each
tile's lights survive the per-pixel range/cone test, how many pixels are sky
(`zEnc == 0`, skipped entirely), and how many lights carry a cube shadow map. **The
per-pixel surviving-light count is the measurement candidate (B) actually needs and
it does not exist yet** — the census above counts per TILE, not per pixel.

### `--cone_fine_tiles` on city — measured, no win

The cone pass is 30.7 ms = 44 % of the city frame and runs on the coarse 6×4 grid
there (greets defaults it to 12×8, where it was worth ~8 %). Interleaved A/B at
city t=1961, 3 reps each, load 28–37 — an unusually stable pair (`renderFrame` spread
0.8 % within each arm):

| | coarse 6×4 | fine 12×8 |
|---|--:|--:|
| `cones-call` | 30.866 | 30.967 |
| `renderFrame` | 70.837 | 71.399 |
| `RNDR` min | 79.763 | 79.136 |

**No gain — the greets result does not transfer.** The two arms are within 0.8 %,
i.e. inside the run-to-run spread. city's cone cost is not a tile-balance problem.

---

## 1. Pipeline overview — one greets frame in deferred mode

| # | Stage | File | Function | Threading | ms (greets t=500, 1080p, full shadows) |
|--:|---|---|---|---|--:|
| 1 | Splines + animation | `FDS/RENDER/Transform.cpp` | `Animate_Objects` | single | ~0 |
| 2 | Per-frame greets driver (robot spot tracking, orbit spots) | `DEMO/GREETS.CPP` | inline in `Tick_Greets` (~L1732+) | single | ~0 |
| 3 | Main camera transform/cull | `FDS/RENDER/Transform.cpp` | `Transform_Objects(GreetSc, g_mainCamera, g_mainFaces)` | per-mesh thread tasks | ~few |
| 4 | Lighting (per-vertex, forward) | `FDS/RENDER/Lighting.cpp` | `Lighting(GreetSc)` | single | low — deferred path skips most of it |
| 5 | Radix sort of FList | `FDS/SORTS` | `Radix_Sort` | single | low |
| 6 | Main rasterize (G-buffer fill) | `FDS/RENDER/RENDER.CPP` L344-358 → `RenderInnerMekalele` → `Mekalele.h::Mekalele` | tiled 6×4 over threadpool | included in baseline-no-shadow |
| 7 | Dynamic-omni shadow bake | `GREETS.CPP:1850` → `FDS/RENDER/Shadows.cpp::Render_DeferredShadowMaps(_, DynamicOmnisPerFrame)` | per-light task (Phase A) + light×tile task (Phase B) | ~12.5 ms |
| 8 | Dynamic-mesh-into-static bake | `GREETS.CPP:1856` (gated on `--shadow-dynamic`) | as above, mode `DynamicMeshesPerFrame` | ~0 (off by default in measurements above) |
| 9 | Deferred lighting kernel | `FDS/RENDER/DeferredLighting.cpp::Render_DeferredLighting` (L3032) | tiled 12×8 over threadpool | ~44 ms (12 ms "other" + 32 ms cube tap) |
| 10 | Deferred skybox | same file, `Render_DeferredSkybox` | gated `--deferred-skybox` (off by default) | 0 |
| 11 | Deferred fog | `DeferredLighting.cpp::Render_DeferredFogPass` (L5411) | tiled 6×4 | ~small (Scn_Fogged) |
| 12 | Transparent peel (per mesh, 2-layer G-buffer) | `RENDER.CPP::renderFrame` L431-530 | per-mesh batches → tiled raster + tiled lighting | small for greets |
| 13 | Particles / sprites | inline in renderFrame L534-555 + `TBR_Render` | single + tile | low |
| 14 | Volumetric (cones + halos OR unified) | `DeferredLighting.cpp::Render_VolumetricCones`/`_OmniHalos`/`_DeferredVolumetric` | tiled | depends on flags |
| 15 | Lightmap viz overlay | `FDS/RENDER/LightmapBake.cpp::Render_LightmapViz` | single | 0 when off |
| 16 | Shadow-map debug overlay | `FDS/FILLERS/ShadowMap.cpp::ShadowMap_Overlay` | single | small |
| 17 | SDL flip | `DEMO/SDL2.cpp` | `MainSurf->Flip` → `SDL_UpdateTexture`+`SDL_RenderCopy`+`SDL_RenderPresent` | single | ~vsync |

Baseline-no-shadow = stages 1–6 + 9–17 with `prof_no_cube_tap` and `--no-shadows`. Stage 9 is
still ~12 ms even without any cube taps (see §2). The 33 ms baseline is roughly:

| Component of the 33 ms baseline | rough share |
|---|--:|
| Per-pixel deferred kernel "other" (texel fetch + ambient + omni loop + spec + sat + modulate + store) | ~12 ms |
| G-buffer fill (Mekalele rasterize over 5.5k Piramid + robot faces) | ~few ms |
| Main `Transform_Objects` + sort | low single-digit |
| Fog pass (Scn_Fogged) | low single-digit |
| Volumetric (cones/halos default on for greets) | a few ms — depends on cone_strength / halo_strength |
| Skybox/forward sprite + flip + present | rest |

The 1920×1080 framebuffer is 2.07 Mpx. Even an empty per-pixel pass that just stores 4 bytes
is ~3-4 ms at memory bandwidth, so ~12 ms of "kernel other" is plausibly:
texel gather (random reads into Material->Txtr->Mipmap[mip]) + ambient compute + Mekalele
mat32-decode + ZPage decode + (tiny) per-tile omni loop + spec for the lit pixels +
saturate/modulate/store. The texel-fetch cache miss pattern is the largest single piece.

## 2. The deferred lighting kernel

Entry: `Render_DeferredLighting()` at `DeferredLighting.cpp:3032`.

Per frame the entry does:
1. Build view-space omni list `ViewLightsSoA lights` (L3083-3174). Walks `Sc->OmniHead`,
   transforms each `Omni::IPos` into view space, copies color×ISize, range², 1/range, spot
   cone, world position, `shadowMapIdx`, `cubeShadowIdx`, halo params. Capped at
   `DEFERRED_MAX_LIGHTS=128`.
2. `computeTileDepthBounds` + `buildTileLightLists` populates 12×8 `TileLights` (L3192-3199).
   Each tile gets a compacted SoA of omnis whose screen bounding circle overlaps it.
3. (Optional) `buildStripLightLists` for unified-TBR transparent path.
4. Wave 1: 96 tile jobs into `Render_DeferredLighting_Tile` (L953) or
   `Render_DeferredLighting_Tile_OuterVec` (L2193), selected by `deferredLightingOuterVecEnabled()`.
5. Wave 2 (only if `checkerboard` or `quarter` on): 96 tile jobs into
   `Render_DeferredLighting_TileFill` (L2728).

### Two kernel implementations

| Aspect | Scalar `_Tile` (L953) | OuterVec `_Tile_OuterVec` (L2193) |
|---|---|---|
| Outer loop | scalar pixel-by-pixel | 8 px per row in `__m256` |
| Inner omni loop | scalar with branch-predicted early-out OR (when `deferred_vec=1`) 8 omnis-per-pixel vec body L1268-1399 | 1 omni × 8 pixels per iter L2510-2566 |
| Texel fetch / matTable / ambient | scalar | scalar per-lane (no vgather on arm64/simde) |
| Normal decode (oct → x/y/z) | scalar `meka::oct_decode_u16` | 8-wide via the inline code L2360-2391 |
| Normal-map (TBN) | scalar block L1108-1152 | per-lane scalar L2400-2457 — forces lanes through scalar fallback in the omni loop |
| Spec | per-omni `pow_glossClass` (template ladder by gloss) | spec lanes go through full **scalar fallback** L2612-2694 — the OuterVec omni loop is short-circuited for them |
| Water | special case `isWater` skip + half-blend | same lanes go through scalar fallback |
| 2D spot shadow tap | scalar inline at L1521-1640 | NOT in OuterVec vec body; only reached via scalar fallback (and the fallback inside L2612-2694 does not run the 2D shadow tap either — see §8 quirk) |
| Cube shadow tap (`resolveCubeAtten`) | scalar L1646-1657 (called once per omni per pixel) | scalarized lane-by-lane L1349-1378 *inside* the inner-vec body of the standard tile when `deferred_vec=1`, plus the OuterVec spec fallback omits cube tap entirely (see §8) |

`deferredLightingOuterVecEnabled()` (L737-744) tri-state:
- Explicit CLI/env `--deferred-outer-vec` wins.
- Else `CurScene->PreferOuterVec`.
- City / Crash / Fountain set it to 1 (matte/spec-light scenes win).
- Greets does NOT set it → defaults to 0 → greets runs the **scalar `_Tile`**.

This matches memory tag `project_outervec_incorrect`: OuterVec matches std visually (0.42%
pixel diff) but is ~2 ms slower on greets because most greets pixels are spec+nmap → scalar
fallback path. Off by default for greets.

### Per-pixel stages in scalar `_Tile` (the actual greets kernel)

Reading the code top-to-bottom in `Render_DeferredLighting_Tile`, every alive pixel goes
through (assuming default flags + matte path):

| Step | Lines | What happens | SIMD? |
|---|---|---|---|
| Wave-skip test | 998-1001 | early `continue` for odd cells if `checker`/`quarter` | scalar |
| Z decode | 1003-1006 | one load `ZPage16[i]`, branch on `zEnc==0` | scalar |
| mat32 decode | 1008-1011 | one load, 3 shifts | scalar |
| matTable bounds + Material* + Txtr null guards | 1012-1014 | one indirection | scalar |
| `surfaceShadowId` decode | 1023-1025 | one load `gb.shadowMatID[i]` (16b) or fallback | scalar |
| Texel fetch | 1042-1054 | indirect `Mipmap[mip][swizzledUV]` | scalar; **single biggest L1/L2 miss source** |
| Lightmap resolve (`resolvePixelLightmap`) | 1059 → L786 | reads `lightmapMF[i]`+`lightmapST[i]`, indexes scene `staticLMTable` | scalar; quick fall-through when off |
| Normal decode | 1069-1070 | `oct_decode_u16` | scalar |
| Normal-map TBN | 1090-1152 | only when `Mat->NormalMap` ≠ null; reads tangent G-buffer, TBN matmul, renormalize | scalar |
| Reconstruct view-space pos | 1157-1159 | `(0xFF80-zEnc)*invZScale` + projection | scalar |
| Ambient | 1163-1172 | Lumin + Diffuse×Ambient per channel | scalar |
| View dir (for spec) | 1196-1203 | `-pos / |pos|` via `fast_rsqrt` — only if `wantSpecular` | scalar |
| World-pos reconstruct | 1242-1250 | `viewToWorld * pos + cameraWorld` — needed for cube tap | scalar |
| Omni loop | 1252-1697 | one of: scalar branch-predicted (L1480+), vec 8-omnis (L1267+), or skip if `profNoLights`/`isWater` | scalar by default; vec only when `--deferred-vec` (off — slower on arm64) |
| **per-omni** Spot cone | 1499-1506 | scalar smoothstep | |
| **per-omni** 2D shadow tap | 1521-1640 | scalar 2×2 PCF on `sm.depth`+`sm.depth_dynamic`; OR polyId identity test on `sm.polyId`+`sm.polyId_dynamic` | scalar |
| **per-omni** Cube shadow tap | 1646-1657 | `resolveCubeAtten` → lightmap atlas (if applicable + `--shadow-lightmap`) else `CubeShadow_Sample` 4-tap PCF | scalar |
| **per-omni** Spec accumulate | 1666-1695 | half-vector + `pow_glossClass` | scalar |
| Saturate | 1701-1706 | 6 cmps | |
| Modulate | 1721-1726 | `tex*lit/256` | |
| Water composite | 1738-1746 | reads `out[i]` and half-adds | |
| Viz overrides | 1754-1772 | `--viz-normal` / `--viz-tangent` (dev) | |
| Final saturate + store | 1774-1781 | one `out[i] = ...` | |

So a typical greets pixel pays: 1 ZPage load, 1 mat32 load, 1 shadowMatID load, 1 normal
load, 0–2 nmap reads, 1 lightmap-G-buffer-pair load, 0–2 tangent loads, 1 ambient compute,
3 viewToWorld FMA chains, then for each omni in the tile list: ~6 FMAs (Lambertian) +
optional cone smoothstep + optional shadow tap (cube or 2D) + optional `pow_glossClass`.

### Shadow attenuation paths

| Light type | Path | Where | Cost |
|---|---|---|---|
| 2D spot shadow (Light_SpotLight + Omni_CastsShadow) | Scalar inline L1521-1640 (NOT in OuterVec vec body, NOT in TileFill, NOT in OuterVec spec fallback) | per-pixel-per-omni | 4 depth taps + bilinear weights + bias math |
| 2D spot polyId mode | L1579-1600 — replaces depth compare with id-not-equal-and-non-zero | per-pixel-per-omni | 4 polyId taps + identity tests |
| Cube tap (Light_Omni + Omni_CastsShadow + has cube ref) | `resolveCubeAtten` L813-951 | per-pixel-per-omni | as below |
| Cube + lightmap (Light_Omni + Omni_CastsShadow + Omni_StaticShadow + pixel on static mesh + `--shadow-lightmap`) | `pl.lm->sampleBilinear`/`sampleBilinearPlanar`/`sampleNearest` from `StaticShadowLightmap.h` (~L130-220) | per-pixel-per-omni | 4 atlas taps, bilinear blend, no cube projection |
| Cube + lightmap × dynamic (above + `--shadow-dynamic`) | composite: lightmap atlas × `CubeShadow_Sample(dynamicOnly=true)` | per-pixel-per-omni | 1 atlas tap + 1 dynamic cube tap |

**Critical**: the OuterVec kernel does NOT do 2D shadow taps in its vec body. The 2D shadow
attenuation only fires in the scalar `_Tile` and in the per-pixel branch reached via the
`deferred_vec=1` inner-vec path inside that scalar tile fn. The OuterVec's scalar fallback
(L2612-2694 for spec/water lanes) does the omni loop but **skips both shadow attenuation
calls entirely**. Cube shadows are present in `_Tile`'s standard scalar omni loop (L1646)
and the standard vec body (L1349-1378), but absent in OuterVec's omni loop and absent in
OuterVec's scalar fallback. This is consistent with `project_outervec_incorrect` note that
OuterVec matches std visually only on scenes without shadows.

## 3. Sub-pixel sampling modes

CLI flags: `--deferred-checkerboard` and `--deferred-quarter`. Env: `FDS_DEFERRED_CHECKERBOARD`,
`FDS_DEFERRED_QUARTER`. Both default off. Defined in `FeatureFlags.def` L22-23.

`deferredLightingQuarterEnabled()` wins when both are set (L767, L964).

### Checkerboard

Wave 1: `((px ^ py) & 1) == 0` → only even cells shaded. L1000 (scalar `_Tile`), L2261-2270
(OuterVec — uses lane mask).

Wave 2: `Render_DeferredLighting_TileFill` (L2728+) handles odd cells:
- For each odd pixel:
  - Left neighbor's matID and Right neighbor's matID compared against center matID (L2823-2825).
  - If both match: dword-level half-blend `((pL & 0xFEFEFEFE) >> 1) + ((pR & 0xFEFEFEFE) >> 1)`.
  - Else: full fallback shade (replays the wave-1 kernel for this pixel, L2841+).

Note: only the L/R neighbor pattern is implemented for checkerboard; no diagonal/vertical
forms.

User reports this looks acceptable.

### Quarter

Wave 1: `((px | py) & 1) == 0` → only `(even, even)` cells shaded. L1001 + L2272-2279.

Wave 2 patterns (L2754-2816), each requires all neighbors share matID with center; else
fallback:

| Cell parity | Pattern | Taps | Blend |
|---|---|---|---|
| odd_x, even_y | horizontal | `out[i-1]`, `out[i+1]` | `(pL>>1)+(pR>>1)` |
| even_x, odd_y | vertical | `out[i-XRes]`, `out[i+XRes]` | `(pT>>1)+(pB>>1)` |
| odd_x, odd_y | diagonal | 4 corners | `(pTL>>2)+(pTR>>2)+(pBL>>2)+(pBR>>2)` |

User reports: **"the robot looks cartoonish AND the floor looks blocky especially on far
pixels"**.

Likely contributing factors (descriptive, not prescriptive):

- The matID similarity test is on the per-pixel material from the **G-buffer**, not surface
  normals or depth. Two pixels can share matID but live on different faces with very
  different `nGeo` (e.g. opposite walls of a hex column). With 3 of 4 pixels reconstructed by
  averaging colors from same-matID neighbors that have very different lighting, depth-
  discontinuous and orientation-discontinuous boundaries get smeared. On the floor at far
  pixels the same matID covers a large area with steeply receding depth → blocky 2×2 patches
  of identical lit color.
- The fallback runs the **scalar wave-1 kernel** (L2841+), not the vec body, even when
  OuterVec is on. So a quarter wave-2 fallback pixel on greets pays the full scalar shading
  cost. The fallback also re-samples the normal map, ambient, etc — divergent code in a
  tight tile.
- The 4-corner diagonal blend has a 2-bit precision loss per channel.
- TileFill always runs scalar, both waves.

Memory tag `project_strict_fill_test_no_payoff` notes that adding normal/depth checks to
the fill pass cost ~2.5 ms with no visible quality gain; that route is parked.

## 4. Cube shadow tap — `CubeShadow_Sample` at `FILLERS/ShadowMap.h:210`

Steps per call:

| Step | Lines | Notes |
|---|---|---|
| Cube face select | L222 → `CubeShadow_SelectFace` L179-187 | dominant-axis on `worldP - lightWorldP`; 6 ops |
| ShadowMap fetch | L223 | one index `g_cubeShadowRefs[cubeIdx].faceIdx[face]` → `g_shadowMaps[..]` |
| `viewToLight` 3×3 matmul | L225-230 | 9 muls + 6 adds + 3 adds — produces `(lx,ly,lz)` in face view space |
| Near-plane reject | L236 | `lz <= 0.05f → return 1` |
| Face-frustum reject | L245-246 | `|lx|/lz, |ly|/lz` ≤ 1.5 |
| Projection | L247-249 | `smX = cntrX + perspX*lx/lz` and `smY` |
| NaN/sanity guard | L256-275 | `std::abort()` on insanely-out-of-range smX/smY; expensive only if it ever fires |
| Integer trunc | L276-277 | iX, iY |
| In-bounds reject | L278 | iX/iY against `sm.xres-1`, `sm.yres-1` |
| Bilinear weights | L280-285 | fx, fy, w00..w11 |
| **4 polyId taps from static buffer** (`sm.polyId`) | L306-307, 327-330 | 8 bytes (4×u16) |
| **4 polyId taps from dynamic buffer** (`sm.polyId_dynamic`) | L308-309, 336+ | 8 bytes |
| **4 depth taps from static** (`sm.depth`) | L310-311, 331-334 | 8 bytes |
| **4 depth taps from dynamic** (`sm.depth_dynamic`) | L312-313, 336+ | 8 bytes |
| `closestPoly` decision per tap | L316-324 (polyId), L364-367 (depth) | picks polyId of whichever buffer has the bigger occluder Z |
| Occlusion accumulate | L343-346 (polyId) or L368-371 (depth) | 4 cmps + 4 adds |
| Final blend | L373 | `return (occ >= 1) ? 0 : 1 - occ` |

User-noted measurement: ~32 ms total cube-tap time / ~1.44 M taps ≈ **22 ns / tap**, on
greets.

Cache footprint per tap (polyId mode): 4 × u16 polyId + 4 × u16 depth × 2 buffers = 32 bytes
of address space per buffer, but those 4 taps are at iX/iX+1 on rows iY/iY+1 — so 2 cache
lines per buffer × 4 buffers (2 polyId + 2 depth) = up to **8 cache lines per pixel per
omni**, with the four buffer streams (polyId static / polyId dynamic / depth static /
depth dynamic) for the same omni potentially hot at once. In depth mode the polyId loads
are skipped (only ~4 cache lines per pixel per omni).

Flags affecting it:

| Flag | Default | Effect |
|---|---|---|
| `shadow_polyid_no_pcf` | 0 | Single nearest-neighbor tap instead of 4 → ~9 ms saved on greets; jagged silhouettes |
| `shadow_polyid` | on | PolyId identity test instead of depth bias compare |
| `shadow_lightmap` | 0 | Skip cube tap on static mesh × static omni pixels; sample pre-baked atlas instead |
| `shadow_lightmap_planar` | 0 | Bake/sample atlas in world-axis-aligned planar projection |
| `shadow_lightmap_nearest` | 0 | Atlas: nearest instead of bilinear |
| `prof_no_cube_tap` | 0 | Short-circuit `resolveCubeAtten` → return 1.0 (full lit). The user's primary A/B handle. |
| `shadow_dynamic` | 0 | Each frame, re-bake animated meshes into `*_dynamic` buffers; lightmap path composites them on top via `dynamicOnly=true` cube tap |

## 5. Shadow render — `Render_DeferredShadowMaps` at `FDS/RENDER/Shadows.cpp:90`

The bake side. Called per frame from `GREETS.CPP:1850` (and 1856 for `shadow_dynamic`).

Three modes (`ShadowBakeMode`):

| Mode | Touches | Buffer |
|---|---|---|
| `StaticOnce` | static omnis (Omni_StaticShadow) | static `sm.depth`/`sm.polyId` |
| `DynamicOmnisPerFrame` | non-static omnis (mech-attached etc) | static `sm.depth`/`sm.polyId` (full rebake each frame) |
| `DynamicMeshesPerFrame` | static omnis, but **only animated meshes** projected; gated by per-mesh filter inside `Transform_Objects` flipped by `g_inDynamicShadowBake` | parallel `sm.depth_dynamic`/`sm.polyId_dynamic` |

Greets per-frame: `DynamicOmnisPerFrame` always (1850), `DynamicMeshesPerFrame` only when
`--shadow-dynamic` (1856).

`StaticOnce` runs once per scene init from `Initialize_Greets` → `ShadowMaps_BakeStatic` →
`FILLERS/ShadowMap.cpp:442`. Must happen **after** `Animate_Objects` so FLD-loaded omni IPos
splines resolve — see memory tag `project_static_bake_before_animate`.

Inside `Render_DeferredShadowMaps`:

| Phase | Lines | What | Threading |
|---|---|---|---|
| A: setup + Transform_Objects per light | 150-330 | Build per-light camera, swap `lightCtx`, clear depth+polyId buffers, enqueue `Transform_Objects` task per matching light | N tasks (1 per active shadow map) in threadpool |
| Barrier | 322-325 | Wait on `tileCounter` | |
| B: tile rasterize (light × tile) | 332-489 | Flat 6×4 tile job per light. Each task runs `clipper.Render(F, MekaleleShadowDepth, ...)` for each face that passed the mesh-level cull | N×24 tasks (flat) |
| Barrier | 490-494 | Wait | |
| Per-frame precompute `viewToLight` + `viewToLightOffset` | 549-565 | One affine per shadow map, used per pixel by the lighting kernel | single |

The flat (light × tile) batch in Phase B is intentional — straggler lights don't hold up
others. Mesh-level culling fires in `Transform_Objects` because:
- `g_currentShadowOmni` carries the omni pose so `sphereOutsideSpotCone` can reject meshes
  outside the cone.
- `g_currentShadowMap` carries `cubeFace` so the per-cube-face cull can pick the right face
  axis (gated on `--shadow-cube-face-cull`, default **on**).

The `Piramid` (greets wall mesh) chunking is set at scene init: one giant TriMesh becomes
N³ smaller TriMeshes via `--greets-piramid-chunk-grid=8` (default 8 → 512 cells, ~50-150
non-empty in practice). Without it the bsphere-vs-pyramid cull would never fire on the wall.

A `MatShadowCache` (L393-419) skips materials that look emissive (Mat_Transparent /
Mat_Additive / Mat_SkipZ or name-substring "lamp"/"emit*"). Lamp omnis don't self-occlude.

The shadow rasterizer is `MekaleleShadowDepth` (FILLERS/Mekalele.cpp / ShadowMap.h:93)
— depth-only (no color, no G-buffer, no texture). It's the rasterizer-side counterpart to
the deferred kernel's bilinear-PCF sampler.

`shadow_prof` (default off) prints per-frame Transform vs Raster cost rolling avg every
`FDS_SHADOW_PROF_INTERVAL` frames (60 default).

## 6. FeatureFlags inventory (perf-relevant subset)

All flags defined in `FDS/Base/FeatureFlags.def`. Forms: `--<name>`, `--no-<name>`,
`--<name>=value`, plus env `FDS_<NAME>`.

### Deferred

| Flag | Default | What | Note |
|---|---|---|---|
| `deferred` | FDS_DEFERRED_DEFAULT_ON | Master enable for deferred pipeline | |
| `deferred_zcull` | 1 | Cull G-buffer ROP writes against existing Z | |
| `deferred_vec` | 0 | 8-wide SIMD inner omni loop in scalar tile | Slower on arm64-via-simde |
| `deferred_outer_vec` | 0 (else Scene::PreferOuterVec) | 8 px × 1 omni outer kernel | Greets defaults off |
| `deferred_checkerboard` | 0 | Half-rate; see §3 | Works acceptably per user |
| `deferred_quarter` | 0 | Quarter-rate; see §3 | Robot cartoonish + floor blocky per user |
| `deferred_no_spec` | 0 | Skip specular | |
| `deferred_unified_tbr` | 0 | Unified transparent + particle TBR | |
| `deferred_gloss_stats` | 0 | Dump per-scene glossiness histogram | |
| `deferred_tile_stats` | 0 | Dump per-tile lighting cost stats | |
| `deferred_max_range` | 0.0 | Optional Range cap (cull only) | |

### Shadows

| Flag | Default | What |
|---|---|---|
| `shadows` | FDS_SHADOWS_DEFAULT_ON | Master |
| `shadow_polyid` | FDS_SHADOW_POLYID_DEFAULT_ON (on) | PolyId identity test vs depth compare |
| `shadow_polyid_no_pcf` | 0 | 1-tap nearest neighbor instead of 4-tap bilinear PCF |
| `shadow_backface_cull` | 0 | Cull back faces in shadow raster |
| `shadow_validate` | 0 | Capture clipper outputs outside input hull |
| `shadow_prof` | 0 | Per-light timing |
| `shadow_prof_cache` | 0 | Per-frame cache-line transition stats |
| `shadow_cone_cull` | 0 | Mesh bsphere vs spot cone cull |
| `shadow_skip_animated` | 0 | Static bake skips animated meshes |
| `shadow_dynamic` | 0 | Per-frame animated-mesh dynamic bake |
| `shadow_fzp_mult` | 3.0 | Shadow cam FZP multiplier over light IRange |
| `shadow_bias` | 512 | Constant Z bias |
| `shadow_slope_bias` | 1024 | Slope-scale Z bias |
| `shadow_lightmap` | 0 | Pre-baked per-face atlas; lighting reads atlas |
| `shadow_lightmap_res` | 16 | Atlas N×N per face |
| `shadow_lightmap_viz` | 0 | 1..9 different debug viz modes |
| `shadow_lightmap_recompute_bake` | 0 | Debug: skip atlas; call bake sampler per pixel |
| `shadow_lightmap_recompute_at_bary` | 0 | Debug: same but at runtime-stored bary |
| `shadow_lightmap_nearest` | 0 | Atlas nearest sample |
| `shadow_lightmap_planar` | 0 | World-axis-aligned planar projection bake/sample |
| `shadow_cube_face_cull` | 1 | Per-cube-face bsphere cull in Transform_Objects |
| `shadow_cube_vert_cull` | 0 | Per-vertex pyramid cull; only worth it for one giant moving mesh |

### Prof (diagnostic ablation gates)

| Flag | Default | What |
|---|---|---|
| `prof_no_tex` | 0 | Skip texture sample (uses 128/128/128) |
| `prof_no_lights` | 0 | Skip omni loop |
| `prof_no_spec` | 0 | Skip specular term |
| `prof_no_fog` | 0 | Skip fog pass |
| `prof_no_cube_tap` | 0 | `resolveCubeAtten` returns 1.0 |

These are the user's A/B handles; delta vs default = the gated stage's cost.

### Greets-specific

| Flag | Default | What |
|---|---|---|
| `greets_spot_height` | 0 | Override robot spotlight height (0 = scene default 4) |
| `no_greets_spots` | 0 | Skip code-installed robot + orbit spots |
| `greets_omni_shadows` | 0 | Mark all FLD omnis as `Omni_CastsShadow`+`Omni_StaticShadow` |
| `greets_omni_shadow_res` | 256 | Per-face cube map res for static FLD omnis |
| `greets_moving_omni_shadow_res` | 0 (=use above) | Per-face for moving omnis |
| `greets_piramid_chunk_grid` | 8 | N³ chunking of wall mesh; 8 → 512 cells |
| `greets_omni_default_range` | 1500 | Fallback IRange for FLD omnis with empty Range spline |

### Atmospherics (relevant because greets runs cones/halos by default)

| Flag | Default | What |
|---|---|---|
| `draw_cones` | 0 | Volumetric spotlight cones |
| `omni_halo_strength` | 0.0 | Halo brightness scale |
| `volumetric_unified` | 0 | Beer-Lambert unified pass |
| `vol_n_samples` | 4 | Ray-march samples |
| `vol_vec` | 1 | 8-wide SIMD per-sample inner loop |
| `vol_rect_cull` | 1 | Screen-rect cull per batch |
| `vol_halo_analytic` | 1 | Closed-form arctan integral |
| `vol_cone_analytic` | 1 | Cone analytic integral |
| `vol_prof` | 0 | Per-frame volumetric timing |
| `deferred_skybox` | 0 | Skybox-from-G-buffer pass |
| `fog_sigma_mult` | 3.0 | Beer-Lambert sigma |

### Display

| Flag | Default | What |
|---|---|---|
| `no_vsync` | 0 | Lets `SDL_RenderPresent` return immediately |

## 7. SIMD coverage map

| Operation | Path | SIMD | Width |
|---|---|---|---|
| G-buffer rasterize (Mekalele tile) | `FILLERS/Mekalele.h` | yes | 8-wide simde/AVX2-NEON; pixel-major SoA tiles |
| Mekalele octant normal encode | `oct_encode_u16_x8` (Mekalele.h:98) | yes | 8 |
| Mekalele octant normal decode | scalar in fallback, vec inline at L463-491 | partial | 8 in vec path; scalar in some leaf cases (`oct_encode_vec` memory tag) |
| Volumetric cones/halos per-sample loop | `Render_VolumetricCones_Tile` | yes (via `vol_vec`) | 8-wide pixel-major |
| Deferred fog pass | `Render_DeferredFogPass_Tile` L3328 | yes | 8 |
| Deferred lighting — scalar tile outer | `Render_DeferredLighting_Tile` | no | 1 |
| Deferred lighting — scalar tile inner omni loop | L1480+ | no | 1 |
| Deferred lighting — `deferred_vec` inner omni loop | L1267-1399 inside scalar tile | yes | 8 omnis × 1 pixel; off by default |
| Deferred lighting — OuterVec body | `_Tile_OuterVec` L2193+ | yes | 8 pixels × 1 omni |
| Deferred lighting — OuterVec normal-map TBN | L2400-2457 | no | per-lane scalar |
| Deferred lighting — OuterVec spec/water lanes | L2612-2694 | no | per-lane scalar fallback (no shadow taps) |
| Spec `pow(N·H, gloss)` | `pow_glossClass` (scalar tile) → template ladder of vec spec loops | yes for `{4,8,16,32,48,64,128}` | 8 omnis at a time |
| Cube shadow tap | scalar | no | 1 |
| 2D shadow tap (depth + polyId) | scalar | no | 1 |
| Atlas lightmap sample | `StaticShadowLightmap::sampleBilinear*` | no | 1 |
| Tile light list build | `buildTileLightLists` | partial | sphere-vs-tile cull is scalar |
| Shadow rasterizer | `MekaleleShadowDepth` (depth-only Mekalele) | yes | 8 — same pixel-major SoA |

## 8. Known issues, bugs, quirks

### Open

- **Shadow viz flicker even when paused** (task #83 created today). Mode is independent of
  scene tick, suggesting either the shadow-map dispatch is producing different output across
  frames despite identical scene state, or the overlay is reading from a buffer that's still
  being written by Phase B. `Render_DeferredShadowMaps` writes `g_shadowMaps[i].depth/polyId`
  with no read-side fence other than the Phase B barrier, but the V_Flip overlay
  (`ShadowMap_Overlay`) is also single-thread on the main thread after Render() returns.
  Still: needs investigation. Memory tag for follow-up if found: not yet created.
- **OuterVec kernel does not run 2D spot shadow attenuation** — the vec body in L2510-2566
  has no shadow tap, and the scalar fallback L2612-2694 also skips it. Cube shadows are
  similarly absent in both OuterVec paths. This is consistent with the description in
  memory tag `project_outervec_incorrect` (matches std visually only because greets uses
  cube shadows on static surfaces baked into the lightmap atlas, and most of the greets
  geometry actually goes through the scalar fallback anyway because of spec/nmap).
- **`std::abort()` in CubeShadow_Sample** (L274). If the smX/smY sanity guard ever fails the
  process dies. Per memory tag `feedback_no_defensive_backstops` this is intentional; called
  out here because it's not a typical fallback.

### Resolved / parked

- `project_outervec_incorrect` — OuterVec ~2 ms slower on greets (25 vs 23 ms); off by default.
- `project_strict_fill_test_no_payoff` — normal+depth checks on TileFill cost ~2.5 ms with
  no visible quality gain; reverted.
- `feedback_clipper_bary_lightmap` — Mekalele clipper invalidates per-pixel bary for atlas
  lookups when triangle is subdivided; `--shadow-lightmap-no-clipped` (default on) gates.
- `project_volumetric_simd_no_payoff` — pixel-major SIMD across rays wins ~21% at any N; on by default.
- `project_halo_analytic` — analytic atan integral replaces N-sample ray-march; on by default.

### Source-comment markers

`grep -rn 'TODO\|FIXME\|XXX'` in DeferredLighting.cpp + Shadows.cpp + ShadowMap.h returned
no hits — comments are mostly explanatory paragraphs, not deferred work markers.

### Quirks to keep in mind

- `Render_DeferredLighting` calls `getenv`-backed `FeatureFlags::xxx()` per pixel in
  multiple places. The accessors are cached-on-first-access, but per memory tag
  `feedback_use_feature_flags_for_hot_toggles` adding new hot-loop env reads must use
  FeatureFlags registry, not naked `getenv`. The kernel hoists every flag it touches to
  the top of `_Tile` (L963-994), but `resolveCubeAtten` re-reads several flags
  (`prof_no_cube_tap`, `shadow_lightmap_recompute_bake`, `shadow_lightmap_recompute_at_bary`,
  `shadow_lightmap_planar`, `shadow_lightmap_nearest`, `shadow_dynamic`) **per omni per
  pixel** when cube taps are reached. The FeatureFlags::xxx() calls are cached loads but
  not free, and the `prof_no_cube_tap` short-circuit is rechecked every tap. Same for the
  `g_shadowMode.load()` atomic in `CubeShadow_Sample` and `resolveCubeAtten`.
- TileFill always runs scalar even when wave 1 was OuterVec. Quarter-mode wave-2 cost is
  always paid in scalar.
- `Mat->Glossiness` not in `{4,8,16,32,48,64,128}` falls through to scalar libm `std::pow`
  in the `deferred_vec=1` path's default arm (L1452-1466). Should be impossible for shipping
  scenes (gated by `deferred_gloss_stats`).
- The greets per-frame call order is hard-coded in `GREETS.CPP::Tick_Greets`: Animate →
  greets-driver → Transform_Objects → Lighting → Radix_Sort → `gg->Render()` →
  `Render_DeferredShadowMaps(_, DynamicOmnisPerFrame)` → optional DynamicMeshesPerFrame.
  The shadow bake runs **after** the main Render() call, which means the lighting kernel
  inside Render() sees **last frame's** shadow maps. This is intentional (shadows trail by
  one frame to allow async on the next frame), but is worth flagging if assuming "this
  frame's bake feeds this frame's lighting".

  WAIT — re-reading `RENDER.CPP::renderFrame` and the Greets driver: `gg->Render()` is
  `fds::RenderPipeline::renderFrame` (RENDER.CPP:279). It contains `Render_DeferredLighting()`
  at line 371. So lighting fires *inside* Render(), then Greets calls
  `Render_DeferredShadowMaps` *after*. The first frame's lighting reads zero-initialized
  shadow maps; subsequent frames read N-1's bake. Confirmed by code; flagged as a perf
  surprise.

## 9. Measured numbers — today's snapshot

Greets `t=500`, 1920×1080, low-poly Piramid (5.5k faces), chunks on, cube-face cull on.

| Config | mean ms |
|---|--:|
| No shadows | 33 |
| Dynamic only (per-frame mech omni rebake; `--shadow-dynamic` off) | 90 |
| Lightmap only (`--shadow-lightmap` on; static omnis via atlas) | 73 |
| Both (lightmap + dynamic) | 89 |

Component decomposition:

| Component | ms |
|---|--:|
| Cube tap (delta from `--prof-no-cube-tap` at full shadows) | ~32 |
| Shadow render (`Render_DeferredShadowMaps` Phase A + B) | ~12.5 |
| Kernel "other" (everything in `_Tile` except cube tap + shadow render) | ~12 |
| Baseline-no-shadow (kernel + raster + fog + volumetric + flip etc) | 33 |

960×540 (quarter pixel count): ~33.6 ms.
- Frame floor (non-pixel-bound work + vsync alignment): ~16 ms.
- Pixel-bound work at 540p: ~17.6 ms.
- Linear extrapolation to 1080p: ~70 ms pixel-bound, matches the "both" measurement once
  shadow render is amortized.

Pixel-bound cost is therefore the dominant scaling factor. Resolution halving on each axis
divides the kernel + cube tap cost by ~4 directly.

Cube tap rate: 1920×1080 × ~5 omnis-per-pixel × ~0.15 hit-rate (after tile cull) → ~1.44 M
taps/frame at 32 ms → ~22 ns/tap. This includes the bilinear PCF + closestPoly + bounds
checks; the four cache-line read pattern at 64B/line × 4 lines × ~5 ns/L2-hit accounts for
most of it.

---

## 10. Flicker investigation

Symptom: with greets paused, the shadow viz overlay (B/V keys) shows small patches that
change frame to frame. Inputs (camera, omni positions, geometry, ShadowMatIDs) are constant
across paused frames, so the only way the overlay color can change is if the shadow buffer
(`sm.polyId` / `sm.polyId_dynamic`) itself changes — i.e., Render_DeferredShadowMaps
produces non-deterministic output for identical inputs. Candidates below are ordered by how
directly they explain "polyId differs frame-to-frame at fixed inputs."

### Candidate A — Shared tile-boundary pixels racing in `ShadowBarry::apply_exact` (HIGH)

**Where:** `FDS/FILLERS/ShadowMap.cpp:482-551` (`apply_exact`), dispatched from
`FDS/RENDER/Shadows.cpp:368-489` (Phase B tile loop).

**Why it's a candidate:** Phase B enqueues 24 tile lambdas per shadow map onto the shared
`ThreadPool`. The lambdas for one shadow map all read the same per-light `FaceListContext`
and all write to the same `sm.depth` + `sm.polyId` buffers. Each lambda's clipper limits its
output to its tile's `(x1f, y1f, x2f, y2f)` rect.

The shadow-pass tile rect is `tileSizeX = (sm.xres + 5)/6`. At sm.xres=512 that's 86 — not a
multiple of `TILE_SIZE=8`. So the clipper's per-tile rect boundaries fall *inside* an 8×8
ShadowBarry tile. Two adjacent tile-workers (e.g. tx=0 covering x∈[0,86) and tx=1 covering
x∈[86,172)) can both produce clipped polygons whose ShadowBarry tile loop touches the
global 8×8 tile at x∈[80,88) — worker 0 wants to write pixels 80-85, worker 1 wants 86-87.

`apply_exact` at line 530 does `*(__m128i*)zRow = _mm_blendv_epi8(*(__m128i*)zRow, encU16,
maskU16)` — a 16-byte read-modify-write of the whole 8-uint16 row chunk. This is *not*
atomic. If worker 0 reads the 16-byte word, worker 1 reads it concurrently, both blend
their own lanes, and both write back, one worker's lanes are silently lost. Same chunk also
covers the polyId scalar loop at lines 535-540, but those are independent 1-uint16 stores
per lane, so polyId per-pixel race is only an issue when *the same* pixel is masked-in by
*both* workers (i.e., a vertex landed exactly on the shared edge after clipper rounding,
which `lroundf(PX * 16)` will produce often when both triangles' edge passes through the
same integer subpixel coord).

When two coplanar triangles with *different* `ShadowMatID` (greets wall split assigns one
per cluster) share an edge that the clipper cuts across the boundary, the shared-edge
texel's polyId is whichever worker wrote last. With pool scheduling non-deterministic, the
winner flips frame-to-frame even at fixed inputs → patches at cluster seams.

This also explains why city hasn't shown the same flicker: city has fewer `ShadowMatID`
distinctions (no wall-split), so the polyId of the two contenders is usually the same and
the race is invisible.

**How to verify:**
1. Pause greets, B-key the overlay on, then run with `--shadow-tiles=1` (or any flag that
   forces serial Phase B tile execution) and see if flicker disappears. If yes → race.
2. Or: after `Render_DeferredShadowMaps` returns, FNV-1a hash `sm.polyId` for one map and
   log it. If two consecutive paused frames produce different hashes, polyId is being
   written non-deterministically — already confirmed by the `[SHADOW-STALE]` machinery in
   `ShadowMap.cpp:60-98`, just run it under `T` key while paused.
3. Sanity check: change `tileSizeX/Y` to be rounded UP to a multiple of `TILE_SIZE` (8) so
   the per-tile clip rects align with ShadowBarry's 8×8 grid. Flicker should disappear.

**Likelihood:** HIGH. Symptom (patches flicker on pause) matches exactly: race winner
varies with pool scheduling, identical inputs aren't enough to fix the output.

### Candidate B — `tile.x * TILE_SIZE` offset is global, but tiles aren't pixel-aligned (HIGH, same root as A)

**Where:** `FDS/FILLERS/ShadowMap.cpp:483-489`.

```cpp
uint16_t * const zRowBase  = zArr
    + size_t(tile.y) * barry::TILE_SIZE * size_t(xres)
    + size_t(tile.x) * barry::TILE_SIZE;
```

`tile.x` is in 8-pixel units of the *global* shadow map. So workers from different clipper
rects writing to the same global `tile.x` are aliasing the exact same memory region. This
is the second face of (A) — the writes don't just go to nearby memory, they go to *identical*
16-byte words.

Combined with the non-8-aligned clipper rects, two workers WILL touch the same word at
seams.

**How to verify:** Print `(tile.x, tile.y, worker_thread_id)` for every `apply_exact` call
for one shadow map's frame and check for duplicates. Any (tile.x, tile.y) pair from two
different workers proves the aliasing.

**Likelihood:** HIGH. Same root cause as A.

### Candidate C — `MekaleleShadowDepth` skipMipLevel + clipper bary edge case (MEDIUM)

**Where:** `FDS/FILLERS/ShadowMap.cpp:866-907` (clipper validation block), `Shadows.cpp:466`
calls `clipper.Render(..., skipMipLevel=true)`.

**Why it's a candidate:** The `--shadow-validate` block logs clipper outputs whose verts
escape the input triangle's convex hull. There's a memory note about [Mekalele clipper
invalidates per-pixel bary for lightmap lookups](feedback_clipper_bary_lightmap.md) — if a
similar bary-mismatch path is firing for the shadow rasterizer (per-pixel polyId stamp
written based on a face whose bary doesn't actually cover the texel), the polyId at edge
texels could be the *wrong* face's ID, which then races with the correct face's per-tile
write. That makes the patches more visible (correct vs wrong cluster) instead of just
"different cluster on each tie."

But the bary itself is constant across frames for fixed geometry — so this alone wouldn't
explain "different frame to frame." It only amplifies the racing in A/B.

**How to verify:** Run with `--shadow-validate`. If `[SHADOW-CLIP]` lines spam during
paused playback for the same tile each frame, the clipper is producing extra verts; if they
fire only intermittently, it's interacting with the race.

**Likelihood:** MEDIUM. Plausible amplifier of A/B but not the root cause of the time-
varying behavior.

### Candidate D — Stale `g_cubeShadowRefs[*].lightISource` mismatch with `sm.lightISource` (MEDIUM/LOW)

**Where:** `FDS/RENDER/Shadows.cpp:101-103` updates `cr.lightISource = cr.omni->IPos` at the
top of every shadow-render call. `sm.lightISource = O->IPos` is updated again per-light in
Phase A loop at line 249. These are two separate copies and both must agree for
`CubeShadow_Sample` to return correct values (lines 219-223 of `ShadowMap.h`).

The update at line 101-103 iterates `g_cubeShadowRefs` AT ENTRY, before checking the mode
or the omni's active flag. So even if Render_DeferredShadowMaps does nothing for this omni
this frame (wrong mode), cr.lightISource still gets refreshed from the current IPos. Phase
A updates `sm.lightISource` only for omnis included in the mode filter. If the omni is
active for one mode call (DynamicOmnis) but not the other (DynamicMeshes), the two
buffers are in sync after the first call but Phase A's lightISource update from the second
call doesn't run for this omni → still in sync. So this isn't a bug source.

**Why low:** This wouldn't be time-varying under pause (IPos is constant when paused).

**How to verify:** Skip — not a likely flicker source.

**Likelihood:** LOW.

### Candidate E — `thread_local` capture-by-pointer of main thread's `perLightFaces` etc. (LOW)

**Where:** `FDS/RENDER/Shadows.cpp:139-142` declares `static thread_local
std::vector<fds::FaceListContext> perLightFaces;` and friends. Lambdas at lines 300-319
and 376-486 capture pointers (`facesPtr`, `scratchPtr`, `camPtr`) into these vectors.

**Why it's a candidate:** thread_locals live in the thread that called the function. The
main render thread calls `Render_DeferredShadowMaps`, so `perLightFaces` lives on the main
thread. Worker threads dereference pointers that point into another thread's TLS.

The C++ memory model says this is OK as long as the source thread doesn't destroy the
storage before workers finish reading. The barrier between Phase A and Phase B + the final
Phase B barrier guarantee this. Lifetimes are also fine — `static thread_local` lasts the
whole program. So no UAF.

But: if a second concurrent caller existed (e.g. a debug path that called
Render_DeferredShadowMaps from another thread), they would have independent TLS copies and
weird things would happen. As of the current code path, only the main thread calls it, so
no race here.

**How to verify:** Skip — not a flicker source as written.

**Likelihood:** LOW (latent risk only).

### Candidate F — `ShadowMap_TickStalenessTracker` reads `polyId_dynamic` as `uint8_t` (LOW, diagnostic only)

**Where:** `FDS/FILLERS/ShadowMap.cpp:75`: `for (uint8_t v : sm.polyId_dynamic)`. The vector
is `vector<uint16_t>` (header line 49). Iterating with `uint8_t v` truncates each entry.

**Why low:** This is a hash computed for the diagnostic `[SHADOW-STALE]` print only. It
doesn't write to `polyId_dynamic` and doesn't affect rendering. But it does silently lose
the upper byte of each polyId, so the stale-tracker will sometimes report "unchanged" when
only the high byte changed → understates actual non-determinism. Worth fixing alongside
because it'd mask future bug repros.

**Likelihood:** LOW for visible flicker, BUT it would hide regressions in the staleness
tracker.

### Candidate G — `sm.fzp` / `sm.zScale` set at scene init but `fzp` used by Phase A clear / Render (LOW)

**Where:** `ShadowMap.cpp:371,419` set `sm.fzp = O->IRange * sFzpMult` and `sm.zScale =
0xFF00 / (sm.fzp * 1.1f)` at Rebuild. `Shadows.cpp:254` overwrites `sm.zScale` per frame.

**Why low:** Per-frame overwrite is deterministic given constant inputs. Not a flicker
source unless `O->IRange` itself drifts when paused (it doesn't — IRange isn't on the
animation spline).

**Likelihood:** LOW.

### Most likely root causes

1. **Candidate A/B (same bug, two angles)**: ShadowBarry's per-tile `_mm_blendv_epi8`
   read-modify-write on global 8×8-pixel words combined with non-8-aligned Phase B tile
   rects (`tileSizeX = (sm.xres + 5)/6 = 86 @ 512²`) lets two tile-workers concurrently
   stamp the same texel with different ShadowMatIDs. Race winner varies with pool
   scheduling → patches flicker frame-to-frame even when inputs are frozen.
2. **Candidate C** (clipper bary edge case) is a plausible amplifier — wrong-face polyId at
   the seam makes the visible delta larger when the race winner flips — but it's a
   secondary, not the cause of the time-varying behavior.

---

## Quick index

- Pipeline orchestration: `FDS/RENDER/RENDER.CPP::renderFrame` (L279), `GREETS.CPP::Tick_Greets` (L~1700)
- Deferred entry: `FDS/RENDER/DeferredLighting.cpp:3032`
- Scalar kernel: `DeferredLighting.cpp:953`
- OuterVec kernel: `DeferredLighting.cpp:2193`
- Wave-2 fill: `DeferredLighting.cpp:2728`
- Cube tap: `FDS/FILLERS/ShadowMap.h:210`
- Cube tap dispatch (lightmap vs cube): `DeferredLighting.cpp:813`
- Shadow render: `FDS/RENDER/Shadows.cpp:90`
- Per-pixel 2D shadow tap (scalar): `DeferredLighting.cpp:1521-1640`
- Dispatch logic OuterVec: `DeferredLighting.cpp:737`
- Sub-pixel dispatch: `DeferredLighting.cpp:752,767`
- Fog pass: `DeferredLighting.cpp:5411`
- FeatureFlags: `FDS/Base/FeatureFlags.def`
- Lightmap atlas sampler: `FDS/Base/StaticShadowLightmap.h:130-220`
