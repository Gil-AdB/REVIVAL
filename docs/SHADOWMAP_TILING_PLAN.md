# Shadow-map memory layout / tiling — plan & cost analysis (for later)

Parked for a future **shadow-bottleneck campaign**. Bench ground truth: the deferred
per-pixel **shadow taps + attenuation** dominate the greets frame (swapping the BRDF,
the vec/scalar path, or ablating the albedo fetch each moved the frame <0.3 ms;
`prof_no_cube_tap` is the gate that isolates the tap). This doc records the layout
proposal from the 2026 renderer discussion and my cost read, so we don't re-derive it.

## Current layout (verified)

- **2D spot maps** — `ShadowMap::depth` is a flat `std::vector<uint16_t>` in **linear
  row-major** order (`depth[iY*xres + iX]`), plus a parallel `depth_dynamic` and
  `polyId`. The lighting PCF is a bilinear 2×2: it reads `row0 = depth.data()+iY*xres`
  and `row1 = row0 + xres`, taps `(iX, iX+1)` on each. Row1 is `xres` apart → a 2×2
  footprint straddles **two cache lines**.
- **Cube maps** — `g_shadowMaps` entries with `cubeFace ∈ [0,5]`; `CubeShadow_Sample`
  projects the 3D receiver→light direction to a face and samples. Adjacent screen
  pixels can hit **different faces / far-apart texels** — effectively random access.
- **Render** — a shadow rasterizer writes depth per pixel into the static (once) and
  dynamic (per-frame, `--shadow-dynamic`) buffers, **scanline-linear** (write runs are
  sequential in memory → cache-friendly for the *writer*).

## The proposal (from the discussion)

Store shadow maps **tiled/swizzled** (e.g. 8×8 blocks = 128 B = 2 cache lines) so a
2×2 / 3×3 PCF footprint lands in one/few cache lines instead of straddling rows.

## Cost of getting tiled data — two routes

**A. Re-tile pass after render (read linear → write swizzled).**
Pure-overhead memory pass: `2 × mapBytes` traffic per map per frame. A 512² u16 map =
512 KB → **1 MB read+write per map per frame**; greets bakes several dynamic maps/frame
→ multi-MB/frame of traffic *just to re-tile*. This spends the exact
bandwidth/cache we're trying to save. **Self-defeating — reject.**

**B. Render tiled from the start (rasterizer writes swizzled addresses).**
Avoids the re-tile pass. Costs: (1) every depth write computes a swizzled address (a few
int ops) — proportionally heavier because shadow raster is depth-only/cheap; (2) the
per-pixel shadow **Z-buffer read/write during raster** also swizzles; (3) it **breaks the
writer's scanline coherence** (a scanline now scatters across tiles).
This is the only *sane* way to tile — but see below.

## Is it worth it at all? — my call: **not now.** (agrees with your hunch)

The decisive factor is **read/write ratio + which taps dominate**:

- A shadow map is **written once** per frame and **read many times** (every lit pixel
  that samples it × PCF taps). Reads ≫ writes, so optimizing the *reader* (tiling) is the
  right side of the trade *if* the reader is cache-bound. That argues **for** route B *if*
  we tile at all.
- **BUT the dominant tap is the cube map**, and tiling a cube *face* only fixes
  intra-face PCF locality — it does **nothing** for the real killer, the cross-face /
  random-direction scatter (adjacent pixels sampling different faces). So the biggest
  cost benefits **least** from tiling.
- The **2D spot maps** (robot spots) are a *minority* of greets' taps (it's omni/cube-
  heavy), and even linear their 2×2 PCF is only 2 cache lines → tiling saves ~1 line per
  2D tap. Modest win on a minority of taps.
- Against that: route B is a **broad, invasive** change — shadow rasterizer write path,
  the shadow Z-buffer read/write, **every** PCF reader, and the cube sampler — with a
  real correctness-regression surface, for a modest, mostly-2D win.

**Higher-ROI, lower-complexity levers to try first (orthogonal to layout):**

1. **Min/max-per-block fast-path.** Precompute per shadow-map tile the min/max depth;
   for a screen block fully in light or fully in shadow, do **one** tap and skip the PCF
   entirely. Attacks the **tap count** directly (the actual cost), works for 2D *and*
   cube, no format change. Likely the biggest single win.
2. **Prefetch.** From the G-buffer world pos we know pixel `(x+k)`'s shadow address ahead
   of time → `_mm_prefetch` it while shading pixel `x`. Cheap, hides the miss latency,
   **no format change** — the one place latency-hiding actually applies in the lighting
   pass.
3. **Then, only if still bound:** tile the **2D** maps via route B and measure the frame.
   **Skip cube tiling** unless profiling shows intra-face locality actually matters.

## Recommended campaign order (later)

1. Profile split: cube-tap vs 2D-PCF cost (`--prof_no_cube_tap` + a 2D-map ablation).
2. Min/max block fast-path (2D + cube). Measure.
3. Next-pixel shadow-address prefetch. Measure.
4. *Iff still bound:* render-tiled 2D maps (route B), measure the frame. Not cube.

Net: **don't tile now.** The complexity is high, the re-tile pass is self-defeating, and
the dominant (cube) cost barely benefits. Spend the effort on the fast-path + prefetch,
which cut the tap count/latency without touching the storage format.
