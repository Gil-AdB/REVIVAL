
#ifndef REVIVAL_VERTEX_H
#define REVIVAL_VERTEX_H

#include <cstddef>

#include "BaseDefs.h"
#include "Vector.h"

#pragma pack(push, 1)

struct Viewport
{
	float ClipX1, ClipX2, ClipY1, ClipY2;
	float NearZ, FarZ;
	float iNearZ, iFarZ;
};

// FIELD ORDER IS LOAD-BEARING — see docs/SOA_VERTEX_REFACTOR.md.
// `Vertex` is pack(1) / 140 bytes and serves two roles: per-mesh storage
// (TriMesh::Verts, walked by Transform_Objects) and the clipper's transient
// working set (FrustumClipper::C_Verts, read by every filler). Both hot loops
// touch a SUBSET of the struct at a 140-byte stride, so what costs money is how
// many 64-byte lines that subset spans. The order below is grouped for that,
// front to back:
//
//   0..23   the PER-FACE loop's four fields — PX, PY, Flags, TPos_AOS.z.
//           Transform.cpp's face loop chases A/B/C for `Flags` (the visibility
//           test, ~73 % of its cost) and again for PX/PY/TPos_AOS.z (the
//           tile-bbox stamp); RenderInner.cpp's per-tile walk chases the same
//           three for `Flags` and the mirror-mask centroid. Contiguous in 24
//           bytes, those three random derefs touch ~1.56 lines each instead of
//           the ~2.88 they touched when Flags sat at offset 120 and PX at 4.
//   0..87   everything the PER-VERTEX transform loop touches: the four above
//           plus RZ, TN, TTangent (written) and Pos, N, Tangent (read).
//           88 bytes => ~2.375 lines/vertex instead of ~2.875.
//   88..139 the transform-untouched tail: BGRA, UZ/VZ, EUZ/EVZ, U/V, EU/EV, i,
//           OrigBary, ShellH. UZ/VZ joined this group when the transform's dead
//           UZ/VZ stores were deleted (fdc7a07) — the clipper is now their only
//           writer, which is what made this grouping possible.
//
// Before changing the order, check FRUSTRUM.CPP's FInterpolator: it lerps
// contiguous float runs with 2- and 4-wide SIMD and hard-codes which fields are
// adjacent (PX/PY, TN.x..TTangent.x, TTangent.y/z, UZ/VZ, EUZ/EVZ). Nothing
// else in the tree depends on the layout — audited for offsetof, positional
// aggregate init, float*/dword* walks over a Vertex, and serialization; every
// `sizeof(Vertex)` use is a whole-struct memset/memcpy.
struct  Vertex
{
	float			PX = 0.0f, PY = 0.0f;      // Projected X and Y
	DWord			Flags = 0;
	// 17.04.02 consider replacing PX, PY and TPos_AOS with x, y, and z.
	// SoA Phase 5 rename: TPos_AOS has moved to per-mesh / per-clone
	// VertexFrame SoA arrays (frame->TPos_x/_y/_z, indexed by
	// F->A_idx etc.). The AoS slot is kept for the clipper's
	// transient C_Verts working buffer + particle Faces that don't
	// have a frame; both will get cleaned up in Phase 6. Renamed to
	// TPos_AOS to surface every remaining reader at compile time
	// (per "rename-first" technique in docs/SOA_VERTEX_REFACTOR.md).
	Vector			TPos_AOS;
	float			RZ = 0.0f;                 // 1/Z
	Vector			TN;         // Transformed normal (view space; populated per frame
	                            // in Transform_Objects from N * IM, where IM is the
	                            // unscaled View*RotMat. Read by the deferred-path
	                            // clipper + rasterizer; the forward-path Lighting()
	                            // still reads N in object space.
	Vector			TTangent;   // Transformed tangent (view space; mirrors TN)
	Vector			Pos;        // Object-space position
	Vector			N;          // PseudoNormal (object space; computed at scene init)
	// Per-vertex tangent vector for tangent-space normal map support.
	// Computed in object space at scene init from triangle UV deltas
	// (Lengyel's method, then averaged + Gram-Schmidt'd against N).
	// Bitangent is reconstructed at lighting time as N × T.
	Vector			Tangent;    // Object-space tangent (unit; ⟂ N)

