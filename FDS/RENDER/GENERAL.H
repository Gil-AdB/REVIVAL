// Coriolis heritage

#include <Base/Object.h>
#include <Base/Scene.h>

// General FDS stuff
int MMXState;

extern "C"
{
	extern dword	pfMaxLight;
	extern dword	pf255;
	extern dword	pfr256;
	extern dword	pf65536;
	extern dword	pf32768;
	extern dword	pf256;
	extern dword	pf128;
	extern dword	pf16;
	extern dword	pf1;
	extern dword	pf0;
}



void FDS_Title(void)
{
  printf("] Vertigo GL (V%s) - INITIALIZE\n",FDS_Version);
}

void Display_Linked_List(Scene *Sc)
{
  fprintf(LogFile,"<DLL>: Display Linked Lists.\n");
  Object *Obj;

  for(Obj = Sc->ObjectHead;Obj;Obj=Obj->Next)
    fprintf(LogFile,"Object : %s\n",Obj->Name);
}


// A script for Loading a chain of 3DS files from a given string array.
void LoadScenes(char Scenes,char *Name[])
{
  int32_t I;
  Scene *Sc = new Scene[Scenes];
  CurScene = Sc;
  memset(Sc,0,sizeof(Scene)*Scenes);

  for (I=1;I<Scenes;I++)
  {
    if (!Load_3DS(CurScene,Name[I])) fprintf(LogFile,"Warning! Unable to load 3ds scene file %s!\n",Name[I]);
//    Display_Hierarchy_Links(CurScene); // for debugging
//    Display_Linked_List(CurScene);
  }
}

#ifdef _C_WATCOM
void SetGraphicsMode(void)
{
  if (VMode.Driver_ID == VGA_ID)
    SetVGAMode(VMode.Mode_ID);
  if (VMode.Driver_ID == VESA_ID || VMode.Driver_ID == VBE2_ID)
    SetVESAMode(VMode.Mode_ID);
  if (Palette_Reserved)
    Set_Palette(VESA_Pal);
}
#endif

void Planar_Mapping(TriMesh *T)
{
  float mu,mv,Mu,Mv,su,sv;
  Vertex *Vtx = T->Verts, *VEnd = Vtx+T->VIndex;
  mu = Mu = Vtx->U;
  mv = Mv = Vtx->V;
  Vtx++;
  for(;Vtx<VEnd;Vtx++)
  {
    if (Vtx->Pos.x<mu) mu=Vtx->Pos.x;
    if (Vtx->Pos.x>Mu) Mu=Vtx->Pos.x;
    if (Vtx->Pos.y<mv) mv=Vtx->Pos.y;
    if (Vtx->Pos.y>Mv) Mv=Vtx->Pos.y;
  }
  su = 255.0/(Mu-mu);
  sv = 255.0/(Mv-mv);
  Vtx=T->Verts;
  for(;Vtx<VEnd;Vtx++)
	{
		Vtx->U = (Vtx->Pos.x-mu)*su;
		Vtx->V = (Vtx->Pos.y-mv)*sv;
	}
}

void Init_Randomseed()
{
	time_t time_of_day; struct tm tmbuf; time_of_day = time(NULL);
#ifdef _C_WATCOM
	_localtime(&time_of_day, &tmbuf);
	srand( tmbuf.tm_sec*tmbuf.tm_min );
#endif
#ifdef WIN32
	struct tm *newtime;
	newtime = localtime(&time_of_day); 
	srand( newtime->tm_sec*newtime->tm_min );
#endif
	
}

char FDS_Init(unsigned short x,unsigned short y,unsigned char bpp)
{
	int32_t I;
	char *DP;
	char Res;
	DWord RR;
	int32_t XR,YR;

	setbuf(stdout,NULL);
	FDS_Title();

	// Let me guess... you have an MMX enabled processor. Also SSE4.1. If not, tough luck...
	//Detect_MMX();
	//if (MMXState) printf("] Using MMX(TM) for Rasterization.\n");
	//else printf("] MMX(TM) not available.\n");

  // Initialize Internal Variables.
	printf("] Hacking Random-seed.\n");
	Init_Randomseed();

	printf("] Globals initialization.\n");
	Init_Identities();
	printf("] Diffusing Materials.\n");
//  Init_Default_Material();

	// init text message system.
	MsgStr = new char * [10];	
	memset(MsgStr,0,40);
	MsgClock = new int32_t [10];
	MsgID = new dword [10];

	// set FPU precision to single (23bit)
	printf("] FPU >> Low Precision.\n");
	FPU_LPrecision();

	// read std. font from AFT file.
	printf("] Basic font.\n");
	Font1=LoadAFT("Fonts/Standard.AFT");
	Active_Font = Font1;

	printf("] Initializing Video system.\n");
	// VM setup (DOS only)
	// initialize logical part of video system based on specified (x,y)xbpp.
	VESA_InitExternal(x, y, bpp);
	Res = 1;

	Init_FreeCamera();

	//InitFrustrumClipper();

	return Res;
}

