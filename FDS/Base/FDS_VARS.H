#ifndef FDS_VARS_H_INCLUDED
#define FDS_VARS_H_INCLUDED
#include "FDS_DEFS.H"
#include "BaseDefs.h"
#include <stdexcept>
#include <atomic>
#include <float.h>
#include <array>
#include "simde/x86/avx2.h"
#include <simd/vectorclass.h>

// _data must be allocated
class mmreg
{
public:
	union
	{
		byte _d8[8];
		word _d16[4];
		sword _sd16[4];
		dword _d32[2];
		uint64_t _d64;
	};

	static void clearstate();

	// arithmetic operations
	// x.op(r) means x += r
	// x.op(r1,r2) means x = r1+r2


	// addition
	inline void addb(mmreg &r);
	inline void addb(mmreg &r1, mmreg &r2);	
	inline void addw(mmreg &r);
	inline void addw(mmreg &r1, mmreg &r2);
	inline void addd(mmreg &r);
	inline void addd(mmreg &r1, mmreg &r2);
	inline void addq(mmreg &r);
	inline void addq(mmreg &r1, mmreg &r2);

	// subtraction
	inline void subb(mmreg &r);
	inline void subb(mmreg &r1, mmreg &r2);	
	inline void subw(mmreg &r);
	inline void subw(mmreg &r1, mmreg &r2);
	inline void subd(mmreg &r);
	inline void subd(mmreg &r1, mmreg &r2);
	inline void subq(mmreg &r);
	inline void subq(mmreg &r1, mmreg &r2);

	// cxx commands are used for convinient packing/unpacking
	inline void cbw();
	inline void cwb();
	inline void packbw();
	inline void packwb();

	// multiplication
	inline void mulh(mmreg &r1);
	inline void mulh(mmreg &r1, mmreg &r2);
	inline void shrb(uint64_t c);
	inline void shrw(uint64_t c);
	inline void shrd(uint64_t c);
	inline void shrq(uint64_t c);
	inline void sarb(uint64_t c);
	inline void sarw(uint64_t c);
	inline void sard(uint64_t c);
	inline void sarq(uint64_t c);
	inline void shlb(uint64_t c);
	inline void shlw(uint64_t c);
	inline void shld(uint64_t c);
	inline void shlq(uint64_t c);
};
// implementation using inline functions
//#include "MISC/mmreg.inl"
#include "Material.h"
#include "Vertex.h"
#include "Face.h"
#include "Camera.h"
#include "Edge.h"

#pragma pack(push, 1)
// Math variables and structures
// [4 Bytes]
struct Four_Chars
{
	unsigned char A,B,C,D;
};
// [4 Bytes]
union Radix256
{
	uint32_t DW; //the whole DWord
	Four_Chars CH; // A = Highest byte,D = Lowest Byte.
};

// [8 Bytes]
struct Complex
{
	float x, y;
};


// [16 Bytes]
struct Plane
{
	Vector N;
	float O;
};

struct Line
{
	Vector O,D;
};

struct Vector4
{
	float x, y, z, T;
};

extern float SinTab[1440];
extern float CosTab[1440];
extern Matrix Mat_ID;

// Digital Image (32bit) with variable size.
typedef struct
{
	int32_t x, y;
	DWord *Data;
	char *FileName;
} Image;

typedef struct
{
	int32_t U,V;   // 24:8 Fixed point mapping coordinates.
//	int32_t RGB;   // Lighting/Spectral components.	
//	int32_t PARAGRAPH_ALIGN;
	int32_t R,G,B;
} GridPoint;

typedef struct
{
	int32_t U,V;   // 24:8 Fixed point mapping coordinates.
	union
	{
		uint64_t RGBA;   // Lighting/Spectral components.
		struct
		{
			word R, G, B, A;
		};
	};
//	int32_t PARAGRAPH_ALIGN;
//	int32_t R,G,B;
} NewGridPoint;

struct GridPointTG
{
	float u, v;	
	Vec8s BGRA; // we are only using lower 4 shorts
};

struct GridPointT
{
	int32_t u, v;	
};

typedef struct
{
	float x ,y;  // Position on screen
	int32_t RGB;   // Lighting/Spectral components
	int32_t PARAGRAPH_ALIGN;
} AGridPoint;

typedef struct
{
	int32_t x, y;
	float z, U, V;
} Bi_Triangle;

typedef struct
{
	int32_t x, y;
	float z, U, V;
	float R,G,B;
} Bi_GTriangle;

