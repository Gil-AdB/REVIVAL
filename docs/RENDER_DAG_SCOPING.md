# Render DAG / barrier-tail reclaim — scoping

> **CONCLUSION (phase-2 de-risk, 2026-06-19): DO NOT BUILD THIS.** Phase-1 instrumentation
> measured the cone wave at **effPar ≈ 12–13 on a 12-core box = full pool utilization, idle
> ≈ 0** — the render-frame parallel waves are *already balanced* (96 fine tiles + work-stealing
> pool spread the hot disco center; no solo-tile tail). So overlapping cones ∥ lighting
> reclaims only the inter-wave barrier gap — the same shape that made Stage A a net-negative
> wash. The ~24–28% worker idle (`__psynch_cvwait`) is the **SERIAL fraction** (tick-thread
> Animate/Transform/Radix_Sort/orchestration while the pool parks) + the shadow bake — Amdahl,
> NOT reclaimable by fusing balanced render waves. The frame is near its parallel floor. The
> design below is correct and elegant, but it would not pay. Remaining levers are either
> serial-section parallelization (harder, different campaign) or algorithmic/rate cuts (visual
> tradeoff). Kept for the record + the (good) `accumBuf`/commutative-additive design.



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

**THE APPARENT BLOCKER — and the clean way around it (the better design):** cones currently
run *after* `Hdr_ActivateNoFog` because they composite *into* `g_hdrBuf`, which lighting also
writes. But that is a **write-conflict, not a data dependency.** Verified in
`DeferredVolumetric.cpp:56–77` (`compositeScatter`): in HDR mode the cone composite is purely
`g_hdrBuf[i] += scatter` — it **never reads the lit color or `g_hdrActive`'s buffer state**.
The cone *computation* depends only on gbuffer depth + the light list + shadow maps, all
ready after the gbuffer.

→ **Give cones their own `accumBuf`.** Then:
- Cones compute from gbuffer depth (independent of lighting) and write `accumBuf` additively.
- Lighting (→`g_hdrBuf`) and cones (→`accumBuf`) write **disjoint buffers**, so they run as
  **one interleaved pool wave with NO hard dependency** — unlike Stage A's shadow-bake (which
  lighting truly depended on), a worker that finishes a fast lighting tile just grabs a cone
  tile instead of idling on the barrier. *This* is the tail reclaim, and it's free of the HDR
  ordering trap that broke greets beams when we tried fusing lighting+cones earlier.
- After activate, one cheap full-screen compose `g_hdrBuf[i] += accumBuf[i]`, run **before
  bloom** (so the disco beams still bloom). Legacy 8-bit mode's lit-read (`out[i]` add+
  saturate) simply moves into this compose step — same decoupling.
- This **removes the per-tile-`Hdr_ActivateNoFog` linchpin entirely** (was the riskiest part).
  Cost: one `accumBuf` (~24 MB float RGB @1080p) + a memory-bound ~0.5 ms compose pass.

Waves 0 (shadow bake) and 5 (bloom/tonemap) stay **global-barriered** (genuinely need all
tiles): shadow maps feed every lighting tile; bloom's bright-pass reads the whole buffer.
Stage A already showed overlapping the *shadow bake* with gbuffer doesn't pay (both saturate
the pool) — so wave 0 stays as-is; this campaign is about waves 1→2→(3)→4.

## Design options (in increasing scope)

**A. gbuffer→lighting per-tile fuse (minimum viable).** One task per tile does
`RenderInnerMekalele(tile)` then immediately `Render_DeferredLighting_Tile(tile)` — no
barrier between. Removes barrier #1. Cones/HDR unchanged (still global after). Smallest,
safest, no HDR-ordering issue. Est: removes one of the fatter tails.

**B. Decoupled cone wave via `accumBuf` (the better path — replaces the per-tile-activate
idea).** Cones write their own `accumBuf` instead of `g_hdrBuf`, so lighting + cones dispatch
as **one combined pool wave** (lighting tiles → `g_hdrBuf`, cone tiles → `accumBuf`, disjoint,
no inter-dependency). One barrier instead of two, and idle workers during lighting's tail
pick up cone tiles. Then `Hdr_ActivateNoFog` (unchanged, global) → compose
`g_hdrBuf += accumBuf` → bloom → tonemap. No per-tile-activate restructure, no HDR-ordering
trap. Removes the lighting↔cones barrier — the fattest pair (cones 18888 + lighting 12513 are
the two biggest leaves). This is the main reclaim. **Generalizes to the whole additive layer
(cones + halos + flares + particles), which is commutative → one parallel wave — see "The
whole ADDITIVE layer is one commutative parallel wave" below.**

