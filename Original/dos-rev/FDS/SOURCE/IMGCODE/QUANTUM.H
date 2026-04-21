// Silvatar's DAC->Pal Quantitizer. not Functional, Optimized for Space,
// and has poor Time performance.
// Upgrade next: Time Optimize, MultiImage Quantitize. (Structs??)

struct Quantum_Node
{
  long Count;
  QColor *Col;
  Quantum_Node *Next[8];
};

Quantum_Node *Root;
long LNum;
long Max_Count;
Quantum_Node *Cut_This;
Quantum_Node **QList;
long **PalEnum;
Palette *UPal;
QColor *UCol;

long Col;
long LCntr;
long LJmp;

long PalsNum;

void Init_Tree()
{
  Root = new Quantum_Node;
  memset(Root,0,sizeof(Quantum_Node));
}

void Texture2QTree(char *Txtr,char TPP)
{
  char CP;
  void (* Read)(DWord Src,QColor *C);
  QColor Cl;
  char *Ptr = Txtr;
  char RB,GB,BB,N;
  Quantum_Node *QN;
  unsigned char Mask;
  signed char Shr;
  long I,J;

  switch (TPP)
  {
    case 15: CP=2; Read=&IMGExtract_15Bit; break;
    case 16: CP=2; Read=&IMGExtract_16Bit; break;
    case 24: CP=3; Read=&IMGExtract_24Bit; break;
    case 32: CP=4; Read=&IMGExtract_32Bit; break;
  }

  for(I=0;I<65536;I++)
  {
    Read(*((DWord *)Ptr),&Cl);
    Ptr+=CP;
    QN = Root;
    Mask = 128;
    Shr = 7;
    while (Shr>1) //Two last bits are ignored,Palette has 6-bit accuracy.
    {
      RB = (Cl.R&Mask)>>Shr;
      GB = (Cl.G&Mask)>>Shr;
      BB = (Cl.B&Mask)>>Shr;

      N = (RB<<2)+(GB<<1)+BB;
      QN->Count++;

      if (!QN->Next[N])
      {
        QN->Next[N] = new Quantum_Node;
        memset(QN->Next[N],0,sizeof(Quantum_Node));
      }
      QN=QN->Next[N];

      Mask >>= 1;
      Shr--;
    }
  }
}

void Count_Leaves(Quantum_Node *QN)
{
  long I;
  char Flag = 1;
  for(I=0;I<8;I++)
    if (QN->Next[I]) {Flag = 0; Count_Leaves(QN->Next[I]);}
  LNum+=Flag;
}

void Count_Nonleaves(Quantum_Node *QN)
{
  long I;
  char Flag = 0;
  for(I=0;I<8;I++)
    if (QN->Next[I]) {Flag = 1; Count_Nonleaves(QN->Next[I]);}
  LNum+=Flag;
}

void QTree2QList(Quantum_Node *QN)
{
  long I;
  char Flag = 0;
  for(I=0;I<8;I++)
    if (QN->Next[I]) {Flag = 1; QTree2QList(QN->Next[I]);}
  if (Flag)
  {
    QList[LJmp] = QN;
    LJmp++;
  }
}

void Quant_Sorting(long L,long R)
{
  long I,J;
  float Av;
  Quantum_Node *Swapper;
  I=L;
  J=R;
  Av=QList[L+R>>1]->Count;
  while(I<=J)
  {
    while (QList[I]->Count<Av) I++;
    while (Av<QList[J]->Count) J--;
    if (I<=J) {
      Swapper=QList[I];
      QList[I]=QList[J];
      QList[J]=Swapper;
      I++;
      J--;
    }
  }
  if (L<J) Quant_Sorting(L,J);
  if (I<R) Quant_Sorting(I,R);
}


/*void Reduce_Tree(long Limit)
{
  long I,J;

  LNum=1;
  Count_NonLeaves(Root);
  QList = new Quantum_Node *[LNum];
  QList[0] = Root;
  LNum=1;
  QTree2QList(Root);
  printf("** %d Nodes...Sorting\n",LNum);
  Quant_Sorting(0,LNum-1);
  printf("** Sort complete\n");

  LNum=0;
  Count_Leaves(Root);
  LCntr=LNum;
  printf("** Total leaves = %d\n",LNum);
  cprintf("** Reducing [32]");
  LJmp=LCntr>>5;
  J=0;
  while (LNum>256)
  {
    while (!QList[J++]);
    Cut_This = QList[J];

    for(I=0;I<8;I++)
      if (Cut_This->Next[I])
      {
        LNum--;
        delete Cut_This->Next[I];
        Cut_This->Next[I]=NULL;
      }

    LNum++;
    if (LNum<LCntr) {LCntr-=LJmp; cprintf(".");}
  }
  printf("\n");
}*/

