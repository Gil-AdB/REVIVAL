
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

	// 17.04.02 consider replacing PX, PY and TPos with x, y, and z.
	float			UZ = 0.0f, VZ = 0.0f, RZ = 0.0f; // U/Z, V/Z and 1/Z. (should be called UZ, VZ, RZ)
	Vector			Pos,TPos;   // Position and transformed position
	Vector			N;          // PseudoNormal
//	float			EU,EV;      // Environment mapping coordinates
//	float			REU,REV;    // EU/Z, EV/Z.
	float			EUZ = 0.0f, EVZ = 0.0f;
	float			U = 0.0f, V = 0.0f;        // Original mapping coordinates
	float			EU = 0.0f, EV = 0.0f;        // Original mapping coordinates
	DWord			Flags = 0;
	int i = -1;

//	dword			align16[3]; // this structure requires 16-byte alignment
	//  Word           Faces,FRem; // Faces = How many faces share that perticular
	// vertex, FRem = remaining visible faces.
	// when FRem reaches 0,the vertex will not be
	// transformed. (RULEZ)
};

#pragma pack(pop)

#endif //REVIVAL_VERTEX_H
