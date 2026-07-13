#include "Rev.h"
#include "MaterialImport.h"
#include <Threads.h>
#include "FrameProfiler.h"
#include <Base/Object.h>
#include <Base/TriMesh.h>
#include <Base/Scene.h>

#include "CHASE.H"
#include "ProceduralWater.h"
#include "SceneTick.h"
#include "Scenes.h"
#include "BlasterBolts.h"      // B1: blaster combat (chase_blasters)
#include "ChaseEvents.h"       // B1: pure-t fire table + reconstruction
#include <Base/VertexFrame.h>  // Reflected_Transform dual-write to T->frame
#include <memory>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

#define FRONT_TO_BACK_SORTING
static Scene *ChaseSc;
// L2.4 sky: chase-local sky cube (the shared starfield/nebula skybox city and
// fountain use, via CreateSkyCube/RenderSkyCube). Replaces the L1 ad-hoc
// gradient + the black void — a lit sky above the horizon that the transparent
// water also reflects. Chase-owned (not city's global SkySc) so the chase
// snapshot, which never inits city, still has one.
static Scene *ChaseSkySc = nullptr;

namespace {
// M1.3 engine-glow throttle (default-off chase_engine_mod). The maneuver signal
// is a pure function of CurFrame — the ship rotation-spline angular rate (deg
// per frame, central difference; FLD meshes are always Tri_Euler so Rotate is
// Euler H/P/B). No accumulation → jump-to-t reproduces it exactly (snapshot-safe).
constexpr float kEngineModD = 2.0f;   // finite-difference half-window (frames)

float chaseAngRate(TriMesh *T, float frame, float D)
{
	if (!T || T->Rotate.NumKeys < 2) return 0.0f;
	Vector a, b;
	Spline_Calc_3D(&T->Rotate, frame - D, &a);
	Spline_Calc_3D(&T->Rotate, frame + D, &b);
	float dh = b.x - a.x, dp = b.y - a.y, db = b.z - a.z;
	return std::sqrt(dh * dh + dp * dp + db * db) / (2.0f * D);
}

// P90 of |angular rate| over the frame range — a robust per-ship peak that
// normalises the punch (deterministic: positions/rotations are immutable).
float chaseAngRateRef(TriMesh *T, int32_t f0, int32_t f1, float D)
{
	if (!T) return 1.0f;
	std::vector<float> v;
	for (int32_t f = f0; f <= f1; f += 2) v.push_back(chaseAngRate(T, (float)f, D));
	if (v.empty()) return 1.0f;
	std::sort(v.begin(), v.end());
	float r = v[(size_t)(v.size() * 0.90)];
	return r > 1e-4f ? r : 1e-4f;
}

// ── B1 blaster combat (chase_blasters) ──────────────────────────────────────
// World transform of a ship mesh at an ARBITRARY frame (not necessarily the
// current one): reconstruct IPos + the scaled RotMat straight from the
// Pos/Rotate/Scale splines exactly as Animate_Objects does (FLD ship meshes are
// Tri_Euler). Writes into locals only — never T->IPos/T->RotMat, which the live
// render pass owns. This is what makes the fire table a pure function of t.
void chaseShipXform(TriMesh *T, float frame, Vector &outPos, Matrix outRot)
{
	Vector u, s;
	Spline_Calc_3D(&T->Pos, frame, &outPos);
	Spline_Calc_3D(&T->Scale, frame, &s);
	if (T->EditorScale > 0.0f) Vector_SelfScale(&s, T->EditorScale);
	Spline_Calc_3D(&T->Rotate, frame, &u);
	Euler_Angles(outRot, u.x, u.y, u.z);
	Vector *W = (Vector *)outRot;
	Vector_SelfScale(W, s.x); ++W;
	Vector_SelfScale(W, s.y); ++W;
	Vector_SelfScale(W, s.z);
}

// World centre of a ship at `frame` (bounding-sphere centre through the scaled
// rotation + position) — the aim anchor for the target ship.
Vector chaseShipCenter(TriMesh *T, float frame)
{
	Vector pos; Matrix rot;
	chaseShipXform(T, frame, pos, rot);
	Vector off;
	MatrixXVector(rot, &T->BSphereCtr, &off);
	return Vector{ pos.x + off.x, pos.y + off.y, pos.z + off.z };
}

// ship2 wing-muzzle world position at `frame`. Local offsets are fractions of
// the ship's model bounding radius about its bounding centre (scale-robust); the
// two wings straddle local X. INTERIM hardcode — the FdsMuzzle LWS keyword
// (parent-local muzzle coords in the FLD object record, flag-bit + payload) is
// the later refinement noted in the plan.
Vector chaseMuzzleWorld(TriMesh *T, float frame, int wing)
{
	Vector pos; Matrix rot;
	chaseShipXform(T, frame, pos, rot);
	const float R = T->BSphereRadius;
	Vector local = T->BSphereCtr;
	local.x += (wing ? +0.55f : -0.55f) * R;   // right / left wing
	local.y += 0.10f * R;                        // just above the hull
	local.z += 0.35f * R;                        // toward the nose
	Vector off;
	MatrixXVector(rot, &local, &off);
	return Vector{ pos.x + off.x, pos.y + off.y, pos.z + off.z };
}
} // namespace

static int32_t CHPartTime = 25.0f*100;

static Object *RflObj;
static Vector RflSurfNorm(0, 1, 0);
static float RflSurfOfs = 0;

// World-space extent + plane height of chase's water (water.lwo), captured in
// Reflective_Surface_Setup and fed to the shared procedural-water pass (same
// role as city's g_waterMin/Max/Y).
static float g_chaseWaterMinX = 0, g_chaseWaterMaxX = 0,
             g_chaseWaterMinZ = 0, g_chaseWaterMaxZ = 0, g_chaseWaterY = 0;

static DWord WaterBuf[65536 * 4];

static void Setup_Water_Distort()
{
	TriMesh *T = (TriMesh *)RflObj->Data;
	memcpy(WaterBuf, (DWord *)(T->Faces->Txtr->Txtr->Data), 65536 * 4);//WaterTexture.Data;
}

