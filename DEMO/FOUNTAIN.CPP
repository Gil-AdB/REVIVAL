#include "Rev.h"

#include "FOUNTAIN.H"
#include "VESA/Vesa.h"
#include "Gradient.h"
#include "FRUSTRUM.H"
#include "Clipper.h"

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

static int32_t LightningFlag = -1;

static char MSGStr[160];

static DWord Blaze[64*128];


static Vector Rsv_LSrc,Rsv_LTarg;
static int32_t Rsv_LPow;
static int32_t Rsv_LState = 0;
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
		P->V.Pos.x += ((float)(RAND_15()-16384)/16384.0f)*FntSpawnOutDisp;
		P->V.Pos.z += ((float)(RAND_15()-16384)/16384.0f)*FntSpawnOutDisp;
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
		MatrixXVector(View->Mat,&V,&P->V.TPos);

		if (P->V.TPos.z>=1.0)
		{
			P->V.RZ = 1.0/P->V.TPos.z;
			P->V.PX = CntrX+FOVX*P->V.TPos.x*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos.y*P->V.RZ;
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

			P->Flags = Particle_Active;
		}
	}

	for(P=Sc->Pcl+FntOuterPcls,PE=P+FntInnerPcls*3;P<PE;P++)
	{
		if (!P->Flags) continue;
		Vector_Scale(&P->Vel,dt,&V);
		Vector_SelfAdd(&P->V.Pos,&V);

		Vector_Sub(&P->V.Pos,&View->ISource,&V);
		MatrixXVector(View->Mat,&V,&P->V.TPos);

		if (P->V.TPos.z>=1.0)
		{
			P->V.RZ = 1.0/P->V.TPos.z;
			P->V.PX = CntrX+FOVX*P->V.TPos.x*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos.y*P->V.RZ;
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
			MatrixXVector(View->Mat,&V,&P->V.TPos);

			if (P->V.TPos.z>=1.0)
			{
				P->V.RZ = 1.0/P->V.TPos.z;
				P->V.PX = CntrX+FOVX*P->V.TPos.x*P->V.RZ;
				P->V.PY = CntrY-FOVY*P->V.TPos.y*P->V.RZ;
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
//	FntSc = new Scene; memset(FntSc,0,sizeof(Scene));
	FntSc = (Scene *)getAlignedBlock(sizeof(Scene), 16);
	memset(FntSc,0,sizeof(Scene));

	LoadFLD(FntSc,"SCENES/Fountain.FLD");

	for(M = MatLib;M;M=M->Next)
	{
		if (M->RelScene != FntSc) continue;
		M->Flags |= Mat_RGBInterp;
	}
	Add_Vortex_ToScene();

	FntSc->Ambient.B = 64;
	FntSc->Ambient.G = 64;
	FntSc->Ambient.R = 64;
	FntSc->Ambient.A = 64;
	FntSc->NZP = 20.0f;
	FntSc->FZP = 5000.0f;
	FntSc->Flags |= Scn_SpriteTBR;

	Image Img;
	for(M = MatLib;M;M=M->Next)
	{
		if (M->RelScene != FntSc) continue;
/*		if (!M->Txtr) continue;
		for(Material *Mat = MatLib;Mat!=M;Mat=Mat->Next)
			if (M->Txtr==Mat->Txtr) break;
		if (M!=Mat) continue;

		Convert_Texture2Image(M->Txtr,&Img);
		delete [] M->Txtr->Data; M->Txtr->Data = NULL;
		Gamma_Correction(&Img,1.2f);
		Convert_Image2Texture(&Img,M->Txtr);
		FilterLSB_Texture(M->Txtr);
		delete Img.Data;*/
		M->Flags |= Mat_RGBInterp;
		if (M->Txtr)
			M->Txtr->Flags |= Txtr_Tiled;
	}

	Initialize_Particles(FntSc);
	Preprocess_Scene(FntSc);

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

	std::vector<GradientEndpoint> endpoints;
	endpoints.emplace_back(0, Color{ 0.0, 0.0, 0.0, 0.0 });
	endpoints.emplace_back(0.1, Color{ 0.2, 0.0, 0.0, 0.0 });
	endpoints.emplace_back(0.7, Color{ 0.4, 0.0, 0.1, 0.0 });
	endpoints.emplace_back(0.75, Color{ 1.0, 0.0, 0.1, 0.0 });
	endpoints.emplace_back(0.8, Color{ 1.0, 0.4, 0.8, 0.0 });
	endpoints.emplace_back(0.85, Color{ 1.0, 1.0, 1.0, 0.0 });
	endpoints.emplace_back(1.0, Color{ 1.0, 1.0, 1.0, 0.0 });

	g_LnMat = Generate_Gradient(endpoints, 256, 0.01, false);

//	Noise_Sample();

//	for(I=0;I<128;I++)
//		Iterate_Photonbeam_Texture();

//	Generate_Photon_Plasma();
}


///////////////////////////
// LINE CLIPPER
///////////////////////
void UFP1(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));
void UFP2(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));
void UFP3(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));
void UFP4(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2));