void Reduce_QTree(long Limit)
{
  long I,J;
  Quantum_Node *QN;
  LNum=0;
  Count_Nonleaves(Root);
  QList = new Quantum_Node * [LNum];
  LJmp = 0;
  QTree2QList(Root);
  Quant_Sorting(0,LNum-1);
  LNum=0;
  Count_Leaves(Root);

  J=0;
  while (LNum>Limit)
  {
    while (!QList[J++]);

    QN = QList[J];
    LNum++;
    for(I=0;I<8;I++)
      if(QN->Next[I])
      {
        LNum--;
        delete QN->Next[I];
        QN->Next[I]=NULL;
      }
  }
}


/*void QTree_Palette(char Level,Quantum_Node *QN,Palette *Pal,long R,long G,long B)
{
  char F=1,NR,NG,NB;
  long I;

  for(I=0;I<8;I++)
  {
    NR=R+((I>>2)<<(5-Level));
    NG=G+(((I>>1)&1)<<(5-Level));
    NB=B+((I&1)<<(5-Level));
    if (QN->Next[I]) {F=0; QTree_Palette(Level+1,QN->Next[I],Pal,NR,NG,NB);}
  }
  if (F)   // leaf detected
    if (Col<256)
    {
      Pal->C[Col].R = R;
      Pal->C[Col].G = G;
      Pal->C[Col].B = B;
      QPal[Col]=QN;
      Col++;
    } else Col++;
}*/

void Reconstruct_QImage(Texture *Src,char *Dest)
{
  long I,K;
  char J;

  char Dif;
  short M;
  QColor Cl;
  char *P1,*P2;
  char RR;
  void (* Read)(DWord Src,QColor *C);
  char RB,GB,BB,N;
  char Mask;
  signed char Shr;
  Quantum_Node *QN;
  char F;

  P1=Src->Data;
  P2=Dest;

  RR=Src->BPP+1>>3;
  switch (Src->BPP)
  {
    case 15: Read=&IMGExtract_15Bit; break;
    case 16: Read=&IMGExtract_16Bit; break;
    case 24: Read=&IMGExtract_24Bit; break;
    case 32: Read=&IMGExtract_32Bit; break;
  }


  for(I=0;I<65536;I++,P1+=RR,P2++)
  {
    Read(*((DWord *)P1),&Cl);

    Mask = 128;
    Shr = 7;
    QN = Root;
    do
    {
      RB=(Cl.R&Mask)>>Shr;
      GB=(Cl.G&Mask)>>Shr;
      BB=(Cl.B&Mask)>>Shr;
      N = (RB<<2)+(GB<<1)+BB;
      if (!QN->Next[N]) F=QN->Col-UPal->C;
      QN=QN->Next[N];
      Mask>>=1;
      Shr--;
    } while (QN);

    *P2=F;
  }
}

void Delete_QTree(Quantum_Node *QN)
{
  long I;
  for(I=0;I<8;I++)
    if (QN->Next[I]) Delete_QTree(QN->Next[I]);

  delete QN;
}

void Evaluate_Palettes(Scene *Sc)
{
  long I,J;
  Material *M;
  Palette *Pal;
  long **IEnum,*CEnum;

  // Count Textured Materials on Group.
  I=0;
  for(M=MatLib;M;M=M->Next) if (M->RelScene==Sc&&M->Txtr) I++;

  // Allocate and clear space for Palette enumerator.
  PalEnum = new long * [I];
  for(J=0;J<I;J++)
  {
    PalEnum[J] = new long[256];
    memset(PalEnum[J],0,256*sizeof(long));
  }
  PalsNum = I; // for eventual deletion.

  // Enumerate All textures
  IEnum = PalEnum;
  for(M=MatLib;M;M=M->Next)
  {
    if (M->RelScene!=Sc) continue;
    if (!M->Txtr) continue;
    CEnum = *IEnum;
    for(I=0;I<65536;I++)
      CEnum[M->Txtr->Data[I]]++;
    // Magnefize "importancy" of flare colors due to the annoying tendency
    // the Quantum unit has to ignore them.
    if (M->Flags&Mat_Virtual)
      for(I=0;I<256;I++)
        CEnum[I]*=3;
    IEnum++;
  }
}

