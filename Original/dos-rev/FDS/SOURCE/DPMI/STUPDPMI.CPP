  /***************************/
 /*DPMI*routines*************/
/***************************/

#pragma pack (1)
#include <Mem.H>
#include <DOS.H>
#include <StdLib.H>
#include <StdIO.H>

#include "..\FDS_Defs.H"
#include "..\FDS_Vars.H"
#include "..\FDS_Decs.H"


long Free_Memory(void)
{
  long Buffer[12]; //48 bytes for reserved stuff.
  union REGS  r;
  struct SREGS sr;

  memset(&r,0,sizeof(r));
  memset(&sr,0,sizeof(sr));

  r.x.eax = 0x500;
  sr.es = FP_SEG(Buffer);
  r.x.edi = FP_OFF(Buffer); //hey,it's sort of ES:EDI
  int386x(0x31,&r,&r,&sr);

  return Buffer[0];
}

Memblock *HMB=NULL;
Memblock *CMB=HMB;

void * Mem_Alloc(long Size,char *Name)
{
  if (Size>Free_Memory())
  {
    printf("Get more horse power! %d bytes missing for '%s'\n",Size,Name);
    fcloseall();
    exit(1);
  }
  if (HMB==NULL)
  {
    CMB = new Memblock;
    HMB = CMB;
    HMB->Prev = NULL;
    HMB->Next = NULL;
  } else {
    CMB->Next = new Memblock;
    CMB->Next->Prev = CMB;
    CMB->Next->Next = NULL;
    CMB=CMB->Next;
  }
  CMB->Name = strdup(Name);
  CMB->Data = malloc(Size);
  if (!CMB->Data)
  {
    printf("Get more horse power! %d bytes missing for '%s'\n",Size,Name);
    fcloseall();
    exit(1);
  }
  return CMB->Data;
}

void Destroy_Block(Memblock *MB)
{
  if (HMB==MB)
    if (CMB!=HMB)
    {
      free(HMB->Data);
      free(HMB->Name);
      HMB=HMB->Next;
      free(MB);
    } else {
      free(HMB->Data);
      free(HMB->Name);
      HMB=NULL;
      free(MB);
    }
  else {
    MB->Prev->Next=MB->Next;
    free(MB->Data);
    free(MB->Name);
    free(MB);
  }
}

void MemSeek_Delete(char *Name)
{
  Memblock *MB;
  for(MB=HMB;MB&&strcmp(MB->Name,Name);MB=MB->Next);

  if (MB) Destroy_Block(MB);
}

void Mem_Delete(char *Data)
{
  Memblock *MB;
  for(MB=HMB;MB&&MB->Data!=Data;MB=MB->Next);

  if (MB) Destroy_Block(MB);
}

void DPMI_Error(char *ErrMsg)
{
  char *S=strdup("<DPMI> Fatal Error: ");
  strcat(S,ErrMsg);
  printf("%s",S);
  exit(1);
}


void DPMI_allocRealSeg(int size,int *sel,int *r_seg)
{
  union REGS      r;

  r.w.ax = 0x100;                 // DPMI allocate DOS memory
  r.w.bx = (size + 0xF) >> 4;     // number of paragraphs
  int386(0x31, &r, &r);
  if (r.w.cflag)
    DPMI_Error("DPMI_allocRealSeg failed!");
  *sel = r.w.dx;                  // Protected mode selector
  *r_seg = r.w.ax;                // Real mode segment
}

void DPMI_freeRealSeg(unsigned sel)
{
  union REGS  r;

  r.w.ax = 0x101;                 // DPMI free DOS memory
  r.w.dx = sel;                   // DX := selector from 0x100
  int386(0x31, &r, &r);
}

typedef struct {
  long    edi;
  long    esi;
  long    ebp;
  long    reserved;
  long    ebx;
  long    edx;
  long    ecx;
  long    eax;
  short   flags;
  short   es,ds,fs,gs,ip,cs,sp,ss;
  } _RMREGS;

#define IN(reg)     rmregs.e##reg = in->x.reg
#define OUT(reg)    out->x.reg = rmregs.e##reg

