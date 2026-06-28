# Edge / Outline Post-Effect (NRM_EDGES) — Parked Spec

A screen-space edge-detect pass that draws bright outlines where an image
gradient is steep (silhouettes, surface creases, facet/texture boundaries) and
black elsewhere — the "ink outline / toon edge" look. Named NRM_EDGES because it
was first seen by running a Sobel over a normal-viz frame during the greets
mummy debug.

**Status: PARKED.** Not scheduled. This is a writeup so the idea + design
choices aren't lost; build only if/when it's actually wanted.

## What it is

Discovered as a Python post-process during debugging:
`source → luminance → Sobel/FIND_EDGES → brighten`. The result is an outline
render — edges glow, flat areas go black (see the mummy NRM_EDGES image: clean
silhouette + faint internal facet contours, everything else black).

## In-engine form

A screen-space post-pass, same shape as the existing post passes
(`Render_BloomPass`, SSAO, tonemap): a 6×4 tiled job over the framebuffer,
reusing `Render()`'s tile-job model (cache stays warm, threadpool reused — see
the "parallelize post-passes by tiling" convention). Per pixel: sample a 3×3
neighborhood of the **source buffer**, Sobel-X/Y, magnitude → edge intensity →
composite.

## The two design choices that define the look

1. **Source buffer** (what the edges are computed from):
   - **Depth** (`g_gbuffer` Z / `g_xparZ`) → clean *geometry silhouettes*, no
     texture noise. Best for a toon outline. Note: depth on a faceted surface
     also outlines facet edges (the very thing that made the mummy "lines") —
     desirable for stylization, but it WILL trace low-poly facets.
   - **Normal** (oct-decoded `gb.normal`) → *surface-crease* outlines (where the
     normal turns sharply), independent of depth.
   - **Lit color** (VPage / `g_hdrBuf`) → *everything*, including texture edges —
     this is what the original NRM_EDGES screenshot showed (it ran on a
     normal-viz color frame). Noisiest, most "comic-ink".
   - Typical "good" toon outline = **depth + normal combined** (max of the two
     edge magnitudes), which catches silhouettes AND creases but ignores texture.

2. **Composite mode**:
   - **Outline-only** — black background, white/colored edges (like the parked
     screenshot). A pure stylized mode.
   - **Ink-on-color** — edges darken/overlay the normal lit render (multiply the
     scene by `1 - edge`), i.e. comic-book ink over the painted frame. The usual
     "playable" form.

## Knobs (FeatureFlags, when built)

- `edge_outline` (bool) — enable.
- `edge_source` (int) — 0 depth, 1 normal, 2 depth+normal, 3 color.
- `edge_threshold` / `edge_strength` (float) — gradient cutoff + line intensity.
- `edge_color` — outline color (or "scene" for ink-on-color multiply).
- `edge_thickness` — Sobel radius / dilation passes.

## HDR caveat

Like every VPage post pass on a forced-HDR scene (greets), it must run AFTER the
tonemap (next to the LDR post passes / `Render_LightmapViz`), or the tonemap
overwrites it. See `project_greets_debug_viz_post_tonemap` /
`docs/GRAPHICS_PIPELINE.md`.

## Effort

Small-to-moderate: one tiled post-pass + a handful of flags. The cost is the
extra per-pixel 3×3 fetch; cheap if gated off by default. No new buffers needed
(depth/normal/color all already in the G-buffer).