static void Reflective_Mapper_Setup()
{
	TriMesh *T;
	Vertex *Vtx, *VE;
	Face *F, *FE;
	Vector U, V;

	Object *O;
	int32_t IID = 0;
	for (O = ChaseSc->ObjectHead; O; O = O->Next)
	{
		if (O->Type != Obj_TriMesh) continue;
		//		printf("[%d] Trimesh %s with %d Polygons\n",IID,O->Name,((TriMesh *)O->Data)->FIndex);
		if (!strcmp(O->Name, "water.lwo"))
		{
			RflObj = O;
			T = (TriMesh *)(O->Data);

			F = T->Faces;
			FE = F + T->FIndex;

			// should write a transparent texture/gouraud to enable fog.
//			break;
			for (; F < FE; F++)
			{
				//				F->Filler = IX_Prefiller_TGAcZ;
				//				F->Txtr->Luminosity = 8.0;
								//F->Filler = The_BiMMXTTrue_Fogged;
				//				F->Clipper = &Frust_Set_UVL;
			}
			break;
		}
	}
}


///////////////////////////////////////////////////////////////////////
// PRIMARY TRANSFORMATION PIPELINE, FOR USE WITH REFLECTIONS
static void Reflected_Transform(Scene *Sc)
{
	TriMesh *T;
	Omni *O;
	Matrix M, IM, ReflMat;
	float M34[3][4];
	Vector AP, S, P, U, V, *W = (Vector *)M, *W2 = (Vector *)IM, *Scl;
	float L1, L2, L3;
	Vertex *Vtx, *VEnd;
	Face *F, *FEnd;
	float PX = FOVX, PY = FOVY, Temp;
	float dz;
	int32_t *pdz = (int32_t *)(&dz);
	int32_t I;
	fds::FListEntry *Ins = FList;
	float *f = (float *)(&M);
	float *fv;

	float fzp = CurScene->FZP;

	TriMesh *TR;

	TR = (TriMesh *)(RflObj->Data);

	//for (T=Sc->TriMeshHead;T;T=T->Next)
	Object *Obj;
	for (Obj = Sc->ObjectHead; Obj; Obj = Obj->Next)
	{

		if (Obj->Type != Obj_TriMesh) continue;
		//if (stricmp(Obj->Name, "Ship1.lwo")) continue;
		T = (TriMesh *)(Obj->Data);

		if (T == TR) { T->Flags |= Tri_Invisible; continue; }

		if (!(T->Flags&HTrack_Visible)) { T->Flags |= Tri_Invisible; continue; }

		Matrix_Copy(ReflMat, T->RotMat);
		//Vector_SelfScale(W2+1,-1.0f);
		ReflMat[0][1] *= -1.0f; ReflMat[1][1] *= -1.0f; ReflMat[2][1] *= -1.0f;
		MatrixXMatrix(View->Mat, ReflMat, M);
		Matrix_Copy(IM, M);
		// Advanced Matrix...(watch this)
		Vector_Scale(W, PX, W);
		Vector_Scale(W + 1, -PY, W + 1);
		Vector_Scale(W + 2, CntrEX, &V);
		Vector_SelfAdd(W, &V);
		Vector_Scale(W + 2, CntrEY, &V);
		Vector_SelfAdd(W + 1, &V);
		// Supermatrix ready.

		// postrioric Offset Vector.
		// mirror T->ipos around first
		L1 = (-2.0f)*Dot_Product(&T->IPos, &RflSurfNorm);
		Vector_Scale(&RflSurfNorm, L1, &P);
		Vector_SelfAdd(&P, &T->IPos);

		Vector_Sub(&P, &View->ISource, &U);
		MatrixXVector(View->Mat, &U, &S);
		V.x = CntrEX * S.z + PX * S.x;
		V.y = CntrEY * S.z - PY * S.y;
		V.z = S.z;
		// make a corrected sphere center vector
		MatrixXVector(IM, &T->BSphereCtr, &AP);
		Vector_SelfAdd(&S, &AP);

		//    Vector_Copy(&V,&S);
		// ready
		// 4x3 AFFINE XFORM
		M34[0][0] = M[0][0]; M34[0][1] = M[0][1]; M34[0][2] = M[0][2]; M34[0][3] = V.x;
		M34[1][0] = M[1][0]; M34[1][1] = M[1][1]; M34[1][2] = M[1][2]; M34[1][3] = V.y;
		M34[2][0] = M[2][0]; M34[2][1] = M[2][1]; M34[2][2] = M[2][2]; M34[2][3] = V.z;
		// ready


		// aprioric Offset Vector.
		MatrixTXVector(ReflMat, &U, &AP);
		Vector *WP = (Vector *)ReflMat;
		Vector_SelfScale(&AP, 1.0 / Vector_SelfDot(WP));
		// ready
		// Bounding Sphere Elimination test Begins.
		W2 = (Vector *)(&T->RotMat);
		L2 = Dot_Product(W2, W2);
		if ((L1 = Dot_Product(W2 + 1, W2 + 1)) > L2) L2 = L1;
		if ((L1 = Dot_Product(W2 + 2, W2 + 2)) > L2) L2 = L1;

		T->Flags &= 0xFFFFFFFF - Tri_Invisible - Tri_Ahead - Tri_Inside;
		T->Flags |= Tri_Inside;

		// Out by depth
		dz = S.z - Sc->NZP;
		if (dz*dz > L2*T->BSphereRad)
		{
			if (dz < 0.0f)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
			T->Flags |= Tri_Ahead;
		}
		else {
			T->Flags &= ~Tri_Inside;
		}

		dz = S.z - Sc->FZP;
		if (dz*dz > L2*T->BSphereRad)
		{
			if (dz > 0.0f)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		}
		else {
			T->Flags &= ~Tri_Inside;
		}
		// Out by left/right
		S.x = fabs(S.x);
		L1 = PX * S.x - CntrEX * S.z;
		if (L1*L1 > L2*T->BSphereRad*(PX*PX + CntrEX * CntrEX))
		{
			if (S.x*PX > S.z*CntrEX)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		}
		else {
			if (T->Flags&Tri_Ahead) T->Flags &= ~Tri_Inside;
		}
		// Out by up/down
		S.y = fabs(S.y);
		L1 = PY * S.y - CntrEY * S.z;
		if (L1*L1 > L2*T->BSphereRad*(PY*PY + CntrEY * CntrEY))
		{
			if (S.y*PY > S.z*CntrEY)
			{
				T->Flags |= Tri_Invisible;
				continue;
			}
		}
		else {
			if (T->Flags&Tri_Ahead) T->Flags &= ~Tri_Inside;
		}

		VEnd = T->Verts + T->VIndex;

		/*    FEnd=T->Face+T->NumOfFaces;
		for (F=T->Face;F<FEnd;F++)
		if (!(F->Txtr->Flags&Mat_TwoSided))
		F->Flags = (AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z>=F->NormProd);*/


		//    Main vertex loop,in case no restrictions apply.
		if (!(T->Flags&Tri_Phong))
		{
			if (!(T->Flags&Tri_Inside))
			{
				if (!(T->Flags&Tri_Ahead))
					goto Regular;
				else goto Ahead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos_AOS);
				// 4x3 xform
				Vtx->TPos_AOS.x = M34[0][0] * Vtx->Pos.x + M34[0][1] * Vtx->Pos.y + M34[0][2] * Vtx->Pos.z + M34[0][3];
				Vtx->TPos_AOS.y = M34[1][0] * Vtx->Pos.x + M34[1][1] * Vtx->Pos.y + M34[1][2] * Vtx->Pos.z + M34[1][3];
				Vtx->TPos_AOS.z = M34[2][0] * Vtx->Pos.x + M34[2][1] * Vtx->Pos.y + M34[2][2] * Vtx->Pos.z + M34[2][3];

				Vtx->Flags &= 0xFFFFFFFF - Vtx_Visible;
				Vtx->RZ = 1.0 / Vtx->TPos_AOS.z;
				Vtx->PX = Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY = Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				//if (Vtx->TPos_AOS.z>Sc->FZP) Vtx->Flags|=Vtx_VisFar;
			}

			goto OUT;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
		Ahead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos_AOS);
				Vtx->TPos_AOS.x = M34[0][0] * Vtx->Pos.x + M34[0][1] * Vtx->Pos.y + M34[0][2] * Vtx->Pos.z + M34[0][3];
				Vtx->TPos_AOS.y = M34[1][0] * Vtx->Pos.x + M34[1][1] * Vtx->Pos.y + M34[1][2] * Vtx->Pos.z + M34[1][3];
				Vtx->TPos_AOS.z = M34[2][0] * Vtx->Pos.x + M34[2][1] * Vtx->Pos.y + M34[2][2] * Vtx->Pos.z + M34[2][3];

				Vtx->RZ = 1.0 / Vtx->TPos_AOS.z;
				Vtx->PX = Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY = Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				Vtx->Flags &= 0xFFFFFFFF - Vtx_Visible;
				if (Vtx->PX < 0) Vtx->Flags |= Vtx_VisLeft;
				if (Vtx->PX >= XRes) Vtx->Flags |= Vtx_VisRight;
				if (Vtx->PY < 0) Vtx->Flags |= Vtx_VisUp;
				if (Vtx->PY >= YRes) Vtx->Flags |= Vtx_VisDown;
				if (Vtx->TPos_AOS.z > Sc->FZP) Vtx->Flags |= Vtx_VisFar;
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto OUT;
		Regular:
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				//        MatrixXVector(M,&Vtx->Pos,&U);
				//        Vector_Add(&U,&V,&Vtx->TPos_AOS);
				Vtx->TPos_AOS.x = M34[0][0] * Vtx->Pos.x + M34[0][1] * Vtx->Pos.y + M34[0][2] * Vtx->Pos.z + M34[0][3];
				Vtx->TPos_AOS.y = M34[1][0] * Vtx->Pos.x + M34[1][1] * Vtx->Pos.y + M34[1][2] * Vtx->Pos.z + M34[1][3];
				Vtx->TPos_AOS.z = M34[2][0] * Vtx->Pos.x + M34[2][1] * Vtx->Pos.y + M34[2][2] * Vtx->Pos.z + M34[2][3];

				Vtx->Flags &= 0xFFFFFFFF - Vtx_Visible;
				//      if (*(int32_t *)(&Vtx->TPos_AOS.z)>0x3F800000) // 1.0 in floating point rep.
				if (Vtx->TPos_AOS.z > Sc->NZP)
				{
					Vtx->RZ = 1.0 / Vtx->TPos_AOS.z;
					Vtx->PX = Vtx->TPos_AOS.x*Vtx->RZ;
					Vtx->PY = Vtx->TPos_AOS.y*Vtx->RZ;
					//          Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
					//          Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
					//          Vtx->RU=Vtx->U*Vtx->RZ;
					//          Vtx->RV=Vtx->V*Vtx->RZ;
					if (Vtx->PX < 0) Vtx->Flags |= Vtx_VisLeft;
					if (Vtx->PX >= XRes) Vtx->Flags |= Vtx_VisRight;
					if (Vtx->PY < 0) Vtx->Flags |= Vtx_VisUp;
					if (Vtx->PY >= YRes) Vtx->Flags |= Vtx_VisDown;
					if (Vtx->TPos_AOS.z > Sc->FZP) Vtx->Flags |= Vtx_VisFar;
				}
				else Vtx->Flags |= Vtx_VisNear;

				//      printf("Regular shit!\n");
			}
		}
		else {
			// instead of all of these complications, I've decided to
			// make the face have void (*Clipper), that will do whatever it needs
			// in one call. the pre-filler will call the asm rasterizers twice
			// if necessary. back to the good old Avatar engine techniques ;)
			// at this section, the code also calculates environment mapping
			// coordinates to (EU,EV) by rotating the v. normals accordingly.
			if (!(T->Flags&Tri_Inside))
			{
				if (!(T->Flags&Tri_Ahead))
					goto ERegular;
				else goto EAhead;
			}
			// Intel inside...this rulez,all object completely inside frustrum.
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				MatrixXVector(M, &Vtx->Pos, &U);
				Vector_Add(&U, &V, &Vtx->TPos_AOS);

				Vtx->Flags = 0;
				Vtx->RZ = 1.0 / Vtx->TPos_AOS.z;
				Vtx->PX = Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY = Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
			}
			goto OUT;
			// This is in case 100% of trimesh AHEAD of camera. this saves some chks
		EAhead://Vertex_Loop1(T->Vertex,VEnd,M,&V);
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M, &Vtx->Pos, &U);
				Vector_Add(&U, &V, &Vtx->TPos_AOS);

				Vtx->RZ = 1.0 / Vtx->TPos_AOS.z;

				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+95.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->REU=Vtx->EU*Vtx->RZ;
