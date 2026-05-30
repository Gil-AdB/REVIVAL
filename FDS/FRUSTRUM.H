#pragma once

#include "Base/Face.h"     // RasterFunc, Face, Vertex, Viewport, dword, mword
struct Scene;

#define CLIPPER_MAXVERTS 48

namespace fds { struct CameraContext; struct RenderTarget; }

class FrustumClipper {
public:
	FrustumClipper() {
		Init();
	}
	// Resets thread_local fds::g_clipperTileRect to the "no clamp"
	// default. The rect is published by InitViewport / SetClippingExtents
	// once per worker (out of FrustumClipper::Render's per-triangle hot
	// loop — that was ~45 leaf samples/25s on greets) and stays valid for
	// the lifetime of this clipper; the dtor restores no-clamp so any
	// non-clipper rasterizer call later on the same thread (sky cube,
	// sprites, particles) doesn't inherit a stale rect.
	~FrustumClipper();

	// Pulls NearZ/FarZ from Sc->NZP/FZP and clip extents from XRes/YRes.
	// Also publishes the corresponding tile-rect to g_clipperTileRect.
	void InitViewport(Scene* Sc);
	// New form: pulls NearZ/FarZ from the per-pass CameraContext (so a
	// shadow pass can use its light's range without mutating Sc->FZP).
	// Caller still calls SetClippingExtents to override the X/Y rect
	// when rendering into a tile or a non-engine surface. Also publishes
	// the tile-rect.
	void InitViewport(const fds::CameraContext& cam);
	// skipMipLevel: bypass MiplevelClipper for textured faces. Used by
	// the shadow-map pass — depth-only rendering doesn't need mipmap
	// subdivision and MiplevelClipper's polygon traversal has bugs that
	// manifest under square-aspect (FOVX==FOVY) light cameras.
	void Render(Face *F, RasterFunc filler, bool isEnvCoords,
	            const fds::RenderTarget& rt,
	            const fds::CameraContext& cam,
	            bool skipMipLevel = false);
	// Sets per-worker clip extents and (re-)publishes g_clipperTileRect.
	// Non-inline so the header doesn't have to pull in ClipperTileRect.h
	// / <climits> — the call cost is negligible (~30 calls per frame).
	void SetClippingExtents(float x1, float y1, float x2, float y2);
	// Override the FarZ that InitViewport(Scene*) pulled from Sc->FZP.
	// Used by the shadow pass so each light can cap clipping at its own
	// range without mutating Sc->FZP (which would race across parallel
	// per-light workers sharing the same Scene).
	void SetFarZ(float fz) {
		C_VP.FarZ  = fz;
		C_VP.iFarZ = (fz > 0.0f) ? 1.0f / fz : 0.0f;
	}

private:
	void Init();
	// Publishes C_VP's clip rect into the thread_local g_clipperTileRect
	// so the rasterizers know which tiles this worker owns. Called from
	// SetClippingExtents + both InitViewport variants — once per worker,
	// not per Render.
	void publishClipperTileRect();
	void Calc_Flags(Vertex* V);
	void Calc_YFlags(Vertex* V);
	void Swap();
	void Near();
	void Far();
	void FInterpolator(Vertex* V, float t);
	void Left();
	void Right();
	void Up();
	void Down();
	void CorrectCWOrder();
	void YSort(Vertex** Prim, Vertex** Scnd, mword nVerts);
	void MiplevelClipper(Face *F, RasterFunc filler,
	                     const fds::RenderTarget& rt,
	                     const fds::CameraContext& cam);
	float PolyArea();

	dword g_MipLevel;

	Viewport C_VP;
	mword C_numVerts;
	Vertex C_Verts[CLIPPER_MAXVERTS];
	Vertex* newVert = nullptr;
	Vertex* C_Ptr[3][CLIPPER_MAXVERTS];
	Vertex** C_Prim, ** C_Scnd, ** C_Tetr;

	Vertex* _IA, * _IB;
	float _IVal;
	dword C_Flags;
};


void viewportInit(Viewport& vp, const Scene* Sc);
void viewportInit(Viewport& vp, const fds::CameraContext& cam);
void viewportCalcFlags(const Viewport& vp, Vertex* v);
void viewportCalcYFlags(const Viewport& vp, Vertex* v);
