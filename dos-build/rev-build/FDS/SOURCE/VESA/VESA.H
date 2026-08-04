#define LFB_LIMIT 0x003FFFFF

// May not be used
#define VFormat_TM 1
#define VFormat_4  2
#define VFormat_8  3
#define VFormat_15 4
#define VFormat_16 5
#define VFormat_24 6
#define VFormat_32 7



const char VFormat_Modules[8][40] = {"Abnormal Format","Text Mode (ANSI GFX)","4BPP/16 Colors (EGA)","8BPP/256 Colors (VGA)","15BPP/32K Colors (HiColor)","16BPP/64K Colors (HiColor)","24BPP/16M Colors (TrueColor)","32BPP/16M Colors (TrueColor)"};

#pragma pack(1)

/* SuperVGA information block */
typedef struct {
    char    VESASignature[4];       /* 'VESA' 4 byte signature          */
    short   VESAVersion;            /* VBE version number               */
    long    OemStringPtr;           /* Pointer to OEM string            */
    long    Capabilities;           /* Capabilities of video card       */
    long    VideoModePtr;           /* Pointer to supported modes       */
    short   TotalMemory;            /* Number of 64kb memory blocks     */

    short   OemSoftwareRev;         /* OEM Software revision number     */
    long    OemVendorNamePtr;       /* Pointer to Vendor Name string    */
    long    OemProductNamePtr;      /* Pointer to Product Name string   */
    long    OemProductRevPtr;       /* Pointer to Product Revision str  */
    char    reserved[222];          /* Pad to 256 byte block size       */
    char    OemDATA[256];           /* Scratch pad for OEM data         */
} VBE_vgaInfo;


typedef struct {
    short   ModeAttributes;         // Mode attributes
    char    WinAAttributes;         // Window A attributes
    char    WinBAttributes;         // Window B attributes
    short   WinGranularity;         // Window granularity in k
    short   WinSize;                // Window size in k
    short   WinASegment;            // Window A segment
    short   WinBSegment;            // Window B segment
    long    WinFuncPtr;             // Pointer to window function
    short   BytesPerScanLine;       // Bytes per scanline
    short   XResolution;            // Horizontal resolution
    short   YResolution;            // Vertical resolution
    char    XCharSize;              // Character cell width
    char    YCharSize;              // Character cell height
    char    NumberOfPlanes;         // Number of memory planes
    char    BitsPerPixel;           // Bits per pixel
    char    NumberOfBanks;          // Number of CGA style banks
    char    MemoryModel;            // Memory model type
    char    BankSize;               // Size of CGA style banks
    char    NumberOfImagePages;     // Number of images pages
    char    res1;                   // Reserved
    char    RedMaskSize;            // Size of direct color red mask
    char    RedFieldPosition;       // Bit posn of lsb of red mask
    char    GreenMaskSize;          // Size of direct color green mask
    char    GreenFieldPosition;     // Bit posn of lsb of green mask
    char    BlueMaskSize;           // Size of direct color blue mask
    char    BlueFieldPosition;      // Bit posn of lsb of blue mask
    char    RsvdMaskSize;           // Size of direct color res mask
    char    RsvdFieldPosition;      // Bit posn of lsb of res mask
    char    DirectColorModeInfo;    // Direct color mode attributes

    long    PhysBasePtr;            // Physical address for linear buf
    long    OffScreenMemOffset;     // Pointer to start of offscreen mem
    short   OffScreenMemSize;       // Amount of offscreen mem in 1K's
    char    res2[206];              // Pad to 256 byte block size
  } VBE_modeInfo;


struct Video_entry
{
  short DriverVer; //Best supporting driver
  void (*Driver)(short Mode); //Driver routine
  char *Driver_ID;
  long X,Y,Module;
  short Mode;
  Video_entry *Prev,*Next;
};


// examples for Flip: each surface checks around with the Screen surface.
// if it's LFB and the VS is main, we Flip regular Rep MOVSD. if it's Banks
// and VS is main, we Bankflip. if the VS is partial, we try a slow
// bitblting. LEAT. on application, we simply flip used surface
// (Main is good example) like this: VESA_MVS->Flip(); it will automatically
// use the global variable char *LFB as destination, and VESA_MVS->Data
// as source.
extern void *VESAModule,*VESABuffer;
extern char Graphmode;
extern char Granola;
extern char Screenmem_Model;
extern VideoMode VMode,VEmu;
extern VESA_Surface *MainSurf,*Screen;
extern long VESA_Ver;
extern long PageSize,PageSizeW,PageSizeDW;

