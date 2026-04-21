#include <Mem.H>
#include <Math.H>
#include <StdLib.H>
#include <DOS.H>
#include <StdIO.H>


#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"

TriMesh *V3D_Object;
FILE *V3D_Mesh;

#include "Objects.H"

char Load_V3D(Scene *Sc,char *FileName)
{
  long LPts,LPolys;
  long A,B,C,I;
  signed short Ch;

  V3D_Object=NULL;
  if (!(V3D_Mesh=fopen(FileName,"rb")))
  {
    fclose(V3D_Mesh);
    return 0;
  }

  fread(&LPts,4,1,V3D_Mesh);
  fread(&LPolys,4,1,V3D_Mesh);

  #ifdef DebugInfo
    printf("°  (%d Vertices,%d Polygons) \n",LPts,LPolys);
    getchar();
  #endif

  V3D_Object=new TriMesh;
  V3D_Object->VIndex=LPts;
  V3D_Object->FIndex=LPolys;
  V3D_Object->Verts=new Vertex[LPts];
  V3D_Object->Faces=new Face[LPolys];

  for(I=0;I<LPts;I++)
  {
    // Y and Z SWAPPED!!
    fread(&V3D_Object->Verts[I].Pos.X,4,1,V3D_Mesh);
    fread(&V3D_Object->Verts[I].Pos.Z,4,1,V3D_Mesh);
    fread(&V3D_Object->Verts[I].Pos.Y,4,1,V3D_Mesh);
  }
  for(I=0;I<LPolys;I++)
  {
    fread(&A,4,1,V3D_Mesh);
    fread(&B,4,1,V3D_Mesh);
    fread(&C,4,1,V3D_Mesh);
    V3D_Object->Faces[I].A = (Vertex *)(V3D_Object->Verts+A);
    V3D_Object->Faces[I].B = (Vertex *)(V3D_Object->Verts+B);
    V3D_Object->Faces[I].C = (Vertex *)(V3D_Object->Verts+C);
    V3D_Object->Faces[I].Txtr = &Default_Mat;
  }
  for(I=0;I<LPts;I++)
  {
    fread(&Ch,2,1,V3D_Mesh);
    V3D_Object->Verts[I].U=Ch;

    fread(&Ch,2,1,V3D_Mesh);
    V3D_Object->Verts[I].V=Ch;
  }
  fclose(V3D_Mesh);

  Insert_TriMesh2Scene(Sc,V3D_Object);

//  Scene_Computations(Sc);
  return 1;
}
