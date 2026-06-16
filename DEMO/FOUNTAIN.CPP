#include "Rev.h"
#include <Base/RenderStats.h>
#include <Threads.h>
#include <Base/RenderStats.h>
#include <Base/FeatureFlags.h>
#include <FILLERS/TheOtherBarry.h>
#include "FrameProfiler.h"

#include "FOUNTAIN.H"
#include "VESA/Vesa.h"
#include "Gradient.h"
#include "FRUSTRUM.H"
#include "Clipper.h"
#include "SceneTick.h"
#include "Scenes.h"
#include <memory>
#include <cstdlib>
#include <cstdio>
#include <vector>

namespace {
void dbg_write_ppm(const char* path, const byte* bgra, int xres, int yres, int bpsl) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", xres, yres);
    std::vector<unsigned char> row(xres * 3);
    for (int y = 0; y < yres; ++y) {
        const dword* src = reinterpret_cast<const dword*>(bgra + y * bpsl);
        for (int x = 0; x < xres; ++x) {
            dword px = src[x];
            row[x * 3 + 0] = (px >> 16) & 0xFF;
            row[x * 3 + 1] = (px >>  8) & 0xFF;
            row[x * 3 + 2] = (px      ) & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[FNT-DUMP] wrote %s\n", path);
}
}


static Scene *FntSc;
static Vector FntHead(-0.053f,43.381f, 0.251f);
static Vector FntSrc(-0.672f,-3.957f,-0.721f);
//static Vector FntSrc(-0.672f,-3.957f,-0.721f);
static Vector FntSpring(-0.261f,85.106f,-0.159f);

static Vector LBSrc(0.0f,1000.0f,0.0f);
static Vector Pylon[6];

static float xForm1x = -0.5f;
static float xForm1z = (float)sqrt(3.0f)/2.0f;
static float xForm2x = -0.5f;
static float xForm2z = (float)sqrt(3.0f)/2.0f;

static Vector FntDir(0.000f, 1.000f, 0.000f);

const int32_t FNTPartTime = (int32_t)(50.00 * 100);
static Material *PclT;
static DWord NPMats;
static float SwCarry = 0.0f, SwCarry2 = 0.0f;

static Vector Vortex_Center(-0.053f,343.381f, 0.251f);

static int32_t g_LnLastTimer = -1;   // scrub tracking for strike triggers

static char MSGStr[160];

// Stream HEAD omnis: 9 lights, one per (track, rotIdx) of the inner-
// particle spiral arms. Position recomputed each frame from the same
// emit math the spawn loop uses. No particle tracking.
//
// (Earlier attempts at PARTICLE-tracking lights — water-spray follower
// omnis, inner mid/end-of-trail trackers — were dropped: pre-allocated
// particle slots get recycled on respawn, and any "track-this-slot"
// scheme either jumps wildly when slots recycle, or — with continuity
// heuristics — has multiple omnis converging on the same low-index slot
// over time. Particle::Generation was kept in case a future attempt
// wants to revisit; the current scene works without it.)
struct FollowerOmni {
    Omni*   omni;
    int32_t track;   // 0..2 = inner I-phase (matches inner-spawn switch)
    int32_t rotIdx;  // 0..2 = xForm rotation copy
};
static constexpr int kNumStreamHeadOmnis = 9;
static constexpr int kNumFollowerOmnis   = kNumStreamHeadOmnis;
static FollowerOmni g_FntFollowerOmnis[kNumFollowerOmnis];

// No-op filler for follower omnis: contribute light but don't draw a
// sprite. Render() walks Sc->OmniHead and calls O->F.Filler unconditionally
// for every omni in the visible Z range, so we MUST give it a valid
// function pointer (PREPROC sets one for FLD-loaded omnis but our
// programmatically appended ones miss that pass).
static void NopOmniFiller(Face*, Vertex**, dword, dword,
                          const fds::RenderTarget&,
                          const fds::CameraContext&) {}

// Allocate a new Omni, initialise with single-key splines so
// Animate_Objects produces stable IPos/ISize/IRange values, and append
// it to the end of the scene's OmniHead chain (preserving existing
// indices that other code walks via OmniHead->Next->...).
static Omni* AppendFountainOmni(Scene* Sc, const Vector& pos,
                                 float lr, float lg, float lb,
                                 float intensity, float range)
{
    Omni* O = (Omni*)getAlignedBlock(sizeof(Omni), 16);
    std::memset(O, 0, sizeof(Omni));
    O->IPos = pos;
    O->IRange = range;
    O->rRange = 1.0f / range;
    O->ISize = intensity;
    O->L.R = lr;
    O->L.G = lg;
    O->L.B = lb;
    O->L.A = 1.0f;
    O->Flags = Omni_Active | Omni_Stationary;
    // Single-key splines — Animate_Objects calls Spline_Calc on these
    // each frame, so they need at least one key to avoid OOB reads.
    auto initSingleKey = [](Spline& sp, float val) {
        sp.NumKeys = 1;
        sp.Keys = new SplineKey;
        std::memset(sp.Keys, 0, sizeof(SplineKey));
        sp.Keys[0].Pos.x = val;
        sp.Keys[0].Pos.y = val;
        sp.Keys[0].Pos.z = val;
        sp.Flags = 0;
        sp.CurKey = 0;
    };
    initSingleKey(O->Pos, 0.0f);
    O->Pos.Keys[0].Pos.x = pos.x;
    O->Pos.Keys[0].Pos.y = pos.y;
    O->Pos.Keys[0].Pos.z = pos.z;
    initSingleKey(O->Size, intensity);
    initSingleKey(O->Range, range);
    O->F.A = O->F.B = O->F.C = &O->V;
    O->F.Filler = NopOmniFiller;
    // Append to end of OmniHead chain.
    if (!Sc->OmniHead) {
        Sc->OmniHead = O;
    } else {
        Omni* tail = Sc->OmniHead;
        while (tail->Next) tail = tail->Next;
        tail->Next = O;
        O->Prev = tail;
    }
    return O;
}

static DWord Blaze[64*128];


// Multiple bolts can be alive at once (parallel strikes). Each slot holds one
// bolt for kBoltLife Timer units; a strike burst fills several slots.
struct LBolt { Vector src, targ; int32_t pow; int32_t until; };
static const int     kMaxBolts = 5;
static const int32_t kBoltLife = 16;          // Timer units a bolt stays lit
static LBolt g_bolts[kMaxBolts] = {};
const float basicSceneAmbient = 64.0;

//static Vector *Stars;

static Particle *LBoltPcls;

static Image PhotonPlasma;

static dword g_numPcls;

static Material* g_LnMat;

// Transient strike-flash light. A non-stationary Omni placed on the bolt for
// the flash frames, fed into the deferred light list before the fog pass — so
// the froxel in-scatter (and forward Lighting on geometry) lights up around
// the strike. Intensity rides the existing Ambient_Factor decay; position is
// set per strike. Idle (Omni_Active cleared) between strikes.
static Omni*  g_BoltFlashOmni = nullptr;
static Vector g_BoltFlashPos;
static float  g_BoltFlashInt = 0.0f;        // own envelope (faster than ambient)
// Peak/range/decay are CLI- + script-tunable: --bolt_flash_peak/_range/_decay.

void Particle_Rast(Vertex *A,Vertex *B,Vertex *C)
{
	byte *Pixel;
	int32_t I,J;
	if (A->PX>=1&&A->PX<=XRes-2&&A->PY>=1&&A->PY<=YRes_1-1)
	{
		//addative 32bit 5 putpixels (currently set on 1)
		Pixel = VPage+(((int)A->PX+YOffs[(int)A->PY])<<2);
		Pixel -= XRes<<2;
		J = (*Pixel)+A->LB;
		if (J>255) *Pixel++=255; else *Pixel++=J;
		J = (*Pixel)+A->LG;
		if (J>255) *Pixel++=255; else *Pixel++=J;
		J = (*Pixel)+A->LR;
		if (J>255) *Pixel++=255; else *Pixel++=J;
		Pixel += (XRes<<2)-7;
		J = (*Pixel)+A->LB;
		if (J>255) *Pixel++=255; else *Pixel++=J;
		J = (*Pixel)+A->LG;
		if (J>255) *Pixel++=255; else *Pixel++=J;
		J = (*Pixel)+A->LR;
		if (J>255) *Pixel++=255; else *Pixel++=J;
		Pixel++;
		J = (*Pixel)+((int)A->LB<<2);
		if (J>255) *Pixel++=255; else *Pixel++=J;
		J = (*Pixel)+((int)A->LG<<2);
		if (J>255) *Pixel++=255; else *Pixel++=J;
		J = (*Pixel)+((int)A->LR<<2);
		if (J>255) *Pixel++=255; else *Pixel++=J;
		Pixel++;
		J = (*Pixel)+A->LB;
    if (J>255) *Pixel++=255; else *Pixel++=J;
    J = (*Pixel)+A->LG;
		if (J>255) *Pixel++=255; else *Pixel++=J;
		J = (*Pixel)+A->LR;
		if (J>255) *Pixel++=255; else *Pixel++=J;
    Pixel += (XRes<<2)-7;
		J = (*Pixel)+A->LB;
    if (J>255) *Pixel++=255; else *Pixel++=J;
		J = (*Pixel)+A->LG;
		if (J>255) *Pixel++=255; else *Pixel++=J;
		J = (*Pixel)+A->LR;
		if (J>255) *Pixel++=255; else *Pixel++=J;
	}
}

/*void Run_Rain()
{
	Vector_
}*/


void Initialize_Particles(Scene *Sc)
{
	int32_t I,J;
	Image Img;

	Sc->NumOfParticles = FntInnerPcls*3 + FntOuterPcls + FntSpiralPcls;
	Sc->Pcl = new Particle[Sc->NumOfParticles];
	memset(Sc->Pcl,0,sizeof(Particle)*Sc->NumOfParticles);

	float typicalParticleSize = 30.0/384.0;
	dword typicalSpans = Fist(YRes * typicalParticleSize / 4.0);
	TBR_Init(Sc, Sc->NumOfParticles * typicalSpans);

	/*Omni *Om;
	I = 0;
	for(Om = FntSc->OmniHead;Om;Om=Om->Next) I++;
	NPMats = 3;//I;
	PclT = new Material[I];*/
/*	Om = FntSc->OmniHead;
	for(I=0;I<NPMats;I++)
	{
		PclT[I].Txtr = Om->F.Txtr->Txtr;
		Om=Om->Next;
	}*/
	NPMats = 2;
	//PclT = (Material *)getAlignedBlock(sizeof(Material) * NPMats, 16);
	PclT = getAlignedArray<Material>(2, 16);
	PclT[0].Txtr = new Texture;
	memset(PclT[0].Txtr, 0, sizeof(Texture));
	PclT[0].Txtr->BPP = 32;
	PclT[1].Txtr = new Texture;
	memset(PclT[1].Txtr, 0, sizeof(Texture));
	PclT[1].Txtr->BPP = 32;
	Img.x = 256;
	Img.y = 256;
	Img.Data = new DWord[65536];
	Generate_Flare_Image(&Img,0.2f,0.3f,1.0f);
	Convert_Image2Texture(&Img,PclT[0].Txtr);
	Generate_Flare_Image(&Img,0.1f,0.8f,1.0f);
	Convert_Image2Texture(&Img,PclT[1].Txtr);
	delete Img.Data;

	// override textures with 32x32
	for(I = 0; I < 2; I++)
	{
/*		Img.x = 256;
		Img.y = 256;
		Img.Data =  new dword [256*256];
		switch(I)
		{
		case 0:
//			Generate_Flare_Image(&Img,0.2f,0.3f,1.0f);
			Generate_Flare_Image(&Img,1.0f,1.0f,1.0f);
			break;
		case 1:
//			Generate_Flare_Image(&Img,0.1f,0.8f,1.0f);
			Generate_Flare_Image(&Img,1.0f,1.0f,1.0f);
			break;
		}

		Scale_Image(&Img, 32, 32);*/

//		Img.x = 32;
//		Img.y = 32;
//		Img.Data =  new dword [32*32];
		New_Image(&Img, 32, 32);

		dword x,y;
		float fx,fy,dx = 2.0/32.0,dy =-2.0/32.0;
		for(x = 0,fx=-1.0; x<32; x++,fx+=dx)
		{
			for(y = 0,fy=1.0; y<32; y++,fy+=dy)
			{
				//Img.Data[x+32*y] = (x<<3)+(y<<11);
				if (fx*fx+fy*fy>=1.0)
					Img.Data[x+32*y] = 0;
				else
					Img.Data[x+32*y] = 0x010101 * (dword)((1.0-(fx*fx+fy*fy))*255.0);
			}
		}

		PclT[I].Txtr->Data = (byte *)Img.Data;
		// The Convert_Image2Texture calls above stamped SizeX/SizeY=256;
		// this override swaps in a 32×32 buffer — fix the metadata to
		// match or anything that trusts it (the TBR sprite-res dispatch)
		// samples 64× past the allocation.
		PclT[I].Txtr->SizeX = 32; PclT[I].Txtr->LSizeX = 5;
		PclT[I].Txtr->SizeY = 32; PclT[I].Txtr->LSizeY = 5;
	}
	


	// Install Particles

	// OUTER (WATER SPRAY) particles
	union {
		float f;
		dword dw;
	} pclsize;
	for(I=0;I<FntOuterPcls;I++)
	{
		// Physical Quantities.
		Sc->Pcl[I].Grav = 7.5;
		Sc->Pcl[I].Mass = 0.02;
		Sc->Pcl[I].Charge = 6.5;
		Sc->Pcl[I].Radius = 0.0;
		// Particle Position.
		Sc->Pcl[I].V.Pos.x=FntHead.x;
		Sc->Pcl[I].V.Pos.y=FntHead.y;
		Sc->Pcl[I].V.Pos.z=FntHead.z;
		// Converegance Vectors set at starting position.
//		Vector_Copy(&Sc->Pcl[I].Conv,&Sc->Pcl[I].V.Pos);
		// Particle Virtual Face -> Particle Vertex		
		Sc->Pcl[I].F.FlareSize = 0.06f * 20.0 / 10.0; // indicates this isn't an omni flare polygon.
		Sc->Pcl[I].F.A = Sc->Pcl[I].F.B = &Sc->Pcl[I].V;
		// manual assignment
		Sc->Pcl[I].F.Filler = TBR_Sprite;
		Sc->Pcl[I].F.Txtr = PclT+I%NPMats;

		// Particle Velocity
//		F1=(float)RAND_15()/32768.0;
//		F2=(float)RAND_15()/4096.0;
//		Sc->Pcl[I].Vel.x=(2.0+F2)*cos(F1);
//	Sc->Pcl[I].Vel.y=(0.5+F2*0.5)*sin(F1);
//		Sc->Pcl[I].Vel.z=1.0*cos(F1);
//		Vector_SelfScale(&Sc->Pcl[I].Vel,0.05);
		Sc->Pcl[I].Vel.x=0;
		Sc->Pcl[I].Vel.y=0;
		Sc->Pcl[I].Vel.z=0;
	}

	/// INNER (RING) particles
	for(I=FntOuterPcls;I<FntSpiralPclsOff;I++)
	{
		// Physical Quantities.
		Sc->Pcl[I].Grav = 7.5;
		Sc->Pcl[I].Mass = 0.02;
		Sc->Pcl[I].Charge = 3.5;
		Sc->Pcl[I].Radius = 0.0;
		// Particle Position.
		Sc->Pcl[I].V.Pos.x=FntHead.x;
		Sc->Pcl[I].V.Pos.y=FntHead.y-3.0f;
		Sc->Pcl[I].V.Pos.z=FntHead.z;
		// Converegance Vectors set at starting position.
//		Vector_Copy(&Sc->Pcl[I].Conv,&Sc->Pcl[I].V.Pos);
		// Particle Virtual Face -> Particle Vertex
		Sc->Pcl[I].F.FlareSize = 0.07f * 20 / 10; // indicates this isn't an omni flare polygon.
		Sc->Pcl[I].F.A = Sc->Pcl[I].F.B = &Sc->Pcl[I].V;
		// manual assignment
		Sc->Pcl[I].F.Filler = TBR_Sprite;

		// Particle Velocity
//		F1=(float)RAND_15()/32768.0;
//		F2=(float)RAND_15()/4096.0;
//		Sc->Pcl[I].Vel.x=(2.0+F2)*cos(F1);
//	Sc->Pcl[I].Vel.y=(0.5+F2*0.5)*sin(F1);
//		Sc->Pcl[I].Vel.z=1.0*cos(F1);
//		Vector_SelfScale(&Sc->Pcl[I].Vel,0.05);
		Sc->Pcl[I].F.Txtr = PclT+I%NPMats;

		Sc->Pcl[I].Vel.x=0;
		Sc->Pcl[I].Vel.y=0;
		Sc->Pcl[I].Vel.z=0;
		
	}
	for(I=FntSpiralPclsOff;I<Sc->NumOfParticles;I++)
	{
		// Physical Quantities.
		Sc->Pcl[I].Grav = 7.5;
		Sc->Pcl[I].Mass = 0.02;
		Sc->Pcl[I].Charge = 3.5;
		Sc->Pcl[I].Radius = 0.0;
		// Particle Position.
		Sc->Pcl[I].V.Pos.x=FntHead.x;
		Sc->Pcl[I].V.Pos.y=FntHead.y-3.0f;
		Sc->Pcl[I].V.Pos.z=FntHead.z;
		// Converegance Vectors set at starting position.
//		Vector_Copy(&Sc->Pcl[I].Conv,&Sc->Pcl[I].V.Pos);
		// Particle Virtual Face -> Particle Vertex
		Sc->Pcl[I].F.FlareSize = 0.07f * 20 / 10; // indicates this isn't an omni flare polygon.
		Sc->Pcl[I].F.A=Sc->Pcl[I].F.B = &Sc->Pcl[I].V;

		// manual assignment
		Sc->Pcl[I].F.Filler = TBR_Sprite;

		// Particle Velocity
//		F1=(float)RAND_15()/32768.0;
//		F2=(float)RAND_15()/4096.0;
//		Sc->Pcl[I].Vel.x=(2.0+F2)*cos(F1);
//	Sc->Pcl[I].Vel.y=(0.5+F2*0.5)*sin(F1);
//		Sc->Pcl[I].Vel.z=1.0*cos(F1);
//		Vector_SelfScale(&Sc->Pcl[I].Vel,0.05);
		Sc->Pcl[I].F.Txtr = PclT+I%NPMats;

		Sc->Pcl[I].Vel.x=0;
		Sc->Pcl[I].Vel.y=0;
		Sc->Pcl[I].Vel.z=0;
	}
}


void Particle_Kinematics(Scene *Sc)
{
	const float intensity = 0.8;
	float u,v,cv;
	// Destroy particles
	Particle *P,*PE;
	Vector V;
	Vector Omit,OmitDer,X,Y;

	float dt = 0.01*dTime;
	float expdt = exp(dt*0.4f);
	float x;

	float magwav = 55.0f;//75.0f + 20.0f*cos(Timer*0.005);

	int32_t I,r;

	g_numPcls = 0;
	for(P=Sc->Pcl,PE = P + FntOuterPcls;P<PE;P++)
	{
		if (!P->Flags) continue;
		if (P->V.Pos.y<0.0f) P->Flags = 0; //kill pcl
		g_numPcls++;
	}



	// Create random particles
	float Sw = dt * FntSpawnOutPcl + SwCarry;
	float pow = FntSpawnOutPow + sin(Timer*FntSpawnOutPowFreq)*FntSpawnOutPowTilt,fpow;
	int32_t Spwn = Sw;
	SwCarry = Sw-Spwn;

//	Spwn = 0;
	while(Spwn--)
	{
		fpow = pow + (float)RAND_15()/32768.0f * FntSpawnOutPowRand;
		// search for a place to insert
		for(P=Sc->Pcl;P<PE;P++)
			if (!P->Flags) break;
		if (P==PE)
			break;
		//insert
		fpow *= 0.6;
		Vector_Copy(&P->V.Pos,&FntSpring);
		// Compute jitter into explicit locals before adding. The fully-
		// inline form `Pos.x += ((float)(RAND_15()-16384)/16384.0f)*Disp;`
		// produced a 262143-corruption pattern under ASan+Debug — the
		// generated code somehow stored ~2^18 into Pos.x/z instead of
		// the bounded ±1 jitter. Explicit locals sidestep it.
		{
			const int r_x = RAND_15();
			const float jitter_x = ((float)(r_x - 16384) / 16384.0f) * FntSpawnOutDisp;
			P->V.Pos.x += jitter_x;
			const int r_z = RAND_15();
			const float jitter_z = ((float)(r_z - 16384) / 16384.0f) * FntSpawnOutDisp;
			P->V.Pos.z += jitter_z;
		}
		P->V.Pos.y -= 20.0;

		u = ((float)RAND_15()/32768.0f)*PI_M2;
		v = ((float)RAND_15()/32768.0f)*PI*1.0f+PI*0.6f;
		cv = cos(v);
		P->Vel.x = cv * cos(u) * fpow;
		P->Vel.y = sin(v) * fpow;
		P->Vel.z = cv * sin(u) * fpow;

//		P->Color.R = 63;
//		P->Color.G = 127;
//		P->Color.B = 255;
		float colorscale = 0.3 * intensity;
		P->Color.R = 63*colorscale;
		P->Color.G = 127*colorscale;
		P->Color.B = 255*colorscale;

		++P->Generation;
		P->Flags = Particle_Active;
	}
	// Animate Particles
	for(P=Sc->Pcl;P<PE;P++)
	{
		if (!P->Flags) continue;
		if (P->Flags&2)
		{
			P->Vel.y -= dt * FntSpawnOutGrav;
			Vector_Scale(&P->Vel,dt,&V);
			Vector_SelfAdd(&P->V.Pos,&V);
		} else {
			if (P->V.Pos.y>83.106) P->Flags|=2;
			P->V.Pos.y += Vector_Length(&P->Vel);
		}

		Vector_Sub(&P->V.Pos,&View->ISource,&V);
		MatrixXVector(View->Mat,&V,&P->V.TPos_AOS);

		if (P->V.TPos_AOS.z>=1.0)
		{
			P->V.RZ = 1.0/P->V.TPos_AOS.z;
			P->V.PX = CntrX+FOVX*P->V.TPos_AOS.x*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos_AOS.y*P->V.RZ;
//      P->PRad = 0;//FOVX*P->Radius*RZ;
			P->V.Flags = 0;
		} else P->V.Flags |= Vtx_VisNear;

		// dummy illumination model
		P->V.LB = P->Color.B;
		P->V.LG = P->Color.G;
		P->V.LR = P->Color.R;

	}

	/// INNER PARTICLES
	for(PE += FntInnerPcls*3;P<PE;P++)
	{
		if (!P->Flags) continue;
		g_numPcls++;
		P->Charge-=dt;
		if (P->Charge<0.0f) P->Flags = 0; //kill pcl
	}

	// Create random particles
	float Sw2 = dt * FntSpawnInnerPcl + SwCarry2;
//	float pow = FntSpawnOutPow + sin(Timer*FntSpawnOutPowFreq)*FntSpawnOutPowTilt,fpow;
	int32_t Spwn2 = Sw2;
	SwCarry2 = Sw2-Spwn2;

	for(I=0;I<3;I++)
	{

		const float angularVelocity = 0.008;

		// now, fire particles at random angles away
		// polar base
//		Vector_Norm(&OmitDer);
//		X.x = OmitDer.z;
//		X.y = 0.0;
//		X.z =-OmitDer.x;
//		x = 1.0/sqrt(X.x*X.x+X.z*X.z);
//		X.x *= x;
//		X.z *= x;
//		Cross_Product(&OmitDer,&X,&Y);

//		Spwn = 12;
//		Spwn = 0;
		Spwn = Spwn2*3;

		while(Spwn--)
		{
			// search for a place to insert
			for(P=Sc->Pcl+FntOuterPcls+I*FntInnerPcls,PE=P+FntInnerPcls;P<PE;P++)
				if (!P->Flags) break;
			if (P==PE) 
				break;
			//insert

			// temporal antialiasing
			float rtime = Timer - dt*100.0*RAND_15()/32768.0;
			switch(I)
			{
				case 0:
					// omit particles from base altitude 10 radius 30
					OmitDer.x = cos(rtime*angularVelocity*0.8);
					OmitDer.y = cos(rtime*0.01*0.8)*0.3;
					OmitDer.z = sin(rtime*angularVelocity*0.8);

					Omit.x = OmitDer.x*magwav;
					Omit.y = 35.0f+OmitDer.y*55.0f;
					Omit.z = OmitDer.z*magwav;
				break;
				case 1:
					OmitDer.x = cos(rtime*angularVelocity*0.8);
					OmitDer.y = cos(rtime*0.01*0.8+2.0*PI/3.0f)*0.3;
					OmitDer.z = sin(rtime*angularVelocity*0.8);

					Omit.x = OmitDer.x*magwav;
					Omit.y = 35.0f+OmitDer.y*55.0f;
					Omit.z = OmitDer.z*magwav;
				break;
				case 2:
					OmitDer.x = cos(rtime*angularVelocity*0.8);
					OmitDer.y = cos(rtime*0.01*0.8+4.0f*PI/3.0f)*0.3;
					OmitDer.z = sin(rtime*angularVelocity*0.8);

					Omit.x = OmitDer.x*magwav;
					Omit.y = 35.0f+OmitDer.y*55.0f;
					Omit.z = OmitDer.z*magwav;
				break;
			}

			Vector_Copy(&P->V.Pos,&Omit);

			u = ((float)RAND_15()/32768.0f)*PI_M2;
			v = ((float)RAND_15()/32768.0f)*PI-PI_D2;
			cv = cos(v);
			P->Vel.x = cv * cos(u) * 4.5;
			P->Vel.y = sin(v) * 4.5;
			P->Vel.z = cv * sin(u) * 4.5;
			P->Charge = 2.5f;

			switch(I)
			{
				case 0:
					P->Color.B = 255;
					P->Color.G = 128;
					P->Color.R = 48;
				break;
				case 1:
					P->Color.B = 80;
					P->Color.G = 255;
					P->Color.R = 80;
				break;
				case 2:
					P->Color.B = 48;
					P->Color.G = 128;
					P->Color.R = 255;
				break;
			}

			P->Color.R *= intensity;
			P->Color.G *= intensity;
			P->Color.B *= intensity;

			switch(Spwn%3)
			{
				case 0: break;
				case 1:
					x = P->V.Pos.x * xForm1x + P->V.Pos.z * xForm1z;
					P->V.Pos.z = P->V.Pos.z * xForm1x - P->V.Pos.x * xForm1z;
					P->V.Pos.x = x;

					x = P->Vel.x * xForm1x + P->Vel.z * xForm1z;
					P->Vel.z = P->Vel.z * xForm1x - P->Vel.x * xForm1z;
					P->Vel.x = x;

				case 2:
					x = P->V.Pos.x * xForm2x + P->V.Pos.z * xForm2z;
					P->V.Pos.z = P->V.Pos.z * xForm2x - P->V.Pos.x * xForm2z;
					P->V.Pos.x = x;

					x = P->Vel.x * xForm2x + P->Vel.z * xForm2z;
					P->Vel.z = P->Vel.z * xForm2x - P->Vel.x * xForm2z;
					P->Vel.x = x;
				break;
			}

			++P->Generation;
			P->Flags = Particle_Active;
		}
	}

	for(P=Sc->Pcl+FntOuterPcls,PE=P+FntInnerPcls*3;P<PE;P++)
	{
		if (!P->Flags) continue;
		Vector_Scale(&P->Vel,dt,&V);
		Vector_SelfAdd(&P->V.Pos,&V);

		Vector_Sub(&P->V.Pos,&View->ISource,&V);
		MatrixXVector(View->Mat,&V,&P->V.TPos_AOS);

		if (P->V.TPos_AOS.z>=1.0)
		{
			P->V.RZ = 1.0/P->V.TPos_AOS.z;
			P->V.PX = CntrX+FOVX*P->V.TPos_AOS.x*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos_AOS.y*P->V.RZ;
//      P->PRad = 0;//FOVX*P->Radius*RZ;
			P->V.Flags = 0;
		} else P->V.Flags |= Vtx_VisNear;

		// dummy illumination model
		P->V.LB = P->Color.B*P->Charge*0.7/3.5;
		P->V.LG = P->Color.G*P->Charge*0.7/3.5;
		P->V.LR = P->Color.R*P->Charge*0.7/3.5;

	}

/*	if (Timer>FntVortexSwarm)
	{
		P = FntSc->Pcl + FntSpiralPclsOff;
		PE = P + FntSpiralPcls;
		for(;P<PE;P++)
		{
			if (!P->Flags) continue;
			//pcl destruction
			if (P->V.Pos.y>Vortex_Center.y)
				P->Flags = 0;
		}


		Spwn = 10;
		while (Spwn--)
		{
			P = FntSc->Pcl + FntSpiralPclsOff;
			for(;P<PE;P++)
			{
				if (!P->Flags) break;
			}
			if (P==PE) break;
			// load pcl with stuff
			P->Color.R = 63 + (RAND_15()>>9);
			P->Color.G = 127 + (RAND_15()>>8);
			P->Color.B = 255;

			Vector_Copy(&P->V.Pos,&Vortex_Center);
			P->V.Pos.y = FntHead.y;

			r = RAND_15()>>5;
			if (r&0xFF>=0x80) r -= 0x80;
			x = r * PI_M2 / 1024.0f;
			u = (1.0f+2.0f*RAND_15()/32768.0f);
			P->V.Pos.x += cos(x)*u;
			P->V.Pos.z += sin(x)*u;

			P->Vel.x = 0.0f;
			P->Vel.y = 0.0f;
			P->Vel.z = 0.0f;
			P->Flags = 1;
		}

		float dcx = expdt*cos(dt*1.0f),dsx = expdt*sin(dt*1.0f),tx,ty;
		for(P = FntSc->Pcl + FntSpiralPclsOff;P<PE;P++)
		{
			if (!P->Flags) continue;
//			Vector_Scale(&P->Vel,dt,&V);
//			Vector_SelfAdd(&P->V.Pos,&V);
			P->V.Pos.y += dt*25.0f;
			tx = P->V.Pos.x - Vortex_Center.x;
			ty = P->V.Pos.z - Vortex_Center.z;

			P->V.Pos.x = dcx * tx + dsx * ty + Vortex_Center.x;
			P->V.Pos.z = dcx * ty - dsx * tx + Vortex_Center.z;

			Vector_Sub(&P->V.Pos,&View->ISource,&V);
			MatrixXVector(View->Mat,&V,&P->V.TPos_AOS);

			if (P->V.TPos_AOS.z>=1.0)
			{
				P->V.RZ = 1.0/P->V.TPos_AOS.z;
				P->V.PX = CntrX+FOVX*P->V.TPos_AOS.x*P->V.RZ;
				P->V.PY = CntrY-FOVY*P->V.TPos_AOS.y*P->V.RZ;
//      P->PRad = 0;//FOVX*P->Radius*RZ;
				P->V.Flags = 0;
			} else P->V.Flags |= Vtx_VisNear;

			// dummy illumination model
			P->V.LB = P->Color.B*0.1f;
			P->V.LG = P->Color.G*0.1f;
			P->V.LR = P->Color.R*0.1f;
		}
	}*/

}

struct Four_C
{
	uint8_t R,G,B,A;
};
union FCDW
{
	Four_C C;
	DWord DW;
};

void Generate_Photon_Plasma()
{
	Image *Img = &PhotonPlasma;
//	New_Image(Img,64,128);
	Img->x = 64; Img->y = 128;
	Img->Data = (DWord *)Blaze;
	memset(Img->Data,0,64*128*4);
	Generate_Fractal_Noise(Img,4.0);
	int32_t I,J,K;
	FCDW FCD;
	DWord *P = Img->Data;
	int32_t HX = Img->x>>1;

	 for(J=0;J<Img->y;J++)
		 for(I=0;I<Img->x;I++)
		 {
			 FCD.DW = *P;
			 K = FCD.C.R*0.5*(HX-abs(I-HX))/HX;
			 if (K>255) FCD.C.R=255; else FCD.C.R=K;
			 K = FCD.C.G*1.0*(HX-abs(I-HX))/HX;
			 if (K>255) FCD.C.G=255; else FCD.C.G=K;
			 K = FCD.C.B*2.0*(HX-abs(I-HX))/HX;
			 if (K>255) FCD.C.B=255; else FCD.C.B=K;
			 *P++ = FCD.DW;
		}
}

namespace { void ensureBoltTextures(); }   // defined below; baked at load (see end of init)

extern TriMesh VortexTri;   // defined below; filler swapped to alpha-test post-Preprocess

void Initialize_Fountain()
{
//	Stars = new Vector [200];

	Material *M;
	FntSc = loadSceneAligned("SCENES/FOUNTAIN.FLD");

	// Particle-heavy with mostly matte fountain surfaces — OuterVec
	// wins on the non-spec majority of pixels.
	FntSc->PreferOuterVec = 1;

	for(M = MatLib;M;M=M->Next)
	{
		if (M->RelScene != FntSc) continue;
		M->Flags |= Mat_RGBInterp;
		// Spire orbs (outer "f_sphere" / inner "f in shpere") ship from
		// the FLD as single-sided transparent. With the 2-deep xpar
		// G-buffer the back-facing half is what fills the back layer —
		// without TwoSided those back-facing tris get culled before
		// reaching the deferred dispatch, so the back layer stays empty
		// and the orbs render as a thin shell. Forcing TwoSided lets
		// both halves through and the orbs look like full glass spheres.
		if (M->Name && (std::strcmp(M->Name, "f_sphere") == 0 ||
		                std::strstr(M->Name, "in shpere"))) {
			M->Flags |= Mat_TwoSided;
		}
		// Center "mizraka glass" sphere: crystal-like.
		//   XparBlendAlpha=0.4 → 40% source + 60% destination, linear
		//     no-cap, more see-through than the default saturated formula.
		//   Specular=1.0 + Glossiness=128 → sharp Blinn-Phong highlight
		//     in the xpar compositor for a glassy/crystal look.
		// Spire orbs (f_sphere / in shpere) keep XparBlendAlpha=0 and
		// Specular=0 from the FLD — legacy formula, no highlight.
		if (M->Name && std::strcmp(M->Name, "mizraka glass") == 0) {
			M->XparBlendAlpha = 0.4f;
			M->Specular   = 1.0f;
			M->Glossiness = 128;
		}
	}
	Add_Vortex_ToScene();

	FntSc->Ambient.B = 64;
	FntSc->Ambient.G = 64;
	FntSc->Ambient.R = 64;
	FntSc->Ambient.A = 64;
	FntSc->NZP = 20.0f;
	FntSc->FZP = 5000.0f;
	FntSc->Flags |= Scn_SpriteTBR;

	tagSceneMaterials(FntSc, /*restrictToScene=*/true);

	Initialize_Particles(FntSc);
	Preprocess_Scene(FntSc, "FOUNTAIN");

	// The portal/vortex is an additive sky glow on a square quad whose texture
	// is transparent (black) outside the swirl. Assign_Fillers gave it the plain
	// additive filler, which stamps Z across the whole quad — and that truncates
	// the volumetric fog integral behind the transparent texels, leaving a dark
	// square framing the disc. Swap in the alpha-testing additive filler: it
	// discards texels whose baked alpha is 0 (the circle-clipped corners — see
	// Vortex_Distort) with no colour and no Z, so the fog integrates through the
	// corners, while the opaque swirl stamps Z and occludes the ship.
	for (int32_t fi = 0; fi < VortexTri.FIndex; ++fi)
		VortexTri.Faces[fi].Filler =
			TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL, true, true>;

	struct TrackProfile { float r, g, b; };
	const TrackProfile trackColors[3] = {
		{0.20f, 0.55f, 1.00f},   // track 0 — blue
		{0.30f, 1.00f, 0.30f},   // track 1 — green
		{1.00f, 0.50f, 0.20f},   // track 2 — red/orange
	};

	// 9 stream HEAD lights — one per (track, rotIdx) of the visible
	// spiral arms. Position recomputed each tick from spawn emit math.
	{
		const float intensity = 400.0f;
		const float range     = 50.0f;
		for (int i = 0; i < kNumStreamHeadOmnis; ++i) {
			const int track  = i / 3;
			const int rotIdx = i % 3;
			const auto& c    = trackColors[track];
			g_FntFollowerOmnis[i].track  = track;
			g_FntFollowerOmnis[i].rotIdx = rotIdx;
			g_FntFollowerOmnis[i].omni = AppendFountainOmni(
				FntSc, FntHead, c.r, c.g, c.b, intensity, range);
		}
	}

	Omni *O = FntSc->OmniHead->Next->Next;

	int32_t I;
	Quaternion *W;
	for(I=0;I<6;I++)
	{
		W = &O->Pos.Keys->Pos;
		Vector_Form(Pylon+I,W->x,W->y,W->z);
		O=O->Next;
		if (!O) break;
	}

	LBoltPcls = new Particle[FntLightningPcls];

	// Bolt cross-section. evalGradient FOLDS u (1-|u-0.5|*2), so the
	// endpoint table is the HALF-profile edge(0) → core(1) and the quad
	// center automatically gets the core. Edges are black because the
	// faces draw ADDITIVE (a bolt is light): black adds nothing = free
	// soft AA. Color order is {B,G,R,A}: classic electric lightning runs
	// black → deep blue → blue → cyan-white → white-hot core.
	std::vector<GradientEndpoint> endpoints;
	endpoints.emplace_back(0.00, Color{ 0.0, 0.0,  0.0,  0.0 });
	endpoints.emplace_back(0.40, Color{ 0.7,  0.12, 0.02, 0.0 });  // blue
	endpoints.emplace_back(0.66, Color{ 1.0,  0.55, 0.25, 0.0 });  // bright blue
	endpoints.emplace_back(0.80, Color{ 1.0,  0.92, 0.78, 0.0 });  // white-blue
	endpoints.emplace_back(0.88, Color{ 1.0,  1.0,  1.0,  0.0 });  // white-hot
	endpoints.emplace_back(1.00, Color{ 1.0,  1.0,  1.0,  0.0 });  // core

	g_LnMat = Generate_Gradient(endpoints, 256, 0.04, false);

	// Bake the bolt textures now, at scene load, instead of lazily on the
	// first strike — otherwise the first lightning visibly stalls the frame.
	ensureBoltTextures();

	// Transient strike-flash light, idle until Handle_Lightning pulses it.
	// Must be non-stationary: the per-frame Lighting loop skips stationary
	// omnis on static geometry (they're baked once by StaticLighting at ISize
	// 0), so the strike would never light the scene.
	g_BoltFlashOmni = AppendFountainOmni(FntSc, FntHead, 0.65f, 0.80f, 1.0f, 0.0f,
	                                     fds::FeatureFlags::bolt_flash_range());
	g_BoltFlashOmni->Flags &= ~(Omni_Active | Omni_Stationary);
	// Transient: the froxel fog adds it per-frame (not into history) so the
	// short flash doesn't linger / darken screen edges on a camera pan.
	g_BoltFlashOmni->Flags |= Omni_FogTransient;

	// Reset scrub tracking + any reserved strikes (re-init safe).
	g_LnLastTimer = -1;
	for (int i = 0; i < kMaxBolts; ++i) g_bolts[i].until = -1;

	// Runtime stress knob: FDS_FOUNTAIN_PARTICLE_OMNI_PCT (float in [0..100],
	// default 0). Attaches a stationary Omni to that percentage of fountain
	// particles, spread evenly across Sc->Pcl[]. Used to probe how the
	// deferred lighting kernel handles huge omni counts. Default 0 → no
	// extra omnis spawned, behaviour identical to before this knob existed.
	//
	// Hard cap below: forward Lighting() in RENDER.CPP uses a static LA[128]
	// per-vertex light buffer and overruns it once an *active* omni count
	// passes ~128 for any non-stationary trimesh in the scene. The deferred
	// path itself caps at DEFERRED_MAX_LIGHTS=128 too, so values beyond that
	// don't actually stress the deferred kernel further. We clamp the
	// stress-omni count to a safe budget that keeps total active omnis
	// (existing FLD + 9 stream-head followers + stress) below the LA[128]
	// limit with comfortable headroom.
	{
		float pct = fds::FeatureFlags::fountain_particle_omni_pct();
		if (pct > 100.0f) pct = 100.0f;
		int numAdded = 0;
		if (pct > 0.0f && FntSc->NumOfParticles > 0) {
			// Count omnis already in the chain (FLD-loaded + 9 stream-heads).
			int existingActive = 0;
			for (Omni* eo = FntSc->OmniHead; eo; eo = eo->Next) {
				if (eo->Flags & Omni_Active) ++existingActive;
			}
			constexpr int kForwardLA = 128;
			constexpr int kHeadroom  = 8;
			int budget = kForwardLA - existingActive - kHeadroom;
			if (budget < 0) budget = 0;

			// Desired count by pct.
			int desired = (int)(FntSc->NumOfParticles * (pct / 100.0f));
			if (desired < 1) desired = 1;

			int actual = desired;
			if (actual > budget) actual = budget;

			if (actual > 0) {
				// Match the 9 stream-head omnis (intensity=400) — scene omnis
				// use ISize≈127..400, so anything below ~100 is invisible
				// against the rasterizer's existing illumination. 400 lines
				// us up with the strategic particle-path lights overhead.
				const float omniIntensity = 400.0f;
				const float omniRange     = 75.0f;
				// Spread along the same helical spiral arc the 9 stream-head
				// omnis trace (radius=magwav=55, y in [35-0.3*55, 35+0.3*55]
				// = [18.5, 51.5], with 3 tracks rotated 120° around Y via
				// xForm1/xForm2). We stitch the 3 arms into one continuous
				// parametric curve over theta in [0, 6π): each arm covers
				// a 2π sweep, and across arms we lift y from bottom to top
				// of the visible range so the lights trace the visible
				// spiral instead of all stacking on the same ring.
				const float magwav = 55.0f;
				const float yMid   = 35.0f;
				const float yAmp   = 0.3f * 55.0f;  // matches yPhase scale
				const float kArms  = 3.0f;
				for (int i = 0; i < actual; ++i) {
					// u in [0,1) — fraction along the combined arc.
					const float u = (float)i / (float)actual;
					// Angle: kArms full turns end-to-end.
					const float theta = u * kArms * 2.0f * float(PI);
					// y rises linearly across the full helix.
					const float y = (yMid - yAmp) + 2.0f * yAmp * u;
					float x = std::cos(theta) * magwav;
					float z = std::sin(theta) * magwav;
					// Apply the same arm rotation the stream-head omnis use:
					// rotIdx selected so consecutive omnis cycle 0,1,2,0,1,2…
					// to populate all 3 visible arms.
					const int rotIdx = i % 3;
					if (rotIdx == 1) {
						float nx = x * xForm1x + z * xForm1z;
						float nz = z * xForm1x - x * xForm1z;
						x = nx; z = nz;
					}
					if (rotIdx >= 1) {
						float nx = x * xForm2x + z * xForm2z;
						float nz = z * xForm2x - x * xForm2z;
						x = nx; z = nz;
					}
					Vector p; p.x = x; p.y = y; p.z = z;
					AppendFountainOmni(FntSc, p,
					                   1.0f, 1.0f, 1.0f,
					                   omniIntensity, omniRange);
					++numAdded;
				}
			}
			if (numAdded < desired) {
				std::fprintf(stderr,
					"[FOUNTAIN] particle-omni stress: clamped %d -> %d"
					" (LA[128] budget = %d, %d already active)\n",
					desired, numAdded, budget, existingActive);
			}
		}
		std::fprintf(stderr,
			"[FOUNTAIN] particle-omni stress: N=%d omnis"
			" (FDS_FOUNTAIN_PARTICLE_OMNI_PCT=%.3f)\n",
			numAdded, pct);
	}

//	Noise_Sample();

//	for(I=0;I<128;I++)
//		Iterate_Photonbeam_Texture();

//	Generate_Photon_Plasma();
}


///////////////////////////
// LINE CLIPPER
///////////////////////
// Chained per-boundary 2D viewport clipper for Lightning_Line / etc:
//   Bivex_Frustrum_Perspective  near-Z, project to screen, hand to UFP1
//   UFP1   left edge   (PX = 0)        → UFP2
//   UFP2   right edge  (PX = XRes_1)   → UFP3
//   UFP3   top edge    (PY = 0)        → UFP4
//   UFP4   bottom edge (PY = YRes_1)   → Filler
//
// Each stage trivially rejects if both endpoints are outside its half-
// plane, interpolates the in/out endpoint at the boundary, and recurses.
// Left/top use `L = A/(A-B)`; right/bottom use the symmetric
// `R = (B-bound)/(B-A)` for numerical symmetry around the boundary.
void UFP1(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));
void UFP2(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));
void UFP3(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));
void UFP4(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));

