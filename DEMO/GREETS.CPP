#include "Rev.h"

#include "GREETS.H"
#include "FillerTest.h"
#include "MISC/PREPROC.H"
#include "Base/FDS_DECS.H"
#include <vector>
#include <VESA/Vesa.h>

static float frand() {
	return static_cast <float> (RAND_15()) / static_cast <float> (RAND_15_MAX);
}


static Scene *GreetSc;

static int32_t CHPartTime = 20.0f*100;

// This function is called to alter parameters within scene objects after loading from FLD.
static void SceneCorrections()
{
	// a crazy hack used to adjust omnilights in code.
	float OmniRangeMult[] = 
	//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
		{2.0,  2.0,  2.0, 2.0,  2.0,  1.7,  1.7,  2.0,  2.0,  2.0};
//		{0.9,  0.9,  0.3,  0.3,  0.4,  2.0,  2.0,  0.5,  0.5,  0.5};
//		{0.01,  0.01,  0.01,  0.01,  0.01,  1.0,  1.0,  0.01,  0.01,  0.01};
	float OmniSizeMult[] =
	//	ent1  ent2  ctr1  ctr2  crd1  hal1  hal2  rbtl  rbtr  rbth
		{1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.5,  1.5,  1.5};
	int OmniDisable[] = 
		{  0,    0,    0,    0,    0,    0,    0,    0,    0,    0};

	int OmniPointFlare[] =
		{  0,    0,    0,    0,    0,    0,    0,    0,    0,    0};

	int OmniStationary[] =
		{  1,	 1,	   1,    1,    1,    1,    1,    0,    0,    0};

	int NumOmnis = sizeof(OmniDisable)/sizeof(int);

	Scene *Sc = GreetSc;
	Omni *O;
	int32_t index;
	for(O=Sc->OmniHead, index=0; O; O=O->Next)
	{
		if (index == NumOmnis) break;

		// disable/enable omnies
		if (OmniDisable[index]) O->Flags &=~Omni_Active;
		if (OmniPointFlare[index]) SetPointFlare(O);
		if (OmniStationary[index]) O->Flags |= Omni_Stationary;

		// adjust size / disable flares
		if (OmniSizeMult[index] == 0)
			O->Flags &=~Omni_Flare;
		Spline_Scale(&O->Size, OmniSizeMult[index]);

		// adjust range
		Spline_Scale(&O->Range, OmniRangeMult[index]);
		index++;
	}

	int ObjStationary[] =
	//    ??? ??? ??? ??? ??? ??? ??? ??? ???
		{	1,	0,	0,	0,	0,	0,	0,	0,	0,};
	int NumObjects = sizeof(ObjStationary)/sizeof(int);

	TriMesh *T;
	for(T=Sc->TriMeshHead, index=0; T; T=T->Next, index++)
	{
		if (ObjStationary[index])
		{
			if (!(T->Flags & Tri_Stationary))
			{
				//T->VIndex
				T->SL = (Color *)getAlignedBlock(sizeof(Color) * T->VIndex, 16);
				T->Flags |= Tri_Stationary;
			}
		}
	}
}

#define TRIG_ACC 512 //trigonometric table accuracy. must be a power of 2.
#define TRIG_MASK (TRIG_ACC-1)
#define TRIG_FACTOR (512.0/PI_M2)
#define ARRAYSIZE(a) (sizeof((a))/sizeof((*a)))

struct GreetsGenerator
{
#define numGridPoints (((256 >> 3) + 1)*((256 >> 3) + 1))

#define TEXRES 256

	Texture CodeTexture;
	Image CodeImage;
	Image OldImage;
	GridPointT *Code_GP;
	GridPointT* Smear_GP;
	float LenTable[numGridPoints];
	float SinTable[TRIG_ACC];
	float CosTable[TRIG_ACC];
	DWord* ScaledBuf;
	DWord *CodeBuf;
	DWord* OldBuf;
	DWord *OutBuf;
	float scalex = 1.0;
	float scaley = 1.0;

	static constexpr const char* greetingsRound1[] = {
		"MRI",
		"imr",
		"Cycat",
		"coolphat",
		"borzom",
		"civax",
		"yoe",
	};

	static constexpr const char* greetingsRound2[] = {
		"Doomsday",
		"esteem",
		"adept",
		"kombat",
		"rage",
		"magic intros",
		"calius"
	};

	static constexpr const char* greetingsRound3[] = {
		" Revival\nBy  Flood"
	};


