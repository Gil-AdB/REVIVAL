#include "Rev.h"

#include "CRASH.H"
#include "SceneTick.h"
#include "Scenes.h"
#include <memory>

static Scene *CrashSc;

static int32_t CrPartTime = 0;// 10.0f * 100;

void Initialize_Crash()
{
	CrashSc = (Scene *)getAlignedBlock(sizeof(Scene), 16);
	memset(CrashSc,0,sizeof(Scene));
	LoadFLD(CrashSc,"SCENES/CRASH.FLD");
	CrPartTime = 1.5f*100.0f*(CrashSc->EndFrame-CrashSc->StartFrame)/30.0f;

//	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	CrashSc->FZP = 2000.0f;
	CrashSc->NZP = 10.0f;
	CrashSc->Flags |= Scn_ZBuffer;
	
	CrashSc->Ambient.R = 64;
	CrashSc->Ambient.G = 64;
	CrashSc->Ambient.B = 64;
	// also make the appropriate Layer 2 fillers,
	Material *M;
	for (M = MatLib; M; M = M->Next)
	{
		//if (M->RelScene != CrashSc) continue;
		if (!M->Txtr) continue;
		M->Flags |= Mat_RGBInterp;
		M->Txtr->Flags |= Txtr_Tiled;
	}

	Preprocess_Scene(CrashSc);
}

namespace {
struct CrashScene : SceneDriver {
	char MSGStr[128];
	std::unique_ptr<Face*[]> fListStorage;
	std::unique_ptr<Face*[]> sListStorage;

	~CrashScene() override { Destroy_Scene(CrashSc); }

	void init() override {
		int32_t Polys = 0;
		TriMesh *T;
		Omni *O;

		SetCurrentScene(CrashSc);

		for(T = CrashSc->TriMeshHead; T; T=T->Next)
			Polys += T->FIndex;
		for (O = CrashSc->OmniHead; O; O = O->Next) {
			O->IRange = 2000.0f;
			Polys++;
		}

		fListStorage = std::make_unique<Face*[]>(Polys);
		sListStorage = std::make_unique<Face*[]>(Polys);
		FList = fListStorage.get();
		SList = sListStorage.get();

		View = CrashSc->CameraHead;

		C_FZP = CrashSc->FZP;
		C_rFZP = 1.0f / C_FZP;

		Ambient_Factor = 1;
		Diffusive_Factor = 1;
		ImageSize = 1;
	}

	bool tick() override {
		if (Keyboard[ScESC] || Timer >= CrPartTime) return false;

		g_FrameTime = Timer;

		if (Keyboard[ScTab]) {
			if (View == &FC)
				View = CrashSc->CameraHead;
			else
				View = &FC;
		}

		//memset(VPage,0,PageSize);
		FastWrite(VPage, 0, (PageSize + XRes * YRes * sizeof(word)) >> 2);

		CurFrame = CrashSc->StartFrame + (CrashSc->EndFrame-CrashSc->StartFrame) * ((float)g_FrameTime / (float)CrPartTime);

		Dynamic_Camera();
		if (Keyboard[ScC]) { FC.ISource = View->ISource; Matrix_Copy(FC.Mat, View->Mat); FC.IFOV = View->IFOV; }

		Animate_Objects(CrashSc);

		Transform_Objects(CrashSc);

		Lighting(CrashSc);

		if (!CAll) return true;
		Radix_SortingASM(FList, SList, CAll);

		Render();

		dword scroll = 0;
		snprintf(MSGStr, sizeof(MSGStr), "g_FrameTime %d", g_FrameTime);
		scroll += OutTextXY(VPage, 0, scroll, MSGStr, 255);
		snprintf(MSGStr, sizeof(MSGStr), "CrPartTime %d", CrPartTime);
		scroll += OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);
		snprintf(MSGStr, sizeof(MSGStr), "Frame %f/%f", CurFrame, CrashSc->EndFrame);
		scroll += OutTextXY(VPage, 0, scroll + 15, MSGStr, 255);

		Flip(MainSurf);
		return true;
	}

	void cleanup() override {
		Timer -= CrPartTime;

		// Wait for the user to release ESC before returning, otherwise the
		// next scene will immediately see Keyboard[ScESC] and bail. In the
		// eventual Emscripten port this will need to become a tick state.
		while (Keyboard[ScESC]) continue;
		if (Timer < 0) Timer = 0;
	}
};
} // anonymous namespace

std::unique_ptr<SceneDriver> createCrashScene()
{
	return std::make_unique<CrashScene>();
}

void Run_Crash()
{
	auto scene = createCrashScene();
	runSceneBlocking(*scene);
}