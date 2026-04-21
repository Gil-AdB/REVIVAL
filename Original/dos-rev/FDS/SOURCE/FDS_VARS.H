// FDS - Variables and structures
typedef unsigned char Byte;
typedef unsigned short Word;
typedef unsigned long DWord;

// [12 Bytes]
typedef struct
{
  float R,G,B;
} Color;

#pragma pack(1)
// [4 Bytes]
typedef struct
{
  unsigned char R,G,B,A; //Red,Green,Blue,Alpha.
} QColor;

// [1 KB]
typedef struct
{
  QColor C[256];
} Palette;

// Math variables and structures
// [4 Bytes]
struct Four_Chars
{
  unsigned char A,B,C,D;
};
// [4 Bytes]
union Radix256
{
  unsigned long DW; //the whole DWord
  Four_Chars CH; // A = Highest byte,D = Lowest Byte.
};

// [4 Bytes]
union FDW
{
  float F;
  DWord DW;
};

// [12 Bytes]
struct Vector
{
  float X,Y,Z;
};

// [36 Bytes]
typedef float Matrix[3][3];
typedef float Matrix4[4][4]; //stuff

// [8 Bytes]
struct Complex
{
  float X,Y;
};

// [16 Bytes]
struct Quaternion
{
  float W,X,Y,Z;
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

// [88 Bytes]
struct SplineKey
{
  float Frame;                   // Key Frame Number

  Quaternion Pos;                // Normative Quaternion Rotation value
  Quaternion AA;                 // Angle-Axis Quaternion Rotation value

  // 5 Spline Attributive keys.
  float  Tens,                   // Key tension value
         Cont,                   // Key continuity value
         Bias,                   // Key bias value
         EaseTo,                 // Key ease to value
         EaseFrom;               // Key ease from value

  Quaternion DS, DD;             // Key Quaternionic Derivatives
};

// [16 Bytes]
// Total: [16 Bytes+88 Bytes per Key]
struct Spline
{
  DWord NumKeys,CurKey;
  SplineKey *Keys;
  DWord Flags;
};

struct Vector4
{
  float X,Y,Z,T;
};

extern float SinTab[1440];
extern float CosTab[1440];
extern Vector ZeroVector;
extern Matrix Mat_ID;

// Scene variables and structures
// [16 Bytes]
// Total: [16 Bytes+64K per Charperpixel,+1Kb if Palettized]
typedef struct
{
  Palette *Pal;   // Texture's Local Palette. Relevant only for 8BPP Textures
  char *Data;     // Texture's Storage space.
  DWord BPP;      // Texture's BPP. Adjusts format in memory.
                  // DWord size for Aligning, Should be treated as Byte.
  char *FileName; // Texture's Originating File-name.
                  // 0 used when Texture is invalid.
} Texture;

// Digital Image (32bit) with variable size.
typedef struct
{
  long X,Y;
  DWord *Data;
  char *FileName;
} Image;

typedef struct
{
  long U,V;   // 24:8 Fixed point mapping coordinates.
  long RGB;   // Lighting/Spectral components.
  long PARAGRAPH_ALIGN;
} GridPoint;

typedef struct
{
  float X,Y;  // Position on screen
  long RGB;   // Lighting/Spectral components
  long PARAGRAPH_ALIGN;
} AGridPoint;

typedef struct
{
  int X,Y;
  float Z,U,V;
} Bi_Triangle;

typedef struct
{
  int X,Y;
  float Z,U,V;
  float R,G,B;
} Bi_GTriangle;

typedef struct
{
  float X,Y;
  float Z,U,V;
} Bi_Floating;

// [32 Bytes]
// Total: [48 Bytes+64K per Charperpixel,+1Kb if Palettized]

struct Scene; // Temporary declaration for use with the Material structure.

struct Material
{
  Scene               * RelScene; //Related Scene
  Texture             * Txtr;
  DWord                 Flags;
  DWord                 TFlags; //Texture Flags

