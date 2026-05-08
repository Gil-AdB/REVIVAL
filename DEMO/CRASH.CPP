#include "Rev.h"
#include <Threads.h>

#include "CRASH.H"
#include "FrameProfiler.h"
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

	Preprocess_Scene(CrashSc, "CRASH");
}

namespace {
struct CrashScene : SceneDriver {
	char MSGStr[128];
	std::unique_ptr<Face*[]> fListStorage;
	std::unique_ptr<Face*[]> sListStorage;
	FrameProfiler prof{"crash"};

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

		prof.beginFrame();
		prof.enter(PROF_ZCLR);

		g_FrameTime = Timer;

		if (Keyboard[ScTab]) {
			if (View == &FC)
				View = CrashSc->CameraHead;
			else
				View = &FC;
		}

		//memset(VPage,0,PageSize);
		parallel_memset(VPage, 0, PageSize); parallel_memset(ZPage16, 0, XRes * YRes * sizeof(word));

		CurFrame = CrashSc->StartFrame + (CrashSc->EndFrame-CrashSc->StartFrame) * ((float)g_FrameTime / (float)CrPartTime);

		prof.switchTo(PROF_ANIM);
		Dynamic_Camera();
		if (Keyboard[ScC]) { FC.ISource = View->ISource; Matrix_Copy(FC.Mat, View->Mat); FC.IFOV = View->IFOV; }

		Animate_Objects(CrashSc);

		prof.switchTo(PROF_XFRM);
		Transform_Objects(CrashSc);

		prof.switchTo(PROF_LGHT);
		Lighting(CrashSc);

		prof.leave(PROF_LGHT);
		if (!CAll) return true;
		prof.enter(PROF_SORT);
		Radix_SortingASM(FList, SList, CAll);

		prof.switchTo(PROF_RNDR);
		Render();
		prof.leave(PROF_RNDR);

		dword scroll = 0;
		snprintf(MSGStr, sizeof(MSGStr), "g_FrameTime %d", g_FrameTime);
		scroll += OutTextXY(VPage, 0, scroll, MSGStr, 255);
		snprintf(MSGStr, sizeof(MSGStr), "CrPartTime %d", CrPartTime);
		scroll += OutTextXY(VPage, 0, scroll + 15 * g_fontScale, MSGStr, 255);
		snprintf(MSGStr, sizeof(MSGStr), "Frame %f/%f", CurFrame, CrashSc->EndFrame);
		scroll += OutTextXY(VPage, 0, scroll + 15 * g_fontScale, MSGStr, 255);

		prof.enter(PROF_FLIP);
		Flip(MainSurf);
		prof.leave(PROF_FLIP);
		prof.endFrame();
		return true;
	}

	void cleanup() override {
		if (g_profilerActive) prof.dump();
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