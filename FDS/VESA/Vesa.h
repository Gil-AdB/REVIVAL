#pragma once

#include "Base/FDS_VARS.H"

//#include <intrin.h>
#include "simde/x86/avx2.h"
#include <simd/vectorclass.h>


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

#pragma pack(push, 1)

/* SuperVGA information block */
typedef struct {
    char    VESASignature[4];       /* 'VESA' 4 byte signature          */
    int16_t   VESAVersion;            /* VBE version number               */
    int32_t    OemStringPtr;           /* Pointer to OEM string            */
    int32_t    Capabilities;           /* Capabilities of video card       */
    int32_t    VideoModePtr;           /* Pointer to supported modes       */
    int16_t   TotalMemory;            /* Number of 64kb memory blocks     */

    int16_t   OemSoftwareRev;         /* OEM Software revision number     */
    int32_t    OemVendorNamePtr;       /* Pointer to Vendor Name string    */
    int32_t    OemProductNamePtr;      /* Pointer to Product Name string   */
    int32_t    OemProductRevPtr;       /* Pointer to Product Revision str  */
    char    reserved[222];          /* Pad to 256 byte block size       */
    char    OemDATA[256];           /* Scratch pad for OEM data         */
} VBE_vgaInfo;


typedef struct {
    int16_t   ModeAttributes;         // Mode attributes
    char    WinAAttributes;         // Window A attributes
    char    WinBAttributes;         // Window B attributes
    int16_t   WinGranularity;         // Window granularity in k
    int16_t   WinSize;                // Window size in k
    int16_t   WinASegment;            // Window A segment
    int16_t   WinBSegment;            // Window B segment
    int32_t    WinFuncPtr;             // Pointer to window function
    int16_t   BytesPerScanLine;       // Bytes per scanline
    int16_t   XResolution;            // Horizontal resolution
    int16_t   YResolution;            // Vertical resolution
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

    int32_t    PhysBasePtr;            // Physical address for linear buf
    int32_t    OffScreenMemOffset;     // Pointer to start of offscreen mem
    int16_t   OffScreenMemSize;       // Amount of offscreen mem in 1K's
    char    res2[206];              // Pad to 256 byte block size
  } VBE_modeInfo;


struct Video_entry
{
  int16_t DriverVer; //Best supporting driver
  void (*Driver)(short Mode); //Driver routine
  char *Driver_ID;
  int32_t X,Y,Module;
  int16_t Mode;
  Video_entry *Prev,*Next;
};


#pragma pack (pop)

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
extern int32_t VESA_Ver;
extern int32_t PageSize,PageSizeW,PageSizeDW;

extern byte *VGAPtr,*VPage;
extern float *ZBuffer;
extern Font *Active_Font;
extern int32_t *YOffs;
extern char *VESA_Pal;

extern void VESA_Message(const char *Msg);
extern void VESA_Warning(const char *Msg);
extern char VESA_Init_Video(int32_t X, int32_t Y, int32_t BPP);
extern void Set_Screen(int32_t X, int32_t Y, int32_t BPP,char ZBuf,float FOV);

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

void Build_YOffs_Table(VESA_Surface *VS);

#pragma pack(push, 1)
struct alignas(16) uint128 {
	uint64_t low, high;
};
#pragma pack(pop)


