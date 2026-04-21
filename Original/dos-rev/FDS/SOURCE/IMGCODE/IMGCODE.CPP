#include <StdIO.H>
#include <ConIO.H>
#include <StdLib.H>
#include <DOS.H>
#include <IO.H>
#include <String.H>

#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"
#include "..\FDS_Defs.H"

Material * MatLib = NULL; // Internal Material Library. This is global for
                   // all Scenes.

// Scales Texture Src to Texture Dest. recieves X/Y resolution of Src and
// Dest in (X1,Y1) and (X2,Y2) accordingly.
// Both textures should have a common BPP given as a parameter (CP=BPP+1>>3).
void Texture_Scale(char *Src,char *Dest,long X1,long Y1,long X2,long Y2,char CP)
{
  long I,J,A,B;
  long X,Y,DX,DY;
  DX=(X1<<16)/X2;
  DY=(Y1<<16)/Y2;
  Y=0;
  for(J=0;J<Y2;J++)
  {
    X=0;
    for(I=0;I<X2;I++)
    {
      memcpy(Dest+(I+J*X2)*CP,Src+((X>>16)+(Y>>16)*X1)*CP,CP);
      X+=DX;
    }
    Y+=DY;
  }
}

// I HAVE NO FUCKIN IDEA WHY I HAD TO DO THIS.
extern "C" {
void IMGExtract_15Bit(DWord Src,QColor *C)
{
  C->R = (Src>>10)<<3;
  C->G = ((Src>>5)&0x1F)<<3;
  C->B = (Src&0x1F)<<3;
}
void IMGExtract_16Bit(DWord Src,QColor *C)
{
  C->R = (Src>>11)<<3;
  C->G = ((Src>>5)&0x3F)<<2;
  C->B = (Src&0x1F)<<3;
}
void IMGExtract_24Bit(DWord Src,QColor *C)
{
  C->R = Src>>16;
  C->G = (Src>>8)&0xFF;
  C->B = Src&0xFF;
}
void IMGExtract_32Bit(DWord Src,QColor *C)
{
  C->R = Src>>16;
  C->G = (Src>>8)&0xFF;
  C->B = Src&0xFF;
}
void IMGStore_15Bit(DWord *Src,QColor C)
{
  *((Word *)Src)=((C.R>>3)<<10)+((C.G>>3)<<5)+(C.B>>3);
}
void IMGStore_16Bit(DWord *Src,QColor C)
{
  *((Word *)Src)=((C.R>>3)<<11)+((C.G>>2)<<5)+(C.B>>3);
}
void IMGStore_24Bit(DWord *Src,QColor C)
{
  Byte *BSrc=(Byte *)Src;
  *BSrc++=C.B;
  *BSrc++=C.G;
  *BSrc=C.R;
}
void IMGStore_32Bit(DWord *Src,QColor C)
{
  *Src=(C.R<<16)+(C.G<<8)+C.B;
}
}

#include "Quantum.H"

void BPPConvert_Texture(Texture *Tx,char TB)
{
  long I,J;
  void (* Read)(DWord Src,QColor *C);
  void (* Write)(DWord *Src,QColor C);
  QColor Col;
  char RR,RW;
  char *Data,*P1,*P2;

  if (!Tx->BPP) return;
  if (TB!=8&&TB!=15&&TB!=16&&TB!=24&&TB!=32) return;
  if (Tx->BPP==TB) return;

  RR = Tx->BPP+1>>3;
  RW = TB+1>>3;

  switch(Tx->BPP)
  {
    case 15: Read=&IMGExtract_15Bit; break;
    case 16: Read=&IMGExtract_16Bit; break;
    case 24: Read=&IMGExtract_24Bit; break;
    case 32: Read=&IMGExtract_32Bit; break;
  }

  switch(TB)
  {
    case 15: Write=&IMGStore_15Bit; break;
    case 16: Write=&IMGStore_16Bit; break;
    case 24: Write=&IMGStore_24Bit; break;
    case 32: Write=&IMGStore_32Bit; break;
  }

  if (TB==8) //Hence Tx->BPP>8,so this shit goes for QUANTUM unit.
  {
    Quantitize(Tx);
    return;
  }

  Data = new char[65536*RW];
  P1 = Tx->Data;
  P2 = Data;

  if (Tx->BPP>8)
  {
    for(I=0;I<65536;I++,P1+=RR,P2+=RW)
    {
//      printf("Data : %d\n",*((Word *)P1));
      Read(*((DWord *)P1),&Col);
//      printf("Color read : (%d,%d,%d)\n",Col.R,Col.G,Col.B);
      Write((DWord *)P2,Col);
    }
    delete Tx->Data;
    Tx->Data = Data;
//    memcpy(Tx->Data,Data,RW*65536);
//    delete Data;
    Tx->BPP = TB;
  } else {
    for(I=0;I<65536;I++,P1++,P2+=RW)
    {
      Col.R=Tx->Pal->C[*P1].R<<2;
      Col.G=Tx->Pal->C[*P1].G<<2;
      Col.B=Tx->Pal->C[*P1].B<<2;

      Write((DWord *)P2,Col);
    }
    delete Tx->Data;
    delete Tx->Pal;
    Tx->Pal = NULL;
    Tx->Data = Data;
//    memcpy(Tx->Data,Data,RW*65536);
//    delete Data;
    Tx->BPP = TB;
  }
}