	// ---- transform-untouched tail (offset 88) ----------------------------
	// BGRA 128bit construct, fits a single XMM register
	//float			LB, LG, LR, LA; // Light intensity for R/G/B components.
	union
	{
		dword			BGRA = 0;		//
		struct
		{
			byte LB, LG, LR, LA;
		};
	};
	float			UZ = 0.0f, VZ = 0.0f;      // U/Z and V/Z. Written ONLY by the
	                                           // clipper (FrustumClipper::Render
	                                           // stamps them from F->U1..V3 * RZ)
	                                           // and by the hand-built sprite /
	                                           // water quads in DEMO.
//	float			EU,EV;      // Environment mapping coordinates
//	float			REU,REV;    // EU/Z, EV/Z.
	float			EUZ = 0.0f, EVZ = 0.0f;
	float			U = 0.0f, V = 0.0f;        // Original mapping coordinates
	float			EU = 0.0f, EV = 0.0f;        // Original mapping coordinates
	int i = -1;
	// Object-space barycentric weight of this vertex on its owning face's
	// (A, B, C) vertices. Stamped at scene init for static-mesh faces by
	// LightmapStampOrigBary: A->(0,0), B->(1,0), C->(0,1). The two clippers
	// (Clipper.cpp + FRUSTRUM.CPP::FInterpolator) interpolate these
	// perspective-correctly when generating clip vertices, so the values
	// remain valid object-space bary-on-F at every visible vertex of every
	// clipped sub-polygon. The rasterizer then interpolates them per-pixel
	// (same perspective-correct machinery as UV) to recover the lightmap
	// atlas address. Read-only outside of scene init + clipper.
	float			OrigBaryB = 0.0f, OrigBaryC = 0.0f;
	// S1b SHELL POM (--pom_shell, docs/S1_PIXEL_DISPLACEMENT_PLAN.md): this
	// vertex's height inside the relief SLAB, in units where 0 = the slab
	// floor (authored plane − amp/2), 0.5 = the authored plane, 1 = the LID
	// (authored plane + amp/2). PomShell_Build stamps 1.0 on the verts it
	// pushes out to the lid and leaves 0.5 on pinned/tapered ones; the two
	// clippers interpolate it perspective-correctly (same treatment as
	// OrigBary) and the rasterizer interpolates it per pixel to get the
	// march's ENTRY height. Only read for faces whose material carries a
	// PomShellUvAmp, so 0.5 (= "on the authored plane", no offset) is the safe
	// default for every other vertex in the engine.
	float			ShellH = 0.5f;
#ifdef FDS_VERTEX_PAD_BYTES
	// DIAGNOSTIC ONLY (-DFDS_VERTEX_PAD_BYTES=N). Dead tail padding that
	// inflates sizeof(Vertex) without changing a single instruction in any
	// loop, so per-vertex TIME can be measured as a function of per-vertex
	// BYTES. Never define this in a shipping build.
	char			_diagPad[FDS_VERTEX_PAD_BYTES];
#endif

//	dword			align16[3]; // this structure requires 16-byte alignment
	//  Word           Faces,FRem; // Faces = How many faces share that perticular
	// vertex, FRem = remaining visible faces.
	// when FRem reaches 0,the vertex will not be
	// transformed. (RULEZ)
};

#pragma pack(pop)

// Layout contract the two hot loops (and FInterpolator's SIMD runs) rely on.
#ifndef FDS_VERTEX_PAD_BYTES
static_assert(sizeof(Vertex) == 140, "Vertex must stay pack(1) 140 bytes");
#endif
static_assert(offsetof(Vertex, PX) == 0,   "face-loop block: PX first");
static_assert(offsetof(Vertex, PY) == 4,   "face-loop block: PY after PX (FInterpolator lerps the pair)");
static_assert(offsetof(Vertex, Flags) == 8, "face-loop block: Flags with PX/PY");
static_assert(offsetof(Vertex, TPos_AOS) == 12, "face-loop block: TPos_AOS.z at 20");
static_assert(offsetof(Vertex, RZ) == 24,  "vertex-loop block");
static_assert(offsetof(Vertex, TN) == 28,  "FInterpolator lerps TN.x..TTangent.x 4-wide");
static_assert(offsetof(Vertex, TTangent) == 40, "must follow TN contiguously");
static_assert(offsetof(Vertex, Pos) == 52, "vertex-loop block");
static_assert(offsetof(Vertex, N) == 64,   "vertex-loop block");
static_assert(offsetof(Vertex, Tangent) == 76, "vertex-loop block ends at 88");
static_assert(offsetof(Vertex, BGRA) == 88, "transform-untouched tail starts at 88");
static_assert(offsetof(Vertex, UZ) == 92,  "FInterpolator lerps UZ/VZ as a pair");
static_assert(offsetof(Vertex, VZ) == 96,  "FInterpolator lerps UZ/VZ as a pair");
static_assert(offsetof(Vertex, EUZ) == 100, "FInterpolator lerps EUZ/EVZ as a pair");
static_assert(offsetof(Vertex, EVZ) == 104, "FInterpolator lerps EUZ/EVZ as a pair");

#endif //REVIVAL_VERTEX_H