// Set out = A * (1-t) + B * t for the screen attributes the clipper
// carries through: PX, PY, LR. Each stage overrides one of PX/PY
// afterwards to pin the new vertex exactly on its boundary plane.
static inline void lerpClippedVertex(Vertex& out, const Vertex* A, const Vertex* B, float t)
{
	const float R = 1.0f - t;
	out.PX = A->PX * R + B->PX * t;
	out.PY = A->PY * R + B->PY * t;
	out.LR = A->LR * R + B->LR * t;
}

// Sanity check on the interpolation parameter. ZOL = "if this fires, the
// caller's bounds-test logic is broken and we'd produce garbage geometry
// — bail loudly rather than silently clip outside the line segment."
static inline void assertClipParam(float t, int stage)
{
	if (t < -0.01f || t > 1.01f) {
		std::fprintf(stderr, "UFC%d: abnormal interpolation rate %f\n", stage, t);
		std::exit(1);
	}
}

void Bivex_Frustrum_Perspective(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	if (A->TPos_AOS.z > B->TPos_AOS.z) { Vertex* T = A; A = B; B = T; }
	if (B->TPos_AOS.z < 1) return;
	if (A->TPos_AOS.z < 1) {
		const float t = (A->TPos_AOS.z - 1) / (A->TPos_AOS.z - B->TPos_AOS.z);
		const float R = 1.0f - t;
		Vertex Vtx1;
		Vtx1.PX = CntrX + FOVX * (A->TPos_AOS.x*R + B->TPos_AOS.x*t);
		Vtx1.PY = CntrY - FOVY * (A->TPos_AOS.y*R + B->TPos_AOS.y*t);
		Vtx1.LR = A->LR * R + B->LR * t;
		UFP1(&Vtx1, B, Filler);
		return;
	}
	UFP1(A, B, Filler);
}