  Color                 BaseCol; //Base Color for the material
  float                 Luminosity;
  float                 Diffuse; //Diffuse reflection
  float                 Specular; //Specular reflection
  float                 Reflection; //Rebounded light Reflection
  float                 Transparency; //Transparency ratio
  unsigned short        Glossiness; //unknown parameter
  unsigned short        ReflectionMode; //unknown parameter
  char                * ReflectionImage; //Reflection detail
  float                 ReflectionSeamAngle; //unknown
  float                 RefractiveIndex; //Optic Factor
  float                 EdgeTransparency; //unknown factor
  float                 MaxSmoothingAngle; //?
  char                * ColorTexture; //Color Map Textutre Filename
  char                * DiffuseTexture; //Diffuse Map Texture Filename
  char                * SpecularTexture; //Specular Map Texture Filename
  char                * ReflectionTexture; //Reflection Map Texture Filename
  char                * TransparencyTexture; //Transparency Map Texture Filename
  char                * BumpTexture; //Bump Map Texture Filename
  char                * TextureImage; //Texture Map Filename
  unsigned short        TextureFlags; //Should be replaced by DWord TFlags
  Vector                TextureSize; //Texture Proj. Size in spatial coordinates
  Vector                TextureCenter; //Texture Origin in space
  Vector                TextureFallOff; //??
  Vector                TextureVelocity; //U/V animation speed
  char                * TextureAlpha; //Texture Map for Alpha Channel
  unsigned short        NoiseFrequencies; //??
  unsigned short        TextureWrapX; //Wrap flag for U
  unsigned short        TextureWrapY; //Wrap flag for V
  float                 AAStrength; //Antialiasing Strength
  float                 Opacity; //?? gotta learn some physics
  Material            * Next,
                      * Prev;
  char                * Name;
};

// [96 Bytes...32x3]
typedef struct
{
  float          PX,PY;      // Projected X and Y
  float          RU,RV,RZ;   // U/Z, V/Z and 1/Z.
  Vector         Pos,TPos;   // Position and transformed position
  Vector         N;          // Normal and transformed normal
  float          EU,EV;      // Environment mapping coordinates
  float          REU,REV;    // EU/Z, EV/Z.
  float          U,V;        // Original mapping coordinates
  float          LR,LG,LB;   // Light intensity for R/G/B components.
  DWord          Flags;
//  Word           Faces,FRem; // Faces = How many faces share that perticular
                             // vertex, FRem = remaining visible faces.
                             // when FRem reaches 0,the vertex will not be
                             // transformed. (RULEZ)
} Vertex;

// [72 Bytes]
// [48 w/o U/V shit]
typedef struct
{
  Vertex        * A;
  Vertex        * B;
  Vertex        * C;

  DWord           Flags;
  float           NormProd;
  Vector          N;
  FDW             SortZ;

  float           U1,V1,U2,V2,U3,V3; //(U,V) coordinates on face
  void         (* Filler)(Vertex *A,Vertex *B,Vertex *C);
  void         (* Clipper)();
  Material      * Txtr;
} Face;

// [16 Bytes per Status Key]
typedef struct _Status_
{
  float          Frame;
  DWord          Stat;
  _Status_     * Next;
  _Status_     * Prev;
} ObjectStatus;

// [36 Bytes]
typedef struct _Object_
{
  void           * Data;
  DWord            Type;
  DWord            Number;
  Vector         * Pos;
  Matrix         * Rot;
  Vector           Pivot;
  _Object_       * Next;
  _Object_       * Prev;
  _Object_       * Parent;
  char           * Name;
} Object;

// Edge on a RT.Scene Trimesh representation (Useful for Elastic Models,
// Collision detection, and some other neat stuff like that)
struct Edge
{
  Vertex *A,*B;
  Face *P,*Q;
  float Length;
  DWord Flags;
};

// [172 Bytes]
// Total: [220 Bytes + 84 Bytes per Vertex + 56 Bytes per Face +
//         16 Bytes * (Spline Keys + Hide Keys)]
typedef struct _TriMesh_
{
  DWord            VIndex;     //Amount of Vertices on Mesh
  DWord            FIndex;     //Amount of Faces on Mesh
  DWord            EIndex;     //Amount of Edges on Mesh
  Vertex         * Verts;      //Vertex List
  Face           * Faces;      //Face List
  Edge           * Edges;      //Edge List
  Spline           Pos;        //Position Track Spline
  Spline           Scale;      //Scale Track Spline
  Spline           Rotate;     //Orientation Track Spline
  ObjectStatus   * Status;     //Hide Track (crap)
  ObjectStatus   * CurStat;    //Current Hide status
  Vector           IPos;       //Interpolated Position
  Vector           IScale;     //Interpolated Scale
  Quaternion       IRot;       //Interpolated Orientation (Quaternion)
  Matrix           RotMat;     //Interpolated Orientation (Matrix)
  float            BSphereRad; // Bounding Sphere Radius.
  DWord            Flags;
  _TriMesh_      * Next;
  _TriMesh_      * Prev;
} TriMesh;

// [152 Bytes]
// Total: [216 Bytes + 16 Bytes * Spline Keys]
typedef struct _Camera_
{
  Spline           Roll;
  Spline           FOV;
  Spline           Source;
  Spline           Target;
  Vector           ISource;
  Vector           ITarget;
  Matrix           Mat;
  float            IRoll;
  float            IFOV;
  float            PerspX;
  float            PerspY;
  DWord            Flags;
  _Camera_       * Next;
  _Camera_       * Prev;
} Camera;

// [200 Bytes]
typedef struct _Omni_
{
  Vertex           V;
  Face             F;
  Spline           Pos;
//  Spline           Col; // Not Supported - Light Color Track.
  Vector           IPos;
  DWord            Flags;
  Color            L;
  float            FallOff;
  float            HotSpot;
  _Omni_         * Next;
  _Omni_         * Prev;
} Omni;

// [68 Bytes]
typedef struct _SpotLight_
{
  Spline           Source;
  Spline           Target;
  Vector           ISource;
  Vector           ITarget;
  DWord            Flags;
  _SpotLight_    * Next;
  _SpotLight_    * Prev;
} SpotLight;

// [56 Bytes]
// Total: [(56 + 16 * Objects + (220 + 84 * Vertices + 56 * Faces + 16 *
//         (Spline + Hide Keys)) per TriMesh + 216 + (16 * Spline Keys) per
//         Camera + 200 * Omnies + 68 * SpotLights + 192 * Particles) +
//         44 * Materials + (64K * CharsperPixel * Textures) + 1Kb * Textures
//         (only if BPP==8) Bytes] = ALOT.
struct Scene
{
  Object         * ObjectHead;
  TriMesh        * TriMeshHead;
  Camera         * CameraHead;
  Omni           * OmniHead;
  SpotLight      * SpotLightHead;

