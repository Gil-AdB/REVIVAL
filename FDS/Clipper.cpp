#include "Clipper.h"

// TODO: thread-local
static _2DClipper _2DClipperInst;

_2DClipper::_2DClipper() {
	C_Prim = C_Ptr[0];
	C_Scnd = C_Ptr[1];
}

void _2DClipper::clip(const Viewport& vp, Face& f) {
	C_VP = vp;
	C_numVerts = 3;
	C_Prim[0] = f.A;
	C_Prim[1] = f.B;
	C_Prim[2] = f.C;
	C_Prim[3] = f.A;

	C_Flags = C_Prim[0]->Flags | C_Prim[1]->Flags | C_Prim[2]->Flags;

	correctCWOrder();

	if (C_Flags & Vtx_VisLeft)  left();
	if (C_Flags & Vtx_VisRight) right();
	if (C_Flags & Vtx_VisUp)    up();
	if (C_Flags & Vtx_VisDown)  down();

	if (!C_numVerts) return;

	ySort(C_Prim, C_Scnd, C_numVerts);

	//DoFace = &f;
	f.Filler(&f, C_Prim, C_numVerts, 0);
}
void _2DClipper::left() {
	dword i, j = 0;

	Vertex* newVert = C_Verts;
	Vertex* IA;
	Vertex* IB = C_Prim[0];
	for (i = 0; i < C_numVerts; i++)
	{
		IA = IB;
		IB = C_Prim[i + 1];
		if (IA->Flags & Vtx_VisLeft)
		{
			if (IB->Flags & Vtx_VisLeft) continue;
		} else {
			C_Scnd[j++] = IA;
			if (!(IB->Flags & Vtx_VisLeft)) continue;
		}
		lerp(*IA, *IB, *newVert, (C_VP.ClipX1 - IA->PX) / (IB->PX - IA->PX));
		newVert->PX = C_VP.ClipX1;
		viewportCalcYFlags(C_VP, newVert);
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	swap();
}

void _2DClipper::right() {
	dword i, j = 0;

	Vertex* newVert = C_Verts + 2;
	Vertex* IA;
	Vertex* IB = C_Prim[0];
	for (i = 0; i < C_numVerts; i++)
	{
		IA = IB;
		IB = C_Prim[i + 1];
		if (IA->Flags & Vtx_VisRight)
		{
			if (IB->Flags & Vtx_VisRight) continue;
		} else {
			C_Scnd[j++] = IA;
			if (!(IB->Flags & Vtx_VisRight)) continue;
		}
		lerp(*IA, *IB, *newVert, (C_VP.ClipX2 - IA->PX) / (IB->PX - IA->PX));
		newVert->PX = C_VP.ClipX2;
		viewportCalcYFlags(C_VP, newVert);
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	swap();
}

void _2DClipper::up() {
	dword i, j = 0;

	Vertex* newVert = C_Verts + 4;
	Vertex* IA;
	Vertex* IB = C_Prim[0];
	for (i = 0; i < C_numVerts; i++)
	{
		IA = IB;
		IB = C_Prim[i + 1];
		if (IA->Flags & Vtx_VisUp)
		{
			if (IB->Flags & Vtx_VisUp) continue;
		} else {
			C_Scnd[j++] = IA;
			if (!(IB->Flags & Vtx_VisUp)) continue;
		}
		lerp(*IA, *IB, *newVert, (C_VP.ClipY1 - IA->PY) / (IB->PY - IA->PY));
		newVert->PY = C_VP.ClipY1;
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	swap();
}

void _2DClipper::down() {
	dword i, j = 0;

	Vertex* newVert = C_Verts + 6;
	Vertex* IA;
	Vertex* IB = C_Prim[0];
	for (i = 0; i < C_numVerts; i++)
	{
		IA = IB;
		IB = C_Prim[i + 1];
		if (IA->Flags & Vtx_VisDown)
		{
			if (IB->Flags & Vtx_VisDown) continue;
		} else {
			C_Scnd[j++] = IA;
			if (!(IB->Flags & Vtx_VisDown)) continue;
		}
		lerp(*IA, *IB, *newVert, (C_VP.ClipY2 - IA->PY) / (IB->PY - IA->PY));
		newVert->PY = C_VP.ClipY2;
		C_Scnd[j++] = newVert++;
	}

	C_numVerts = j;
	swap();
}


void _2DClipper::swap()
{
	C_Scnd[C_numVerts] = C_Scnd[0];

	Vertex** Swap = C_Prim;
	C_Prim = C_Scnd;
	C_Scnd = Swap;
}


void _2DClipper::lerp(const Vertex& IA, const Vertex& IB, Vertex& V, float t) const {
	V.PX = IA.PX + t * (IB.PX - IA.PX);
	V.PY = IA.PY + t * (IB.PY - IA.PY);
	V.LA = IA.LA + t * (IB.LA - IA.LA);
	V.LR = IA.LR + t * (IB.LR - IA.LR);
	V.LG = IA.LG + t * (IB.LG - IA.LG);
	V.LB = IA.LB + t * (IB.LB - IA.LB);
	V.RZ = IA.RZ + t * (IB.RZ - IA.RZ);
	V.UZ = IA.UZ + t * (IB.UZ - IA.UZ);
	V.VZ = IA.VZ + t * (IB.VZ - IA.VZ);
}

void _2DClipper::ySort(Vertex** Prim, Vertex** Scnd, mword nVerts)
{
	// Find vertex with minimal y-value.
	mword i, j;
	float minY = Prim[0]->PY;
	mword minYIndex = 0;
	for (i = 1; i < nVerts; i++)
	{
		float y = Prim[i]->PY;
		if (y < minY)
		{
			minYIndex = i;
			minY = y;
		}
	}

	// rotate vertices so that minimal y vertex appears first
	for (i = minYIndex; i < nVerts; i++)
	{
		Scnd[i - minYIndex] = Prim[i];
	}

	j = nVerts - minYIndex;
	for (i = 0; i < minYIndex; i++)
	{
		Scnd[i + j] = Prim[i];
	}
	swap();
}


_2DClipper* _2DClipper::getInstance() {
	return &_2DClipperInst;
}

void _2DClipper::correctCWOrder() {
	Vertex *A = C_Prim[0], *B = C_Prim[1], *C = C_Prim[2];
	
	float area = (B->PX - A->PX) * (C->PY - A->PY) - (C->PX - A->PX) * (B->PY - A->PY);

	if (area > 0.0)
	{
		for(mword i=0; i<C_numVerts; i++)
		{
			C_Scnd[i] = C_Prim[C_numVerts-i];
		}    
		swap();
	}
}