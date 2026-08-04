#include "Rev.H"

char *ModuleBuf;

void Destroy_Scene(Scene *Sc)
{
	TriMesh *Tri;
	long total = 0,f;

//	printf("Shootin' scene...\n");
	for(Tri=Sc->TriMeshHead;Tri;Tri=Tri->Next)
	{
		if (Tri->VIndex>5||Tri->FIndex>5)
		{
			delete Tri->Verts;
			delete Tri->Faces;
			f = sizeof(Vertex)*Tri->VIndex+sizeof(Face)*Tri->FIndex;
			total += f;
//			printf("Freed %d Bytes from mesh killing\n",f);
		}
	}
	for(Material *Mat = MatLib;Mat;Mat=Mat->Next)
	{
		if (Mat->RelScene == Sc)
			if (Mat->Txtr&&Mat->Txtr->Data)
			{
				 delete Mat->Txtr->Data;
				 Mat->Txtr->Data = NULL;
				 total += 262144;
//				 printf("Freed 262144 Bytes from Texture killing\n");
			}
	}
//	printf("%d Frags\n",total);

}


void Initialize_System()
{
#ifdef FDS_NOLOG
	LogFile = fopen("NUL","wt");   // deploy builds: every log write becomes a no-op device call
#else
	LogFile = fopen("Runtime.LOG","wt");
#endif
	fprintf(LogFile,"START\n");fflush(LogFile);

	if (!FDS_Init(320,240,32))    // Initialize FDS
	{
		printf("Initializing failed! Crashing to OS\n");
		exit(1);
	}
	fprintf(LogFile,"P1:FDS_Init returned BPP=%d\n",(int)BPP);fflush(LogFile);

#ifdef Play_Music
	ModuleBuf = LoadARJ("Music\\Music.ARJ","Revival.XM");
	Initialize_Module(ModuleBuf);
#endif
	Generate_RGBFlares();   // build Flare_IR/IG/IB glow textures that Fit_FlareImage assigns to omnis
	fprintf(LogFile,"P1c:postFlares\n");fflush(LogFile);
	if (BPP==8) { fprintf(LogFile,"P1a:preRemap\n");fflush(LogFile); Materials_Remap_Aligned(); fprintf(LogFile,"P1b:postRemap\n");fflush(LogFile); }
//  InitMouse();
	// all Hardwired Initializations complete
}

void Scan_MaterialLib()
{
	printf("Scanning MatLib\n");
	for(Material *M=MatLib;M;M=M->Next)
		printf("Material %s at Scene %xH\n",M->Name,(DWord)M->RelScene);
}

void main(int argc,char *argv[])
{
	Initialize_System();
	fprintf(LogFile,"MA:sysReturned\n");fflush(LogFile);

//	printf("MEM = %d\n",DPMI_Free_Memory());
//	Initialize_Credits();
//	printf("[Credits]MEM = %d\n",DPMI_Free_Memory());

//	printf("[City]MEM = %d\n",DPMI_Free_Memory());
//
//	printf("[Chase]MEM = %d\n",DPMI_Free_Memory());

//	printf("[Fountain]MEM = %d\n",DPMI_Free_Memory());

	setbuf(stdout,NULL);
	fprintf(LogFile,"MB:setbuf\n");fflush(LogFile);
	// Scene-transition profiling: Timer ticks at Timer_Freq=100Hz from the ISR, so
	// deltas print as seconds with two decimals.
	static long _t0;
#define TMARK(tag) {fprintf(LogFile,"INIT-TIME %s: %ld.%02lds free=%luKB\n",tag,(long)(Timer-_t0)/100,(long)(Timer-_t0)%100,(unsigned long)(DPMI_Free_Memory()>>10));fflush(LogFile);\
  {FILE *_p=fopen("PERF.TXT","at"); if(_p){fprintf(_p,"INIT-TIME %s: %ld.%02lds free=%luKB\n",tag,(long)(Timer-_t0)/100,(long)(Timer-_t0)%100,(unsigned long)(DPMI_Free_Memory()>>10));fclose(_p);}}\
  _t0=Timer;}
	_t0 = Timer;
	printf("Initializing, please wait...");
	fprintf(LogFile,"M12:preSetGfx\n");fflush(LogFile);
	SetGraphicsMode();
	fprintf(LogFile,"M13:postSetGfx\n");fflush(LogFile);

#ifdef FDS_MAPTEST
	// Mapper/clipper reproduction harness (TESTMAP.CPP). Runs the controlled-triangle
	// battery instead of the demo, dumps MT_*.PPM + results to the log, and exits.
	{ extern void Run_MapTest(void); Run_MapTest(); }
	FDS_End();
	return;
#endif

	// GREETS-CAPTURE FAST PATH: set to 1 to skip Glato/City/Fountain and jump straight to
	// greets (fast headless iteration). 0 = full demo sequence (the shipping order).
#if 0
	fprintf(LogFile,"SEQ:GREETS-ONLY fast path\n");fflush(LogFile);
	Initialize_Greets();
	TMARK("Initialize_Greets")
#else
	Initialize_City(); //9.3
	TMARK("Initialize_City")
	fprintf(LogFile,"M14:postInitCity\n");fflush(LogFile);
	Initialize_Fountain(); //8.3
	TMARK("Initialize_Fountain")
	fprintf(LogFile,"M15:postInitFnt(fnt skipped)\n");fflush(LogFile);
	// Greets used to initialize between Run_Fountain and Run_Greets, which reads as the demo
	// hanging on the last fountain frame while GREETS.FLD + its textures load. Init it here
	// with the others; the cost moves into the one loading screen the audience expects.
	Initialize_Greets(); //5.6
	TMARK("Initialize_Greets")
	Timer = 1;
	fprintf(LogFile,"SEQ:preGlato\n");fflush(LogFile);
	Run_Glato();
	while (Timer<50);
	Timer = 1;
	fprintf(LogFile,"SEQ:preCity\n");fflush(LogFile);
	Run_City();


	Timer = 1;
	fprintf(LogFile,"SEQ:preFountain\n");fflush(LogFile);
	Run_Fountain();
#endif

	// (Initialize_Greets moved up next to City/Fountain - this gap is now measured only.)
	{ long _tg=Timer;
	fprintf(LogFile,"INIT-TIME inter-scene GAP before greets: %ld.%02lds\n",(long)(Timer-_tg)/100,(long)(Timer-_tg)%100);fflush(LogFile); }
	Timer = 1;
	fprintf(LogFile,"SEQ:preGreets\n");fflush(LogFile);
	Run_Greets();
	fprintf(LogFile,"SEQ:done\n");fflush(LogFile);

//	Initialize_Crash();
	Timer = 1;

//	Run_Crash();
#ifdef Play_Music
	ShutDown();
#endif
	FDS_End();

//	printf("MEM = %d\n",DPMI_Free_Memory());
}