int DPMI_int86(int intno, RMREGS *in, RMREGS *out)
{
  _RMREGS         rmregs;
  union REGS      r;
  struct SREGS    sr;

  memset(&rmregs, 0, sizeof(rmregs));
  IN(ax); IN(bx); IN(cx); IN(dx); IN(si); IN(di);

  segread(&sr);
  r.w.ax = 0x300;                 // DPMI issue real interrupt
  r.h.bl = intno;
  r.h.bh = 0;
  r.w.cx = 0;
  sr.es = sr.ds;
  r.x.edi = (unsigned)&rmregs;
  int386x(0x31, &r, &r, &sr);     // Issue the interrupt

  OUT(ax); OUT(bx); OUT(cx); OUT(dx); OUT(si); OUT(di);
  out->x.cflag = rmregs.flags & 0x1;
  return out->x.ax;
}

int DPMI_int86x(int intno, RMREGS *in, RMREGS *out, RMSREGS *sregs)
{
  _RMREGS         rmregs;
  union REGS      r;
  struct SREGS    sr;

  memset(&rmregs, 0, sizeof(rmregs));
  IN(ax); IN(bx); IN(cx); IN(dx); IN(si); IN(di);
  rmregs.es = sregs->es;
  rmregs.ds = sregs->ds;

  segread(&sr);
  r.w.ax = 0x300;                 // DPMI issue real interrupt
  r.h.bl = intno;
  r.h.bh = 0;
  r.w.cx = 0;
  sr.es = sr.ds;
  r.x.edi = (unsigned)&rmregs;
  int386x(0x31, &r, &r, &sr);     // Issue the interrupt

  OUT(ax); OUT(bx); OUT(cx); OUT(dx); OUT(si); OUT(di);
  sregs->es = rmregs.es;
  sregs->cs = rmregs.cs;
  sregs->ss = rmregs.ss;
  sregs->ds = rmregs.ds;
  out->x.cflag = rmregs.flags & 0x1;
  return out->x.ax;
}

int DPMI_allocSelector(void)
{
  int         sel;
  union REGS  r;

  r.w.ax = 0;                     // DPMI allocate selector
  r.w.cx = 1;                     // Allocate a single selector
  int386(0x31, &r, &r);
  if (r.x.cflag)
    DPMI_Error("DPMI_allocSelector() failed!");
  sel = r.w.ax;

  r.w.ax = 9;                     // DPMI set access rights
  r.w.bx = sel;
  r.w.cx = 0x8092;                // 32 bit page granular
  int386(0x31, &r, &r);
  return sel;
}

long DPMI_mapPhysicalToLinear(long physAddr,long limit)
{
  union REGS  r;

  r.w.ax = 0x800;                 // DPMI map physical to linear
  r.w.bx = physAddr >> 16;
  r.w.cx = physAddr & 0xFFFF;
  r.w.si = limit >> 16;
  r.w.di = limit & 0xFFFF;
  int386(0x31, &r, &r);
  if (r.x.cflag)
    DPMI_Error("DPMI_mapPhysicalToLinear() failed!");
  return ((long)r.w.bx << 16) + r.w.cx;
}



void DPMI_setSelectorBase(int sel,long linAddr)
{
  union REGS  r;

  r.w.ax = 7;                     // DPMI set selector base address
  r.w.bx = sel;
  r.w.cx = linAddr >> 16;
  r.w.dx = linAddr & 0xFFFF;
  int386(0x31, &r, &r);
  if (r.x.cflag)
    DPMI_Error("DPMI_setSelectorBase() failed!");
}

void DPMI_setSelectorLimit(int sel,long limit)
{
  union REGS  r;

  r.w.ax = 8;                     // DPMI set selector limit
  r.w.bx = sel;
  r.w.cx = limit >> 16;
  r.w.dx = limit & 0xFFFF;
  int386(0x31, &r, &r);
  if (r.x.cflag)
    DPMI_Error("DPMI_setSelectorLimit() failed!");
}