// heh heh, ZOL.
void UFP1(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	if (A->PX > B->PX) { Vertex* T = A; A = B; B = T; }
	if (B->PX < 0) return;
	if (A->PX < 0) {
		const float t = A->PX / (A->PX - B->PX);
		assertClipParam(t, 1);
		Vertex Vtx1;
		lerpClippedVertex(Vtx1, A, B, t);
		Vtx1.PX = 0;
		UFP2(&Vtx1, B, Filler);
		return;
	}
	UFP2(A, B, Filler);
}

void UFP2(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	const float XClip = XRes_1;
	if (A->PX > XClip) return;
	if (B->PX > XClip) {
		const float R = (B->PX - XClip) / (B->PX - A->PX);
		assertClipParam(R, 2);
		Vertex Vtx1;
		lerpClippedVertex(Vtx1, A, B, 1.0f - R);
		Vtx1.PX = XClip;
		UFP3(A, &Vtx1, Filler);
		return;
	}
	UFP3(A, B, Filler);
}

void UFP3(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	if (A->PY > B->PY) { Vertex* T = A; A = B; B = T; }
	if (B->PY < 0) return;
	if (A->PY < 0) {
		const float t = A->PY / (A->PY - B->PY);
		assertClipParam(t, 3);
		Vertex Vtx1;
		lerpClippedVertex(Vtx1, A, B, t);
		Vtx1.PY = 0;
		UFP4(&Vtx1, B, Filler);
		return;
	}
	UFP4(A, B, Filler);
}