typedef struct
{
	float x, y;
	float z,U,V;
} Bi_Floating;

// [32 Bytes]
// Total: [48 Bytes+64K per Charperpixel,+1Kb if Palettized]

struct Scene; // Temporary declaration for use with the Material structure.

struct CurLight
{
	Color Col;
	Vector Pos; // position in relation to model
	float Range; // light range (before mesh scaling)
	float Range2;
	float rRange;
	dword	dummy1,dummy2;
};

struct MusicModuleInfo
{
	dword ModuleHandle;
	dword Order;
	dword Pattern;
	dword Row;
};

extern char G_String[256]; //General string.

// illumination
extern float Ambient_Factor;
extern float Diffusive_Factor;
extern float Specular_Factor;
extern float Range_Factor;
extern float ImageSize;

// Global variables that hold the current scene info


extern Scene            * CurScene;       // Globally referenced scene.

extern float           CurFrame;       // Current frame
extern Camera           * View;           // Main active camera
extern Camera             FC;             // Free Camera..(Future Crew rulez)
extern Material           Default_Mat;    // Default Material...
extern Material         * MatLib;         // yosi

#ifdef PORTABLE_CODE
extern unsigned int OldFPUCW, FPUCW;
#else
extern unsigned short int OldFPUCW, FPUCW;
#endif

extern dword OldMXCSR, CurMXCSR;
extern int32_t Timer_Freq;

extern char **MsgStr;
extern int32_t *MsgClock;
extern DWord *MsgID;


extern char *TxtrBuf,*TxtrAlign;
// Face List. Allocate before use.
extern Face **FList;
extern Face **SList;
extern float FOVX,FOVY;
//extern Face *DoFace; //Currently rendered face. Used for further mapper info.
extern float dTime;
extern float Exec_FPS;
extern int32_t Polys,Frames;
extern int32_t CPolys,COmnies,CPcls,CAll,Polys;
extern int32_t Bytes1[257];
extern int32_t Bytes2[257];
extern int32_t Bytes3[257];
extern int32_t Bytes4[257];
extern float C_rFZP,C_FZP, C_rNZP, C_NZP;
extern std::atomic<dword> g_renderedPolys;

extern "C" float g_zscale;
extern "C" float g_zscale256;

extern int32_t g_FrameTime;


extern DWord Colors32[256];

// Register structures
struct _RMWORDREGS {
	unsigned short ax, bx, cx, dx, si, di, cflag;
};

struct _RMBYTEREGS {
	unsigned char   al, ah, bl, bh, cl, ch, dl, dh;
};

typedef union {
	struct  _RMWORDREGS x;
	struct  _RMBYTEREGS h;
} RMREGS;

typedef struct {
	unsigned short  es;
	unsigned short  cs;
	unsigned short  ss;
	unsigned short  ds;
} RMSREGS;

typedef struct {
	int32_t    edi;
	int32_t    esi;
	int32_t    ebp;
	int32_t    reserved;
	int32_t    ebx;
	int32_t    edx;
	int32_t    ecx;
	int32_t    eax;
	short   flags;
	short   es,ds,fs,gs,ip,cs,sp,ss;
} _RMREGS;

// Module memory block
// Total size [32+Name size] bytes.
struct Module_MB
{
	DWord Address;
	DWord Handle;  //For low blocks, this will be [Selector:Segment].
	DWord Size;
	Module_MB *Next,*Prev,*Father,*Child,*Brother; //Brother I can see the light!
	// This holds Linked list, Hierarchial tree and Binary tree at the same
	// time. Linked list used for [de]Allocation,h.tree for info/delete
	char *Name;
};

struct Aligned_MB
{
	intptr_t Pos; //Position in aligned memory space.
	Aligned_MB *Prev,*Next;
};

extern DWord DPMI_InitFlag;
extern void *DPMI_Main;
extern Module_MB *DPMI_Core;
//char *DPMI_Heap; //Internal Heap (for Aligned memory support)
//DWord DPMI_Heap_Handle;

#pragma pack(push, 4)
// VESA variables and structures
typedef char VPalette[768]; // Direct Video 8-BIT Palette

// Quantized point in 2D
/*struct QPoint
{
	int32_t X,Y;
};

struct Rectangle
{
	QPoint *UL,*LR; //Upper left, Lower right
};

// will only be logically supported at this point.
struct Window
{
	Rectangle Loc;   //Location on screen
	QPoint DispOffs; //Display offset
};*/

