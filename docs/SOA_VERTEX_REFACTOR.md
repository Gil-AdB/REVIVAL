# SoA Vertex Refactor — Design

Branch: `feature/soa-vertex` (to be created off `feature/static-shadow-lightmaps`)

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
  at the tail of a long session. **Entry point: `Transform.cpp:317` (the `for (Vtx=Vert...)`
  loop) + lines 346 (Ahead near-clip) + the Regular branch.**

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
