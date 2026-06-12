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


static Vector Rsv_LSrc,Rsv_LTarg;
static int32_t Rsv_LPow;
static int32_t Rsv_LState = 0;
static int32_t Rsv_LUntil = -1;   // strike TTL (Timer units); -1 = unarmed
const float basicSceneAmbient = 64.0;

//static Vector *Stars;

static Particle *LBoltPcls;

static Image PhotonPlasma;

static dword g_numPcls;

static Material* g_LnMat;

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
	// center automatically gets the core. (A pre-symmetrized table gets
	// double-folded into twin stripes.) Edges are black because the
	// faces draw ADDITIVE (a bolt is light): black adds nothing, which
	// also gives free soft anti-aliased edges. vSlack darkens the v≈0/1
	// caps — correct now that V is continuous along the WHOLE ribbon
	// (per-segment V made every segment render as a capped pill: the
	// 'ladder rungs').
	std::vector<GradientEndpoint> endpoints;
	endpoints.emplace_back(0.00, Color{ 0.0,  0.0,  0.0,  0.0 });
	endpoints.emplace_back(0.45, Color{ 0.25, 0.0,  0.06, 0.0 });
	endpoints.emplace_back(0.78, Color{ 1.0,  0.30, 0.60, 0.0 });
	endpoints.emplace_back(0.92, Color{ 1.0,  1.0,  1.0,  0.0 });
	endpoints.emplace_back(1.00, Color{ 1.0,  1.0,  1.0,  0.0 });

	g_LnMat = Generate_Gradient(endpoints, 256, 0.04, false);

	// Reset scrub tracking + any reserved strike (re-init safe).
	g_LnLastTimer = -1;
	Rsv_LState = 0;

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

	*Ptr = 0xFFFFFF;
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

	*Ptr = 0xFFFFFF;
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
static void Lightning_Ribbon(const Vector* pts, int n, int32_t POW, Material* LnMat)
{
	if (POW < 1) POW = 1;
	constexpr int kMax = 160;
	if (n > kMax) n = kMax;
	if (n < 2) return;
	static float px[kMax], py[kMax], rzv[kMax];
	static bool  vis[kMax];
	Vertex T;
	Vector V;
	for (int i = 0; i < n; ++i) {
		Vector_Sub(const_cast<Vector*>(pts + i), &View->ISource, &V);
		MatrixXVector(View->Mat, &V, &T.TPos_AOS);
		vis[i] = T.TPos_AOS.z >= C_NZP;
		if (!vis[i]) continue;
		const float r = 1.0f / T.TPos_AOS.z;
		px[i] = (T.TPos_AOS.z * CntrX + T.TPos_AOS.x * FOVX) * r;
		py[i] = (T.TPos_AOS.z * CntrY - T.TPos_AOS.y * FOVY) * r;
		rzv[i] = r;
	}
	// Joint perpendiculars from central differences over visible runs —
	// a depth-ward middle segment inherits its neighbors' direction
	// instead of flipping the ribbon sideways.
	static float nxv[kMax], nyv[kMax];
	for (int i = 0; i < n; ++i) {
		if (!vis[i]) continue;
		const int ip = (i > 0   && vis[i-1]) ? i-1 : i;
		const int in_ = (i < n-1 && vis[i+1]) ? i+1 : i;
		float dx = px[in_] - px[ip];
		float dy = py[in_] - py[ip];
		const float l2 = dx*dx + dy*dy;
		if (l2 < 1e-4f) {
			// isolated/depth-ward: reuse previous joint's perp (or down)
			nxv[i] = (i > 0 && vis[i-1]) ? nxv[i-1] : 1.0f;
			nyv[i] = (i > 0 && vis[i-1]) ? nyv[i-1] : 0.0f;
			continue;
		}
		const float rl = 1.0f / sqrtf(l2);
		nxv[i] = -dy * rl;
		nyv[i] =  dx * rl;
	}
	// Continuous V along the whole ribbon (cumulative screen length,
	// normalized): the pill texture's dark caps land only at the bolt's
	// two ENDS instead of at every segment boundary.
	static float vcum[kMax];
	float total = 0.0f;
	vcum[0] = 0.0f;
	for (int i = 1; i < n; ++i) {
		float dl = 0.0f;
		if (vis[i] && vis[i-1]) {
			const float ddx = px[i]-px[i-1], ddy = py[i]-py[i-1];
			dl = sqrtf(ddx*ddx + ddy*ddy);
		}
		total += dl;
		vcum[i] = total;
	}
	const float invTotal = total > 1.0f ? 1.0f/total : 1.0f;
	for (int i = 0; i < n; ++i) vcum[i] *= invTotal;
	Viewport vp;
	viewportInit(vp, FntSc);
	const auto rt   = fds::MainRenderTargetFromGlobals();
	const auto& cam = fds::g_mainCamera;
	for (int i = 0; i + 1 < n; ++i) {
		if (!vis[i] || !vis[i+1]) continue;
		if (--g_boltBudget < 0) return;
		// World half-width projected to px, clamped sane at 1080p.
		float hA = POW * 1.6f * rzv[i]   * FOVX;
		float hB = POW * 1.6f * rzv[i+1] * FOVX;
		if (hA < 1.2f) hA = 1.2f; if (hA > 14.0f) hA = 14.0f;
		if (hB < 1.2f) hB = 1.2f; if (hB > 14.0f) hB = 14.0f;
		Vertex quad[4];
		quad[0].PX = px[i]   - hA*nxv[i];   quad[0].PY = py[i]   - hA*nyv[i];
		quad[0].UZ = 0;                     quad[0].VZ = vcum[i]*rzv[i];
		quad[0].RZ = rzv[i];
		quad[1].PX = px[i]   + hA*nxv[i];   quad[1].PY = py[i]   + hA*nyv[i];
		quad[1].UZ = rzv[i];                quad[1].VZ = vcum[i]*rzv[i];
		quad[1].RZ = rzv[i];
		quad[2].PX = px[i+1] + hB*nxv[i+1]; quad[2].PY = py[i+1] + hB*nyv[i+1];
		quad[2].UZ = rzv[i+1];              quad[2].VZ = vcum[i+1]*rzv[i+1];
		quad[2].RZ = rzv[i+1];
		quad[3].PX = px[i+1] - hB*nxv[i+1]; quad[3].PY = py[i+1] - hB*nyv[i+1];
		quad[3].UZ = 0;                     quad[3].VZ = vcum[i+1]*rzv[i+1];
		quad[3].RZ = rzv[i+1];
		for (int k = 0; k != 4; ++k) {
			Vertex& v = quad[k];
			v.LA = v.LR = v.LG = v.LB = 255.0;
			viewportCalcFlags(vp, &v);
		}
		Face f[2];
		f[0].A = &quad[0]; f[0].B = &quad[1]; f[0].C = &quad[2];
		f[0].Filler = (TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL>);
		f[0].Txtr = LnMat;
		f[1].A = &quad[0]; f[1].B = &quad[2]; f[1].C = &quad[3];
		f[1].Filler = (TheOtherBarry<barry::TBlendMode::ADDITIVE, barry::TTextureMode::NORMAL>);
		f[1].Txtr = LnMat;
		_2DClipper::getInstance()->clip(vp, f[0], rt, cam);
		_2DClipper::getInstance()->clip(vp, f[1], rt, cam);
		// Hot core over the ribbon.
		Vertex A, B;
		A.PX = px[i];   A.PY = py[i];   A.RZ = rzv[i];
		A.TPos_AOS.z = 1.0f / rzv[i];
		B.PX = px[i+1]; B.PY = py[i+1]; B.RZ = rzv[i+1];
		B.TPos_AOS.z = 1.0f / rzv[i+1];
		A.LR = B.LR = float(POW);
		if (POW > 1)
			Bivex_Frustrum_Perspective(&A, &B, Lightning_Line);
		else
			Bivex_Frustrum_Perspective(&A, &B, Spark_Line);
	}
}