// [92 Bytes]
struct VESA_Surface
{
	byte *Data,*Targ;
	float *ZBuffer;
	int32_t X,Y;
	int32_t BPP,CPP;
	int32_t BPSL;
	int32_t CX,CY,X_1,Y_1; //X/2,Y/2,X-1,Y-1 respectively.
	float FOV;          //In degrees (for convinience)
	float PX,PY;        //Perspective ratios
	int32_t PageSize;      //Size of allocation in Data.
	DWord Flags;        //Attributes for the surface.
	int32_t *YTable;       //Y Offset table
//	Window *Wnd;        //Lousy piece of shit
	char *Pal;          //SHHIIIIT
	void (*Flip)(VESA_Surface *VS); //appropriate Flipping procedure to LFB.
	VESA_Surface *Next,*Prev;
	void *Handle;
	void *Renderer;
};


// [13 Bytes]
// Packed for [24 Bytes]
typedef struct
{
	byte *Ch;
	byte *Len;
	char X,Y,BPP;
	unsigned short XY;
} Font;

// [11 Bytes]
// Packed for [16 Bytes]
struct VESA_Listinfo
{
	int32_t X,Y;             // X and Y resolution
	unsigned short Mode;  // True mode notation (Displayed in Hex)
	char BPP;             // Bits per Pixel
};

struct VideoMode
{
	int32_t XRes,YRes;
	int32_t BPP;
	int32_t BPSL;
	int32_t Driver_ID;
	int32_t Mode_ID;
	char *VideoMem;
	int32_t PhysMem;
	DWord Flags;
};

extern short CurrentGraphicsMode;
extern void *VESAModule,*VESABuffer;
extern short *VESA_ModesAvail;
extern int32_t VESA_NumModes;
extern int32_t *YOffs;
extern char Palette_Reserved;

// General info
extern char Graphmode;  //set if client is on active g.mode state.
//otherwise it won't be able to determine what
//to do with the surfaces...
extern char Granola;         //GRANOLA=GRANULARITY ;), that is 64Kb/Vesamode->WinGranularity
extern "C" int32_t VESA_BPSL;       //Crazy bytes per scan line crap. not currently supported.
extern "C" int32_t BPP_Shift;
extern "C" int32_t PageSize;
extern char Screenmem_Model; //1=LFB,2=Banks,3=Fake 14bit (not supported)
extern VESA_Surface *Screen; //this is where the LFB surface resides
extern VESA_Surface *MainSurf;   //this is regular surface u flip fastest,aka Page2 aka VPage
extern VESA_Surface *Last_Surface;
extern int32_t VESA_Ver;

extern char Direct_FLIP;

// Global Surface Scalar variables (Fastest access)
extern "C" int32_t XRes,YRes,XRes_1,YRes_1,CntrX,CntrY,BPP,CPP;
extern float CntrEX,CntrEY;
extern int32_t PageSize,PageSizeW,PageSizeDW;

extern VideoMode VMode;
extern byte *VGAPtr,*VPage; //Direct access pointers to LFB/Banks and v.Page[2].
extern float *ZBuffer;
extern Font *Active_Font;    //Currently Active font
extern Font *Font1; //a font
extern char *VESA_Pal;

extern float CParticle_ISize;


extern void (*Flip)(VESA_Surface *VS);
extern VESA_Surface Layer2;

extern int32_t Basic_FOffs; //basic font offset

// ISR variables and structures
const int32_t Rate_Div = 1193180;
const unsigned int Basic_Rate = 0xFFFF;
extern std::atomic<int32_t> Timer;
extern int32_t Old_Int_Count,Old_Int_Rate;
extern volatile char Keyboard[128];

//extern Material Flare_IR,Flare_IG,Flare_IB;
extern Material Flare;

// Span buffer variables
//extern char *Bitmap;

// Frustrum variables
extern float FOVX,FOVY;
extern std::atomic<double> FillerPixelcount;
extern float AspectRatio;

// Filler externals
extern dword zReject, zPass;
extern int64_t precisePixelCount;

#ifdef _MSVC
typedef int (__stdcall *VPFunc)(
								void *lpAddress,		// address of region of committed pages
								uint32_t dwSize,			// size of the region
								uint32_t flNewProtect,	// desired access protection
								uint32_t *lpflOldProtect // address of variable to get old protection
								);

#endif

//extern int32_t ASM_XRes;
extern float *InvTbl;


extern int32_t Kbpos;

