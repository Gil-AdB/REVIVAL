#include "Rev.H"

#include "Fountain.H"

static Scene *FntSc;
static Vector FntHead = {-0.053f,43.381f, 0.251f};
static Vector FntSrc = {-0.672,-3.957,-0.721};
static Vector FntSpring = {-0.261,85.106,-0.159};

static Vector LBSrc = {0.0f,1000.0f,0.0f};
static Vector Pylon[6];

static float xForm1x = -0.5f;
static float xForm1z = sqrt(3.0f)/2.0f;
static float xForm2x = -0.5f;
static float xForm2z = sqrt(3.0f)/2.0f;

static Vector FntDir = { 0.000f, 1.000f, 0.000f};

const long FNTPartTime = 50.00 * 100;
static Material *PclT;
static DWord NPMats;
static float SwCarry = 0.0f;

static Vector Vortex_Center = {-0.053f,343.381f, 0.251f};

static long LightningFlag = 0;

static char MSGStr[160];

static DWord Blaze[64*128];


static Vector Rsv_LSrc,Rsv_LTarg;
static long Rsv_LPow;
static long Rsv_LState = 0;

//static Vector *Stars;

static Particle *LBoltPcls;

static Image PhotonPlasma;

void Particle_Rast(Vertex *A,Vertex *B,Vertex *C)
{
	char *Pixel;
	long I,J;
	if (A->PX>=1&&A->PX<=XRes-2&&A->PY>=1&&A->PY<=YRes_1-1)
	{
		//addative 32bit 5 putpixels (currently set on 1)
		Pixel = VPage+((int)A->PX+YOffs[(int)A->PY]<<2);
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
	long I,J;
	Image Img;

	Sc->NumOfParticles = FntInnerPcls*3 + FntOuterPcls + FntSpiralPcls;
	Sc->Pcl = new Particle[Sc->NumOfParticles];
	memset(Sc->Pcl,0,sizeof(Particle)*Sc->NumOfParticles);

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
	PclT = new Material[2];
	PclT[0].Txtr = new Texture;
	PclT[0].Txtr->BPP = 32;
	PclT[1].Txtr = new Texture;
	PclT[1].Txtr->BPP = 32;
	Img.X = 256;
	Img.Y = 256;
	Img.Data = new DWord[65536];
	Generate_Flare_Image(&Img,0.2f,0.3f,1.0f);
	Convert_Image2Texture(&Img,PclT[0].Txtr);
	Generate_Flare_Image(&Img,0.1f,0.8f,1.0f);
	Convert_Image2Texture(&Img,PclT[1].Txtr);
	delete Img.Data;

	// Install Particles
	for(I=0;I<FntOuterPcls;I++)
	{
		// Physical Quantities.
		Sc->Pcl[I].Grav = 7.5;
		Sc->Pcl[I].Mass = 0.02;
		Sc->Pcl[I].Charge = 6.5;
		Sc->Pcl[I].Radius = 0.0;
		// Particle Position.
		Sc->Pcl[I].V.Pos.X=FntHead.X;
		Sc->Pcl[I].V.Pos.Y=FntHead.Y;
		Sc->Pcl[I].V.Pos.Z=FntHead.Z;
		// Converegance Vectors set at starting position.
//		Vector_Copy(&Sc->Pcl[I].Conv,&Sc->Pcl[I].V.Pos);
		// Particle Virtual Face -> Particle Vertex
		Sc->Pcl[I].F.A=Sc->Pcl[I].F.B=Sc->Pcl[I].F.C=&Sc->Pcl[I].V;
		switch (BPP)
		{
			case 8:  Sc->Pcl[I].F.Filler = &Dark_Sprite; break;
			case 32: Sc->Pcl[I].F.Filler = &The_Seven_UP_HALFSIZE; break;
		}
		Sc->Pcl[I].F.Txtr = PclT+I%NPMats;

		// Particle Velocity
//		F1=(float)rand()/32768.0;
//		F2=(float)rand()/4096.0;
//		Sc->Pcl[I].Vel.X=(2.0+F2)*cos(F1);
//	Sc->Pcl[I].Vel.Y=(0.5+F2*0.5)*sin(F1);
//		Sc->Pcl[I].Vel.Z=1.0*cos(F1);
//		Vector_SelfScale(&Sc->Pcl[I].Vel,0.05);
		Sc->Pcl[I].Vel.X=0;
		Sc->Pcl[I].Vel.Y=0;
		Sc->Pcl[I].Vel.Z=0;
	}

	for(I=FntOuterPcls;I<FntSpiralPclsOff;I++)
	{
		// Physical Quantities.
		Sc->Pcl[I].Grav = 7.5;
		Sc->Pcl[I].Mass = 0.02;
		Sc->Pcl[I].Charge = 4.5;
		Sc->Pcl[I].Radius = 0.0;
		// Particle Position.
		Sc->Pcl[I].V.Pos.X=FntHead.X;
		Sc->Pcl[I].V.Pos.Y=FntHead.Y-3.0f;
		Sc->Pcl[I].V.Pos.Z=FntHead.Z;
		// Converegance Vectors set at starting position.
//		Vector_Copy(&Sc->Pcl[I].Conv,&Sc->Pcl[I].V.Pos);
		// Particle Virtual Face -> Particle Vertex
		Sc->Pcl[I].F.A=Sc->Pcl[I].F.B=Sc->Pcl[I].F.C=&Sc->Pcl[I].V;
		switch (BPP)
		{
			case 8:  Sc->Pcl[I].F.Filler = &Dark_Sprite; break;
			case 32: Sc->Pcl[I].F.Filler = &Particle_Rast; break;
		}

		// Particle Velocity
//		F1=(float)rand()/32768.0;
//		F2=(float)rand()/4096.0;
//		Sc->Pcl[I].Vel.X=(2.0+F2)*cos(F1);
//	Sc->Pcl[I].Vel.Y=(0.5+F2*0.5)*sin(F1);
//		Sc->Pcl[I].Vel.Z=1.0*cos(F1);
//		Vector_SelfScale(&Sc->Pcl[I].Vel,0.05);
		Sc->Pcl[I].Vel.X=0;
		Sc->Pcl[I].Vel.Y=0;
		Sc->Pcl[I].Vel.Z=0;
	}
	for(I=FntSpiralPclsOff;I<Sc->NumOfParticles;I++)
	{
		// Physical Quantities.
		Sc->Pcl[I].Grav = 7.5;
		Sc->Pcl[I].Mass = 0.02;
		Sc->Pcl[I].Charge = 4.5;
		Sc->Pcl[I].Radius = 0.0;
		// Particle Position.
		Sc->Pcl[I].V.Pos.X=FntHead.X;
		Sc->Pcl[I].V.Pos.Y=FntHead.Y-3.0f;
		Sc->Pcl[I].V.Pos.Z=FntHead.Z;
		// Converegance Vectors set at starting position.
//		Vector_Copy(&Sc->Pcl[I].Conv,&Sc->Pcl[I].V.Pos);
		// Particle Virtual Face -> Particle Vertex
		Sc->Pcl[I].F.A=Sc->Pcl[I].F.B=Sc->Pcl[I].F.C=&Sc->Pcl[I].V;
		switch (BPP)
		{
			case 8:  Sc->Pcl[I].F.Filler = &Dark_Sprite; break;
			case 32: Sc->Pcl[I].F.Filler = &Particle_Rast; break;
		}

		// Particle Velocity
//		F1=(float)rand()/32768.0;
//		F2=(float)rand()/4096.0;
//		Sc->Pcl[I].Vel.X=(2.0+F2)*cos(F1);
//	Sc->Pcl[I].Vel.Y=(0.5+F2*0.5)*sin(F1);
//		Sc->Pcl[I].Vel.Z=1.0*cos(F1);
//		Vector_SelfScale(&Sc->Pcl[I].Vel,0.05);
		Sc->Pcl[I].Vel.X=0;
		Sc->Pcl[I].Vel.Y=0;
		Sc->Pcl[I].Vel.Z=0;
	}
}


void Particle_Kinematics(Scene *Sc)
{
	float u,v,cv;
	// Destroy particles
	Particle *P,*PE;
	Vector V;
	Vector Omit,OmitDer,X,Y;

	float dt = 0.01*dTime;
	float expdt = exp(dt*0.3f);
	float x;

	float magwav = 55.0f;//75.0f + 20.0f*cos(Timer*0.005);

	long I,r;

	for(P=Sc->Pcl,PE = P + FntOuterPcls;P<PE;P++)
	{
		if (!P->Flags) continue;
		if (P->V.Pos.Y<0.0f) P->Flags = 0; //kill pcl
	}
	// Create random particles
	float Sw = dt * FntSpawnOutPcl + SwCarry;
	float pow = FntSpawnOutPow + sin(Timer*FntSpawnOutPowFreq)*FntSpawnOutPowTilt,fpow;
	long Spwn = Sw;
	SwCarry = Sw-Spwn;

//	Spwn = 0;
	while(Spwn--)
	{
		fpow = pow + (float)rand()/32768.0f * FntSpawnOutPowRand;
		// search for a place to insert
		for(P=Sc->Pcl;P<PE;P++)
			if (!P->Flags) break;
		if (P==PE) break;
		//insert
		Vector_Copy(&P->V.Pos,&FntSrc);
		P->V.Pos.X += ((float)(rand()-16384)/16384.0f)*FntSpawnOutDisp;
		P->V.Pos.Z += ((float)(rand()-16384)/16384.0f)*FntSpawnOutDisp;

		u = ((float)rand()/32768.0f)*PI_M2;
		v = ((float)rand()/32768.0f)*PI*0.15f+PI*0.30f;
		cv = cos(v);
		P->Vel.X = cv * cos(u) * fpow;
		P->Vel.Y = sin(v) * fpow;
		P->Vel.Z = cv * sin(u) * fpow;

		P->Color.R = 63;
		P->Color.G = 127;
		P->Color.B = 255;

		P->Flags = Particle_Active;
	}
	// Animate Particles
	for(P=Sc->Pcl;P<PE;P++)
	{
		if (!P->Flags) continue;
		if (P->Flags&2)
		{
			P->Vel.Y -= dt * FntSpawnOutGrav;
			Vector_Scale(&P->Vel,dt,&V);
			Vector_SelfAdd(&P->V.Pos,&V);
		} else {
			if (P->V.Pos.Y>83.106) P->Flags|=2;
			P->V.Pos.Y += Vector_Length(&P->Vel);
		}

		Vector_Sub(&P->V.Pos,&View->ISource,&V);
		MatrixXVector(View->Mat,&V,&P->V.TPos);

		if (P->V.TPos.Z>=1.0)
		{
			P->V.RZ = 1.0/P->V.TPos.Z;
			P->V.PX = CntrX+FOVX*P->V.TPos.X*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos.Y*P->V.RZ;
//      P->PRad = 0;//FOVX*P->Radius*RZ;
			P->V.Flags = 0;
		} else P->V.Flags |= Vtx_VisNear;

		// dummy illumination model
		P->V.LB = P->Color.B*0.5;
		P->V.LG = P->Color.G*0.5;
		P->V.LR = P->Color.R*0.5;

	}


	/// INNER PARTICLES
	for(PE += FntInnerPcls*3;P<PE;P++)
	{
		if (!P->Flags) continue;
		P->Charge-=dt;
		if (P->Charge<0.0f) P->Flags = 0; //kill pcl
	}

	for(I=0;I<3;I++)
	{
		switch(I)
		{
			case 0:
				// omit particles from base altitude 10 radius 30
				OmitDer.X = cos(Timer*0.004*0.8);
				OmitDer.Y = cos(Timer*0.01*0.8)*0.3;
				OmitDer.Z = sin(Timer*0.004*0.8);

				Omit.X = OmitDer.X*magwav;
				Omit.Y = 35.0f+OmitDer.Y*55.0f;
				Omit.Z = OmitDer.Z*magwav;
			break;
			case 1:
				OmitDer.X = cos(Timer*0.004*0.8);
				OmitDer.Y = cos(Timer*0.01*0.8+2.0*PI/3.0f)*0.3;
				OmitDer.Z = sin(Timer*0.004*0.8);

				Omit.X = OmitDer.X*magwav;
				Omit.Y = 35.0f+OmitDer.Y*55.0f;
				Omit.Z = OmitDer.Z*magwav;
			break;
			case 2:
				OmitDer.X = cos(Timer*0.004*0.8);
				OmitDer.Y = cos(Timer*0.01*0.8+4.0f*PI/3.0f)*0.3;
				OmitDer.Z = sin(Timer*0.004*0.8);

				Omit.X = OmitDer.X*magwav;
				Omit.Y = 35.0f+OmitDer.Y*55.0f;
				Omit.Z = OmitDer.Z*magwav;
			break;
		}

		// now, fire particles at random angles away
		// polar base
		Vector_Norm(&OmitDer);
		X.X = OmitDer.Z;
		X.Y = 0.0;
		X.Z =-OmitDer.X;
		x = 1.0/sqrt(X.X*X.X+X.Z*X.Z);
		X.X *= x;
		X.Z *= x;
		Cross_Product(&OmitDer,&X,&Y);

		Spwn = 12;
//		Spwn = 0;
		while(Spwn--)
		{
			// search for a place to insert
			for(P=Sc->Pcl+FntOuterPcls+I*FntInnerPcls,PE=P+FntInnerPcls;P<PE;P++)
				if (!P->Flags) break;
			if (P==PE) break;
			//insert
			Vector_Copy(&P->V.Pos,&Omit);

			u = ((float)rand()/32768.0f)*PI_M2;
			v = ((float)rand()/32768.0f)*PI-PI_D2;
			cv = cos(v);
			P->Vel.X = cv * cos(u) * 4.5;
			P->Vel.Y = sin(v) * 4.5;
			P->Vel.Z = cv * sin(u) * 4.5;
			P->Charge = 3.5f;

			switch(I)
			{
				case 0:
					P->Color.B = 255;
					P->Color.G = 63;
					P->Color.R = 32;
				break;
				case 1:
					P->Color.B = 63;
					P->Color.G = 255;
					P->Color.R = 63;
				break;
				case 2:
					P->Color.B = 32;
					P->Color.G = 63;
					P->Color.R = 255;
				break;
			}


			switch(Spwn>>2)
			{
				case 0: break;
				case 1:
					x = P->V.Pos.X * xForm1x + P->V.Pos.Z * xForm1z;
					P->V.Pos.Z = P->V.Pos.Z * xForm1x - P->V.Pos.X * xForm1z;
					P->V.Pos.X = x;

					x = P->Vel.X * xForm1x + P->Vel.Z * xForm1z;
					P->Vel.Z = P->Vel.Z * xForm1x - P->Vel.X * xForm1z;
					P->Vel.X = x;

				case 2:
					x = P->V.Pos.X * xForm2x + P->V.Pos.Z * xForm2z;
					P->V.Pos.Z = P->V.Pos.Z * xForm2x - P->V.Pos.X * xForm2z;
					P->V.Pos.X = x;

					x = P->Vel.X * xForm2x + P->Vel.Z * xForm2z;
					P->Vel.Z = P->Vel.Z * xForm2x - P->Vel.X * xForm2z;
					P->Vel.X = x;
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

		if (P->V.TPos.Z>=1.0)
		{
			P->V.RZ = 1.0/P->V.TPos.Z;
			P->V.PX = CntrX+FOVX*P->V.TPos.X*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos.Y*P->V.RZ;
//      P->PRad = 0;//FOVX*P->Radius*RZ;
			P->V.Flags = 0;
		} else P->V.Flags |= Vtx_VisNear;

		// dummy illumination model
		P->V.LB = P->Color.B*P->Charge*0.3;
		P->V.LG = P->Color.G*P->Charge*0.3;
		P->V.LR = P->Color.R*P->Charge*0.3;

	}

	if (Timer>FntVortexSwarm)
	{
		P = FntSc->Pcl + FntSpiralPclsOff;
		PE = P + FntSpiralPcls;
		for(;P<PE;P++)
		{
			if (!P->Flags) continue;
			//pcl destruction
			if (P->V.Pos.Y>Vortex_Center.Y)
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
			P->Color.R = 63 + (rand()>>9);
			P->Color.G = 127 + (rand()>>8);
			P->Color.B = 255;

			Vector_Copy(&P->V.Pos,&Vortex_Center);
			P->V.Pos.Y = FntHead.Y;

			r = rand()>>5;
			if (r&0xFF>=0x80) r -= 0x80;
			x = r * PI_M2 / 1024.0f;
			u = (1.0f+2.0f*rand()/32768.0f);
			P->V.Pos.X += cos(x)*u;
			P->V.Pos.Z += sin(x)*u;

			P->Vel.X = 0.0f;
			P->Vel.Y = 0.0f;
			P->Vel.Z = 0.0f;
			P->Flags = 1;
		}

		float dcx = expdt*cos(dt*1.0f),dsx = expdt*sin(dt*1.0f),tx,ty;
		for(P = FntSc->Pcl + FntSpiralPclsOff;P<PE;P++)
		{
			if (!P->Flags) continue;
//			Vector_Scale(&P->Vel,dt,&V);
//			Vector_SelfAdd(&P->V.Pos,&V);
			P->V.Pos.Y += dt*25.0f;
			tx = P->V.Pos.X - Vortex_Center.X;
			ty = P->V.Pos.Z - Vortex_Center.Z;

			P->V.Pos.X = dcx * tx + dsx * ty + Vortex_Center.X;
			P->V.Pos.Z = dcx * ty - dsx * tx + Vortex_Center.Z;

			Vector_Sub(&P->V.Pos,&View->ISource,&V);
			MatrixXVector(View->Mat,&V,&P->V.TPos);

			if (P->V.TPos.Z>=1.0)
			{
				P->V.RZ = 1.0/P->V.TPos.Z;
				P->V.PX = CntrX+FOVX*P->V.TPos.X*P->V.RZ;
				P->V.PY = CntrY-FOVY*P->V.TPos.Y*P->V.RZ;
//      P->PRad = 0;//FOVX*P->Radius*RZ;
				P->V.Flags = 0;
			} else P->V.Flags |= Vtx_VisNear;

			// dummy illumination model
			P->V.LB = P->Color.B*0.1f;
			P->V.LG = P->Color.G*0.1f;
			P->V.LR = P->Color.R*0.1f;
		}
	}

}

struct Four_C
{
	char R,G,B,A;
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
	Img->X = 64; Img->Y = 128;
	Img->Data = (DWord *)Blaze;
	memset(Img->Data,0,64*128*4);
	Generate_Fractal_Noise(Img,4.0);
	long I,J,K;
	FCDW FCD;
	DWord *P = Img->Data;
	long HX = Img->X>>1;

	 for(J=0;J<Img->Y;J++)
		 for(I=0;I<Img->X;I++)
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
	FntSc = new Scene; memset(FntSc,0,sizeof(Scene));
	LoadFLD(FntSc,"Scenes\\Fountain.FLD");

	for(M = MatLib;M;M=M->Next)
	{
		if (M->RelScene != FntSc) continue;
		M->Flags |= Mat_RGBInterp;
	}
	Add_Vortex_ToScene();

	Preprocess_Scene(FntSc);

	Image Img;
	for(M = MatLib;M;M=M->Next)
	{
		if (M->RelScene != FntSc) continue;
		if (!M->Txtr) continue;
		for(Material *Mat = MatLib;Mat!=M;Mat=Mat->Next)
			if (M->Txtr==Mat->Txtr) break;
		if (M!=Mat) continue;

		Convert_Texture2Image(M->Txtr,&Img);
		delete M->Txtr->Data; M->Txtr->Data = NULL;
		Gamma_Correction(&Img,1.2f);
		Convert_Image2Texture(&Img,M->Txtr);
		FilterLSB_Texture(M->Txtr);
		delete Img.Data;
		M->Flags |= Mat_RGBInterp;
	}


	FntSc->FZP = 5000.0f;

	Initialize_Particles(FntSc);

	Omni *O = FntSc->OmniHead->Next->Next;

	long I;
	Quaternion *W;
	for(I=0;I<6;I++)
	{
		W = &O->Pos.Keys->Pos;
		Vector_Form(Pylon+I,W->X,W->Y,W->Z);
		O=O->Next;
		if (!O) break;
	}

	LBoltPcls = new Particle[FntLightningPcls];


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

	if (A->TPos.Z>B->TPos.Z) {T=A; A=B; B=T;}

	if (B->TPos.Z<1) return;
	if (A->TPos.Z<1)
	{
		L=(A->TPos.Z-1)/(A->TPos.Z-B->TPos.Z);
		R=1-L;
		Vtx1.PX = CntrX+FOVX*(A->TPos.X*R + B->TPos.X*L);
		Vtx1.PY = CntrY-FOVY*(A->TPos.Y*R + B->TPos.Y*L);

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
			printf("UFC1:Abnormal Interpolation rate (A=(%f,%f,%f),B=(%f,%f,%f),Y1=%f,Y2=%f,L=%f)",A->TPos.X,A->TPos.Z,A->TPos.Y,B->TPos.X,B->TPos.Z,B->TPos.Y,Y1,Y2,L);
			exit(1);
		}
		R = 1-L;
		Vtx1.PX = 0; // CntrX + FOVX*Vtx1.TPos.X/Vtx1.TPos.Z;
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
			printf("UFC2:Abnormal Interpolation rate (A=(%f,%f,%f),B=(%f,%f,%f),Y1=%f,Y2=%f,R=%f)",A->TPos.X,A->TPos.Z,A->TPos.Y,B->TPos.X,B->TPos.Z,B->TPos.Y,Y1,Y2,R);
			exit(1);
		}
		L = 1-R;
		Vtx1.PX = XClip; // CntrX + FOVX*Vtx1.TPos.X/Vtx1.TPos.Z;
		Vtx1.PY = A->PY*R + B->PY*L; //Vtx1.TPos.Y/Vtx1.TPos.Z;
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
			printf("UFC3:Abnormal Interpolation rate (A=(%f,%f,%f)|(%f,%f),B=(%f,%f,%f)|(%f,%f),Y1=%f,Y2=%f,L=%f)",A->TPos.X,A->TPos.Z,A->TPos.Y,A->PX,A->PY,B->TPos.X,B->TPos.Z,B->TPos.Y,B->PX,B->PY,Y1,Y2,L);
			exit(1);
		}
		R = 1-L;
		Vtx1.PX = A->PX*R + B->PX*L;//Vtx1.TPos.X/Vtx1.TPos.Z;
		Vtx1.PY = 0;//CntrY - FOVY*Vtx1.TPos.Y/Vtx1.TPos.Z;

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
			printf("UFC4:Abnormal Interpolation rate (A=(%f,%f,%f)|(%f,%f),B=(%f,%f,%f)|(%f,%f),Y1=%f,Y2=%f,R=%f)",A->TPos.X,A->TPos.Z,A->TPos.Y,A->PX,A->PY,B->TPos.X,B->TPos.Z,B->TPos.Y,B->PX,B->PY,Y1,Y2,R);
			exit(1);
		}
		L = 1-R;
		Vtx1.PX = A->PX*R + B->PX*L;
		Vtx1.PY = YClip;// CntrY - FOVY*Vtx1.TPos.Y/Vtx1.TPos.Z;

		Vtx1.LR = A->LR*R + B->LR*L;

		Filler(A,&Vtx1);
		return;
	}
	Filler(A,B);
}


