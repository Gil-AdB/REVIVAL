#ifndef FDS_DEFS_H_INCLUDED
#define FDS_DEFS_H_INCLUDED

// basic ANSI-C includes.
#include <stdio.h>
//#include <conio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define RAND_15() (rand() / (((unsigned)RAND_MAX+1) / 32768))
#define RAND_15_MAX 0x7fff

// Enivronmental IDs (Not used for now,only works for dos32/watcom)
#define _O_DOS32 //DOS PROTECTED MODE RUNTIME
//#define _O_WIN32 //WINDOWS 32-BIT APPLICATION
//#define _O_UNIX //UNIX TASK
//#define _C_WATCOM //Compiles under WATCOM C
//#define _C_GCC //Compiles under G.C.C. (or DJGPP)
//#define WIN32 //Compiles under Visual C 6.0

#define MAX_GSTRING 128

#if defined(__APPLE__) || defined(EMSCRIPTEN_MACOS)
#define _strdup strdup
//#define _aligned_malloc aligned_alloc
#define _aligned_malloc(a,b) malloc(a)
#define _aligned_free free
#endif

// Unit IDs
#define _KERNEL_ID          0
#define _INTERPRETER_ID     1
#define _DPMI_ID            2
#define _FLUXIO_ID          3
#define _INFORM_ID          4
#define _EXEC_ID            5

#define COLLISION_DET_ID    0x1000

// Compilation definations
#define Symbol_Variable     0
#define Symbol_Function     1
#define Symbol_Typedef      2
#define Symbol_ReservedWord 3

// Kernel Definitions
#define Kernel_Interactive 0x0001 //generates a page and is workable

#define Console_Active 0x0001
#define Console_NAI    0x0002 //no auto-identification

#define Message_WOdot  0x0001

#define Key_Caps  0x0001
#define Key_Num   0x0002
#define Key_Shift 0x0004
#define Key_Ctrl  0x0008
#define Key_Alt   0x0010

// Video Mode flags
#define VMode_Alternated  0x0001
#define VMode_Segmentated 0x0002
#define VMode_TextMode    0x0004
#define VMode_Nonlinear   0x0006
#define VMode_PhysHandle  0x0008

// Video Surface Attribute flags
#define VSurf_Noalloc     0x0001 //Surface is host at other allocated space
#define VSurf_ZBuffered   0x0002 //Surface contains a Z-Buffer
#define VSurf_Outdated    0x0004 //Surface is out-dated (=flip may crash)
#define VSurf_Windowed    0x0008 //Surface is bounded by a window.
#define VSurf_EffScreen   0x0010 //Surface used exclusively as a screen
#define VSurf_Nonexist    0x0020 //Fatal: don't use the surface
#define VSurf_Locked	  0x0040 //data pointer invalid unless surface is LOCKED or in system memory.

// Video Driver ID's
#define VGA_ID  0x0100
#define VESA_ID 0x1000
#define VBE2_ID 0x1200

// Mathematic Constants.
#define PI_M2            6.28318530716
#define TWOPI			 6.28318530716
#define PI               3.14159265358
#define PI_D2            1.57079632679
#define HALFPI			 1.57079632679
#define EX               2.71828182846
#define EPSILON          0.000001

// Spline Flags.
#define TrackREPEAT      0x0001
#define TrackLOOP        0x0002

// Object Types.
#define Obj_TriMesh      1
#define Obj_Camera       2
#define Obj_CameraTarget 3
#define Obj_Omni         4
#define Obj_SpotLight    5
#define Obj_Dummy        7
#define Obj_Ambient      255

// Scene Flags
enum SceneFlags
{
	Scn_Nolighting		= 1 << 0,
	Scn_Stationary		= 1 << 1,
	Scn_Fogged			= 1 << 2,
	Scn_ZBuffer			= 1 << 3,

	// This flag is set the first time lighting is applied to the scene using the Lighting()
	// function. at this point the StaticLighting() function is called to precalculate all
	// stationary lighting onto stationary objects.
	Scn_StaticLighting	= 1 << 4,