void Lightning_Bolt(Vector *Src,Vector *Targ,int32_t BasePow, Material* LnMat)
{
	// Walk Src->Targ gathering the path, then draw ONE connected ribbon.
	float Distance,Seg;
	Vector Pos,V,NPos,Yosi;
	Vector pts[160];
	int np = 0;
	Vector_Copy(&Pos,Src);
	pts[np++] = Pos;
	do
	{
		// randomize direction. Uniform 30-60u segments deflected up to
		// ~30 deg EVERY step read as a mechanical staircase at 1080p
		// (fine at the 1998 320x240). Real bolts run mostly straight
		// with occasional hard kinks: shorter segments, mild deviation,
		// 20% chance of a sharp kink.
		Vector_Sub(Targ,&Pos,&V);
		Vector_Norm(&V);
		{
			const float amp = (RAND_15() < 6553) ? 0.55f : 0.18f;
			V.x += ((float)(RAND_15()-16384))*amp/16384.0f;
			V.y += ((float)(RAND_15()-16384))*amp/16384.0f;
			V.z += ((float)(RAND_15()-16384))*amp/16384.0f;
		}
		Vector_Norm(&V);
		Seg=16.0f+RAND_15()*16.0f/32768.0f;
		Vector_SelfScale(&V,Seg);
		Vector_Add(&Pos,&V,&NPos);
		Vector_Copy(&Pos,&NPos);
		if (np < 160) pts[np++] = Pos;
		Vector_Sub(Targ,&Pos,&V);
		Distance = Vector_Length(&V);
		if (g_boltBudget>0&&Distance>60.0f&&Distance<400.0f&&(RAND_15()<320*BasePow-Distance*15))
		{
			Yosi.y = 0;
			Yosi.x = Pos.x;
			Yosi.z = Pos.z;
			Lightning_Bolt(&Pos,&Yosi,BasePow>>1, LnMat);
		}
	} while (Distance>60.0f&&g_boltBudget>0&&np<160);

	Lightning_Ribbon(pts, np, BasePow>>4, LnMat);
}

