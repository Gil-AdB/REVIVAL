// Forward (per-vertex) lighting pass. Computes Vtx->L{R,G,B,A} from the
// scene's omnis + ambient + per-material luminosity; consumed by the
// forward fillers' colorize step. Deferred path bypasses this entirely
// and runs its own per-pixel kernel against the G-buffer.
//
// Three entry points:
//   LightingOld(Sc)     — pre-LightWave3D ad-hoc model. Dead code, kept
//                         as a reference until the Tier 3 cleanup.
//   StaticLighting(Sc)  — bakes lights tagged Omni_Stationary into
//                         T->SL[] once per scene. Called automatically
//                         from Lighting() on first frame; some scenes
//                         set Scn_StaticLighting up front to opt out.
//   Lighting(Sc)        — per-frame: re-evaluates non-stationary omnis,
//                         folds in the cached SL[], writes Vtx->L*.

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "Base/Scene.h"
#include "Base/TriMesh.h"
#include "Base/Vertex.h"
#include "Base/Omni.h"
#include "Base/Material.h"
#include "Base/Camera.h"
#include "Base/FeatureFlags.h"

#include <semaphore>
#include <vector>
#include <climits>
#include "Threads.h"              // ThreadPool — fan Lighting() per mesh

float Vector_CosAngleFAST(Vector *U,Vector *V)
{
	return (U->x*V->x+U->y*V->y+U->z*V->z)*
		RSQRT((U->x*U->x+U->y*U->y+U->z*U->z)*(V->x*V->x+V->y*V->y+V->z*V->z));
}