	// enables TBR (Tile-based rendering) for sprites (particles, flares). This should be
	// more efficient when such particles have a large overdraw factor.
	Scn_SpriteTBR		= 1 << 5,
};

// Hide Track Enumeration.
#define HTrack_Visible   1
#define HTrack_Hidden    0

// Material Flags.
#define Mat_Virtual      0x0001
#define Mat_Nonconv      0x0002
#define Mat_RGBInterp    0x0004
#define Mat_Phong        0x0008
#define Mat_TwoSided     0x0010
#define Mat_Transparent  0x0020
#define Mat_Additive	 0x0040
#define Mat_SrcAlpha	 0x0080
#define Mat_SkipZ		 0x0100

#define DEFAULT_BLOCKSIZEX 2
#define DEFAULT_BLOCKSIZEY 2
enum TEXTURE_FLAGS
{
	Txtr_Tiled	= 1 << 0,
	Txtr_Nomip  = 1 << 1,
};

// TriMesh Flags.
// First Bit: Reserved for Hide Track interpolation
enum TrimeshFlags
{
	Tri_Invisible		= 1 << 1,  //all of the trimesh is invisible to camera
	Tri_Processed		= 1 << 2,  //already calculated stuff for this trimesh
	Tri_Ahead			= 1 << 3,  //all of the trimesh is ahead of the camera
	Tri_Inside			= 1 << 4,  //all of the trimesh is inside the frustrum
	Tri_Noshading		= 1 << 5,  //no face in the trimesh requires lighting
	Tri_Notexture		= 1 << 6,  //no face in the trimesh has a texture
	Tri_Phong			= 1 << 7,  //Trimesh is rendered via blinn-phong mapping
	Tri_EdgeError		= 1 << 8,  //Fails to be fitted with Winged edges
	Tri_Closed			= 1 << 9,  //Trimesh is completely Closed(has a volume)
	Tri_Possessed		= 1 << 10, //Trimesh serves a Creature(no spline anim.)
	Tri_Euler			= 1 << 11, //Euler interpolation Method
	Tri_AlignToPath		= 1 << 12, //Rotation/Position Path alignment
	Tri_Radiosity		= 1 << 13, //Trimesh is rendered using Radiosity Methods

	// This flag indicates the trimesh has a fixed position throughout scene runtime.
	// It is set by the scene loader. Manual removal of this flag should be made prior to
	// scene Preprocess routine.
	Tri_Stationary		= 1 << 14,
};

enum SortPriority
{
	SP_Normal = 0,
	SP_DrawFirst = 1,
	SP_DrawLast = 2
};


#define Cam_Euler        0x0001

// Vertex Flags.
#define Vtx_VisNear      0x0001
#define Vtx_VisFar       0x0002
#define Vtx_VisLeft      0x0004
#define Vtx_VisRight     0x0008
#define Vtx_VisUp        0x0010
#define Vtx_VisDown      0x0020
#define Vtx_Visible      0x003F
#define Vtx_Spike        0x0040

// Edge Flags
#define Edge_Concave     0x0001 // Edge is folded so it spiky
#define Edge_True        0x0002 // Edge really forms the outline of the mesh

// Face Flags.
#define Face_Visible     0x0001
#define Face_UWrap       0x0002
#define Face_VWrap       0x0004
#define Face_TwoSided    0x0008
#define Face_Transparent 0x0010
#define Face_PointZTest  0x0020
#define Face_Reflective  0x0040

// Camera Flags.
#define Cam_Euler        0x0001

#define Particle_Active  0x0001