// Particle System Variables
struct Particle
{
	Vertex V;
	Face F;
	Vector Conv,Vel;
	QColor Color;
	float Grav = 0.0f, Charge = 0.0f;
	float Mass = 0.0f;
	void (*GFX)(Particle *P) = nullptr;
	float Radius = 0.0f;
	float PRad = 0.0f;
	DWord Flags = 0;

	Vertex TrailV[4];
	Face TrailF[2];
	float TrailLength = 0.0f;
	float TrailWidth = 0.0f;

	void InitTrail(Material* M, RasterFunc rasterFunc, float length, float width) {
		TrailLength = length;
		TrailWidth = width;
		TrailV[0].U = 0.0f;
		TrailV[0].V = 0.0f;
		TrailV[1].U = 1.0f;
		TrailV[1].V = 0.0f;
		TrailV[2].U = 1.0f;
		TrailV[2].V = 1.0f;
		TrailV[3].U = 0.0f;
		TrailV[3].V = 1.0f;

		TrailF[0].A = &TrailV[0];
		TrailF[0].B = &TrailV[1];
		TrailF[0].C = &TrailV[2];
		TrailF[0].Filler = rasterFunc;
		TrailF[0].Txtr = M;
		TrailF[0].uvFromVertices();

		TrailF[1].A = &TrailV[0];
		TrailF[1].B = &TrailV[2];
		TrailF[1].C = &TrailV[3];
		TrailF[1].Filler = rasterFunc;
		TrailF[1].Txtr = M;
		TrailF[1].uvFromVertices();
	}

};





////////////////////////////////////
// Radiosity unit - Global variables
extern DWord SCacheSize;

#pragma pack(push , 1)

//stuff
extern "C" dword WobNumOfHorizontalBlocks;
extern "C" dword WobNumOfVerticalBlocks;
extern "C" dword WobBlockJump;
extern "C" dword WobBPSL;

extern "C" dword MMXWobNumOfHorizontalBlocks;
extern "C" dword MMXWobNumOfVerticalBlocks;
extern "C" dword MMXWobBlockJump;
extern "C" dword MMXWobBPSL;

extern "C" DWord WOBPOINTSHEIGHT;
extern int32_t Grid_X,Grid_Y,Grid_XY;
extern float Plane_Z2U_Conv;
extern float Plane_X2V_Conv;
extern float WRFOV;
extern Vector *WPDT;

// for the rend1 scene renderer
extern Particle *ExpPcls;
extern int32_t NumExpPcls;

extern Vertex **ColVtx_Stack,**CVS;
extern Edge **ColEdge_Stack,**CES;

extern FILE *LogFile;

///////////////////////
/// TABLES!!!!!!!!!!!
extern DWord _t_sqrttab[4096];
extern DWord _t_rsqrttab[4096];
extern DWord _t_exptab[4096];
extern DWord _t_rexptab[4096];
extern float _t_fxfl;


////////////////////////////////////////////////////////////////////
///////////// ASSEMBLER / AUXILERY PRAGMAS SECTION /////////////////
////////////////////////////////////////////////////////////////////
#ifdef _C_WATCOM
void MatrixXVector(Matrix M,Vector *U,Vector *V);
#pragma aux MatrixXVector=\
	"FLD   DWORD PTR [ESI]"\
	"FMUL  DWORD PTR [EDX]"\
	"FLD   DWORD PTR [ESI]"\
	"FMUL  DWORD PTR [EDX+12]"\
	"FLD   DWORD PTR [ESI]"\
	"FMUL  DWORD PTR [EDX+24]"\
	"FLD   DWORD PTR [ESI+4]"\
	"FMUL  DWORD PTR [EDX+4]"\
	"FLD   DWORD PTR [ESI+4]"\
	"FMUL  DWORD PTR [EDX+16]"\
	"FLD   DWORD PTR [ESI+4]"\
	"FMUL  DWORD PTR [EDX+28]"\
	"FXCH  ST(2)"\
	"FADDP ST(5),ST(0)"\
	"FADDP ST(3),ST(0)"\
	"FADDP ST(1),ST(0)"\
	"FLD   DWORD PTR [ESI+8]"\
	"FMUL  DWORD PTR [EDX+8]"\
	"FLD   DWORD PTR [ESI+8]"\
	"FMUL  DWORD PTR [EDX+20]"\
	"FLD   DWORD PTR [ESI+8]"\
	"FMUL  DWORD PTR [EDX+32]"\
	"FXCH  ST(2)"\
	"FADDP ST(5),ST(0)"\
	"FADDP ST(3),ST(0)"\
	"FADDP ST(1),ST(0)"\
	"FXCH  ST(1)"\
	"FXCH  ST(2)"\
	"FSTP  DWORD PTR [EDI]"\
	"FSTP  DWORD PTR [EDI+8]"\
	"FSTP  DWORD PTR [EDI+4]"\
	parm [EDX] [ESI] [EDI]\
