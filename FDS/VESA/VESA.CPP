// Silvatar's Modular VESA/VBE interface unit,Version 2.01.
// Next: Make sure System can be reinitialized without loss of memory
//       also make sure all windows + screenclones auto-adjust.
//       also try to logically support windows, and insert mouse interface

#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"

#include "Vesa.h"

void *VESAModule,*VESABuffer;
short *VESA_ModesAvail;
int32_t VESA_NumModes;

// General info
char Graphmode = 0;  //set if client is on active g.mode state.
								 //otherwise it won't be able to determine what
                 //to do with the surfaces...
char Granola;         //GRANOLA=GRANULARITY ;), that is 64Kb/Vesamode->WinGranularity
int32_t VESA_BPSL;       //Crazy bytes per scan line crap. not currently supported.
int32_t BPP_Shift;
char Screenmem_Model; //1=LFB,2=Banks,3=Fake 14bit (not supported)
VESA_Surface *Screen; //this is where the LFB surface resides
VESA_Surface *MainSurf;   //this is regular surface u flip fastest,aka Page2 aka VPage
VESA_Surface *Last_Surface;
VideoMode VMode;
int32_t VESA_Ver = 0;

char Direct_FLIP = 0;

// Global Surface Scalar variables (Fastest access)
int32_t XRes,YRes,XRes_1,YRes_1,CntrX,CntrY,BPP,CPP;
float CntrEX,CntrEY;
int32_t PageSize,PageSizeW,PageSizeDW;
short CurrentGraphicsMode;
char Palette_Reserved = 0;

byte *VGAPtr,*VPage; //Direct access pointers to LFB/Banks and v.Page[2].
float *ZBuffer;
Font *Active_Font;    //Currently Active font
char *VESA_Pal = NULL;

char *Fake8_R,*Fake8_G,*Fake8_B;

int32_t VMdMenuItems[8],VMdMenuTotal;

void (*Flip)(VESA_Surface *VS);
Video_entry *VE_Root;

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

void VESA_Warning(const char *Msg)
{
//  printf("<VESA> Warning: %s\n",Msg);
}

// SDL2 backend handles its own teardown; cleanup here is a no-op.
void Deinitialize_VESA()
{
}

// initializes video unit assuming a remote unit handles the actual display.
// this setup will only allow access to logical (secondary) buffer. direct
// access to linear FB will be impossible.
void VESA_InitExternal(int32_t x, int32_t y,int32_t bpp)
{
	// set assumed videomode.
	XRes = VMode.XRes = x;
	YRes = VMode.YRes = y;
	BPP = VMode.BPP = bpp;

	Graphmode = 1;
	VESA_BPSL = x * ((bpp+7)>>3);
	// dword align.
	if (VESA_BPSL&3) VESA_BPSL += 4 - VESA_BPSL&3;

	Screen = new VESA_Surface;
	memset(Screen,0,sizeof(VESA_Surface));
	Screen->Flags = VSurf_Nonexist | VSurf_EffScreen;

	MainSurf = new VESA_Surface;
	memset(MainSurf, 0, sizeof(VESA_Surface));
	MainSurf->Flags = VSurf_Nonexist | VSurf_EffScreen;

	VPage = NULL;
	VGAPtr = NULL;
}

// recieves external vPagePtr and sets up MainSurf according to mode information
// given by VESA_InitExternal. second paramater is a callback function to execute flipping
// from secondary buffer to the screen.
void VESA_VPageExternal(VESA_Surface *ExtrnVS)//void *vPagePtr, void (*FlipperCallback)(VESA_Surface *VS))
{
	VESA_Surface *VS = MainSurf;
	memcpy(VS, ExtrnVS, sizeof(VESA_Surface));


//	VS->Flip = FlipperCallback;
//	VS->Data = (char *)vPagePtr;
//	VS->Targ = NULL; // external flipper cant support TARGET.
//	VS->ZBuffer = NULL; // Z-buffers aren't supported.

	int32_t X = VS->X;
	int32_t Y = VS->Y;

//	VS->BPP = VMode.BPP;
	VS->CPP = (VS->BPP+7)>>3;
	VS->BPSL = VESA_BPSL;

	VS->CX = X>>1;
	VS->CY = Y>>1;
	VS->X_1 = X-1;
	VS->Y_1 = Y-1;
	VS->FOV = 0;

	VS->PageSize = VS->BPSL * VS->Y;
	VS->Flags = VSurf_EffScreen;

	//if (VS->YTable) Build_YOffs_Table(Screen);
	//VESA_MainFlip();

	//if (BPP==8) Screen->Pal = new VPalette;
	Build_YOffs_Table(MainSurf);
	VESA_Surface2Global(VS);
}

void VESA_UninitExternal()
{
}
