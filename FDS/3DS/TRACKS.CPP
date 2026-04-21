#include "Base/FDS_VARS.H"
#include "Base/FDS_DECS.H"
#include "I_defs.h"
#include "FUtils.h"
#include "Base/SpotLight.h"
#include "Base/Scene.h"

// Low-level Animation Tracks Reader, Format: 3DS ver 4.0
extern FILE *IN_3DS;     // This is the File handle which will be used


void ReadTrackHeader(Word *Keys,Word *Flags)
{
  Read(Flags,10);      // Read 5 flags
  Read(Keys,2);
  Read(&Flags[5],2);   // Read the sixth flag
}

void ReadSplineFlags(float *OutFlags)
{
  int32_t I,J;
  float Temp;
  Word SplineFlags;

  Read(&SplineFlags,2);
  memset(OutFlags,0,20);
  for (I=0,J=1;I<16;I++,J<<=1)
    if (SplineFlags & J)
    {
      Read(&Temp,4);

/*      if (I>4)
        printf("<3DS:Trk>: Unknown s"); // Gives "Unknown spline flag" message
      else
        printf("<3DS:Trk>: S");         // Gives "Spline flag" message
      printf("pline flag %i: %f\n",I,Temp);*/
      if (I<5) OutFlags[I]=Temp;
    }
}

void ReadPositionTrack(unsigned int EndPos)
{
  Word Keys;
  Word Flags[6];
  Word TrackFlag;
  Word i;
  Word Frame;
  float SplineFlags[5];
  Spline Pos;
  Vector Position;

  ReadTrackHeader(&Keys,Flags);
  memset(&Pos,0,sizeof(Spline));
  Pos.NumKeys=Keys;
  Pos.Flags=Flags[0]&0x0003;
  Pos.Keys=new SplineKey[Keys];
  memset(Pos.Keys,0,sizeof(SplineKey)*Keys);
  for (i=0;i<Keys;i++)
  {
    Read(&Frame,2);
    Read(&TrackFlag,2);
    ReadSplineFlags(SplineFlags);
    //Read(&Position,sizeof(Vector));
	Position.Read(IN_3DS);
    SwapYZ(&Position);
    Spline_SetKey_3D(&Pos,&Position,Frame,SplineFlags);
  }
  Spline_Init_3D(&Pos);

  // Apply Position Spline where needed, depending on last Object Read.
  switch (Cur_Track)
  {
    case _TrackInfo: memcpy(&((TriMesh *)Cur_Key->Object->Data)->Pos,&Pos,sizeof(Spline)); break;
    case _TrackOmni: memcpy(&((Omni *)Cur_Key->Object->Data)->Pos,&Pos,sizeof(Spline)); break;
    case _TrackCameraSrc: memcpy(&((Camera *)Cur_Key->Object->Data)->Source,&Pos,sizeof(Spline)); break;
    case _TrackCameraTgt: memcpy(&((Camera *)Cur_Key->Object->Data)->Target,&Pos,sizeof(Spline)); break;
    case _TrackSpotSrc: memcpy(&((SpotLight *)Cur_Key->Object->Data)->Source,&Pos,sizeof(Spline)); break;
    case _TrackSpotTgt: memcpy(&((SpotLight *)Cur_Key->Object->Data)->Target,&Pos,sizeof(Spline)); break;
  }
}

void ReadScaleTrack(unsigned int EndPos)
{
  Word Keys;
  Word Flags[6];
  Word TrackFlag;
  Word i;
  Word Frame;
  float SplineFlags[5];
  Vector Scale;
  TriMesh *T = (TriMesh *)Cur_Key->Object->Data;

  ReadTrackHeader(&Keys,Flags);
  T->Scale.NumKeys=Keys;
  T->Scale.Flags=Flags[0]&0x0003;
  T->Scale.Keys=new SplineKey[Keys];
  memset(T->Scale.Keys,0,sizeof(SplineKey)*Keys);

  for (i=0;i<Keys;i++)
  {
    Read(&Frame,2);
    Read(&TrackFlag,2);
    ReadSplineFlags(SplineFlags);
    //Read(&Scale,sizeof(Vector));
	Scale.Read(IN_3DS);
    SwapYZ(&Scale);
    Spline_SetKey_3D(&T->Scale,&Scale,Frame,SplineFlags);
  }
  Spline_Init_3D(&T->Scale);
}