// This lighting module is complient with LightWave 3D, and should be optimized using
// SSE assembly code
//#define LIGHTING_CALC_ALPHA
void StaticLighting(Scene *Sc)
{
	TriMesh *T;
	Vertex *V,*VE;
	Face *F,*FE;
	Omni *O;

	Vector u, v, w;
	dword n;

	Matrix M;
	Material *Mat;
	float Lumin;

	// hopefully, paragraph aligned
	static Color l;
	static Color Ambient;

	// light position vector
	static CurLight LA[128], *L, *LE;

	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T->Flags&Tri_Noshading) continue;
		if (!(T->Flags & Tri_Stationary)) continue;

		if (T->FIndex)
		{
			Mat = T->Faces[0].Txtr;
			Lumin = Mat->Luminosity;
		} else Lumin = 0.0;

		// Calculate ambient factor
		// LW3D: Luminosity * base color + scene Ambient * diffuse level
		if (Mat->Txtr)
		{
			Ambient.B = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.R;
#ifdef LIGHTING_CALC_ALPHA
			Ambient.A = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.A;
#endif
		} else {
			Ambient.B = Lumin * Mat->BaseCol.B + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * Mat->BaseCol.G + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * Mat->BaseCol.R + Mat->Diffuse * Sc->Ambient.R;
#ifdef LIGHTING_CALC_ALPHA
			Ambient.A = Lumin * Mat->BaseCol.A + Mat->Diffuse * Sc->Ambient.A;
#endif
		}

		for(O=Sc->OmniHead, L=LA;O;O=O->Next)
		{
			if (!(O->Flags & Omni_Active))
				continue;
			// Mirror-clone omnis light only their mirror's clone pixels
			// (a per-pixel DEFERRED concept — gb.mirrorId gating).
			// Forward vertex lighting has no such gate, so the ~6×15
			// full-range greets clones would brighten REAL geometry.
			// Vertex colors' only consumer in deferred mode is the
			// mirror RTT offscreen pass, which renders the real scene
			// and must see real lights only.
			if (O->Flags & Omni_MirrorClone)
				continue;
			if (!(O->Flags & Omni_Stationary))
				continue;
			Vector_Sub(&O->IPos, &T->IPos, &u);
			MatrixTXVector(T->RotMat, &u, &w);
			Vector *wp = (Vector *)(T->RotMat);
			float rScale = 1.0/Vector_Length(wp);
			Vector_SelfScale(&w, rScale*rScale);
			Vector_Copy(&L->Pos, &w);

			L->Range = O->IRange * rScale;
			L->Range2 = L->Range * L->Range;
			L->rRange = 1.0/L->Range;

			// Spot light: rotate the cone axis into the mesh's local
			// space so the per-vertex `dot(L->Dir, vertex_to_light)`
			// matches the same coordinate system as the rest of this
			// loop. cosInner/cosOuter come from the Omni unchanged
			// (they're cosines, no scaling needed).
			L->isSpot = (O->Type == Light_SpotLight) ? 1 : 0;
			if (L->isSpot) {
				MatrixTXVector(T->RotMat, (Vector*)&O->IDir, &L->Dir);
				Vector_Norm(&L->Dir);
				L->cosInner = O->HotSpot;
				L->cosOuter = O->FallOff;
			} else {
				L->Dir.x = L->Dir.y = L->Dir.z = 0.0f;
				L->cosInner = -2.0f;  // sentinel: "always inside"
				L->cosOuter = -2.0f;
			}

			if (Mat->Txtr)
			{
				float intensity = O->ISize * Mat->Diffuse;
				L->Col.B = O->L.B * intensity;
				L->Col.G = O->L.G * intensity;
				L->Col.R = O->L.R * intensity;
#ifdef LIGHTING_CALC_ALPHA
				L->Col.A = O->L.A * intensity;
#endif
			} else {
				float intensity = O->ISize * Mat->Diffuse / 256.0;
				L->Col.B = O->L.B * Mat->BaseCol.B * intensity;
				L->Col.G = O->L.G * Mat->BaseCol.G * intensity;
				L->Col.R = O->L.R * Mat->BaseCol.R * intensity;
#ifdef LIGHTING_CALC_ALPHA
				L->Col.A = O->L.A * Mat->BaseCol.A * intensity;
#endif
			}
			L++;
		}
		LE = L;

		mword vi=0;
		for(V=T->Verts, VE = V+T->VIndex; V<VE; V++,vi++)
		{
			l.B = Ambient.B;
			l.G = Ambient.G;
			l.R = Ambient.R;
#ifdef LIGHTING_CALC_ALPHA
			l.A = Ambient.A;
#endif
			for(L=LA; L<LE; L++)
			{
				Vector_Sub(&L->Pos, &V->Pos, &w);
				float dot = Dot_Product(&w, &V->N);
				if (dot < 0.0) continue;
				float v = Vector_Length(&w);
				float len2 = w.x*w.x+w.y*w.y+w.z*w.z;
				if (len2 > L->Range2) continue;
				float len = SQRT(len2);
				float k = dot / len * (1.0-len*L->rRange);

				// Spot cone attenuation: cosTheta = dot(-Dir, light_to_vertex_unit).
				// w runs vertex→light, so the light's outgoing direction
				// at the vertex is -w/|w| relative to the vertex but +w/|w|
				// relative to the light. We want angle between L->Dir
				// (cone axis) and the vector FROM the light TO the vertex
				// (= -w/|w|). Equivalently: cosTheta = dot(L->Dir, -w/|w|)
				//                                   = -dot(L->Dir, w)/|w|.
				if (L->isSpot) {
					float invLen = 1.0f / len;
					float cosTheta = -(L->Dir.x*w.x + L->Dir.y*w.y + L->Dir.z*w.z) * invLen;
					if (cosTheta <= L->cosOuter) continue;  // outside outer cone
					if (cosTheta < L->cosInner) {
						float t = (cosTheta - L->cosOuter) / (L->cosInner - L->cosOuter);
						k *= t * t * (3.0f - 2.0f * t);  // smoothstep edge falloff
					}
					// (cosTheta >= cosInner: inside inner cone, no attenuation)
				}

				l.B += k*L->Col.B;
				l.G += k*L->Col.G;
				l.R += k*L->Col.R;
#ifdef LIGHTING_CALC_ALPHA
				l.A += k*L->Col.A;
#endif
			}

			// saturation
			if (l.B > 250.0) l.B = 250.0;
			if (l.G > 250.0) l.G = 250.0;
			if (l.R > 250.0) l.R = 250.0;
#ifdef LIGHTING_CALC_ALPHA
			if (l.A > 250.0) l.A = 250.0;
#endif

			T->SL[vi].B = l.B;
			T->SL[vi].G = l.G;
			T->SL[vi].R = l.R;
#ifdef LIGHTING_CALC_ALPHA
			T->SL[vi].A = l.A;
#endif
		}
	}
}