// Key Scancodes.
#define ScESC 41 // an oracle told me it's 41
#define ScF1  59
#define ScF2  60
#define ScF3  61
#define ScF4  62
#define ScF5  63
#define ScF6  64
#define ScF7  65
#define ScF8  66
#define ScF9  67
#define ScF10 68
#define ScF11 87
#define ScF12 88
#define ScScrlLock  70
#define ScTilde 41
#define Sc1 2
#define Sc2 3
#define Sc3 4
#define Sc4 5
#define Sc5 6
#define Sc6 7
#define Sc7 8
#define Sc8 9
#define Sc9 10
#define Sc0 11
#define ScMinus 12
#define ScEqual 13
#define ScBackSpace 14
#define ScTab 15
#define ScQ 16
#define ScW 17
#define ScE 18
#define ScR 19
#define ScT 20
#define ScY 21
#define ScU 22
#define ScI 23
#define ScO 24
#define ScP 25
#define ScOpenSq 26
#define ScCloseSq 27
#define ScEnter 28
#define ScCapsLock 58
#define ScA 30
#define ScS 31
#define ScD 32
#define ScF 33
#define ScG 34
#define ScH 35
#define ScJ 36
#define ScK 37
#define ScL 38
#define ScSemicolon 39
#define ScQuote 40
#define ScLShift 42
#define ScBackSlash 43
#define ScZ 44
#define ScX 45
#define ScC 46
#define ScV 47
#define ScB 48
#define ScN 49
#define ScM 50
#define ScComma 51
#define ScPeriod 52
#define ScSlash 53
#define ScRShift 54
#define ScCtrl 29
#define ScAlt 56
#define ScSpace 57
#define ScHome 71
#define ScUp 72
#define ScPgUp 73
#define ScLeft 75
#define ScKey5 76
#define ScRight 77
#define ScEnd 79
#define ScDown 80
#define ScPgDn 81
#define ScNumLock 69
#define ScGrayMinus 74
#define ScGrayPlus 78
#define ScAsterisk 55
#define ScIns 82
#define ScDel 83

#define Swap_YZ           // All Ys and Zs in 3DS reader will be swapped
#define UV_Wrapping       // Use Mapping coordinate Wrapping
#define UVFlip 0.8        // Flip U or V on maximum difference of..
//#define SpanBuffer      // Use Span-Buffer
//#define DebugMode       // Show all data when it is created/read
//#define DebugStructs    // Display data structures, after reading meshes
#define RUNTIME_INFO_L1 // Runtime info level 1
//#define RUNTIME_INFO_L2 // Runtime info level 2
//#define RUNTIME_INFO_L3 // Runtime info level 3

#ifdef RUNTIME_INFO_L3
#define RUNTIME_INFO_L2
#endif
#ifdef RUNTIME_INFO_L2
#define RUNTIME_INFO_L1
#endif


/*
	FDS version 0.85 (last change: Silvatar, 05.07.02)

	v0.9:
	- mappers with optimized MMX/SSE assembly; optimized software T&L.
	- upgraded class scene representation, operative full scene convertion
	- modular software renderer
	v1.0:
	- render using hardware APIs.
	- 3dsmax exporter plugin
	- full code cleanup
*/
#define FDS_Version "0.85"

#define Tex_Default_Size 256

#define TAS_256x256 0
#define TAS_Arbitrary 1
#define TAS_Disable 2

// Geometric code definitions
#define Poly_CoPlanar 0x01
#define Poly_Convex 0x02

#define TriMesh_Abnormal 0x01
#define TriMesh_Convex 0x02

enum OmniFlags
{
	// 1<<0 used by HTrack_Visible

	// if this flag is clear, the omni will be ignored by the std. rendering pipeline
	Omni_Active		= 1<<1,

	// omnilight will be rendered as a simple lens flare
	Omni_Flare		= 1<<2,

	// depth rejection is based only on central point
	Omni_PointFlare	= 1<<3,

	// If this flag is set, the omnilight is used during Radiosity-based precalculations.
	// uh-huh
	Omni_Radiosity	= 1<<4,

	// This flag indicates the light source has a fixed position throughout scene runtime.
	// It is set by the scene loader. Manual removal of this flag should be made prior to
	// scene Preprocess routine.
	Omni_Stationary = 1<<5,

	Omni_Rand = 1 << 6,
};

//MACROS
#define RGB32(R,G,B) ((R<<16)+(G<<8)+B)
#define RGB16(R,G,B) (((R>>3)<<11)+((G>>2)<<5)+(B>>3))
#define Sqr(X)       ((X)*(X))

// Table generator flags
#define TblMod_Made 0x0001

#ifdef WIN32
// ignorance is bliss
#pragma warning(disable : 4103)
#pragma warning(disable : 4731)
#endif

#endif //FDS_DEFS_H_INCLUDED