void Bivex_Frustrum_Perspective(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	Vertex *T;
	Vertex Vtx1;
	float L,R;

	if (A->TPos.z>B->TPos.z) {T=A; A=B; B=T;}

	if (B->TPos.z<1) return;
	if (A->TPos.z<1)
	{
		L=(A->TPos.z-1)/(A->TPos.z-B->TPos.z);
		R=1-L;
		Vtx1.PX = CntrX+FOVX*(A->TPos.x*R + B->TPos.x*L);
		Vtx1.PY = CntrY-FOVY*(A->TPos.y*R + B->TPos.y*L);

		Vtx1.LR = A->LR*R + B->LR*L;

		UFP1(&Vtx1,B,Filler);
		return;
	}
	UFP1(A,B,Filler);
}

// heh heh, ZOL.
void UFP1(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	Vertex *T;
	Vertex Vtx1;
	float Y1,Y2;
	float L,R;
	float FDiv;

	if (A->PX>B->PX) {T=A; A=B; B=T;}
	if (B->PX<0) return;
	if (A->PX<0)
	{
		L = A->PX/(A->PX-B->PX);   //ZOL
		if (L<-0.01||L>1.01) {
//			printf("UFC1:Abnormal Interpolation rate (A=(%f,%f,%f),B=(%f,%f,%f),Y1=%f,Y2=%f,L=%f)",A->TPos.x,A->TPos.z,A->TPos.y,B->TPos.x,B->TPos.z,B->TPos.y,Y1,Y2,L);
			exit(1);
		}
		R = 1-L;
		Vtx1.PX = 0; // CntrX + FOVX*Vtx1.TPos.x/Vtx1.TPos.z;
		Vtx1.PY = A->PY*R + B->PY*L;
		Vtx1.LR = A->LR*R + B->LR*L;

		UFP2(&Vtx1,B,Filler);
		return;
	}
	UFP2(A,B,Filler);
}

void UFP2(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	Vertex *T;
	Vertex Vtx1;
	float Y1,Y2;
	float L,R;
	float XClip = XRes_1;

	if (A->PX>XClip) return;
	if (B->PX>XClip)
	{
		R = (B->PX-XClip)/(B->PX-A->PX);  // ZOL
		if (R<-0.01||R>1.01) {
//			printf("UFC2:Abnormal Interpolation rate (A=(%f,%f,%f),B=(%f,%f,%f),Y1=%f,Y2=%f,R=%f)",A->TPos.x,A->TPos.z,A->TPos.y,B->TPos.x,B->TPos.z,B->TPos.y,Y1,Y2,R);
			exit(1);
		}
		L = 1-R;
		Vtx1.PX = XClip; // CntrX + FOVX*Vtx1.TPos.x/Vtx1.TPos.z;
		Vtx1.PY = A->PY*R + B->PY*L; //Vtx1.TPos.y/Vtx1.TPos.z;
		Vtx1.LR = A->LR*R + B->LR*L;

		UFP3(A,&Vtx1,Filler);
		return;
	}
	UFP3(A,B,Filler);
}