modify exact [8087];


void Vector_Add(Vector *U,Vector *V,Vector *W);
#pragma aux Vector_Add=\
	"FLD   DWORD PTR [ESI]"\
	"FADD  DWORD PTR [EDX]"\
	"FLD   DWORD PTR [ESI+4]"\
	"FADD  DWORD PTR [EDX+4]"\
	"FLD   DWORD PTR [ESI+8]"\
	"FADD  DWORD PTR [EDX+8]"\
	"FXCH  ST(2)"\
	"FSTP  DWORD PTR [EDI]"\
	"FSTP  DWORD PTR [EDI+4]"\
	"FSTP  DWORD PTR [EDI+8]"\
	parm [ESI] [EDX] [EDI]\
modify exact [8087];

void Vector_Sub(Vector *U,Vector *V,Vector *W);
#pragma aux Vector_Sub=\
	"FLD   DWORD PTR [ESI]"\
	"FSUB  DWORD PTR [EDX]"\
	"FLD   DWORD PTR [ESI+4]"\
	"FSUB  DWORD PTR [EDX+4]"\
	"FLD   DWORD PTR [ESI+8]"\
	"FSUB  DWORD PTR [EDX+8]"\
	"FXCH  ST(2)"\
	"FSTP  DWORD PTR [EDI]"\
	"FSTP  DWORD PTR [EDI+4]"\
	"FSTP  DWORD PTR [EDI+8]"\
	parm [ESI] [EDX] [EDI]\
modify exact [8087];

/*void Vector_Scale(Vector *U,float &Scale,Vector *W);
#pragma aux Vector_Scale=\
"FLD   DWORD PTR [ESI]"\
"FMUL  DWORD PTR [EDX]"\
"FLD   DWORD PTR [ESI+4]"\
"FMUL  DWORD PTR [EDX]"\
"FLD   DWORD PTR [ESI+8]"\
"FMUL  DWORD PTR [EDX]"\
"FXCH  ST(2)"\
"FSTP  DWORD PTR [EDI]"\
"FSTP  DWORD PTR [EDI+4]"\
"FSTP  DWORD PTR [EDI+8]"\
parm [ESI] [EDX] [EDI]\
modify exact [8087];
*/

void FPU_LPrecision(void);
#pragma aux FPU_LPrecision=\
	"   FNStCW  [OldFPUCW]            " \
	"   Mov     ax,[OldFPUCW]         " \
	"   And     eax,1111101111111111b " \
	"   Or      eax,0000100000000000b " \
	"   And     eax,64767             " \
	"   Mov     [FPUCW],ax        " \
	"   FLdCW   [FPUCW]           " \
modify exact [eax 8087];

void FPU_NormalState(void);
#pragma aux FPU_NormalState=\
	"   FLdCW   [OldFPUCW]"\
modify exact [8087];

void RoundToInt(int32_t *dest, float val);
#pragma aux RoundToInt=\
	"fistp DWORD [eax]"\
	parm nomemory [eax] [8087]\
modify exact [eax 8087];

void RoundToDW(DWord *dest, float val);
#pragma aux RoundToDW=\
	"fistp DWORD [eax]"\
	parm nomemory [eax] [8087]\
modify exact [eax 8087];


float SQRT(float Value);
#pragma aux SQRT =                               \
	"     mov   ecx,eax"                          \
	"     and   eax,0xffffff"                     \
	"     shr   ecx,23"                           \
	"     xor   eax,0x800000"                     \
	"     shr   eax,12"                           \
	"     mov   edx,[eax*4+_t_sqrttab]"           \
	"     mov   ecx,[ecx*4+_t_exptab]"            \
	"     or    ecx,edx"                          \
	"     mov   _t_fxfl,ecx"                      \
	"     fld   _t_fxfl"                          \
    parm caller [eax]                            \
modify [eax ecx edx];

/*����������������������������������������������������������������������������*/
// 1.0 / sqrt - fast reciprocal square root (to within 5 digits)

float RSQRT(float Value);

