#ifndef INTERNAL_3DS_DEFS
#define INTERNAL_3DS_DEFS

// Definations.
#define _Main             0x4D4D  // Main chunk
#define  _MeshData        0x3D3D  // Mesh data
#define   _ObjBlock       0x4000  // Object block
#define    _Polys         0x4100  // Polygon data
#define     _VertList     0x4110  // Vertex list
#define     _FaceList     0x4120  // Face list
#define     _FaceMaterial 0x4130  // Material name, and attachment to faces
#define     _MapList      0x4140  // Mapping coordinates (U and V) list
#define     _TransMatrix  0x4160  // Translation Matrix
#define    _Camera        0x4700  // Camera data
#define    _Light         0x4600  // Light data
//#define     _SpotLight    0x4610  // Spot light data
#define  _KeyFramer       0xB000  // Key framer
#define   _TrackInfo      0xB002  // Track information
#define    _PivotPoint    0xB013  // Pivot point
#define    _TrackPosition 0xB020  // Track position
#define    _TrackRotation 0xB021  // Track rotate
#define    _TrackScale    0xB022  // Track scale
#define    _TrackStatus   0xB029  // Track object status(Visible or hidden)
#define    _NameAndParent 0xB010  // Object name and parent
#define    _DummyName     0xB011  // Dummy object name
#define    _ObjectNumber  0xB030  // Object number
#define   _TrackCameraSrc 0xB003  // Track Camera source
#define    _TrackFOV      0xB023  // Track field of view
#define    _TrackRoll     0xB024  // Track roll
#define   _TrackCameraTgt 0xB004  // Track camera target
#define   _TrackOmni      0xB005  // Track omni light
#define   _TrackSpotTgt   0xB006  // Track spot light target
#define   _TrackSpotSrc   0xB007  // Track spot light source
#define   _Frames         0xB008  // Frame start and finish
#define  _Material        0xAFFF  // Material data
#define   _MaterialName   0xA000  // Material name
#define   _Material2Sides 0xA081  // Material has two sides
#define   _MaterialType   0xA100  // Type of shading
#define   _MaterialTransp 0xA050  // Material is trasparent
#define    _Percentage    0x0030  // Percentage of data
#define   _Texture        0xA200  // Texture material
#define    _MapFile       0xA300  // Name of texture file

// Structures
typedef struct
{
  unsigned short ID;
  signed int     Length;
} Chunk;

// Variables
extern Scene *Host;
extern FILE *IN_3DS;
extern DWord Cur_ID,Chk_ID;
extern DWord Cur_Track;  // Current Track Chunk
extern DWord IN_SIZE;
extern char *CurObjName;
extern char ReadTextureToo;

extern Object *Cur_Obj;
extern TriMesh *Cur_Tri;
extern Omni *Cur_Omni;
extern Camera *Cur_Cam;
extern Material *Cur_Mat;
extern KeyFrame *Cur_Key,*KeyFramer;


// Functions
extern void Read3DSChunk(DWord ChunkEnd,DWord Parent);

#endif