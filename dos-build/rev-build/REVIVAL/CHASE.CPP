#include "Rev.H"

#include "Chase.H"

static Scene *ChaseSc;

static long CHPartTime = 200.0f*100;

void Initialize_Chase()
{
	ChaseSc = new Scene; memset(ChaseSc,0,sizeof(Scene));
	LoadFLD(ChaseSc,"Scenes\\CHASE.FLD");
	CHPartTime = 100.0*(ChaseSc->EndFrame-ChaseSc->StartFrame)/30.0f;
//	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
	Preprocess_Scene(ChaseSc);
//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	ChaseSc->FZP = 25000.0f;
	ChaseSc->Flags |= Scn_Fogged;
	// also make the appropriate Layer 2 fillers.
}

void Run_Chase()
{
	long Polys = 0;
	TriMesh *T;

	CurScene = ChaseSc;
	for(T = ChaseSc->TriMeshHead;T;T=T->Next)
		Polys+=T->FIndex;
	FList = new Face * [Polys];
	SList = new Face * [Polys];

	View = ChaseSc->CameraHead;

	C_FZP = ChaseSc->FZP;
	C_rFZP = 1.0f/C_FZP;

	while ((!Keyboard[ScESC])&&Timer<CHPartTime)
	{
		memset(VPage,0,PageSize);

		CurFrame = ChaseSc->StartFrame + (ChaseSc->EndFrame-ChaseSc->StartFrame) * (float)Timer / (float)CHPartTime;

		Animate_Objects(ChaseSc);

		Transform_Objects(ChaseSc);

		Lighting(ChaseSc);

		if (!CAll) continue;
		Radix_SortingASM(FList,SList,CAll);

		Render();

		Flip(Screen);

	} Timer-=CHPartTime;
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
	Destroy_Scene(ChaseSc);
}