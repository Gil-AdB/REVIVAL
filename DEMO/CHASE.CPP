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

// Deterministic hash → [0,1). Pure — the impact spray is reconstructed from
// (seed,k) every frame (no rand state), so snapshots reproduce it.
inline float chaseHash01(uint32_t x) {
	x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
	return float(x & 0x00FFFFFFu) / float(0x01000000u);
}

// Build the 32×32 radial additive disc for impact-spark sprites (the fountain
// particle-texture recipe: white radial falloff, tinted per-particle via the
// vertex LR/LG/LB the sprite filler reads). Linear buffer — TBR_Sprite samples
// it directly (unlike the swizzled deferred textures).
Material *g_chaseSparkMat = nullptr;
Material *buildChaseSparkMat() {
	Texture *T = new Texture;
	std::memset(T, 0, sizeof(Texture));
	T->BPP = 32;
	T->SizeX = 32; T->LSizeX = 5;
	T->SizeY = 32; T->LSizeY = 5;
	DWord *px = new DWord[32 * 32];
	for (int y = 0; y < 32; ++y) {
		const float fy = 1.0f - 2.0f * (float(y) + 0.5f) / 32.0f;
		for (int x = 0; x < 32; ++x) {
			const float fx = -1.0f + 2.0f * (float(x) + 0.5f) / 32.0f;
			const float r2 = fx * fx + fy * fy;
			DWord c = 0;
			if (r2 < 1.0f) {
				const float f = (1.0f - r2);
				c = 0x010101u * (DWord)(f * f * 255.0f);   // soft core
			}
			px[x + 32 * y] = c;
		}
	}
	T->Data = (byte *)px;
	Material *M = new Material;
	*M = Material{};
	M->Txtr = T;
	return M;
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
	// B2 mountain-hit aim: world centre + radius of every canyon-wall instance,
	// captured at init (static single-key meshes). resolveAimPoint(kind=2) picks
	// the wall whose SURFACE is nearest Ship1 and aims a spark burst at it.
	struct MtnInst { Vector ctr; float rad; };
	std::vector<MtnInst> mountains;

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
		mountains.clear();
		for (Object *o = ChaseSc->ObjectHead; o; o = o->Next) {
			if (!o->Name || !o->Data) continue;
			if (std::strstr(o->Name, "hip1")) ship1M = (TriMesh *)o->Data;
			else if (std::strstr(o->Name, "hip2")) ship2M = (TriMesh *)o->Data;
			// Canyon walls: m1..m5, mm7, big_m — static single-key meshes. Capture
			// their world centre + radius once (any frame; motion is constant) for
			// the B2 mountain-hit aim. Only needed when combat is on.
			else if (fds::FeatureFlags::chase_blasters() &&
			         (o->Name[0] == 'm' || o->Name[0] == 'M')) {
				TriMesh *M = (TriMesh *)o->Data;
				mountains.push_back({ chaseShipCenter(M, 0.0f), shipWorldRadius(M, 0.0f) });
			}
		}
		ship1Ref = chaseAngRateRef(ship1M, ChaseSc->StartFrame, ChaseSc->EndFrame, kEngineModD);
		ship2Ref = chaseAngRateRef(ship2M, ChaseSc->StartFrame, ChaseSc->EndFrame, kEngineModD);
		TTrd = Timer;

		// B2: impact-spark particle pool (chase_blasters only). Reconstructed
		// pure-t in driveBlasters from the impact events; TBR_Sprite additive
		// discs. Not allocated when the flag is off → NumOfParticles stays 0 and
		// the engine's particle loops are no-ops (default chase byte-identical).
		if (fds::FeatureFlags::chase_blasters() && !ChaseSc->Pcl) {
			if (!g_chaseSparkMat) g_chaseSparkMat = buildChaseSparkMat();
			const int N = 2560;
			ChaseSc->NumOfParticles = N;
			ChaseSc->Pcl = new Particle[N];
			std::memset(ChaseSc->Pcl, 0, sizeof(Particle) * N);
			for (int i = 0; i < N; ++i) {
				Particle &P = ChaseSc->Pcl[i];
				P.F.A = P.F.B = &P.V;
				P.F.Filler = TBR_Sprite;
				P.F.Txtr = g_chaseSparkMat;
				P.Flags = 0;
			}
		}
	}

	// Ship world bounding radius at a frame (miss-offset + fx scaling). Shared
	// by the blaster spawner and the camera-fx proximity math.
	float shipWorldRadius(TriMesh *M, float f) {
		if (!M) return 1.0f;
		Vector s; Spline_Calc_3D(&M->Scale, f, &s);
		const float sc = M->EditorScale > 0.0f ? M->EditorScale : 1.0f;
		return M->BSphereRadius * sc * (s.x > 1e-4f ? s.x : 1.0f);
	}

	// Resolve a bolt's aim / impact world point. kind (the fire-table `water`
	// field): 0 hull → ship1 centre + miss·R; 1 water → the sea beside Ship1
	// (x,z = ship1 + miss·R, y = the water plane); 2 mountain → the nearest
	// canyon-wall surface point to Ship1 (miss nudges it tangentially). One place
	// so bolts, flashes, sparks and camera-fx agree.
	Vector resolveAimPoint(int kind, float aimFrame, float mx, float my, float mz) {
		const Vector c = chaseShipCenter(ship1M, aimFrame);
		const float R = shipWorldRadius(ship1M, aimFrame);
		if (kind == 1) return Vector{ c.x + mx * R, g_chaseWaterY, c.z + mz * R };
		if (kind == 2) return resolveMountainPoint(c, R, mx, my, mz);
		return Vector{ c.x + mx * R, c.y + my * R, c.z + mz * R };
	}

	// Nearest canyon-wall surface point to Ship1 centre `c`. Picks the mountain
	// whose bounding-sphere SURFACE is closest, then a point on that sphere facing
	// the ship (so sparks read as landing ON the rock). miss·R nudges it along the
	// wall so a burst walks fire; my raises it up the face. Falls back to a hull-
	// style point if no mountain was captured (combat off, or none in scene).
	Vector resolveMountainPoint(const Vector &c, float R, float mx, float my, float mz) {
		if (mountains.empty()) return Vector{ c.x + mx * R, c.y + my * R, c.z + mz * R };
		int best = -1; float bestD = 1e30f;
		for (size_t i = 0; i < mountains.size(); ++i) {
			const Vector &m = mountains[i].ctr;
			const float d = std::sqrt((m.x-c.x)*(m.x-c.x) + (m.y-c.y)*(m.y-c.y) +
			                          (m.z-c.z)*(m.z-c.z)) - mountains[i].rad;
			if (d < bestD) { bestD = d; best = (int)i; }
		}
		const MtnInst &M = mountains[best];
		// Direction from the wall centre toward the ship → the near face. Place the
		// burst JUST OUTSIDE the bounding sphere toward the ship (×1.0) so it sits
		// on/in front of the rock and is never occluded by it. A tangential/vertical
		// nudge (miss·R, R = ship radius) scatters bursts along the wall.
		float dx = c.x-M.ctr.x, dy = c.y-M.ctr.y, dz = c.z-M.ctr.z;
		float L = std::sqrt(dx*dx + dy*dy + dz*dz); if (L < 1e-3f) L = 1e-3f;
		dx /= L; dy /= L; dz /= L;
		Vector p{ M.ctr.x + dx * M.rad,
		          M.ctr.y + dy * M.rad,
		          M.ctr.z + dz * M.rad };
		p.x += mx * R; p.z += mz * R;
		p.y += (my + 0.5f) * R;                 // bias up the face
		if (p.y < g_chaseWaterY) p.y = g_chaseWaterY;
		return p;
	}

	// Bounding radius of the canyon wall a mountain-hit lands on (nearest-surface
	// to Ship1 @ frame). Used to scale the spark burst to the WALL, not the ship —
	// a ship-radius burst is an invisible speck on a thousand-unit rock. Returns a
	// sane fallback if no mountains were captured.
	float nearestMountainRadius(float aimFrame) {
		if (mountains.empty()) return 1.0f;
		const Vector c = chaseShipCenter(ship1M, aimFrame);
		int best = -1; float bestD = 1e30f;
		for (size_t i = 0; i < mountains.size(); ++i) {
			const Vector &m = mountains[i].ctr;
			const float d = std::sqrt((m.x-c.x)*(m.x-c.x) + (m.y-c.y)*(m.y-c.y) +
			                          (m.z-c.z)*(m.z-c.z)) - mountains[i].rad;
			if (d < bestD) { bestD = d; best = (int)i; }
		}
		return mountains[best].rad;
	}

	// C1: deterministic camera FX (chase_cam_fx) — shake + FOV punch on hits.
	// Applied AFTER Animate_Objects (which set View's base pose + FOVX/FOVY) so
	// the perturbation rides on the authored dolly; reconstructed pure-t from the
	// impact table (amplitude decays after each hit, scaled by camera proximity),
	// so snapshots reproduce it and flag-off / no-active-hit leaves View untouched.
	void applyCameraFx() {
		if (!fds::FeatureFlags::chase_cam_fx() || !ship1M || !View) return;
		const float t = CurFrame;
		constexpr float kShakeWin = 12.0f;   // frames a hit shakes the camera
		std::vector<chase::ImpactState> impacts;
		chase::BlasterImpactsAt(chase::BlasterFireTable(), t, kShakeWin, impacts);
		if (impacts.empty()) return;

		const Vector cam = View->ISource;
		float amp = 0.0f;
		for (const chase::ImpactState &im : impacts) {
			const Vector P = resolveAimPoint(im.water, im.aimFrame, im.missX, im.missY, im.missZ);
			const float R = shipWorldRadius(ship1M, im.aimFrame);
			const float dx = P.x-cam.x, dy = P.y-cam.y, dz = P.z-cam.z;
			const float dist = std::sqrt(dx*dx + dy*dy + dz*dz) + 1e-3f;
			const float decay = 1.0f - im.age / kShakeWin;    // 1 at hit → 0
			float prox = (6.0f * R) / dist; if (prox > 1.5f) prox = 1.5f;
			amp += decay * decay * prox;
		}
		if (amp < 1e-3f) return;
		if (amp > 2.0f) amp = 2.0f;

		const float R0 = shipWorldRadius(ship1M, t);
		const float m  = amp * fds::FeatureFlags::chase_cam_shake_gain() * R0;
		const float ph = t;
		const float sx = std::sin(ph*2.9f)      * 0.55f + std::sin(ph*6.1f+1.3f) * 0.45f;
		const float sy = std::sin(ph*3.7f+0.7f) * 0.55f + std::sin(ph*7.3f+2.1f) * 0.45f;
		const float sz = std::sin(ph*3.3f+2.7f);
		View->ISource.x += sx * m;
		View->ISource.y += sy * m * 0.7f;   // less vertical jitter
		View->ISource.z += sz * m;

		// Roll about the view forward axis: rotate Mat rows 0 (right) / 1 (up).
		const float rollAng = amp * 0.012f * std::sin(ph*4.3f + 0.9f);   // radians
		const float cr = std::cos(rollAng), sr = std::sin(rollAng);
		for (int c = 0; c < 3; ++c) {
			const float r0 = View->Mat[0][c], r1 = View->Mat[1][c];
			View->Mat[0][c] =  cr*r0 + sr*r1;
			View->Mat[1][c] = -sr*r0 + cr*r1;
		}

		// FOV punch inward on the hit (brief zoom), recovering as amp decays.
		// Re-set the FOVX/FOVY globals after CalcPersp exactly like Animate_Objects.
		const float kick = fds::FeatureFlags::chase_cam_fov_kick();
		if (kick != 0.0f) {
			View->IFOV -= kick * (amp > 1.0f ? 1.0f : amp);
			if (View->IFOV < 4.0f) View->IFOV = 4.0f;
			CalcPersp(View);
			FOVX = View->PerspX;
			FOVY = View->PerspY;
		}
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

		// Bolt streak size scales with the current ship2→Ship1 gap so tracers
		// read at chase scale (ships tens of units, water spans thousands) —
		// full bolt ≈ 12% of the gap, width a slim fraction of that.
		{
			const Vector s2 = chaseShipCenter(ship2M, t);
			const Vector s1 = chaseShipCenter(ship1M, t);
			const float dx=s1.x-s2.x, dy=s1.y-s2.y, dz=s1.z-s2.z;
			float half = 0.06f * std::sqrt(dx*dx + dy*dy + dz*dz);
			if (half < 2.0f) half = 2.0f;
			fds::BlasterBolts_SetBoltSize(half, 0.14f * half);
		}

		// Ship1 world bounding radius at a frame (for miss-offset scaling).
		auto ship1Radius = [&](float f) -> float { return shipWorldRadius(ship1M, f); };

		// Bolts: place each live bolt at its lerped muzzle→aim position.
		std::vector<chase::BoltState> bolts;
		chase::BlasterBoltsAliveAt(table, t, bolts);
		fds::BlasterBolts_ResetActive();
		for (const chase::BoltState &bs : bolts) {
			const Vector muzzle = chaseMuzzleWorld(ship2M, bs.fireFrame, bs.wing);
			const Vector aim = resolveAimPoint(bs.water, bs.aimFrame, bs.missX, bs.missY, bs.missZ);
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
			const Vector p = (fs.kind == 0)
				? chaseMuzzleWorld(ship2M, fs.fireFrame, fs.wing)
				: resolveAimPoint(fs.water, fs.aimFrame, fs.missX, fs.missY, fs.missZ);
			fds::BlasterBolts_AddFlash(p, fs.intensity, fs.r, fs.g, fs.b);
		}

		// B2: impact-spark particles — a deterministic ballistic spray per bolt
		// impact, reconstructed pure-t (positions are f(seed,k,age), never
		// integrated). HULL hits burst up-and-out and fall; WATER near-misses
		// (kind 1) throw a TALL vertical splash column; MOUNTAIN hits (kind 2)
		// spray sparks off the rock. Additive TBR_Sprite discs tinted by bolt.
		if (ChaseSc->Pcl) {
			constexpr float kPclLife = 52.0f;   // frames
			constexpr int   kPerImpact = 30;
			// TBR_Sprite screen size = ImageSize(1000)·RZ·PerspX·FlareSize, so a
			// world-space disc of radius w has FlareSize ≈ w/1000. Sparks are a
			// small fraction of the target ship radius; size + brightness are
			// tunable FeatureFlags, spread + gravity baked to the ship scale.
			const float szK = fds::FeatureFlags::chase_spark_size();
			const float brK = fds::FeatureFlags::chase_spark_bright();
			constexpr float spK = 0.008f;   // spread speed  (× shipRadius / frame)
			constexpr float gK  = 0.0004f;  // gravity       (× shipRadius / frame^2)
			std::vector<chase::ImpactState> impacts;
			chase::BlasterImpactsAt(table, t, kPclLife, impacts);
			const int N = ChaseSc->NumOfParticles;
			Particle *Pcl = ChaseSc->Pcl;
			int slot = 0;
			for (const chase::ImpactState &im : impacts) {
				const Vector P = resolveAimPoint(im.water, im.aimFrame, im.missX, im.missY, im.missZ);
				const bool  isWater = (im.water == 1);
				const bool  isMtn   = (im.water == 2);
				// Burst scale: ship-radius for hull/water sparks; a fraction of the
				// CANYON-WALL radius for mountain hits (a ship-sized burst is an
				// invisible speck on a thousand-unit rock — scale it to the wall).
				float R = ship1Radius(im.aimFrame);
				if (isMtn) { const float mr = nearestMountainRadius(im.aimFrame);
				             R = mr * 0.13f;      // spread scale (fly distance)
				             if (R < 180.0f) R = 180.0f; else if (R > 650.0f) R = 650.0f; }
				const float lifeFrac = im.age / im.life;
				const float fade = 1.0f - lifeFrac;              // brightness/size decay
				// Water columns rise high on a lighter gravity (tall spout, up then
				// fall); mountain sparks fly off the wall on light gravity; hull
				// sparks burst up-and-out.
				const float g = R * gK * (isWater ? 1.5f : (isMtn ? 0.7f : 1.0f));
				for (int k = 0; k < kPerImpact && slot < N; ++k) {
					const uint32_t base = im.seed + uint32_t(k) * 40503u;
					const float r1 = chaseHash01(base + 1u);
					const float r2 = chaseHash01(base + 2u);
					const float r3 = chaseHash01(base + 3u);
					const float az = r1 * 6.2831853f;
					const float ct = isWater ? (0.92f + 0.08f * r2)    // tall near-vertical column
					               : isMtn   ? (0.05f + 0.95f * r2)     // wide spray off the rock
					                         : (0.15f + 0.85f * r2);    // up-biased hemisphere
					const float st = std::sqrt(std::max(0.0f, 1.0f - ct * ct));
					const Vector dir{ st * std::cos(az), ct, st * std::sin(az) };
					const float speed = R * spK * (isWater ? (1.9f + 1.3f * r3)   // taller column
					                             : isMtn   ? (0.7f + 0.9f * r3)
					                                       : (0.5f + 0.5f * r3));
					Particle &pc = Pcl[slot++];
					pc.V.Pos.x = P.x + dir.x * speed * im.age;
					pc.V.Pos.y = P.y + dir.y * speed * im.age - 0.5f * g * im.age * im.age;
					pc.V.Pos.z = P.z + dir.z * speed * im.age;
					// Water columns stay bright longer (slower fade); mountain sparks
					// are dimmer + more saturated (many small discs over a wide spray
					// must NOT additively blow to a white ball). brK = FeatureFlags peak.
					const float bright = brK * (isWater ? (0.42f + 0.45f * fade)
					                          : isMtn   ? (0.40f * fade)
					                                    : fade);
					// hull/water: hot-white core + tint; mountain: warmer/less white.
					const float coreW = isMtn ? 0.25f : 0.5f;
					pc.V.LR = (coreW + (1.0f - coreW) * im.r) * bright;
					pc.V.LG = (coreW + (1.0f - coreW) * im.g) * bright;
					pc.V.LB = (coreW + (1.0f - coreW) * im.b) * bright;
					pc.F.FlareSize = R * szK * (isWater ? 1.35f : (isMtn ? 0.30f : 1.0f)) * (0.35f + 0.65f * fade);
					pc.Flags = Particle_Active;
				}
			}
			for (int i = slot; i < N; ++i) Pcl[i].Flags &= ~Particle_Active;
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

		// C1: camera shake / FOV punch on hits (default-off chase_cam_fx).
		// After Animate_Objects (View's base pose + FOVX/FOVY are set), before the
		// reflection + main transforms so every pass uses the perturbed camera.
		applyCameraFx();

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
		if (fds::FeatureFlags::water_bump()) {
			// drama-5: chase opts into the varied (less-regular) sea. Call-site
			// dispatch keeps the default RenderGlints byte-identical for city.
			if (fds::FeatureFlags::water_variation())
				pwater::RenderGlintsVaried(g_chaseWaterY, g_chaseWaterMinX, g_chaseWaterMaxX,
				                           g_chaseWaterMinZ, g_chaseWaterMaxZ);
			else
				pwater::RenderGlints(g_chaseWaterY, g_chaseWaterMinX, g_chaseWaterMaxX,
				                     g_chaseWaterMinZ, g_chaseWaterMaxZ);
		}

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
	// Procedural FIELD only (ripple/glints/caustics): the kernel's fresnel
	// deep-colour composite assumes a bright reflection underlay (city); chase
	// reflects a black night sky, so it must keep the forward-style lit-texel
	// blend or the water goes dark with a view-angle band (soa-vertex 9902349).
	fds::FeatureFlags::setDefault(fds::FeatureFlags::BoolId::water_fresnel_composite, false);
	// Varied, less-regular sea (drama-5): layer a low-frequency swell + an extra
	// rotated ripple octave on the wave field and sum multi-scale caustics so
	// chase's open water reads natural instead of a uniform repeating field.
	// Default-off flag → city/fountain byte-identical; chase opts in here.
	fds::FeatureFlags::setDefault(fds::FeatureFlags::BoolId::water_variation, true);
	ApplyCinematicProfile(cine::kChase);          // same cityscape atmosphere as city
	// Chase-scale bolt-light reach (B1): the module default (range 14) is
	// greets-room scale — invisible here where ships are hundreds of units
	// apart. Give the travelling bolt-lights a scene-appropriate reach so they
	// pool moving glows on the water. setDefault → explicit CLI/env still wins,
	// and it only matters under --chase_blasters (EmitLights is gated on it).
	fds::FeatureFlags::setDefault(fds::FeatureFlags::FloatId::blaster_light_range, 90.0f);
	fds::FeatureFlags::setDefault(fds::FeatureFlags::FloatId::blaster_light_intensity, 260.0f);
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