// Lighting() now runs unconditionally. The deferred path bypasses
// Mekalele for Face_Reflective faces and dispatches them through the
// forward TheOtherBarry<OVERWRITE, TEXTURETEXTURE> filler — which reads
// Vtx->LR/LG/LB for its colorize step — so the per-vertex pass has to
// populate those even when the rest of the frame is going through the
// G-buffer compositor.
// Per-mesh body of Lighting(), extracted so the per-frame pass can fan
// meshes across the thread pool: each mesh's writes are its own Verts'
// L{R,G,B,A} — disjoint per task — and every read (omnis, materials,
// scene ambient) is stable for the duration of the pass. The scratch
// that used to be function-static (the CurLight array) is thread_local;
// the tiny Color temporaries live on the stack. Math is untouched, so
// parallel == serial byte-for-byte per vertex.
static void LightMeshVerts(Scene *Sc, TriMesh *T)
{
	Vertex *V, *VE;
	Omni *O;

	Vector u, w;

	Material *Mat;
	float Lumin;

	Color plStorage, *pl = &plStorage;
	Color Ambient;

	// light position vector
	static thread_local CurLight LA[128];
	CurLight *L, *LE;

	{
		if (T->FIndex)
		{
			Mat = T->Faces[0].Txtr;
			Lumin = Mat->Luminosity;
		}
		else return;   // no faces → no material; the old code read a stale
		               // Mat pointer here (latent UB), never hit in practice.


		// Calculate ambient factor
		// LW3D: Luminosity * base color + scene Ambient * diffuse level
		if (Mat->Txtr)
		{
			Ambient.B = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.R;
			Ambient.A = Lumin * 255.0 + Mat->Diffuse * Sc->Ambient.A;
		} else {
			Ambient.B = Lumin * Mat->BaseCol.B + Mat->Diffuse * Sc->Ambient.B;
			Ambient.G = Lumin * Mat->BaseCol.G + Mat->Diffuse * Sc->Ambient.G;
			Ambient.R = Lumin * Mat->BaseCol.R + Mat->Diffuse * Sc->Ambient.R;
			Ambient.A = Lumin * Mat->BaseCol.A + Mat->Diffuse * Sc->Ambient.A;

		}

		for (O = Sc->OmniHead, L = LA; O; O = O->Next)
		{
			if (!(O->Flags & Omni_Active))
				continue;
			// See the stationary loop above — clones are deferred-only.
			if (O->Flags & Omni_MirrorClone)
				continue;
			if ((T->Flags & Tri_Stationary) && (O->Flags & Omni_Stationary))
				continue;
			Vector_Sub(&O->IPos, &T->IPos, &u);
			MatrixTXVector(T->RotMat, &u, &w);
			Vector *wp = (Vector *)(T->RotMat);
			// INVERSE scale, matching StaticLighting: RotMat rows are
			// s·unit, so MatrixTX gives s²·v_local — scaling by (1/s)²
			// recovers the light offset in mesh-local units, and
			// Range/s converts world range likewise. The non-inverted
			// form (previously live here, inverse commented out)
			// inflated distances by s⁴ vs a range of only s·R — on any
			// scaled mesh every omni failed the range test and the
			// mesh went ambient-only. Invisible for years because the
			// deferred path ignores vertex colors; surfaced as the
			// black robot in the mirror-RTT pass (scale-1 wall chunks
			// were unaffected, so the walls stayed bright).
			float rScale = 1.0f / Vector_Length(wp);
			Vector_SelfScale(&w, rScale*rScale);
			Vector_Copy(&L->Pos, &w);

			L->Range = O->IRange * rScale;
			L->Range2 = L->Range * L->Range;
			L->rRange = 1.0/L->Range;

			// bounding sphere check
			Vector_Sub(&L->Pos, &T->BSphereCtr, &u);
			if (T->BSphereRadius + L->Range < Vector_Length(&u))
				continue;

			// Spot params (mirrors the other Lighting() loop above).
			L->isSpot = (O->Type == Light_SpotLight) ? 1 : 0;
			if (L->isSpot) {
				MatrixTXVector(T->RotMat, (Vector*)&O->IDir, &L->Dir);
				Vector_Norm(&L->Dir);
				L->cosInner = O->HotSpot;
				L->cosOuter = O->FallOff;
			} else {
				L->Dir.x = L->Dir.y = L->Dir.z = 0.0f;
				L->cosInner = -2.0f;
				L->cosOuter = -2.0f;
			}

			if (Mat->Txtr)
			{
				float intensity = O->ISize * Mat->Diffuse;
				L->Col.B = O->L.B * intensity;
				L->Col.G = O->L.G * intensity;
				L->Col.R = O->L.R * intensity;
				L->Col.A = O->L.A * intensity;
			}
			else {
				float intensity = O->ISize * Mat->Diffuse / 256.0;
				L->Col.B = O->L.B * Mat->BaseCol.B * intensity;
				L->Col.G = O->L.G * Mat->BaseCol.G * intensity;
				L->Col.R = O->L.R * Mat->BaseCol.R * intensity;
				L->Col.A = O->L.A * Mat->BaseCol.A * intensity;

			}
			L++;
		}
		LE = L;

		bool stat = false;
		if (T->Flags & Tri_Stationary)
			stat = true;

		Color *sl = T->SL;
		for (V = T->Verts, VE = V + T->VIndex; V < VE; V++, sl++)
		{
			// this compare isn't so bad as soon as it gets into the BTB.
			if (stat)
			{
				pl->B = sl->B;
				pl->G = sl->G;
				pl->R = sl->R;
				pl->A = sl->A;
			} else {
				pl->B = Ambient.B;
				pl->G = Ambient.G;
				pl->R = Ambient.R;
				pl->A = Ambient.A;
			}

			for (L = LA; L < LE; L++)
			{
				Vector_Sub(&L->Pos, &V->Pos, &w);
				float dot = Dot_Product(&w, &V->N);
				if (dot < 0.0) continue;
				float v = Vector_Length(&w);
				float len2 = w.x*w.x + w.y*w.y + w.z*w.z;
				if (len2 > L->Range2) continue;
				/*				float len = SQRT(len2);
								float k = dot / len * (1.0-len*L->rRange);*/

				float len = RSQRT(len2);
				float k = dot * len * (1.0-L->rRange/len);

				// Spot cone — same shape as the other Lighting() loop. RSQRT
				// here returns 1/sqrt, so `len` is actually the inverse
				// length. cosTheta = -dot(L->Dir, w) * (1/|w|).
				if (L->isSpot) {
					float cosTheta = -(L->Dir.x*w.x + L->Dir.y*w.y + L->Dir.z*w.z) * len;
					if (cosTheta <= L->cosOuter) continue;
					if (cosTheta < L->cosInner) {
						float t = (cosTheta - L->cosOuter) / (L->cosInner - L->cosOuter);
						k *= t * t * (3.0f - 2.0f * t);
					}
				}

				pl->B += k*L->Col.B;
				pl->G += k*L->Col.G;
				pl->R += k*L->Col.R;
				pl->A += k*L->Col.A;

			}

			// saturation
			if (pl->B > 250.0) pl->B = 250.0;
			if (pl->G > 250.0) pl->G = 250.0;
			if (pl->R > 250.0) pl->R = 250.0;
			if (pl->A > 250.0) pl->A = 250.0;

			V->LB = pl->B;
			V->LG = pl->G;
			V->LR = pl->R;
			V->LA = pl->A;
		}
	}
}