// weakens Texture intensity by 1/2.
void Darken_Texture(Texture *T)
{
  long I;
  Word *W;
  DWord *DW;
  switch(T->BPP)
  {
    case 8: return;
    case 15: break; //printf("Go get a life, or something.\n"); break;
    case 16:
      W = (Word *)T->Data;
      for(I=0;I<65536;I++)
      {
        *W >>= 1;
        *W &= 0x7BEF;
        W++;
      }
    break;
    case 32:
      DW = (DWord *)T->Data;
      for(I=0;I<65536;I++)
      {
        *DW >>= 1;
        *DW &= 0x7F7F7F7F;
        DW++;
      }
    break;
  }
}

// Reduces last DAC bit on the target Texture. A minor
// Quanlity reduction will follow.
void FilterLSB_Texture(Texture *T)
{
  long I;
  Word *W;
  DWord *DW;
  switch(T->BPP)
  {
    case 8: return;
    case 15: break; //printf("Go get a life, or something.\n"); break;
    case 16:
      W = (Word *)T->Data;
      for(I=0;I<65536;I++)
      {
        *W &= 0xF7DE;
        W++;
      }
    break;
    case 32:
      DW = (DWord *)T->Data;
      for(I=0;I<65536;I++)
      {
        *DW &= 0xFEFEFEFE;
        DW++;
      }
    break;
  }
}

#pragma pack(1)
struct ImageDetectResult
{
  long X,Y;
  char BPP; // 0 - file invalid
};

struct PCXHeader {
  Byte ID;
  Byte Version;
  Byte RLE;
  Byte BPP;
  Word XStart;
  Word YStart;
  Word XEnd;
  Word YEnd;
  Word HRes;
  Word VRes;
  Byte Pal[48];
  Byte Rsvd1;
  Byte NBitP;
  Word BytesPerLine;
  Word PalType;
  Word HSize;
  Word VSize;
  Byte Rsvd2[54];
};

struct TGAHeader {
  Byte IDLen;
  Byte ColorMapType;
  Byte ImageType;
  Word ColorMapOrigin;
  Word ColorMapLength;
  Byte ColorMapEntry;
  Word OriginX;
  Word OriginY;
  Word Width;
  Word Height;
  Byte ImagePixelSize;
  Byte ImageDescriptor;
};


// Detects whether Filename FN contains a valid PCX header.
// returns 0 on failure, otherwise returns image size in bytes.
ImageDetectResult PCXDetect(char *FN)
{

  PCXHeader PH;
  ImageDetectResult Res;
  FILE *F;

  Res.BPP=0;
  F = fopen(FN,"rb");

  fread (&PH, 1, sizeof(PH), F);
  fclose(F);
  if (PH.ID != 0x0A)
  {
#ifdef ShowImageErrors
    printf("* PCX Detect fail on ID\n");
#endif
    return Res;
  }
  if (PH.Version != 5)
  {
#ifdef ShowImageErrors
    printf("* PCX Detect fail: Version mismatch (Ver = %d,Required = 5)\n",PH.Version);
#endif
    return Res;
  }
  if (PH.RLE != 1)
  {
#ifdef ShowImageErrors
    printf("* PCX Detect fail: no RLE Compression\n");
#endif
    return Res;
  }
  if (PH.BPP == 8 && PH.NBitP != 1)
  {
#ifdef ShowImageErrors
    printf("* PCX Detect fail: BPP/NBitP mismatch\n");
#endif
    return Res;
  }

  Res.X = PH.XEnd - PH.XStart + 1;
  Res.Y = PH.YEnd - PH.YStart + 1;
  Res.BPP = PH.BPP;
#ifdef ShowImageErrors
  printf("PCX Image file detected <%dx%d-%d>\n",Res.X,Res.Y,Res.BPP);
#endif
  return Res;
}

ImageDetectResult JPGDetect(char *FN)
{
  ImageDetectResult Res;
  FILE *F;

  char ID[5];
  ID[4]=0;

  Res.BPP=0;
  F = fopen(FN,"rb");
  if (!F) return Res;
  fseek(F,6,SEEK_SET);
  fread (ID,1,4,F);
  fclose(F);
  if (!strcmp(ID,"JFIF")) Res.BPP=24; else Res.BPP=0;
//  printf("File: %s Here's the ID: %s, Here's the Res.BPP: %i\n",
//         FN,ID,Res.BPP);


  return Res;
}