void UFP4(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	const float YClip = YRes_1;
	if (A->PY > YClip) return;
	if (B->PY > YClip) {
		const float R = (B->PY - YClip) / (B->PY - A->PY);
		assertClipParam(R, 4);
		Vertex Vtx1;
		lerpClippedVertex(Vtx1, A, B, 1.0f - R);
		Vtx1.PY = YClip;
		Filler(A, &Vtx1);
		return;
	}
	Filler(A, B);
}


void Lightning_Line(Vertex *V1,Vertex *V2)
{
	DWord C=(255<<16)+(255<<8)+255;
	int32_t D;
	int32_t Dx,Dy;
	int32_t Ai,Bi,Xi,Yi;
	int32_t Offs,sat;
	int32_t X1,Y1,X2,Y2;
	int32_t depth = V1->LR,hd = depth>>1;
	uint8_t *cPtr;
	DWord *Ptr = (DWord *)VPage;
	DWord *End;

	X1=V1->PX+0.5; Y1=V1->PY+0.5;
	X2=V2->PX+0.5; Y2=V2->PY+0.5;
	// The viewport clip is float-exact but the +0.5 rounding above can
	// push an edge-clipped endpoint to XRes/YRes — YOffs[YRes] is OOB and
	// a missed `Ptr != End` walks the Bresenham off the framebuffer
	// (SEGV'd on the spark ring's near-horizontal lines). Clamp into the
	// drawable region; the AA writes touch +-1 row and +-3 bytes.
	if (X1 < 2) X1 = 2; if (X1 > (int32_t)XRes-4) X1 = XRes-4;
	if (X2 < 2) X2 = 2; if (X2 > (int32_t)XRes-4) X2 = XRes-4;
	if (Y1 < 1) Y1 = 1; if (Y1 > (int32_t)YRes-2) Y1 = YRes-2;
	if (Y2 < 1) Y2 = 1; if (Y2 > (int32_t)YRes-2) Y2 = YRes-2;

	if (X1<=X2)
	{
		Xi = 1;
		Dx = X2-X1;
	} else {
		Xi = -1;
		Dx = X1-X2;
	}

	if (Y1<=Y2) {
		Yi = XRes;
		Dy = Y2-Y1;
	} else {
		Yi = -XRes;
		Dy = Y1-Y2;
	}
//	Ptr-=hd;
	int32_t guard = Dx + Dy + 2;   // exact step count + slack
	// Per-pixel Z test, perspective-correct via screen-linear RZ lerp.
	// These fillers draw after the whole frame and used to scribble over
	// every surface ('not respecting render order').
	const int32_t zsteps = (Dx > Dy ? Dx : Dy);
	float rzCur = V1->RZ;
	const float rzStep = zsteps > 0 ? (V2->RZ - V1->RZ) / (float)zsteps : 0.0f;
	DWord* const vbase = (DWord *)VPage;
	End=Ptr+X2+YOffs[Y2];
	Ptr+=X1+YOffs[Y1];

	// Z-test the start pixel too (it used to write unconditionally). The core
	// is drawn as many short segments, so one un-tested start dot per segment
	// = a dotted line punching through opaque geometry.
	{
		const float zw0 = 1.0f / (rzCur > 1e-9f ? rzCur : 1e-9f);
		const dword zc0 = 0xFF80 - (dword)Fist(g_zscale * zw0);
		if (ZPage16[(size_t)(Ptr - vbase)] < zc0) *Ptr = 0xFFFFFF;
	}
//	 memset(Ptr,255,depth<<2);

	if (Dx > Dy) {
		Ai = (Dy-Dx)<<1;
		Bi = Dy<<1;
		D = Bi-Dx;
		while (Ptr != End && --guard >= 0) {
			if (D >= 0) {
				Ptr += Yi;
				D += Ai;
			} else D = D + Bi;
			Ptr += Xi;

			rzCur += rzStep;
			{
				const float zw = 1.0f / (rzCur > 1e-9f ? rzCur : 1e-9f);
				const dword zc = 0xFF80 - (dword)Fist(g_zscale * zw);
				if (ZPage16[(size_t)(Ptr - vbase)] >= zc) continue;
			}
			*Ptr = 0xFFFFFF;
//			memset(Ptr,255,depth<<2);
		}
	} else {
		Ai = (Dx-Dy)<<1;
		Bi = Dx<<1;
		D = Bi-Dy;
		while (Ptr != End && --guard >= 0) {

			if (D >= 0) {
				Ptr += Xi;
				D += Ai;
			} else D += Bi;
			Ptr += Yi;

			rzCur += rzStep;
			{
				const float zw = 1.0f / (rzCur > 1e-9f ? rzCur : 1e-9f);
				const dword zc = 0xFF80 - (dword)Fist(g_zscale * zw);
				if (ZPage16[(size_t)(Ptr - vbase)] >= zc) continue;
			}

			//left anti-alias
			cPtr = ((uint8_t *)Ptr)-8;
			Offs = 128*(D-Ai)/(Bi-Ai);
			sat = *cPtr+(Offs>>5);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>4);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>1);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			cPtr++;

			sat = *cPtr+(Offs>>3);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>2);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>1);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;

			*Ptr = 0xFFFFFF;

			cPtr+=5;
			Offs = 128-Offs;

			sat = *cPtr+(Offs>>3);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>2);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>1);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;

			cPtr++;
			sat = *cPtr+(Offs>>5);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>4);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>3);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;

//			memset(Ptr,255,depth<<2);
		}
	}
}

void Spark_Line(Vertex *V1,Vertex *V2)
{
	DWord C=(255<<16)+(255<<8)+255;
	int32_t D;
	int32_t Dx,Dy;
	int32_t Ai,Bi,Xi,Yi;
	int32_t Offs,sat;
	int32_t X1,Y1,X2,Y2;
	int32_t depth = V1->LR,hd = depth>>1;
	uint8_t *cPtr;
	DWord *Ptr = (DWord *)VPage;
	DWord *End;

	X1=V1->PX+0.5; Y1=V1->PY+0.5;
	X2=V2->PX+0.5; Y2=V2->PY+0.5;
	// The viewport clip is float-exact but the +0.5 rounding above can
	// push an edge-clipped endpoint to XRes/YRes — YOffs[YRes] is OOB and
	// a missed `Ptr != End` walks the Bresenham off the framebuffer
	// (SEGV'd on the spark ring's near-horizontal lines). Clamp into the
	// drawable region; the AA writes touch +-1 row and +-3 bytes.
	if (X1 < 2) X1 = 2; if (X1 > (int32_t)XRes-4) X1 = XRes-4;
	if (X2 < 2) X2 = 2; if (X2 > (int32_t)XRes-4) X2 = XRes-4;
	if (Y1 < 1) Y1 = 1; if (Y1 > (int32_t)YRes-2) Y1 = YRes-2;
	if (Y2 < 1) Y2 = 1; if (Y2 > (int32_t)YRes-2) Y2 = YRes-2;

	if (X1<=X2)
	{
		Xi = 1;
		Dx = X2-X1;
	} else {
		Xi = -1;
		Dx = X1-X2;
	}

	if (Y1<=Y2) {
		Yi = XRes;
		Dy = Y2-Y1;
	} else {
		Yi = -XRes;
		Dy = Y1-Y2;
	}
//	Ptr-=hd;
	int32_t guard = Dx + Dy + 2;   // exact step count + slack
	// Per-pixel Z test, perspective-correct via screen-linear RZ lerp.
	// These fillers draw after the whole frame and used to scribble over
	// every surface ('not respecting render order').
	const int32_t zsteps = (Dx > Dy ? Dx : Dy);
	float rzCur = V1->RZ;
	const float rzStep = zsteps > 0 ? (V2->RZ - V1->RZ) / (float)zsteps : 0.0f;
	DWord* const vbase = (DWord *)VPage;
	End=Ptr+X2+YOffs[Y2];
	Ptr+=X1+YOffs[Y1];

	// Z-test the start pixel too (it used to write unconditionally). The core
	// is drawn as many short segments, so one un-tested start dot per segment
	// = a dotted line punching through opaque geometry.
	{
		const float zw0 = 1.0f / (rzCur > 1e-9f ? rzCur : 1e-9f);
		const dword zc0 = 0xFF80 - (dword)Fist(g_zscale * zw0);
		if (ZPage16[(size_t)(Ptr - vbase)] < zc0) *Ptr = 0xFFFFFF;
	}
//	 memset(Ptr,255,depth<<2);

	if (Dx > Dy) {
		Ai = (Dy-Dx)<<1;
		Bi = Dy<<1;
		D = Bi-Dx;
		while (Ptr != End && --guard >= 0) {
			if (D >= 0) {
				Ptr += Yi;
				D += Ai;
			} else D = D + Bi;
			Ptr += Xi;

			rzCur += rzStep;
			{
				const float zw = 1.0f / (rzCur > 1e-9f ? rzCur : 1e-9f);
				const dword zc = 0xFF80 - (dword)Fist(g_zscale * zw);
				if (ZPage16[(size_t)(Ptr - vbase)] >= zc) continue;
			}
//			*Ptr = 0xFFFFFF;
//			memset(Ptr,255,depth<<2);

			cPtr = ((uint8_t *)Ptr)-VESA_BPSL;
			Offs = 128*(D-Ai)/(Bi-Ai);
			sat = *cPtr+(Offs>>5);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>4);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>3);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;

			cPtr+=VESA_BPSL-3;
			sat = *cPtr+(128);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(128);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(128);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;

			cPtr+=VESA_BPSL-3;
			Offs = 128-Offs;

			sat = *cPtr+(Offs>>5);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>4);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>3);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;



		}
	} else {
		Ai = (Dx-Dy)<<1;
		Bi = Dx<<1;
		D = Bi-Dy;
		while (Ptr != End && --guard >= 0) {

			if (D >= 0) {
				Ptr += Xi;
				D += Ai;
			} else D += Bi;
			Ptr += Yi;

			rzCur += rzStep;
			{
				const float zw = 1.0f / (rzCur > 1e-9f ? rzCur : 1e-9f);
				const dword zc = 0xFF80 - (dword)Fist(g_zscale * zw);
				if (ZPage16[(size_t)(Ptr - vbase)] >= zc) continue;
			}

			//left anti-alias
			cPtr = ((uint8_t *)Ptr)-4;
			Offs = 128*(D-Ai)/(Bi-Ai);
			sat = *cPtr+(Offs>>5);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>4);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>3);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;

			cPtr++;
			sat = *cPtr+(24);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(48);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(96);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;

			//*Ptr = 0xFFFFFF;

			cPtr++;
			Offs = 128-Offs;

			sat = *cPtr+(Offs>>5);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>4);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
			sat = *cPtr+(Offs>>3);
			if (sat>255) *cPtr++ = 255; else *cPtr++ = sat;
		}
	}
}


// Per-strike fragment budget. The recursive brancher is a supercritical
// process at high BasePow (each segment spawns a branch with ~constant
// probability, branches spawn sub-branches...) — at the port's flat 80 the
// expected tree size diverged and the demo hung on the first strike. The
// 1998 powers (45..70) are restored, but termination must not depend on
// tuning: cap the total fragments a strike may draw.
static int32_t g_boltBudget = 0;

