/*
	antialiasing mesh raytracer, intended to support glow effect and transparency.
	[10.09.02] Version 0.01: basic version operative
*/

#include "Rev.h"

static void rayFaceIsect(rtIntersection &isect, rtLocalRay &ray, Face *F)
{
	float dot = ray.direction * F->N;
	if (fabs(dot)<1E-09) return;

	float t = ((F->A->Pos - ray.position)*F->N) / dot;

	if (t < 1E-06 || isect.t < t)
		return;

	Vector p = ray.position + t * ray.direction - F->A->Pos;
	
	Vector u, v;
	u = F->B->Pos - F->A->Pos;
	v = F->C->Pos - F->A->Pos;

	float det, a, b;

	det = u.x * v.y - u.y * v.x;
	if (fabs(det) < 1E-12)
	{
		det = 1.0 / (u.x * v.z - u.z * v.z);

		a = (p.x * v.z - p.z * v.x) * det;
		b = (u.x * p.z - u.z * p.x) * det;
	} else {
		det = 1.0 / det;

		a = (p.x * v.y - p.y * v.x) * det;
		b = (u.x * p.y - u.y * p.x) * det;
	}

	if (a < 0 || b < 0 || a+b > 1) return;

	isect.t = t;
	isect.u = a;
	isect.v = b;
	isect.F = F;	
}

static Color raytrace(Scene *Sc, Vector *position, Vector *direction)
{
	TriMesh *T;
	Face *F, *FE;
	Color ref, col;
	rtIntersection isect;
	Vector u;

	isect.F = NULL;
	isect.T = NULL;
	isect.t = 1E+38; // should calculate t based on far-Z clipping plane? maybe not.
	isect.u = 0;
	isect.v = 0;

	for(T=Sc->TriMeshHead; T; T=T->Next)
	{		
		if (!(T->Flags&HTrack_Visible)) continue;

		rtLocalRay ray;
		// xform (position, direction) to object space.
		u.sub(*position, T->IPos);
		float l2 = 1.0/Vector_SelfDot((Vector *)T->RotMat);
		
		MatrixTXVector(T->RotMat, &u, &ray.position);
		ray.position *= l2;

		MatrixTXVector(T->RotMat, direction, &ray.direction);
		ray.direction *= l2;

		for(F = T->Faces, FE = F + T->FIndex; F<FE; F++)
		{
			rayFaceIsect(isect, ray, F);
		}
	}

	if (isect.F == NULL)
	{
		col.R = 0.0;
		col.G = 0.0;
		col.B = 0.0;
		col.A = 0.0;
		return col;
	}

	// calculate color (based on gouraud, and a trilinear texture sampler)	
	col.R = isect.F->A->LR
		+ (isect.F->B->LR - isect.F->A->LR) * isect.u
		+ (isect.F->C->LR - isect.F->A->LR) * isect.v;
			
	col.G = isect.F->A->LG
		+ (isect.F->B->LG - isect.F->A->LG) * isect.u
		+ (isect.F->C->LG - isect.F->A->LG) * isect.v;

	col.B = isect.F->A->LB
		+ (isect.F->B->LB - isect.F->A->LB) * isect.u
		+ (isect.F->C->LB - isect.F->A->LB) * isect.v;

	// calculate lighting
	col.R *= 1.0;
	col.G *= 1.0;
	col.B *= 1.0;
	col.A = 255.0f;

	// check if intersected mesh is transparent. if so, refract the ray (use raytracer again)
	if (isect.F->Txtr->Flags & Mat_Transparent)
	{
		//ref = raytrace(Sc, ..., ...);
		//interpolate between col and ref
	} 

	// atmospheric effects: col decays based on isect.t
	
	return col;
}

// stochastic raytracer that renders polygonal surfaces. Approximates volumetric glow.
// Research log: Try to approximate integrals directly or use the alpha-hull spectrum in
// conjunction with a rasterizing depth tracer.
void GlowRaytracer(Scene *Sc, Camera *Viewer)
{
	Vector tdir, dir;
	mword i, j, k;
	float x, y, z = 1.0f;
	float PX=FOVX,PY=FOVY;
	
	float dx = 1.0/PX;
	float dy =-1.0/PY;

	mword gridSamples = 2;
	mword stochSamples = gridSamples * gridSamples;

	byte *page = (byte *)VPage;
	
	mword tthr = Timer;
	
	for(j=0, y=-CntrEY*dy; j<YRes; j++, y+=dy)
	{
		dword *scanline = (dword *)(page + VESA_BPSL*j);
		for(i=0, x=-CntrEX*dx; i<XRes; i++, x+=dx)
		{
			Color colorAccum;
			colorAccum.R = 0.0;
			colorAccum.G = 0.0;
			colorAccum.B = 0.0;
			for(k=0; k<stochSamples; k++)
			{				
//				tdir.x = x + RAND_15()*dx/32768.0;
//				tdir.y = y + RAND_15()*dy/32768.0;
				tdir.x = x + (float)(k%gridSamples)*dx/(gridSamples);
				tdir.y = y + (float)(k/gridSamples)*dy/(gridSamples);
				tdir.z = 1.0;
				Vector_Norm(&tdir);

				MatrixTXVector(Viewer->Mat, &tdir, &dir);
				Color col = raytrace(Sc, &Viewer->ISource, &dir);
				colorAccum.R += col.R;
				colorAccum.G += col.G;
				colorAccum.B += col.B;
			}

			float stochScale = 1.0/(float)stochSamples;
			colorAccum.R *= stochScale;
			colorAccum.G *= stochScale;
			colorAccum.B *= stochScale;

			dword r = colorAccum.R; if (r>255.0) r = 255.0;
			dword g = colorAccum.G; if (g>255.0) g = 255.0;
			dword b = colorAccum.B; if (b>255.0) b = 255.0;
			scanline[i] = (r<<16)+(g<<8)+b;
		}
		if (Timer > tthr + 50)
		{
			tthr = Timer;
			Flip(MainSurf);
		}
	}	
}