// Loads a PCX image from FN to Store,using IDR as results from previous
// Detection via PCXDetect, and Pal to store the Palette.
char PCXLoad(char *FN,unsigned char *Store,ImageDetectResult IDR,Palette *Pal)
{
  FILE *F;
  char Ret;
  long I,J,K;
  long X,Y;
  long FSize;
  unsigned char V;
  unsigned char C;
  unsigned char *outpt;
  int NP = 0;
  PCXHeader PH;

  F = fopen(FN,"rb");

  fseek(F,0,SEEK_END);
  FSize=ftell(F);

  Ret = 0;

  fseek(F,0,SEEK_SET);
  fread(&PH,1,sizeof(PH),F);

  if (PH.BPP == 1 && PH.NBitP == 4)
  {
    memset(Pal, 0, 768);
    for (I = 0; I < 16; I++)
    {
      Pal->C[I].R = PH.Pal[I*3  ] >> 2;
      Pal->C[I].G = PH.Pal[I*3+1] >> 2;
      Pal->C[I].B = PH.Pal[I*3+2] >> 2;
    }
  } else if (PH.BPP == 8 && PH.NBitP == 1)
    {
      fseek(F, FSize-768, SEEK_SET);
      for (I = 0; I < 256; I++)
      {
        Pal->C[I].R = ((unsigned char)getc(F)) >> 2;
        Pal->C[I].G = ((unsigned char)getc(F)) >> 2;
        Pal->C[I].B = ((unsigned char)getc(F)) >> 2;
      }
    } else goto bye;

  fseek(F, sizeof(PCXHeader), SEEK_SET);
  X = IDR.X;
  Y = IDR.Y;
  Ret = 1;

  while (Y-- > 0) {
    outpt = Store;
    memset(Store, 0, X);
    for (NP = 0; NP < PH.NBitP; NP++) {
      I = 0;
      Store = outpt;
      do {
        C = (Byte)getc(F);
        if ((C & 0xC0) != 0xC0) {
          if (PH.BPP == 1) {
            for (K = 7; K >= 0; K--)
              *Store++ |= ((C >> K) & 1) << NP;
              I += 8;
          } else {
            *Store++ = C;
            I++;
          }
        } else {
          V = (Byte)getc(F);
          C &= ~0xC0;
          while (C > 0 && I < X) {
            if (PH.BPP == 1) {
              for (K = 7; K >= 0; K--)
                *Store++ |= ((V >> K) & 1) << NP;
                I += 8;
            } else {
              *Store++ = V;
              I++;
            }
            C--;
          }
        }
      } while (I < X);
    }
  }

  bye:  if (feof(F)) Ret=0;
  if (F) fclose(F);
  return Ret;
}


// GIF definations.
const long MAX_CODES = 4096;

static int            curr_size;
static int            clear;
static int            ending;
static int            newcodes;
static int            top_slot;
static int            slot;
static int            navail_bytes = 0;
static int            nbits_left = 0;
static unsigned char  b1;
static unsigned char  byte_buff[257];
static unsigned char* pbytes;

static unsigned long code_mask[13] =
{
  0L,
  0x0001L, 0x0003L,
  0x0007L, 0x000FL,
  0x001FL, 0x003FL,
  0x007FL, 0x00FFL,
  0x01FFL, 0x03FFL,
  0x07FFL, 0x0FFFL
};

static Byte *stack;
static Byte *suffix;
static Word *prefix;

static int get_next_code(FILE *F)
{
   register int i;
   static unsigned long ret;

  if( ! nbits_left )
  {
    if( navail_bytes <= 0 )
    {
      pbytes = byte_buff;
      navail_bytes = getc(F);
      if( navail_bytes )
        for( i = 0; i < navail_bytes; ++i )
          byte_buff[i] = (Byte)getc(F);
    }
    b1 = *pbytes++;
      nbits_left = 8;
      --navail_bytes;
   }
  ret = b1 >> (8 - nbits_left);
   while( curr_size > nbits_left )
   {
    if( navail_bytes <= 0 )
      {
         pbytes = byte_buff;
         navail_bytes = getc(F);
         if( navail_bytes )
            for( i = 0; i < navail_bytes; ++i )
               byte_buff[i] = (Byte)getc(F);
      }
    b1 = *pbytes++;
      ret |= b1 << nbits_left;
    nbits_left += 8;
    --navail_bytes;
   }
  nbits_left -= curr_size;

  return( (int) (ret & code_mask[curr_size]) );
}

#define OUTB(c) (*Store++ = (Byte)(c))

/*static int getw(FILE *f) {
   int k = (unsigned)fgetc(f);
   k += 256*fgetc(f);
   return k;
}*/


ImageDetectResult GIFDetect(const char *FN)
{
  FILE *F;
  unsigned char  buf[1028];
  ImageDetectResult IDR;
  unsigned short X,Y;

  IDR.BPP = 0;

  F = fopen(FN,"rb");
  fread (buf,1,6,F);
  if( strncmp( (char *)buf, "GIF", 3 ) )
  {
    fclose(F);
#ifdef ShowImageErrors
    printf("GIF Detection fail on ID.\n");
#endif
    return IDR;
  }
  fread(buf,1,7,F);
  fread(buf,1,3*(2 << (buf[4] & 7)),F);
  fread(buf,1,5,F);
//  IDR.X = getw(F);
//  IDR.Y = getw(F);
  fread(&X,2,1,F);
  fread(&Y,2,1,F);

  IDR.X = X;
  IDR.Y = Y;
  IDR.BPP = 8;
  fclose(F);
  return IDR;
}

