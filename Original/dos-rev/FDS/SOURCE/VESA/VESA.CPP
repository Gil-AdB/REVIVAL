// Silvatar's Modular VESA/VBE interface unit,Version 2.01.
// Next: Make sure System can be reinitialized without loss of memory
//       also make sure all windows + screenclones auto-adjust.
//       also try to logically support windows, and insert mouse interface

#include <StdIO.H>
#include <StdLib.H>
#include <String.H>
#include <ConIO.H>
#include <DOS.H>
#include <Math.H>

#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"

#include "VESA.H"

#include "Fonts.CPP"
#include "Tables.CPP"


void No_operand();
#pragma aux No_operand=\
  "NOP";

void *VESAModule,*VESABuffer;
short *VESA_ModesAvail;
long VESA_NumModes;

// General info
char Graphmode = 0;  //set if client is on active g.mode state.
                 //otherwise it won't be able to determine what
                 //to do with the surfaces...
char Granola;         //GRANOLA=GRANULARITY ;), that is 64Kb/Vesamode->WinGranularity
long VESA_BPSL;       //Crazy bytes per scan line crap. not currently supported.
long BPP_Shift;
char Screenmem_Model; //1=LFB,2=Banks,3=Fake 14bit (not supported)
VESA_Surface *Screen; //this is where the LFB surface resides
VESA_Surface *MainSurf;   //this is regular surface u flip fastest,aka Page2 aka VPage
VESA_Surface *Last_Surface;
VideoMode VMode;
long VESA_Ver = 0;

char Direct_FLIP = 0;

// Global Surface Scalar variables (Fastest access)
long XRes,YRes,XRes_1,YRes_1,CntrX,CntrY,BPP,CPP;
float CntrEX,CntrEY;
long PageSize,PageSizeW,PageSizeDW;
short CurrentGraphicsMode;
char Palette_Reserved = 0;

char *VGAPtr,*VPage; //Direct access pointers to LFB/Banks and v.Page[2].
float *ZBuffer;
Font *Active_Font;    //Currently Active font
char *VESA_Pal = NULL;

char *Fake8_R,*Fake8_G,*Fake8_B;

long VMdMenuItems[8],VMdMenuTotal;

void (*Flip)(VESA_Surface *VS);


Video_entry *VE_Root;

// Screen and Main are handled by VESA unit and shouldn't be interfered.
// Any other surfaces are on client responsability to get dealloced.

void Initialize_VESA()
{
  if (!DPMI_Main)
  {
    DPMI_Init_Core();
    DPMI_Init_MB(4096);
  }
  VESAModule = DPMI_New(DPMI_Main,0,"VESA unit Module");
  VESABuffer = DPMI_New(VESAModule,1024,"VESA Information buffer -low");
  SetVGAMode(0x03); // Standard Text Mode.
  VMode.XRes = 80;
  VMode.YRes = 25;
  VMode.BPP = 16;
  VMode.BPSL = 80*2;
  VMode.VideoMem = (char *)0xB8000; //Correct me if I'm wrong!
  VMode.Flags = VMode_TextMode;
  VMode.Driver_ID = VGA_ID;
  VMode.Mode_ID = 0x0003; //Yeah
  Screen = new VESA_Surface;
  memset(Screen,0,sizeof(VESA_Surface));
  Screen->Flags = VSurf_Nonexist | VSurf_EffScreen;

  VE_Root = NULL;
}

void VESA_Message(char *Msg)
{
//  printf("<VESA>: %s\n",Msg);
}

void VESA_Warning(char *Msg)
{
//  printf("<VESA> Warning: %s\n",Msg);
}

// ha! it worked! and I don't need that pathetic lfbprof crap!
int VBE_Detect()
{
  RMREGS R;
  RMSREGS S;
  short *p1,*p2;
  VBE_vgaInfo *vgaInfo = (VBE_vgaInfo *)VESABuffer;
  long I;
  char Str[160];

  // I hope this shit works.
  strncpy(vgaInfo->VESASignature,"VBE2",4);
  R.x.ax = 0x4F00;
  S.es = DPMI_Get_Handle(VESABuffer)&0xFFFF; // Segment
  R.x.di = 0;
  DPMI_int86x(0x10,&R,&R,&S);
  if (R.x.ax != 0x004F||strncmp(vgaInfo->VESASignature,"VESA",4)) return 0;
  DPMI_Delete(VESA_ModesAvail);
  p1 = (short *)(((unsigned)(vgaInfo->VideoModePtr)>>12)+(vgaInfo->VideoModePtr&0xFFFF));
  p2 = p1;
  while (*p1 != -1) p1++;
  I = p1-p2; //I=Amount of modes.
  //Re-Allocate VESA Modelist.
  VESA_NumModes = I;
  VESA_ModesAvail = (short *)DPMI_New(VESAModule,I<<1,"VESA Available mode List.");
  p1 = p2;
  p2 = VESA_ModesAvail;
  while (*p1 != -1)
    *p2++ = *p1++;
  *p2 = -1;

  sprintf(Str,"VESA Bios Extensions detected (Version %d.%d)",vgaInfo->VESAVersion>>8,vgaInfo->VESAVersion&0xFF);
  VESA_Message(Str);
  VESA_Ver = vgaInfo->VESAVersion;
  return vgaInfo->VESAVersion;
}

char VESA_GetModeInfo(int mode)
{
  RMREGS  R;
  RMSREGS S;
  VBE_modeInfo *VM = (VBE_modeInfo *)VESABuffer;

  R.x.ax = 0x4F01;
  R.x.cx = mode;
  S.es = DPMI_Get_Handle(VESABuffer)&0xFFFF;
  R.x.di = 0;
  DPMI_int86x(0x10,&R,&R,&S);
  if (R.x.ax != 0x004F)
    return 0;
  if ((VM->ModeAttributes & vbeMdAvailable) == 0)
    return 0;
  return 1;
}

void SetVESAMode(int Mode)
{
  RMREGS R;
  R.x.ax = 0x4F02;
  R.x.bx = Mode;
  DPMI_int86(0x10,&R,&R);
}