void ReadRotationTrack(unsigned int EndPos)
{
  Word Keys;
  Word Flags[6];
  Word TrackFlag;
  Word i;
  Word Frame;
  float SplineFlags[5];
  Quaternion Rotation;
  Quaternion Q,P,Prev;
  TriMesh *T = (TriMesh *)Cur_Key->Object->Data;

  ReadTrackHeader(&Keys,Flags);
  T->Rotate.NumKeys=Keys;
  T->Rotate.Flags=Flags[0]&0x0003;
  T->Rotate.Keys=new SplineKey[Keys];
  memset(T->Rotate.Keys,0,sizeof(SplineKey)*Keys);

  Quaternion_Form(&Prev,0,0,0,1);
  for (i=0;i<Keys;i++)
  {
    Read(&Frame,2);
    Read(&TrackFlag,2);
    ReadSplineFlags(SplineFlags);
    Read(&Rotation,sizeof(Quaternion));
    SwapYZ(&Rotation);
    Spline_SetKey_4D(&T->Rotate,&Rotation,Frame,SplineFlags);
  }
  Spline_Init_3D(&T->Rotate);
}

void ReadFOVTrack(unsigned int EndPos)
{
  Word Keys;
  Word Flags[6];
  Word TrackFlag;
  Word i;
  Word Frame;
  float SplineFlags[5];
  float FOV;
  Camera *C = (Camera *)Cur_Key->Object->Data;

  ReadTrackHeader(&Keys,Flags);
  C->FOV.NumKeys = Keys;
  C->FOV.Flags=Flags[0]&0x0003;
  C->FOV.Keys=new SplineKey[Keys];
  memset(C->FOV.Keys,0,sizeof(SplineKey)*Keys);

  for (i=0;i<Keys;i++)
  {
    Read(&Frame,2);
    Read(&TrackFlag,2);
    ReadSplineFlags(SplineFlags);
    Read(&FOV,4);
    Spline_SetKey_1D(&C->FOV,FOV,Frame,SplineFlags);
  }
  Spline_Init_3D(&C->FOV);
}

void ReadRollTrack(unsigned int EndPos)
{
  Word Keys;
  Word Flags[6];
  Word TrackFlag;
  Word i;
  Word Frame;
  float SplineFlags[5];
  float Roll;
  Camera *C = (Camera *)Cur_Key->Object->Data;

  ReadTrackHeader(&Keys,Flags);
  C->Roll.NumKeys=Keys;
  C->Roll.Flags=Flags[0]&0x0003;
  C->Roll.Keys=new SplineKey[Keys];
  memset(C->Roll.Keys,0,sizeof(SplineKey)*Keys);

  for (i=0;i<Keys;i++)
  {
    Read(&Frame,2);
    Read(&TrackFlag,2);
    ReadSplineFlags(SplineFlags);
    Read(&Roll,4);
    Spline_SetKey_1D(&C->Roll,Roll,Frame,SplineFlags);
  }
  Spline_Init_3D(&C->Roll);
}

void ReadStatusTrack(unsigned int EndPos)
{
  Word Keys;
  Word Flags[6];
  Word TrackFlag[2];
  Word i;
  Word Frame;
  unsigned char Status;
  ObjectStatus *OS;
  TriMesh *T;

  ReadTrackHeader(&Keys,Flags);
  Status=HTrack_Visible;
  T=(TriMesh *)(Cur_Key->Object->Data);
  T->Status=new ObjectStatus;
  OS=T->Status;
  memset(OS,0,sizeof(ObjectStatus));
  OS->Stat=Status;
  OS->Frame=0;
  for (i=0;i<Keys;i++)
  {
    Read(&Frame,2);
    Read(TrackFlag,4);

    OS->Next=new ObjectStatus;
    memset(OS->Next,0,sizeof(ObjectStatus));
    OS->Next->Prev=OS;
    OS=OS->Next;
    OS->Stat=Status;
    OS->Frame=Frame;
    Status^=1;
  }
  OS->Next = NULL; // End list.
}

