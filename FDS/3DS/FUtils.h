#ifndef _FUTILS_H_
#define _FUTILS_H_

#include "Base/Quaternion.h"
#include "Base/Matrix.h"

void ReadASCIIZ(char **s);
void Read(void *Ptr, DWord Size);
void SwapYZ(Vector *Vec);
void SwapYZ(Quaternion *Q);
void SwapYZ(Matrix Mat);
Object *FindObject(short Number);
Object *FindObject(char *Name);
void FSeek(int32_t Where);
DWord FTell();
void Read(void *Ptr,DWord Size);
void Compute_UVWrapping_Coordinates(TriMesh *T);

#endif