// awawawiwa. kewl.
void VESA_FullInfo(long RX,long RY,char RBPP)
{
  int Ver = VBE_Detect();
  char Res;
  VBE_vgaInfo *VI = (VBE_vgaInfo *)VESABuffer;
  VBE_modeInfo *VM = (VBE_modeInfo *)VESABuffer;
  long I,J;
  float Aspect;
  char Str[33],Str2[33];
  void *P2L;

  if (!Ver)
  {
    printf("Fatal Error - Video Adapter is not VESA Complient.\n");
    return;
  }
  printf("VESA Unit FULL INFORMATION Requested for Criteria (%d,%d),%d.\n",RX,RY,RBPP);
  printf("VBE Version %d.%d Available.\n",Ver>>8,Ver&0xFF);
  printf("OEM Information: \n  -- %s\n  - Version %d.%d\n  - Vender: %s\n  - Product: %s\n  - Revision: %s\n",(char *)RP2LP(VI->OemStringPtr),VI->OemSoftwareRev>>8,VI->OemSoftwareRev&0xFF,(char *)RP2LP(VI->OemVendorNamePtr),(char *)RP2LP(VI->OemProductNamePtr),(char *)RP2LP(VI->OemProductRevPtr));
  printf("Video Memory Available: %dKb.\n",VI->TotalMemory<<6);
  printf("Requested Video Mode information:\n");
  //
  if (RX&&RY&&RBPP)
  {
    if (RX!=320&&RX!=300&&RX!=512&&RX!=640&&RX!=800&&RX!=1024&&RX!=1280&&RX!=1600) printf("Alert! Abnormal Horizontal resolution value(X=%d)\n",RX);
    if (RY!=200&&RY!=240&&RY!=384&&RY!=400&&RY!=480&&RY!=600&&RY!=768&&RY!=1024&&RY!=1200) printf("Alert! Abnormal Vertical resolution value(Y=%d)\n",RY);
    if (RBPP!=8&&RBPP!=15&&RBPP!=16&&RBPP!=24&&RBPP!=32) printf("Alert! Abnormal Bits per pixel Value(BPP=%d).\n",RBPP);
    if (Ver<0x100&&RBPP>8) printf("Alert! DAC mode requested on old VBE interface.\n");
    if (VI->TotalMemory<<16<RX*RY*(RBPP+1>>3)) printf("Alert! Insufficient Video memory [needs more %dKb] for Requested mode.\n",(RX*RY*(RBPP+1>>3)-(VI->TotalMemory<<16))>>8);

    J = 0;
    for(I=0;I<VESA_NumModes;I++)
    {
      Res = VESA_GetModeInfo(VESA_ModesAvail[I]);
      if (VM->XResolution!=RX||VM->YResolution!=RY||VM->BitsPerPixel!=RBPP) continue;
      printf("Requested Mode Found: (ID %xH): ",VESA_ModesAvail[I]);
      if (!Res) {printf(" Not Available! (Unknown reason)"); continue;}
      J = VESA_ModesAvail[I];
      if (VM->ModeAttributes&vbeMdColorMode) printf("C"); else printf(" ");
      if (VM->ModeAttributes&vbeMdGraphMode) printf("G"); else printf(" ");
      if (!(VM->ModeAttributes&vbeMdNonBanked)) printf("B"); else printf(" ");
      if (VM->ModeAttributes&vbeMdLinear) printf("L"); else printf(" ");
      printf("\n");
    }
    if (!J) {printf("Alert! no Graphic modes fit into given Resolution Criteria.\n"); return;}
    printf("Detailed information on last found mode (%xH):\n",J);
    VESA_GetModeInfo(J);
    printf("Window A attrib: %xH\n",VM->WinAAttributes);
    printf("Window B attrib: %xH\n",VM->WinBAttributes);
    printf("Bank/Seg Granularity: %dKb\n",VM->WinGranularity);
    printf("LFB/Active banks total mem: %dKb\n",VM->WinSize);
    printf("Bytes per scanline: %d. Expected: %d.\n",VM->BytesPerScanLine,RX*(RBPP+1>>3));
    printf("Character cell size (%d,%d)\n",VM->XCharSize,VM->YCharSize);
    printf("Total memory planes: %d.\n",VM->NumberOfPlanes);
    switch (VM->MemoryModel)
    {
      case 4:  printf("Memory model type: Packed. (Palettized)\n"); break;
      case 6:  printf("Memory model type: Hicolor (RGB).\n"); break;
      default: printf("Memory model type: Unknown!\n"); break;
    }
    printf("Single Bank size: %dKb.\n",VM->BankSize);
    if (VM->MemoryModel==6)
    {
      memset(Str,0,33);
      memset(Str+VM->RedFieldPosition,'R',VM->RedMaskSize);
      memset(Str+VM->GreenFieldPosition,'G',VM->GreenMaskSize);
      memset(Str+VM->BlueFieldPosition,'B',VM->BlueMaskSize);
      memset(Str+VM->RsvdFieldPosition,'A',VM->RsvdMaskSize);
      for(I=0;I<RBPP;I++)
        Str2[I]=Str[RBPP-I-1];
      Str2[RBPP]=0;

      printf("DAC formation: %s\n",Str2);
      printf("DAC Attributes: %xH\n",VM->DirectColorModeInfo);
    }
    if (Ver>=0x200)
    {
      printf("\n* VBE2 additional Information\n");
      if (VM->ModeAttributes&vbeMdLinear) {
        // Converts Physical Address to Linear address,Displays and then Dumps it.
        P2L = DPMI_Map_P2L(VM->PhysBasePtr,LFB_LIMIT);
        printf("Linear frame buffer Located at: Physical %xH,Linear %xH.\n",VM->PhysBasePtr,(DWord)P2L);
        DPMI_Del_P2L(P2L);
      }
      printf("Offscreen memory: Offset %xH, Size %dKb.\n",VM->OffScreenMemOffset,VM->OffScreenMemSize);
    }

  } else {
    printf("   Mode    | Attrib | Resolution  |\n");
    printf("-----------------------------------\n");
    J = 0;
    for(I=0;I<VESA_NumModes;I++)
    {
      Res = VESA_GetModeInfo(VESA_ModesAvail[I]);
      if ((RX&&(VM->XResolution!=RX))||(RY&&(VM->YResolution!=RY))||(RBPP&&(VM->BitsPerPixel!=RBPP))) continue;
      printf("Mode %xH: |  ",VESA_ModesAvail[I]);
      if (!Res) {printf(" ### Not Available!\n"); continue;}
      if (VM->ModeAttributes&vbeMdColorMode) printf("C"); else printf(" ");
      if (VM->ModeAttributes&vbeMdGraphMode) {printf("G"); J=1;} else printf(" ");
      if (!(VM->ModeAttributes&vbeMdNonBanked)) printf("B"); else printf(" ");
      if (VM->ModeAttributes&vbeMdLinear) printf("L"); else printf(" ");
      printf("  | ");
      printf("%dx%d,%dBPP",VM->XResolution,VM->YResolution,VM->BitsPerPixel);
      printf("\n");
    }
    if (!J) printf("Alert! no Graphic modes fit into given Resolution Criteria.\n");
  }
}