void Palettes2QTree(Scene *Sc)
{
  Quantum_Node *QN;

  Material *M;
  Palette *Pal;
  QColor C;
  char N,RB,GB,BB;
  long I,J,K;
  long **IEnum,*CEnum,Val;
  char Mask,Shr;

  IEnum = PalEnum;
  for(M=MatLib;M;M=M->Next)
  {
    if (M->RelScene!=Sc) continue;
    if (!M->Txtr) continue; // *sigh* - not again

    CEnum = *IEnum;
    Pal = M->Txtr->Pal;
//      printf("-----------\n");
    for(I=0;I<256;I++)
    {
      C.R = Pal->C[I].R;
      C.G = Pal->C[I].G;
      C.B = Pal->C[I].B;
      Val = CEnum[I];
      if (!Val) continue;//{printf("Nil Entry at %d\n",I); continue;}
      Mask=32;
      Shr=5;
      QN = Root;
      while (Mask)
      {
        QN->Count += Val;
        RB = (C.R&Mask)>>Shr;
        GB = (C.G&Mask)>>Shr;
        BB = (C.B&Mask)>>Shr;
        N = (RB<<2)+(GB<<1)+BB;
        if (!QN->Next[N])
        {
          QN->Next[N] = new Quantum_Node;
          memset(QN->Next[N],0,sizeof(Quantum_Node));
        }
        QN = QN->Next[N];

        Mask>>=1;
        Shr--;
      }
      QN->Count += Val;
    }

    IEnum++;
  }
}

void Seek_Leaves(Quantum_Node *QN,char R,char G,char B,char L)
{
  char Flag = 1;
  char HL = L>>1;
  if(QN->Next[0]) {Flag = 0; Seek_Leaves(QN->Next[0],R  ,G  ,B  ,HL);}
  if(QN->Next[1]) {Flag = 0; Seek_Leaves(QN->Next[1],R  ,G  ,B+L,HL);}
  if(QN->Next[2]) {Flag = 0; Seek_Leaves(QN->Next[2],R  ,G+L,B  ,HL);}
  if(QN->Next[3]) {Flag = 0; Seek_Leaves(QN->Next[3],R  ,G+L,B+L,HL);}
  if(QN->Next[4]) {Flag = 0; Seek_Leaves(QN->Next[4],R+L,G  ,B  ,HL);}
  if(QN->Next[5]) {Flag = 0; Seek_Leaves(QN->Next[5],R+L,G  ,B+L,HL);}
  if(QN->Next[6]) {Flag = 0; Seek_Leaves(QN->Next[6],R+L,G+L,B  ,HL);}
  if(QN->Next[7]) {Flag = 0; Seek_Leaves(QN->Next[7],R+L,G+L,B+L,HL);}
  if (Flag)
  {
#ifdef DebugStructs
   printf("Color %d set, (%d,%d,%d)\n",UCol-UPal->C,R,G,B);
#endif
     UCol->R = R;
     UCol->G = G;
     UCol->B = B;
     QN->Col = UCol;
     UCol++;
  }
}

// Goes into UPal
void The_Final_Palette()
{
  UPal = new Palette;
  UCol = UPal->C;
  Seek_Leaves(Root,0,0,0,32);
}

