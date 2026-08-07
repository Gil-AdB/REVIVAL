
#ifndef REVIVAL_FACE_H
#define REVIVAL_FACE_H

#include "BaseDefs.h"
#include "Material.h"
#include "Vertex.h"
#include "FDS_DEFS.H"

namespace fds {
    struct RenderTarget;    // FDS/Base/RenderTarget.h
    struct CameraContext;   // FDS/Base/CameraContext.h
}

#pragma pack(push, 1)

struct Face;

// Per-tile rasterizer entry. Phase 4 of the re-entrant refactor passes
// the render target + camera context explicitly so the body doesn't need
// to read VPage / ZPage16 / XRes / YRes / g_zscale / etc. as file-scope
// globals. Migration is incremental: signatures land first; bodies
// migrate in follow-up commits and currently still read from globals.
typedef void (*RasterFunc)(Face* F, Vertex** V, dword numVerts, dword miplevel,
                           const fds::RenderTarget& rt,
                           const fds::CameraContext& cam);

// [4 Bytes]
union FDW
{
    float F;
    DWord DW = 0;
};

struct Face
{
	Vertex        * A = nullptr;
	// flares need only one vertex pointer, so B,C can be overwritten with flare-related information
	Vertex        * B = nullptr;
	union
	{
		Vertex        * C = nullptr;
		float			FlareSize;
	};

	dword           Flags = 0;
	float           NormProd = 0.0f;
	Vector          N;
	FDW             SortZ;

	float           U1  = 0.0f, V1  = 0.0f;
	float           U2  = 0.0f, V2  = 0.0f;
	float           U3  = 0.0f, V3  = 0.0f;
	float           EU1 = 0.0f, EV1 = 0.0f;
	float           EU2 = 0.0f, EV2 = 0.0f;
	float           EU3 = 0.0f, EV3 = 0.0f;