void Lightning_Bolt_Frag(Vector *Src,Vector *Targ,int32_t POW, Material* LnMat)
{
	if (--g_boltBudget < 0) return;
//	memset(VPage,255,PageSize);
	if (POW<1) POW=1;
	Vertex A,B;
	Vector V;

	Vector_Sub(Src,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&A.TPos_AOS);
	A.TPos_AOS.x = A.TPos_AOS.z * CntrX + A.TPos_AOS.x * FOVX;
	A.TPos_AOS.y = A.TPos_AOS.z * CntrY - A.TPos_AOS.y * FOVY;
	A.RZ = 1.0f/A.TPos_AOS.z;
	A.PX = A.TPos_AOS.x * A.RZ;
	A.PY = A.TPos_AOS.y * A.RZ;

	Vector_Sub(Targ,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&B.TPos_AOS);
	B.TPos_AOS.x = B.TPos_AOS.z * CntrX + B.TPos_AOS.x * FOVX;
	B.TPos_AOS.y = B.TPos_AOS.z * CntrY - B.TPos_AOS.y * FOVY;
	B.RZ = 1.0f/B.TPos_AOS.z;
	B.PX = B.TPos_AOS.x * B.RZ;
	B.PY = B.TPos_AOS.y * B.RZ;

	if (A.TPos_AOS.z < C_NZP || B.TPos_AOS.z < C_NZP) {
		return;
	}

	float dx = B.PX - A.PX;
	float dy = B.PY - A.PY;
	const float seglen2 = dx * dx + dy * dy;
	if (seglen2 < 1e-6f) return;          // degenerate (endpoints coincide)
	float rlen = 1.0f / sqrt(seglen2);
	dx *= rlen;
	dy *= rlen;
	// Unit perpendicular IN PIXEL SPACE. The old code scaled its x by
	// FOVX and its y by -FOVY — un-normalizing, skewing and sign-
	// flipping the width vector for any non-vertical segment, so bolt
	// quads twisted as segments rotated ("not transformed correctly").
	// The world half-width projects to pixels as w·RZ·FOVX; apply that
	// scalar to the unit perpendicular on BOTH axes.
	float nx = -dy;
	float ny = dx;
	Vertex quad[4];
	const float halfA = POW * 2.0f * A.RZ * FOVX;   // px at the A end
	const float halfB = POW * 2.0f * B.RZ * FOVX;   // px at the B end
	quad[0].PX = A.PX - halfA * nx;
	quad[0].PY = A.PY - halfA * ny;
	quad[0].UZ = 0;
	quad[0].VZ = 0;
	quad[0].RZ = A.RZ;

	quad[1].PX = A.PX + halfA * nx;
	quad[1].PY = A.PY + halfA * ny;
	quad[1].UZ = A.RZ;
	quad[1].VZ = 0;
	quad[1].RZ = A.RZ;

	quad[2].PX = B.PX + halfB * nx;
	quad[2].PY = B.PY + halfB * ny;
	quad[2].UZ = B.RZ;
	quad[2].VZ = B.RZ;
	quad[2].RZ = B.RZ;

	quad[3].PX = B.PX - halfB * nx;
	quad[3].PY = B.PY - halfB * ny;
	quad[3].UZ = 0;
	quad[3].VZ = B.RZ;
	quad[3].RZ = B.RZ;

	Viewport vp;
	viewportInit(vp, FntSc);

	for (int i = 0; i != 4; ++i) {
		Vertex& v = quad[i];
		v.LA = v.LR = v.LG = v.LB = 255.0;
		viewportCalcFlags(vp, &v);
	}

	Face f[2];
	f[0].A = &quad[0];
	f[0].B = &quad[1];
	f[0].C = &quad[2];
	// ADDITIVE: a bolt is light — it brightens whatever is behind it,
	// and the gradient's black edges add nothing (free soft AA).
	f[0].Filler = (TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL>);
	f[0].Txtr = LnMat;

	f[1].A = &quad[0];
	f[1].B = &quad[2];
	f[1].C = &quad[3];
	f[1].Filler = (TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL>);
	f[1].Txtr = LnMat;

	const auto rt  = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;
	_2DClipper::getInstance()->clip(vp, f[0], rt, cam);
	_2DClipper::getInstance()->clip(vp, f[1], rt, cam);

	// The strike CORE: the original's anti-aliased pure-white line
	// (Spark_Line for the thin branch tips), drawn over the additive
	// glow quads. Bivex_Frustrum_Perspective near-Z + viewport clips
	// before the line filler touches VPage. LR carries the thickness.
	A.LR = B.LR = float(POW);
	if (POW>1)
		Bivex_Frustrum_Perspective(&A,&B,Lightning_Line);
	else
		Bivex_Frustrum_Perspective(&A,&B,Spark_Line);
}


void Generate_Lightning_Particles(Vector *Src,float Num)
{
	Particle *P,*PE;
	while(Num--)
	{
		for(P = LBoltPcls,PE = P + FntLightningPcls;P->Charge>0.0f&&P<PE;P++);
		if (P==PE) return;
		P->Charge = RAND_15()*0.5f/32768.0f+0.5f;
		Vector_Copy(&P->V.Pos,Src);
		P->Vel.x = ((RAND_15()-16384)/16384.0f)*32.5f;
		P->Vel.y = ((RAND_15()-16384)/16384.0f)*5.5f;
		P->Vel.z = ((RAND_15()-16384)/16384.0f)*32.5f;
		P->Color.R = 128;
		P->Color.G = 96+(RAND_15()>>10);
		P->Color.B = 32+(RAND_15()>>9);
	}
}

void Lightning_Particles()
{
	Particle *P,*PE;
	Vector V;
	float dt = dTime*0.01f;
	for(P=LBoltPcls,PE=P+FntLightningPcls;P<PE;P++)
	{
		if (P->Charge<=0.0f) continue;
		P->Charge-=dt;
		if (P->V.Pos.y<=0.0f) P->Charge = 0.0f;

		P->Vel.y -= dt * 16.0f;
		Vector_Scale(&P->Vel,dt,&V);
		Vector_SelfAdd(&P->V.Pos,&V);

		Vector_Sub(&P->V.Pos,&View->ISource,&V);
		MatrixXVector(View->Mat,&V,&P->V.TPos_AOS);

		if (P->V.TPos_AOS.z>=1.0)
		{
			P->V.RZ = 1.0/P->V.TPos_AOS.z;
			P->V.PX = CntrX+FOVX*P->V.TPos_AOS.x*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos_AOS.y*P->V.RZ;
//      P->PRad = 0;//FOVX*P->Radius*RZ;
			P->V.Flags = 0;
		} else P->V.Flags |= Vtx_VisNear;

		// dummy illumination model
		P->V.LB = P->Color.B*0.5*(P->Charge+0.5f);
		P->V.LG = P->Color.G*0.5*(P->Charge+0.5f);
		P->V.LR = P->Color.R*0.5*(P->Charge+0.5f);
		Particle_Rast(&P->V,&P->V,&P->V);
	}


}

// Connected-ribbon bolt renderer. The 1998 per-segment quads crack at the
// joints, and any segment of the 3D walk that points depth-ward projects
// to ~zero screen length while keeping its full width — fat sideways bars
// ('ladder rungs'). One ribbon with joint-averaged screen perpendiculars
// and shared edge vertices fixes both. The hot core line still draws per
// segment over the ribbon (it is endpoint-exact and crack-free already).
// ── Texture-based bolt ──────────────────────────────────────────────────
// A polygon ribbon shows every quad facet (flat translucent parallelograms
// = crumpled foil, not lightning). The fix is to stop putting the jaggedness
// in GEOMETRY: a bolt is ONE camera-facing quad stretched src->target,
// carrying a pre-rendered jagged-bolt TEXTURE (white-hot core + blue glow,
// anti-aliased, black elsewhere so additive = transparent). One quad → zero
// facets; jaggedness and glow are smooth at any size. A small set of texture
// variants is baked at init and cycled per restrike.
namespace {
struct LnRng {
	uint32_t s;
	inline uint32_t u() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
	inline float f01() { return (u() >> 8) * (1.0f / 16777216.0f); }
	inline float c()   { return f01() * 2.0f - 1.0f; }
};
constexpr int32_t kRestrike   = 7;    // Timer units between fresh paths
constexpr int     kBoltVar    = 6;    // baked texture variants
Material*         g_boltTex[kBoltVar] = { nullptr };
float             g_boltTexKink = -1.0f;
float             g_boltWig[kBoltVar][129] = {{0}};  // per-variant centre wiggle
int               g_boltWigN[kBoltVar] = {0};        // wiggle sample count

// Bake one jagged-bolt texture. The bolt runs down V (texture height); its
// horizontal centre wiggles via 1D midpoint displacement. Each texel = a
// white-hot core (sharp) + a blue glow (soft) by distance to the centre,
// faded at the two ends so the stretched quad doesn't hard-cut.
Material* makeBoltTexture(uint32_t seed, float rough, float* outWig, int* outN) {
	constexpr int W = 512, H = 512;
	LnRng rng{ seed ? seed : 1u };
	float vals[129]; int n = 2;
	vals[0] = 0.5f; vals[1] = 0.5f;          // normalised centre x at the ends
	float amp = rough;
	for (int lvl = 0; lvl < 7; ++lvl) {
		const int m = n * 2 - 1;
		if (m > 129) break;
		float tmp[129];
		for (int i = 0; i < n; ++i)     tmp[2*i] = vals[i];
		for (int i = 0; i + 1 < n; ++i) tmp[2*i+1] = 0.5f*(vals[i]+vals[i+1]) + rng.c()*amp;
		n = m;
		for (int i = 0; i < n; ++i) vals[i] = tmp[i];
		amp *= 0.5f;
	}
	Image Img; Img.x = W; Img.y = H; Img.Data = new DWord[W*H];
	const float glowR2 = H * 0.10f;
	// Centre as a polyline in texel space. core/glow use the 2D DISTANCE to
	// this polyline — not the per-column |y-cy(x)|, which made every column an
	// independent vertical glow streak that banded into 'ladder rungs' wherever
	// the centre wiggled. A true distance field is a smooth tube. Full segment
	// scan (a windowed search drops the true-nearest segment as it shifts per
	// column = the banding); cheap at 512^2, baked once / on kink change.
	for (int i = 0; i < n; ++i) outWig[i] = vals[i];   // hand the wiggle to the core-line draw
	*outN = n;
	float px[129], py[129];
	for (int k = 0; k < n; ++k) { px[k] = (float)k/(n-1)*(W-1); py[k] = vals[k]*H; }
	auto distSeg2 = [](float Px,float Py,float ax,float ay,float bx,float by)->float{
		const float vx=bx-ax, vy=by-ay, wx=Px-ax, wy=Py-ay;
		const float vv=vx*vx+vy*vy;
		float tt = vv > 1e-6f ? (wx*vx+wy*vy)/vv : 0.0f;
		tt = tt<0.0f?0.0f:(tt>1.0f?1.0f:tt);
		const float dx=Px-(ax+vx*tt), dy=Py-(ay+vy*tt);
		return dx*dx+dy*dy;
	};
	for (int x = 0; x < W; ++x) {
		// 1-texel guard border so bilinear can't bleed the bolt past the edge;
		// no length-end fade (the bolt must reach its endpoints).
		const float ef = (x == 0 || x == W-1) ? 0.0f : 1.0f;
		for (int y = 0; y < H; ++y) {
			float best = 1e18f;
			for (int k = 0; k < n-1; ++k) {
				const float dd = distSeg2((float)x,(float)y, px[k],py[k], px[k+1],py[k+1]);
				if (dd < best) best = dd;
			}
			const float d = sqrtf(best);
			// Glow ONLY — the sharp white core now draws as a crisp geometric
			// AA line (Lightning_Line) on top, so it can't staircase the way a
			// magnified nearest-sampled texture core did. Tail clipped to ZERO
			// before the edge so it doesn't fill the quad as a blue rectangle.
			float glow = expf(-d / glowR2) - 0.06f;
			glow = glow > 0.0f ? (glow / 0.94f) * 0.9f : 0.0f;
			int Ri = (int)((glow*0.20f) * 255.0f * ef);
			int Gi = (int)((glow*0.55f) * 255.0f * ef);
			int Bi = (int)((glow*1.00f) * 255.0f * ef);
			if (Ri>255)Ri=255; if (Gi>255)Gi=255; if (Bi>255)Bi=255;
			Img.Data[x + y*W] = (DWord)Bi | ((DWord)Gi<<8) | ((DWord)Ri<<16) | (255u<<24);
		}
	}
	Material* M = getAlignedType<Material>(16);
	M->Flags = Mat_Virtual;
	M->Txtr = new Texture;
	std::memset(M->Txtr, 0, sizeof(Texture));
	M->Txtr->BPP = 32;
	M->Txtr->Flags = Txtr_Nomip;
	Convert_Image2Texture(&Img, M->Txtr);
	delete[] Img.Data;
	M->Txtr->Mipmap[0] = M->Txtr->Data;
	M->Txtr->numMipmaps = 1;
	return M;
}

void ensureBoltTextures() {
	const float k = fds::FeatureFlags::bolt_kink();
	if (g_boltTex[0] && fabsf(k - g_boltTexKink) < 0.02f) return;
	for (int i = 0; i < kBoltVar; ++i) {
		if (g_boltTex[i]) {        // free the previous set on a roughness change
			if (g_boltTex[i]->Txtr) { delete[] g_boltTex[i]->Txtr->Data; delete g_boltTex[i]->Txtr; }
		}
		g_boltTex[i] = makeBoltTexture(0xB017u + i*2654435761u, k, g_boltWig[i], &g_boltWigN[i]);
	}
	g_boltTexKink = k;
}

float boltFlicker() {
	uint32_t h = (uint32_t)(Timer) * 2654435761u;
	h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13;
	return 0.70f + 0.30f * ((h & 0xFFFF) * (1.0f/65536.0f));
}

// Crisp geometric AA core: a bundle of parallel Lightning_Lines through the
// wiggle (screen space already computed by the caller). Resolution-independent
// (no staircase); bundle width scales with YRes*bolt_width so it's not a 1px
// thread at 1080p. Each segment is Liang-Barsky-clipped to the drawable rect
// first — Lightning_Line clamps X/Y independently, which would turn an
// off-screen endpoint into a bogus border line. Shared by both glow paths.
void drawBoltCore(float ax, float ay, float bx, float by, float nx, float ny,
                  float hwA, float hwB, float rzA, float rzB,
                  const float* wig, int wigN) {
	if (!wig || wigN < 2) return;
	int thick = (int)(YRes / 270.0f * fds::FeatureFlags::bolt_width() + 0.5f);
	if (thick < 1) thick = 1; if (thick > 8) thick = 8;
	const float cxmin=2.0f, cymin=1.0f, cxmax=(float)XRes-4.0f, cymax=(float)YRes-2.0f;
	auto clipSeg = [&](float&x0,float&y0,float&x1,float&y1)->bool{
		const float dx=x1-x0, dy=y1-y0; float t0=0.0f, t1=1.0f;
		const float p[4]={-dx,dx,-dy,dy};
		const float q[4]={x0-cxmin,cxmax-x0,y0-cymin,cymax-y0};
		for(int i=0;i<4;i++){
			if(p[i]==0.0f){ if(q[i]<0.0f) return false; }
			else { const float r=q[i]/p[i];
				if(p[i]<0.0f){ if(r>t1) return false; if(r>t0) t0=r; }
				else         { if(r<t0) return false; if(r<t1) t1=r; } }
		}
		const float bx0=x0, by0=y0;
		x0=bx0+t0*dx; y0=by0+t0*dy; x1=bx0+t1*dx; y1=by0+t1*dy; return true;
	};
	Vertex cp{}, cc{}; bool have = false;
	for (int k = 0; k < wigN; ++k) {
		const float t   = (float)k / (wigN - 1);
		const float hwk = hwA + (hwB - hwA) * t;
		const float off = hwk * (2.0f * wig[k] - 1.0f);
		cc.PX = ax + (bx - ax) * t + nx * off;
		cc.PY = ay + (by - ay) * t + ny * off;
		cc.RZ = rzA + (rzB - rzA) * t;
		cc.LR = 1.0f;
		if (have) {
			for (int o = 0; o < thick; ++o) {
				const float od = (float)o - (thick - 1) * 0.5f;
				Vertex a2 = cp, b2 = cc;
				a2.PX += nx * od; a2.PY += ny * od;
				b2.PX += nx * od; b2.PY += ny * od;
				if (clipSeg(a2.PX, a2.PY, b2.PX, b2.PY))
					Lightning_Line(&a2, &b2);
			}
		}
		cp = cc; have = true;
	}
}

// Stretch ONE textured quad from world A to world B, billboarded (screen-
// space perpendicular), additive. hwWorld = half-width in world units;
// bright scales the additive output via vertex colour.
void drawBoltQuad(const Vector& A, const Vector& B, Material* tex,
                  float hwWorld, float bright, const float* wig, int wigN) {
	if (--g_boltBudget < 0 || !tex) return;
	Vertex TA, TB; Vector V;
	Vector_Sub(const_cast<Vector*>(&A), &View->ISource, &V); MatrixXVector(View->Mat, &V, &TA.TPos_AOS);
	Vector_Sub(const_cast<Vector*>(&B), &View->ISource, &V); MatrixXVector(View->Mat, &V, &TB.TPos_AOS);
	if (TA.TPos_AOS.z < C_NZP || TB.TPos_AOS.z < C_NZP) return;
	const float rzA = 1.0f/TA.TPos_AOS.z, rzB = 1.0f/TB.TPos_AOS.z;
	const float ax = (TA.TPos_AOS.z*CntrX + TA.TPos_AOS.x*FOVX)*rzA;
	const float ay = (TA.TPos_AOS.z*CntrY - TA.TPos_AOS.y*FOVY)*rzA;
	const float bx = (TB.TPos_AOS.z*CntrX + TB.TPos_AOS.x*FOVX)*rzB;
	const float by = (TB.TPos_AOS.z*CntrY - TB.TPos_AOS.y*FOVY)*rzB;
	float dx = bx-ax, dy = by-ay; const float l = sqrtf(dx*dx+dy*dy);
	if (l < 1.0f) return; dx/=l; dy/=l;
	const float nx = -dy, ny = dx;
	float hwA = hwWorld * rzA * FOVX, hwB = hwWorld * rzB * FOVX;
	if (hwA < 1.5f) hwA = 1.5f; if (hwA > 400.0f) hwA = 400.0f;
	if (hwB < 1.5f) hwB = 1.5f; if (hwB > 400.0f) hwB = 400.0f;
	float col = bright * 255.0f; if (col < 0.0f) col = 0.0f; if (col > 255.0f) col = 255.0f;
	Vertex q[4];
	// This rasterizer samples UZ->texture-Y, VZ->texture-X (verified with a
	// 4-band diagnostic). So put the bolt's LENGTH on U and width on V:
	//   U (UZ): 0 at the A end, 1 at the B end  -> texture-Y (jagged length)
	//   V (VZ): 0 / 1 across the width          -> texture-X (glow profile)
	// U = across width, V = along length (the working-ribbon convention);
	// the texture is built jagged along its V axis to match.
	q[0].PX = ax - hwA*nx; q[0].PY = ay - hwA*ny; q[0].UZ=0;   q[0].VZ=0;   q[0].RZ = rzA;
	q[1].PX = ax + hwA*nx; q[1].PY = ay + hwA*ny; q[1].UZ=rzA; q[1].VZ=0;   q[1].RZ = rzA;
	q[2].PX = bx + hwB*nx; q[2].PY = by + hwB*ny; q[2].UZ=rzB; q[2].VZ=rzB; q[2].RZ = rzB;
	q[3].PX = bx - hwB*nx; q[3].PY = by - hwB*ny; q[3].UZ=0;   q[3].VZ=rzB; q[3].RZ = rzB;
	Viewport vp; viewportInit(vp, FntSc);
	for (int k = 0; k != 4; ++k) { q[k].LA = q[k].LR = q[k].LG = q[k].LB = col; viewportCalcFlags(vp, &q[k]); }
	Face f[2];
	f[0].A=&q[0]; f[0].B=&q[1]; f[0].C=&q[2];
	f[0].Filler=(TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL_BILINEAR, false>); f[0].Txtr=tex;
	f[1].A=&q[0]; f[1].B=&q[2]; f[1].C=&q[3];
	f[1].Filler=(TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL_BILINEAR, false>); f[1].Txtr=tex;
	const auto rt = fds::MainRenderTargetFromGlobals(); const auto& cam = fds::g_mainCamera;
	_2DClipper::getInstance()->clip(vp, f[0], rt, cam);
	_2DClipper::getInstance()->clip(vp, f[1], rt, cam);

	drawBoltCore(ax, ay, bx, by, nx, ny, hwA, hwB, rzA, rzB, wig, wigN);
}

// Option A: fully geometric, NO texture. A 3-rail additive Gouraud ribbon
// (edge=black, centre=bright, edge=black) gives a smooth glow tube via vertex-
// colour interpolation — no bake, no texture sample (so no stall, no maze, no
// magnification stairs), and properly clipped (no out-of-bounds). NOT the flat
// translucent textured ribbon that looked like foil: this is additive + smooth
// colour, overlaps just brighten. The crisp AA core line draws on top.
void drawBoltRibbon(const Vector& A, const Vector& B,
                    float hwWorld, float bright, const float* wig, int wigN) {
	if (--g_boltBudget < 0 || !wig || wigN < 2) return;
	Vertex TA, TB; Vector V;
	Vector_Sub(const_cast<Vector*>(&A), &View->ISource, &V); MatrixXVector(View->Mat, &V, &TA.TPos_AOS);
	Vector_Sub(const_cast<Vector*>(&B), &View->ISource, &V); MatrixXVector(View->Mat, &V, &TB.TPos_AOS);
	if (TA.TPos_AOS.z < C_NZP || TB.TPos_AOS.z < C_NZP) return;
	const float rzA = 1.0f/TA.TPos_AOS.z, rzB = 1.0f/TB.TPos_AOS.z;
	const float ax = (TA.TPos_AOS.z*CntrX + TA.TPos_AOS.x*FOVX)*rzA;
	const float ay = (TA.TPos_AOS.z*CntrY - TA.TPos_AOS.y*FOVY)*rzA;
	const float bx = (TB.TPos_AOS.z*CntrX + TB.TPos_AOS.x*FOVX)*rzB;
	const float by = (TB.TPos_AOS.z*CntrY - TB.TPos_AOS.y*FOVY)*rzB;
	float dx = bx-ax, dy = by-ay; const float l = sqrtf(dx*dx+dy*dy);
	if (l < 1.0f) return; dx/=l; dy/=l;
	const float nx = -dy, ny = dx;
	float hwA = hwWorld * rzA * FOVX, hwB = hwWorld * rzB * FOVX;
	if (hwA < 1.5f) hwA = 1.5f; if (hwA > 400.0f) hwA = 400.0f;
	if (hwB < 1.5f) hwB = 1.5f; if (hwB > 400.0f) hwB = 400.0f;
	// Centre glow colour (additive). Cool blue; edges = 0. Since a66668b fixed
	// TheOtherBarry's colorize to pack the vertex colour BGRA, LR→screen-Red and
	// LB→screen-Blue (the natural RGB mapping), so the blue now rides LB (cb).
	// Scaled by bright.
	float cr = bright * 55.0f, cg = bright * 150.0f, cb = bright * 255.0f;
	if (cr > 255.0f) cr = 255.0f; if (cg > 255.0f) cg = 255.0f; if (cb > 255.0f) cb = 255.0f;

	Viewport vp; viewportInit(vp, FntSc);
	const auto rt = fds::MainRenderTargetFromGlobals(); const auto& cam = fds::g_mainCamera;
	// N-rail additive Gouraud ribbon. A peaked brightness profile (vs a flat
	// 3-rail tent, which reads as a uniform beam) gives a hot core fading to a
	// soft halo — like the texture's exp glow. Outer rails at <hw keep the body
	// tight. UZ/VZ unused (NONE mode); vertex colour IS the additive output.
	constexpr int NR = 5;
	const float rOff[NR] = { -0.70f, -0.28f, 0.0f, 0.28f, 0.70f };  // × hw
	const float rBri[NR] = {  0.0f,   0.40f, 1.0f, 0.40f, 0.0f  };  // × centre
	Vertex prev[NR]{}, cur[NR]{};
	bool have = false;
	auto setV = [&](Vertex& v, float px, float py, float rz, float w){
		v.PX = px; v.PY = py; v.RZ = rz; v.UZ = 0; v.VZ = 0;
		v.LR = cr*w; v.LG = cg*w; v.LB = cb*w; v.LA = cb*w;
		viewportCalcFlags(vp, &v);
	};
	auto emit = [&](Vertex& a, Vertex& b2, Vertex& c){
		// Skip near-degenerate triangles: the rasterizer's colour gradient
		// divides by signed area, so a ~0-area sliver blows the Gouraud up into
		// garbage-coloured dashes. (Edge-on branches / tiny segments hit this.)
		// Partial / off-screen triangles are CLIPPED normally — the NONE-mode
		// colour clamp keeps the out-of-triangle fringe black, so the halo can
		// reach the screen edges and around forks without garbage.
		const float area = (b2.PX-a.PX)*(c.PY-a.PY) - (b2.PY-a.PY)*(c.PX-a.PX);
		if (area > -1.0f && area < 1.0f) return;
		Face f; f.A=&a; f.B=&b2; f.C=&c;
		f.Filler=(TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NONE, false>);
		f.Txtr=nullptr;
		_2DClipper::getInstance()->clip(vp, f, rt, cam);
	};
	// Decimate the glow path to ~40 segments: the soft glow doesn't need the
	// full 128-pt jaggedness (the crisp core line does, and draws it), and long
	// segments avoid the thin near-degenerate triangles. Big perf win too.
	const int step = (wigN > 48) ? (wigN - 1) / 40 : 1;
	for (int k = 0; ; k += step) {
		const bool last = (k >= wigN - 1);
		const int  kk   = last ? wigN - 1 : k;
		const float t   = (float)kk / (wigN - 1);
		const float rz  = rzA + (rzB - rzA) * t;
		const float hwk = hwA + (hwB - hwA) * t;
		const float off = hwk * (2.0f * wig[kk] - 1.0f);   // wiggle centre
		const float mx  = ax + (bx - ax) * t + nx * off;
		const float my  = ay + (by - ay) * t + ny * off;
		for (int r = 0; r < NR; ++r) {
			const float ro = rOff[r] * hwk;
			setV(cur[r], mx + nx*ro, my + ny*ro, rz, rBri[r]);
		}
		if (have) {
			for (int r = 0; r < NR-1; ++r) {
				emit(prev[r], prev[r+1], cur[r+1]);
				emit(prev[r], cur[r+1], cur[r]);
			}
		}
		for (int r = 0; r < NR; ++r) prev[r] = cur[r];
		have = true;
		if (last) break;
	}

	drawBoltCore(ax, ay, bx, by, nx, ny, hwA, hwB, rzA, rzB, wig, wigN);
}
} // namespace