  float            StartFrame;     // Scene start frame
  float            EndFrame;       // Scene end frame
  DWord            NumOfObjs;      // Number of objects
  struct Particle *Pcl;            // Particle Dynamic array.
  void          (* PclExec)();     // Particle Sys. Kinematics and Effects.
  DWord            NumOfParticles; // Number of particles
  Palette        * Pal;            // Collective palette
  DWord            Flags;          // Scene flags, for optimization
};

// [16 Bytes]
// The functionability of this structure is still unclear... (Silvatar)
// I added the ID variable for more exact tracking of parent fitting
typedef struct _KeyFrame_
{
  Object        * Object;
  DWord           ID;
  _KeyFrame_    * Prev;
  _KeyFrame_    * Next;
} KeyFrame;

//Game Variables (not used)
typedef struct _Item_
{
  long Class;
  long Type;
  long Qual;
  long Quant;
  _Item_ *Prev,*Next;
} Item;

// U know, for kids. (This will be used in games)
typedef struct _Creature_
{
  TriMesh *Shape; // Trimesh will not be animated with splines
  Vector Position;
  Vector Velocity;
  Quaternion QO;
  Matrix Orientation;
  TriMesh *Held; // Held weapon/item (will be copied from loaded trimeshes)
  Camera *View; // Camera is Fixed relative to the trimesh

  long Type;
  long Controller; // (0=1st person,1=CPU,2=mp clone,3=networked dude,etc)
  float Health;
  float Defense;
  Item *Inventory;
  void (*Weaponary)(Vector *Origin,Vector *Direction,float Power); //generates shots
  DWord StatusFlags;
  DWord AI_Flags;
  _Creature_ *Prev,*Next; //Linked list
} Creature;

typedef struct _Shot_
{
  TriMesh *Shape;
  Vector Position;
  Vector Velocity;
  Quaternion QO;
  Matrix Orientation;
  float Clock;
  float Charge;

  _Shot_ *Prev,*Next; //Linked list
} Shot;

extern "C" MMXState;

extern char G_String[256]; //General string.

// Global variables that hold the current scene info
extern Scene            * CurScene;       // Globally referenced scene.

extern float           CurFrame;       // Current frame
extern Camera           * View;           // Main active camera
extern Camera             FC;             // Free Camera..(Future Crew rulez)
extern Material           Default_Mat;    // Default Material...
extern Material         * MatLib;         // yosi

extern unsigned short int OldFPUCW,FPUCW;

extern long Timer_Freq;

extern char **MsgStr;
extern long *MsgClock;
extern DWord *MsgID;


extern char *TxtrBuf,*TxtrAlign;
// Face List. Allocate before use.
extern Face **FList;
extern Face **SList;
extern float FOVX,FOVY;
extern Face *DoFace; //Currently rendered face. Used for further mapper info.
extern float dTime;
extern float Exec_FPS;
extern long Polys,Frames;
extern long CPolys,COmnies,CPcls,CAll,Polys;
extern long Bytes1[257];
extern long Bytes2[257];
extern long Bytes3[257];
extern long Bytes4[257];
extern Texture Phong_Mapping;

extern DWord Colors32[256];

#pragma pack(1)
typedef unsigned long DWord;

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
  long    edi;
  long    esi;
  long    ebp;
  long    reserved;
  long    ebx;
  long    edx;
  long    ecx;
  long    eax;
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
  long Pos; //Position in aligned memory space.
  Aligned_MB *Prev,*Next;
};