extern char *VGAPtr,*VPage;
extern float *ZBuffer;
extern Font *Active_Font;
extern long *YOffs;
extern char *VESA_Pal;

extern void VESA_Message(char *Msg);
extern void VESA_Warning(char *Msg);
extern char VESA_Init_Video(long X,long Y,long BPP);
extern void Set_Screen(long X,long Y,long BPP,char ZBuf,float FOV);
extern void Build_YOffs_Table(VESA_Surface *VS);

extern char *Gouraud_Table;
extern char *ATable,*STable,*CTable,*TTable;

#define VS_LFB         0x01   //It's the actual LFB
#define VS_Banks       0x02   //Sux!! we use BANKS
#define VS_Main        0x04   //Main buffer flips fastest to LFB
#define VS_Partial     0x08   //Partial size buffer, slow, available to LFB only
#define VS_Hires       0x10   //Hiresolution buffer, DEAD SLOW,prolly not available now


#define vbeMemPK        4
#define vbeHiColor      6

#define vbeUseLFB       0x4000      // Enable linear framebuffer mode



#define vbeMdAvailable  0x0001      // Video mode is available
#define vbeMdColorMode  0x0008      // Mode is a color video mode
#define vbeMdGraphMode  0x0010      // Mode is a graphics mode
#define vbeMdNonBanked  0x0040      // Banked mode is not supported
#define vbeMdLinear     0x0080      // Linear mode supported

// Realmode Pointer to Linear Pointer Conversion.
#define RP2LP(p)   (void*)(((unsigned)(p) >> 12) + ((p) & 0xFFFF))


// yeah
void Kill_TMPointer();
#pragma aux Kill_TMPointer=\
  "Mov Ah,01h",\
  "Mov Ch,20h",\
  "Xor Cl,Cl",\
  "Int 10h";

void Flip_DPixel_8(VESA_Surface *VS)
{
  long I,J,K;
  char C;
  char *DWP = VS->Data;
  for(J=0;J<YRes;J++)
    for(I=0;I<XRes;I++)
    {
      C = *DWP++;
      K = (I<<1)+(YOffs[J]<<2);
      VGAPtr[K] = C;
      VGAPtr[K+1] = C;
      K+=XRes<<1;
      VGAPtr[K] = C;
      VGAPtr[K+1] = C;
    }
}

void Flip_DPixel_16(VESA_Surface *VS)
{
  short *DWP = (short *)VS->Data;
  short *DWS = (short *)VGAPtr;
  long I,J,K;
  short C;
  for(J=0;J<YRes;J++)
    for(I=0;I<XRes;I++)
    {
      C = *DWP++;
      K = (I<<1)+(YOffs[J]<<2);
      DWS[K] = C;
      DWS[K+1] = C;
      K+=XRes<<1;
      DWS[K] = C;
      DWS[K+1] = C;
    }
}

void Flip_DPixel_32(VESA_Surface *VS)
{
  long *DWP = (long *)VS->Data;
  long *DWS = (long *)VGAPtr;
  long I,J,K;
  long C;
  for(J=0;J<YRes;J++)
    for(I=0;I<XRes;I++)
    {
      C = *DWP++;
      K = (I<<1)+(YOffs[J]<<2);
      DWS[K] = C;
      DWS[K+1] = C;
      K+=XRes<<1;
      DWS[K] = C;
      DWS[K+1] = C;
    }
}

void Flip_Fake14H(VESA_Surface *VS)
{
  // ok, for each 16bit pixel, we put two 7bit colors next to each other
  unsigned short *DWP = (unsigned short *)VS->Data;
  char *DWS = (char *)VGAPtr;
  long I;
  unsigned short C;
  char R,G,B;
  I=PageSizeW;
  while(I--)
  {
    C = *DWP++;
    R = C>>11;
    G = ((C>>5)&0x3F)>>2;
    B = C&0x1F;
    *DWS++ = (R<<2)+(G>>2);
    *DWS++ = ((G&0x3)<<5)+B+128;
  }
}