	RasterFunc		Filler = nullptr;
	Material      * Txtr = nullptr;
	Texture		  *	ReflectionTexture = nullptr;
	// Owning TriMesh — set per-frame in Transform_Objects when this Face
	// is added to FList. Used by the deferred transparent pass to walk
	// FList in TriMesh-grouped segments, so per-object depth-peeling can
	// composite each transparent object independently (handles nested
	// objects like the fountain's outer+inner spire spheres, which
	// otherwise lose their inner layer to the outer in the 2-deep
	// xpar G-buffer).
	struct TriMesh * ParentTri = nullptr;
	// Index of this face within ParentTri->Faces[]. Populated by
	// LightmapBake_Static at scene init (only for meshes with a
	// lightmap; left 0 for everything else, which is fine because
	// dynamic-mesh pixels skip the lightmap planes anyway). Survives
	// VertexScratch::cloneOf since clone is element-wise assign().
	// Necessary because the Mekalele dispatcher receives F from FList
	// (cloned), so `F - F->ParentTri->Faces` is invalid.
	uint16_t         MeshFaceIdx = 0;
	// Per-face 16-bit ShadowMatID override for the shadow rasterizer.
	// 0 = use `Txtr->ShadowMatID` if set, else `uint16_t(Txtr->ID + 1)`
	// (default — matID-based identity, what ShadowBarry historically
	// wrote). Non-zero = use this value directly. Lets scene-init code
	// merge faces into shadow groups beyond their material (greets
	// hull+hull2 share a polyId so the cube PolyId path doesn't fire
	// spurious "occluded" between them; greets walls split into
	// per-coplanar-cluster groups so distinct walls cross-shadow while
	// same-wall faces self-match — 16-bit space supports the ~2600
	// distinct clusters greets needs without inflating matTable past
	// the 8-bit matID cap).
	uint16_t         ShadowMatID = 0;
	// S1b SHELL POM (--pom_shell): 1-based index into
	// Material::PomShellDomains — the UV bounding box of the CONTIGUOUS
	// COPLANAR PATCH this face belongs to, not of the authored quad. The
	// march's lateral-exit discard tests against that box, so a ray crossing
	// into a sibling patch of the same wall (where the height field simply
	// continues) is NOT a silhouette, while a ray leaving the wall entirely
	// is. Measured on greets: authored wall quads are only ~0.4-2.1 UV tiles
	// wide, so per-quad domains discarded mid-wall everywhere. 0 = ungrouped
	// (the rasterizer falls back to this face's own U1..V3 box).
	uint16_t         PomShellGroup = 0;
	// S1d-5 PRISM-CLIPPED MARCH (--pom_prism_march): this face is a PRISM
	// SIDE QUAD (PomShell_BuildPrism wall geometry), not a lid face. Only
	// side-quad fragments arm the rasterizer's per-lane SIGNED 1/(V·N)
	// (they can be seen from behind the owner lid's plane, where the ray
	// ASCENDS through the slab); lid fragments keep the legacy descending
	// clamp — at grazing their interpolated V·N dips below 0 on real wall
	// pixels and the ascent semantics there discard half the wall
	// (measured: 200k void px over the review poses when applied to lids).
	// false everywhere unless BuildPrism stamped it, so shipping is inert.
	bool             PomPrismSide = false;
	// Planar-mirror identity (DEMO/GreetsMirror.cpp). Per Mirror, a unique
	// 1..255 id assigned at scene init. Two distinct roles depending on
	// the face's purpose in the mirror system:
	//   * Mirror SURFACE face (the wall): mask-only pre-pass stamps this
	//     value into GBuffer::mirrorId at the face's screen-space pixels.
	//   * Cloned reflected face: Mekalele's per-pixel inner loop tests
	//     gb.mirrorId[pixel] == F->mirrorMaskTag and skips the write
	//     when mismatched — so clone pixels can only land where their
	//     owning mirror's wall covered.
	// 0 = not a mirror face, no mask write or check.
	uint8_t          mirrorMaskTag = 0;
	// Last mip level MiplevelClipper chose for this face (0xFF = none yet).
	// Drives --mip-hysteresis: a face whose continuous mip metric sits near
	// a level boundary otherwise flips levels frame-to-frame — visible
	// texture-detail flicker, since the point-sampled fillers switch mips
	// hard. Written from tile workers without synchronization: a lost
	// update only weakens hysteresis for one frame, never correctness.
	// (Mirror-clone faces are re-cloned per frame, so their state resets —
	// their mip pops ride the water distortion anyway.)
	uint8_t          LastMip = 0xFF;
	// Per-face mirror identity, written into gb.mirrorId by Mekalele's
	// commit path for every rasterized pixel (z-correct because the
	// write only happens past p_mask, which already folds in zmask and
	// the existing 2D mask gate). Distinct from mirrorMaskTag:
	//   * mirrorMaskTag gates which pixels a CLONE face is allowed to
	//     commit to (the 2D pre-stamped mask is sloppy on z but only
	//     errs on the side of permitting too much; the per-face
	//     opaque z-test then rejects clones occluded by foreground).
	//   * ownerMirrorId tags the committed pixel so the deferred
	//     lighting kernel can filter per-pixel by mirror context.
	// Set on clone faces by BuildMirror (= mirror's m.id). Stays 0 on
	// originals so the deferred filter treats their committed pixels
	// as original-world (which is correct even where they overdraw a
	// previously pre-stamped 2D mask cell — that mask was only
	// authoritative for clone gating, not for surface ownership).
	uint8_t          ownerMirrorId = 0;
	// Bitmask of mirror ids this ORIGINAL face sits behind. Bit (id) is
	// set if any vertex of the face is on the back side of mirror `id`'s
	// plane (N·P + d < 0). The mirror surface is transparent (it doesn't
	// write opaque Z), so without this gate the real-world geometry
	// behind the mirror rasterises straight through the mirror's screen
	// footprint and beats the reflected clones on Z — the "room leaking
	// through the mirror" bug. Mekalele's commit rejects any pixel where
	// gb.mirrorMask[pixel] == M and bit M is set here, so behind-mirror
	// geometry is suppressed exactly inside that mirror's footprint and
	// the clones (or empty backdrop) win. Cheap, conservative tag: a
	// face straddling the plane is suppressed entirely inside the
	// footprint (its front part vanishes there too) — the full fix is
	// to clip the geometry against each plane at scene init. Covers
	// mirror ids 1..31; ids ≥32 are silently ignored (variable shift
	// returns 0). 0 = not behind any mirror (the common case).
	uint32_t         behindMirrorMask = 0;
	// SoA refactor Phase 3 (see docs/SOA_VERTEX_REFACTOR.md). Indices
	// of A/B/C into ParentTri->Verts[] (and equivalently into the
	// per-mesh VertexFrame SoA arrays). Populated by
	// Compute_FaceVertexIndices in Scene_Computations at scene init.
	// 0 for sprite/flare faces (where A == B and C is FlareSize,
	// not a vertex pointer — sprite rendering uses a different path
	// that doesn't read these indices).
	uint32_t         A_idx = 0;
	uint32_t         B_idx = 0;
	uint32_t         C_idx = 0;

	// SoA refactor Phase 4. The per-frame VertexFrame this Face's
	// transformed-vertex values live in. nullptr until Transform_Objects
	// sets it during FList build. For main pass: ParentTri->frame.
	// For shadow per-light scratch: clone.frame (so concurrent shadow
	// lights don't share storage). Consumers read
	// `F->frame->TPos_z[F->A_idx]` etc.
	struct VertexFrame *frame = nullptr;
	//	Surface       * Surf; // For T-Caching. (what??!)

	void uvFromVertices() {
		U1 = A->U;
		V1 = A->V;
		U2 = B->U;
		V2 = B->V;
		U3 = C->U;
		V3 = C->V;
	}

	void uvToVertices() {
		A->U = U1;
		A->V = V1;
		B->U = U2;
		B->V = V2;
		C->U = U3;
		C->V = V3;
	}

	DWord VisibilityFlagsAll() {
		auto flags = A->Flags & B->Flags & C->Flags;
		return flags & Vtx_Visible;
	}

	DWord VisibilityFlagsAny() {
		auto flags = A->Flags | B->Flags | C->Flags;
		return flags & Vtx_Visible;
	}

};

#pragma pack(pop)

#endif //REVIVAL_FACE_H
