#include <Base/FDS_VARS.H>
#include "I_defs.h"
#include <Base/FDS_DECS.H>
#include <Base/Scene.h>

int32_t Temp;
Object *Obj_Temp,*Obj_Ptr;
extern FILE *IN_3DS;     // This is the File handle which will be used

void FSeek(int32_t Where)
{
  fseek(IN_3DS,Where,SEEK_SET);
}

DWord FTell()
{
  return ftell(IN_3DS);
}

void Read(void *Ptr,DWord Size)
{
  fread(Ptr,1,Size,IN_3DS);
}

void ReadASCIIZ(char **s)
{
  int a=0;
  char Temp[256];

  do Read(Temp+a,1); while (Temp[a++]);
  *s=strdup(Temp);
}


void SwapYZ(Vector *Vec)
{
  float e;

  e=Vec->y;
  Vec->y=Vec->z;
  Vec->z=e;
}

void SwapYZ(Quaternion *Q)
{
  float e;

  e=Q->y;
  Q->y=Q->z;
  Q->z=e;
}

void SwapYZ(Matrix Mat)
{
  Matrix_Form(Mat,Mat[0][0],Mat[0][2],Mat[0][1],
                  Mat[2][0],Mat[2][2],Mat[2][1],
                  Mat[1][0],Mat[1][2],Mat[1][1]);
}

Object *FindObject(short Number)
{
  Object *Temp;

  for (Temp=Host->ObjectHead;Temp;Temp=Temp->Next)
    if (Temp->Number==Number) return Temp;
  return NULL;
}

Object *FindObject(char *Name)
{
  Object *Temp;

  for (Temp=Host->ObjectHead;Temp;Temp=Temp->Next)
    if (!strcmp(Temp->Name,Name)) return Temp;
  return NULL;
}

/*int NumOfParents(Object *O)
{
  Object *Temp=O;
  int i=0;
  while (Temp->Parent)
  {
    Temp=Temp->Parent;
    i++;
  }
  return i;
}

// Sorts all object in scene by their hierarchial position in the scene
// Note: This procedure is very ZOL! which means, EXTREMELY unefficient!
//       But who cares? (As int32_t as it is not done in real-time... ;)
void Hierarchy_Sort(Scene *Sc)
{
  Object *OldList=Sc->ObjectHead;
  Object *NewList,*NewHead=NULL;
  Object *O,*O2;
  int i=0,f=1;
  while (f)
  {
    f=0;
    for (O=OldList;O;O=O->Next)
      if (NumOfParents(O)==i)
      {
        f=1;
        if (!NewHead)
        {
          NewHead=new Object;
          NewList=NewHead;
          memcpy(NewList,O,sizeof(Object));
          NewList->Prev=NewList->Next=NULL;
        }
        else
        {
          NewList->Next=new Object;
          NewList->Next->Prev=NewList;
          memcpy(NewList->Next,O,sizeof(Object));
          NewList=NewList->Next;
        }
        for (O2=OldList;O2;O2=O2->Next)
          if (O==O2->Parent) O2->Parent=NewList;
      }
    i++;
  }
  while (OldList)
  {
    O=OldList;
    OldList=O->Next;
		delete O;
	}
	Sc->ObjectHead=NewHead;

}
*/



void Compute_UVWrapping_Coordinates(TriMesh *T)
{
/*	Face *F,*FEnd = T->Faces+T->FIndex;
  //float U1,V1,U2,V2,U3,V3;
  float m,M,F1;

  //What this thing should REALLY do,is check the validity of the
  //Flag. If warp is necessary,then
  //Recalculate Face mapping coordinates using UV wrapping.
  for(F=T->Faces;F<FEnd;F++)
	{
    F->U1 = F->A->U; F->V1 = F->A->V;
    F->U2 = F->B->U; F->V2 = F->B->V;
    F->U3 = F->C->U; F->V3 = F->C->V;

    // Fix U Wrapping
    if (F->Flags&Face_UWrap)
    {
			m = F->U1; M = F->U1;
      if (F->U2<m) m=F->U2; if (F->U2>M) M=F->U2;
      if (F->U3<m) m=F->U3; if (F->U3>M) M=F->U3;

      if (M-m>UVFlip*256)
      {
        F1 = ceil((M-m)/256)*256;
        if (F->U1<128) F->U1+=F1;
        if (F->U2<128) F->U2+=F1;
        if (F->U3<128) F->U3+=F1;
      } else F->Flags&=0xFFFF-Face_UWrap;
		}

    // Fix V Wrapping
    if (F->Flags&Face_VWrap)
    {
      m = F->V1; M = F->V1;
      if (F->V2<m) m=F->V2; if (F->V2>M) M=F->V2;
      if (F->V3<m) m=F->V3; if (F->V3>M) M=F->V3;

      if (M-m>UVFlip*256)
			{
				F1 = ceil((M-m)/256)*256;
				if (F->V1<128) F->V1+=F1;
				if (F->V2<128) F->V2+=F1;
				if (F->V3<128) F->V3+=F1;
			} else F->Flags&=0xFFFF-Face_VWrap;
		}
	}*/
}