void Lightning_Line(Vertex *V1,Vertex *V2)
{
	DWord C=(255<<16)+(255<<8)+255;
	long D;
	long Dx,Dy;
	long Ai,Bi,Xi,Yi;
	long Offs,sat;
	long X1,Y1,X2,Y2;
	long depth = V1->LR,hd = depth>>1;
	char *cPtr;
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
		Ai = Dy-Dx<<1;
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
		Ai = Dx-Dy<<1;
		Bi = Dx<<1;
		D = Bi-Dy;
		while (Ptr != End) {

			if (D >= 0) {
				Ptr += Xi;
				D += Ai;
			} else D += Bi;
			Ptr += Yi;

			//left anti-alias
			cPtr = ((char *)Ptr)-8;
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
	long D;
	long Dx,Dy;
	long Ai,Bi,Xi,Yi;
	long Offs,sat;
	long X1,Y1,X2,Y2;
	long depth = V1->LR,hd = depth>>1;
	char *cPtr;
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
		Ai = Dy-Dx<<1;
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

			cPtr = ((char *)Ptr)-VESA_BPSL;
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
		Ai = Dx-Dy<<1;
		Bi = Dx<<1;
		D = Bi-Dy;
		while (Ptr != End) {

			if (D >= 0) {
				Ptr += Xi;
				D += Ai;
			} else D += Bi;
			Ptr += Yi;

			//left anti-alias
			cPtr = ((char *)Ptr)-4;
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


void Lightning_Bolt_Frag(Vector *Src,Vector *Targ,long POW)
{
//	memset(VPage,255,PageSize);
	if (POW<1) POW=1;
	Vertex A,B;
	Vector V;

	Vector_Sub(Src,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&A.TPos);
	A.TPos.X = A.TPos.Z * CntrX + A.TPos.X * FOVX;
	A.TPos.Y = A.TPos.Z * CntrY - A.TPos.Y * FOVY;
	A.RZ = 1.0f/A.TPos.Z;
	A.PX = A.TPos.X * A.RZ;
	A.PY = A.TPos.Y * A.RZ;

	Vector_Sub(Targ,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&B.TPos);
	B.TPos.X = B.TPos.Z * CntrX + B.TPos.X * FOVX;
	B.TPos.Y = B.TPos.Z * CntrY - B.TPos.Y * FOVY;
	B.RZ = 1.0f/B.TPos.Z;
	B.PX = B.TPos.X * B.RZ;
	B.PY = B.TPos.Y * B.RZ;

	A.LR = B.LR = POW;

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
		P->Charge = rand()*0.5f/32768.0f+0.5f;
		Vector_Copy(&P->V.Pos,Src);
		P->Vel.X = ((rand()-16384)/16384.0f)*32.5f;
		P->Vel.Y = ((rand()-16384)/16384.0f)*5.5f;
		P->Vel.Z = ((rand()-16384)/16384.0f)*32.5f;
		P->Color.R = 128;
		P->Color.G = 96+(rand()>>10);
		P->Color.B = 32+(rand()>>9);
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
		if (P->V.Pos.Y<=0.0f) P->Charge = 0.0f;

		P->Vel.Y -= dt * 16.0f;
		Vector_Scale(&P->Vel,dt,&V);
		Vector_SelfAdd(&P->V.Pos,&V);

		Vector_Sub(&P->V.Pos,&View->ISource,&V);
		MatrixXVector(View->Mat,&V,&P->V.TPos);

		if (P->V.TPos.Z>=1.0)
		{
			P->V.RZ = 1.0/P->V.TPos.Z;
			P->V.PX = CntrX+FOVX*P->V.TPos.X*P->V.RZ;
			P->V.PY = CntrY-FOVY*P->V.TPos.Y*P->V.RZ;
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

void Lightning_Bolt(Vector *Src,Vector *Targ,long BasePow)
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
		V.X += ((float)(rand()-16384))*0.5f/16384.0f;
		V.Y += ((float)(rand()-16384))*0.5f/16384.0f;
		V.Z += ((float)(rand()-16384))*0.5f/16384.0f;
		Vector_Norm(&V);
		Seg=30.0f+rand()*30.0f/32768.0f;
		Vector_SelfScale(&V,Seg);
		Vector_Add(&Pos,&V,&NPos);
		Lightning_Bolt_Frag(&Pos,&NPos,BasePow>>4);
		Vector_Copy(&Pos,&NPos);
		Vector_Sub(Targ,&Pos,&V);
		Distance = Vector_Length(&V);
		if (Distance>60.0f&&Distance<400.0f&&(rand()<320*BasePow-Distance*15))
		{
			Yosi.Y = 0;
			Yosi.X = Pos.X;
			Yosi.Z = Pos.Z;
			Lightning_Bolt(&Pos,&Yosi,BasePow>>1);
		}
	} while (Distance>60.0f);
	// attach pos with target

//	Generate_Lightning_Particles(&Pos,50);
}

void Spark(Vector *Src,Vector *Targ)
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
		V.X += ((float)(rand()-16384))*0.3f/16384.0f;
		V.Y += ((float)(rand()-16384))*0.3f/16384.0f;
		V.Z += ((float)(rand()-16384))*0.3f/16384.0f;
		Vector_Norm(&V);
		Seg=15.0f+rand()*15.0f/32768.0f;
		Vector_SelfScale(&V,Seg);
		Vector_Add(&Pos,&V,&NPos);
		Lightning_Bolt_Frag(&Pos,&NPos,1);
		Vector_Copy(&Pos,&NPos);
		Vector_Sub(Targ,&Pos,&V);
		Distance = Vector_Length(&V);
	} while (Distance>30.0f);
	// attach pos with target
	Lightning_Bolt_Frag(&Pos,Targ,1);

	Generate_Lightning_Particles(Targ,10);
}


void Reserve_Lightning_Bolt(Vector *Src,Vector *Targ,long Pow)
{
	Vector_Copy(&Rsv_LSrc,Src);
	Vector_Copy(&Rsv_LTarg,Targ);
	Rsv_LPow = Pow;
	Rsv_LState = 1;
}


void Handle_Lightning()
{
	Vector Upwards,Downwards;
	if (Ambient_Factor>32.0f)
	{
		Ambient_Factor -= 32.0f;
		Ambient_Factor *= exp(-dTime*0.02);
		Ambient_Factor += 32.0f;
	}
	switch (LightningFlag)
	{
		case 0:
			if (Timer>FntLightningTime1)
			{
				LightningFlag = 1;
				Upwards.X = Pylon->X+(rand()-16384.0f)/16384.0f*75.0f;
				Upwards.Y = Pylon->Y+1000.0f;
				Upwards.Z = Pylon->Z+(rand()-16384.0f)/16384.0f*75.0f;
				Downwards.X = Pylon->X;
				Downwards.Y = 0.0f;
				Downwards.Z = Pylon->Z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,45);
				Ambient_Factor = 255.0f;
			}
		break;
		case 1:
			if (Timer>FntLightningTime2)
			{
				LightningFlag = 2;
				Upwards.X = Pylon[1].X+(rand()-16384.0f)/16384.0f*75.0f;
				Upwards.Y = Pylon[1].Y+1000.0f;
				Upwards.Z = Pylon[1].Z+(rand()-16384.0f)/16384.0f*75.0f;
				Downwards.X = Pylon[1].X;
				Downwards.Y = 0.0f;
				Downwards.Z = Pylon[1].Z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,50);
				Ambient_Factor = 255.0f;
			}
		break;
		case 2:
			if (Timer>FntLightningTime3)
			{
				LightningFlag = 3;
				Upwards.X = Pylon[2].X+(rand()-16384.0f)/16384.0f*75.0f;
				Upwards.Y = Pylon[2].Y+1000.0f;
				Upwards.Z = Pylon[2].Z+(rand()-16384.0f)/16384.0f*75.0f;
				Downwards.X = Pylon[2].X;
				Downwards.Y = 0.0f;
				Downwards.Z = Pylon[2].Z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,55);
				Ambient_Factor = 255.0f;
			}
		break;
		case 3:
			if (Timer>FntLightningTime4)
			{
				LightningFlag = 4;
				Upwards.X = Pylon[3].X+(rand()-16384.0f)/16384.0f*75.0f;
				Upwards.Y = Pylon[3].Y+1000.0f;
				Upwards.Z = Pylon[3].Z+(rand()-16384.0f)/16384.0f*75.0f;
				Downwards.X = Pylon[3].X;
				Downwards.Y = 0.0f;
				Downwards.Z = Pylon[3].Z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,60);
				Ambient_Factor = 255.0f;
			}
		break;
		case 4:
			if (Timer>FntLightningTime5)
			{
				LightningFlag = 5;
				Upwards.X = Pylon[4].X+(rand()-16384.0f)/16384.0f*75.0f;
				Upwards.Y = Pylon[4].Y+1000.0f;
				Upwards.Z = Pylon[4].Z+(rand()-16384.0f)/16384.0f*75.0f;
				Downwards.X = Pylon[4].X;
				Downwards.Y = 0.0f;
				Downwards.Z = Pylon[4].Z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,65);
				Ambient_Factor = 255.0f;
			}
		break;
		case 5:
			if (Timer>FntLightningTime6)
			{
				LightningFlag = 6;
				Upwards.X = Pylon[5].X+(rand()-16384.0f)/16384.0f*75.0f;
				Upwards.Y = Pylon[5].Y+1000.0f;
				Upwards.Z = Pylon[5].Z+(rand()-16384.0f)/16384.0f*75.0f;
				Downwards.X = Pylon[5].X;
				Downwards.Y = 0.0f;
				Downwards.Z = Pylon[5].Z;
				Reserve_Lightning_Bolt(&Upwards,&Downwards,70);
				Ambient_Factor = 255.0f;
			}
		break;
		case 6:
			if (Timer>FntLightningTime6+100)
			{
				if (Timer>FntLightningTime6+400)
					LightningFlag = 7;

				Reserve_Lightning_Bolt(NULL,NULL,-1); //special code
				Ambient_Factor = 63.0f;
			}
		break;
	}
}



