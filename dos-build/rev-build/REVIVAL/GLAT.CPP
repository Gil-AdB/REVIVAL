#include "Rev.H"

#ifdef FDS_GLATBENCH
extern "C" unsigned long RDTSC(void);   // low 32 bits of the cycle counter
#pragma aux RDTSC = 0x0F 0x31 value [eax] modify [eax edx];

// Wrap-correct cycle delta, one conversion site for the profiler. Converting
// (double)(unsigned long)(t1-t0) inline intermittently yielded ~2^31 per frame (see CITY.CPP).
static double TscDelta(unsigned long t0,unsigned long t1)
{
  unsigned long d = t1 - t0;
  return (double)d;
}
#endif


void Cross_Fade(char *U1,char *U2,char *Target,long Perc)
{
	long I;
	for(I=0;I<PageSize;I++)
		Target[I] = U1[I]*(256-Perc) + U2[I]*Perc>>8;
}

// Wrapped sin/cos lookup for the wobble plane (table filled in Run_Glato). The AND wraps
// modulo 2pi for negative arguments too (two's complement), so no fmod is needed.
#define WOBTAB 1024
#define WOBSIN(a) WobSinTab[((long)((a)*((float)WOBTAB/PI_M2))) & (WOBTAB-1)]
#define WOBCOS(a) WobSinTab[(((long)((a)*((float)WOBTAB/PI_M2)))+(WOBTAB/4)) & (WOBTAB-1)]