void Spark(Vector *Src,Vector *Targ, Material* LnMat)
{
	// Same gather-then-ribbon structure as Lightning_Bolt (per-segment
	// frags render the pill texture as dashes — see Lightning_Ribbon).
	float Distance,Seg;
	Vector Pos,V,NPos;
	Vector pts[160];
	int np = 0;
	Vector_Copy(&Pos,Src);
	pts[np++] = Pos;
	do
	{
		// randomize direction
		Vector_Sub(Targ,&Pos,&V);
		Vector_Norm(&V);
		V.x += ((float)(RAND_15()-16384))*0.3f/16384.0f;
		V.y += ((float)(RAND_15()-16384))*0.3f/16384.0f;
		V.z += ((float)(RAND_15()-16384))*0.3f/16384.0f;
		Vector_Norm(&V);
		Seg=15.0f+RAND_15()*15.0f/32768.0f;
		Vector_SelfScale(&V,Seg);
		Vector_Add(&Pos,&V,&NPos);
		Vector_Copy(&Pos,&NPos);
		if (np < 159) pts[np++] = Pos;
		Vector_Sub(Targ,&Pos,&V);
		Distance = Vector_Length(&V);
	} while (Distance>30.0f && g_boltBudget>0 && np<159);
	// attach pos with target
	pts[np++] = *Targ;
	Lightning_Ribbon(pts, np, 1, LnMat);

	Generate_Lightning_Particles(Targ,10);
}