/*DWord Photonbeam[64*128];
Word Noise[1800];
long NoiseSampler = 0;

void Noise_Sample()
{
	Word *N = Noise;
	long I = 1800;
	while (I--) *N++ = rand();
}

void Iterate_Photonbeam_Texture()
{
	long I,J;
	// first line
	DWord *Pen = Photonbeam + 64*127;
	for(I=0;I<=32;I++)
		*Pen++ = (155*I>>5)+(rand()>>9)+32;
	for(I=32;I;I--)
		*Pen++ = (155*I>>5)+(rand()>>9)+32;
	for(I=0;I<=32;I++)
		*Pen++ = (155*I>>5)+(rand()>>9)+32;
	for(I=32;I;I--)
		*Pen++ = (155*I>>5)+(rand()>>9)+32;

//	for(I=0;I<=32;I++)
//		*Pen++ = 96+(rand()>>8);
//	for(I=32;I;I--)
//		*Pen++ = 96+(rand()>>8);
//	for(I=0;I<=32;I++)
//		*Pen++ = 96+(rand()>>8);
//	for(I=32;I;I--)
//		*Pen++ = 96+(rand()>>8);


	Pen = Photonbeam;
	for(J=0;J<126;J++)
	{
		for(I=1;I<=62;I++)
		{
			//*Pen = (*Pen + Pen[63] + Pen[64] + Pen[65] + ((rand()>>10)-14) )>>2;
			//*Pen = (*Pen + Pen[63] + Pen[64] + Pen[65] + (Noise[NoiseSampler++]>>10)-14 )>>2;
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
	long T;
	long I,J;
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
	A.TPos.X = A.TPos.Z * CntrX + A.TPos.X * FOVX;
	A.TPos.Y = A.TPos.Z * CntrY - A.TPos.Y * FOVY;
	A.RZ = 1.0f/A.TPos.Z;
	A.PX = A.TPos.X * A.RZ;
	A.PY = A.TPos.Y * A.RZ;

	Vector_Sub(Target,&View->ISource,&V);
	MatrixXVector(View->Mat,&V,&B.TPos);
	B.TPos.X = B.TPos.Z * CntrX + B.TPos.X * FOVX;
	B.TPos.Y = B.TPos.Z * CntrY - B.TPos.Y * FOVY;
	B.RZ = 1.0f/B.TPos.Z;
	B.PX = B.TPos.X * B.RZ;
	B.PY = B.TPos.Y * B.RZ;

//	Iterate_Photonbeam_Texture();
//	Iterate_Photonbeam_Texture();
//	Blaze_Photon();

	long Width = 3000.0f*A.RZ;
	The_Seven_UP_64(A.PX,(A.PY+B.PY)*0.5f,Width,fabs(B.PY-A.PY)*0.5f,Blaze,VPage);
}

Texture VorTexture;
DWord VorBuf[65536];
Material VortexMat;

void Load_Vortex()
{
	long X,Y;
	VorTexture.FileName = strdup("Textures//Vortex.JPG");
	Identify_Texture(&VorTexture);
	if (!VorTexture.BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(&VorTexture);
	BPPConvert_Texture(&VorTexture,32);

	Material *M = &VortexMat;
	M->Txtr = &VorTexture;
	M->Flags = 0;
	M->RelScene = FntSc;
	Material *Mat;
	for(Mat=MatLib;Mat->Next;Mat=Mat->Next);
	Mat->Next = M;
	memcpy(VorBuf,VorTexture.Data,65536*4);
}

void Vortex_Distort()
{
	long I;
	GridPoint *Vortex_GP;
	DWord *TempBuf = new DWord[65536];
	Image VorImg;
	VorImg.X = 256;
	VorImg.Y = 256;
	DWord *VorSurf = (DWord *)VorTexture.Data;
	VorImg.Data = VorSurf;
	int x,y,X,Y,i;
	float R1,R3,R4,sinr4,cosr4,u,v;

	WOBPOINTSHEIGHT = 32;
	Vortex_GP = new GridPoint[33*33];
	// Run wobbler
	R1=-Timer*0.01;
	i=0;
	memcpy(VorSurf,VorBuf,65536*4);
	for (y=0;y<=256;y+=8)
		for (x=0;x<=256;x+=8,i++)
		{
			X = (x - 128)<<1;
			Y = (y - 128)<<1;
			R3 = sqrt(X * X + Y * Y) / (128.0*1.412);
			if (R3>0.85) R3 = 0.85;
			R4 = cos((R3)*5) + R1 - R3;
			sinr4 = sin(R4);
			cosr4 = cos(R4);
			u = x * 256 * 2;
			v = y * 256 * 2;
			Vortex_GP[i].U=sinr4*(v-32767)+cosr4*(u-32767)+32767;
			Vortex_GP[i].V=cosr4*(v-32767)-sinr4*(u-32767)+32767;
		}
	Grid_Texture_Mapper(Vortex_GP,&VorImg,TempBuf);
	memcpy(VorSurf, TempBuf, 65536 * 4);

	for(Y=-128;Y<128;Y++)
		for(X=-128;X<128;X++)
			if (X*X+Y*Y>128*128)
				((DWord *)VorTexture.Data)[128+128*256+X+(Y<<8)] = 0;


	delete TempBuf;
	delete Vortex_GP;
}

Object *VortexObj;
TriMesh *VortexTri;

void Add_Vortex_ToScene()
{
	Object *Obj;
	for(Obj=FntSc->ObjectHead;Obj;Obj=Obj->Next)
		if (!strcmp(Obj->Name,"vortex.lwo")) break;

	VortexObj = Obj;
	if (!Obj) {printf("kiebinimat!!!!!!!!\n"); return;}

	VortexTri = (TriMesh *)(Obj->Data);
}

void FntLighting(Scene *Sc)
{
	TriMesh *T;
	Vertex *Vtx,*VEnd;
	Face *Face,*FEnd;
	Omni *O;
	float R,G,B,Dot,Dot2,Its,Color,Distance;
	const float Ka=Ambient_Factor/*255.0*/,Kd=Diffusive_Factor,Ks=0.0,n=4.0;
	Vector L,U,V,W,TN,CU,CV;
	Matrix M;
	float rLen,Len,Len2;

	for(T=Sc->TriMeshHead;T;T=T->Next)
	{
		if (T->Flags&Tri_Invisible) continue;
		if (T->Flags&Tri_Noshading) continue;

		VEnd=T->Verts+T->VIndex;
		FEnd=T->Faces+T->FIndex;
		Vector_Sub(&T->IPos,&View->ISource,&U);
		MatrixTXVector(T->RotMat,&U,&W);
		// Camera Illumination model (Lambertian)
/*		if (Cam_HeadLight)
		{
			for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
			{
				Vector_Add(&Vtx->Pos,&W,&V);
				Its = 350000.0/Dot_Product(&V,&V);
				if (Its<1.0) Its=0.5;
				if (Its>229.0) Its=229.0;
				Vtx->LR = Its+Ka;
				Vtx->LG = Its+Ka;
				Vtx->LB = Its+Ka;
//      Vtx->LR=Vtx->LG=Vtx->LB = 0.5;
			}
		} else {*/
			for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
				Vtx->LR=Vtx->LG=Vtx->LB = Ka;
//		}
		// Lightsource based Illumination model
/*    for(O=Sc->OmniHead;O;O=O->Next)
			if(O->Flags&Flare_LightSource)
			{
				Vector_Sub(&T->IPos,&O->IPos,&U);
				MatrixTXVector(T->RotMat,&U,&W);
				for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
				{
					Vector_Add(&Vtx->Pos,&W,&L);
					Dot=1.0/Dot_Product(&L,&L);
					Vtx->LR += O->L.R * Dot;
					Vtx->LG += O->L.G * Dot;
					Vtx->LB += O->L.B * Dot;
				}
			}*/
/*
	N = surface normal
	L     = unit vector between point and light
	V = unit vector between point and view
*/
//    Ka + Kd * (N dot L) + Ks * (N dot ( L + V / 2))^n
		for(O=Sc->OmniHead;O;O=O->Next)
			if(1)//O->Flags&Flare_LightSource)
			{
				Vector_Sub(&T->IPos,&O->IPos,&U);
				MatrixTXVector(T->RotMat,&U,&W);

//				Vector_Sub(&View->ISource,&O->IPos,&CU);
//				MatrixTXVector(View->Mat,&CU,&CV);

				// check the lousy b-sphere
//				if (Vector_SelfDot(&U)-T->BSphereRad<90000000.0f) continue;

				if (O->IRange)
				{
					for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
					{
						Vector_Add(&Vtx->Pos,&W,&L);
//						if (Vector_SelfDot(&L)>O->IRange) continue;
						rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
						Dot=-Dot_Product(&L,&Vtx->N)*rLen;

//					Vector_Add(&Vtx->Pos,&CV,&L);
//					rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
//					Dot2=-Dot_Product(&L,&Vtx->N)*rLen;

						if (Dot>0.0f)
						{
						//Color=(Kd*Dot/*+Ks*(Dot*Dot*Dot+Dot2)*0.5f*/)*rLen*rLen;
							Color=/*(Dot*(Kd+Ks*Dot*Dot)*0.5f)*/rLen*rLen*O->ISize;
							Vtx->LR += O->L.R * Color;
							Vtx->LG += O->L.G * Color;
							Vtx->LB += O->L.B * Color;
						}
					}
				} else {
					for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
					{
						Vector_Add(&Vtx->Pos,&W,&L);
						rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
						Dot=-Dot_Product(&L,&Vtx->N)*rLen;

//					Vector_Add(&Vtx->Pos,&CV,&L);
//					rLen = RSQRT(L.X*L.X+L.Y*L.Y+L.Z*L.Z);
//					Dot2=-Dot_Product(&L,&Vtx->N)*rLen;

						if (Dot>0.0f)
						{
						//Color=(Kd*Dot/*+Ks*(Dot*Dot*Dot+Dot2)*0.5f*/)*rLen*rLen;
							Color=(Dot*(Kd+Ks*Dot*Dot)*0.5f)*O->IRange*rLen*rLen*O->ISize;
							Vtx->LR += O->L.R * Color;
							Vtx->LG += O->L.G * Color;
							Vtx->LB += O->L.B * Color;
						}
					}
				}
			}

		// Saturation.
		for(Vtx=T->Verts;Vtx<VEnd;Vtx++)
		{
			if (Vtx->LR>253.0) Vtx->LR=253.0;
			if (Vtx->LG>253.0) Vtx->LG=253.0;
			if (Vtx->LB>253.0) Vtx->LB=253.0;
//      Vtx->LR = 0.25; Vtx->LG = 0.25; Vtx->LB = 0.25; //Or some other jack,like an Ambient constant
		}
	}
