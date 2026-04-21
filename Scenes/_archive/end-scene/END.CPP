#include "Rev.h"

static Scene *Endc;

static int32_t CHPartTime = (int32_t) (20.0f*100);

void Initialize_End()
{
	Endc = (Scene *)getAlignedBlock(sizeof(Scene), 16);
	memset(Endc,0,sizeof(Scene));
	LoadFLD(Endc,"SCENES/End.FLD");
	CHPartTime = (int32_t)(100.0f*(Endc->EndFrame-Endc->StartFrame)/30.0f);

//	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
	Preprocess_Scene(Endc);
//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	Endc->FZP = 25000.0f;
	Endc->Flags |= Scn_Fogged;
	// also make the appropriate Layer 2 fillers,

}

void Run_End()
{
	int32_t Polys = 0;
	TriMesh *T;
	Omni *O;

	CurScene = Endc;
	for(T = Endc->TriMeshHead;T;T=T->Next)
		Polys+=T->FIndex;
	for(O = Endc->OmniHead;O;O=O->Next)
		Polys++;

	FList = new Face * [Polys];
	SList = new Face * [Polys];

	View = Endc->CameraHead;

	C_FZP = Endc->FZP;
	C_rFZP = 1.0f/C_FZP;

	Ambient_Factor = 16;
	Diffusive_Factor = 1500;
	ImageSize = 50;
	while ((!Keyboard[ScESC])&&Timer<CHPartTime)
	{
		memset(VPage,0,PageSize);

		CurFrame = Endc->StartFrame + (Endc->EndFrame-Endc->StartFrame) * (float)Timer / (float)CHPartTime;

		Animate_Objects(Endc);

		Transform_Objects(Endc);

		Lighting(Endc);

		if (!CAll) continue;
		Radix_SortingASM(FList,SList,CAll);

		Render();

		Flip(Screen);

	} Timer-=CHPartTime;
	/*
	if (Keyboard[ScESC])
	{
		#ifdef Play_Music
		ShutDown();
		#endif
		FDS_End();
		exit(-1);
	}*/

	delete FList;
	delete SList;
	Destroy_Scene(Endc);
}