//				Vtx->EV=128.0+95.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);
//				Vtx->REV=Vtx->EV*Vtx->RZ;

				Vtx->PX = Vtx->TPos_AOS.x*Vtx->RZ;
				Vtx->PY = Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
				//        Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
				//        Vtx->RU=Vtx->U*Vtx->RZ;
				//        Vtx->RV=Vtx->V*Vtx->RZ;
				if (Vtx->PX < 0) Vtx->Flags = Vtx_VisLeft; else Vtx->Flags = 0;
				if (Vtx->PX >= XRes) Vtx->Flags += Vtx_VisRight;
				if (Vtx->PY < 0) Vtx->Flags += Vtx_VisUp;
				if (Vtx->PY >= YRes) Vtx->Flags += Vtx_VisDown;
			}
			//    printf("Ahead VGA/Wizard.\n");
			goto OUT;
		ERegular:
			for (Vtx = T->Verts; Vtx < VEnd; Vtx++)
			{
				//    if (!Vtx->FRem) continue;
				MatrixXVector(M, &Vtx->Pos, &U);
				Vector_Add(&U, &V, &Vtx->TPos_AOS);

				Vtx->Flags = 0;
				//      if (*(int32_t *)(&Vtx->TPos_AOS.z)>0x3F800000) // 1.0 in floating point rep.
				// Environment mapping support removed at 11.04.02
//				Vtx->EU=128.0+127.0*(Vtx->N.x*IM[0][0]+Vtx->N.y*IM[0][1]+Vtx->N.z*IM[0][2]);
//				Vtx->EV=128.0+127.0*(Vtx->N.x*IM[1][0]+Vtx->N.y*IM[1][1]+Vtx->N.z*IM[1][2]);

				if (Vtx->TPos_AOS.z > Sc->NZP)
				{
					Vtx->RZ = 1.0 / Vtx->TPos_AOS.z;
					Vtx->PX = Vtx->TPos_AOS.x*Vtx->RZ;
					Vtx->PY = Vtx->TPos_AOS.y*Vtx->RZ;
					//          Vtx->PX=CntrEX+PX*Vtx->TPos_AOS.x*Vtx->RZ;
					//          Vtx->PY=CntrEY-PY*Vtx->TPos_AOS.y*Vtx->RZ;
					//          Vtx->RU=Vtx->U*Vtx->RZ;
					//          Vtx->RV=Vtx->V*Vtx->RZ;
//					Vtx->REU=Vtx->EU*Vtx->RZ;
//					Vtx->REV=Vtx->EV*Vtx->RZ;
					if (Vtx->PX < 0) Vtx->Flags = Vtx_VisLeft;
					if (Vtx->PX >= XRes) Vtx->Flags += Vtx_VisRight;
					if (Vtx->PY < 0) Vtx->Flags += Vtx_VisUp;
					if (Vtx->PY > YRes_1) Vtx->Flags += Vtx_VisDown;
				}
				else Vtx->Flags = Vtx_VisNear;
				//      printf("Regular shit!\n");
			}

		}
	OUT:
		// SoA Phase 6.1: dual-write T->Verts → T->frame. Without this,
		// the clipper's TPos-from-frame override reads stale main-camera
		// values during reflection rendering. Mirror of CITY's
		// Reflected_Transform — see that comment + Phase 6.2 docs.
		if (!T->frame) T->frame = new VertexFrame();
		T->frame->ensureSized(int(T->VIndex));
		VertexFrame_DumpFromAoS(T->frame, T->Verts, T->VIndex);

		FEnd = T->Faces + T->FIndex;
		for (F = T->Faces; F < FEnd; F++)
			if ((!(F->A->Flags&F->B->Flags&F->C->Flags&Vtx_Visible))
				&& ((F->Txtr->Flags&Mat_TwoSided) ||
					//(1)
				(AP.x*F->N.x + AP.y*F->N.y + AP.z*F->N.z < F->NormProd) //backface culling

					))
			{
				dz = F->A->TPos_AOS.z;
				if (F->B->TPos_AOS.z > dz) dz = F->B->TPos_AOS.z;
				if (F->C->TPos_AOS.z > dz) dz = F->C->TPos_AOS.z;
#ifdef FRONT_TO_BACK_SORTING
				F->SortZ.F = dz;
#else
				F->SortZ.F = fzp - dz;
#endif
				*Ins++ = { F->SortZ.DW, F };
			}
	}
	CPolys = Ins - FList;
	for (O = Sc->OmniHead; O; O = O->Next)
	{
		Vtx = &O->V;
		Vector ReflectedPos = O->IPos;
		ReflectedPos.y = -ReflectedPos.y;
		Vector_Sub(&ReflectedPos, &View->ISource, &V);
		MatrixXVector(View->Mat, &V, &Vtx->TPos_AOS);
		if (Vtx->TPos_AOS.z > Sc->NZP&&Vtx->TPos_AOS.z < Sc->FZP)
		{
			Vtx->RZ = 1.0 / Vtx->TPos_AOS.z;
			Vtx->PX = CntrEX + Vtx->TPos_AOS.x*PX*Vtx->RZ;
			Vtx->PY = CntrEY - Vtx->TPos_AOS.y*PY*Vtx->RZ;
			// Insert to List
			dz = Vtx->TPos_AOS.z;
			//dz *=-16384;
			//dz +=0x7FFFFFFF;
			//RoundToInt(&O->Face.SortZ.DW,dz);

			// FlareScale parity with the main pass (Transform.cpp:1835 /
			// CITY.CPP:823): decouple the reflected flare sprite from ISize so
			// authored FdsFlareScale shrinks the flare in the water reflection
			// too, not just the direct view. 0 = legacy 1.0 (byte-identical).
			O->F.FlareSize = O->ISize * (O->FlareScale > 0.0f ? O->FlareScale : 1.0f);
#ifdef FRONT_TO_BACK_SORTING
			O->F.SortZ.F = 2 * fzp - dz;
#else
			O->F.SortZ.F = fzp - dz;
#endif
			// no reflection of flares; single push with sortKey now
			// that SortZ.F has been computed above.
			*Ins++ = { O->F.SortZ.DW, &O->F };
		}
	}
	COmnies = (Ins - FList) - CPolys;
	for (I = 0; I < Sc->NumOfParticles; I++)
	{
		if (Sc->Pcl[I].Flags&Particle_Active)
		{
			if ((dz = Sc->Pcl[I].V.TPos_AOS.z) >= Sc->NZP) {
				F = &Sc->Pcl[I].F;
				//dz *=-16384;
				//dz +=0x7FFFFFFF;
				//RoundToInt(&F->SortZ.DW,dz);
#ifdef FRONT_TO_BACK_SORTING
				F->SortZ.F = 2 * fzp - dz;
#else
				F->SortZ.F = fzp - dz;
#endif
				//				*Ins++ = F;
			}
		}
	}
	CAll = Ins - FList;
	CPcls = CAll - COmnies - CPolys;
}