void MsgToDisplay(DWord ID,DWord Issue,char *Str)
{
  int32_t CurMsg=0;
  int32_t IDDW = (ID<<16)+Issue;
  while (MsgStr[CurMsg]&&MsgID[CurMsg]!=IDDW) CurMsg++;
  if (MsgStr[CurMsg]) delete MsgStr[CurMsg]; else MsgID[CurMsg]=IDDW;
  MsgStr[CurMsg] = strdup(Str);
  MsgClock[CurMsg] = Timer;
}

// function disabled...unknown reason
void Get_Palette2Prime(Scene *Sc)
{
  Material *M;
  if (BPP!=8) return;

//  for(M=MatLib;M&&(Group!=M->Group)&&(!M->Txtr);M=M->Next);
//  if (M) memcpy(Pal,M->Txtr->Pal,sizeof(Palette));
}

void FDS_End(void)
{
#ifdef _C_WATCOM
  Deinitialize_VESA();
  Set_Rate(Basic_Rate);
  Restore_9();
  Restore_8();
  fclose(LogFile);
#endif
  FPU_NormalState();
// DeallocateAll();
}

void Display_Hierarchy_Node(Scene *Sc,Object *Last,char Level)
{
  Object *O;
  char *s = new char[Level*2+1];
  int32_t I;
  for(I=0;I<Level*2;I++) s[I]=32;
  s[Level*2]=0;
  for(O=Sc->ObjectHead;O;O=O->Next)
    if (O->Parent==Last)
    {
      if (Last) printf("%s|%s [%s].\n",s,O->Name,Last->Name); else printf("|%s\n",O->Name);
      Display_Hierarchy_Node(Sc,O,Level+1);
    }
  delete [] s;
}

void Display_Hierarchy_Links(Scene *Sc)
{
  printf("<DH>: Displaying Hierarchy Linkage Tree.\n");
  Display_Hierarchy_Node(Sc,NULL,0);
}


#ifdef _C_WATCOM
void ClearInterrupts(void);
#pragma aux ClearInterrupts=\
  "CLI";

void SetInterrupts(void);
#pragma aux SetInterrupts=\
  "STI";

// Waits for a Vertical Retrace.
void WaitRetrace(void);
#pragma aux WaitRetrace=\
  "Mov DX,3DAh",\
  "@W1: In AL,DX",\
  "And AL,8",\
  "JNE @W1",\
  "@W2: In AL,DX",\
  "And AL,8",\
  "JE @W2",\
  modify exact [EAX EDX];


// returns 0 if Lose95 is present, or the Vertical Retrace rate in
// mHz (miliHertz) if it runs under DOS.
DWord Find_RetraceRate()
{
  int         FailCount = 0, Success = 0;
  int32_t        Count1, Count2, PrevCount = 0, Count = 0;
  int32_t        RefreshRate;

  // Try to be safe. Nasty for possible music playback, but the only way
  // to get reliable results:
  ClearInterrupts();

  while ( (FailCount < 4) && (Success != 1) )
  {
    WaitRetrace();
    outp(0x43, 0x36);
    outp(0x40, 0);
    outp(0x40, 0);
    WaitRetrace();
    outp(0x43, 0);
    Count1 = inp(0x40);
    Count1 |= (inp(0x40)) << 8;
    Count1 = 0x10000-Count1;

    WaitRetrace();
    outp(0x43, 0x36);
    outp(0x40, 0);
    outp(0x40, 0);
    WaitRetrace();
    outp(0x43, 0);
    Count2 = inp(0x40);
    Count2 |= (inp(0x40)) << 8;
    Count2 = 0x10000-Count2;

    if (abs(Count2 - Count1) > 2) FailCount++;
    else
    {
      Count = Count1 >> 1;
      if (abs(PrevCount - Count) <= 2)
        Success = 1;
      else
      {
        PrevCount = Count;
        FailCount++;
      }
    }
  }

  if ( Success )
  {
    RefreshRate = 1193180000 / Count;

    // rates higher than 250Hz or lower than 30Hz are considered impossible.
    if ( (RefreshRate > 250000) || (RefreshRate < 35000) )
      RefreshRate = 0;
  }
  else RefreshRate = 0;

  SetInterrupts();

  return RefreshRate;
}

extern "C" void __interrupt __far new_int6(void);
extern "C" void emmshit(void);

void (__interrupt __far *old_int6)(void);

void Detect_MMX()
{
   old_int6 =_dos_getvect(0x6);
   _dos_setvect(0x6,new_int6);
  MMXState=1;
  emmshit();
   _dos_setvect(0x6,old_int6);
}
#endif

#ifdef WIN32
void ClearInterrupts()
{
	// disallow CLIs
}
void SetInterrupts()
{
	// disallow STIs
}

void WaitRetrace()
{
	// Wait for vertical retrace. (currently unavailable)
}

DWord Find_RetraceRate()
{
	// Currently unavailable
	return 0;
}

//void Detect_MMX()
//{
//	// Assume MMX is avilable
//	MMXState = 1;
//	// use cpuid.
//	__asm {
//		mov eax, 1
//		cpuid
//		shr edx, 23
//		and edx, 1
//		mov [MMXState], edx
//	}
//}


#endif