// Conveys DATA from given Surface to global variables. Performed on init
// for MainSurf.
void VESA_Surface2Global(VESA_Surface *VS)
{
  XRes = VS->X; YRes = VS->Y;
  XRes_1 = XRes-1; YRes_1 = YRes-1;
  CntrX = XRes>>1; CntrY = YRes>>1;
  CntrEX = CntrX-0.5; CntrEY = CntrY-0.5;
  BPP = VS->BPP;
  CPP = VS->CPP;
  switch (CPP)
  {
    case 1: BPP_Shift = 0; break;
    case 2: BPP_Shift = 1; break;
    case 3:
    case 4: BPP_Shift = 2; break;
    default: printf("Standing by. byebyebye.\n");
  }
  VESA_BPSL = VS->BPSL;
  PageSize = VS->PageSize;
  PageSizeW = PageSize>>1;
  PageSizeDW = PageSizeW>>1;
  VPage = VS->Data;
  ZBuffer = VS->ZBuffer;
  Flip = VS->Flip;
  YOffs = VS->YTable;
}

void Test_Direct_Flipping()
{
  Direct_FLIP = 0;
  if (Screen->Flags&VSurf_Nonexist) return;
  if (VMode.Flags&VMode_Segmentated) return;
  if (VMode.Flags&VMode_TextMode) return;
  if (VMode.XRes==Screen->X&&VMode.YRes==Screen->Y&&VMode.BPP==Screen->BPP)
    Direct_FLIP = 1;
}

// Finds the appropriate Flipping routine for a general Surface.
// (According to current SCREEN surface)
void VESA_Find_Flip(VESA_Surface *VS)
{
  // OK. first of all, we check if BPP match.
  if (VS->BPP==Screen->BPP)
  {
    // everything ok so far...now we check memory model
    // Linear flipping is best. now we check whether it's
    // Even size or shit bitblt.
    if (VS->BPSL==Screen->BPSL)
    {
      if (VS->Y==Screen->Y)
      {
        VS->Flip = &Flip_Linear;
        return;
      } else {
        VS->Flip = &Flip_YMLinear;
        return;
      }
    }
      // we're in deep trouble... (just blit it. nike.)
    VS->Flip = &Flip_BitBlt;
  } else {
    // now we're supposed to check alot of nasty cases
    VS->Flip = &Flip_Impossible;
  }
}

void VESA_MainFlip()
{
  VESA_Surface *VS = Screen;
  char *Palptr = (char *)VESA_Pal;
  long I,J,K;
  VS->Flip = &Flip_Impossible;
  Palette_Reserved = 0;
  // OK. first of all, we check if BPP match.
  if (!(VMode.Flags&VMode_Nonlinear))
  {
    if (VS->BPP==VMode.BPP)
    {
      // everything ok so far...now we check memory model
      // Linear flipping is best. now we check whether it's
      // Even size or shit bitblt.
      if (VS->BPSL==VMode.BPSL)
      {
        if (VS->Y==VMode.YRes)
        {
          VS->Flip = &Flip_Linear;
          return;
        } else {
          VS->Flip = &Flip_YMLinear;
          return;
        }
      }
      // we're in deep trouble... (just blit it. nike.)
      //VS->Flip = &Flip_BitBlt;
      if (VS->X<<1==VMode.XRes&&VS->Y<<1==VMode.YRes)
      {
        switch (VS->BPP)
        {
          case 8:VS->Flip = &Flip_DPixel_8; break;
          case 15:
          case 16: VS->Flip = &Flip_DPixel_16; break;
          case 32: VS->Flip = &Flip_DPixel_32; break;
        }
      }
    } else {
      if (VS->X==VMode.XRes&&VS->Y==VMode.YRes)
      {
        switch (VS->BPP)
        {
          case 8:
            switch (VMode.BPP)
            {
              case 15: VS->Flip = &Flip_Emu8_15; break;
              case 16: VS->Flip = &Flip_Emu8_16; break;
              case 24: VS->Flip = &Flip_Emu8_24; break;
              case 32: VS->Flip = &Flip_Emu8_32;
            }
          break;
          case 16:
            switch (VMode.BPP)
            {
              case 8:
                VS->Flip = &Flip_Fake16_8;
                //Generate Palette
                for(I=0;I<8;I++)
                  for(J=0;J<8;J++)
                    for(K=0;K<4;K++)
                    {
                      *Palptr++ = (I<<3)+4;
                      *Palptr++ = (J<<3)+4;
                      *Palptr++ = (K<<4)+8;
                    }
                Palette_Reserved = 1;
              break;
              case 15:VS->Flip = &Flip_Conv16_15; break;
              case 24:VS->Flip = &Flip_Conv16_24; break;
              case 32:VS->Flip = &Flip_Conv16_32;
            }
          break;
          case 32:
            switch (VMode.BPP)
            {
              case 8:
                VS->Flip = &Flip_Fake32_8;
                for(I=0;I<8;I++)
                  for(J=0;J<8;J++)
                    for(K=0;K<4;K++)
                    {
                      *Palptr++ = (I<<3)+4;
                      *Palptr++ = (J<<3)+4;
                      *Palptr++ = (K<<4)+8;
                    }
                Palette_Reserved = 1;
              break;
              case 15:VS->Flip = &Flip_Conv32_15; break;
              case 16:VS->Flip = &Flip_Conv32_16; break;
              case 24:VS->Flip = &Flip_Conv32_24;
            }
          break;
        }
      }
/*      if (VS->X<<1==VMode.XRes&&VS->Y==VMode.YRes)
      {
        if (VS->
      }*/

    }

  } else {
    // now we're supposed to check alot of nasty cases
    if (VMode.Flags&VMode_Segmentated)
      if (VS->X==VMode.XRes&&VS->Y==VMode.YRes&&VS->BPP==VMode.BPP)
      {
        VS->Flip = &Flip_Banks;
        return;
      }
    if (VMode.Flags&VMode_TextMode)
    {
      if (VS->X==VMode.XRes&&VS->Y==VMode.YRes)
      {
        switch(VS->BPP)
        {
          case 15: VS->Flip = &Flip_Text15;
          case 16: VS->Flip = &Flip_Text16;
          case 24: VS->Flip = &Flip_Text24;
          case 32: VS->Flip = &Flip_Text32;
        }
      }
    }
  }
}


