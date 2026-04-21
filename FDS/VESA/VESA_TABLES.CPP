#include "Vesa.h"

// Available tables: Y Offset table
// Next: 8Bit Palette tables (Gouraud-txtr,Transparence,Addative,DAC Emu)
// Next: 15/16Bit Diffuse tables (Gouraud-txtr)
// SLOW

int32_t *YOffs = NULL;
char *GrdTbl = NULL;
char *ATable = NULL,*STable = NULL,*CTable = NULL,*TTable = NULL;
char *Tblmem = NULL,*Tblali = NULL;

void Build_YOffs_Table(VESA_Surface *VS)
{
  int32_t I;
  if (VS->YTable) delete [] VS->YTable; //DPMI_Delete(VS->YTable);
  VS->YTable = new int32_t [VS->Y];//(int32_t *)DPMI_New(VESAModule,VS->Y<<2,"Y Offset table");
  for(I=0;I<VS->Y;I++)
    VS->YTable[I] = I*VS->X;
}

// Maps an approximation to a Linear palette inside *Pal to GrdTbl.
// Doesn't Allocate memory for GrdTbl, it could be useful to allocate
// 256-aligned memory for it,since then we can put address in eax
// and modify al at will
void Make_Gouraud_8(char R0,char G0,char B0,char R1,char G1,char B1)
{
  char R,G,B;
  int32_t I,J;
  int32_t M,K,E;
  char *Pal = Screen->Pal;
  if (BPP!=8) return;
  for(I=0;I<64;I++)
  {
    J=63-I;
    R=(R0*J+R1*I)>>6;
    G=(G0*J+G1*I)>>6;
    B=(B0*J+B1*I)>>6;
    M=192;
    Pal = Screen->Pal;
    for(J=0;J<256;J++)
    {
      //E = abs((*Pal++)-R)+abs((*Pal++)-G)+abs((*Pal++)-B);
	  E = abs((*(Pal)) - R) + abs((*(Pal+1)) - G) + abs((*(Pal+2)) - B);
	  Pal += 3;
	  if (E<M) {M=E; K=J;}
    }
    GrdTbl[I]=K;
  }
}

//Maps an approximation to Addative Transparency under Palette *Pal to ATable
void Make_Addative()
{
  char R,G,B;
  int32_t I,J,L;
  int32_t M,K,E;
  char *Pal = Screen->Pal,*Pal1,*Pal2;
  if (BPP!=8) return;
  printf("Addative Table: ");
  Pal1 = Pal;
  for(I=0;I<256;I++)
  {
    Pal2 = Pal1;
    for(J=I;J<256;J++)
    {
      R=(*Pal1)+(*Pal2++);
      if (R>63) R=63;
      G=(*(Pal1+1))+(*Pal2++);
      if (G>63) G=63;
      B=(*(Pal1+2))+(*Pal2++);
      if (B>63) B=63;

      M=192;
      Pal = Screen->Pal;
      for(K=0;K<256;K++)
      {
        //E = abs((*Pal++)-R)+abs((*Pal++)-G)+abs((*Pal++)-B);
		E = abs((*Pal) - R) + abs((*(Pal+1)) - G) + abs((*(Pal+2)) - B);
		Pal += 3;
		if (E<M) {M=E; L=K;}
      }
      ATable[J+(I<<8)]=L;
    }
    if (!(I&0x7)) printf("+");
    Pal1+=3;
  }
  for(I=0;I<256;I++)
    for(J=0;J<I;J++)
      ATable[J+(I<<8)]=ATable[I+(J<<8)];
  printf("...Complete.\n");
}

// Maps an approximation to a 15Bit DAC using the 8Bit palette *Pal to CTable
void Make_ColorMap()
{
  char R,G,B;
  int32_t I,J,L,N;
  int32_t M,K,E;
  char *Pal;
  if (BPP!=8) return;
  printf("Color(DAC->Pal) Table: ");
  for(I=0;I<32;I++)
  {
    for(J=0;J<32;J++)
      for(K=0;K<32;K++)
      {
        R=I<<1;
        G=J<<1;
        B=K<<1;

        M=192;
        Pal = Screen->Pal;
        for(L=0;L<256;L++)
        {
          // E = abs((*Pal++)-R)+abs((*Pal++)-G)+abs((*Pal++)-B);
		  E = abs((*Pal) - R) + abs((*(Pal + 1)) - G) + abs((*(Pal + 2)) - B);
		  Pal += 3;
		  if (E<M) {M=E; N=L;}
        }
        CTable[K+(J<<5)+(I<<10)]=N;
      }
    printf("+");
  }
  printf("...Complete.\n");
}