int GIFLoad(const char *FN,Byte *Store,Palette *Pal)
{
  unsigned char* sp;
  int            code, fc, oc;
  int            I;
  unsigned char  size;
  int            c;
  unsigned char  buf[1028];
  unsigned short X,Y;
  FILE *F;


  F = fopen(FN,"rb");
  fread( buf, 1, 6, F);

  fread( buf, 1, 7, F );
  memset(Pal, 0, 768);

  for( I = 0; I < 2 << (buf[4] & 7); I++)
  {
    Pal->C[I].R = (Byte)getc(F) >> 2;
    Pal->C[I].G = (Byte)getc(F) >> 2;
    Pal->C[I].B = (Byte)getc(F) >> 2;
  }

  fread( buf, 1, 5, F);
//  X = getw(F);
//  Y = getw(F);
  fread(&X,2,1,F);
  fread(&Y,2,1,F);

  fread( buf, 1, 1, F );
  size = (Byte)getc( F );
  if( size < 2 || 9 < size )
  {
    fclose(F);
    return 0;
  }

  stack  = new Byte[MAX_CODES + 1];
  suffix = new Byte[MAX_CODES + 1];
  prefix = new Word[MAX_CODES + 1];

  curr_size = size + 1;
  top_slot = 1 << curr_size;
  clear = 1 << size;
  ending = clear + 1;
  slot = newcodes = ending + 1;
  navail_bytes = nbits_left = 0;
  oc = fc = 0;
  sp = stack;

  while( (c = get_next_code(F)) != ending )
   {
      if( c == clear )
      {
         curr_size = size + 1;
         slot = newcodes;
         top_slot = 1 << curr_size;
         while( (c = get_next_code(F)) == clear );
         if( c == ending )
            break;
         if( c >= slot )
            c = 0;
         oc = fc = c;
         OUTB(c);
      }
      else
      {
     code = c;
         if( code >= slot )
         {
        code = oc;
            *sp++ = (Byte)fc;
     }
         while( code >= newcodes )
         {
            *sp++ = suffix[code];
        code = prefix[code];
         }
         *sp++ = (Byte)code;
         if( slot < top_slot )
         {
            fc = code;
            suffix[slot]   = (Byte)fc;
            prefix[slot++] = (Word)oc;
            oc = c;
         }
      if( slot >= top_slot && curr_size < 12 )
      {
        top_slot <<= 1;
        ++curr_size;
      }
      while( sp > stack )
      {
        --sp;
        OUTB(*sp);
      }
    }
  }
  delete stack;
  delete suffix;
  delete prefix;
  fclose(F);
  return 1;
}

