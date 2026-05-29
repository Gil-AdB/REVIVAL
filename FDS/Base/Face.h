
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
