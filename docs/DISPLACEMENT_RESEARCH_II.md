# Displacement rendering, round II — what shipped, what we deviated from, and what that costs

Research deliverable, 2026-08-06 (fog-wt). No implementation. Remit: re-read the
displacement literature against what this campaign has now MEASURED, and find
what we are missing.

The task was set in these words, and the whole document answers them:

> *"parallax pom is not the answer. this shifts pixels badly. What happens with
> the displacement mapping? how can we actually solve it? we already had some
> pretty good results, apart from the edges, but then it only got worse. I think
> it's time to re-read the research and see where we are missing something.
> because we have an almost working impl, but the edge cases are killing us. and
> these techniques were working on a gpu before they got good enough to just do
> micro tessellation and rt."*

**Evidence discipline.** **[M]** = measured in this repo, cited to the document
that holds the number. **[P-claim]** = a source asserts it. **[P-demo]** = a
source demonstrates it (figures, video, shipping evidence, perf on named
hardware). **[E]** = my inference, with the assumption stated. Where a source is
silent I write **unknown** rather than filling the gap. Costs in ms are never
called negligible; single-digit ms decide things here.

**Read first, in this order:** `docs/S1D_CLOSED_SHELL_PLAN.md` (the closed-shell
campaign and the S1d-1 seam census), `docs/S1_DISCREPANCY_INVENTORY.md` §10
(recess-only), `docs/greets_review_poses.txt`. This document supersedes
`docs/DISPLACEMENT_RESEARCH.md` §6 in two places; §6 of that file now carries a
pointer here.

---

## 1. THE VERDICT — the answer to question 1, up front, because it reframes everything

**Question:** did anyone SHIP silhouette-correct per-pixel displacement in a
real-time product?

**Answer: no. Shipped parallax-occlusion mapping had flat silhouettes, rendered
the relief RECESSED (pushed down, never proud), faded to plain normal mapping
with distance, and the documented mitigation for the silhouette was to author the
art so the defect does not show.** This is not inference. It is stated by the
author of the technique, in the course notes for the demo that shipped it.