// deallocates all real surfaces and set the "outdated" flag on them.
void Outdate_All()
{
  VESA_Surface *VS;
  for(VS = Screen;VS;VS=VS->Next)
  {
    if (VS->Flags&VSurf_EffScreen) continue;
    if (VS->Flags&VSurf_Noalloc) continue;
    if (VS->Flags&VSurf_Outdated) continue; //rulati
    if (VS->Flags&VSurf_Windowed)
    {
      delete VS->Data;
      if (VS->Flags&VSurf_ZBuffered) delete VS->ZBuffer;
      VS->Flags|=VSurf_Nonexist;
    }
    VS->Flags|=VSurf_Outdated;
  }
}

// deallocates all surfaces and set the "outdated" and "nonexist" flags on
void Delete_All()
{
  VESA_Surface *VS;
  for(VS = Screen;VS;VS=VS->Next)
  {
    if (VS->Flags&VSurf_Noalloc) continue;
    if ((VS->Flags&VSurf_Outdated)&&(VS->Flags&VSurf_Windowed)) continue;
    delete VS->Data;
    if (VS->Flags&VSurf_ZBuffered) delete VS->ZBuffer;
    VS->Flags|=VSurf_Nonexist;
    VS->Flags|=VSurf_Outdated;
  }
}


// should clip size to new screen
void VESA_UpdateWindow(VESA_Surface *VS)
{
  VS->Data = new char[VS->PageSize];
  if (VS->Flags&VSurf_ZBuffered) VS->ZBuffer = new float[VS->X*VS->Y];
}

void Renew_All()
{
  VESA_Surface *VS;
  for(VS = Screen;VS;VS=VS->Next)
  {
    if (VS->Flags&VSurf_EffScreen) continue;
    if (VS->Flags&VSurf_Noalloc) continue;
    if (!(VS->Flags&VSurf_Outdated)) continue;
    if (VS->Flags&VSurf_Windowed)
      VESA_UpdateWindow(VS);

    VESA_Find_Flip(VS);
    VS->Flags &= 0xFFFFFFFF-VSurf_Outdated;
  }
}

void Calculate_FOV2Persp(VESA_Surface *VS)
{
  float CTG_half = 1.0/tan(VS->FOV*3.14159265/360.0);
  VS->PX = VS->CX * CTG_half;
  VS->PY = VS->CY * CTG_half;
}

void Set_Screen(long X,long Y,long BPP,char ZBuf,float FOV)
{
  if (!(Screen->Flags&VSurf_Nonexist))
  {
    delete Screen->Data;
    if (Screen->Flags&VSurf_ZBuffered)
      delete Screen->ZBuffer;
  }
  Screen->X = X;
  Screen->Y = Y;
  Screen->BPP = BPP;
  Screen->CPP = BPP+1>>3;
  Screen->BPSL = Screen->X * Screen->CPP;
  Screen->CX = X>>1;
  Screen->CY = Y>>1;
  Screen->X_1 = X-1;
  Screen->Y_1 = Y-1;
  Screen->FOV = FOV;
  if (FOV>0) Calculate_FOV2Persp(Screen);

  Screen->PageSize = Screen->BPSL * Screen->Y;
  Screen->Flags = VSurf_EffScreen;
  Screen->Data = new char[Screen->PageSize];
  if (ZBuf)
  {
    Screen->Flags |= VSurf_ZBuffered;
    Screen->ZBuffer = new float[Screen->X * Screen->Y];
  }
  Screen->Targ = VMode.VideoMem; //this rulez
  Screen->YTable = NULL;
  VESA_MainFlip();
  Screen->Next = Screen->Prev = NULL;
  Last_Surface = Screen; // This is the first one
  Test_Direct_Flipping();
  if (BPP==8) Screen->Pal = new VPalette;
}

VESA_Surface * VESA_Alloc_Surface(long RX,long RY,long RBPP,long FOV,DWord Flags)
{
  VESA_Surface *VS = new VESA_Surface;
  long CPP = RBPP+1>>3;
  VS->X   = RX;    VS->Y   = RY;
  VS->CX  = RX>>1; VS->CY  = RY>>1;
  VS->X_1 = RX-1;  VS->Y_1 = RY-1;
  VS->BPP = RBPP;
  VS->CPP = CPP;
  VS->BPSL = CPP*VS->X;
  VS->PageSize = CPP*RX*RY;
  VS->Flags = Flags;
  VS->FOV = FOV;
  if (FOV!=0.0) Calculate_FOV2Persp(VS);
  if (Direct_FLIP) VS->Targ = VMode.VideoMem; else VS->Targ = Screen->Data;

  if (!(Flags&VSurf_Noalloc))
  {
    VS->Data = new char[VS->PageSize];
    if (Flags&VSurf_ZBuffered)
      VS->ZBuffer = new float[RX*RY];
  }
  VS->Prev = Last_Surface;
  VS->Next = NULL;
  Last_Surface->Next = VS;
  Last_Surface = VS;
  VESA_Find_Flip(VS);
  if (VS->BPP==8) VS->Pal = new VPalette;
  return VS;
}