// This function is called to alter parameters within scene objects after loading from FLD.
static void SceneCorrections()
{
	// a crazy hack used to adjust omnilights in code.
	float OmniRangeMult[] =
		//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
	{ 0.9,  0.9,  0.3,  0.3,  0.4,  0.5,  0.5,  0.5,  0.5,  0.5 };
	float OmniSizeMult[] =
		//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
	{ 0.1,  0.1,  0.1,  0.1,  0.1,  0.1,  0.1,  0.1,  0.1,  0.1 };
	int OmniDisable[] =
	{ 0,    0,   0,    0,    0,    0,    0,    0,    0,    0 };
	int NumOmnis = sizeof(OmniDisable) / sizeof(int);

	Scene *Sc = ChaseSc;
	Omni *O;
	int32_t index;
	float redRangeMult = 100000.0;
	for (O = Sc->OmniHead, index = 0; O; O = O->Next)
	{
		//if (index == NumOmnis) break;

		// disable/enable omnies
		//if (OmniDisable[index]) O->Flags &=~Omni_Active;

		// adjust size / disable flares
//		if (OmniSizeMult[index] == 0)
//			O->Flags &=~(Omni_Flare | Omni_PointFlare);
		Spline_Scale(&O->Size, OmniSizeMult[index]);
		O->IRange = 1000.0;
		O->rRange = 1000.0;
		//O->ISize *= 50.0;
		//O->IRange *= 50.0;
		O->FallOff = 5000.0;

		// adjust range
//		Spline_Scale(&O->Range, OmniRangeMult[index]);
		//if (O->L.R == 255.0)
		Spline_Scale(&O->Range, redRangeMult);
		//Spline_Scale(&O->R, redRangeMult);

		//O->Flags |= Omni_Rand;
		index++;
	}
}

