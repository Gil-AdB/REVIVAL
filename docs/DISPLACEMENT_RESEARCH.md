# Displacement rendering for the greets stone — literature survey + engine mapping

> **CORRECTION 2026-08-06 — every "+54.5 ms" / "way too slow" statement about
> `--greets_displace` in this file is STALE by 7.4×.** Re-measured from scratch
> on fog-wt tip: the tessellated arm costs **+7.3 ms** over flat POM at t=5780
> (and is CHEAPER than the recess-shell arm at three of six review poses). The
> retirement number predated `9b6d70d --tile_bbox_cull` (which landed 1 h 40 m
> after the edge-carve commit and measures −12.5 ms of delta on its own),
> `a1f89d4 --xfrm_soa_inline` (−2.0 ms) and `799c808` (the faceless mesh was
> **84.3 % of that arm's shadow verts**), and the mirror clone was
> re-transforming the whole tessellated wall (−5.9 ms, now
> `--greets_displace_flat_mirror`). Full tables, look crops and gates in
> **`docs/ENVDYN_DISPLACEMENT_PLAN.md` §ADDENDUM 2026-08-06**. Leave the
> historical numbers below as written; do not reason from them.


Research deliverable, 2026-08-04 (fog-wt). Remit: survey the displacement-rendering
literature and determine what maps into THIS engine at reasonable cost — either a
much cheaper poly path or a true per-pixel displacement technique — including
explicit verdicts on the two user-named ideas (VDM; a max-height offset plane +
sphere-bound ray tracing). Research only; nothing here is implemented.

Evidence discipline: **[M]** = measured in this repo (docs/HEIGHTMAP_POM_PLAN.md,
docs/ENVDYN_DISPLACEMENT_PLAN.md, docs/VISIBILITY_PLAN.md, all on this machine,
1920×1080 threaded unless stated). **[P]** = paper claim, on the paper's hardware.
**[E]** = my estimate, assumptions stated. Costs in ms are never dismissed as
negligible; single-digit ms matter here.

---

## 1. Executive summary

Two working implementations exist for the greets stone relief, both measured at the
worst pose (t=5780, walls fill the frame, ~1.2–1.9 M wall pixels of ~2.08 M shaded):

- **A. Geometric tessellation bake** (`DisplaceStoneSubdiv`, edge-aligned plateau
  carve): looks right — true silhouettes, real depth, works with every downstream
  consumer — but costs **+54.5 ms** (86.6 k faces vs 52.5 ms flags-off) [M], split
  across RNDR + a per-frame shadow-bake re-raster (+~9 ms at 43 k) + XFRM (+~3 ms)
  [M]. Visibility culling cannot reclaim it — the polys are the looked-at wall
  (VISIBILITY_PLAN §7, code-tested) [M]. Remaining poly levers: chunk LOD + SoA
  transform.
- **B. Per-pixel POM** (naive N-step and relaxed-cone marches in `Mekalele.h`):
  **+1.7–2 ms** threaded for naive-8, **+9.4 ms** for the converged cone-8 march
  [M] — 6–30× cheaper than A — but: no silhouettes, **no depth write** (relief is
  invisible to SSAO/fog/Z-consumers), and grazing-angle quality limits.

The survey's central finding: **the part of the problem the literature spends most
of its effort on — finding the ray/height-field intersection cheaply — is already
solved and measured cheap in this engine.** The march is arithmetic-bound, not
gather-bound (relief mapping's fewer-taps variant was tried here and won nothing
[M]). What the classic GPU papers could *not* do, this engine *can*: **we own
ZPage16**, so a marched pixel can write its true depth; and the raster is ours, so
a missed march can kill the pixel (masked stores already exist). Those two facts
convert 1998-era POM into a **shell-traced displacement with real depth and real
silhouettes** — the technique family of Hirche 2004 / shell maps / Policarpo — for
an estimated **+3–12 ms** at the worst pose [E], not +54.5 ms.

On the two named ideas:
- **VDM (Wang 2003): rejected, with numbers.** Its per-texel×azimuth weight data
  scales to ~**400 MB** for our 1024² wall map (§5.1) [E from paper numbers], its
  14 gathers/pixel land on our weakest axis (scalar per-lane gathers) [M-anchored],
  and it solves a problem (march cost) measured cheap here, while its silhouette
  machinery (curvature bending) does nothing for flat interior walls with open
  boundaries — a case the paper explicitly does not handle [P].
- **Offset plane at max height + sphere tracing: half right, and the right half is
  valuable.** The offset-plane (shell) part is shortlist candidate S1 — it is what
  buys silhouettes + depth. The sphere-tracing part (Donnelly's 3D distance field)
  is a *valid* march accelerator but a weak fit for this content: sphere tracing
  wins by leaping through empty space, and the greets stone is shallow,
  mostly-solid relief — the same reason the baked cone map was measured to barely
  contribute [M]. Keep the shell, drive it with the existing cone/bisect or naive
  march (§5.2).

### Decision matrix

Worst pose = full-screen wall, 1080p, threaded, this machine. "id-scheme" = the
PolyId identity-test cube shadows (no depth compare). Memory = for our texture set
(wall 1024² + floor variant), including mips (~×1.33).

| Technique | Silhouettes | Depth write | Self-shadow vs id-scheme | Precompute + memory (our set) | Per-px cost @worst pose | SIMD fit (8-wide) | Integration risk |
|---|---|---|---|---|---|---|---|
| **A. Tessellation bake (ships, default-off)** [M] | yes (true) | yes | none by design (single wall id kills acne); cast-on-others via shadow bake | init bake 2–6 s; ~+80 k faces resident | **+54.5 ms** [M] (incl. shadow bake +~9, XFRM +~3) | n/a (per-poly front-end) | 0 (done) |
| **B. Naive-8 POM (ships, default-off)** [M] | no | no | no (needs light march, +~1–2 ms [E]) | none beyond height map (~2.7 MB set) | **+1.7–2 ms** [M], swims | excellent (fixed-count) | 0 (done) |
| **B′. Cone-8 + bisect (ships)** [M] | no | no | same as B | cone bake 62/230 ms init [M]; +~2.7 MB | **+9.4 ms** [M], converged | good (fixed-count, exact div) | 0 (done) |
| **S1a. B + depth write** | no | **yes** | receives shadows same as today (wall id) | none new | +2.5–3 ms naive [E]; +10–11 ms cone [E] | same as B | low; Z-order audit needed |
| **S1b. Offset-shell POM (lid at maxH, march down, miss→discard, Z write)** | **yes** (block edges, grazing occlusion; authored borders fade like A) | **yes** | receives per wall-id; casts none (shadow bake sees flat lid); optional light march +~1–2 ms [E] | init lid extrude (ms-class [E]); no new maps | **+3–4.5 ms naive-8-quality; +8–12 ms converged** [E] | same as B/B′ | medium: borders/corners, coverage shift, pin sweep |
| **S3. Offline exact relaxed cones (EGSR '24) + secant refine** | (inherits S1) | (inherits S1) | (inherits S1) | offline bake + disk cache (~1.4 MB/map, 8-bit) | −2–4 ms off the converged march [E] | same | low (drop-in map + refine swap) |
| **Donnelly 3D distance field sphere trace** | with shell: yes | yes | same as S1 | Danielsson O(n) bake; 256²×32 = 2 MB, 512²×32 = 8 MB /map [P/E] | ≈ cone class, unlikely better on solid relief [E; M-anchored] | fixed-count OK; 3D gathers scalar | medium; new 3D sampler |
| **Max-mipmap / quadtree traversal (Tevs '08, Drobot '10)** | with shell: yes | yes | same as S1 | trivial bake, +⅓ map memory [P] | guaranteed hit; branchy descent → ≥ cone class here [E] | **poor** (divergent traversal depth) | medium |
| **VDM (Wang '03)** | curved-mesh silhouettes only; open boundaries unhandled [P] | derivable from stored distance | built-in (2nd lookup) [P] | **~400 MB @1024²** [E]; 68→4 MB only at 128² [P] | 14 gathers + 40 ALU/px [P] — gather-shaped | poor (gather-heavy) | **reject** |
| **GDM (Wang '04)** | yes (5D volume) [P] | same | built-in [P] | 5D volume — worse than VDM for tiling set [E] | shader-heavy, gather-shaped | poor | **reject** |
| **Prism/tetrahedra extrusion (Hirche '04, shell maps '05)** | yes | yes | n/a here | ×3 tetrahedra per face — face-count multiplier | per-px march + heavy geometry front-end | n/a | dominated by S1b on flat walls |
| **TFDM / RMIP (Adobe '21/'23)** | yes | yes | n/a | minmax mip, low footprint [P] | built for RT BVH traversal, not raster fill | — | wrong infrastructure; ideas only |
| **DMM (NVIDIA '22)** | yes | yes | n/a | ~1 byte/µtri class [P] | HW-RT-specific | — | ideas only (LOD + watertight edge rules) |
| **S2. Chunk LOD on the bake (A + per-chunk level)** | yes (near); POM-carried far | yes (near) | as A | LOD ladder resident ~25–40 MB [E] or lazy re-bake | reclaim **~half+ of A's +54.5** at typical poses [E, from ~0.75–0.85 µs/face] | n/a | medium: crack pins exist, geomorph optional |

### Shortlist (detail in §4)

1. **S1 — depth-writing offset-shell POM.** The user's offset-plane idea, executed
   with the existing march. Two tiers: S1a depth-write only (no geometry change,
   recessed look, Z-consumers see relief) → S1b lid extrusion (true block-edge
   silhouettes). Estimated +3–12 ms at the worst pose depending on march quality
   [E] vs A's measured +54.5 ms — and it avoids A's shadow-bake and XFRM costs
   entirely, because the shadow/transform world stays flat.
2. **S2 — chunk LOD for the tessellation bake** (the backlog S5 lever, now with a
   literature-backed error metric + crack story). Keeps A's look where it matters;
   estimated reclaim of half or more of A's cost at typical poses [E].
3. **S3 — march-quality upgrade inside S1**: offline exact relaxed-cone bake
   (Robust Cone Step Mapping, EGSR 2024) cached to disk for our one texture set,
   + secant/interval refinement replacing some bisection steps. Estimated −2–4 ms
   off the converged march [E].

S1 and S2 compose as a ladder (near = tessellation, far = shell POM, both fed by
the existing residual-height split), but S1 alone may retire the tessellation path
for the demo's actual camera work — that is a look call to make on captures.

---

## 2. Engine constraints that decided the verdicts (all verified in-repo)

- Deferred CPU raster; the POM march runs **at G-buffer fill** in
  `FDS/FILLERS/Mekalele.h` (~1006–1170), 8-wide SIMD rows, where the smooth float
  UV lives. Flags: `parallax_pom`, `parallax_pom_cone/_refine/_relax/_quarter/_lod`,
  `pom_height_mip` (`FDS/Base/FeatureFlags.def`).
- **We own the Z-buffer**: `ZPage16`, encode `0xFF80 − zscale·z`. A march can write
  real depth — the single constraint that shaped 2004–2010 GPU papers does not
  bind here. Masked stores already kill lanes (edge/Z), so per-pixel discard is
  architecturally free.
- **Shadows are id-buffer cube maps** (PolyId identity, no depth compare). The
  displaced-wall campaign already forced single-id-per-wall (acne) then
  parent-plane ids (bleed) [M]. Consequence: *neither* path self-shadows relief
  today; per-pixel relief keeps the flat wall's id and changes nothing in the
  shadow system. Relief self-shadow, if wanted, must be a height-space light march
  (or horizon maps) in either path.
- March economics [M]: ~0.25 ms/tap threaded at full wall coverage; naive-8
  +1.7–2 ms; cone-8+refine-6 +9.4 ms converged; **arithmetic-bound** (relief
  mapping's tap reduction won nothing; quarter-res offset field saves only
  ~14–19 %, not ÷4). Gathers are scalar per lane — gather-heavy techniques are
  penalized, fixed-count arithmetic marches are the engine's fast shape.
- Honest cone finding [M]: the conservative cone map barely contributes at
  relax=4 — bracket+bisect does the work. The *proper* relaxed bake (GPU Gems 3
  second-intersection march) was deferred as ~100× the runtime bake budget — but
  we have ONE tiling texture set, so an offline+disk-cache bake amortizes to zero.
- Texture set: `rooms` 1024² 8-bit height (block pitch ~256 texels) + floor
  variant; existing per-material tables: height, cone, residual ≈ 1.37 MB each
  with mips. A few more MB of per-texture tables is acceptable; hundreds are not.
- Frame elephants for context [M]: cube-shadow tap ~32 ms, deferred kernel
  ~12 ms, shadow bake ~12–27 ms; frames 48–107 ms. Per-pixel costs scale with
  wall coverage (worst poses are exactly full-screen wall); the geometry path's
  BAKE/XFRM costs are camera-independent [M].
- wasm caveat: 8-wide AVX2 maps to 4-wide wasm SIMD — expect roughly ×2 on all
  per-pixel march costs there [E].

---

## 3. The literature, mapped

Taxonomy spine: Szirmay-Kalos & Umenhoffer's STAR classifies displacement methods
as per-vertex (tessellate + move) vs per-pixel, and per-pixel as **unsafe** (fast,
may miss features), **safe** (guaranteed first hit), and **combined** (safe
bracket + fast refine). Our shipped cone+bisect march is a textbook "combined"
method. [P]

### 3.1 Unsafe iterative family (we ship this)

- Parallax mapping (Kaneko '01, Welsh '04 offset-limiting): the tier-0
  single-shift. In-repo [M].
- Steep parallax (McGuire & McGuire, I3D '05 poster), POM (Tatarchuk, I3D '06 —
  linear search + one secant step, soft shadows, LOD): the naive-N march.
  In-repo as `parallax_pom=N` [M].
- Relief mapping (Policarpo, Oliveira, Comba, I3D '05): linear bracket + binary
  refine; also the canonical **depth-correction** reference — the fragment writes
  the marched intersection's depth so relief interacts with the Z-buffer. Tried
  in-repo for speed (no win, arithmetic-bound [M]); its depth-write idea is S1a.
- Interval mapping (Risser, Shah, Pattanaik '06): secant instead of midpoint in
  the refine — fewer iterations for the same landing. Cheap drop-in for our
  6-iteration bisection (S3) [P].

### 3.2 Safe / combined marches (precomputed acceleration)

- Cone step mapping (Dummer '06), relaxed cone stepping (Policarpo & Oliveira,
  GPU Gems 3 ch. 18, '07). In-repo, measured, with the honest finding above [M].
- **Robust Cone Step Mapping (Bán, Valasek, Bálint, Vad — EGSR 2024)**: an
  *exact* relaxed-cone construction (any ray inside a cone crosses the field at
  most once — the guarantee the GG3 bake only approximates and sometimes gets
  wrong [P]), plus bilinear-safe cone maps and an artifact-free minimum step.
  Directly answers our deferred "proper relaxed bake too slow at runtime": bake
  offline once per map, cache to disk (same culture as `city_envmap*.bin`).
  Companion: Quick Cone Map Generation on the GPU (Bán & Valasek, EG '22 short).
- Maximum mipmaps (Tevs, Ihrke, Seidel, I3D '08): max-pyramid over the height
  map; hierarchical descent with guaranteed first hit; negligible bake, +⅓
  memory, view-dependent LOD [P]. Drobot's Quadtree Displacement Mapping (GPU
  Pro, '10) is the production version + height blending + approximate soft
  shadows [P]. **Engine fit is poor**: the descent is data-dependent branching —
  8 lanes diverge in traversal depth and the row runs at the slowest lane; our
  measured arithmetic-bound march says tap *count* is not the cost. Worth noting
  as the trivial-bake alternative if the cone bake ever becomes a problem; not
  otherwise.
- Distance fields / sphere tracing (Donnelly, GPU Gems 2 ch. 8, '05): 3D distance
  map (paper: 256×256×16, up to 512×512×32 [P]), fixed 16 iterations (8 for
  smooth fields [P]), Danielsson O(n) bake [P]. See §5.2 for the verdict.

### 3.3 Silhouette-correct family (shell/prism) — the family S1b belongs to

- Hirche, Ehlert, Guthe, Doggett (GI '04): extrude base triangles along normals to
  a max-height shell; raster the shell (as tetrahedra) and march inside it; pixels
  that exit without a hit are discarded → true silhouettes.
- Shell maps (Porumbescu, Budge, Feng, Joy, SIGGRAPH '05): formalizes the shell
  volume ↔ texture-space bijection via tetrahedral barycentrics (general volumes,
  not just height fields). Smooth/curved variant: Jeschke et al. '07.
- Policarpo & Oliveira, Relief Mapping of Non-Height-Field Surface Details
  (I3D '06) + Oliveira & Policarpo '05 (per-vertex quadric coefficients): curved
  silhouettes for relief-mapped *closed* surfaces; also multi-layer height fields.
- Prism Parallax Occlusion Mapping with Accurate Silhouette Generation (Dachsbacher
  & Tatarchuk, '07): POM inside extruded prisms.
- Engine mapping: our walls are **flat axis-charted quads**, so the whole
  tetrahedral machinery collapses — the shell of a flat quad is a box, the entry
  point is the rasterized fragment on the offset lid, and the tangent frame is
  constant per face. S1b is this family reduced to its cheapest special case.
  The general curved-surface versions are not needed (greets stone is flat; the
  mummies keep their geometric subdivision path).

### 3.4 Precomputed exotic: VDM / GDM

- View-Dependent Displacement Mapping (Wang, Wang, Tong, Lin, Guo, Shum, Hu,
  SIGGRAPH '03) and Generalized Displacement Maps (Wang, Tong, Lin, Hu, Guo,
  Shum, EGSR '04): precompute ray-intersection answers over (u,v,view,curvature)
  — 5D — and SVD-compress. Numbers and the §5.1 verdict below.

### 3.5 RT-era (2021–2024) — idea sources, wrong infrastructure

- Tessellation-Free Displacement Mapping for Ray Tracing (Thonat, Beaune, Sun,
  Carr, Boubekeur, SIGGRAPH Asia '21): minmax-mipmap as an implicit BVH over the
  displacement map, affine-arithmetic bounds during traversal, low memory
  footprint vs pre-tessellation [P].
- RMIP (Thonat, Georgiev, Beaune, Boubekeur, SIGGRAPH Asia '23): inverse
  displacement + oblong (anisotropic) bounds; 2×–10× over prior displacement-RT
  [P]. Also: Nonlinear Ray Tracing for Displacement and Shell Mapping (Seyb et
  al., SIGA '23); Projective Displacement Mapping (arXiv '25).
- These solve "which base triangle's shell does an *arbitrary* ray enter, with
  tight bounds" — a problem a rasterizer does not have (the fragment tells us the
  face). Their in-shell traversal is the Tevs/Drobot machinery (§3.2 verdict
  applies). Mined for ideas, not adopted.
- NVIDIA Displaced Micro-Mesh (Ada '22; Maggiordomo & Karis-adjacent construction
  work, SIGGRAPH '23): per-base-triangle subdivision *level* + compressed
  µ-vertex displacements (~1 byte/µtri class [P]) + **watertight edge rules**
  between triangles at different levels. HW-RT-specific as shipped, but the LOD
  cost model and the edge-consistency discipline are exactly what S2 needs.

### 3.6 Poly-side LOD (for S2)

- Chunked LOD (Ulrich, SIGGRAPH '02 course): per-chunk static levels, screen-space
  error `ρ = ε/D · viewport_scale`, stitch or skirt the seams.
- CDLOD (Strugar '09): continuous distance-based morph — no stitching, no pops;
  the morph is a vertex lerp toward the parent grid.
- Geometry clipmaps (Losasso & Hoppe '04): nested regular grids — terrain-shaped,
  less applicable to our chunked walls.
- Nanite (Karis, Stubbe, Wihlidal, SIGGRAPH Advances '21): cluster DAG LOD chosen
  by projected error, software raster for small triangles; UE 5.2+ adds
  displacement as on-demand patch tessellation inside that structure. The
  long-term shape of a poly path; our per-chunk two-or-three-level ladder is the
  pragmatic subset.

---

## 4. Shortlist — integration sketches + cost models

### S1 — depth-writing offset-shell POM (per-pixel path completed)

> **✅ S1a IMPLEMENTED (2026-08-04, fog-wt) — `--pom_depth_write`, default OFF.**
> Both marches (naive + cone) write the marched crossing's depth to ZPage16;
> flag-off byte-identical (render_gate 3/3, city `37e62845`, fountain
> `51fff7cd`). Implementation detail that superseded the sketch below: instead
> of `Δz = t·amp/…` with a divide, the depth uses the offset-limited-consistent
> closed form `Δz = (h−0.5)·strength·w·(Vz + Nz·(1−VtN))` — no divide, bounded
> |Δz| ≤ strength·w at any angle, with `w` = per-face world-per-UV-tile from a
> per-triangle Lengyel solve on the raster inputs (so the Z relief is exactly
> the relief the texels show, and no `amp` flag is needed). Measured [M],
> greets 1080p threaded: zscale 395.64 (FZP 150; 1 code = 2.5e-3 world);
> zEnc relief span at t=5780 (naive-8, full map, strength 0.3): −185..+13
> codes = −0.47..+0.03 world, 36.5 % of frame pixels moved; cone-8+refine:
> med +5 codes (honest slight protrusion, map mean 0.55), grooves to −159.
> Cost [M]: depth write itself = **+0.1 ms median** (7 interleaved pairs,
> run noise ±1.3 ms) on top of the march — the +0.3–1 ms estimate's floor, so
> naive-8 TOTAL ≈ +2 ms, under the +2.5–3 ms estimate. SSAO/GTAO is the
> headline: the AO-debug field goes from a featureless gradient to crisp
> per-joint contact darkening. Ordering hazard swept (momy statue, jambs,
> t=6097): no geometry artifacts, only soft AO shifts; adjacent-t frame-step
> deltas unchanged (13.34→13.30 mean|Δ|) = no added shimmer. Residual config
> (`--greets_displace`): wall residual depth is sub-quantum (geometry carries
> the band — no Z double-count by construction); the grazing FLOOR carries
> near-full relief in its residual (cracks are finer than mip2) and the
> naive-8 late-crossing bias shows as ~−0.29 world median there — a march-
> quality issue (cone fixes it), not a depth-write issue. S1b (lid + discard)
> remains open.

**Tier S1a — depth write, no geometry change.** In the Mekalele fill, after the
march lands at tangent-space depth t (uf,vf known; the code already has the
per-lane marched height), compute the view-space depth delta along the ray and
store `0xFF80 − zscale·(z + Δz)` instead of the planar z, for parallax-material
pixels only. Δz = t · amp · (view-ray z-component scaled by the same TBN math the
march already built) — a handful of FMAs per lane [E]. Everything downstream reads
ZPage16: the deferred kernel's §5 reconstruction, SSAO (grooves gain contact
darkening — the AO-map look for free), fog/froxels, volumetrics, env-dyn overlay
depth compares, prev-frame hi-Z. G-buffer, shadow ids, sort order all unchanged.

- Correctness audit needed [E → verify]: recessed z is *deeper* — faces sorted
  behind the wall within the relief band (≤ amp ≈ 0.3 world) could now win groove
  pixels. Walls are thicker than amp, and borders pin to zero in the bake culture,
  so the expected exposure is abutting trims/jambs; sweep the campaign poses with
  a z-diff (`_z.pgm` snapshots exist for exactly this).
- Cost [E]: +0.3–1 ms over the current march at worst-pose coverage (encode +
  store + Δz math; the store replaces an existing planar-z store). Naive-8 total
  ≈ +2.5–3 ms; cone-8 total ≈ +10–11 ms.

**Tier S1b — the offset lid (the user's #2, minus the sphere tracer).** At init
(MeshOps, next to `DisplaceStoneSubdiv`), extrude each stone-material quad's
interior along its normal to `+amp·maxH`, pinning authored-border verts at zero
exactly like the geometric bake's border pinning (one subdivision ring per quad,
~×4 faces on the ~230 wall/floor quads — hundreds of faces, not tens of
thousands [E]). The rasterized fragment now sits ON the max-height surface:
- March **downward** from h=maxH (single-sided — simpler than today's
  bidirectional (h−0.5) convention; matches white=protruding).
- **Miss ⇒ kill the lane** (fold into `p_mask` before the gather/store block —
  the mechanism the fill already uses for edge/Z rejection). This is what creates
  true silhouettes: grazing rays that clear a block top and exit the shell leave
  those pixels to the geometry behind.
- **Hit ⇒ write marched depth** (S1a math, now spanning the full shell).
- Borders: relief amplitude fades to zero at authored edges (the lid is pinned
  there), so wall↔wall corners and jamb junctions stay closed by construction —
  the same contract the geometric bake proved out. No side-skirt geometry needed
  at pinned borders.
- Shadow story vs the id scheme: the lid inherits the parent plane's ShadowMatID
  (S4c machinery — one id per wall). Walls occlude other geometry by identity as
  today; relief neither self-shadows nor casts (the shadow cube sees the flat
  lid). This is *visual parity with the geometric path as shipped* — that path
  also deliberately runs one id per wall and does not self-shadow relief [M].
  Optional relief self-shadow for both paths: a short light-direction march
  toward the dominant light (~+1–2 ms threaded for naive-4-class [E]) or baked
  horizon maps (Max '88; Sloan & Cohen '00 — 8 dirs × 1024² ≈ 8–11 MB with mips
  [E]; gather-shaped, the march is likely the better engine fit).
- What S1b does NOT give vs the tessellation bake: relief-cast shadows onto other
  objects (bake-raster of displaced geometry — worth +~9 ms/frame in path A [M]),
  and protrusion beyond the authored wall plane silhouette is bounded by the lid
  (correct within the shell). Mirrors/env-probes: the march runs in those raster
  passes too — cost multiplies per pass at their (smaller) coverage.
- Interaction detail: the residual-height split (B4) is not needed when S1b
  carries the FULL map (geometry stays flat) — S1b marches the original height
  map; the residual only returns if S1b is combined with tessellated near chunks
  (§ ladder below).

**Cost model [E — assumptions: worst pose ~1.9 M wall px, measured tap costs
hold, lid raster coverage ≈ base coverage]:**

| S1 configuration | est. threaded @worst pose | quality |
|---|---|---|
| naive-8 + Δz + discard | **+3–4.5 ms** | swims like today's naive-8, but with depth + silhouettes |
| cone-8 + refine-6 + Δz + discard | **+10.5–12 ms** | converged, no swim |
| + S3 (exact cones, secant refine) | **+7–9 ms** | converged |
| + `parallax_pom_lod` fade engaged (corridor framings) | pose-dependent −0.3…−few ms [M-anchored] | far walls degrade to single-shift |

vs path A's measured +54.5 ms — an estimated 5–15× cost reduction for the same
feature checklist minus relief-cast shadows. Risk register: Z-order exposure
sweep, corner/lid coverage shift (the lid moves silhouette edges out by ≤ amp),
determinism (byte-null flag-off, campaign-pose pins), quarter-res offset field
interaction with discard (an odd lane borrowing a killed even lane's depth must
re-march — the coverage-validity mask noted in HEIGHTMAP_POM_PLAN's staged work).

**Validation recipe** (house style): `--snapshot=greets@t=5780` + `_z.pgm` diff
proves the depth write; `FDS_DUMP_TXTR` UV metric gates march landings; the
silhouette check is the DisplaceTest rig's top-down square-wave pose
(`FDS_DISPLACETEST_DUMP=1`) — the same instrument that convicted the dome path.

### S2 — chunk LOD on the tessellation bake (poly path completed)

The bake already emits per-chunk face lists (`--greets_chunk_size`), the carve is
deterministic per level, and the crack machinery (per-param-list side registry +
polyline pinning: finer side pinned onto the coarser side's displaced segment)
already heals mixed-level seams [M — S2/edge-carve rounds]. What's missing is only
the *selection*:

- **Bake** L∈{0=flat+full-POM, 1≈dome/coarse carve, 2=edge carve} per chunk at
  init; keep all levels resident (~140 k faces total ≈ 25–40 MB at Face+Vertex
  struct sizes [E]; alternative = lazy re-bake, but init bakes measured 2–6 s [M]
  are too slow for live switching).
- **Select** per chunk per frame by projected relief error: screen px of the
  relief step ≈ `amp · FOVX / viewZ` — with amp 0.3 and FOVX ~1.4 k px: ~84 px at
  z=5, ~10 px at z=40, ~5 px at z=80 [E]. Drop a level when the step falls under
  a few px; distant walls carry the look in the POM residual (the B4 machinery
  exists precisely to keep geometry-scale and POM-scale from double-counting).
- **Seams**: pin the finer chunk's boundary verts to the coarser neighbour's
  segment (the existing S2-crack rule applied across chunk borders); DMM's edge
  rule (an edge's level = min of its two owners, decided *per edge*, not per
  chunk) is the literature-hardened version. Geomorph (CDLOD-style vertex lerp)
  only if block-scale pops read on camera moves — at ~10 px steps they may.
- **Shadow bake + XFRM**: LOD applies to the shadow-bake world too (it re-rasters
  the walls every frame [M]) — but the shadow camera's error metric must be its
  own (a light close to a far-from-camera wall). Conservative rule: shadow world
  uses max(level over lights in range) [E].
- **Cost model** [E from measured ~0.75–0.85 µs/face threaded whole-frame across
  the 52.5→74→107→121 ms / 10 k→43 k→87 k→103 k points]: a pose with the near
  wall at L2 and the rest at L0–L1 lands around 30–45 k faces → **+20–30 ms
  instead of +54.5** — real but still an order above S1's estimate. S2 is the
  path if the look review rejects per-pixel relief at close range (grazing-angle
  micro-quality is the one axis where geometry stays strictly better).

### S3 — march-quality upgrades (slots into S1, independent value)

1. **Offline exact relaxed-cone bake** (EGSR '24 construction) for the two maps,
   cached to disk keyed by map hash (env-bake culture). Expected effect: the
   cone map finally carries real per-texel guidance, so relax returns to ~1 and
   bracketing needs fewer taps; with the bilinear-safe correction the min-step
   artifacts that forced conservative settings disappear [P]. Estimated −2–4 ms
   off the converged cone-8 configuration [E]; measure with the FDS_DUMP_TXTR
   metric against naive-256 truth exactly as before.
2. **Secant (interval) refinement** replacing 3–4 of the 6 bisection iterations
   [P: interval mapping]; arithmetic-only change in the refine loop, SIMD-neutral.
3. Keep the LOD fade + (fixed) quarter-res offset field as the coverage dials;
   their measured savings are modest [M] but compose.

### The ladder (S1 + S2 composed)

Near chunks (relief step > ~30 px [E]) tessellated at L2, carrying only
geometry-scale relief, POM marching the **residual** map on top (shipped B4
behavior); everything else flat with S1b marching the **full** map. One knob
(projected-error threshold) moves the frontier; both endpoints are already
byte-gated features. This is the configuration to A/B against "S1b everywhere" —
if the close-range look holds, the tessellation path retires and +54.5 ms comes
off the table entirely.

---

## 5. The user's two ideas, answered directly

### 5.1 VDM — not viable here; the numbers kill it independently of taste

Paper facts [P]: 128×128 height field × 32×8 viewing directions × 16 curvatures
= 64 MB raw + 4 MB MVM (max-view-angle map for silhouettes) = 68 MB, SVD to
**4 MB** using 8 VDM + 4 MVM eigen-functions; runtime = one PS2.0 pass, **40 ALU
+ 14 texture lookups per pixel**; silhouettes come from bending by per-vertex
curvature; open surface boundaries explicitly unhandled.

Mapping to this engine:
1. **Memory scaling is per-texel×azimuth and does not SVD away.** The SVD
   compresses the (θ, curvature) axes; the weight maps W(x,y,φ) remain ~12
   coefficients × 32 azimuths per texel ≈ 384 B/texel at 8-bit. At the paper's
   128² that is ~6 MB (matches their 4 MB after quantization); at our 1024² wall
   map it is **~400 MB** — two orders past the table budget, for one material
   [E, derived from paper structure]. Downsampling the spatial axis to fit
   (256² → ~25 MB) throws away exactly the block-edge frequencies the technique
   would exist to keep.
2. **The runtime shape is wrong for this CPU.** 14 dependent lookups per pixel is
   gather-shaped; our gathers are scalar per lane and the engine's measured fast
   shape is fixed-count arithmetic marches [M].
3. **It solves the wrong bottleneck.** VDM's value is skipping the intersection
   search; ours is measured at +1.7–9.4 ms threaded [M] — already inside budget.
4. **Its silhouette power does not apply.** Flat interior walls have zero
   curvature: VDM degenerates to a view-indexed POM table, and its silhouettes
   (curvature bending of a closed surface) never engage; boundary silhouettes at
   jambs/corners are the "open boundary" case the paper defers. GDM (EGSR '04)
   generalizes to non-height-fields with a 5D volume and inherits the same
   memory/gather story.

Verdict: **reject** for this content. The one configuration where a VDM-family
idea returns is a far-field *horizon* table for self-shadow (a 3D slice of the
same data), and even there the light march is the better engine fit (§4-S1b).

### 5.2 Max-height offset plane + sphere-bound ray tracing — adopt the plane, skip the spheres

Split the idea:
- **The offset plane is the valuable half** — it is the flat-wall special case of
  the shell/prism family (§3.3) and the mechanism behind shortlist S1b:
  rasterize the relief's upper bound, march down, discard misses, write hit
  depth. It is what converts POM's two disqualifiers (no silhouettes, no depth)
  into solved problems using machinery this engine already has (owned Z, masked
  stores, border pinning, TBN at fill).
- **Sphere tracing (Donnelly '05) is a valid but ill-fitting accelerator for this
  content.** Facts [P]: 3D distance map (256×256×16 typical, 512×512×32 for
  complex data), fixed 16 iterations (8 for smooth fields), O(n) Danielsson bake.
  Engine analysis: (a) sphere tracing's advantage is leaping across *empty*
  space; the greets stone is shallow, mostly-solid relief — the same content
  property that made our baked cone map barely contribute [M]; near-surface and
  grazing rays degenerate to a crawl exactly where our quality problem lives.
  (b) Each step needs a 3D gather (scalar per lane) vs the 2D cache-resident
  height/cone fetches; the march is arithmetic-bound, so trading arithmetic for
  bigger, colder fetches points the wrong way [M-anchored]. (c) Memory is fine —
  2–8 MB/map [E] — memory was never the objection. (d) The known failure mode
  (fixed iteration cap stopping short of the surface → surface holes/warp at
  grazing) is the artifact class we already fought with cone convergence.
  Verdict: **the shell + existing cone/bisect (or naive-N, or S3 exact cones)
  achieves the user's stated goal cheaper and with machinery that is already
  measured**; revisit sphere tracing only if a future map has deep empty relief
  (grates, lattices) where distance fields genuinely leap.

---

## 6. Explicit non-recommendations

> **⚠️ SUPERSEDED IN PART, 2026-08-06 — read `docs/DISPLACEMENT_RESEARCH_II.md`
> §2.3 and §6 before acting on the prism bullet below.** The prism
> non-recommendation was made without reading Hirche '04 in full, and it is
> wrong on two counts. (1) It conflated "flat quads make the machinery cheap"
> with "flat quads make the machinery pointless" — Hirche ABANDONED the cheap
> single-pass prism renderer only because *"the assumption that the prism faces
> are flat is a very strong restriction that makes the algorithm in this form
> generally unusable"*, and our flat axis-charted quads satisfy that restriction
> exactly. (2) It missed the load-bearing mechanism: a prism's side faces are
> **rasterized geometry**, not merely an exit test, so a pixel one prism's ray
> escapes from is owned by the NEIGHBOUR prism's own fragment (entered through
> the shared side quad, arbitrated by Z). Our lid-only shell has no such
> fragment, and every hole, gash, smear and rust stripe the campaign has fought
> traces to that single missing thing. S1b is therefore **not** the prism
> family's flat special case — it is the prism family with the side faces
> deleted. Estimated cost of the full prism on our content: ~1 800 faces for the
> 226 shelled quads, ≈ 2 % of the tessellation carve's measured 86 600 [E].
> Also refuted by the same document: the assumption that shipped POM used
> offset limiting. It did not — Policarpo I3D '05, GPU Gems 3 ch. 18 and the
> DirectX SDK POM shader all travel the TRUE ray. Our deviation was running the
> true ray in a finite UV chart, which no source does.
>
> Still valid below: the VDM/GDM, max-mip, TFDM/RMIP, naive-256 and
> visibility-system bullets.

- **VDM / GDM** — §5.1. Memory scaling + gather shape + solves a non-bottleneck.
- ~~**Prism/tetrahedra rasterization** (Hirche '04, shell maps as-shipped) — the
  general-surface machinery; our flat quads reduce it to S1b at a fraction of the
  geometry and per-pixel cost.~~ **RETRACTED — see the note above.**
- **Max-mip / quadtree traversal as the march** — divergence-hostile to the
  8-wide row loop, and the engine's measured bottleneck is arithmetic, not tap
  count. Keep as the fallback bake if cone baking ever becomes the problem.
- **TFDM / RMIP adoption** — they accelerate arbitrary-ray × many-shells queries
  for ray tracers; the rasterizer's fragment already answers "which shell".
  Idea-mine only.
- **Full-res naive-256-class marches** for converged quality — measured
  +55 ms-class threaded [M]; the combined (cone/bracket+refine) methods exist
  precisely to avoid this.
- **A new visibility system to pay for path A** — measured dead end
  (VISIBILITY_PLAN §7) [M].

## 7. Sources

Survey spine
- L. Szirmay-Kalos, T. Umenhoffer, *Displacement Mapping on the GPU — State of
  the Art*, Computer Graphics Forum 27(6), 2008.
  http://cg.iit.bme.hu/~szirmay/egdisfinal3.pdf

Iterative / unsafe
- T. Kaneko et al., *Detailed Shape Representation with Parallax Mapping*, ICAT 2001.
- T. Welsh, *Parallax Mapping with Offset Limiting*, Infiscape TR, 2004.
- M. McGuire, M. McGuire, *Steep Parallax Mapping*, I3D 2005 poster.
- N. Tatarchuk, *Dynamic Parallax Occlusion Mapping with Approximate Soft
  Shadows*, I3D 2006. https://cgg.mff.cuni.cz/~pepca/lectures/pdf/Tatarchuk-ParallaxOcclusionMapping-Sketch-print.pdf
- F. Policarpo, M. M. Oliveira, J. Comba, *Real-Time Relief Mapping on Arbitrary
  Polygonal Surfaces*, I3D 2005.
  https://www.inf.ufrgs.br/~oliveira/pubs_files/Policarpo_Oliveira_Comba_RTRM_I3D_2005.pdf
- E. Risser, M. Shah, S. Pattanaik, *Interval Mapping*, UCF tech. sketch, 2006.

Safe / combined
- J. Dummer, *Cone Step Mapping: An Iterative Ray-Heightfield Intersection
  Algorithm*, self-published, 2006.
- F. Policarpo, M. M. Oliveira, *Relaxed Cone Stepping for Relief Mapping*,
  GPU Gems 3 ch. 18, 2007.
  https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-18-relaxed-cone-stepping-relief-mapping
- R. Bán, G. Valasek, C. Bálint, V. A. Vad, *Robust Cone Step Mapping*,
  EGSR 2024. https://diglib.eg.org/items/72110813-71ae-4cb3-b438-c9b0f7fc5b7f ;
  impl. https://github.com/Bundas102/robust-cone-map
- R. Bán, G. Valasek, *Quick Cone Map Generation on the GPU*, Eurographics 2022
  short.
- W. Donnelly, *Per-Pixel Displacement Mapping with Distance Functions*,
  GPU Gems 2 ch. 8, 2005.
  https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-8-pixel-displacement-mapping-distance-functions
- A. Tevs, I. Ihrke, H.-P. Seidel, *Maximum Mipmaps for Fast, Accurate, and
  Scalable Dynamic Height Field Rendering*, I3D 2008.
  https://dl.acm.org/doi/10.1145/1342250.1342279
- M. Drobot, *Quadtree Displacement Mapping with Height Blending*, GPU Pro, 2010.
  https://www.gamedevs.org/uploads/quadtree-displacement-mapping-with-height-blending.pdf

Silhouette / shell
- J. Hirche, A. Ehlert, S. Guthe, M. Doggett, *Hardware Accelerated Per-Pixel
  Displacement Mapping*, Graphics Interface 2004.
  http://download.hrz.tu-darmstadt.de/media/FB20/GCC/paper/Hirche-2004-GI.pdf
- S. Porumbescu, B. Budge, L. Feng, K. Joy, *Shell Maps*, SIGGRAPH 2005.
  https://dl.acm.org/doi/10.1145/1186822.1073239
- S. Jeschke, S. Mantler, M. Wimmer, *Interactive Smooth and Curved Shell
  Mapping*, EGSR 2007.
- F. Policarpo, M. M. Oliveira, *Relief Mapping of Non-Height-Field Surface
  Details*, I3D 2006.
  https://www.inf.ufrgs.br/~oliveira/pubs_files/Policarpo_Oliveira_RTM_multilayer_I3D2006.pdf
- C. Dachsbacher, N. Tatarchuk, *Prism Parallax Occlusion Mapping with Accurate
  Silhouette Generation*, 2007. https://inria.hal.science/inria-00606806/en

Precomputed exotic
- L. Wang, X. Wang, X. Tong, S. Lin, B. Guo, H.-Y. Shum, S.-M. Hu,
  *View-Dependent Displacement Mapping*, SIGGRAPH 2003.
  https://cg.cs.tsinghua.edu.cn/papers/sig2003.pdf
- X. Wang, X. Tong, S. Lin, S.-M. Hu, B. Guo, H.-Y. Shum, *Generalized
  Displacement Maps*, EGSR 2004.
  https://www.microsoft.com/en-us/research/wp-content/uploads/2016/12/Generalized_Displacement_Maps.pdf

RT-era
- T. Thonat, F. Beaune, X. Sun, N. Carr, T. Boubekeur, *Tessellation-Free
  Displacement Mapping for Ray Tracing*, SIGGRAPH Asia 2021.
  https://perso.telecom-paristech.fr/boubek/papers/TFDM/
- T. Thonat, I. Georgiev, F. Beaune, T. Boubekeur, *RMIP: Displacement Ray
  Tracing via Inversion and Oblong Bounding*, SIGGRAPH Asia 2023.
  https://perso.telecom-paristech.fr/boubek/papers/RMIP/
- NVIDIA, *Displaced Micro-Mesh* SDK/Toolkit, 2022–23.
  https://github.com/NVIDIAGameWorks/Displacement-MicroMap-Toolkit

Poly-side LOD
- T. Ulrich, *Rendering Massive Terrains Using Chunked Level of Detail Control*,
  SIGGRAPH 2002 course.
- F. Strugar, *Continuous Distance-Dependent Level of Detail for Rendering
  Heightmaps (CDLOD)*, JGGT 2009. https://aggrobird.com/files/cdlod_latest.pdf
- F. Losasso, H. Hoppe, *Geometry Clipmaps*, SIGGRAPH 2004.
- B. Karis, R. Stubbe, G. Wihlidal, *Nanite — A Deep Dive*, SIGGRAPH Advances
  2021. https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf

Self-shadow (referenced in §4)
- N. Max, *Horizon Mapping: Shadows for Bump-Mapped Surfaces*, The Visual
  Computer, 1988.
- P.-P. Sloan, M. Cohen, *Interactive Horizon Mapping*, EGWR 2000.

In-repo evidence
- docs/HEIGHTMAP_POM_PLAN.md (march economics, cone findings, relief-mapping
  negative result), docs/ENVDYN_DISPLACEMENT_PLAN.md (tessellation costs, shadow
  id fixes, border pinning, DisplaceTest rig), docs/VISIBILITY_PLAN.md (§7
  occlusion experiment), docs/GRAPHICS_PIPELINE.md (G-buffer/Z/tiling),
  docs/OPTIMIZATION_BACKLOG.md (S5 chunk LOD, SoA transform).