void Lighting(Scene *Sc)
{
	if (!(Sc->Flags & Scn_StaticLighting))
	{
		Sc->Flags |= Scn_StaticLighting;
		StaticLighting(Sc);
	}

	// Fan the per-mesh work across the pool (the pool is parked during the
	// scene tick, so this is free parallelism — measured ~1 ms serial on
	// greets). --no-vertex_light_parallel restores the serial walk.
	if (fds::FeatureFlags::vertex_light_parallel())
	{
		// Chunked fan: greets is many SMALL meshes (wall chunks), so a
		// task per mesh drowned in enqueue/semaphore overhead (measured
		// 1.27 ms vs 0.83 serial). Gather once, stride across a fixed
		// task count instead — 12 enqueues total, meshes interleaved so
		// the big mech parts spread across tasks.
		static std::vector<TriMesh*> sMeshes;   // tick-thread only
		sMeshes.clear();
		for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next)
		{
			if (T->Flags & (Tri_Invisible | Tri_Noshading)) continue;
			sMeshes.push_back(T);
		}
		const int nTasks = std::min<int>(12, (int)sMeshes.size());
		if (nTasks > 1)
		{
			static std::counting_semaphore<INT_MAX> sDone{0};
			TriMesh **meshes = sMeshes.data();
			const int count = (int)sMeshes.size();
			for (int k = 0; k < nTasks; ++k)
			{
				ThreadPool::instance().enqueue([Sc, meshes, count, k, nTasks]() {
					for (int i = k; i < count; i += nTasks)
						LightMeshVerts(Sc, meshes[i]);
					sDone.release();
				});
			}
			for (int i = 0; i < nTasks; ++i) sDone.acquire();
		}
		else
		{
			for (TriMesh *T : sMeshes) LightMeshVerts(Sc, T);
		}
		return;
	}

	for (TriMesh *T = Sc->TriMeshHead; T; T = T->Next)
	{
		if (T->Flags & (Tri_Invisible | Tri_Noshading)) continue;
		LightMeshVerts(Sc, T);
	}
}