// sets required properties and modifies the object 'water.lwo' which acts like
// a mirror for the rest of the scene.
static void Reflective_Surface_Setup()
{
	TriMesh *T;
	Vertex *Vtx, *VE;
	Face *F, *FE;
	Vector U, V;

	Object *O;
	int32_t IID = 0;
	for (O = ChaseSc->ObjectHead; O; O = O->Next)
	{
		if (O->Type != Obj_TriMesh) continue;
		//		printf("[%d] Trimesh %s with %d Polygons\n",IID,O->Name,((TriMesh *)O->Data)->FIndex);
		T = (TriMesh *)(O->Data);

		if (!strcmp(O->Name, "water.lwo"))
		{
			RflObj = O;
			//T->SortPriorityBias = SP_DrawLast;
			F = T->Faces;
			Vector_Sub(&F->B->Pos, &F->A->Pos, &U);
			Vector_Sub(&F->C->Pos, &F->A->Pos, &V);
			Cross_Product(&U, &V, &RflSurfNorm);
			Vector_Norm(&RflSurfNorm);

			// patch: modify keys to position zero
			//Vector_Zero(&T->Pos.Keys[0].Pos.im());
			//Vector_Add(&F->A->Pos, &T->Pos.Keys[0].Pos.im(), &U);
			//RflSurfOfs = -Dot_Product(&RflSurfNorm, &U);

			F = T->Faces;
			FE = F + T->FIndex;

			// should write a transparent texture/gouraud to enable fog.
			for (; F < FE; F++)
			{
				F->U1 *= 0.25;
				F->V1 *= 0.25;
				F->U2 *= 0.25;
				F->V2 *= 0.25;
				F->U3 *= 0.25;
				F->V3 *= 0.25;
				F->Txtr->Flags |= Mat_Transparent;
			}

			// Capture the water plane (height + XZ extent) for the shared
			// procedural-water glint pass — the surface is XZ-aligned so the
			// plane height is the (constant) vertex Y.
			{
				float wmx = T->Verts[0].Pos.x, wMx = wmx;
				float wmz = T->Verts[0].Pos.z, wMz = wmz;
				for (dword wi = 1; wi < T->VIndex; ++wi) {
					const Vector& P = T->Verts[wi].Pos;
					if (P.x < wmx) wmx = P.x; if (P.x > wMx) wMx = P.x;
					if (P.z < wmz) wmz = P.z; if (P.z > wMz) wMz = P.z;
				}
				g_chaseWaterMinX = wmx; g_chaseWaterMaxX = wMx;
				g_chaseWaterMinZ = wmz; g_chaseWaterMaxZ = wMz;
				g_chaseWaterY = T->Verts[0].Pos.y;
			}

			// force scale
			//Vtx = T->Verts;
			//VE = Vtx + T->VIndex;
			//for (; Vtx < VE; Vtx++)
			//{
			//	Vector_SelfScale(&Vtx->Pos, 0.25);
			//	//int banana=1;
			//}

			// Tesselate reflective surface (helps affine gouraud/ shader work nicely)

			// This code section assumes rflSurfNorm = (0, 1, 0)
			// which means the surface is aligned to the XZ plane
			// furthermore it is assumed UV space maps onto XZ with only a scalar transformation
			// (no rotation)

			// Extract coverage (range) over XZ plane and over texture
			// (notice: this reads texture coords. from vertices, not faces).
/*			float mx, Mx, mz, Mz, y;
			float mu, Mu, mv, Mv;
			dword i, j;
			mx = Mx = T->Verts[0].Pos.x;
			mz = Mz = T->Verts[0].Pos.z;
			mu = Mu = T->Verts[0].U;
			mv = Mv = T->Verts[0].V;
			y = T->Verts[0].Pos.y;

			Material *RMat = T->Faces[0].Txtr;
			for(i=1; i<T->VIndex; i++)
			{
				float x = T->Verts[i].Pos.x;
				float z = T->Verts[i].Pos.z;
				if (x<mx) mx = x;
				if (x>Mx) Mx = x;
				if (z<mz) mz = z;
				if (z>Mz) Mz = z;

				float u = T->Verts[i].U;
				float v = T->Verts[i].V;
				if (u<mu) mu = u;
				if (u>Mu) Mu = u;
				if (v<mv) mv = v;
				if (v>Mv) Mv = v;

			}

			// replace vertices / polygons with a regular planar mesh
			delete [] T->Verts;
			delete [] T->Faces;
			const dword NumSections = 64;

			T->VIndex = (NumSections+1)*(NumSections+1);
			T->FIndex = NumSections*NumSections*2;

			T->Verts = new Vertex [T->VIndex];
			T->Faces = new Face [T->FIndex];

			memset(T->Verts, 0, sizeof(Vertex) * T->VIndex);
			memset(T->Faces, 0, sizeof(Face) * T->FIndex);

			Vtx = T->Verts;

			float dx = (Mx-mx) / (float)NumSections;
			float dz = (Mz-mz) / (float)NumSections;

			float du = (Mu-mu) / (float)NumSections;
			float dv = (Mv-mv) / (float)NumSections;

			float u, v, x, z;
			for(j=0, v = mv, z = mz; j<=NumSections; j++, v += dv, z += dz)
			{
				for(i=0, u = mu, x = mx; i<=NumSections; i++, u += du, x += dx)
				{
					Vtx->Pos.x = x;
					Vtx->Pos.y = y;
					Vtx->Pos.z = z;
					Vtx->U = u;
					Vtx->V = v;
					Vtx++;
				}
			}

			F = T->Faces;
			Vtx = T->Verts;
			for(j=0; j < NumSections; j++, Vtx += NumSections+1)
			{
				for(i=0; i < NumSections; i++)
				{
					F->A = Vtx+i;
					F->B = Vtx+i+1;
					F->C = Vtx+i+NumSections+2;
					F->Txtr = RMat;
					F->U1 = F->A->U;
					F->V1 = F->A->V;
					F->U2 = F->B->U;
					F->V2 = F->B->V;
					F->U3 = F->C->U;
					F->V3 = F->C->V;
					F++;
					F->A = Vtx+i;
					F->B = Vtx+i+NumSections+2;
					F->C = Vtx+i+NumSections+1;
					F->Txtr = RMat;
					F->U1 = F->A->U;
					F->V1 = F->A->V;
					F->U2 = F->B->U;
					F->V2 = F->B->V;
					F->U3 = F->C->U;
					F->V3 = F->C->V;
					F++;
				}
			}*/

			//			break;
		}
		else {
			//			T->Flags &=~HTrack_Visible;
		}
		if (strstr(O->Name, "pas"))
		{
			// attach rail tracks to ground
//			T->Pos.Keys[0].Pos.y = 0.0;

		}

		IID++;
	}
}



