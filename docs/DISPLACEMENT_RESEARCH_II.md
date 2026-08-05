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

> ## ▶ START AT §8.
>
> §8 is the answer to the question that matters — **why protrusion works on most
> faces and fails on specific ones** — and it retracts §5's R5. Protrusion is
> **not** in question: the tessellation arm proved the look and the user has seen
> and liked see-through between stones. §§1–4 remain valid as the literature
> record; §5's ranking is superseded by §8.5.

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

### 2.4 Q5 — the cheap-geometry path, re-costed

The modern answer is that geometry became cheap. The question is whether any part
of that transfers to a CPU software rasterizer, and whether the tessellation arm
was retired on numbers that no longer hold.

**What the modern techniques actually are.**

- **REYES (Cook, Carpenter, Catmull, SIGGRAPH 1987)** — the canonical CPU answer,
  read first-hand. Its dicing criterion, verbatim: micropolygons "are flat-shaded
  quadrilaterals that are **approximately 1/2 pixel on a side**. Since half a
  pixel is the Nyquist limit for an image, surface shading can be adequately
  represented with a single color per micropolygon." And: "Dicing is done in eye
  space, with no knowledge of screen space except for an **estimate of the
  primitive's size on the screen**. This estimate is used to determine how finely
  to dice… Primitives are diced so that micropolygons are approximately half a
  pixel on a side in screen space." Grids share vertices between adjacent
  micropolygons, which is how the crack problem is avoided *within* a primitive.
- **Adaptive tessellation with no cracks (Cantlay, *DirectX 11 Terrain
  Tessellation*, NVIDIA whitepaper, 2011)** — read first-hand, and the rule is
  exactly the one our S2 crack machinery already implements: "For patches of
  uniform size, a crack-free surface is achieved by computing tessellation factors
  **purely as a function of quad patch edges**. Since edges are shared, each patch
  arrives at a result that agrees with its neighbors' edges." It also records why
  a single tessellation factor is never enough — "DirectX 11 limits tessellation
  factors to the range 1 to 64. This is not nearly sufficient to represent the
  range of scales required" — and falls back to Ulrich-style rings of
  differently-sized patches. Our per-chunk ladder is that structure.
- **Nanite (Karis, Stich, Schied, SIGGRAPH 2021 Advances)** — characterised from
  secondary sources this round [P-claim, secondary]: a compute-shader software
  rasterizer handles clusters whose triangles are under ~32 pixels, reportedly ~3×
  the hardware rasterizer on small triangles, because fixed-function rasterizers
  are parallel in *pixels* (2×2 quads) rather than in triangles and waste most of
  that on sub-pixel geometry. LOD is a cluster-DAG selection by projected error.
- **NVIDIA Displaced Micro-Mesh (2022)** — not obtained first-hand this round.
  Characterised previously in `DISPLACEMENT_RESEARCH.md` §3.5 as base triangle +
  per-triangle subdivision level + compressed µ-vertex displacements along
  interpolated normals, with watertight edge rules between differing levels
  [P-claim]. Note the shape: **displacement along the interpolated normal over a
  barycentric subdivision is a prism parameterisation.** DMM is the shell family
  with the volume made implicit and the samples baked. **Unknown** whether the
  edge rule is stated as per-edge-min in the spec; our §3.6 already assumed so.

**The transfer to a CPU rasterizer, quantified.** Nanite's finding is that
software rasterization *wins* below ~32 px/triangle — but that is software raster
on a GPU's thousands of lanes, and it says nothing about a 12-thread CPU's
absolute cost. Our own measured per-face cost is the number that decides this:
**~0.75–0.85 µs/face, threaded, whole-frame**, from the four measured points
52.5 → 74 → 107 → 121 ms at 10 k → 43 k → 87 k → 103 k faces [M,
`DISPLACEMENT_RESEARCH.md` §4-S2].

Apply the REYES criterion to our worst pose and the answer is immediate. ~1.9 M
wall pixels at half-pixel micropolygons is ~7.6 M micropolygons; at 0.8 µs/face
that is **~6 seconds/frame** [E, arithmetic on the measured rate]. The
micropolygon pipeline is five orders of magnitude out of reach and always will be
on this machine. But the useful form of the question is the inverse one:

| screen area per face | faces for a full-screen wall | added cost at 0.8 µs/face [E] |
|---|---|---|
| 0.5 px (REYES) | ~7 600 000 | ~6 000 ms |
| 100 px (10×10 px triangles) | ~19 000 | **~15 ms** |
| 400 px (20×20 px) | ~4 750 | **~4 ms** |
| 1 600 px (40×40 px) | ~1 200 | ~1 ms |

