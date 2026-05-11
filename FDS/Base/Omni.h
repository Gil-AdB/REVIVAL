#ifndef REVIVAL_OMNI_H
#define REVIVAL_OMNI_H

#include "Color.h"
#include "Vertex.h"
#include "Face.h"
#include "Spline.h"

#pragma pack(push, 1)

// Light source category. The Omni struct (named for historical reasons —
// the original 1998 engine only had point lights) holds both:
//   Light_Omni      — radiates equally in all directions. IDir, HotSpot,
//                     FallOff are ignored.
//   Light_SpotLight — cone-attenuated. IDir is the cone axis (unit
//                     vector); HotSpot is cos(inner angle), FallOff is
//                     cos(outer angle). Light is full inside the inner
//                     cone, smoothstep'd to zero between inner and
//                     outer, zero outside.
enum LightType : uint8_t {
	Light_Omni      = 0,
	Light_SpotLight = 1,
};

// [256 Bytes]
struct Omni
{
	Color            L;
	Vertex           V;
	Face             F;
	Spline           Pos;
	Spline					 Size;
	Spline					 Range;
	//  Spline           Col; // Not Supported - Light Color Track.
	Vector           IPos;
	DWord            Flags;
	float            FallOff;
	float            HotSpot;
	float						 ISize;
	float						 IRange,rRange;
	// Spot light parameters (only consulted when Type == Light_SpotLight).
	// IDir is the cone axis — unit vector pointing the direction the
	// light shines. HotSpot/FallOff are repurposed as cos(inner) /
	// cos(outer) cone half-angles, since the lighting kernel's only
	// operation on them is a dot-product comparison.
	Vector           IDir;
	LightType        Type;
	Omni           * Next;
	Omni           * Prev;
	dword			 dummy1,dummy2;
};

#pragma pack(pop)

#endif //REVIVAL_OMNI_H
