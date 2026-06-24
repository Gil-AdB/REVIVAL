# AGENTS.md

Agent-facing guidance for the REVIVAL / FLOOD engine. Project setup, build,
and architecture live in **`CLAUDE.md`**; the rendering pipeline is detailed in
**`docs/ENGINE.md`**. This file collects the hard-won gotchas that have each
cost a debug session — read them before touching the relevant subsystem.

## Hand-built textures must be block-tiled ("shachletz")

The rasterizers (`TheOtherBarry`, `Mekalele`) **always** sample textures in a
block-tile **swizzled** layout (`packed_tile_u/v` + `swizzle_umask`,
`FDS/FILLERS/SimdHelpers.h`). A texture's pixel data must be stored in that
interleaved order or every fetch lands on the wrong texel.

`Convert_Image2Texture` does **not** produce that layout — it only resamples to
256×256 and converts BPP, leaving the data **linear / row-major**. The
block-tiling is a **separate** step, done by either of (they produce the same
4×4-block, X-outer/Y-inner layout):
- `Sachletz(data, w, h)` (`FDS/IMGGENR/IMGGENR.CPP`) — the standalone in-place
  swizzle, used across the codebase (disco, mirrors, RTTs, scene-builder,
  skybox, env-bake). This is the "shachletz" the layout is named after.
- `Generate_Mipmaps(Tx, DEFAULT_BLOCKSIZEX, DEFAULT_BLOCKSIZEY, enableMip)`
  (`FDS/IMGCODE/IMGCODE.CPP`) — swizzles **and** builds the mip chain; the
  disk-load path uses this. Set the `Txtr_Tiled` flag alongside it.

- **Symptom of skipping it:** the texture renders as evenly-spaced **repeated
  cells** (looks like 4× UV tiling). The UVs and mip level are correct — the
  *bytes* are in the wrong order. Don't chase it in the clipper or UV math.
- **Fix / the right way:** build any hand-baked texture through
  **`Scene_MakeTiledTexture(w, h, pixels, buildMips)`** (`DEMO/MeshOps.h`),
  which does `Convert_Image2Texture` → `Txtr_Tiled` → `Generate_Mipmaps` in one
  call.
- **Orientation:** once correctly tiled, UV→texel is the **standard**
  `U → texture-column`, `V → texture-row`. Code that reads UVs "swapped" (e.g.
  the fountain bolt's `UZ → texture-Y` comment) was compensating for the
  un-tiled bug and bakes its image transposed — do not copy that as a
  convention. The fountain lightning still carries this latent bug; its noisy
  pattern hides the corruption, so it was left as-is.

## Adding custom geometry to a scene

Use `Scene_AddDynamicMesh` (`DEMO/MeshOps.h`) — it handles the plumbing that
each cost a session when missed (per-face vertex indices, dynamic-marking Pos
keys, bounding sphere, flags, list links, poly budget). The header documents
the caller's preconditions, including the texture gotcha above.

## Read UVs from the FACE, not the vertex

`Get_UV` writes both per-vertex `Vertex::U/V` and per-face `Face::U1..V3`.
Shared vertices across differently-projected faces get the per-vertex value
clobbered by the last face mapped; the per-face `U1..V3` snapshot is always
correct. See `docs/ENGINE.md` → "Per-face vs per-vertex UVs".