extern DWord DPMI_InitFlag;
extern void *DPMI_Main;
extern Module_MB *DPMI_Core;
//char *DPMI_Heap; //Internal Heap (for Aligned memory support)
//DWord DPMI_Heap_Handle;

#pragma pack(4)
// VESA variables and structures
typedef char VPalette[768]; // Direct Video 8-BIT Palette

// Quantized point in 2D
struct QPoint
{
  long X,Y;
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
};

// [92 Bytes]
struct VESA_Surface
{
  char *Data,*Targ;
  float *ZBuffer;
  long X,Y;
  long BPP,CPP;
  long BPSL;
  long CX,CY,X_1,Y_1; //X/2,Y/2,X-1,Y-1 respectively.
  float FOV;          //In degrees (for convinience)
  float PX,PY;        //Perspective ratios
  long PageSize;      //Size of allocation in Data.
  DWord Flags;        //Attributes for the surface.
  long *YTable;       //Y Offset table
  Window *Wnd;        //Lousy piece of shit
  char *Pal;          //SHHIIIIT
  void (*Flip)(VESA_Surface *VS); //appropriate Flipping procedure to LFB.
  VESA_Surface *Next,*Prev;
};


// [13 Bytes]
// Packed for [24 Bytes]
typedef struct
{
  char *Ch;
  char *Len;
  char X,Y,BPP;
  unsigned short XY;
} Font;

// [11 Bytes]
// Packed for [16 Bytes]
struct VESA_Listinfo
{
  long X,Y;             // X and Y resolution
  unsigned short Mode;  // True mode notation (Displayed in Hex)
  char BPP;             // Bits per Pixel
};

struct VideoMode
{
  long XRes,YRes;
  long BPP;
  long BPSL;
  long Driver_ID;
  long Mode_ID;
  char *VideoMem;
  long PhysMem;
  DWord Flags;
};

extern short CurrentGraphicsMode;
extern void *VESAModule,*VESABuffer;
extern short *VESA_ModesAvail;
extern long VESA_NumModes;
extern long *YOffs;
extern char Palette_Reserved;

