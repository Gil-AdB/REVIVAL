#pragma once

#define CLIPPER_MAXVERTS 48

class FrustumClipper {
public:
	FrustumClipper() {
		Init();
	}

	void InitViewport(Scene* Sc);
	// skipMipLevel: bypass MiplevelClipper for textured faces. Used by
	// the shadow-map pass — depth-only rendering doesn't need mipmap
	// subdivision and MiplevelClipper's polygon traversal has bugs that
	// manifest under square-aspect (FOVX==FOVY) light cameras.
	void Render(Face *F, RasterFunc filler, bool isEnvCoords,
	            bool skipMipLevel = false);
	void SetClippingExtents(float x1, float y1, float x2, float y2) {
		C_VP.ClipX1 = x1;
		C_VP.ClipY1 = y1;
		C_VP.ClipX2 = x2;
		C_VP.ClipY2 = y2;
	}
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
	void MiplevelClipper(Face *F, RasterFunc filler);
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
void viewportCalcFlags(const Viewport& vp, Vertex* v);
void viewportCalcYFlags(const Viewport& vp, Vertex* v);
