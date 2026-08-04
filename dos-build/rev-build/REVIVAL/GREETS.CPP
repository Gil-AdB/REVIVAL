#include "Rev.H"

#include "Greets.H"

// RDTSC (Pentium+) low 32 bits, for per-pass cycle profiling. Emit the opcode bytes
// (0F 31) directly so the assembler needn't recognize the mnemonic; returns EDX:EAX,
// we keep EAX (low 32 — deltas are far below the ~21s 32-bit wrap at 200 MHz).
extern "C" unsigned long RDTSC(void);
#pragma aux RDTSC = 0x0F 0x31 value [eax] modify [eax edx];

// Wrap-correct cycle delta, one conversion site for the profiler. Converting
// (double)(unsigned long)(t1-t0) inline intermittently yielded ~2^31 per frame (see CITY.CPP).
static double TscDelta(unsigned long t0,unsigned long t1)
{
  unsigned long d = t1 - t0;
  return (double)d;
}

static Scene *GreetSc;

static long CHPartTime = 20.0f*100;

void Initialize_Greets()
{
	GreetSc = new Scene; memset(GreetSc,0,sizeof(Scene));
	LoadFLD(GreetSc,"Scenes\\GREETS.FLD");
	CHPartTime = 100.0f*(GreetSc->EndFrame-GreetSc->StartFrame)/30.0f;

//	printf("FLD-loaded MEM = %d\n",DPMI_Free_Memory());
	Preprocess_Scene(GreetSc);
//	printf("Scene-Proc MEM = %d\n",DPMI_Free_Memory());
	GreetSc->NZP = 0.01f;    // master DEMO/GREETS.CPP:1253 - the mech comes right up to the lens
	GreetSc->FZP = 150.0f;   // was 25000 -> geometry is ~tens-of-units scale so fog never engaged (native uses 150)
	GreetSc->Flags |= Scn_Fogged;
	// also make the appropriate Layer 2 fillers,

#ifdef FDS_TEX_SWIZZLE
	Scene_SwizzleTextures(GreetSc); // one-shot: after all init-time texture processing
#endif
}