void Set_FlipTargets()
{
  VESA_Surface *VS;
  for(VS = Screen;VS;VS=VS->Next)
  {
    if (VS->Flags&VSurf_EffScreen) continue;
    if (VS->Flags&VSurf_Noalloc) continue; //manually handled
    if (Direct_FLIP) VS->Targ = VMode.VideoMem; else VS->Targ = Screen->Data;
  }
}

// Sets up a Video mode apropriate to display at (X,Y,BPP) if available.
// Otherwise, it tries several other modes (Fakemodes).
// returns 0 upon failure.
// It's so stupid.
char VESA_Init_Video(long X,long Y,long BPP)
{
  long Ver = VBE_Detect();
  long I,J,K;
  char Res;
  VBE_vgaInfo *VI = (VBE_vgaInfo *)VESABuffer;
  VBE_modeInfo *VM = (VBE_modeInfo *)VESABuffer;

  if (Ver>=0x200)
  {
    //Look for a Graphic, Non-banked, LFB mode with exactly the right
    //Metrics.
    for(I=0;I<VESA_NumModes;I++)
    {
      Res = VESA_GetModeInfo(VESA_ModesAvail[I]);
      if (VM->XResolution!=X||VM->YResolution!=Y||VM->BitsPerPixel!=BPP) continue;
      if (!(VM->ModeAttributes&vbeMdLinear)) continue;
      // OK, this seems fine - set this mode and fill videomode data
      VMode.XRes = X;
      VMode.YRes = Y;
      VMode.BPP = BPP;

      // Update 8-Bit Table allocations
      if ((BPP==8)&&(!VESA_Pal)) {VESA_Pal = new VPalette; memset(VESA_Pal,0,sizeof(VPalette));}
      if ((BPP!=8)&&( VESA_Pal)) {delete VESA_Pal; VESA_Pal = NULL;}

      VMode.BPSL = VM->BytesPerScanLine;
      VMode.PhysMem = VM->PhysBasePtr;
      VMode.VideoMem = (char *)DPMI_Map_P2L(VMode.PhysMem,LFB_LIMIT);;
      VGAPtr = VMode.VideoMem;
      VMode.Flags = VMode_PhysHandle;
      VMode.Driver_ID = VBE2_ID;
      CurrentGraphicsMode = VMode.Mode_ID = VESA_ModesAvail[I] | vbeUseLFB;
      VESA_MainFlip(); //Find a new flipping routine
      Test_Direct_Flipping(); //Test if direct flipping is possible
      Set_FlipTargets();
      Build_YOffs_Table(Screen); //Mock,yeah,bird,yeah,it,yeah,yeah,YEAH.
      Outdate_All();
      Renew_All();

      SetVESAMode(VMode.Mode_ID); //Actually set the selected mode
      if (BPP==8) Set_Palette(VESA_Pal);

      return 1;
    }

  }

  if (Ver>=0x100)
  {
    //Look for a Graphic, Non-banked, LFB mode with exactly the right
    //Metrics.
    for(I=0;I<VESA_NumModes;I++)
    {
      Res = VESA_GetModeInfo(VESA_ModesAvail[I]);
      if (VM->XResolution!=X||VM->YResolution!=Y||VM->BitsPerPixel!=BPP) continue;
      if (VM->ModeAttributes&vbeMdNonBanked) continue;
      // OK, this seems fine - set this mode and fill videomode data
      VMode.XRes = X;
      VMode.YRes = Y;
      VMode.BPP = BPP;
      VMode.BPSL = VM->BytesPerScanLine;
      VMode.PhysMem = 0;
      //this tells the flipper how many banks to jump per 64k block mapped
      Granola = 64/VM->WinGranularity;
      VGAPtr = VMode.VideoMem = (char *)0xA0000;
      VMode.Flags = VMode_Segmentated;
      VMode.Driver_ID = VESA_ID;
      CurrentGraphicsMode = VMode.Mode_ID = VESA_ModesAvail[I];

      // Update 8-Bit Table allocations
      if ((BPP==8)&&(!VESA_Pal)) {VESA_Pal = new VPalette; memset(VESA_Pal,0,sizeof(VPalette));}
      if ((BPP!=8)&&( VESA_Pal)) {delete VESA_Pal; VESA_Pal = NULL;}

      VESA_MainFlip(); //Find a new flipping routine
      Test_Direct_Flipping(); //Test if direct flipping is possible
      Set_FlipTargets();
      Build_YOffs_Table(Screen); //Mock,yeah,bird,yeah,it,yeah,yeah,YEAH.
      Outdate_All();
      Renew_All();   //for all other surfaces

      SetVESAMode(VMode.Mode_ID); //Actually set the selected mode

      return 1;
    }
  }
  return 0;
}