	struct GreetsEntry {
		GreetsEntry(const char* text): text(text) {
			OutTextXYExtents(12, 0, text, 256, 256, x, y);
			// y -= 2;
		}

		const char* text;
		int32_t x, y;
	};

	GreetsEntry ge0 { "Flood\nGreets" };
	std::vector<GreetsEntry> ge1{ std::begin(greetingsRound1) , std::end(greetingsRound1) };
	std::vector<GreetsEntry> ge2{ std::begin(greetingsRound2) , std::end(greetingsRound2) };
	std::vector<GreetsEntry> ge3{ std::begin(greetingsRound3) , std::end(greetingsRound3) };
	//GreetsEntry ge_empty { "" };


	void AttachMatToScene()
	{
		Material *Mat;
		for (Mat = MatLib; Mat->Next; Mat = Mat->Next) {
			if (Mat->RelScene == GreetSc && Mat->Txtr && Mat->Txtr->FileName && !strcmp(Mat->Txtr->FileName, "Textures/p_text.jpg"))
			{
				Mat->Txtr->Mipmap[0] = (byte *)OutBuf;
				Mat->Txtr->Flags = Txtr_Nomip;
				Mat->Txtr->numMipmaps = 0;
			}
		}

	}


	GreetsGenerator() {}

	void Init()
	{
		OutBuf = (DWord*)_aligned_malloc(256 * 256 * 4, 16);
		CodeBuf = (DWord *)_aligned_malloc(256 * 256 * 4, 16);
		OldBuf = (DWord*)_aligned_malloc(256 * 256 * 4, 16);
		ScaledBuf = (DWord*)_aligned_malloc(256 * 256 * 4, 16);


		CodeTexture.FileName = strdup("Textures/Code.JPG");
		Identify_Texture(&CodeTexture);
		if (!CodeTexture.BPP)
		{
			printf("Error Loading texture!\n");
			exit(1);
		}
		Load_Texture(&CodeTexture);
		Convert_Texture2Image(&CodeTexture, &CodeImage);
		memset(CodeImage.Data, 0, sizeof(uint32_t) * CodeImage.x * CodeImage.y);
		OldImage.Data = OldBuf;
		OldImage.x = 256;
		OldImage.y = 256;

		memset(OutBuf, 0, 256 * 256 * 4);

		Code_GP = new GridPointT[numGridPoints];
		Smear_GP = new GridPointT[numGridPoints];

		AttachMatToScene();
		ST = 0.0;
	}
	float ST;
	void Render() {
		float Code_R1, Code_RS, Code_R2, CCosR1, CSinR1, CCosR2, CSinR2;
		static float u, v, u1, v1, u2, v2, r, g, b;
		//float ST = (Timer * 2500) / (1000 + Timer);//  sqrt(Timer*1600);

		double greetTime = 0;
		GreetsEntry* greet;
		const int GREET1_START_FRAME = 350;
		const int GREET1_END_FRAME = 730;
		const int GREET2_START_FRAME = 900;
		const int GREET2_END_FRAME = 1200;
		const int GREET3_START_FRAME = 2000;
		const int GREET3_END_FRAME = 2500;
		scalex = fabs(sin(Timer / 60.0)) * 0.2;
		scaley = fabs(cos(Timer / 47.0)) * 0.2;
		if (CurFrame < GREET1_START_FRAME) {
			greet = &ge0;
		}
		else if (CurFrame < GREET1_END_FRAME) {
			greetTime = double(CurFrame - GREET1_START_FRAME) / (GREET1_END_FRAME - GREET1_START_FRAME);
			greet = &ge1[size_t(greetTime * 0.9999 * ge1.size())];
		}
		else if (CurFrame < GREET2_START_FRAME) {
			greet = &ge0;
		}
		else if (CurFrame < GREET2_END_FRAME) {
			greetTime = double(CurFrame - GREET2_START_FRAME) / (GREET2_END_FRAME - GREET2_START_FRAME);
			greet = &ge2[size_t(greetTime * 0.9999 * ge2.size())];
		}
		else if (CurFrame < GREET3_START_FRAME) {
			greet = &ge0;
		}
		else if (CurFrame < GREET3_END_FRAME) {
			greetTime = double(CurFrame - GREET3_START_FRAME) / (GREET3_END_FRAME - GREET3_START_FRAME);
			greet = &ge3[size_t(greetTime * 0.9999 * ge3.size())];
			scalex = 0.0;
			scaley = 0.0;

		}
		else {
			greet = &ge0;
		}

		memset(CodeImage.Data, 0, sizeof(uint32_t) * CodeImage.x * CodeImage.y);
		OutTextXY((byte*)(CodeImage.Data), (CodeImage.x - greet->x) / 2, (CodeImage.y - greet->y) / 2, greet->text, 255, CodeImage.x, CodeImage.y);

		int X, Y;
		int j = 0;
		ST = (Timer * 2500) / (1000 + Timer);//  sqrt(Timer*1600);

		for (int y = 0; y <= TEXRES; y += 8) {
			for (int x = 0; x <= TEXRES; x += 8)
			{
				u = (x / 256.0f - 0.5) * (0.25 +scalex) + 0.5;
				v = (y / 256.0f - 0.5) * (0.25 +scaley) + 0.5;

				Code_GP[j].u = u * 65536.0;
				Code_GP[j].v = v * 65536.0;
				if (Code_GP[j].u > 65535) Code_GP[j].u = 65535;
				if (Code_GP[j].v > 65535) Code_GP[j].v = 65535;
				if (Code_GP[j].u < 0) Code_GP[j].u = 0;
				if (Code_GP[j].v < 0) Code_GP[j].v = 0;

				j++;
			}
		}

		GridRendererT(Code_GP, &CodeImage, CodeBuf, TEXRES, TEXRES);

		j = 0;
		for (int y = 0; y <= TEXRES; y += 8) {
			for (int x = 0; x <= TEXRES; x += 8)
			{
				u = (x / 256.0f - 0.5) * 0.98 + 0.5;
				v = (y / 256.0f - 0.5) * 0.95 + 0.5;

				Smear_GP[j].u = u * 65536.0;
				Smear_GP[j].v = v * 65536.0;
				if (Smear_GP[j].u > 65535) Smear_GP[j].u = 65535;
				if (Smear_GP[j].v > 65535) Smear_GP[j].v = 65535;
				if (Smear_GP[j].u < 0) Smear_GP[j].u = 0;
				if (Smear_GP[j].v < 0) Smear_GP[j].v = 0;

				j++;
			}
		}

		GridRendererT(Smear_GP, &OldImage, ScaledBuf, TEXRES, TEXRES);

		auto cola1 = uint32_t(frand() * 100) + 150;
		auto colb1 = uint32_t(frand() * 100) + 150;
		auto colc1 = uint32_t(frand() * 100) + 150;
		auto cold1 = uint32_t(frand() * 100) + 150;

		auto cola2 = uint32_t(frand() * 100) + 150;
		auto colb2 = uint32_t(frand() * 100) + 150;
		auto colc2 = uint32_t(frand() * 100) + 150;
		auto cold2 = uint32_t(frand() * 100) + 150;


		DWord pSrc = (cola1 << 24) + (colb1 << 16) + (colc1 << 8) + cold1;
		DWord pDst = (cola2 << 24) + (colb2 << 16) + (colc2 << 8) + cold2;
		AlphaBlend((byte*)CodeBuf, (byte *)ScaledBuf, pSrc, pDst, 256 * 256 * 4);
		//AlphaBlend((byte*)ScaledBuf, (byte*)OldBuf, pSrc, pDst, 256 * 256 * 4);
		memcpy(OldBuf, ScaledBuf, 256 * 256 * 4);

		TexutreRemap();
	}

