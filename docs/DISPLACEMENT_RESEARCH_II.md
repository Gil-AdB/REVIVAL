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

---

## 2. Per-question findings

### 2.1 Q2 — what IS the standard grazing mitigation?

Not offset limiting. The shipped stack is four separate mechanisms, none of which
bounds lateral travel, plus one that removes the technique entirely.

**(i) Raise the sample count as the surface goes edge-on.** This is the SI3D 2006
contribution. Tatarchuk, sketch p18, verbatim [P-demo]:

> "Dynamically adjust the sampling rate for ray tracing as a linear function of
> angle between the geometric normal and the view direction ray… This ensures
> that we take more samples when the surface is viewed at steep grazing angles,
> where more samples are desired."

In the shipped shader that is exactly one line:

```hlsl
int nNumSteps = (int)lerp( g_nMaxSamples, g_nMinSamples, dot( vViewWS, vNormalWS ) );
```

i.e. `n = n_min` head-on, `n = n_max` at grazing. Note what this fixes and what
it does not: it fixes **undersampling** of a long ray. It does nothing about the
ray's **length**. And our data says length is our problem, not sampling —
`--parallax_pom=32/128/512` give identical slip, and the 512-step reference at
cap 16 swims exactly as much as the shipping cone march (12.37 vs 12.64 texels)
[M, §S1d-2e.1]. **We have already banked mitigation (i) and it is not our
lever.**

**(ii) Perspective / depth bias toward the horizon — tried and explicitly
rejected by its own authors.** Tatarchuk 2004 (Brawley & Tatarchuk) applied it;
2006 removed it [P-demo]:

> "In the [Brawley04] approach we applied perspective bias to fix this artifact.
> Unfortunately, that results in strong flattening of the surface details along
> the horizon, which is undesirable."