// Generate a list of modes
void Generate_Entry(void (*Driver)(short Mode),short Priority,char *DID,short Mode,long X,long Y,long Module)
{
  Video_entry *VE;
  if (!VE_Root)
  {
    VE_Root = new Video_entry;
    VE_Root->DriverVer = Priority;
    VE_Root->Driver_ID = DID;
    VE_Root->Driver = Driver;
    VE_Root->X = X;
    VE_Root->Y = Y;
    VE_Root->Module = Module; //shit
    VE_Root->Mode = Mode;
    VE_Root->Next = VE_Root->Prev = NULL;
    return;
  }
  VE = VE_Root;
  while (VE->Module<Module&&VE->Next)
    VE=VE->Next;
  if (VE->Module<Module) goto Make_EndVE;
  if (VE->Module>Module) goto Make_InsVE;

  while (VE->Module==Module&&VE->X<X&&VE->Next)
    VE=VE->Next;
  if (VE->Module==Module&&VE->X<X) goto Make_EndVE;
  if (VE->Module>Module||VE->X>X) goto Make_InsVE;

  while (VE->Module==Module&&VE->X==X&&VE->Y<Y&&VE->Next)
    VE=VE->Next;
  if (VE->Module==Module&&VE->X==X&&VE->Y<Y) goto Make_EndVE;
  if (VE->Module>Module||VE->X>X||VE->Y>Y) goto Make_InsVE;
  // same mode - different drivers
  if (VE->DriverVer>=Priority) return; //Inferior Driver Version.
  VE->Driver = Driver; //Superior Driver - Install
  VE->DriverVer = Priority;
  VE->Driver_ID = DID;
  VE->Mode = Mode;
  return;

  Make_EndVE:
  VE->Next = new Video_entry;
  VE->Next->Prev = VE;
  VE = VE->Next;

  VE->DriverVer = Priority;
  VE->Driver_ID = DID;
  VE->Driver = Driver;
  VE->X = X;
  VE->Y = Y;
  VE->Module = Module;
  VE->Mode = Mode;
  VE->Next = NULL;
  return;

  Make_InsVE:
  VE->Prev->Next = new Video_entry;
  VE->Prev->Next->Next = VE;
  VE->Prev->Next->Prev = VE->Prev;
  VE->Prev = VE->Prev->Next;
  VE = VE->Prev;

  VE->DriverVer = Priority;
  VE->Driver_ID = DID;
  VE->Driver = Driver;
  VE->X = X;
  VE->Y = Y;
  VE->Module = Module;
  VE->Mode = Mode;
}


// VGA driver.

//VGA standard tables.             0  1  2  3   4  5   6   7              14   15  16  17 18  19   20
const long VGA_XRES_TABLE[0x14] = {40,40,80,80,320,320,640,80,0,0,0,0,0,320,640,640,640,640,640,320};
const long VGA_YRES_TABLE[0x14] = {25,25,25,25,200,200,200,25,0,0,0,0,0,200,200,350,350,480,480,200};
const long VGA_BPP_TABLE[0x14] = {4,4,4,4,0,2,1,1,0,0,0,0,0,4,4,1,4,1,4,8};
const long VGA_MEMORY_TABLE[0x14] = {0xB8000,0xB8000,0xB8000,0xB8000,0xB8000,0xB8000,0xB8000,0xB0000,0,0,0,0,0,0xA0000,0xA0000,0xA0000,0xA0000,0xA0000,0xA0000,0xA0000};
const long VGA_FLAGS_TABLE[0x14] = {0x0004,0x0004,0x0004,0x0004,0x0000,0x0000,0x0000,0x0004,0,0,0,0,0,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000};
const long VGA_MODULE_TABLE[0x14] = {1,1,1,1,0,0,0,0,0,0,0,0,0,2,2,0,2,0,2,3};
const char VGA_ID_STRING[9] = "Std. VGA";

void VGA_Driver(short Mode)
{
  //Fill in the info
  VMode.XRes = VGA_XRES_TABLE[Mode];
  VMode.YRes = VGA_YRES_TABLE[Mode];
  VMode.BPP = VGA_BPP_TABLE[Mode];

  // Update 8-Bit Table allocations
  if ((VMode.BPP==8||VMode.BPP==4)&&(!VESA_Pal)) {VESA_Pal = new VPalette; memset(VESA_Pal,0,sizeof(VPalette));}
  if ((VMode.BPP!=8&&VMode.BPP!=4)&&( VESA_Pal)) {delete VESA_Pal; VESA_Pal = NULL;}

  VMode.BPSL = VMode.XRes*VMode.BPP;
  Screen->Targ = VMode.VideoMem = VGAPtr = (char *)VGA_MEMORY_TABLE[Mode];
  VMode.Flags = VGA_FLAGS_TABLE[Mode];
  VMode.Driver_ID = VGA_ID;
  CurrentGraphicsMode = VMode.Mode_ID = Mode;
  VESA_MainFlip(); //Find a new flipping routine
  Test_Direct_Flipping(); //Test if direct flipping is possible
  Set_FlipTargets();
  Build_YOffs_Table(Screen); //Mock,yeah,bird,yeah,it,yeah,yeah,YEAH.
  Outdate_All();
  Renew_All();

  SetVGAMode(Mode);
  if (VMode.BPP==4||VMode.BPP==8) Set_Palette(VESA_Pal);
}

void VGA_Driver_Apply()
{
  // I still don't have the VGA detection code, so I'll just assume
  // a VGA BIOS is there.
  long I;
  for(I=0;I<0x14;I++)
  {
    if (!VGA_MODULE_TABLE[I]) continue; //Filter out non-supported modes.
    Generate_Entry(&VGA_Driver,VGA_ID,(char *)VGA_ID_STRING,I,VGA_XRES_TABLE[I],VGA_YRES_TABLE[I],VGA_MODULE_TABLE[I]);
  }
}

//VESA Driver.
const char VESA_ID_STRING[] = {"Generic VESA"};

void VESA_Driver(short Mode)
{
  char Res;
  long I,J,K;
  VBE_vgaInfo *VI = (VBE_vgaInfo *)VESABuffer;
  VBE_modeInfo *VM = (VBE_modeInfo *)VESABuffer;

  Res = VESA_GetModeInfo(Mode);
  // OK, this seems fine - set this mode and fill videomode data
  VMode.XRes = VM->XResolution;
  VMode.YRes = VM->YResolution;
  VMode.BPP = VM->BitsPerPixel;
  VMode.BPSL = VM->BytesPerScanLine;
  VMode.PhysMem = 0;

  //this tells the flipper how many banks to jump per 64k block mapped
  Granola = 64/VM->WinGranularity;
  Screen->Targ = VGAPtr = VMode.VideoMem = (char *)0xA0000;
  VMode.Flags = VMode_Segmentated;
  VMode.Driver_ID = VESA_ID;
  CurrentGraphicsMode = VMode.Mode_ID = Mode;

  // Update 8-Bit Table allocations
  if ((VMode.BPP==8)&&(!VESA_Pal)) {VESA_Pal = new VPalette; memset(VESA_Pal,0,sizeof(VPalette));}
  if ((VMode.BPP!=8)&&( VESA_Pal)) {delete VESA_Pal; VESA_Pal = NULL;}

  VESA_MainFlip(); //Find a new flipping routine
  Test_Direct_Flipping(); //Test if direct flipping is possible
  Set_FlipTargets();
  Build_YOffs_Table(Screen); //Mock,yeah,bird,yeah,it,yeah,yeah,YEAH.
  Outdate_All();
  Renew_All();   //for all other surfaces

  SetVESAMode(Mode); //Actually set the selected mode
}

