
#ifndef REVIVAL_VERTEX_H
#define REVIVAL_VERTEX_H

#include "BaseDefs.h"
#include "Vector.h"

#pragma pack(push, 1)

struct Viewport
{
	float ClipX1, ClipX2, ClipY1, ClipY2;
	float NearZ, FarZ;
	float iNearZ, iFarZ;
};

// 72 bytes mod 16 = 8
// 4+20+12+12+12+8+4
struct  Vertex
{
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

	float			PX = 0.0f, PY = 0.0f;      // Projected X and Y

	// 17.04.02 consider replacing PX, PY and TPos_AOS with x, y, and z.
	float			UZ = 0.0f, VZ = 0.0f, RZ = 0.0f; // U/Z, V/Z and 1/Z. (should be called UZ, VZ, RZ)
	// SoA Phase 5 rename: TPos_AOS has moved to per-mesh / per-clone
	// VertexFrame SoA arrays (frame->TPos_x/_y/_z, indexed by
	// F->A_idx etc.). The AoS slot is kept for the clipper's
	// transient C_Verts working buffer + particle Faces that don't
	// have a frame; both will get cleaned up in Phase 6. Renamed to
	// TPos_AOS to surface every remaining reader at compile time
	// (per "rename-first" technique in docs/SOA_VERTEX_REFACTOR.md).
	Vector			Pos,TPos_AOS;
	Vector			N;          // PseudoNormal (object space; computed at scene init)
	Vector			TN;         // Transformed normal (view space; populated per frame
	                            // in Transform_Objects from N * IM, where IM is the
	                            // unscaled View*RotMat. Read by the deferred-path
	                            // clipper + rasterizer; the forward-path Lighting()
	                            // still reads N in object space.
	// Per-vertex tangent vector for tangent-space normal map support.
	// Computed in object space at scene init from triangle UV deltas
	// (Lengyel's method, then averaged + Gram-Schmidt'd against N).
	// Bitangent is reconstructed at lighting time as N × T.
	Vector			Tangent;    // Object-space tangent (unit; ⟂ N)
	Vector			TTangent;   // Transformed tangent (view space; mirrors TN)
//	float			EU,EV;      // Environment mapping coordinates
//	float			REU,REV;    // EU/Z, EV/Z.
	float			EUZ = 0.0f, EVZ = 0.0f;
	float			U = 0.0f, V = 0.0f;        // Original mapping coordinates
	float			EU = 0.0f, EV = 0.0f;        // Original mapping coordinates
	DWord			Flags = 0;
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

//	dword			align16[3]; // this structure requires 16-byte alignment
	//  Word           Faces,FRem; // Faces = How many faces share that perticular
	// vertex, FRem = remaining visible faces.
	// when FRem reaches 0,the vertex will not be
	// transformed. (RULEZ)
};

#pragma pack(pop)

#endif //REVIVAL_VERTEX_H