This is the same defect class as our hard cap and as offset limiting: bound the
travel, lose the relief. Three independent sources (Welsh's limiter as reviewed
by Tatarchuk, Brawley's bias as retracted by Tatarchuk, our own cap ladder)
converge on it. **A travel bound has no good setting.** Our measured version:
cap 2 keeps rays in the patch but gets grazing depth wrong by up to 0.945 world;
cap 64 is correct and pushes 12.8 % of the wall into the flat clamp [M,
§S1d-2e.3]. The user's independent read of a hard offset clamp — 24 texels = "no
swim but no displacement", 48/64 = "artifacts in the polygons" — is the same
finding by eye.

**(iii) LOD: fade the technique out to plain normal mapping.** This is the
mechanism that actually removes grazing cost in shipped work, and it is
distance-driven, not angle-driven. Tatarchuk, **"Adaptive Level-of-Detail
System"**, verbatim [P-demo]:

> "Compute the current mip map level. For furthest LOD levels, render using
> normal mapping (threshold level). As the surface approaches the viewer,
> increase the sampling rate as a function of the current mip map level. In
> transition region between the threshold LOD level, blend between the normal
> mapping and the full parallax occlusion mapping… We specify an
> artist-directable threshold level where the transition between the parallax
> occlusion mapping and the normal mapping computations will occur."

**This one does not help us, and it is worth saying why so nobody proposes it.**
`--mips` defaults to 0 in this engine and `MiplevelClipper` forces `g_MipLevel =
0`; measured from the G-buffer's own mip nibble, **every deferred pixel of every
one of the 13 review poses is mip 0**, and over a 16-frame sweep 0 of 29 484 424
stone pixel-pairs change mip [M, §S1d-2e.1]. A mip-driven LOD has no signal to
read here. Worse, the poses the user reviews from are *close* grazing wall views
— exactly where every shipped LOD scheme would keep POM at full rate. Shipped
POM's grazing mitigation is largely "the grazing surfaces are far away". Ours are
not.

**(iv) Author the art around it.** Tatarchuk, **"Authoring Strategies"** and
**"Authoring Art Considerations for POM"**, verbatim [P-demo]:

> "Avoid drastic height changes… This relates back to limitation of 'stretching'
> of texture coordinates. The more gradual the height change the less noticeable
> this will be. If you have a height map that is causing texture stretching try
> blurring it in the problematic areas."

> "Can alias at extreme viewing angles. Stretching of texture coordinates. In
> some cases requires smooth height maps or high resolution maps. Intersecting
> geometry clips at original height, not at displaced height…"

Our greets stone is a block wall with **sharp mortar cuts at a ~256-texel block
pitch** — the opposite of "avoid drastic height changes". The relief the user
wants (stones standing proud, visible gaps between blocks) is precisely the
height-map content the shipped technique's own author tells you not to author.
That is worth stating plainly: **we are asking POM to do the thing its authoring
guide forbids.**

**(v) Cone stepping / relaxed cones / max-mipmaps do NOT address grazing.** They
guarantee the first hit is not skipped, i.e. they attack sampling (mitigation i)
with fewer taps. Our own measurement agrees they are not the lever: cone-32
against a 512-step reference at the same cap is p50 0.02 / p90 0.08 / p99 0.5
texels — converged [M, §S1d-2e.1].

**Sub-question: is the true ray used in production, or only inside closed
volumes?** Both. §1.1's table settles it: it is used in production *in the open*,
with a wrapping texture as the domain. The closed-volume family (Hirche, shell
maps) uses it too, with the volume as the domain. What no source does is use it
in a finite chart — **unknown** whether anyone tried and did not publish.

### 2.2 Q3 — how do shipped implementations handle the chart / patch boundary?

**They do not have one.** This is the answer, and it took reading the shaders
rather than the prose to see it.

**(a) A single tiling texture over the whole surface, WRAP addressing.** The
Policarpo/Oliveira I3D 2005 fragment shader takes a scalar uniform `tile` and
computes `dp = IN.texcoord * tile` — the height field is a repeating pattern
scaled over the geometry, so a ray that walks past the pattern's edge wraps into
the next instance and always samples something plausible [P-demo]. The paper
advertises the *absence* of a chart budget as a feature: "contrary to
high-dimensional representations of surface details, the low memory requirements
of the proposed technique **do not restrict its use to tiled textures**" — i.e.
tiling is the norm for the alternatives, and relief mapping is merely not
*limited* to it. Every POM demo surface in the literature (brick, cobblestone,
stone floor, the ToyShop sidewalk) is a tiling pattern.

**(b) When you must use a tile SET, pad it — stated as an art requirement.**
Tatarchuk, **"Authoring Art Considerations for POM"**, verbatim [P-demo]:

> "**Tile sets require buffer region to eliminate seam artifacts.**"

That is the only direct statement in the shipped literature on our exact problem,
and it is one line in an art-pipeline slide. It is real but bounded: a buffer
region covers a ray of bounded length. Ours travels 0.48 UV at grazing against a
0.42 UV median patch [M, §S1d-1.1], i.e. a buffer would have to be wider than the
patch. **The literature does not address a ray longer than the chart, and I found
no source that does.**

**(c) Discard on domain exit — the UE-community "silhouette" trick.** Test
whether the parallaxed UV left [0,1] and kill the pixel (opacity 0 in UE4). This
is the mechanism we implemented as `--pom_shell_domain`, and it is the mechanism
that produces the user's black gashes: it is only correct if the polygon's UV
domain coincides with the object's real boundary. On a single decorative quad it
does. On a wall built of 67 patches over 51 authored planes [M, §S1d-1.1] it
punches holes through solid wall.

**(d) Closed volumes, where exit is meaningful.** Hirche 2004, verbatim from the
paper [P-demo]:

> "To avoid sampling outside of the prism, the exit point of the viewing ray has
> to be determined."

The prism *is* the domain. Exiting is a defined event with a defined answer, and
the answer is the neighbouring prism's own fragment (§2.5).

**(e) Per-triangle vs global tracing, and the thing nobody discusses.** I found
**no source** in the POM/relief line that considers a ray crossing into a
neighbouring triangle's texture space and continuing there. Hirche's tetrahedral
formulation makes the question moot (the primitive bounds the ray); shell maps
make it moot the other way (one global bijection). Our S1d-2c "angled
continuation" — hand the ray to the neighbour's chart through a per-seam affine
transform — appears to be **unattested in the literature**. Our own census
measured what that would cost on greets: 71 distinct (fold, chart-scale,
mirror) buckets, 27 distinct fold angles, and **41.8 % of angled segments
mirrored**, so the transform is a full 2×3 affine including reflection [M,
§S1d-1.8]. Nobody has published this because nobody put a marching ray in a chart
this small.

**The honest summary of Q3: our chart-boundary problem is not a solved problem we
failed to find. It is a problem shipped practice avoided by construction — one
wrapping chart, or one padded tile, or a closed volume.**

### 2.3 Q4 — the silhouette family, re-examined with our numbers in hand

`DISPLACEMENT_RESEARCH.md` §3.3/§6 dismissed this family on one sentence: *"our
flat quads reduce it to S1b at a fraction of the geometry and per-pixel cost —
the shell of a flat quad is a box, the entry point is the rasterized fragment on
the offset lid, and the tangent frame is constant per face."* **That claim is
wrong, and reading the primary sources shows exactly where.** The verdict first,
then the evidence.

**The dismissal confuses "the machinery becomes cheap" with "the machinery
becomes pointless".** On a flat quad the prism machinery becomes *trivial to
compute and exactly correct* — which is an argument FOR it. What it does not
become is *absent*, because a prism carries two things a lid-only box does not:

1. **The side faces are the ray's exit domain, as geometry rather than as a UV
   test.** Hirche 2004, verbatim: *"To avoid sampling outside of the prism, the
   exit point of the viewing ray has to be determined."* Our substitute is a
   UV bounding-box test against `F->U1..V3`, which fires at internal seams where
   the surface demonstrably continues [M, `--pom_shell_domain` in
   `FeatureFlags.def`; 583 922 of 800 513 unanswerable pixels sit at a convex
   ridge, §S1d-1.3].
2. **The side faces are RASTERIZED, so the neighbour's prism owns the pixel the
   ray escaped from.** Hirche renders *eight* triangles per base triangle — "The
   sides of the prism are quads and have to be split into two triangles, the
   bottom and top of the prism remain unchanged resulting in eight triangles to
   be rendered per base triangle" — back-face culled. A screen pixel is therefore
   covered by a fragment from **every** prism whose volume projects there, each
   marching its own interior, with the Z-buffer arbitrating. A ray leaving prism
   A is discarded (*"In case no surface was hit the pixel is removed"*) and
   prism B's own fragment answers it correctly, having entered through the shared
   side quad.

**That second mechanism is the one our implementation does not have, and it is
the mechanism behind every defect in the campaign.** We raster the lid only, so
where prism B's lid does not project there is no fragment for B at all, and A's
discard has nothing behind it — a hole. Our `--pom_shell_side_entry` reconstructs
the entry analytically (clip the ray against four leaning side planes, per-lane
start height) and measured a genuine improvement — lid void 413 100 → 14 163 px
over the 13 review poses [M, §S1d-2d] — but it is confined to *one patch's own*
side planes. It cannot hand the pixel to the neighbouring patch, because the
neighbour has no fragment there. **We built half of Hirche's prism and got half
of the result.**

#### Per-technique, against our five constraints

**Hirche, Ehlert, Guthe, Doggett — *Hardware Accelerated Per-Pixel Displacement
Mapping* (Graphics Interface 2004).** Read in full.
- **(A) lateral bound:** the prism/tetrahedron volume. Absolute, geometric, no
  cap and no offset limit. The tetrahedral variant interpolates entry and exit
  texture coordinates at the rasterizer and marches *between them*, so the ray
  cannot leave by construction: "the texture space coordinates of the entry and
  exit point can be interpolated at the same time by the rasterization units."
- **(B) chart boundary:** does not exist — the primitive *is* the chart. Exit is
  a discard, answered by the neighbour's fragment.
- **(C) silhouette:** demonstrated [P-demo], though the paper never uses the word
  "silhouette" (0 occurrences). It falls out of "In case no surface was hit the
  pixel is removed."
- **(D) depth write:** required, and done, in 2004: "the resulting fragment has
  to be shaded and written to the framebuffer with its correct z-value." Our
  measured +0.1 ms makes this the cheapest of its five requirements [M].
- **(E) precompute + memory:** ×3 tetrahedra or ×8 triangles per base face, plus
  — for the tetrahedral variant — a **per-frame, view-dependent CPU
  decomposition** via Projected Tetrahedra: "So far all the processing has to be
  done on the driver side by the host computer's CPU." On a CPU renderer that is
  a front-end cost, not a free one.
- **The finding §6 missed.** Hirche *abandoned* the cheap single-pass prism
  renderer for one reason, and it is a reason that **does not apply to us**:
  > "The assumption that the prism faces are flat is a very strong restriction
  > that makes the algorithm in this form generally unusable. In case the faces
  > are not flat, a viewing ray may intersect the same face it originates from
  > which will cause holes when rendering."

  Our greets walls are flat axis-charted quads with a per-face constant tangent
  frame [M, `rooms` per-plane world-per-UV constant to four decimals, §8.3 of the
  inventory]. **The restriction that killed the cheap variant is satisfied
  exactly by our content.** So the correct reading of "flat quads simplify the
  machinery" is the opposite of §6's: flat quads let us run the variant Hirche
  could not, at 8 rasterized triangles per quad and no per-frame CPU tetrahedral
  decomposition.

**Porumbescu, Budge, Feng, Joy — *Shell Maps* (SIGGRAPH 2005 / ACM TOG 24(3)).**
Not read first-hand this round; characterised from the 2025 PDM survey
[P-claim, secondary]: "Shell mapping constructs a bijective map between texture
space and shell space — the region between two offset surfaces — where
meso-geometry will be rendered locally. Rays are traced in shell space and
transformed… Both techniques precompute the shell space prism as three
tetrahedra."
- **(A)** the shell volume. **(B)** one *global* bijection, so there is no chart
  boundary anywhere — the opposite strategy to Hirche's per-primitive locality,
  and the only member of the family that structurally answers our cross-chart
  question. **(C)** yes. **(D)** yes. **(E)** offline tetrahedral shell
  construction over the whole mesh; the bijection is the asset. For a 196-face
  wall the construction is small; the cost is that the bijection must be built
  and kept consistent, which on our angle-split mesh is precisely what is broken
  (see the watertightness finding below).

**Oliveira & Policarpo — per-vertex quadric silhouettes (UFRGS TR RP-351, 2005;
ShaderX4), as described first-hand in Policarpo & Oliveira, I3D 2006.**
Verbatim [P-demo]:
> "Oliveira and Policarpo extended the technique to render silhouettes implied by
> the relief data. For this, the attributes of each vertex of the polygonal model
> are enhanced with two coefficients, a and b, representing a quadric surface
> (z = ax² + by²) that locally approximates the object's geometry at the vertex.
> Such coefficients are computed off-line using least-squares fitting and are
> interpolated during rasterization."
- **(A)** the quadric extends the marchable domain past the polygon; travel is
  bounded by where the quadric leaves the slab. **(B)** not addressed. **(C)**
  demonstrated — on a teapot and a sphere, with a wireframe overlay proving the
  relief crosses the polygon outline [P-demo, their Fig. 3 and Fig. 9].
  **(D)** yes. **(E)** two floats per vertex plus an offline least-squares fit —
  by far the cheapest silhouette mechanism in the family.
- **Verdict for us: structurally inert.** The silhouette comes from the object's
  own **curvature**. On a flat wall the least-squares fit gives a = b = 0, the
  quadric is the plane, and the mechanism produces nothing. This is the same
  reason §5.1 rejected VDM's curvature bending, and it is correct — but note it
  is a *different* reason from §6's, and only this one actually holds.

**Policarpo & Oliveira — *Relief Mapping of Non-Height-Field Surface Details*
(I3D 2006).** Read. Multi-layer (dual-depth) relief, so a ray can see the back
surface through a hole. Its silhouettes are the quadric mechanism above, not a new
one ("The silhouettes were rendered using the piecewise-quadric approximation
described in [Oliveira and Policarpo 2005]"). Relevant to us only if we ever want
non-height-field relief; we do not.

**Dachsbacher & Tatarchuk — *Prism Parallax Occlusion Mapping with Accurate
Silhouette Generation* (I3D 2007 poster).** Not obtained first-hand — the poster
is two pages and I could not retrieve a readable copy this round. Characterised
by the 2025 PDM survey [P-claim, secondary], and the characterisation is the
strongest single statement in this document's favour:
> "Prism parallax occlusion mapping enhances this with correct silhouettes by
> intersecting view rays with extruded prisms split into three tetrahedra."
> … "Among the previous direct sampling methods **only Prism Parallax Occlusion
> mapping (PPOM) produces correct silhouettes** — and achieves this with offset
> prisms — yet has difficulty with ambient occlusion in the raster pipeline."

So a 2025 survey, looking back over the whole raster-compatible field, names
**exactly one** technique that gets silhouettes right, and it is the prism. That
is the family §6 non-recommended. Its documented weakness — AO in the raster
pipeline — is one we should take seriously given how much our own look now leans
on SSAO/GTAO reading the marched depth [M, §4-S1a: "the AO-debug field goes from
a featureless gradient to crisp per-joint contact darkening"]. **Unknown** how
severe that is for us; it is the first thing to measure if we build this.

**Jeschke, Mantler, Wimmer — *Interactive Smooth and Curved Shell Mapping*
(EGSR 2007).** Characterised from PDM [P-claim, secondary]: "Curved shell mapping
removes discontinuities by modeling Coons patches within each prism at the cost of
solving a cubic equation per ray step." A cubic solve per step is the wrong shape
for an 8-wide arithmetic-bound row loop, and the discontinuities it removes are
curvature discontinuities we do not have. **Not applicable.**

**Projective Displacement Mapping (arXiv 2025) — the source that most directly
vindicates the flat case.** Its contribution #1 is the *parallel offset prism*:
the standard prism offsets each vertex along its own normal, so "offset triangles
in standard prisms may not be parallel to the base triangle since the unit
normals point in different directions… the projection of a ray in this volume to
texture space is non-linear", and PDM introduces a correction factor
`nf = Σ bary_i /(N_i · N_g)` to make the offset triangles parallel and the ray
projection **linear** in texture space.
**On a flat quad with a shared normal, N₀ = N₁ = N₂ = N_g, so nf ≡ 1 and the
standard prism IS the parallel prism.** The ray is linear in texture space with
no correction at all. A 2025 paper's headline contribution is something our
content gives us for free. That is the precise sense in which flat quads
"simplify the machinery" — and it is an argument for building it, not against.

#### The precondition we are violating, and it is not in the march

Hirche's mesh construction has a requirement that reads like housekeeping and is
not: *"It has to be ensured that neighboring tetrahedral edges are aligned in a
consistent way to avoid aliasing effects between adjacent triangles"*, achieved
by a global vertex enumeration so both owners of a shared side quad split it the
same way. Shell maps state the same requirement as a bijection. Both presuppose
**a watertight offset surface**, which presupposes **shared vertex normals**.

Our mesh does not have them. `MakeFacesIndependentByAngle` has split `rooms` so
completely that it owns **588 verts over 196 faces — exactly 3 per face, nothing
shared** — so the two copies of one corner move along two different normals and
the offset surface tears open at every convex ridge. Measured: lid void against
offset 0.02 / 0.06 / 0.18 / 0.36 world runs **19 416 / 58 665 / 198 131 /
383 364 px — linear in the offset**, and with the lid offset forced to zero
(`--pom_shell_lid_probe`, everything else identical) it is **10 px** [M,
§S1d-2d, and the control corrected in §S1d-2e.5].

**That is not a displacement defect. It is a mesh-topology defect that the shell
family's own preconditions forbid, and we have been attacking it from the march
side.** `--pom_shell_weld` exists precisely to restore the shared normal, and the
measured weld ladder (1 → 14 163 px; 3 → 24 334; 4 → 51 012; 5 → 10 646;
6 → 26 765) says the residue is a *shelled-to-UNSHELLED* junction — the wall
meeting a ceiling and columns that have no shell at all [M, §S1d-2e.5]. In
Hirche's model that case cannot arise, because every triangle of the surface gets
a prism. **Ours arises because we shell one material at a time.** The literature's
answer is not a better weld: it is that the shell covers the whole surface.
