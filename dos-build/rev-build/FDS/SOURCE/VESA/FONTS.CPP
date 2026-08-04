long Basic_FOffs;

Font * LoadAFT(char *FileName)
{
  long I,J,K;
  FILE *F;
  char Binary[8],Nums[8];
  char Num,Mask,BPP;
  char XS,YS;
  char Bfr[156],Jump;
  Font *FFI = new Font;

  F = fopen(FileName,"rb");
  if (!F) {
    printf("AFT Font File %s open failure.\n",FileName);
    exit(-1);
  }
//  printf("± Loading an AFT font file...\n");

  // Header...
  fread(&XS,1,1,F);
  fread(&YS,1,1,F);
  fread(&BPP,1,1,F);
  Jump=8/BPP;

  FFI->X=XS;
  FFI->Y=YS;
  FFI->XY=FFI->X*FFI->Y;
  FFI->BPP=BPP;
  FFI->Ch=(char *)malloc(128*XS*YS);
  FFI->Len=(char *)malloc(128);

  for(K=0;K<128*YS*XS;K+=Jump)
  {
  fread(&Num,1,1,F);
  Mask = 1;
  for(J=0;J<8;J++)
  {
    Binary[J]=(Num&Mask)/Mask;
    Mask*=2;
  }
  for(J=0;J<Jump;J++)
  {
    Mask=1;
    Nums[J]=0;
    for(I=0;I<BPP;I++)
    {
      Nums[J]+=Binary[J*BPP+I]*Mask;
      Mask*=2;
    }
  }
  for(J=0;J<Jump;J++)
    FFI->Ch[K+J]=Nums[J];

  }
  for(I=0;I<128;I++)
  {
    fread(&Bfr,1,1,F);
    FFI->Len[I] = Bfr[0];
  }
  FFI->Len[32]--;
  fclose(F);
  return FFI;
}

long OutTextXY(char *Where,long X,long Y, char *S,char C)
{
  int DX;
  int II,IJ,IK;
  char CP=0,Ch;
  long int PgOffset, POfs, FOfs, FEnd, Hisx;
  char TC[2];
  char CPP = BPP+1>>3;
  char *Ptr,*Pen;
  Font *F = Active_Font;
  if (Y>=YRes-F->Y) return Y;


  if (BPP==8) {TC[0]=0; TC[1]=C;} else {
    TC[0]=0;
    TC[1]=192;
  }

  DX = X;
  PgOffset = YOffs[Y];

  Hisx = F->X;

  while (Ch=*S++)
  {
    FEnd = F->Len[Ch];
    if (Ch==13||Ch==10||DX+FEnd+2>=XRes) {Y+=F->Y+2; PgOffset = YOffs[Y]; DX = Basic_FOffs; if (Y>=YRes-F->Y) return Y;}

    POfs = (PgOffset+DX)*CPP;
    FOfs = Ch*F->XY;
    for (IJ=0;IJ<F->Y;IJ++)
    {
      Ptr = F->Ch+FOfs;
      II=FEnd;
      Pen=Where+POfs;
      while (II--)
      {
//        Where[POfs+II] = TC[F->Ch[FOfs+II]];
        if (TC[*Ptr++]) memset(Pen,C,CPP);
        Pen+=CPP;
      }
      FOfs += Hisx;
      POfs += XRes*CPP;
    }
    DX += FEnd + 2;
  }
  return Y;
}

// Writes string S on [X,Y] using the color C, transparently.
long Write_String_T32(long X,long Y,char *S,DWord C)
{
  int DX;

  char CP=0,Ch;
  DWord TC[2] = {0,C},DW;
  char *Ptr;
  DWord *Pen;
  long CLen,FOfs;
  long I,J;

  Font *F = Active_Font;
  DWord *VP = ((DWord *)VPage) + YOffs[Y],*WP;

  if (Y>=YRes-F->Y) return Y;
  DX = X;

  while (Ch=*S++)
  {
    CLen = F->Len[Ch];
    if (Ch==13||Ch==10||DX+CLen+2>=XRes) {Y+=F->Y+2; VP+=YOffs[F->Y+2]; DX = Basic_FOffs; if (Y>=YRes-F->Y) return Y;}

    WP = VP+DX;
    FOfs = Ch*F->XY;
    for (J=0;J<F->Y;J++)
    {
      Ptr = F->Ch+FOfs;
      I=CLen;
      Pen = WP;
      while (I--)
      {
        if (DW=TC[*Ptr++]) *Pen=DW;
        Pen++;
      }
      WP += YOffs[1];
      FOfs += F->X;
    }
    DX += CLen + 2;
  }
  return Y;
}
