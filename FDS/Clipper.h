#pragma once

#include "Base/Vertex.h"
#include "Base/FDS_DECS.H"
#include "FRUSTRUM.H"

class _2DClipper {
public:
	_2DClipper();
	void clip(const Viewport& vp, Face& f);
	static _2DClipper* getInstance();
private:
	void left();
	void right();
	void up();
	void down();
	void swap();

	void lerp(const Vertex& IA, const Vertex& IB, Vertex& V, float t) const;
	void ySort(Vertex** Prim, Vertex** Scnd, mword nVerts);
	void correctCWOrder();

	constexpr static const size_t MAXVERTS = 16;
	Viewport C_VP;
	mword C_numVerts;
	Vertex C_Verts[MAXVERTS];
	Vertex* C_Ptr[2][MAXVERTS];
	Vertex** C_Prim, ** C_Scnd;
	dword C_Flags;
};