	void TexutreRemap() {
		// right now, there is no support for non-blocked textures,
		// so we need to rearrange TempBuf by 4x4 blocks
		//memcpy(VorSurf, TempBuf, 65536 * 4);
		int X, Y;
		DWord *writePtr = OutBuf;
		for (X = 0; X < 64; X++)
			for (Y = 0; Y < 64; Y++)
			{
				dword *blockPtr = OldBuf + ((X + (Y << 8)) << 2);
				int x, y;
				for (y = 0; y < 4; y++)
					for (x = 0; x < 4; x++)
						*writePtr++ = blockPtr[x + (y << 8)];
			}
	}

	~GreetsGenerator() {
		delete OutBuf;
		delete CodeTexture.Data;
		delete CodeImage.Data;
	}


};

GreetsGenerator *gg;

void Initialize_Greets()
{	
	GreetSc = (Scene *)getAlignedBlock(sizeof(Scene), 16);
	memset(GreetSc,0,sizeof(Scene));
	
	LoadFLD(GreetSc,"SCENES/Greets.FLD");

	gg = new GreetsGenerator;

	SceneCorrections();

	// 5 seconds ending time
	CHPartTime = 500.0 + 100.0f*(GreetSc->EndFrame-GreetSc->StartFrame)/30.0f;


	for(Material *M = MatLib;M;M=M->Next)
	{
		if (M->RelScene != GreetSc) continue;
		if (!M->Txtr) continue;
		M->Flags |= Mat_RGBInterp;
		M->Txtr->Flags |= Txtr_Tiled;
	}


//	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	GreetSc->NZP = 0.01f;
	GreetSc->FZP = 150.0f;
//	GreetSc->Flags |= Scn_Fogged;
	Preprocess_Scene(GreetSc);
	// also make the appropriate Layer 2 fillers,

	GreetSc->Ambient.R = 32;
	GreetSc->Ambient.G = 32;
	GreetSc->Ambient.B = 32;

	GreetSc->PathingMinVelocity = 0.01;

	gg->Init();

}

