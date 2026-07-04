// DOSRECOVER.CPP — definitions that were present in the original 1998 build
// but are NOT in the preserved Original/dos-rev snapshot (they survived only
// compiled, in FDS.LIB/REV.LIB, and were later culled from the modern port by
// the Tier-1 cleanup, commit 3b09fcb, under _C_WATCOM / #if 0). Reconstructed
// here to complete the from-source Open Watcom link. Pattern matches the base
// definitions still in FILLERS.CPP / IMGGENR.CPP.

#include <StdIO.H>
#include <StdLib.H>
#include <String.H>
#include <Math.H>

#include "Source/FDS_Defs.H"
#include "Source/FDS_Vars.H"
#include "Source/FDS_Decs.H"

// ---- demo globals (extern-declared in REVIVAL/FDS_VARS.H; storage TU missing) ----
float Ambient_Factor;
float Diffusive_Factor;
float ImageSize;
float C_FZP;
float C_rFZP;
float CParticle_ISize;

// ---- WobPointsHeight: WOBTR.ASM does `EXTRN _WobPointsHeight : DWORD` (=YRes/8).
// Set from YRes at grid-init time; provide storage here. ----
extern "C" DWord WobPointsHeight;
#pragma aux WobPointsHeight "_*";
DWord WobPointsHeight;

// ---- base spriter ASM (as declared in FILLERS.CPP) ----
extern "C" void Seven_UP(int X,int Y,int SizeX,int SizeY,char *Txtr,char *Page);
#pragma aux Seven_UP parm [eax] [ebx] [ecx] [edx] [esi] [edi] modify exact [eax ebx ecx edx esi edi];

// ---- filler variants (culled) ----
void The_Seven_UP_HALFSIZE(Vertex *A,Vertex *B,Vertex *C)
{
  long Size = 5.0*A->RZ;
  Seven_UP(A->PX,A->PY,Size<<1,Size<<1,DoFace->Txtr->Txtr->Data,VPage);
}

void The_Seven_UP_64(long X,long Y,long SX,long SY,DWord *Bitmap,char *Page)
{
  Seven_UP(X,Y,SX,SY,(char *)Bitmap,Page);
}

// ---- base grid mapper ASM (Grid_Texture_MapASM_ lives in WOBTR.ASM, 320-wide) ----
extern "C" void Grid_Texture_MapASM(char *Texture,GridPoint *Points,char *VirScr);
#pragma aux Grid_Texture_MapASM parm [ecx] [esi] [edi] modify exact [eax ebx ecx edx esi edi];

// ---- mapper variants (culled). The base ASM is already 320-wide, so _320 maps
// straight to it; _MMX falls back to the base until WOBTRMMX.ASM is recovered
// from its .LST (visually identical, just no MMX acceleration). ----
void Grid_Texture_Mapper_320(GridPoint *GP,Image *Img,DWord *DAC)
{
  WobPointsHeight = YRes >> 3;    // ScreenHeight/8 — the ASM's row-loop count
  Grid_Texture_MapASM((char *)Img->Data,GP,(char *)DAC);
}

void Grid_Texture_Mapper_MMX(GridPoint *GP,Image *Img,DWord *DAC)
{
  WobPointsHeight = YRes >> 3;
  Grid_Texture_MapASM((char *)Img->Data,GP,(char *)DAC);
}
