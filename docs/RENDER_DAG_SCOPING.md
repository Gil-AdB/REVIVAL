# Render DAG / barrier-tail reclaim — scoping

Goal: reclaim the ~24–28% worker idle that is the **single biggest line in the greets
profile** — `__psynch_cvwait` = 23287 samples, larger than any compute leaf (cones 18888,
lighting 12513, cube tap 8970, clipper 6238). The idle is **barrier tails**: the engine
renders as a sequence of bulk-synchronous parallel waves, each ending in a full
`renderns::tileDone` drain, so the fast tiles sit idle waiting for the slowest tile of that
wave before the next wave starts.

This is NOT a micro-opt (those are exhausted — cones/tap/clipper/kernel are all already
SIMD'd / analytic / hoisted / culled). It's a structural scheduler change. Quality-neutral.

## Current per-frame structure (greets, deferred path)

Serial scene tick (one thread): `Animate_Objects` → main `Transform_Objects` → `Radix_Sort`
→ `ShadowBake_DispatchGreets`. Then `gg->Render()` → `renderFrame` (RENDER.CPP:325).

Each of these is a **parallel wave with its own full barrier** (dispatch N tiles/tasks to
the pool, then `tileDone.acquire()` ×N):

| # | wave | barrier site | ~profile weight | per-tile-local? |
|---|------|--------------|-----------------|-----------------|
| 0 | shadow bake (`Render_DeferredShadowMaps`) | Shadows.cpp 438 / 645 (now `shadowDone`) | bake 5472 + ShadowBarry 3631 + MekShadow 1554 | **global** (all maps, before lighting) |
| 1 | gbuffer fill (`RenderInnerMekalele` tiles) | RENDER.CPP **439** | (raster) | **local** (tile T writes gbuffer tile T) |
| 2 | `Render_DeferredLighting` (surface kernel) | inside (DeferredSurfaceKernel 3289+) | 12513 + dispatch 5412 + cube tap 8970 | **local** (tile T reads gbuffer T) |
| 3 | fog composite + `Hdr_ActivateNoFog` | RENDER.CPP 495–520, **590** | (HDR/fog) | activate currently **global** ⚠ |
| 4 | `Render_VolumetricCones` | inside (DeferredVolumetric) | **18888** | **local** (tile T) — but see HDR rule |
| 5 | `Render_BloomPass` → `Render_TonemapToVPage` | RENDER.CPP 757–758 | tonemap 1133 + bloom | **global** (bright-pass reads whole hdrBuf) |

Plus mirror RTT / shard passes (recursive `renderFrame`) and `StampMirrorMasks` around
wave 1.

## The opportunity + the hard constraint

Waves **1→2→4** are per-**tile**-local: tile T's lighting needs only tile T's gbuffer; tile
T's cones need only tile T's lit buffer + depth. So they *could* be **fused into one
per-tile task chain** — each tile flows gbuffer→lighting→cones without ever hitting a
cross-tile barrier. That removes barriers 1 and 3-ish and lets the slow center tiles (38
lights) overlap the fast edge tiles' whole chain. This is the bulk of the reclaim.

**THE BLOCKER (learned the hard way this session):** cones MUST run *after*
`Hdr_ActivateNoFog`, which is currently **global** (back-fills uncovered/sky pixels in
linear, sets `g_hdrActive`; cones composite into `g_hdrBuf` gated on `g_hdrActive`). Fusing
lighting+cones already broke greets beams once (reverted) for exactly this reason. So:
- **To fuse cones into the per-tile chain, `Hdr_ActivateNoFog` must become per-tile**
  (back-fill tile T's uncovered pixels right after lighting tile T; set `g_hdrActive` once
  after all tiles, or make cones not depend on the global flag). This is the linchpin task.
- Without that, the fuse stops at gbuffer→lighting (still removes barrier 1 — one fat tail).

Waves 0 (shadow bake) and 5 (bloom/tonemap) stay **global-barriered** (genuinely need all
tiles): shadow maps feed every lighting tile; bloom's bright-pass reads the whole buffer.
Stage A already showed overlapping the *shadow bake* with gbuffer doesn't pay (both saturate
the pool) — so wave 0 stays as-is; this campaign is about waves 1→2→(3)→4.

## Design options (in increasing scope)