void Flip_Fake14V(VESA_Surface *VS)
{
  // ok, for each 16bit pixel, we put two 7bit colors next to each other
  unsigned short *DWP = (unsigned short *)VS->Data;
  char *DWS = (char *)VGAPtr;
  long I,J;
  unsigned short C;
  char R,G,B;
  for(J=0;J<YRes;J++)
  {
    for(I=0;I<XRes;I++)
    {
      C = *DWP++;
      R = C>>11;
      G = ((C>>5)&0x3F)>>2;
      B = C&0x1F;
      *DWS = (R<<2)+(G>>2);
      *(DWS+XRes) = ((G&0x3)<<5)+B+128;
      DWS++;
    }
    DWS+=XRes;
  }
}

// this SUX
void Flip_Fake16_8(VESA_Surface *VS)
{
  long I,J;
  short *DWP = (short *)VS->Data;
  char *DWS = VGAPtr;
  short C;
  char R,G,B;
  for(J=0;J<YRes;J++)
    for(I=0;I<XRes;I++)
    {
      C = *DWP++;
      R = C>>13;
      G = ((C>>5)&0x3F)>>3;
      B = (C&0x1F)>>3;
      *DWS++ = (R<<5)+(G<<2)+B;
    }
}

void Flip_Fake32_8(VESA_Surface *VS)
{
  long I,J;
  DWord *DWP = (DWord *)VS->Data;
  char *DWS = VGAPtr;
  DWord C;
  char R,G,B;
  for(J=0;J<YRes;J++)
    for(I=0;I<XRes;I++)
    {
      C = *DWP++;
      R = (C&0x00FF0000)>>21;
      G = (C&0x0000FF00)>>13;
      B = (C&0x000000FF)>>6;
      *DWS++ = (R<<5)+(G<<2)+B;
    }
}

// crap
// convertions from/to all pixel formats
void Flip_Text15(VESA_Surface *VS) {}
void Flip_Text16(VESA_Surface *VS) {}
void Flip_Text24(VESA_Surface *VS) {}
void Flip_Text32(VESA_Surface *VS)
{
  //YEAH.
  long I,J;
  unsigned short *ANSI_W = (unsigned short *)VGAPtr,C_W;
  char R,G,B;
  char *Data = VS->Data,*DE = Data + VS->PageSize;
  while (Data<DE)
  {
    B = *Data++;
    G = *Data++;
    R = *Data++;
    Data++;
    C_W = 0;
    if (R+G+B>=255) C_W |= 8;
    if (R>=128) C_W |= 4;
    if (G>=128) C_W |= 2;
    if (B>=128) C_W |= 1;
    C_W = (C_W<<8)+219; //219 = ASCII for 'Û'.
    *ANSI_W++ = C_W;
  }
}


// Emulation of Palettized/Packed Pixel format on a Hicolor mode - for kids
void Flip_Emu8_15(VESA_Surface *VS) {}
void Flip_Emu8_16(VESA_Surface *VS) {}
void Flip_Emu8_24(VESA_Surface *VS) {}
void Flip_Emu8_32(VESA_Surface *VS)
{
  char *Data = VS->Data,*DE = Data + VS->X*VS->Y;
  char *Vid = VGAPtr;
  char *PalTbl = VS->Pal,*CPal;
  while(Data<DE)
  {
    CPal = PalTbl + 3*(*Data++)+2;
    *Vid++ = (*CPal--)<<2;
    *Vid++ = (*CPal--)<<2;
    *Vid++ = (*CPal)<<2;
    Vid++;
  }
}
// Convertion between (almost) any two Hicolor/DAC modes.
void Flip_Conv16_15(VESA_Surface *VS) {}
void Flip_Conv16_24(VESA_Surface *VS) {}
void Flip_Conv16_32(VESA_Surface *VS) {}