void VESA_Driver_Apply()
{
  short Ver = VBE_Detect();
  if (!Ver) return;
  VBE_vgaInfo *VI = (VBE_vgaInfo *)VESABuffer;
  VBE_modeInfo *VM = (VBE_modeInfo *)VESABuffer;
  long I;
  char Res;

  for(I=0;I<VESA_NumModes;I++)
  {
    Res = VESA_GetModeInfo(VESA_ModesAvail[I]);
    if (!Res) continue;
//    if (VM->ModeAttributes&vbeMdNonBanked) continue;
    switch (VM->BitsPerPixel)
    {
      case 8:Generate_Entry(&VESA_Driver,VESA_ID,(char *)VESA_ID_STRING,VESA_ModesAvail[I],VM->XResolution,VM->YResolution,3); break;
      case 15:Generate_Entry(&VESA_Driver,VESA_ID,(char *)VESA_ID_STRING,VESA_ModesAvail[I],VM->XResolution,VM->YResolution,4); break;
      case 16:Generate_Entry(&VESA_Driver,VESA_ID,(char *)VESA_ID_STRING,VESA_ModesAvail[I],VM->XResolution,VM->YResolution,5); break;
      case 24:Generate_Entry(&VESA_Driver,VESA_ID,(char *)VESA_ID_STRING,VESA_ModesAvail[I],VM->XResolution,VM->YResolution,6); break;
      case 32:Generate_Entry(&VESA_Driver,VESA_ID,(char *)VESA_ID_STRING,VESA_ModesAvail[I],VM->XResolution,VM->YResolution,7); break;
    }
  }
}


//VBE2 Driver.
const char VBE2_ID_STRING[] = {"VBE 2.0"};

void VBE2_Driver(short Mode)
{
  char Res;
  long I,J,K;
  VBE_vgaInfo *VI = (VBE_vgaInfo *)VESABuffer;
  VBE_modeInfo *VM = (VBE_modeInfo *)VESABuffer;

  Res = VESA_GetModeInfo(Mode);
  // OK, this seems fine - set this mode and fill videomode data
  VMode.XRes = VM->XResolution;
  VMode.YRes = VM->YResolution;
  VMode.BPP = VM->BitsPerPixel;
  VMode.BPSL = VM->BytesPerScanLine;

  VMode.PhysMem = VM->PhysBasePtr;
  VMode.VideoMem = (char *)DPMI_Map_P2L(VMode.PhysMem,LFB_LIMIT);;
  Screen->Targ = VGAPtr = VMode.VideoMem;
  VMode.Flags = VMode_PhysHandle;

  VMode.Driver_ID = VBE2_ID;
  CurrentGraphicsMode = VMode.Mode_ID = Mode;

  // Update 8-Bit Table allocations
  if ((VMode.BPP==8)&&(!VESA_Pal)) {VESA_Pal = new VPalette; memset(VESA_Pal,0,sizeof(VPalette));}
  if ((VMode.BPP!=8)&&( VESA_Pal)) {delete VESA_Pal; VESA_Pal = NULL;}

  VESA_MainFlip(); //Find a new flipping routine
  Test_Direct_Flipping(); //Test if direct flipping is possible
  Set_FlipTargets();
  Build_YOffs_Table(Screen); //Mock,yeah,bird,yeah,it,yeah,yeah,YEAH.
  Outdate_All();
  Renew_All();   //for all other surfaces

  SetVESAMode(Mode); //Actually set the selected mode
}

void VBE2_Driver_Apply()
{
  short Ver = VBE_Detect();
  if (Ver<0x200) return;
  VBE_vgaInfo *VI = (VBE_vgaInfo *)VESABuffer;
  VBE_modeInfo *VM = (VBE_modeInfo *)VESABuffer;
  long I,J;
  char Res;

  for(I=0;I<VESA_NumModes;I++)
  {
    Res = VESA_GetModeInfo(VESA_ModesAvail[I]);
    if (!Res) continue;
    if (!(VM->ModeAttributes&vbeMdLinear)) continue;
    J = VESA_ModesAvail[I] | vbeUseLFB;
    switch (VM->BitsPerPixel)
    {
      case 8:Generate_Entry(&VBE2_Driver,VBE2_ID,(char *)VBE2_ID_STRING,J,VM->XResolution,VM->YResolution,3); break;
      case 15:Generate_Entry(&VBE2_Driver,VBE2_ID,(char *)VBE2_ID_STRING,J,VM->XResolution,VM->YResolution,4); break;
      case 16:Generate_Entry(&VBE2_Driver,VBE2_ID,(char *)VBE2_ID_STRING,J,VM->XResolution,VM->YResolution,5); break;
      case 24:Generate_Entry(&VBE2_Driver,VBE2_ID,(char *)VBE2_ID_STRING,J,VM->XResolution,VM->YResolution,6); break;
      case 32:Generate_Entry(&VBE2_Driver,VBE2_ID,(char *)VBE2_ID_STRING,J,VM->XResolution,VM->YResolution,7); break;
    }
  }
}


void Mdset_Available()
{
  Video_entry *VE;
  VMdMenuTotal = 0;
  long I,J;
  for(VE=VE_Root;VE;VE=VE->Next)
  {
    //look for this module inside the menu.
    J = 1;
    for(I=0;I<VMdMenuTotal;I++)
      if (VMdMenuItems[I]==VE->Module) {J = 0; break;}
    if (J)
    {
      //ok, it wasn't there - we insert
      VMdMenuItems[VMdMenuTotal] = VE->Module;
      VMdMenuTotal++;
    }
  }
}

