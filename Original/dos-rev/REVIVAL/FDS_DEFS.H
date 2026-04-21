// Enivronmental IDs (Not used for now,only works for dos32/watcom)
#define _O_DOS32 //DOS PROTECTED MODE RUNTIME
//#define _O_WIN32 //WINDOWS 32-BIT APPLICATION
//#define _O_UNIX //UNIX TASK
#define _C_WATCOM //Compiles under WATCOM C
//#define _C_GCC //Compiles under G.C.C. (or DJGPP)
//#define _C_VC //Compiles under Visual C

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

// Video Driver ID's
#define VGA_ID  0x0100
#define VESA_ID 0x1000
#define VBE2_ID 0x1200

// Mathematic Constants.
#define PI_M2            6.28318530716
#define PI               3.14159265358
#define PI_D2            1.57079632679
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
#define Scn_Nolighting   0x0001
#define Scn_Stationary   0x0002
#define Scn_Fogged       0x0004

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

// TriMesh Flags.
// First Bit: Reserved for Hide Track interpolation
#define Tri_Invisible    0x0002 //all of the trimesh is invisible to camera
#define Tri_Processed    0x0004 //already calculated stuff for this trimesh
#define Tri_Ahead        0x0008 //all of the trimesh is ahead of the camera
#define Tri_Inside       0x0010 //all of the trimesh is inside the frustrum
#define Tri_Noshading    0x0020 //no face in the trimesh requires lighting
#define Tri_Notexture    0x0040 //no face in the trimesh has a texture
#define Tri_Phong        0x0080 //Trimesh is rendered via blinn-phong mapping
#define Tri_EdgeError    0x0100 //Fails to be fitted with Winged edges
#define Tri_Closed       0x0200 //Trimesh is completely Closed(has a volume)
#define Tri_Possessed    0x0400 //Trimesh serves a Creature(no spline anim.)
#define Tri_Euler        0x0800 //Euler interpolation Method
#define	Tri_AlignToPath	 0x1000 //Rotation/Position Path alignment
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

// Camera Flags.
#define Cam_Euler        0x0001

#define Particle_Active  0x0001

// Key Scancodes.
#define ScESC 1
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

#define Play_Music

#define Swap_YZ           // All Ys and Zs in 3DS reader will be swapped
#define UV_Wrapping       // Use Mapping coordinate Wrapping
#define UVFlip 0.8        // Flip U or V on maximum difference of..
//#define SpanBuffer      // Use Span-Buffer
//#define DebugMode       // Show all data when it is created/read
//#define DebugStructs    // Display data structures, after reading meshes
//#define RUNTIME_INFO_L1 // Runtime info level 1
#define RUNTIME_INFO_L2 // Runtime info level 2
//#define RUNTIME_INFO_L3 // Runtime info level 3

#ifdef RUNTIME_INFO_L3
#define RUNTIME_INFO_L2
#endif
#ifdef RUNTIME_INFO_L2
#define RUNTIME_INFO_L1
#endif

#define FDS_Version "0.76"

// Geometric code definitions
#define Poly_CoPlanar 0x01
#define Poly_Convex 0x02

#define TriMesh_Abnormal 0x01
#define TriMesh_Convex 0x02

#define Flare_LightSource 0x01
#define Flare_Iguladim 0x02




//MACROS
#define RGB32(R,G,B) ((R<<16)+(G<<8)+B)
#define RGB16(R,G,B) (((R>>3)<<11)+((G>>2)<<5)+(B>>3))
#define Sqr(X)       ((X)*(X))

// Table generator flags
#define TblMod_Made 0x0001