void FlipConverter3215(char *,char *);
#pragma aux FlipConverter3215=\
  "mov  ecx, PageSize"\
  "shr  ecx, 3"\
  "sub  edi,4"\
  "@@LoopBlast:"\
  "mov eax,[esi]"\
  "mov edx,[esi+4]"\
  "ror eax,16"\
  "add edi,4"\
  "ror edx,16"\
  "shr al,3"\
  "shr dl,3"\
  "rol eax,16"\
  "add esi,8"\
  "rol edx,16"\
  "shr ah,3"\
  "shr dh,3"\
  "shl ax,3"\
  "shl dx,3"\
  "shr eax,6"\
  "shr edx,6"\
  "mov bx,dx"\
  "rol ebx,16"\
  "mov bx,ax"\
  "mov [edi],ebx"\
  "dec ecx"\
  "jnz @@LoopBlast"\
  parm [esi] [edi] modify exact [ecx edi esi];

void Flip_Conv32_15(VESA_Surface *VS)
{
  FlipConverter3215(VS->Data,VS->Targ);
}


void FlipConverter3216(char *,char *);
#pragma aux FlipConverter3216=\
  "mov  ecx, PageSize"\
  "shr  ecx, 3"\
  "sub  edi,4"\
  "@@LoopBlast:"\
  "mov eax,[esi]"\
  "mov edx,[esi+4]"\
  "ror eax,16"\
  "add edi,4"\
  "ror edx,16"\
  "shr al,3"\
  "shr dl,3"\
  "rol eax,16"\
  "add esi,8"\
  "rol edx,16"\
  "shr ah,2"\
  "shr dh,2"\
  "shl ax,2"\
  "shl dx,2"\
  "shr eax,5"\
  "shr edx,5"\
  "mov bx,dx"\
  "rol ebx,16"\
  "mov bx,ax"\
  "mov [edi],ebx"\
  "dec ecx"\
  "jnz @@LoopBlast"\
  parm [esi] [edi] modify exact [ecx edi esi];


void Flip_Conv32_16(VESA_Surface *VS)
{
  FlipConverter3216(VS->Data,VS->Targ);
}


void FlipConvertor2(char *Src,char *Dst);
#pragma aux FlipConvertor2 =    \
  "mov  ecx, [PageSize]" \
  "shr  ecx, 4" \
  "@@LoopBlast:"    \
  "mov eax,[esi]"   \
  "mov ebx,[esi+4]"  \
  "mov [edi],eax"   \
  "mov eax,[esi+8]"\
  "mov [edi+3],bl"\
  "mov [edi+6],ax"\
  "shr ebx,8"\
  "mov [edi+4],bx"\
  "shr eax,16"\
  "mov ebx,[esi+12]"\
  "shl ebx,8"\
  "mov bl,al"\
  "mov [edi+8],ebx"\
  "add esi,16"        \
  "add edi,12"      \
  "dec ecx"     \
  "jnz @@LoopBlast"   \
  parm [esi] [edi]             \
  modify exact [ecx edi esi];

void Flip_Conv32_24(VESA_Surface *VS)
{
  FlipConvertor2(VS->Data,VS->Targ);
}

void Flip_Impossible(VESA_Surface *VS)
{
  //Mission is impossible - we return instantly.
}

void ASM_Flip_Linear(char *Src,char *Dst);
#pragma aux ASM_Flip_Linear=\
  "Mov ECX,PageSizeDW",\
  "Rep MovSD",\
  parm [ESI] [EDI];

void Flip_Linear(VESA_Surface *VS)
{
  ASM_Flip_Linear(VS->Data,VS->Targ);
}

void ASM_Flip_YMLinear(char *Src,char *Dst,long PS);
#pragma aux ASM_Flip_YMLinear=\
  "Rep MovSD",\
  parm [ESI] [EDI] [ECX];

void Flip_YMLinear(VESA_Surface *VS)
{
  long Y2;
  if (Y2>Screen->Y) Y2=Screen->Y; else Y2=VS->Y;
  ASM_Flip_YMLinear(VS->Data,VGAPtr,Y2*VS->X*(BPP+1>>3));
}