void Run_Glato(void)
{
	Vector CameraPos = {0,0,0};
	Matrix CamMat;
	float Radius;
	static int x,y,i,j;
	static float a,bb,c,d,Delta,X1,X2,X3,z,Rx,Ry,Rz;
	static float u,v,u1,v1,u2,v2,r,g,b;
	static float Code_R1,Code_RS,Code_R2,CCosR1,CSinR1,CCosR2,CSinR2;
	static float Gfx_R1,Gfx_R2,GCosR1,GSinR1,GCosR2,GSinR2,Gfx_RS;
	static Vector Intersection1,Origin1,Direction1,U,Origin2,Direction2,Intersection2;
	static Vector Intersection3,Origin3,Direction3;
	int Gfx = 0,Sfx = 0, Code = 1;
	float GridR;                       // sqrt(X*X+Y*Y), first pass only (feeds the tables)
	// Grid is a fixed lattice: y=0..240 step 8 (31 rows) x x=0..320 step 8 (41 cols) = 1271.
	static float WobC200[1344],WobS200[1344],WobC120[1344],WobS120[1344];
	static int   WobTabReady = 0;
	float WobCosB,WobSinB;             // cos/sin of the per-frame ST term
	float WobPhase;                    // per-frame plane-wobble phase (was per point, x2)
#ifdef FDS_GLATBENCH
	static double gbGrid=0,gbPlane=0,gbLayer=0; static long gbN=0; unsigned long gbT0=0;
#endif
	Vector WobRow,WobStep;             // (3) incremental ray direction along a row
	// (1) The plane layer's cos/sin take a PER-POINT argument (Intersection.Z / .X plus the
	// frame phase), so angle-addition cannot table them the way the wobble layers do. A
	// 1024-entry sin LUT indexed by the wrapped argument is ~5 cycles against ~100+ for a
	// Pentium FSIN. Step is 2pi/1024 = 0.0061 rad and the result is scaled by 0.5*0.5, so the
	// worst-case UV error is far under a thousandth of a texel.
	static float WobSinTab[WOBTAB];
	GridPoint *Plane_GP;
	GridPoint *Code_GP;
	GridPoint *Gfx_GP;
	GridPoint *Sfx_GP;
	Texture *LogoTexture = new Texture; memset(LogoTexture,0,sizeof(Texture));
	Image *LogoImage = new Image;
	Texture *PlaneTexture = new Texture; memset(PlaneTexture,0,sizeof(Texture));
	Image *PlaneImage = new Image;
	Texture *CodeTexture = new Texture; memset(CodeTexture,0,sizeof(Texture));
	Image *CodeImage = new Image;
	Texture *GfxTexture = new Texture; memset(GfxTexture,0,sizeof(Texture));
	Image *GfxImage = new Image;
	Texture *SfxTexture = new Texture; memset(SfxTexture,0,sizeof(Texture));
	Image *SfxImage = new Image;
	int X,Y;
	float R1,R3,R4;

	Matrix_Identity(CamMat);
	Radius = 1;

	LogoTexture->FileName = strdup("Textures//Logo.JPG");
	Identify_Texture(LogoTexture);
	if (!LogoTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(LogoTexture);
	BPPConvert_Texture(LogoTexture,32);
	Convert_Texture2Image(LogoTexture,LogoImage);
	Scale_Image(LogoImage,XRes,YRes);

	PlaneTexture->FileName = strdup("Textures//SC13.JPG");
	Identify_Texture(PlaneTexture);
	if (!PlaneTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(PlaneTexture);
	Convert_Texture2Image(PlaneTexture,PlaneImage);
//	WOBPOINTSHEIGHT = 30;

	CodeTexture->FileName = strdup("Textures//Code.JPG");
	Identify_Texture(CodeTexture);
	if (!CodeTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(CodeTexture);
	Convert_Texture2Image(CodeTexture,CodeImage);


	GfxTexture->FileName = strdup("Textures//Gfx.JPG");
	Identify_Texture(GfxTexture);
	if (!GfxTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(GfxTexture);
	Convert_Texture2Image(GfxTexture,GfxImage);

	SfxTexture->FileName = strdup("Textures//Sfx.JPG");
	Identify_Texture(SfxTexture);
	if (!SfxTexture->BPP)
	{
		printf("Error Loading texture!\n");
		exit(1);
	}
	Load_Texture(SfxTexture);
	Convert_Texture2Image(SfxTexture,SfxImage);

	// Reorder all four wobbler textures into 8-wide vertical strips once at init, so both
	// mappers can use the cache-friendly tiled fetch (see Wob_SwizzleImage): the C layer
	// mapper (Grid_Texture_MapC_Tiled) and the plane's MMX ASM (Grid_Texture_MapG_MMX_T).
	// A one-texel VERTICAL step costs 32 bytes instead of 1024, and the wobble moves u and v
	// in both screen axes over a 256KB texture - 16x the P55C's L1.
#ifdef FDS_WOBTILE
#ifdef FDS_WOBCHK
	// Keep a row-major copy so Grid_Texture_Mapper_Gouraud can run both plane mappers per
	// frame and byte-compare them (WOBCHK lines in the log).
	{ extern DWord *g_WobRowTex;
	  g_WobRowTex = new DWord[65536];
	  memcpy(g_WobRowTex,PlaneImage->Data,65536*sizeof(DWord)); }
#endif
	Wob_SwizzleImage((DWord *)PlaneImage->Data);
	Wob_SwizzleImage((DWord *)CodeImage->Data);
	Wob_SwizzleImage((DWord *)GfxImage->Data);
	Wob_SwizzleImage((DWord *)SfxImage->Data);
#endif


	char *Page1 = new char[PageSize];
	char *Page2 = new char[PageSize];
	char *Page3 = new char[PageSize];
	char *Page4 = new char[PageSize];
	memset(Page1,0,PageSize); memset(Page2,0,PageSize);   // clear after alloc — uninitialized heap = the wobbler corner noise
	memset(Page3,0,PageSize); memset(Page4,0,PageSize);

	VESA_Surface Surf1;
	VESA_Surface Surf2;
	VESA_Surface Surf3;
	VESA_Surface Surf4;
	memcpy(&Surf1,Screen,sizeof(VESA_Surface));
	memcpy(&Surf2,Screen,sizeof(VESA_Surface));
	memcpy(&Surf3,Screen,sizeof(VESA_Surface));
	memcpy(&Surf4,Screen,sizeof(VESA_Surface));
	Surf1.Data = Page1;
	Surf2.Data = Page2;
	Surf3.Data = Page3;
	Surf4.Data = Page4;
	Surf1.Flags = VSurf_Noalloc;
	Surf1.Targ = VGAPtr;
	Surf2.Flags = VSurf_Noalloc;
	Surf2.Targ = VGAPtr;
	Surf3.Flags = VSurf_Noalloc;
	Surf3.Targ = VGAPtr;
	Surf4.Flags = VSurf_Noalloc;
	Surf4.Targ = VGAPtr;

	Plane_GP = new GridPoint[XRes*YRes/60];
	Code_GP = new GridPoint[XRes*YRes/60];
	Gfx_GP = new GridPoint[XRes*YRes/60];
	Sfx_GP = new GridPoint[XRes*YRes/60];
	// Run wobbler
#ifdef Play_Music
	Play_Module();
#endif

	Timer = 0;
	float ST;
	while (!Keyboard[ScESC]&&(!Keyboard[ScBackSpace])&&Timer<3500)   // BACKSPACE = skip scene, ESC = quit
	{

		if (Timer <= 100 * 11)
			ST = (Timer*2500)/(1000+Timer);//  sqrt(Timer*1600);
		if (Timer > 100 * 11){ Gfx = 1;Code = 0;
			ST = ((Timer-1100)*2500)/(1000+(Timer-1100));//  sqrt(Timer*1600);
		}
		if (Timer > 100 * 23){ Gfx = 0;Sfx = 1;
			ST = ((Timer-2300)*2500)/(1000+(Timer-2300));//  sqrt(Timer*1600);
		}
//		ST = (Timer*2000)/(1000+Timer);//  sqrt(Timer*1600);
		Euler_Angles(CamMat,Rx,Ry,Rz);
		i=0;
		j=0;
		//code
		Code_R1 = ST * 0.0005;
		// back
		CSinR1 = sin(Code_R1*0.1);
		CCosR1 = cos(Code_R1*0.1);
	if (ST < 700)
		Code_RS = Code_R1 * 0.5;

		Gfx_R1 = - (ST) * 0.000001;
		GSinR1 = sin(Gfx_R1*0.05);
		GCosR1 = cos(Gfx_R1*0.05);
		if (ST < 400)
			Gfx_RS = (ST) * 0.00001;



		// TRIG ELIMINATION for the wobble layers. Every layer's angle is
		//     Code/Sfx: GridR/200 + ST/100      Gfx: GridR/120 - ST/100
		// and GridR = sqrt((x-160)^2+(y-120)^2) is CONSTANT per grid point (the grid is a
		// fixed 8px lattice). So cos/sin of the constant part are tabulated ONCE, and each
		// frame only needs cos/sin of the ST term - two calls instead of two per point.
		// Angle addition then costs 4 muls + 2 adds per point in place of sin+cos
		// (~100+ cycles each on a Pentium, over 1271 points per live layer).
		{
			float bAng = ST/100.0;
			WobCosB = cos(bAng);
			WobSinB = sin(bAng);
			// was recomputed per grid point, TWICE: (float)(ST*0.1)/28.65
			WobPhase = (float)(ST*0.1)/28.65;
		}
		if (!WobTabReady)
		{
			long _ti;
			for (_ti=0;_ti<WOBTAB;_ti++)
				WobSinTab[_ti] = sin((double)_ti*(PI_M2/(double)WOBTAB));
		}
		Origin1.X=CameraPos.X;
		Origin1.Y=CameraPos.Y;
		Origin1.Z=CameraPos.Z;
		// (3) The ray direction is CamMat*(x-160, y-100, 256) - LINEAR in x, so stepping x by 8
		// just adds 8*(column 0 of CamMat). One MatrixXVector per ROW plus three adds per point,
		// instead of 9 muls + 6 adds per point.
#ifdef FDS_GLATBENCH
		gbT0 = RDTSC();
#endif
		WobStep.X = 8.0*CamMat[0][0];
		WobStep.Y = 8.0*CamMat[1][0];
		WobStep.Z = 8.0*CamMat[2][0];
		for (y=0;y<=240;y+=8)
		{
			Direction1.X = 0.0-160.0;
			Direction1.Y = y-100.0;
			Direction1.Z = 256.0;
			MatrixXVector(CamMat,&Direction1,&WobRow);   // ray at x=0 for this row
			for (x=0;x<=320;x+=8)   // 41 points/row: ASM row stride = 40 cells*ES + 1 more (WOBTR.ASM:203) = 41 GridPoints
			{
				Direction1=WobRow;
				// (2) NO Vector_Norm here. It cost a sqrt + 3 divides per grid point and is
				// mathematically DEAD: the only use of Direction1 is
				//     Intersection = Origin + Direction*(a/Direction.Y)
				// which is invariant under scaling Direction by any k (k cancels between the
				// vector and the divisor). Same for Direction2/3, and for the
				// r = |Intersection-Origin| distance, which is that same invariant product.
				Direction2=Direction1;
				Origin2=Origin1;
				Direction3=Direction1;
				Origin3=Origin1;
				a=Radius-Origin1.Y;
				bb=Direction1.Y;

				c=-Radius-Origin1.Y;
				d=Direction1.Y;
				// (2) d and bb are BOTH Direction1.Y, so one reciprocal serves both
				// intersections - saves an FDIV (~39 cycles) per grid point.
				if (bb==0) X1=X2=0;
				else
				{
					float rbb = 1.0/bb;
					X1=a*rbb;
					X2=c*rbb;
				}
				Vector_SelfScale(&Direction1,X1);
				Vector_Add(&Origin1,&Direction1,&Intersection1);
				Vector_SelfScale(&Direction2,X2);
				Vector_Add(&Origin2,&Direction2,&Intersection2);
				if (X2<X1)
				{
					// (1) LUT PARKED: WOBCOS/WOBSIN are correct and remove the FSIN, but the
					// float->long index cast is expensive under FPU_LPrecision (RC is not
					// truncate, so Watcom must swap the control word per cast). Needs the
					// deterministic GLAT capture to measure before it ships.
					u=(Intersection2.X+cos(Intersection2.Z+WobPhase)*0.5)*0.5;
					v=(Intersection2.Z+sin(Intersection2.X+WobPhase)*0.5)*0.5;
					Intersection2.X-=Origin2.X;
					Intersection2.Y-=Origin2.Y;
					Intersection2.Z-=Origin2.Z;
					r=(sqrt(Intersection2.X*Intersection2.X+Intersection2.Y*Intersection2.Y+Intersection2.Z*Intersection2.Z)*32);
					if (r>255.0) r=255.0;
					r=255-r;
					if (r<1.0) r=1;
					b=r * 0.7;
					g= r*0.8;
//					r*= 0.5;
					g-=Frames /10;
					b-=Frames /20;

					if (g>255.0) g=255.0;
					if (g<1.0) g=1;
					if (b>255.0) b=255.0;
					if (b<1.0) b=1;

					u*=65536.0;
					v*=65536.0;
				}
				else
				{
					// (1) LUT PARKED: WOBCOS/WOBSIN are correct and remove the FSIN, but the
					// float->long index cast is expensive under FPU_LPrecision (RC is not
					// truncate, so Watcom must swap the control word per cast). Needs the
					// deterministic GLAT capture to measure before it ships.
					u=(Intersection1.X+cos(Intersection1.Z+WobPhase)*0.5)*0.5;
					v=(Intersection1.Z+sin(Intersection1.X+WobPhase)*0.5)*0.5;
					Intersection1.X-=Origin1.X;
					Intersection1.Y-=Origin1.Y;
					Intersection1.Z-=Origin1.Z;

					u*=65536.0;
					v*=65536.0;

					r=(sqrt(Intersection1.X*Intersection1.X+Intersection1.Y*Intersection1.Y+Intersection1.Z*Intersection1.Z)*32);
					if (r>255.0) r=255.0;
					r=255-r;
					if (r<1.0) r=1;
					b=r * 0.7;
					g= r*0.8;
					g-=Frames /10;
					b-=Frames /20;

					if (g>255.0) g=255.0;
					if (g<1.0) g=1;
					if (b>255.0) b=255.0;
					if (b<1.0) b=1;
				}
				r*=254.0;
				g*=254.0;
				b*=254.0;

				Plane_GP[j].U=u;
				Plane_GP[j].V=v;
				Plane_GP[j].R=r;
				Plane_GP[j].G=g;
				Plane_GP[j].B=b;


				X = x - 160;
				Y = y - 120;
				// First pass only: tabulate cos/sin of each point's CONSTANT angle base.
				// This also retires the per-frame sqrt (it was three identical
				// sqrt(X*X+Y*Y) calls - Code, Gfx, and the Sfx block repeating Code's).
				if (!WobTabReady)
				{
					float a200,a120;
					GridR = sqrt(X*X + Y*Y);
					a200 = GridR/200.0; a120 = GridR/120.0;
					WobC200[j] = cos(a200); WobS200[j] = sin(a200);
					WobC120[j] = cos(a120); WobS120[j] = sin(a120);
				}

			  if (Code) {
				CCosR2 = WobC200[j]*WobCosB - WobS200[j]*WobSinB;   // cos(a200 + ST/100)
				CSinR2 = WobS200[j]*WobCosB + WobC200[j]*WobSinB;   // sin(a200 + ST/100)

				u = X * 204.8 * -(Code_RS * 5);
				v = Y * 327.68 * -(Code_RS * 5);

				u1 = (u) * CSinR1  + (v) * CCosR1;
				v1 = (u) * CCosR1  - (v) * CSinR1;

				u2 = (u1) * CSinR2  + (v1) * CCosR2;
				v2 = (u1) * CCosR2  - (v1) * CSinR2;

				r = g = b = 127.0;

				r*=254.0;
				g*=254.0;
				b*=254.0;

				Code_GP[j].U=u2+32767;
				Code_GP[j].V=v2+32767;
				if (Code_GP[j].U > 65535) Code_GP[j].U = 65535;
				if (Code_GP[j].V > 65535) Code_GP[j].V = 65535;
				if (Code_GP[j].U < 0) Code_GP[j].U = 0;
				if (Code_GP[j].V < 0) Code_GP[j].V = 0;
			  }
//				Code_GP[j].R=r;
//				Code_GP[j].G=g;
//				Code_GP[j].B=b;




			  if (Gfx) {
				GCosR2 = WobC120[j]*WobCosB + WobS120[j]*WobSinB;   // cos(a120 - ST/100)
				GSinR2 = WobS120[j]*WobCosB - WobC120[j]*WobSinB;   // sin(a120 - ST/100)

				u = X * 204.8 * (Gfx_RS*20.0);
				v = Y * 327.68 * (Gfx_RS*20.0);

				u1 = (u) * GSinR1  + (v) * GCosR1;
				v1 = (u) * GCosR1  - (v) * GSinR1;

				u1*=10.0;	v1*=10.0;

				u2 = (u1) * GSinR2  + (v1) * GCosR2;
				v2 = (u1) * GCosR2  - (v1) * GSinR2;

				r = g = b = 127.0;

				r*=254.0;
				g*=254.0;
				b*=254.0;

				Gfx_GP[j].U=u2+32767;
				Gfx_GP[j].V=v2+32767;
				if (Gfx_GP[j].U > 65535) Gfx_GP[j].U = 65535;
				if (Gfx_GP[j].V > 65535) Gfx_GP[j].V = 65535;
				if (Gfx_GP[j].U < 0) Gfx_GP[j].U = 0;
				if (Gfx_GP[j].V < 0) Gfx_GP[j].V = 0;
			  }
//				Gfx_GP[j].R=r;
//				Gfx_GP[j].G=g;
//				Gfx_GP[j].B=b;


			  if (Sfx) {
				// angle = -(a200 + ST/100): cos is even, sin is odd
				CCosR2 =   WobC200[j]*WobCosB - WobS200[j]*WobSinB;
				CSinR2 = -(WobS200[j]*WobCosB + WobC200[j]*WobSinB);

				u = X * 204.8 * -(Code_RS * 5);
				v = Y * 327.68 * -(Code_RS * 5);

				u1 = (u) * CSinR1  + (v) * CCosR1;
				v1 = (u) * CCosR1  - (v) * CSinR1;

				u2 = (u1) * CSinR2  + (v1) * CCosR2;
				v2 = (u1) * CCosR2  - (v1) * CSinR2;

				r = g = b = 127.0;

				r*=254.0;
				g*=254.0;
				b*=254.0;

				Sfx_GP[j].U=u2+32767;
				Sfx_GP[j].V=v2+32767;
				if (Sfx_GP[j].U > 65535) Sfx_GP[j].U = 65535;
				if (Sfx_GP[j].V > 65535) Sfx_GP[j].V = 65535;
				if (Sfx_GP[j].U < 0) Sfx_GP[j].U = 0;
				if (Sfx_GP[j].V < 0) Sfx_GP[j].V = 0;
			  }





				j++;
				WobRow.X += WobStep.X;      // (3) next x, no matrix multiply
				WobRow.Y += WobStep.Y;
				WobRow.Z += WobStep.Z;
			}
		}

#ifdef FDS_GLATBENCH
		gbGrid += TscDelta(gbT0,RDTSC());
		gbT0 = RDTSC();
#endif
		Grid_Texture_Mapper_Gouraud(Plane_GP,PlaneImage,(DWord *)Page1);   // distance-color modulation (was _MMX = texture only)
		// Only one of Code/Gfx/Sfx is ever live (they are one-way time gates: Code until
		// T=1100, Gfx 1100..2300, Sfx after), and each surface is read ONLY by the
		// correspondingly gated Modulate pair below. Mapping the other two was three
		// full-screen 320x240 texture passes per frame that nothing ever sampled.
		WobTabReady = 1;   // the grid loop above has now filled the constant-angle tables
#ifdef FDS_GLATBENCH
		gbPlane += TscDelta(gbT0,RDTSC());
		gbT0 = RDTSC();
#endif
#ifdef FDS_WOBTILE
		// Tiled (cache-friendly) fetch, ON by default; build FDS_WOBROW=1 for the row-major A/B.
		// VERIFIED equivalent to Grid_Texture_MapC, not by eye: a host harness runs both mappers
		// over 48 full 320x240 frames - 24 layer-style grids and 24 plane-style ones with the
		// multi-million U/V and >16-bit deltas the raytraced plane produces - and the two
		// destination buffers are byte-identical. (An earlier "renders washed-out" note here was
		// from the first, naive per-pixel-swizzle attempt; it does not apply to this form.)
		// Reads +12% slower under DOSBox, which does NOT model the L1/L2 cache: it charges the
		// extra index ALU and refunds nothing for the misses it cannot see. On a P55C the
		// 256KB row-major texture misses ~per pixel (a 1-texel vertical step jumps 1024B), so
		// the strip layout is what pays; measure this one on 86Box or real hardware only.
		if (Code) Grid_Texture_MapC_Tiled((DWord *)CodeImage->Data,Code_GP,(DWord *)Page2,(long)(YRes>>3));
		if (Gfx)  Grid_Texture_MapC_Tiled((DWord *)GfxImage->Data, Gfx_GP, (DWord *)Page3,(long)(YRes>>3));
		if (Sfx)  Grid_Texture_MapC_Tiled((DWord *)SfxImage->Data, Sfx_GP, (DWord *)Page4,(long)(YRes>>3));
#else
		if (Code) Grid_Texture_Mapper_320(Code_GP,CodeImage,(DWord *)Page2);
		if (Gfx)  Grid_Texture_Mapper_320(Gfx_GP,GfxImage,(DWord *)Page3);
		if (Sfx)  Grid_Texture_Mapper_320(Sfx_GP,SfxImage,(DWord *)Page4);
#endif
#ifdef FDS_GLATBENCH
		gbLayer += TscDelta(gbT0,RDTSC());
		gbN++;
		if (gbN==60||gbN==200)
		{
			// Cycles per call. Wall-clock FPS is NOT comparable across GLAT runs (real-time
			// ST changes which layer is live), but cycles/call is - it is the metric to quote
			// for any wobbler change.
			fprintf(LogFile,"GLATBENCH n=%ld ST=%ld  grid=%.0f  planeMap=%.0f  layerMap=%.0f "
			                "(cycles/frame; grid=raytrace+UV, planeMap=MMX ASM, layerMap=C)\n",
			        gbN,(long)ST,gbGrid/gbN,gbPlane/gbN,gbLayer/gbN);
			fflush(LogFile);
		}
#endif

		if (Code)
		{
			Modulate(&Surf1,&Surf2,0xa0a0a0,0xa0a0a0);
			Modulate(&Surf2,Screen,0xa0a0a0,0xa0a0a0);
		}
		if (Gfx)
		{
			Modulate(&Surf1,&Surf3,0xa0a0a0,0xd0d0d0);
			Modulate(&Surf3,Screen,0xa0a0a0,0xa0a0a0);
 //			Modulate(&Surf2,&Surf3,0xa0a0a0,0xa0a0a0);
		}
		if (Sfx)
		{
			Modulate(&Surf1,&Surf4,0xa0a0a0,0xa0a0a0);
			Modulate(&Surf4,Screen,0xa0a0a0,0xa0a0a0);
		}
		if (Timer>3200)
			Cross_Fade(VPage,(char *)LogoImage->Data,VPage,(Timer-3200)*256/300);
#ifdef FDS_SNAP
		// Ungated these cost ~460KB of DOS disk I/O mid-scene. Diagnostic only.
		{ static long _gl=0;
		  if (_gl==30||_gl==90) {
		    char fn[32]; sprintf(fn,"GLAT%ld.PPM",_gl);
		    FILE *pf=fopen(fn,"wb");
		    if (pf) { unsigned char *px=(unsigned char *)VPage; long ii,nn=(long)XRes*(long)YRes;
		      fprintf(pf,"P6\n%d %d\n255\n",(int)XRes,(int)YRes);
		      for(ii=0;ii<nn;ii++){ fputc(px[ii*4+2],pf); fputc(px[ii*4+1],pf); fputc(px[ii*4+0],pf); } fclose(pf); } }
		  _gl++; }
#endif
		Show_FPS();
		Flip(Screen);

		Rx += 0.01;
		Ry += 0.01;
		CameraPos.Z += 0.01;
		Frames++;

//      r1,r2
// bg  code      gfx sfx

//		Flip(Screen);
	}
	Keyboard[ScBackSpace]=0;   // consume the skip key so it doesn't cascade into the next scene
	if (Keyboard[ScESC])
	{
		#ifdef Play_Music
		ShutDown();
		#endif
		FDS_End();
		exit(-1);
	}


	delete Plane_GP;
	delete Code_GP;
	delete Gfx_GP;
	delete Sfx_GP;
	delete Page1;
	delete Page2;
	delete Page3;
	delete Page4;
}
