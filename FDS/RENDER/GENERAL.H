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
	srand(static_cast<unsigned>(time(NULL)));
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
	Font1=LoadAFT("FONTS/STANDARD.AFT");
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
  FPU_NormalState();
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