// One strike: the main channel + a few branches, each a stretched bolt quad.
// Restrike-bucketed (steady between ~3 restrikes/strike), brightness flicker,
// scrub-reproducible.
static void RenderStrike(const Vector& src, const Vector& targ,
                         int basePow, Material* /*mat*/)
{
	ensureBoltTextures();
	const int   bucket = (int)(Timer / kRestrike);
	LnRng rng{ (uint32_t)(bucket*2654435761u) ^ (uint32_t)(basePow*40503u) ^ 0xB07Du };
	const float bright = boltFlicker() * fds::FeatureFlags::bolt_seg();
	const float wf     = fds::FeatureFlags::bolt_width();
	Vector d = { targ.x-src.x, targ.y-src.y, targ.z-src.z };
	const float len = sqrtf(d.x*d.x+d.y*d.y+d.z*d.z);
	const float hw  = wf * 0.05f * len;     // half-width tracks bolt length

	const int mvi=bucket%kBoltVar;
	if (fds::FeatureFlags::bolt_geom()) drawBoltRibbon(src, targ, hw, bright, g_boltWig[mvi], g_boltWigN[mvi]);
	else                                drawBoltQuad(src, targ, g_boltTex[mvi], hw, bright, g_boltWig[mvi], g_boltWigN[mvi]);

	// Branches must VEER OFF at an angle — sending them along the main
	// direction (+ a little jitter) just drew parallel verticals next to
	// the bolt ("electric fence"). Direction is mostly LATERAL (a random
	// horizontal vector ~perpendicular to the descent) with only a little
	// continuation, so each fork clearly diverges and dead-ends in the air.
	const float invLen = len > 1e-3f ? 1.0f/len : 0.0f;
	const Vector ud = { d.x*invLen, d.y*invLen, d.z*invLen };   // unit main dir
	int nBranch = basePow/22; if (nBranch<1) nBranch=1; if (nBranch>3) nBranch=3;
	for (int b = 0; b < nBranch; ++b) {
		const float u = 0.30f + 0.45f*rng.f01();          // fork point along the bolt
		Vector base = { src.x + d.x*u, src.y + d.y*u, src.z + d.z*u };
		// lateral (horizontal) kick + a little forward continuation
		Vector lat = { rng.c(), 0.15f*rng.c(), rng.c() };
		const float ll = sqrtf(lat.x*lat.x + lat.y*lat.y + lat.z*lat.z);
		const float li = ll > 1e-3f ? 1.0f/ll : 0.0f;
		Vector bdir = { lat.x*li + ud.x*0.45f, lat.y*li + ud.y*0.45f, lat.z*li + ud.z*0.45f };
		const float bl = sqrtf(bdir.x*bdir.x + bdir.y*bdir.y + bdir.z*bdir.z);
		const float bi = bl > 1e-3f ? 1.0f/bl : 0.0f;
		const float blen = (0.22f + 0.22f*rng.f01()) * len;
		Vector end = { base.x + bdir.x*bi*blen, base.y + bdir.y*bi*blen, base.z + bdir.z*bi*blen };
		const int bvi=(bucket+b+1)%kBoltVar;
		if (fds::FeatureFlags::bolt_geom()) drawBoltRibbon(base, end, hw*0.55f, bright*0.75f, g_boltWig[bvi], g_boltWigN[bvi]);
		else                                drawBoltQuad(base, end, g_boltTex[bvi], hw*0.55f, bright*0.75f, g_boltWig[bvi], g_boltWigN[bvi]);
	}
}

// Dispatch-facing entry (Reserve state machine calls this each frame).
void Lightning_Bolt(Vector *Src,Vector *Targ,int32_t BasePow, Material* LnMat)
{
	RenderStrike(*Src, *Targ, BasePow, LnMat);
}

// Spire-ring arc: one stretched bolt quad, bucket-seeded per endpoint pair.
void Spark(Vector *Src,Vector *Targ, Material* /*LnMat*/)
{
	ensureBoltTextures();
	const int bucket = (int)(Timer / kRestrike);
	const uint32_t v = ((uint32_t)(bucket*2246822519u) ^ (uint32_t)(uintptr_t)Src) % kBoltVar;
	Vector d = { Targ->x-Src->x, Targ->y-Src->y, Targ->z-Src->z };
	const float len = sqrtf(d.x*d.x+d.y*d.y+d.z*d.z);
	const float hw  = fds::FeatureFlags::bolt_width() * 0.05f * len;
	const float sbr = boltFlicker() * fds::FeatureFlags::bolt_seg();
	if (fds::FeatureFlags::bolt_geom()) drawBoltRibbon(*Src, *Targ, hw, sbr, g_boltWig[v], g_boltWigN[v]);
	else                                drawBoltQuad(*Src, *Targ, g_boltTex[v], hw, sbr, g_boltWig[v], g_boltWigN[v]);
	Generate_Lightning_Particles(Targ, 10);
}


void Reserve_Lightning_Bolt(Vector *Src,Vector *Targ,int32_t Pow)
{
	// Drop into a free slot (until <= Timer), else evict the oldest — so a
	// burst can light several parallel bolts at once.
	int slot = 0; int32_t oldest = g_bolts[0].until;
	for (int i = 0; i < kMaxBolts; ++i) {
		if (g_bolts[i].until <= Timer) { slot = i; break; }
		if (g_bolts[i].until < oldest) { oldest = g_bolts[i].until; slot = i; }
	}
	g_bolts[slot].src   = *Src;
	g_bolts[slot].targ  = *Targ;
	g_bolts[slot].pow   = Pow;
	g_bolts[slot].until = Timer + kBoltLife;
}

