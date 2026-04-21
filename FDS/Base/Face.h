
#ifndef REVIVAL_FACE_H
#define REVIVAL_FACE_H

#include "BaseDefs.h"
#include "Material.h"
#include "Vertex.h"
#include "FDS_DEFS.H"

#pragma pack(push, 1)

struct Face;

typedef void (*RasterFunc)(Face* F, Vertex** V, dword numVerts, dword miplevel);

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