void UFP3(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	Vertex *T;
	Vertex Vtx1;
	float Y1,Y2;
	float L,R;
	float FDiv;

	if (A->PY>B->PY) {T=A; A=B; B=T;}
	if (B->PY<0) return;
	if (A->PY<0)
	{
		L = A->PY/(A->PY-B->PY);   //  ZOL
		if (L<-0.01||L>1.01) {
//			printf("UFC3:Abnormal Interpolation rate (A=(%f,%f,%f)|(%f,%f),B=(%f,%f,%f)|(%f,%f),Y1=%f,Y2=%f,L=%f)",A->TPos.x,A->TPos.z,A->TPos.y,A->PX,A->PY,B->TPos.x,B->TPos.z,B->TPos.y,B->PX,B->PY,Y1,Y2,L);
			exit(1);
		}
		R = 1-L;
		Vtx1.PX = A->PX*R + B->PX*L;//Vtx1.TPos.x/Vtx1.TPos.z;
		Vtx1.PY = 0;//CntrY - FOVY*Vtx1.TPos.y/Vtx1.TPos.z;

		Vtx1.LR = A->LR*R + B->LR*L;

		UFP4(&Vtx1,B,Filler);
		return;
	}
	UFP4(A,B,Filler);
}

void UFP4(Vertex *A,Vertex *B,void (*Filler)(Vertex *V1,Vertex *V2))
{
	Vertex *T;
	Vertex Vtx1;
	float Y1,Y2;
	float L,R;
	float YClip = YRes_1;

	if (A->PY>YClip) return;
	if (B->PY>YClip)
	{
		R = (B->PY-YClip)/(B->PY-A->PY);   //ZOL
		if (R<-0.01||R>1.01) {
//			printf("UFC4:Abnormal Interpolation rate (A=(%f,%f,%f)|(%f,%f),B=(%f,%f,%f)|(%f,%f),Y1=%f,Y2=%f,R=%f)",A->TPos.x,A->TPos.z,A->TPos.y,A->PX,A->PY,B->TPos.x,B->TPos.z,B->TPos.y,B->PX,B->PY,Y1,Y2,R);
			exit(1);
		}
		L = 1-R;
		Vtx1.PX = A->PX*R + B->PX*L;
		Vtx1.PY = YClip;// CntrY - FOVY*Vtx1.TPos.y/Vtx1.TPos.z;

		Vtx1.LR = A->LR*R + B->LR*L;

		Filler(A,&Vtx1);
		return;
	}
	Filler(A,B);
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
	End=Ptr+X2+YOffs[Y2];
	Ptr+=X1+YOffs[Y1];

	*Ptr = 0xFFFFFF;
//	 memset(Ptr,255,depth<<2);

	if (Dx > Dy) {
		Ai = (Dy-Dx)<<1;
		Bi = Dy<<1;
		D = Bi-Dx;
		while (Ptr != End) {
			if (D >= 0) {
				Ptr += Yi;
				D += Ai;
			} else D = D + Bi;
			Ptr += Xi;
			*Ptr = 0xFFFFFF;
//			memset(Ptr,255,depth<<2);
		}
	} else {
		Ai = (Dx-Dy)<<1;
		Bi = Dx<<1;
		D = Bi-Dy;
		while (Ptr != End) {

			if (D >= 0) {
				Ptr += Xi;
				D += Ai;
			} else D += Bi;
			Ptr += Yi;

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
	End=Ptr+X2+YOffs[Y2];
	Ptr+=X1+YOffs[Y1];

	*Ptr = 0xFFFFFF;
//	 memset(Ptr,255,depth<<2);

	if (Dx > Dy) {
		Ai = (Dy-Dx)<<1;
		Bi = Dy<<1;
		D = Bi-Dx;
		while (Ptr != End) {
			if (D >= 0) {
				Ptr += Yi;
				D += Ai;
			} else D = D + Bi;
			Ptr += Xi;
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
		while (Ptr != End) {

			if (D >= 0) {
				Ptr += Xi;
				D += Ai;
			} else D += Bi;
			Ptr += Yi;

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


void Lightning_Bolt_Frag(Vector *Src,Vector *Targ,int32_t POW, Material* LnMat)
{
//	memset(VPage,255,PageSize);
	if (POW<1) POW=1;
	Vertex A,B;
	Vector V;

	Vector_Sub(Src,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&A.TPos);
	A.TPos.x = A.TPos.z * CntrX + A.TPos.x * FOVX;
	A.TPos.y = A.TPos.z * CntrY - A.TPos.y * FOVY;
	A.RZ = 1.0f/A.TPos.z;
	A.PX = A.TPos.x * A.RZ;
	A.PY = A.TPos.y * A.RZ;

	Vector_Sub(Targ,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&B.TPos);
	B.TPos.x = B.TPos.z * CntrX + B.TPos.x * FOVX;
	B.TPos.y = B.TPos.z * CntrY - B.TPos.y * FOVY;
	B.RZ = 1.0f/B.TPos.z;
	B.PX = B.TPos.x * B.RZ;
	B.PY = B.TPos.y * B.RZ;

	if (A.TPos.z < C_NZP || B.TPos.z < C_NZP) {
		return;
	}

	float dx = B.PX - A.PX;
	float dy = B.PY - A.PY;
	float rlen = 1.0f / sqrt(dx * dx + dy * dy);
	dx *= rlen;
	dy *= rlen;
	float nx = -dy;
	float ny = dx;
	Vertex quad[4];
	float lenA = POW * 2.0 * A.RZ;
	float lenB = POW * 2.0 * B.RZ;
	quad[0].PX = A.PX - lenA * FOVX * nx;
	quad[0].PY = A.PY - lenA * (-FOVY) * ny;
	quad[0].UZ = 0;
	quad[0].VZ = 0;
	quad[0].RZ = A.RZ;

	quad[1].PX = A.PX + lenA * FOVX * nx;
	quad[1].PY = A.PY + lenA * (-FOVY) * ny;
	quad[1].UZ = A.RZ;
	quad[1].VZ = 0;
	quad[1].RZ = A.RZ;

	quad[2].PX = B.PX + lenB * FOVX * nx;
	quad[2].PY = B.PY + lenB * (-FOVY) * ny;
	quad[2].UZ = B.RZ;
	quad[2].VZ = B.RZ;
	quad[2].RZ = B.RZ;

	quad[3].PX = B.PX - lenB * FOVX * nx;
	quad[3].PY = B.PY - lenB * (-FOVY) * ny;
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
	f[0].Filler = IX_Prefiller_TGZSAM;
	f[0].Txtr = LnMat;

	f[1].A = &quad[0];
	f[1].B = &quad[2];
	f[1].C = &quad[3];
	f[1].Filler = IX_Prefiller_TGZSAM;
	f[1].Txtr = LnMat;

	_2DClipper::getInstance()->clip(vp, f[0]);
	_2DClipper::getInstance()->clip(vp, f[1]);

/*	if (POW>1)
		Bivex_Frustrum_Perspective(&A,&B,Lightning_Line);
	else
		Bivex_Frustrum_Perspective(&A,&B,Spark_Line);*/
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
		MatrixXVector(View->Mat,&V,&P->V.TPos);

		if (P->V.TPos.z>=1.0)
		{
			P->V.RZ = 1.0/P->V.TPos.z;
			P->V.PX = CntrX+FOVX*P->V.TPos.x*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos.y*P->V.RZ;
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

void Lightning_Bolt(Vector *Src,Vector *Targ,int32_t BasePow, Material* LnMat)
{
	//ok, now we need to run from Src to Targ...
	float Distance,Seg;
	Vector Pos,V,NPos,Yosi;
	Vector_Copy(&Pos,Src);
	do
	{
		// randomize direction
		Vector_Sub(Targ,&Pos,&V);
		Vector_Norm(&V);
		V.x += ((float)(RAND_15()-16384))*0.5f/16384.0f;
		V.y += ((float)(RAND_15()-16384))*0.5f/16384.0f;
		V.z += ((float)(RAND_15()-16384))*0.5f/16384.0f;
		Vector_Norm(&V);
		Seg=30.0f+RAND_15()*30.0f/32768.0f;
		Vector_SelfScale(&V,Seg);
		Vector_Add(&Pos,&V,&NPos);
		Lightning_Bolt_Frag(&Pos,&NPos,BasePow>>4, LnMat);
		Vector_Copy(&Pos,&NPos);
		Vector_Sub(Targ,&Pos,&V);
		Distance = Vector_Length(&V);
		if (Distance>60.0f&&Distance<400.0f&&(RAND_15()<320*BasePow-Distance*15))
		{
			Yosi.y = 0;
			Yosi.x = Pos.x;
			Yosi.z = Pos.z;
			Lightning_Bolt(&Pos,&Yosi,BasePow>>1, LnMat);
		}
	} while (Distance>60.0f);
	// attach pos with target

//	Generate_Lightning_Particles(&Pos,50);
}

void Spark(Vector *Src,Vector *Targ, Material* LnMat)
{
	//ok, now we need to run from Src to Targ...
	float Distance,Seg;
	Vector Pos,V,NPos,Yosi;
	Vector_Copy(&Pos,Src);
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
		Lightning_Bolt_Frag(&Pos,&NPos,1, LnMat);
		Vector_Copy(&Pos,&NPos);
		Vector_Sub(Targ,&Pos,&V);
		Distance = Vector_Length(&V);
	} while (Distance>30.0f);
	// attach pos with target
	Lightning_Bolt_Frag(&Pos,Targ,1, LnMat);

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
	switch (LightningFlag)
	{
		case 0:
			if (Timer>FntLightningTime1)
			{
				LightningFlag = 1;
				Upwards.x = Pylon->x+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Upwards.y = Pylon->y+1000.0f;
				Upwards.z = Pylon->z+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Downwards.x = Pylon->x;
				Downwards.y = 0.0f;
				Downwards.z = Pylon->z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,80);
				Ambient_Factor = 255.0f;
			}
		break;
		case 1:
			if (Timer>FntLightningTime2)
			{
				LightningFlag = 2;
				Upwards.x = Pylon[1].x+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Upwards.y = Pylon[1].y+1000.0f;
				Upwards.z = Pylon[1].z+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Downwards.x = Pylon[1].x;
				Downwards.y = 0.0f;
				Downwards.z = Pylon[1].z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,80);
				Ambient_Factor = 255.0f;
			}
		break;
		case 2:
			if (Timer>FntLightningTime3)
			{
				LightningFlag = 3;
				Upwards.x = Pylon[2].x+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Upwards.y = Pylon[2].y+1000.0f;
				Upwards.z = Pylon[2].z+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Downwards.x = Pylon[2].x;
				Downwards.y = 0.0f;
				Downwards.z = Pylon[2].z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,80);
				Ambient_Factor = 255.0f;
			}
		break;
		case 3:
			if (Timer>FntLightningTime4)
			{
				LightningFlag = 4;
				Upwards.x = Pylon[3].x+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Upwards.y = Pylon[3].y+1000.0f;
				Upwards.z = Pylon[3].z+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Downwards.x = Pylon[3].x;
				Downwards.y = 0.0f;
				Downwards.z = Pylon[3].z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,80);
				Ambient_Factor = 255.0f;
			}
		break;
		case 4:
			if (Timer>FntLightningTime5)
			{
				LightningFlag = 5;
				Upwards.x = Pylon[4].x+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Upwards.y = Pylon[4].y+1000.0f;
				Upwards.z = Pylon[4].z+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Downwards.x = Pylon[4].x;
				Downwards.y = 0.0f;
				Downwards.z = Pylon[4].z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,80);
				Ambient_Factor = 255.0f;
			}
		break;
		case 5:
			if (Timer>FntLightningTime6)
			{
				LightningFlag = 6;
				Upwards.x = Pylon[5].x+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Upwards.y = Pylon[5].y+1000.0f;
				Upwards.z = Pylon[5].z+(RAND_15()-16384.0f)/16384.0f*75.0f;
				Downwards.x = Pylon[5].x;
				Downwards.y = 0.0f;
				Downwards.z = Pylon[5].z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,80);
				Ambient_Factor = 255.0f;
			}
		break;
		case 6:
			if (Timer>FntLightningTime6+100)
			{
				if (Timer>FntLightningTime6+400)
					LightningFlag = 7;

//				Reserve_Lightning_Bolt(NULL,NULL,-1); //special code
				Reserve_Lightning_Bolt(&Upwards,&Downwards,-1);
				Ambient_Factor = basicSceneAmbient*2.0;
			}
		break;
		default:
		break;
	}
	FntSc->Ambient.R = Ambient_Factor;
	FntSc->Ambient.G = Ambient_Factor;
	FntSc->Ambient.B = Ambient_Factor;
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
	MatrixXVector(View->Mat,&V,&A.TPos);
	A.TPos.x = A.TPos.z * CntrX + A.TPos.x * FOVX;
	A.TPos.y = A.TPos.z * CntrY - A.TPos.y * FOVY;
	A.RZ = 1.0f/A.TPos.z;
	A.PX = A.TPos.x * A.RZ;
	A.PY = A.TPos.y * A.RZ;

	Vector_Sub(Target,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&B.TPos);
	B.TPos.x = B.TPos.z * CntrX + B.TPos.x * FOVX;
	B.TPos.y = B.TPos.z * CntrY - B.TPos.y * FOVY;
	B.RZ = 1.0f/B.TPos.z;
	B.PX = B.TPos.x * B.RZ;
	B.PY = B.TPos.y * B.RZ;

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
	VorTexture.FileName = strdup("Textures/Vortex.JPG");
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

void Run_Fountain()
{
	int32_t Polys = 0;
	TriMesh *T;
	Omni *O;
	int32_t TTrd;
	Vector U,V;

	byte *BPage = new byte[PageSize];

#ifdef _C_WATCOM
#define VSurface Screen
#else
#define VSurface MainSurf
#endif
	VESA_Surface Blur;
	memcpy(&Blur,VSurface,sizeof(VESA_Surface));
	Blur.Data = BPage;
	Blur.Flags = VSurf_Noalloc;
	Blur.Targ = VGAPtr;

	int32_t i, timerStack[20], timerIndex = 0;
	for(i=0; i<20; i++)
		timerStack[i] = Timer;

//Setup_Grid_Texture_Mapper_XXX(256, 256);

	SetCurrentScene(FntSc);
	for(T = FntSc->TriMeshHead;T;T=T->Next)
		Polys+=T->FIndex;
	for(O = FntSc->OmniHead;O;O=O->Next)
		Polys++;
	Polys += FntSc->NumOfParticles;
	FList = new Face * [Polys];
	SList = new Face * [Polys];


	View = FntSc->CameraHead;
//	View = &FC;

	//Timer = 4000;
	TTrd = Timer;
	mword numFrames = 0;

	CParticle_ISize = 0.07f;
	Ambient_Factor = basicSceneAmbient;
	Diffusive_Factor = 500.0f;
	ImageSize = 10.0;

	g_renderedPolys = 0;
	FillerPixelcount = 0;

	dword Profiler[PROF_NUM];
	float ProfSample[PROF_NUM];
	float ProfPerc[PROF_NUM];
	const char *ProfNames[] = {"ZCLR","ANIM","XFRM", "LGHT", "SORT", "RNDR", "FLIP"};
	dword ProfSum = 0;

	for(i=0; i<PROF_NUM; i++)
		Profiler[i] = 0;	

	while ((!Keyboard[ScESC])&&Timer<FNTPartTime)
	{
		numFrames++;

		Profiler[PROF_ZCLR] -= Timer;

//		memset(VPage, 0, PageSize + XRes*YRes*sizeof(word));
		FastWrite(VPage, 0, (PageSize + XRes*YRes*sizeof(word)) >> 2);
		bool SkipCameraAnimation = false;
		RenderSkyCube(SkySc, View, SkipCameraAnimation);

		dTime = Timer-TTrd;
		// fast forward/rewind
		if (Keyboard[ScF2])
		{
			Timer += dTime * 8;
		}
		if (Keyboard[ScF1])
		{
			if (dTime * 8 > Timer)
				Timer = 0;
			else
				Timer -= dTime * 8;
		}
		static bool pause_mode = false;
		if (Keyboard[ScP])
			pause_mode = true;
		if (Keyboard[ScU])
			pause_mode = false;
		if (pause_mode)
		{
			Timer = TTrd;
		}
		g_FrameTime = TTrd = Timer;

		Profiler[PROF_ZCLR] += Timer;
		Profiler[PROF_ANIM] -= Timer;

		Dynamic_Camera();
		if (Keyboard[ScC]) {FC.ISource = View->ISource; Matrix_Copy(FC.Mat,View->Mat); FC.IFOV=View->IFOV;}
		if (Keyboard[ScTab])
		{
			if (View == &FC)
				View = FntSc->CameraHead;
			else
				View = &FC;
		}

		Handle_Lightning();

		CurFrame = FntSc->StartFrame + (FntSc->EndFrame-FntSc->StartFrame) * (float)g_FrameTime / (float)FNTPartTime;

		if (Timer>FntVortexStart)
		{
			Vortex_Distort();
			float scl;
			if (Timer>FntVortexMax)
				scl = 1.0f + 0.1 * sin((Timer - FntVortexMax)*0.01);
			else
				scl = (float)(Timer-FntVortexStart)/(float)(FntVortexMax-FntVortexStart);
			Quaternion_Form(&VortexTri.Scale.Keys->Pos,scl,scl,scl,0.0f);
			//Quaternion_Form(&VortexTri.Scale.Keys->Pos,1.0,1.0,1.0,0.0f);
			
//			Vector_Form(&VortexTri.Verts[0].Pos,-150.0f*scl,0.0f,-150.0f*scl);
//			Vector_Form(&VortexTri.Verts[1].Pos,-150.0f*scl,0.0f, 150.0f*scl);
//			Vector_Form(&VortexTri.Verts[2].Pos, 150.0f*scl,0.0f, 150.0f*scl);
//			Vector_Form(&VortexTri.Verts[3].Pos, 150.0f*scl,0.0f,-150.0f*scl);
		}

		Animate_Objects(FntSc);

//		float xfov = View->IFOV;
//		float zf = tan(xfov*PI/360.0);
//		float yfov = 360.0/PI * atan(zf*3.0/4.0);

		if (Timer>FntVortexStart)
		{
			VortexTri.Flags |= HTrack_Visible;
		} else VortexTri.Flags &= 0xFFFFFFFF-HTrack_Visible;

		Particle_Kinematics(FntSc);
		Profiler[PROF_ANIM] += Timer;		
		Profiler[PROF_XFRM] -= Timer;		

		Transform_Objects(FntSc);

		Profiler[PROF_XFRM] += Timer;		
		Profiler[PROF_LGHT] -= Timer;		

		Lighting(FntSc);

		Profiler[PROF_LGHT] += Timer;		

		if (!CAll) continue;
		Profiler[PROF_SORT] -= Timer;		
		Radix_SortingASM(FList,SList,CAll);
		Profiler[PROF_SORT] += Timer;		

//		Run_Rain();

		Profiler[PROF_RNDR] -= Timer;		
		Render();

//		sprintf(MSGStr,"Rendered %d Polys,%d Omnis and %d pcls.\n",CPolys,COmnies,CPcls);
//		OutTextXY(VPage,0,0,MSGStr,64);

		if (Rsv_LState)
		{
			if (Rsv_LPow>-1)
			{
				Lightning_Bolt(&Rsv_LSrc,&Rsv_LTarg,Rsv_LPow, g_LnMat);
				Rsv_LState = 0;
			} else {
//				Spark(Pylon  ,Pylon+1);
//				Spark(Pylon+1,Pylon+2);
//				Spark(Pylon+2,Pylon+3);
//				Spark(Pylon+3,Pylon+4);
//				Spark(Pylon+4,Pylon+5);
//				Spark(Pylon+5,Pylon  );
				Rsv_LState = 0;
			}
		}
		Profiler[PROF_RNDR] += Timer;		

/*		if (Timer>FntPhotonStart)
		{
			if (Timer>FntPhotonAtVortex)
				Fire_Photon_Beam(&FntHead,&Vortex_Center);
			else
			{
				Vector_Sub(&Vortex_Center,&FntHead,&V);
				Vector_SelfScale(&V,(float)(Timer-FntPhotonStart)/(float)(FntPhotonAtVortex-FntPhotonStart));
				Vector_SelfAdd(&V,&FntHead);
				Fire_Photon_Beam(&FntHead,&V);
			}
		}*/
		if (g_profilerActive)
		{
			mword scroll = 0;
			// FPS printer
			timerStack[timerIndex++] = Timer;
			if (timerIndex==20) 
			{
				timerIndex = 0;
				sprintf(MSGStr,"%f FPS", 2000.0/(float)(timerStack[19]-timerStack[timerIndex]) );
			} else {
				sprintf(MSGStr,"%f FPS", 2000.0/(float)(timerStack[timerIndex-1]-timerStack[timerIndex]) );
			}
			scroll = OutTextXY(VPage,0,0,MSGStr,255);
			sprintf(MSGStr,"%d outer pcls", g_numPcls);
			sprintf(MSGStr, "%dK pixels/frame" , (int32_t)(FillerPixelcount/(1000.0*numFrames)));
			scroll = OutTextXY(VPage,0,scroll+15,MSGStr,255);
			sprintf(MSGStr, "%dK pixels/second" , (int32_t)(FillerPixelcount/1000.0 * 100.0 / Profiler[PROF_RNDR]));
			scroll = OutTextXY(VPage,0,scroll+15,MSGStr,255);

			sprintf(MSGStr, "%d polys/frame" , (int32_t)(g_renderedPolys / numFrames) );
			scroll = OutTextXY(VPage,0,scroll+15,MSGStr,255);

	//		PROFILER: display stats
			for(i=0; i<PROF_NUM; i++)
			{
				sprintf(MSGStr, "%s %3.1fms (%3.1f%%)" , ProfNames[i], ProfSample[i], ProfPerc[i]);
				scroll = OutTextXY(VPage,0,scroll+15,MSGStr,255);
			}

			sprintf(MSGStr, "TOTL %3.1fms" , ProfSum*10.0/ numFrames);
			scroll = OutTextXY(VPage,0,scroll+15,MSGStr,255);
		}
		Profiler[PROF_FLIP] -= Timer;		
//		Lightning_Particles();

		Flip(VSurface);
		//Modulate(VSurface,&Blur,0x808080,0x808080);
		//Flip(&Blur);
		Profiler[PROF_FLIP] += Timer;		


//		PROFILER: Generate sample
		ProfSum = 0;
		for(i=0; i<PROF_NUM; i++)
			ProfSum += Profiler[i];
		for(i=0; i<PROF_NUM; i++)
		{
			ProfSample[i] =
				Profiler[i]*10.0 / numFrames;
			ProfPerc[i] = 
				Profiler[i]*100.0/ProfSum;
		}
	} 
	Timer-=FNTPartTime;
	while (Keyboard[ScESC]) continue;
	if (Timer < 0) Timer = 0;

	FILE *F = fopen("fountain-profiling.txt", "wt");
	for(i=0; i<PROF_NUM; i++)
	{
		fprintf(F, "%s %3.1fms (%3.1f%%)\n" , ProfNames[i], ProfSample[i], ProfPerc[i]);
	}
	fclose(F);
	
	delete FList;
	delete SList;
	delete [] BPage;
	Destroy_Scene(FntSc);
}
