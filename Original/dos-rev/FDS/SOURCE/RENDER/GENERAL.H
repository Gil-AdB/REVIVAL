// General FDS stuff

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
  long I;
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

void SetGraphicsMode(void)
{
  if (VMode.Driver_ID == VGA_ID)
    SetVGAMode(VMode.Mode_ID);
  if (VMode.Driver_ID == VESA_ID || VMode.Driver_ID == VBE2_ID)
    SetVESAMode(VMode.Mode_ID);
  if (Palette_Reserved)
    Set_Palette(VESA_Pal);
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
    if (Vtx->Pos.X<mu) mu=Vtx->Pos.X;
    if (Vtx->Pos.X>Mu) Mu=Vtx->Pos.X;
    if (Vtx->Pos.Y<mv) mv=Vtx->Pos.Y;
    if (Vtx->Pos.Y>Mv) Mv=Vtx->Pos.Y;
  }
  su = 255.0/(Mu-mu);
  sv = 255.0/(Mv-mv);
  Vtx=T->Verts;
  for(;Vtx<VEnd;Vtx++)
  {
    Vtx->U = (Vtx->Pos.X-mu)*su;
    Vtx->V = (Vtx->Pos.Y-mv)*sv;
  }
}

void Init_Randomseed()
{
  time_t time_of_day; struct tm tmbuf; time_of_day = time(NULL);
  _localtime(&time_of_day, &tmbuf); srand( tmbuf.tm_sec*tmbuf.tm_min );
}

char FDS_Init(unsigned short x,unsigned short y,unsigned char BPP)
{
  long I;
  char *DP;
  char Res;
  DWord RR;

  setbuf(stdout,NULL);
  FDS_Title();
  Detect_MMX();

  if (MMXState) printf("] Using MMX(TM) for Rasterization.\n");
  else printf("] MMX(TM) not available.\n");

  // Initialize Internal Variables.
  printf("] Hacking Random-seed.\n");
  Init_Randomseed();
  printf("] Mathematical Identities.\n");
  Init_Identities();
  printf("] Diffusing Materials.\n");
//  Init_Default_Material();

  MsgStr = new char * [10];
  memset(MsgStr,0,40);
  MsgClock = new long [10];

  printf("] FPU >> Low Precision.\n");
  FPU_LPrecision();

  printf("] Basic font.\n");
  Font1=LoadAFT("Fonts\\Standard.AFT");
  Active_Font = Font1;
  printf("] Timer ISR.\n");
  Install_8();
  printf("] Keyboard ISR.\n");
  Install_9();
  Timer_Freq = 100;
  Set_Rate(Rate_Div/Timer_Freq); //100Hz

  if (!Find_RetraceRate())
  {
    delay(200);
    printf("Warning: FDS may not run well under ");
    switch(rand()*3>>15)
    {
      case 0:printf("M$ WINSLOW 95.\n"); break;
      case 1:printf("M$ WINDOZE 95.\n"); break;
      case 2:printf("Micro$UX WINZOL 95.\n");
    }
    printf("Press <ESC> to Terminate FDS operation,or any other key to continue.\n");
    fflush(stdout);
    while (!Keypressed());
    if (Keyboard[ScESC])
    {
      Restore_8();
      Restore_9();
      Set_Rate(Basic_Rate);
      exit(-1);
    }
  }
  printf("] Initializing Video system.\n");

  Initialize_VESA(); //yeah

  Set_Screen(x,y,BPP,0,0);
  Res=Modesetup();
  VESA_Surface2Global(Screen);
  //  Res=ModeSetup(x,y,32);
//  if (!Res) Res=ModeSetup(x,y,24);

  Set_Rate(Basic_Rate);

  if (Res&&BPP==8)
  {
    for(I=0;I<64;I++)
    {
      VESA_Pal[I*3]=I>>2;
      VESA_Pal[I*3+1]=I>>1;
      VESA_Pal[I*3+2]=I;
    }
    VESA_Pal[192]=63; VESA_Pal[193]=63; VESA_Pal[194]=63;
    Set_Palette(VESA_Pal);
  }
  Init_FreeCamera();

  SetVGAMode(3);
#ifdef Play_Music
  printf("] Timer >> MIDAS Callback.\n");
  Restore_8();
  Restore_9();
  printf("] MIDAS Sound system - Installing.\n");
  Install_MIDAS();
  Install_9();
#endif
  //Set 100Hz
  Timer_Freq = 100;
  Set_Rate(Rate_Div/Timer_Freq);
//  printf("XRes : %d  YRes : %d  VESA_BPSL : %d\n",XRes,YRes,VESA_BPSL);
  return Res;
}


void MsgToDisplay(DWord ID,DWord Issue,char *Str)
{
  long CurMsg=0;
  long IDDW = (ID<<16)+Issue;
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
  Deinitialize_VESA();
  Set_Rate(Basic_Rate);
  FPU_NormalState();
  Restore_9();
  Restore_8();
  fclose(LogFile);
// DeallocateAll();
}

void Display_Hierarchy_Node(Scene *Sc,Object *Last,char Level)
{
  Object *O;
  char *s = new char[Level*2+1];
  long I;
  for(I=0;I<Level*2;I++) s[I]=32;
  s[Level*2]=0;
  for(O=Sc->ObjectHead;O;O=O->Next)
    if (O->Parent==Last)
    {
      if (Last) printf("%s|%s [%s].\n",s,O->Name,Last->Name); else printf("|%s\n",O->Name);
      Display_Hierarchy_Node(Sc,O,Level+1);
    }
  delete s;
}

void Display_Hierarchy_Links(Scene *Sc)
{
  printf("<DH>: Displaying Hierarchy Linkage Tree.\n");
  Display_Hierarchy_Node(Sc,NULL,0);
}


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
  long        Count1, Count2, PrevCount = 0, Count = 0;
  long        RefreshRate;

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


int MMXState;

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