ImageDetectResult TGADetect(const char *FN)
{
  ImageDetectResult IDR;
  FILE *F;
  TGAHeader TH;
  long I;

  IDR.BPP=0;

  F = fopen(FN,"rb");
  fread(&TH,18,1,F); //Read the header.

  if (feof(F))
  {
#ifdef ShowImageErrors
    printf("* TGA Detect fail: File has Incomplete Header.\n");
#endif
    fclose(F);
    return IDR;
  }

  if (TH.IDLen)
  {
//    fread(NULL,1,TH.IDLen,F);
//    if (feof(F))
    if (fseek(F,TH.IDLen,SEEK_CUR))
    {
#ifdef ShowImageErrors
      printf("* TGA detect Fail: File has incomplete ID field.\n");
#endif
      fclose(F);
      return IDR;
    }
  }
  switch (TH.ImageType)
  {
    case 1:
      if (TH.ColorMapType!=1)
      {
#ifdef ShowImageErrors
        printf("* TGA detect Fail: \"Color-Mapped\" Image File does not contain Color maps.\n");
#endif
        fclose(F);
        return IDR;
      }
      if (TH.ColorMapEntry!=16 && TH.ColorMapEntry!=24 && TH.ColorMapEntry!=32)
      {
#ifdef ShowImageErrors
        printf("* TGA detect Fail: Invalid Color map entry format.\n");
#endif
        fclose(F);
        return IDR;
      }
      I = TH.ColorMapLength*(TH.ColorMapEntry>>3);
      fseek(F,I,SEEK_CUR);
      if (ferror(F))
      {
#ifdef ShowImageErrors
        printf("* TGA detect Fail: File has incomplete Color map.\n");
#endif
        fclose(F);
        return IDR;
      }
      if (TH.ImageDescriptor)
      {
#ifdef ShowImageErrors
        printf("* TGA detect Fail: File has abnormal Image Descriptor (%xh).\n",TH.ImageDescriptor);
#endif
        fclose(F);
        return IDR;
      }

      if (TH.ColorMapOrigin>255||TH.ColorMapOrigin+TH.ColorMapLength>256)
      {
#ifdef ShowImageErrors
        printf("* TGA detect Fail: File OK but Color maps overflow the 256 PAL limit.\n");
#endif
        fclose(F);
        return IDR;
      }
      if (TH.ImagePixelSize>8)
      {
#ifdef ShowImageErrors
        printf("* TGA detect Fail: File OK but Color maps cannot be applied to a DAC Texture.\n");
#endif
        fclose(F);
        return IDR;
      }

      fseek(F,TH.Width*TH.Height*(TH.ImagePixelSize>>3),SEEK_CUR);
      if (ferror(F))
      {
#ifdef ShowImageErrors
        printf("* TGA detect Fail: File has incomplete Image data.\n");
#endif
        fclose(F);
        return IDR;
      }
      // it worked. bail.
      IDR.X = TH.Width;
      IDR.Y = TH.Height;
      IDR.BPP = TH.ImagePixelSize;
      if (IDR.BPP==16) IDR.BPP--;
      fclose(F);
      return IDR;
    break;
    case 2:
      // Rubbish, waste of mem inside the file!!! handle it anyway.
      if (TH.ColorMapType==1)
      {
#ifdef ShowImageErrors
        printf("* TGA detect Warning: Color map exists in an Unmapped Color Image File.\n");

        if (TH.ColorMapEntry!=16 && TH.ColorMapEntry!=24 && TH.ColorMapEntry!=32)
          printf("* TGA detect Warning: Invalid Color map entry format.\n");
#endif
        I = TH.ColorMapLength*(TH.ColorMapEntry>>3);
        fseek(F,I,SEEK_CUR);
        if (ferror(F))
        {
#ifdef ShowImageErrors
          printf("* TGA detect Fail: File has incomplete Color map.\n");
#endif
          return IDR;
        }
      }
      if (TH.ImageDescriptor)
      {
#ifdef ShowImageErrors
        printf("* TGA detect Fail: File has abnormal Image Descriptor (%xh).\n",TH.ImageDescriptor);
#endif
        return IDR;
      }
      fseek(F,TH.Width*TH.Height*(TH.ImagePixelSize>>3),SEEK_CUR);
      if (ferror(F))
      {
#ifdef ShowImageErrors
        printf("* TGA detect Fail: File has incomplete Image data.\n");
#endif
        return IDR;
      }
      IDR.X = TH.Width;
      IDR.Y = TH.Height;
      IDR.BPP = TH.ImagePixelSize;
      // well, it's their problem they didn't know VESA pixel formats!
      if (IDR.BPP==16) IDR.BPP--;
      fclose(F);
      return IDR;
    break;
    case 9:
      return IDR;
    break;
    case 10:
      return IDR;
    break;
    default:
#ifdef ShowImageErrors
      printf("* TGA detect Fail: Unknown Image format.\n");
#endif
      return IDR;
  }
}

char TGALoad(const char *FN,Byte *Store,Palette *Pal)
{
  FILE *F;
  TGAHeader TH;
  QColor C;
  char *cPal = (char *)Pal;
  char PalBytes;
  DWord Uniform;
  long I;

  F = fopen(FN,"rb");
  fread(&TH,18,1,F);

  if (TH.IDLen)
    fseek(F,TH.IDLen,SEEK_CUR);

  switch (TH.ImageType)
  {
    case 1:
      PalBytes = TH.ColorMapEntry>>3;
      cPal+=TH.ColorMapOrigin*PalBytes;
      for(I=0;I<TH.ColorMapLength;I++)
      {
        fread(&Uniform,PalBytes,1,F);
        switch (PalBytes)
        {
          case 2: IMGExtract_15Bit(Uniform,&C);
          case 3: IMGExtract_24Bit(Uniform,&C);
          case 4: IMGExtract_32Bit(Uniform,&C);
        }
        *cPal++=C.R>>2;
        *cPal++=C.G>>2;
        *cPal++=C.B>>2;
      }

      fread(Store,TH.Width*TH.Height*(TH.ImagePixelSize>>3),1,F);
      return 1;
    break;
    case 2:
      //Just do it. Nike.
      fread(Store,TH.Width*TH.Height*(TH.ImagePixelSize>>3),1,F);
      return 1;
    break;
    case 9:
      return 0;
    break;
    case 10:
      return 0;
    break;
  }
  return 0;
}