void ASM_Flip_BPSLCorrLinear(char *Src,char *Dst,long Ofs);
#pragma aux ASM_Flip_BPSLCorrLinear=\
  "Mov EDX,Screen",\
  "Mov EAx,DWord Ptr [EDX+8]",\
  "Mov EDx,DWord Ptr [EDX+20]",\
  "Shr EDx,2",\
  "@OUTER: Mov ECx,EDx",\
  "  Rep MovSD",\
  "  Add EDI,EBx",\
  "  Dec EAx",\
  "JNZ @OUTER",\
  parm [ESI] [EDI] [EBx] modify exact [EAX EBX EDI ESI ECX EDX];

void Flip_BPSLCorrLinear(VESA_Surface *VS)
{
  ASM_Flip_BPSLCorrLinear(VS->Data,VGAPtr,Screen->BPSL-Screen->X*Screen->CPP);
}

void BankS(char *Src,char *Dst);
#pragma aux BankS =\
  "Xor EDx,EDx",\
  "Mov ECx,PageSize",\
  "Shr ECx,16",\
  "JZ @AfterBS",\
  "@BS:Push ECx",\
  "  Push EDI",\
  "  Mov EAx,4F05h",\
  "  Xor EBx,EBx",\
  "  Int 10h",\
  "  Add Dl,Granola",\
  "  Mov ECx,16384",\
  "  Rep MovSD",\
  "  Pop EDI",\
  "  Pop ECx",\
  "  Dec ECx",\
  "JNZ @BS",\
  "@AfterBS: Xor EBx,EBx",\
  "Mov EAx,4F05h",\
  "Int 10h",\
  "Mov ECx,PageSize",\
  "And ECx,0FFFFh",\
  "Shr ECx,2",\
  "Rep MovSD",\
  modify exact [EDI ESI EAX EBX ECX EDX] parm [ESI] [EDI];

void Flip_Banks(VESA_Surface *VS)
{
  BankS(VS->Data,VGAPtr);
}

void FlipConverter(char *Src,char *Dst);
#pragma aux FlipConverter=\
  "mov  ecx, PageSizeDW"\
  "@@LoopBlast:"\
  "mov eax,[esi]"\
  "add esi,4"\
  "mov [edi],eax"\
  "add edi,3"\
  "dec ecx"\
  "jnz @@LoopBlast"\
  parm [esi] [edi] modify exact [ecx edi esi];

// SUX - Keibinimat
void BitBlt(VESA_Surface *VS,long X,long Y);
#pragma aux BitBlt=\
  "Mov EDI,VGAPtr",\
  "Xor ECx,ECx",\
  "Shl EAX,16",\
  "Mov Cx,Word Ptr [EDx+14]",\
  "Mov ESI,DWord Ptr [EDx]",\
  "Shr ECX,2",\
  "REP MovSD",\
  "Mov Cx,Ax",\
  "REP MovSB",\
  "Shr EAX,16",\
  "Add EDI,XRes",\
  "Sub EDI,EAx",\
  "Add ESI,DWord Ptr [EDX+4]",\
  "Sub ESI,EAx",\
  parm [EDx] [EAx] [EBx];

void Flip_BitBlt(VESA_Surface *VS)
{
  long X=VS->X,Y=VS->Y;
  if (Screen->X<X) X=Screen->X;
  if (Screen->Y<Y) Y=Screen->Y;
  BitBlt(VS,X,Y);
}

void PRAG_Set_RGB(char Reg,char R,char G,char B);
#pragma aux PRAG_Set_RGB=\
  "Mov Dx,3c8h",\
  "Out Dx,Al",\
  "Inc Dx",\
  "Mov Al,Ah",\
  "Out Dx,Al",\
  "Mov Al,Bl",\
  "Out Dx,Al",\
  "Mov Al,Bh",\
  "Out Dx,Al",\
  parm [Al] [Ah] [Bl] [Bh] modify exact [EAX EBX EDX];

void Set_RGB(char Reg,char R,char G,char B) {PRAG_Set_RGB(Reg,R,G,B);}

void PRAG_Set_Palette(VPalette Pal);
#pragma aux PRAG_Set_Palette=\
  "Mov Dx,3c8h",\
  "Xor Al,Al",\
  "Out Dx,Al",\
  "Inc Dx",\
  "Mov ECx,300h",\
  "Rep OutSB",\
  parm [ESI] modify exact [EAX EBX ECX EDX ESI];