**C. General task-DAG with explicit dependencies.** A scheduler where tasks declare deps
(tile-T-lighting depends on tile-T-gbuffer + global-shadow-bake) and the pool runs any
ready task. Most flexible, handles mirror RTT / shards uniformly, but the largest rewrite
and the highest race surface. Overkill for the win; A→B is the pragmatic path.

Recommend **A first (measure), then B if A pays.** Skip C.

## The whole ADDITIVE layer is one commutative parallel wave (the real Option B)

The closest analog to cones is NOT the alpha peel — it's the **additive geometry path**.
Verified: `TheOtherBarry<TBlendMode::ADDITIVE>` already does `g_hdrBuf[i] += src` in HDR
(`TheOtherBarry.h:563–578`), **identical** to cones' `compositeScatter`
(`DeferredVolumetric.cpp:64–66`, `g_hdrBuf[i] += scatter`). Halos and flares are the same.
So all of these are already the *same operation* into the *same buffer*.

KEY PROPERTY: **additive is commutative.** Cones, halos, flares, additive particles are
therefore **mutually order-independent**, and in HDR mode independent of the lit color. Their
only input is the **gbuffer depth** (to Z-test against opaque — a flare behind a wall is
hidden), which is ready right after the gbuffer. So the entire additive layer can be **one
parallel accumulation wave** that runs *concurrently with lighting* (lighting → `g_hdrBuf`,
additive → shared `accumBuf`, disjoint), with NO internal ordering and NO dependency on
lighting's output. Compose `g_hdrBuf += accumBuf` once, before bloom.

This is the strongest form of the reclaim: lighting + the *entire* additive layer (cones +
halos + flares + particles) become one big pool of independent tiles/faces — the pool stays
full, one barrier instead of several, and idle workers during any one sub-pass's tail pick up
another's work. It also parallelizes cones *against the additive geometry*, per the obvious
commutativity.

Concrete reuse (small, uniform change — not a new architecture):
- Both cones (`compositeScatter`) and `TheOtherBarry<ADDITIVE>` already target `g_hdrBuf`
  additively → **redirect both to a shared `accumBuf`** (same `+=`, just a different base
  pointer), then one compose `g_hdrBuf += accumBuf` before bloom.
- **Buffer lifecycle** — screen-size float alloc + `parallel_memset` clear (mirror the xpar
  G-buffer clear, RENDER.CPP:388) + the existing compose-before-bloom slot.
- **Do NOT reuse** the xpar G-buffer / `RenderXparClumpInStrip` (those re-light transparent
  fragments — far heavier). Greets uses *forward* additive anyway (`deferredUnifiedTbrEnabled`
  is fountain-only); the additive faces render per-face via `TheOtherBarry<ADDITIVE>`.

Caveats: additive is Z-tested against opaque depth (ready after gbuffer — fine) and currently
*writes* Z alongside opaque (`RenderInner.cpp:260`); writing to `accumBuf` instead means the
additive Z-writes go away, which is safe IFF nothing opaque draws after the additive layer
(it's last before bloom — verify). Color stays exact (commutative add).

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
3. **Option B** (decoupled cone `accumBuf` wave) — likely the bigger and *easier* win than A,
   since it removes the lighting↔cones barrier (the two fattest leaves) without the
   HDR-activate restructure. Land the `accumBuf` + compose as a byte-identical step (compose
   reproduces the current additive result), then merge the lighting + cone dispatch into one
   wave. ~2–3 days. May be worth doing BEFORE A.
4. Leave waves 0 (shadow bake) and 5 (bloom/tonemap) global.

## Expected payoff / risk
- Ceiling: the idle is ~24–28%, but part is serial (Animate/Transform/Radix_Sort/orchestration
  — Amdahl, not reclaimable) and part is wave 0/5 (stay global). Realistic reclaim from
  fusing waves 1→2→4: **~10–15% of frame** (the lighting+cone barrier tails, which are the
  fattest given the 38-vs-few light variance across tiles).
- Risk: HIGH (TSan-blind race neighborhood) but the per-tile-disjoint invariant + the flag
  gate + the STEADY-count validation contain it. The HDR-activate restructure (Option B) is
  the trickiest correctness piece (the cone-after-activate rule that bit us before).
