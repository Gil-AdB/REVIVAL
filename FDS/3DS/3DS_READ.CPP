// Main File Reader, Format: 3DS ver 4.0
#include "Base/FDS_DEFS.H"
#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"

#ifdef _C_WATCOM
#include <Mem.H>
#endif
#include <math.h>
#include <stdlib.h>
//#include <dos.h>
#include <stdio.h>

#include "I_defs.h"

#include "WORLD.CPP"
#include "TRACKS.CPP"
#include "MATERIAL.CPP"
#include "FUtils.h"

// Available Readers.
struct
{
  unsigned short ID;
  void (*Reader)(unsigned int EndPos);
} ChunkData[]=
{
  {_Main          , NULL},
// Object chunks
  {_MeshData      , NULL},
  {_ObjBlock      , ReadObjectName},
  {_Polys         , ReadTriMesh},
  {_VertList      , ReadVertList},
  {_FaceList      , ReadFaceList},
  {_FaceMaterial  , ReadFaceMaterial},
  {_MapList       , ReadMapList},
  {_TransMatrix   , ReadTranslation},
  {_Camera        , ReadCamera},
  {_Light         , ReadLight},
//  {_SpotLight     , ReadSpotLight},
// Keyframer chunks
  {_KeyFramer     , NULL},
  {_TrackInfo     , NULL},
  {_TrackCameraSrc, NULL},
  {_TrackCameraTgt, NULL},
  {_TrackOmni     , NULL},
  {_TrackSpotTgt  , NULL},
  {_TrackSpotSrc  , NULL},
  {_TrackPosition , ReadPositionTrack},
  {_TrackRotation , ReadRotationTrack},
  {_TrackScale    , ReadScaleTrack},
  {_TrackFOV      , ReadFOVTrack},
  {_TrackRoll     , ReadRollTrack},
  {_TrackStatus   , ReadStatusTrack},
  {_NameAndParent , ReadObjectNameAndParent},
  {_DummyName     , ReadDummyName},
  {_PivotPoint    , ReadPivotPoint},
  {_ObjectNumber  , ReadObjectNumber},
  {_Frames        , ReadFrames},
// Material chunks
  {_Material      , NULL},
  {_MaterialName  , ReadMaterialName},
  {_Material2Sides, SetTwoSides},
  {_MaterialType  , ReadShading},
  {_MaterialTransp, SetTransparent},
  {_Texture       , NULL},
  {_MapFile       , ReadMapFile}
};


const DWord Readable_Chunks = sizeof(ChunkData)/sizeof(ChunkData[0]);
Scene *Host;  // All information will be read to this scene
FILE *IN_3DS;     // This is the File handle which will be used
DWord IN_SIZE;    // Total 3DS file size.
DWord Cur_ID;     // Current Object ID
DWord Chk_ID;     // Current Chunk ID.
DWord Cur_Track;  // Current Track Chunk
char *CurObjName;  // Current Object Name.
char ReadTextureToo; // Flag : read the material's map file or not.
Object *Cur_Obj;
TriMesh *Cur_Tri;
Omni *Cur_Omni;
Camera *Cur_Cam;
Material *Cur_Mat;
KeyFrame *Cur_Key,*KeyFramer;

short FindChunk(Chunk Chk)
{
  int I;
  for (I=0;I<Readable_Chunks;I++)
    if (ChunkData[I].ID==Chk.ID) return I;
  return -1;
}


void Read3DSChunk(DWord ChunkEnd,DWord Parent)
{
  short CurChunk;
  Chunk Chk;
  DWord Pos;

  while (FTell()<ChunkEnd)
  {
    Pos=FTell();
    Read(&Chk,sizeof(Chunk));
    if ((Chk.ID==_TrackInfo)      || (Chk.ID==_TrackOmni)    ||
        (Chk.ID==_TrackCameraSrc) || (Chk.ID==_TrackSpotSrc) ||
        (Chk.ID==_TrackCameraTgt) || (Chk.ID==_TrackSpotTgt))
      Cur_Track=Chk.ID;
    if (Chk.Length==0) return;
    CurChunk=FindChunk(Chk);
    if (CurChunk>=0)
    {
      Pos+=Chk.Length;
      Chk_ID=Chk.ID;
      if (ChunkData[CurChunk].Reader)
        ChunkData[CurChunk].Reader(Pos);
      else
        Read3DSChunk(Pos,Chk.ID);
      FSeek(Pos);
    }
    else FSeek(Pos+Chk.Length);
  }
}


char Load_3DS(Scene *Sc,char *FileName)
{
  TriMesh *T;
  if (!Sc) return 0;
  memset(Sc,0,sizeof(Scene));
  Host = Sc;
  IN_3DS = fopen(FileName,"rb");
  if (!IN_3DS) return 0;

  // ok, now we can get going
  fseek(IN_3DS,0,SEEK_END);
  IN_SIZE = ftell(IN_3DS);
  FSeek(0);
  Cur_ID = Chk_ID = 0;
  Read3DSChunk(IN_SIZE,0);

  Hierarchy_Sort(Host);
  // Correct U/V warping
  for (T=Sc->TriMeshHead;T;T=T->Next)
    Compute_UVWrapping_Coordinates(T);
  return 1;
}