**A. gbuffer→lighting per-tile fuse (minimum viable).** One task per tile does
`RenderInnerMekalele(tile)` then immediately `Render_DeferredLighting_Tile(tile)` — no
barrier between. Removes barrier #1. Cones/HDR unchanged (still global after). Smallest,
safest, no HDR-ordering issue. Est: removes one of the fatter tails.

**B. + per-tile HDR activate + cones (full local chain).** Make `Hdr_ActivateNoFog`
per-tile, then one task per tile = gbuffer→lighting→activate→cones. Removes barriers #1, #3,
#4. Biggest reclaim. Requires the activate restructure (the linchpin) and careful
`g_hdrActive` handling.

**C. General task-DAG with explicit dependencies.** A scheduler where tasks declare deps
(tile-T-lighting depends on tile-T-gbuffer + global-shadow-bake) and the pool runs any
ready task. Most flexible, handles mirror RTT / shards uniformly, but the largest rewrite
and the highest race surface. Overkill for the win; A→B is the pragmatic path.

Recommend **A first (measure), then B if A pays.** Skip C.

## Dependency / correctness notes
- Per-tile fuse keeps each tile's writes disjoint (gbuffer tile T, framebuffer tile T) — the
  same disjoint-slice invariant the current waves rely on. The fuse only changes *when*, not
  *what*, each tile writes.
- `buildTileLightLists` + `computeTileDepthBounds` run once before lighting (need the gbuffer
  depth). In a fuse, they must run per-tile (after that tile's gbuffer) or stay a global
  pre-pass between gbuffer and the fused lighting+cones. Cleanest: keep light-list build as a
  short global step (it's cheap), fuse only lighting+cones.
- `g_hdrActive`, `g_deferredCtx` publication, the cone `spotIdx`/tile lists — audit each for
  per-tile vs global lifetime.
- Mirror RTT / shard recursive `renderFrame` calls must still work (they re-enter renderFrame;
  the fuse is inside renderFrame so they inherit it).

## Validation (REQUIRED — this is the TSan-blind shadow-race neighborhood)
1. Build clean.
2. **Byte-gate** (`tools/render_gate.sh`, run PLAIN — `SDL_VIDEODRIVER=dummy` alters
   mirrortest; see [[reference_sdl_headless_dummy]]) → byte-identical. The fuse reorders
   *when* tiles run, not results, so it MUST stay byte-identical.
3. **`FDS_THREADS=1`** greets snapshot == baseline `aad05c9b1c48c9e2250662f3e790e7b4`.
4. **`FDS_THREADS=N` × many runs** == baseline (a consistent diff = a real race).
5. **12-concurrent-bench STEADY-count under load** — the torn-read detector TSan is blind to.
6. Gate it behind a flag (e.g. `--render-fuse-tiles`, default off) so it lands byte-identical
   and the STEADY-count can run before default-on.

## Recommended phased plan
1. **Instrument the barrier tails first** (dump-and-look): wrap each wave's `tileDone` drain
   with per-tile completion timestamps; report `max_tile − mean_tile` per wave = that wave's
   reclaimable tail. Confirms which barrier is fattest (hypothesis: lighting #2 and cones #4,
   since the center tiles carry 38 lights vs edge tiles' few — huge per-tile variance). ~½ day.
2. **Option A** (gbuffer→lighting fuse), gated, validate 1–6, measure. ~1–2 days.
3. If A pays, **Option B** (per-tile HDR activate + cones). The activate restructure is the
   linchpin; do it as its own byte-identical step first, then fuse. ~2–4 days.
4. Leave waves 0 and 5 global.

## Expected payoff / risk
- Ceiling: the idle is ~24–28%, but part is serial (Animate/Transform/Radix_Sort/orchestration
  — Amdahl, not reclaimable) and part is wave 0/5 (stay global). Realistic reclaim from
  fusing waves 1→2→4: **~10–15% of frame** (the lighting+cone barrier tails, which are the
  fattest given the 38-vs-few light variance across tiles).
- Risk: HIGH (TSan-blind race neighborhood) but the per-tile-disjoint invariant + the flag
  gate + the STEADY-count validation contain it. The HDR-activate restructure (Option B) is
  the trickiest correctness piece (the cone-after-activate rule that bit us before).
