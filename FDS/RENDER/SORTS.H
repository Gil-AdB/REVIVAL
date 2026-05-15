// Sorting stuff
// Operates on `fds::FListEntry`. The radix sort reads `entry.sortKey`
// directly (was: deref through entry.face->SortZ.DW), so each pass
// streams 16-byte contiguous slots instead of pointer-chasing into
// random Face structs.
void Quick_Sorting(long L,long R)
{
  long I,J;
  uint32_t Av;
  fds::FListEntry Swapper;
  I=L;
  J=R;
  Av=FList[(L+R)>>1].sortKey;
  while(I<=J)
  {
    while (FList[I].sortKey<Av) I++;
    while (Av<FList[J].sortKey) J--;
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
int32_t Bytes1[257];
int32_t Bytes2[257];
int32_t Bytes3[257];
int32_t Bytes4[257];

void Radix_Sorting()
{
  uint32_t Z;
  long I;
  fds::FListEntry *F, *FEnd;

  memset(Bytes1,0,257*4);
  memset(Bytes2,0,257*4);
  memset(Bytes3,0,257*4);
  memset(Bytes4,0,257*4);

  for(I=0;I<CAll;I++)
  {
    Z = FList[I].sortKey;
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
    Z=Bytes1[F->sortKey & 0x000000FF]++;
    SList[Z] = *F;
  }

  F = SList;
  FEnd = F+CAll;
  for(;F<FEnd;F++)
  {
    Z=Bytes2[(F->sortKey & 0x0000FF00) >> 8]++;
    FList[Z] = *F;
  }

  F = FList;
  FEnd = F+CAll;
  for(;F<FEnd;F++)
  {
    Z=Bytes3[(F->sortKey & 0x00FF0000) >> 16]++;
    SList[Z] = *F;
  }

  F = SList;
  FEnd = F+CAll;
  for(;F<FEnd;F++)
  {
    Z=Bytes4[F->sortKey >> 24]++;
    FList[Z] = *F;
  }
}

void Radix_SortingASM(fds::FListEntry *f, fds::FListEntry *s, unsigned int n)
{
  Radix_Sorting();
}
