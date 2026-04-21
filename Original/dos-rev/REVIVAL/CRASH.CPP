#include "Rev.H"

#include "Crash.H"

static Scene *CrashSc;

static long CrPartTime = 20.0f*100;

void Initialize_Crash()
{
	CrashSc = new Scene; memset(CrashSc,0,sizeof(Scene));
	LoadFLD(CrashSc,"Scenes\\Crash.FLD");
	CrPartTime = 100.0f*(CrashSc->EndFrame-CrashSc->StartFrame)/30.0f*100;

//	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
	Preprocess_Scene(CrashSc);
//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	CrashSc->FZP = 25000.0f;
	CrashSc->Flags |= Scn_Fogged;
	// also make the appropriate Layer 2 fillers,

}

void Run_Crash()
{
	long Polys = 0;
	TriMesh *T;
	Omni *O;

	CurScene = CrashSc;
	for(T = CrashSc->TriMeshHead;T;T=T->Next)
		Polys+=T->FIndex;
	for(O = CrashSc->OmniHead;O;O=O->Next)
		Polys++;

	FList = new Face * [Polys];
	SList = new Face * [Polys];

	View = &FC;//CrashSc->CameraHead;

	C_FZP = CrashSc->FZP;
	C_rFZP = 1.0f/C_FZP;

	Ambient_Factor = 40;
	Diffusive_Factor = 50;
	ImageSize = 400;
	while ((!Keyboard[ScESC])&&Timer<CrPartTime)
	{
		memset(VPage,0,PageSize);

		CurFrame = CrashSc->StartFrame + (CrashSc->EndFrame-CrashSc->StartFrame) * (float)Timer / (float)CrPartTime;

		Dynamic_Camera();

		Animate_Objects(CrashSc);

		Transform_Objects(CrashSc);

		Lighting(CrashSc);

		if (!CAll) continue;
		Radix_SortingASM(FList,SList,CAll);

		Render();

		Flip(Screen);

	} Timer-=CrPartTime;
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
	Destroy_Scene(CrashSc);
}