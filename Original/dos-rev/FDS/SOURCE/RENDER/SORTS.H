// Sorting stuff
void Quick_Sorting(long L,long R)
{
  long I,J;
//  float Av;
  long Av;
  Face *Swapper;
  I=L;
  J=R;
  Av=FList[L+R>>1]->SortZ.DW;
  while(I<=J)
  {
    while (FList[I]->SortZ.DW<Av) I++;
    while (Av<FList[J]->SortZ.DW) J--;
    if (I<=J) {
      Swapper=FList[I];
      FList[I]=FList[J];
      FList[J]=Swapper;
      I++;
      J--;
    }
  }
  if (L<J) Quick_Sorting(L,J);
  if (I<R) Quick_Sorting(I,R);
}


// <..<.*.>..> Radix_256

// <*The Radix*>
long Bytes1[257];
long Bytes2[257];
long Bytes3[257];
long Bytes4[257];

void Radix_Sorting()
{
  unsigned long Z;
//  char *ZC1 = (char *)(&Z),*ZC2 = ZC1+1,*ZC3 = ZC2+1,*ZC4 = ZC3+1;
  long I;
  long *P;
  Face **F,**FEnd;

  memset(Bytes1,0,257*4);
  memset(Bytes2,0,257*4);
  memset(Bytes3,0,257*4);
  memset(Bytes4,0,257*4);

  for(I=0;I<CAll;I++)
  {
    Z = FList[I]->SortZ.DW;
    Bytes1[ (Z & 0x000000FF)+1]++;
    Bytes2[((Z & 0x0000FF00)>>8)+1]++;
    Bytes3[((Z & 0x00FF0000)>>16)+1]++;
    Bytes4[ (Z >> 24)+1]++;
  }

  for(I=1;I<256;I++)
  {
    Bytes1[I]+=Bytes1[I-1];
    Bytes2[I]+=Bytes2[I-1];
    Bytes3[I]+=Bytes3[I-1];
    Bytes4[I]+=Bytes4[I-1];
  }

  F = FList;
  FEnd = F+CAll;
  for(;F<FEnd;F++)
  {
    Z=Bytes1[(*F)->SortZ.DW & 0x000000FF]++;
    SList[Z] = *F;
  }

  F = SList;
  FEnd = F+CAll;
  for(;F<FEnd;F++)
  {
    Z=Bytes2[((*F)->SortZ.DW & 0x0000FF00) >> 8]++;
    FList[Z] = *F;
  }

  F = FList;
  FEnd = F+CAll;
  for(;F<FEnd;F++)
  {
    Z=Bytes3[((*F)->SortZ.DW & 0x00FF0000) >> 16]++;
    SList[Z] = *F;
  }

  F = SList;
  FEnd = F+CAll;
  for(;F<FEnd;F++)
  {
    Z=Bytes4[(*F)->SortZ.DW >> 24]++;
    FList[Z] = *F;
  }
}

extern "C" void Radix_Asm(Face **f,Face **s,unsigned int n);
#pragma aux Radix_Asm parm [esi] [edx] [ebx] modify [eax ebx ecx edx edi esi ebp];

void Radix_SortingASM(Face **f,Face **s,unsigned int n)
{
	Radix_Asm(f,s,n);
}