Natalya Tatarchuk, *Practical Parallax Occlusion Mapping for Highly Detailed
Surface Rendering* (SIGGRAPH 2006 course *Advanced Real-Time Rendering in 3D
Graphics and Games*; she was lead engineer on ATI's ToyShop demo), slide
**"Performance vs Image Quality"**, verbatim [P-demo]:

> "Tradeoffs between speed and quality. Less samples means more possibility for
> missed features and incorrect intersections. This can result in stair stepping
> artifacts at oblique angles. **Silhouettes are not computed correctly. Art can
> be authored to hide this artifact.** Alternatives exist (at the expense of
> memory and extra computations): Use vertex curvature data and texkill in the
> pixel shader to clip pixels at the silhouettes. Relief Mapping example shows a
> result. **Aliasing at the object silhouettes can be very strong.**"

Same document, on which direction the relief goes, slide **"How Does One Render
Height Maps, Exactly?"** [P-demo]:

> "Two possibilities. Render surface details as if pushed down — the actual
> polygonal surface will be above the rendered surface. In this case the top
> (polygon face) is at height = 1, and the deepest value is at 0. Or actually
> push surface details upward (ala displacement mapping). This affects both the
> art pipeline and the actual algorithm. **In the presented algorithm, we render
> the surface pushed down.**"

Same document, slide **"Correct Depth Output"** — and this is the constraint that
shaped the whole GPU era [P-claim]:

> "Simply using parallax occlusion mapping will yield incorrect object
> intersection. Depth will be computed for the reference surface. May display
> object gaps or cut-throughs. Solution: update each pixel's Z value when
> computing the displacement… Performance will be affected. Z is output from the
> pixel shader. **No longer able to use HiZ for optimization.**"
>
> "Since the computation is in tangent space, the approach can be used with any
> surfaces. Works equally well on curved objects. **Beware of silhouettes.**"

And the shipped reference implementation confirms it in code. Microsoft's
DirectX SDK `DetailTessellation11` POM shader — the sample the technique
propagated through — computes colour only, `SV_TARGET`, and **writes no depth**;
it has no texture-coordinate domain test and no comment about one
([POM.hlsl](https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl))
[P-demo].

At the practitioner level the same verdict holds a decade later. On Epic's own
developer forum, asked directly "Can Parallax Occlusion Mapping break a
silhouette?", the answer is that you can discard where the parallaxed UV leaves
[0,1] (in UE4, set opacity to zero, since discard is not exposed in the material
editor), but that proper silhouette generation "is quite complicated" and points
at Dachsbacher & Tatarchuk's *Prism Parallax Occlusion Mapping* — as a licensed
third-party plugin, not an engine feature
([thread](https://forums.unrealengine.com/t/can-parallax-occlusion-mapping-break-a-silhouette/399186))
[P-claim].

**So the user's premise is half right, and the half that is wrong is the
expensive half.** These techniques did work on GPUs and did ship — as
*recessed, flat-silhouetted, LOD-faded surface shading*. They shipped by
**declining** the problem this campaign has been trying to solve. Our silhouette
premise is the deviation from shipped practice, not the bug.

### 1.1 But the true-ray form is NOT our deviation — and this is the actual finding

The brief hypothesised that shipped POM used the offset-limited form
(`offset = (h−0.5)·strength·V.xy`, no divide) and that our true-ray form
(`÷V·N`, capped) was the deviation that causes our slip. **The primary sources
refute that.** Every canonical implementation in the relief/POM line divides by
the tangent-space view z, i.e. travels the true ray with lateral extent
unbounded at grazing:

| source | the line, verbatim | form |
|---|---|---|
| Policarpo, Oliveira, Comba, I3D 2005, appendix shader | `ds = s.xy * depth / s.z;` | true ray (÷ tangent z) |
| Policarpo & Oliveira, GPU Gems 3 ch. 18, Listing 18-3 | `ray_dir /= ray_dir.z;` | true ray |
| DirectX SDK POM.hlsl (ATI/Tatarchuk lineage) | `fParallaxLength = sqrt(fLength*fLength − vViewTS.z*vViewTS.z) / vViewTS.z;` | true ray (`tan θ`) |

`tan θ` and our `1/cos θ` both diverge at the same rate as the surface goes
edge-on. Offset limiting is **Welsh's fix for single-shift parallax mapping**,
which has no search at all and therefore cannot survive an unbounded shift — and
Tatarchuk's own review of it says what it costs [P-claim]:

> "Parallax Mapping with Offset Limiting… Reduces visual artifacts at grazing
> angles ('swimming texels') by limiting the offset to be at most equal to
> current height value. **Flattens geometry significantly at grazing angles
> (just a heuristic).**"

That is our measured cap ladder, described in 2006. Our own numbers say the same
thing from the other end: at `--pom_shell_cap=16` the landed UV is up to 228
texels (p99) from the true ray's on 21 % of the wall, and that error moves 6.5
texels per frame; at cap 64 the error goes sub-texel [M,
`S1D_CLOSED_SHELL_PLAN.md` §S1d-2e.1].

**So what IS our deviation?** Not the ray. The *domain the ray travels in*:

| configuration | lateral travel | domain | outcome |
|---|---|---|---|
| **(a) shipped POM / relief mapping** | unbounded (true ray) | **infinite — a tiling texture with WRAP addressing** | no swim complaint, no holes, no silhouettes |
| **(b) Hirche '04 prism / shell maps** | unbounded (true ray) | **closed volume the ray cannot leave** | silhouettes, exit only where the surface really ends |
| **(c) offset-limited parallax** | bounded by `strength` | irrelevant — travel is short | stable, but "flattens geometry significantly at grazing" |
| **(d) OURS** | unbounded true ray, **hard-capped** | **finite UV chart, 0.4–2.1 tiles wide** | swim from the cap, holes/smears from the chart exit |

Configuration (d) does not appear in the literature, and the reason is that it is
the one combination that has no consistent answer. We took the true ray from
family (a)/(b) and put it in a finite chart, then bounded it with a hard clamp to
keep it inside — and the clamp is a C0 kink in ray direction as a function of
incidence, which is exactly the thing that moves under the camera. Our measured
median wall patch is **0.42 UV across its short axis while one grazing ray
travels 0.48 UV** [M, §S1d-1.1] — the median patch is narrower than a single
ray. No shipped implementation ever ran POM under that condition.

**The single most actionable consequence, and it needs no new code.** Our
`--no-pom_shell_domain` arm *is* configuration (a): it keeps the marched depth
and never discards, letting the ray keep its out-of-patch UV and sample the
tiling stone map there. The campaign has used that flag **only as a differencing
instrument** to size the clamped population [M, `S1_DISCREPANCY_INVENTORY.md`
§10.6] and has never evaluated it as a LOOK arm. Combined with recess-only
(which rasters the authored polygon, so nothing can void) and cap 64 (which makes
the ray correct), `--pom_recess_only --no-pom_shell_domain --pom_shell_cap=64` is
the shipped-POM configuration, reachable today with existing flags. Its error is
that past a patch boundary it shows the wrong *instance* of a tiling pattern —
which is precisely the error every shipped POM surface has everywhere, and which
no shipped title's players noticed. **That is the cheapest untested arm this
campaign owns.**

### 1.2 The one thing we can do that the GPU era could not

Tatarchuk's "no longer able to use HiZ" is the constraint that pushed the whole
field toward recession: if writing depth from the march costs you early-Z, you
write as little of it as possible, and conservative depth (`depth_greater`) only
ever permits pushing pixels *further away*. We measured our own depth write at
**+0.1 ms median** [M, `DISPLACEMENT_RESEARCH.md` §4-S1a] because we own
`ZPage16` and there is no HiZ to lose.

So the 2004–2010 papers' recessed bias is an artifact of hardware we do not have.
That is a genuine freedom — but note carefully what it does *not* buy: depth
write gives correct *interpolation and occlusion of the relief against other
geometry*. It does not give a silhouette. A silhouette needs a fragment to exist
where the base polygon has none, and that is geometry, not depth. Hirche 2004
already wrote correct per-pixel z in 2004 (§2.5 below) and still needed eight
rasterized triangles per base triangle to get the silhouette.