void Set_Palette(VPalette Pal)
{
  if (!VESA_Pal) return;
  if (Palette_Reserved&&Pal!=VESA_Pal) return;
  if (VMode.Flags&VMode_TextMode) return;
  PRAG_Set_Palette(Pal);
}


void SetVGAMode(unsigned short Mode)
{
  union REGS r;
  r.w.ax = Mode;
  int386 (0x10, &r, &r);
}
void GotoXY (char X, char Y)
{
  union REGS r;

  r.h.ah = 2;
  r.h.bh = 0;
  r.h.dl = X;
  r.h.dh = Y;

  int386 (0x10, &r, &r);
}

void Put_Char(char C)
{
  union REGS r;
  r.h.al = 0;
  r.h.ah = 2;
  r.h.dl = C;
  int386(0x21, &r, &r);
}

void Transparence_8(char *Source,char *Target) {}
// mask = f7de
void Transparence_16(char *Source,char *Target);
#pragma aux Transparence_16=\
"mov ecx,[PageSize]"\
"add esi,ecx"\
"add edi,ecx"\
"neg ecx"\
"@@MotionBlurLoop:"\
" mov eax,[esi+ecx]"\
" mov ebx,[edi+ecx]"\
" and eax,0f7deh"\
" and ebx,0f7deh"\
" add ebx,eax"\
" shr ebx,1"\
" mov word ptr [edi+ecx],bx"\
" add ecx,2"\
" jnz @@MotionBlurLoop"\
parm [esi] [edi]\
modify exact [eax ebx esi edi ecx]


void Transparence_32(char *Source,char *Target);
#pragma aux Transparence_32=\
"mov ecx,[PageSize]"\
"add esi,ecx"\
"add edi,ecx"\
"neg ecx"\
"@@MotionBlurLoop:"\
" mov eax,[esi+ecx]"\
" mov ebx,[edi+ecx]"\
" and eax,0fefefeh"\
" and ebx,0fefefeh"\
" add ebx,eax"\
" shr ebx,1"\
" mov [edi+ecx],ebx"\
" add ecx,4"\
" jnz @@MotionBlurLoop"\
parm [esi] [edi]\
modify exact [eax ebx esi edi ecx]

void Transparence(VESA_Surface *Source,VESA_Surface *Target)
{
  if (Source->BPP!=Target->BPP) return;
  switch (Source->BPP)
  {
    case 8: Transparence_8(Source->Data,Target->Data); break;
    case 16: Transparence_16(Source->Data,Target->Data); break;
    case 32: Transparence_32(Source->Data,Target->Data);
  }
}

void AlphaBlend(char *Source,char *Target,DWord *PerSource,DWord *PerTarget);
#pragma aux AlphaBlend=\
"emms"\
"punpcklbw mm3,[ebx]"\
"punpcklbw mm2,[ecx]"\
"psrlw mm3,8"\
"psrlw mm2,8"\
"mov ecx,[PageSize]"\
"add esi,ecx"\
"add edi,ecx"\
"neg ECX"\
"@@AlphaBlendLoop:"\
"punpcklbw mm1,[esi+ecx]"\
"punpcklbw mm0,[edi+ecx]"\
"psrlw mm1,8"\
"psrlw mm0,8"\
"pmullw mm1,mm3"\
"pmullw mm0,mm2"\
"psrlw mm1,8"\
"psrlw mm0,8"\
"packuswb mm1,mm1"\
"packuswb mm0,mm0"\
"paddusb  mm1,mm0"\
" movd [edi+ecx],mm1"\
" add ecx,4"\
" jnz @@AlphaBlendLoop"\
" emms"\
parm [esi] [edi] [ebx] [ecx]\
modify exact [eax ebx esi edi ecx 8087]

void Modulate(VESA_Surface *Source,VESA_Surface *Target,DWord SrcMask,DWord TrgMask)
{
  static DWord SrcMask_ = SrcMask;
  static DWord TrgMask_ = TrgMask;
  if (Source->BPP!=Target->BPP) return;
  switch (Source->BPP)
  {
    //it really supports all the modes, so we HAD to do a switch
    case 8: break;
    case 16: break;
    case 32: AlphaBlend(Source->Data,Target->Data,&SrcMask_,&TrgMask_);
  }
}