So the CPU's affordable geometric density for a full-screen wall is roughly
**one face per 400–1 600 screen pixels**, i.e. 20–40 px triangles, for a
single-digit-ms budget. Against that, the projected relief step at our poses is
~84 px at z = 5 and ~10 px at z = 40 [E, §4-S2's model]. **Block-scale relief is
inside a 20–40 px budget at typical distances; mortar-scale relief is not, at any
distance.** That is a clean statement of the split, and the machinery to exploit
it — the residual-height split (B4), which gives geometry the low band and the
march the residual — already ships.

**Now the re-costing the coordinator asked for, and the answer is "partly, and
less than it looks".** The tessellation arm's measured delta was +54.5 ms at
86.6 k faces (and +35.56 ms against recess-only at t=5780 [M, §10.7]), taken with
two things wrong: the retired-mesh bug (`799c808` — an orphan faceless mesh was
66–70 % of shadow-pass verts and 18–72 % of main-view verts) and no SoA transform.
The honest arithmetic:

- The +54.5 ms decomposed as RNDR + a per-frame shadow-bake re-raster (+~9 ms at
  43 k faces) + XFRM (+~3 ms) [M].
- **XFRM was ~3 ms of 54.5.** The SoA inline cut the whole main-view
  `Transform_Objects` by 1.97 ms (7.93 → 5.96, −25 %) [M], and the retired-mesh
  fix took the shadow front end 23.07 → 10.67 core-ms [M] — which at the pool's
  speedup is ~1 ms of wall clock.
- **So the two fixes retire single-digit-ms of a 35–55 ms delta, and none of it
  from the two largest terms.** RNDR's per-face setup and the per-frame shadow
  re-raster of displaced geometry are untouched by either fix. Full-scene
  tessellation is **not** rehabilitated. I will not dress a ~2–4 ms improvement
  up as a reprieve for a 35 ms cost.

**What IS newly worth a look, and it is a different proposition.** The arm that
was retired is *uniform full-scene* tessellation. Nothing in the campaign has ever
measured **silhouette-only** tessellation, and the cost model says the target is
concrete: at 0.8 µs/face a **+5 ms budget buys ~6 000 faces**, i.e. ~7 % of the
full carve's 86.6 k. The question "can 7 % of the faces carry the silhouettes?"
is answerable with instruments that already exist — `--pom_seam_census` already
classifies every patch boundary and already reports screen-weighted coverage at
the 13 review poses [M, §S1d-1.3], and greets' relevant population is small:
72.9 % of the defect sits on convex ridges totalling 305.07 world of edge in
`rooms` out of 2 289 world of boundary overall [M, §S1d-1.2]. A ring of
tessellated faces along *only* the convex ridges, with the flat interior left to
per-pixel POM, is the hybrid the literature has recommended since Tatarchuk's LOD
slide — run in the opposite direction from hers (geometry where the eye checks the
outline, shading everywhere else).

Two honest caveats on that. First, it inherits the shadow-bake cost: the displaced
faces are re-rastered into the shadow cubes every frame, and the shadow passes are
340–790 core-ms across 33–36 calls [M, `VISIBILITY_PLAN.md` §8a] — a ridge ring
would add to that, and it must be measured, not assumed. Second, the crack rule is
non-negotiable (Cantlay's shared-edge-only factor, DMM's per-edge level), and our
S2 machinery already implements it [M].

**Also worth recording: UE5's own retirement of DX11 tessellation.** I did not
obtain a primary Epic statement this round; the *fact* that UE5 dropped the DX11
tessellation displacement path in favour of Nanite/virtual heightfield is widely
reported but I am not citing a source I have not read. **Unknown, pending a
primary citation.**

---

## 3. Every candidate against OUR measured constraints

Rows above the rule are arms we have built or can reach with existing flags; below
it are the literature's options. "Lateral bound" is what stops the ray running
away at grazing — the term our slip data says is decisive. Costs marked [E] state
their assumption.

| technique | chart-boundary behaviour | lateral-travel bound | silhouette | needs depth write | CPU / 8-wide SIMD shape | precompute + memory |
|---|---|---|---|---|---|---|
| **Flat POM (shipping default)** | none — the ray keeps its UV, offset-limited so it barely moves | `strength` (offset-limited) | no | no | ideal: fixed-count arithmetic march, +1.7–2 ms naive-8 [M] | none beyond the height map |
| **Recess-only + clamp (standing arm)** | **clamp to flat** — 0.8–8.5 % of frame renders as flat wall [M §10.6] | hard cap; cap 16 is 21 % of the wall on a wrong ray [M] | no, structurally [M §10.8] | yes (+0.1 ms [M]) | same as flat POM; measured within 0.01 ms of it at t=5780 [M §10.7] | none |
| **Recess-only + NO domain test + cap 64** — *the shipped-POM configuration, UNTESTED as a look arm* | **none — ray samples the tiling map past the boundary, exactly as shipped POM does everywhere** | none needed (cap 64 = uncapped, error sub-texel [M]) | no | yes | identical to the above; the domain test is *removed*, so cheaper | none |
| **Lid + weld + side entry (best protrusion arm)** | leaning side planes as an exit test + analytic entry, within one patch | cap | **yes, geometrically** — nearer depth on 16 k–176 k px [M §S1d-2d.6] | yes | +4 divides, ~16 FMAs, 8 selects per covered shell px [M-stated] | lid extrude + weld at init |
| **Offset-limited ray in a finite chart** | ray barely leaves, so no boundary problem | `strength` | no | optional | ideal | none |
| — | | | | | | |
| **Hirche '04 prism, flat-quad single-pass variant** | **the prism IS the domain; escaped pixels are owned by the NEIGHBOUR prism's fragment** | the prism volume — absolute, geometric | **yes** [P-demo] | **yes** (Hirche does it in 2004) | 8 tris/base tri; ~1 800 faces for our 226 shelled quads = **~2 % of the tessellation carve** [E] | extrude + consistent edge split; no maps |
| **Hirche '04 tetrahedral variant** | tetrahedron; entry/exit UV interpolated by the rasterizer | the tetrahedron | yes [P-demo] | yes | **poor**: needs per-frame view-dependent Projected-Tetrahedra decomposition on the CPU [P] | ×3 tetrahedra/face |
| **Shell maps '05** | **one global bijection — no chart boundary anywhere** | the shell volume | yes [P-claim] | yes | tetrahedral traversal; bijection lookup | offline shell + bijection over the whole mesh |
| **Quadric silhouettes (Oliveira & Policarpo '05)** | not addressed | the quadric's exit from the slab | yes on **curved** surfaces [P-demo] | yes | 2 floats/vertex, trivial | offline least-squares fit |
| **Prism POM (Dachsbacher & Tatarchuk '07)** | prism | prism | **yes — the only direct-sampling method a 2025 survey credits with correct silhouettes** [P-claim, secondary] | yes | prism + 3 tetrahedra; documented difficulty with **AO in the raster pipeline** | prism extrusion |
| **Curved shell mapping (Jeschke '07)** | Coons patch per prism | prism | yes [P-claim] | yes | **bad**: a cubic solve per ray step | patch fit per prism |
| **Cross-chart march continuation (our S1d-2c)** | **hand-off through a per-seam affine transform — unattested in the literature** | one hop suffices (p99 = 232 texels [M]) | no by itself | yes | bounded kernel cost; the work is the bake | 71 affine buckets, 41.8 % mirrored [M §S1d-1.8] |
| **Silhouette-only tessellation** | n/a — real geometry | n/a | **yes, true** | no | +5 ms buys ~6 000 faces at 0.8 µs/face [E]; must also pay the shadow re-raster | per-chunk bake + shared-edge crack rule |
| **Full-scene tessellation (retired)** | n/a | n/a | yes, true | no | **+35.6 ms vs recess-only at t=5780** [M §10.7] | 2–6 s init bake, ~80 k faces resident |

Two entries in that table deserve to be read together: Hirche's flat-quad prism
gets silhouettes for an estimated **~1 800 faces**, against the tessellation
carve's measured **86 600**. That ~2 % ratio is the number `DISPLACEMENT_RESEARCH.md`
§6 should have computed before non-recommending the family, and it is the single
strongest technical argument in this document. The estimate assumes our measured
0.75–0.85 µs/face holds at these counts and that the side faces' *pixel* coverage
is small (they are seen edge-on almost everywhere) — **the pixel coverage is
unmeasured and could change the verdict.**

---

## 4. The one measurement that would settle the swim, and it has not been taken

Everything about the grazing complaint currently rests on comparing per-pixel arms
to each other and to a per-pixel reference. At cap 64 the error *against the true
ray* is zero **by construction** — cap 64 IS the reference [M, §S1d-2e.0] — yet the
absolute surface-registered slip at cap 64 is p90 15.3 / p99 501 texels per frame
[M, commit `7bfbc87`]. Those two facts are not in conflict: they say **the true
ray's own landing point is genuinely hypersensitive to camera motion at grazing**,
because the landed UV carries a factor 1/cos θ whose derivative in θ blows up
faster still.

So there are two candidate mechanisms for what the user is seeing, and they demand
opposite responses:

- **(H1) The march is wrong.** Then the fix is in the march or the ray.
- **(H2) The motion is physically correct, and it reads as "swimming" because it
  is painted onto a FLAT surface.** On real relief the fast-moving deep-valley
  samples are broken up by the near blocks' own silhouettes and edges; on a flat
  recess-only wall there is nothing to break them up, so the eye reads correct
  parallax as sliding texture. [E — this is inference, not measurement.]

**The discriminator is cheap and has never been run: take the tessellation arm
through the identical sweep-A camera path and measure ITS slip on the same
surface-registered metric.** The instrument exists (`--pom_path_viz`'s
`*_uvgeo.bin` gives a camera-free surface coordinate; `--parallax_max_offset=1e-6`
gives the geometric UV). Tessellation moves real geometry, so its "slip" must be
computed from the depth/albedo registered on that coordinate rather than from a
marched UV — a scripting job, not a code change.

- If tessellation's slip at grazing is **comparable** to cap 64's, H2 holds: the
  motion is physical, the march is exonerated, and the remaining work is
  silhouettes and shading normals (`--pom_normal`), not the ray.
- If tessellation's slip is **low**, H1 holds and the march is still wrong.

Until that is measured, "the swim" is an unattributed symptom and any march change
aimed at it is a guess. This is the cheapest-discriminator-first move and it should
precede every other item below.

One further term is already known to be in the frame and is *not* the march:
`--texture_filter` defaults to 0, the deferred kernel point-samples at texel
granularity, and one height texel covers ~15 screen pixels at these poses, so the
wall is drawn as ~4 px texel blocks that crawl under camera motion — present in
flat POM and in `--no-parallax` alike [M, §S1d-2e.4]. It was not shown to be what
the user means, and is recorded rather than claimed.

---

## 5. Ranked recommendation, with the negative option stated as a real option

**R0 — measure the tessellation arm's grazing slip (§4).** No code. Decides
whether anything in the march needs to change at all. Do this first.

**R1 — render the shipped-POM configuration and look at it.** No code:
`--pom_recess_only --no-pom_shell_domain --pom_shell_cap=64`, all 13 review poses
plus sweep A. This is configuration (a) from §1.1 — the exact domain model every
shipped POM surface used — and the campaign has only ever run `--no-pom_shell_domain`
as a differencing instrument [M §10.6]. Expected: zero void (recess-only rasters
the authored polygon), no clamp band at all (the clamp is what we are removing),
sub-texel error vs the true ray, and the only defect being the *wrong instance* of
a tiling stone pattern past a patch boundary — the defect shipped POM has
everywhere. It is also strictly *cheaper* than the standing arm, since a compare
group per pixel disappears. **If this looks acceptable, most of S1d is unnecessary.**

**R2 — fix the mesh, not the march.** The shell family's stated precondition is a
watertight offset surface, i.e. shared vertex normals; `rooms` has exactly 3
unshared verts per face and the lid void is measured **linear in the offset**
[M §S1d-2d]. Either weld across *every* surface the shell touches — including
`siling` and the columns, which currently have no shell and are where the residue
lives [M §S1d-2e.5] — or stay in recess-only, where the precondition is vacuous
because nothing moves. There is no third option that the literature supports.

**R3 — if protrusion is wanted, build Hirche's prism properly, not another half of
it.** Raster all eight faces of each shelled quad's prism (back-face culled), let
every prism march its own interior, discard on exit, and let the Z-buffer arbitrate
between overlapping prisms. That is what makes a neighbour's fragment answer the
pixel our ray escaped from — the mechanism `--pom_shell_side_entry` approximates
within one patch and cannot complete [M §S1d-2d]. Estimated ~1 800 faces for our
226 shelled quads, ≈ 2 % of the tessellation carve's face count [E]. **Gate it on
the side faces' measured pixel coverage first**, because that is the one term in
the estimate with no number behind it. Requires R2.

**R4 — silhouette-only tessellation, as R3's competitor.** +5 ms buys ~6 000 faces
[E]; the target population is already localised by `--pom_seam_census` (72.9 % of
the defect on convex ridges) [M]. Must be costed *including* the per-frame shadow
re-raster, which the retired-mesh and SoA fixes did **not** touch. Uses the
existing shared-edge crack rule.

**R5 — the honest negative verdict, and it is the front-runner on the evidence.**

> ## ⛔ R5 IS RETRACTED — 2026-08-06. See §8.
>
> **The evidence R5 rested on was a broken metric, and I re-measured it.** The
> "see-through is 0–24 px" figure counted only pixels where a surface **more than
> 3 world units** behind the wall wins — 17× the 0.18-world relief slab. That is a
> detector for *distant background*, not for what a viewer calls see-through
> between two proud stones (the adjacent wall, the floor, or the mortar bed, all
> well inside 1 world unit). Re-measured with the threshold set at the slab
> amplitude, the same lid arm at the same pose shows **57 270 px** of see-through,
> not 23 — **2 490× the retracted figure** (§8.1). Protrusion produces the look;
> the metric could not see it. The negative recommendation is withdrawn, and
> §8 answers the question that actually matters: *why does it work on most faces
> and fail on specific ones?*
>
> The struck-through argument is kept below for the record only. **Do not cite the
> 0–24 px figure again.**

~~**Recess-only per-pixel displacement for the surface, in the shipped-POM domain
model (R1), and no silhouette program at all — because this scene cannot show
one.**~~

~~The argument is not aesthetic, it is measured. §S1d-2d.6 went looking for
see-through in the mortar valleys with an instrument rather than by eye, at all 13
review poses and at 3.3× the standing amplitude, and found **0–24 px at every pose
except one**, where the 24 737 px turned out to be a residual geometry slit rather
than relief. The reason given there is structural and I believe it: **greets is a
closed room.** Behind every shelled wall is another wall at zero distance or
nothing at all, so a stone standing proud has nothing to be seen past.~~ On top of
that, only **0.4 % of 2 289 world of patch boundary is a true free edge** [M
§S1d-1.2] — that remains true, and it means the silhouettes that matter here are
*internal* ones between stones, not outline silhouettes against a background.

Still valid from the retracted argument: recess-only is measured at **0 void, 0
offscreen delta, and within 0.01 ms of flat POM**, and with `--pom_normal` plus
`--pom_horizon` it was ranked by eye as `tess ≈ recW18pn > recW18 > rec >> lid`
[M §10.7]. What it gives up — "a groove that can only sink is not the same as a
block that can rise" [M §10.7] — is exactly the thing the user wants back, so
recess-only is the **fallback**, not the destination.

**Explicitly do not build:** coplanar cross-chart continuation (31 px of 800 513,
already shipping [M]); curved shell mapping (a cubic per step); quadric silhouettes
(a = b = 0 on a flat wall); a hard offset clamp as a quality knob — three
independent sources plus our own ladder say a travel bound has no good setting
(§2.1 (ii)).

---

## 6. What this contradicts in `docs/DISPLACEMENT_RESEARCH.md`

That document's §6 non-recommended "Prism/tetrahedra rasterization (Hirche '04,
shell maps as-shipped)" on the grounds that "our flat quads reduce it to S1b at a
fraction of the geometry and per-pixel cost". **Two errors, and the second one
cost the campaign months of the wrong work:**

1. **It conflated cheap with pointless.** Flat quads make the prism *trivially
   correct* (Hirche's abandoned single-pass variant becomes usable; PDM 2025's
   parallel-offset correction becomes the identity). §2.3.
2. **It missed the mechanism.** A prism's side faces are not just an exit test,
   they are *rasterized geometry*, which is what gives the neighbouring prism a
   fragment at the pixel your ray escaped from. Our lid-only shell has no such
   fragment, and every hole, gash, smear and rust stripe in the campaign traces
   back to that single missing thing. §2.3.

It also implied that the offset-lid shell (S1b) was the flat-wall *special case* of
the prism family. It is not: it is the prism family **with the side faces deleted**,
which is the one part that was load-bearing.

Two things in `DISPLACEMENT_RESEARCH.md` this round **confirms**: the VDM/GDM
rejection (§5.1 — and the curvature argument is now backed by the verbatim quadric
mechanism), and the sphere-tracing verdict (§5.2). And one thing it got right for
the wrong reason: quadric silhouettes really are inert for us, but because a flat
wall has no curvature, not because the machinery collapses.

`docs/DISPLACEMENT_RESEARCH.md` §6 now carries a pointer to this file.

---

## 7. Sources

Read first-hand this round (text extracted and quoted above):

- N. Tatarchuk, *Practical Parallax Occlusion Mapping for Highly Detailed Surface
  Rendering*, in *Advanced Real-Time Rendering in 3D Graphics and Games*, SIGGRAPH
  2006 course. https://advances.realtimerendering.com/s2006/Tatarchuk-POM.pdf
- N. Tatarchuk, *Practical Dynamic Parallax Occlusion Mapping*, SIGGRAPH 2005
  sketch / I3D 2006 slide deck.
  https://cgg.mff.cuni.cz/~pepca/lectures/pdf/Tatarchuk-ParallaxOcclusionMapping-Sketch-print.pdf
- F. Policarpo, M. M. Oliveira, J. Comba, *Real-Time Relief Mapping on Arbitrary
  Polygonal Surfaces*, I3D 2005 (appendix Cg shader: `ds = s.xy*depth/s.z`,
  `dp = IN.texcoord*tile`, `RM_DEPTHCORRECT`).
  https://www.inf.ufrgs.br/~oliveira/pubs_files/Policarpo_Oliveira_Comba_RTRM_I3D_2005.pdf
- F. Policarpo, M. M. Oliveira, *Relief Mapping of Non-Height-Field Surface
  Details*, I3D 2006 (the verbatim quadric-silhouette mechanism).
  https://www.inf.ufrgs.br/~oliveira/pubs_files/Policarpo_Oliveira_RTM_multilayer_I3D2006.pdf
- J. Hirche, A. Ehlert, S. Guthe, M. Doggett, *Hardware Accelerated Per-Pixel
  Displacement Mapping*, Graphics Interface 2004.
  http://download.hrz.tu-darmstadt.de/media/FB20/GCC/paper/Hirche-2004-GI.pdf
- F. Policarpo, M. M. Oliveira, *Relaxed Cone Stepping for Relief Mapping*, GPU
  Gems 3 ch. 18, 2007 (`ray_dir /= ray_dir.z`; the z-buffer update).
  https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-18-relaxed-cone-stepping-relief-mapping
- R. L. Cook, L. Carpenter, E. Catmull, *The Reyes Image Rendering Architecture*,
  SIGGRAPH 1987 (half-pixel micropolygons; eye-space dicing from a screen-size
  estimate).
- I. Cantlay, *DirectX 11 Terrain Tessellation*, NVIDIA whitepaper, 2011
  (crack-free factors from shared patch edges only; the 1–64 factor limit).
- *Projective Displacement Mapping for Ray Traced Editable Surfaces*, arXiv 2025
  (the parallel-offset prism; the survey line naming PPOM as the only
  direct-sampling method with correct silhouettes).
  https://arxiv.org/pdf/2502.02011
- Microsoft DirectX SDK `DetailTessellation11` POM shader (`fParallaxLength`;
  `nNumSteps = lerp(g_nMaxSamples, g_nMinSamples, dot(V,N))`; no depth output; no
  UV domain test).
  https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl
- Epic Developer Community, *Can Parallax Occlusion Mapping break a silhouette?*
  https://forums.unrealengine.com/t/can-parallax-occlusion-mapping-break-a-silhouette/399186

Characterised from secondary sources this round, flagged as such in the text
(**not** read first-hand — obtain before relying on any detail):
S. Porumbescu et al., *Shell Maps*, SIGGRAPH 2005 · C. Dachsbacher, N. Tatarchuk,
*Prism Parallax Occlusion Mapping with Accurate Silhouette Generation*, I3D 2007
poster · S. Jeschke et al., *Interactive Smooth and Curved Shell Mapping*, EGSR
2007 · B. Karis et al., *Nanite — A Deep Dive*, SIGGRAPH Advances 2021 · NVIDIA
*Displaced Micro-Mesh*, 2022.

Checked and found NOT to contain the expected material:
M. Mittring, *Finding Next Gen — CryEngine 2*, SIGGRAPH 2007 course ch. 8 — read
in full; contains **no mention of parallax or parallax occlusion mapping**. Whether
Crysis shipped POM, and whether recessed or protruding, is **unknown** from this
source.

In-repo evidence, all measured on this machine:
`docs/S1D_CLOSED_SHELL_PLAN.md` (§S1d-1 seam census, §S1d-2 side faces, §S1d-2d
side entry, §S1d-2e the cap/swim attribution and the weld ladder) ·
`docs/S1_DISCREPANCY_INVENTORY.md` (§9 grazing cap, §10 recess-only, §10.6 the
clamp population, §10.7 look + perf, §10.8 what recess-only cannot do) ·
`docs/DISPLACEMENT_RESEARCH.md` (§2 march economics, §4-S1a depth-write cost,
§4-S2 the µs/face rate) · `docs/OPTIMIZATION_BACKLOG.md` (XFRM/SoA, BVH refutation)
· `docs/VISIBILITY_PLAN.md` §8a (per-pass front-end census) ·
`docs/greets_review_poses.txt` · commit `7bfbc87` (the surface-registered slip
figures) · commit `799c808` (the retired-mesh fix).

---

# 8. THE DISCRIMINATOR — why protrusion works on most faces and fails on specific ones

Added 2026-08-06 after a user correction. His words: *"we know Protrusion will give
us visual gain since the mesh displacement did, and we had faces where we actually
see through and it looks good. the method just doesn't work for some case, for some
reason, that your research should find at last."*

So the question is not "is the technique viable". It is **which property separates
the faces that work from the faces that fail.** Everything below is measured on
this machine today, with dummy SDL drivers, 1920×1080, on the current `fog-wt`
binary.

## 8.1 First, the metric that was driving the wrong conclusion

`S1d-2d.6` concluded "see-through NOT demonstrable" from an instrument defined as
*"pixels where a surface more than 3 world units BEHIND the wall wins"*. The relief
slab under that arm is **0.18 world** (`--pom_shell_world_amp_set=0.18`). The
threshold was therefore **17× the entire slab depth** — it can only fire when a
*distant room* is visible through the wall, and it is blind to a stone occluding
the mortar bed 0.02 world behind it, or the adjacent wall a fraction of a unit
away. **It was a background detector, and it was reported as a see-through
detector.**

Re-derived properly. See-through = *the lid arm's winning surface is farther away
than the flat arm's by more than the relief slab can account for.* Relief carving
can push depth back by at most the slab, so anything beyond the slab is a genuinely
different, farther surface. No face ids needed (they are not comparable across arms
— inventory §2), depth alone suffices: `sep = (z16_flat − z16_lid)/zscale`, zscale
395.64, both arms' depth non-zero.

Arms: `flat` = flat POM; `lid` = the standing S1d-2d arm
(`--pom_shell --pom_shell_weld=1 --pom_shell_side_faces=3 --pom_shell_lid_edge=1
--no-pom_shell_base_clip --pom_shell_world_amp_set=0.18 --pom_normal`, cone-32,
cap 16). `--face_id_dump` + `FDS_SNAPSHOT_ZDUMP=1`. Zero bad flags in all 6 run logs.

| pose | both-wrote px | **sep > 0.18 (slab)** | > 0.25 | > 0.36 | > 0.54 | > 1.0 | > 3.0 *(the retracted metric)* |
|---|---|---|---|---|---|---|---|
| **p5743** | 2 073 447 | **57 270** | 30 711 | 9 930 | 1 762 | 592 | **23** |
| p6097 | 2 073 600 | 0 | 0 | 0 | 0 | 0 | 0 |
| p5963 | 2 064 900 | 53 793 | 42 211 | 28 983 | 25 115 | 24 818 | 24 818 |

At the user's primary review pose the retracted metric saw **23 px**; the correct
one sees **57 270 px**, with separation p50 0.258 / p90 0.412 / p99 1.014 / max
17.37 world. The 3-world threshold captured **0.04 %** of the population.

Reading it honestly, per pose:
- **p5743 — real see-through.** 56 678 of the 57 270 px lie between 0.18 and 1.0
  world of separation: the scale of an adjacent wall, the floor, or the mortar bed
  behind a proud stone. Only 592 px exceed 1.0 world, so this is *not* the geometry
  slit.
- **p5963 — mostly the slit.** 24 818 px beyond 1.0 world, matching the 24 737 the
  old metric found. That one really was the tear, and the old reading was right
  *there* and wrong everywhere else.
- **p6097 — genuinely none.** A corner pose looking *into* a concave fold; there is
  nothing behind to reveal. The scene does have poses with no see-through; that was
  never the same claim as "the scene cannot show it".

**So the record is corrected: protrusion demonstrably produces see-through at the
pose the user reviews from.** `S1d-2d.6`'s structural explanation ("greets is a
closed room, a mortar valley has nothing to reveal") was an artifact of the
threshold and is withdrawn.

## 8.2 What the viable methods REQUIRE of the base mesh and the charts

Quoted, so the requirements can be checked rather than paraphrased.

**Hirche 2004 — a globally consistent vertex ENUMERATION shared by adjacent faces.**
The diagonal split of each shared side quad must agree between both owners, and the
paper achieves that by index comparison:
> "It has to be ensured that neighboring tetrahedral edges are aligned in a
> consistent way to avoid aliasing effects between adjacent triangles. This can be
> achieved without knowledge of the connectivity in the tetrahedral mesh, by just
> setting up an enumeration of the vertices in the mesh that allows for an index
> comparison. **The enumeration can be obtained from the vertex indices as they are
> usually given in an array.**"

followed by the `IF(v0>v1) SWAP…` ordering that only means anything if adjacent
faces *reference the same vertex indices*. **A mesh with three unique vertices per
face cannot satisfy this at all** — every face's indices are private, so there is no
comparison to make and no reason for two neighbours to split their shared quad the
same way.

**Hirche 2004 — extrusion along the VERTEX normal.**
> "The algorithm iterates over all faces in the triangle mesh folding up a prism by
> displacing every vertex of the base triangle along the vertex normal direction."

If two coincident vertex copies carry different normals, the two prisms' shared side
quad is not shared, and the shell has a gap of exactly the offset width.

**Hirche 2004 — planar prism faces, for the cheap single-pass variant.**
> "The assumption that the prism faces are flat is a very strong restriction that
> makes the algorithm in this form generally unusable. In case the faces are not
> flat, a viewing ray may intersect the same face it originates from which will
> cause holes when rendering."

**Shell maps 2005 — a bijection**, which presupposes a non-self-intersecting,
watertight shell volume [P-claim, secondary].

**PDM 2025 — a parallel offset prism** needs `N_i · N_g` per vertex; it is the
identity exactly when the vertex normals equal the geometric normal, i.e. on a flat
face with shared normals.

**Consolidated, the base-mesh contract is:** (1) coincident positions share one
vertex record, or at least one extrusion direction; (2) a consistent global vertex
ordering so shared quads split identically; (3) manifold edges — every interior edge
has exactly two owners; (4) the *whole* surface is shelled, so no prism abuts a
non-prism; (5) for the cheap variant, planar faces. Charts additionally need (6) a
domain the ray cannot outrun, or a defined hand-off.

## 8.3 The measured partition of greets' actual mesh

### Candidate 1 — TORN EXTRUSION. **This is the discriminator.**

`--pom_shell_weld=1` census, run today on the lid arm:

```
[POM-SHELL-WELD] 'rooms': 155 distinct vertex POSITIONS, 153 carry 2+ copies;
                 420 vertex uses redirected by >1 degree (worst 78.7 deg)
[POM-SHELL-WELD] 'floor':  42 distinct vertex POSITIONS,  30 carry 2+ copies;
                   0 vertex uses redirected by >1 degree (worst 0.0 deg)
```

`rooms` carries **588 vertex uses over 196 faces for only 155 geometric positions**
— a 3.8× duplication — and **420 of those 588 uses (71.4 %) point somewhere other
than the shared direction, by up to 78.7°.** Requirement (1) is violated on 71 % of
the wall, and requirement (2) is unsatisfiable in principle.

Bounds on the per-face partition that follow arithmetically: at most
⌊168/3⌋ = **56 of 196 `rooms` faces are fully clean**, and at least ⌈420/3⌉ = **140
have at least one torn vertex.**

**`floor` violates none of it — worst spread 0.0°.** The floor is planar, so every
copy of a position already shares the normal. That is the working/failing split in
one line: **the floor satisfies Hirche's contract exactly and the walls violate it
on most faces.**

And the confirming experiment is already in the record: the weld is a **pure
geometry change with the march untouched**, and it takes the lid arm's void from
**413 100 → 14 163 px over the 13 review poses, −96.6 %** [M §S1d-2d]. Void is also
measured **linear in the offset** (19 416 / 58 665 / 198 131 / 383 364 px at 0.02 /
0.06 / 0.18 / 0.36 world) and **10 px with the offset forced to zero**
(`--pom_shell_lid_probe`) — the signature of a torn extrusion and of nothing else.
**96.6 % of the failure is the tear.**

### Candidate 2 — MIRRORED CHARTS / HANDEDNESS. **Measured, and refuted as the cause.**

Rendered today: the recess arm at p5743 with `--pom_recess_edge=2` (discard, which
turns the failing population into countable void), material handedness vs per-face
geometric handedness.

| arm | void px | shared with the other arm | unique |
|---|---|---|---|
| `Material::TbnHandedness` (default) | **77 492** | 74 257 | 3 235 |
| `--pom_tbn_face_sign=1` (per-face geometric) | **89 633** | 74 257 | 15 376 |

**74 257 of 77 492 failures (95.8 %) occur under BOTH conventions**, and the
"correct" per-face sign makes the failure **15.7 % worse**. Handedness cannot be the
discriminator: the failing faces fail whichever way the bitangent points. This
agrees with the independent finding in commit `7bfbc87` — applying the material sign
moved slip by *nothing* (p50/p90/p99 0.05/15.28/500.61 before and after), and the
per-face sign flipped 96.8 % of the swimming crop while moving slip p90 only
15.28 → 13.82 and making the cap-8/16 tails **worse** (3.41 → 6.93, 11.37 → 27.12).

Handedness is a real correctness bug with a measured non-effect on this defect.

### Candidate 3 — THE MATERIAL SPLIT MANUFACTURING A PATCH CLIFF. **Refuted in code.**

This was the leading hypothesis handed to me, and it is wrong for greets as built.
The two halves that *are* true:

- **The union-find IS per material.** `PomShell_Build`'s target test is an exact
  name compare — `return F && F->Txtr && F->Txtr->Name && !std::strcmp(F->Txtr->Name, matName);`
  (`DEMO/MeshOps.cpp:4158`) — and the function is called once per material name, so
  two differently-named materials can never share a patch.
- **The sibling-box union cannot bridge materials.** The boxes live on
  `mat->PomShellSibBoxes` / `PomShellSibOfs` (`MeshOps.cpp:4831`), built inside a
  call that only ever sees one material's faces.

**But the split runs AFTER the build, so no cliff is ever created.**
`PomShell_Build` is called at `DEMO/GREETS.CPP:1857`;
`GreetsFixBitangentHandedness` at `DEMO/GREETS.CPP:2588`. At build time the walls
are ONE material, the union-find sees all their faces, and the clone is a memberwise
copy (`*c = *M`, `GREETS.CPP:1296`) that **inherits `PomShellSibBoxes`,
`PomShellSibOfs` and `PomShellUvAmp` by value**, while every `Face` keeps the
`PomShellGroup` it was already stamped with. `S1d-1.2` states this correctly.

The engine also knows the hazard and defends against it. `DisplaceRebuild.cpp:316`,
verbatim:
> "PomShell_Build runs at init BEFORE the Piramid chunk split, the "::mirUV"
> handedness split and the mirror clone build. Re-running it on the final scene
> therefore has to put those three back the way they were for the duration of the
> call, or it solves a different problem: … **"::mirUV" faces: 'floor' has no faces
> left under its own name, so nothing is built for it at all.**"

so the live-rebuild path folds every `::mirUV` face back onto its base material
*before* rebuilding and restores the split afterwards (steps (b) and (d),
`DisplaceRebuild.cpp:340` and `:384`).

**So the artificial material boundary is not a domain boundary.** What the user's
`--poly_viz` capture shows on that diagonal line is real, and two of the three
coincident boundaries are real — the handedness flip and the underlying UV chart
seam — but the patch domain is continuous across it. Given §8.3 candidate 2, the
handedness flip is also not the cause. **The most likely explanation for that
specific capture is that it predates commit `7bfbc87` (2026-08-05), which fixed the
march ignoring `Material::TbnHandedness` entirely — before it, the march built a
FIXED-SIGN N×T and therefore walked the height field *backwards along V* on every
mirrored chart.** That is precisely a defect confined to the magenta region and
seaming against the cyan one. **Unverified** — the discriminating render is §8.5 R0b.

One thing candidate 3 *does* still bind: any future per-material shell or prism
build must fold the clones first, exactly as `DisplaceRebuild` does, or it will
create the cliff that does not currently exist. That belongs in the prism spec
(§8.6), and it is a real trap.

### Candidate 4 — CHART NARROWER THAN THE RAY. **Secondary, real, and face-weighted.**

Computed today from `--pom_seam_census --pom_shell_patch_dump`, face-weighted, own
box vs sibling-rescued, against a grazing travel of `uvAmp 0.03 × cap`:

| material | faces | short side < travel @cap 16 (0.48 UV) | …**and no sibling to rescue it** | @cap 64 (1.92 UV) | …no sibling |
|---|---|---|---|---|---|
| `rooms` | 196 | 86 (**43.9 %**) | 80 (**40.8 %**) | 150 (76.5 %) | 130 (66.3 %) |
| `floor` | 30 | 2 (6.7 %) | **0 (0 %)** | 30 (100 %) | **0 (0 %)** |

The narrowest `rooms` patch is **0.082 UV** across — one sixth of a single grazing
ray's travel — and the distribution has 36 of 196 faces at or below 0.206 UV.
**Again the floor comes out clean at both caps (every floor patch has siblings) and
the walls do not.** This is a genuine second discriminator, it is correlated with
candidate 1, and it is what remains *after* the weld: the 14 163-px residue and the
recess arm's 0.8–8.5 %-of-frame clamp band both live here.

### Candidate 5 — SHELLED ABUTTING UNSHELLED. **Secondary, real.**

Requirement (4). Measured: **424.06 world (30 %)** of `rooms`' 1 393.30 world of
concave boundary has an **unshelled** neighbour — the ceiling `siling` (43 edges)
and the `teleporter` [M §S1d-1.2, reproduced in today's run]. `--pom_shell_weld=5`
(pin against unshelled neighbours only) takes the welded lid's void 14 163 → **10 646
(−25 %)**, i.e. most of the post-weld residue is this [M §S1d-2e.5]. In Hirche's
model this case cannot arise, because every triangle of the surface gets a prism.

## 8.4 The ranking, and what each candidate predicts

| rank | candidate | predicts | measured share of the failure |
|---|---|---|---|
| **1** | **Torn extrusion** — coincident verts, unshared normals | walls fail, floor works; failure ∝ offset | **96.6 %** (413 100 → 14 163 px on a pure geometry fix) |
| 2 | Shelled↔unshelled junction | failures at wall/ceiling, wall/column | most of the 3.4 % residue (14 163 → 10 646) |
| 3 | Chart narrower than the ray | 40.8 % of wall faces unrescued at cap 16; floor 0 % | the post-weld clamp band / smear |
| 4 | Mirrored charts / handedness | — | **refuted**: 95.8 % of failures common to both conventions; "correct" sign 15.7 % worse |
| 5 | Material split → patch cliff | — | **refuted in code**: split runs after the build; clone inherits the domains |

**The partition matches the user's observation.** Faces whose coincident vertices
happen to agree on a normal — the floor entirely, and at most 56 of 196 wall faces —
extrude into a watertight prism and look right, including the see-through §8.1 now
measures. Faces at a torn corner open a slit whose width is the offset, which reads
exactly as *"the method just doesn't work for some case"*.

## 8.5 Fix order — and it is (a) then (b), for the tear reason

**Yes: fix the mesh first, then build the prism.** Building a prism on a mesh with
three unshared vertices per face would reproduce every current failure inside a more
expensive machine, because Hirche's requirements (1) and (2) are preconditions of the
construction, not quality knobs.

But the ordering argument is the **tear**, not the material split — the split is
refuted (§8.3 candidate 3). Retiring `::mirUV` for a per-face handedness bit
(already queued in `docs/OPTIMIZATION_BACKLOG.md`) remains worth doing for its own
reasons (one less material, one less mirror-clone split, and it removes the fold-back
trap the prism spec has to carry), but it is **not** on the critical path for this
defect and must not be sold as the fix.

- **R0a — no code.** Already done in §8.1: the see-through metric is corrected and
  the negative recommendation withdrawn.
- **R0b — one render, no code.** Re-render the user's `--poly_viz` arm on the current
  binary and compare the magenta-region artifacts against his capture. If they are
  substantially reduced, `7bfbc87`'s handedness fix already closed that specific
  complaint and it should not be chased further. This is the cheapest open question
  in the document.
- **R1 — WELD, and make it the default for any lid arm.** `--pom_shell_weld=1` is
  measured at −96.6 % of the void for a pure geometry change. It is the single
  highest-value flag in the campaign and it is still default OFF. Combine with
  `=5` (pin against unshelled) for the −25 % residue, or better, extend the shell to
  `siling` and the columns so requirement (4) holds and nothing needs pinning.
- **R2 — a real weld, not an averaged one.** Modes 1/3 move a welded corner along the
  *mean* normal, which retracts every welded boundary laterally by
  `off·(1−cos(half-fold))` and measurably opens more slit than it closes on the floor
  (14 163 → 24 334 for mode 3) [M §S1d-2e.5]. Mode 4's true mitre is the correct
  construction and measured worse (51 012) — that disagreement is unexplained and is
  a real open bug, not a tuning choice. **It should be root-caused before the prism
  is built on top of it**, because the prism needs exactly this: coincident corners
  landing on one point with no retraction.
- **R3 — then the prism** (§8.6).
- **R4 — chart width, last.** After the weld, the residue is candidate 3/5. Either
  widen the domain (the shipped-POM model, §1.1/§5-R1 — still the cheapest arm in the
  document and still unrendered) or hand off across the seam (S1d-2c).

## 8.6 Prism path — the mesh contract an implementation agent must satisfy

The leading implementation candidate, sized: **8 triangles per shelled base triangle,
back-face culled — ~1 800 faces for greets' 226 shelled quads, ≈ 2 % of the
tessellation carve's measured 86 600** [E, assumes the measured 0.75–0.85 µs/face
holds and side-face pixel coverage is small].

Preconditions, each traceable to a quoted requirement in §8.2:

1. **Weld first (req. 1).** Every coincident position must extrude along ONE
   direction. `PomShell_WeldPrepare` + `--pom_shell_weld=3` already builds a
   scene-wide position bucket before any material moves; the prism must consume that,
   not per-vertex `Vertex::N`. **Blocked on R2**: the mean-normal weld retracts
   boundaries and the mitre variant measures worse — resolve that first.
2. **A consistent global vertex ordering (req. 2).** Hirche's `v0<v1<v2` swap needs
   comparable indices across adjacent faces. On an angle-split mesh, derive the
   ordering from the **quantized position key** (`qpos`, already in `MeshOps.cpp`)
   rather than the vertex index, so both owners of a shared side quad split it
   identically. This is the one place a naive port will silently produce cracks.
3. **Shell the whole surface (req. 4).** `rooms`, `floor`, **and** `siling`, the
   columns and the stairs — 30 % of the wall's concave boundary currently abuts
   unshelled geometry. A prism next to a non-prism is a gap by construction.
4. **Fold `::mirUV` before building (§8.3 candidate 3).** Follow
   `DisplaceRebuild.cpp` steps (b)/(d) exactly: re-point clone faces to the base
   material, build, restore. Otherwise the patch/prism grouping fragments at the
   handedness line and `floor` gets no shell at all.
5. **Hide mirror clones during the build** — `DisplaceRebuild.cpp:326` zeroes their
   `FIndex`; without it `rooms` goes 67 → 113 patches and the domain changes for the
   real walls too (measured 58 % of the frame).
6. **Planar faces are already satisfied (req. 5)** — greets' walls are flat
   axis-charted quads with per-face constant tangent frames, so the cheap
   single-pass prism variant applies and no per-frame Projected-Tetrahedra CPU
   decomposition is needed. This is the whole reason the path is affordable.
7. **Depth write is required and already exists** — `--pom_depth_write`, measured
   +0.1 ms.
8. **Discard on prism exit is correct** and must stay a discard: the neighbouring
   prism's own fragment answers the pixel, arbitrated by Z. Do **not** add a clamp;
   that is what the lid-only arm needed and it is what produced the rust stripe.

Gates it must pass, all pre-existing: void vs tessellation at all 13 review poses;
the §8.1 see-through metric (slab-relative, **not** 3-world); offscreen shadow-cube
and mirror deltas; error vs a converged reference of its own semantics; and the
side-faces' measured **pixel** coverage, which is the one number in the cost estimate
with nothing behind it.

## 8.7 Reproduction

All numbers in §8 come from these, run on `fog-wt` at `2bac9a4`, dummy SDL drivers,
1920×1080, every run log grepped for `unknown flag` / `requires a value` (zero hits
across all 10 runs):

```sh
# §8.3 candidate 1 + 4 — censuses (init-time only, no behaviour change)
./DEMO --snapshot=greets@t=5743 --deferred --pom_shell --pom_recess_only \
       --pom_seam_census --pom_shell_patch_dump
./DEMO --snapshot=greets@t=5743 --deferred --pom_shell --pom_shell_weld=1 \
       --pom_shell_patch_dump

# §8.1 — see-through, flat vs lid, 3 poses, FDS_SNAPSHOT_ZDUMP=1 --face_id_dump
#   lid arm: --pom_shell --pom_shell_weld=1 --pom_shell_side_faces=3
#            --pom_shell_lid_edge=1 --no-pom_shell_base_clip
#            --pom_shell_world_amp --pom_shell_world_amp_set=0.18 --pom_normal
#            --parallax_pom_cone --parallax_pom=32 --pom_cone_exact=1
#            --pom_cone_min_step=1 --pom_march_earlyout --pom_shell_cap=16
#   sep = (z16_flat - z16_lid)/395.64 ; see-through = sep > 0.18 (the slab)

# §8.3 candidate 2 — handedness, recess arm + --pom_recess_edge=2 (void = failure)
#   A: default (Material::TbnHandedness)      B: --pom_tbn_face_sign=1
```

Scripts and dumps: session scratchpad `rq/` (`st.sh`, `census.out`, `weld.out`,
`h_matsign/`, `h_facesign/`).