/*	if (Sc->Flags&Scn_Fogged)
	{

		for(T=Sc->TriMeshHead;T;T=T->Next)
		{
			if (T->Flags&Tri_Invisible) continue;
			if (T->Flags&Tri_Noshading) continue;

			Vtx=T->Verts;
			VEnd=Vtx+T->VIndex;
			for(;Vtx<VEnd;Vtx++)
			{
				//apply fog
				Color = 1.0f-Vtx->TPos.Z*C_rFZP;

				Vtx->LR = Vtx->LR + (128.0f-Vtx->LR) * Color;
				Vtx->LG = Vtx->LG + (128.0f-Vtx->LG) * Color;
				Vtx->LB = Vtx->LB + (128.0f-Vtx->LB) * Color;
//				Vtx->LR *= Color;
//				Vtx->LG *= Color;
//				Vtx->LB *= Color;

			}
		}
	}*/
}


void Run_Fountain()
{
	long Polys = 0;
	TriMesh *T;
	Omni *O;
	long TTrd;
	Vector U,V;

	char *BPage = new char[PageSize];

	VESA_Surface Blur;
	memcpy(&Blur,Screen,sizeof(VESA_Surface));
	Blur.Data = BPage;
	Blur.Flags = VSurf_Noalloc;
	Blur.Targ = VGAPtr;

	CurScene = FntSc;
	for(T = FntSc->TriMeshHead;T;T=T->Next)
		Polys+=T->FIndex;
	for(O = FntSc->OmniHead;O;O=O->Next)
		Polys++;
	Polys += FntSc->NumOfParticles;
	FList = new Face * [Polys];
	SList = new Face * [Polys];

//	View = FntSc->CameraHead;
	View = &FC;

	C_FZP = FntSc->FZP;
	C_rFZP = 1.0f/C_FZP;

	TTrd = Timer;

	CParticle_ISize = 0.1f;
	Diffusive_Factor = 500.0f;
	ImageSize = 1000;

	while ((!Keyboard[ScESC])&&Timer<FNTPartTime)
	{
		memset(VPage,0,PageSize);
		dTime = Timer-TTrd;
		TTrd = Timer;

		Dynamic_Camera();

		Handle_Lightning();

		CurFrame = FntSc->StartFrame + (FntSc->EndFrame-FntSc->StartFrame) * (float)Timer / (float)FNTPartTime;

		if (Timer>FntVortexStart)
		{
			Vortex_Distort();
			float scl;
			if (Timer>FntVortexMax)
				scl = 1.0f;
			else
				scl = (float)(Timer-FntVortexStart)/(float)(FntVortexMax-FntVortexStart);
			Quaternion_Form(&VortexTri->Scale.Keys->Pos,scl,scl,scl,0.0f);
		}

		Animate_Objects(FntSc);

		if (Timer>FntVortexStart)
		{
			VortexTri->Flags |= HTrack_Visible;
		} else VortexTri->Flags &= 0xFFFFFFFF-HTrack_Visible;

		Particle_Kinematics(FntSc);

		Transform_Objects(FntSc);

		FntLighting(FntSc);

		if (!CAll) continue;
		Radix_SortingASM(FList,SList,CAll);

//		Run_Rain();

		Render();

//		sprintf(MSGStr,"Rendered %d Polys,%d Omnis and %d pcls.\n",CPolys,COmnies,CPcls);
//		OutTextXY(VPage,0,0,MSGStr,64);

		if (Rsv_LState)
		{
			if (Rsv_LPow>-1)
			{
				Lightning_Bolt(&Rsv_LSrc,&Rsv_LTarg,Rsv_LPow);
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

//		Lightning_Particles();

		Flip(Screen);
//		if (MMXState)
//			Modulate(Screen,&Blur,0xb0b0b0,0x404040);
//		else
//			Transparence(Screen,&Blur);
//		Flip(&Blur);
	} Timer-=FNTPartTime;
	if (Keyboard[ScESC])
	{
		#ifdef Play_Music
		ShutDown();
		#endif
		FDS_End();
		exit(-1);
	}

	delete FList;
	delete SList;
	delete BPage;
	Destroy_Scene(FntSc);
}