#pragma aux RSQRT =                           \
	"     mov   ecx,eax"                           \
	"     and   eax,0xffffff"                      \
	"     shr   ecx,23"                            \
	"     xor   eax,0x800000"                      \
	"     shr   eax,12"                            \
	"     mov   edx,[eax*4+_t_rsqrttab]"           \
	"     mov   ecx,[ecx*4+_t_rexptab]"            \
	"     test  edx,edx"                           \
	"     jz    rsqrt_psqr"                        \
	"     or    ecx,edx"                           \
	"     mov   _t_fxfl,ecx"                       \
	"     fld   _t_fxfl"                           \
	"     jmp   rsqrt_exit"                        \
	"rsqrt_psqr:"                                  \
	"     add   ecx,0x800000"                      \
	"     or    ecx,edx"                           \
	"     mov   _t_fxfl,ecx"                       \
	"     fld   _t_fxfl"                           \
	"rsqrt_exit:"                                  \
    parm caller [eax]                             \
modify [eax ecx edx];

#endif

template <typename MATRIX, typename VECTOR>
inline void MatrixXVector(const MATRIX M, const VECTOR *U, VECTOR *V)
{
	V->x = M[0][0] * U->x + M[0][1] * U->y + M[0][2] * U->z;
	V->y = M[1][0] * U->x + M[1][1] * U->y + M[1][2] * U->z;
	V->z = M[2][0] * U->x + M[2][1] * U->y + M[2][2] * U->z;
/*
	__asm {
		mov	EDX, [M]
			mov ESI, [U]
			mov EDI, [V]
			FLD   DWORD PTR [ESI]
			FMUL  DWORD PTR [EDX]
			FLD   DWORD PTR [ESI]
			FMUL  DWORD PTR [EDX+12]
			FLD   DWORD PTR [ESI]
			FMUL  DWORD PTR [EDX+24]
			FLD   DWORD PTR [ESI+4]
			FMUL  DWORD PTR [EDX+4]
			FLD   DWORD PTR [ESI+4]
			FMUL  DWORD PTR [EDX+16]
			FLD   DWORD PTR [ESI+4]
			FMUL  DWORD PTR [EDX+28]
			FXCH  ST(2)
			FADDP ST(5),ST(0)
			FADDP ST(3),ST(0)
			FADDP ST(1),ST(0)
			FLD   DWORD PTR [ESI+8]
			FMUL  DWORD PTR [EDX+8]
			FLD   DWORD PTR [ESI+8]
			FMUL  DWORD PTR [EDX+20]
			FLD   DWORD PTR [ESI+8]
			FMUL  DWORD PTR [EDX+32]
			FXCH  ST(2)
			FADDP ST(5),ST(0)
			FADDP ST(3),ST(0)
			FADDP ST(1),ST(0)
			FXCH  ST(1)
			FXCH  ST(2)
			FSTP  DWORD PTR [EDI]
			FSTP  DWORD PTR [EDI+8]
			FSTP  DWORD PTR [EDI+4]
	}*/
}

inline void Vector_Add(Vector *U,Vector *V,Vector *W)
{
	W->x = U->x + V->x;
	W->y = U->y + V->y;
	W->z = U->z + V->z;
	//__asm {
	//	mov esi, [U]
	//		mov edx, [V]
	//		mov edi, [W]
	//		FLD   DWORD PTR [ESI]
	//		FADD  DWORD PTR [EDX]
	//		FLD   DWORD PTR [ESI+4]
	//		FADD  DWORD PTR [EDX+4]
	//		FLD   DWORD PTR [ESI+8]
	//		FADD  DWORD PTR [EDX+8]
	//		FXCH  ST(2)
	//		FSTP  DWORD PTR [EDI]
	//		FSTP  DWORD PTR [EDI+4]
	//		FSTP  DWORD PTR [EDI+8]
	//}
}

inline void Vector_Sub(Vector *U,Vector *V,Vector *W)
{
	W->x = U->x - V->x;
	W->y = U->y - V->y;
	W->z = U->z - V->z;
	//__asm {
	//	mov esi, [U]
	//		mov edx, [V]
	//		mov edi, [W]
	//		FLD   DWORD PTR [ESI]
	//		FSUB  DWORD PTR [EDX]
	//		FLD   DWORD PTR [ESI+4]
	//		FSUB  DWORD PTR [EDX+4]
	//		FLD   DWORD PTR [ESI+8]
	//		FSUB  DWORD PTR [EDX+8]
	//		FXCH  ST(2)
	//		FSTP  DWORD PTR [EDI]
	//		FSTP  DWORD PTR [EDI+4]
	//		FSTP  DWORD PTR [EDI+8]
	//}
}