void Run_Greets()
{
	long Polys = 0;
	TriMesh *T;
	Omni *O;

	CurScene = GreetSc;
	for(T = GreetSc->TriMeshHead;T;T=T->Next)
		Polys+=T->FIndex;
	for(O = GreetSc->OmniHead;O;O=O->Next)
		Polys++;

	FList = new Face * [Polys];
	SList = new Face * [Polys];

	View = GreetSc->CameraHead;

	fprintf(LogFile,"GCAM: Flags=%lx SrcKeys=%ld TgtKeys=%ld FOV=%.1fdeg (from FLD zoom)\n",
		(long)View->Flags,(long)View->Source.NumKeys,(long)View->Target.NumKeys,View->FOV.Keys[0].Pos.X);
	fflush(LogFile);

	C_FZP = GreetSc->FZP;
	C_rFZP = 1.0f/C_FZP;
	C_NZP = GreetSc->NZP;
	C_rNZP = 1.0f/C_NZP;

	Ambient_Factor = 16;
	Diffusive_Factor = 1500;
	ImageSize = 50;

	// DETERMINISTIC CAPTURE PRE-PASS (headless): the Timer-gated loop below never
	// renders under cycles=max (Timer races past CHPartTime before a frame draws).
	// Drive CurFrame directly over a fixed set, render + snapshot each, then return.
	// Build with -DGREETS_CAPTURE to enable; default OFF = normal interactive greets loop.
#ifdef GREETS_CAPTURE
	{
		static const float CAPF[8] = {20.0f,30.0f,45.0f,60.0f,150.0f,300.0f,600.0f,1000.0f};
		long ci;
		for (ci=0; ci<8; ci++)
		{
			memset(VPage,0,PageSize);
			CurFrame = CAPF[ci];
			Animate_Objects(GreetSc);
			Transform_Objects(GreetSc);
			Lighting(GreetSc);
			if (!CAll) { fprintf(LogFile,"GCAP frame=%.0f CAll=0 (nothing)\n",CAPF[ci]);fflush(LogFile); continue; }
			Radix_SortingASM(FList,SList,CAll);
			Render();
			{ char fn[32]; sprintf(fn,"GCAP%04ld.PPM",(long)CAPF[ci]);
			  FILE *pf=fopen(fn,"wb");
			  if (pf){ unsigned char *px=(unsigned char *)VPage; long i,n=(long)XRes*(long)YRes;
			    fprintf(pf,"P6\n%d %d\n255\n",(int)XRes,(int)YRes);
			    for(i=0;i<n;i++){ fputc(px[i*4+2],pf); fputc(px[i*4+1],pf); fputc(px[i*4+0],pf); } fclose(pf); }
			  fprintf(LogFile,"GCAP %s CurFrame=%.1f CAll=%ld ISrc=(%.1f,%.1f,%.1f) ITgt=(%.1f,%.1f,%.1f)\n",fn,
			    (float)CurFrame,(long)CAll,View->ISource.X,View->ISource.Y,View->ISource.Z,
			    View->ITarget.X,View->ITarget.Y,View->ITarget.Z);
			  fflush(LogFile); }
			// per-mesh frustum/near stats + bad-face scan
			{ Object *DO;
			  for(DO=GreetSc->ObjectHead;DO;DO=DO->Next){
			    if(DO->Type!=Obj_TriMesh) continue;
			    TriMesh *DT=(TriMesh *)DO->Data; if(!DT) continue;
			    if(DT->Flags&Tri_Invisible) { fprintf(LogFile,"  MESH '%s' INVISIBLE\n",DO->Name?DO->Name:"?");fflush(LogFile); continue; }
			    Vertex *dv,*dve=DT->Verts+DT->VIndex; float zmin=1e30f,zmax=-1e30f; long nnear=0;
			    float pxmn=1e30f,pxmx=-1e30f,pymn=1e30f,pymx=-1e30f,txmx=0.0f; long nwx=0;
			    for(dv=DT->Verts;dv<dve;dv++){
			      if(dv->TPos.Z<zmin)zmin=dv->TPos.Z; if(dv->TPos.Z>zmax)zmax=dv->TPos.Z; if(dv->TPos.Z<1.0f)nnear++;
			      if(dv->PX<pxmn)pxmn=dv->PX; if(dv->PX>pxmx)pxmx=dv->PX;
			      if(dv->PY<pymn)pymn=dv->PY; if(dv->PY>pymx)pymx=dv->PY;
			      float ax=(float)fabs(dv->TPos.X); if(ax>txmx)txmx=ax;
			      if(dv->PX<-5000.0f||dv->PX>5000.0f||dv->PY<-5000.0f||dv->PY>5000.0f)nwx++;
			    }
			    fprintf(LogFile,"  MESH '%s' Fl=%lx VI=%ld Z[%.1f,%.1f] nNear=%ld PX[%.0f,%.0f] PY[%.0f,%.0f] TXmax=%.1f nWildXY=%ld BSR=%.2f\n",
			      DO->Name?DO->Name:"?",(unsigned long)DT->Flags,(long)DT->VIndex,zmin,zmax,nnear,pxmn,pxmx,pymn,pymx,txmx,nwx,DT->BSphereRad);
			    fflush(LogFile);
			  }
			  { long fi,nbad=0,nwild=0; for(fi=0;fi<CAll;fi++){ Face *ff=FList[fi];
			      if(!ff||!ff->A||!ff->B||!ff->C){nbad++;continue;}
			      if(ff->A->TPos.Z<-1e3f||ff->A->TPos.Z>1e5f||ff->B->TPos.Z>1e5f||ff->C->TPos.Z>1e5f)nwild++;
			      else if(ff->A->PX<-5000.0f||ff->A->PX>5000.0f||ff->B->PX<-5000.0f||ff->B->PX>5000.0f||ff->C->PX<-5000.0f||ff->C->PX>5000.0f)nwild++; }
			    fprintf(LogFile,"  FLSCAN CAll=%ld NULLface=%ld wildFaces=%ld\n",(long)CAll,nbad,nwild);
			    fflush(LogFile); }
			}
		}
		fprintf(LogFile,"GCAP:done — returning (capture build)\n");fflush(LogFile);
		delete FList; delete SList; Destroy_Scene(GreetSc);
		return;
	}
#endif

	// Self-paced playback: advance CurFrame by a fixed step per rendered frame instead of
	// gating on Timer. The Timer-gated form raced past the scene end under cycles=max (nothing
	// drew); this renders every frame at a steady pace regardless of emulator speed.
	{
		float frameStep = (GreetSc->EndFrame-GreetSc->StartFrame)/1200.0f;   // ~1200 frames over the scene
		if (frameStep<=0.0f) frameStep = 1.0f;
		CurFrame = GreetSc->StartFrame;
		// PERF PROFILING (RDTSC cycle counts per pass, accumulated over the run).
		double cAnim=0,cXform=0,cLight=0,cSort=0,cRend=0,cFlip=0; long pf=0;
		unsigned long startTimer=Timer, t0,t1;
		char pstr[96];
		while ((!Keyboard[ScESC])&&(!Keyboard[ScBackSpace])&&(CurFrame < GreetSc->EndFrame))   // BACKSPACE = skip, ESC = quit
		{
			memset(VPage,0,PageSize);

			if ((Keyboard[ScRight]||Keyboard[ScCloseSq])) CurFrame += frameStep*15.0f;                               // scrub forward
			if ((Keyboard[ScLeft]||Keyboard[ScOpenSq]))  { CurFrame -= frameStep*15.0f; if (CurFrame<GreetSc->StartFrame) CurFrame=GreetSc->StartFrame; }   // scrub back

			t0=RDTSC();
#ifdef FDS_CLIPCHK
			{ extern void ClipChk_Reset(void); extern long g_MeshCullLog;
			  ClipChk_Reset();
			  g_MeshCullLog = (pf==199||pf==249||pf==299||pf==349||pf==399); }
#endif
			Animate_Objects(GreetSc);          t1=RDTSC(); cAnim +=TscDelta(t0,t1); t0=t1;
			Transform_Objects(GreetSc);        t1=RDTSC(); cXform+=TscDelta(t0,t1); t0=t1;
#ifdef FDS_CLIPCHK
			{ extern long g_MeshCullLog;
			  if (g_MeshCullLog)
			  { Object *DO;
			    for(DO=GreetSc->ObjectHead;DO;DO=DO->Next)
			      if (DO->Type==Obj_TriMesh && DO->Data)
			        fprintf(LogFile,"  MESHNAME T=%p '%s'\n",DO->Data,DO->Name?DO->Name:"?");
			    fflush(LogFile); }
			  g_MeshCullLog = 0; }
#endif
			Lighting(GreetSc);                 t1=RDTSC(); cLight+=TscDelta(t0,t1); t0=t1;

			if (CAll)
			{
				Radix_SortingASM(FList,SList,CAll); t1=RDTSC(); cSort+=TscDelta(t0,t1); t0=t1;
				Render();                           t1=RDTSC(); cRend+=TscDelta(t0,t1); t0=t1;
			}
			pf++;
#ifdef FDS_CLIPCHK
			if (pf==1||pf==50||pf==100||pf==200||pf==300||pf==400||pf==500||pf==600||pf==800||pf==1000)
			{ extern void ClipChk_Dump(const char *); char _t[24]; sprintf(_t,"greets-f%ld",pf); ClipChk_Dump(_t);
			  // where is the mech / every mesh, in view space, this frame?
			  Object *DO;
			  for(DO=GreetSc->ObjectHead;DO;DO=DO->Next){
			    if(DO->Type!=Obj_TriMesh) continue;
			    TriMesh *DT=(TriMesh *)DO->Data; if(!DT||(DT->Flags&Tri_Invisible)) continue;
			    Vertex *dv,*dve=DT->Verts+DT->VIndex;
			    float zmin=1e30f,zmax=-1e30f,txmx=0.0f; long nNear=0,nFar=0;
			    for(dv=DT->Verts;dv<dve;dv++){
			      if(dv->TPos.Z<zmin)zmin=dv->TPos.Z; if(dv->TPos.Z>zmax)zmax=dv->TPos.Z;
			      if(dv->TPos.Z<C_NZP)nNear++; if(dv->TPos.Z>C_FZP)nFar++;
			      float ax=(float)fabs(dv->TPos.X); if(ax>txmx)txmx=ax; }
			    fprintf(LogFile,"  CCMESH f%ld '%s' VI=%ld Z[%.2f,%.2f] behindNear=%ld beyondFar=%ld maxTX=%.1f\n",
			      pf,DO->Name?DO->Name:"?",(long)DT->VIndex,zmin,zmax,nNear,nFar,txmx); }
			  fflush(LogFile); }
			if (pf==100||pf==300||pf==400||pf==500||pf==600||pf==800)
			{
				char fn[32]; sprintf(fn,"GRT%ld.PPM",pf);
				FILE *pfx=fopen(fn,"wb");
				if (pfx)
				{
					unsigned char *px=(unsigned char *)VPage;
					long i,n=(long)XRes*(long)YRes;
					fprintf(pfx,"P6\n%d %d\n255\n",(int)XRes,(int)YRes);
					for(i=0;i<n;i++){ fputc(px[i*4+2],pfx); fputc(px[i*4+1],pfx); fputc(px[i*4+0],pfx); }
					fclose(pfx);
				}
			}
#endif
#ifdef FDS_SNAP
			// Ungated these cost ~460KB of DOS disk I/O mid-scene. Diagnostic only.
			if (pf==200||pf==201)
			{
				char fn[32]; sprintf(fn,"GRT%ld.PPM",pf); // 8.3! GREETS200 truncated to GREETS20
				FILE *pfm=fopen(fn,"wb");
				if (pfm)
				{
					unsigned char *px=(unsigned char *)VPage;
					long i,n=(long)XRes*(long)YRes;
					fprintf(pfm,"P6\n%d %d\n255\n",(int)XRes,(int)YRes);
					for(i=0;i<n;i++){ fputc(px[i*4+2],pfm); fputc(px[i*4+1],pfm); fputc(px[i*4+0],pfm); }
					fclose(pfm);
				}
			}
#endif

			// on-screen per-pass % + FPS (readable live on 86Box)
			{ double tot=cAnim+cXform+cLight+cSort+cRend; if(tot<1) tot=1;
			  double secs=(double)(Timer-startTimer)/(double)Timer_Freq; double fps=(secs>0)?pf/secs:0;
			  sprintf(pstr,"FPS %4.1f  xf%2.0f lt%2.0f so%2.0f rn%2.0f  n%ld",fps,
			    cXform*100/tot,cLight*100/tot,cSort*100/tot,cRend*100/tot,(long)CAll);
			  OutTextXY(VPage,0,0,pstr,64); }

			Demo_Controls("greets",(float)CurFrame,GreetSc);
			Flip(Screen);
			t1=RDTSC(); cFlip+=TscDelta(t0,t1);

			CurFrame += frameStep;
		}
		// end-of-scene perf log
		{ double tot=cAnim+cXform+cLight+cSort+cRend+cFlip; if(tot<1) tot=1;
		  double secs=(double)(Timer-startTimer)/(double)Timer_Freq; double fps=(secs>0)?pf/secs:0;
		  FILE *pfil=fopen("PERF.TXT","at"); if(pfil){
		  fprintf(pfil,"PERF greets: frames=%ld fps=%.1f wall=%.1fs | anim=%.1f%% xform=%.1f%% light=%.1f%% sort=%.1f%% render=%.1f%% flip=%.1f%% | Mcy/frame=%.2f\n",
		    pf,fps,secs, cAnim*100/tot,cXform*100/tot,cLight*100/tot,cSort*100/tot,cRend*100/tot,cFlip*100/tot,(pf>0?(tot/pf/1e6):0));
		  fclose(pfil); } }
	}
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
	Destroy_Scene(GreetSc);
}