void Initialize_Chase()
{
	ChaseSc = loadSceneAligned("SCENES/CHASE.FLD");
	// Playback duration in Timer ticks (10 ms each → 100/sec). Chase plays FAST
	// by design: 1 authored frame ≈ 1 tick → the 1..1760 range runs in ~17.6 s,
	// the intended fast-paced feel (the authored 30 fps / ~58.6 s reads too slow
	// for this scene). The finale (Ship1's climb-out, keys ≤1760) plays because
	// EndFrame is 1760 — the only prior bug was an amputated frame range, not
	// the speed. Duration IS the speed knob: to re-time, change this mapping or
	// the authored frame range.
	CHPartTime = ChaseSc->EndFrame - ChaseSc->StartFrame;
	ChaseSc->FZP = 50000.0f;
	ChaseSc->NZP = 2.0f;
	ChaseSc->Flags |= Scn_Fogged | Scn_ZBuffer;

	// L2 mood fix: the 1998 ambient was (8,8,128) — a near-pure dark blue that
	// crushed every surface's albedo R/G to ~3% while blue sat at 50% (pixel =
	// texel x light / 256), so everything not directly lit read as flat navy
	// ("too dark, too blue"). Rebalanced to a desaturated cool grey-blue: R/G
	// lifted so surface albedo reads, blue dominance dropped so it's a night
	// tone not a blue filter. Directional shape now comes from the (revived)
	// moon key light (CHASE.LWS "Light": real LightRange + cool-white) rather
	// than from a saturated ambient wash.
	ChaseSc->Ambient.R = 47;
	ChaseSc->Ambient.G = 55;
	ChaseSc->Ambient.B = 76;

	tagSceneMaterials(ChaseSc, /*restrictToScene=*/true);

	Reflective_Surface_Setup();

	// Legacy 1998 load-time omni fixup — retired: the effective Size/Range
	// values are now authored in CHASE.LWS (LgtIntensity x0.1, LightRange
	// x100000). Default OFF; --chase-legacy-omni-hack re-applies it (only
	// correct against an un-baked LWS/FLD — it double-applies on the shipped
	// baked scene). See FeatureFlags.def.
	if (fds::FeatureFlags::chase_legacy_omni_hack())
		SceneCorrections();
	Preprocess_Scene(ChaseSc, "CHASE");

	// Persisted editor overrides (PBR maps + numeric surface props) — the
	// browser editor's "Save" writes this sidecar; no-op when absent.
	fds::MaterialImport_ApplySidecar(ChaseSc, "SCENES/CHASE.MAT");
	//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());

	// L2.4: build the sky cube (same SB*.JPG starfield skybox as city). Loaded
	// here so RunChaseSnapshot (which inits chase in isolation, never city)
	// gets it too. Rendered per-frame in tick() BEFORE the reflection pass.
	ChaseSkySc = CreateSkyCube(0);

	// B1: blaster bolts (ship2 fires at Ship1) — bake the bolt texture. Gated so
	// flag-off leaves the scene byte-identical (no texture, no omnis, no draw).
	if (fds::FeatureFlags::chase_blasters())
		fds::BlasterBolts_Init();
}