// 32bit MMX Alpha blending, 
inline void AlphaBlend(byte *Source,byte *Target,DWord &PerSource,DWord &PerTarget, dword page_size)
{
	Vec16uc perSrc = Vec16uc(Vec4ui(PerSource));
	Vec16uc perDst = Vec16uc(Vec4ui(PerTarget));

	auto psrc = _mm_srli_epi16(_mm_unpacklo_epi8(perSrc, perSrc), 8);
	auto pdst = _mm_srli_epi16(_mm_unpacklo_epi8(perDst, perDst), 8);

	//uint128 perSrc;
	//perSrc.low = perSrc.high = PerSource | static_cast<uint64>(PerSource) << 32;
	//uint128 perDst;
	//perDst.low = perDst.high = PerTarget | static_cast<uint64>(PerTarget) << 32;

	//uint128 *perSrcRef = &perSrc;
	//uint128 *perDstRef = &perDst;

	Vec16uc src, dst;
	for (int ii = 0; ii < page_size; ii += 32) {
		src.load_a(Source + ii);
		dst.load_a(Target + ii);

		auto srcl_ = _mm_unpacklo_epi8(src, src);
		auto srch_ = _mm_unpackhi_epi8(src, src);
		auto dstl_ = _mm_unpacklo_epi8(dst, dst);
		auto dsth_ = _mm_unpackhi_epi8(dst, dst);

		auto msl = _mm_mulhi_epu16(srcl_, psrc);
		auto msh = _mm_mulhi_epu16(srch_, psrc);
		auto mdl = _mm_mulhi_epu16(dstl_, pdst);
		auto mdh = _mm_mulhi_epu16(dsth_, pdst);

		auto l = _mm_adds_epu8(msl, mdl);
		auto h = _mm_adds_epu8(msh, mdh);

		auto result = Vec16uc(_mm_packus_epi16(l, h));

		result.store_a(Target + ii);

		src.load_a(Source + ii + 16);
		dst.load_a(Target + ii + 16);

		srcl_ = _mm_unpacklo_epi8(src, src);
		srch_ = _mm_unpackhi_epi8(src, src);
		dstl_ = _mm_unpacklo_epi8(dst, dst);
		dsth_ = _mm_unpackhi_epi8(dst, dst);

		msl = _mm_mulhi_epu16(srcl_, psrc);
		msh = _mm_mulhi_epu16(srch_, psrc);
		mdl = _mm_mulhi_epu16(dstl_, pdst);
		mdh = _mm_mulhi_epu16(dsth_, pdst);

		l = _mm_adds_epu8(msl, mdl);
		h = _mm_adds_epu8(msh, mdh);

		result = Vec16uc(_mm_packus_epi16(l, h));

		result.store_a(Target + ii + 16);
	}
//
//	dword ps = page_size;
//
//	__asm
//	{
//#ifdef __clang__
//		pushad
//		mov ebx, perSrcRef
//		mov ecx, perDstRef
//		mov edi, Target
//		mov esi, Source
//#else
//		pushad
//		mov edi, Target
//		mov esi, Source
//		mov eax, perSrcRef
//		mov edx, perDstRef
//		mov ebx, eax
//		mov ecx, edx
//#endif
//		punpcklbw xmm7, [ebx]
//		punpcklbw xmm6, [ecx]
//		psrlw xmm7, 8
//		psrlw xmm6, 8
//		mov ecx, ps
//		add esi, ecx
//		add edi, ecx
//		neg ECX
//#ifndef __clang__
//		Align 16
//#endif
//		AlphaBlendLoop :
//		punpcklbw xmm1, [esi + ecx]
//		punpcklbw xmm0, [edi + ecx]
//		pmulhuw xmm1, xmm7
//		punpckhbw xmm3, [esi + ecx]
//		pmulhuw xmm0, xmm6
//		pmulhuw xmm3, xmm7
//		punpckhbw xmm2, [edi + ecx]
//		paddusb  xmm1, xmm0
//		pmulhuw xmm2, xmm6
//
//		paddusb  xmm3, xmm2
//		packuswb xmm1, xmm3
//		movdqa [edi + ecx], xmm1
//
//		punpcklbw xmm1, [esi + ecx + 16]
//		punpcklbw xmm0, [edi + ecx + 16]
//		pmulhuw xmm1, xmm7
//		punpckhbw xmm3, [esi + ecx + 16]
//		pmulhuw xmm0, xmm6
//		pmulhuw xmm3, xmm7
//		punpckhbw xmm2, [edi + ecx + 16]
//		paddusb  xmm1, xmm0
//		pmulhuw xmm2, xmm6
//
//		paddusb  xmm3, xmm2
//		packuswb xmm1, xmm3
//		movdqa[edi + ecx + 16], xmm1
//
//
//		add ecx, 32
//		jnz AlphaBlendLoop
//		popad
//
//	}


//	__asm
//	{
//		mov esi, Source
//		mov edi, Target
//		mov ebx, PerSource
//		mov ecx, PerTarget
//
//		punpcklbw mm7,[ebx]
//		punpcklbw mm6,[ecx]
//		psrlw mm7,8
//		psrlw mm6,8
//		mov ecx,[PageSize]
//		add esi,ecx
//		add edi,ecx
//		neg ECX
//AlphaBlendLoop:
//			punpcklbw mm1,[esi+ecx]
//			punpcklbw mm0,[edi+ecx]
//			punpcklbw mm3,[esi+ecx+4]
//			punpcklbw mm2,[edi+ecx+4]
//
//;			This should be more efficient
//;			USING OPCODES! stupid assember.
//			pmulhuw mm1,mm7
//			pmulhuw mm0,mm6
//			pmulhuw mm3,mm7
//			pmulhuw mm2,mm6
//;			Original code
//;			psrlw mm1,8
//;			psrlw mm0,8
//;			pmullw mm1,mm7
//;			pmullw mm0,mm6
//;			psrlw mm1,8
//;			psrlw mm0,8
//;			psrlw mm1,8
//;			psrlw mm0,8
//;			pmullw mm3,mm7
//;			pmullw mm2,mm6
//;			psrlw mm1,8
//;			psrlw mm0,8
//
//;			optimized double pixel writeback
//			paddusb  mm1,mm0
//			paddusb  mm3,mm2
//			packuswb mm1,mm3
//;			movq [edi+ecx],mm1
//			movq [edi+ecx],mm1
//			add ecx,8
//		jnz AlphaBlendLoop
//		emms
//	}
 //   byte *mulsrc = reinterpret_cast<byte*>(&PerSource);
 //   byte *muldst = reinterpret_cast<byte*>(&PerTarget);
	//for(int j=0; j<YRes; ++j)
	//{	
	//	for(int i=0, n=XRes; i<n; ++i)
	//	{
 //           byte *src = reinterpret_cast<byte *>(&Source[i*4]);
 //           byte *dst = reinterpret_cast<byte *>(&Target[i*4]);
 //           for (int channel=0; channel < 4; ++channel) {
 //               int res = (src[channel] * mulsrc[channel] +  dst[channel]*muldst[channel]) >> 8;
	//			if (res > 255) res = 255;
	//			dst[channel] = res;
 //           }
	//	}
	//	Source += MainSurf->BPSL;
	//	Target += MainSurf->BPSL;
	//}
}