// Draw every live bolt (Timer < until). Shared by the per-frame tick path and
// the snapshot harness (the override camera re-renders the scene but not the
// bolts, which draw at the tail of tick()).
void DrawActiveBolts()
{
	g_boltBudget = 1600 * kMaxBolts;   // shared across all live bolts this frame
	for (int i = 0; i < kMaxBolts; ++i)
		if (g_bolts[i].until > Timer)
			Lightning_Bolt(&g_bolts[i].src, &g_bolts[i].targ, g_bolts[i].pow, g_LnMat);
}
void Snapshot_DrawReservedBolt() { DrawActiveBolts(); }


void Handle_Lightning()
{
	Vector Upwards,Downwards;

	if (Ambient_Factor>basicSceneAmbient)
	{
		Ambient_Factor -= basicSceneAmbient;
		Ambient_Factor *= exp(-dTime*0.04);   // faster global flash decay
		Ambient_Factor += basicSceneAmbient;
	}
	// Flash light envelope — decays faster than the ambient so the strike pop
	// is short/snappy (--bolt_flash_decay, default 0.05 > the ambient's 0.04).
	if (g_BoltFlashInt > 0.0f) g_BoltFlashInt *= exp(-dTime*fds::FeatureFlags::bolt_flash_decay());
	// Scrub-aware PERIODIC strike triggers. A strike's time is a pure function
	// of its index i (base + i*interval + bounded per-index jitter), so the
	// whole schedule is deterministic — fires on the upward crossing of each
	// time, re-arms on rewind, scrubs naturally. interval is live-tunable
	// (--bolt_interval); spire + power are hashed per index for variety.
	const int32_t base     = FntLightningTime1;
	int32_t       interval = (int32_t)fds::FeatureFlags::bolt_interval();
	if (interval < 40) interval = 40;
	const int32_t lastT = FNTPartTime - 100;          // stop before the finale/scene end
	auto strikeTime = [&](int i)->int32_t {
		uint32_t h = (uint32_t)i * 2654435761u; h ^= h >> 15;
		const int32_t jit = (int32_t)(h % (uint32_t)interval) - interval/2;  // |jit| < interval/2 -> monotonic
		return base + i*interval + jit;
	};
	const int32_t now = Timer;
	if (g_LnLastTimer >= 0 && now > g_LnLastTimer) {
		int i0 = (g_LnLastTimer < base) ? 0 : (g_LnLastTimer - base)/interval;
		for (int i = i0; ; ++i) {
			const int32_t st = strikeTime(i);
			if (st > now) break;
			if (st <= g_LnLastTimer || st < base || st >= lastT) continue;
			uint32_t h = (uint32_t)i*2654435761u; h^=h>>15; h*=0x85EBCA6Bu; h^=h>>13;
			// Fire a BURST of 1..3 parallel bolts at different spires.
			const int nBurst = 1 + (int)(h % 3);
			for (int b = 0; b < nBurst; ++b) {
				uint32_t hb = h ^ ((uint32_t)(b+1) * 2654435761u);
				hb^=hb>>15; hb*=0x85EBCA6Bu; hb^=hb>>13;
				const int   pi = hb % 6;                          // random spire
				const int32_t pw = 45 + (int32_t)((hb >> 8) % 30);  // power 45..74
				Upwards.x = Pylon[pi].x+(RAND_15()-16384.0f)/16384.0f*120.0f;
				Upwards.y = Pylon[pi].y+600.0f;
				Upwards.z = Pylon[pi].z+(RAND_15()-16384.0f)/16384.0f*120.0f;
				// Strike DOWN to the spire TIP (Pylon is the tip omni position),
				// not the ground (which descended through the spire, z-occluded).
				Downwards = Pylon[pi];
				Reserve_Lightning_Bolt(&Upwards,&Downwards,pw);
				g_BoltFlashPos.x = (Upwards.x + Downwards.x) * 0.5f;
				g_BoltFlashPos.y = (Upwards.y + Downwards.y) * 0.5f;
				g_BoltFlashPos.z = (Upwards.z + Downwards.z) * 0.5f;
			}
			Ambient_Factor = 255.0f;
			g_BoltFlashInt  = 1.0f;
		}
	}
	g_LnLastTimer = now;
	// Cancel any bolt left in the future by a rewind (until = reserveT+life, so
	// until-now > life means we scrubbed back before it was reserved).
	for (int i = 0; i < kMaxBolts; ++i)
		if (g_bolts[i].until - now > kBoltLife + 5) g_bolts[i].until = -1;
	// Spire-ring finale window — also pulses the flash, but DIMMER than a main
	// strike (the ring is a low crackle, not a full strike).
	if (now > FntLightningTime6+100 && now < FntLightningTime6+400) {
		// Global light PULSATES with the ring crackle (was a steady 2x bump).
		const float pulse = (boltFlicker() - 0.70f) * (1.0f/0.30f);   // 0..1, per-frame
		Ambient_Factor = basicSceneAmbient * (1.2f + 1.3f*pulse);     // ~1.2x..2.5x
		Vector c = {0,0,0};
		for (int i = 0; i < 6; ++i) { c.x+=Pylon[i].x; c.y+=Pylon[i].y; c.z+=Pylon[i].z; }
		c.x/=6.0f; c.y/=6.0f; c.z/=6.0f;
		g_BoltFlashPos = c;
		g_BoltFlashInt = 0.12f + 0.15f*pulse;   // low pulsing crackle — lights the scene much less than a main strike
	}
}



/*DWord Photonbeam[64*128];
Word Noise[1800];
int32_t NoiseSampler = 0;

void Noise_Sample()
{
	Word *N = Noise;
	int32_t I = 1800;
	while (I--) *N++ = RAND_15();
}

void Iterate_Photonbeam_Texture()
{
	int32_t I,J;
	// first line
	DWord *Pen = Photonbeam + 64*127;
	for(I=0;I<=32;I++)
		*Pen++ = (155*I>>5)+(RAND_15()>>9)+32;
	for(I=32;I;I--)
		*Pen++ = (155*I>>5)+(RAND_15()>>9)+32;
	for(I=0;I<=32;I++)
		*Pen++ = (155*I>>5)+(RAND_15()>>9)+32;
	for(I=32;I;I--)
		*Pen++ = (155*I>>5)+(RAND_15()>>9)+32;

//	for(I=0;I<=32;I++)
//		*Pen++ = 96+(RAND_15()>>8);
//	for(I=32;I;I--)
//		*Pen++ = 96+(RAND_15()>>8);
//	for(I=0;I<=32;I++)
//		*Pen++ = 96+(RAND_15()>>8);
//	for(I=32;I;I--)
//		*Pen++ = 96+(RAND_15()>>8);


	Pen = Photonbeam;
	for(J=0;J<126;J++)
	{
		for(I=1;I<=62;I++)
		{
			// *Pen = (*Pen + Pen[63] + Pen[64] + Pen[65] + ((RAND_15()>>10)-14) )>>2;
			// *Pen = (*Pen + Pen[63] + Pen[64] + Pen[65] + (Noise[NoiseSampler++]>>10)-14 )>>2;
			*Pen = (Pen[64] + Pen[127] + Pen[128] + Pen[129] + (Noise[NoiseSampler++]>>10)-13 )>>2;
			Pen++;
		}
		if (NoiseSampler>1700) NoiseSampler-=1700;
		Pen+=2;
	}
}



void Blaze_Photon()
{
	DWord *Src = Photonbeam;
	DWord *Trg = Blaze;
	DWord S;
	int32_t T;
	int32_t I,J;
	// first 96 scanlines : convert B => RGB
	for(I=64;I;I--)
	{
		for(J=32;J;J--)
		{
			//T = 256;
			T = 512-(I*I>>3)-(J<<3);
			if (T<0) T=0;
			if (T>256) T=256;

			S = (*Src++)*T>>8;
			if (S<128) *Trg++ = (S<<1)+(S<<8)+((S>>1)<<16);
			else if (S<192) *Trg++ = 255+(S<<8)+((S>>1)<<16);
			else *Trg++ = 0xFFFF+(S<<16);

		}
		for(J=0;J<32;J++)
		{
			//256+I*I-J*8
			//T = 256;
			T = 512-(I*I>>3)-(J<<3);
			if (T<0) T=0;
			if (T>256) T=256;

			S = (*Src++)*T>>8;
			if (S<128) *Trg++ = (S<<1)+(S<<8)+((S>>1)<<16);
			else if (S<160) *Trg++ = 255+(S<<8)+((S>>1)<<16);
			else *Trg++ = 0xFFFF+(S<<16);
		}
	}

	for(I=0;I<64;I++)
	{
		for(J=32;J;J--)
		{
			//T = 256;
			T = 512-(I*I>>3)-(J<<3);
			if (T<0) T=0;
			if (T>256) T=256;

			S = (*Src++)*T>>8;
			if (S<128) *Trg++ = (S<<1)+(S<<8)+((S>>1)<<16);
			else if (S<192) *Trg++ = 255+(S<<8)+((S>>1)<<16);
			else *Trg++ = 0xFFFF+(S<<16);

		}
		for(J=0;J<32;J++)
		{
			//256+I*I-J*8
			//T = 256;
			T = 512-(I*I>>3)-(J<<3);
			if (T<0) T=0;
			if (T>256) T=256;

			S = (*Src++)*T>>8;
			if (S<128) *Trg++ = (S<<1)+(S<<8)+((S>>1)<<16);
			else if (S<192) *Trg++ = 255+(S<<8)+((S>>1)<<16);
			else *Trg++ = 0xFFFF+(S<<16);
		}
	}
}
*/
// let's do something different...




void Fire_Photon_Beam(Vector *Source,Vector *Target)
{
	// 3..2..1..FIRE
	// this should be a heavy-duty bombastic stuff,not a frag...
	//Lightning_Bolt_Frag(Source,Target,2);

	Vertex A,B;
	Vector V;

	Vector_Sub(Source,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&A.TPos_AOS);
	A.TPos_AOS.x = A.TPos_AOS.z * CntrX + A.TPos_AOS.x * FOVX;
	A.TPos_AOS.y = A.TPos_AOS.z * CntrY - A.TPos_AOS.y * FOVY;
	A.RZ = 1.0f/A.TPos_AOS.z;
	A.PX = A.TPos_AOS.x * A.RZ;
	A.PY = A.TPos_AOS.y * A.RZ;

	Vector_Sub(Target,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&B.TPos_AOS);
	B.TPos_AOS.x = B.TPos_AOS.z * CntrX + B.TPos_AOS.x * FOVX;
	B.TPos_AOS.y = B.TPos_AOS.z * CntrY - B.TPos_AOS.y * FOVY;
	B.RZ = 1.0f/B.TPos_AOS.z;
	B.PX = B.TPos_AOS.x * B.RZ;
	B.PY = B.TPos_AOS.y * B.RZ;

//	Iterate_Photonbeam_Texture();
//	Iterate_Photonbeam_Texture();
//	Blaze_Photon();

	int32_t Width = 3000.0f*A.RZ;
	The_Seven_UP_64(A.PX,(A.PY+B.PY)*0.5f,Width,fabs(B.PY-A.PY)*0.5f,Blaze,VPage);
}

Texture VorTexture;
DWord VorBuf[65536];
Material VortexMat;