// Identifies and Gets BPP for Texture Tx.
char Identify_Texture(Texture *Tx)
{
  FILE *F;
  long I,J;
  ImageDetectResult IDR; // Stores results from format detectors.
  char *Txd; // Host for extracted image. Transferred/Scaled to Tx->Data.

/*  if (Tx->BPP) {
    if (Tx->BPP==8) delete Tx->Pal;
    Tx->BPP=0;
    delete Tx->Data;
  }*/
  Tx->BPP=0;
  F = fopen(Tx->FileName,"rb");
  if (!F)
  {
//    printf("Texture file \"%s\" does not exist or Filename invalid.\n",Tx->FileName);
    return 0;
  }
  fclose(F);
  IDR=PCXDetect(Tx->FileName);
  if (IDR.BPP)
  {
    I = strlen(Tx->FileName);
//    if (strcmp(Tx->FileName+I-4,".PCX")) printf("PCX Extension Mismatch.\n");

    Tx->BPP=IDR.BPP;
    return 1;
  }
  IDR = GIFDetect(Tx->FileName);
  if (IDR.BPP)
  {
    I = strlen(Tx->FileName);
//    if (strcmp(Tx->FileName+I-4,".GIF")) printf("GIF Extension Mismatch.\n");
    Tx->BPP=IDR.BPP;
    return 1;
  }

/*  IDR = TGADetect(Tx->FileName);
  if (IDR.BPP)
  {
    Tx->BPP=IDR.BPP;
    return 1;
  }*/

  IDR = JPGDetect(Tx->FileName);
  if (IDR.BPP)
  {
//    printf("Texture Identified : Bits per pixel = %d\n",IDR.BPP);
    Tx->BPP = IDR.BPP;
    return 1;
  }
  return 0;
}


// Attempts to Load the neccesary file to Texture Tx. Tx->Filename is used
// In the process, failure to give a valid filename will cause an error.
// Returns 1 on success.
char Load_Texture(Texture *Tx)
{
  FILE *F;
  long I,J;
  ImageDetectResult IDR; // Stores results from format detectors.
  char *Txd; // Host for extracted image. Transferred/Scaled to Tx->Data.

//  printf("zol\n");
  fflush(stdout);
/*  if (Tx->BPP) {
    if (Tx->BPP==8) delete Tx->Pal;
    Tx->BPP=0;
    delete Tx->Data;
  }*/
//  printf("Attempting to read: texture file %s as %d bpp tmap\n",Tx->FileName,Tx->BPP);

  F = fopen(Tx->FileName,"rb");
  if (!F)
  {
//    printf("Texture file \"%s\" does not exist or Filename invalid.\n",Tx->FileName);
    return 0;
  }
  fclose(F);
  IDR=PCXDetect(Tx->FileName);
  if (IDR.BPP)
  {
    I = strlen(Tx->FileName);
//    if (strcmp(Tx->FileName+I-4,".PCX")) printf("PCX Extension Mismatch.\n");

    Txd = new char[IDR.X*IDR.Y*(IDR.BPP+1>>3)];
//    printf("Allocated %d Bytes for original Texture\n",IDR.X*IDR.Y*(IDR.BPP+1>>3));

    if (IDR.BPP==8)
    {
      Tx->Pal = new Palette;
//      printf("Allocated memory for palette\n");
    }

    if (PCXLoad(Tx->FileName,(unsigned char *)Txd,IDR,Tx->Pal))
    {
      Tx->BPP=IDR.BPP; //validate the texture
      Tx->Data = new char[65536*(Tx->BPP+1>>3)];

      Texture_Scale(Txd,Tx->Data,IDR.X,IDR.Y,256,256,IDR.BPP+1>>3);

      delete Txd;
      return 1;
    }
    else
    {
#ifdef ShowImageErrors
      printf("PCX file is Corrupted.\n");
#endif
      if (IDR.BPP==8)
      {
        delete Tx->Pal;
        Tx->Pal = NULL;
      }
      delete Txd;
    }
  }

  // GIF detection and load goes here
  IDR = GIFDetect(Tx->FileName);
  if (IDR.BPP)
  {
    I = strlen(Tx->FileName);
//    if (strcmp(Tx->FileName+I-4,".GIF")) printf("GIF Extension Mismatch.\n");

    Txd = new char[IDR.X*IDR.Y*(IDR.BPP+1>>3)];
//    printf("Allocated %d Bytes for original Texture\n",IDR.X*IDR.Y*(IDR.BPP+1>>3));

    if (IDR.BPP==8)
    {
      Tx->Pal = new Palette;
//      printf("Allocated memory for palette\n");
    }
    if (GIFLoad(Tx->FileName,(unsigned char *)Txd,Tx->Pal))
    {
      Tx->BPP=IDR.BPP; //validate the texture

      Tx->Data = new char[65536*(Tx->BPP+1>>3)];
      printf("Allocated memory for texture at %xh\n",(long)Tx->Data);

      Texture_Scale(Txd,Tx->Data,IDR.X,IDR.Y,256,256,IDR.BPP+1>>3);

      delete Txd;
      return 1;
    }
    else
    {
#ifdef ShowImageErrors
      printf("GIF file is Corrupted.\n");
#endif
      if (IDR.BPP==8)
      {
        delete Tx->Pal;
        Tx->Pal = NULL;
      }
      delete Txd;
    }

  }

  IDR = TGADetect(Tx->FileName);
  if (IDR.BPP)
  {
    I = strlen(Tx->FileName);
//    if (strcmp(Tx->FileName+I-4,".TGA")) printf("TGA Extension Mismatch.\n");

    Txd = new char[IDR.X*IDR.Y*(IDR.BPP+1>>3)];
//    printf("Allocated %d Bytes for original Texture\n",IDR.X*IDR.Y*(IDR.BPP+1>>3));

    if (IDR.BPP==8)
    {
      Tx->Pal = new Palette;
//      printf("Allocated memory for palette\n");
    }

    if (TGALoad(Tx->FileName,(unsigned char *)Txd,Tx->Pal))
    {
      Tx->BPP=IDR.BPP; //validate the texture
      Tx->Data = new char[65536*(Tx->BPP+1>>3)];

      Texture_Scale(Txd,Tx->Data,IDR.X,IDR.Y,256,256,IDR.BPP+1>>3);

      delete Txd;
      return 1;
    }
    else
    {
#ifdef ShowImageErrors
      printf("TGA file is Corrupted.\n");
#endif
      if (IDR.BPP==8)
      {
        delete Tx->Pal;
        Tx->Pal = NULL;
      }
      delete Txd;
    }
  }

  // JPG detection and load goes here
	fflush(stdout);
	IDR = JPGDetect(Tx->FileName);
	if (IDR.BPP)
	{
		Txd=LoadJPEG(Tx->FileName,0);

		IDR.X = *(unsigned short *)Txd;
		IDR.Y = *(unsigned short *)(Txd+2);
		char r,g,b;
		for (I=4;I<4+IDR.X*IDR.Y*3;I+=3)
		{
			 r=Txd[I];
			 g=Txd[I+1];
			 b=Txd[I+2];
			 Txd[I+2]=r;
			 Txd[I+1]=g;
			 Txd[I]=b;
		}
		Tx->BPP=IDR.BPP; //validate the texture
		Tx->Data = new char[65536*(Tx->BPP+1>>3)];
		Texture_Scale(Txd+4,Tx->Data,IDR.X,IDR.Y,256,256,IDR.BPP+1>>3);
		delete Txd;
		return 1;
	}
	// CEL detection and load goes here
  return 0;
}