//inline void AlphaBlend(byte* Source, byte* Target, DWord& PerSource, DWord& PerTarget, DWord Size) {
//	//auto tmp = PageSize;
//	//PageSize = Size;
//	AlphaBlend(Source, Target, PerSource, PerTarget, Size);
//	//PageSize = tmp;
//}


inline void Transparence_16(byte *Source, byte *Target)
{
	// x86 asm motion-blur kernel deleted; no portable replacement yet.
}

inline void Transparence_32(byte *Source, byte *Target)
{
	// x86 asm motion-blur kernel deleted; no portable replacement yet.
}

inline void Modulate(VESA_Surface *Source,VESA_Surface *Target,DWord SrcMask,DWord TrgMask, dword PageSize)
{
  if (Source->BPP!=Target->BPP) return;
  switch (Source->BPP)
  {
    //it really supports all the modes, so we HAD to do a switch
    case 8: break;
    case 16: break;
    case 32: AlphaBlend(Source->Data,Target->Data,SrcMask,TrgMask, PageSize);
  }
}

inline void Transparence_8(byte *Source,byte *Target) {}


inline void Transparence(VESA_Surface *Source,VESA_Surface *Target)
{
  if (Source->BPP!=Target->BPP) return;
  switch (Source->BPP)
  {
    case 8: Transparence_8(Source->Data,Target->Data); break;
    case 16: Transparence_16(Source->Data,Target->Data); break;
    case 32: Transparence_32(Source->Data,Target->Data);
  }
}
