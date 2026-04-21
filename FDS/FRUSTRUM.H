#pragma once

#define CLIPPER_MAXVERTS 48

class FrustumClipper {
public:
	FrustumClipper() {
		Init();
	}

	void InitViewport(Scene* Sc);
	void Render(Face *F, RasterFunc filler, bool isEnvCoords);
	void SetClippingExtents(float x1, float y1, float x2, float y2) {
		C_VP.ClipX1 = x1;
		C_VP.ClipY1 = y1;
		C_VP.ClipX2 = x2;
		C_VP.ClipY2 = y2;
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