void View_Texture(Texture *T)
{
  long PX,PY;
  char PBPP;
  long I,J,K;

  PX = XRes; PY = YRes; PBPP=BPP;

/*  if (!InitGraphics(640,480,T->BPP)) return;
  memset(VPage,0,PageSize);
  if (T->BPP==8) Conv_Set_Palette(T->Pal);
  K = T->BPP+1>>3;
  // Display Texture
  for(I=0;I<256;I++)
    for(J=0;J<256;J++)
			memcpy(VPage+(I*K+YOffs[J]),T->Data+(I+(J<<8))*K,K);

  // Display Palette
  if (BPP==8)
    for(J=300;J<400;J++)
      for(I=260;I<516;I++)
        VPage[I+YOffs[J]]=I-260;

  Flip(VPage,VGAPtr);
  while(!Keypressed());
  EndGraphics();

  if (PX&&PY&&PBPP) InitGraphics(PX,PY,PBPP);*/
}

void Unify_Textures(Scene *Sc)
{
  Material *M;
  char xBPP=0;

	for(M=MatLib;M;M=M->Next)
	{
    if (M->RelScene!=Sc) continue; //Ignore nonrelevant materials.
    if (!M->Txtr) continue; //Ignore nontextured materials.
    xBPP|=M->Txtr->BPP;
    if (M->Txtr->BPP!=BPP) BPPConvert_Texture(M->Txtr,BPP);
    if (M->Txtr->BPP!=8&&(!MMXState)) FilterLSB_Texture(M->Txtr);
  }
  if (BPP==8&&xBPP)
  {
    Universal_Palette(Sc); //So much work for the Quantum unit,heh heh.
    // Store collective palette on scene.
    Sc->Pal = new Palette;
    for(M=MatLib;M;M=M->Next)
    {
      if (M->RelScene!=Sc) continue;
      if (!M->Txtr) continue;
      memcpy(Sc->Pal,M->Txtr->Pal,sizeof(Palette));
      break;
    }
		// kill all palettes
		for(M=MatLib;M;M=M->Next)
    {
      if (M->RelScene!=Sc) continue;
      if (!M->Txtr) continue;
      delete M->Txtr->Pal;
      M->Txtr->Pal = Sc->Pal; //redirection keeps m->txtr->pal valid
    }

  }
}

// Calculates a Standard base color for a given Material. This will be
// white unless The material has a Texture. In this case it will be the
// average color of the texture.
void Material_StdBaseColor(Material *M)
{

}


////////////////////////////////////////////
// shit end here, now real image code starts
////////////////////////////////////////////