inline Vector operator * (const Matrix M, const Vector &V)
{
	Vector out;
	MatrixXVector(M, &V, &out);
	return out;
}


static uint64_t GetFPCR()
{
#ifdef _MSC_VER
  return _ReadStatusReg(ARM64_FPCR);
#else
  uint64_t fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr;
#endif
}

static void SetFPCR(uint64_t fpcr)
{
#ifdef _MSC_VER
  _WriteStatusReg(ARM64_FPCR, fpcr);
#else
  __asm__ __volatile__("msr fpcr, %0" : : "ri"(fpcr));
#endif
}

inline void FPU_LPrecision()
{
#ifdef PORTABLE_CODE
#ifdef _MSVC

//	OldFPUCW = _controlfp(_RC_UP | _PC_24, _MCW_RC | _MCW_PC);
    CurMXCSR = OldMXCSR = _mm_getcsr();
	CurMXCSR &= 0b11111111111111110000000001000000;
	CurMXCSR |= 0b00000000000000001101111111000000;
	_mm_setcsr(CurMXCSR);
#elif defined(__ARM_ARCH)

	uint64_t default_fpcr = GetFPCR();
	OldMXCSR = default_fpcr;
	// When AH is disabled, FZ controls flush-to-zero for both inputs and outputs. When AH is enabled,
	// FZ controls flush-to-zero for outputs, and FIZ controls flush-to-zero for inputs.
	constexpr uint32_t FZ = 1 << 24;
	constexpr uint32_t AH = 1 << 1;
	constexpr uint32_t FIZ = 1 << 0;
	constexpr uint32_t flush_to_zero_mask = FZ | AH | FIZ;

	enum ROUNDING_MODE {
		NEAREST,
		ROUND_DOWN,
		ROUND_UP,
		ROUND_TO_ZERO
	};

	constexpr uint32_t rounding_mode_mask = 3 << 22;
	const uint32_t rounding_mode_bits = ROUND_UP;
	const uint32_t flush_to_zero_bits = FZ | AH;

	const uint64_t base = default_fpcr & ~(flush_to_zero_mask | rounding_mode_mask);
  	SetFPCR(base | rounding_mode_bits | flush_to_zero_bits);
#else
// #error "Unsopported arch"
#endif

#else
	__asm {
		// FPU control word - round up, 24 bit precision
		FNStCW  [OldFPUCW]
		Mov     ax,[OldFPUCW]
		And     eax,1111101111111111b
		Or      eax,0000100000000000b
		And     eax,1111110011111111b
		Mov     [FPUCW],ax
		FLdCW   [FPUCW]

		// SSE control word
		STMXCSR	 [OldMXCSR];      RCMSKBIT EXPBIT
		mov eax, [OldMXCSR];  8765432109876543210
		and eax, 11111111111111110000000001000000b
		or  eax, 00000000000000001101111111000000b
		mov [CurMXCSR], eax
		LDMXCSR  [CurMXCSR]
	}
#endif
}


inline void FPU_NormalState()
{
#ifdef PORTABLE_CODE
//	_controlfp(OldFPUCW, _MCW_RC | _MCW_PC);
#else
	__asm {
		FLdCW   [OldFPUCW]
	}
#endif
}

inline void RoundToInt(int32_t *dest, float val)
{
	*dest = static_cast<int32_t>(val);
	//__asm {
	//	fld dword ptr [val]
	//		mov eax, [dest]
	//		fistp [eax]
	//}
}

inline void RoundToDW(DWord *dest, float val)
{
	*dest = static_cast<DWord>(val);
	//__asm {
	//	fld dword ptr [val]
	//		mov eax, [dest]
	//		fistp [eax]
	//}
}


inline float SQRT(float Value)
{
	return sqrtf(Value);
	//__asm {
	//	mov eax, dword ptr [Value]
	//		mov   ecx,eax
	//		and   eax,0xffffff
	//		shr   ecx,23
	//		xor   eax,0x800000
	//		shr   eax,12
	//		mov   edx,[eax*4+_t_sqrttab]
	//		mov   ecx,[ecx*4+_t_exptab] 
	//		or    ecx,edx
	//		mov   _t_fxfl,ecx
	//}
	//return _t_fxfl;
}

/*����������������������������������������������������������������������������*/
// 1.0 / sqrt - fast reciprocal square root (to within 5 digits)