// General info
extern char Graphmode;  //set if client is on active g.mode state.
                 //otherwise it won't be able to determine what
                 //to do with the surfaces...
extern char Granola;         //GRANOLA=GRANULARITY ;), that is 64Kb/Vesamode->WinGranularity
extern "C" long VESA_BPSL;       //Crazy bytes per scan line crap. not currently supported.
extern "C" long BPP_Shift;
extern char Screenmem_Model; //1=LFB,2=Banks,3=Fake 14bit (not supported)
extern VESA_Surface *Screen; //this is where the LFB surface resides
extern VESA_Surface *MainSurf;   //this is regular surface u flip fastest,aka Page2 aka VPage
extern VESA_Surface *Last_Surface;
extern long VESA_Ver;

extern char Direct_FLIP;

// Global Surface Scalar variables (Fastest access)
extern "C" long XRes,YRes,XRes_1,YRes_1,CntrX,CntrY,BPP,CPP;
extern float CntrEX,CntrEY;
extern long PageSize,PageSizeW,PageSizeDW;

extern VideoMode VMode;
extern char *VGAPtr,*VPage; //Direct access pointers to LFB/Banks and v.Page[2].
extern float *ZBuffer;
extern Font *Active_Font;    //Currently Active font
extern Font *Font1; //a font
extern char *VESA_Pal;

extern void (*Flip)(VESA_Surface *VS);
extern VESA_Surface Layer2;

extern long Basic_FOffs; //basic font offset

// ISR variables and structures
const long Rate_Div = 1193180;
const Basic_Rate = 0xFFFF;
extern long Timer,Old_Int_Count,Old_Int_Rate;
extern char Keyboard[128];

extern Material Flare_IR,Flare_IG,Flare_IB;

// Span buffer variables
extern char *Bitmap;

// Frustrum variables
extern float FOVX,FOVY;

// ASM Filler externals
//extern long ASM_XRes;




extern long Kbpos;

// Particle System Variables
// [192 Bytes]
struct Particle
{
  Vertex V;
  Face F;
  Vector Conv,Vel;
  QColor Color;
  float Grav,Charge;
  float Mass;
  void (*GFX)(Particle *P);
  float Radius;
  float PRad;
  DWord Flags;
};
#pragma pack(1)

//stuff
extern "C" DWord WOBPOINTSHEIGHT;
extern long Grid_X,Grid_Y,Grid_XY;
extern float Plane_Z2U_Conv;
extern float Plane_X2V_Conv;
extern TriMesh *Meatballs; // Target of Meat-Balls tesselation routine
extern float WRFOV;
extern Vector *WPDT;

// for the rend1 scene renderer
extern Particle *ExpPcls;
extern long NumExpPcls;

extern Vertex **ColVtx_Stack,**CVS;
extern Edge **ColEdge_Stack,**CES;

extern FILE *LogFile;

////////////////////////////////////////////////////////////////////
///////////// ASSEMBLER / AUXILERY PRAGMAS SECTION /////////////////
////////////////////////////////////////////////////////////////////