// Toughest routine on reader, try not to get lost
void ReadObjectNameAndParent(unsigned int EndPos)
{
  char *Name;
  signed short  Flags[2];
  signed short  Parent;
  KeyFrame *K;
  Object *Obj;
  TriMesh *T;

  ReadASCIIZ(&Name);

  // New entry for Keyframer list.
  if (!KeyFramer)
  {
    Cur_Key=new KeyFrame;
    KeyFramer=Cur_Key;
    memset(Cur_Key,0,sizeof(KeyFrame));
  }
  else
  {
    Cur_Key->Next=new KeyFrame;
    memset(Cur_Key->Next,0,sizeof(KeyFrame));
    Cur_Key->Next->Prev = Cur_Key;
    Cur_Key=Cur_Key->Next;
  }

  if (strcmp(Name,"$AMBIENT$")==0)
  {
//    printf("<3DS:Key>: Ambient object is meaningless.\n");
    Cur_Key->Object=new Object;
    memset(Cur_Key->Object,0,sizeof(Object));
    strcpy(Cur_Key->Object->Name,"Ambient object");
    Cur_Key->Object->Type=Obj_Ambient;
    Cur_Key->Object->Number=Cur_ID++;
  }
  if (strcmp(Name,"$$$DUMMY")==0)
  {
//    printf("<3DS:Key>: Dummy object detected.\n");
    Cur_Key->Object=new Object;
    Obj = Cur_Key->Object;
    memset(Obj,0,sizeof(Object));
    Obj->Type=Obj_Dummy;
    Obj->Number=Cur_ID++;

    T = (TriMesh *)getAlignedBlock(sizeof(TriMesh), 16);
    Obj->Data=(void *)T;
    memset(T,0,sizeof(TriMesh));
    Obj->Rot=&T->RotMat;
    Obj->Pos=&T->IPos;
    // Insert to World List
    Cur_Obj->Next = Obj;
    Obj->Prev = Cur_Obj;
    Cur_Tri->Next = T;
    T->Prev = Cur_Tri;
  }
  if (strcmp(Name,"$$$DUMMY") && strcmp(Name,"$AMBIENT"))
  {
    Cur_Key->Object=FindObject(Name);
    Cur_Key->Object->Number=Cur_ID++;
  }
  Read(&Flags,4);
  Read(&Parent,2);
/*  if (Cur_Key->Object) printf("<3DS:Hrc>: Object %s [ID=%d]: Parent ID %d.\n",Cur_Key->Object->Name,Cur_Key->Object->Number,Parent);
    else return;*/
  if (!Cur_Key->Object) return;//this means the reader is having trouble
  if (Parent!=-1)
  {
    Cur_Key->Object->Parent=NULL;
    for (K=KeyFramer;K;K=K->Next)
      if (K->Object->Number==Parent)
      {
        Cur_Key->Object->Parent=K->Object;
//        printf("<3DS:Hrc>: Found matching Parent:%s.\n",K->Object->Name);
        break;
      }
  }
  else Cur_Key->Object->Parent=NULL;
}

void ReadDummyName(unsigned int EndPos)
{
  ReadASCIIZ(&Cur_Key->Object->Name);
//  printf("<3DS:Dum>: Dummy object name: %s\n",Cur_Key->Object->Name);
}

void ReadPivotPoint(unsigned int EndPos)
{
  Vector Pivot;
  TriMesh *T = (TriMesh *)Cur_Key->Object->Data;
  Vertex *Vtx = T->Verts ,*VE = Vtx + T->VIndex;

  //Read(&Pivot,sizeof(Vector));
  Pivot.Read(IN_3DS);
  SwapYZ(&Pivot);
  for (;Vtx<VE;Vtx++)
    Vector_SelfSub(&Vtx->Pos,&Pivot);
}

void ReadObjectNumber(unsigned int EndPos)
{
  Read(&Cur_ID,2);
//  printf("<3DS:ID>: Read an ID of %d\n",Cur_ID);
}

void ReadFrames(unsigned int EndPos)
{
  DWord SF,EF;
  Read(&SF,4);
  Read(&EF,4);
  Host->StartFrame=float(SF);
  Host->EndFrame= float(EF);
}