inline float RSQRT(float Value)
{
#ifdef PORTABLE_CODE
	return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_load_ss(&Value)));
	//	return 1.0f / sqrt(Value);
#else
	float &val = Value;
	float retval;
	__asm
	{
		//mov edi, [val]
		rsqrtss xmm0, [val]
		movss [retval], xmm0
	}
	return retval;
#endif

//	__asm {
//		mov eax, dword ptr [Value]
//			mov   ecx,eax
//			and   eax,0xffffff
//			shr   ecx,23
//			xor   eax,0x800000
//			shr   eax,12
//			mov   edx,[eax*4+_t_rsqrttab]
//			mov   ecx,[ecx*4+_t_rexptab]
//			test  edx,edx
//			jz    rsqrt_psqr
//			or    ecx,edx
//			mov   _t_fxfl,ecx
//			jmp   rsqrt_exit
//rsqrt_psqr:
//        add   ecx,0x800000
//			or    ecx,edx
//			mov   _t_fxfl,ecx        
//rsqrt_exit:
//	}
//	return _t_fxfl;
}

inline void Fist(word &r, float x)
{
#ifdef PORTABLE_CODE
	r = static_cast<word>(x);
#else
	__asm
	{
		mov edi, [r]
		FLD dword ptr x
		FISTP word ptr [edi]
	}
#endif
}

inline void Fist(sword &r, float x)
{
#ifdef PORTABLE_CODE
	r = static_cast<sword>(x);
#else
	__asm
	{
		mov edi, [r]
		FLD dword ptr x
		FISTP word ptr [edi]
	}
#endif
}

inline void Fist(dword &r, float x)
{
#ifdef PORTABLE_CODE
	r = static_cast<dword>(x);
#else
	__asm
	{
		mov edi, [r]
		FLD dword ptr x
		FISTP dword ptr [edi]
	}
#endif
}

inline dword Fist(float x)
{
#ifdef PORTABLE_CODE
	//x += 0.99999;
	return (dword)_mm_cvtt_ss2si(_mm_load_ss(&x));
	//return static_cast<dword>(ceilf(x));
#else
	dword ceiled;
	__asm
	{
		FLD   dword ptr [x]
		FISTP dword ptr [ceiled]
		;mov eax, [ceiled]
	}
	return ceiled;
#endif
}

// stores 'data' n times starting from address p
// (p[0...n-1] = data)
inline void FastWrite(void *p, dword data, dword n)
{
#ifdef PORTABLE_CODE
	memset(p, data, n*4);
#else
	__asm
	{
		mov edi, [p]
		mov eax, data
		mov ecx, n

		cmp ecx, 16
		jge optver
		rep stosd
		jmp afterfill
		; setup MMX register
optver:
		movd MM0, data
		movq MM1, MM0
		punpckldq MM0, MM1

		lea edi, [edi+ecx*4]
		neg ecx

		; align write
		test edi, 4
		jz afterprior

		mov [edi+ecx*4], eax
		inc ecx

afterprior:
		mov edx, ecx

		sar ecx, 1 ; # qwords
		and edx, 1 ; remainder dwords

quads:
		;movntq [edi+ecx*8], MM0
		__asm _emit 0x0F
		__asm _emit 0xE7
		__asm _emit 0x04
		__asm _emit 0xCF
		inc ecx
		jnz quads

		and edx,edx
		jz afterpost
		mov [edi-4], eax
afterpost:
		emms
afterfill:
	}
#endif
}

inline void rcpss(float *s)
{
#ifdef PORTABLE_CODE
	*s = _mm_cvtss_f32(_mm_rcp_ss(_mm_load_ss(s)));

	//*s = 1.0 / *s;
#else
	__asm
	{
		mov edi, [s]
		rcpss xmm0, [edi]
		movss [edi], xmm0
	}
#endif
}

#pragma pack (pop)
#pragma pack (pop)
#pragma pack (pop)

inline void range_check_01(float f) {
	if (f < 0 || f > 1) {
		throw std::runtime_error("bug");
	}
}

inline DWord ToQColor(Color c) {
	DWord a = Fist(c.A * 254.9f) << 24;
	DWord r = Fist(c.R * 254.9f) << 16;
	DWord g = Fist(c.G * 254.9f) << 8;
	DWord b = Fist(c.B * 254.9f) << 0;
	return a | r | g | b;
}


#endif //FDS_VARS_H_INCLUDED