//void MMXClear(void *Page, dword Len)
//{
//	dword Addr = (dword)Page;
//	__asm 
//	{
//		mov ecx, dword ptr [Len]
//		mov edi, dword ptr [Addr]
//		pxor MM0, MM0
//		xor eax, eax
//
//;		shr ecx, 2
//;		rep stosd
//		add edi, ecx
//		neg ecx
//		Inner:
//			movq [edi+ecx], MM0
//;			movq MM0, [edi+ecx]
//			add ecx, 8
//
//;			mov [edi+ecx], eax
//;			mov eax, [edi+ecx]
//;			add ecx, 4
//		jnz Inner
//		
//		emms
//	}
//}

void Run_Greets()
{
	char MSGStr[128];
	int32_t Polys = 0, TTrd;
	TriMesh *T;
	Omni *O;

	int32_t i, timerStack[20], timerIndex = 0;
	for(i=0; i<20; i++)
		timerStack[i] = Timer;

	SetCurrentScene(GreetSc);
//	for(T = GreetSc->TriMeshHead;T;T=T->Next)
	for(Object *Obj = GreetSc->ObjectHead;Obj;Obj=Obj->Next)
	{
		if (Obj->Type != Obj_TriMesh) continue;
		T = (TriMesh *)Obj->Data;
//		if (!stricmp(Obj->Name, "piramid.lwo"))
//			T->Flags &=~HTrack_Visible;
		Polys+=T->FIndex;
	}
		
	for(O = GreetSc->OmniHead;O;O=O->Next)
		Polys++;

	FList = new Face * [Polys];
	SList = new Face * [Polys];

	View = GreetSc->CameraHead;

	Ambient_Factor = 0.25;
	Diffusive_Factor = 1.0;
	Specular_Factor = 0.0;
	Range_Factor = 1.0;
	ImageSize = 0.25;//2.5;

	dword numFrames = 0;


	// this scene messes up at time zero
	Timer++;

	TTrd = Timer;
	g_renderedPolys = 0;
	FillerPixelcount = 0;


	dword Profiler[PROF_NUM];
	float ProfSample[PROF_NUM];
	float ProfPerc[PROF_NUM];
	const char *ProfNames[] = {"ZCLR","ANIM","XFRM", "LGHT", "SORT", "RNDR", "FLIP"};
	dword ProfSum = 0;

	for(i=0; i<PROF_NUM; i++)
		Profiler[i] = 0;	


//	View = &FC;	
	while ((!Keyboard[ScESC])&&Timer<CHPartTime)
	{
		numFrames++;


//		PROFILER: ZClear phase. also includes control keys handling
		Profiler[PROF_ZCLR] -= Timer;


		if (Keyboard[ScTab])
		{
			if (View == &FC)
				View = GreetSc->CameraHead;
			else
				View = &FC;
		}

		
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
		
		// Clear Framebuffer and ZBuffer
//		memset(VPage,0,PageSize + XRes*YRes*sizeof(word));
		FastWrite(VPage, 0, (PageSize + XRes*YRes*sizeof(word)) >> 2);

//		PROFILER: ANIM phase. also includes Dynamic Camera manager
		Profiler[PROF_ANIM-1] += Timer;
		Profiler[PROF_ANIM] -= Timer;

		if (Timer < CHPartTime-500.0)
			CurFrame = GreetSc->StartFrame + (GreetSc->EndFrame-GreetSc->StartFrame) * (float)g_FrameTime / (float)(CHPartTime-500.0);
		else
			CurFrame = GreetSc->EndFrame;

		Dynamic_Camera();
		if (Keyboard[ScC]) {FC.ISource = View->ISource; Matrix_Copy(FC.Mat,View->Mat); FC.IFOV=View->IFOV;}


		Animate_Objects(GreetSc);

//		PROFILER: XFRM phase. 
		Profiler[PROF_XFRM-1] += Timer;
		Profiler[PROF_XFRM] -= Timer;

		Transform_Objects(GreetSc);

//		PROFILER: LGHT phase. 
		Profiler[PROF_LGHT-1] += Timer;
		Profiler[PROF_LGHT] -= Timer;

		Lighting(GreetSc);

//		PROFILER: SORT phase. 
		Profiler[PROF_SORT-1] += Timer;
		if (!CAll) continue;
		Profiler[PROF_SORT] -= Timer;

		Radix_SortingASM(FList,SList,CAll);

//		PROFILER: RNDR phase. 
		Profiler[PROF_RNDR-1] += Timer;
		Profiler[PROF_RNDR] -= Timer;


        gg->Render();
		// reset per-frame zbuffer statistics
		zPass = zReject = 0;
		Render();
//		memset(VPage,0,PageSize);
//		MMXClear(VPage,PageSize);
//		FillerPixelcount += XRes*YRes;

//		PROFILER: FLIP phase. also contains display of runtime stats
		Profiler[PROF_FLIP-1] += Timer;
		if (g_profilerActive)
		{
			// FPS printer
			timerStack[timerIndex++] = Timer;
			if (timerIndex==20) 
			{
				timerIndex = 0;
				sprintf(MSGStr,"%f FPS", 2000.0/(float)(timerStack[19]-timerStack[timerIndex]) );
			} else {
				sprintf(MSGStr,"%f FPS", 2000.0/(float)(timerStack[timerIndex-1]-timerStack[timerIndex]) );
			}
			dword scroll = 0;
			scroll = OutTextXY(VPage, 0, 0, MSGStr, 255);
			sprintf(MSGStr, "Frame %f", CurFrame);
			scroll = OutTextXY(VPage,0, scroll+15,MSGStr,255);
			sprintf(MSGStr, "%d polys/frame", (int)(g_renderedPolys / numFrames));
			scroll = OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);
			sprintf(MSGStr, "%dK pixels/frame" , (int)(FillerPixelcount/(1000.0*numFrames)));
			scroll = OutTextXY(VPage,0,scroll+15,MSGStr,255);
			sprintf(MSGStr, "%dK pixels/second" , (int)(FillerPixelcount/1000.0 * 100.0 / Profiler[PROF_RNDR]));
			scroll = OutTextXY(VPage,0,scroll+15,MSGStr,255);

	//		PROFILER: display stats	
			for(i=0; i<PROF_NUM; i++)
			{
				sprintf(MSGStr, "%s %3.1fms (%3.1f%%)" , ProfNames[i], ProfSample[i], ProfPerc[i]);
				scroll = OutTextXY(VPage,0,scroll+15,MSGStr,255);
			}

			sprintf(MSGStr, "TOTL %3.1fms" , ProfSum*10.0/ numFrames);
			scroll = OutTextXY(VPage,0,scroll+15,MSGStr,255);

	//		sprintf(MSGStr, "%d percent Z-rejection", (int32_t) ( (float)zReject*100.0/ (zReject+zPass) ));
	//		OutTextXY(VPage,0,45,MSGStr,255);
	//		sprintf(MSGStr, "%d polys/frame" , (int32_t)(g_renderedPolys / numFrames) );
	//		OutTextXY(VPage,0,45,MSGStr,255);
			// display messages.
	/*		int32_t I, Y = -15;
			for(I=0;I<10;I++)
			if (MsgStr[I])
			{
				Y = OutTextXY(VPage,0,Y+15,MsgStr[I],255);
				if (Timer>MsgClock[I]+150)
				{
					delete MsgStr[I];
					MsgStr[I]=NULL;
				}
			}*/
		}

		Profiler[PROF_FLIP] -= Timer;

#ifdef _C_WATCOM
		Flip(Screen);
#else
		Flip(MainSurf);
#endif
		//		PROFILER: frame finished
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

	} Timer-=CHPartTime;
//	if (Keyboard[ScESC])
//	{
//		#ifdef Play_Music
//		ShutDown();
//		#endif
//		FDS_End();
//		exit(-1);
//	}
	while (Keyboard[ScESC]) continue;
	if (Timer<0) Timer = 0;

	delete [] FList;
	delete [] SList;
	Destroy_Scene(GreetSc);
}