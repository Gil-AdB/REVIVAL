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
	// Shadow-map resolution hint, consumed by ShadowMaps_Rebuild. 0 =
	// use the global default passed to Rebuild. Set to e.g. 256 on
	// short-range lights to dramatically cut rasterization cost.
	dword            shadowMapRes;
	// 0 = original-world omni, lights any non-mirror pixel.
	// >0 = clone omni belonging to mirror with this id; lights only
	// pixels whose gb.mirrorId matches. Set by GreetsMirror's
	// BuildMirror when the omni is cloned across the plane; original
	// omnis stay at 0. The deferred lighting kernel reads this when
	// building per-tile light lists and skips omnis whose mirrorId
	// disagrees with the pixel's gb.mirrorId byte.
	dword            mirrorId;
	// Clone omnis only: the source omni + the mirror plane (world
	// space). Lets the deferred kernel shadow a clone light with ZERO
	// extra bakes — the clone's visibility of a point equals the
	// SOURCE's visibility of the point reflected across the plane, so
	// the kernel samples the source's existing shadow map at the
	// reflected position. Set by GreetsMirror's omni cloning.
	Omni            *mirrorSrcOmni;   // nullptr on originals
	Vector           mirrorPlaneN;
	float            mirrorPlaneD;
	// Mirror-bounce spots only (Omni_BounceCone): the source mirror's
	// window AABB (world space). The deferred surface kernel uses it for
	// a per-pixel portal test so a bounce only lights room surfaces
	// reachable through the window, not "through" the surrounding wall.
	Vector           mirrorWinMin;
	Vector           mirrorWinMax;
	// Per-omni halo controls (decouple halo brightness/extent from the
	// omni's surface-lighting parameters L*ISize and IRange). Render_-
	// OmniHalos and Render_DeferredVolumetric multiply the per-omni
	// halo density by HaloIntensity and use HaloRange in place of
	// IRange for the halo sphere bounds. Both default to 0 →
	// "use legacy behavior" (HaloIntensity=0 → treat as 1.0;
	// HaloRange=0 → fall back to IRange). Surface lighting kernel
	// still reads L*ISize and IRange unchanged.
	float            HaloIntensity;
	float            HaloRange;
	// Flare-sprite size multiplier, decoupling the flare from the light's
	// intensity (F.FlareSize = ISize × FlareScale — historically the flare
	// size WAS ISize, so brightening a light also grew its flare). Same
	// sentinel convention as the halo overrides: 0 → legacy 1.0, so the
	// many memset(0)-initialized Omni creation sites keep their look.
	float            FlareScale;
	// Per-light volumetric beam gain (spots with Omni_ForceVolCone):
	// multiplies the cone pass's global density for THIS light only.
	// Authored via LWS "VolumetricLightIntensity" → FLD bit-2048 payload
	// (FLD_CONV.CPP); same 0 → 1.0 sentinel convention as FlareScale /
	// HaloIntensity, so memset(0) creation sites (greets disco spots,
	// bounce pool) and GreetsMirror's memcpy clones keep their look.
	float            VolBeamGain;
};

#pragma pack(pop)

#endif //REVIVAL_OMNI_H