namespace {
struct ChaseScene : SceneDriver {
	int32_t partTime() const override { return CHPartTime; }
	FrameProfiler prof{"chase"};
	int32_t TTrd = 0;          // shared transport state (pause / scrub)
	bool    pause_mode = false;
	// M1.3 engine-glow throttle state (chase_engine_mod): the two ship meshes
	// + their P90 angular-rate reference, resolved once at init.
	TriMesh *ship1M = nullptr, *ship2M = nullptr;
	float    ship1Ref = 1.0f, ship2Ref = 1.0f;

	~ChaseScene() override { Destroy_Scene(ChaseSc); }

	void init() override {
		SetCurrentScene(ChaseSc);
		// Unified-TBR transparent path (same wiring as greets/city — see
		// GreetsScene::init). Chase's consumer is the water surface
		// (Mat_Transparent, set in the water setup above/below).
		if (fds::FeatureFlags::deferred_unified_tbr()) {
			TBR_EnsureInit(ChaseSc, 256);
			ChaseSc->Flags |= Scn_SpriteTBR;
		}
		Calibrate_FreeCamera_ForScene(ChaseSc->FZP, ChaseSc->CameraHead);
		Reflective_Mapper_Setup();
		Setup_Water_Distort();
		setupFaceLists(ChaseSc, /*includeOmnisInCount=*/false);
		// Procedural water (shared with city): build the field + caustic texture,
		// and register chase's water material so the deferred fresnel composite
		// (water_procedural) recognizes its pixels. The screen-space caustic/glint
		// pass runs in tick(); both need the water plane captured above.
		pwater::BuildField();
		if (RflObj) {
			TriMesh *wT = (TriMesh *)RflObj->Data;
			if (wT && wT->Faces && wT->Faces->Txtr)
				SetDeferredWaterMatID((int)wT->Faces->Txtr->ID);
		}
		// M1.3: locate the ship meshes (by name) + precompute their maneuver
		// reference for the engine-glow throttle. Init-time only; inert unless
		// chase_engine_mod is turned on in tick().
		ship1M = ship2M = nullptr;
		for (Object *o = ChaseSc->ObjectHead; o; o = o->Next) {
			if (!o->Name || !o->Data) continue;
			if (std::strstr(o->Name, "hip1")) ship1M = (TriMesh *)o->Data;
			else if (std::strstr(o->Name, "hip2")) ship2M = (TriMesh *)o->Data;
		}
		ship1Ref = chaseAngRateRef(ship1M, ChaseSc->StartFrame, ChaseSc->EndFrame, kEngineModD);
		ship2Ref = chaseAngRateRef(ship2M, ChaseSc->StartFrame, ChaseSc->EndFrame, kEngineModD);
		TTrd = Timer;
	}

	// B1: reconstruct the blaster combat as a PURE FUNCTION of CurFrame (§8.B) —
	// which bolts/flashes exist at t and where, rebuilt from the fire table every
	// frame (no accumulation → the jump-to-t snapshot reproduces it exactly).
	// Places bolts, drives the transient bolt-lights + muzzle/impact flashes.
	// MUST run after Animate_Objects (ship IPos/RotMat + omni IPos are set) and
	// before Lighting + Reflected_Transform (so the bolt omnis light + reflect).
	void driveBlasters() {
		if (!fds::FeatureFlags::chase_blasters() || !ship1M || !ship2M) return;
		const float t = CurFrame;
		const auto &table = chase::BlasterFireTable();

		// Ship1 world bounding radius at a frame (for miss-offset scaling).
		auto ship1Radius = [&](float f) -> float {
			Vector s; Spline_Calc_3D(&ship1M->Scale, f, &s);
			float sc = ship1M->EditorScale > 0.0f ? ship1M->EditorScale : 1.0f;
			return ship1M->BSphereRadius * sc * (s.x > 1e-4f ? s.x : 1.0f);
		};

		// Bolts: place each live bolt at its lerped muzzle→aim position.
		std::vector<chase::BoltState> bolts;
		chase::BlasterBoltsAliveAt(table, t, bolts);
		fds::BlasterBolts_ResetActive();
		for (const chase::BoltState &bs : bolts) {
			const Vector muzzle = chaseMuzzleWorld(ship2M, bs.fireFrame, bs.wing);
			Vector aim = chaseShipCenter(ship1M, bs.aimFrame);
			const float R = ship1Radius(bs.aimFrame);
			aim.x += bs.missX * R; aim.y += bs.missY * R; aim.z += bs.missZ * R;
			const Vector pos{ muzzle.x + (aim.x-muzzle.x)*bs.u,
			                  muzzle.y + (aim.y-muzzle.y)*bs.u,
			                  muzzle.z + (aim.z-muzzle.z)*bs.u };
			const Vector dir{ aim.x-muzzle.x, aim.y-muzzle.y, aim.z-muzzle.z };
			fds::BlasterBolts_Place(pos, dir, bs.r, bs.g, bs.b);
		}
		fds::BlasterBolts_EmitLights(ChaseSc);

		// Muzzle + impact flashes (fountain bolt_flash envelope, pure-t).
		const float decay  = fds::FeatureFlags::blaster_flash_decay();
		constexpr float kFlashWindow = 26.0f;   // frames
		std::vector<chase::FlashState> flashes;
		chase::BlasterFlashesAt(table, t, decay, kFlashWindow, flashes);
		fds::BlasterBolts_ResetFlashes(ChaseSc);
		for (const chase::FlashState &fs : flashes) {
			Vector p;
			if (fs.kind == 0) {
				p = chaseMuzzleWorld(ship2M, fs.fireFrame, fs.wing);
			} else {
				p = chaseShipCenter(ship1M, fs.aimFrame);
				const float R = ship1Radius(fs.aimFrame);
				p.x += fs.missX * R; p.y += fs.missY * R; p.z += fs.missZ * R;
			}
			fds::BlasterBolts_AddFlash(p, fs.intensity, fs.r, fs.g, fs.b);
		}
	}