/*void MatrixXMatrix(Matrix A,Matrix B,Matrix C);
#pragma aux MatrixXMatrix=\
"FLD   DWORD PTR [ESI]"\
"FMUL  DWORD PTR [EDX]"\
"FLD   DWORD PTR [ESI]"\
"FMUL  DWORD PTR [EDX+4]"\
"FLD   DWORD PTR [ESI]"\
"FMUL  DWORD PTR [EDX+8]"\
"FLD   DWORD PTR [ESI+4]"\
"FMUL  DWORD PTR [EDX+12]"\
"FLD   DWORD PTR [ESI+4]"\
"FMUL  DWORD PTR [EDX+16]"\
"FLD   DWORD PTR [ESI+4]"\
"FMUL  DWORD PTR [EDX+20]"\
"FXCH  ST(2)"\
"FADDP ST(5),ST(0)"\
"FADDP ST(3),ST(0)"\
"FADDP ST(1),ST(0)"\
"FLD   DWORD PTR [ESI+8]"\
"FMUL  DWORD PTR [EDX+24]"\
"FLD   DWORD PTR [ESI+8]"\
"FMUL  DWORD PTR [EDX+28]"\
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
"FLD   DWORD PTR [ESI+12]"\
"FMUL  DWORD PTR [EDX]"\
"FLD   DWORD PTR [ESI+12]"\
"FMUL  DWORD PTR [EDX+4]"\
"FLD   DWORD PTR [ESI+12]"\
"FMUL  DWORD PTR [EDX+8]"\
"FLD   DWORD PTR [ESI+16]"\
"FMUL  DWORD PTR [EDX+12]"\
"FLD   DWORD PTR [ESI+16]"\
"FMUL  DWORD PTR [EDX+16]"\
"FLD   DWORD PTR [ESI+16]"\
"FMUL  DWORD PTR [EDX+20]"\
"FXCH  ST(2)"\
"FADDP ST(5),ST(0)"\
"FADDP ST(3),ST(0)"\
"FADDP ST(1),ST(0)"\
"FLD   DWORD PTR [ESI+20]"\
"FMUL  DWORD PTR [EDX+24]"\
"FLD   DWORD PTR [ESI+20]"\
"FMUL  DWORD PTR [EDX+28]"\
"FLD   DWORD PTR [ESI+20]"\
"FMUL  DWORD PTR [EDX+32]"\
"FXCH  ST(2)"\
"FADDP ST(5),ST(0)"\
"FADDP ST(3),ST(0)"\
"FADDP ST(1),ST(0)"\
"FXCH  ST(1)"\
"FXCH  ST(2)"\
"FSTP  DWORD PTR [EDI+12]"\
"FSTP  DWORD PTR [EDI+20]"\
"FSTP  DWORD PTR [EDI+16]"\
"FLD   DWORD PTR [ESI+24]"\
"FMUL  DWORD PTR [EDX]"\
"FLD   DWORD PTR [ESI+24]"\
"FMUL  DWORD PTR [EDX+4]"\
"FLD   DWORD PTR [ESI+24]"\
"FMUL  DWORD PTR [EDX+8]"\
"FLD   DWORD PTR [ESI+28]"\
"FMUL  DWORD PTR [EDX+12]"\
"FLD   DWORD PTR [ESI+28]"\
"FMUL  DWORD PTR [EDX+16]"\
"FLD   DWORD PTR [ESI+28]"\
"FMUL  DWORD PTR [EDX+20]"\
"FXCH  ST(2)"\
"FADDP ST(5),ST(0)"\
"FADDP ST(3),ST(0)"\
"FADDP ST(1),ST(0)"\
"FLD   DWORD PTR [ESI+32]"\
"FMUL  DWORD PTR [EDX+24]"\
"FLD   DWORD PTR [ESI+32]"\
"FMUL  DWORD PTR [EDX+28]"\
"FLD   DWORD PTR [ESI+32]"\
"FMUL  DWORD PTR [EDX+32]"\
"FXCH  ST(2)"\
"FADDP ST(5),ST(0)"\
"FADDP ST(3),ST(0)"\
"FADDP ST(1),ST(0)"\
"FXCH  ST(1)"\
"FXCH  ST(2)"\
"FSTP  DWORD PTR [EDI+24]"\
"FSTP  DWORD PTR [EDI+32]"\
"FSTP  DWORD PTR [EDI+28]"\
parm [EDX] [ESI] [EDI]\
modify exact [8087];*/


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

void RoundToInt(long *dest, float val);
#pragma aux RoundToInt=\
	"fistp DWORD [eax]"\
	parm nomemory [eax] [8087]\
	modify exact [eax 8087];

void RoundToDW(DWord *dest, float val);
#pragma aux RoundToDW=\
	"fistp DWORD [eax]"\
	parm nomemory [eax] [8087]\
	modify exact [eax 8087];




///////////////////////
/// TABLES!!!!!!!!!!!
extern DWord _t_sqrttab[];
extern DWord _t_rsqrttab[];
extern DWord _t_exptab[];
extern DWord _t_rexptab[];
extern float _t_fxfl;

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

/*北北北北北北北北北北北北北北北北北北北北北北北北北北北北北北北北北北北北北北*/
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