void Remap_Textures(Scene *Sc)
{
  Material *M;
  unsigned char T_Vector[256];
  Palette *Pal;
  QColor C;
  char Mask,Shr,Out;
  char N,RB,GB,BB;
  long I;
  Quantum_Node *QN;
  char *Pen;

  for(M=MatLib;M;M=M->Next)
  {
    if (M->RelScene!=Sc) continue;
    if (!M->Txtr) continue; // *sigh* - not again

    Pal = M->Txtr->Pal;
    for(I=0;I<256;I++)
    {
      C.R = Pal->C[I].R;
      C.G = Pal->C[I].G;
      C.B = Pal->C[I].B;
      Mask=32;
      Shr=5;
      Out=1;
      QN = Root;
      while (Out)
      {
        RB = (C.R&Mask)>>Shr;
        GB = (C.G&Mask)>>Shr;
        BB = (C.B&Mask)>>Shr;
        N = (RB<<2)+(GB<<1)+BB;
        if (!QN->Next[N])
        {
          T_Vector[I]=QN->Col - UPal->C;
          Out = 0;
        }
        else
          QN = QN->Next[N];

        Mask>>=1;
        Shr--;
      }
    }
//      for(I=0;I<256;I++)
//          printf("Translation Vector [%d]=%d\n  Source (%d,%d,%d) Target (%d,%d,%d) Error [%d]\n",I,T_Vector[I],M->Txtr.Pal->C[I].R,M->Txtr.Pal->C[I].G,M->Txtr.Pal->C[I].B,UPal->C[T_Vector[I]].R,UPal->C[T_Vector[I]].G,UPal->C[T_Vector[I]].B,
  //            abs(M->Txtr.Pal->C[I].R-UPal->C[T_Vector[I]].R)+abs(M->Txtr.Pal->C[I].G-UPal->C[T_Vector[I]].G)+abs(M->Txtr.Pal->C[I].B-UPal->C[T_Vector[I]].B));
    // Convert Textures to new Palette
    Pen = M->Txtr->Data;
    for(I=0;I<65536;I++)
      *Pen++=T_Vector[*Pen];
    // Update Palette
    memcpy(M->Txtr->Pal,UPal,sizeof(Palette));
  }
}

void Delete_Enums()
{
  long I;
  for(I=0;I<PalsNum;I++)
    delete PalEnum[I];
  delete PalEnum;
}

void Universal_Palette(Scene *Sc)
{
  Material *M;
  long ST;
  char TexturesOK = 1;

  for(M=MatLib;M;M=M->Next)
  {
    if (M->RelScene!=Sc) continue;
    if (M->Txtr&&M->Txtr->BPP!=8) TexturesOK = 0;
  }

  if (!TexturesOK)
  {
    printf("[PQ] Critical Error: At least one texture is not Palettized (8BPP)\n");
    return;
  }

//  Install_8();
//  Set_Rate(Rate_Div/100.0);
  printf("<<Uniting Palettes>>\n");
//  getchar();
  ST = Timer;
  printf("[PQ] Doing Octal Tree Quantization, Please wait...\n");
  printf("[PQ] Octal Tree->Initialize\n");
  Init_Tree();
  printf("[PQ] Palettes->Weight Evaluation\n");
  Evaluate_Palettes(Sc);
  printf("[PQ] Palettes->Octal Tree\n");
  Palettes2QTree(Sc);
  printf("[PQ] Octal Tree->Reduction (256)\n");
  Reduce_QTree(256);
  delete QList;
  printf("[PQ] United Palette->Construction\n");
  The_Final_Palette();
  printf("[PQ] Textures->Remapping\n");
  Remap_Textures(Sc);
  // Delete all of those Enums and The Quantum Tree...
  Delete_QTree(Root);
  Delete_Enums();
  delete UPal;
  printf("[PQ] Quant. Time = %dcs",Timer-ST);
//  Restore_8();
//  Set_Rate(Basic_Rate);
//  getchar();
}

void Quantitize(Texture *Tx)
{
  long ST1,ST2;
  char *Buffer;
  ST1=Timer;
  printf("Doing Octal tree Quantization... Please wait\n");
  printf("* Octal Tree->Initialize\n");
  Init_Tree();
  printf("* Image->Octal Tree\n");
  Texture2QTree(Tx->Data,Tx->BPP);
  printf("* Octal Tree->Reduction\n");
  Reduce_QTree(256);
  delete QList;
  printf("* Octal Tree->Palette\n");
  The_Final_Palette();
  Tx->Pal = UPal;
  Buffer = new char[65536];
  printf("* Image->Palette\n");
  Reconstruct_QImage(Tx,Buffer);
  delete Tx->Data;
  Tx->Data = Buffer;
  Tx->BPP=8;
  Delete_QTree(Root);
  ST2=Timer;
  printf("Quant. Time = %d\n",ST2-ST1);
//  while(!Keypressed());
//  Restore_8();
//  Restore_9();
//  exit(1);
}