// Allocates and does everything by itself
char Load_Image_PCX(Image *Img,char *FN)
{
  FILE *F;
  char Ret;
  long I,J,K;
  long X,Y;
  long FSize;
  Byte V;
  Byte C;
  DWord *Pen,*PP,*Line;
  int NP = 0;
  PCXHeader PH;
	char *Pal = new char[768];

	F = fopen(FN,"rb");
	if (!F) {delete Pal; return 0;}

  Ret = 0;

  // measure size of file
  fseek(F,0,SEEK_END);
  FSize=ftell(F);

  // reset file and read header
  fseek(F,0,SEEK_SET);
  fread(&PH,1,sizeof(PH),F);

  // Header check
  if (PH.ID != 0x0A || PH.Version != 5 || PH.RLE != 1 || (PH.BPP == 8 && PH.NBitP != 1) ) goto OUT;

  // 4Bit Palette
  if (PH.BPP == 1 && PH.NBitP == 4)
	{
    memset(Pal, 0, 768);
		for (I = 0; I < 16; I++)
		{
      Pal[I*3  ] = PH.Pal[I*3  ];
      Pal[I*3+1] = PH.Pal[I*3+1];
      Pal[I*3+2] = PH.Pal[I*3+2];
    }
  } else if (PH.BPP == 8 && PH.NBitP == 1)
    {
      // 8Bit Palette stored at the end of file
      fseek(F, FSize-768, SEEK_SET);
      fread(Pal, 1, 768, F);
    } else goto OUT;

  fseek(F, sizeof(PCXHeader), SEEK_SET);

  // get metrics
  X = PH.XEnd - PH.XStart + 1;
  Y = PH.YEnd - PH.YStart + 1;
	Ret = 1;

	// store metrics and allocate
	Img->X = X; Img->Y = Y; Img->Data = new DWord[X*Y]; //yeah
  Pen = Img->Data;

  // data read, RLE decompression
  while (Y-- > 0)
  {
    Line = Pen; //sets beginning of line
    memset(Pen, 0, X << 2);
    for (NP = 0; NP < PH.NBitP; NP++)
    {
      I = 0;
      Pen = Line; //move to beginning of line multiple times until all bits are processed
      do {
        C = (Byte)getc(F);
        if ((C & 0xC0) != 0xC0) {
          if (PH.BPP == 1) {
            for (K = 7; K >= 0; K--)
							*Pen++ |= ((C >> K) & 1) << NP; //Extract bit
              I += 8;
					} else {
						*Pen++ = C;
            I++;
          }
        } else {
          V = (Byte)getc(F);
          C &= 0x3F; //reciporal to 0xC0
          while (C > 0 && I < X) {
            if (PH.BPP == 1) {
              for (K = 7; K >= 0; K--)
                *Pen++ |= ((V >> K) & 1) << NP;
                I += 8;
            } else {
              *Pen++ = V;
							I++;
            }
            C--;
          }
				}
      } while (I < X);
		}
		// reconstruct as Truecolor
    PP = Pen;
    Pen = Line;
    while (I--)
    {
			C = *Pen;
			*Pen++ = RGB32(Pal[C*3],Pal[C*3+1],Pal[C*3+2]);
    }
		Pen = PP;
	}
OUT:
	if (feof(F)) Ret=0;
	if (F) fclose(F);
	delete Pal;
	return Ret;
}

char Load_Image_GIF(Image *Img,char *FN)
{
	Byte *Txd,*Tx;

	FILE *F;
	unsigned char  buf[1028];
	unsigned short X,Y;

	F = fopen(FN,"rb");
	if (!F) return 0;
	fread(buf,1,6,F);
	if( strncmp( (char *)buf, "GIF", 3 ) )
	{
		fclose(F);
		return 0;
	}
	fread(buf,1,7,F);
	fread(buf,1,3*(2 << (buf[4] & 7)),F);
	fread(buf,1,5,F);

	fread(&X,2,1,F);
	fread(&Y,2,1,F);

	fclose(F);
	Tx = new Byte[X*Y];
	Txd = Tx;
	Palette *Pal = new Palette;
	if (!GIFLoad(FN,Tx,Pal)) return 0;

	Img->X = X;
	Img->Y = Y;

	Img->Data = new DWord[X*Y];
	DWord *DWP = Img->Data,*DWE = DWP+X*Y;
	while(DWP<DWE)
		*DWP++ = (Pal->C[*Txd].R<<18)+(Pal->C[*Txd].G<<10)+(Pal->C[*Txd++].B<<2);
	delete Tx;
	delete Pal;
	return 1;
}

char Load_Image_JPEG(Image *Img,char *FN)
{
	char *Tx,*Txd;
	if (!(Tx=LoadJPEG(FN,0))) return 0;
	Txd=Tx;
	Img->X = *(unsigned short *)Txd;
	Img->Y = *(unsigned short *)(Txd+2);
	Img->Data = new DWord[Img->X*Img->Y];
	DWord *VP = Img->Data;
	Txd+=4;
	char r,g,b;
	char *TE = Txd + Img->X*Img->Y*3;

	while (Txd<TE)
	{
		 r=*Txd++;
		 g=*Txd++;
		 b=*Txd++;
		 *VP++ = (r<<16)+(g<<8)+b;
	}
	delete Tx;
	return 1;
}