// Maps an approximation to Gouraud values over the palette.
// Dark is the minimal "light" mapped,and Bright is the maximal one.
// The Gouraud value are Quantitized to increments of 1/64.
void Make_Shade(float Dark,float Bright)
{
  char R,G,B;
  int32_t I,J,K;
  int32_t M,E,L;
  char *Pal=Screen->Pal,*Pal1;
  float LMul = Dark,dL = (Bright-Dark)*0.015625;
  if (BPP!=8) return;
  printf("Shade Table: ");
  if (Dark<0||Bright<0) return;

  for(I=0;I<64;I++)
  {
    Pal1=Pal;
    for(J=0;J<256;J++)
    {
      R=(*Pal1++)*LMul;
      if (R>63) R=63;
      G=(*Pal1++)*LMul;
      if (G>63) G=63;
      B=(*Pal1++)*LMul;
      if (B>63) B=63;

      M=192;
      Pal=Screen->Pal;
      for(K=0;K<256;K++)
      {
        //E = abs((*Pal++)-R)+abs((*Pal++)-G)+abs((*Pal++)-B);
		  E = abs((*Pal) - R) + abs((*(Pal + 1)) - G) + abs((*(Pal + 2)) - B);
		  Pal += 3;
		  if (E<M) {M=E; L=K;}
      }
      STable[J+(I<<8)]=L;
    }
    LMul+=dL;
    if (!(I&1)) printf("+");
  }
  printf("...Complete.\n");
}


//Maps an approximation to Transparency under Palette *Pal to ATable
void Make_Transparence()
{
  char R,G,B;
  int32_t I,J,L;
  int32_t M,K,E;
  char *Pal = Screen->Pal,*Pal1,*Pal2;
  if (BPP!=8) return;
  printf("Transparency Table: ");
  for(I=0;I<256;I++)
    TTable[I+(I<<8)]=I;

  Pal1 = Screen->Pal;
  for(I=0;I<256;I++)
  {
    Pal2 = Pal1+3;
    for(J=I+1;J<256;J++)
    {
      R=((*Pal1)    +(*Pal2++))>>1;
      G=((*(Pal1+1))+(*Pal2++))>>1;
      B=((*(Pal1+2))+(*Pal2++))>>1;
//      R=Pal[I*3] + Pal[J*3]>>1;
//      G=Pal[I*3+1] + Pal[J*3+1]>>1;
//      B=Pal[I*3+2] + Pal[J*3+2]>>1;
//      Pal2+=3;

      M=192;
      Pal = Screen->Pal;
      for(K=0;K<256;K++)
      {
        E = abs((*(Pal+1))-R)+abs((*(Pal+2))-G)+abs((*(Pal+2))-B);
//        E = abs(Pal[K*3]-R)+abs(Pal[K*3+1]-G)+abs(Pal[K*3+2]-B);
        Pal+=3;
        if (E<M) {M=E; L=K;}
      }
      TTable[J+(I<<8)]=L;
    }
    Pal1+=3;
    if (!(I&0xF)) printf("+");
  }
  for(I=0;I<256;I++)
    for(J=0;J<I;J++)
      TTable[J+(I<<8)]=TTable[I+(J<<8)];
  printf("...Complete.\n");
}

void View_8Tables()
{
  int32_t I,J;
  int32_t MY;
  if (YRes<256) MY=YRes; else MY=256;
  if (!Tblmem) {VESA_Warning("Alert! Attempting to view non-allocated Tables"); return;}

  memset(VPage,0,PageSize);
  for(I=0;I<MY;I++)
  {
    VPage[I+2] = I;
    VPage[I+262] = I;
    VPage[YOffs[I+2]] = I;
    VPage[YOffs[I+2]+260] = I;
  }

  for(I=0;I<MY;I++)
    memcpy(VPage+YOffs[I+2]+2,ATable+(I<<8),256);
  for(I=0;I<MY;I++)
    memcpy(VPage+YOffs[I+2]+262,TTable+(I<<8),256);

//  MainSurf->Flip(MainSurf);
}

void Build_8Tables()
{
  if (Tblmem) {VESA_Warning("Table memory space already allocated - aborting Table Build command.\n"); return;}
  if (BPP!=8) {VESA_Warning("Graphic mode is not Palettized - Cannot build Palette-based tables"); return;}
  // Allocating shit
  //Tblmem = (char *)DPMI_New(VESAModule,327680,"8Bit Aligned Table space");
  Tblmem = new char [327680];
  Tblali = Tblmem+0x10000-(((uintptr_t)Tblmem)&0xFFFF);
  ATable = Tblali;
  TTable = Tblali+0x10000;
  CTable = Tblali+0x20000;
  STable = Tblali+0x30000;
  //GrdTbl = (char *)DPMI_New(VESAModule,64,"8Bit Gouraud Table");
  GrdTbl = new char [64];
  // Computing Tables
  Make_Gouraud_8(0,0,0,16,32,63);
  Make_Addative();
  Make_Transparence();
//  Make_ColorMap();
//  Make_Shade(0.3,1.3);
}