void Load_Vortex()
{
	int32_t X,Y;
	VorTexture.FileName = strdup("TEXTURES/VORTEX.JPG");
	Identify_Texture(&VorTexture);
	if (!VorTexture.BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(&VorTexture);
	BPPConvert_Texture(&VorTexture,32);

	VorTexture.Flags = Txtr_Nomip;

	Material *M = &VortexMat;
	M->Txtr = &VorTexture;
	M->Luminosity = 2.f; // X2 saturated ambient
	M->Flags = Mat_RGBInterp | Mat_Additive;
	M->RelScene = FntSc;
	Material *Mat;
	for(Mat=MatLib;Mat->Next;Mat=Mat->Next);
	Mat->Next = M;
	memcpy(VorBuf,VorTexture.Data,65536*4);
}

void Vortex_Distort()
{
	int32_t I;
	GridPointT *Vortex_GP;
	DWord *TempBuf = new DWord[65536];
	Image VorImg;
	VorImg.x = 256;
	VorImg.y = 256;
	DWord *VorSurf = (DWord *)VorTexture.Data;
	VorImg.Data = VorSurf;
	int x,y,X,Y,i;
	float R1,R3,R4,sinr4,cosr4,u,v;

//	WOBPOINTSHEIGHT = 32;
	Vortex_GP = new GridPointT[33*33];
	// Run wobbler
	R1=-Timer*0.01;
	i=0;
	memcpy(VorSurf,VorBuf,65536*4);
	for (y=0;y<=256;y+=8)
		for (x=0;x<=256;x+=8,i++)
		{
			X = (x - 128)<<1;
			Y = (y - 128)<<1;
			R3 = sqrt((float)(X * X + Y * Y)) / (128.0*1.412);
			if (R3>0.85) R3 = 0.85;
			R4 = cos((R3)*5) + R1 - R3;
			sinr4 = sin(R4);
			cosr4 = cos(R4);
			u = x * 256 * 2;
			v = y * 256 * 2;
			Vortex_GP[i].u=sinr4*(v-32767)+cosr4*(u-32767)+32767;
			Vortex_GP[i].v=cosr4*(v-32767)-sinr4*(u-32767)+32767;
		}
//	Grid_Texture_Mapper_XXX(Vortex_GP,&VorImg,TempBuf);
	GridRendererT(Vortex_GP,&VorImg,TempBuf, 256, 256);

	// Circle-clip AND bake coverage into the alpha channel: texels outside the
	// inscribed circle are fully transparent (0); everything inside is stamped
	// opaque (alpha 0xFF). The portal's alpha-testing additive filler reads this
	// alpha to skip Z on the transparent corners — so the volumetric fog isn't
	// truncated behind them and no dark square frames the disc — while the swirl
	// still stamps Z and occludes the ship.
	for(Y=-128;Y<128;Y++)
		for(X=-128;X<128;X++)
			if (X*X+Y*Y>128*128)
				TempBuf[128+128*256+X+(Y<<8)] = 0;
			else
				TempBuf[128+128*256+X+(Y<<8)] |= 0xFF000000;

	// right now, there is no support for non-blocked textures,
	// so we need to rearrange TempBuf by 4x4 blocks
	//memcpy(VorSurf, TempBuf, 65536 * 4);
	dword *writePtr = VorSurf;
	for(X=0; X<64; X++)
		for(Y=0; Y<64; Y++)
		{
			dword *blockPtr = TempBuf + ((X + (Y<<8))<<2);
			for(y=0; y<4; y++)
				for(x=0; x<4; x++)
					*writePtr++ = blockPtr[x + (y<<8)];
		}

	delete [] TempBuf;
	delete [] Vortex_GP;
}

Object VortexObj;
TriMesh VortexTri;

void Add_Vortex_ToScene()
{
	Load_Vortex();
	Object *O = &VortexObj,*Obj;
	TriMesh *T = &VortexTri;

	memset(O, 0, sizeof(Object));
	memset(T, 0, sizeof(TriMesh));

	for(Obj=FntSc->ObjectHead;Obj->Next;Obj=Obj->Next);
	Obj->Next = O;
	O->Prev = Obj;
	O->Data = T;
	O->Type = Obj_TriMesh;

	TriMesh *Tri;
	for(Tri=FntSc->TriMeshHead;Tri->Next;Tri=Tri->Next);
	Tri->Next = T;
	T->Prev = Tri;

	// set rotation/position ptrs
	O->Rot =&T->RotMat;
	O->Pos =&T->IPos;
	Vector_Form(&O->Pivot, 0, 0, 0);

	T->VIndex = 4;
	T->Verts = new Vertex[4];
	const float vortex_size = 75.0;
	Vector_Form(&T->Verts[0].Pos,-vortex_size,0.0f,-vortex_size);
	Vector_Form(&T->Verts[1].Pos,-vortex_size,0.0f,vortex_size);
	Vector_Form(&T->Verts[2].Pos, vortex_size,0.0f,vortex_size);
	Vector_Form(&T->Verts[3].Pos, vortex_size,0.0f,-vortex_size);


	T->FIndex = 2;
	T->Faces = new Face[2];
	T->Faces[0].A = T->Verts;
	T->Faces[0].B = T->Verts+1;
	T->Faces[0].C = T->Verts+2;
	T->Faces[0].U1 = 0;
	T->Faces[0].V1 = 0;
	T->Faces[0].U2 = 0.99999;
	T->Faces[0].V2 = 0;
	T->Faces[0].U3 = 0.99999;
	T->Faces[0].V3 = 0.99999;
	T->Faces[0].Txtr = &VortexMat;
	T->Faces[1].U1 = 0;
	T->Faces[1].V1 = 0;
	T->Faces[1].U2 = 0.99999;
	T->Faces[1].V2 = 0.99999;
	T->Faces[1].U3 = 0;
	T->Faces[1].V3 = 0.99999;
	T->Faces[1].A = T->Verts;
	T->Faces[1].B = T->Verts+2;
	T->Faces[1].C = T->Verts+3;
	T->Faces[1].Txtr = &VortexMat;
	T->Pos.CurKey = 0;
	T->Pos.NumKeys = 1;
	T->Pos.Keys = new SplineKey[1];
	memset(T->Pos.Keys, 0, sizeof(SplineKey));
	Quaternion_Form(&T->Pos.Keys->Pos,Vortex_Center.x,Vortex_Center.y,Vortex_Center.z,0.0f);
	//T->IPos = Vortex_Center;
	T->Scale.CurKey = 0;
	T->Scale.NumKeys = 1;
	T->Scale.Keys = new SplineKey[1];
	Quaternion_Form(&T->Scale.Keys->Pos,1.0f,1.0f,1.0f,0.0f);
	T->Rotate.CurKey = 0;
	T->Rotate.NumKeys = 1;
	T->Rotate.Keys = new SplineKey[1];
	Quaternion_Form(&T->Rotate.Keys->Pos,0.0f,0.0f,0.0f,1.0f);
	Vector_Form(&T->BSphereCtr,0.0f,0.0f,0.0f);
	T->BSphereRad = 10000.0f;
	T->SortPriorityBias = SP_DrawFirst;
}
extern Scene* SkySc;

namespace {
struct FountainScene : SceneDriver {
	std::unique_ptr<byte[]> BPage;
	VESA_Surface Blur = {};
	int32_t timerStack[20] = {};
	int32_t timerIndex = 0;
	int32_t TTrd = 0;
	mword numFrames = 0;
	FrameProfiler prof{"fountain"};
	bool pause_mode = false;

	~FountainScene() override { Destroy_Scene(FntSc); }

	// Rebuild every per-resolution buffer the scene owns: blur backing
	// page, mirrored Blur surface, and the TBR span buffer (SBufferHead
	// is sized by YRes / TILESIZE). Called from init() so a resize that
	// fired before Fountain became active gets reflected, and from
	// on_resize() for mid-scene resizes.
	void rebuildSizedBuffers() {
		BPage = std::make_unique<byte[]>(PageSize);
		memcpy(&Blur, MainSurf, sizeof(VESA_Surface));
		Blur.Data = BPage.get();
		Blur.Flags = VSurf_Noalloc;
		Blur.Targ = VGAPtr;

		delete[] FntSc->SBuffer;
		delete[] FntSc->SBufferHead;
		FntSc->SBuffer = nullptr;
		FntSc->SBufferHead = nullptr;
		float typicalParticleSize = 30.0/384.0;
		dword typicalSpans = Fist(YRes * typicalParticleSize / 4.0);
		TBR_Init(FntSc, FntSc->NumOfParticles * typicalSpans);
	}

	void on_resize(int /*newX*/, int /*newY*/) override {
		rebuildSizedBuffers();
	}

	void init() override {
		int32_t Polys = 0;
		TriMesh *T;
		Omni *O;

		rebuildSizedBuffers();

		for(int32_t i = 0; i < 20; i++)
			timerStack[i] = Timer;

		SetCurrentScene(FntSc);
		Calibrate_FreeCamera_ForScene(FntSc->FZP, FntSc->CameraHead);
		for(T = FntSc->TriMeshHead; T; T = T->Next)
			Polys += T->FIndex;
		for(O = FntSc->OmniHead; O; O = O->Next)
			Polys++;
		Polys += FntSc->NumOfParticles;
		fds::g_mainFaces.resize(Polys);

		View = FntSc->CameraHead;

		TTrd = Timer;

		CParticle_ISize = 0.07f;
		Ambient_Factor = basicSceneAmbient;
		Diffusive_Factor = 500.0f;
		ImageSize = 10.0;

		g_renderedPolys = 0;
		FillerPixelcount = 0;
	}

	bool tick() override {
		// ESC = exit demo (handled by runSceneBlocking); Backspace = skip scene.
		if (Keyboard[ScBackSpace] || Timer >= FNTPartTime) return false;

		numFrames++;

		prof.beginFrame();
		prof.enter(PROF_ZCLR);

		clearFrame();

		prof.switchTo(PROF_ANIM);

		dTime = Timer - TTrd;
		// fast forward/rewind
		if (Keyboard[ScF2]) {
			Timer += dTime * 8;
		}
		if (Keyboard[ScF1]) {
			if (dTime * 8 > Timer)
				Timer = 0;
			else
				Timer -= dTime * 8;
		}
		if (Keyboard[ScP])
			pause_mode = true;
		if (Keyboard[ScU])
			pause_mode = false;
		if (pause_mode) {
			Timer = TTrd;
		}
		g_FrameTime = TTrd = Timer;

		Dynamic_Camera();
		if (Keyboard[ScC]) {FC.ISource = View->ISource; Matrix_Copy(FC.Mat,View->Mat); FC.IFOV=View->IFOV;}
		tickTabToggle(FntSc, "fountain");

		// SkyCube uses the main camera; render it AFTER Dynamic_Camera
		// so it tracks the current frame's view (previously was rendered
		// one frame stale because it ran before camera update).
		prof.switchTo(PROF_SKY);
		bool SkipCameraAnimation = false;
		RenderSkyCube(SkySc, View, SkipCameraAnimation);
		prof.switchTo(PROF_ANIM);

		// 'I' = info: print camera position + forward (rising edge only).
		{
			static bool prevI = false;
			bool curI = Keyboard[ScI] != 0;
			if (curI && !prevI) {
				fprintf(stderr,
					"[CAM] pos=(%.0f, %.0f, %.0f)  fwd=(%.3f, %.3f, %.3f)  IFOV=%.1f\n",
					View->ISource.x, View->ISource.y, View->ISource.z,
					View->Mat[2][0], View->Mat[2][1], View->Mat[2][2],
					View->IFOV);
			}
			prevI = curI;
		}

		Handle_Lightning();

		CurFrame = FntSc->StartFrame + (FntSc->EndFrame-FntSc->StartFrame) * (float)g_FrameTime / (float)FNTPartTime;

		if (Timer > FntVortexStart) {
			Vortex_Distort();
			float scl;
			if (Timer > FntVortexMax)
				scl = 1.0f + 0.1 * sin((Timer - FntVortexMax)*0.01);
			else
				scl = (float)(Timer-FntVortexStart)/(float)(FntVortexMax-FntVortexStart);
			Quaternion_Form(&VortexTri.Scale.Keys->Pos, scl, scl, scl, 0.0f);
		}

		Animate_Objects(FntSc, View);

		if (Timer > FntVortexStart) {
			VortexTri.Flags |= HTrack_Visible;
		} else VortexTri.Flags &= 0xFFFFFFFF-HTrack_Visible;

		Particle_Kinematics(FntSc);

		// Update stream-head omnis from inner-spawn emit math.
		{
			const float magwav          = 55.0f;
			const float angularVelocity = 0.008f;
			const float t               = float(Timer);
			const float ax              = std::cos(t * angularVelocity * 0.8f);
			const float az              = std::sin(t * angularVelocity * 0.8f);
			for (int i = 0; i < kNumFollowerOmnis; ++i) {
				FollowerOmni& f = g_FntFollowerOmnis[i];
				if (!f.omni) continue;
				const float yPhase = std::cos(t * 0.01f * 0.8f
					+ float(f.track) * (2.0f * float(PI) / 3.0f)) * 0.3f;
				float x = ax * magwav;
				float y = 35.0f + yPhase * 55.0f;
				float z = az * magwav;
				// xForm1/xForm2 are 120° rotations; spawn falls case-1
				// into case-2 so:
				//   rotIdx 0 -> identity
				//   rotIdx 1 -> xForm1 then xForm2
				//   rotIdx 2 -> xForm2 only
				if (f.rotIdx == 1) {
					float nx = x * xForm1x + z * xForm1z;
					float nz = z * xForm1x - x * xForm1z;
					x = nx; z = nz;
				}
				if (f.rotIdx >= 1) {
					float nx = x * xForm2x + z * xForm2z;
					float nz = z * xForm2x - x * xForm2z;
					x = nx; z = nz;
				}
				f.omni->IPos.x = x;
				f.omni->IPos.y = y;
				f.omni->IPos.z = z;
			}
		}

		// Drive the strike-flash omni from the Ambient_Factor decay (255 on a
		// strike, eased back to basicSceneAmbient). After Animate so the spline
		// doesn't overwrite IPos/ISize; before Lighting/Render so the deferred
		// light list + froxel fog in-scatter pick it up — lightning lighting
		// the scene and the fog volume.
		if (g_BoltFlashOmni) {
			const float fi = g_BoltFlashInt;   // own fast-decay envelope
			if (fi > 0.02f) {
				const float frange = fds::FeatureFlags::bolt_flash_range();
				g_BoltFlashOmni->Flags  |= Omni_Active;
				g_BoltFlashOmni->IPos    = g_BoltFlashPos;
				g_BoltFlashOmni->ISize   = fds::FeatureFlags::bolt_flash_peak() * fi;
				g_BoltFlashOmni->IRange  = frange;
				g_BoltFlashOmni->rRange  = 1.0f / frange;
			} else {
				g_BoltFlashOmni->Flags &= ~Omni_Active;
				g_BoltFlashOmni->ISize  = 0.0f;
			}
		}

		prof.switchTo(PROF_XFRM);

		Transform_Objects(FntSc, fds::g_mainCamera, fds::g_mainFaces);

		prof.switchTo(PROF_LGHT);

		Lighting(FntSc);

		prof.leave(PROF_LGHT);

		if (!CAll) return true;
		prof.enter(PROF_SORT);
		Radix_Sort(FList, SList, CAll);
		prof.switchTo(PROF_RNDR);
		Render();
		// Attribute the deferred-skybox tile cost (timed inside
		// Render_DeferredSkybox via an atomic) back to PROF_SKY so
		// the on-screen overlay matches what the forward path used
		// to show. No-op when --deferred-skybox is off.
		prof.addExternalNs(PROF_SKY, DeferredSkybox_TakeFrameNs());

		// Fold the transparent peel's depth into the main Z so the bolt (drawn
		// next, additive) is occluded by translucent surfaces (the glass dome)
		// too — not just opaque. No-op outside the deferred xpar path.
		Deferred_FoldXparDepthIntoMainZ();

		// All live (parallel) bolts. Each holds for kBoltLife Timer units;
		// RenderStrike regenerates the fractal path only on restrike-bucket
		// changes, so each holds steady + flickers across its window.
		DrawActiveBolts();
		// Spire-ring finale: a ring of arcs crackling between consecutive spire
		// TOPS, re-randomized every frame for the ~3s window.
		if (Timer > FntLightningTime6+100 && Timer < FntLightningTime6+400) {
			g_boltBudget = 2400;
			for (int pi = 0; pi < 6; ++pi)
				Spark(Pylon + pi, Pylon + ((pi + 1) % 6), g_LnMat);
		}
		// Debug labels at each stream-head omni's screen position.
		for (int i = 0; i < kNumFollowerOmnis; ++i) {
			const FollowerOmni& f = g_FntFollowerOmnis[i];
			const Omni* O = f.omni;
			if (!O) continue;
			if (O->V.TPos_AOS.z < FntSc->NZP || O->V.TPos_AOS.z > FntSc->FZP) continue;
			int px = int(O->V.PX);
			int py = int(O->V.PY);
			if (px < 0 || px >= XRes || py < 0 || py >= YRes) continue;
			char label[32];
			std::snprintf(label, sizeof(label), "H%d/%d", f.track, f.rotIdx);
			OutTextXY(VPage, px, py, label, 255);
		}

		prof.leave(PROF_RNDR);

		if (g_profilerActive) {
			int scroll = 0;
			// FPS printer — sourced from FrameProfiler (steady_clock) so it
			// agrees with TOTL/mean_fps in the scene-end dump.
			float meanMs = prof.meanFrameMs();
			float fps = meanMs > 0 ? 1000.0f / meanMs : 0.0f;
			snprintf(MSGStr, sizeof(MSGStr), "%f FPS", fps);
			scroll = OutTextXY(VPage, 0, 0, MSGStr, 255);

			fds::RenderStats_Flush();
			prof.noteCounters((double)g_renderedPolys, (double)FillerPixelcount);
			scroll = prof.drawOverlay(scroll);
		}
		prof.enter(PROF_FLIP);

		// Dump screenshots at specific timestamps for debugging
		// transparent triangle hole. Set FNT_DUMP_DIR=/path env to enable.
		if (const char* dumpDir = std::getenv("FNT_DUMP_DIR")) {
			static int dumpCounter = 0;
			++dumpCounter;
			// Dump every 100 frames
			if (dumpCounter % 100 == 0) {
				char path[1024];
				snprintf(path, sizeof(path), "%s/fnt_f%05d.ppm", dumpDir, dumpCounter);
				dbg_write_ppm(path, MainSurf->Data, XRes, YRes, MainSurf->BPSL);
			}
		}

		Flip(MainSurf);
		prof.leave(PROF_FLIP);
		prof.endFrame();
		return true;
	}

	void cleanup() override {
		if (g_profilerActive) prof.dump();
		Timer -= FNTPartTime;
		waitBackspaceRelease();
		if (Timer < 0) Timer = 0;
	}
};
} // anonymous namespace

std::unique_ptr<SceneDriver> createFountainScene()
{
	return std::make_unique<FountainScene>();
}

void Run_Fountain()
{
	auto scene = createFountainScene();
	runSceneBlocking(*scene);
}