void Reserve_Lightning_Bolt(Vector *Src,Vector *Targ,int32_t Pow)
{
	Vector_Copy(&Rsv_LSrc,Src);
	Vector_Copy(&Rsv_LTarg,Targ);
	Rsv_LPow = Pow;
	Rsv_LState = 1;
}


void Handle_Lightning()
{
	Vector Upwards,Downwards;

	if (Ambient_Factor>basicSceneAmbient)
	{
		Ambient_Factor -= basicSceneAmbient;
		Ambient_Factor *= exp(-dTime*0.02);
		Ambient_Factor += basicSceneAmbient;
	}
	// Scrub-aware strike triggers (replaces the one-way LightningFlag
	// state machine, which never re-armed: F1/F2 rewind and pause made
	// strikes undebuggable). A strike fires on the UPWARD crossing of its
	// time; rewinding below a time re-arms it. The spire-ring finale is a
	// pure Timer window, so it scrubs naturally in both directions.
	static const int32_t strikeT[6] = {
		FntLightningTime1, FntLightningTime2, FntLightningTime3,
		FntLightningTime4, FntLightningTime5, FntLightningTime6,
	};
	static const int32_t strikeP[6] = { 45, 50, 55, 60, 65, 70 };   // 1998 escalation
	const int32_t now = Timer;
	if (g_LnLastTimer >= 0 && now > g_LnLastTimer) {
		for (int i = 0; i < 6; ++i) {
			if (g_LnLastTimer < strikeT[i] && now >= strikeT[i]) {
				Upwards.x = Pylon[i].x+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Upwards.y = Pylon[i].y+1000.0f;
				Upwards.z = Pylon[i].z+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Downwards.x = Pylon[i].x;
				Downwards.y = 0.0f;
				Downwards.z = Pylon[i].z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,strikeP[i]);
				Ambient_Factor = 255.0f;
			}
		}
	}
	g_LnLastTimer = now;
	// A strike rewound out from under its TTL is cancelled (the TTL is
	// Timer+20 at reserve time; anything 25+ below it means we scrubbed).
	if (Rsv_LState && Rsv_LUntil > 0 && now < Rsv_LUntil - 25) {
		Rsv_LState = 0;
		Rsv_LUntil = -1;
	}
	// Spire-ring finale window.
	if (now > FntLightningTime6+100 && now < FntLightningTime6+400) {
		Reserve_Lightning_Bolt(&Upwards,&Downwards,-1);
		Ambient_Factor = basicSceneAmbient*2.0;
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

	// change this to a radial smooth fade to black
	for(Y=-128;Y<128;Y++)
		for(X=-128;X<128;X++)
			if (X*X+Y*Y>128*128)
				TempBuf[128+128*256+X+(Y<<8)] = 0;

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

		if (Rsv_LState) {
			if (Rsv_LPow > -1) {
				// Stretch the strike over ~200ms: redraw with fresh
				// randomness every frame (the crackle IS the look) until
				// the TTL passes. 1998 drew a single frame — at modern
				// framerates that reads as a barely-there flicker.
				if (Rsv_LUntil < 0) Rsv_LUntil = Timer + 20;
				g_boltBudget = 900;            // ~main bolt + a dozen branches
				Lightning_Bolt(&Rsv_LSrc, &Rsv_LTarg, Rsv_LPow, g_LnMat);
				if (Timer >= Rsv_LUntil) { Rsv_LState = 0; Rsv_LUntil = -1; }
			} else {
				// The 1998 finale (commented out even in the original):
				// a ring of arcs crackling between consecutive spire
				// TOPS, re-randomized every frame for the ~3s window
				// (case 6 keeps re-reserving with Pow=-1).
				g_boltBudget = 900;
				for (int pi = 0; pi < 6; ++pi)
					Spark(Pylon + pi, Pylon + ((pi + 1) % 6), g_LnMat);
				Rsv_LState = 0;
				Rsv_LUntil = -1;
			}
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