	bool tick() override {
		// ESC = exit demo (handled by runSceneBlocking); Backspace = skip scene.
		if (Keyboard[ScBackSpace] || Timer >= CHPartTime) return false;

		// Mid-scene resize: keep the TBR strips tracking YRes.
		if (ChaseSc->Flags & Scn_SpriteTBR) TBR_EnsureInit(ChaseSc, 256);

		prof.beginFrame();
		prof.enter(PROF_ZCLR);

		// Shared transport: pause (P/U), fast-forward/rewind (F2/F1), and
		// the race-free scene-time snapshot. See SceneDriver::tickSceneTimer.
		tickSceneTimer(TTrd, pause_mode);
		clearFrame();

		CurFrame = ChaseSc->StartFrame + (ChaseSc->EndFrame-ChaseSc->StartFrame) * (float)g_FrameTime / (float)CHPartTime;

		tickTabToggle(ChaseSc, "chase");

		prof.switchTo(PROF_ANIM);
		Dynamic_Camera();
		if (Keyboard[ScC]) { FC.ISource = View->ISource; Matrix_Copy(FC.Mat, View->Mat); FC.IFOV = View->IFOV; }

		// L2.4 sky cube: paint the starfield backdrop into VPage now — BEFORE
		// the reflection + main passes. Rendered first (like city), it fills
		// every no-geometry pixel and clears the Z-buffer, so: (a) above the
		// horizon the main geometry draws over the sky, sky where nothing does;
		// (b) below the horizon the reflection pass draws mirrored geometry over
		// the sky, and the transparent water then blends the reflected sky where
		// no mirrored geometry covers it — the sky reflects on the water for
		// free. Uses the current-frame View (set by Dynamic_Camera above).
		RenderSkyCube(ChaseSkySc, View, false);

		Animate_Objects(ChaseSc, View);

		// M1.3 engine-glow throttle (default-off). Animate_Objects has just
		// re-set each omni's ISize from its (constant) Size spline, so this
		// per-frame multiply is a clean override — no accumulation. It runs
		// before Lighting + both Transform passes, so the punched ISize flows
		// into the light contribution AND the flare sprite (F.FlareSize =
		// ISize x FlareScale) in the main and reflected views alike.
		if (fds::FeatureFlags::chase_engine_mod()) {
			const float gain = fds::FeatureFlags::chase_engine_mod_gain();
			auto punch = [&](TriMesh *M, float ref) -> float {
				if (!M) return 1.0f;
				float n = chaseAngRate(M, CurFrame, kEngineModD) / ref;
				if (n > 1.0f) n = 1.0f;
				return 1.0f + gain * n;
			};
			const float mul1 = punch(ship1M, ship1Ref);
			const float mul2 = punch(ship2M, ship2Ref);
			for (Omni *O = ChaseSc->OmniHead; O; O = O->Next) {
				// warm engine glows only (253,65,2) — the white moon (255,255,255)
				// fails the green test and is left untouched.
				if (O->L.R < 200.0f || O->L.G > 130.0f || O->L.B > 60.0f) continue;
				const float z = O->Pos.NumKeys ? O->Pos.Keys[0].Pos.z : 0.0f;
				O->ISize *= (z < -76.0f) ? mul1 : mul2;   // -88 Ship1 / -64 ship2
			}
		}

		// B1: pure-t blaster reconstruction — after Animate_Objects (ship poses
		// + omni IPos set, engine-glow punch applied), before Lighting +
		// Reflected_Transform so the bolt omnis + flashes light and reflect.
		driveBlasters();

		((TriMesh *)(RflObj->Data))->Flags &= 0xFFFFFFFF - HTrack_Visible;

		prof.switchTo(PROF_LGHT);
		Lighting(ChaseSc);

		prof.switchTo(PROF_XFRM);
		Reflected_Transform(ChaseSc);
		prof.leave(PROF_XFRM);

		if (CAll) {
			prof.enter(PROF_SORT);
			Radix_Sort(FList, SList, CAll);
			prof.switchTo(PROF_RNDR);
			Render();
			prof.leave(PROF_RNDR);
		}

		// B1 (§8.E): reflect the bolt streaks on the water — mirrored, dimmed
		// additive billboards drawn AFTER the reflection Render and BEFORE the
		// main pass, so the transparent water composites over them like the
		// reflected world. Bolts only (cheap); dense particles are NOT reflected.
		if (fds::FeatureFlags::chase_blasters())
			fds::BlasterBolts_DrawReflected(g_chaseWaterY);

		prof.enter(PROF_XFRM);
		((TriMesh *)(RflObj->Data))->Flags |= HTrack_Visible;
		Transform_Objects(ChaseSc, fds::g_mainCamera, fds::g_mainFaces);
		prof.leave(PROF_XFRM);
		if (!CAll) return true;
		prof.enter(PROF_SORT);
		Radix_Sort(FList, SList, CAll);
		prof.switchTo(PROF_RNDR);
		Render();
		prof.leave(PROF_RNDR);

		// Procedural caustic texture + specular glints over the water plane
		// (shared with city). Screen-space, peel-independent, occlusion-tested.
		if (fds::FeatureFlags::water_bump())
			pwater::RenderGlints(g_chaseWaterY, g_chaseWaterMinX, g_chaseWaterMaxX,
			                     g_chaseWaterMinZ, g_chaseWaterMaxZ);

		// B1: blaster bolts — additive screen-space draw over the finished frame
		// (post-tonemap, like greets). Occluded against terrain/ships by the
		// filler's built-in ZPage16 Z-test (§8.C: WriteZ=false still gates on
		// depth). Drawn last so bolts glow over the composited scene + glints.
		if (fds::FeatureFlags::chase_blasters())
			fds::BlasterBolts_Draw();

		prof.enter(PROF_FLIP);
		Flip(MainSurf);
		prof.leave(PROF_FLIP);
		prof.endFrame();
		return true;
	}

	void cleanup() override {
		if (g_profilerActive) prof.dump();
		Timer -= CHPartTime;
		waitBackspaceRelease();
		if (Timer < 0) Timer = 0;
	}
};
} // anonymous namespace

std::unique_ptr<SceneDriver> createChaseScene()
{
	fds::FeatureFlags::setDefault(fds::FeatureFlags::BoolId::water_procedural, true);
	ApplyCinematicProfile(cine::kChase);          // same cityscape atmosphere as city
	// Authored scene-level defaults (scene:|key|value sidecar lines) AFTER
	// the profile so authored values beat it; explicit CLI/env still wins.
	fds::MaterialImport_ApplySceneDefaults("SCENES/CHASE.MAT");
	return std::make_unique<ChaseScene>();
}

void Run_Chase()
{
	auto scene = createChaseScene();
	runSceneBlocking(*scene);
}