// ultimate modesetup, checkout.
char Modesetup()
{
  long ModuleSelect = 0;
  long ModuleScroll = 0;
  long ModuleMax;
  long ModeSelect = 0;
  long ModeScroll = 0;
  long ModeMax = 0;
  long ModeTotal = 0;
  long SetupComplete = 0;
  long Modified = 1,Modified2 = 1;
  long MainMenu = 1;
  long First = 1,FirstSelect;
  long I,J,K;
  Video_entry *VE;
  struct XYRES
  {
    long X,Y;
    char *ID;
  };
  FILE *F;
  XYRES *ResMenuItems = NULL;


  VGA_Driver_Apply();
  VESA_Driver_Apply();
  VBE2_Driver_Apply();
  Mdset_Available();
  // ok, now for the first menu
  SetVGAMode(0x03);
  Kill_TMPointer();

  // read cfg file
  F = fopen("VMS.CFG","rt");
  if (F)
  {
    fscanf(F,"%d\n",&ModuleSelect);
    if (ModuleSelect>=VMdMenuTotal) {ModuleSelect = 0; First = 0;}
    fscanf(F,"%d\n",&FirstSelect);
    fclose(F);
  }

  while ((!Keyboard[ScESC])&&(!SetupComplete))
  {
    if (MainMenu)
    {
      if (Modified)
      {
        Modified = 0;
        // Update Scrolls & Selections
        if (ModuleScroll+8<=VMdMenuTotal) ModuleMax = ModuleScroll+8; else ModuleMax = VMdMenuTotal;
        //Refresh View
        GotoXY(0,0);
        printf("Video Mode Setup\n");
        printf("================\n");
        printf(" Available Pixel Formats\n");
        printf(" -----------------------\n");
        for(I=ModuleScroll;I<ModuleMax;I++)
        {
          GotoXY(0,4+I-ModuleScroll);
          if (ModuleSelect==I) printf("* "); else printf("  ");
          printf("%s\n",VFormat_Modules[VMdMenuItems[I]]);
        }
        fflush(stdout);

        // Generate Resolution Menu
        ModeTotal = 0;
        // Count/Allocate
        VE = VE_Root;
        while (VE->Module<VMdMenuItems[ModuleSelect]) VE = VE->Next;
        while (VE->Module==VMdMenuItems[ModuleSelect])
        {
          ModeTotal++;
          VE = VE->Next;
        }
        if (First&&FirstSelect<ModeTotal)
        {
          ModeSelect = FirstSelect;
          ModeScroll = ModeSelect-8;
          if (ModeScroll<0) ModeScroll=0;
          First = 0;
        } else ModeScroll = ModeSelect = 0;

        if (ResMenuItems) delete ResMenuItems;
        ResMenuItems = new XYRES[ModeTotal];
        // Fill in mode info
        VE = VE_Root;
        I = 0;
        while (VE->Module<VMdMenuItems[ModuleSelect]) VE = VE->Next;
        while (VE->Module==VMdMenuItems[ModuleSelect])
        {
          ResMenuItems[I].X = VE->X;
          ResMenuItems[I].Y = VE->Y;
          ResMenuItems[I].ID = VE->Driver_ID;
          I++;
          VE = VE->Next;
        }
        Modified2 = 1;
        //Delete The resolution list space
        for(J=ModeTotal;J<8;J++)
        {
          GotoXY(0,15+J);
          for(I=0;I<40;I++)
            Put_Char(' ');
        }
      }
      // Check Key input
      if (Keyboard[ScUp]&&ModuleSelect)
      {
        ModuleSelect--;
        Modified = 1;
      }
      if (Keyboard[ScDown]&&ModuleSelect<VMdMenuTotal-1)
      {
        ModuleSelect++;
        Modified = 1;
      }
    } else {
      //This should move around in the resolution screen
      // Check Key input
      if (Keyboard[ScUp]&&ModeSelect)
      {
        ModeSelect--;
        Modified2 = 1;
      }
      if (Keyboard[ScDown]&&ModeSelect<ModeTotal-1)
      {
        ModeSelect++;
        Modified2 = 1;
      }

    }
    if (Modified2)
    {
      if (ModeScroll+8<=ModeSelect) ModeScroll++;
      if (ModeScroll>ModeSelect) ModeScroll--;

      if (ModeScroll+8<=ModeTotal) ModeMax = ModeScroll+8; else ModeMax = ModeTotal;
      GotoXY(0,13);
      printf("Available Resolutions\n");
      printf("---------------------\n");
      for(I=ModeScroll;I<ModeMax;I++)
      {
        GotoXY(0,15+I-ModeScroll);
        if (ModeSelect==I) printf("* "); else printf("  ");
        printf("%dx%d (%s)             \n",ResMenuItems[I].X,ResMenuItems[I].Y,ResMenuItems[I].ID);
      }
      Modified2 = 0;
    }
    if (Keyboard[ScEnter])
    {
      F = fopen("VMS.CFG","wt");
      fprintf(F,"%d\n",ModuleSelect);
      fprintf(F,"%d\n",ModeSelect);
      fclose(F);
      VE = VE_Root;
      while (VE->Module<VMdMenuItems[ModuleSelect]) VE = VE->Next;
      while (VE->X<ResMenuItems[ModeSelect].X) VE = VE->Next;
      while (VE->Y<ResMenuItems[ModeSelect].Y) VE = VE->Next;
      VE->Driver(VE->Mode);
      SetupComplete = 1;
    }
    if (Keyboard[ScTab]) MainMenu^=1;
    delay(50);
  }
  return SetupComplete;
}

void Deinitialize_VESA()
{
  Delete_All();
  //destroy VE_root list

  DPMI_Delete(VESAModule);
  DPMI_Delete(VESABuffer);
  if (VMode.Flags&VMode_PhysHandle) DPMI_Del_P2L((void *)VMode.PhysMem);
  SetVGAMode(0x03); // Standard Text Mode.
  if (!(DPMI_Core->Child)) // All DPMI allocations were previously deleted
    DPMI_Deinit_Core();
}